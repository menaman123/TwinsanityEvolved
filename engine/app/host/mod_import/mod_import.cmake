# engine/app/host/mod_import/mod_import.cmake -- the mod importer as one static library, `ps2r_mod_import` (plain C++17, no Qt,
# desktop only: never built for the Vita). Included by the root CMakeLists (the command line tool twinsanityevolved-modtool
# and the tests) and by the launcher's CMakeLists (its Mods page); include it once per project.
#
# Before include(), optionally set:
#   MOD_IMPORT_ENGINE_ROOT   the engine tree (the folder holding engine/ and app/); default: two folders above this file's
#   MOD_IMPORT_GAME_DIR      the decomp's game/ (symbol_addrs.txt + its RETAIL() labels) for the code-change report's function
#                            names; default <engine root>/../game. Without it (or without Python) the table is empty and a
#                            changed code address is reported by itself.
#   MOD_IMPORT_ENGINE_DISC   a target that already compiles the engine's ISO reader, SHA-256 and host file layer
#                            (engine/cdvd/iso_fs.cpp, engine/core/hash/sha256.cpp, engine/platform/platform.cpp and the host
#                            arm): the launcher passes its ps2r_launcher_disc. Without it, `ps2r_mod_import_engine` is made here.
# Then link `ps2r_mod_import` and include "app/host/mod_import/mod_import.h".
if(TARGET ps2r_mod_import)
    return()
