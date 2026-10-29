# platform/nativeee/tests -- one ctest per header over the host bodies (te_platform_host) with a 32 MB test memory in
# place of the runner's binding (te_test_env.cpp) and a GENERATED ISO 9660 fixture (te_test_iso.cpp: byte patterns,
# no game data). The weak seam stubs answer the owners' imports that have no body yet (graphics, math ...).
#
#   cmake --build build --target te_tests && ctest --test-dir build -L nativeee --output-on-failure
add_library(te_test_support STATIC te_test_env.cpp te_test_iso.cpp)
target_include_directories(te_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(te_test_support PUBLIC te_platform_host)
target_compile_options(te_test_support PRIVATE -Wall -Wextra)

set(TE_TESTS test_time test_pads test_disc_files test_saves test_system_io test_audio test_sound test_stream test_movie test_savestate test_harness test_graphics_screen_models test_mods_folder test_game_folder test_disc_overlay test_mods_page test_load_pipeline)
add_custom_target(te_tests)
foreach(t ${TE_TESTS})
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE te_test_support "$<LINK_LIBRARY:WHOLE_ARCHIVE,te_platform_host>" nativeee_libs te_seam_stubs)
    target_compile_options(${t} PRIVATE -Wall -Wextra)
    add_dependencies(te_tests ${t})
    add_test(NAME nativeee.${t} COMMAND ${t})
    set_tests_properties(nativeee.${t} PROPERTIES LABELS nativeee TIMEOUT 120 ENVIRONMENT "TE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
endforeach()
# the game folder's rules are shared host code the runner compiles in (engine/app/host/game_folder.cpp)
target_sources(test_game_folder PRIVATE "${TE_ENGINE}/app/host/game_folder.cpp")

# te_text_shape_test: the text layer's Arabic shaping (platform/nativeee/wasm/text_shape.cpp: plain C++ compiled into the game
# module) built natively over the same source; with TE_SOURCE_DIR it also shapes every line of the shipped language mod's bank
# against the shipped font's codepoint table (mods/arabic/data/)
add_executable(te_text_shape_test test_text_shape.cpp "${CMAKE_SOURCE_DIR}/platform/nativeee/wasm/text_shape.cpp")
target_include_directories(te_text_shape_test PRIVATE "${CMAKE_SOURCE_DIR}/platform/nativeee")
target_compile_options(te_text_shape_test PRIVATE -Wall -Wextra)
add_dependencies(te_tests te_text_shape_test)
add_test(NAME nativeee.te_text_shape_test COMMAND te_text_shape_test)
set_tests_properties(nativeee.te_text_shape_test PROPERTIES LABELS nativeee TIMEOUT 60 ENVIRONMENT "TE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")

# engine/app/host/disc_identity (the launcher's and the game's disc identity): generated images and a generated table of
# known dumps always; the TE_TEST_ISO disc against the repo's tables when it is set (a modified PAL image)
add_executable(test_disc_identity test_disc_identity.cpp)
target_link_libraries(test_disc_identity PRIVATE te_test_support "$<LINK_LIBRARY:WHOLE_ARCHIVE,te_platform_host>" te_disc_identity nativeee_libs te_seam_stubs)
target_compile_options(test_disc_identity PRIVATE -Wall -Wextra)
add_dependencies(te_tests test_disc_identity)
add_test(NAME nativeee.test_disc_identity COMMAND test_disc_identity)
set_tests_properties(nativeee.test_disc_identity PROPERTIES LABELS nativeee TIMEOUT 300 ENVIRONMENT "TE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")

# engine/app/host/disc_languages (the launcher's two language rows) against the game's own lists (te_language.h) over generated
# discs; the TE_TEST_ISO disc and the North American one (TE_TEST_NTSC_ISO) when named
add_executable(test_disc_languages test_disc_languages.cpp)
target_link_libraries(test_disc_languages PRIVATE te_test_support "$<LINK_LIBRARY:WHOLE_ARCHIVE,te_platform_host>" te_disc_identity nativeee_libs te_seam_stubs)
target_compile_options(test_disc_languages PRIVATE -Wall -Wextra)
add_dependencies(te_tests test_disc_languages)
add_test(NAME nativeee.test_disc_languages COMMAND test_disc_languages)
set_tests_properties(nativeee.test_disc_languages PROPERTIES LABELS nativeee TIMEOUT 120 ENVIRONMENT "TE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")

# engine/app/host/mod_import (the mod importer the launcher and twinsanityevolved-modtool share) over generated discs: the
# library alone (no host bodies, no module), with the generated-ISO writer
add_executable(test_mod_import test_mod_import.cpp te_test_iso.cpp)
target_include_directories(test_mod_import PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(test_mod_import PRIVATE ps2r_mod_import)
target_compile_options(test_mod_import PRIVATE -Wall -Wextra)
add_dependencies(te_tests test_mod_import)
add_test(NAME nativeee.test_mod_import COMMAND test_mod_import)
set_tests_properties(nativeee.test_mod_import PROPERTIES LABELS nativeee TIMEOUT 300)

# engine/app/host/disc_regions (the region-aware loader: the table, the maps, the image laid out through a map): generated
# executables always; the real one with TE_TEST_ISO + TE_TEST_REGION_MAP_DIR (tools/judges/region_map_judge.py)
add_executable(test_region_map test_region_map.cpp)
target_link_libraries(test_region_map PRIVATE te_test_support "$<LINK_LIBRARY:WHOLE_ARCHIVE,te_platform_host>" te_disc_identity nativeee_libs te_seam_stubs)
target_compile_options(test_region_map PRIVATE -Wall -Wextra)
target_compile_definitions(test_region_map PRIVATE TE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")   # game/symbol_addrs.txt
add_dependencies(te_tests test_region_map)
add_test(NAME nativeee.test_region_map COMMAND test_region_map)
set_tests_properties(nativeee.test_region_map PROPERTIES LABELS nativeee TIMEOUT 120)
