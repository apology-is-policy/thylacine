# 101 -- Halls of Extinction (Tier-1 crash dump + Tier-2 symbolization)

**Status**: Tier 1 (HX-1) + Tier 2 (HX-2, in-kernel symbols) LANDED. Tier 3
(persistence) is designed in `docs/HALLS-OF-EXTINCTION.md`, not yet built. The
Tier-2 section below documents the as-built symbol table + live symbolization.

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

## Tier 2 -- in-kernel symbolization (HX-2)

Every code-address line the Tier-1 dump prints (`ELR`, `LR`, each backtrace
frame) now carries `func+0xN` live, resolved in-kernel against an embedded
symbol table -- no offline `addr2line` step. A real `wxe_violation` dump:

```
HALLS: ELR  0xffffa00193480bfc  link 0xffffa00000080bfc  provoke_wxe_violation+0x8
HALLS: LR   0xffffa00193480be0  link 0xffffa00000080be0  fault_test_run+0x1c
HALLS: backtrace (fp-chain; link addrs for addr2line):
HALLS:   #0  0xffffa00193480bfc  link 0xffffa00000080bfc  provoke_wxe_violation+0x8
HALLS:   #1  0xffffa00193480938  link 0xffffa00000080938  boot_main+0x7e0
HALLS:   #2  0x0000000040080154  link 0x0000000040080154
```

The raw + KASLR link addrs are unchanged (HX-1); the trailing `name+0xN` is
HX-2. `#2` is the early-boot LMA (`0x40080154`, below the KASLR offset) -- no
high-VA symbol, so it degrades gracefully to raw, the same as any address
outside `.text`.

### The table

`struct halls_sym { u32 off; u32 name_off; }` (`arch/arm64/halls_symtab.h`),
sorted ascending by `off`, alongside `halls_symtab_count`,
`halls_symtab_link_base`, and a NUL-separated `halls_symtab_names[]` blob.

- **Offsets, not absolute VAs.** `off` is `sym_link_va - halls_symtab_link_base`
  (the base is the minimum text address the generator saw, == `_start` ==
  `KERNEL_LINK_VA`). A u32 offset is a plain constant, so the table emits **zero
  relocations** -- crucially, an absolute VA in initialized data would draw an
  `R_AARCH64_RELATIVE` reloc per symbol that the boot stub *slides* by the KASLR
  offset, both bloating `.rela.dyn` and turning the stored value into a runtime
  address (defeating "link-relative"). Verified: 0 relocs land inside the table
  region. The table lives in `.rodata` (R-only; always mapped; never slid).
- **Cost**: ~1.7k symbols -> ~14 KiB index + ~41 KiB names ~= 55 KiB of `.rodata`
  (well under the ~1.7 MiB headroom against the 4 MiB L3-mapping cap in
  `kernel.ld`).

### Lookup

`halls_symbolize(link_va, &off)` (the global wrapper) -> `halls_symbolize_table`
(`arch/arm64/halls.c`): a binary search for the greatest entry with
`off <= (link_va - base)`. Returns NULL (raw fallback) when `count == 0` (an
unsymbolized / stub build), `link_va < base`, the offset exceeds the u32 window,
or `link_va` is below the first symbol. **Dump-path-safe**: it reads only the
`.rodata` table (no faulting stack reads, unlike the fp-walk), is bounded by
`log2(count)`, takes no locks, and allocates nothing -- so it composes with
HX-I1 (a fault during the dump still trips the re-entrancy guard) and HX-I2 (the
fp-walk's bounds are unchanged). `q - tab[lo].off` cannot underflow (the search
invariant guarantees `tab[lo].off <= q`).

**Known caveat (HX-2 audit F4, accepted):** the table carries no per-symbol
size, so a code address in inter-function padding -- or a corrupt backtrace
return address that happens to land in `[base, _text_end)` -- resolves to the
nearest preceding function with a large offset rather than "no symbol". This is
memory-safe (still a bounded `.rodata` read) and best-effort by design; the raw
+ KASLR link address is always printed on the same line, so the operator is
never misled -- they have the exact address alongside the symbol guess. A
per-symbol upper-bound check is a possible v1.x refinement, not warranted now.

### The build (kallsyms-lite, two-pass)

The table is **generated per-build-dir** from the linked ELF, never committed to
the source tree, because the default and sanitizer builds have different `.text`
layouts (default 1727 symbols, UBSan 1760) and would otherwise clobber each
other.

1. `kernel/CMakeLists.txt` seeds `<build>/generated/halls_symtab.c` from the
   committed stub (`arch/arm64/halls_symtab.stub.c`, `count == 0`) at first
   configure, and compiles it into the kernel. A bare `cmake` / IDE build thus
   always links + runs (the stub -> NULL -> HX-1 raw fallback).
2. `tools/regen-halls-symtab.sh <build-dir>` (shared by `tools/build.sh` and,
   via `build.sh kernel --build-dir`, `tools/test-fault.sh`) runs
   `llvm-nm --defined-only` over the linked ELF through
   `tools/gen-halls-symtab.py`, overwrites the build-dir copy, and re-links.
   Because the table sits in `.rodata` (after `.text`), the re-link does not
   move the symbolized `.text` addresses, so it **converges after one re-link**;
   a second pass re-derives a byte-identical table (the stability assert), and a
   no-op incremental build converges in pass 1 with no extra link.
3. The generator is **deterministic** (sorted, deduped by address, no
   timestamps) so the stability check is a byte-compare and builds are
   reproducible. It is **best-effort**: a missing `llvm-nm`/`python3` or a
   generation failure keeps the existing table and the build stays green --
   symbolization is ergonomics, never a build gate.

### Tests

- `halls.symbolize_table` (`kernel/test/test_halls.c`): the binary search +
  boundary logic over a synthetic table -- exact hit, mid-function offset,
  past-the-last-symbol, below-first-entry -> NULL, below-base -> NULL,
  `count == 0` -> NULL, u32-overflow -> NULL.
- E2E: `tools/test-fault.sh wxe_violation` produces the symbolized dump above
  (the fault build is symbolized automatically -- it runs through
  `build.sh kernel --build-dir`).

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
