# 101 -- Halls of Extinction (Tier-1 crash dump)

**Status**: Tier 1 LANDED (HX-1). Tier 2 (in-kernel symbols) + Tier 3
(persistence) are designed in `docs/HALLS-OF-EXTINCTION.md`, not yet built.

## Purpose

When the kernel dies, `extinction()` prints `EXTINCTION: <msg>` and halts. Pre
HX-1 that single line -- plus, for `extinction_with_addr`, one hex address --
was the entire record. The Halls turn the fatal moment into a full crash scene:
on every extinction, after the ABI line and before `_torpor()`, dump the saved
register frame, a frame-pointer backtrace, the KASLR slide, and a stack
hexdump, all under a greppable `HALLS:` prefix. The motivating bug is #806 (a
kernel-stack overflow whose bare faulting address carried almost no signal); the
dump's backtrace + KASLR-relative link addresses turn that into a one-boot
diagnosis. Sits at the bottom of the exception stack, called from
`kernel/extinction.c`; arch-specific (reads `struct exception_context`, walks
the AAPCS64 x29 chain, uses `xpaci` + the KASLR accessors), so it lives in
`arch/arm64/`.

## Public API

```c
// arch/arm64/halls.h

// Tier-1 dump. Called from extinction()/extinction_with_addr() after the
// "EXTINCTION: " line, before _torpor(). ctx==NULL -> consult the per-CPU
// live exception frame, falling back to capture-current for a bare assert.
void halls_dump(const struct exception_context *ctx);

// Per-CPU live-exception-frame tracking. The exception entry C handlers
// call enter at the top, leave on normal return. enter returns the prior
// slot; leave restores it (correct save/restore on nesting).
const struct exception_context *halls_enter_frame(const struct exception_context *ctx);
void halls_leave_frame(const struct exception_context *prev);
const struct exception_context *halls_current_frame(void);   // for tests

// Pure helpers (exposed for tests).
bool halls_fp_is_sane(u64 fp, u64 prev_fp, u64 lo, u64 hi);   // HX-I2 gate
u64  halls_link_addr(u64 addr, u64 kaslr_offset);             // runtime -> link VA
```

## Implementation

`arch/arm64/halls.c`.

### How the dump reaches the dying register frame

`extinction()` is called from hundreds of sites and does not carry a
`struct exception_context`. Rather than thread `ctx` through every signature,
HX-1 uses a **per-CPU live-frame slot** `g_halls_frame[DTB_MAX_CPUS]`:

- The four exception entry C handlers in `arch/arm64/exception.c`
  (`exception_sync_curr_el`, `exception_irq_curr_el`,
  `exception_sync_lower_el`, `exception_unexpected`) are thin public wrappers.
  Each renames its former body to a `static *_impl`, brackets it with
  `halls_enter_frame(ctx)` ... `halls_leave_frame(prev)`, and the asm
  (`vectors.S`) still `bl`s the public name.
- A normal return restores the previous slot. An `extinction()` (noreturn)
  *inside* the impl skips the restore, so when `halls_dump(NULL)` runs from the
  extinction tail it reads the dying frame.
- `vectors.S`'s `KERNEL_ENTRY` does `sub sp, #EXCEPTION_CTX_SIZE` then
  `mov x0, sp`, so the interrupted kernel SP is `ctx + EXCEPTION_CTX_SIZE` --
  this is how the dump anchors the stack hexdump for a kernel fault.

This was deliberately kept in C rather than `vectors.S`: the asm slots are
capped at 0x80 bytes (the IRQ slots are near the limit) and `KERNEL_EXIT` is
the single most load-bearing exit in the kernel; adding per-CPU stores there
risked the budget and the audit surface for no gain.

### The dump body (`halls_emit`)

Output order is deliberate -- the register block (pure `ctx`-field reads, no
stack walk) is emitted FIRST so the most-likely-to-survive data lands before
the backtrace / hexdump touch live (possibly corrupt) stack:

1. CPU index + source EL (`SPSR.M[3:2]`: 0=EL0, 1=EL1).
2. `ELR` (+ link), `ESR`/`FAR`/`SPSR`, `SP`, `LR` (+ link), `FP`.
3. `x0..x28` (the rest; `x29`/`x30` already shown as FP/LR).
4. KASLR offset + kernel base.
5. Backtrace (below).
6. 256-byte ascending stack hexdump from the kernel SP.

### Backtrace + PAC

`#0` is `ELR` (the faulting/interrupted PC). Subsequent frames walk the x29
chain, reading the saved LR at `[fp+8]`. Return addresses spilled to the kernel
stack are **PAC-signed** (the kernel ships `hardening=PAC`; `paciasp` on entry),
so `halls_strip_pac` runs `xpaci` on every code address before
`halls_link_addr` removes the KASLR slide -- otherwise the link address carries
the pointer-auth code in its high bits and is unresolvable. `xpaci` is in the
hint encoding space (a NOP without FEAT_PAuth) and only strips (never
authenticates), so it is safe and idempotent on any value.

`halls_link_addr(addr, off)` returns `addr - off` for a slid code address
(`addr >= off`) and leaves anything below the offset untouched (not a slid code
address). The printed link VA feeds `addr2line -e thylacine.elf <link>`.

## Data structures

`struct exception_context` (see `08-exception.md`) is the input -- `regs[31]`
(x0..x30), `sp`, `elr`, `spsr`, `esr`, `far`; 288 bytes, layout pinned by
`_Static_assert`s in `exception.c`. The Halls add no on-wire/on-disk struct at
Tier 1 (Tier 3 adds a versioned sink record).

