# Web / WebAssembly (Emscripten) Toolchain
# Requirements:
#   - Emscripten SDK 4.0+ (wasm64 / -sMEMORY64 support)
#     Install: https://emscripten.org/docs/getting_started/downloads.html
#   - EMSDK set by emsdk_env.sh
#
# Setup (headless runtime build, see docs/WEB_BUILDING.md):
#   source <emsdk>/emsdk_env.sh
#   cmake -S glue/rexglue-sdk-main -B out/web -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=toolchains/web-emscripten.cmake \
#         -DCMAKE_BUILD_TYPE=Release -DREXGLUE_BUILD_GTA4_RECOMP=ON

set(LIBERTY_RECOMP_TARGET_PLATFORM "web" CACHE STRING "" FORCE)

if(NOT DEFINED EMSDK)
    if(DEFINED ENV{EMSDK})
        set(EMSDK "$ENV{EMSDK}")
    else()
        message(FATAL_ERROR
            "Web build requires the Emscripten SDK.\n"
            "Run `source <emsdk>/emsdk_env.sh` before configuring, or pass -DEMSDK=<path>.")
    endif()
endif()

# These must be set before Emscripten.cmake is included: it reads
# CMAKE_C_FLAGS to decide between wasm32 and wasm64 (sizeof(void*)), and
# every object must agree on them or wasm-ld refuses to link.
#   -sMEMORY64        64-bit linear memory (the guest needs a full 4 GB window)
#   -pthread          guest threads map to Web Workers on SharedArrayBuffer
#   -fwasm-exceptions native wasm exception handling (toml++/CLI11/std throw)
#   -msimd128 -msse4.2  recompiled SSE intrinsics lower to WASM SIMD128 via
#                     Emscripten's x86 compatibility headers
set(_LIBERTY_WEB_FLAGS "-sMEMORY64=1 -pthread -fwasm-exceptions -msimd128 -msse4.2")
set(CMAKE_C_FLAGS_INIT   "${_LIBERTY_WEB_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${_LIBERTY_WEB_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_LIBERTY_WEB_FLAGS}")
set(CMAKE_C_FLAGS "${_LIBERTY_WEB_FLAGS}" CACHE STRING "")
set(CMAKE_CXX_FLAGS "${_LIBERTY_WEB_FLAGS}" CACHE STRING "")

include("${EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
