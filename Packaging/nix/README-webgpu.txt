
# WebGPU / HD build
This build presents through WebGPU (Dawn on Vulkan) and can show the HD art.
SDL3 and SDL3_image are built in, so ignore "Install libsdl2" above: no SDL package is needed.

Runtime requirements:
 - glibc 2.38 or newer and libstdc++ from GCC 13 or newer (Ubuntu 24.04, Debian 13, Fedora 39 or later)
 - libpng16 and zlib (installed on practically every desktop)
 - The Vulkan loader (libvulkan1 / vulkan-loader) and a Vulkan driver for your GPU:
   Mesa (mesa-vulkan-drivers on Debian/Ubuntu/Fedora, vulkan-radeon / vulkan-intel on Arch)
   or the NVIDIA proprietary driver.
   Check with: vulkaninfo --summary
 - An X11 or Wayland session (SDL picks one at runtime)

HD art: put the `hd` folder next to the devilutionx binary (or in ~/.local/share/diasurgical/devilution/).
Run with --no-webgpu to fall back to the classic SDL renderer if Vulkan is unavailable.
