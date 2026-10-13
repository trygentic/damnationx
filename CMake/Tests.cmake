include(GoogleTest)

# Quality targets are deliberately named so native differential and
# renderer checks can be invoked independently of the public test aggregate.
set(QUALITY_NATIVE_DIFFERENTIAL_TARGET quality_native_differential)
set(QUALITY_CANONICAL_TICK_STATE_TARGET quality_canonical_tick_state)
set(QUALITY_PERSISTENCE_INVARIANCE_TARGET quality_persistence_invariance)
set(QUALITY_RENDERER_GOLDEN_TARGET quality_renderer_goldens)
set(QUALITY_OWNED_DATA_TARGET quality_owned_data)

add_custom_target(${QUALITY_NATIVE_DIFFERENTIAL_TARGET}
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L "^quality-native$"
  USES_TERMINAL)
add_custom_target(${QUALITY_CANONICAL_TICK_STATE_TARGET}
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "^quality_canonical_tick_state$"
  USES_TERMINAL)
add_custom_target(${QUALITY_PERSISTENCE_INVARIANCE_TARGET}
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "^quality_persistence_invariance$"
  USES_TERMINAL)
add_custom_target(${QUALITY_RENDERER_GOLDEN_TARGET}
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "^renderer_test\\.|^palette_blending_test\\.|^quality_renderer_goldens$|^quality_.*_classic_goldens$|^DunRenderGoldenTest\\.|^ClxRenderGolden\\.|^ScrollrtGolden\\."
  USES_TERMINAL
  VERBATIM)
if(DEVILUTIONX_Q01_ENABLE_OWNED_DATA_TESTS)
  add_custom_target(${QUALITY_OWNED_DATA_TARGET})
endif()

add_test(NAME quality_native_differential
  COMMAND $<TARGET_FILE:timedemo_test> --gtest_filter=CanonicalTickState.*:WasmtimeWasmi.*)
set_tests_properties(quality_native_differential PROPERTIES LABELS "quality;native")
add_test(NAME quality_canonical_tick_state
  COMMAND $<TARGET_FILE:timedemo_test> --gtest_filter=CanonicalTickState.*)
set_tests_properties(quality_canonical_tick_state PROPERTIES LABELS "quality;native")
add_test(NAME quality_persistence_invariance
  COMMAND $<TARGET_FILE:timedemo_test> --gtest_filter=PersistenceInvariance.*)
set_tests_properties(quality_persistence_invariance PROPERTIES LABELS "quality;native")
set_tests_properties(quality_native_differential quality_canonical_tick_state quality_persistence_invariance
  PROPERTIES RESOURCE_LOCK timedemo_save_archive)
add_test(NAME quality_renderer_goldens
  COMMAND $<TARGET_FILE:palette_blending_test> --gtest_filter=ClassicGoldens.*:EnhancedGoldens.*)
set_tests_properties(quality_renderer_goldens PROPERTIES LABELS "quality;renderer")
# The classic byte-identity goldens of the 2.5D hybrid lighting: mode off must equal master.
add_test(NAME quality_dun_render_classic_goldens
  COMMAND $<TARGET_FILE:dun_render_golden_test> --gtest_filter=DunRenderGoldenTest.*)
add_test(NAME quality_clx_render_classic_goldens
  COMMAND $<TARGET_FILE:clx_render_golden_test> --gtest_filter=ClxRenderGolden.*)
add_test(NAME quality_scrollrt_classic_goldens
  COMMAND $<TARGET_FILE:scrollrt_golden_test> --gtest_filter=ScrollrtGolden.*)
set_tests_properties(quality_dun_render_classic_goldens quality_clx_render_classic_goldens quality_scrollrt_classic_goldens
  PROPERTIES LABELS "quality;renderer")
if(DEVILUTIONX_Q01_ENABLE_OWNED_DATA_TESTS)
  add_test(NAME quality_owned_data
    COMMAND $<TARGET_FILE:timedemo_test> --gtest_filter=OwnedData.*)
  set_tests_properties(quality_owned_data PROPERTIES LABELS "quality;owned-data")
