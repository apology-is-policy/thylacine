# 11 — ARM generic timer (as-built reference)

The kernel's first IRQ source. ARMv8 generic **virtual** timer (Lazarus W2; physical at P1-G), programmed at 1000 Hz. Each fire increments a global tick counter; the IRQ is acknowledged via the GIC and re-armed by writing `CNTV_TVAL_EL0` with the cached reload value.

> **Lazarus W2 (virtual-timer switch, task #889).** The driver now uses the **virtual** timer, not the EL1 physical timer: `CNTV_CTL_EL0` / `CNTV_TVAL_EL0` / `CNTVCT_EL0` (was `CNTP_*` / `CNTPCT_EL0`) and **INTID 27 (virtual PPI 11)** (was INTID 30 / PPI 14); EL0 counter access adds `CNTKCTL_EL1.EL0VCTEN`. Why: under a hypervisor (HVF on Apple Silicon, the Lazarus fast-dev target) the physical timer is reserved, and an EL1-guest `CNTP_TVAL_EL0` write reflects as an `EC=0` undefined-instruction exception. The virtual timer is the EL1-guest timer on every substrate (TCG, HVF, and direct-EL1 bare-metal where `CNTVOFF=0` makes the virtual counter equal the physical) -- one timebase everywhere. **The mechanism described below is identical with the `V`-variant registers** (some interior pseudocode still names the `CNTP_*` forms -- read them as their `CNTV_*` counterparts; the headline facts here are authoritative). The physical *counter* `CNTPCT_EL0` remains readable at EL1 for entropy (kaslr/canary); only the physical *timer control* is hypervisor-reserved. `PORTABILITY.md §5`.

P1-G deliverable (physical); virtual-timer switch at Lazarus W2. The tick counter is visible in the boot banner ("ticks: N") to confirm IRQ delivery is live.

Scope: `arch/arm64/timer.{h,c}`, `kernel/main.c`'s `timer_init` slot in `boot_main`, banner `ticks:` line. Cross-cuts the GIC driver (registration via `gic_attach`) and the IRQ vector path (dispatch via `gic_dispatch`).

Reference: `ARCHITECTURE.md §12.3` (interrupt handling), ARMv8-A ARM section D11 (Generic Timer).

---

## Purpose

Timer ticks are the kernel's heartbeat. At P1-G the tick is a passive observation — the boot path waits 5 ticks before printing the count, demonstrating the IRQ delivery path is alive. Phase 2 adds a real scheduler tick driving EEVDF deadline updates; Phase 3 adds high-resolution timeouts via `CNTP_CVAL_EL0` deadlines for I/O completions.

The ARMv8 generic timer is a system-register peripheral (no MMIO). Each PE has a banked physical + virtual timer pair; we use the **virtual** timer (Lazarus W2). The timer counts down a 32-bit signed `CNTV_TVAL_EL0` value (set by software); when it reaches zero the timer fires an IRQ on PPI 11 (architecturally fixed; INTID 27 in GIC numbering). Re-arming is one MSR write.

---

## Public API

`arch/arm64/timer.h`:

```c
#define TIMER_INTID_EL1_PHYS_NS  GIC_PPI_TO_INTID(14)   /* = 30 */

bool timer_init(u32 hz);             // one-time bring-up (boot CPU); hz typically 1000
void timer_arm_this_cpu(void);       // arm THIS CPU's banked timer (per-CPU; #810)
void timer_irq_handler(u32 intid, void *arg);

u64  timer_get_ticks(void);          // monotonic; 0 before timer_init
u64  timer_get_counter(void);        // CNTPCT_EL0 (architectural counter)
u32  timer_get_freq(void);           // CNTFRQ_EL0 cached at init
u64  timer_now_ns(void);             // monotonic time in ns (P5-tsleep)
u64  timer_ns_to_counter(u64 ns);    // absolute ns → CNTPCT value (P5-tsleep)

void timer_busy_wait_ticks(u64 n);   // WFI loop until N ticks elapsed
```

`timer_init(hz)` is one-time (boot CPU): reads `CNTFRQ_EL0`, computes and caches `reload = freq / hz`, then calls `timer_arm_this_cpu()` to program `CNTP_TVAL_EL0 = reload` + enable (`CNTP_CTL_EL0 = ENABLE | !IMASK`). Returns `false` if `hz == 0` or `hz > freq`. Caller follows with `gic_attach(TIMER_INTID_EL1_PHYS_NS, timer_irq_handler, NULL)` and `gic_enable_irq(TIMER_INTID_EL1_PHYS_NS)` to route the IRQ through.

`timer_arm_this_cpu()` arms the **current** CPU's timer from the cached `g_reload` (it does NOT recompute freq/reload or reset `g_ticks`). `CNTP_TVAL_EL0`/`CNTP_CTL_EL0` are **per-CPU banked**, so each CPU must arm its own to receive the preemptive scheduler tick. The boot CPU arms via `timer_init`; each secondary arms via this function from `per_cpu_main`'s idle loop, gated on `smp_enable_secondary_preemption()` (see below). A secondary with no armed timer never preempts -- a CPU-bound EL0 thread there monopolizes the CPU and starves co-runnable peers (invariants I-8 / I-17; bug #810). The caller must also `gic_enable_irq(TIMER_INTID_EL1_PHYS_NS)` on that CPU (the PPI enable is per-redistributor).

`timer_irq_handler(intid, arg)` is registered with the GIC (one global handler, dispatched on whichever CPU takes the IRQ). It re-arms **this** CPU's banked `CNTP_TVAL_EL0`, then calls `sched_tick()` (per-CPU preemption accounting + the tsleep deadline scan). `g_ticks` is incremented **only on the boot CPU** (`smp_cpu_idx_self() == 0`): it is the single-writer monotonic timebase for `timer_busy_wait_ticks` + diagnostics, and gating it to cpu0 preserves the pre-#810 advance rate (1 kHz from cpu0) without a multi-writer race now that secondaries also tick.

`timer_get_ticks()` is monotonic and lock-free: only the boot CPU writes `g_ticks` (inside the IRQ handler, non-preemptable since DAIF is masked there) and `_get_ticks` reads it. Secondary ticks (post-#810) do not touch `g_ticks`, so the single-writer property holds under SMP without atomics.

`timer_get_counter()` reads `CNTPCT_EL0` directly — useful for fine-grained delta measurements (sub-millisecond) where the 1 ms tick granularity is too coarse.

`timer_now_ns()` (P5-tsleep) converts the architectural counter to nanoseconds — the monotonic timebase for `tsleep` / `poll` / `futex` deadlines; a caller computes a deadline as `timer_now_ns() + timeout_ns`. `timer_ns_to_counter()` is the inverse, mapping an absolute-ns timestamp back to a `CNTPCT_EL0` value so a deadline check can compare raw counter reads without a per-call division. Both use the split (quotient, remainder) form — a flat `value × 1e9` would overflow `u64` within ~5 minutes at a 62.5 MHz counter — and both return 0 before `timer_init`.

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
static u64 g_freq;       // CNTFRQ_EL0, cached at init (write-once before SMP)
static u64 g_reload;     // freq / hz (write-once before SMP; read by every CPU's arm + re-arm)
static u64 g_ticks;      // incremented per IRQ, BOOT CPU ONLY
```

Three u64s. No arrays, no locks. `g_freq` / `g_reload` are written once by `timer_init` on the boot CPU, before any secondary arms its timer (the `smp_init` bring-up barrier + the RELEASE/ACQUIRE on `g_secondary_preempt_enabled` give the happens-before edge), so secondaries read them unsynchronized but always see the final value -- a future writer to `g_reload` (e.g. dynamic tick reprogramming) would need to add ordering.

`g_ticks` stays a **single-writer** counter even under SMP: as of #810 every CPU takes timer IRQs and re-arms its own banked timer, but only the boot CPU (`smp_cpu_idx_self() == 0`) increments `g_ticks`. This deliberately keeps the `timer_busy_wait_ticks` timebase identical to the pre-SMP-preemption behavior (1 kHz from cpu0) and avoids the multi-writer read-modify-write race a per-CPU-summed counter would otherwise require. (The earlier plan to make `g_ticks` per-CPU + aggregate was dropped: the boot-CPU-only increment is simpler and every `g_ticks` consumer runs on the boot path.)

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
- Banner: `timer: <freq_kHz> kHz freq, 1000 Hz tick (virtual timer, PPI 11 / INTID 27)` + `ticks: N (kernel breathing)`.
- Test: `timer.tick_increments` (end-to-end IRQ delivery smoke).

**Not yet implemented**:

- One-shot timer (`CNTP_CVAL_EL0` deadline). Phase 2 with high-resolution timeouts.
- Per-CPU timer state for SMP. Phase 2 with thread machinery.
- TSC-deadline alternative path for x86-64. Out of scope at v1.0 (ARM64-only).
- Frequency calibration / drift compensation. Post-v1.0.
- Suspend-resume timer state save. Way post-v1.0 (suspend support arrives at v1.1 / v2.0).

**Landed**: P1-G at commit `39eafb4`.

---

## Caveats

### `CNTFRQ_EL0` is read once at init

The frequency is cached at `timer_init` time. If firmware changes `CNTFRQ_EL0` later (which it doesn't on any platform we support), the timer would tick at the wrong rate without our knowing. Mitigation: re-read in the IRQ handler on every fire. We don't, because the cost (~10 cycles per tick) compounds and the scenario is theoretical.

### EL2 timer access is set up by `start.S`

Without `CNTHCTL_EL2.{EL1PCEN, EL1PCTEN} = 1` set during the EL2 → EL1 drop, EL1 access to `CNTPCT_EL0` and `CNTP_*` traps. QEMU virt firmware sets these for direct EL1 entry; bare-metal Pi 5 boots us at EL2 and we set them in `_real_start`. If a future port enters at EL3 or via a different bootloader, this needs revisiting.

### Banked timer per PE

The EL1 non-secure physical timer is banked per PE (CPU). At P1-G with NCPUS=1, there's only one timer; Phase 2 SMP introduces per-CPU `g_ticks` counters. Until then, the global `g_ticks` is correct because only CPU 0 receives ticks.

### Tickless idle (NO_HZ_IDLE) — as-built (TI arc, #299)

The timer fires at 1 kHz for any **running** thread (slice accounting), but a **genuinely-idle** CPU stops the periodic tick and arms a **one-shot** instead. This is the as-built reversal of the original "the timer fires unconditionally at 1 kHz; we don't do NOHZ" caveat — the never-stopped tick was measured at **332% HVF host CPU when idle** (per-tick VTIMER exit + emulated-GICv2 MMIO vmexits + a WFI that never parks at the 1 ms period); see `docs/TICKLESS-IDLE.md` for the measurement + design.

- **The primitive** (TI-1, `arch/arm64/timer.c`): `timer_arm_oneshot_cnt(u64 target_cnt)` arms the banked virtual timer as a one-shot at an absolute `CNTVCT` value, via the pure clamp `timer_oneshot_tval(target, now)` → `[TIMER_MIN_RELOAD, TIMER_MAX_RELOAD]` (target≤now → MIN = fire-ASAP; over-horizon → MAX). `TIMER_MIN_RELOAD`/`TIMER_MAX_RELOAD` are the public clamp contract in `timer.h`.
- **The integration** (TI-2, `kernel/sched.c::sched_idle_park`): an idle CPU arms to `min(nearest g_timerwait deadline, now + TICKLESS_IDLE_BACKSTOP_NS=100ms)`. The 1 kHz tick is byte-unchanged for running threads (I-17 / the §8.2 slice model untouched). Work-arrival wakes ride the existing `IPI_RESCHED` (tick-independent). On wake the loop re-arms periodic (`timer_arm_this_cpu`) **before** dispatching placed work and runs `timerwait_tick()` explicitly (the re-arm deasserts the one-shot's pending IRQ). The I-9 no-lost-wake arm-race is register-then-observe (`specs/sched_tickless.tla`).
- **The handler is unchanged**: when the one-shot fires it re-enters `timer_irq_handler`, which re-arms the periodic `g_reload` exactly as for any tick — correct the moment the CPU has runnable work. There is no `timer_disable_this_cpu` (the always-arm-the-backstop design makes it dead code).
- **Cost**: an idle CPU wakes ~10×/s (the backstop) instead of 1000×/s → HVF idle ~0% (re-measured at TI-3). Timekeeping is unaffected (`CLOCK_MONOTONIC`/`REALTIME` ride `CNTVCT`, not the tick; `g_ticks` freezes during cpu0 idle but is busy-wait/diagnostic-only, run from a running context). Full design + invariants: `docs/TICKLESS-IDLE.md` + ARCH §8.6.

### `timer_busy_wait_ticks` doesn't yield

It WFIs until the next IRQ arrives — fine on UP. On SMP it would block the CPU from work-stealing other threads. Phase 2's `msleep` is the proper API for "wait N ms"; `timer_busy_wait_ticks` survives at v1.0 only as a boot-path observation tool.

### Nanosecond conversion

`timer_get_counter` returns `CNTPCT_EL0` directly. P5-tsleep added `timer_now_ns()` (counter → ns) and `timer_ns_to_counter()` (ns → counter) for deadline arithmetic — see the Public API section. Both are overflow-safe (split quotient/remainder form). A flat `counter × 1e9 / freq` is NOT — it overflows `u64` within ~5 minutes at a 62.5 MHz counter; callers must use the provided helpers, not hand-roll the conversion.

### Reload race on IRQ entry

The handler's `g_ticks++` happens before `write_cntp_tval_el0(g_reload)`. If a subsequent IRQ source has higher priority and pre-empts (post-v1.0 nested IRQs), the timer could run for an unbounded gap before we re-arm. v1.0 IRQs are non-nested, so the gap is bounded by handler runtime (~10 cycles). Phase 2 may tighten by ordering reload first.

---

## Wall clock + the LS-K clock surface (CLOCK_REALTIME / CLOCK_MONOTONIC)

LS-K (ARCH §22.6) exposes two clocks to userspace through `SYS_CLOCK_GETTIME`,
built on this timer plus a one-shot PL031 RTC read.

### The two clocks

- **`CLOCK_MONOTONIC`** is `timer_now_ns()` verbatim — ns since boot from
  `CNTVCT_EL0`, the existing tsleep/poll timebase. Never goes backward.
- **`CLOCK_REALTIME`** is `timer_realtime_ns()` = `timer_now_ns()` + a single
  boot-time offset. The offset ties the wall-clock epoch (read once from the RTC)
  to the monotonic counter: `realtime(now) = epoch_anchor_ns + (mono_now −
  mono_at_anchor)`. The fast counter supplies the resolution + the elapsed delta;
  the slow RTC is touched exactly once.

### The wall-clock anchor (`arch/arm64/timer.c`)

```c
u64 timer_wallclock_offset_ns(u64 epoch_seconds, u64 mono_now_ns);  // pure; epoch 0 -> 0
void timer_set_wallclock_anchor(u64 epoch_seconds);                 // write-once, boot CPU
void timer_reset_wallclock_anchor_ns(u64 epoch_ns);                 // runtime (SYS_CLOCK_SETTIME)
u64 timer_realtime_ns(void);                                        // mono + offset
```

A single `static u64 g_wallclock_offset_ns` holds `epoch_ns − mono_at_anchor`.
It is written **once** on the boot CPU (from `boot_main`, right after
`timer_init`, before `smp_init`) and read unsynchronized by `clock_gettime` on
any CPU. Soundness mirrors `g_freq`: an aligned `u64` load on aarch64 is atomic,
and the SMP bring-up barrier orders the single write before any secondary's
first read — so the read is always a coherent snapshot with no lock. A `0` epoch
(no RTC) yields a `0` offset, so `CLOCK_REALTIME == CLOCK_MONOTONIC` (1970 +
uptime) — the honest "no wall clock" signal, never a fabricated time.

**Runtime re-anchor (net-7a).** `timer_reset_wallclock_anchor_ns(epoch_ns)`
re-publishes `g_wallclock_offset_ns` after boot for `SYS_CLOCK_SETTIME`. Because
the offset is a *single* aligned `u64`, the publish is one `__atomic_store_n`
(`RELAXED`) and the GETTIME read is one `__atomic_load_n` — a concurrent
`timer_realtime_ns` on another CPU reads either the old or the new offset, each
internally consistent, so no seqlock is needed even though a runtime setter races
readers (the LS-K single-`u64` design is what makes the setter SMP-safe by
construction; the only skew is ns-scale at the instant of a deliberate step, which
is itself a discontinuity). The boot anchor now routes through the same
ns-granular publish (`timer_set_wallclock_anchor(secs)` → `wallclock_publish_ns(
secs·1e9)`), byte-equivalent to the prior path. `CLOCK_MONOTONIC` is untouched.

The offset math cannot underflow for any plausible epoch: `epoch_ns` (~1.6e18
for 2020+) dominates the boot-early `mono_at_anchor` (< ~1e10), and
`epoch_seconds × 1e9` stays well inside `u64` (a 32-bit RTC epoch × 1e9 ≈
4.3e18 < 1.8e19).

### The PL031 RTC (`arch/arm64/rtc.c`)

`rtc_read_epoch_seconds()` discovers the ARM PrimeCell PL031 via the DTB
(`dtb_get_compat_reg("arm,pl031", …)`) with the QEMU-`virt` fixed-base fallback
`0x09010000` (the same I-15 pattern as PL011), maps it with `mmu_map_mmio`, and
reads the 32-bit `RTCDR` (offset 0) — the Unix epoch in seconds (QEMU seeds it
from the host clock). Returns `0` (fail-soft) if no PL031 is reachable or the
read is below a 2020 plausibility floor. The MMIO region is reserved in
`kobj_mmio_reserve_kernel_ranges` (`reserve_compat("arm,pl031")`) so a
`CAP_HW_CREATE` userspace driver cannot claim the RTC slot (I-5).

The kernel reads the RTC **once** at boot and never again at v1.0; the mapping is
held (there is no `mmu_unmap_mmio`, and one page of vmalloc for a reserved device
is harmless). A settable clock / re-read is the recorded v1.x seam.

### The syscall handlers (`kernel/syscall.c`)

| Syscall | Number | Returns |
|---|---|---|
| `SYS_GETPID` | 72 | `current->proc->pid` (always > 0) |
| `SYS_GETUID` | 73 | `current->proc->principal_id` |
| `SYS_GETGID` | 74 | `current->proc->primary_gid` |
| `SYS_CLOCK_GETTIME` | 75 | `0` / `-EINVAL` (bad clk_id) / `-EFAULT` (bad va) |
| `SYS_CLOCK_SETTIME` | 79 | `0` / `-EINVAL` / `-EFAULT` / `-EACCES` (not host owner) |

The three identity reads carry no capability and mutate nothing; the field
values are `< 2^32`, so the `s64` return is never negative (no error-aliasing).
`SYS_CLOCK_GETTIME` validates `clk_id` **first** (a bad id returns `-EINVAL`
without touching the buffer), then fills `struct t_timespec { s64 tv_sec;
s64 tv_nsec; }` (16 bytes, `_Static_assert`-pinned; the musl/arm64 layout) via
four `uaccess_store_u32` writes (low/high of each `i64`, little-endian) — any
fault routes to `-EFAULT`. `T_CLOCK_REALTIME = 0` / `T_CLOCK_MONOTONIC = 1` match
Linux `clockid_t`.

`SYS_CLOCK_SETTIME` (net-7a) steps `CLOCK_REALTIME` to a `t_timespec` read with
four `uaccess_load_u32` reads. It gates in order: `clk_id == T_CLOCK_REALTIME`
(MONOTONIC is non-settable → `-EINVAL`); then **`CAP_HOSTOWNER`** (a clock step is
system-global — the host owner's authority, never an identity's, I-22 →
`-EACCES`) — both checked *before* the buffer is touched, so a non-elevated caller
never reads through `ts_va`; then the buffer (`-EFAULT`), `tv_sec ≥ 0` /
`tv_nsec ∈ [0,1e9)` / `tv_sec ≤ 1e10` (the `tv_sec·1e9 + tv_nsec` overflow guard,
~year 2286) → `-EINVAL`. It calls `timer_reset_wallclock_anchor_ns`. The handler
is the *only* re-anchor entry from EL0; `CLOCK_MONOTONIC` is never affected.

### Tests (`kernel/test/test_clock.c`)

`clock.monotonic_advances` (timer_now_ns strictly advances across a busy-wait),
`clock.realtime_anchored` (realtime ≥ monotonic always; and a plausible wall
clock on the QEMU-virt test target — a 0 here would be a real RTC regression, not
the fail-soft path, which is for RTC-less bare metal), `clock.wallclock_offset_math`
(the pure offset helper: fail-soft 0 and `E·1e9 − M`), `clock.identity_syscalls`
(getpid/getuid/getgid return the calling Proc's own fields), `clock.gettime_errors`
(bad clk_id → `-EINVAL`; NULL buffer → `-EFAULT`), `clock.settime_reanchors`
(the runtime re-anchor steps `CLOCK_REALTIME` to a target and leaves
`CLOCK_MONOTONIC` unmoved, then restores), `clock.settime_cap_gate` (the
cap-then-buffer ordering: no `CAP_HOSTOWNER` → `-EACCES`; with it + a NULL buffer
→ `-EFAULT`; MONOTONIC → `-EINVAL` either way). The RTC hardware read is covered
transitively (the boot anchor feeds the realtime-plausibility assertion); the
gettime success copy-out is covered by the `id`/`whoami`/`date` LS-CI E2E; the
**settime** end-to-end path (real dispatch + `uaccess_load_u32` from a valid
buffer + a live `CAP_HOSTOWNER` — unreachable from the kernel test [va=0] or a
non-elevated tool [`-EACCES`]) is covered by joey's elevated round-trip probe
(`joey: net-7a SYS_CLOCK_SETTIME round-trip OK`).

### v1.x seams (recorded in ARCH §22.6)

A continuous timekeeper that *slews* rather than steps (the `SYS_CLOCK_SETTIME`
step primitive landed net-7a; the userspace UTC-clock-object discipline is the
v1.x refinement); a vDSO `clock_gettime` (EL0 CNTVCT is
already enabled, see `timer_enable_el0_counter_access`); a 64-bit RTC (the 32-bit
`RTCDR` wraps in 2106); uid→name resolution + `getgroups` (whoami/id are numeric
at v1.0); the pouch boundary-line mapping musl `getpid`/`clock_gettime` onto these
numbers.

---

## See also

- `docs/reference/01-boot.md` — entry sequence (`timer_init` slot in `boot_main`).
- `docs/reference/10-gic.md` — interrupt controller; timer IRQ routes through this.
- `docs/reference/08-exception.md` — `exception_irq_curr_el` dispatches to `timer_irq_handler` via `gic_dispatch`.
- `docs/ARCHITECTURE.md §12.3` — design intent for IRQ + timer.
- ARMv8-A Architecture Reference Manual (DDI 0487) — section D11 (Generic Timer).
