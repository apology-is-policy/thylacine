# 11 — ARM generic timer (as-built reference)

The kernel's first IRQ source. ARMv8 generic timer at EL1 non-secure physical, programmed at 1000 Hz. Each fire increments a global tick counter; the IRQ is acknowledged via the GIC and re-armed by writing `CNTP_TVAL_EL0` with the cached reload value.

P1-G deliverable. The tick counter is visible in the boot banner ("ticks: N") to confirm IRQ delivery is live.

Scope: `arch/arm64/timer.{h,c}`, `kernel/main.c`'s `timer_init` slot in `boot_main`, banner `ticks:` line. Cross-cuts the GIC driver (registration via `gic_attach`) and the IRQ vector path (dispatch via `gic_dispatch`).

Reference: `ARCHITECTURE.md §12.3` (interrupt handling), ARMv8-A ARM section D11 (Generic Timer).

---

## Purpose

Timer ticks are the kernel's heartbeat. At P1-G the tick is a passive observation — the boot path waits 5 ticks before printing the count, demonstrating the IRQ delivery path is alive. Phase 2 adds a real scheduler tick driving EEVDF deadline updates; Phase 3 adds high-resolution timeouts via `CNTP_CVAL_EL0` deadlines for I/O completions.

The ARMv8 generic timer is a system-register peripheral (no MMIO). Each PE has a banked physical + virtual timer pair; we use the EL1 non-secure physical timer. The timer counts down a 32-bit signed `CNTP_TVAL_EL0` value (set by software); when it reaches zero the timer fires an IRQ on PPI 14 (architecturally fixed; INTID 30 in GICv3 numbering). Re-arming is one MSR write.

---

## Public API

`arch/arm64/timer.h`:

```c
#define TIMER_INTID_EL1_PHYS_NS  GIC_PPI_TO_INTID(14)   /* = 30 */

bool timer_init(u32 hz);             // one-time bring-up; hz typically 1000
void timer_irq_handler(u32 intid, void *arg);

u64  timer_get_ticks(void);          // monotonic; 0 before timer_init
u64  timer_get_counter(void);        // CNTPCT_EL0 (architectural counter)
u32  timer_get_freq(void);           // CNTFRQ_EL0 cached at init

void timer_busy_wait_ticks(u64 n);   // WFI loop until N ticks elapsed
```

`timer_init(hz)` is one-time: reads `CNTFRQ_EL0`, computes `reload = freq / hz`, programs `CNTP_TVAL_EL0 = reload`, enables the timer (`CNTP_CTL_EL0 = ENABLE | !IMASK`). Returns `false` if `hz == 0` or `hz > freq`. Caller follows with `gic_attach(TIMER_INTID_EL1_PHYS_NS, timer_irq_handler, NULL)` and `gic_enable_irq(TIMER_INTID_EL1_PHYS_NS)` to route the IRQ through.

`timer_irq_handler(intid, arg)` is registered with the GIC. Increments `g_ticks`; reloads `CNTP_TVAL_EL0` for the next fire. The handler signature matches `gic_irq_handler_t`.

`timer_get_ticks()` is monotonic and lock-free at v1.0 (single CPU; the increment-and-reload is non-preemptable since IRQs are masked at PSTATE during the handler). Phase 2 adds atomic semantics for SMP.

`timer_get_counter()` reads `CNTPCT_EL0` directly — useful for fine-grained delta measurements (sub-millisecond) where the 1 ms tick granularity is too coarse.

`timer_busy_wait_ticks(n)` is a WFI-based busy wait. The CPU sleeps until the next IRQ arrives (typically the next timer tick); the loop exits when `g_ticks` has advanced by at least `n`.

---

## Implementation

### Boot sequence

```
gic_init                   (boot_main step 5a)
timer_init(1000):          (boot_main step 5b)
   freq = read(CNTFRQ_EL0)
   if freq == 0: extinction("CNTFRQ_EL0 = 0")
   if hz > freq: return false
   reload = freq / hz
   ticks  = 0
   write(CNTP_TVAL_EL0, reload)
   write(CNTP_CTL_EL0, ENABLE)         ; IMASK = 0
gic_attach(INTID 30, timer_irq_handler, NULL)
gic_enable_irq(INTID 30)
msr daifclr, #2            ; PSTATE.I = 0
```

