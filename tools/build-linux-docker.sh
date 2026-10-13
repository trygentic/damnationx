#!/usr/bin/env bash
# Build DevilutionX Linux release packages inside an Ubuntu 24.04 container,
# mirroring .github/workflows/Linux_aarch64.yml and Linux_x86_64.yml.
#
# Usage: tools/build-linux-docker.sh [arm64|x64|all|arm64-sdl3|arm64-webgpu|x64-webgpu|webgpu] [OUT_DIR]
#   env JOBS=6              parallel jobs (default 6)
#   env DAWN_JOBS=4         parallel jobs for the one-time Dawn build (*-webgpu)
#   env DAWN_BUILD_ROOT=... where Dawn is fetched/built/installed once per arch
#                           (default: build-linux-dawn in the checkout, shared by both arches)
#   env DAWN_DIR=...        use an already installed Dawn prefix instead (DEVILUTIONX_DAWN_DIR)
#   env STRIP_PACKAGE=1     strip the binary in the release tarball (default 1 for *-webgpu, 0 otherwise);
#                           the debug info is kept next to it as <build>/devilutionx.debug
#   env TARBALL_DEB_RPM=0   leave the .deb/.rpm out of the tarball (default 0 for *-webgpu, which copy
#                           them to OUT_DIR as separate files, 1 otherwise as in CI)
#   env BUILD_TYPE=RelWithDebInfo
#   env EXTRA_CMAKE_ARGS="..."  appended to the cmake configure line
#
# arm64 is built natively in an arm64 container; x86_64 is cross-compiled from
# that same container with gcc-14-x86-64-linux-gnu and :amd64 multiarch libs
# (CMake/platforms/x86_64-linux-gnu.toolchain.cmake). On an x86_64 host the
# roles reverse: use Packaging/nix/debian-cross-aarch64-prep.sh as CI does.
set -euo pipefail

ARCH="${1:-all}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${2:-${REPO}/out/linux}"
IMAGE="damnationx-linux-builder:24.04"
JOBS="${JOBS:-6}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
EXTRA_CMAKE_ARGS="${EXTRA_CMAKE_ARGS:-}"
DAWN_BUILD_ROOT="${DAWN_BUILD_ROOT:-${REPO}/build-linux-dawn}"
DAWN_DIR="${DAWN_DIR:-}"

docker build --platform linux/arm64 -t "$IMAGE" "${REPO}/tools/linux-docker"
mkdir -p "$OUT_DIR"

