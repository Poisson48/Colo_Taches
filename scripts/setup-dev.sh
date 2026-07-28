#!/usr/bin/env bash
# Installe tout le nécessaire pour builder Colo_Taches en local (Ubuntu/WSL).
# Usage : bash scripts/setup-dev.sh   (dans un vrai terminal — sudo demande le mot de passe)
set -euo pipefail

PKGS=(
  # toolchain
  build-essential cmake ninja-build gdb unzip
  # Qt 6 dev (aligné sur .github/workflows/ci.yml)
  qt6-base-dev qt6-declarative-dev qt6-websockets-dev libqt6sql6-sqlite
  # caméra (scan du QR d'appairage)
  qt6-multimedia-dev
  # modules QML nécessaires à l'exécution de l'app
  qml6-module-qtquick-controls qml6-module-qtquick-layouts
  qml6-module-qtquick-window qml6-module-qtquick-templates
  qml6-module-qtqml-workerscript qml6-module-qtmultimedia
  # crypto
  libsodium-dev libsecp256k1-dev
)

echo "== Installation des paquets (sudo requis) =="
sudo apt-get update
sudo apt-get install -y "${PKGS[@]}"

echo
echo "== Vérification =="
cmake --version | head -1
g++ --version | head -1
ninja --version
qmake6 --version | tail -1 || true

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
echo
echo "== Build de contrôle =="
cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$ROOT/build"
ctest --test-dir "$ROOT/build" --output-on-failure

echo
echo "OK — environnement prêt. Binaire : build/src/colotaches"