endif()
get_filename_component(_mi_here "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
if(NOT MOD_IMPORT_ENGINE_ROOT)
    get_filename_component(MOD_IMPORT_ENGINE_ROOT "${_mi_here}/../../.." ABSOLUTE)
endif()
if(NOT DEFINED MOD_IMPORT_GAME_DIR)
    get_filename_component(MOD_IMPORT_GAME_DIR "${MOD_IMPORT_ENGINE_ROOT}/../game" ABSOLUTE)
endif()
find_package(Threads REQUIRED)

if(NOT MOD_IMPORT_ENGINE_DISC)
    set(_mi_engine_sources
        "${MOD_IMPORT_ENGINE_ROOT}/engine/cdvd/iso_fs.cpp"
        "${MOD_IMPORT_ENGINE_ROOT}/engine/core/hash/sha256.cpp"
        "${MOD_IMPORT_ENGINE_ROOT}/engine/platform/platform.cpp")
    if(WIN32)
        list(APPEND _mi_engine_sources "${MOD_IMPORT_ENGINE_ROOT}/engine/platform/host/host_windows.cpp")
    else()
        list(APPEND _mi_engine_sources "${MOD_IMPORT_ENGINE_ROOT}/engine/platform/host/host_posix.cpp"
                                       "${MOD_IMPORT_ENGINE_ROOT}/engine/platform/posix/fiber_posix.cpp")
    endif()
    add_library(ps2r_mod_import_engine STATIC ${_mi_engine_sources})
    target_include_directories(ps2r_mod_import_engine PUBLIC "${MOD_IMPORT_ENGINE_ROOT}")
    target_link_libraries(ps2r_mod_import_engine PUBLIC Threads::Threads)
    set_target_properties(ps2r_mod_import_engine PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    if(WIN32)
        target_link_libraries(ps2r_mod_import_engine PUBLIC dbghelp psapi winmm)
    endif()
    if(APPLE)
        # posix/fiber_posix.cpp is the engine's ucontext fiber (deprecated on macOS); the engine builds it the same way
        target_compile_definitions(ps2r_mod_import_engine PRIVATE _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
    endif()
    set(MOD_IMPORT_ENGINE_DISC ps2r_mod_import_engine)
endif()

# The executable's function names (build time: game/symbol_addrs.txt + the decomp's RETAIL() labels; addresses and names only)
set(_mi_gen_script "${MOD_IMPORT_ENGINE_ROOT}/../tools/mods/gen_function_names.py")
if(NOT Python3_EXECUTABLE)
    find_package(Python3 3.8 COMPONENTS Interpreter QUIET)
endif()
if(Python3_EXECUTABLE AND EXISTS "${MOD_IMPORT_GAME_DIR}/symbol_addrs.txt" AND EXISTS "${_mi_gen_script}")
    file(GLOB_RECURSE _mi_decomp CONFIGURE_DEPENDS "${MOD_IMPORT_GAME_DIR}/include/*.h" "${MOD_IMPORT_GAME_DIR}/src/*.cpp"
                                                   "${MOD_IMPORT_GAME_DIR}/src/*.h")
    set(_mi_names "${CMAKE_BINARY_DIR}/gen/mod_import/function_names.cpp")
    add_custom_command(OUTPUT "${_mi_names}"
        COMMAND "${Python3_EXECUTABLE}" "${_mi_gen_script}" --game "${MOD_IMPORT_GAME_DIR}" --out "${_mi_names}"
        DEPENDS "${_mi_gen_script}" "${MOD_IMPORT_GAME_DIR}/symbol_addrs.txt" ${_mi_decomp}
        COMMENT "The executable's function names for the mod importer"
        VERBATIM)
else()
    set(_mi_names "${_mi_here}/function_names_none.cpp")
    message(STATUS "mod importer: no function-name table (Python or ${MOD_IMPORT_GAME_DIR}/symbol_addrs.txt missing): code changes are named by address")
endif()

# The releases' archive indexes (launcher/discs/archives/*.json: every archive entry's name and size, made by
# tools/discs/archive_index.py; a loose entry made from another release is told by them). Embedded at configure time
# as string pieces of at most 8000 characters (under every compiler's literal limit), joined at run time; no Python needed.
# MOD_IMPORT_ARCHIVE_TABLES overrides the folder; none found = an empty list (the check is then not made).
if(NOT DEFINED MOD_IMPORT_ARCHIVE_TABLES)
    get_filename_component(MOD_IMPORT_ARCHIVE_TABLES "${MOD_IMPORT_ENGINE_ROOT}/../launcher/discs/archives" ABSOLUTE)
endif()
file(GLOB _mi_tables CONFIGURE_DEPENDS "${MOD_IMPORT_ARCHIVE_TABLES}/*.json")
list(SORT _mi_tables)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_mi_tables})
set(_mi_tables_cpp "// Generated by mod_import.cmake from launcher/discs/archives/*.json -- do not edit.\nnamespace te::modimport::detail {\n")
set(_mi_table_list "")
set(_mi_n 0)
foreach(_mi_table ${_mi_tables})
    file(READ "${_mi_table}" _mi_json)
    string(LENGTH "${_mi_json}" _mi_len)
    string(APPEND _mi_tables_cpp "static const char* const kTable${_mi_n}[] = {\n")
    set(_mi_at 0)
    while(_mi_at LESS _mi_len)
        string(SUBSTRING "${_mi_json}" ${_mi_at} 8000 _mi_piece)
        string(APPEND _mi_tables_cpp "R\"TEJSON(${_mi_piece})TEJSON\",\n")
        math(EXPR _mi_at "${_mi_at} + 8000")
    endwhile()
    string(APPEND _mi_tables_cpp "nullptr};\n")
    string(APPEND _mi_table_list "kTable${_mi_n}, ")
    math(EXPR _mi_n "${_mi_n} + 1")
endforeach()
string(APPEND _mi_tables_cpp "extern const char* const* const kArchiveTables[];\nconst char* const* const kArchiveTables[] = {${_mi_table_list}nullptr};\n}  // namespace te::modimport::detail\n")
set(_mi_tables_out "${CMAKE_BINARY_DIR}/gen/mod_import/archive_tables.cpp")
file(WRITE "${_mi_tables_out}.in" "${_mi_tables_cpp}")
configure_file("${_mi_tables_out}.in" "${_mi_tables_out}" COPYONLY)
message(STATUS "mod importer: ${_mi_n} release archive table(s) built in (${MOD_IMPORT_ARCHIVE_TABLES})")

add_library(ps2r_mod_import STATIC
    "${_mi_here}/archive.cpp" "${_mi_here}/elf_diff.cpp" "${_mi_here}/hashes.cpp" "${_mi_here}/inflate.cpp" "${_mi_here}/io.cpp"
    "${_mi_here}/json.cpp" "${_mi_here}/mod_changes.cpp" "${_mi_here}/mod_import.cpp" "${_mi_here}/patches.cpp"
    "${_mi_here}/release_archives.cpp" "${_mi_here}/zip_reader.cpp" "${_mi_names}" "${_mi_tables_out}"
    # the game's own overlay: what Verify composes a mod through, and the base check's sampled hash
    "${MOD_IMPORT_ENGINE_ROOT}/engine/cdvd/disc_overlay.cpp")
target_include_directories(ps2r_mod_import PUBLIC "${MOD_IMPORT_ENGINE_ROOT}")
target_link_libraries(ps2r_mod_import PUBLIC ${MOD_IMPORT_ENGINE_DISC})
set_target_properties(ps2r_mod_import PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
# Warnings as errors under the compilers the sources were checked with (Apple clang; GCC 15 at -O3); cl.exe (the Qt MSVC kit
# the Windows launcher uses) was not available to check: its own level, not fatal
if(MSVC)
    target_compile_options(ps2r_mod_import PRIVATE /W3)
else()
    target_compile_options(ps2r_mod_import PRIVATE -Wall -Wextra -Werror)
endif()
