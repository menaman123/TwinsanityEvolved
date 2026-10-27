# platform/nativeee/tests -- one ctest per header over the host bodies (te_platform_host) with a 32 MB test memory in
# place of the runner's binding (te_test_env.cpp) and a GENERATED ISO 9660 fixture (te_test_iso.cpp: byte patterns,
# no game data). The weak seam stubs answer the owners' imports that have no body yet (graphics, math ...).
#
#   cmake --build build --target te_tests && ctest --test-dir build -L nativeee --output-on-failure
add_library(te_test_support STATIC te_test_env.cpp te_test_iso.cpp)
target_include_directories(te_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(te_test_support PUBLIC "$<LINK_LIBRARY:WHOLE_ARCHIVE,te_platform_host>")
target_compile_options(te_test_support PRIVATE -Wall -Wextra)

set(TE_TESTS test_time test_pads test_disc_files test_saves test_system_io test_audio test_sound test_stream test_movie test_savestate test_harness test_graphics_screen_models)
add_custom_target(te_tests)
foreach(t ${TE_TESTS})
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE te_test_support "$<LINK_LIBRARY:WHOLE_ARCHIVE,te_platform_host>" nativeee_libs te_seam_stubs)
    target_compile_options(${t} PRIVATE -Wall -Wextra)
    add_dependencies(te_tests ${t})
    add_test(NAME nativeee.${t} COMMAND ${t})
    set_tests_properties(nativeee.${t} PROPERTIES LABELS nativeee TIMEOUT 120 ENVIRONMENT "TE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
endforeach()
