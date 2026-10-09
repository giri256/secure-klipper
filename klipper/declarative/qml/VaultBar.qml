/*
    SPDX-FileCopyrightText: 2026 giri256

    SPDX-License-Identifier: GPL-2.0-or-later
*/

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents3
import org.kde.plasma.extras as PlasmaExtras
import org.kde.plasma.private.clipboard as Private

/**
 * History vault controls shown in the popup header, under the search field.
 *
 *   not configured: hidden until the user clicks the "encrypt" button (setupRequested)
 *   setting up:     password + confirmation
 *   locked:         "N older entries are encrypted", password field, Unlock
 *   unlocked:       nothing here; the header shows a Lock button (see ClipboardMenu)
 *   changing:       old + new + confirmation
 */
ColumnLayout {
    id: vaultBar

    required property Private.HistoryModel model

    // Toggled by the header buttons
    property bool setupRequested: false
    property bool changeRequested: false

    // Values match HistoryModel::VaultOperation
    readonly property int opSetup: 0
    readonly property int opUnlock: 1
    readonly property int opChangePassword: 2

    readonly property string mode: {
        if (!model.vaultConfigured) {
            return setupRequested ? "setup" : "";
        }
        if (!model.vaultUnlocked) {
            return model.lockedCount > 0 ? "locked" : "";
        }
        return changeRequested ? "change" : "";
    }

    property string errorText: ""
    property bool confirmReset: false

    spacing: Kirigami.Units.smallSpacing
    visible: mode !== ""

    function focusPassword(): void {
        if (mode === "locked") {
            unlockField.forceActiveFocus();
        }
    }

    function clearFields(): void {
        // QML strings can't be wiped; at least drop the references promptly
        unlockField.text = "";
        newField.text = "";
        confirmField.text = "";
        oldField.text = "";
    }

    function submit(): void {
        if (model.vaultBusy) {
            return;
        }
        errorText = "";
        switch (mode) {
        case "locked":
            if (unlockField.text.length > 0) {
                model.unlockVault(unlockField.text);
            }
            break;
        case "setup":
        case "change":
            if (newField.text.length < 8) {
                errorText = i18nd("klipper", "Use at least 8 characters.");
                return;
            }
            if (newField.text !== confirmField.text) {
                errorText = i18nd("klipper", "The passwords do not match.");
                return;
            }
            if (mode === "setup") {
                model.setupVault(newField.text);
            } else {
                model.changeVaultPassword(oldField.text, newField.text);
            }
            break;
        }
    }

    onModeChanged: {
        errorText = "";
        confirmReset = false;
        clearFields();
    }

    Connections {
        target: vaultBar.model
        function onVaultOperationFinished(operation: int, success: bool): void {
            vaultBar.clearFields();
            if (success) {
                vaultBar.errorText = "";
                vaultBar.setupRequested = false;
                vaultBar.changeRequested = false;
                return;
            }
            switch (operation) {
            case vaultBar.opUnlock:
                vaultBar.errorText = i18nd("klipper", "Wrong password.");
                vaultBar.focusPassword();
                break;
            case vaultBar.opChangePassword:
                vaultBar.errorText = i18nd("klipper", "The current password is wrong.");
                oldField.forceActiveFocus();
                break;
            default:
                vaultBar.errorText = i18nd("klipper", "Could not set up encryption.");
                break;
            }
        }
        function onUnlockRequested(): void {
            vaultBar.focusPassword();
        }
    }

    Kirigami.Heading {
        Layout.fillWidth: true
        level: 5
        wrapMode: Text.Wrap
        text: {
            switch (vaultBar.mode) {
            case "setup":
                return i18nd("klipper", "Choose a password. Only the newest %1 entries will stay readable without it.", vaultBar.model.plaintextItems);
            case "locked":
                return i18ndp("klipper", "%1 older entry is encrypted", "%1 older entries are encrypted", vaultBar.model.lockedCount);
            case "change":
                return i18nd("klipper", "Change the history password");
            }
            return "";
        }
    }

    PlasmaExtras.PasswordField {
        id: oldField
        Layout.fillWidth: true
        visible: vaultBar.mode === "change"
        enabled: !vaultBar.model.vaultBusy
        placeholderText: i18nd("klipper", "Current password")
        Keys.onReturnPressed: newField.forceActiveFocus()
        Keys.onEnterPressed: newField.forceActiveFocus()
    }

    RowLayout {
        Layout.fillWidth: true
        visible: vaultBar.mode === "locked"
        spacing: Kirigami.Units.smallSpacing

        PlasmaExtras.PasswordField {
            id: unlockField
            Layout.fillWidth: true
            enabled: !vaultBar.model.vaultBusy
            placeholderText: i18nd("klipper", "Password")
            Keys.onReturnPressed: vaultBar.submit()
            Keys.onEnterPressed: vaultBar.submit()
        }
        PlasmaComponents3.Button {
            icon.name: "object-unlocked"
            text: i18nd("klipper", "Unlock")
            enabled: !vaultBar.model.vaultBusy && unlockField.text.length > 0
            onClicked: vaultBar.submit()
        }
    }

    PlasmaExtras.PasswordField {
        id: newField
        Layout.fillWidth: true
        visible: vaultBar.mode === "setup" || vaultBar.mode === "change"
        enabled: !vaultBar.model.vaultBusy
        placeholderText: vaultBar.mode === "change" ? i18nd("klipper", "New password") : i18nd("klipper", "Password")
        Keys.onReturnPressed: confirmField.forceActiveFocus()
        Keys.onEnterPressed: confirmField.forceActiveFocus()
    }

    PlasmaExtras.PasswordField {
        id: confirmField
        Layout.fillWidth: true
        visible: newField.visible
        enabled: !vaultBar.model.vaultBusy
        placeholderText: i18nd("klipper", "Repeat password")
        Keys.onReturnPressed: vaultBar.submit()
        Keys.onEnterPressed: vaultBar.submit()
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing

        PlasmaComponents3.BusyIndicator {
            Layout.preferredHeight: Kirigami.Units.iconSizes.small
            Layout.preferredWidth: Kirigami.Units.iconSizes.small
            visible: vaultBar.model.vaultBusy
            running: visible
        }

        PlasmaComponents3.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: text.length > 0
            color: Kirigami.Theme.negativeTextColor
            text: vaultBar.errorText
        }

        Item {
            Layout.fillWidth: true
            visible: vaultBar.errorText.length === 0
        }

        // Forgot password: the only way out is deleting what can't be decrypted. Two clicks.
        PlasmaComponents3.ToolButton {
            visible: vaultBar.mode === "locked"
            enabled: !vaultBar.model.vaultBusy
            icon.name: vaultBar.confirmReset ? "edit-delete" : ""
            text: vaultBar.confirmReset ? i18nd("klipper", "Delete encrypted entries") : i18nd("klipper", "Forgot password…")
            onClicked: {
                if (vaultBar.confirmReset) {
                    vaultBar.model.resetVault();
                } else {
                    vaultBar.confirmReset = true;
                }
            }
        }

        PlasmaComponents3.Button {
            visible: vaultBar.mode === "setup" || vaultBar.mode === "change"
            text: i18nd("klipper", "Cancel")
            onClicked: {
                vaultBar.setupRequested = false;
                vaultBar.changeRequested = false;
            }
        }

        PlasmaComponents3.Button {
            visible: vaultBar.mode === "setup" || vaultBar.mode === "change"
            enabled: !vaultBar.model.vaultBusy && newField.text.length > 0 && confirmField.text.length > 0
            icon.name: "object-locked"
            text: vaultBar.mode === "setup" ? i18nd("klipper", "Encrypt") : i18nd("klipper", "Change")
            onClicked: vaultBar.submit()
        }
    }
}
