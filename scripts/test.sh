#!/bin/bash
# Builds Klipper with its autotests into build/ and runs them in a private Xvfb with a throwaway
# HOME, so your real clipboard and ~/.local/share/klipper are never touched.
#   scripts/test.sh                      all tests
#   scripts/test.sh -R HistoryVault -V   one test, verbose (arguments go to ctest)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
b="$root/build"
[ -f "$b/CMakeCache.txt" ] || cmake -S "$root" -B "$b" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON >/dev/null
make -C "$b" -j"$(nproc)" >"$b/build.log" 2>&1 || { grep -E "error" "$b/build.log" | head -20; echo "build failed, see $b/build.log"; exit 1; }
th="$b/test-home"
rm -rf "$th" && mkdir -p "$th"
cd "$b"
HOME="$th" XDG_DATA_HOME="$th/.local/share" XDG_CONFIG_HOME="$th/.config" XDG_CACHE_HOME="$th/.cache" \
  QT_QPA_PLATFORM=xcb WAYLAND_DISPLAY= xvfb-run -a ctest --timeout 60 --output-on-failure "$@"