build_one() {
	local arch="$1" build_dir triple cmake_extra suffix="" strip_tool=strip objcopy_tool=objcopy strip_default=0
	local x64_toolchain="${REPO}/CMake/platforms/x86_64-linux-gnu.toolchain.cmake"
	# SDL3 + native WebGPU presenter (Dawn/Vulkan): SDL3/SDL3_image are built from source and linked
	# statically. The first configure fetches and builds Dawn under DAWN_BUILD_ROOT (one install per
	# arch); DAWN_JOBS caps it so the Docker VM does not run out of memory.
	local webgpu_cmake="-DUSE_SDL3=ON -DDEVILUTIONX_SYSTEM_SDL3=OFF -DDEVILUTIONX_STATIC_SDL3=ON -DDEVILUTIONX_SYSTEM_SDL_IMAGE=OFF -DDEVILUTIONX_WEBGPU=ON -DDEVILUTIONX_DAWN_JOBS=${DAWN_JOBS:-4} -DDEVILUTIONX_DAWN_BUILD_ROOT=${DAWN_BUILD_ROOT}"
	[[ -n "$DAWN_DIR" ]] && webgpu_cmake+=" -DDEVILUTIONX_DAWN_DIR=${DAWN_DIR}"
	case "$arch" in
	arm64)
		build_dir=build-linux-arm64
		triple=aarch64-linux-gnu
		# CI's aarch64 job does not enable Discord either.
		cmake_extra=""
		;;
	arm64-sdl3)
		# Experimental: not a CI configuration. SDL3/SDL3_image built from source, linked statically.
		build_dir=build-linux-arm64-sdl3
		triple=aarch64-linux-gnu
		suffix=-sdl3
		cmake_extra="-DUSE_SDL3=ON -DDEVILUTIONX_SYSTEM_SDL3=OFF -DDEVILUTIONX_STATIC_SDL3=ON -DDEVILUTIONX_SYSTEM_SDL_IMAGE=OFF"
		;;
	arm64-webgpu)
		build_dir=build-linux-arm64-webgpu
		triple=aarch64-linux-gnu
		suffix=-webgpu
		strip_default=1
		cmake_extra="$webgpu_cmake"
		;;
	x64)
		build_dir=build-linux-x64
		triple=x86_64-linux-gnu
		strip_tool=x86_64-linux-gnu-strip objcopy_tool=x86_64-linux-gnu-objcopy
		cmake_extra="-DCMAKE_TOOLCHAIN_FILE=${x64_toolchain} -DDISCORD_INTEGRATION=ON"
		;;
	x64-webgpu)
		# Cross-compiled like x64; CMake/Dawn.cmake forwards the toolchain file to the nested Dawn build.
		build_dir=build-linux-x64-webgpu
		triple=x86_64-linux-gnu
		suffix=-webgpu
		strip_default=1
		strip_tool=x86_64-linux-gnu-strip objcopy_tool=x86_64-linux-gnu-objcopy
		cmake_extra="-DCMAKE_TOOLCHAIN_FILE=${x64_toolchain} $webgpu_cmake"
		;;
	*) echo "unknown arch $arch" >&2; exit 1 ;;
	esac

	# Mount the checkout at its host path, plus the git common dir when it lives
	# outside it (git worktrees), so CMake can read the commit hash.
	local git_common git_mount=()
	git_common="$(cd "$REPO" && cd "$(git rev-parse --git-common-dir)" && pwd)"
	case "$git_common" in
	"$REPO"/*) ;;
	*) git_mount=(-v "${git_common}:${git_common}:ro") ;;
	esac
	# Dawn lives outside the checkout when DAWN_BUILD_ROOT / DAWN_DIR point elsewhere.
	local d
	for d in "$DAWN_BUILD_ROOT" "$DAWN_DIR"; do
		[[ -z "$d" || "$d" == "$REPO"/* ]] && continue
		mkdir -p "$d"
		git_mount+=(-v "${d}:${d}")
	done

	docker run --rm --platform linux/arm64 \
		-v "${REPO}:${REPO}" "${git_mount[@]}" -w "${REPO}" \
		-e SDL_AUDIODRIVER=dummy -e SDL_VIDEODRIVER=dummy \
		-e STRIP_PACKAGE="${STRIP_PACKAGE:-$strip_default}" -e STRIP="$strip_tool" -e OBJCOPY="$objcopy_tool" \
		-e TARBALL_DEB_RPM="${TARBALL_DEB_RPM:-$((1 - strip_default))}" \
		"$IMAGE" bash -euxo pipefail -c "
			cmake -S. -B${build_dir} -G Ninja -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DCMAKE_INSTALL_PREFIX=/usr -DCPACK=ON \
				-DBUILD_TESTING=OFF -DDEVILUTIONX_SYSTEM_LIBSODIUM=OFF -DDEVILUTIONX_SYSTEM_BZIP2=OFF \
				${cmake_extra} ${EXTRA_CMAKE_ARGS}
			cmake --build ${build_dir} -j ${JOBS} --target package
			rm -rf ${build_dir}/package
			Packaging/nix/LinuxReleasePackaging.sh ${build_dir}
			mv devilutionx.tar.xz ${build_dir}/devilutionx-${triple}${suffix}.tar.xz
			{
				echo 'NEEDED (DT_NEEDED of ${build_dir}/package/devilutionx):'
				readelf -d ${build_dir}/package/devilutionx | sed -n 's/.*Shared library: \\[\\(.*\\)\\]/  \\1/p'
				echo -n 'Newest GLIBC symbol version: '
				readelf -W --dyn-syms ${build_dir}/package/devilutionx | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n1
				echo -n 'Newest GLIBCXX symbol version: '
				readelf -W --dyn-syms ${build_dir}/package/devilutionx | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -n1
			} | tee ${build_dir}/runtime-deps.txt
		"
	cp "${REPO}/${build_dir}/devilutionx-${triple}${suffix}.tar.xz" "$OUT_DIR/"
	cp "${REPO}/${build_dir}/package/devilutionx" "$OUT_DIR/devilutionx-${triple}${suffix}"
	cp "${REPO}/${build_dir}/runtime-deps.txt" "$OUT_DIR/devilutionx-${triple}${suffix}.runtime-deps.txt"
	if [[ -f "${REPO}/${build_dir}/devilutionx.debug" ]]; then
		cp "${REPO}/${build_dir}/devilutionx.debug" "$OUT_DIR/devilutionx-${triple}${suffix}.debug"
	fi
	if [[ "$arch" == *-webgpu ]]; then
		# WebGPU builds ship their .deb/.rpm next to the tarball rather than inside it.
		for f in "${REPO}/${build_dir}/"devilutionx*.deb "${REPO}/${build_dir}/"devilutionx*.rpm; do
			[[ -f "$f" ]] || continue
			local base="${f##*/}"
			cp "$f" "$OUT_DIR/${base%.*}${suffix}.${base##*.}"
		done
	fi
	[[ -n "$suffix" ]] && return 0
	ls "${REPO}/${build_dir}/"devilutionx*.deb >/dev/null 2>&1 && cp "${REPO}/${build_dir}/"devilutionx*.deb "$OUT_DIR/" || true
	ls "${REPO}/${build_dir}/"devilutionx*.rpm >/dev/null 2>&1 && cp "${REPO}/${build_dir}/"devilutionx*.rpm "$OUT_DIR/" || true
}

case "$ARCH" in
all) build_one arm64; build_one x64 ;;
webgpu) build_one arm64-webgpu; build_one x64-webgpu ;;
arm64 | x64 | arm64-sdl3 | arm64-webgpu | x64-webgpu) build_one "$ARCH" ;;
*) echo "usage: $0 [arm64|x64|all|arm64-sdl3|arm64-webgpu|x64-webgpu|webgpu] [OUT_DIR]" >&2; exit 1 ;;
esac

ls -la "$OUT_DIR"
