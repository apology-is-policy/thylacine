# 17 — SMP secondary bring-up (as-built reference)

P2-Ca's deliverable: secondary CPUs (1..N-1) come up via PSCI_CPU_ON, run a minimal asm trampoline that flips a per-CPU online flag, and park at WFI. Boot CPU iterates DTB cpus + waits for each secondary's flag with a per-CPU timeout. Foundation for P2-Cb (per-CPU sched + per-CPU init) and P2-Cd (cross-CPU IPIs).

Scope: `lib/dtb.c` (cpu enumeration + PSCI method detection), `kernel/include/thylacine/dtb.h`, `arch/arm64/psci.{h,c}` (new), `arch/arm64/start.S` (`secondary_entry` trampoline), `kernel/include/thylacine/smp.h` (new), `kernel/smp.c` (new), `kernel/main.c` (smp_init wiring), `kernel/test/test_smp.c` (new).

Reference: `ARCHITECTURE.md §20` (per-core discipline), §22.2 (DTB-driven hardware discovery). Arm DEN 0022D (PSCI specification).

---

## Purpose

ARM64 multicore systems hold secondary CPUs in a low-power "PSCI parked" state at reset; only the primary CPU runs the bootloader-to-kernel handoff. For the kernel to actually use multiple cores, it must explicitly wake each secondary via the PSCI (Power State Coordination Interface) mechanism. PSCI is a firmware/hypervisor-level protocol: the kernel issues `HVC #0` (when running under a hypervisor-style firmware like QEMU virt) or `SMC #0` (when running under EL3 secure firmware on real hardware) with a function ID and arguments; the firmware/hypervisor wakes the requested CPU and points it at the entry-point address the kernel provides.

P2-Ca is the **minimum viable PSCI bring-up**. Each secondary, once awakened, runs an intentionally-minimal asm trampoline (no MMU, no PAC, no per-CPU exception stack, no per-CPU run tree) that:
1. Validates the PSCI-passed `context_id` (= secondary CPU index) is in range.
2. Computes the address of `g_cpu_online[idx]` via PC-relative addressing.
3. Stores `1` to that byte (to memory directly, since caches are off).
4. `dsb sy` to globally publish the store.
5. Parks at `wfi` indefinitely.

The boot CPU's `smp_init()` waits for each secondary's flag with a per-CPU timeout. If observed, the secondary is counted online; if timeout or PSCI failure, the secondary is counted offline and boot continues. Failures are logged to the UART but do not abort the kernel.

P2-Cb adds full per-CPU init (MMU, PAC, vector table, sched_init, idle thread) — at which point secondaries can run actual kernel code and be scheduled to. P2-Cd lands the IPI infrastructure that lets the primary signal secondaries (e.g., IPI_RESCHED).

---

## Public API

### `<thylacine/dtb.h>` — CPU + PSCI enumeration

```c
#define DTB_MAX_CPUS 8u

u32  dtb_cpu_count(void);                         // # /cpus/cpu@* nodes
bool dtb_cpu_mpidr(u32 idx, u64 *out_mpidr);     // MPIDR aff value
typedef enum { DTB_PSCI_NONE = 0,
               DTB_PSCI_HVC  = 1,
               DTB_PSCI_SMC  = 2 } dtb_psci_method_t;
dtb_psci_method_t dtb_psci_method(void);
```

