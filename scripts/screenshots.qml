// Drives the real Klipper popup through the vault states and saves the README screenshots.
// Run with scripts/screenshots.sh (private Xvfb and HOME; your clipboard is never touched).
// All clipboard content below is made up.
import QtQuick
import QtQuick.Window
import org.kde.ksvg as KSvg
import org.kde.plasma.private.clipboard as Private

Window {
    id: win
    width: 520; height: 640; visible: true
    color: "transparent"
    property string out: Qt.application.arguments[Qt.application.arguments.length - 1]
    property int step: 0
    // Oldest first; the newest 5 stay readable, the rest get encrypted
    property var texts: [
        "Your verification code is 482 913. It expires in 10 minutes.",
        "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIK3m9Qx7Lr2vYtP8wZcN4sHf0eJbGd1aUoV6yXkRq2Tn demo@workstation",
        "IBAN DE89 3704 0044 0532 0130 00",
        "postgres://orders:Xk9#mQ2v@db.internal:5432/orders",
        "Recovery codes: 7F3K-92LD  QW8M-1ZP4  H6TR-55XA",
        "https://invent.kde.org/plasma/plasma-workspace",
        "git rebase -i origin/main",
        "Meet at 7:30 at gate B, flight LH 761",
        "sudo pacman -Syu",
        "Only the newest 5 entries stay readable on disk."
    ]
    readonly property string pw: "correct horse battery"

    KSvg.FrameSvgItem {
        id: frame
        imagePath: "dialogs/background"
        width: 460; height: 580
        anchors.centerIn: parent
        Private.KlipperPopup {
            id: popup
            anchors.fill: parent
            anchors.leftMargin: frame.fixedMargins.left + 4
            anchors.rightMargin: frame.fixedMargins.right + 4
            anchors.topMargin: frame.fixedMargins.top + 4
            anchors.bottomMargin: frame.fixedMargins.bottom + 4
        }
    }
    TextEdit { id: te; visible: false }
    Private.HistoryModel { id: hm }

    function shot(name) {
        frame.grabToImage(r => { r.saveToFile(out + "/" + name + ".png"); });
    }
    function find(item, pred) {
        if (pred(item)) return item;
        for (let c of item.children) { const f = find(c, pred); if (f) return f; }
        return null;
    }
    function bar() { return find(popup, i => i.hasOwnProperty("setupRequested")); }
    function field(ph) { return find(popup, i => i.hasOwnProperty("placeholderText") && i.placeholderText === ph && i.visible); }
    function list() { return find(popup, i => i.hasOwnProperty("highlightFollowsCurrentItem")); }

    Timer {
        interval: 700; repeat: true; running: true
        onTriggered: {
            const s = win.step++;
            if (s < win.texts.length) { te.text = win.texts[s]; te.selectAll(); te.copy(); return; }
            switch (s - win.texts.length) {
            case 0: bar().setupRequested = true; field("Password").text = win.pw; field("Repeat password").text = win.pw; list().currentIndex = -1; break;
            case 1: shot("setup"); break;
            case 2: bar().submit(); break;
            case 3: case 4: if (!hm.vaultUnlocked || hm.vaultBusy) win.step--; break;
            case 5: hm.lockVault(); break;
            case 6: list().currentIndex = -1; break;
            case 7: shot("locked"); break;
            case 8: field("Password").text = "correct horse"; bar().submit(); break;
            case 9: if (hm.vaultBusy) win.step--; break;
            case 10: list().currentIndex = -1; shot("wrong-password"); break;
            case 11: field("Password").text = win.pw; bar().submit(); break;
            case 12: case 13: if (!hm.vaultUnlocked || hm.vaultBusy) win.step--; break;
            case 14: list().currentIndex = 0; break;
            case 15: shot("unlocked"); break;
            case 16: Qt.exit(hm.lockedCount === 0 && hm.count === win.texts.length ? 0 : 3); break;
            }
        }
    }
}
