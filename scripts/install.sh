#!/bin/bash
# Installs the Secure Klipper tray entry built by scripts/build.sh, next to the stock Klipper.
# Everything goes into your home folder (no root); uninstall.sh removes all of it.
#   ~/.local/opt/klipper-vault/                         renamed QML module (the code)
#   ~/.local/share/plasma/plasmoids/org.kde.plasma.clipboardvault/   the tray applet
#   ~/.config/systemd/user/plasma-plasmashell.service.d/klipper-vault.conf   tells plasmashell where the module is
# Restarts plasmashell (panels blink for a second or two).
set -euo pipefail
bt="$(cd "$(dirname "$0")/.." && pwd)/build-tray"
prefix="${KLIPPER_VAULT_PREFIX:-$HOME/.local/opt/klipper-vault}"
pkgdst="$HOME/.local/share/plasma/plasmoids/org.kde.plasma.clipboardvault"
dropin="$HOME/.config/systemd/user/plasma-plasmashell.service.d/klipper-vault.conf"

[ -f "$bt/build/Makefile" ] || { echo "run scripts/build.sh first"; exit 1; }
make -C "$bt/build" install >"$bt/install.log"
[ -f "$prefix/lib/qml/org/kde/plasma/private/clipboardvault/qmldir" ] || { echo "module not installed under $prefix"; exit 1; }

rm -rf "$pkgdst" && mkdir -p "$(dirname "$pkgdst")" && cp -a "$bt/plasmoid/org.kde.plasma.clipboardvault" "$pkgdst"

mkdir -p "$(dirname "$dropin")"
printf '# Added by secure-klipper scripts/install.sh; removed by scripts/uninstall.sh\n[Service]\nEnvironment=QML_IMPORT_PATH=%s/lib/qml\n' "$prefix" >"$dropin"
systemctl --user daemon-reload
systemctl --user restart plasma-plasmashell.service
echo "installed. Look for the padlock icon in the system tray (it may be under the ^ arrow)."
echo "If it is missing: right-click the tray > Configure System Tray > Entries > Secure Klipper > Always shown."
