/* config.h for DOSBox-X on Thylacine (aarch64-thylacine, Pouch/musl, SDL2).
 *
 * Hand-authored for the Cryptid DOS/Win9x-emulation arc (docs/DOSBOX.md, arc
 * prefix DX), adapted from the upstream vs/config.h template. This is the
 * DX-1 posture: core=normal ONLY, software-surface video via the SDL_thylacine
 * Tapestry backend, sound stubbed to a null mixer, and every external-dep
 * feature (GL, D3D, TTF, zlib, libpng, SDL_net, fluidsynth, mt32, libpcap,
 * libslirp, curses debugger, dynarec) OFF. The later DX sub-chunks light some
 * of these up -- most importantly the dynarec (C_DYNREC / C_DYNAMIC_X86) via
 * CAP_JIT at DX-4.
 *
 * DELIBERATE INVARIANT: an OFF feature is left UNDEFINED, never "#define X 0".
 * DOSBox-X mixes "#if C_FOO" (value) and "#ifdef C_FOO" (definedness) checks;
 * defining an off toggle as 0 would wrongly satisfy the definedness form.
 *
 * The vs/config.h "#if defined(_M_ARM64)" arms are DEAD on aarch64-thylacine
 * (the fork clang defines __aarch64__, not the MSVC _M_ARM64), so their "#else"
 * arms would select the WRONG values (SIZEOF_INT_P=4, C_DYNAMIC_X86, C_FPU_X86).
 * The values below are therefore set unconditionally for LP64 aarch64.
 *
 * The vendored tree is never edited; tools/build.sh::build_dosbox_x copies this
 * file (and config_package.h) into the build-dir src copy (the SDL2/musl idiom).
 */

/* --- Platform selector ---------------------------------------------------
 * Thylacine is a POSIX-ish musl target -- NOT Linux/Windows/macOS/BSD/OS2.
 * Define none of the platform macros; code takes the generic (#else) arms.
 * Where a generic arm is missing, a boundary-line patch (patches/) supplies it.
 *   (LINUX / WIN32 / MACOSX / OS2 / BSD deliberately left undefined) */

/* --- Target CPU + emulator cores ----------------------------------------
 * DX-1 = the interpreter (core=normal) only. The generic recompiler
 * (C_DYNREC) and the x86 dynarec (C_DYNAMIC_X86) are OFF until DX-4 wires
 * them to CAP_JIT. C_TARGETCPU is only consulted inside those ifdefs. */
/* #undef C_TARGETCPU */
/* #undef C_DYNAMIC_X86 */
/* #undef C_DYNREC */

/* Floating point: the portable FPU core (no x86 asm). */
#define C_FPU 1
/* #undef C_FPU_X86 */

/* Compiler capabilities (clang 22, aarch64). */
#define C_HAS_ATTRIBUTE 1
#define C_HAS_BUILTIN_EXPECT 1
#define C_ATTRIBUTE_ALWAYS_INLINE 1
/* #undef C_ATTRIBUTE_FASTCALL */   /* x86-only calling convention */

/* --- Sound: hard v1.0 non-goal (no virtio-sound). The mixer still runs the
 * emulation into a null sink; the external MIDI synths are OFF. */
/* #undef C_FLUIDSYNTH */
/* #undef C_MT32 */

/* --- Video: software surface only for DX-1 (SDL_thylacine -> Tapestry weave).
 * GL / D3D / TTF / xBRZ output backends OFF. */
/* #undef C_OPENGL */
/* #undef C_DIRECT3D */
/* #undef C_D3DSHADERS */
/* #undef C_FREETYPE */
/* #undef C_XBRZ */
/* #undef C_SURFACE_POSTRENDER_ASPECT */

/* --- SDL: the SDL2 path (Thylacine's backend is SDL2). */
/* #undef C_SDL1 */
#define C_SDL2 1

/* --- Networking + host passthrough: OFF (no SDL_net / libpcap / libslirp;
 * no real hardware ports on Thylacine). */
/* #undef C_SDL_NET */
/* #undef C_IPX */
/* #undef C_MODEM */
/* #undef C_PCAP */
/* #undef C_SLIRP */
/* #undef C_DIRECTSERIAL */
/* #undef C_DIRECTLPT */
/* #undef C_PRINTER */

/* --- Compression: zlib IS provided (build_zlib -> the pouch sysroot), because
 * cdrom_image.cpp unity-includes libchdr and include/zip.h both hard-require it.
 * libpng is NOT (screenshots gate off; the bios.cpp png path is C_SSHOT-guarded
 * by patch). */
#define C_LIBZ 1
/* #undef C_LIBPNG */
/* #undef C_SSHOT */

/* --- Internal debugger: OFF (needs curses). */
/* #undef C_DEBUG */
/* #undef C_HEAVY_DEBUG */

/* --- Misc features OFF for DX-1. */
/* #undef C_GAMELINK */
/* #undef C_ICONV */
/* #undef C_AVCODEC */
/* #undef C_SET_PRIORITY */
/* #undef C_X11_XKB */
/* #undef HAVE_ALSA */

