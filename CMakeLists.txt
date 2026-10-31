cmake_minimum_required(VERSION 3.21)
project(TwinsanityEvolved LANGUAGES C CXX)
# The game fiber's switch is assembly on the POSIX arms (runner/te_fiber_switch.S: AAPCS64, SysV x86-64); the Windows x64
# arm is inline asm inside a C file (runner/te_fiber_win64.c), so the Windows lane (clang-cl) enables no assembler.
if(NOT WIN32)
    enable_language(ASM)
endif()

# TwinsanityEvolved -- native PC Crash Twinsanity: the twinsanity-reversed decomp (game/) compiled to wasm32,
# lowered with wasm2c, linked with the NativeEE engine (engine/, our branch of github.com/menaman123/NativeEE, pinned commit) through the include/platform seam
# implemented by platform/nativeee.
#
#   tools/fetch_toolchain.sh                      # once: pinned wasi-sdk + wabt into tools/toolchain/
#   cmake -S . -B build -G Ninja && cmake --build build
#   build/twinsanityevolved --iso <disc image> --dry-run

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
# The Windows lane: clang-cl through the windows-clang-cl-* presets (CMakePresets.json) and
# cmake/windows-clang-cl.toolchain.cmake. Every Windows difference in this tree sits under `if(WIN32)`; the macOS and
# Linux configurations are untouched by it.
if(WIN32)
    set(TE_EXE ".exe")
    # The UCRT and <windows.h> as the engine's tree sees them (engine/CMakeLists.txt, the same list): no "unsafe"
    # deprecation of the C library (fopen/getenv/strncpy -> *_s), the POSIX names (fileno, strdup, unlink), M_PI,
    # and a <windows.h> without the min/max macros or the GDI/winsock surface.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS _USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    set(TE_EXE "")
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
# The PlayStation Vita lane: the vita-* presets configure through cmake/vita.toolchain.cmake (VitaSDK's
# arm-vita-eabi GCC, newlib; armv7-a, 32-bit pointers) with TE_VITA=ON. Every Vita difference in this tree sits under
# `if(TE_VITA)`; the macOS, Linux and Windows configurations are untouched by it. What differs: no window and no GL 4.1
# adapter (the engine's ps2e draw surface is its NULL arm: draws counted, nothing rasterised), SDL2's Vita port for the
# pads (sceCtrl) and the audio device (sceAudioOut), FFmpeg built for the lane (tools/vita/ffmpeg_vita.sh: the engine's
# LGPL recipe cross-compiled), no save states / autopilot /
# probe (host-process features), the runner's arguments baked in (ux0:data/twinsanity/), and the eboot packed as a .vpk.
option(TE_VITA "The PlayStation Vita lane: cross-compiled through cmake/vita.toolchain.cmake (the vita-* presets)" OFF)
if(TE_VITA AND NOT VITA)
    message(FATAL_ERROR "TwinsanityEvolved: TE_VITA=ON needs the Vita toolchain -- configure with --preset vita-release (cmake/vita.toolchain.cmake)")
endif()
# TE_VITA_NATIVE (the Vita lane only; OFF = the wasm32 + wasm2c module as before): the game's code compiled NATIVELY for
# the Vita's armv7 by the Vita's GCC (cmake/native_lane.cmake, runner/te_native.h) instead of wasm32 -> wasm2c -> C. The
# desktop lanes keep the module (the decomp is ILP32: a 64-bit host cannot compile it natively).
option(TE_VITA_NATIVE "Vita: the game code compiled natively for ARM (no wasm2c module)" OFF)
if(TE_VITA_NATIVE AND NOT TE_VITA)
    message(FATAL_ERROR "TwinsanityEvolved: TE_VITA_NATIVE=ON is the Vita lane's (configure with --preset vita-native-release)")
endif()
if(TE_VITA_NATIVE)
    add_compile_definitions(TE_NATIVE=1)   # the runner and the host side (te_host.h's address model, runner/te_native.h)
endif()
find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)

set(TE_GAME "${CMAKE_SOURCE_DIR}/game")
# The engine: the vendored tree at engine/ (edited in-tree), or -DTE_ENGINE_DIR=<checkout>
# to build against a separate engine checkout -- a development override for trying an engine change before it is made
# in-tree. Never the default; never a path inside this tree other than engine/.
set(TE_ENGINE_DIR "" CACHE PATH "engine checkout to build against instead of the vendored engine/ tree (development only)")
if(TE_ENGINE_DIR)
    set(TE_ENGINE "${TE_ENGINE_DIR}")
    message(WARNING "TwinsanityEvolved: building against the engine at ${TE_ENGINE}, NOT the vendored engine/ tree (bring the change in-tree before integrating)")
else()
    set(TE_ENGINE "${CMAKE_SOURCE_DIR}/engine")
endif()
set(TE_TOOLS "${CMAKE_SOURCE_DIR}/tools")
set(TE_WASI_SDK "${TE_TOOLS}/toolchain/wasi-sdk" CACHE PATH "pinned wasi-sdk (tools/fetch_toolchain.sh)")
set(TE_WABT "${TE_TOOLS}/toolchain/wabt" CACHE PATH "pinned wabt (tools/fetch_toolchain.sh)")
if(WIN32)
    set(TE_FETCH_TOOLCHAIN "tools/fetch_toolchain.ps1")
else()
    set(TE_FETCH_TOOLCHAIN "tools/fetch_toolchain.sh")
endif()
foreach(tool "${TE_WASI_SDK}/bin/clang++${TE_EXE}" "${TE_WASI_SDK}/bin/wasm-ld${TE_EXE}" "${TE_WABT}/bin/wasm2c${TE_EXE}" "${TE_WABT}/bin/wasm-objdump${TE_EXE}")
    if(NOT EXISTS "${tool}" AND NOT TE_VITA_NATIVE)
        message(FATAL_ERROR "TwinsanityEvolved: ${tool} missing -- run ${TE_FETCH_TOOLCHAIN} (pinned wasi-sdk 24.0 + wabt 1.0.42)")
    endif()
endforeach()
if(NOT EXISTS "${TE_GAME}/include/platform/pads.h" OR NOT EXISTS "${TE_ENGINE}/CMakeLists.txt")
    message(FATAL_ERROR "TwinsanityEvolved: engine/ or game/ is missing -- the clone is incomplete (both are ordinary tracked trees of this repository; re-clone, or check TE_ENGINE_DIR if you set it)")
endif()

set(TE_GEN "${CMAKE_BINARY_DIR}/gen")
set(TE_WASM "${CMAKE_BINARY_DIR}/wasm")
set(TE_W2C "${CMAKE_BINARY_DIR}/w2c")
file(MAKE_DIRECTORY "${TE_GEN}/seam" "${TE_WASM}/obj" "${TE_WASM}/rep" "${TE_W2C}")

# ---------------------------------------------------------------------------------------------------------------------
# 1. The seam, generated from game/include/platform/*.h (tools/gen_seam.py) + tools/second_seam.tsv
# ---------------------------------------------------------------------------------------------------------------------
file(GLOB TE_SEAM_HEADERS CONFIGURE_DEPENDS "${TE_GAME}/include/platform/*.h")
set(TE_SEAM_FORWARDERS "")
foreach(h ${TE_SEAM_HEADERS})
    get_filename_component(stem "${h}" NAME_WE)
    if(NOT stem STREQUAL "memory")   # memory.h is implemented INSIDE the module (platform/nativeee/wasm/memory.cpp)
        list(APPEND TE_SEAM_FORWARDERS "${TE_GEN}/seam/seam_forwarders_${stem}.cpp")
    endif()
endforeach()
list(APPEND TE_SEAM_FORWARDERS "${TE_GEN}/seam/seam_forwarders_second.cpp")
set(TE_SEAM_OUTPUTS "${TE_GEN}/seam/ps2e_seam.h" "${TE_GEN}/seam/seam_imports.txt" "${TE_GEN}/seam/seam_host_thunks.c"
    "${TE_GEN}/seam/seam_stubs.cpp" "${TE_GEN}/seam/seam_manifest.json" "${TE_GEN}/seam/seam_prof_names.c" ${TE_SEAM_FORWARDERS})
add_custom_command(OUTPUT ${TE_SEAM_OUTPUTS}
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_seam.py" --game "${TE_GAME}" --out "${TE_GEN}/seam" --module game
    DEPENDS "${TE_TOOLS}/gen_seam.py" "${TE_TOOLS}/second_seam.tsv" "${TE_TOOLS}/wasm_side_functions.txt"
            "${TE_TOOLS}/platform_imports.tsv" ${TE_SEAM_HEADERS}
    COMMENT "gen_seam: the platform seam (imports, forwarders, host thunks, stubs)")
add_custom_target(te_seam DEPENDS ${TE_SEAM_OUTPUTS})

if(TE_VITA_NATIVE)
    include("${CMAKE_SOURCE_DIR}/cmake/native_lane.cmake")   # sections 2-5 for the Vita's native build of the game code
else()
# ---------------------------------------------------------------------------------------------------------------------
# 2. Game TUs -> wasm32 objects (tools/te_cxx.py: ZERO edits to game code, the MIPS-only constructs rewritten)
# ---------------------------------------------------------------------------------------------------------------------
file(GLOB TE_GAME_SOURCES CONFIGURE_DEPENDS "${TE_GAME}/src/game/*.cpp" "${TE_GAME}/src/*.cpp")
# The decomp's own libc/libm/malloc are KEPT (compiled straight from game/); memory.cpp is replaced.
list(APPEND TE_GAME_SOURCES "${TE_GAME}/src/platform/ps2/libc.cpp" "${TE_GAME}/src/platform/ps2/libm.cpp"
    "${TE_GAME}/src/platform/ps2/malloc.cpp")
