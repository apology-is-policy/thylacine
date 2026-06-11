# RW-7 — Drivers / Hardware + Halls of Extinction

**Tier**: STANDARD (DELTA-eligible where W2/W3/HX-2/A-4c were already audited).
**Status**: CLOSED (dirty close — round-2 re-audit converged; see below).
**Register rows**: `docs/holotype/00-register.md` (HT07.*).
**Closed list**: `memory/audit_holotype_rw7_closed_list.md`.

## Surface

The driver / hardware substrate + the dying-machine crash dump:

| Group | Files | Prior audit |
|---|---|---|
| GIC + EL1 virtual timer + IRQ forwarding | `arch/arm64/gic.c`, `arch/arm64/timer.c`, `kernel/irqfwd.c` | W2 (GICv2 + vtimer) |
| Console RX + SAK trusted path | `arch/arm64/uart.c`, `kernel/cons.c`, the `proc.c` console-owner machinery | A-4c-1/2 |
| VirtIO transport + KObj MMIO/IRQ/DMA | `kernel/virtio.c`, `kernel/virtio_pci.c`, `kernel/mmio_handle.c`, `kernel/dma_handle.c` | P4-F/I |
| Kernel CSPRNG | `kernel/chacha20.c`, `kernel/random.c` | W3 |
| Halls of Extinction (Tier-1 + Tier-2) | `arch/arm64/halls.c`, `arch/arm64/halls.h`, the `exception.c` frame wrappers | HX-1, HX-2 |

The userspace virtio drivers (`usr/virtio-*`) are RW-8, out of this scope.

## Reviewers

**Round 1**: 4× Fable-max `holotype-reviewer` (R1 gic/timer/irqfwd; R2 uart/cons; R3 virtio/mmio/dma; R4 chacha/random/halls — all `claude-fable-5`, `MODEL(start)==MODEL(end)` on all four, NO fallback even on the R4 crypto surface) + an Opus self-audit double-covering R3 (device memory safety — least-recently-audited as a unit) + R4 (CSPRNG + the dying-machine dump) + R1's irqfwd half.

