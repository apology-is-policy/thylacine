// Halls of Extinction -- Tier-1 crash dump (HX-1).
//
// When the kernel dies, that boot's lineage is extinct (see
// kernel/extinction.c). The Halls record the fallen: at the fatal moment,
// capture everything hard or impossible to obtain after the fact --
// registers, a frame-pointer backtrace, a stack hexdump, the KASLR slide --
// and UART it under a greppable "HALLS:" prefix, AFTER the "EXTINCTION: "
// ABI line (TOOLING.md section 10) and BEFORE _torpor().
//
// Design scripture: docs/HALLS-OF-EXTINCTION.md. As-built: docs/reference.
//
// AUDIT-TRIGGER SURFACE (CLAUDE.md / ARCHITECTURE.md section 25.4: exception
// entry). The per-CPU live-frame pointer is set/restored by the exception
// entry C handlers; the dump reads the saved register frame and walks a
// live stack. Invariants HX-I1 (re-entrancy guard) + HX-I2 (bounded,
// sanity-gated fp walk) keep a dump that faults from looping or recursing.

#ifndef THYLACINE_ARCH_ARM64_HALLS_H
#define THYLACINE_ARCH_ARM64_HALLS_H

#include <thylacine/types.h>

struct exception_context;

// Tier-1 crash dump. Called from extinction()/extinction_with_addr() AFTER
// the "EXTINCTION: <msg>" line, BEFORE _torpor(). `ctx`:
//   - non-NULL: dump that saved exception frame (the fault/IRQ/unexpected
//     entry register state -- the valuable case: a kernel fault's full
//     register set + backtrace).
//   - NULL: consult the per-CPU live exception frame (set by the exception
//     entry handlers via halls_enter_frame). If that is also NULL -- a bare
//     extinction()/ASSERT_OR_DIE deep in code with no exception frame --
//     capture the CURRENT sp/fp/lr and walk from there (the GP register
//     VALUES at the assert are gone post-call, so they are labelled
//     unavailable; the backtrace is the artifact).
// HX-I1: re-entrant-safe. A fault DURING the dump trips a per-CPU guard and
// returns immediately; the caller then reaches _torpor().
void halls_dump(const struct exception_context *ctx);

// Per-CPU live-exception-frame tracking. The exception entry C handlers
// (exception.c) call halls_enter_frame at the top and halls_leave_frame on
// normal return. An extinction (noreturn) inside the handler skips the
// leave, so the per-CPU slot still points at the dying frame when
// halls_dump(NULL) reads it from the extinction path.
//
// enter returns the previous slot value; leave restores it -- so a nested
// exception (rare at v1.0; handlers do not migrate CPUs mid-execution)
// saves/restores correctly rather than clobbering an outer frame.
const struct exception_context *halls_enter_frame(const struct exception_context *ctx);
void halls_leave_frame(const struct exception_context *prev);

// The current per-CPU live exception frame (NULL if none). For tests.
const struct exception_context *halls_current_frame(void);

// HX-I2 (pure; exposed for tests): frame-pointer sanity gate. True iff `fp`
// is a plausible next AAPCS64 frame: 16-byte aligned, strictly greater than
// `prev_fp` (the kernel stack grows down, so caller frames sit at higher
// addresses -- the chain is strictly increasing), and within [lo, hi). The
// strict-increase kills cycles; [lo, hi) + a caller-side depth cap bound the
// walk; reading a sane-but-unmapped fp still faults but trips HX-I1.
bool halls_fp_is_sane(u64 fp, u64 prev_fp, u64 lo, u64 hi);

// HX-I4 (pure; exposed for tests): true iff the per-CPU slot `frame` is a
// PLAUSIBLE live exception frame given the dumper's current SP -- it must sit
// at-or-above cur_sp and within one stack of it (the handler -> extinction ->
// halls_dump chain is shallow). A slot stranded by a since-exited Proc points
// at a freed/other-thread stack (gap huge or negative) and is rejected, so a
// stale slot never produces a fabricated dump nor suppresses capture-current.
bool halls_frame_is_live(u64 frame, u64 cur_sp);

// Translate a runtime code address to its link-time address by removing the
// KASLR slide (so `addr2line -e thylacine.elf <link-addr>` resolves it).
// Underflow-guarded: if addr < offset (addr is not a slid kernel code
// address -- a stack/heap/zero value), returns addr unchanged.
u64 halls_link_addr(u64 addr, u64 kaslr_offset);

#endif // THYLACINE_ARCH_ARM64_HALLS_H
