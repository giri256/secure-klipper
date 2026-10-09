#!/bin/bash
# Loads the stock Klipper QML module and the Secure Klipper tray edition into ONE process (as
# plasmashell does) and checks they keep separate histories. Needs scripts/build.sh first.
# Runs with a private HOME, D-Bus session and Xvfb; nothing real is touched.
d=$(cd "$(dirname "$0")" && pwd); root=$(cd "$d/../.." && pwd); th="$root/build-tray/coexist-home"
[ -d "$root/build-tray/build/bin/org/kde/plasma/private/clipboardvault" ] || { echo "run scripts/build.sh first"; exit 1; }
rm -rf "$th" && mkdir -p "$th"
k="$th/.local/share/klipper/history3.sqlite"; v="$th/.local/share/klipper-vault/history3.sqlite"
HOME="$th" XDG_DATA_HOME="$th/.local/share" XDG_CONFIG_HOME="$th/.config" XDG_CACHE_HOME="$th/.cache" \
  QT_QPA_PLATFORM=xcb WAYLAND_DISPLAY= QML_IMPORT_PATH="$root/build-tray/build/bin" \
  dbus-run-session -- xvfb-run -a timeout 120 qml6 "$d/coexist.qml" >"$root/build-tray/coexist.log" 2>&1 </dev/null
rc=$?
# D-Bus-activated helpers (e.g. a portal) can outlive the private session; stop any still using this HOME
for p in /proc/[0-9]*; do grep -qz "^HOME=$th$" "$p/environ" 2>/dev/null && kill "${p#/proc/}" 2>/dev/null; done
echo "stock rows: $(sqlite3 "$k" 'select count(*) from main')  secure rows: $(sqlite3 "$v" 'select count(*) from main')  of them encrypted: $(sqlite3 "$v" 'select count(*) from vault')"
if [ $rc = 0 ]; then echo "PASS"; else echo "FAIL ($rc), log: build-tray/coexist.log"; exit 1; fi
