// Loads the STOCK clipboard module and the renamed vault copy into one process (like plasmashell)
// and checks they keep separate histories. Run via run.sh only (private X, D-Bus and HOME).
import QtQuick
import org.kde.plasma.private.clipboard as Stock
import org.kde.plasma.private.clipboardvault as Vault

Item {
    id: root
    property int step: 0
    property var texts: ["one", "two", "three", "four", "five", "six", "seven"]
    Stock.HistoryModel { id: stock }
    Vault.HistoryModel { id: vault }
    TextEdit { id: te; visible: false }
    Timer {
        interval: 600; repeat: true; running: true
        onTriggered: {
            const s = root.step++;
            if (s < root.texts.length) { te.text = "coexist-" + root.texts[s]; te.selectAll(); te.copy(); return; }
            switch (s - root.texts.length) {
            case 0: vault.setupVault("test password"); break;
            case 1: case 2: if (vault.vaultBusy || !vault.vaultConfigured) root.step--; break;
            case 3: vault.lockVault(); break;
            case 4:
                // Exit code is the verdict (console output is not reliable under xvfb-run here):
                // 0 = stock sees all 7, vault shows the newest 5 and holds 2 locked, stock has no vault API
                Qt.exit(stock.count === 7 && vault.count === 5 && vault.lockedCount === 2 && stock.vaultConfigured === undefined ? 0 : 3);
                break;
            }
        }
    }
}
