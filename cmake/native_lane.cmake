# cmake/native_lane.cmake -- the PlayStation Vita's NATIVE build of the game code (TE_VITA_NATIVE=ON; included by the
# top-level CMakeLists.txt in place of its sections 2-5, the wasm32 + wasm2c module). runner/te_native.h has the design.
#
#   1. every game TU (game/src/game/*.cpp, game/src/*.cpp, the decomp's libc / libm / malloc), the module's own platform side
#      (platform/nativeee/wasm/*.cpp), the generated seam forwarders and platform/nativeee/native/*.cpp -> armv7 objects
#      through tools/te_cxx_arm.py (te_cxx.py's rewrites of the decomp's MIPS-only constructs with their ARM forms; zero
#      edits to game code)
#   2. the guest block (tools/gen_retail_image.py --arm: the 32 MB retail address space, every retail data symbol at
#      te_guest_block + its address) and the retail function table (tools/gen_retail_table.py over the native objects)
#   3. tools/native/prelink.py: one relocatable object, every symbol local but the runner's entry points, its remaining
#      references checked (seam bodies, the trampolines, the C runtime)
#   4. the runner's tables: the known retail function starts and the retail data pointer words
#      (tools/native/retail_data_ptrs.tsv)
#
# Output variables for the top-level CMakeLists.txt: TE_NATIVE_GAME_OBJECT (the prelinked object), TE_NATIVE_RUNNER_SOURCES
# (generated C the runner compiles).
set(TE_NATIVE_DIR "${CMAKE_BINARY_DIR}/native")
file(MAKE_DIRECTORY "${TE_NATIVE_DIR}/obj" "${TE_NATIVE_DIR}/rep")
get_filename_component(_te_vita_bin "${CMAKE_C_COMPILER}" DIRECTORY)
set(TE_NATIVE_BINUTILS "${_te_vita_bin}/arm-vita-eabi-")
set(TE_NATIVE_GXX "${_te_vita_bin}/arm-vita-eabi-g++")
if(NOT EXISTS "${TE_NATIVE_GXX}")
    message(FATAL_ERROR "TwinsanityEvolved: TE_VITA_NATIVE needs ${TE_NATIVE_GXX} (the VitaSDK's GCC)")
endif()

file(GLOB TE_GAME_SOURCES CONFIGURE_DEPENDS "${TE_GAME}/src/game/*.cpp" "${TE_GAME}/src/*.cpp")
list(APPEND TE_GAME_SOURCES "${TE_GAME}/src/platform/ps2/libc.cpp" "${TE_GAME}/src/platform/ps2/libm.cpp"
    "${TE_GAME}/src/platform/ps2/malloc.cpp")
file(GLOB TE_WASM_PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/platform/nativeee/wasm/*.cpp")
file(GLOB TE_NATIVE_PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/platform/nativeee/native/*.cpp")
file(GLOB TE_GAME_HEADERS CONFIGURE_DEPENDS "${TE_GAME}/include/*.h" "${TE_GAME}/include/*/*.h"
    "${CMAKE_SOURCE_DIR}/platform/nativeee/sdk_shim/*.h" "${CMAKE_SOURCE_DIR}/platform/nativeee/*.h")
set(TE_NATIVE_OPT "2" CACHE STRING "Vita native lane: the game TUs' optimisation level (tools/te_cxx_arm.py -O)")
set(TE_CXX_ARM ${Python3_EXECUTABLE} "${TE_TOOLS}/te_cxx_arm.py" --gxx "${TE_NATIVE_GXX}" --game "${TE_GAME}"
    --shim "${CMAKE_SOURCE_DIR}/platform/nativeee/sdk_shim" -I "${TE_GEN}/seam" -I "${CMAKE_SOURCE_DIR}/platform/nativeee"
    -D TE_NATIVE=1 -O${TE_NATIVE_OPT})
# TE_PROF_SEAM=ON (a profiling build, as on the module lane): the seam forwarders mark the game thread's profiler section
# with their import's number (tools/gen_seam.py) -- TE_PROF=1 then splits the game thread's "(own code)" into the game's
# own code and each seam body it calls. Off: the markers are nothing.
option(TE_PROF_SEAM "Vita: the profiler's section markers in the seam (TE_PROF=1 names the game thread's seam calls)" OFF)
if(TE_PROF_SEAM)
    list(APPEND TE_CXX_ARM -D TE_PROF_SEAM=1)
endif()
set(TE_NATIVE_OBJECTS "")
foreach(src ${TE_GAME_SOURCES} ${TE_WASM_PLATFORM_SOURCES} ${TE_NATIVE_PLATFORM_SOURCES} ${TE_SEAM_FORWARDERS})
    file(RELATIVE_PATH rel "${CMAKE_SOURCE_DIR}" "${src}")
    string(REPLACE "/" "_" obj "${rel}")
    set(out "${TE_NATIVE_DIR}/obj/${obj}.o")
    add_custom_command(OUTPUT "${out}"
        COMMAND ${TE_CXX_ARM} --report "${TE_NATIVE_DIR}/rep/${obj}.json" -o "${out}" "${src}"
        DEPENDS "${src}" "${TE_TOOLS}/te_cxx_arm.py" "${TE_TOOLS}/te_cxx.py" ${TE_GAME_HEADERS} "${TE_GEN}/seam/ps2e_seam.h"
        COMMENT "armv7 (native) ${rel}")
    list(APPEND TE_NATIVE_OBJECTS "${out}")
