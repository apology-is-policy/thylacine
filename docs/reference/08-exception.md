# 08 — Exception handling (as-built reference)

The kernel's ARM64 exception vector table and synchronous-fault dispatch. Every CPU exception (sync abort, IRQ, FIQ, SError) at any source EL routes through one of 16 vector entries; entries either dispatch to a real C handler or `extinction` with a vector-index diagnostic.

P1-F deliverable. The deferred fault paths from P1-C-extras Part A (boot-stack guard) and P1-C (W^X violation) finally close: a stack overflow now triggers `extinction("kernel stack overflow", FAR_EL1)`; a write to `.text` or exec from `.data/.bss` triggers `extinction("PTE violates W^X (kernel image)", FAR_EL1)`.

Scope: `arch/arm64/vectors.S` (16-entry table, save/restore macros), `arch/arm64/exception.{h,c}` (struct exception_context, ESR/FAR decode, dispatch handlers), `kernel/main.c`'s call to `exception_init()` after `slub_init()`, banner update. Also see `docs/reference/01-boot.md` (entry sequence) and `docs/reference/04-extinction.md` (the ELE primitive that handlers terminate with).

Reference: `ARCHITECTURE.md §12` (interrupt handling design intent), `§28` (invariant I-12 enforcement).

---

## Purpose

Until P1-F, any CPU exception (translation fault, SP misalignment, deliberate `brk`, etc.) wedged QEMU silently — there was no vector table installed, so the CPU's behavior on exception entry was UB. The boot-stack guard page (P1-C-extras Part A) and W^X PTE constructors (P1-C) declared their failure modes but had no path to surface them.

P1-F installs the canonical 16-entry ARMv8 vector table at a 2 KiB-aligned address inside the kernel image, points VBAR_EL1 at it, and provides:

- A live synchronous-fault handler that decodes ESR_EL1 + FAR_EL1, recognises kernel stack overflow + W^X violations, and routes to `extinction` with the right diagnostic.
- A catch-all `exception_unexpected` for the 15 vector entries that aren't expected to fire at this phase (lower-EL entries — no userspace yet; current-EL-SP0 entries — kernel always uses SP_EL1; IRQ / FIQ / SError — GIC + IRQ-driven UART land at P1-G or a P1-F-extras chunk).

After P1-F, the boot path is "fragile-by-default" no longer — every fault produces a clean `EXTINCTION:` diagnostic the agentic loop can recognise.

---

## Public API

`arch/arm64/exception.h`:

```c
#define EXCEPTION_CTX_SIZE   288     // struct exception_context size (bytes)

#ifndef __ASSEMBLER__
struct exception_context {
    u64 regs[31];   // x0..x30
    u64 sp;         // SP_EL0 at fault
    u64 elr;        // ELR_EL1 (faulting / interrupted PC)
    u64 spsr;       // SPSR_EL1
    u64 esr;        // ESR_EL1
    u64 far;        // FAR_EL1
};

void exception_init(void);                                   // install VBAR_EL1
void exception_sync_curr_el(struct exception_context *);     // sync handler
__attribute__((noreturn))
void exception_unexpected(struct exception_context *, u64 vector_idx);
#endif
```

`exception.h` is `#include`d from both C and `vectors.S`; the `__ASSEMBLER__` guard hides the C-only declarations during preprocessing of the assembler, leaving only `EXCEPTION_CTX_SIZE` visible.

The struct's field offsets are load-bearing (vectors.S writes by literal byte offset). `_Static_assert`s in `exception.c` verify size + each field's offset matches the macro / hardcoded layout. Reordering the struct fails the build.

---

## Implementation

### Vector table (`arch/arm64/vectors.S`)

ARMv8 mandates a 0x800-aligned 16-entry table. Each entry is 0x80 bytes (32 instructions). The 16 entries are:

