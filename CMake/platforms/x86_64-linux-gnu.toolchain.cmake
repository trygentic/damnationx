# Cross-compile for x86_64 Linux from another Debian/Ubuntu host (e.g. arm64)
# using gcc-x86-64-linux-gnu and :amd64 multiarch libraries.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER "/usr/bin/x86_64-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "/usr/bin/x86_64-linux-gnu-g++")

set(CMAKE_FIND_ROOT_PATH "/usr/x86_64-linux-gnu;/usr")
set(CMAKE_LIBRARY_ARCHITECTURE x86_64-linux-gnu)

set(CMAKE_STRIP "/usr/bin/x86_64-linux-gnu-strip")
set(PKG_CONFIG_EXECUTABLE "${CMAKE_CURRENT_LIST_DIR}/x86_64-linux-gnu-pkg-config" CACHE STRING "Path to pkg-config")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE amd64)
# rpmbuild would otherwise label the package with the build host's arch and run the
# host strip on foreign ELF files (e.g. discord_game_sdk.so).
set(CPACK_RPM_PACKAGE_ARCHITECTURE x86_64)
set(CPACK_RPM_SPEC_MORE_DEFINE "%define __strip ${CMAKE_STRIP}\n%define __objdump /usr/bin/x86_64-linux-gnu-objdump")
