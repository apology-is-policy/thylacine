# Reference: page-fault dispatcher (P3-C)

## Purpose

The fault dispatcher decodes ARMv8 ESR_EL1 / FAR_EL1 / ELR_EL1 into a structured `fault_info` and routes the fault to a handler that either resolves it (returns `FAULT_HANDLED` — the ERET resumes the interrupted instruction) or extincts with a specific diagnostic. At v1.0 P3-C the resolve path is empty (no in-tree handler resolves a fault); every call extincts. P3-D adds VMA-tree dispatch for user-mode faults — that wires the demand-paging path that turns `FAULT_UNHANDLED_USER` into a satisfied page allocation.

The dispatcher is the substrate for:
- ARCH §28 I-12 (W^X) runtime detection — kernel-image permission faults are recognized and produce "PTE violates W^X (kernel image)" extinctions.
- Per-thread kstack overflow detection (P3-Bca: kstack guard pages mapped no-access in the kernel direct map).
- Per-Proc VMA dispatch at P3-D (TODO).

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
6. **User-mode** → returns `FAULT_UNHANDLED_USER` (caller extincts at v1.0; P3-D wires VMA-tree dispatch here).
7. **Anything else** → `extinction("unclassified kernel fault (ESR)")`.

Returns `FAULT_HANDLED` only if a future P3-D path resolves the fault (none yet).

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

### Future state at P3-D

At P3-D the user-mode path becomes:

```
6. user-mode
   │
   ├── VMA tree lookup (current_thread()->proc->vmas)
   │      │
   │      ├── VMA covers FAR + permissions match
   │      │   ├── allocate page (alloc_pages)
   │      │   ├── install PTE in per-Proc page table
   │      │   ├── flush single TLB entry (tlbi vaae1is)
   │      │   └── return FAULT_HANDLED
   │      │
   │      └── no VMA / permission mismatch
   │              │
   │              └── return FAULT_UNHANDLED_USER (→ Phase 5+ SIGSEGV)
```

## Spec cross-reference

No new TLA+ spec at P3-C (config parsing per CLAUDE.md "Features that usually don't [benefit from spec]"). The dispatch logic is straight if-else; the resolve paths (P3-D's COW + demand paging) ARE spec-worthy and will extend `vmo.tla` or land a new `mm.tla`.

ARCH §28 I-12 (W^X) is enforced at runtime by check #2 in arch_fault_handle. The PTE constructors enforce I-12 at PTE-construction time; the dispatcher provides the runtime "you violated it" diagnostic.

## Tests

`kernel/test/test_fault_decode.c` — five unit tests on the decoder:

- `fault.decode_kernel_data_translation_l2`: kernel-mode data abort, translation fault L2, read.
- `fault.decode_kernel_data_permission_write`: kernel-mode data abort, permission fault L3, write.
- `fault.decode_user_data_translation`: lower-EL data abort, translation fault L1.
- `fault.decode_user_instruction_fetch`: lower-EL instruction abort, translation fault L3.
- `fault.decode_access_flag`: data abort, access-flag fault L2 (FEAT_HAFDBS).

Each test constructs a synthetic ESR via `mk_esr(ec, iss)` and verifies the decoded `fault_info` fields.

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

- **Implemented at P3-C**: `fault_info_decode`, `arch_fault_handle` with the kernel-mode classification paths, exception.c refactor to use the dispatcher, 5 decoder unit tests.
- **Stubbed**: user-mode VMA dispatch (P3-D), demand paging (P3-D), COW (P3-D).

Commit landing point: `<TODO: pending>`.

## Known caveats / footguns

1. **`exception_sync_curr_el` only sees Current EL/SPx aborts**. EC_DATA_ABORT_LOWER (0x24) and EC_INST_ABORT_LOWER (0x20) are unreachable here. P3-E adds `exception_sync_lower_el` for the EL0 sync vector; that handler reuses the same dispatcher.

2. **`from_user` is derived from EC, not SPSR**. We rely on the ARMv8 architecture rule that EC discriminates lower-EL vs current-EL aborts; SPSR isn't consulted. Cleaner; matches the ARM ARM intent.

3. **Access-flag fault path extincts at v1.0**. PTE_AF is set eagerly by the constructors so the path shouldn't fire. If it does, something built a PTE without AF — kernel bug. P3-D's PTE installation must continue to set AF.

4. **`FAULT_FATAL` is reserved**. Currently no path returns it; `arch_fault_handle` extincts internally on every fatal path. The enum value exists for API completeness in case a P3-D handler needs to report fatality without extincting (e.g., to clean up Proc state before extincting from the caller).

5. **`addr_is_stack_guard` checks all three address forms**. FAR_EL1 may carry the PA (legacy TTBR0-identity access — should be impossible post-P3-Bda but defensive), the boot-stack high VA, OR the per-thread direct-map KVA. The classifier handles all three.

## Naming rationale

`fault.{c,h}` — standard. No thematic name proposed; `fault` is the universal term for ARMv8 page faults / aborts. The dispatcher is `arch_fault_handle` (no `arm64_` prefix because the abstraction itself is architecture-specific — the function lives in `arch/arm64/`).

`fault_result` enum values use FAULT_ prefix. Could have been `FAULT_OK / FAULT_BAD / FAULT_USER` but the explicit naming (HANDLED / FATAL / UNHANDLED_USER) makes the call-site read more clearly.