endforeach()
set(TE_NATIVE_OBJECTS_RSP "${TE_NATIVE_DIR}/objects.rsp")
list(JOIN TE_NATIVE_OBJECTS "\n" _te_objects_text)
set(_te_objects_text "${_te_objects_text}\n")
if(EXISTS "${TE_NATIVE_OBJECTS_RSP}")
    file(READ "${TE_NATIVE_OBJECTS_RSP}" _te_objects_old)
else()
    set(_te_objects_old "")
endif()
if(NOT _te_objects_text STREQUAL _te_objects_old)
    file(WRITE "${TE_NATIVE_OBJECTS_RSP}" "${_te_objects_text}")
endif()

# the guest block + the retail function table (their symbols from the native objects themselves)
add_custom_command(OUTPUT "${TE_NATIVE_DIR}/guest_block.s" "${TE_NATIVE_DIR}/guest_block.json"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_retail_image.py" --arm --game "${TE_GAME}" --nm "${TE_NATIVE_BINUTILS}nm"
            --out "${TE_NATIVE_DIR}/guest_block.s" --json "${TE_NATIVE_DIR}/guest_block.json" "@${TE_NATIVE_OBJECTS_RSP}"
    DEPENDS "${TE_TOOLS}/gen_retail_image.py" "${TE_TOOLS}/rspfile.py" "${TE_GAME}/symbol_addrs.txt" ${TE_NATIVE_OBJECTS} "${TE_NATIVE_OBJECTS_RSP}"
    COMMENT "gen_retail_image --arm: the guest block, the retail data symbols at their addresses in it")
add_custom_command(OUTPUT "${TE_NATIVE_DIR}/guest_block.o"
    COMMAND "${CMAKE_C_COMPILER}" -c "${TE_NATIVE_DIR}/guest_block.s" -o "${TE_NATIVE_DIR}/guest_block.o"
    DEPENDS "${TE_NATIVE_DIR}/guest_block.s")
add_custom_command(OUTPUT "${TE_NATIVE_DIR}/retail_table.c" "${TE_NATIVE_DIR}/retail_table.json" "${TE_GEN}/retail_functions_host.c"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_retail_table.py" --game "${TE_GAME}" --nm "${TE_NATIVE_BINUTILS}nm"
            --out "${TE_NATIVE_DIR}/retail_table.c" --json "${TE_NATIVE_DIR}/retail_table.json"
            --host-out "${TE_GEN}/retail_functions_host.c" "@${TE_NATIVE_OBJECTS_RSP}"
    DEPENDS "${TE_TOOLS}/gen_retail_table.py" "${TE_TOOLS}/rspfile.py" "${TE_GAME}/symbol_addrs.txt" ${TE_NATIVE_OBJECTS} "${TE_NATIVE_OBJECTS_RSP}"
    COMMENT "gen_retail_table: retail code address -> native function")
add_custom_command(OUTPUT "${TE_NATIVE_DIR}/retail_table.o"
    COMMAND "${CMAKE_C_COMPILER}" -O2 -c "${TE_NATIVE_DIR}/retail_table.c" -o "${TE_NATIVE_DIR}/retail_table.o"
    DEPENDS "${TE_NATIVE_DIR}/retail_table.c")

# the prelinked game
set(TE_NATIVE_GAME_OBJECT "${TE_NATIVE_DIR}/game_native.o")
add_custom_command(OUTPUT "${TE_NATIVE_GAME_OBJECT}" "${TE_NATIVE_DIR}/prelink.json"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/native/prelink.py" --binutils "${TE_NATIVE_BINUTILS}" --out "${TE_NATIVE_GAME_OBJECT}"
            --report "${TE_NATIVE_DIR}/prelink.json" -- ${TE_NATIVE_OBJECTS} "${TE_NATIVE_DIR}/retail_table.o" "${TE_NATIVE_DIR}/guest_block.o"
    DEPENDS "${TE_TOOLS}/native/prelink.py" ${TE_NATIVE_OBJECTS} "${TE_NATIVE_DIR}/retail_table.o" "${TE_NATIVE_DIR}/guest_block.o"
    COMMENT "prelink: the native game as one object (its symbols local but the runner's entry points)")
add_custom_target(te_native_game DEPENDS "${TE_NATIVE_GAME_OBJECT}")
add_dependencies(te_native_game te_seam)

# the runner's generated tables
add_custom_command(OUTPUT "${TE_NATIVE_DIR}/retail_data_ptrs.c"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/native/retail_data_ptrs.py" c "${TE_TOOLS}/native/retail_data_ptrs.tsv"
            --out "${TE_NATIVE_DIR}/retail_data_ptrs.c"
    DEPENDS "${TE_TOOLS}/native/retail_data_ptrs.py" "${TE_TOOLS}/native/retail_data_ptrs.tsv"
    COMMENT "retail_data_ptrs: the retail data image's pointer words (the native lane's relocation list)")
set(TE_NATIVE_RUNNER_SOURCES "${TE_NATIVE_DIR}/retail_data_ptrs.c" "${TE_GEN}/retail_functions_host.c" "${TE_GEN}/seam/seam_prof_names.c")
set_source_files_properties("${TE_GEN}/seam/seam_prof_names.c" PROPERTIES GENERATED TRUE)
message(STATUS "TwinsanityEvolved: the Vita's NATIVE lane (TE_VITA_NATIVE) -- the game code compiled by ${TE_NATIVE_GXX} -O${TE_NATIVE_OPT}, no wasm2c module")
