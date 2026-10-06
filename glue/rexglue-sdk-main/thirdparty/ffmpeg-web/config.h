/* FFmpeg configuration for the web (Emscripten, wasm64) build.
 *
 * FFmpeg has no WebAssembly target. Emscripten's musl libc matches Linux
 * closely, so start from the Linux AArch64 configuration (which has no x86
 * assembly) and switch off every architecture, SIMD and inline-assembly
 * feature so only FFmpeg's portable C paths are compiled.
 *
 * This directory is placed ahead of the FFmpeg source root on the include
 * path for libavutil/libavcodec on Emscripten only. */
#ifndef REX_FFMPEG_WEB_CONFIG_H
#define REX_FFMPEG_WEB_CONFIG_H

#include "config_linux_aarch64.h"

#undef ARCH_AARCH64
#define ARCH_AARCH64 0
#undef HAVE_ARMV8
#define HAVE_ARMV8 0
#undef HAVE_NEON
#define HAVE_NEON 0
#undef HAVE_VFP
#define HAVE_VFP 0
#undef HAVE_ARMV8_EXTERNAL
#define HAVE_ARMV8_EXTERNAL 0
#undef HAVE_NEON_EXTERNAL
#define HAVE_NEON_EXTERNAL 0
#undef HAVE_VFP_EXTERNAL
#define HAVE_VFP_EXTERNAL 0
#undef HAVE_ARMV8_INLINE
#define HAVE_ARMV8_INLINE 0
#undef HAVE_NEON_INLINE
#define HAVE_NEON_INLINE 0
#undef HAVE_VFP_INLINE
#define HAVE_VFP_INLINE 0
#undef HAVE_INLINE_ASM
#define HAVE_INLINE_ASM 0
#undef HAVE_INLINE_ASM_LABELS
#define HAVE_INLINE_ASM_LABELS 0
#undef HAVE_INLINE_ASM_NONLOCAL_LABELS
#define HAVE_INLINE_ASM_NONLOCAL_LABELS 0
#undef HAVE_SCHED_GETAFFINITY
#define HAVE_SCHED_GETAFFINITY 0
/* No arc4random in Emscripten; FFmpeg falls back to /dev/urandom, which
 * Emscripten backs with crypto.getRandomValues(). */
#undef HAVE_ARC4RANDOM
#define HAVE_ARC4RANDOM 0

#endif /* REX_FFMPEG_WEB_CONFIG_H */
