# Reference: kernel-mode user-VA access primitives (R12-uaccess)

## Purpose

Kernel syscall handlers occasionally need to dereference user-VA pointers. Until R12-uaccess, the only such consumer was `SYS_PUTS`, which read user bytes directly with a kernel-mode `ldrb`. If the user page lived in a VMA but had not yet been PTE-installed in the per-Proc TTBR0 tree (the demand-paging path had not yet fired for that page), the kernel-mode load took a translation fault that the dispatcher classified as "unhandled kernel translation fault" and extincted.

The workaround in place pre-R12 was the per-binary `pretouch_rodata_pages()` Rust function — read each `.rodata` page from EL0 before the first SYS_PUTS so the EL0 fault path installed the PTE via the existing `exception_sync_lower_el` route. This had three problems: every binary had to know its own LOAD layout (and update it as the binary grew); pages outside `.rodata` (e.g. stack-resident format buffers) were not covered; touching a page past the binary's mapped LOAD itself faulted, so the discipline was bounded to exactly the pages the binary knew were mapped.

R12-uaccess replaces the discipline with a kernel-mode fault-recoverable accessor — `uaccess_load_u8(user_va, *out)`. The asm primitive's load instruction is registered in a fixup table; the sync fault dispatcher recognizes faults at registered instructions, demand-pages the user page on the caller's Proc, and either retries the load (success) or transfers control to a fault-recovery label that returns -1 (failure). The pattern is the Thylacine equivalent of Linux's `__ex_table` discipline and applies uniformly to any future kernel-mode user-VA access (SYS_WRITE, SYS_READ, ioctl, etc.).

## Public API

### `arch/arm64/uaccess.h`

```c
// Read a single byte from a user VA. Returns 0 on success (*out
// updated) or -1 on translation/permission fault. The caller MUST
// have validated `user_va < USER_VA_TOP`; the primitive does not
// range-check.
extern s64 uaccess_load_u8(u64 user_va, u8 *out);

// Look up the fixup PC for a faulting instruction PC. Returns 0 if
// `fault_pc` is not in the table (the fault is not a uaccess fault).
u64 uaccess_fixup_lookup(u64 fault_pc);

// Shared with arch/arm64/mmu.h's USER_VA_TOP. _Static_assert in
// uaccess.c cross-checks at compile time.
#define UACCESS_USER_VA_TOP  (1ull << 47)
```

### Caller contract

- `user_va` must be a valid user-half VA (`< USER_VA_TOP`). The asm primitive does not range-check. Callers must validate at the syscall surface.
- `out` must point at a kernel-writable byte. Typically a stack variable.
- A return of 0 implies `*out` was written; a return of -1 implies `*out` was NOT written (the load faulted before the store could run). Callers must not assume any state of `*out` on the fault path.
- A return of -1 is the EFAULT-equivalent on the syscall surface. The kernel does NOT extinct on a uaccess fault; the caller propagates the failure via the syscall return.

## Implementation

### `arch/arm64/uaccess.S`

The primitive is hand-written assembly:

```asm
.global uaccess_load_u8
.type uaccess_load_u8, @function
uaccess_load_u8:
.global uaccess_load_u8_op
uaccess_load_u8_op:
    ldrb    w2, [x0]              // FAULT POINT — table entry below
    strb    w2, [x1]
    mov     x0, xzr
    ret
.global uaccess_load_u8_fault
uaccess_load_u8_fault:
    mov     x0, #-1
    ret
.size uaccess_load_u8, . - uaccess_load_u8

.pushsection .uaccess_fixup, "a", @progbits
.balign 4
.long   uaccess_load_u8_op    - .
.long   uaccess_load_u8_fault - .
.popsection
```

The `.uaccess_fixup` section emits two s32 PC-relative offsets per entry: `(op_pc - here, fixup_pc - here2)`. Absolute PCs are reconstructed by `(u64)&slot + (s64)slot`. PC-relative encoding is the Linux `__ex_table` convention and avoids R_AARCH64_ABS64 relocations (rejected by the PIE-mode linker) and `.rela.dyn` runtime fixup entries.

The `uaccess_load_u8_op` and `uaccess_load_u8_fault` labels are exported as global symbols for the in-kernel test (`kernel/test/test_uaccess.c`) to cross-check fixup-table contents. They are not consumed at runtime by the dispatcher — the dispatcher walks the table by PC equality alone.

### `arch/arm64/uaccess.c`

