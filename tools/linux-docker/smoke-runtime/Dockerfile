# Runtime-only image for tools/linux-docker/hd-smoke-test.sh on a non-native architecture
# (x86_64 tarballs on an arm64 Docker host, run under Docker Desktop's emulation): Xvfb,
# Mesa lavapipe (software Vulkan), xdotool/xwd/netpbm, and the release binary's runtime libs.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    xvfb xauth x11-utils x11-apps xdotool netpbm mesa-vulkan-drivers libvulkan1 vulkan-tools \
    libpng16-16t64 zlib1g libstdc++6 xz-utils ca-certificates \
 && rm -rf /var/lib/apt/lists/*