| Offset | EL source | Type | P1-F handler |
|---|---|---|---|
| `0x000` | Current EL with SP_EL0 | Sync | unexpected (idx 0) |
| `0x080` | Current EL with SP_EL0 | IRQ | unexpected (idx 1) |
| `0x100` | Current EL with SP_EL0 | FIQ | unexpected (idx 2) |
| `0x180` | Current EL with SP_EL0 | SError | unexpected (idx 3) |
| **`0x200`** | **Current EL with SP_ELx** | **Sync** | **`exception_sync_curr_el` ← LIVE** |
| `0x280` | Current EL with SP_ELx | IRQ | unexpected (idx 5) |
| `0x300` | Current EL with SP_ELx | FIQ | unexpected (idx 6) |
| `0x380` | Current EL with SP_ELx | SError | unexpected (idx 7) |
| `0x400` | Lower EL using AArch64 | Sync | unexpected (idx 8) |
| `0x480` | Lower EL using AArch64 | IRQ | unexpected (idx 9) |
| `0x500` | Lower EL using AArch64 | FIQ | unexpected (idx 10) |
| `0x580` | Lower EL using AArch64 | SError | unexpected (idx 11) |
| `0x600` | Lower EL using AArch32 | Sync | unexpected (idx 12) |
| `0x680` | Lower EL using AArch32 | IRQ | unexpected (idx 13) |
| `0x700` | Lower EL using AArch32 | FIQ | unexpected (idx 14) |
| `0x780` | Lower EL using AArch32 | SError | unexpected (idx 15) |

Most entries are stamped out via the `VEC_UNEXPECTED <idx>` macro: `KERNEL_ENTRY` (save 31 GP regs + 5 special regs onto SP_EL1), pass `(ctx_pointer, idx)` to `exception_unexpected`, fall through to `_hang` for safety. The Current-EL-SPx Sync entry calls `exception_sync_curr_el(ctx)` for real diagnostics.

Each entry must fit within 0x80 bytes (32 instructions). KERNEL_ENTRY is ~24 instructions; the dispatch + safety branch is 4 more, total 28 — comfortably under the slot budget. KERNEL_EXIT (the eret-side mirror) is defined in `vectors.S` for Phase 2's recoverable handlers but is NOT used inline at P1-F (would push slots over 0x80 — every P1-F exception terminates in extinction, so the restore path is dead code).

### `KERNEL_ENTRY` save layout

```
sp+0x000:  x0,  x1
sp+0x010:  x2,  x3
sp+0x020:  x4,  x5
sp+0x030:  x6,  x7
sp+0x040:  x8,  x9
sp+0x050:  x10, x11
sp+0x060:  x12, x13
sp+0x070:  x14, x15
sp+0x080:  x16, x17
sp+0x090:  x18, x19
sp+0x0A0:  x20, x21
sp+0x0B0:  x22, x23
sp+0x0C0:  x24, x25
sp+0x0D0:  x26, x27
sp+0x0E0:  x28, x29
sp+0x0F0:  x30 (lr), sp_el0
sp+0x100:  elr_el1, spsr_el1
sp+0x110:  esr_el1, far_el1
```

Total 0x120 = 288 bytes = `EXCEPTION_CTX_SIZE`. ESR + FAR are read once on entry and saved in the context so the C handler doesn't need to re-read MSRs (and won't see a value that changed if a follow-on fault overwrites them).

### Sync handler (`exception_sync_curr_el`)

Decodes `ESR_EL1` exception class (`EC` field, bits 31:26) and routes:

```c
switch (ec) {
case EC_DATA_ABORT_SAME:                    // 0x25
case EC_INST_ABORT_SAME:                    // 0x21
    if (addr_is_stack_guard(far))
        extinction_with_addr("kernel stack overflow", far);
    if (fsc_is_permission(fsc) && addr_is_kernel_image(far))
        extinction_with_addr("PTE violates W^X (kernel image)", far);
    if (fsc_is_translation(fsc))
        extinction_with_addr("unhandled translation fault", far);
    if (fsc_is_permission(fsc))
        extinction_with_addr("unhandled permission fault", far);
    extinction_with_addr("data/instruction abort", far);

case EC_SP_ALIGN:           // 0x26
    extinction_with_addr("SP alignment fault", far);
case EC_PC_ALIGN:           // 0x22
    extinction_with_addr("PC alignment fault", elr);
case EC_BRK:                // 0x3C
    extinction_with_addr("brk instruction (assertion?)", elr);
default:
    extinction_with_addr("unhandled sync exception (EC in ESR_EL1)", esr);
}
```

The data/instruction abort branch implements the ARCH-mandated diagnostics: `addr_is_stack_guard(far)` returns true iff FAR ∈ `[_boot_stack_guard, _boot_stack_bottom)` (in either PA or high-VA form — the helper checks both bounds). `addr_is_kernel_image(far)` returns true iff FAR ∈ `[_kernel_start, _kernel_end)`.

