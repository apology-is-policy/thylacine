# Reference: page-fault dispatcher (P3-C / P3-Dc)

## Purpose

The fault dispatcher decodes ARMv8 ESR_EL1 / FAR_EL1 / ELR_EL1 into a structured `fault_info` and routes the fault to a handler that either resolves it (returns `FAULT_HANDLED` — the ERET resumes the interrupted instruction) or extincts with a specific diagnostic.

History:
- **P3-C** (`12ff454`): structured decode + classification. Resolve path empty; every call extincts.
- **P3-Dc**: user-mode demand-paging path live. `userland_demand_page` looks up the VMA covering FAR, validates the access against VMA prot, resolves the VMO offset to a backing PA, and installs a leaf PTE in the per-Proc TTBR0 tree (`mmu_install_user_pte`). User-mode resolves return `FAULT_HANDLED`. Failures fall through to `FAULT_UNHANDLED_USER`.

The dispatcher is the substrate for:
- ARCH §28 I-12 (W^X) runtime detection — kernel-image permission faults are recognized and produce "PTE violates W^X (kernel image)" extinctions.
- Per-thread kstack overflow detection (P3-Bca: kstack guard pages mapped no-access in the kernel direct map).
- Per-Proc VMA dispatch + demand-paging at P3-Dc.

ARCH §12: "Synchronous exceptions enter `exception_sync_*`. Page faults dispatch through `arch_fault_handle` after structured decoding via `fault_info_decode`."

## Public API

### `<arch/arm64/fault.h>`

```c
struct fault_info {
    u64 vaddr;            // FAR_EL1
    u64 elr;              // ELR_EL1 (faulting PC)
    u64 esr;              // raw ESR_EL1
    u32 ec;               // ESR.EC[31:26]
    u32 fsc;              // ESR.ISS[5:0]
    u8  fault_level;      // FSC[1:0]
    bool from_user;       // EL0 origin
    bool is_instruction;  // instruction abort vs data abort
    bool is_write;        // ESR.WnR (data aborts only)
    bool is_translation;  // FSC ∈ FSC_TRANS_FAULT_L{0..3}
    bool is_permission;   // FSC ∈ FSC_PERM_FAULT_L{1..3}
    bool is_access_flag;  // FSC ∈ FSC_ACCESS_FAULT_L{1..3}
};

enum fault_result {
    FAULT_HANDLED         = 0,
    FAULT_FATAL           = 1,
    FAULT_UNHANDLED_USER  = 2,
};

void fault_info_decode(u64 esr, u64 far, u64 elr, struct fault_info *out);
enum fault_result arch_fault_handle(const struct fault_info *fi);

// P3-Dc: user-mode demand paging.
struct Proc;
enum fault_result userland_demand_page(struct Proc *p,
                                       const struct fault_info *fi);
```

#### `fault_info_decode(esr, far, elr, *out)`

Pure decoder — no kernel state reads. Extracts:
- EC (exception class) from ESR[31:26].
- FSC (fault status code) from ESR[5:0].
- WnR (write/read) from ESR[9] (data aborts only; instruction aborts always read=0).
- `from_user` from EC (0x20/0x24 = lower EL = EL0).
- `is_instruction` from EC (0x20/0x21 = instruction abort).
- Classification booleans from FSC value lookup.
- `fault_level` from FSC[1:0].

Used by `exception_sync_curr_el` (and future `exception_sync_lower_el` at P3-E) when the EC indicates a data or instruction abort.

#### `arch_fault_handle(fi)`

Top-level dispatcher. At v1.0 P3-C the order of checks is:

