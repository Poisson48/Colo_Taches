#!/usr/bin/env bash
# Empaquette le binaire desktop en AppImage autonome (Qt embarqué), lançable sur
# n'importe quelle distribution Linux x86-64 sans rien installer.
#
# Prérequis : le projet est déjà construit dans ./build (cmake --build build), et
# QT_ROOT pointe sur le kit Qt desktop (celui qui a servi au build).
#
# Variables :
#   QT_ROOT        racine du kit Qt (contient bin/qmake)   [défaut : ~/Qt/6.8.2/gcc_64]
#   VERSION_NAME   version, pour le nom du fichier          [défaut : 0.0.0]
#   OUT_DIR        où déposer l'AppImage                     [défaut : racine du dépôt]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QT_ROOT="${QT_ROOT:-$HOME/Qt/6.8.2/gcc_64}"
VERSION_NAME="${VERSION_NAME:-0.0.0}"
OUT_DIR="${OUT_DIR:-$ROOT}"
BIN="$ROOT/build/src/colotaches"

[ -x "$BIN" ] || { echo "Binaire absent : $BIN (construire d'abord)" >&2; exit 1; }
[ -x "$QT_ROOT/bin/qmake" ] || { echo "qmake introuvable dans $QT_ROOT/bin" >&2; exit 1; }

# --- AppDir : l'arborescence que linuxdeploy transformera en AppImage ---
APPDIR="$ROOT/build/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/512x512/apps"

cp "$BIN" "$APPDIR/usr/bin/colotaches"
cp "$ROOT/packaging/colotaches.png" \
   "$APPDIR/usr/share/icons/hicolor/512x512/apps/colotaches.png"

cat > "$APPDIR/usr/share/applications/colotaches.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Colo Tâches
Comment=Liste de courses partagée
Exec=colotaches
Icon=colotaches
Categories=Utility;Network;
Terminal=false
EOF

# --- Outils de déploiement (téléchargés une fois, mis en cache par le workflow) ---
TOOLS="$ROOT/build/appimage-tools"
mkdir -p "$TOOLS"
fetch() {
    local name="$1" url="$2"
    if [ ! -x "$TOOLS/$name" ]; then
        echo "== téléchargement $name =="
        wget -q -O "$TOOLS/$name" "$url"
        chmod +x "$TOOLS/$name"
    fi
}
BASE="https://github.com/linuxdeploy"
fetch linuxdeploy         "$BASE/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
fetch linuxdeploy-plugin-qt "$BASE/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"

# Sur les runners CI, FUSE est absent : exécuter les AppImages en les extrayant.
export APPIMAGE_EXTRACT_AND_RUN=1

# Le plugin Qt lit les imports QML pour n'embarquer que les modules utilisés. Il scanne
# les SOURCES : or ScanPage.qml (scanner de QR) importe QtMultimedia et le module de
# l'app — modules absents du build desktop (COLO_HAS_CAMERA=OFF, scanner désactivé),
# donc introuvables et fatals pour le scan. On scanne une copie sans ce fichier.
QML_SCAN="$ROOT/build/qml-scan"
rm -rf "$QML_SCAN"
cp -r "$ROOT/src/qml" "$QML_SCAN"
rm -f "$QML_SCAN/ScanPage.qml"
export QML_SOURCES_PATHS="$QML_SCAN"

# Qt embarque un driver SQL Mimer (base propriétaire) qui dépend de libmimerapi.so,
# absente du système : linuxdeploy échoue à résoudre cette dépendance. On ne touche PAS
# au kit Qt (le supprimer casse la config CMake de Qt6Sql, et le kit est mis en cache) —
# on fournit un stub libmimerapi.so que linuxdeploy embarquera (inutilisé, on n'utilise
# que SQLite).
STUBDIR="$ROOT/build/stublibs"
mkdir -p "$STUBDIR"
if [ ! -f "$STUBDIR/libmimerapi.so" ]; then
    echo 'void colo_mimer_stub(void){}' | cc -x c -shared -fPIC -o "$STUBDIR/libmimerapi.so" -
fi
export LD_LIBRARY_PATH="$STUBDIR:${LD_LIBRARY_PATH:-}"

export QMAKE="$QT_ROOT/bin/qmake"
export PATH="$QT_ROOT/bin:$TOOLS:$PATH"

OUTPUT="ColoTaches-${VERSION_NAME}-x86_64.AppImage"
export OUTPUT

cd "$ROOT/build"
"$TOOLS/linuxdeploy" \
    --appdir "$APPDIR" \
    --plugin qt \
    --output appimage \
    --desktop-file "$APPDIR/usr/share/applications/colotaches.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/512x512/apps/colotaches.png"

mkdir -p "$OUT_DIR"
mv "$ROOT/build/$OUTPUT" "$OUT_DIR/$OUTPUT"
echo
echo "AppImage : $OUT_DIR/$OUTPUT"