/* --- Memory model. aarch64 permits unaligned normal accesses. The dynarec
 * code-cache primitives stay OFF (dynarec off until DX-4). */
#define C_UNALIGNED_MEMORY 1
#define C_HAVE_POSIX_MEMALIGN 1
/* #undef C_HAVE_MMAP */
/* #undef C_HAVE_MPROTECT */
/* #undef C_HAVE_MEMFD_CREATE */
/* #undef C_HAVE_MACH_VM_REMAP */
/* #undef C_HAVE_LINUX_KVM */

/* --- POSIX headers (musl provides these). */
#define STDC_HEADERS 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_MEMORY_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define DIRENT_HAS_D_TYPE 1
#define ENVIRON_INCLUDED 1
#define ENVIRON_LINKED 1
/* Networking / passwd headers exist in musl, but the code paths that use them
 * are OFF at DX-1; leave undefined so the no-net / no-pwd arms are taken. */
/* #undef HAVE_SYS_SOCKET_H */
/* #undef HAVE_NETINET_IN_H */
/* #undef HAVE_PWD_H */
/* #undef TM_IN_SYS_TIME */

/* --- Endianness: aarch64 little-endian. */
/* #undef WORDS_BIGENDIAN */

/* --- sizeof table (aarch64-thylacine, LP64). */
#define SIZEOF_INT_P              8
#define SIZEOF_UNSIGNED_CHAR      1
#define SIZEOF_UNSIGNED_SHORT     2
#define SIZEOF_UNSIGNED_INT       4
#define SIZEOF_UNSIGNED_LONG      8
#define SIZEOF_UNSIGNED_LONG_LONG 8

/* --- const / inline conformance (ANSI C; clang conforms). */
#ifndef CONST
#define CONST const
#endif

#if C_ATTRIBUTE_ALWAYS_INLINE
#define INLINE inline __attribute__((always_inline))
#else
#define INLINE inline
#endif

#if C_ATTRIBUTE_FASTCALL
#define DB_FASTCALL __attribute__((fastcall))
#else
#define DB_FASTCALL
#endif

#if C_HAS_ATTRIBUTE
#define GCC_ATTRIBUTE(x) __attribute__ ((x))
#else
#define GCC_ATTRIBUTE(x) /* attribute not supported */
#endif

#if C_HAS_BUILTIN_EXPECT
#define GCC_UNLIKELY(x) __builtin_expect((x),0)
#define GCC_LIKELY(x) __builtin_expect((x),1)
#else
#define GCC_UNLIKELY(x) (x)
#define GCC_LIKELY(x) (x)
#endif


typedef         double     Real64;

#if SIZEOF_UNSIGNED_CHAR != 1
#  error "sizeof (unsigned char) != 1"
#else
  typedef unsigned char Bit8u;
  typedef   signed char Bit8s;
#endif

#if SIZEOF_UNSIGNED_SHORT != 2
#  error "sizeof (unsigned short) != 2"
#else
  typedef unsigned short Bit16u;
  typedef   signed short Bit16s;
#endif

#if SIZEOF_UNSIGNED_INT == 4
  typedef unsigned int Bit32u;
  typedef   signed int Bit32s;
#elif SIZEOF_UNSIGNED_LONG == 4
  typedef unsigned long Bit32u;
  typedef   signed long Bit32s;
#else
#  error "can't find sizeof(type) of 4 bytes!"
#endif

#if SIZEOF_UNSIGNED_LONG == 8
  typedef unsigned long Bit64u;
  typedef   signed long Bit64s;
#elif SIZEOF_UNSIGNED_LONG_LONG == 8
  typedef unsigned long long Bit64u;
  typedef   signed long long Bit64s;
#else
#  error "can't find data type of 8 bytes"
#endif

#if SIZEOF_INT_P == 4
  typedef Bit32u Bitu;
  typedef Bit32s Bits;
#else
  typedef Bit64u Bitu;
  typedef Bit64s Bits;
#endif

/* Fuck off MSVC I don't care if some C library functions aren't POSIX compliant --J.C. */
#if defined(WIN32)
# pragma warning(disable:4996)
#endif

/*
  Define HAS_CDIRECTLPT as 1 if C_DIRECTLPT is defined (as 1) *and* parallel
  pass-through is available on the current platform. It is only available on
  x86{_64} with Windows or BSD, and on Linux.
*/
#ifdef C_DIRECTLPT
#if (defined __i386__ || defined __x86_64__ || defined _M_IX86 || defined _M_X64) && \
    defined WIN32
#define HAS_CDIRECTLPT 1
#endif
#endif // C_DIRECTLPT
#ifndef HAS_CDIRECTLPT
#define HAS_CDIRECTLPT 0
#endif

/* Linux-side configure script will write/rewrite this file so both Windows and Linux builds carry the same information --J.C. */
#include "config_package.h"