1. **Kernel-mode + stack-guard region** → `extinction("kernel stack overflow")`.
2. **Kernel-mode + permission fault + kernel image** → `extinction("PTE violates W^X (kernel image)")`.
3. **Kernel-mode + translation fault** → `extinction("unhandled kernel translation fault")`.
4. **Kernel-mode + permission fault (other)** → `extinction("unhandled kernel permission fault")`.
5. **Kernel-mode + access-flag fault** → `extinction("unhandled kernel access-flag fault")`.
6. **User-mode** → routes through `userland_demand_page(current_thread()->proc, fi)`. Returns `FAULT_HANDLED` on success / `FAULT_UNHANDLED_USER` on failure (caller extincts at v1.0; Phase 5+ note delivery upgrades to SIGSEGV).
7. **Anything else** → `extinction("unclassified kernel fault (ESR)")`.

Returns `FAULT_HANDLED` for resolved user-mode faults (P3-Dc); for kernel-mode faults the resolve path remains empty (every kernel-mode fault extincts).

#### `userland_demand_page(p, fi)` — P3-Dc

The per-Proc VMA-tree dispatcher. Steps:

1. Validate Proc magic + non-zero `pgtable_root` (kproc has 0 — never demand-pages).
2. `vma_lookup(p, fi->vaddr)` — sorted-list walk to find the VMA covering the faulting VA. Returns NULL → `FAULT_UNHANDLED_USER`.
3. Permission check vs fault type:
   - `is_write` requires `VMA_PROT_WRITE`.
   - `is_instruction` requires `VMA_PROT_EXEC`.
   - Read fault requires `VMA_PROT_READ`.
4. Resolve the VMO offset: `vmo_byte_off = vma->vmo_offset + (page_va - vma->vaddr_start)`. Reject if offset ≥ `vmo->size`.
5. Resolve to a backing PA: `vmo_base_pa + (vmo_byte_off & ~PAGE_MASK)`. v1.0 anonymous VMO: pages are eagerly allocated in a single `alloc_pages(order)` chunk; page i is at `page_to_pa(vmo->pages) + i * PAGE_SIZE`.
6. `mmu_install_user_pte(p->pgtable_root, p->asid, page_va, page_pa, vma->prot)` — walks the L0 → L1 → L2 → L3 tree, allocates sub-tables KP_ZERO as needed, installs the leaf PTE.

Exposed in the header so tests can drive demand paging directly (a synthetic `fault_info` + a manually constructed Proc) without triggering a real EL0 fault — at v1.0 pre-exec there's no userspace to fault.

## Implementation

### `arch/arm64/fault.c`

`fault_info_decode` is straight bit-extraction. The constants for FSC values mirror ARM ARM D17.2.40:

| FSC | Meaning |
|---|---|
| 0x04..0x07 | Translation fault L0..L3 |
| 0x09..0x0B | Access flag fault L1..L3 (FEAT_HAFDBS) |
| 0x0D..0x0F | Permission fault L1..L3 |

`arch_fault_handle` performs the priority-ordered checks above. Each check that matches calls `extinction_with_addr` (noreturn). The `FAULT_UNHANDLED_USER` return is the only non-extinction path; the caller (`exception_sync_curr_el`) currently extincts on it.

### `arch/arm64/exception.c::exception_sync_curr_el`

Refactored at P3-C to a thin dispatcher:

```c
case EC_DATA_ABORT_SAME:
case EC_INST_ABORT_SAME: {
    struct fault_info fi;
    fault_info_decode(esr, far, ctx->elr, &fi);
    enum fault_result r = arch_fault_handle(&fi);
    switch (r) {
    case FAULT_HANDLED: return;
    case FAULT_UNHANDLED_USER:
        extinction_with_addr("unhandled user-mode fault (no VMA / SIGSEGV pending)",
                             (uintptr_t)fi.vaddr);
    case FAULT_FATAL:
        extinction_with_addr("arch_fault_handle returned FAULT_FATAL",
                             (uintptr_t)fi.vaddr);
    }
    extinction_with_addr("arch_fault_handle returned unknown result",
                         (uintptr_t)fi.vaddr);
}
```