file(GLOB TE_WASM_PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/platform/nativeee/wasm/*.cpp")
file(GLOB TE_GAME_HEADERS CONFIGURE_DEPENDS "${TE_GAME}/include/*.h" "${TE_GAME}/include/*/*.h"
    "${CMAKE_SOURCE_DIR}/platform/nativeee/sdk_shim/*.h" "${CMAKE_SOURCE_DIR}/platform/nativeee/*.h")
set(TE_CXX ${Python3_EXECUTABLE} "${TE_TOOLS}/te_cxx.py" --wasi-sdk "${TE_WASI_SDK}" --game "${TE_GAME}"
    --shim "${CMAKE_SOURCE_DIR}/platform/nativeee/sdk_shim" -I "${TE_GEN}/seam" -I "${CMAKE_SOURCE_DIR}/platform/nativeee")
# The vtable thunk pools, generated from the committed slot-signature snapshot tools/vtable_slots.tsv (itself
# regenerated from the decomp by gen_vtable_slots.py -- target te_vtable_slots, compared by tools/ci/build_all.sh).
set(TE_VT_POOL 48 CACHE STRING "thunk instances per declared vtable slot signature (the PAL image needs 28 at most)")
file(MAKE_DIRECTORY "${TE_GEN}/vtable")
add_custom_command(OUTPUT "${TE_GEN}/vtable/vtable_thunks.cpp" "${TE_GEN}/vtable/vtable_thunks.json"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_vtable_thunks.py" --tsv "${TE_TOOLS}/vtable_slots.tsv" --pool ${TE_VT_POOL}
            --wasm-out "${TE_GEN}/vtable/vtable_thunks.cpp" --json "${TE_GEN}/vtable/vtable_thunks.json"
    DEPENDS "${TE_TOOLS}/gen_vtable_thunks.py" "${TE_TOOLS}/vtable_slots.tsv"
    COMMENT "gen_vtable_thunks: type-adapted vtable trampolines (wasm side)")
set(TE_WASM_OBJECTS "")
set(TE_WASM_REPORTS "")
foreach(src ${TE_GAME_SOURCES} ${TE_WASM_PLATFORM_SOURCES} ${TE_SEAM_FORWARDERS} "${TE_GEN}/vtable/vtable_thunks.cpp")
    file(RELATIVE_PATH rel "${CMAKE_SOURCE_DIR}" "${src}")
    string(REPLACE "/" "_" obj "${rel}")
    set(out "${TE_WASM}/obj/${obj}.o")
    set(rep "${TE_WASM}/rep/${obj}.json")
    add_custom_command(OUTPUT "${out}" "${rep}"
        COMMAND ${TE_CXX} --report "${rep}" -o "${out}" "${src}"
        DEPENDS "${src}" "${TE_TOOLS}/te_cxx.py" ${TE_GAME_HEADERS} "${TE_GEN}/seam/ps2e_seam.h"
        COMMENT "wasm32 ${rel}")
    list(APPEND TE_WASM_OBJECTS "${out}")
    list(APPEND TE_WASM_REPORTS "${rep}")
endforeach()
# The WASI stubs (plain C, no rewrite needed)
add_custom_command(OUTPUT "${TE_WASM}/obj/wasi_stubs.o"
    COMMAND "${TE_WASI_SDK}/bin/clang" --target=wasm32-wasi -O2 -c "${CMAKE_SOURCE_DIR}/platform/nativeee/wasm/wasi_stubs.c" -o "${TE_WASM}/obj/wasi_stubs.o"
    DEPENDS "${CMAKE_SOURCE_DIR}/platform/nativeee/wasm/wasi_stubs.c" COMMENT "wasm32 wasi_stubs.c")
list(APPEND TE_WASM_OBJECTS "${TE_WASM}/obj/wasi_stubs.o")
# The object list (~200 paths) travels to the generators and the linker as a response file (tools/rspfile.py): on the
# Windows lane Ninja runs custom commands through cmd.exe (8191 characters) and a Python subprocess line is capped at
# 32767; the same file on every host (what is generated does not depend on how the list arrived). Rewritten only when
# its content changes, so an unchanged list does not re-run the generators.
set(TE_WASM_OBJECTS_RSP "${TE_WASM}/objects.rsp")
list(JOIN TE_WASM_OBJECTS "\n" _te_objects_text)
set(_te_objects_text "${_te_objects_text}\n")
if(EXISTS "${TE_WASM_OBJECTS_RSP}")
    file(READ "${TE_WASM_OBJECTS_RSP}" _te_objects_old)
else()
    set(_te_objects_old "")
endif()
if(NOT _te_objects_text STREQUAL _te_objects_old)
    file(WRITE "${TE_WASM_OBJECTS_RSP}" "${_te_objects_text}")
endif()

# ---------------------------------------------------------------------------------------------------------------------
# 3. The retail data image (addresses only) and the retail function table, from the objects
# ---------------------------------------------------------------------------------------------------------------------
add_custom_command(OUTPUT "${TE_WASM}/retail_image.s" "${TE_WASM}/retail_image.json"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_retail_image.py" --game "${TE_GAME}" --nm "${TE_WASI_SDK}/bin/llvm-nm"
            --out "${TE_WASM}/retail_image.s" --json "${TE_WASM}/retail_image.json" "@${TE_WASM_OBJECTS_RSP}"
    DEPENDS "${TE_TOOLS}/gen_retail_image.py" "${TE_TOOLS}/rspfile.py" "${TE_GAME}/symbol_addrs.txt" ${TE_WASM_OBJECTS} "${TE_WASM_OBJECTS_RSP}"
    COMMENT "gen_retail_image: retail data symbols at retail addresses")
add_custom_command(OUTPUT "${TE_WASM}/retail_image.o"
    COMMAND "${TE_WASI_SDK}/bin/clang" --target=wasm32-wasi -c "${TE_WASM}/retail_image.s" -o "${TE_WASM}/retail_image.o"
    DEPENDS "${TE_WASM}/retail_image.s")
add_custom_command(OUTPUT "${TE_WASM}/retail_table.c" "${TE_WASM}/retail_table.json" "${TE_GEN}/retail_functions_host.c"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_retail_table.py" --game "${TE_GAME}" --nm "${TE_WASI_SDK}/bin/llvm-nm"
            --out "${TE_WASM}/retail_table.c" --json "${TE_WASM}/retail_table.json" --host-out "${TE_GEN}/retail_functions_host.c" "@${TE_WASM_OBJECTS_RSP}"
    DEPENDS "${TE_TOOLS}/gen_retail_table.py" "${TE_TOOLS}/rspfile.py" "${TE_GAME}/symbol_addrs.txt" ${TE_WASM_OBJECTS} "${TE_WASM_OBJECTS_RSP}"
    COMMENT "gen_retail_table: retail code address -> function")
add_custom_command(OUTPUT "${TE_WASM}/retail_table.o"
    COMMAND "${TE_WASI_SDK}/bin/clang" --target=wasm32-wasi -O2 -c "${TE_WASM}/retail_table.c" -o "${TE_WASM}/retail_table.o"
    DEPENDS "${TE_WASM}/retail_table.c")

# ---------------------------------------------------------------------------------------------------------------------
# 4. wasm-ld: one module, imported memory (32 MB fixed), explicit import list (the seam), explicit exports.
#    tools/link_wasm.py links twice: the C++'s .rodata/.data/.bss go BELOW the retail image (global-base = 0x2D9D90 - their
#    measured size), the retail image (a .bss segment, linked last) lands exactly on its retail addresses, and
#    __data_end == __heap_base == the image's end (0x3DB200) whatever the build (the game's heap layout must not move
#    with the C++'s data; the pools start at a fixed address above it, platform/nativeee/wasm/memory.cpp). --stack-first
#    puts the shadow stack at 0..stack-size (the PS2 kernel's range, unused by the game).
# ---------------------------------------------------------------------------------------------------------------------
set(TE_WASM_EXPORTS entry te_retail_image te_retail_functions te_retail_function_count __heap_base __data_end
    te_vt_thunks te_vt_thunk_count te_vt_targets te_vt_signature_count)
set(TE_EXPORT_FLAGS "")
foreach(e ${TE_WASM_EXPORTS})
    list(APPEND TE_EXPORT_FLAGS "--export=${e}")
endforeach()
set(TE_STACK_SIZE 262144 CACHE STRING "wasm shadow stack bytes (the PS2 main thread ran on 16 KB of scratchpad)")
add_custom_command(OUTPUT "${TE_WASM}/game.wasm"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/link_wasm.py" --wasm-ld "${TE_WASI_SDK}/bin/wasm-ld"
            --wasm-objdump "${TE_WABT}/bin/wasm-objdump" --image-start 2989456 --image-end 4043264
            --stack-size ${TE_STACK_SIZE} --out "${TE_WASM}/game.wasm" --
            "${TE_WASM}/retail_table.o" "@${TE_WASM_OBJECTS_RSP}"
            -L "${TE_WASI_SDK}/share/wasi-sysroot/lib/wasm32-wasi" -lc "${TE_WASM}/retail_image.o"
            --no-entry --import-memory --initial-memory=33554432 --max-memory=33554432
            --allow-undefined-file="${TE_GEN}/seam/seam_imports.txt" ${TE_EXPORT_FLAGS} --export-table --gc-sections
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/check_module.py" --wasm-objdump "${TE_WABT}/bin/wasm-objdump"
            --wasm "${TE_WASM}/game.wasm" --imports "${TE_GEN}/seam/seam_imports.txt" --image-start 2989456
            --image-end 4043264 --report "${TE_WASM}/module_report.json"
    DEPENDS "${TE_WASM}/retail_image.o" "${TE_WASM}/retail_table.o" ${TE_WASM_OBJECTS} "${TE_WASM_OBJECTS_RSP}" "${TE_GEN}/seam/seam_imports.txt"
            "${TE_TOOLS}/check_module.py" "${TE_TOOLS}/link_wasm.py" "${TE_TOOLS}/rspfile.py"
    COMMENT "wasm-ld game.wasm in two passes (+ import/export/layout check)")
