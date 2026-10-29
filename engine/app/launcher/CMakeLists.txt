# app/launcher -- the engine's launcher: one application a player downloads, opens, points at their disc image and
# presses Play. Qt 6 Widgets. Title-neutral: everything about a game comes from the manifest a title ships
# (`title.json` and the disc tables beside it, read at run time); nothing of a title is compiled in. The finished game
# is downloaded from the title's releases; the launcher never compiles anything.
#
# Built in two ways:
#   * alone:  cmake -S engine/app/launcher -B build-launcher -DCMAKE_PREFIX_PATH=<Qt 6 prefix> && cmake --build build-launcher
#   * by the engine root with -DPS2R_LAUNCHER=ON (default OFF: no existing target, option or default changes when it is off).
cmake_minimum_required(VERSION 3.21)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(NativeEELauncher LANGUAGES C CXX)
    enable_testing()
    find_package(Python3 3.8 COMPONENTS Interpreter)
endif()
get_filename_component(PS2R_LAUNCHER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent Test)
# Qt's zip reader (QZipReader) is a private class of QtCore (QtGui before 6.7): the release zip is unpacked in-process.
find_package(Qt6 QUIET COMPONENTS CorePrivate GuiPrivate)
# cl.exe (the Qt MSVC kit's compiler) reads -Wall/-Wextra as malformed /W options: its own spelling there.
if(MSVC)
    set(PS2R_LAUNCHER_WARNINGS /W4 /WX)
else()
    set(PS2R_LAUNCHER_WARNINGS -Wall -Wextra -Werror)
endif()

# The disc identity the launcher shares with the game (app/host/disc_identity: the program, the region, and whether the
# whole image is a known retail dump; app/host/disc_languages: the text and speech languages the game offers for it) over the engine's ISO 9660 reader, SHA-256 and the image hashes (CRC-32, SHA-1), and
# the host shim the reader opens files through (the engine's ps2r_host sources). Plain C++, compiled here with the engine's
# own warning policy.
set(PS2R_LAUNCHER_ENGINE_SOURCES
    "${PS2R_LAUNCHER_ROOT}/app/host/disc_identity.cpp"
    "${PS2R_LAUNCHER_ROOT}/app/host/disc_json.cpp"
    "${PS2R_LAUNCHER_ROOT}/app/host/disc_image_hash.cpp"
    "${PS2R_LAUNCHER_ROOT}/app/host/disc_languages.cpp"
    "${PS2R_LAUNCHER_ROOT}/app/host/disc_regions.cpp"
    "${PS2R_LAUNCHER_ROOT}/engine/cdvd/iso_fs.cpp"
    "${PS2R_LAUNCHER_ROOT}/engine/core/hash/sha256.cpp"
    "${PS2R_LAUNCHER_ROOT}/engine/platform/platform.cpp")
if(WIN32)
    list(APPEND PS2R_LAUNCHER_ENGINE_SOURCES "${PS2R_LAUNCHER_ROOT}/engine/platform/host/host_windows.cpp")
else()
    list(APPEND PS2R_LAUNCHER_ENGINE_SOURCES "${PS2R_LAUNCHER_ROOT}/engine/platform/host/host_posix.cpp"
                                             "${PS2R_LAUNCHER_ROOT}/engine/platform/posix/fiber_posix.cpp")