(The `LOWER` EC values aren't tested here — `exception_sync_curr_el` is wired to the Current EL/SPx vector. P3-E will add `exception_sync_lower_el` for the EL0 sync vector, using the same dispatcher.)

### Address-range classifiers

- `addr_is_stack_guard(addr)` checks: boot-stack guard PA range, boot-stack guard high VA range, and current-thread kstack guard region (direct-map KVA via `t->kstack_base`). Defense-in-depth: all three forms because FAR_EL1 may carry the PA OR the VA depending on which translation root caught the fault.
- `addr_is_kernel_image(addr)` checks: kernel-image PA range, kernel-image high VA range, and direct-map alias of kernel-image PA range (P3-Bca added the third).

## Data structures

```c
struct fault_info { ... };  // 56 bytes (3×u64 + 2×u32 + u8 + 6×bool + padding)
enum fault_result { ... };  // u32-sized enum
```

No kernel-side state; pure decoded view.

## State machines

### Fault dispatch flow (P3-C)

```
EXCEPTION (sync, current EL)
   │
   │ exception_sync_curr_el
   ▼
EC switch
   │
   ├── EC_DATA_ABORT_SAME / EC_INST_ABORT_SAME
   │      │
   │      │ fault_info_decode
   │      ▼
   │   struct fault_info
   │      │
   │      │ arch_fault_handle
   │      ▼
   │   ┌────────────────────────┐
   │   │ Priority checks (P3-C) │
   │   │  1. kstack guard       │── extinction
   │   │  2. W^X kernel image   │── extinction
   │   │  3. kernel translation │── extinction
   │   │  4. kernel permission  │── extinction
   │   │  5. kernel access-flag │── extinction
   │   │  6. user-mode          │── FAULT_UNHANDLED_USER
   │   │  7. unclassified       │── extinction
   │   └────────────────────────┘
   │      │
   │      └── FAULT_HANDLED → ERET (none at v1.0 P3-C)
   │      └── FAULT_UNHANDLED_USER → caller extincts
   │
   ├── EC_SP_ALIGN / EC_PC_ALIGN / EC_BTI / EC_BRK
   │      │
   │      └── extinction (handled inline in exception.c)
   │
   └── (default)
          │
          └── extinction("unhandled sync exception")
```

### User-mode resolved (P3-Dc)

```
6. user-mode
   │
   │ userland_demand_page(p, fi)
   ▼
   ├── vma_lookup(p, fi->vaddr)
   │      │
   │      ├── VMA covers FAR
   │      │   │
   │      │   ├── permission check vs is_write / is_instruction / read
   │      │   │      │
   │      │   │      ├── allowed
   │      │   │      │   ├── resolve VMO offset → backing PA
   │      │   │      │   ├── mmu_install_user_pte (walks/grows L0..L3)
   │      │   │      │   └── return FAULT_HANDLED
   │      │   │      │
   │      │   │      └── denied → return FAULT_UNHANDLED_USER
   │      │   │
   │      │   └── (no TLB flush: invalid → valid; ARM ARM B2.7.1)
   │      │
   │      └── no VMA → return FAULT_UNHANDLED_USER
```

PTE bit encoding for user pages:
- `VMA_PROT_R`  → `AP_RO_ANY | PXN | UXN` (RO for both ELs; no exec).
- `VMA_PROT_RW` → `AP_RW_ANY | PXN | UXN` (RW for both ELs; no exec).
- `VMA_PROT_RX` → `AP_RO_ANY | PXN` (RO for both ELs; user can exec).
- `VMA_PROT_W|X` is rejected at the VMA layer (vma_alloc) AND at the PTE installer (defense in depth).

W^X (I-12) holds by construction at every PTE: writable PTEs always have UXN+PXN set; executable-at-EL0 PTEs are read-only.

## Spec cross-reference

No new TLA+ spec at P3-C or P3-Dc. The reasoning:

- **P3-C dispatch**: straight if-else over decoded ESR — config parsing per CLAUDE.md "Features that usually don't [benefit from spec]".

- **P3-Dc demand paging at v1.0**: structurally simple under the v1.0 single-thread-Proc invariant.
  - **No new concurrency**: only the running thread of Proc P faults P's pgtable. No two CPUs concurrently demand-page on the same Proc. (Phase 5+ multi-thread Procs DO introduce concurrency on `mmu_install_user_pte`'s walk; a per-Proc pgtable lock OR a TLA+ extension at that point becomes necessary.)
  - **No new refcount semantics**: `userland_demand_page` doesn't take or release VMO refs. The VMA already holds `mapping_count`; the demand-paged page is just a PA reference, not a fresh ref. `vmo.tla::NoUseAfterFree` continues to hold by construction.
  - **W^X invariant by construction**: PTE bits derived from VMA prot, and VMA prot already excludes W+X. Every PTE the installer can produce satisfies "writable XOR executable-at-EL0".

