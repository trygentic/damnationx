#!/usr/bin/env bash
# Cross-compile the Windows x64 (MinGW) build inside an Ubuntu 24.04 container,
# mirroring .github/workflows/Windows_MinGW_x64.yml. Works on any Docker host
# (including Apple Silicon: the arm64 Ubuntu image ships the mingw-w64 cross compilers).
#
# Usage: tools/build-windows-docker.sh [extra cmake args...]
# Env:   JOBS (default 6), BUILD_DIR (default build-windows-x64), IMAGE (default ubuntu:24.04)
# Output: $BUILD_DIR/<package>.zip (CPack name, e.g. devilutionx-<version>-<commit>.zip) and $BUILD_DIR/devilutionx.exe (unstripped)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JOBS="${JOBS:-6}"
BUILD_DIR="${BUILD_DIR:-build-windows-x64}"
IMAGE="${IMAGE:-ubuntu:24.04}"

# In a git worktree, .git is a file pointing at the main repo's git dir by absolute path;
# mount that common dir (read-only) at the same path so CMake's git-describe versioning works.
GIT_COMMON_DIR="$(cd "${REPO_ROOT}" && cd "$(git rev-parse --git-common-dir)" && pwd)"
GIT_MOUNT=()
case "${GIT_COMMON_DIR}" in
  "${REPO_ROOT}"/*) ;;
  *) GIT_MOUNT=(-v "${GIT_COMMON_DIR}:${GIT_COMMON_DIR}:ro") ;;
esac

docker run --rm \
  -v "${REPO_ROOT}:/src" -w /src ${GIT_MOUNT[@]+"${GIT_MOUNT[@]}"} \
  -e JOBS="${JOBS}" -e BUILD_DIR="${BUILD_DIR}" \
  "${IMAGE}" bash -euo pipefail -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends ca-certificates make cmake \
      gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 pkg-config-mingw-w64-x86-64 libz-mingw-w64-dev \
      gettext dpkg-dev wget git zip unzip >/dev/null
    if [ "$(uname -m)" = x86_64 ]; then
      apt-get install -y -qq --no-install-recommends smpq >/dev/null
    else
      # Ubuntu'"'"'s arm64 smpq (StormLib 9.22) aborts on an assertion
      # (sizeof(hf->hctx) >= sizeof(hash_state)) when writing an MPQ; build it from source.
      apt-get install -y -qq --no-install-recommends g++ curl zlib1g-dev libbz2-dev >/dev/null
      NOSUDO=1 PREFIX=/usr /src/tools/build_and_install_smpq.sh >/tmp/smpq-build.log 2>&1 \
        || { tail -40 /tmp/smpq-build.log; exit 1; }
    fi
    git config --global --add safe.directory "*"
    # Run the prep script from a scratch dir so its tmp folder stays out of the repo.
    mkdir -p /tmp/prep && cd /tmp/prep && /src/Packaging/windows/mingw-prep64.sh && cd /src
    cmake -S. -B"${BUILD_DIR}" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DCPACK=ON \
      -DCMAKE_TOOLCHAIN_FILE=../CMake/platforms/mingwcc64.toolchain.cmake \
      -DDEVILUTIONX_SYSTEM_BZIP2=OFF -DDEVILUTIONX_STATIC_LIBSODIUM=ON \
      -DDISCORD_INTEGRATION=ON -DSCREEN_READER_INTEGRATION=ON '"$*"'
    cmake --build "${BUILD_DIR}" -j "${JOBS}" --target package
  '
ls -la "${REPO_ROOT}/${BUILD_DIR}"/*.zip "${REPO_ROOT}/${BUILD_DIR}/devilutionx.exe"