```c
struct uaccess_fixup_entry {
    s32 op_rel;       // op_pc    = (u64)&op_rel    + (s64)op_rel
    s32 fixup_rel;    // fixup_pc = (u64)&fixup_rel + (s64)fixup_rel
};

_Static_assert(sizeof(struct uaccess_fixup_entry) == 8,
    "uaccess_fixup_entry must be exactly two s32 fields");

extern const struct uaccess_fixup_entry _uaccess_fixup_start[];
extern const struct uaccess_fixup_entry _uaccess_fixup_end[];

_Static_assert(UACCESS_USER_VA_TOP == USER_VA_TOP,
    "UACCESS_USER_VA_TOP must equal mmu.h USER_VA_TOP");

u64 uaccess_fixup_lookup(u64 fault_pc) {
    const struct uaccess_fixup_entry *e = _uaccess_fixup_start;
    while (e < _uaccess_fixup_end) {
        u64 op_pc = (u64)(uintptr_t)&e->op_rel + (u64)(s64)e->op_rel;
        if (op_pc == fault_pc) {
            return (u64)(uintptr_t)&e->fixup_rel + (u64)(s64)e->fixup_rel;
        }
        e++;
    }
    return 0;
}
```

Linear scan — at v1.0 the table holds one entry. The `(s32 → s64 → u64) + u64` arithmetic is well-defined under wraparound semantics (the s32 offset is sign-extended to s64; adding to the u64 base computes the absolute PC). Compile-time check pins `UACCESS_USER_VA_TOP` against `mmu.h`'s `USER_VA_TOP` so drift surfaces at build time.

### `arch/arm64/kernel.ld`

The `.uaccess_fixup` section is emitted inside `.rodata` (R-only protection — the table is read at exception-time but never mutated):

```
.rodata : ALIGN(PAGE_SIZE) {
    *(.rodata .rodata.*)
    . = ALIGN(4);
    _uaccess_fixup_start = .;
    KEEP(*(.uaccess_fixup))
    _uaccess_fixup_end = .;
}
```

`KEEP()` is defensive against `--gc-sections` (off today, but the section carries no C-side symbol references and would otherwise be eligible for stripping by future toolchain configurations).

### `arch/arm64/exception.c` — dispatcher integration

`exception_sync_curr_el` checks every kernel-mode sync fault against the uaccess fixup table BEFORE handing to `arch_fault_handle`:

```c
if (!fi.from_user && fi.vaddr < UACCESS_USER_VA_TOP &&
    (fi.is_translation || fi.is_permission || fi.is_access_flag)) {
    u64 fixup_pc = uaccess_fixup_lookup(fi.elr);
    if (fixup_pc != 0) {
        struct Thread *t = current_thread();
        if (t && t->magic == THREAD_MAGIC &&
            t->proc && t->proc->magic == PROC_MAGIC &&
            t->proc->pgtable_root != 0) {
            struct fault_info uf = fi;
            uf.from_user = true;
            if (userland_demand_page(t->proc, &uf) == FAULT_HANDLED) {
                return;   // ERET re-executes the faulting load.
            }
        }
        ctx->elr = fixup_pc;
        return;
    }
}
```

Three conditions gate the recovery:
1. `!fi.from_user` — kernel-mode fault (lower-EL faults are handled by `exception_sync_lower_el`).
2. `fi.vaddr < UACCESS_USER_VA_TOP` — FAR is in the user half; a fault on a kernel VA from kernel mode is a genuine bug and must extinct via the existing dispatcher.
3. `fi.elr` is in the fixup table — the faulting instruction is a registered uaccess primitive. False positives are impossible — `op_pc` is the address of a specific instruction.

On hit, the dispatcher first attempts demand-paging via `userland_demand_page` (synthesizing a from_user=true fault_info; `userland_demand_page` does not actually inspect `from_user`). The typical case (user page in VMA but not yet PTE-installed) succeeds — the dispatcher returns, vectors.S ERETs to `ctx->elr` (unchanged = the faulting load), and the load re-executes successfully.

If demand-paging fails (no VMA covers the page, permission denied, OOM during sub-table alloc) OR the caller is not in a proc-context, the dispatcher overwrites `ctx->elr` with the fixup PC. ERET then transfers control to `uaccess_load_u8_fault`, which sets `x0 = -1` and returns to the caller.

### `arch/arm64/vectors.S` — recoverable Sync slot

Pre-R12-uaccess, every kernel-mode sync exception either extincted in `exception_sync_curr_el` or fell through to `_torpor` (kernel WFI halt loop) — vector slots 0x000 and 0x200 ended with `b _torpor`. R12-uaccess makes Sync recoverable: both slots now end with `b .Lexception_return` (the shared KERNEL_EXIT trampoline) so a successful return from `exception_sync_curr_el` ERETs through the standard restore path. Slot size remains under the 0x80-byte budget (~28 instructions; budget is 32).

## Data structures

| Field | Type | Width | Notes |
|---|---|---|---|
| `struct uaccess_fixup_entry.op_rel` | s32 | 4 B | PC-relative offset from this slot to the faulting instruction. |
| `struct uaccess_fixup_entry.fixup_rel` | s32 | 4 B | PC-relative offset from this slot to the fault-recovery label. |

Total size: 8 bytes per entry. Pinned by `_Static_assert`.

## Tests

`kernel/test/test_uaccess.c` (~125 LOC) covers:

