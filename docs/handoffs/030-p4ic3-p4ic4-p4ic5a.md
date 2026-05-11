# Handoff 030 — P4-Ic3 + P4-Ic4 + P4-Ic5a

**Window**: post-handoff 029 (`1179df9`) → tip `b96db5a`. 6 substantive commits + 4 hash fixups + 1 audit close = ~10 commits worth of work (across spec, kernel, userspace runtime, first userspace hw-handle binary).

**TL;DR**: Closed the v1.0 capability-grant hole and shipped the first userspace binary that holds `CAP_HW_CREATE` and reads real hardware. P4-Ic3 added the kernel-internal `rfork_with_caps` primitive (spec-first; R11 audit closed clean). P4-Ic4 extracted the `libthyla-rs` runtime crate. P4-Ic5a landed `/mmio-probe` — a Rust userspace binary spawned by kproc via `rfork_with_caps(CAP_HW_CREATE)` that calls `t_mmio_create` + `t_mmio_map` against QEMU virt's PL031 RTC, demand-pages the device-memory PTE, and reads the live `PeriphID0=0x31`. Two binding toolchain shifts landed alongside: kernel `-mcmodel=tiny → -mcmodel=small` (image crossed 1 MiB), userspace Rust `target-feature=...,-neon,-fp-armv8` (kernel doesn't enable EL0 FPEN; rustc's precompiled compiler-builtins memset uses NEON intrinsics by default). 207 → 211 tests; 4 specs / 14 cfg variants all clean; `handles.cfg` explores 11.7M distinct states in 8m55s.

**This is the first time userspace touches real hardware in Thylacine.**

---

## Tip + posture

- **Tip**: `b96db5a` (P4-Ic5a hash fixup).
- **Substantive**: `17855dd` (P4-Ic5a).
- **Predecessors**: `caa49e3` / `81da4eb` (P4-Ic4); `031a91f` / `ed6c587` (P4-Ic3 + R11 audit close).
- **Branch**: `main`. 41 commits ahead of `origin/main`.
- **Tree**: clean.

### Test posture

- **211/211** in-kernel tests PASS × default (~395 ms boot) + UBSan (~404 ms).
- **4/4** fault matrix PASS (`tools/test-fault.sh`).
- **10/10** KASLR distinct offsets (`tools/verify-kaslr.sh` under `BOOT_TIMEOUT=20`).
- **9 cfgs** for `handles.tla` all behave correctly: correct cfg 11.7M distinct states / 317M generated / depth 25 / 8m55s on 8 cores (full convergence — 0 states left on queue); 8 buggy cfgs all produce expected counterexamples at depths 4-5 (75-534 states).

### Verify on pickup

```bash
# Default build + tests
tools/build.sh kernel && tools/test.sh
# Expected: 211/211 PASS, boot ~395 ms.

# UBSan
tools/build.sh kernel --sanitize=undefined && tools/test.sh
# Expected: 211/211 PASS.

# Fault matrix
tools/test-fault.sh
# Expected: 4/4 PASS. (If smp.exception_stack_smoke flakes under fault
# builds, re-run — known intermittent host-pressure issue.)

# KASLR
tools/verify-kaslr.sh
# Expected: 10/10 distinct offsets. (Re-run if 9/10 — flake at high
# host load; deterministic at low load.)

# Specs
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs && for cfg in handles.cfg handles_buggy_*.cfg; do
    echo "== $cfg =="
    java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
        -config "$cfg" handles.tla 2>&1 | tail -3
done
# Expected: handles.cfg clean (8m55s converged); 8 buggy cfgs produce
# expected counterexamples at depths 4-5.

# Boot a probe-running VM by hand (optional — confirms the userspace
# hw-handle path runs cleanly in a single VM, not just under the test
# harness):
tools/run-vm.sh </dev/null 2>&1 | grep -E "mmio-probe:"
# Expected output:
#   mmio-probe: starting (P4-Ic5a)
#   mmio-probe: SYS_MMIO_CREATE ok
#   mmio-probe: SYS_MMIO_MAP ok
#   mmio-probe: rtcdr=0x<epoch> periphid0=0x00000031
#   mmio-probe: PASS
```

---

## P4-Ic3 — kernel-internal `rfork_with_caps` capability grant primitive

**Commits**: `031a91f` substantive + `ed6c587` hash fixup. R11 audit close folded into the substantive commit (0 P0 + 0 P1 + 0 P2 + 3 P3 doc-hygiene closed).

### What

Closes the v1.0 capability-grant hole that blocked P4-Ic5+: until now `rfork(RFPROC, ...)` produced children with `caps = CAP_NONE` (per `KP_ZERO`), so the future driver Proc had no way to obtain `CAP_HW_CREATE` without breaking the v1.0 "no userspace cap-grant syscall" stance.

P4-Ic3 adds a kernel-internal-only primitive:

```c
int rfork_with_caps(unsigned flags, void (*entry)(void *), void *arg,
                    caps_t caps_mask);
```

