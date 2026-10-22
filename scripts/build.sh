#!/bin/bash
# Builds the Secure Klipper tray edition: a renamed copy of this repository's Klipper that runs in
# plasmashell NEXT TO the stock Klipper. The sources are copied into build-tray/src and renamed
# there; the files in the repository are never modified.
#
# Everything the stock Klipper shares inside plasmashell gets its own name:
#   QML module      org.kde.plasma.private.clipboard  -> org.kde.plasma.private.clipboardvault
#   applet id       org.kde.plasma.clipboard          -> org.kde.plasma.clipboardvault
#   SQL connection  "klipper"                         -> "klipper-vault"   (process-wide name!)
#   data folder     ~/.local/share/klipper            -> ~/.local/share/klipper-vault
#   config          klipperrc                         -> klippervaultrc (NoEmptyClipboard off by default)
#   D-Bus           org.kde.klipper /klipper          -> org.kde.klippervault /klippervault
#   shortcuts       action names prefixed "vault-", no default keys (Meta+V stays with Klipper)
#   image provider  klipperpreview (URL/file thumbs)   -> klippervaulturlpreview
#   encrypted image previews                           -> klippervaultpreview
#   libklipper      shared library                    -> static, linked into the QML plugin
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
prefix="${KLIPPER_VAULT_PREFIX:-$HOME/.local/opt/klipper-vault}"
bt="$root/build-tray"
src="$bt/src"

rm -rf "$src" && mkdir -p "$src/applets"
cp -a "$root/klipper" "$root/cmake" "$root/config-X11.h.cmake" "$root/c_ptr.h" "$root/CMakeLists.txt" "$src/"
cp -a "$root/applets/clipboard" "$src/applets/"
rm -rf "$src/klipper/autotests"

cd "$src/klipper"
# Names that would collide with the stock Klipper loaded in the same plasmashell process
grep -rlZ --include='*.qml' --include='*.cpp' --include='*.h' --include='CMakeLists.txt' 'org.kde.plasma.private.clipboard\|org_kde_plasma_private_clipboard\|klipperpreview' . \
  | xargs -0 sed -i 's/org\.kde\.plasma\.private\.clipboard/org.kde.plasma.private.clipboardvault/g; s/org_kde_plasma_private_clipboard/org_kde_plasma_private_clipboardvault/g; s/klipperpreview/klippervaulturlpreview/g'
# These providers must not collide: Qt replaces an existing image provider when the same key is registered.
grep -q 'addImageProvider(QStringLiteral("klippervaulturlpreview"), new PreviewImageProvider)' declarative/klipperplugin.cpp || { echo 'Missing distinct URL thumbnail provider'; exit 1; }
grep -q 'addImageProvider(QStringLiteral("klippervaultpreview"), new VaultPreviewImageProvider' declarative/klipperplugin.cpp || { echo 'Missing encrypted-image preview provider'; exit 1; }
grep -q 'image://klippervaulturlpreview/' declarative/qml/UrlItemDelegate.qml || { echo 'URL delegate does not use its dedicated thumbnail provider'; exit 1; }
grep -q 'image://klippervaultpreview/' historymodel.cpp || { echo 'Missing encrypted image preview URL'; exit 1; }
sed -i 's/u"klipper"_s/u"klipper-vault"_s/g; s|u"/klipper"|u"/klipper-vault"|g; s|u"/klipper/data/"|u"/klipper-vault/data/"|g' historymodel.cpp systemclipboard.cpp
sed -i 's/QStringLiteral("org\.kde\.klipper")/QStringLiteral("org.kde.klippervault")/; s|QStringLiteral("/klipper")|QStringLiteral("/klippervault")|' klipper.cpp
sed -i 's/addAction(QStringLiteral("/addAction(QStringLiteral("vault-/g; s/QKeySequence(Qt::META | Qt::CTRL | Qt::Key_X)/QKeySequence()/; s/QKeySequence(Qt::META | Qt::Key_V)/QKeySequence()/' klipper.cpp
sed -i 's/QStringLiteral("repeat_action")/QStringLiteral("vault-repeat_action")/g' configdialog.cpp
# KConfigDialog::showDialog() finds dialogs by name process-wide; window-state groups are cosmetic but kept apart too
sed -i 's/QStringLiteral("preferences")/QStringLiteral("vault-preferences")/' configdialog.cpp klipper.cpp
sed -i 's/group(u"klipper"_s)/group(u"klipper-vault"_s)/g' configdialog.cpp editactiondialog.cpp editcommanddialog.cpp
# Qt's metatype registry is process-wide and keyed by C++ class name: with the stock plugin loaded first,
# QML resolves "DeclarativeHistoryModel*" to the stock type and the stock EditPage push fails
# ("Cannot assign QObject* to DeclarativeHistoryModel*"). Rename the QML-facing classes (and HistoryModel,
# whose Q_ENUMs are also registered by name); the QML type names stay HistoryModel / KlipperInterface.
grep -rlZw --include='*.cpp' --include='*.h' 'HistoryModel\|DeclarativeHistoryModel\|KlipperInterface' . \
  | xargs -0 sed -i -E 's/\b(Declarative)?HistoryModel\b/Vault\1HistoryModel/g; s/\bKlipperInterface\b/VaultKlipperInterface/g; s/QML_NAMED_ELEMENT\(VaultHistoryModel\)/QML_NAMED_ELEMENT(HistoryModel)/'