**Round 2** (dirty close): 2× Fable-max `holotype-reviewer` on the FIXES themselves (A: irqfwd wait/wake + virtio proc-exit; B: console SAK death/notes — the tree's most bug-prone lineage) + an Opus self-audit on the same surfaces.

## Headline

The recurrent **P6-multi-thread-lift theme, a fourth time**: the P6 multi-thread lift, the LS-5 interactive-Ctrl-C landing, and the Lazarus arc each layered onto the driver/HW substrate **without re-examining the death paths, the multi-thread wait/wake, or the cons -> notes -> SAK seam** — and that is exactly where all three RW-7 P1s lived. None was in the crypto (R4: 0 P0/P1/P2 — CSPRNG + HX SOUND, confirming the self-audit) or the GICv2/vtimer mechanics; all three were lifecycle/role/wait-wake regressions that the layered-on features never reconciled.

Dirty close: 3 P1 + 3 P2 (P1+P2 = 6) AND the fixes are structurally invasive (the irqfwd IRQ wait/wake protocol; a new proc-exit device-quiesce hook; the SAK notes/death behavior) — so a round-2 re-audit on the fix set was mandatory.

## The three P1s (all fixed in-arc, each with a regression)

### R1-F1 [P1] — irqfwd single-waiter Rendez extinction (commit `0c0e484`)
`kobj_irq_wait` called `sleep()` on a SINGLE-WAITER `Rendez` with no guard. A `KObj_IRQ` handle is shared across a multi-thread Proc's peer Threads; the live driver (stratumd, multi-threaded, drives virtio-blk IRQs via `SYS_IRQ_WAIT`) with two Threads waiting on the same IRQ fd hits `sched.c`'s `extinction("sleep: rendez already has a waiter")` — an unprivileged kernel DoS (CAP_HW_CREATE-gated, so narrower than fully-unprivileged, but the exact class CLAUDE.md §self-audit names). **Converged** with the Opus self-audit (SA-R1-1), independently.
**Fix**: a per-`KObj_IRQ` `bool waiting` busy-guard under `k->rendez.lock`; a 2nd concurrent waiter returns `KOBJ_IRQ_WAIT_BUSY` (-> `SYS_IRQ_WAIT` -> -1) instead of reaching `sleep()`. Regression `irqfwd.second_waiter_refused`.

### R3-F1 [P1] — DMA-into-freed-buddy-pages on driver death (commit `3ec134e`)
The Opus self-audit MISSED this (it covered create/destroy/refcount but not the death path); R3 caught it — the value of the two-prosecutor design. A CAP_HW_CREATE driver (stratumd virtio-blk, the live v1.0 case) that dies ABNORMALLY (`_Exit(127)` on an abort / mallocng assert) leaves its virtio device ARMED (status DRIVER_OK, virtqueue PAs latched); the reaper frees the `KObj_DMA` pages back to the buddy allocator; an in-flight used-ring completion DMAs into recycled memory — silent cross-Proc corruption (the AEGIS/mallocng class). I-7's device-stop clause is unmet on death (deliberate teardown is already safe — `random.c` resets first, W3-F5).
**Fix**: `proc_quiesce_owned_devices` walks the dying Proc's `KObj_MMIO` handles and resets every probed virtio transport in each claimed range (via the kernel `.base` mapping, independent of the user mapping `vma_drain` tears down), BEFORE the `KObj_DMA` pages free, at BOTH proc-exit close sites (`proc_close_handles_at_exit` single-thread; `proc_free` multi-thread reap + orphan/rollback). The structural close is a per-device `KObj_VIRTIO_DEV` device-session model (R3-F8, Phase 5+). Regression `virtio.proc_death_quiesces_device` (+ `reset_in_range_no_match`, `vq_size_for`).

### R2-F1 [P1] — Ctrl-C after SAK terminates the trusted login authority (commit `2608c88`)
`proc_console_sak` set `g_console_owner = trusted`, conflating two roles LS-5 made distinct: the console OWNER (the `interrupt`/Ctrl-C target) and the console ATTACH (the SAK/elevation authority). A Ctrl-C AFTER a SAK posted `interrupt` to `g_console_owner == corvus`; corvus is non-self-managing, so LS-5's default-terminate armed its terminate latch and **the trusted login authority died until reboot** — an unprivileged Ctrl-C taking down the trusted path.
**Fix**: SAK grants corvus only the console-ATTACH and leaves the OWNER NULL; the Ctrl-C owner is re-established when login spawns the session shell (`SPAWN_PERM_CONSOLE_OWNER`). Regression `cons.sak_does_not_terminate_trusted` (with a positive latch control).

## The three P2s (all fixed in-arc)

- **R1-F2 [P2]** (`0c0e484`): `kobj_irq_free_internal` SMP-raced an in-flight `kobj_irq_dispatch` on another CPU (the GIC slot holds a raw non-refcounted `arg=k`; dispatch touches `k` after the lock release via `wakeup()`'s re-lock) — UAF. **Converged** with self-audit SA-R1-2. Fix: `dying` + `in_dispatch` flags; `free_internal` sets `dying` then spins until `!in_dispatch` (the LAST k-touch under the lock) before `kfree`.
- **R3-F2 [P2]** (`3ec134e`): `virtio_virtqueue_destroy` wrote QUEUE_READY=0 with no readback + an over-claiming comment ("device doesn't continue posting"). **Converged** with self-audit SA-R3-2. Fix: a QUEUE_READY readback (the VIRTIO 1.2 §4.2.3.2 sync point) + an honest comment (only a full reset drains an in-flight DMA; the two page-freeing callers reset first).
- **R2-F2 [P2]** (`2608c88`): the SAK's `interrupt` courtesy-post to the OLD owner now TERMINATES a non-self-managing owner (joey during bringup -> init dies) since LS-5 made `interrupt` load-bearing. Fix: drop the courtesy post (no consumer); the attach-bit revoke is the SAK's observable effect on the old owner.

## P3 + the verified-SOUND set

- **R3-F6 [P3]** (`3ec134e`): no pow2 validation of QueueNumMax. Fix: `virtio_vq_size_for` fails closed on a zero or non-pow2 clamped size + 3 `_Static_assert`s pin `VIRTIO_VQ_NUM_DEFAULT`.
- **R4-F1 [P3]**: the per-CPU Halls frame-slot comment claimed "handlers do not migrate CPUs mid-execution" — FALSE for a blocking EL0 syscall (it can resume on another CPU, so `halls_leave_frame` runs there, stranding/clobbering the slot). STILL SAFE (HX-I4 `halls_frame_is_live` gates every read -> capture-current), but the false comment could lead a maintainer to delete the HX-I4 gate (the "stale load-bearing comment" class — RW-2 2A-F5, RW-4 R-A). Fix: corrected all 3 sites (halls.c / halls.h / exception.c) crediting HX-I4 for BOTH the noreturn-skip AND the migration-stranding cases.

**Verified SOUND** (round-1, no finding): I-5 non-transferability (`handle_dup` rejects KOBJ_MMIO/IRQ/DMA; the masks are disjoint by `_Static_assert`; no transfer syscall exists); the I-5 INTID + MMIO kernel-range reservations (GICv3/v2 dist+GICC, PL011, ECAM); the dma_handle overflow/refcount/KP_ZERO; the GICv2 EOI-token no-nesting + I-18 SGI order + the CNTV timebase (W2 DELTA); the ChaCha20 RFC-8439 block vector + forward-secrecy byte accounting + the fail-closed seeded gate + the all-zero guard + the virtio-rng pull memory-safety (W3 DELTA); HX-I1 (the in-dump guard set before any faulting read) + HX-I2 (the depth-capped + sanity-gated fp walk) + HX-I3 (the EXTINCTION ABI line) + HX-I4 (the frame-is-live plausibility gate) + the reloc-free KASLR-independent HX-2 symtab.

## H-items (registered, not built — non-soundness completeness/SOTA)

R3-F8 (per-device `KObj_VIRTIO_DEV` device-session model — the structural close for R3-F1, Genode/Fuchsia SOTA); R2-F3 (a dedicated `hangup`/`console-revoked` note name so a foreground app distinguishes SAK-revoke from Ctrl-C — the structural close for R2-F2); R1-F3 (the GICR_TYPER.Last redistributor walk + the stale 0x40000 stride comment); R3-F4 (VERSION_1 feature ack — completeness; fail-closed today); R3-F5 (the reservation rests on a hand-maintained DTB-compat list — board-drift, a W4 seam); R3-F7 (probe_cb leaks an `mmu_map_mmio` on a magic mismatch — boot-bounded); R4-F2/F3 (chacha partial-tail KAT + a continuity block-1 KAT — test completeness); the `smp_cpu_idx_self()` dense-Aff0 I-15 seam (#7, already tracked).

## SA-R2-1 (Opus round-2 self-audit, bounded residual)

`virtio_mmio_reset_in_range` can reset the kernel-driven virtio-rng slot if a (trusted) driver claimed the rng's page and died while `random.c` pulls — racing `g_rng_dev_lock`. BOUNDED: anomalous (no legit driver claims the rng's page), CAP_HW_CREATE-gated, the driver could already scribble the rng while alive (the documented kproc-trust residual — no widening), and the consequence is one re-init'd pull. See the round-2 disposition below.

## Round-2 (dirty-close) re-audit

Two Fable prosecutors on the fixes themselves (A: irqfwd + virtio SMP/lifetime/proc-exit; B: console death/notes) + an Opus self-audit. Both `claude-fable-5`, MODEL start==end, no fallback.

**Reviewer B (console): CLEAN — 0 P0 / 0 P1 / 0 P2.** It actively prosecuted and confirmed all 14 soundness properties of the SAK fix (R2-F1/F2 hold; the repurposed guard ≡ the body in every state where it fires; fail-CLOSED on a dead trusted Proc; single-attach; I-9 no stranded sleeper; I-19 fewer notes / no reorder; I-27 preserved; lock order strictly simplified). 4 P3 + 2 H, all dispositioned at the close: F1 (stale pre-fix contracts, 7 sites — doc-swept); F2 (console_mgr's fixed intr-then-sak batch order can re-synthesize the init-death via a coalesced BREAK+Ctrl-C — fixed: a SAK now supersedes a batched Ctrl-C); F3 (pre-existing bare-Ctrl-C-during-bringup kills init — tracked #35, decide+document); F4 (the production-typical first-SAK-from-relinquished-state untested — added); H1/H2 registered.

**Reviewer A (irqfwd + virtio): 1 P1 + 1 P2 + 1 P3 — the dirty close earned its round.** The irqfwd commit was verified fully sound (both R1-F1/F2 wait/wake-protocol changes survive prosecution: the dispatch/free `in_dispatch` drain terminates + closes the UAF window under the same-lock atomicity argument; the busy-guard clears on every sleep return incl. SLEEP_INTR; the live-waiter-vs-free is excluded by the SYS_IRQ_WAIT obj ref). But the virtio R3-F1 quiesce was found **incomplete** (F1, P1): it walked only the handle table, while `SYS_MMIO_MAP` lets a driver `mmap` a device then close the fd — the `KObj_MMIO` then lives only on the `BURROW_TYPE_MMIO` VMA mapping, invisible to the handle walk, so a map-then-close-fd driver dying armed reopens the corruption. Fixed by adding a `p->vmas` walk. F2 (P2): the in-range reset hits the kernel-driven virtio-rng slot (co-paged + un-reserved; the blk driver in the rng's page legitimately claims it — the live config) without `g_rng_dev_lock`, racing `random.c`; fixed by skipping `device_id==RNG`. This F2 independently re-derived + escalated the Opus self-audit's SA-R2-1 (the two-prosecutor convergence). F3 (P3): the `KOBJ_IRQ_WAIT_BUSY` sentinel collided with a maximal `pending_count`; fixed by saturation.

Because round-2 did not converge clean on the virtio surface (1 P1 + 1 P2), a round-3 re-audited the F1 VMA-walk + F2 RNG-skip fixes.

## Round-3 re-audit (the round-2 virtio fixes)

One Fable prosecutor on `proc_quiesce_owned_devices` (the VMA walk), `virtio_mmio_reset_in_range` (the RNG skip), the `pending_count` saturation, and the new `virtio.proc_death_quiesces_vma_only_device` regression + an Opus self-audit. `claude-fable-5`, MODEL start==end.

**CONVERGENCE VERDICT: CLEAN — 0 P0 / 0 P1 / 0 P2 + 2 P3.** The dirty-close recursion terminates. The prosecutor verified the three round-2 fixes correct: the VMA walk's lifetime (the mapping-ref → burrow-ref → kobj-ref pin chain holds until vma_drain, which runs AFTER both quiesce sites), quiescence (thread_count==1 at exit / all-reaped at proc_free, the full mutator inventory enumerated), and idempotency (status=0 is a no-op, no re-arm in the exit→reap window); the RNG skip's completeness (random.c is the sole kernel-side virtio driver and targets only device_id RNG); and the saturation (caps at 0xFFFFFFFE strictly below the sentinel, costs no wakeup). It **withdrew 5 candidate findings** after proving each guarded (double-quiesce-on-freed-state, stale kobj_mmio on a non-MMIO burrow, guard-VMA NULL deref, cross-Proc reset via a shared KObj_MMIO, half-built-Proc rollback through the relaxed `if (!p)`).

The 2 P3s are **residuals of the round-1 mechanism's documented trust envelope, not fix-introduced defects**:
- **round-3 F1** [P3]: a driver that FULLY releases its KObj_MMIO claim (SYS_BURROW_DETACH + close fd) while armed is invisible to both walks — a malicious/buggy CAP_HW_CREATE driver (outside the v1.0 trust envelope; the same posture as the documented rng-slot residual; stratumd maps-and-holds). **Documented** in the `proc_quiesce_owned_devices` header + closed by the R3-F8 structural per-device-kobj (resets at last-claim-drop).
- **round-3 F2** [P3]: the quiesce `virtio_reset` lacked the R3-F2-style completion readback (dormant where MMIO stores trap synchronously — every current substrate; Lazarus-track). **Fixed** — a `virtio_get_status` readback after each reset, mirroring `virtio_virtqueue_destroy`.

## Disposition + close

3 P1 + 3 P2 fixed in-arc (round-1), + 1 P1 + 1 P2 found and fixed by the dirty-close round-2 (the R3-F1 quiesce completion), + 8 P3 fixed and 1 tracked (#35), + the H-items registered (HT07.* in the register). Two clean dirty-close rounds (2 then 3) converged. Posture: default build **823/823 PASS**, 0 EXTINCTION, full `/sbin/login` E2E green; the SMP gate (default+UBSan × smp4/smp8) is the standing witness for the death-path/proc-exit/wait-wake changes. The virtio-input QMP E2E timing flake (#34) is orthogonal + self-heals on re-run.

Closed list: `memory/audit_holotype_rw7_closed_list.md`.
