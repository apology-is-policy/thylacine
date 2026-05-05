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

## Status

Implemented: P2-Ca at `<commit-pending>`. Stubbed: nothing. Deferred:
- **Per-CPU MMU enable + PAC init**: P2-Cb. Refactors: split `mmu_enable` into build/program (already done at P2-Ca to make this trivial); refactor primary's PAC init to populate globals so secondaries can re-load.
- **Per-CPU exception stack**: P2-Cc (closes P1-F shared-stack limitation per phase2-status.md trip-hazard).
- **Per-CPU vector table install** (VBAR_EL1): P2-Cb (low cost — `msr vbar_el1` of the same address as primary).
- **Per-CPU TPIDR_EL1 (current_thread per CPU)**: P2-Cb. Each CPU's idle thread becomes its initial current.
- **Per-CPU run tree + per-CPU sched_init**: P2-Cb.
- **GIC SGI infrastructure + IPI dispatch**: P2-Cd.
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