The intermediate invariants `userland_demand_page` upholds — VMA-presence implies access permitted; PTE bits respect VMA prot; sub-table allocation rolls back cleanly on failure — are local to the function and verified by the unit tests. They're documented in commentary above the impl rather than formalized in TLA+; if a future bug demonstrates the structural reasoning was insufficient, a spec extension lands at that point.

ARCH §28 I-12 (W^X) is enforced at runtime by check #2 in arch_fault_handle (kernel-image case) and at PTE-construction time in `mmu_install_user_pte` (user-image case). PTE constructors at static layer + VMA layer + ELF loader form the layered defense.

## Tests

`kernel/test/test_fault_decode.c` — five unit tests on the decoder:

- `fault.decode_kernel_data_translation_l2`: kernel-mode data abort, translation fault L2, read.
- `fault.decode_kernel_data_permission_write`: kernel-mode data abort, permission fault L3, write.
- `fault.decode_user_data_translation`: lower-EL data abort, translation fault L1.
- `fault.decode_user_instruction_fetch`: lower-EL instruction abort, translation fault L3.
- `fault.decode_access_flag`: data abort, access-flag fault L2 (FEAT_HAFDBS).

Each test constructs a synthetic ESR via `mk_esr(ec, iss)` and verifies the decoded `fault_info` fields.

`kernel/test/test_demand_page.c` (P3-Dc) — seven unit tests on the demand-paging pipeline:

- `pgtable.install_user_pte_smoke`: install RW + RX leaf PTEs; verify L0→L3 chain is allocated; verify PTE bits match expected encoding (AP, PXN, UXN, AF, nG).
- `pgtable.install_user_pte_constraints`: zero pgtable_root, unaligned vaddr/pa, high-VA vaddr, W+X prot — all return -1.
- `pgtable.install_user_pte_idempotent`: identical re-install returns 0 without reallocating; mismatching PA at the same vaddr returns -1.
- `demand_page.smoke`: synthetic fault_info on a mapped VMA returns FAULT_HANDLED; L3 entry installed at expected vaddr pointing at the VMO's backing page.
- `demand_page.no_vma`: fault on unmapped vaddr → FAULT_UNHANDLED_USER.
- `demand_page.permission_denied`: write fault on RO VMA / instruction fault on non-EXEC VMA → FAULT_UNHANDLED_USER. Read fault on R-only VMA → FAULT_HANDLED.
- `demand_page.lifecycle_round_trip`: 4-page VMA + demand-page each page; proc_free + vmo_unref returns `phys_free_pages` to baseline (sub-tables freed by P3-Db walker; backing pages freed by VMO lifecycle).

The integration tests (`tools/test-fault.sh`) cover the dispatch:

- `kstack_overflow` → check #1 fires → "kernel stack overflow" extinction.
- `wxe_violation` → check #2 fires → "PTE violates W^X (kernel image)" extinction.
- `bti_fault` / `canary_smash` → not page faults; handled inline in `exception_sync_curr_el`.

