/*
    SPDX-FileCopyrightText: 2014 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2014 Sebastian Kügler <sebas@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

pragma ComponentBehavior: Bound

import QtQuick

import org.kde.kirigami as Kirigami

ClipboardItemDelegate {
    id: menuItem

    // The thumbnail source is for painting only. In particular, an unlocked
    // encrypted thumbnail uses image://, which cannot be consumed as a file URI.
    // Prepare a real PNG path only while a drag is active, then release it.
    property url dragImageUrl: ""

    Connections {
        target: menuItem.dragHandler
        function onActiveChanged() {
            const view = menuItem.ListView.view
            const historyModel = view ? view.clipboardMenu.model : null
            if (!historyModel) {
                return
            }
            if (menuItem.dragHandler.active) {
                menuItem.dragImageUrl = historyModel.imageFileUrlForDrag(menuItem.uuid)
            } else if (menuItem.dragImageUrl.toString() !== "") {
                historyModel.releaseImageFileUrlForDrag(menuItem.dragImageUrl)
                menuItem.dragImageUrl = ""
            }
        }
    }

    mainItem: Item {
        implicitHeight: childrenRect.height

        Drag.active: menuItem.dragHandler.active && menuItem.dragImageUrl.toString() !== ""
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction
        Drag.mimeData: {
            "text/uri-list": [menuItem.dragImageUrl.toString()],
        }

        Image {
            id: previewImage

            width: Math.min(Math.round(height * menuItem.imageSize.width / menuItem.imageSize.height), parent.width)
            height: sourceSize.height

            // align left
            // right in RTL
            anchors.left: parent.left

            cache: false
            source: menuItem.decoration
            sourceSize.height: Math.min(menuItem.imageSize.height, Kirigami.Units.gridUnit * 4 + Kirigami.Units.smallSpacing * 2)
            smooth: true
            fillMode: Image.PreserveAspectFit
        }
    }
}