Identical to `rfork` except the child's caps are set to:

```c
caps_t parent_caps = __atomic_load_n(&parent->caps, __ATOMIC_ACQUIRE);
child->caps = parent_caps & caps_mask;
```

The AND with `parent->caps` enforces `specs/handles.tla::RforkWithCaps`'s `granted ⊆ proc_caps[parent]` precondition (CapsCeiling) regardless of what mask the caller passes — a caller cannot grant a capability the caller itself doesn't hold. The acquire-fence on `parent->caps` matches R9 F146's discipline (in `syscall.c::sys_*_handler`) so a concurrent Phase 5+ `ReduceCaps` writer cannot tear the read.

`rfork` becomes a one-liner delegate (`rfork_internal(..., CAP_NONE)`) — same observable behavior at every existing call site, no churn.

### Spec extension

`specs/handles.tla` gained:

- **New state variable** `proc_ceiling \in [Procs -> SUBSET Caps]` — per-Proc capability ceiling. At Init: ProcRoot's ceiling = `Caps`; all others = `{}` (matching the pre-P4-Ic3 `InitialCapsOf` semantics so existing buggy cfgs still produce counterexamples via the direct-from-Init path).
- **Strengthened `CapsCeiling`** from `proc_caps[p] \subseteq InitialCapsOf(p)` (static) to `proc_caps[p] \subseteq proc_ceiling[p]` (dynamic). Preserves the old "non-root procs start with `{}` ceiling so BuggyCapsElevate fires" guarantee while modeling P4-Ic3's rfork-time inheritance.
- **New action `RforkWithCaps(parent, child, granted)`**: preconditions `granted \subseteq proc_caps[parent]`, child slot uninitialized (caps = `{}`, ceiling = `{}`, handles = `{}`), and `parent # child`. Effect: `proc_ceiling[child] := proc_caps[parent]`; `proc_caps[child] := granted`. Models the kernel-internal `rfork_with_caps` primitive.
- **New bug class `BuggyRforkElevate(parent, child, gained)`**: mirrors `RforkWithCaps`'s effects but drops the subset-of-parent precondition, granting the child caps the parent doesn't hold. Caught by `CapsCeiling` (child's ceiling is parent's caps, but caps exceed ceiling). Maps to the kernel-side bug "rfork_with_caps replaces `&` with `=` or `|`".
- **New cfg variant `handles_buggy_rfork_elevate.cfg`**. Counterexample: `ReduceCaps(ProcRoot, {C})` → ProcRoot has `{HW}`; `BuggyRforkElevate` grants p2 = `{C}` ⊄ `{HW}`. `CapsCeiling` fires at depth 5.

TLC verified across 9 cfgs:

- `handles.cfg` (correct): 317M states / 11.7M distinct / depth 25 / 8m55s on 8 cores; 0 states left on queue (full convergence).
- 8 buggy cfgs: each produces expected counterexample (depths 4-5, 75-534 distinct states).

### Tests

3 new kernel tests in `kernel/test/test_caps.c`:

- `caps.rfork_with_caps_grants_subset` — kproc rforks with `caps_mask=CAP_HW_CREATE`; child observes exactly that bit.
- `caps.rfork_with_caps_clamps_to_parent` — zero-cap intermediate Proc rforks grandchild with `caps_mask=CAP_HW_CREATE`; grandchild gets `CAP_NONE`. Proves AND-with-parent is the real ceiling (a bug-class impl that replaced `&` with `=` would fail this test by yielding `CAP_HW_CREATE` on the grandchild).
- `caps.rfork_with_caps_zero_mask` — `caps_mask=CAP_NONE` equivalent to plain `rfork`; child has `CAP_NONE`. Pins the rfork delegate.

### R11 adversarial audit

`memory/audit_r11_closed_list.md`. **0 P0 + 0 P1 + 0 P2 + 3 P3 doc-hygiene findings (F160-F162); ALL CLOSED in audit-close commit; 0 deferred. Clean spec-first landing.**

- **F160 [P3]**: `specs/SPEC-TO-CODE.md` handles.tla section was P2-Fa-era (listed 4 buggy cfgs + 5 invariants). Closed by comprehensive refresh covering P4-Ib + P4-Ic3 actions, invariants, 11.7M state-count.
- **F161 [P3]**: `docs/reference/39-hw-handles.md` "7 buggy cfgs" / "1.4M distinct states" stale. Closed by updating to 8 cfgs / 11.7M state-count with explanatory note (proc_ceiling adds a state dimension; RforkWithCaps is a reachable action).
- **F162 [P3]**: `BuggyCapsElevate`'s catch surface narrowed post-dynamic-ceiling — after `RforkWithCaps(ProcRoot, p, {})` raises p's ceiling to `Caps`, `BuggyCapsElevate(p, {HW})` doesn't violate `CapsCeiling` (post-state `{HW} \subseteq Caps`). The direct-from-Init path remains catchable. Closed by Option A (documented intentional relaxation in `handles.tla`): the dynamic ceiling correctly models "caps shrink within ceiling" semantics; v1.0 has no syscall that raises caps post-fork so the narrowed catch surface is unreachable. Option B (stricter `CapsMonotonicReduce` action property) considered and rejected — would weaken `RforkWithCaps`'s expression while not changing v1.0 reachability.

