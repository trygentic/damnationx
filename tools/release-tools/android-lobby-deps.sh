#!/usr/bin/env bash
set -euo pipefail
out="$(realpath -m "${1:?output directory}")"
mkdir -p "$out"
cd "$out"
if [ ! -d boost_1_88_0 ]; then
  curl -fL --retry 3 https://archives.boost.io/release/1.88.0/source/boost_1_88_0.tar.bz2 -o boost.tar.bz2
  echo '46d9d2c06637b219270877c9e16155cbd015b6dc84349af064c088e9b5b12f7b  boost.tar.bz2' | sha256sum -c -
  tar -xf boost.tar.bz2
fi
if [ ! -f openssl.tar.gz ]; then
  curl -fL --retry 3 https://github.com/openssl/openssl/releases/download/openssl-3.5.7/openssl-3.5.7.tar.gz -o openssl.tar.gz
fi
echo 'a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8  openssl.tar.gz' | sha256sum -c -
export ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:?set NDK root}"
export PATH="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
for abi in arm64-v8a x86_64; do
  [ -f "openssl/$abi/lib/libssl.a" ] && continue
  src="$out/openssl-src-$abi"
  mkdir -p "$src"
  tar -xf openssl.tar.gz --strip-components=1 -C "$src"
  target=android-arm64
  [ "$abi" = x86_64 ] && target=android-x86_64
  (cd "$src" && ./Configure "$target" -D__ANDROID_API__=24 -fPIC no-shared no-tests --prefix="$out/openssl/$abi" --libdir=lib && make -j4 && make install_sw)
done