endif()

include(functions/copy_files)

add_library(libdevilutionx_so SHARED)
set_target_properties(libdevilutionx_so PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

target_link_dependencies(libdevilutionx_so PUBLIC libdevilutionx)
set_target_properties(libdevilutionx_so PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_library(test_main OBJECT test/main.cpp)
target_link_dependencies(test_main PUBLIC libdevilutionx_so GTest::gtest GTest::gmock)

set(tests
  animationinfo_test
  appfat_test
  assets_test
  automap_test
  cursor_test
  dead_test
  diablo_test
  drlg_common_test
  drlg_l1_test
  drlg_l2_test
  drlg_l3_test
  drlg_l4_test
  effects_test
  inv_test
  items_test
  math_test
  missiles_test
  multi_logging_test
  pack_test
  player_test
  quests_test
  scrollrt_test
  stores_test
  tile_properties_test
  timedemo_test
  townerdat_test
  town_portal_test
  writehero_test
  vendor_test
  panel_state_test
  store_transaction_test
  visual_store_test
  stash_test
  inventory_ui_test
  spell_ui_test
  char_panel_test
  game_menu_test
  renderer_test
  scrollrt_golden_test
  ui_scale_hit_test
)
if(DEVILUTIONX_D2_SOUND_IMPORT)
  list(APPEND tests d2sound_test)
endif()
set(standalone_tests
  codec_test
  crawl_test
  data_file_test
  file_util_test
  format_int_test
  hd_floor_test
  ini_test
  mod_identity_test
  palette_blending_test
  parse_int_test
  path_test
  freeform_movement_test
  fpv_math_test
  vision_test
  random_test
  rectangle_test
  sheen_bidi_test
  static_vector_test
  str_cat_test
  utf8_test
  dvlnet_session_test
  gbuffer_test
  clx_normal_map_test
  tile_geometry_test
  hybrid_reference_test
  hd_sprites_test
  hd_tiles_test
  hd_ui_test
  hd_sprites_monster_test
  hd_items_test
  hd_objects_test
  hd_view_transform_test
  gltf_model_test
  ui_scale_test
  dun_render_golden_test
  clx_render_golden_test
)
if(NOT USE_SDL1)
  list(APPEND standalone_tests text_render_integration_test)
endif()
set(benchmarks
  clx_render_benchmark
  crawl_benchmark
  dun_render_benchmark
  light_render_benchmark
  palette_blending_benchmark
  path_benchmark
  gbuffer_benchmark
  clx_normal_map_benchmark
)

include(test/Fixtures.cmake)

foreach(test_target ${tests} ${standalone_tests} ${benchmarks})
  add_executable(${test_target} "test/${test_target}.cpp")
  set_target_properties(${test_target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  if(GPERF)
    target_link_libraries(${test_target} PUBLIC ${GPERFTOOLS_LIBRARIES})
  endif()
endforeach()

foreach(test_target ${tests} ${standalone_tests})
  if(test_target STREQUAL "timedemo_test")
    # Every timedemo case replays into the same save archive, so they must not run concurrently.
    gtest_discover_tests(${test_target} PROPERTIES RESOURCE_LOCK timedemo_save_archive)
  else()
    gtest_discover_tests(${test_target})
  endif()
endforeach()

foreach(test_target ${tests})
  target_link_libraries(${test_target} PRIVATE test_main)
endforeach()

foreach(test_target ${standalone_tests})
  # Standalone tests also use GoogleMock matchers. gtest_main does not
  # propagate GoogleMock's include directories or library dependencies.
  target_link_libraries(${test_target} PRIVATE GTest::gtest_main GTest::gmock)
  target_include_directories(${test_target} PRIVATE "${PROJECT_SOURCE_DIR}/Source")
endforeach()

foreach(target ${benchmarks})
  target_link_libraries(${target} PRIVATE benchmark::benchmark benchmark::benchmark_main)
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/Source")
endforeach()

add_library(app_fatal_for_testing OBJECT test/app_fatal_for_testing.cpp)
target_sources(app_fatal_for_testing INTERFACE $<TARGET_OBJECTS:app_fatal_for_testing>)

add_library(language_for_testing OBJECT test/language_for_testing.cpp)
target_sources(language_for_testing INTERFACE $<TARGET_OBJECTS:language_for_testing>)

target_link_dependencies(codec_test PRIVATE libdevilutionx_codec app_fatal_for_testing)

add_custom_target(clx_render_benchmark_resources
  DEPENDS
  "${DEVILUTIONX_ASSETS_OUTPUT_DIRECTORY}/data/resistance.clx"
  "${DEVILUTIONX_ASSETS_OUTPUT_DIRECTORY}/ui_art/dvl_lrpopup.clx"
)
add_dependencies(clx_render_benchmark clx_render_benchmark_resources)
target_link_dependencies(clx_render_benchmark
  PRIVATE
  DevilutionX::SDL
  tl
  app_fatal_for_testing
  language_for_testing
  libdevilutionx_clx_render
  libdevilutionx_load_clx
  libdevilutionx_log
  libdevilutionx_surface
)
target_link_dependencies(crawl_test PRIVATE libdevilutionx_crawl)
target_link_dependencies(crawl_benchmark PRIVATE libdevilutionx_crawl)
target_link_dependencies(data_file_test PRIVATE libdevilutionx_txtdata app_fatal_for_testing language_for_testing)
target_link_dependencies(dun_render_benchmark
  PRIVATE
  libdevilutionx_assets
  libdevilutionx_dun_tile_data
  libdevilutionx_tile_geometry
  libdevilutionx_light_tables
  libdevilutionx_dun_render
  libdevilutionx_options
  app_fatal_for_testing
  language_for_testing
  tl
  unordered_dense::unordered_dense
)
target_link_dependencies(file_util_test PRIVATE libdevilutionx_file_util app_fatal_for_testing)
target_link_dependencies(format_int_test PRIVATE libdevilutionx_format_int language_for_testing)
target_link_dependencies(hd_floor_test PRIVATE libdevilutionx_hd_floor)
target_link_dependencies(ini_test PRIVATE libdevilutionx_ini app_fatal_for_testing)
target_link_dependencies(mod_identity_test PRIVATE libdevilutionx_mod_identity app_fatal_for_testing)
target_include_directories(mod_identity_test PRIVATE "${PROJECT_SOURCE_DIR}/3rdParty/PicoSHA2")
target_link_dependencies(light_render_benchmark PRIVATE libdevilutionx_light_render DevilutionX::SDL libdevilutionx_surface libdevilutionx_paths app_fatal_for_testing)
target_link_dependencies(palette_blending_test PRIVATE libdevilutionx_palette_blending DevilutionX::SDL libdevilutionx_strings GTest::gmock app_fatal_for_testing)
target_link_dependencies(palette_blending_benchmark
  PRIVATE
  DevilutionX::SDL
  libdevilutionx_palette_blending
  libdevilutionx_palette_kd_tree
  app_fatal_for_testing
)
target_link_dependencies(parse_int_test PRIVATE libdevilutionx_parse_int)
target_link_dependencies(path_test PRIVATE libdevilutionx_pathfinding libdevilutionx_direction app_fatal_for_testing)
target_link_dependencies(freeform_movement_test PRIVATE libdevilutionx_freeform_movement)
target_link_dependencies(fpv_math_test PRIVATE libdevilutionx_fpv_math)
target_link_dependencies(vision_test PRIVATE libdevilutionx_vision)
target_link_dependencies(path_benchmark PRIVATE libdevilutionx_pathfinding app_fatal_for_testing)
target_link_dependencies(random_test PRIVATE libdevilutionx_random)
target_link_dependencies(static_vector_test PRIVATE libdevilutionx_random app_fatal_for_testing)
target_link_dependencies(str_cat_test PRIVATE libdevilutionx_strings)
if(DEVILUTIONX_SCREENSHOT_FORMAT STREQUAL DEVILUTIONX_SCREENSHOT_FORMAT_PNG AND NOT USE_SDL1)
  target_link_dependencies(text_render_integration_test
    PRIVATE
    DevilutionX::SDL
    GTest::gmock
    GTest::gtest
    tl
    app_fatal_for_testing
    language_for_testing
    libdevilutionx_primitive_render
    libdevilutionx_strings
    libdevilutionx_surface
    libdevilutionx_surface_to_png
    libdevilutionx_text_render
  )
  copy_files(
    FILES
      basic-colors.png
      basic.png
      horizontal_overflow.png
      horizontal_overflow-colors.png
      kerning_fit_spacing-colors.png
      kerning_fit_spacing.png
      kerning_fit_spacing__align_center-colors.png
      kerning_fit_spacing__align_center.png
      kerning_fit_spacing__align_center__newlines.png
      kerning_fit_spacing__align_center__newlines_in_fmt-colors.png
      kerning_fit_spacing__align_center__newlines_in_value-colors.png
      kerning_fit_spacing__align_right-colors.png
      kerning_fit_spacing__align_right.png
      vertical_overflow.png
      vertical_overflow-colors.png
      cursor-start.png
      cursor-middle.png
      cursor-end.png
      multiline_cursor-end_first_line.png
      multiline_cursor-start_second_line.png
      multiline_cursor-middle_second_line.png
      multiline_cursor-end_second_line.png
      highlight-partial.png
      highlight-full.png
      multiline_highlight.png
    SRC_PREFIX test/fixtures/text_render_integration_test/
    OUTPUT_DIR "${DEVILUTIONX_TEST_FIXTURES_OUTPUT_DIRECTORY}/text_render_integration_test"
    OUTPUT_VARIABLE _text_render_integration_test_fixtures
  )
  add_custom_target(text_render_integration_test_resources
    DEPENDS
    "${DEVILUTIONX_ASSETS_OUTPUT_DIRECTORY}/fonts/12-00.clx"
    "${DEVILUTIONX_ASSETS_OUTPUT_DIRECTORY}/fonts/goldui.trn"
    "${DEVILUTIONX_ASSETS_OUTPUT_DIRECTORY}/fonts/golduis.trn"
    "${DEVILUTIONX_ASSETS_OUTPUT_DIRECTORY}/fonts/grayuis.trn"
    "${DEVILUTIONX_ASSETS_OUTPUT_DIRECTORY}/fonts/grayui.trn"
    "${DEVILUTIONX_ASSETS_OUTPUT_DIRECTORY}/ui_art/diablo.pal"
    ${_text_render_integration_test_fixtures}
  )
  add_dependencies(text_render_integration_test text_render_integration_test_resources)
endif()
target_link_dependencies(sheen_bidi_test PRIVATE libdevilutionx_sheen_bidi)
target_link_dependencies(utf8_test PRIVATE libdevilutionx_utf8)

target_include_directories(writehero_test PRIVATE 3rdParty/PicoSHA2)

# ---- 2.5D hybrid lighting ----
# Standalone tests copy their own fixtures: devilutionx_copied_fixtures only hangs off test_main.
function(add_hybrid_test_fixtures test_target fixture_dir)
  copy_files(
    FILES ${ARGN}
    SRC_PREFIX "test/fixtures/${fixture_dir}/"
    OUTPUT_DIR "${DEVILUTIONX_TEST_FIXTURES_OUTPUT_DIRECTORY}/${fixture_dir}"
    OUTPUT_VARIABLE _${test_target}_fixtures
  )
  add_custom_target(${test_target}_resources DEPENDS ${_${test_target}_fixtures})
  add_dependencies(${test_target} ${test_target}_resources)
endfunction()

target_link_dependencies(gbuffer_test PRIVATE libdevilutionx_gbuffer libdevilutionx_surface app_fatal_for_testing)
target_link_dependencies(gbuffer_benchmark PRIVATE libdevilutionx_gbuffer app_fatal_for_testing)

target_link_dependencies(clx_normal_map_test
  PRIVATE
  libdevilutionx_clx_normal_map
  libdevilutionx_load_clx
  libdevilutionx_log
  app_fatal_for_testing
  language_for_testing
)
add_hybrid_test_fixtures(clx_normal_map_test normal_map README.md disc.clx ramp.clx)
target_link_dependencies(clx_normal_map_benchmark
  PRIVATE
  libdevilutionx_clx_normal_map
  libdevilutionx_load_clx
  libdevilutionx_log
  app_fatal_for_testing
  language_for_testing
)
add_dependencies(clx_normal_map_benchmark clx_normal_map_test_resources)

target_link_dependencies(tile_geometry_test PRIVATE libdevilutionx_tile_geometry app_fatal_for_testing language_for_testing)
add_hybrid_test_fixtures(tile_geometry_test tile_geometry README.md synthetic_level.bin)

target_link_dependencies(hybrid_reference_test PRIVATE libdevilutionx_hybrid_reference)
target_link_dependencies(hd_sprites_test PRIVATE libdevilutionx_hd_sprites_core)
target_link_dependencies(hd_tiles_test PRIVATE libdevilutionx_hd_tiles_core)
target_link_dependencies(hd_ui_test PRIVATE libdevilutionx_hd_ui_core)
target_link_dependencies(hd_sprites_monster_test PRIVATE libdevilutionx_hd_sprites_core)
target_link_dependencies(hd_items_test PRIVATE libdevilutionx_hd_sprites_core)
target_link_dependencies(hd_objects_test PRIVATE libdevilutionx_hd_sprites_core)
target_link_dependencies(hd_view_transform_test PRIVATE libdevilutionx_hd_sprites_core)
target_link_dependencies(gltf_model_test PRIVATE libdevilutionx_gltf_model)
target_compile_definitions(gltf_model_test PRIVATE "WARRIOR_GLB_PATH=\"${PROJECT_SOURCE_DIR}/assets/3d/warrior/warrior.glb\"")
add_hybrid_test_fixtures(hybrid_reference_test hybrid README.md volume_synthetic.bin planes_synthetic.bin)

# Mirrors dun_render_benchmark.
target_link_dependencies(dun_render_golden_test
  PRIVATE
  libdevilutionx_assets
  libdevilutionx_dun_tile_data
  libdevilutionx_tile_geometry
  libdevilutionx_gbuffer
  libdevilutionx_light_tables
  libdevilutionx_dun_render
  libdevilutionx_options
  app_fatal_for_testing
  language_for_testing
  tl
  unordered_dense::unordered_dense
)
add_hybrid_test_fixtures(dun_render_golden_test dun_render README.md goldens.tsv frames.bin)

# Mirrors clx_render_benchmark (and reuses its copied assets).
add_dependencies(clx_render_golden_test clx_render_benchmark_resources)
target_link_dependencies(clx_render_golden_test
  PRIVATE
  DevilutionX::SDL
  tl
  app_fatal_for_testing
  language_for_testing
  libdevilutionx_clx_normal_map
  libdevilutionx_clx_render
  libdevilutionx_gbuffer
  libdevilutionx_load_clx
  libdevilutionx_log
  libdevilutionx_surface
)
add_hybrid_test_fixtures(clx_render_golden_test clx_render README.md goldens.tsv)

# WGSL validation: naga and/or tint when installed; exit 77 = skipped.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME wgsl_validate
    COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/wgsl_validate.py" "${PROJECT_SOURCE_DIR}/Source/engine/render/webgpu_shaders.hpp")
  set_tests_properties(wgsl_validate PROPERTIES SKIP_RETURN_CODE 77 LABELS "hybrid;wgsl")
endif()