Audited deeply with NO findings: privilege escalation via mask bits outside CAP_ALL, memory ordering for child->caps publication, concurrent ReduceCaps writer race, UAF on rollback paths, kproc-self-rfork degenerate case, BuggyRforkElevate counterexample shape, one-shot RforkWithCaps per child, boundary masks, cap drift via `_Static_assert`, test-to-bug-class mapping tightness.

---

## P4-Ic4 — `libthyla-rs` runtime crate extraction

**Commits**: `caa49e3` substantive + `81da4eb` hash fixup. Non-audit-bearing pure Rust toolchain refactor.

### What

Lifts the inline SVC wrappers, `_start` global_asm!, and `#[panic_handler]` from `usr/hello-rs/src/main.rs` (where they lived at P4-Ia2) into a new `usr/lib/libthyla-rs/` crate — sibling of the C-side `usr/lib/libt/`.

Why now: P4-Ic5+ needs the same SVC machinery + `_start` + panic_handler. Inline-copying ~100 lines of syscall plumbing into every new Rust binary would be both noise and a drift hazard. Extracting now gives downstream crates (`mmio-probe`, future virtio-blk) a clean `libthyla-rs` dependency to import.

### Surface

`usr/lib/libthyla-rs/src/lib.rs` (~230 lines) exposes:

- **Syscall numbers**: `T_SYS_EXITS=0`, `T_SYS_PUTS=1`, `T_SYS_MMIO_CREATE=2`, `T_SYS_IRQ_CREATE=3`, `T_SYS_IRQ_WAIT=4`, `T_SYS_MMIO_MAP=5`.
- **Right bits**: `T_RIGHT_READ`, `T_RIGHT_WRITE`, `T_RIGHT_MAP`, `T_RIGHT_TRANSFER`, `T_RIGHT_SIGNAL`.
- **Prot bits**: `T_PROT_READ`, `T_PROT_WRITE`, `T_PROT_EXEC`.
- **SVC wrappers** (`pub unsafe fn` for SVC-issuing primitives; safe `pub fn` for type-bounded convenience): `t_exits`, `t_puts`, `t_putstr`, `t_mmio_create`, `t_mmio_map`, `t_irq_create`, `t_irq_wait`.
- **`_start`** via `global_asm!` with `.globl _start`: BTI c → `bl rs_main` → `mov x8, #0` → `svc #0` → defensive `wfe; b 1b`.
- **`#[panic_handler]`** tail-calling `t_exits(1)`.

### Linker-side correctness

`usr/scripts/aarch64-userspace.ld` already has `ENTRY(_start)`, which keeps `_start` alive across the rlib boundary even though no Rust code in the binary references it directly. The linker treats ENTRY's argument as a liveness root and pulls the symbol from `libthyla_rs.rlib` automatically. This avoids the cortex-m-rt-style contortions (`#[used]` anchors, `--whole-archive` flags) that would otherwise be needed.

### ELF layout pinned identical to pre-extraction

`hello-rs` binary size before P4-Ic4: 66,336 bytes (P4-Ic3 baseline). After: **66,336 bytes** (same).

```
0000000000400000 <_start>:
  400000:  hint #0x22       // BTI c
  400004:  bl 400018        // -> rs_main
  400008:  mov x8, #0       // T_SYS_EXITS
  40000c:  svc #0
  400010:  wfe
  400014:  b 400010

0000000000400018 <rs_main>:
  400018:  nop
  40001c:  adr x0, 400040   // "hello..."
  400020:  mov w1, #0x34    // 52 bytes
  400024:  mov w8, #1       // T_SYS_PUTS
  400028:  svc #0
  40002c:  mov x0, xzr      // return 0
  400030:  ret
```

Bit-identical to the P4-Ic3 layout.

`hello-rs/src/main.rs` collapses from ~110 lines to ~30:

```rust
#![no_std]
#![no_main]

use libthyla_rs::t_putstr;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("hello from /hello-rs (Rust no_std, built via cargo)\n");
    0
}
```

---

## P4-Ic5a — `/mmio-probe` first userspace hw-handle binary

**Commits**: `17855dd` substantive + `b96db5a` hash fixup. Audit-bearing at the test-coverage discipline level; no new findings (the SVCs exercised here are P4-Ib + P4-Ic2 which were already audited at R9 + R10). 211/211 PASS × default (~395 ms) + UBSan (~404 ms); 4/4 fault; 10/10 KASLR.

### What

Lands the first non-kproc Proc that holds `CAP_HW_CREATE` and the first userspace binary to call `SYS_MMIO_CREATE` + `SYS_MMIO_MAP`. Reads live hardware (PL031 RTC) via the demand-page MMIO dispatch installed at P4-Ic2 — verifying that the full chain works end-to-end:

```
kproc (CAP_ALL)
  -> rfork_with_caps(CAP_HW_CREATE)              [P4-Ic3]
  -> libthyla-rs _start                          [P4-Ic4]
  -> rs_main calling t_mmio_create + t_mmio_map  [P4-Ic5a]
  -> SVC handlers: cap check + range claim + ownership transfer [P4-Ib]
  -> SYS_MMIO_MAP: handle validation + prot check + burrow_map [P4-Ic2]
  -> first MMIO read at user-VA -> page fault
  -> userland_demand_page -> case BURROW_TYPE_MMIO
  -> mmu_install_user_pte(..., device_memory=true)
  -> MAIR_IDX_DEVICE (nGnRnE) PTE installed
  -> LDR returns live MMIO data -> PeriphID0 == 0x31  [REAL HARDWARE]
```

Closes the bulk of deferred R10 F159 (SVC-path test coverage for `SYS_MMIO_CREATE` + `SYS_MMIO_MAP`). Remaining `SYS_IRQ_CREATE` + `SYS_IRQ_WAIT` SVC-path coverage lands at **P4-Ic5b** when the full virtio-blk driver ships with IRQ-driven block completion.

### Why PL031 RTC and not virtio-mmio

At P4-Ic2's R10 F154 fix, the kernel reserves the entire virtio-mmio range (0x0a000000-0x0a003fff) in `g_mmio_claims` via `kobj_mmio_reserve_kernel_ranges()` so a `CAP_HW_CREATE`-holding userspace driver can't claim a slot the kernel might own. That fix correctly blocks userspace from claiming virtio-mmio slots — including empty ones, because the kernel reserves them all defensively.

PL031 RTC (PA 0x09010000) is NOT in the kernel-reserved list because Thylacine v1.0 has no kernel-side RTC driver. It's freely claimable from userspace and ideal as the test surface. When a Phase 5+ kernel RTC driver lands, the kernel will reserve PL031 and `mmio-probe` will need to move to a different test surface (or get replaced by the actual virtio-blk driver at P4-Ic5b which will get a kernel-side delegation API to selectively un-reserve a virtio-mmio slot for the driver).

This is a **known design constraint to revisit at P4-Ic5b**: the binary "userspace cannot claim virtio-mmio" rule is correct as a default (F154 protection), but a real driver needs an escape hatch — either a delegated-claim mechanism (kernel hands a kernel-reserved range to a child Proc at rfork time) or a refinement to the reservation policy (only reserve slots the kernel actively uses, not all 32).

### Verification target

PeriphID0 at offset `0xfe0` = **`0x31`** (the "31" in PL031, per ARM PrimeCell PL031 r1p3 TRM Table 3-2). A compile-time constant from device ROM proves the read returned LIVE device data rather than cached zeros / RAM — the canonical failure mode of forgetting device-memory PTE attrs. RTCDR at offset 0 (current epoch time) is informational.

Observed boot output:

```
mmio-probe: starting (P4-Ic5a)
mmio-probe: SYS_MMIO_CREATE ok
mmio-probe: SYS_MMIO_MAP ok
mmio-probe: rtcdr=0x6a018e2e periphid0=0x00000031
mmio-probe: PASS
/mmio-probe reaped pid=1309 status=0 — SVC path verified end-to-end
```

`0x6a018e2e` ≈ 1,778,024,494 seconds since the Unix epoch ≈ **2026-04-05 23:01:34 UTC** — QEMU seeds RTCDR from the host clock at boot.

### Kernel test

`userspace.mmio_probe_rfork_with_caps` (in `kernel/test/test_mmio_probe.c`):

1. `devramfs_lookup("mmio-probe", ...)` — locates the ELF in the boot ramfs cpio.
2. Copies into 8-aligned static buffer (per R5-G F61 alignment requirement on the Ehdr cast in `exec_setup`).
3. `rfork_with_caps(RFPROC, mmio_probe_exec_thunk, &args, CAP_HW_CREATE)` — spawns the child with the cap mask AND'd against kproc's CAP_ALL.
4. The thunk verifies `(p->caps & CAP_HW_CREATE) != 0` as a defense-in-depth check, then runs `exec_setup` + `userland_enter`.
5. Parent `wait_pid`s, asserts `exit_status == 0`.

If `/mmio-probe` wasn't built (fresh checkout where `tools/build.sh userspace` hasn't run yet), the test prints a skip notice and returns PASS — keeps the kernel test suite green for the bootstrap-only configuration. Production / CI always builds the full ramfs.

---

## Two binding toolchain shifts

### Kernel `-mcmodel=tiny → -mcmodel=small`

**Why**: the kernel image crossed 1 MiB during this chunk (new `test_mmio_probe.c` + the mmio-probe binary in the ramfs cpio + the libthyla-rs library code). `cmodel=tiny` uses single-instruction ADR with ±1 MiB range; crossing 1 MiB trips `R_AARCH64_ADR_PREL_LO21 out of range: 1048652` on `_kernel_end` references in `kaslr.c`.

