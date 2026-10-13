# Ubuntu 24.04 arm64 builder for DevilutionX Linux packages.
#  - native aarch64 build (deps from Packaging/nix/debian-host-prep.sh, gcc-14)
#  - x86_64 cross build (gcc-14-x86-64-linux-gnu + :amd64 multiarch libs)
# smpq comes from :amd64 (run through Docker Desktop binfmt emulation): the
# arm64 smpq 1.6 / StormLib 9.22 in noble aborts with
# "Assertion `sizeof(hf->hctx) >= sizeof(hash_state)' failed" when packing.
# Mirrors .github/workflows/Linux_aarch64.yml / Linux_x86_64.yml.
# The X11/Wayland/audio/Vulkan -dev packages (native and :amd64) are only for the
# optional SDL3 and WebGPU builds (SDL3 is built from source there); python3 is
# for Dawn's build. xvfb + Mesa lavapipe (mesa-vulkan-drivers) + xdotool/xwd/
# netpbm let tools/linux-docker/hd-smoke-test.sh run the arm64 WebGPU build
# headless and screenshot it.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# arm64 image pulls from ports.ubuntu.com; amd64 packages live on archive.ubuntu.com.
RUN sed -i '/^Suites:/a Architectures: arm64' /etc/apt/sources.list.d/ubuntu.sources \
 && printf '%s\n' \
    'Types: deb' \
    'URIs: http://archive.ubuntu.com/ubuntu/' \
    'Suites: noble noble-updates noble-backports' \
    'Architectures: amd64' \
    'Components: main restricted universe multiverse' \
    'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg' \
    '' \
    'Types: deb' \
    'URIs: http://security.ubuntu.com/ubuntu/' \
    'Suites: noble-security' \
    'Architectures: amd64' \
    'Components: main restricted universe multiverse' \
    'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg' \
    > /etc/apt/sources.list.d/amd64.sources \
 && dpkg --add-architecture amd64 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
    ca-certificates build-essential ninja-build file xz-utils patch \
    rpm pkg-config cmake git gettext dpkg-dev python3 \
    smpq:amd64 \
    gcc-14 g++-14 \
    libsdl2-dev libsdl2-image-dev libsodium-dev libpng-dev libbz2-dev libspeechd-dev \
    gcc-14-x86-64-linux-gnu g++-14-x86-64-linux-gnu binutils-x86-64-linux-gnu \
    dpkg-cross libc6-dev-amd64-cross \
    libsdl2-dev:amd64 libsdl2-image-dev:amd64 libsodium-dev:amd64 \
    libpng-dev:amd64 libbz2-dev:amd64 libspeechd-dev:amd64 \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libgl-dev libgles-dev libdrm-dev libgbm-dev \
    libvulkan-dev libx11-xcb-dev libxcb1-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libdecor-0-dev libudev-dev libdbus-1-dev \
 && apt-get install -y --no-install-recommends \
    libx11-dev:amd64 libxext-dev:amd64 libxrandr-dev:amd64 libxcursor-dev:amd64 libxi-dev:amd64 \
    libxss-dev:amd64 libxfixes-dev:amd64 libxtst-dev:amd64 libxkbcommon-dev:amd64 libwayland-dev:amd64 \
    libegl-dev:amd64 libgl-dev:amd64 libgles-dev:amd64 libdrm-dev:amd64 libgbm-dev:amd64 libvulkan-dev:amd64 \
    libx11-xcb-dev:amd64 libxcb1-dev:amd64 libasound2-dev:amd64 libpulse-dev:amd64 libpipewire-0.3-dev:amd64 \
    libdecor-0-dev:amd64 libudev-dev:amd64 libdbus-1-dev:amd64 \
    xvfb xauth x11-utils x11-apps xdotool netpbm mesa-vulkan-drivers libvulkan1 vulkan-tools \
 && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 --slave /usr/bin/g++ g++ /usr/bin/g++-14 \
 && update-alternatives --install /usr/bin/x86_64-linux-gnu-gcc x86_64-linux-gnu-gcc /usr/bin/x86_64-linux-gnu-gcc-14 100 \
      --slave /usr/bin/x86_64-linux-gnu-g++ x86_64-linux-gnu-g++ /usr/bin/x86_64-linux-gnu-g++-14 \
 && rm -rf /var/lib/apt/lists/*

RUN git config --global --add safe.directory '*'
