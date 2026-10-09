#!/bin/bash
# Regenerates docs/images/{setup,locked,wrong-password,unlocked}.png from the dev build
# (scripts/test.sh builds it). Breeze Dark, 2x scale, private Xvfb and HOME.
#   scripts/screenshots.sh [light]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
bin="${BUILD_DIR:-$root/build}/bin"
[ -d "$bin/org/kde/plasma/private/clipboard" ] || { echo "run scripts/test.sh first (needs $bin)"; exit 1; }
scheme=BreezeDark; theme=breeze-dark
[ "${1:-}" = light ] && { scheme=BreezeLight; theme=default; }
out="$root/docs/images"; mkdir -p "$out"
th=$(mktemp -d "${TMPDIR:-$root/build}/screenshots-home.XXXXXX")
mkdir -p "$th/.config"
cp "/usr/share/color-schemes/$scheme.colors" "$th/.config/kdeglobals"
printf '[Theme]\nname=%s\n' "$theme" >"$th/.config/plasmarc"
HOME="$th" XDG_DATA_HOME="$th/.local/share" XDG_CONFIG_HOME="$th/.config" XDG_CACHE_HOME="$th/.cache" \
  QT_QPA_PLATFORM=xcb QT_QPA_PLATFORMTHEME=kde WAYLAND_DISPLAY= QT_SCALE_FACTOR=2 \
  QML_IMPORT_PATH="$bin" LD_LIBRARY_PATH="$bin" \
  dbus-run-session -- xvfb-run -a -s "-screen 0 1600x1600x24" timeout 120 qml6 "$root/scripts/screenshots.qml" -- "$out" </dev/null >"$th.log" 2>&1
rc=$?
for p in /proc/[0-9]*; do grep -qz "^HOME=$th$" "$p/environ" 2>/dev/null && kill "${p#/proc/}" 2>/dev/null; done
rm -rf "$th"
[ $rc = 0 ] || { echo "failed ($rc), log: $th.log"; exit 1; }
rm -f "$th.log"
ls "$out"