Note that all branches end in `extinction_*` calls, which are `noreturn` — the compiler elides any fall-through. Recoverable sync exceptions (page faults backed by VMOs, demand-paging) land in Phase 2 with the per-process VM machinery; until then, every sync fault is fatal.

### `exception_unexpected`

Catch-all for the 15 non-live vector entries. `vector_idx` (0..15) maps to a static name table so the diagnostic identifies which slot fired:

```c
static const char *names[16] = {
    "[Curr EL/SP0] Sync",  "[Curr EL/SP0] IRQ",  ...
    "[Curr EL/SPx] Sync",  "[Curr EL/SPx] IRQ",  ...   // SPx Sync is LIVE; never reaches here
    "[Lower EL a64] Sync", "[Lower EL a64] IRQ", ...
    "[Lower EL a32] Sync", "[Lower EL a32] IRQ", ...
};
extinction(names[vector_idx]);
```

For a developer reading the QEMU output post-extinction, the prefix immediately identifies the vector class — invaluable when debugging an unexpected fault while building out future phases.

### `exception_init`

```c
void exception_init(void) {
    u64 vbar = (u64)(uintptr_t)_exception_vectors;
    __asm__ __volatile__("msr vbar_el1, %0\n isb\n"
                         :: "r" (vbar) : "memory");
}
```

PC-relative resolution of `_exception_vectors` runs at high VA (boot_main is post-long-branch); VBAR_EL1 is set to that high VA. After return, faults fetch their handler at the high VA via TTBR1 — same way the rest of the kernel runs. The `isb` after the MSR is mandatory per ARM ARM (subsequent instructions might depend on the new VBAR).

### PA/VA helpers

`addr_is_stack_guard` and `addr_is_kernel_image` need PA bounds because:

- **Stack accesses** at P1-F use SP_EL1 = PA (the boot stack lives at PA below 4 GiB; TTBR0 identity-maps it). FAR for stack-related faults is therefore a low VA == PA.
- **Other accesses** (kernel data at high VA, MMIO at low PA) can produce FAR in either range.

Both helpers check both PA and high-VA bounds for robustness:

```c
bool addr_is_stack_guard(u64 addr) {
    u64 guard_pa = sym_to_pa(_boot_stack_guard);
    u64 bottom_pa = sym_to_pa(_boot_stack_bottom);
    if (addr >= guard_pa && addr < bottom_pa) return true;
    u64 guard_va = (uintptr_t)_boot_stack_guard;
    u64 bottom_va = (uintptr_t)_boot_stack_bottom;
    return addr >= guard_va && addr < bottom_va;
}
```

`sym_to_pa(sym) = (uintptr_t)sym - kaslr_kernel_high_base() + kaslr_kernel_pa_start()` — converts a high-VA linker symbol to its load PA via the kaslr.c accessors. Cheap (subtract + add) and always-correct.

---

## Data structures

### `struct exception_context`

