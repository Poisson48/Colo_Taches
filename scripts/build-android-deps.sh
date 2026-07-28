#!/usr/bin/env bash
# Cross-compile libsodium et libsecp256k1 pour arm64-v8a.
# Utilisé par setup-android.sh (poste de dev) et par la CI (job android).
#
# Variables (avec valeurs par défaut) :
#   ANDROID_NDK_ROOT  chemin du NDK              (obligatoire)
#   PREFIX            préfixe d'installation     ($HOME/android-libs/arm64-v8a)
#   API               niveau d'API Android       (24)
set -euo pipefail

: "${ANDROID_NDK_ROOT:?ANDROID_NDK_ROOT non défini}"
PREFIX="${PREFIX:-$HOME/android-libs/arm64-v8a}"
API="${API:-24}"
JOBS="$(nproc)"
TC="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin"

echo "== libsodium (arm64) =="
if [ ! -f "$PREFIX/lib/libsodium.a" ]; then
  cd /tmp
  curl -fLO https://download.libsodium.org/libsodium/releases/libsodium-1.0.20.tar.gz
  tar xzf libsodium-1.0.20.tar.gz && cd libsodium-1.0.20
  ./configure --host=aarch64-linux-android --prefix="$PREFIX" \
    --disable-shared --enable-static --with-pic \
    CC="$TC/aarch64-linux-android${API}-clang" >/dev/null
  make -j"$JOBS" >/dev/null && make install >/dev/null
fi

echo "== libsecp256k1 (arm64) =="
if [ ! -f "$PREFIX/lib/libsecp256k1.a" ]; then
  cd /tmp
  curl -fL -o secp256k1-0.7.0.tar.gz https://github.com/bitcoin-core/secp256k1/archive/refs/tags/v0.7.0.tar.gz
  tar xzf secp256k1-0.7.0.tar.gz && cd secp256k1-0.7.0
  cmake -B build-android -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=$API \
    -DSECP256K1_ENABLE_MODULE_SCHNORRSIG=ON -DSECP256K1_ENABLE_MODULE_EXTRAKEYS=ON \
    -DSECP256K1_BUILD_TESTS=OFF -DSECP256K1_BUILD_EXHAUSTIVE_TESTS=OFF \
    -DSECP256K1_BUILD_BENCHMARK=OFF -DSECP256K1_BUILD_CTIME_TESTS=OFF \
    -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
  cmake --build build-android -j"$JOBS" >/dev/null
  cmake --install build-android >/dev/null
fi

echo "OK — libs arm64 dans $PREFIX"