add_custom_target(te_game_wasm DEPENDS "${TE_WASM}/game.wasm")

# The per-TU compile report (tools/compile_report.md is the committed snapshot: tools/ci/build_all.sh refreshes it)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/compile_report.md"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/compile_report.py" --reports "${TE_WASM}/rep" --game "${TE_GAME}"
            --module-report "${TE_WASM}/module_report.json" --retail-table "${TE_WASM}/retail_table.json"
            --out "${CMAKE_BINARY_DIR}/compile_report.md"
    DEPENDS ${TE_WASM_REPORTS} "${TE_WASM}/game.wasm" "${TE_TOOLS}/compile_report.py"
    COMMENT "compile_report.md")
add_custom_target(te_compile_report ALL DEPENDS "${CMAKE_BINARY_DIR}/compile_report.md")

# tools/vtable_slots.tsv is the committed snapshot of the decomp's vtable slot signatures. Regenerated on demand
# (libclang from tools/toolchain/libclang, ~40 s over 184 TUs); tools/ci/build_all.sh compares it like compile_report.md.
set(TE_LIBCLANG "${TE_TOOLS}/toolchain/libclang" CACHE PATH "pinned libclang python wheel contents (tools/fetch_toolchain.sh)")
set(TE_GAME_SOURCES_RSP "${TE_WASM}/game_sources.rsp")   # the source list, as the object list above (cmd.exe's 8191)
list(JOIN TE_GAME_SOURCES "\n" _te_sources_text)
set(_te_sources_text "${_te_sources_text}\n")
if(EXISTS "${TE_GAME_SOURCES_RSP}")
    file(READ "${TE_GAME_SOURCES_RSP}" _te_sources_old)
else()
    set(_te_sources_old "")
endif()
if(NOT _te_sources_text STREQUAL _te_sources_old)
    file(WRITE "${TE_GAME_SOURCES_RSP}" "${_te_sources_text}")
endif()
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/vtable_slots.tsv"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_vtable_slots.py" --wasi-sdk "${TE_WASI_SDK}" --libclang "${TE_LIBCLANG}"
            --game "${TE_GAME}" --shim "${CMAKE_SOURCE_DIR}/platform/nativeee/sdk_shim" -I "${TE_GEN}/seam"
            -I "${CMAKE_SOURCE_DIR}/platform/nativeee" --out "${CMAKE_BINARY_DIR}/vtable_slots.tsv"
            --json "${CMAKE_BINARY_DIR}/vtable_slots.json" "@${TE_GAME_SOURCES_RSP}"
    DEPENDS "${TE_TOOLS}/gen_vtable_slots.py" "${TE_TOOLS}/te_cxx.py" "${TE_TOOLS}/rspfile.py" ${TE_GAME_SOURCES} ${TE_GAME_HEADERS} "${TE_GEN}/seam/ps2e_seam.h" "${TE_GAME_SOURCES_RSP}"
    COMMENT "gen_vtable_slots: the decomp's vtable slot signatures (libclang)")
add_custom_target(te_vtable_slots DEPENDS "${CMAKE_BINARY_DIR}/vtable_slots.tsv")

# ---------------------------------------------------------------------------------------------------------------------
# 5. wasm2c -> C, the EABI dispatcher from the module's type section, the host static library
# ---------------------------------------------------------------------------------------------------------------------
set(TE_W2C_PARTS 8)
set(TE_W2C_SOURCES "")
math(EXPR last "${TE_W2C_PARTS} - 1")
foreach(i RANGE 0 ${last})
    list(APPEND TE_W2C_SOURCES "${TE_W2C}/game_${i}.c")