After `daifclr`, the next time `CNTP_TVAL_EL0` reaches 0 the GIC delivers INTID 30 → the IRQ vector slot dispatches → `timer_irq_handler` increments `g_ticks` and reloads.

### IRQ handler (`timer_irq_handler`)

```c
void timer_irq_handler(u32 intid, void *arg) {
    (void)intid;
    (void)arg;
    g_ticks++;
    write_cntp_tval_el0(g_reload);
    // CNTP_CTL stays at ENABLE
}
```

The IRQ does not auto-clear; writing `CNTP_TVAL_EL0` re-arms the decrementer (HW recomputes "fires when value <= 0" relative to now). `CNTP_CTL_EL0.ISTATUS` would read 1 while the IRQ is asserted; we don't poll it because the GIC's ack/EOI path is the canonical signal.

The timer tick rate is set by `g_reload`. At QEMU virt's `CNTFRQ = 62.5 MHz` (sometimes 1 GHz on `-cpu max` — the boot banner reports the actual value), `reload = 62500` for 1 ms ticks. `CNTP_TVAL_EL0` is 32-bit signed; reload values up to ~2 × 10⁹ fit. Above that (sub-1 Hz tick), use `CNTP_CVAL_EL0` (64-bit absolute compare).

### EL2 → EL1 timer access setup

At EL1 we can read `CNTPCT_EL0` and program `CNTP_*` only if `CNTHCTL_EL2.{EL1PCEN, EL1PCTEN}` are set. `arch/arm64/start.S`'s EL2 → EL1 drop sequence sets both bits to 1 before `eret`ing to EL1 — see `01-boot.md` §EL2→EL1 drop. On QEMU virt with direct EL1 entry, the firmware has already configured these. The result is that `mrs cntpct_el0` doesn't trap.

### CNTFRQ_EL0 source

`CNTFRQ_EL0` reports the system-counter frequency. On QEMU virt under HVF, this is typically 62.5 MHz (24 MHz on some configurations); on hardware (Pi 5) it's 50 MHz or similar. We trust the firmware-provided value; if it's zero (`mrs` returns 0), the timer can't be programmed and `timer_init` extincts.

### Spin-wait via WFI

`timer_busy_wait_ticks(n)` loops on `g_ticks < target` with WFI inside:

```c
u64 target = g_ticks + n;
while (g_ticks < target) {
    __asm__ __volatile__("wfi" ::: "memory");
}
```

WFI suspends the CPU until an interrupt arrives. The next timer tick (or any other IRQ) wakes it; the handler increments `g_ticks`; the loop checks and either exits or WFIs again. This is the correct idiom for "wait until the kernel has observed N ticks" — it doesn't busy-burn the CPU.

---

## Data structures

```c
static u64 g_freq;       // CNTFRQ_EL0, cached at init
static u64 g_reload;     // freq / hz
static u64 g_ticks;      // incremented per IRQ
```

Three u64s. No arrays. No locks at v1.0 (single CPU; the IRQ handler is the only mutator of `g_ticks` and `_get_ticks` is the only reader from non-IRQ context).

Phase 2 adds atomic load-acquire on `g_ticks` (multiple CPUs read; only the IRQ-holding CPU writes — the standard single-writer-multi-reader pattern). The increment in the handler can stay non-atomic on SMP because each CPU's banked timer fires its handler on that CPU; `g_ticks` becomes per-CPU and a sum-of-tickers helper aggregates.

---

## State machines

The hardware timer has a tiny state machine:

```
DISABLED (CNTP_CTL_EL0.ENABLE = 0)
  → write CTL.ENABLE = 1, CTL.IMASK = 0
ARMED (decrementing CNTP_TVAL_EL0)
  → CNTP_TVAL_EL0 reaches 0
FIRING (ISTATUS = 1; IRQ asserted to GIC)
  → write CNTP_TVAL_EL0 = reload (re-arms)
ARMED
  ...
```