Per-CPU state (file-static in `halls.c`):
- `g_halls_frame[DTB_MAX_CPUS]` -- live exception frame pointer, or NULL.
- `g_halls_in_dump[DTB_MAX_CPUS]` -- re-entrancy guard (HX-I1).

Both are per-CPU and touched only by the local CPU (handlers do not migrate
mid-execution at v1.0), so no atomics. `halls_cpu()` clamps `MPIDR.Aff0 >=
DTB_MAX_CPUS` to 0 so a wild index never indexes out of bounds.

## Invariants

| # | Invariant | Enforcement |
|---|---|---|
| HX-I1 | The dump never loops or recursively faults; a fault during the dump bails to `_torpor`. | Per-CPU `g_halls_in_dump` set BEFORE any faulting read; a recursive extinction sees it set and returns immediately. |
| HX-I2 | The backtrace walk is bounded + sanity-gated; a wild x29 cannot spin or read unboundedly. | `halls_fp_is_sane` (16-aligned, strictly increasing, in `[lo,hi)`) + a 32-frame depth cap; a single bad read still faults but trips HX-I1. |
| HX-I3 | The `EXTINCTION: ` ABI line (TOOLING.md 10) is emitted first and unchanged; the dump follows under `HALLS:`. | `extinction()` emits the ABI line, then `halls_dump`, then `_torpor`. `tools/test.sh` still matches `^EXTINCTION:` and `Thylacine boot OK`. |

## Tests

`kernel/test/test_halls.c` (7 unit tests, all in the default boot suite):
- `halls.fp_sane_{accepts_valid,rejects_misaligned,rejects_non_increasing,rejects_out_of_range}` -- the HX-I2 gate, including cycle-freedom (strictly-increasing) and the `[lo,hi)` exclusivity.
- `halls.link_addr_{removes_slide,underflow_guarded}` -- slide removal + the below-offset passthrough (no bogus wrap).
- `halls.frame_enter_leave_nesting` -- the wrapper save/restore contract under nesting; IRQ-masked across the slot manipulation so a timer tick cannot perturb the per-CPU slot mid-assert (snapshots-then-asserts so a failure never early-returns with IRQs masked).

Behavioral validation (HX-1 landing): a deliberate unmapped-high-VA store
injected in `boot_main` (then reverted) drove the full path -- the dump fired
on a real EL1 frame with correct EL detection, `FAR`/registers matching the
fault, KASLR offset correct, PAC-stripped backtrace, and the ELR link address
resolving to `boot_main` via `addr2line`.

## Error paths

`halls_dump` has no return value and never reports failure -- it is a
best-effort dump on a dying machine. A fault during the dump is absorbed by
HX-I1 (the partial output already printed survives; control reaches `_torpor`).

## Performance

Runs once per kernel death (off the hot path entirely). Cost is dominated by
polled-UART output (~hundreds of `uart_putc`); irrelevant since the kernel is
about to halt forever.

## Status

- Tier 1 (UART dump): LANDED (HX-1).
- Tier 2 (in-kernel kallsyms-style symbol table -> live `func+0xN`): designed,
  not built (HX-2). Until then, symbolization is offline:
  `addr2line -e build/kernel/thylacine.elf <HALLS link addr>`.
- Tier 3 (pstore-style persistence across reboot): designed only.

## Known caveats / footguns

- **Offline symbolization toolchain.** GNU `aarch64-linux-gnu-addr2line` may
  warn `Dwarf Error: Invalid or unhandled FORM value: 0x25` and omit line
  numbers (clang emits DWARF5 forms older binutils does not parse); it still
  resolves the function name. Use `llvm-addr2line` for line numbers. HX-2 makes
  this moot for live triage.
- **Concurrent extinctions interleave.** `extinction()` (and thus the dump) is
  unlocked; two CPUs dying at once interleave their `HALLS:` lines. This
  predates HX-1 (`extinction()` was always unlocked) and is cosmetic on a dying
  machine, not a soundness issue. Per-CPU slots/guards are independent.
- **`extinction_with_addr` prints `0x0x<addr>`.** A pre-existing double-prefix
  (`extinction_with_addr` writes `" 0x"` then `uart_puthex64` adds its own
  `0x`); the `HALLS:` lines do not have this (they rely on `uart_puthex64`'s
  prefix). Untouched by HX-1 to keep the ABI line byte-stable; cosmetic.
- **Bare-extinction register values.** For an `extinction()` with no exception
  frame (a deep assert), `x0..x28` are gone post-call; the dump labels them
  unavailable and prints only the captured `sp`/`fp`/`lr` + the (stale,
  labelled) EL1 syndrome regs. The backtrace from the current x29 still walks
  the assert's call chain.

## Naming rationale

"Halls of Extinction" -- the subsystem that records every kernel death, fitting
the project identity (extinction = ELE; the Halls keep the lineage's dead).
Surfaces: `halls_dump()` (the capture), the `HALLS:` UART prefix, and (Tier 3)
the `/var/halls` archive + `halls` tool. The `EXTINCTION:` prefix is unchanged
-- the Halls *follow* it.

## References

- Design scripture: `docs/HALLS-OF-EXTINCTION.md`.
- `08-exception.md` (struct exception_context + vectors), `25-fault-dispatcher.md`
  (the kernel-fault extinct paths), `04-extinction.md` (the ABI line + naming),
  `05-kaslr.md` (`kaslr_get_offset`).
- Audit-trigger surface: CLAUDE.md + `ARCHITECTURE.md` 25.4 (exception entry).
