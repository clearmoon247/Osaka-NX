# Osaka-NX: Nintendo Switch build of the PokemonStadiumRecomp recompilation. Own root (like DK64Recomp-NX and the Android ports) so the
# upstream CMakeLists, which requires RT64, recomp-ui and Windows toolchains, stays untouched.
# Build with switch/build.sh (devkitA64 container).
cmake_minimum_required(VERSION 3.24)
project(osaka_nx LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Shim headers (sys/mman.h, dlfcn.h) take precedence everywhere, including inside N64ModernRuntime.
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/shim)
# Same switches as DK64Recomp-NX: newlib hides POSIX under strict -std=c++20, and has no putc_unlocked.
add_compile_definitions(_GNU_SOURCE=1 FMT_USE_FALLBACK_FILE=1)
# Frame records let crash_switch.c walk the call stack.
add_compile_options(-fno-omit-frame-pointer)
# Source paths in the binary relative to the repository: no local paths in a built .nro.
get_filename_component(ROOT_ABS ${ROOT} ABSOLUTE)
add_compile_options(-ffile-prefix-map=${ROOT_ABS}/=)
# fmt's os.cc needs fdopen/fileno, which newlib hides; nothing here uses fmt::file.
set(FMT_OS OFF CACHE BOOL "" FORCE)

# The runtime pulls in N64Recomp (lib/N64ModernRuntime/N64Recomp, EXCLUDE_FROM_ALL: only its libraries are built) and
# LiveRecomp. Nothing built here loads mods; libtcc is never present (dlopen is a stub), so the JIT tiers stay off.
add_subdirectory(${ROOT}/lib/N64ModernRuntime ${CMAKE_BINARY_DIR}/N64ModernRuntime)

find_package(SDL2 REQUIRED)

set(RECOMP_INCLUDES
    ${ROOT}/include
    ${ROOT}/lib/N64ModernRuntime/ultramodern/include
    ${ROOT}/lib/N64ModernRuntime/librecomp/include
    ${ROOT}/lib/N64ModernRuntime/N64Recomp/include
)
# Generated code: the flags the PC build uses, plus the C diagnostics GCC (unlike clang) makes errors.
set(RECOMP_C_FLAGS
    -fno-strict-aliasing
    -Wno-unused-variable
    $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>
    $<$<COMPILE_LANGUAGE:C>:-Wno-int-conversion>
    $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>
)

# Size of the generated code (about 90% of the .nro). Measured with switch/tools/size_experiment.sh on generated/funcs_9.c:
#   as built (-O2, probe in)           274 KB code + 27 KB unwind data
#   without the recomp_cf_note probe   127 KB code +  5 KB unwind data      <- default here, see shim_generated/recomp.h
#   plus -Os                           114 KB code
#   -O1 is bigger than -O2 (170 KB); -O3 is the same as -O2.
# Unwind tables are KEPT on purpose: osDestroyThread throws ultramodern::thread_terminated and that C++ exception unwinds through
# the generated C frames up to the thread's catch, so frames without unwind data would turn it into std::terminate.
#   PSR_KEEP_CF_NOTE=1   keep the control-flow probe (needed only to debug with PSR_CFDIAG)
#   PSR_SMALL_CODE=OS    additionally build the generated code with -Os (about 10% smaller again, somewhat slower at run time)
#   PSR_GENERATED_DEBUG=1  keep -g on the generated code (default: none; it only makes the .elf and the link larger, the .nro never
#                          holds debug info, and symbolize.sh needs only the symbol table)
# Read from the environment on every configure. Changing any of them recompiles all of generated/*.c.
if(NOT "$ENV{PSR_SMALL_CODE}" STREQUAL "")
    set(PSR_SMALL_CODE "$ENV{PSR_SMALL_CODE}")
else()
    set(PSR_SMALL_CODE "OFF")
endif()
message(STATUS "PSR_SMALL_CODE=${PSR_SMALL_CODE}")
if(PSR_SMALL_CODE STREQUAL "ON")
    message(WARNING "PSR_SMALL_CODE=ON (no unwind tables) is no longer supported: thread_terminated must unwind through generated code. Use OS.")
