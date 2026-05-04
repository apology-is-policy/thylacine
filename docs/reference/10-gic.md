# 10 — GIC v3 driver (as-built reference)

The kernel's interrupt controller driver. Detects ARM Generic Interrupt Controller version from DTB ("arm,gic-v3" → live; "arm,cortex-a15-gic" / "arm,gic-400" → extinctions cleanly), brings up the distributor + redistributor + system-register CPU interface, exposes IRQ enable / disable / acknowledge / EOI, and dispatches INTIDs to registered handlers.

P1-G deliverable. The first IRQ source wired through is the ARM generic timer (PPI 14 → INTID 30; see `docs/reference/11-timer.md`).

Scope: `arch/arm64/gic.{h,c}`, the IRQ slot in `arch/arm64/vectors.S` (P1-G repoint), `arch/arm64/exception.c`'s `exception_irq_curr_el`, `kernel/main.c`'s `gic_init` slot in `boot_main`, banner updates.

Reference: `ARCHITECTURE.md §12.3` (GIC autodetect commitment), `§28` invariant I-15 (DTB-driven hardware discovery), ARM IHI 0069 (GICv3 + GICv4 architecture specification).

---

## Purpose

Until P1-G, the kernel's IRQ vector slot routed to `exception_unexpected` — any IRQ would be a kernel bug. P1-G installs a real GIC driver and turns the IRQ slot live. From here on, the kernel can:

- Receive periodic timer ticks (1000 Hz) — the basis for Phase 2's scheduler.
- Receive device IRQs (PL011 IRQ-driven TX, virtio devices) — though P1-G wires only the timer; UART IRQ-driven mode is post-v1.0.
- Send inter-processor interrupts (SGIs) — Phase 2 with SMP.