**Fix**: `cmodel=small` uses ADRP+ADD (32-bit, ±4 GiB) — the standard for kernels. KASLR semantics unaffected (both models produce PC-relative code; only `R_AARCH64_RELATIVE` absolute-pointer relocations participate in KASLR sliding). Verified: KASLR 10/10 distinct under `BOOT_TIMEOUT=20` post-shift.

**File**: `cmake/Toolchain-aarch64-thylacine.cmake`. Comment block rewritten to document the rationale + the failure mode that triggered the switch.

### Userspace Rust `target-feature=...,-neon,-fp-armv8`

**Why**: the Thylacine kernel doesn't yet save/restore FP register state on context switch (`-mgeneral-regs-only` kernel-side; `CPACR_EL1.FPEN` traps EL0 FP/SIMD instructions). rustc's compiler-builtins `memset`/`memcpy` emit NEON intrinsics by default on `aarch64-unknown-none` — `dup v0.4h, w1` + `mov h0..h3, v0.h[N]` for small zero-fills. These trap EL0 with `EC=0x07` ("Trapped FP/SIMD/SVE access") on the first such instruction. Disabling FP/SIMD in rustc's target features forces general-register implementations everywhere.

**Caveat**: rustflags don't reach the precompiled compiler-builtins rlib that ships with rustc. So even with the flag, code that triggers a memset/memcpy call into compiler-builtins still sees the NEON path. We refactored mmio-probe to use a `static mut HEX_BUF` pre-initialized at link time (avoiding the `let arr = [0u8; N]` pattern that lowers to compiler-builtins memset). Future Rust crates need to follow the same discipline OR we get the proper fix in (see next).

**File**: `usr/.cargo/config.toml`. Comment block documents the rationale + the future fix (Phase 5+ lazy-FP-trap-and-allocate).

**The proper v1.0 fix** (deferred to P4-Ic5b or a dedicated chunk): lazy-FP context save in the kernel. Set `CPACR_EL1.FPEN = 0b01` (trap on EL0 FP access; allow at EL1). On first EL0 FP trap, allocate a per-Thread FP context buffer + set `FPEN = 0b11` for that Thread. On context switch, save FP regs if the thread has FP state, restore if the next thread does. ~80-150 LOC kernel change. Until then, the userspace toolchain stays SIMD-disabled — and any future Rust userspace binary that triggers a memset/memcpy intrinsic needs to either avoid array-init patterns or rebuild compiler-builtins from source via `-Z build-std=core,compiler_builtins` (nightly Rust).

---

## What's NEXT — P4-Ic5b onward

### P4-Ic5b — Full virtio-blk driver crate

The next major milestone. Decompose into:

1. **Add virtio-blk device to QEMU** (`tools/run-vm.sh`): `-drive driver=null-co,if=none,id=blk0,size=1M` + `-device virtio-blk-device,drive=blk0`. Or `-drive file=disk.img,if=none,id=blk0,format=raw` if we want known-content reads. The null-co approach is sufficient for "device exists + responds to commands"; a real disk image with known bytes is needed for "read returns expected pattern" verification.

2. **Kernel-side virtio-mmio reservation refinement**: P4-Ic5a-followup. Refine `kobj_mmio_reserve_kernel_ranges()` so it only reserves virtio-mmio slots the kernel is actively using (those with non-zero `device_id` + kernel-side virtio module attached), OR add a kernel-side delegation API (`kobj_mmio_delegate_to_proc(pa, size, target_proc)`) that hands a kernel-reserved range to a child Proc at rfork time. Either approach lets P4-Ic5b's driver claim a specific virtio-mmio slot. Recommend the delegation API — cleaner contract; F154 protection stays intact for everything except explicitly delegated ranges.

3. **`usr/virtio-blk/` driver crate** (Rust no_std, depends on libthyla-rs):
   - **Discovery**: kproc-side test passes the virtio-mmio slot PA to the driver via argv-like mechanism (or via a known mapping at exec_setup time).
   - **Bring-up**: feature negotiation per VIRTIO 1.2 §3.1.1 (reset → ACKNOWLEDGE → DRIVER → FEATURES_OK → DRIVER_OK).
   - **Virtqueue setup**: split-virtqueue per VIRTIO 1.2 §4.1.5 (descriptor table, avail ring, used ring; all three live in userspace memory and must be physically contiguous + page-aligned; need a kernel-side helper to allocate page-pinned memory the device can DMA to — this is a non-trivial sub-problem, see below).
   - **Block read**: submit `VIRTIO_BLK_T_IN` descriptor chain for LBA 0; `t_irq_wait` on the device's INTID until the used ring increments; parse the response; verify the data.
   - **Userspace driver test**: kproc rforks the driver via `rfork_with_caps(CAP_HW_CREATE)`; driver reads block 0; kernel test verifies via a side channel (e.g., driver writes the read bytes to a known location and kernel test reads them post-wait_pid).

