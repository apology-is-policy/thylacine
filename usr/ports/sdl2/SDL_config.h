/* SDL_config.h for aarch64-thylacine (the G-7 SDL seam; TAPESTRY.md §9).
 *
 * Hand-generated, the libsodium precedent: pouch has no autoconf, so this
 * is what ./configure would have produced for the pouch cross-toolchain
 * (musl libc + the usr/lib/pouch/patches boundary line). build_sdl2()
 * copies it over include/SDL_config.h in the BUILD-DIR copy of the
 * vendored tree (third_party/SDL2 itself stays pristine).
 *
 * Driver set (mirrors the pruned vendored tree — PRUNE-MANIFEST.md):
 *   video    = thylacine (usr/ports/sdl2/thylacine/) + dummy
 *   audio    = dummy (no virtio-sound yet — TAPESTRY.md §10 item 4)
 *   thread   = pthread (pouch patch 0004)
 *   timer    = unix (clock_gettime = 75; nanosleep = torpor, patch 0022)
 *   the rest = dummy/disabled stubs
 */

#ifndef SDL_config_h_
#define SDL_config_h_

#include "SDL_platform.h"

/* C library headers (musl provides the full set). */
#define STDC_HEADERS 1
#define HAVE_CTYPE_H 1
#define HAVE_FLOAT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_MATH_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_WCHAR_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_LIBC 1

/* C library functions (musl). */
#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_GETENV 1
#define HAVE_SETENV 1
#define HAVE_PUTENV 1
#define HAVE_UNSETENV 1
#define HAVE_QSORT 1
#define HAVE_BSEARCH 1
#define HAVE_ABS 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP 1
#define HAVE_STRLEN 1
#define HAVE_STRNLEN 1
#define HAVE_STRLCPY 1
#define HAVE_STRLCAT 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRTOK_R 1
#define HAVE_STRTOL 1
#define HAVE_STRTOUL 1
#define HAVE_STRTOLL 1
#define HAVE_STRTOULL 1
#define HAVE_STRTOD 1
#define HAVE_ATOI 1
#define HAVE_ATOF 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
#define HAVE_SSCANF 1
#define HAVE_VSSCANF 1
#define HAVE_SNPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_SETJMP 1
#define HAVE_FSEEKO 1

/* Math (musl libm is complete; SDL's internal libm covers any gap). */
#define HAVE_ACOS 1
#define HAVE_ACOSF 1
#define HAVE_ASIN 1
#define HAVE_ASINF 1
#define HAVE_ATAN 1
#define HAVE_ATANF 1
#define HAVE_ATAN2 1
#define HAVE_ATAN2F 1
#define HAVE_CEIL 1
#define HAVE_CEILF 1
#define HAVE_COPYSIGN 1
#define HAVE_COPYSIGNF 1
#define HAVE_COS 1
#define HAVE_COSF 1
#define HAVE_EXP 1
#define HAVE_EXPF 1
#define HAVE_FABS 1
#define HAVE_FABSF 1
#define HAVE_FLOOR 1
#define HAVE_FLOORF 1
#define HAVE_FMOD 1
#define HAVE_FMODF 1
#define HAVE_LOG 1
#define HAVE_LOGF 1
#define HAVE_LOG10 1
#define HAVE_LOG10F 1
#define HAVE_LROUND 1
#define HAVE_LROUNDF 1
#define HAVE_POW 1
#define HAVE_POWF 1
#define HAVE_ROUND 1
#define HAVE_ROUNDF 1
#define HAVE_SCALBN 1
#define HAVE_SCALBNF 1
#define HAVE_SIN 1
#define HAVE_SINF 1
#define HAVE_SQRT 1
#define HAVE_SQRTF 1
#define HAVE_TAN 1
#define HAVE_TANF 1
#define HAVE_TRUNC 1
#define HAVE_TRUNCF 1

/* OS surface actually wired by the pouch boundary line. */
#define HAVE_NANOSLEEP 1      /* patch 0022: torpor-backed */
#define HAVE_CLOCK_GETTIME 1  /* SYS_CLOCK_GETTIME = 75 (patch 0001) */
#define HAVE_SIGACTION 1      /* patch 0007: notes-backed signals */
#define HAVE_SYSCONF 1        /* musl; unwired values fail soft */
#define HAVE_GETAUXVAL 1      /* musl reads the exec auxv (AT_HWCAP live) */
#define HAVE_POSIX_MEMALIGN 1

/* Atomics: clang builtins on aarch64. */
#define HAVE_GCC_ATOMICS 1

/* The static-link posture: no dlopen, no hidapi. (SDL_DYNAMIC_API 0 is
 * deliberately NOT here — SDL #errors on a config-forced value; the
 * sanctioned off-switch is the __thylacine__ arm the 0001 patch adds to
 * SDL_dynapi.h's master platform switch, keyed on -D__thylacine__.) */
#define SDL_HIDAPI_DISABLED 1

/* Subsystem drivers (each names a kept src/ dir — PRUNE-MANIFEST.md). */
#define SDL_AUDIO_DRIVER_DUMMY 1
#define SDL_JOYSTICK_DUMMY 1
#define SDL_HAPTIC_DUMMY 1
#define SDL_SENSOR_DUMMY 1
#define SDL_LOADSO_DUMMY 1
#define SDL_THREAD_PTHREAD 1
#define SDL_THREAD_PTHREAD_RECURSIVE_MUTEX 1
#define SDL_TIMER_UNIX 1
#define SDL_FILESYSTEM_DUMMY 1
#define SDL_POWER_DISABLED 1
#define SDL_LOCALE_DUMMY 1
#define SDL_MISC_DUMMY 1

/* Video: the tapestry compositor client (usr/ports/sdl2/thylacine/). */
#define SDL_VIDEO_DRIVER_THYLACINE 1
#define SDL_VIDEO_DRIVER_DUMMY 1

#endif /* SDL_config_h_ */