GICv3-only at P1-G. ARCH §12 commits to v2/v3 autodetect at v1.0; the autodetect mechanism IS implemented (DTB compat probe), but the v2 path extinctions cleanly with a deferred-to-future-chunk diagnostic. Rationale in [Caveats](#caveats).

---

## Public API

`arch/arm64/gic.h`:

```c
// INTID conventions (architectural).
#define GIC_NUM_INTIDS         1020
#define GIC_INTID_SPURIOUS     1023
#define GIC_SGI_MIN            0    // 0..15
#define GIC_PPI_MIN            16   // 16..31
#define GIC_SPI_MIN            32   // 32..1019
#define GIC_PPI_TO_INTID(ppi)  ((ppi) + 16)

typedef enum {
    GIC_VERSION_NONE = 0,
    GIC_VERSION_V2   = 2,
    GIC_VERSION_V3   = 3,
} gic_version_t;

typedef void (*gic_irq_handler_t)(u32 intid, void *arg);

bool          gic_init(void);
gic_version_t gic_version(void);
u64           gic_dist_base(void);
u64           gic_redist_base(void);

bool gic_attach(u32 intid, gic_irq_handler_t handler, void *arg);
bool gic_enable_irq(u32 intid);
bool gic_disable_irq(u32 intid);

u32  gic_acknowledge(void);
void gic_eoi(u32 intid);
void gic_dispatch(u32 intid);   // called by exception_irq_curr_el
```

`gic_init` is one-time — autodetects version, maps MMIO, brings up distributor + this CPU's redistributor + CPU interface. Must run after `dtb_init` and `exception_init` (so unexpected IRQs during bring-up route through the vector table cleanly). Returns `true` on success; on detection failure or v2 detection, it `extinction`s.

`gic_attach(intid, handler, arg)` registers `handler` for `intid`. The handler runs in IRQ context (PSTATE.I masked) on the existing SP_EL1; it must complete in bounded time and must not block. Replacing an existing handler is allowed (no return slot for the previous handler at v1.0; if the caller needs to chain, they cache the previous from elsewhere).

`gic_enable_irq` / `gic_disable_irq` flip the source-side enable bit. SGI/PPI (intid < 32) hits the redistributor's banked `GICR_ISENABLER0`; SPI (intid >= 32) hits the distributor's `GICD_ISENABLER<n>`.

`gic_acknowledge` reads `ICC_IAR1_EL1` and returns the INTID portion (low 24 bits). `gic_eoi(intid)` writes `ICC_EOIR1_EL1` (one-step priority drop + deactivate, since `ICC_CTLR_EL1.EOImode = 0`). For spurious INTID (1023) the caller MUST NOT EOI per GICv3 SR conventions; `exception_irq_curr_el` handles this.

`gic_dispatch(intid)` is called by `exception_irq_curr_el` after `gic_acknowledge`; it looks up the handler and invokes it. If no handler is registered or `intid` is out of range, it extincts with the diagnostic.

---

## Implementation

### Boot sequence

```
dtb_init                 (boot_main step 1)
exception_init           (boot_main step 4)
gic_init:                (boot_main step 5)
   detect_version_from_dtb
   if v2: extinction (deferred path)
   if v3:
      dist_base, dist_size  = dtb_get_compat_reg_n("arm,gic-v3", 0)
      redist_base, redist_size = dtb_get_compat_reg_n("arm,gic-v3", 1)
      mmu_map_device(dist_base, dist_size)
      mmu_map_device(redist_base, redist_size)
      dist_init                   (one-time, CPU 0)
      redist_init_cpu0            (per-CPU, banked SGI/PPI)
      cpu_iface_init              (per-CPU, system regs)
   return true
timer_init(1000)
gic_attach(INTID 30, timer_irq_handler, NULL)
gic_enable_irq(INTID 30)
msr daifclr, #2          (PSTATE.I = 0)
```

After this sequence the timer fires at 1000 Hz and `exception_irq_curr_el` dispatches each interrupt to `timer_irq_handler`.

### Distributor init (`dist_init`, one-time)

```c
mmio_w32(dist, GICD_CTLR, 0);                          // disable
typer = mmio_r32(dist, GICD_TYPER);
max_intid = ((typer & 0x1f) + 1) * 32 - 1;             // ITLinesNumber

// SPIs (intid >= 32): clear pending, disable, group=1NS, prio=0xa0,
// edge/level=level, route to CPU 0.
for n in [1 .. max_intid/32]:
    GICD_ICENABLER(n) = 0xFFFFFFFF
    GICD_ICPENDR(n)   = 0xFFFFFFFF
    GICD_IGROUPR(n)   = 0xFFFFFFFF      // group 1 NS
    GICD_ICFGR(n*2),(n*2+1) = 0          // level

for n in [32 .. max_intid]:
    GICD_IPRIORITYR(n>>2) byte n%4 = 0xa0
    GICD_IROUTER(n) = 0                  // affinity 0.0.0.0 (CPU 0)

mmio_w32(dist, GICD_CTLR, ARE_NS | EnableGrp1NS);      // enable + ARE
```

### Redistributor init (`redist_init_cpu0`)

```c
// Wake.
waker = mmio_r32(redist, GICR_WAKER) & ~ProcessorSleep
mmio_w32(redist, GICR_WAKER, waker)
while (mmio_r32(redist, GICR_WAKER) & ChildrenAsleep) {}

// SGI/PPI bank: disable, group=1NS, priority=0xa0, level.
mmio_w32(redist, GICR_ICENABLER0, 0xFFFFFFFF)
mmio_w32(redist, GICR_IGROUPR0,   0xFFFFFFFF)
for n in [0 .. 31]:
    GICR_IPRIORITYR(n>>2) byte n%4 = 0xa0
mmio_w32(redist, GICR_ICFGR1, 0)        // PPI level
```

### CPU interface init (`cpu_iface_init`, system regs)

```c
ICC_SRE_EL1   |= 1            ; enable system-register interface
ICC_PMR_EL1    = 0xff         ; lowest priority mask (admit all)
ICC_BPR1_EL1   = 0            ; full priority comparison
ICC_CTLR_EL1   = 0            ; EOImode = 0 (one-step EOI)
ICC_IGRPEN1_EL1 = 1           ; enable group 1 interrupts
```

ISBs surround the SRE write (it changes the architectural state from MMIO-CPU-interface mode to system-register mode) and the IGRPEN1 write (subsequent code needs the new value visible).

### Spurious INTID handling

`gic_acknowledge` returns the INTID portion of `ICC_IAR1_EL1`. If the value is `1023` (spurious), `exception_irq_curr_el` returns without dispatching and without EOIing — per ARM IHI 0069 §3.7, EOIing a spurious read corrupts the running-priority stack. The IRQ vector slot's `.Lexception_return` then `eret`s back to whatever was running, and the IRQ line is re-asserted (or not — spurious typically means "the IRQ was taken away between assertion and acknowledge").

---

## Data structures

### Handler dispatch table

```c
struct gic_irq_slot {
    gic_irq_handler_t handler;
    void *arg;
};
static struct gic_irq_slot g_handlers[GIC_NUM_INTIDS];
```

1020 entries × 16 bytes = 16320 bytes BSS. Indexed directly by INTID (no hashing, no compaction). Cleared at boot by the BSS clear; `gic_attach` writes; `gic_dispatch` reads.

A future optimization: split into per-CPU SGI/PPI slots (32 INTIDs × NCPUS) plus a shared SPI slot table (988 INTIDs). At v1.0 with NCPUS=1 the gain is zero; revisit at SMP.

### Cached MMIO bases + version

```c
static gic_version_t g_version;
static u64 g_dist_base, g_redist_base;
static u32 g_max_intid;
```

Populated by `gic_init`; read by every public function. No locking needed at v1.0 (single-threaded boot path; per-CPU bring-up happens before SMP secondaries start).

---

## State machines

The GIC itself has hardware state machines (ack-pending-active-deactivated per INTID; affinity routing; priority drop) but the driver doesn't model them — it passes through to the hardware via MMIO + system registers and trusts the IHI 0069 architectural guarantees.

The audit-relevant state machine is the IRQ flow:

```
IDLE
  → (HW: external IRQ asserts source line, distributor forwards to redist, CPU interface signals IRQ to PE)
PE delivers IRQ → vectors.S 0x280 entry
  → KERNEL_ENTRY (save ctx)
  → exception_irq_curr_el(ctx)
      → gic_acknowledge: read ICC_IAR1_EL1 → intid
      → if intid == 1023: return (spurious, no dispatch, no EOI)
      → gic_dispatch(intid):
          → handler(intid, arg)
      → gic_eoi(intid): write ICC_EOIR1_EL1 = intid
  → b .Lexception_return → KERNEL_EXIT → eret
IDLE
```

Single-threaded at P1-G. No nested IRQs (PSTATE.I is set on entry; we don't clear it). Phase 2 may opt into nested IRQs for low-priority sources during a high-priority handler, but v1.0 doesn't.

---

## Spec cross-reference

No formal spec at P1-G. ARCH §28 invariants potentially implicated by future work:

| Invariant | When relevant | Spec |
|---|---|---|
| I-9 (no wakeup lost between wait check and sleep) | Phase 2 wait/wake on IRQ-driven completions | `scheduler.tla`, `poll.tla`, `futex.tla` |
| I-18 (IPIs in send order) | Phase 2 SMP scheduler IPIs (SGIs) | `scheduler.tla` |

Neither invariant is live at P1-G (no scheduler, no SMP, no wait queues). The IRQ entry path itself is invariant-free at v1.0 — it's a straightforward ack/dispatch/EOI flow.

---

## Tests

Two leaf-API tests at landing (in-kernel, `kernel/test/`):

| Test | What | Coverage |
|---|---|---|
| `gic.init_smoke` (`test_gic.c`) | `gic_version() == V3`, `gic_dist_base() != 0`, `gic_redist_base() != 0` | confirms autodetect found v3 + bases populated |
| `timer.tick_increments` (`test_timer.c`) | tick count advances after `timer_busy_wait_ticks(2)` | end-to-end IRQ delivery (dist + redist + CPU iface + vector slot + handler + EOI) |

The `timer.tick_increments` test is the canonical smoke for the entire IRQ infrastructure — if any step is broken, it wedges or fails.

What's NOT tested:

- Non-architectural priorities (we use `0xa0` for everything; no priority hierarchy yet).
- IRQ source enable/disable race (no concurrent users at v1.0).
- ICFGR edge-vs-level (UART IRQs are level; future virtio is per-spec).
- Spurious INTID handling (would need fault injection).
- v2 path (deferred — see [Caveats](#caveats)).

P1-I will add a sanitizer matrix run that exercises the IRQ path under ASan / UBSan.

---

## Error paths

| Condition | Behavior |
|---|---|
| DTB has no GIC compat string | `gic_init` extinctions: `"gic_init: no GIC compat in DTB ..."` |
| DTB advertises v2 (`arm,cortex-a15-gic` / `arm,gic-400`) | `gic_init` extinctions: `"gic_init: GICv2 detected; v2 path deferred ..."` |
| DTB advertises v3 but `reg[0]` (distributor) absent | `gic_init` extinctions: `"gic_init: DTB arm,gic-v3 has no reg[0] (distributor)"` |
| DTB advertises v3 but `reg[1]` (redistributor) absent | `gic_init` extinctions: `"gic_init: DTB arm,gic-v3 has no reg[1] (redistributor)"` |
| MMIO region above 4 GiB (Pi 5) | `gic_init` extinctions: `"gic_init: mmu_map_device(...) failed (>4 GiB?)"` (deferred — TTBR0 extension at Pi 5 port) |
| `gic_attach(intid, ...)` with `intid >= GIC_NUM_INTIDS` | returns `false`; caller is expected to check |
| `gic_enable_irq(intid)` / `gic_disable_irq(intid)` with `intid >= GIC_NUM_INTIDS` | returns `false` |
| `gic_dispatch(intid)` with `intid >= GIC_NUM_INTIDS` | extinctions: `"gic_dispatch: INTID out of range"` (the IAR was bogus — hardware bug or spec violation) |
| `gic_dispatch(intid)` with no handler attached | extinctions: `"gic_dispatch: no handler for INTID"` |
| `gic_eoi(GIC_INTID_SPURIOUS)` | no-op (per IHI 0069 §3.7 — spurious must not EOI) |

---

## Performance characteristics

| Metric | Estimated | Notes |
|---|---|---|
| `gic_init` total cost | ~50–100 µs | dist init walks all SPI groups; on QEMU virt with `max_intid = 159` (5 groups) it's a few hundred MMIO writes |
| `gic_acknowledge` cost | ~10–20 cycles | one MRS via system register + dsb |
| `gic_eoi` cost | ~10 cycles | one MSR + isb |
| `gic_dispatch` cost (excluding handler) | ~5 cycles | array index + indirect call |
| Total IRQ service overhead (excluding handler) | ~50 cycles | ack + dispatch + EOI + KERNEL_ENTRY + KERNEL_EXIT |
| BSS footprint (handler table) | 16 320 bytes | 1020 × 16 |
| Code size (gic.c) | ~3 KB stripped | reasonable for a driver |

VISION §4.5's IRQ-to-userspace handler p99 budget is < 5 µs. Kernel-internal IRQ overhead (P1-G's measure) is comfortably under that — the userspace path adds context switches at Phase 5.

---

## Status

**Implemented at P1-G**:

- Version detection via DTB (`arm,gic-v3` → V3; `arm,cortex-a15-gic` / `arm,gic-400` → V2 → extinction).
- GICv3 distributor init (one-time, CPU 0): SPI groups, priorities, ICFGR, IROUTER, GICD_CTLR enable.
- GICv3 redistributor init (per-CPU, CPU 0 at v1.0): wake, SGI/PPI bank config.
- CPU interface init (per-CPU): SRE / PMR / BPR / CTLR / IGRPEN1.
- IRQ enable/disable for SGI/PPI (banked) + SPI (non-banked).
- `gic_acknowledge` / `gic_eoi` via system registers (ICC_IAR1_EL1, ICC_EOIR1_EL1).
- Handler dispatch table (1020 INTIDs × {handler, arg}).
- `gic_attach` registration.
- `mmu_map_device` integration: GIC distributor + redistributor mapped Device-nGnRnE in TTBR0 from DTB.
- Tests: `gic.init_smoke` (autodetect + base addresses).

**Not yet implemented**:

- GICv2 path (distributor + MMIO CPU interface + GICC ack/EOI). Deferred until there's a Pi 4 / similar testbed.
- SMP redistributor walk for secondary CPUs. Phase 2 with thread machinery.
- SGI (inter-processor interrupt) generation via ICC_SGI1R_EL1. Phase 2 scheduler.
- Per-IRQ priority hierarchy. Post-v1.0 hardening.
- ITS (Interrupt Translation Service) for MSI. Phase 3 with VirtIO over PCIe (we don't currently expose virtio-pci; virtio-mmio uses regular SPIs).
- Pi 5 support: GIC distributor at PA > 4 GiB; needs TTBR0 extension or high-VA mapping.
- IRQ-driven UART. Mechanism is in place; routing through `gic_attach` is post-v1.0.

**Landed**: P1-G at commit `39eafb4`.

---

## Caveats

### v2 detection extincts; doesn't run

ARCH §12 commits to v2/v3 autodetect. The autodetect mechanism IS in place — `gic_init` probes `arm,gic-v3`, then `arm,cortex-a15-gic` / `arm,gic-400`. v2 is NOT implemented at v1.0 because:

1. No test target. QEMU virt with `tools/run-vm.sh` defaults to `gic-version=3`. Pi 5 ships GICv3.
2. v2 is a different ABI: distributor + MMIO CPU interface (vs system-register on v3); GICC_IAR / GICC_EOIR (vs ICC_IAR1_EL1 / ICC_EOIR1_EL1); different end-of-interrupt semantics.
3. Implementing untested code risks silent regression. The v2 path lands cleanly when there's a Pi 4 (or older board) testbed driving it.

The autodetect commitment is preserved by extinguishing cleanly with a deferred-to-future-chunk diagnostic, rather than running untested code on misconfigured hardware. The user-facing experience is "this kernel doesn't run on v2-only hardware yet" — which is honest. v3-on-v2 silent run would be much worse.

### Pi 5 GIC at PA > 4 GiB needs TTBR0 extension

Pi 5's GICv3 distributor sits at PA ~0x107FFF8000 — above 4 GiB. Our TTBR0 identity covers `[0, 4 GiB)` only; `mmu_map_device` rejects anything past that. Pi 5 port plan: extend TTBR0 to cover the GIC PA range, or map the GIC into TTBR1 high VA. Lands when there's a Pi 5 testbed.

### Handler context: SP_EL1, no nesting

P1-G's IRQ handler runs on the existing SP_EL1 (boot stack at v1.0). Same recursive-fault hazard as the sync handler — a stack overflow inside a handler wedges QEMU. Mitigation: per-CPU exception stack at Phase 2.

PSTATE.I is set on IRQ entry; we don't clear it. Nested IRQs (high-priority preempting low-priority handler) are a Phase 2+ feature.

### `gic_attach` does not return the previous handler

Attaching twice silently overwrites. v1.0 callers register exactly once at boot; misuse extincts at the first IRQ via the dispatch path's "no handler / unexpected handler" diagnostics. If a future caller needs to chain (multiplexed IRQ), they cache the previous attach themselves before overwriting.

### Default priority is 0xa0 for everything

Source-side priority register holds 8 bits per INTID; we set every active INTID to `0xa0`. Phase 2 may want a hierarchy (timer above device IRQs above SGIs); the API supports it (just write a different byte to `IPRIORITYR` / `GICR_IPRIORITYR`), but v1.0 doesn't use it.

### `dsb sy` before `mrs ICC_IAR1_EL1`

We add `dsb sy` before reading IAR per ARM IHI 0069 §3.7 to ensure prior gic_eoi writes have observed before this acknowledge. On a non-pipelined CPU this is overkill, but on speculative cores the ordering matters for re-entry chains. Cost is one DSB per IRQ (~10 cycles); not load-bearing for performance at v1.0.

### Group 1 NS only

We use group 1 non-secure exclusively. Group 0 (secure) and group 1 secure are not used; `IGROUPR` writes the INTID to non-secure group 1. This is correct for EL1-non-secure operation. Pi 5's secure boot may require revisiting (we may need group-0 IRQs from the secure firmware).

---

## See also

- `docs/reference/01-boot.md` — entry sequence (`gic_init` slot in `boot_main`).
- `docs/reference/03-mmu.md` — `mmu_map_device` API used to map GIC regions Device-nGnRnE.
- `docs/reference/02-dtb.md` — `dtb_get_compat_reg_n`, `dtb_has_compat` used for autodetect + region discovery.
- `docs/reference/08-exception.md` — `exception_irq_curr_el` calls into `gic_dispatch`; `.Lexception_return` trampoline.
- `docs/reference/11-timer.md` — first IRQ source wired through (PPI 14 → INTID 30).
- `docs/ARCHITECTURE.md §12.3` — design intent for the GIC autodetect commitment.
- ARM IHI 0069 (current rev H.b) — GICv3 + GICv4 architecture specification.
- Linux `drivers/irqchip/irq-gic-v3.c` — reference implementation; we reference its register map but not its code.
