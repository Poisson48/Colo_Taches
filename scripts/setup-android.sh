#!/usr/bin/env bash
# Installe le kit de build Android pour Colo_Taches (sans sudo) :
#   - Android SDK (cmdline-tools, platform-tools, platform 35, build-tools, NDK r26b)
#   - Qt pour Android + Qt hôte (via aqtinstall dans un venv)
#   - libsodium et libsecp256k1 cross-compilées pour arm64-v8a
# Usage : bash scripts/setup-android.sh   (~7 Go de téléchargements)
set -euo pipefail

SDK_ROOT="$HOME/Android/Sdk"
NDK_VER="26.1.10909125"
QT_VER="6.8.2"
QT_ROOT="$HOME/Qt"
PREFIX="$HOME/android-libs/arm64-v8a"
API=24
JOBS="$(nproc)"

echo "== 1/4 Android cmdline-tools =="
if [ ! -x "$SDK_ROOT/cmdline-tools/latest/bin/sdkmanager" ]; then
  mkdir -p "$SDK_ROOT/cmdline-tools"
  cd /tmp
  curl -fLO https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
  # python3 -m zipfile : pas besoin d'unzip, mais il perd les bits d'exécution
  python3 -m zipfile -e commandlinetools-linux-11076708_latest.zip "$SDK_ROOT/cmdline-tools"
  rm -f commandlinetools-linux-11076708_latest.zip
  mv "$SDK_ROOT/cmdline-tools/cmdline-tools" "$SDK_ROOT/cmdline-tools/latest"
  chmod +x "$SDK_ROOT/cmdline-tools/latest/bin/"*
fi
SDKMANAGER="$SDK_ROOT/cmdline-tools/latest/bin/sdkmanager"

echo "== 2/4 SDK + NDK =="
yes | "$SDKMANAGER" --licenses >/dev/null || true
"$SDKMANAGER" --install \
  "platform-tools" \
  "platforms;android-35" \
  "build-tools;35.0.0" \
  "ndk;$NDK_VER" >/dev/null
NDK="$SDK_ROOT/ndk/$NDK_VER"

echo "== 3/4 Qt $QT_VER (hôte + android_arm64_v8a) =="
# aqtinstall en install user (python3-venv absent d'Ubuntu par défaut)
if ! python3 -m aqt version >/dev/null 2>&1; then
  if ! python3 -m pip --version >/dev/null 2>&1; then
    curl -fsSL https://bootstrap.pypa.io/get-pip.py -o /tmp/get-pip.py
    python3 /tmp/get-pip.py --user --break-system-packages >/dev/null
  fi
  python3 -m pip install --user --break-system-packages -q aqtinstall
fi
AQT="python3 -m aqt"
HOST_ARCH="$($AQT list-qt linux desktop --arch $QT_VER | tr ' ' '\n' | grep -m1 gcc_64)"
[ -d "$QT_ROOT/$QT_VER/gcc_64" ] || $AQT install-qt linux desktop "$QT_VER" "$HOST_ARCH" -O "$QT_ROOT"
[ -d "$QT_ROOT/$QT_VER/android_arm64_v8a" ] || \
  $AQT install-qt linux android "$QT_VER" android_arm64_v8a -m qtwebsockets qtmultimedia -O "$QT_ROOT"

echo "== 4/4 libsodium + libsecp256k1 (arm64) =="
# Même script qu'en CI : une seule source de vérité pour la cross-compilation.
ANDROID_NDK_ROOT="$NDK" PREFIX="$PREFIX" API="$API" \
  bash "$(dirname "$0")/build-android-deps.sh"

echo
echo "OK — kit Android prêt :"
echo "  SDK   : $SDK_ROOT"
echo "  NDK   : $NDK"
echo "  Qt    : $QT_ROOT/$QT_VER/{gcc_64,android_arm64_v8a}"
echo "  Libs  : $PREFIX"
echo "Build de l'APK : bash scripts/build-android.sh"