288 bytes. Fields documented in the [Public API](#public-api). Layout asserted at compile time via `_Static_assert` on each field's `__builtin_offsetof`.

### Static name table for `exception_unexpected`

In `.rodata`. 16 string pointers covering the full vector layout. Keeping them in `.rodata` (not on the boot stack) means a stack-overflow-during-exception still produces correct diagnostic text.

---

## Spec cross-reference

No formal spec at P1-F. Future `exception.tla` candidates:

- The "every sync fault terminates in extinction at P1-F" property — could be proved structurally over the switch.
- Stack-overflow detection completeness — the FAR check covers exactly `[_boot_stack_guard, _boot_stack_bottom)`; any stack write outside that range corrupts non-guarded BSS without triggering a fault. (Mitigation: per-CPU exception stack at Phase 2.)
- W^X violation completeness — a permission fault on the kernel image is necessarily a W^X violation given our PTE constructors. The proof would inspect the L3 table state.

These are post-v1.0 unless a real bug surfaces. P1-I's audit pass will exercise the handler under sanitizers.

---

## Tests

P1-F integration test: `tools/test.sh` boots and verifies the boot banner. Banner now shows `hardening: MMU+W^X+extinction+KASLR+vectors (P1-F; PAC/MTE/CFI at P1-H)` confirming `exception_init` ran.

The boot path doesn't deliberately trigger a fault — that would fail `tools/test.sh` (which reports `EXTINCTION:` as a kernel failure). The exception path is exercised by code review (disassembly of `_exception_vectors` shows correct entries; `exception_init` correctly sets VBAR_EL1; the sync handler decodes ESR/FAR per the table above).

Future tests (P1-I+):

- A `test_fault.sh` target that builds with a `THYLACINE_DELIBERATE_FAULT=stack_overflow` flag, boots, expects `EXTINCTION: kernel stack overflow ...` in the output, and PASSES on that result.
- ASan / UBSan-instrumented builds wrap the sync handler entry — catches handler bugs that would otherwise produce a recursive fault.
- Phase 2 thread machinery introduces per-thread exception stacks; the stack-overflow path becomes recoverable (the handler can swap stacks before printing).

---

## Error paths

| Condition | Behavior |
|---|---|
| Sync fault: stack overflow (FAR in guard region) | `extinction("kernel stack overflow", far)`. |
| Sync fault: kernel-image permission fault | `extinction("PTE violates W^X (kernel image)", far)`. |
| Sync fault: kernel-image translation fault | `extinction("unhandled translation fault", far)`. |
| Sync fault: kernel-image alignment fault (SP/PC) | `extinction("SP alignment fault" / "PC alignment fault", far_or_elr)`. |
| Sync fault: deliberate `brk #imm` | `extinction("brk instruction (assertion?)", elr)`. |
| Sync fault: anything else | `extinction("unhandled sync exception (EC in ESR_EL1)", esr)`. |
| IRQ / FIQ / SError at current EL | `extinction("[Curr EL/SPx] {IRQ,FIQ,SError}")` — unexpected at P1-F. |
| Lower-EL entry of any kind | `extinction("[Lower EL a64/a32] ...")` — no userspace yet. |
| Current-EL-SP0 entry of any kind | `extinction("[Curr EL/SP0] ...")` — kernel always uses SP_EL1. |
| Recursive fault during KERNEL_ENTRY (stack already in guard region) | QEMU wedge. Documented limitation; per-CPU exception stack at Phase 2. |

---

## Performance characteristics

P1-F is reactive — no per-tick overhead. Cost is paid only on exceptions:

| Metric | Estimated | Notes |
|---|---|---|
| KERNEL_ENTRY save cost | ~30 cycles | 16 stp + 5 mrs + 1 sub. Cache-warm. |
| Sync dispatch (extinction path) | ~50-100 cycles | ESR decode + extinction print starts. |
| `exception_init` total cost | ~10ns | One MSR + one ISB. |
| Kernel ELF size (debug) | ~165 KB | +10 KB from P1-E (vectors.S + exception.c). |
| Kernel flat binary | ~16 KB | +0.5 KB. |
| Vector table | 2 KiB | In `.text` between the page-aligned section start and the rest of the code. |

---

## Status

**Implemented at P1-F**:

- 16-entry exception vector table at `_exception_vectors` (page-aligned; 0x800 bytes total).
- `exception_init()` sets VBAR_EL1 to the table's high VA.
- `KERNEL_ENTRY` save macro (32 GP regs + 5 special regs = 288 bytes onto SP_EL1).
- `KERNEL_EXIT` restore macro (defined; unused inline at P1-F because every fault terminates in extinction).
- `exception_sync_curr_el` — decodes ESR + FAR; recognises stack-overflow (boot-stack guard region), W^X violations (kernel image permission fault), translation faults, alignment faults, brk instructions; everything else generic extinction.
- `exception_unexpected` — catch-all for non-live entries with descriptive name table in `.rodata`.
- Compile-time layout assertions for `struct exception_context` field offsets.
- Banner update: `hardening: MMU+W^X+extinction+KASLR+vectors (P1-F; ...)`.

**Not yet implemented**:

- GIC + real IRQ dispatch (the IRQ vector entries call `exception_unexpected`). P1-G or a P1-F-extras chunk.
- IRQ-driven PL011 TX (still polled). Same chunk as GIC.
- Per-CPU exception stack — handler runs on the existing SP_EL1 (boot stack at v1.0). A stack-overflow recursion wedges the kernel without producing diagnostic output. Phase 2 introduces per-thread + per-CPU exception stacks.
- Recoverable sync faults — Phase 2 page-fault handler with VMO backing. KERNEL_EXIT is defined and ready.
- Userspace exception entry — Phase 2 (lower-EL Sync becomes the syscall + page-fault path).
- Deliberate-fault test target — P1-I.

**Landed**: P1-F at commit `(pending hash-fixup)`.

---

## Caveats

### Recursive fault on stack overflow

P1-F's KERNEL_ENTRY pushes 288 bytes onto SP_EL1. If the fault that fired was itself a stack overflow (SP already in or below the guard page), the push faults again, recursively, until QEMU wedges. The diagnostic chain "stack overflow → recursive fault during handler entry → wedge" produces no output to UART before the wedge.

Mitigation paths:
- **Phase 2**: per-CPU exception stack switched on entry via TPIDR_EL1 scratch, so the handler runs on a known-good stack regardless of the faulting SP.
- **Workaround for P1-F**: gradual stack growth that just barely overflows can still trigger a clean diagnostic if the overflowing access is small (<= 288 bytes from `_boot_stack_bottom`), because KERNEL_ENTRY's 288-byte push lands in regular BSS rather than back into the guard.

We accept the limitation at v1.0 because (a) `boot_main` does no significant stack work; (b) Phase 2's per-thread stacks introduce the dedicated exception stack as part of the same chunk that cares about stack overflow at all (per-thread overflows being more likely than boot-stack overflows).

### `exception.h` includes from `.S` files

`vectors.S` `#include`s `exception.h` to get `EXCEPTION_CTX_SIZE`. The C struct, function declarations, and `<thylacine/types.h>` include must be guarded with `#ifndef __ASSEMBLER__` or the assembler chokes on the C syntax. The pattern is standard (Linux uses it everywhere) but easy to miss if a future header carries C declarations into `.S`-included territory.

### KERNEL_EXIT is dead code at P1-F

The macro is defined in `vectors.S` and ready for Phase 2's recoverable handlers. Inlining it into the Current-EL-SPx Sync slot would push the slot from 28 instructions (108 bytes, fits in 128-byte slot) to 47 instructions (188 bytes, overflows). The fix when Phase 2 needs it: move the Sync-slot body to a trampoline outside the vector area — `vectors.S` slot just `b sync_trampoline`; the trampoline runs KERNEL_ENTRY + dispatch + KERNEL_EXIT.

### `exception_unexpected` strings live in `.rodata`

They're accessed via PC-relative `adrp+add` from the handler — works through TTBR1 high-VA. No special handling required for the names lookup. If a future audit tightens the diagnostic to include ESR/FAR alongside the vector name, the same approach extends.

### Permission-fault detection conflates "W^X" with "any kernel image permission fault"

`addr_is_kernel_image(far) && fsc_is_permission(fsc)` triggers `extinction("PTE violates W^X (kernel image)")`. In practice the only permission faults possible on kernel image pages today ARE W^X violations (PTE constructors guarantee no other permission combinations exist for kernel pages). If Phase 2 introduces additional permission distinctions (e.g., PXN-only vs UXN-only), the message becomes mildly inaccurate; refine then.

### No frame pointer chain dump

`extinction_with_addr` prints message + one address. We don't yet walk the frame-pointer chain (x29 is saved in the context but not reported). Phase 2's debug infrastructure adds a `dump_stack` helper that walks fp/lr from the saved context.

---

## See also

- `docs/reference/00-overview.md` — system-wide layer cake.
- `docs/reference/01-boot.md` — exception_init slot in the boot sequence.
- `docs/reference/03-mmu.md` — TTBR0/TTBR1 + W^X PTE constructors that the handler validates against.
- `docs/reference/04-extinction.md` — the ELE primitive that handlers terminate with.
- `docs/reference/05-kaslr.md` — `kaslr_kernel_pa_start` / `kaslr_kernel_high_base` accessors used for PA conversion.
- `docs/ARCHITECTURE.md §12` — interrupt handling design intent.
- ARM Architecture Reference Manual ARMv8 — section D1.10 (exception entry), D17.2.40 (ESR_EL1 layout).
- Linux `arch/arm64/kernel/entry.S` — reference implementation of the kernel entry/exit pattern.