## Error paths

The dispatcher's "errors" are extinctions. There's no other error mode — a fault either resolves (FAULT_HANDLED) or extincts (with a specific message). Diagnostic strings are stable and grep-able for tooling:

- `"kernel stack overflow"` (kstack guard).
- `"PTE violates W^X (kernel image)"` (kernel-image permission fault).
- `"unhandled kernel translation fault"` (kernel-mode translation fault).
- `"unhandled kernel permission fault"` (kernel-mode permission fault, not in kernel image).
- `"unhandled kernel access-flag fault"` (kernel-mode access-flag fault).
- `"unclassified kernel fault (ESR)"` (caught by step 7 — every other case).
- `"unhandled user-mode fault (no VMA / SIGSEGV pending)"` (caller-emitted on FAULT_UNHANDLED_USER).

## Performance characteristics

- `fault_info_decode`: ~10 instructions of bit extraction; sub-microsecond.
- `arch_fault_handle`: ~5 conditional branches on the fault-info booleans; sub-microsecond.

The dispatcher itself is not on a hot path (faults are exceptional). Performance-critical at P3-D when demand paging fires for every fresh user page.

## Status

- **Implemented at P3-C** (`12ff454`): `fault_info_decode`, `arch_fault_handle` with the kernel-mode classification paths, exception.c refactor to use the dispatcher, 5 decoder unit tests.
- **Implemented at P3-Dc**: `userland_demand_page` (vma_lookup → permission check → VMO offset → PTE install). `mmu_install_user_pte` walks/grows the per-Proc TTBR0 tree. `arch_fault_handle`'s user-mode case routes through `userland_demand_page`. 7 new unit tests in `test_demand_page.c`.
- **Stubbed**: COW (post-v1.0; not on the critical path for /init or the typical static-ELF case). EL0 sync exception vector (P3-E adds `exception_sync_lower_el` which reuses the dispatcher).

Commit landing points: `12ff454` (P3-C), `<P3-Dc hash>`.

## Known caveats / footguns

1. **`exception_sync_curr_el` only sees Current EL/SPx aborts**. EC_DATA_ABORT_LOWER (0x24) and EC_INST_ABORT_LOWER (0x20) are unreachable here. P3-E adds `exception_sync_lower_el` for the EL0 sync vector; that handler reuses the same dispatcher.

2. **`from_user` is derived from EC, not SPSR**. We rely on the ARMv8 architecture rule that EC discriminates lower-EL vs current-EL aborts; SPSR isn't consulted. Cleaner; matches the ARM ARM intent.

3. **Access-flag fault path extincts at v1.0**. PTE_AF is set eagerly by the constructors so the path shouldn't fire. If it does, something built a PTE without AF — kernel bug. P3-D's PTE installation must continue to set AF.

4. **`FAULT_FATAL` is reserved**. Currently no path returns it; `arch_fault_handle` extincts internally on every fatal path. The enum value exists for API completeness in case a P3-D handler needs to report fatality without extincting (e.g., to clean up Proc state before extincting from the caller).

5. **`addr_is_stack_guard` checks all three address forms**. FAR_EL1 may carry the PA (legacy TTBR0-identity access — should be impossible post-P3-Bda but defensive), the boot-stack high VA, OR the per-thread direct-map KVA. The classifier handles all three.

## Naming rationale

`fault.{c,h}` — standard. No thematic name proposed; `fault` is the universal term for ARMv8 page faults / aborts. The dispatcher is `arch_fault_handle` (no `arm64_` prefix because the abstraction itself is architecture-specific — the function lives in `arch/arm64/`).

`fault_result` enum values use FAULT_ prefix. Could have been `FAULT_OK / FAULT_BAD / FAULT_USER` but the explicit naming (HANDLED / FATAL / UNHANDLED_USER) makes the call-site read more clearly.