endif()
if(PSR_SMALL_CODE STREQUAL "OS")
    list(APPEND RECOMP_C_FLAGS $<$<COMPILE_LANGUAGE:C>:-Os>)
endif()
if(NOT "$ENV{PSR_GENERATED_DEBUG}" STREQUAL "1")
    list(APPEND RECOMP_C_FLAGS $<$<COMPILE_LANGUAGE:C>:-g0>)
endif()

# The game's RSP microcode (rsp/*.cpp, listed further down) is also made by switch/codegen.sh from your own ROM.
foreach(_rsp aspMain gbTowerMain gbTowerColorMain njpgdspMain)
    if(NOT EXISTS ${ROOT}/rsp/${_rsp}.cpp)
        message(FATAL_ERROR "rsp/${_rsp}.cpp missing: run switch/codegen.sh <your ROM> (it makes the RSP code from your ROM; it is not stored in the repository)")
    endif()
endforeach()

# ~1000 generated functions; produced by switch/codegen.sh from your own ROM (nothing derived from it is tracked).
file(GLOB FUNC_SOURCES CONFIGURE_DEPENDS ${ROOT}/generated/*.c)
if(NOT FUNC_SOURCES)
    message(FATAL_ERROR "generated/*.c missing: run switch/codegen.sh first")
endif()
# Biggest files first. Compile time grows faster than file size, so with several compiles running at once the build finishes
# sooner if the slowest files start first (otherwise one huge file started last keeps a single processor busy while the
# others sit idle). CMake and make start objects in the order they are listed here. The sort key is the file size padded
# with zeros to 12 digits, so a plain text sort is a numeric sort; the key is stripped again afterwards.
set(_sized_sources "")
foreach(_src IN LISTS FUNC_SOURCES)
    file(SIZE ${_src} _src_size)
    string(LENGTH "${_src_size}" _digits)
    math(EXPR _pad "12 - ${_digits}")
    string(REPEAT "0" ${_pad} _zeros)
    list(APPEND _sized_sources "${_zeros}${_src_size}|${_src}")
endforeach()
list(SORT _sized_sources COMPARE STRING ORDER DESCENDING)
set(FUNC_SOURCES "")
foreach(_entry IN LISTS _sized_sources)
    string(REGEX REPLACE "^[0-9]+\\|" "" _src "${_entry}")
    list(APPEND FUNC_SOURCES "${_src}")
endforeach()
# The few huge files (the biggest are about 19 MB of C and need several GB of memory each, measured peak for a 2.7 MB file is 380 MB)
# are compiled in libraries of their own, one file per library, chained one after the other. Everything else compiles in parallel
# next to that chain. Result: a high -j (see PSR_JOBS in build.sh) can no longer start several multi-GB compiles at the same time,
# which is what made a low, safe job count necessary before.
#   PSR_BIG_FILE_BYTES=N  files larger than this are "big" (default 6000000; 0 = no separate handling)
#   PSR_BIG_LANES=N       how many big files may compile at the same time (default 1; use 2 with 16 GB or more of free memory)
# Read from the environment on every configure. Changing either one moves some objects to another library, so those files (and
# only those) are compiled again.
if(NOT "$ENV{PSR_BIG_FILE_BYTES}" STREQUAL "")
    set(PSR_BIG_FILE_BYTES "$ENV{PSR_BIG_FILE_BYTES}")
else()
    set(PSR_BIG_FILE_BYTES 6000000)
endif()
if(NOT "$ENV{PSR_BIG_LANES}" STREQUAL "")
    set(PSR_BIG_LANES "$ENV{PSR_BIG_LANES}")
else()
    set(PSR_BIG_LANES 1)
endif()
if(PSR_BIG_LANES LESS 1)
    set(PSR_BIG_LANES 1)
endif()
# Two mechanisms, because the generators differ: the chain of add_dependencies below orders the big libraries with "Unix Makefiles"
# (CMake's default on Linux); Ninja ignores such a chain for static libraries, so it gets a job pool that lets PSR_BIG_LANES
# of them run at once. Each generator ignores the mechanism meant for the other.
set_property(GLOBAL PROPERTY JOB_POOLS psr_big=${PSR_BIG_LANES})
set(_small_sources "")
set(_big_sources "")
foreach(_src IN LISTS FUNC_SOURCES)
    file(SIZE ${_src} _src_size)
    if(PSR_BIG_FILE_BYTES GREATER 0 AND _src_size GREATER PSR_BIG_FILE_BYTES)
        list(APPEND _big_sources "${_src}")
    else()
        list(APPEND _small_sources "${_src}")
    endif()
endforeach()

add_library(RecompiledFuncs STATIC ${_small_sources})
set(_recompiled_targets RecompiledFuncs)
set(_big_targets "")
set(_lane_last_target "")           # per lane: the target that the next big file of that lane has to wait for
set(_lane 0)
foreach(_src IN LISTS _big_sources)
    get_filename_component(_name ${_src} NAME_WE)
    set(_target "RecompiledBig_${_name}")
    add_library(${_target} STATIC ${_src})
    set_property(TARGET ${_target} PROPERTY JOB_POOL_COMPILE psr_big)
    list(APPEND _recompiled_targets ${_target})
    list(APPEND _big_targets ${_target})
    list(LENGTH _lane_last_target _lane_count)
    if(_lane LESS _lane_count)
        list(GET _lane_last_target ${_lane} _prev)
        if(NOT "${_prev}" STREQUAL "")
            add_dependencies(${_target} ${_prev})
        endif()
        list(REMOVE_AT _lane_last_target ${_lane})
        list(INSERT _lane_last_target ${_lane} ${_target})
    else()
        list(APPEND _lane_last_target ${_target})
    endif()
    math(EXPR _lane "(${_lane} + 1) % ${PSR_BIG_LANES}")
endforeach()
list(LENGTH _big_targets _big_count)
message(STATUS "generated code: ${_big_count} big file(s) compiled ${PSR_BIG_LANES} at a time, the rest in parallel")

foreach(_target IN LISTS _recompiled_targets)
    target_include_directories(${_target} PRIVATE ${RECOMP_INCLUDES})
    target_compile_options(${_target} PRIVATE ${RECOMP_C_FLAGS})
    # The shim directory always comes first: it declares the call-site helpers (call_post_switch.c) and, unless PSR_KEEP_CF_NOTE=1,
    # compiles the recomp_cf_note probe out.
    target_include_directories(${_target} BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim_generated)
    if("$ENV{PSR_KEEP_CF_NOTE}" STREQUAL "1")
        target_compile_definitions(${_target} PRIVATE PSR_KEEP_CF_NOTE)
    endif()
endforeach()
if("$ENV{PSR_KEEP_CF_NOTE}" STREQUAL "1")
    message(STATUS "PSR_KEEP_CF_NOTE=1: generated code keeps the recomp_cf_note probe")
endif()
# funcs_4.c hung cc1 for hours at -O2 on devkitA64 GCC 16; -O1 compiles it.
set_source_files_properties(${ROOT}/generated/funcs_4.c PROPERTIES COMPILE_OPTIONS "-O1")

# GLideN64 as the renderer: mupen64plus plugin API, desktop GL through Mesa/EGL (like mupen64plus-libretro-nx on
# Switch), no texture packs (NOHQ) and no OSD font (NO_OSD). The same build DK64Recomp-NX runs.
set(GLIDEN64_DIR ${ROOT}/lib/GLideN64/src)
include(gliden64_sources.cmake)
# The ARM64 setup Android builds use: hardware CRC32 for texture hashing, NEON for matrices and vertex transforms.
list(REMOVE_ITEM GLIDEN64_SOURCES ${GLIDEN64_DIR}/CRC32.cpp ${GLIDEN64_DIR}/3DMath.cpp ${GLIDEN64_DIR}/RSP_LoadMatrix.cpp)
list(APPEND GLIDEN64_SOURCES
    ${GLIDEN64_DIR}/CRC32_ARMV8.cpp
    ${GLIDEN64_DIR}/Neon/3DMathNeon.cpp
    ${GLIDEN64_DIR}/Neon/gSPNeon.cpp
    ${GLIDEN64_DIR}/Neon/RSP_LoadMatrixNeon.cpp
)
add_library(GLideN64 STATIC ${GLIDEN64_SOURCES})
target_include_directories(GLideN64 PRIVATE
    ${GLIDEN64_DIR}
    ${GLIDEN64_DIR}/inc
    ${GLIDEN64_DIR}/osal
    ${CMAKE_CURRENT_SOURCE_DIR}/gliden64
    $ENV{DEVKITPRO}/portlibs/switch/include
)
target_compile_definitions(GLideN64 PRIVATE MUPENPLUSAPI TXFILTER_LIB OS_LINUX EGL NDEBUG __NEON_OPT __VEC4_OPT)
# GLideN64 targets C++17; its winlnxdefs.h #defines true/false, which breaks C++20's <format>.
set_target_properties(GLideN64 PROPERTIES CXX_STANDARD 17)
target_compile_options(GLideN64 PRIVATE -Wno-narrowing $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>)

add_executable(osaka-nx
    # Upstream runner sources that are portable (compile-checked on x86 Linux only for extras.c, gb_audio.cpp,
    # rsp_aspmain_hook.cpp, transfer_pak.cpp: no Windows headers needed).
    ${ROOT}/extras.c
    ${ROOT}/generated/lookup.cpp
    ${ROOT}/src/main/register_overlays.cpp
    ${ROOT}/src/main/rsp_aspmain_hook.cpp
    ${ROOT}/src/main/transfer_pak.cpp
    ${ROOT}/src/main/gb_audio.cpp
    # Recompiled RSP microcode (committed upstream): audio, GB Tower display/colour, JPEG.
    ${ROOT}/rsp/aspMain.cpp
    ${ROOT}/rsp/gbTowerMain.cpp
    ${ROOT}/rsp/gbTowerColorMain.cpp
    ${ROOT}/rsp/njpgdspMain.cpp
    # Switch layer.
    audio_switch.cpp
    core_switch.cpp
    call_post_switch.c
    crash_switch.c
    gbpicker.cpp
    gbpicker_gfx.cpp
    gbpicker_switch.cpp
    gbtower_dump_switch.c
    stadium_skin.cpp
    input_switch.cpp
    main_switch.cpp
    mman_switch.c
    psrnx_config.cpp
    runtime_hooks_switch.cpp
    render_gliden64.cpp
    rsp_select.cpp
    stadium_setup.cpp
    threads_switch.c
    window_switch.c
)
target_include_directories(osaka-nx PRIVATE
    ${RECOMP_INCLUDES}
    ${ROOT}/src/main
    ${ROOT}/lib/N64ModernRuntime/thirdparty/sse2neon
    ${ROOT}/lib/N64ModernRuntime/thirdparty/concurrentqueue
    ${ROOT}/lib/GLideN64/src/inc
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_options(osaka-nx PRIVATE -fno-strict-aliasing -fms-extensions)
# libnx gives std::thread 128 KiB stacks and cannot detach threads: see threads_switch.c.
target_link_options(osaka-nx PRIVATE -Wl,--wrap=pthread_create,--wrap=pthread_detach,--wrap=pthread_join,--wrap=ultramodern_scheduler_tick_vram,--wrap=pkmnstadium_gbtower_state_trace)
# The recompiled library and the runtime call into each other in both directions; CMake only predefines RESCAN for
# desktop platforms.
string(REPLACE ";" "," _recompiled_link "${_recompiled_targets}")      # a link group takes a comma-separated list
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
target_link_libraries(osaka-nx PRIVATE
    "$<LINK_GROUP:RESCAN,${_recompiled_link},librecomp,ultramodern>"
    GLideN64
    SDL2::SDL2
    EGL
    GLESv2
    glapi
    drm_nouveau
    nx
)

nx_generate_nacp(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/osaka-nx.nacp
    NAME "Osaka-NX"
    AUTHOR "Osaka-NX contributors (Switch port); based on mstan/PokemonStadiumRecomp"
    VERSION "0.1.0"
)
nx_create_nro(osaka-nx_nro
    TARGET osaka-nx
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/osaka-nx.nro
    ICON ${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg
    NACP ${CMAKE_CURRENT_BINARY_DIR}/osaka-nx.nacp
)
