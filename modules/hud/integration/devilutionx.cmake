# Opt in from the host after defining its target. No native TU is added.
if(NOT TARGET diablo_hud::core)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/diablo-hud)
endif()
# target_link_libraries(<host-target> PRIVATE diablo_hud::core)
# The host adapter supplies output/coverage/unlit stamps, UI/input ownership,
# renderer lifecycle and profile I/O. No SDL renderer pointer is required.