The driver doesn't track this state explicitly — the hardware does. We just program `CNTP_TVAL_EL0` and trust the architectural guarantee that ENABLE + cleared IMASK → IRQ fires when TVAL hits zero.

---

## Spec cross-reference

No formal spec at P1-G. ARCH §28 invariants:

| Invariant | When relevant | Spec |
|---|---|---|
| I-8 (every runnable thread eventually runs) | Phase 2 EEVDF tick | `scheduler.tla` |
| I-17 (EEVDF latency bound) | Phase 2 deadline math | `scheduler.tla` |

At P1-G the timer is a heartbeat with no scheduling consequences; spec gating is Phase 2's responsibility.

---

## Tests

One leaf-API test at landing (`kernel/test/test_timer.c`):

| Test | What | Coverage |
|---|---|---|
| `timer.tick_increments` | `timer_get_freq() > 0`; `g_ticks` advances ≥ 2 after `timer_busy_wait_ticks(2)` | end-to-end IRQ delivery + handler invocation + reload re-arm |

The boot banner's `ticks: N (kernel breathing)` line — printed after `timer_busy_wait_ticks(5)` — is an integration smoke that runs every boot. If the timer is broken, `tools/test.sh` times out at 10s rather than emitting the boot-OK line.

What's NOT tested:

- `CNTFRQ_EL0 = 0` extinction path (would need a fault-injection build).
- Out-of-range hz (covered by code review; `if (hz == 0) return false`; `if (hz > freq) return false` are both reachable for malformed callers).
- `timer_get_counter` matching `mrs cntpct_el0` (trivially true; tested implicitly by the handler reload path).
- Reload precision across multiple ticks (drift over 10000+ ticks). Post-v1.0.
- Timer interaction with the EEVDF tick (Phase 2's responsibility).

---

## Error paths

| Condition | Behavior |
|---|---|
| `timer_init(0)` | returns `false` |
| `timer_init(hz > CNTFRQ)` | returns `false` |
| `timer_init` with `CNTFRQ_EL0 = 0` | extinctions: `"timer_init: CNTFRQ_EL0 = 0"` (firmware bug) |
| Lost IRQ (ENABLE flag cleared by something else) | tick counter stops advancing; `timer_busy_wait_ticks` wedges. Mitigation: nothing should ever clear ENABLE; if a regression does, the boot-time tick observation surfaces it as a hung boot. |
| Reload value wraps (hz × reload > 2³¹) | We use 32-bit `CNTP_TVAL_EL0`; with hz ≥ 1 and freq ≤ 2 GHz, reload ≤ 2 × 10⁹ which is under the signed-32 limit. `timer_init` doesn't validate this; if a future caller passes hz < 1 (e.g., 0.1 Hz in a sub-Hz config), the math overflows. v1.0 hz is fixed at 1000; v1.1 will validate. |

---

## Performance characteristics

| Metric | Estimated | Notes |
|---|---|---|
| `timer_init` cost | ~50 cycles | one MRS + one MSR + arithmetic |
| `timer_irq_handler` cost | ~10 cycles | one increment + one MSR |
| Tick → handler overhead (excluding handler body) | ~50 cycles | KERNEL_ENTRY + ack + dispatch + EOI + KERNEL_EXIT |
| Tick rate at 1000 Hz | 1 fire / ms | 50 cycles / fire = 0.05 µs / tick = 50 µs / s = 0.005% CPU |
| BSS footprint | 24 bytes | three u64s |
| Code size (timer.c) | ~1 KB stripped | trivial |

CPU overhead at 1000 Hz is negligible. Phase 2's scheduler tick adds a few hundred cycles of bookkeeping per fire — still well under 1% CPU.

---

## Status

**Implemented at P1-G**:

- `timer_init(hz)` — cache CNTFRQ, compute reload, program CNTP_TVAL_EL0 + CNTP_CTL_EL0.
- `timer_irq_handler(intid, arg)` — increment ticks, reload TVAL.
- `timer_get_ticks()`, `timer_get_counter()`, `timer_get_freq()` — observation API.
- `timer_busy_wait_ticks(n)` — WFI-based wait for N ticks.
- IRQ wired through GIC: `gic_attach(TIMER_INTID_EL1_PHYS_NS, timer_irq_handler, NULL)` + `gic_enable_irq`.
- Banner: `timer: <freq_kHz> kHz freq, 1000 Hz tick (PPI 14 / INTID 30)` + `ticks: N (kernel breathing)`.
- Test: `timer.tick_increments` (end-to-end IRQ delivery smoke).

**Not yet implemented**:

- One-shot timer (`CNTP_CVAL_EL0` deadline). Phase 2 with high-resolution timeouts.
- Per-CPU timer state for SMP. Phase 2 with thread machinery.
- TSC-deadline alternative path for x86-64. Out of scope at v1.0 (ARM64-only).
- Frequency calibration / drift compensation. Post-v1.0.
- Suspend-resume timer state save. Way post-v1.0 (suspend support arrives at v1.1 / v2.0).

**Landed**: P1-G at commit `*(pending)*`.

---

## Caveats

### `CNTFRQ_EL0` is read once at init

The frequency is cached at `timer_init` time. If firmware changes `CNTFRQ_EL0` later (which it doesn't on any platform we support), the timer would tick at the wrong rate without our knowing. Mitigation: re-read in the IRQ handler on every fire. We don't, because the cost (~10 cycles per tick) compounds and the scenario is theoretical.

### EL2 timer access is set up by `start.S`

Without `CNTHCTL_EL2.{EL1PCEN, EL1PCTEN} = 1` set during the EL2 → EL1 drop, EL1 access to `CNTPCT_EL0` and `CNTP_*` traps. QEMU virt firmware sets these for direct EL1 entry; bare-metal Pi 5 boots us at EL2 and we set them in `_real_start`. If a future port enters at EL3 or via a different bootloader, this needs revisiting.

### Banked timer per PE

The EL1 non-secure physical timer is banked per PE (CPU). At P1-G with NCPUS=1, there's only one timer; Phase 2 SMP introduces per-CPU `g_ticks` counters. Until then, the global `g_ticks` is correct because only CPU 0 receives ticks.

### No tickless / NOHZ mode

The timer fires unconditionally at 1 kHz. A modern Linux kernel suppresses the periodic tick when no thread needs it; we don't. Cost is ~50 µs/s per CPU = 0.005% — acceptable at v1.0. NOHZ is a post-v1.0 power optimization (matters for battery-powered hardware; QEMU virt doesn't care).

### `timer_busy_wait_ticks` doesn't yield

It WFIs until the next IRQ arrives — fine on UP. On SMP it would block the CPU from work-stealing other threads. Phase 2's `msleep` is the proper API for "wait N ms"; `timer_busy_wait_ticks` survives at v1.0 only as a boot-path observation tool.

### No high-resolution counter exported

`timer_get_counter` returns `CNTPCT_EL0` directly. Conversion to nanoseconds (×1e9 / freq) isn't provided — callers do it themselves. Phase 2 will add a `timer_ns()` wrapper.

### Reload race on IRQ entry

The handler's `g_ticks++` happens before `write_cntp_tval_el0(g_reload)`. If a subsequent IRQ source has higher priority and pre-empts (post-v1.0 nested IRQs), the timer could run for an unbounded gap before we re-arm. v1.0 IRQs are non-nested, so the gap is bounded by handler runtime (~10 cycles). Phase 2 may tighten by ordering reload first.

---

## See also

- `docs/reference/01-boot.md` — entry sequence (`timer_init` slot in `boot_main`).
- `docs/reference/10-gic.md` — interrupt controller; timer IRQ routes through this.
- `docs/reference/08-exception.md` — `exception_irq_curr_el` dispatches to `timer_irq_handler` via `gic_dispatch`.
- `docs/ARCHITECTURE.md §12.3` — design intent for IRQ + timer.
- ARMv8-A Architecture Reference Manual (DDI 0487) — section D11 (Generic Timer).