4. **DMA-coherent memory in userspace**: virtqueue rings + data buffers need to be PA-stable + page-pinned (the device DMAs to them). Userspace anonymous pages today are paged via the BURROW system with no pinning guarantee. Needed: a new syscall (`SYS_DMA_ALLOC(size)` → `(va, pa)` tuple, or `SYS_DMA_PIN_HANDLE(burrow_handle)`) that allocates a userspace-mapped + kernel-pinned + PA-stable buffer. Audit-bearing (introduces a new kobj type `KObj_DMA`; spec extension to `handles.tla`). **This is the gnarliest sub-problem of P4-Ic5b.**

5. **IRQ delegation**: at v1.0 userspace can call `t_irq_create(intid)` for SPI INTIDs (kernel-reserved SGI/PPI are rejected per R9 F142+F145). Per the same delegation theme as item 2, the kernel needs a way to declare "this Proc can claim this specific INTID range" — currently any CAP_HW_CREATE holder can grab any SPI, which is too permissive. **Defer to Phase 5+** if not a blocker.

**Recommendation**: split P4-Ic5b into **P4-Ic5b1** (virtio-mmio delegation API + DMA-pinning syscall + spec extension) and **P4-Ic5b2** (driver crate itself, depending on b1's primitives). b1 is audit-bearing; b2 is non-audit-bearing once b1's invariants are pinned.

### P4-Ic5b alternatives — what to do without virtio-blk

If P4-Ic5b is too ambitious for the next session, simpler stepping stones:

- **P4-Ic5-FP**: kernel-side lazy-FP context save (CPACR_EL1.FPEN trap-and-allocate). Removes the `-neon,-fp-armv8` v1.0 patch. Spec-light but audit-bearing (concurrency on FP state per thread). ~80-150 LOC.
- **P4-Ic5-IRQ-probe**: new userspace binary `irq-probe` that calls `t_irq_create` + `t_irq_wait` on a kernel-generated test SGI. Closes the remaining R10 F159 SVC coverage (`SYS_IRQ_CREATE` + `SYS_IRQ_WAIT`) without needing virtio infrastructure. Requires a kernel-side helper to send an SGI from kproc context to the userspace child's CPU. ~100 LOC.

### P4-Ic6/7 + P4-Id (unchanged from handoff 029)

- **P4-Ic6**: end-to-end driver test — once P4-Ic5b lands, kproc rforks the virtio-blk driver, driver reads block 0, kernel test verifies the bytes.
- **P4-Ic7**: cumulative Phase 4 audit of P4-Ia through P4-Ic (R12 if findings emerge).
- **P4-Id**: driver-as-9P-server — the driver exposes `#blk` synthetic Dev with `read`/`write` Stratum-style ops. Closes ROADMAP §6.3 exit criterion ("a userspace driver reads from virtio-blk via 9P").

---

## File inventory — what's new in this session window

### P4-Ic3

**New files** (2):
- `memory/audit_r11_closed_list.md` (~140 lines).
- `specs/handles_buggy_rfork_elevate.cfg` (21 lines).

**Modified files**:
- `kernel/include/thylacine/proc.h` — new `rfork_with_caps` decl + caps.h include.
- `kernel/proc.c` — new `rfork_internal` shared worker; `rfork` becomes a delegate.
- `kernel/test/test_caps.c` — 3 new tests (~80 lines).
- `kernel/test/test.c` — registry entries for the new tests.
- `specs/handles.tla` — new `proc_ceiling` state var + `RforkWithCaps` + `BuggyRforkElevate` + strengthened `CapsCeiling` + F162 commentary.
- `specs/handles*.cfg` (8 files) — added `BUGGY_RFORK_ELEVATE = FALSE` to each pre-existing cfg.
- `specs/SPEC-TO-CODE.md` — handles.tla section comprehensively refreshed (R11 F160 close).
- `docs/reference/39-hw-handles.md` — test rows + Status entries + R11 F161 close ("7 buggy" → "8" + state count refresh).
- `docs/phase4-status.md` — P4-Ic3 row.
- `docs/REFERENCE.md` — snapshot.

### P4-Ic4

**New files** (2):
- `usr/lib/libthyla-rs/Cargo.toml`.
- `usr/lib/libthyla-rs/src/lib.rs` (~230 lines).

**Modified files**:
- `usr/Cargo.toml` — workspace member list extended.
- `usr/hello-rs/Cargo.toml` — `[dependencies] libthyla-rs = { path = "../lib/libthyla-rs" }`.
- `usr/hello-rs/src/main.rs` — collapsed from ~110 → ~30 lines.
- `usr/Cargo.lock` — auto-updated.
- `docs/reference/38-userspace.md` — Rust-side `_start` section rewritten; new `libthyla-rs` surface table.
- `docs/phase4-status.md` — P4-Ic4 row.
- `docs/REFERENCE.md` — snapshot.

### P4-Ic5a

**New files** (3):
- `usr/mmio-probe/Cargo.toml`.
- `usr/mmio-probe/src/main.rs` (~185 lines).
- `kernel/test/test_mmio_probe.c` (~155 lines).

**Modified files**:
- `cmake/Toolchain-aarch64-thylacine.cmake` — `-mcmodel=tiny → -mcmodel=small` + rationale comment.
- `usr/.cargo/config.toml` — `target-feature=...,-neon,-fp-armv8` + rationale comment.
- `usr/Cargo.toml` — `mmio-probe` workspace member.
- `usr/Cargo.lock` — auto-updated.
- `tools/build.sh` — `build_ramfs::usr_rs_bins` adds `mmio-probe`.
- `kernel/CMakeLists.txt` — `test/test_mmio_probe.c` source.
- `kernel/test/test.c` — registry entry.
- `docs/reference/39-hw-handles.md` — test row.
- `docs/phase4-status.md` — P4-Ic5a row.
- `docs/REFERENCE.md` — snapshot.

**Total LOC added this session window**: ~1130 (per `git diff --stat 1179df9..HEAD`).

---

## Bug-hunting / lessons learned

### P4-Ic3

1. **Spec extension scope creep is real**. The initial intent was "tiny spec change to model rfork-with-caps." It became `proc_ceiling` state variable + strengthened invariant + new action + new bug class + 9-cfg matrix verification. Reason: the existing `CapsCeiling` was static (`InitialCapsOf`), incompatible with rfork-time inheritance. Lesson: when adding a new action that creates *new state* (a new Proc that didn't exist at Init), check whether existing invariants depend on Init-time facts.

2. **R11's F162 surfaced a "narrowed catch surface" subtlety**. Strengthening `CapsCeiling` to dynamic ceiling preserved the OLD bug-catch path (direct-from-Init) but admitted a NEW state (post-RforkWithCaps with elevated ceiling) where `BuggyCapsElevate` no longer fires. The audit caught this; we chose Option A (documented intentional relaxation) over Option B (stricter `CapsMonotonicReduce`) because Option B's expressive cost > its v1.0 catch value. Lesson: when strengthening an invariant by introducing dynamic state, audit whether any existing bug-class's catch surface narrows.

3. **`__atomic_load_n(ACQUIRE)` on parent->caps is forward-proof**. There's no v1.0 writer of `parent->caps` post-creation (no ReduceCaps syscall). But the acquire fence matches R9 F146's discipline at the syscall layer and pre-empts a future race when ReduceCaps lands. Lesson: when adding read paths to shared state, install the right fence even if there's no current writer — keeps the invariant locally true regardless of caller context.

### P4-Ic4

1. **`ENTRY(_start)` in the linker script is doing real work**. Without `ENTRY` listing `_start` as a liveness root, the linker would garbage-collect the `_start` symbol from `libthyla_rs.rlib` since no Rust source in the consuming binary references it directly. The linker script was always carrying this load (P4-Ia2 setup); P4-Ic4 just makes it visible. Lesson: when extracting symbols into a shared library/rlib, the linker's liveness analysis can be the difference between "works" and "silently drops the symbol." Verify the symbol is in the final ELF (`objdump -d <bin> | grep _start`).

2. **The hello-rs binary size pinned post-refactor (66,336 bytes) is a quiet but meaningful verification**. Identical layout pre/post = no functional change. If size had grown, that'd flag either a real change (e.g., extra symbols pulled from the rlib) or a configuration drift. Lesson: for extraction refactors, pin the output size as a smoke test — cheap and catches surprises.

### P4-Ic5a

1. **R10 F154's "reserve all virtio-mmio" was over-broad and creates a real footgun for the next chunk**. The reservation reserved every virtio,mmio compatible from DTB, including the 31 empty slots that QEMU populates only on demand. P4-Ic5b will need a delegation API or a refinement to the reservation policy. Lesson: defensive reservations done at one chunk should explicitly account for the consumer chunks. The F154 close was correct in its threat model but unnecessarily blocked the legitimate use case it was meant to enable.

2. **rustc's NEON memset is a stealth FP dependency**. The `let arr = [0u8; N]` pattern lowers to `compiler_builtins::memset` which uses NEON `dup v0.4h` intrinsics on `aarch64-unknown-none`. No FP/SIMD in the user code; the dependency is fully hidden in the precompiled compiler-builtins rlib. `-C target-feature=-neon,-fp-armv8` doesn't reach that rlib (rustflags only apply to the crates being compiled). Workarounds: (a) avoid array-init patterns (`static mut`, byte-by-byte writes, MaybeUninit); (b) `-Z build-std=core,compiler_builtins` (nightly); (c) enable EL0 FP in the kernel (the proper v1.0 fix). Lesson: when targeting a no-FP environment, audit the toolchain's intrinsic library AS WELL as your code; precompiled artifacts can carry hidden dependencies on instruction set features.

3. **`-mcmodel=tiny` is too tight for any kernel that crosses 1 MiB**. The comment in `Toolchain-aarch64-thylacine.cmake` claimed "kernel image < 200 KB" which was true at P1-G but no longer at P4-Ic5a. We hit `R_AARCH64_ADR_PREL_LO21 out of range` on `_kernel_end` references in `kaslr.c` — clang chose ADR (single instruction, ±1 MiB) for `extern char _kernel_end[]` because tiny allows it; once `_kernel_end` was > 1 MiB from kaslr.c's load site, the relocation overflowed. Switching to `cmodel=small` (ADRP+ADD, ±4 GiB) fixed it without affecting KASLR. Lesson: code-model comments age. If a comment cites a size constraint, verify the constraint still holds when growing past comfortable thresholds.

4. **PL031 is a useful "free MMIO" target for v1.0 hardware tests**. Standard ARM PrimeCell IP, well-documented, deterministic compile-time magic value (PeriphID0 = 0x31), present in QEMU virt at a known PA, NOT in Thylacine's kernel reservation list. Lesson: when looking for a userspace-claimable hardware surface for SVC-path testing, look for standard ARM IP blocks that the kernel doesn't depend on. PL031 + PL061 (GPIO) + PL022 (SPI) + similar are options.

---

## Naming + commitment + open follow-ups

- **Project motto**: "The thylacine runs again." This window earned its use — first userspace touches real hardware. Keep for end-of-phase / release moments; don't wear out.
- **Naming conventions (post-rename)**: unchanged from handoff 028. Chan→Spoor, Pgrp+namespace→Territory, Vmo→Burrow, devtab→bestiary, _hang→_torpor, /init→/joey, panic→extinction.
- **Audit-prosecutor naming**: kept as "prosecutor" for Stratum continuity per CLAUDE.md "Hold for explicit signoff" list.

**Held thematic-rename candidates** (none load-bearing):
- `rfork_with_caps` could be `rfork_dowry` (a gift conferred at fork-time) — held; descriptive name wins.
- `libthyla-rs` is locked (mirrors `libt`).
- `mmio-probe` is descriptive; could be `pelt-probe` (thylacine's striped pelt is the identifying feature, mirroring how PL031's PeriphID0 identifies the device); held — `mmio-probe` is what it does.

**Cumulative open follow-ups** (deferred findings carried forward):
- F108-F110 (R6-A): pid=0 transient / wakeup-in-lock latency / orphan leak — all forward-looking.
- F113/F115/F116/F119 (R6-B): kernel direct map + W^X alias hardening deferrals.
- F130/F132/F137 (R7): W^X defense-in-depth / proc_free TLB ordering / proc_alloc rollback symmetry.
- F140/F141 (R8): defensive wfe loop / alt `msr sp_el0` fix.
- F149/F150 (R9): per-CPU SGI/PPI semantics / ReduceCaps drop-precondition.
- **F159 (R10): SVC-path test coverage — partially closed at P4-Ic5a (SYS_MMIO_CREATE + SYS_MMIO_MAP); remaining SYS_IRQ_CREATE + SYS_IRQ_WAIT closes at P4-Ic5b.**
- **NEW design constraint from this window**: virtio-mmio reservation refinement (P4-Ic5b needs a delegation API or policy refinement to allow userspace driver claim of a specific virtio-mmio slot).
- **NEW design constraint from this window**: kernel-side lazy-FP save/restore — until it lands, userspace Rust toolchain stays SIMD-disabled and binaries must avoid `let arr = [0u8; N]` patterns that trigger compiler-builtins memset.

---

## References

- `docs/handoffs/029-p4ic1-p4ic2.md` — predecessor (Burrow MMIO type + SYS_MMIO_MAP + R10 audit close).
- `docs/handoffs/028-p4ib.md` — KObj_MMIO + KObj_IRQ handle integration + caps + R9 audit close.
- `docs/handoffs/027-p4ia1-p4ia2.md` — C + Rust userspace runtimes.
- `memory/audit_r11_closed_list.md` — full R11 audit close (P4-Ic3).
- `specs/handles.tla` — the spec extended this window (proc_ceiling + RforkWithCaps + BuggyRforkElevate).
- `specs/SPEC-TO-CODE.md` — handles.tla section comprehensively refreshed (R11 F160 close).
- `docs/reference/38-userspace.md` — Rust-side _start + libthyla-rs surface table.
- `docs/reference/39-hw-handles.md` — hw-handle reference doc (Tests table updated for caps + mmio-probe).
- `docs/phase4-status.md` — Phase 4 sub-chunk landed table.
- `kernel/proc.c::rfork_internal` — the kernel-internal capability grant primitive (~50 LOC).
- `usr/lib/libthyla-rs/src/lib.rs` — Rust userspace runtime crate (~230 LOC).
- `usr/mmio-probe/src/main.rs` — first userspace hw-handle binary (~185 LOC).
- `cmake/Toolchain-aarch64-thylacine.cmake` — kernel toolchain (cmodel switch documented here).
- `usr/.cargo/config.toml` — userspace Rust toolchain (FP/SIMD-disabled rationale here).
