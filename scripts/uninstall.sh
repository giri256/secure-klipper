#!/bin/bash
# Removes everything install.sh added and restarts plasmashell. The stock Klipper is untouched.
#   --purge  also deletes the variant's own history and settings
#            (~/.local/share/klipper-vault, ~/.config/klippervaultrc)
set -euo pipefail
prefix="${KLIPPER_VAULT_PREFIX:-$HOME/.local/opt/klipper-vault}"
appletsrc="$HOME/.config/plasma-org.kde.plasma.desktop-appletsrc"

rm -rf "$prefix" "$HOME/.local/share/plasma/plasmoids/org.kde.plasma.clipboardvault"
rm -f "$HOME/.config/systemd/user/plasma-plasmashell.service.d/klipper-vault.conf"
rmdir "$HOME/.config/systemd/user/plasma-plasmashell.service.d" 2>/dev/null || true
systemctl --user daemon-reload

# Drop the applet id from the tray's item lists while plasmashell is stopped (it rewrites the file on exit)
systemctl --user stop plasma-plasmashell.service
if [ -f "$appletsrc" ]; then
    cp "$appletsrc" "$appletsrc.before-klipper-vault-uninstall"
    sed -i -E '/^(extraItems|knownItems|hiddenItems|shownItems)=/ { s/,org\.kde\.plasma\.clipboardvault//g; s/=org\.kde\.plasma\.clipboardvault,?/=/ }' "$appletsrc"
    # ...and the tray's applet instance: the group with plugin=org.kde.plasma.clipboardvault plus its sub-groups
    python3 - "$appletsrc" <<'PY'
import sys
p = sys.argv[1]
groups, cur = [], None
for line in open(p, encoding="utf-8").read().split("\n"):
    if line.startswith("["):
        cur = [line, []]; groups.append(cur)
    elif cur is None:
        groups.append(cur := [None, []]); cur[1].append(line)
    else:
        cur[1].append(line)
dead = [g[0] for g in groups if g[0] and "plugin=org.kde.plasma.clipboardvault" in g[1]]
keep = [g for g in groups if not g[0] or not any(g[0] == d or g[0].startswith(d + "[") for d in dead)]
out = "\n".join("\n".join(([g[0]] if g[0] else []) + g[1]) for g in keep)
open(p, "w", encoding="utf-8").write(out)
PY
fi
systemctl --user start plasma-plasmashell.service

if [ "${1:-}" = "--purge" ]; then
    rm -rf "$HOME/.local/share/klipper-vault"
    rm -f "$HOME/.config/klippervaultrc" "$HOME/.local/state/klippervaultrc"
    echo "removed, including the variant's history and settings"
else
    echo "removed. Its history is kept in ~/.local/share/klipper-vault (run with --purge to delete it)"
fi