endforeach()
# The Vita lane compiles the loads and stores unchecked (below): its header carries the switch. The rewrite also reads
# the parts (the table's allocation; no table.grow) for TE_W2C_FIXED_TABLE; every switch is a -D, the text is the same.
set(TE_W2C_UNCHECKED_STEP)
set(TE_W2C_UNCHECKED_DEP)
if(TE_VITA)
    set(TE_W2C_UNCHECKED_STEP COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/w2c_unchecked.py" "${TE_W2C}/game-impl.h" ${TE_W2C_SOURCES})
    set(TE_W2C_UNCHECKED_DEP "${TE_TOOLS}/w2c_unchecked.py")
endif()
add_custom_command(OUTPUT ${TE_W2C_SOURCES} "${TE_W2C}/game.h" "${TE_W2C}/game-impl.h"
    COMMAND "${TE_WABT}/bin/wasm2c" "${TE_WASM}/game.wasm" -o "${TE_W2C}/game.c" --module-name=game --num-outputs=${TE_W2C_PARTS}
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/w2c_diag.py" "${TE_W2C}/game-impl.h" --adapt-header "${TE_GEN}/eabi_adapt.h"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/w2c_sigfix.py" "${TE_W2C}/game-impl.h" ${TE_W2C_SOURCES} --json "${TE_W2C}/sigfix.json"
    ${TE_W2C_UNCHECKED_STEP}
    DEPENDS "${TE_WASM}/game.wasm" "${TE_TOOLS}/w2c_diag.py" "${TE_TOOLS}/w2c_sigfix.py" ${TE_W2C_UNCHECKED_DEP}
    COMMENT "wasm2c game.wasm (+ call_indirect type-adapter fallback + direct-call signature adapters)")
add_custom_command(OUTPUT "${TE_GEN}/vtable/vtable_slots_host.c" "${TE_GEN}/vtable/vtable_slots_host.json"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_vtable_thunks.py" --tsv "${TE_TOOLS}/vtable_slots.tsv"
            --host-out "${TE_GEN}/vtable/vtable_slots_host.c" --wasm "${TE_WASM}/game.wasm" --wasm-objdump "${TE_WABT}/bin/wasm-objdump"
            --symbols "${TE_GAME}/symbol_addrs.txt" --module game --json "${TE_GEN}/vtable/vtable_slots_host.json"
    DEPENDS "${TE_WASM}/game.wasm" "${TE_TOOLS}/gen_vtable_thunks.py" "${TE_TOOLS}/vtable_slots.tsv" "${TE_GAME}/symbol_addrs.txt"
    COMMENT "gen_vtable_thunks: the vtable slot table for the patcher (host side)")
# eabi_adapt.h: the call-site adapters' prototypes, included by the rewritten game-impl.h (every w2c TU depends on it)
add_custom_command(OUTPUT "${TE_GEN}/eabi_dispatch.c" "${TE_GEN}/eabi_adapt.h"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_eabi_dispatch.py" --wasm-objdump "${TE_WABT}/bin/wasm-objdump"
            --wasm "${TE_WASM}/game.wasm" --module game --out "${TE_GEN}/eabi_dispatch.c" --adapt-header "${TE_GEN}/eabi_adapt.h"
    DEPENDS "${TE_WASM}/game.wasm" "${TE_TOOLS}/gen_eabi_dispatch.py" COMMENT "gen_eabi_dispatch (+ call-site type adapters)")
set_source_files_properties(${TE_W2C_SOURCES} PROPERTIES OBJECT_DEPENDS "${TE_GEN}/eabi_adapt.h")

set(TE_WASM_RT "${TE_WABT}/share/wabt/wasm2c")
add_library(tegame STATIC
    ${TE_W2C_SOURCES}
    "${TE_WASM_RT}/wasm-rt-impl.c"
    "${TE_WASM_RT}/wasm-rt-mem-impl.c"
    "${TE_WASM_RT}/wasm-rt-exceptions-impl.c"
    "${TE_GEN}/seam/seam_host_thunks.c"
    "${TE_GEN}/eabi_dispatch.c"
    "${TE_GEN}/vtable/vtable_slots_host.c"
    "${TE_GEN}/retail_functions_host.c")
target_include_directories(tegame PUBLIC "${TE_W2C}" "${TE_WABT}/include" "${TE_WASM_RT}" "${TE_GEN}/seam" "${TE_GEN}")
# Software bounds checks: the memory is the engine's array (no guard-page mmap). One thread, no WASI.
# The Vita runs the module unchecked: on its 32-bit CPU every check is a 64-bit add and compare against the size read
# from memory, on every load and store, which costs the game a large share of its frame; an access outside the 32 MB the
# retail game was written for faults the application there instead of trapping (the desktop lanes keep the checks).
target_compile_definitions(tegame PUBLIC WASM_RT_USE_MMAP=0 WASM_RT_MEMCHECK_BOUNDS_CHECK=1)
if(TE_VITA)
    # TE_VITA_UNCHECKED_MEMORY (ON): the per-access check compiled out (tools/w2c_unchecked.py; a shipped Vita build has
    # had it since 2026-10-22: the hot TransformJoints is 278 instructions instead of the checked form's 754, game_7.c's
    # code 187 KB instead of 591 KB). Safe for the game as it runs: the checked lanes (the desktop, this option OFF) trap
    # on any access outside the 32 MB ("wasm trap: out of bounds memory access", fatal) and no judged route or recorded
    # run has ever trapped; a run that would trap there faults or reads beside the memory here. OFF = the checked build
    # (the 64-bit compare per access, the base reloaded per access, the trapping float->int; with TE_VITA_W2C_LEAN=OFF
    # too, exactly the desktop lanes' module code) for a device A/B or a hunt for a stray access.
    option(TE_VITA_UNCHECKED_MEMORY "Vita: the module's loads and stores without the per-access bounds check (OFF = the checked build)" ON)
    if(TE_VITA_UNCHECKED_MEMORY)
        target_compile_definitions(tegame PUBLIC TE_W2C_UNCHECKED_MEMORY=1)   # tools/w2c_unchecked.py
    endif()
    # TE_VITA_W2C_LEAN (ON): three more of wasm2c's semantic guards, each identical in value (tools/w2c_unchecked.py):
    # f32/f64.abs as the bare VABS (ARM's FPAbs IS wasm's abs for every input, NaNs and denormals included: no NaN test,
    # no VMRS -- runner/te_vita.cpp checks VABS at start-up under the game thread's FPSCR), i32.trunc_f32 as the bare VCVT
    # (the same value wherever the checked form does not trap; it never has), call_indirect's bound against the table's
    # fixed size (no load of table.size). Measured on the 451 game functions the Totem Hokum profile runs: -2.3 % of
    # their instructions, -24 % of their VMRS flag transfers, -18 % of their VFP-to-core moves. OFF = the earlier, checked code.
    option(TE_VITA_W2C_LEAN "Vita: wasm's abs as VABS, float->int as the bare VCVT, call_indirect against the fixed table size" ON)
    if(TE_VITA_W2C_LEAN)
        target_compile_definitions(tegame PUBLIC TE_W2C_VFP_ABS=1 TE_W2C_UNCHECKED_TRUNC=1 TE_W2C_FIXED_TABLE=1)
    endif()
    # TE_VITA_W2C_COUNT (OFF; a diagnosis build, never shipped): the module counts its function entries, loads and stores,
    # abs, float->int, call_indirect and sqrt; "[te.w2c.count]" lines per TE_W2C_COUNT_EVERY boundaries (runner/main.cpp)
    option(TE_VITA_W2C_COUNT "Vita diagnosis: count the module's operations per boundary (slower; never ship)" OFF)
    if(TE_VITA_W2C_COUNT)
        target_compile_definitions(tegame PUBLIC TE_W2C_COUNT=1)
    endif()
    # TE_PROF_SEAM=ON (a profiling build): the seam thunks mark the game thread's section for the sampling profiler
    # (platform/nativeee/host/te_prof.h, TE_PROF=1 in args.txt) -- a load and two stores per host call, and the thunk is
    # no longer a tail call (~5-6 thousand host calls a picture in the title and the hub, measured: by the instruction
    # count ~0.1 ms of the game thread a frame). Off, the shipped build's thunks are plain branches; TE_PROF=1 then reports
    # the host sections, the imports inside "(own code)".
    option(TE_PROF_SEAM "Vita: the profiler's section markers in the seam thunks (TE_PROF=1 names the game thread's imports)" OFF)
    if(TE_PROF_SEAM)
        set_source_files_properties("${TE_GEN}/seam/seam_host_thunks.c" PROPERTIES COMPILE_DEFINITIONS TE_PROF_SEAM=1)
    endif()
    # and without the call-depth count: every function's entry and exit would bump a global the Vita reads and writes
    # back on every call; the retail game's recursion is the game's own (the fiber's stack is its bound)
    target_compile_definitions(tegame PUBLIC WASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION=1)
endif()
# C99 on purpose (the save states carry the runtime state): wasm-rt.h makes its per-thread state (call depth, trap jump buffer) `_Thread_local` only under
# C11; on Apple a TLS variable's address is a per-process malloc'd block that the generated code caches in registers
# across calls, which a save state cannot carry. Without C11 the runtime state is plain globals (the game is one thread).
# runner/te_run.c shares the setting (it declares the same variables).
set_target_properties(tegame PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_compile_options(tegame PRIVATE -O2 -w)
if(TE_VITA)
    # The Vita's CPU needs the optimiser's every pass. The VitaSDK GCC already targets the Cortex-A9 by default
    # (--with-arch=armv7-a --with-tune=cortex-a9 --with-fpu=neon --with-float=hard --with-mode=thumb: Thumb-2, unaligned
    # LDR/STR for the memcpy loads, VFPv3-D32 + NEON), so nothing is added for the target. Measured on w2c/game_3.c
    # (GCC 15.2): -O3 grows the code 18 % (a 32 KB I-cache) and -Os shrinks it 11 %; over the whole module -O3 is +9 % of
    # w2c code, -O3 -fno-tree-vectorize the same, LTO (TE_VITA_W2C_LTO) +0.3 % -- and in the emulator's game-thread profile
    # (the instruction-count proxy: deterministic routes, the module's own code in ms per picture) none of them moves the
    # game's own code beyond +-1-2 % of -O2 (the game's cost is spread over hundreds of functions): -O2 stays. The knobs
    # TE_VITA_W2C_FLAGS / TE_VITA_W2C_LTO are for a device A/B. -fno-math-errno: the module's sqrtf (wasm's f32.sqrt) is a
    # bare VSQRT instead of VSQRT + a compare +
    # an errno-setting libcall for negative inputs (the value is the same: the default NaN either way; nothing reads errno).
    set(TE_VITA_W2C_FLAGS "-O2;-fno-math-errno" CACHE STRING "Vita: the w2c parts' optimisation flags (a ;-list)")
    set_source_files_properties(${TE_W2C_SOURCES} PROPERTIES COMPILE_OPTIONS "${TE_VITA_W2C_FLAGS}")
    # TE_VITA_W2C_LTO: the module (the 8 parts, the seam thunks, the dispatchers: everything in tegame) as GCC LTO
    # objects, optimised as one unit at the eboot's link (the rest of the eboot is ordinary objects)
    option(TE_VITA_W2C_LTO "Vita: link-time optimisation across the wasm2c module (tegame) only" OFF)
    set(TE_VITA_W2C_LTO_JOBS 8 CACHE STRING "Vita: the LTRANS jobs of TE_VITA_W2C_LTO")
    if(TE_VITA_W2C_LTO)
        target_compile_options(tegame PRIVATE -flto -fno-fat-lto-objects)
    endif()
else()
    set_source_files_properties(${TE_W2C_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1")   # 39 MB of C: -O1 keeps the build minutes, not hours
endif()
add_dependencies(tegame te_seam)
if(WIN32)
    # The runtime's per-thread state stays PLAIN GLOBALS on the Windows lane as on macOS. CMake has no C99
    # flag for clang-cl (it compiles C17), so wasm-rt.h's own chain picks `_Thread_local`, and under `_MSC_VER` it would
    # pick `__declspec(thread)` even without C11 -- the header has no override hook. The Windows lane therefore compiles
    # against a COPY of the pinned header with one hook added (`#if defined(WASM_RT_THREAD_LOCAL)` ahead of the chain),
    # written here from the fetched file (FATAL_ERROR if the pinned text is not the expected one), first on the include
    # path, and defines WASM_RT_THREAD_LOCAL empty for the module and the runner (te_run.c checks it at compile time).
    set(TE_WASM_RT_WIN "${TE_GEN}/wasm-rt-win")
    file(MAKE_DIRECTORY "${TE_WASM_RT_WIN}")
    file(READ "${TE_WABT}/include/wasm-rt.h" _te_rt)
    string(REPLACE "\r\n" "\n" _te_rt "${_te_rt}")   # the Windows wabt release ships CRLF text (verified: identical to the macOS release modulo CRLF)
    set(_te_rt_needle "#ifdef WASM_RT_C11_AVAILABLE\n#define WASM_RT_THREAD_LOCAL _Thread_local\n#elif defined(_MSC_VER)")
    string(FIND "${_te_rt}" "${_te_rt_needle}" _te_rt_pos)
    if(_te_rt_pos EQUAL -1)
        message(FATAL_ERROR "TwinsanityEvolved: ${TE_WABT}/include/wasm-rt.h is not the pinned wabt 1.0.42 text (the WASM_RT_THREAD_LOCAL chain) -- D55's Windows hook cannot be applied")
    endif()
    string(REPLACE "${_te_rt_needle}"
        "#if defined(WASM_RT_THREAD_LOCAL)\n/* TwinsanityEvolved: the storage class is the build's choice (plain globals: the game is one thread and its save states carry this state) */\n#elif defined(WASM_RT_C11_AVAILABLE)\n#define WASM_RT_THREAD_LOCAL _Thread_local\n#elif defined(_MSC_VER)"
        _te_rt "${_te_rt}")
    set(_te_rt "/* GENERATED by CMakeLists.txt from ${TE_WABT}/include/wasm-rt.h (wabt 1.0.42) -- the Windows lane's copy */\n${_te_rt}")
    if(EXISTS "${TE_WASM_RT_WIN}/wasm-rt.h")
        file(READ "${TE_WASM_RT_WIN}/wasm-rt.h" _te_rt_old)
    else()
        set(_te_rt_old "")
    endif()
    if(NOT _te_rt STREQUAL _te_rt_old)
        file(WRITE "${TE_WASM_RT_WIN}/wasm-rt.h" "${_te_rt}")
    endif()
    target_include_directories(tegame BEFORE PUBLIC "${TE_WASM_RT_WIN}")
    target_compile_definitions(tegame PUBLIC WASM_RT_THREAD_LOCAL=)
    # COFF objects: a wasm2c part is ~5 MB of C with tens of thousands of functions and relocations -- /bigobj lifts the
    # 65535-section limit (clang-cl honours it; harmless where not needed).
    target_compile_options(tegame PRIVATE /bigobj)
endif()
endif()   # TE_VITA_NATIVE (sections 2-5)

# ---------------------------------------------------------------------------------------------------------------------
# 6. NativeEE as a library: the engine root configured as its OWN project (never patched, consumed by tag),
#    only the library targets the runner needs are built.
# ---------------------------------------------------------------------------------------------------------------------
include(ExternalProject)
set(TE_ENGINE_BUILD "${CMAKE_BINARY_DIR}/engine")
# The release shape (tools/ci/release_dist.sh / .ps1; the release workflow): the engine's own
# dependency recipes and code-generation options, forwarded so the engine's archives and the port's
# final link see the SAME SDL2 / FFmpeg / compiler front end / LTO choice:
#   TE_DIST=ON              SDL2 fetched and built STATICALLY from the engine's pinned release
#                           (engine/cmake/Sdl2Dependency.cmake: PS2R_FETCH_SDL2, the system package
#                           hidden) and FFmpeg from engine/tools/ci/ffmpeg_static.sh through
#                           CMAKE_PREFIX_PATH (LGPL, mpeg2 + swscale only) -- dist/<platform>/ then
#                           carries no lib/ on macOS/Linux; OFF = the system packages (development).
#   TE_LTO=ON               PS2R_LTO on the engine + CMAKE_INTERPROCEDURAL_OPTIMIZATION here
#                           (CheckIPOSupported; needs lld-link with clang-cl).
#   CMAKE_<LANG>_COMPILER_LAUNCHER, CMAKE_TOOLCHAIN_FILE, VCPKG_TARGET_TRIPLET, CMAKE_LINKER,
#   CMAKE_PREFIX_PATH, CMAKE_OSX_ARCHITECTURES   forwarded when set (the clang-cl shim, vcpkg).
option(TE_DIST "Release shape: static SDL2 (engine recipe) + the static LGPL FFmpeg under CMAKE_PREFIX_PATH" OFF)
option(TE_LTO "Link-time optimisation for the port and the engine (PS2R_LTO)" OFF)
set(TE_ENGINE_FORWARDED_ARGS "")
foreach(var CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER CMAKE_TOOLCHAIN_FILE VCPKG_TARGET_TRIPLET CMAKE_LINKER
            CMAKE_PREFIX_PATH CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET)
    if(DEFINED ${var} AND NOT "${${var}}" STREQUAL "")
        string(REPLACE ";" "|" _fwd "${${var}}")   # ExternalProject LIST_SEPARATOR below
        list(APPEND TE_ENGINE_FORWARDED_ARGS "-D${var}=${_fwd}")
    endif()
endforeach()
if(WIN32 AND DEFINED _VCPKG_INSTALLED_DIR)
    # vcpkg manifest mode (cmake/windows-clang-cl.toolchain.cmake): the engine's ExternalProject reads the same toolchain
    # file and would run a SECOND manifest install (engine/vcpkg.json) into its own tree. The two manifests are kept
    # IDENTICAL (sdl2, ffmpeg[avcodec,swscale], opengl-registry), so the engine is pointed at the port's installed
    # tree with its own install step off: FFmpeg is built once per build dir.
    list(APPEND TE_ENGINE_FORWARDED_ARGS "-DVCPKG_INSTALLED_DIR=${_VCPKG_INSTALLED_DIR}" -DVCPKG_MANIFEST_INSTALL=OFF)
endif()
if(TE_VITA)
    # The engine's ps2e draw surface without a GL 4.1 context (engine/platform/ps2e: PS2R_PS2E_DRAW_BACKEND); the
    # engine's own configure reads the same toolchain file (forwarded above) and so builds its vita target folder.
    list(APPEND TE_ENGINE_FORWARDED_ARGS -DPS2R_PS2E_DRAW_BACKEND=gl)
    # FFmpeg for the movies' MPEG-2 decoder (engine/ipu/hle/ffmpeg_video_decoder.cpp: the mpeg2video decoder, the
    # mpegvideo parser, swscale). Not the VitaSDK's FFmpeg package: it is configured for audio / MPEG-4 players
    # (--enable-decoder=aac,...,mpeg4,...) and has neither, so every movie opened "0x0 @ 0.000 fps" and ended at once.
    # The engine's static LGPL recipe (engine/tools/ci/ffmpeg_static.sh, the desktop release's FFmpeg) cross-compiled by
    # tools/vita/ffmpeg_vita.sh into <build>/third_party/ffmpeg -- at build time (the target te_ffmpeg_vita, ~3 min once
    # per build dir; the script caches on its configure record) and before the engine's configure, which is pointed at
    # the same headers and archives (the decoder must be compiled against the headers of the library it links).
    # TE_VITA_FFMPEG_SOURCE: a local copy of the pinned release tarball for an offline build (empty: the script downloads
    # it and checks its sha256).
    set(TE_VITA_FFMPEG "${CMAKE_BINARY_DIR}/third_party/ffmpeg")
    set(TE_VITA_FFMPEG_SOURCE "" CACHE FILEPATH "a local ffmpeg-<version>.tar.xz for tools/vita/ffmpeg_vita.sh (empty: download the pinned release)")
    set(TE_VITA_FFMPEG_LIBS "${TE_VITA_FFMPEG}/lib/libavcodec.a" "${TE_VITA_FFMPEG}/lib/libswscale.a" "${TE_VITA_FFMPEG}/lib/libavutil.a")
    add_custom_command(OUTPUT "${TE_VITA_FFMPEG}/PS2R_FFMPEG_CONFIGURE"
        BYPRODUCTS ${TE_VITA_FFMPEG_LIBS}
        COMMAND ${CMAKE_COMMAND} -E env "VITASDK=${VITASDK}" "FFMPEG_SOURCE=${TE_VITA_FFMPEG_SOURCE}"
                bash "${CMAKE_SOURCE_DIR}/tools/vita/ffmpeg_vita.sh" "${TE_VITA_FFMPEG}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tools/vita/ffmpeg_vita.sh" "${TE_ENGINE}/tools/ci/ffmpeg_static.sh"
        COMMENT "FFmpeg for the Vita: the engine's LGPL recipe cross-compiled (tools/vita/ffmpeg_vita.sh) -> ${TE_VITA_FFMPEG}"
        USES_TERMINAL VERBATIM)
    add_custom_target(te_ffmpeg_vita DEPENDS "${TE_VITA_FFMPEG}/PS2R_FFMPEG_CONFIGURE")
    file(MAKE_DIRECTORY "${TE_VITA_FFMPEG}/include")   # an imported target's include directory must exist when generating
    list(APPEND TE_ENGINE_FORWARDED_ARGS "-DFFMPEG_INCLUDE_DIR=${TE_VITA_FFMPEG}/include"
         "-DFFMPEG_AVCODEC_LIBRARY=${TE_VITA_FFMPEG}/lib/libavcodec.a"
         "-DFFMPEG_AVUTIL_LIBRARY=${TE_VITA_FFMPEG}/lib/libavutil.a"
         "-DFFMPEG_SWSCALE_LIBRARY=${TE_VITA_FFMPEG}/lib/libswscale.a")
endif()
if(TE_DIST)
    set(PS2R_FETCH_SDL2 ON CACHE BOOL "" FORCE)
    set(CMAKE_DISABLE_FIND_PACKAGE_SDL2 ON)
    list(APPEND TE_ENGINE_FORWARDED_ARGS -DPS2R_FETCH_SDL2=ON -DCMAKE_DISABLE_FIND_PACKAGE_SDL2=ON)
endif()
if(TE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _te_ipo_ok OUTPUT _te_ipo_msg LANGUAGES C CXX)
    if(NOT _te_ipo_ok)
        message(FATAL_ERROR "TwinsanityEvolved: TE_LTO=ON but the toolchain refused IPO: ${_te_ipo_msg}")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    list(APPEND TE_ENGINE_FORWARDED_ARGS -DPS2R_LTO=ON)
endif()
# The engine archives the port links, in link order: the title's services (title::Input/SaveStorage/Files),
# the title's audio (title::Audio, SampleVoice, MusicAsset -- the sample/music arithmetic the audio façade sits
# on, host-typed), the graphics slice (ps2e_draw -- engine/platform/ps2e, the C typed-draw surface -- the GL
# backend, the GS format / transfer / texture-decode layers that decode the game's texture resources), the SDL host half
# (window + GL context, InputSystem, SdlAudioSink + AudioPullRing), then the engine tiers (sound = sound::AudioMixer,
# VagDecoder). The movie slice: ps2e_movie -- engine/platform/ps2e, the C movie-decoder surface -- over
# ps2r_ipu (PssDemux, the FFmpeg MPEG-2 decoder, FmvAudioCapture), which is why the port links FFmpeg (the engine's own
# cmake/FindFFmpeg.cmake, included below). Each is an existing engine target; only the objects a seam body pulls are
# linked.
# <engine build dir>/<dir>/<prefix><target><suffix>: libps2r_core.a on macOS/Linux, ps2r_core.lib on the Windows lane.
set(TE_ENGINE_LIB_DIRS
    games/slus_20909_crash_twinsanity/hle:ps2r_title_hle
    games/slus_20909_crash_twinsanity/vu1_library:ps2r_title_vu1_library
    games/slus_20909_crash_twinsanity/hle/audio:ps2r_title_audio
    engine/platform/ps2e:ps2r_ps2e_draw
    engine/platform/ps2e:ps2r_ps2e_movie
    engine/ipu:ps2r_ipu
    engine/gs/hle/texsrc:ps2r_gs_hle_texsrc
    engine/gs/hle/targets:ps2r_gs_hle_targets
    engine/gs/xfer:ps2r_gs_xfer
    engine/gs/vram:ps2r_gs_vram
    engine/platform/sdl:ps2r_platform_sdl
    engine/gs/hle/ir:ps2r_gs_ir
    engine/gs:ps2r_gs_format
    engine/cdvd:ps2r_cdvd
    engine/vu:ps2r_vu
    engine/gif:ps2r_gif
    engine/ee:ps2r_ee
    engine/sound:ps2r_sound
    engine/core:ps2r_core
    engine/platform:ps2r_platform
    engine/platform:ps2r_host)
if(NOT TE_VITA)
    # The GL 4.1 adapter behind ps2e_draw's GL arm (the Vita's ps2e_draw is the null arm: no GL at all). Listed after the
    # draw surface and before the SDL half, where the link order wants it.
    list(FIND TE_ENGINE_LIB_DIRS engine/platform/sdl:ps2r_platform_sdl _te_sdl_index)
    list(INSERT TE_ENGINE_LIB_DIRS ${_te_sdl_index} engine/render/backends/gl:ps2r_render_gl)
else()
    # the GL arm's backend (vitaGL on the Vita, the desktop GL driver elsewhere)
    list(FIND TE_ENGINE_LIB_DIRS engine/platform/sdl:ps2r_platform_sdl _te_sdl_index)
    list(INSERT TE_ENGINE_LIB_DIRS ${_te_sdl_index} engine/render/backends/gl:ps2r_render_gl)
endif()
set(TE_ENGINE_LIB_TARGETS "")
set(TE_ENGINE_LIB_FILES "")
foreach(entry ${TE_ENGINE_LIB_DIRS})
    string(REPLACE ":" ";" _parts "${entry}")
    list(GET _parts 0 _dir)
    list(GET _parts 1 _target)
    list(APPEND TE_ENGINE_LIB_TARGETS "${_target}")
    list(APPEND TE_ENGINE_LIB_FILES "${TE_ENGINE_BUILD}/${_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}${_target}${CMAKE_STATIC_LIBRARY_SUFFIX}")
endforeach()
ExternalProject_Add(nativeee
    SOURCE_DIR "${TE_ENGINE}"
    BINARY_DIR "${TE_ENGINE_BUILD}"
    # The engine is the generalised NativeEE (github.com/menaman123/NativeEE) on its twinsanityevolved branch, which
    # carries the Crash Twinsanity title module beside the engine's template title: the title is named explicitly
    # (the engine auto-discovers a title only when games/ holds exactly one). No disc input is given, so the title's
    # recompiled code and play composition are skipped -- the port links the title's HLE libraries only.
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DPS2R_BUILD_RECOMPILER=OFF -DPS2R_DIAGNOSTICS=OFF
               -DPS2R_TITLE=slus_20909_crash_twinsanity
               -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
               ${TE_ENGINE_FORWARDED_ARGS}
    LIST_SEPARATOR |
    BUILD_COMMAND ${CMAKE_COMMAND} --build "${TE_ENGINE_BUILD}" --target ${TE_ENGINE_LIB_TARGETS}
    BUILD_BYPRODUCTS ${TE_ENGINE_LIB_FILES}
    INSTALL_COMMAND ""
    # ON: a re-pin of engine/ in an EXISTING build dir must rebuild the engine's archives, or the port's
    # headers see the new engine while its libraries are the old pin's (undefined symbols at link). Up to date, the
    # engine's ninja pass is a no-op of ~1 s.
    BUILD_ALWAYS ON)
if(TE_VITA)
    add_dependencies(nativeee te_ffmpeg_vita)   # the engine's configure reads the lane's FFmpeg (above)
endif()
add_library(nativeee_libs INTERFACE)
target_link_libraries(nativeee_libs INTERFACE ${TE_ENGINE_LIB_FILES})
target_include_directories(nativeee_libs INTERFACE "${TE_ENGINE}")
find_package(Threads REQUIRED)
# SDL2 and OpenGL: the graphics window and GL context (ps2e_draw), the engine's InputSystem (pads) and the audio sink +
# ring (ps2r_platform_sdl carries all three in one archive). The engine's own configure already required SDL2; the same
# package is found here. REQUIRED: every owner's archive links it.
# ONE recipe with the engine (engine/cmake/Sdl2Dependency.cmake): find_package(SDL2 CONFIG), or under
# TE_DIST / PS2R_FETCH_SDL2 the pinned release fetched and built statically -- the target SDL2::SDL2 either way.
include("${TE_ENGINE}/cmake/Sdl2Dependency.cmake")
if(TE_VITA)
    # vitaGL (and the kernel stubs it calls); FFmpeg is the lane's own static LGPL build (te_ffmpeg_vita above:
    # libavcodec with the mpeg1/mpeg2 video decoders only, libswscale, libavutil), never the VitaSDK package's: the
    # finder's cache entries are pinned to it, so a build dir configured before keeps no path into the SDK's copy.
    set(FFMPEG_INCLUDE_DIR "${TE_VITA_FFMPEG}/include" CACHE PATH "FFmpeg headers (the Vita lane's own build)" FORCE)
    set(FFMPEG_AVCODEC_LIBRARY "${TE_VITA_FFMPEG}/lib/libavcodec.a" CACHE FILEPATH "libavcodec (the Vita lane's own build)" FORCE)
    set(FFMPEG_AVUTIL_LIBRARY "${TE_VITA_FFMPEG}/lib/libavutil.a" CACHE FILEPATH "libavutil (the Vita lane's own build)" FORCE)
    set(FFMPEG_SWSCALE_LIBRARY "${TE_VITA_FFMPEG}/lib/libswscale.a" CACHE FILEPATH "libswscale (the Vita lane's own build)" FORCE)
    include("${TE_ENGINE}/cmake/FindFFmpeg.cmake")
    set(TE_HAVE_SDL_INPUT 1)
    # Static archives: each after the ones that use it (avcodec -> swscale -> avutil, then libm and libpthread, which
    # everything above uses).
    # libpthread WHOLE: libstdc++'s std::thread / mutex / condition_variable test `__gthread_active_p()` through weak
    # pthread symbols -- an archive pulled in piecemeal leaves them unresolved and every std::thread throws
    # "Enable multithreading to use std::thread".
    include("${TE_ENGINE}/cmake/OpenGlDependency.cmake")
    target_link_libraries(nativeee_libs INTERFACE SDL2::SDL2 OpenGL::GL FFmpeg::avcodec FFmpeg::swscale FFmpeg::avutil m)
    target_link_options(nativeee_libs INTERFACE "LINKER:--whole-archive,-lpthread,--no-whole-archive")
else()
find_package(OpenGL REQUIRED)
# FFmpeg (libavcodec / libavutil / libswscale): the movies' MPEG-2 video decoder (ps2r_ipu, behind ps2e_movie). The
# engine's finder (its FATAL_ERROR names the packages to install); the same libraries the engine's configure found.
include("${TE_ENGINE}/cmake/FindFFmpeg.cmake")
set(TE_HAVE_SDL_INPUT 1)
target_link_libraries(nativeee_libs INTERFACE Threads::Threads SDL2::SDL2 OpenGL::GL FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale)
if(WIN32)
    # The engine's archives arrive as files (ExternalProject), so ps2r_host's own PUBLIC system libraries
    # (engine/engine/platform/CMakeLists.txt: dbghelp psapi winmm) do not follow them: name them here.
    target_link_libraries(nativeee_libs INTERFACE dbghelp psapi winmm)
endif()
endif()
message(STATUS "TwinsanityEvolved: SDL2 ${SDL2_VERSION} -- window + GL (graphics), pads over the engine's InputSystem, audio sink")
add_dependencies(nativeee_libs nativeee)
enable_testing()

# The disc identity (engine/app/host/disc_identity: what a disc image IS by its content -- the program SYSTEM.CNF boots,
# the region, and whether the whole image is a known retail dump: launcher/discs/redump.json; the file lists
# launcher/discs/<id>.json, labelled by provenance; the region maps launcher/discs/region_map_<serial>.json). The game logs
# it at boot; the tables are metadata (names, sizes, hashes, addresses), embedded here at configure time as raw string
# literals so the binary needs no file beside it. The launcher reads the same JSON files at run time (title.json).
file(GLOB TE_RETAIL_LISTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/launcher/discs/*.json")
list(SORT TE_RETAIL_LISTS)
# the tables' CONTENTS are embedded at configure time: an edit of one re-runs the configure (else the binary would keep
# the table it was configured with)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TE_RETAIL_LISTS})
set(_te_lists_cpp "// Generated by CMakeLists.txt from launcher/discs/*.json -- do not edit.\nnamespace te::discs {\nextern const char* const kRetailLists[];\nconst char* const kRetailLists[] = {\n")
foreach(_list ${TE_RETAIL_LISTS})
    file(READ "${_list}" _json)
    string(APPEND _te_lists_cpp "R\"TEJSON(${_json})TEJSON\",\n")
endforeach()
string(APPEND _te_lists_cpp "nullptr};\n}  // namespace te::discs\n")
file(WRITE "${TE_GEN}/disc/retail_lists.cpp.in" "${_te_lists_cpp}")
configure_file("${TE_GEN}/disc/retail_lists.cpp.in" "${TE_GEN}/disc/retail_lists.cpp" COPYONLY)
add_library(te_disc_identity STATIC "${TE_ENGINE}/app/host/disc_identity.cpp" "${TE_ENGINE}/app/host/disc_json.cpp"
                                    "${TE_ENGINE}/app/host/disc_image_hash.cpp" "${TE_ENGINE}/app/host/disc_regions.cpp"
                                    "${TE_ENGINE}/app/host/disc_languages.cpp"
                                    "${TE_ENGINE}/app/host/region_map_make.cpp"
                                    "${TE_GEN}/disc/retail_lists.cpp")
target_include_directories(te_disc_identity PUBLIC "${TE_ENGINE}")
target_link_libraries(te_disc_identity PUBLIC nativeee_libs)
if(NOT MSVC)
    target_compile_options(te_disc_identity PRIVATE -Wall -Wextra)
endif()

# The mod importer (engine/app/host/mod_import: a Mod Crate, a modified disc image, an xdelta / PPF / BPS patch or a folder of
# files into one overlay folder of the mods folder) and its command line tool. The launcher's Mods
# page links the same library (engine/app/host/mod_import/mod_import.cmake). Desktop only: the Vita never imports.
if(NOT TE_VITA)
    set(MOD_IMPORT_ENGINE_ROOT "${TE_ENGINE}")
    set(MOD_IMPORT_GAME_DIR "${CMAKE_SOURCE_DIR}/game")
    include("${TE_ENGINE}/app/host/mod_import/mod_import.cmake")
    # the PAL symbol list the region-map sub-command reads (game/symbol_addrs.txt: names and addresses, no game data),
    # embedded at configure time so the tool needs no repository beside it
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/game/symbol_addrs.txt")
    file(READ "${CMAKE_SOURCE_DIR}/game/symbol_addrs.txt" _te_symbols)
    file(WRITE "${TE_GEN}/disc/pal_symbol_addrs.cpp.in" "// Generated by CMakeLists.txt from game/symbol_addrs.txt -- do not edit.\n#include \"app/host/region_map_make.h\"\nnamespace {\nconst char* const kSymbolAddrs = R\"TESYM(${_te_symbols})TESYM\";\nstruct Register { Register() { ps2r::app::SetDefaultPalSymbols(kSymbolAddrs); } } g_register;\n}  // namespace\n")
    configure_file("${TE_GEN}/disc/pal_symbol_addrs.cpp.in" "${TE_GEN}/disc/pal_symbol_addrs.cpp" COPYONLY)
    add_executable(twinsanityevolved-modtool "${TE_ENGINE}/app/host/mod_import/modtool_main.cpp"
        # the region-map sub-command (engine/app/host/region_map_make.h: another region's executable mapped onto the European one)
        "${TE_ENGINE}/app/host/region_map_make.cpp" "${TE_ENGINE}/app/host/disc_regions.cpp" "${TE_ENGINE}/app/host/disc_identity.cpp"
        "${TE_ENGINE}/app/host/disc_json.cpp" "${TE_ENGINE}/app/host/disc_image_hash.cpp" "${TE_GEN}/disc/pal_symbol_addrs.cpp")
    target_include_directories(twinsanityevolved-modtool PRIVATE "${TE_ENGINE}")
    target_link_libraries(twinsanityevolved-modtool PRIVATE ps2r_mod_import)
    set_target_properties(twinsanityevolved-modtool PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    if(NOT MSVC)
        target_compile_options(twinsanityevolved-modtool PRIVATE -Wall -Wextra)
    endif()
endif()

# ---------------------------------------------------------------------------------------------------------------------
# 7. platform/nativeee host side (the seam bodies over the engine) and the runner
# ---------------------------------------------------------------------------------------------------------------------
add_subdirectory(platform/nativeee)

# The game fiber (runner/te_fiber*: the module on its own fixed-address stack) and the save states (runner/te_savestate*)
# The pad-log authoring aids (runner/te_probe*: the played character over guest memory; runner/te_autopilot*: a plan
# driven closed-loop from the probe, recorded with --record-pad) are part of the runner but never of a fixture.
if(WIN32)
    set(TE_FIBER_SWITCH runner/te_fiber_win64.c)      # the Windows x64 switch: inline asm, clang-cl
else()
    set(TE_FIBER_SWITCH runner/te_fiber_switch.S)     # AAPCS64 / SysV x86-64
endif()
if(TE_VITA)
    set(TE_SAVESTATE_RUNNER runner/te_savestate_null.cpp)   # no fixed-address image / relaunch on the device: every entry refuses
else()
    set(TE_SAVESTATE_RUNNER runner/te_savestate.cpp)
endif()
add_executable(twinsanityevolved runner/main.cpp runner/te_env.cpp runner/te_run.c runner/te_startup.cpp
    runner/te_fiber.c ${TE_FIBER_SWITCH} runner/te_game.cpp ${TE_SAVESTATE_RUNNER}
    runner/te_probe.cpp runner/te_autopilot.cpp runner/te_profile.cpp runner/te_disc_log.cpp runner/te_region.cpp)
# the game folder (ps2r.ini beside the game, its memory cards and mods): shared host code the launcher resolves the same way
target_sources(twinsanityevolved PRIVATE "${TE_ENGINE}/app/host/game_folder.cpp")
set_target_properties(twinsanityevolved PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)   # te_run.c: as tegame (above)
if(TE_VITA)
    # The Vita runner: the baked arguments + the heap size + the log file (runner/te_vita.cpp), the AAPCS32 fiber switch
    # (te_fiber_switch.S's __arm__ arm), the save-state runner's refusing arm (te_savestate_null.cpp: no second process,
    # no fixed-address image). The wasm2c runtime includes <sys/mman.h> unconditionally on POSIX; newlib has none, so the lane
    # supplies the declarations (never called: WASM_RT_USE_MMAP=0) from platform/nativeee/vita/shim.
    target_sources(twinsanityevolved PRIVATE runner/te_vita.cpp)
    target_link_libraries(twinsanityevolved PRIVATE ScePower_stub)   # the clocks te_vita.cpp raises
    if(TE_VITA_NATIVE)
        # the native game (cmake/native_lane.cmake): its prelinked object, the trampolines and tables of the runner
        target_sources(twinsanityevolved PRIVATE runner/te_native.cpp ${TE_NATIVE_RUNNER_SOURCES} "${TE_NATIVE_GAME_OBJECT}")
        add_dependencies(twinsanityevolved te_native_game)
        # the game's object uses 32-bit enums (-fno-short-enums: the R5900's and wasm32's size) beside the Vita's short ones;
        # no enum type crosses the seam (its arguments are s32), so ld's attribute warning says nothing here
        target_link_options(twinsanityevolved PRIVATE -Wl,--no-enum-size-warning)
    else()
    target_include_directories(tegame BEFORE PUBLIC "${CMAKE_SOURCE_DIR}/platform/nativeee/vita/shim")
    # wasm-rt.h unwinds traps with sigsetjmp / siglongjmp, which the Vita's newlib has only for Cygwin / RTEMS: the plain
    # pair stands in (no signals on the device) through a force-included header on the module and the runner.
    target_compile_options(tegame PUBLIC "$<$<COMPILE_LANGUAGE:C,CXX>:-include${CMAKE_SOURCE_DIR}/platform/nativeee/vita/shim/te_vita_setjmp.h>")
    endif()
    target_link_options(twinsanityevolved PRIVATE -Wl,-q)   # the relocations vita-elf-create reads
    if(TE_VITA_W2C_LTO AND NOT TE_VITA_NATIVE)
        # the LTO objects (tegame) are optimised here, with the parts' own flags; -Wl,-q keeps the LTRANS objects'
        # relocations like any other input's
        target_link_options(twinsanityevolved PRIVATE "-flto=${TE_VITA_W2C_LTO_JOBS}" ${TE_VITA_W2C_FLAGS} -w)
    endif()
    # The drawing thread's census on the device (platform/nativeee/host/vita_gpu_census.cpp): the GXM calls vitaGL can
    # block in, its pool allocator and frees, glFinish and the long sleeps of its out-of-memory path, each timed or
    # counted through a wrapper (the wrapped call itself unchanged) -- and the GXM LAST-STATE CACHE in the per-draw
    # wrappers: a set that hands vitaGL's context, inside its scene, the value it already holds is not made
    # (TE_GFX_VITA_GXM_CACHE=0: every call made)
    foreach(te_wrapped sceGxmBeginScene sceGxmEndScene sceGxmDisplayQueueAddEntry sceGxmFinish sceGxmNotificationWait
                       sceGxmShaderPatcherCreateFragmentProgram sceGxmShaderPatcherCreateVertexProgram sceGxmDraw
                       vgl_memalign vgl_free glFinish sceKernelDelayThread glDrawArrays
                       # the per-frame GXM call census (counted only)
                       sceGxmDrawInstanced sceGxmSetVertexStream sceGxmSetFragmentTexture sceGxmSetVertexProgram
                       sceGxmSetFragmentProgram sceGxmSetVertexDefaultUniformBuffer sceGxmSetFragmentDefaultUniformBuffer
                       sceGxmSetFrontDepthFunc sceGxmSetBackDepthFunc sceGxmSetFrontDepthWriteEnable sceGxmSetBackDepthWriteEnable
                       sceGxmSetFrontFragmentProgramEnable sceGxmSetBackFragmentProgramEnable sceGxmSetFrontStencilRef
                       sceGxmSetBackStencilRef sceGxmSetFrontPolygonMode sceGxmSetBackPolygonMode sceGxmSetCullMode
                       sceGxmSetFrontStencilFunc sceGxmSetBackStencilFunc sceGxmSetFrontDepthBias sceGxmSetBackDepthBias
                       sceGxmSetRegionClip
                       # the GXM last-state cache's invalidations: the calls that change a cached value other than
                       # through its setter (vita_gpu_census.cpp)
                       sceGxmReserveVertexDefaultUniformBuffer sceGxmReserveFragmentDefaultUniformBuffer
                       sceGxmSetVertexUniformBuffer sceGxmSetFragmentUniformBuffer sceGxmSetTwoSidedEnable
                       sceGxmShaderPatcherReleaseVertexProgram sceGxmShaderPatcherReleaseFragmentProgram
                       sceGxmShaderPatcherUnregisterProgram sceGxmShaderPatcherForceUnregisterProgram
                       # vitaGL's level transfers, not made while the GL backend writes a chain's levels itself
                       sceGxmTransferDownscale)
        target_link_options(twinsanityevolved PRIVATE "-Wl,--wrap=${te_wrapped}")
    endforeach()
    # vita-elf-create appends its module data (4 KB and more) after the code segment, in the space before the data
    # segment, which the default script starts at the next 64 KB boundary: code ending in a page's last few KB left no
    # room ("segment 1 overlaps"), so whether the image converted depended on the code's size. The default script with
    # the data moved a further page out whenever the code ends in a page's last 16 KB.
    execute_process(COMMAND ${CMAKE_C_COMPILER} -Wl,--verbose -x c /dev/null -o /dev/null
                    OUTPUT_VARIABLE TE_VITA_LD_VERBOSE ERROR_QUIET)
    string(REGEX MATCH "==================================================\n(.*)\n==================================================" TE_VITA_LD_MATCH "${TE_VITA_LD_VERBOSE}")
    set(TE_VITA_LD_SCRIPT "${CMAKE_MATCH_1}")
    string(FIND "${TE_VITA_LD_SCRIPT}" ". = DATA_SEGMENT_ALIGN (0x10000, 0x10000);" TE_VITA_LD_AT)
    if(TE_VITA_LD_AT LESS 0)
        message(FATAL_ERROR "TwinsanityEvolved: the Vita linker's default script has no DATA_SEGMENT_ALIGN (0x10000, 0x10000) line to adjust")
    endif()
    # (DATA_SEGMENT_ALIGN takes no added term: the location is moved past the boundary first, so it aligns to the next)
    string(REPLACE ". = DATA_SEGMENT_ALIGN (0x10000, 0x10000);"
                   ". = ((. & 0xFFFF) > 0xC000) ? ALIGN(0x10000) + 1 : .;\n  . = DATA_SEGMENT_ALIGN (0x10000, 0x10000);"
                   TE_VITA_LD_SCRIPT "${TE_VITA_LD_SCRIPT}")
    file(WRITE "${CMAKE_BINARY_DIR}/vita_link.ld" "${TE_VITA_LD_SCRIPT}\n")
    target_link_options(twinsanityevolved PRIVATE "-Wl,-T,${CMAKE_BINARY_DIR}/vita_link.ld")
endif()
if(WIN32)
    # Save states carry return addresses into this image: the image must load at the same address in the saving
    # and the loading process. POSIX re-execs without ASLR (te_savestate.cpp EnsureNoAslr); Windows has no per-process
    # switch, so the image is linked without a dynamic base and the loader keeps the preferred base.
    target_link_options(twinsanityevolved PRIVATE /DYNAMICBASE:NO)
    # A windowed program: started from a shortcut no console opens behind the game (runner/te_startup.cpp attaches the
    # terminal's console when started from one). main() stays the entry point.
    target_link_options(twinsanityevolved PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    # the first start's Open dialog for the disc image (runner/te_startup.cpp ChooseDiscImage)
    target_link_libraries(twinsanityevolved PRIVATE comdlg32)
endif()
# The coroutine's contract on this host's ABI arm: fixed mapping + guard, enter / yield / finish, callee-saved state
# across both directions. No module, no engine: the fiber files alone.
add_executable(te_fiber_test runner/te_fiber_test.c runner/te_fiber.c ${TE_FIBER_SWITCH})
set_target_properties(te_fiber_test PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_include_directories(te_fiber_test PRIVATE "${CMAKE_SOURCE_DIR}/runner")
target_compile_options(te_fiber_test PRIVATE -Wall -Wextra)
if(NOT TE_VITA)   # a host-run ctest; the Vita binary cannot run here
add_test(NAME nativeee.te_fiber COMMAND te_fiber_test)
set_tests_properties(nativeee.te_fiber PROPERTIES LABELS nativeee TIMEOUT 60)
endif()
target_include_directories(twinsanityevolved PRIVATE "${CMAKE_SOURCE_DIR}/runner" "${CMAKE_SOURCE_DIR}/platform/nativeee/host")
target_compile_options(twinsanityevolved PRIVATE -Wall -Wextra)
# Link order: the module (tegame: its thunks reference every ps2e_*), the owner bodies (te_platform_host, WHOLE
# archive: a body nothing but a thunk references must still be linked -- the linker scans an archive once, in order),
# the engine, then the weak stubs LAST so a strong body anywhere earlier wins (platform/nativeee/CMakeLists.txt).
if(TE_VITA)
    # CMake's WHOLE_ARCHIVE link feature knows Linux / Darwin / Windows, not the Generic system the Vita toolchain names:
    # the same GNU ld bracket spelled out.
    if(TE_VITA_NATIVE)
        target_link_libraries(twinsanityevolved PRIVATE -Wl,--whole-archive te_platform_host -Wl,--no-whole-archive te_disc_identity nativeee_libs te_seam_stubs)
    else()
    target_link_libraries(twinsanityevolved PRIVATE tegame -Wl,--whole-archive te_platform_host -Wl,--no-whole-archive te_disc_identity nativeee_libs te_seam_stubs)
    endif()
else()
target_link_libraries(twinsanityevolved PRIVATE tegame "$<LINK_LIBRARY:WHOLE_ARCHIVE,te_platform_host>" te_disc_identity nativeee_libs te_seam_stubs)
endif()
add_dependencies(twinsanityevolved nativeee)

if(TE_VITA)
    # The eboot and the .vpk (VitaSDK's vita.cmake: vita-elf-create -> vita-make-fself -> vita-mksfoex -> vita-pack-vpk).
    # ATTRIBUTE2=12: the "extra memory" mode the title's 32 MB linear memory + the engine's heaps need beyond the default
    # application budget. The disc image and the settings live under ux0:data/twinsanity/
    # (runner/te_vita.cpp) -- game data is never part of the package.
    include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
    set(VITA_APP_NAME "TwinsanityEvolved")
    set(VITA_TITLEID "TWEV00001")
    set(VITA_VERSION "01.00")
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
    vita_create_self(eboot.bin twinsanityevolved UNSAFE)
    # The shipped shader cache: every <key>.tegxp of TE_VITA_SHADER_SEED_DIR packed as app0:shaders/<key>.tegxp -- read when
    # the memory card's cache (ux0:data/twinsanity/shaders) has no file for a key (gl_shader_library.cpp SeedDir), so a
    # first launch compiles none of the programs whose sources the set was made from. tools/vita/shader_cache_seed.sh makes
    # the set (the package run once in Vita3K: the same libshacccg) and either packs it into a built .vpk or installs it
    # here for the next build. An empty directory ships none: the first launch compiles, as before.
    set(TE_VITA_SHADER_SEED_DIR "${CMAKE_SOURCE_DIR}/tools/vita/shader_seed" CACHE PATH
        "the shipped shader cache (.tegxp files) packed into the .vpk as app0:shaders/")
    file(GLOB TE_VITA_SHADER_SEED_FILES CONFIGURE_DEPENDS "${TE_VITA_SHADER_SEED_DIR}/*.tegxp")
    set(TE_VITA_VPK_EXTRA "")
    foreach(seed_file ${TE_VITA_SHADER_SEED_FILES})
        get_filename_component(seed_name "${seed_file}" NAME)
        list(APPEND TE_VITA_VPK_EXTRA FILE "${seed_file}" "shaders/${seed_name}")
    endforeach()
    list(LENGTH TE_VITA_SHADER_SEED_FILES TE_VITA_SHADER_SEED_COUNT)
    message(STATUS "TwinsanityEvolved: the shipped shader cache -- ${TE_VITA_SHADER_SEED_COUNT} programs from ${TE_VITA_SHADER_SEED_DIR}")
    vita_create_vpk(twinsanityevolved.vpk ${VITA_TITLEID} eboot.bin VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME} ${TE_VITA_VPK_EXTRA})
    add_custom_target(twinsanityevolved_vpk ALL DEPENDS twinsanityevolved.vpk)
    message(STATUS "TwinsanityEvolved: the Vita lane -- ${CMAKE_BINARY_DIR}/twinsanityevolved.vpk (title id ${VITA_TITLEID}); the judges' tools and the tests are host-only and skipped")
else()
# tools/judges: the graphics seam's host self-test (no game module; its frame is judged by title_frame_judge.py)
add_executable(te_graphics_selftest tools/judges/graphics_selftest.cpp)
target_include_directories(te_graphics_selftest PRIVATE "${CMAKE_SOURCE_DIR}/platform/nativeee/host"
    "${CMAKE_SOURCE_DIR}/platform/nativeee" "${TE_GEN}/seam")
target_compile_options(te_graphics_selftest PRIVATE -Wall -Wextra)
target_link_libraries(te_graphics_selftest PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,te_platform_host>" nativeee_libs te_seam_stubs)
add_dependencies(te_graphics_selftest te_seam nativeee)

# tools/oracle: the perceptual frame descriptor of PPM files through the port's own te::harness::Describe (diff.py runs it
# over the recomp oracle's dumped frames so both legs' frames.pdesc rows come from one implementation)
add_executable(te_pdesc tools/oracle/te_pdesc.cpp)
target_include_directories(te_pdesc PRIVATE "${CMAKE_SOURCE_DIR}/platform/nativeee/host")
target_compile_options(te_pdesc PRIVATE -Wall -Wextra)
target_link_libraries(te_pdesc PRIVATE te_test_support "$<LINK_LIBRARY:WHOLE_ARCHIVE,te_platform_host>" nativeee_libs te_seam_stubs)   # the tests' guest-memory stand-in: no module
add_dependencies(te_pdesc te_seam nativeee)
endif()

message(STATUS "TwinsanityEvolved: ${CMAKE_BUILD_TYPE}; wasi-sdk ${TE_WASI_SDK}; wabt ${TE_WABT}; engine ${TE_ENGINE}")
