//===-- aarch64-thylacine.c — Thylacine's arm of compiler-rt's LSE probe ---===//
//
// W1u-b (#71). compiler-rt's `cpu_model/aarch64.c` detects FEAT_LSE at startup
// and publishes the answer in `__aarch64_have_lse_atomics`, which every
// outline-atomics helper (`__aarch64_cas*` / `swp*` / `ldadd*` / ...) branches
// on. Its detection is a per-OS `#if` chain — Linux, FreeBSD, Fuchsia,
// Android, Windows — ending in:
//
//     #else
//     // When unimplemented, we leave __aarch64_have_lse_atomics initialized
//     // to false.
//     #endif
//
// Thylacine lands in that `#else`, so W1u-a shipped correct-but-slow: every
// pouch binary took the LL/SC arm even on a FEAT_LSE core. This file is the
// missing arm. The kernel already publishes the truth — `AT_HWCAP` bit 8, from
// `arch/arm64/hwfeat.c` (landed by the CF-4 A AEAD work) — so this is a
// wire-up, not a new mechanism.
//
// WHY A WRAPPER INSTEAD OF A PATCH. Three options were live; this is the one
// whose failure mode is loud:
//
//   - Adding a `#elif defined(__thylacine__)` arm to their `#if` chain reads
//     best (the macro is real -- the fork clang's ThylacineTargetInfo defines
//     it, as does the pouch cmake toolchain), but `third_party/compiler-rt` is
//     a byte-pristine vendor drop; Thylacine's OTHER `__thylacine__` arm lives
//     in the LLVM fork (CL-3b), not here. An edit inside the drop is silently
//     LOST on a re-vendor, and a lost LSE probe is a perf regression nobody
//     notices. This wrapper `#include`s their file instead, so a re-vendor
//     that moves or restructures it is a BUILD ERROR.
//   - `-D__linux__` for this one TU would take their Linux arm verbatim, but
//     it is a lie to the toolchain that also silently re-routes the FMV block
//     below it.
//   - A separate object with only a constructor would never be LINKED: a
//     static-archive member is pulled in only to resolve an undefined symbol,
//     and nothing references a constructor. Textual inclusion puts our
//     constructor in the same object that DEFINES the flag, so it arrives
//     exactly when — and only when — outline atomics are actually used.
//
// `tools/build.sh::build_compiler_rt` compiles THIS file in place of
// `cpu_model/aarch64.c`; the vendored tree stays byte-pristine.
//
//===----------------------------------------------------------------------===//

#include "cpu_model/aarch64.c"

// getauxval + AT_HWCAP + HWCAP_ATOMICS. musl's <sys/auxv.h> pulls <elf.h>
// (AT_HWCAP) and <bits/hwcap.h> (HWCAP_ATOMICS) itself. Kept below the
// inclusion above so their file still sees the include set upstream chose.
#include <sys/auxv.h>

// The ATOMICS bit is spelled in three places that must agree: the kernel's
// THWCAP_ATOMICS (arch/arm64/hwfeat.c), musl's bits/hwcap.h (used here), and
// compiler-rt's own cpu_model/aarch64/hwcap.inc (used on their Linux arm).
// It is Linux uapi, so it does not move; assert it anyway, because a silent
// disagreement would read the WRONG hwcap bit and could set the flag on a
// core with no FEAT_LSE — the one failure mode here that is not fail-safe.
_Static_assert(HWCAP_ATOMICS == (1 << 8),
               "AT_HWCAP ATOMICS bit disagrees with the kernel's THWCAP_ATOMICS");

// Priority 90 is compiler-rt's own CONSTRUCTOR_ATTRIBUTE (cpu_model.h) — it
// runs ahead of application constructors (which start at 101), so a C++ static
// initializer that takes an atomic already sees the answer. Anything that runs
// EARLIER simply takes the LL/SC arm: correct, just slower, which is the same
// fail-safe posture the zero-initialized flag gives us.
//
// A plain store is deliberate (it mirrors upstream's getauxval.inc): this runs
// single-threaded before main, and the thread-spawn syscall that could publish
// the address space to a peer is itself a barrier.
static void CONSTRUCTOR_ATTRIBUTE thylacine_init_have_lse_atomics(void) {
  __aarch64_have_lse_atomics = (getauxval(AT_HWCAP) & HWCAP_ATOMICS) != 0;
}