`dtb_cpu_count()` walks the structure block, counts nodes whose `device_type` property contains "cpu". `dtb_cpu_mpidr(idx, out)` returns the `reg` cell (a single u32 under /cpus's #address-cells = 1, #size-cells = 0). On QEMU virt, MPIDR aff values are linear (0, 1, 2, 3) — the PSCI target arg passes the raw cell value.

`dtb_psci_method()` finds `/psci` (or `/psci@*`) and reads its `method` property. Returns `DTB_PSCI_HVC` for QEMU virt + KVM-style hypervised environments; `DTB_PSCI_SMC` for ARM TF-A and similar EL3 firmware. `DTB_PSCI_NONE` if the node is missing or the method is unknown — callers fall back to UP.

### `<arch/arm64/psci.h>` — calling primitives

```c
#define PSCI_VERSION              0x84000000u
#define PSCI_CPU_ON_64            0xC4000003u
#define PSCI_SUCCESS              0
#define PSCI_NOT_SUPPORTED        -1
#define PSCI_INVALID_PARAMETERS   -2
#define PSCI_DENIED               -3
#define PSCI_ALREADY_ON           -4
#define PSCI_ON_PENDING           -5
#define PSCI_INTERNAL_FAILURE     -6
#define PSCI_NOT_PRESENT          -7
#define PSCI_DISABLED             -8
#define PSCI_INVALID_ADDRESS      -9

bool psci_init(void);
bool psci_is_ready(void);
int  psci_cpu_on(u64 target, u64 entry_point, u64 context_id);
```

`psci_init` reads `/psci/method` from the DTB and caches the conduit (HVC or SMC). `psci_cpu_on` issues the `PSCI_CPU_ON_64` SMCCC call via the cached conduit. The function ID (0xC4000003) is the standard PSCI 0.2+ CPU_ON 64-bit variant — older PSCI variants with custom IDs are not supported (the DTB would have to declare them via `cpu_on = <id>`; we ignore those overrides at v1.0).

### `<thylacine/smp.h>` — bring-up + introspection

```c
extern volatile u8 g_cpu_online[DTB_MAX_CPUS];
unsigned smp_init(void);
unsigned smp_cpu_count(void);
unsigned smp_cpu_online_count(void);
```

`smp_init` does the actual bring-up loop. `g_cpu_online` is the per-CPU online flag (a byte per CPU in BSS); the asm trampoline writes the secondary's slot, the primary reads via volatile. `smp_cpu_count()` is `dtb_cpu_count()` cached. `smp_cpu_online_count()` returns the count after smp_init's wait loop.

---

## Implementation

### DTB walker for /cpus and /psci

`dtb_walk_cpus()` (lib/dtb.c) walks all nodes; for each, tracks `device_type` and `reg` per-node via a stack indexed by depth. On `END_NODE`, if the node was a cpu (device_type contains "cpu") and had a reg property, its reg cell is appended to the output array.

`dtb_psci_method()` walks until it finds a node whose name starts with "psci"; reads its `method` property. The walk stops at the END_NODE that pops out of /psci (depth tracking).

The walker handles the QEMU virt convention (#address-cells = 1, #size-cells = 0 under /cpus, single u32 reg cell). Other DTB formats (e.g., 64-bit MPIDR with #address-cells = 2) would need a parent-property walker — deferred until a Pi 5 / non-QEMU port needs it.

### PSCI calling

`smccc_call` (arch/arm64/psci.c) issues the SMCCC HVC or SMC instruction depending on the cached conduit. Function ID in x0; args in x1-x3; return in x0. The `register u64 x0 __asm__("x0")` constraint pins the function ID + return value; the inline asm uses `+r(x0)` for in/out.

The conduit (HVC vs SMC) is fixed at psci_init time and doesn't change at runtime. The two-branch dispatch in `smccc_call` is unavoidable because the same binary may run on different boards.

### Secondary trampoline (start.S)

`secondary_entry` lives in `.text` of the kernel image. The primary computes its PA at runtime as `kaslr_kernel_pa_start() + (secondary_entry_va - _kernel_start_va)`. Both are linked at high VA, but the offset between them is the same regardless of KASLR slide — so the PA computation is correct.

The trampoline runs at low PA with MMU off, IRQs masked (PSCI default). Sequence:
1. `bti c` — defensive landing pad.
2. Validate `x0` (= secondary CPU index, passed as PSCI's `context_id`) is in `[1, DTB_MAX_CPUS)`. Out-of-range branches to `secondary_bad` (a silent WFI loop with no flag set).
3. Compute `&g_cpu_online[idx]` via `adrp + lo12 + add`. PC-relative addressing works because PC = PA at trampoline entry, and `g_cpu_online` is in the same kernel image.
4. `strb #1` to write the online flag. With MMU off, the store is treated as Device-nGnRnE per ARM ARM B2.7 — goes directly to memory bypassing cache.
5. `dsb sy` — System-shareable Data Synchronization Barrier. Ensures the store is globally observable before any subsequent instruction.
6. `wfi` loop. PSCI-default IRQ masking means no IRQ delivery, so WFI sleeps until an external event (sev, reset). For P2-Ca there's no event source — secondaries sleep forever (intended).

**Why no MMU enable?** Enabling MMU on the secondary requires:
- Programming TTBR0/TTBR1 with the primary's already-built page tables.
- Enabling PAC (paciasp/autiasp need APIA programmed), or compiling secondary code without PAC.
- Setting up VBAR_EL1 (vector table) — secondaries inherit primary's, but the table assumes per-CPU exception stack.
- Per-CPU TPIDR_EL1 (current_thread).

Each of these is a substantive sub-piece. Doing them at P2-Ca would balloon the chunk to 1500+ LOC and conflate "PSCI works" with "per-CPU init works." Splitting cleanly: P2-Ca proves PSCI; P2-Cb adds per-CPU init.

**Why dsb sy and not dsb ishst?** The store target (g_cpu_online) is observable to ALL CPUs (boot included), not just the inner-shareable domain. dsb sy is the strongest barrier, ensuring globally visibility through caches + interconnect. dsb ishst would suffice on QEMU virt (boot is in the inner-shareable domain) but not on systems with non-coherent CPU clusters. Cheap insurance.

### `smp_init()` flow

1. Cache `dtb_cpu_count()` in `g_cpu_count`. If 0 (malformed DTB), treat as UP.
2. Mark `g_cpu_online[0] = 1` (boot CPU).
3. If `psci_is_ready()` is false, log "PSCI not available" and return 0.
4. Compute `entry_pa = kaslr_kernel_pa_start() + (secondary_entry - _kernel_start)`.
5. For `i = 1..N-1`:
    - Get `mpidr = dtb_cpu_mpidr(i)`.
    - Call `psci_cpu_on(mpidr, entry_pa, i)`.
    - On success or ALREADY_ON, wait for `g_cpu_online[i]` with timeout (100 ticks ≈ 100 ms).
    - On any other PSCI status, log + skip.
6. Return the count of secondaries that came online.

The wait loop uses `dmb ish` on each iteration to ensure the volatile read picks up the secondary's `dsb sy`-published store. On QEMU virt this is automatic; on real hardware the ish barrier prevents any reordering of the load with respect to the secondary's store-via-memory.

---

## Data structures

### `g_cpu_online` (BSS, 8 bytes)

```c
extern volatile u8 g_cpu_online[DTB_MAX_CPUS];
```

One byte per CPU. Set by:
- `smp_init` for boot CPU (slot 0).
- Asm trampoline `strb` for each secondary (slot 1..N-1).

Read by:
- `smp_init`'s wait loop.
- `test_smp_bringup_smoke` for verification.

`volatile` so the compiler doesn't hoist reads out of polling loops. Cache coherence on QEMU virt is automatic; bare-metal might require explicit `dc ivac` before reads (P2-Ca trip-hazard).

`DTB_MAX_CPUS = 8` — matches ARCH §20.7's v1.0 SMP cap. Increasing requires bumping the macro + auditing the per-CPU array dimensions everywhere.

---

## Spec cross-reference

P2-Ca touches no formal spec — PSCI bring-up is hardware/firmware coordination, not a kernel concurrency invariant. The cross-CPU spec work begins at P2-Cd (IPI ordering, ARCH §28 I-18) when SGIs are used to coordinate scheduler events across CPUs.

---

## Tests

| Test | What it verifies |
|---|---|
| `smp.bringup_smoke` | After `smp_init` completes (during main.c boot): `dtb_cpu_count() ≥ 1`; `smp_cpu_count() == dtb_cpu_count()`; `smp_cpu_online_count() == smp_cpu_count()` (all CPUs online); `g_cpu_online[0..N-1]` all 1; `g_cpu_online[N..DTB_MAX_CPUS-1]` all 0 (untouched). |

The test runs in the boot kthread context after smp_init has already executed; it inspects the resulting state. Direct injection of "force a secondary to fail" would require a test-only PSCI hook — deferred.

---

## Error paths

| Condition | Behavior |
|---|---|
| `dtb_init` failed | smp_init treats cpu_count = 1, marks boot online, returns 0. |
| No /psci node | smp_init logs "PSCI not available — secondaries held", returns 0. |
| PSCI returns non-success/ALREADY_ON for a specific cpu | Logs the PSCI error name + cpu index, skips, continues with next cpu. |
| Timeout waiting for online flag | Logs "PSCI ok but online-flag timed out", counts cpu as offline. |
| `dtb_cpu_count() > DTB_MAX_CPUS` | Caps at DTB_MAX_CPUS, logs warning. |

No extinction at P2-Ca's smp_init — secondaries failing to come up degrade gracefully to lower-CPU operation.

---

## Performance characteristics

PSCI HVC on QEMU virt: microseconds per call (HVC is just a hypercall to the QEMU-emulated hypervisor). The trampoline runs in tens of nanoseconds (a handful of asm instructions). Wait loop polls `g_cpu_online[i]` with `dmb ish` each iteration — sub-microsecond per iteration; the secondary's flag is typically observable within microseconds of PSCI success.

Total smp_init for 4 CPUs on QEMU virt: ~50 µs.

Bare metal will be slower (PSCI calls go to TF-A/EL3 firmware which programs the power controller). 100 ticks = 100 ms timeout is generous.

---

## Per-CPU init at high VA (P2-Cb)

P2-Cb extends the trampoline so secondaries reach a real C entry point at the kernel's high VA. After the minimal P2-Ca trampoline (online flag + WFI), the trampoline now:

1. **Sets SP** from `g_secondary_boot_stacks[idx-1]` (16 KiB per secondary, BSS, `aligned(16)`).
2. **Flips `g_cpu_online[idx]`** with caches off — early "trampoline reached" signal (kept for diagnostic distinction from `g_cpu_alive`).
3. **`bl pac_apply_this_cpu`** — leaf asm function (start.S) loads `g_pac_keys[8]` into `AP*KEY*_EL1` + sets SCTLR.EnIA/EnIB/EnDA/EnDB/BT0. Cross-CPU PAC consistency is REQUIRED for thread migration (P2-Ce work-stealing): a thread's signed return address on its kstack must auth-validate against APIA on whichever CPU resumes it.
4. **`bl mmu_program_this_cpu`** — re-uses primary's already-built page tables; programs MAIR/TCR/TTBR0/TTBR1/SCTLR.M.
5. **Long-branch via `kaslr_high_va_addr` to high VA `per_cpu_main(idx)`**.

`per_cpu_main(int cpu_idx)` (kernel/smp.c) is `noreturn` and:
1. Sets `VBAR_EL1` to `_exception_vectors` (shared with primary).
2. Sets `TPIDR_EL1 = NULL` (no per-CPU current thread at P2-Cb; P2-Cd or later assigns per-CPU idle threads).
3. Flips `g_cpu_alive[cpu_idx] = 1` + `dsb sy` — the "fully initialized" signal.
4. Enters idle WFI loop indefinitely with IRQs masked.

**PAC keys refactor** (P2-Cb): primary's inline PAC code in start.S replaced with `bl pac_derive_keys` (asm function that derives 8 key halves from `cntpct_el0` + ROR chain, stores to `g_pac_keys[8]` BSS) + `bl pac_apply_this_cpu`. Each CPU calls `pac_apply_this_cpu` to load the same shared keys.

**`mmu_enable` refactor** (P2-Ca, used at P2-Cb): split into `mmu_program_this_cpu` (program MMU registers from already-built tables) + `mmu_enable` (build_page_tables + program). Primary calls `mmu_enable` once; secondaries call `mmu_program_this_cpu` directly.

**`smp_init` watches `g_cpu_alive`** at P2-Cb (was `g_cpu_online` at P2-Ca). The stricter signal catches PAC/MMU/VBAR/TPIDR failures that would leave a secondary stuck mid-init. `g_cpu_online` is still set by the trampoline as a diagnostic for "trampoline reached but per_cpu_main didn't" failures (logged with that specific message).

---

## Per-CPU exception stacks via SPSel=0 (P2-Cc)

P2-Cc separates the kernel's "normal mode" stack from its "exception handling" stack on every CPU. The kernel runs at EL1 with `PSTATE.SPSel = 0` — `sp` refers to `SP_EL0`, which holds the current thread/boot stack. Hardware exception entry sets `SPSel = 1` (per ARM ARM D1.10.4), so `KERNEL_ENTRY` in vectors.S operates on `SP_EL1`. Each CPU's `SP_EL1` is set to the top of its own slot in `g_exception_stacks[DTB_MAX_CPUS][4096]` BSS — total 32 KiB. After the handler returns, `KERNEL_EXIT`'s `eret` restores `SPSel` from `SPSR_EL1.M[0] = 0` and the kernel resumes on `SP_EL0`.

### What this gives us

1. **Per-CPU isolation**. CPU N's `SP_EL1` is `&g_exception_stacks[N][4096]`. Concurrent IRQs on different CPUs land on different exception stacks — there's no cross-CPU stack sharing for handler frames.
2. **Stack-overflow safety**. A kernel thread that overruns its `SP_EL0` stack into the guard page faults. The fault's `KERNEL_ENTRY` runs on `SP_EL1` (a known-good stack with full headroom), not on the dying `SP_EL0`. So `KERNEL_ENTRY`'s `sub sp, sp, #EXCEPTION_CTX_SIZE` does NOT recursively fault, and `exception_sync_curr_el`'s `kernel stack overflow` diagnostic is reachable. This closes the P1-F "KNOWN LIMITATION" comment in vectors.S.

### How SP_EL0 / SP_EL1 are set up

Both registers are written via different mechanisms because of ARM ARM access rules:

- `SP_EL0`: writable from EL1 via `msr sp_el0, xN` regardless of `SPSel`.
- `SP_EL1`: `msr sp_el1, xN` is **UNDEFINED** at EL1 (ARM ARM B6.2). The only legal way to write `SP_EL1` from EL1 is `mov sp, xN` while `SPSel = 1` (so that `sp` refers to `SP_EL1`).

So the boot sequence (start.S `_real_start` step 4.6) is:
1. `mov x0, sp` — copy current sp (= `SP_EL1` = boot stack, set in step 3).
2. `msr sp_el0, x0` — `SP_EL0 := boot stack top`.
3. Compute `g_exception_stacks[0]`'s top.
4. `mov sp, x0` — `SP_EL1 := exception-stack top` (still `SPSel = 1`).
5. `msr SPSel, #0` — `sp` now refers to `SP_EL0` (= boot stack).
6. `isb`.

`SP_EL1` at this point is still a low PA (PC-relative resolution before MMU is on). Step 8.5 (after `bl mmu_enable`) re-anchors `SP_EL1` at the post-KASLR HIGH VA via `kaslr_high_va_addr` + the SPSel-dance:
```asm
adrp x0, g_exception_stacks
add  x0, x0, :lo12:g_exception_stacks
add  x0, x0, #4096                    // EXCEPTION_STACK_SIZE; CPU 0 top
bl   kaslr_high_va_addr               // x0 = HIGH VA of slot 0 top
msr  SPSel, #1                         // sp now refers to SP_EL1
isb
mov  sp, x0                            // SP_EL1 := HIGH VA top
msr  SPSel, #0                         // sp back to SP_EL0
isb
```

Secondaries do the equivalent in `secondary_entry`: SP_EL0 = per-CPU boot stack, SP_EL1 = per-CPU exception stack at LOW PA, SPSel = 0; then after `bl mmu_program_this_cpu`, the SPSel-dance re-anchors SP_EL1 at the HIGH VA of `g_exception_stacks[idx]`.

### Vector dispatch under SPSel=0

ARM64 routes "current EL" exceptions to one of two vector groups based on `PSTATE.SPSel` at exception time:
- `SPSel = 0` → offsets `0x000-0x180` ("Current EL with SP_EL0")
- `SPSel = 1` → offsets `0x200-0x380` ("Current EL with SP_ELx")

P2-Cc moves the live Sync + IRQ slots into the `SP_EL0` group (`0x000` + `0x080`) — that's where the kernel's normal-mode SPSel=0 exceptions land. The `SP_ELx` slots remain live too (`0x200` + `0x280`) because the kernel transits through SPSel=1 mode after a sched() context-switch from inside an IRQ handler: cpu_switch_context's `mov sp` writes the current-SP register (= SP_EL1 since hardware set SPSel=1 on entry) to the new thread's kstack value, then the ret-path lands on `thread_trampoline` which unmasks IRQs while still in SPSel=1 mode. The next IRQ on that CPU dispatches to the SPx group rather than the SP_EL0 group. Both groups call into the same C handlers (`exception_sync_curr_el`, `exception_irq_curr_el`) — identical recovery path, both routes observably equivalent. After the eventual eret unwinds back to the original thread's natural EL1t state, subsequent IRQs route to the SP_EL0 group.

FIQ and SError slots in both groups remain `VEC_UNEXPECTED` — neither is unmasked at v1.0.

### Observability

`smp.exception_stack_smoke` (test_smp.c) verifies the discipline at runtime:
- Confirms `PSTATE.SPSel == 0` in normal kernel mode (read via `mrs ..., SPSel`).
- Confirms `g_exception_stacks` BSS is sized exactly `DTB_MAX_CPUS * EXCEPTION_STACK_SIZE` and slots are contiguous.
- Reads `g_exception_stack_observed[0]`, written by `timer_irq_handler` on its first invocation per CPU as `&local`. Verifies the address falls inside `g_exception_stacks[0]`'s slot — runtime evidence that the timer IRQ ran on the per-CPU exception stack as expected. (Direct `mrs sp_el1` from EL1 is UNDEFINED per ARM ARM, so the observation is captured indirectly via the C handler's local-address.)

`smp_cpu_idx_self()` (smp.c) returns `MPIDR_EL1.Aff0` masked to 8 bits — the boot CPU sees 0, secondaries see 1..N-1. Used by `timer_irq_handler` to index `g_exception_stack_observed`; reusable as a per-CPU dispatch key in future sub-chunks.

### What stays at LOW PA

`g_secondary_boot_stacks` (the per-secondary "normal mode" stack used as `SP_EL0` initially) is set by `secondary_entry` to its low-PA address pre-MMU. After `mmu_program_this_cpu`, `SP_EL1` is re-anchored to high VA but `SP_EL0` is NOT — secondaries continue executing on the low-PA boot stack until P2-Cd assigns each one a per-CPU idle thread (which has its own kstack at high VA). This is a deliberate v1.0 P2-Cc choice: the SP_EL0-side address space transition is part of the per-CPU idle-thread bring-up, not the exception-stack bring-up.

---

## Status

Implemented: P2-Ca + P2-Cb + P2-Cc + P2-Cd (Cda + Cdb + Cdc) at `<commit-pending>`. Stubbed: nothing. Deferred:
- **Cross-CPU thread placement**: P2-Ce work-stealing. Currently `ready(t)` inserts into THIS CPU's tree; threads created on the boot CPU stay on the boot CPU.
- **finish_task_switch pattern (closes SMP wait/wake race)**: P2-Cf.
- **scheduler.tla SMP refinement** (per-CPU runqueues + Steal action + IPI ordering invariant I-18): P2-Cg.
- **IPI_TLB_FLUSH / IPI_HALT / IPI_GENERIC** (ARCH §20.4): land when use-cases arrive (TLB shootdown for namespace rebind in Phase 5+; shutdown for clean halt; generic callback delivery).
- **Per-CPU SP_EL0 to high VA on secondaries**: P2-Ce (alongside per-CPU work delivery).
- **Pi 5 / multi-cluster Aff{1,2,3} encoding**: Phase 7 hardening pass.

---

## P2-Cd: per-CPU run trees + idle threads + IPI infrastructure

P2-Cd extends per_cpu_main beyond a pure WFI park. After per-CPU init (PAC, MMU, VBAR, exception stack — all from P2-Cb/Cc), each secondary now:

1. Allocates an idle Thread descriptor via `thread_init_per_cpu_idle(cpu_idx)` (no kstack — runs on the per-CPU boot stack assigned by start.S secondary_entry).
2. Sets TPIDR_EL1 to that idle Thread.
3. Calls `sched_init(cpu_idx)` — initializes this CPU's slot in `g_cpu_sched[]` (run tree, vd_counter, idle pointer).
4. Calls `smp_cpu_ipi_init(cpu_idx)` — see below.
5. Sets `g_cpu_alive[cpu_idx]` (the "fully ready" signal that smp_init waits for).
6. Enters the idle loop: `for(;;){sched();wfi;}`.

### Per-CPU GIC bring-up (P2-Cdc)

`gic_init_secondary(cpu_idx)` performs this CPU's GIC initialization:
- Per-CPU redistributor wake + SGI/PPI bank config (group 1 NS, default priority, all disabled). Frame at `g_redist_base + cpu_idx * 0x20000`.
- Per-CPU CPU interface system-register bring-up: ICC_SRE/PMR/BPR/CTLR/IGRPEN1.

The redistributor MMIO region was mapped Device-nGnRnE in `gic_init` — covers all per-CPU frames in one mmu_map_device call (region size from DTB).

### IPI infrastructure

SGI INTID assignments (`IPI_*` macros in `<thylacine/smp.h>`):
- **`IPI_RESCHED = 0`** — wake target CPU's WFI to process its run tree. v1.0 P2-Cdc lands this only.
- IPI_TLB_FLUSH / HALT / GENERIC reserved per ARCH §20.4; deferred until use-cases arrive.

`gic_send_ipi(target_cpu_idx, sgi_intid)` writes ICC_SGI1R_EL1:
```
sgi = (sgi_intid << 24) | (1 << target_cpu_idx);
```
Encoding pinned for QEMU virt's flat-Aff0 cluster (Aff{1,2,3}=0). Multi-cluster hardware needs Aff fields populated — Phase 7 work.

`ipi_resched_handler(intid, arg)` (smp.c) increments `g_ipi_resched_count[smp_cpu_idx_self()]` for observability. The handler doesn't manually call sched() — vectors.S IRQ slot calls `preempt_check_irq` after `exception_irq_curr_el → gic_dispatch → ipi_resched_handler` returns; if any cross-CPU placer set this CPU's `need_resched`, preempt_check_irq picks it up. At v1.0 P2-Cdc no cross-CPU placer exists yet (P2-Ce); IPI_RESCHED is purely a "wake from WFI" signal proving the SGI delivery path works.

### smp_cpu_ipi_init order

Called from per_cpu_main BEFORE `g_cpu_alive[cpu_idx] = 1` so by the time the boot CPU's smp_init wait observes alive, the secondary is fully IPI-receivable:
1. gic_init_secondary(cpu_idx)
2. gic_attach(IPI_RESCHED, ipi_resched_handler, NULL)
3. gic_enable_irq(IPI_RESCHED)
4. msr daifclr, #2 (unmask IRQs at PSTATE)

### gic_enable_irq SGI/PPI now per-CPU

P2-Cdc bug fix: `gic_enable_irq` for SGI/PPI was hardcoded to write `g_redist_base` (CPU 0's frame). For secondaries calling it from their own context, the write must target the calling CPU's frame. Refactored to use `cpu_redist_base(smp_cpu_idx_self())`. The fix doesn't affect existing CPU-0 callers (gic_init's timer-PPI enable in main.c) because `cpu_redist_base(0) == g_redist_base`.

### Test

`smp.ipi_resched_smoke`:
- Snapshot pre-send `g_ipi_resched_count[]` baseline.
- Boot CPU calls `gic_send_ipi(i, IPI_RESCHED)` for each secondary `i`.
- Polls (with 100-tick timeout per secondary) until each secondary's count > baseline.
- Verifies boot CPU's slot unchanged (we don't IPI ourselves).
- **GIC SGI infrastructure + IPI dispatch** (IPI_RESCHED/TLB_FLUSH/HALT/GENERIC): P2-Cd.
- **Work-stealing**: P2-Ce.
- **finish_task_switch pattern (closes SMP wait/wake race)**: P2-Cf.
- **scheduler.tla SMP refinement** (per-CPU runqueues + Steal action + IPI ordering invariant I-18): P2-Cg.

---

## Known caveats / footguns

1. **Secondaries cannot execute kernel code beyond the trampoline at P2-Ca**. They have no MMU, no PAC, no exception vectors, no per-CPU stack, no per-CPU TPIDR_EL1. Any IRQ to a secondary would fault into the (unset) vector table → undefined behavior. Currently all device IRQs are routed to the boot CPU (default GIC routing); P2-Cd's per-CPU IRQ routing closes this.

2. **PSCI's `context_id` arg is the secondary CPU index** (`i` from the loop). Don't confuse with MPIDR — MPIDR is the target-cpu arg, context_id is what arrives in the secondary's x0 at trampoline entry.

3. **The secondary trampoline's PA depends on `_kernel_start_va` matching the linker script's BASE**. If the linker script changes the base of the kernel image, `kaslr_kernel_pa_start() + (secondary_entry - _kernel_start)` may compute wrong. The KASLR base is `KASLR_LINK_VA = 0xFFFFA00000080000`; `_kernel_start` is the symbol at the linker-script's image base; their relationship is fixed by the linker script. Don't move `_kernel_start` without updating the trampoline PA computation.

4. **Cache coherence assumption**: the asm trampoline writes `g_cpu_online[idx]` with caches off (Device-nGnRnE). The boot CPU reads via TTBR1's Normal-WB cacheable mapping — `volatile + dmb ish` is sufficient on QEMU virt. On bare-metal hardware where coherency might not include CPUs in pre-MMU state, the boot CPU might need an explicit `dc ivac` before the read. Phase 7 hardening (Pi 5 bring-up) revisits.

5. **No PSCI version check**. We assume PSCI 0.2+ standard function IDs. Older PSCI variants use different IDs declared in the DTB (`cpu_on = <id>` etc.); we ignore those. QEMU virt advertises PSCI 1.0; bare-metal will too.

6. **The trampoline is in `.text` (executable from the kernel image)**. It must therefore be reachable via the kernel's load PA. The boot loader (QEMU's load_aarch64_image) loads the entire image at a single PA; the trampoline is part of the image; it's reachable by computing kaslr_kernel_pa_start + offset.

7. **DTB_MAX_CPUS = 8 is hard-coded throughout**. The DTB walker's stack arrays, the asm trampoline's bounds check (`mov x9, #8`), the `g_cpu_online` array dimension, all depend on this. Bumping it requires updating each site.

---

## Naming rationale

`smp_init`, `smp_cpu_count`, `smp_cpu_online_count` — standard Linux/BSD vocabulary. `secondary_entry` — Linux uses `secondary_startup`; we shorten. `g_cpu_online` — Linux uses `cpu_online_mask` (bitmap); we use a byte array for simplicity at v1.0.

PSCI function names + return codes are pinned by the Arm spec (DEN 0022D); no thematic alternative would communicate intent better.