| Test | What it checks |
|---|---|
| `uaccess.fixup_table_well_formed` | `_uaccess_fixup_start < _uaccess_fixup_end`; byte span is a multiple of 8 (whole entries); every resolved `op_pc` and `fixup_pc` is non-zero. |
| `uaccess.fixup_lookup_known` | `uaccess_fixup_lookup(uaccess_load_u8_op) == uaccess_load_u8_fault` — the lookup correctly resolves the only entry. |
| `uaccess.fixup_lookup_unknown_returns_zero` | Lookup of an arbitrary kernel PC (this test function's own address) and lookup of NULL both return 0 — false-positive insurance. |
| `uaccess.load_u8_unmapped_user_va_returns_minus1` | Real fault-and-recover: from kproc context (TTBR0 = l0_ttbr0, no user-VA mappings), `uaccess_load_u8(0x10000000, &out)` issues an `ldrb` that translation-faults at L0, the dispatcher catches via the fixup table, demand-page fails (no VMA in kproc), the fixup label runs and returns -1. The output byte is verified unchanged from its poisoned value (0xAB), confirming the fault path never wrote `*out`. A second VA (0x40000000) verifies the dispatcher doesn't latch on a specific FAR. |

Integration coverage comes from the existing userspace tests — `virtio-blk-rw`, `virtio-net-arp`, `virtio-net-loop` all call SYS_PUTS many times against `.rodata` strings that span multiple pages; with `pretouch_rodata_pages()` retired by R12-uaccess, those tests exercise the demand-page-via-uaccess path on the first SYS_PUTS to each previously-unmapped page. If the dispatcher logic regresses, every multi-page userspace test fails at boot.

## Error paths

| Surface | Return | Caused by |
|---|---|---|
| `uaccess_load_u8` | -1 | Translation/permission fault at the `ldrb` AND demand-page failed (no VMA covers the page / permission denied / OOM). |
| `uaccess_fixup_lookup` | 0 | `fault_pc` is not in the fixup table. The dispatcher falls through to `arch_fault_handle`'s existing kernel-fault handling (extinction at v1.0). |

## Performance characteristics

- Fast path (page already PTE-installed): a single `ldrb` + `strb` + `mov` + `ret` — 4 instructions, no fault overhead.
- Slow path (page in VMA but not yet PTE-installed): fault delivery + `exception_sync_curr_el` decode + `uaccess_fixup_lookup` linear scan (1 entry at v1.0) + `userland_demand_page` (vma_lookup + mmu_install_user_pte) + ERET retry. Linear in the LOAD-segment page count for the binary's first runs; amortizes to fast path once every page is touched.
- Failure path (no VMA): fault delivery + lookup + demand-page-fails + ERET to fixup + return -1. Bounded constant overhead.

## Status

- **Landed** at R12-uaccess (substantive commit pending; hash fixup pending). 235 → 239 tests.
- **Audit-bearing per CLAUDE.md trigger surfaces (Exception entry + Page fault / mprotect / mmap + Capability checks).** Self-audit clean across the 10 adversarial categories enumerated in the landed-chunk row (false-positive on non-uaccess kernel-mode faults, recursion if uaccess primitive faults again, demand-page side effects on synthetic from_user=true, ctx->elr overwrite race, user-VA boundary check correctness, fixup table PIE encoding correctness, SP_EL1 vs SP_EL0 SPSel discipline through `.Lexception_return`, struct uaccess_fixup_entry layout drift, linker section placement R-only enforcement, kproc-context test correctness). Formal R12-uaccess prosecutor pass deferred-or-on-finding.

## Known caveats / footguns

- **Single-byte primitive at v1.0**: only `uaccess_load_u8` exists. Larger accesses (u16/u32/u64, copy_from_user, copy_to_user, strnlen_from_user, etc.) are added on demand as future syscalls land. The fixup-table pattern generalizes uniformly — each new primitive adds one `.uaccess_fixup` entry per fault-prone instruction.
- **Range check is the caller's job**: the asm primitive does not validate `user_va < USER_VA_TOP`. SYS_PUTS validates at the syscall surface (R7 F127 + R12-vaddr); future callers must do the same. A bypass that passes a kernel VA would translate via TTBR1, succeed on most kernel-mapped addresses, and leak kernel data via the syscall output.
- **TOCTOU between range check + uaccess**: under v1.0 single-thread Procs with no `munmap`, the VMA backing the user page cannot disappear between the syscall's bound check and the asm primitive's `ldrb`. Phase 5+ multi-threaded Procs + `munmap` will surface a real race; the fixup-table machinery is the right shape to extend (a TOCTOU-induced fault would correctly return -1 to the caller without extincting).
- **Per-page slow path is per-binary at startup**: a binary that spans N `.rodata` pages takes N-1 faults on the first SYS_PUTS sweep. v1.0 binaries are small (LOAD ≤ ~6 KiB = 2 pages = 1 fault). Phase 5+ may add an explicit `madvise(WILLNEED)`-style hint if any binary pushes this.
- **The fixup-table walker is O(table size)**: at v1.0 the table is 1 entry; the linear scan is trivial. If future syscalls grow the table past ~16 entries, consider sorting + binary search (Linux's `__ex_table` is sorted at link time and binary-searched).
