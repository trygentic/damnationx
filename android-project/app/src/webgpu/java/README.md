The `org/libsdl/app` Java sources in this directory are copied with whitespace cleanup from
SDL revision `e442a9a5e1d6eb2621e9206f5176c53b0d034e09`, the same revision used by
`3rdParty/SDL3/CMakeLists.txt`. They are used only by the optional Android WebGPU
build (`./gradlew assembleDebug -Pwebgpu`). The default APK continues to use the
SDL2 Java sources in `src/main/java/org/libsdl/app`.

SDL is distributed under the zlib license; see the SDL source archive's `LICENSE.txt`.