endif()
add_library(ps2r_launcher_disc STATIC ${PS2R_LAUNCHER_ENGINE_SOURCES})
target_include_directories(ps2r_launcher_disc PUBLIC "${PS2R_LAUNCHER_ROOT}")
find_package(Threads REQUIRED)
target_link_libraries(ps2r_launcher_disc PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(ps2r_launcher_disc PUBLIC dbghelp psapi winmm)
endif()
if(APPLE)
    # posix/fiber_posix.cpp is the engine's ucontext fiber (deprecated on macOS); the engine builds it the same way
    target_compile_definitions(ps2r_launcher_disc PRIVATE _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
endif()

# The mod import library (app/host/mod_import: the stage's API, plain C++; the Mods page calls it on a worker thread).
# PS2R_LAUNCHER_IMPORT=auto takes the library when it is in the tree (its own app/host/mod_import/mod_import.cmake, given
# the disc library above for the engine's ISO reader and SHA-256), else the stub that refuses every artefact with a
# sentence; `stub` forces the stub. The page is the same either way.
set(PS2R_LAUNCHER_IMPORT "auto" CACHE STRING "the Mods page's importer: auto (the library when present) | stub")
set_property(CACHE PS2R_LAUNCHER_IMPORT PROPERTY STRINGS auto stub)
set(PS2R_LAUNCHER_IMPORT_CMAKE "${PS2R_LAUNCHER_ROOT}/app/host/mod_import/mod_import.cmake")
if(NOT PS2R_LAUNCHER_IMPORT STREQUAL "stub" AND EXISTS "${PS2R_LAUNCHER_IMPORT_CMAKE}")
    set(PS2R_LAUNCHER_IMPORT_KIND "library")
    set(MOD_IMPORT_ENGINE_DISC ps2r_launcher_disc)
    include("${PS2R_LAUNCHER_IMPORT_CMAKE}")
    set(PS2R_LAUNCHER_IMPORT_TARGET ps2r_mod_import)
else()
    set(PS2R_LAUNCHER_IMPORT_KIND "stub")
    add_library(ps2r_launcher_import_stub STATIC "${PS2R_LAUNCHER_ROOT}/app/host/mod_import/mod_import_stub.cpp")
    target_include_directories(ps2r_launcher_import_stub PUBLIC "${PS2R_LAUNCHER_ROOT}")
    target_compile_options(ps2r_launcher_import_stub PRIVATE ${PS2R_LAUNCHER_WARNINGS})
    set(PS2R_LAUNCHER_IMPORT_TARGET ps2r_launcher_import_stub)
endif()
message(STATUS "launcher: the Mods page's importer is the ${PS2R_LAUNCHER_IMPORT_KIND} (${PS2R_LAUNCHER_IMPORT_TARGET})")

# The launcher's classes as a library (the application and the tests link it).
add_library(ps2r_launcher_lib STATIC
    launcher_theme.cpp launcher_style.cpp title_manifest.cpp app_data.cpp release_catalogue.cpp release_installer.cpp
    game_folder.cpp disc_scanner.cpp disc_text.cpp game_library.cpp ini_settings.cpp process_runner.cpp game_process.cpp
    launcher_app.cpp main_window.cpp sidebar.cpp page_header.cpp flat_button.cpp segmented_control.cpp
    switch_control.cpp folder_row.cpp generated_artwork.cpp badge.cpp choice_card.cpp disc_choice_card.cpp
    game_card.cpp games_page.cpp empty_state.cpp mod_list.cpp mod_importer.cpp mod_row.cpp mods_page.cpp settings_row.cpp settings_page.cpp
    about_page.cpp progress_panel.cpp details_sheet.cpp sheet_overlay.cpp
    setup_sheet.cpp setup_disc_step.cpp setup_folder_step.cpp setup_target_step.cpp setup_install_step.cpp vita_folder.cpp
    launcher_version.cpp zip_unpack.cpp release_download.cpp self_update.cpp launcher_update_box.cpp
    launcher_theme.h launcher_style.h title_manifest.h app_data.h release_catalogue.h release_installer.h
    game_folder.h disc_scanner.h disc_text.h game_library.h ini_settings.h process_runner.h game_process.h
    launcher_app.h main_window.h sidebar.h page_header.h flat_button.h segmented_control.h
    switch_control.h folder_row.h generated_artwork.h badge.h choice_card.h disc_choice_card.h
    game_card.h games_page.h empty_state.h mod_list.h mod_importer.h mod_row.h mods_page.h settings_row.h settings_page.h
    about_page.h progress_panel.h details_sheet.h sheet_overlay.h
    setup_sheet.h setup_disc_step.h setup_folder_step.h setup_target_step.h setup_install_step.h vita_folder.h
    launcher_version.h zip_unpack.h release_download.h self_update.h launcher_update_box.h)
target_include_directories(ps2r_launcher_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ps2r_launcher_lib PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Concurrent ps2r_launcher_disc
                                               ${PS2R_LAUNCHER_IMPORT_TARGET})
target_compile_definitions(ps2r_launcher_lib PUBLIC PS2R_LAUNCHER_IMPORT_${PS2R_LAUNCHER_IMPORT_KIND}=1)
if(TARGET Qt6::CorePrivate)
    target_link_libraries(ps2r_launcher_lib PRIVATE Qt6::CorePrivate)
endif()
if(TARGET Qt6::GuiPrivate)
    target_link_libraries(ps2r_launcher_lib PRIVATE Qt6::GuiPrivate)
endif()
target_compile_options(ps2r_launcher_lib PRIVATE ${PS2R_LAUNCHER_WARNINGS})
# The version this launcher is made as (the packaging passes the tree's describe): read by ONE source file, so that a new
# version rebuilds one object (the self-update test makes two launchers from one build directory).
if(DEFINED PS2R_LAUNCHER_VERSION)
    set_source_files_properties(launcher_version.cpp PROPERTIES COMPILE_DEFINITIONS "PS2R_LAUNCHER_VERSION=\"${PS2R_LAUNCHER_VERSION}\"")
endif()

# The application. Named by the engine (ps2r_launcher); a title's packaging wraps it in a bundle carrying the
# title's name and manifest (Contents/Resources/titles/*.json on macOS, titles/*.json beside the .exe).
add_executable(ps2r_launcher MACOSX_BUNDLE main.cpp)
target_link_libraries(ps2r_launcher PRIVATE ps2r_launcher_lib)
target_compile_options(ps2r_launcher PRIVATE ${PS2R_LAUNCHER_WARNINGS})
set_target_properties(ps2r_launcher PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Launcher"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.nativeee.launcher"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")

# The tests (tests/app/launcher/): the models without a window; every screen offscreen (and, with PS2R_SCREENS_OUT, the
# documentation's screenshots); the Settings rows against ps2r.ini (the settings judge's driver); the cold run against a
# release server (the cold judge's driver, registered only when a judge runs it). A small HTTP server stands in for the
# releases in the screens test (fixture_release_server: offline-safe, 127.0.0.1 only).
add_library(ps2r_launcher_test_support STATIC "${PS2R_LAUNCHER_ROOT}/tests/app/launcher/fixture_release_server.cpp"
                                              "${PS2R_LAUNCHER_ROOT}/tests/app/launcher/fixture_release_server.h")
target_include_directories(ps2r_launcher_test_support PUBLIC "${PS2R_LAUNCHER_ROOT}/tests/app/launcher")
target_link_libraries(ps2r_launcher_test_support PUBLIC Qt6::Core Qt6::Network)
target_compile_options(ps2r_launcher_test_support PRIVATE ${PS2R_LAUNCHER_WARNINGS})

add_executable(test_launcher_models "${PS2R_LAUNCHER_ROOT}/tests/app/launcher/test_launcher_models.cpp")
target_link_libraries(test_launcher_models PRIVATE ps2r_launcher_lib ps2r_launcher_test_support Qt6::Test)
target_compile_options(test_launcher_models PRIVATE ${PS2R_LAUNCHER_WARNINGS})
add_test(NAME LauncherModels COMMAND test_launcher_models -platform offscreen)
set_tests_properties(LauncherModels PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_launcher_cold "${PS2R_LAUNCHER_ROOT}/tests/app/launcher/test_launcher_cold.cpp")
target_link_libraries(test_launcher_cold PRIVATE ps2r_launcher_lib Qt6::Test)
target_compile_options(test_launcher_cold PRIVATE ${PS2R_LAUNCHER_WARNINGS})

add_executable(test_launcher_settings "${PS2R_LAUNCHER_ROOT}/tests/app/launcher/test_launcher_settings.cpp")
target_link_libraries(test_launcher_settings PRIVATE ps2r_launcher_lib Qt6::Test)
target_compile_options(test_launcher_settings PRIVATE ${PS2R_LAUNCHER_WARNINGS})

# The Mods page against a mods folder (the mods judge's driver; on its own it makes its own folder): the list and its lines,
# the switches, the order, Remove, a drop and the import.
add_executable(test_launcher_mods "${PS2R_LAUNCHER_ROOT}/tests/app/launcher/test_launcher_mods.cpp")
target_link_libraries(test_launcher_mods PRIVATE ps2r_launcher_lib Qt6::Test)
target_compile_options(test_launcher_mods PRIVATE ${PS2R_LAUNCHER_WARNINGS})
add_test(NAME LauncherMods COMMAND test_launcher_mods)
set_tests_properties(LauncherMods PROPERTIES TIMEOUT 300 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The launcher's own update: the newer-release rule, the file steps, the whole update of a bundle / a folder /
# an AppImage against the release server serving two launcher versions (the relaunch recorded, not run).
add_executable(test_launcher_self_update "${PS2R_LAUNCHER_ROOT}/tests/app/launcher/test_launcher_self_update.cpp")
target_link_libraries(test_launcher_self_update PRIVATE ps2r_launcher_lib ps2r_launcher_test_support Qt6::Test)
target_compile_options(test_launcher_self_update PRIVATE ${PS2R_LAUNCHER_WARNINGS})
add_test(NAME LauncherSelfUpdate COMMAND test_launcher_self_update)
set_tests_properties(LauncherSelfUpdate PROPERTIES TIMEOUT 300 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_launcher_screens "${PS2R_LAUNCHER_ROOT}/tests/app/launcher/test_launcher_screens.cpp")
target_link_libraries(test_launcher_screens PRIVATE ps2r_launcher_lib ps2r_launcher_test_support Qt6::Test)
target_compile_options(test_launcher_screens PRIVATE ${PS2R_LAUNCHER_WARNINGS})
# the tests that write zips (QZipWriter: Qt's private core / gui module, as the installer's reader)
foreach(t test_launcher_models test_launcher_screens test_launcher_self_update)
    if(TARGET Qt6::CorePrivate)
        target_link_libraries(${t} PRIVATE Qt6::CorePrivate)
    endif()
    if(TARGET Qt6::GuiPrivate)
        target_link_libraries(${t} PRIVATE Qt6::GuiPrivate)
    endif()
endforeach()
add_test(NAME LauncherScreens COMMAND test_launcher_screens)
set_tests_properties(LauncherScreens PROPERTIES TIMEOUT 300 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(Python3_EXECUTABLE)
    add_test(NAME LauncherOneClass
        COMMAND ${Python3_EXECUTABLE} ${PS2R_LAUNCHER_ROOT}/tests/lint/lint_one_class.py --root ${PS2R_LAUNCHER_ROOT} --scan app/launcher tests/app/launcher)
endif()
