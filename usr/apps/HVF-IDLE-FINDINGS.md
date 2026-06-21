# HVF idle-spin: re-investigation findings

Aux-track research deliverable (no boots — `cargo build` ceiling). Cross-cuts
`kernel/` + `arch/` + `tools/` (the MAIN track's domain), so this is a findings
report for main to action, not a patch. Pairs with the prior notes
`[[bug-108-idle-test-responder-spin]]` (FIXED) and
`[[bug-110-hvf-interactive-idle-vcpu-kick]]` (closed as "proven host behavior" --
**that closure is re-opened below**).

## TL;DR

The user's instinct is right: "HVF just won't let the cores sleep" is **not**
the whole story, and "nothing can be done" is premature. Three grounded facts:

1. **Our guest idle path is correct on every CPU** -- `WFI` everywhere, the
   virtual-timer PPI is re-armed to a *future* deadline and EOI'd in the right
   order, so the guest is genuinely idle and genuinely executes `WFI`. This is
   ground-truthed from the code (citations below), not assumed.
2. **HVF's WFI-sleep is correct and has been since QEMU 6.0.** We run QEMU
   **10.0.2**. So this is not a missing-handler / stale-QEMU problem. When a
   guest spins under HVF but idles under TCG, upstream's own maintainers trace it
   to **guest-side timer/idle programming** tripping HVF's `VTIMER_ACTIVATED`
   re-exit or the "interrupt still pending -> WFI returns immediately" guard --
   things TCG's different timer/halt accounting hides.
3. **The single thing production systems do that we don't: tickless idle.** Our
   kernel runs a **1 kHz periodic tick on every CPU and never stops it at idle**
   (`timer_init(1000)`, no dynticks). Under HVF every tick also costs **two MMIO
   vmexits** (GICC_IAR read + GICC_EOIR write -- the GIC is QEMU-emulated). The
   famous fix for FreeBSD-on-M1 (UTM/HVF) was `kern.hz=1000 -> 100`, which took
   idle from **300-400% to ~4-5%**. Linux's equivalent is `CONFIG_NO_HZ_IDLE`.
   We have neither lever.

The honest caveat: I cannot boot (aux constraint), so I cannot measure *which*
sub-cause dominates our 340%. This doc gives main the cheap diagnostics that
decide it in seconds, then the matched fixes. The unifying, production-standard,
architecturally-good answer that helps in **every** branch is **tickless idle**.

## What I ground-truthed in our tree

| Fact | Where |
|---|---|
| Boot CPU idle loop: `irqsave -> set idle_in_wfi -> sched() -> WFI` | `kernel/sched.c:439` `bootcpu_idle_main` |
| Secondary idle loop: identical WFI structure | `kernel/smp.c:488-502` |
| Timer ISR re-arms `CNTV_TVAL = g_reload` (a *future* countdown) | `arch/arm64/timer.c:155` |
| IRQ dispatch order: `gic_acknowledge -> gic_dispatch(handler) -> gic_eoi` | `arch/arm64/exception.c:283-291` |
| => timer PPI line deasserted (ISTATUS cleared) BEFORE the EOI | (the two above, composed) |
| Tick rate: **1000 Hz on every CPU** | `kernel/main.c:352` `timer_init(1000)` |
| GICv2 ack/eoi are **MMIO** (`GICC_IAR`/`GICC_EOIR`) | `arch/arm64/gic.c:872-914`, accessors `:237-245` |
| Under HVF the GIC is **emulated by QEMU** => each MMIO access is a vmexit | (research; HVF irqchip is out-of-kernel) |
| **No** tickless / dynticks / idle-tick-stop anywhere | grep of `sched.c` + `timer.c` (empty) |

So the guest does the right thing; the cost is in *how often* it has to do it
under HVF, and/or in HVF not parking the host thread on the `WFI`.

## Why bug_110's "proven host behavior, nothing to do" is re-opened

bug_110's decisive comparison was **HVF-interactive (340%) vs TCG-interactive
(30%)** -- that proves the spin is *HVF-specific*, but it does **not** prove it's
*interactive-specific*. Its "automation = 0%" run was over the test harness,
which **defaults to `THYLACINE_ACCEL=tcg`** -- so "automation 0%" was almost
certainly a **TCG** measurement. **HVF-over-a-PTY idle was never measured.** The
interactive-ness and the HVF-ness were never cleanly separated. That gap is
exactly what lets "it's just the interactive stdio chardev kicking the vCPUs"
stand unchallenged. It may be true, partly true, or a red herring -- one cheap
measurement decides it (below).

Per the project's own "no host load" discipline: a surfaced spin preempts, and
"the host won't let them sleep" is a conclusion you may reach only *after* ruling
out the guest -- and the guest-side lever (tickless idle) was never tried.

## The leading mechanism (most-likely, stated as a hypothesis to test)

Under HVF, an idle CPU pays a per-tick tax that an idle CPU should not pay at
all, because the tick never stops:

```
vtimer deadline (1 ms)
  -> HVF VTIMER_ACTIVATED exit            (vmexit)
  -> guest takes IRQ
  -> gic_acknowledge(): read GICC_IAR     (MMIO vmexit)
  -> timer_irq_handler(): re-arm CNTV     (sysreg, cheap)
  -> gic_eoi(): write GICC_EOIR           (MMIO vmexit)
  -> idle loop -> WFI -> EC_WFX_TRAP exit  (vmexit) -> QEMU parks vCPU
  ... 1 ms later, repeat -- 1000x/sec, on EVERY core, forever, even idle.
```

A truly idle CPU has nothing to preempt, so it does not need a periodic tick at
all. Linux/FreeBSD stop it; we don't. Two amplifiers make this worse than the
raw vmexit arithmetic suggests: (a) HVF wake latency on Apple cores is ~1 ms
(agraf's measurement), comparable to our 1 ms tick period, and (b) if QEMU's
synced view of `CNTV_CTL.ISTATUS` ever lags the guest's re-arm, the emulated GIC
line stays asserted and `hvf_wfi` returns *immediately* -> the host spins in
`hv_vcpu_run` instead of parking (this is the `VTIMER_ACTIVATED`-storm class;
upstream hit it as the Windows `ctl=7, cval=0` case). bug_110's own data ("`WFI`
isn't halting the cores; PCs in the idle loop; only 1 kHz timer IRQs") is
*consistent with* "the host spins re-entering the guest idle loop between ticks"
-- an IRQ counter cannot tell that apart from "the host parks."

## Diagnostic plan (for the MAIN/kernel track -- needs a boot; do these FIRST)

Cheapest-first; the first two decide the root cause before any code changes.

1. **`sample` the QEMU vCPU threads under HVF-idle.** While an idle HVF VM spins:
   `sample $(pgrep -n qemu-system-aarch64) 3` (or `sudo sample <pid> 5`). Read
   the per-thread stacks:
   - vCPU threads parked in `hvf_wfi` / `qemu_wait_io_event` / halt
     -> **host IS sleeping on WFI**; the CPU% is elsewhere (main thread, or the
     re-wake tax). Look at the main thread next.
   - vCPU threads churning in `hv_vcpu_run` / re-entering after `VTIMER_ACTIVATED`
     -> **WFI is NOT parking** -> the vtimer-sync / emulated-GIC branch. This is
     the storm class; fix guest-side (tickless idle / disable vtimer at idle) or
     move off emulated-GICv2 (GICv3-under-HVF).
   - the **main** QEMU thread hot in `glib`/`pselect`/chardev
     -> the **bug_110 interactive-chardev-kick** branch.
   This one output resolves which of the three branches you're in.

2. **Close bug_110's gap: measure HVF-over-PTY idle.** Run the VM **non**-interactively
   under HVF (force `THYLACINE_ACCEL=hvf`, drive over a PTY / `expect`, no real
   TTY) and measure idle CPU for >=60 s. If it still spins -> it's a **general
   HVF tax** (tick / vtimer), not the interactive stdio chardev -> tickless idle
   is the fix. If it drops to ~0% -> bug_110 was right and it's the real-TTY
   chardev -> the serial-backend / Halcyon-ssh answer.

3. **Lower-tick experiment (one-liner, high signal).** `timer_init(1000) ->
   timer_init(250)` or `100` (`kernel/main.c:352`) and re-measure HVF idle. If
   idle collapses ~4-10x (the FreeBSD datapoint), the periodic tick is the
   dominant cost -> tickless idle captures it fully and a lower default is a
   stopgap. (Tradeoff: coarser preemption granularity; weigh vs the I-17 latency
   budget. 250 Hz = 4 ms slices is a reasonable dev default.)

4. **Latched-ISTATUS check.** Log `CNTV_CTL_EL0` at the `WFI` entry in the idle
   loop; if `ISTATUS` (bit 2) is ever set there, the guest is about to `WFI` with
   the timer still "firing" -> confirms the storm branch directly.

## Fix directions, ranked by payoff / cost

1. **Tickless idle (NO_HZ_IDLE / dynticks).** *The real fix; production parity.*
   When the idle thread is selected (nothing runnable), stop the periodic tick and
   arm a **one-shot** vtimer only for the next real deadline (the nearest
   `tsleep`/`torpor` wake), or disable `CNTV_CTL.ENABLE` entirely if there is no
   pending deadline; re-arm on wake. This removes the per-tick tax, removes any
   chance of a vtimer-latch storm at idle (the timer is off when idle), and is
   what Linux/FreeBSD do to idle cool under HVF. Compatible with our design: the
   "work arrived" wake already runs over `sched_notify_idle_peer`'s IPI
   (`sched.c:569`), independent of the periodic tick; the periodic tick is only
   needed to preempt a *running* thread, of which idle has none. Cost: real
   kernel work (a one-shot timer path + idle-enter/exit hooks), but the correct
   long-term answer regardless of which diagnostic branch wins.

2. **Lower the default tick (stopgap).** `timer_init(1000) -> 250`. One line,
   large fraction of the win (per the FreeBSD 1000->100 = 300%->5% datapoint),
   ships today. Coarser preemption; weigh against I-17. Pairs well as the interim
   before tickless lands.

3. **GICv3-under-HVF via ISV-safe MMIO accessors.** We fell back to emulated
   **GICv2** under HVF (Lazarus W2) to dodge the GICv3-distributor `isv`
   data-abort assert -- but that same `isv` class was already solved for
   *userspace* virtio under HVF (the W3.5 single-instruction ISV-safe accessor,
   #890). Applying that to the kernel GIC init would let HVF run **GICv3** like
   every production setup does, removing the per-ack/eoi MMIO-vmexit tax and the
   emulated-GICv2 vtimer-sync quirks. Bigger lift; pays off broadly (not just
   idle). Worth scoping if diagnostic #1 shows the cost is in `hv_vcpu_run` /
   GIC MMIO.

4. **Interactive serial backend (if diagnostic #2 says it's the chardev).** Try
   `-serial pty` / a unix-socket console for *interactive* HVF runs instead of
   `-serial mon:stdio`, and measure. The long-term answer bug_110 already noted
   (Halcyon framebuffer / headless-ssh console) moves interaction off the host
   TTY by construction. `tools/run-vm.sh` change.

## Sources (QEMU HVF internals + the production datapoints)

- QEMU HVF ARM WFI handler + `VTIMER_ACTIVATED` mask/sync:
  `target/arm/hvf/hvf.c` (github.com/qemu/qemu).
- WFI handler patch (agraf v9): patchwork.kernel.org/project/qemu-devel/patch/20210912230757.41096-6-agraf@csgraf.de/
- "Optimize/simplify WFI handling" thread (the `ctl=7,cval=0` 100% case):
  lists.gnu.org/archive/html/qemu-devel/2020-12/msg00300.html
- Sync `CNTV_CTL`/`CVAL` correctness patch:
  mail-archive.com/qemu-devel@nongnu.org/msg1149428.html
- FreeBSD-on-M1 100% idle == guest `kern.hz` (300-400% -> 4-5% at `hz=100`):
  gitlab.com/qemu-project/qemu/-/issues/959 ; github.com/utmapp/UTM/discussions/2533
- Linux tickless (`NO_HZ_IDLE`): docs.kernel.org/5.19/timers/no_hz.html
- Docker-for-Mac HVF idle 100%->low across QEMU 6.x: github.com/docker/for-mac/issues/5812
- HVF GICv3 support: patchwork.kernel.org/comment/23936563/

## Disposition

Enqueued as a tracked item for the main/kernel track. Aux owns the
investigation + this report; main owns the boot-measurement (the diagnostics
above) + any kernel/tools change. Recommended sequence: run diagnostic #1 + #2
(minutes), then land #2-stopgap (lower tick) if it helps, then scope #1-real
(tickless idle) as the durable fix.