sed -i -E 's/^(\s*)QML_ELEMENT$/\1QML_NAMED_ELEMENT(KlipperInterface)/' declarative/klipperinterface.h
sed -i 's/<kcfgfile name="klipperrc"\/>/<kcfgfile name="klippervaultrc"\/>/' klipper.kcfg
# Two clipboard managers both "preventing an empty clipboard" would keep re-setting it at each other
python3 - klipper.kcfg <<'EOF'
import re, sys
p = sys.argv[1]; s = open(p).read()
s, n = re.subn(r'(<entry name="PreventEmptyClipboard"[^>]*>.*?<default>)true(</default>)', r'\1false\2', s, flags=re.S)
assert n == 1, "PreventEmptyClipboard default not found"
open(p, 'w').write(s)
EOF
# libklipper becomes a private static library inside the QML plugin: no second libklipper.so in the process
sed -i 's/add_library(klipper SHARED/add_library(klipper STATIC/; s/^install(TARGETS klipper .*$/set_target_properties(klipper PROPERTIES POSITION_INDEPENDENT_CODE ON)\ntarget_compile_definitions(klipper PUBLIC KLIPPER_STATIC_DEFINE)/' CMakeLists.txt
sed -i '/org.kde.klipper.desktop/d; /^add_subdirectory(kconf_update)/d' CMakeLists.txt

# Fail loudly if any shared name survived
leftover=$(grep -rn 'u"klipper"_s\|u"/klipper"\|QStringLiteral("org\.kde\.klipper")\|showDialog(QStringLiteral("preferences")\|"/klipper"\|klipperrc"\|private\.clipboard[^v]\|private\.clipboard$\|"klipperpreview\|//klipperpreview\|add_library(klipper SHARED' . || true)
leftover+=$(grep -rnwE --include='*.h' 'class (Declarative)?HistoryModel|class KlipperInterface' . || true)
if [ -n "$leftover" ]; then echo "Unrenamed shared names:"; echo "$leftover"; exit 1; fi

# Applet: plain QML package (no compiled applet class needed)
pkg="$bt/plasmoid/org.kde.plasma.clipboardvault"
rm -rf "$bt/plasmoid" && mkdir -p "$pkg/contents/ui" "$pkg/contents/config"
sed 's/org\.kde\.plasma\.private\.clipboard/org.kde.plasma.private.clipboardvault/g; s/Plasmoid.icon: "klipper-symbolic"/Plasmoid.icon: "object-locked"/' "$src/applets/clipboard/qml/main.qml" > "$pkg/contents/ui/main.qml"
cp "$src/applets/clipboard/main.xml" "$pkg/contents/config/main.xml"
python3 - "$src/applets/clipboard/metadata.json" "$pkg/metadata.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
kp = {k: v for k, v in m["KPlugin"].items() if "[" not in k}  # drop translated Name/Description
kp.update(Id="org.kde.plasma.clipboardvault", Name="Secure Klipper", Icon="object-locked",
          Description="Clipboard history; entries older than the newest few are encrypted with a password")
out = {k: v for k, v in m.items() if "[" not in k and k != "KPlugin"}
out.update(KPlugin=kp, KPackageStructure="Plasma/Applet")
json.dump(out, open(sys.argv[2], "w"), indent=4, ensure_ascii=False)
EOF
grep -q 'org.kde.plasma.private.clipboardvault' "$pkg/contents/ui/main.qml"

cmake -S "$src" -B "$bt/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF -DKDE_INSTALL_QMLDIR=lib/qml >"$bt/configure.log" 2>&1 || { tail -30 "$bt/configure.log"; exit 1; }
if ! make -C "$bt/build" -j"$(nproc)" >"$bt/build.log" 2>&1; then
    grep -E "error" "$bt/build.log" | head -20; echo "build failed, see $bt/build.log"; exit 1
fi
echo "built: $bt/build  (next: scripts/install.sh)"
