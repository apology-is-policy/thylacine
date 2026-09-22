---
id: view-closed-sub-kernel-poll
type: view
title: "Do-not-re-report preamble — sub-kernel-poll"
query: closed:sub-kernel-poll
---
# Do-not-re-report preamble — sub-kernel-poll

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-kernel-poll`). Paste or transclude
into a prosecutor prompt as the closed-findings preamble.

Read it WITH two standing facts about this surface:

- **The F3 → 2C-F1 pair is the surface's history lesson**: a P1
  closed by documenting a single-thread precondition, voided by the
  multi-thread lift, detonated at RW-2, closed structurally by the
  retain. Any disposition of the form "safe because only one thread
  does X" on this surface must name the tripwire that fires when X
  stops being true.
- **The retain has a known-inert kind**: KObj_Srv listener polls pin
  nothing ([[seam-poll-srv-registry-retain]]) — safe only while the
  boot registry is immortal. A prosecutor finding this again has
  found the seam, not a new bug.

<!-- generated:begin -->
16 closed findings on [[sub-kernel-poll]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-b0poll-r5-f1]] [P1] A noise-driven poll(-1) holds its CPU IRQ-masked for as long as the noise lasts, and the noise is unprivileged (fixed) — FIXED at the time with a per-thread spin budget (`nsleeps`) plus a 1 ms
- [[fnd-b0poll-r6-s1]] [P1] Two masked pollers on one CPU hand it to each other through sched() inside the masked syscall, so a per-thread sleep bound never unmasks the CPU (fixed) — FIXED by [[chg-2026-09-22-poll-preemption-point]]: the backstop is deleted and
- [[fnd-b0poll-r7-f3]] [P2] Both point tests stay GREEN with sched_preempt_point's body replaced by `return;` -- the test pollers are kthreads, so the unmask is inert (fixed) — FIXED by `sched.preempt_point_takes_a_pending_irq`: mask with
- [[fnd-b0poll-r7-s4]] [P3] The isb sabotage PASSES: the prose had upgraded the point into a per-pass delivery guarantee the architecture never gives, while the model stated it correctly (documented) — FIXED at `0434a4bc`: the emitted instruction sequence is unchanged (the `isb`
- [[fnd-b0self-r1-f3]] [P3] poll.timeout_survives_a_busy_list passed with the deadline test removed: a producer on another thread hits the clear-to-tsleep window only by luck (fixed) — Rebuilt @237ba793: the producer is the polled object itself, a test Dev whose `.poll` registers, walks its own hook list on every sample, and is never ready. Every re-sample then re-flags the hook inside the window. The walking stops after 1 s so a kernel without the test still returns; the test asserts WHEN the last sample happened, with non-vacuity asserts on the re-sleep counter and the sample count.
- [[fnd-kt1-r1-a5]] [P3] the two audit-trigger surfaces' as-built references and the trigger table do not carry the new arm; two loop comments describe the mechanism that was removed (fixed) — Fixed in 062efe18: the three stale comments (poll.h's registering-path list, halcyond main.rs's menu-wait comment, menuset.rs) rewritten; the two AUDIT-TRIGGERS rows (the pollable Loom; the kaua-term seam + the session compositor) appended with the CLAUDE.md index lines.
- [[fnd-poll-r1-f1]] [P2] A client polling its own srv connection got the SERVER endpoint's revents (fixed)
- [[fnd-poll-r1-f2]] [P2] Teardown latched the two EOF flags under separate locks — a poll between saw half a hangup (fixed)
- [[fnd-poll-r1-f3]] [P1] The handle-slot borrow across the scan — doc-fixed on a precondition the lift later voided (fixed)
- [[fnd-poll-r1-f4]] [P1] A NULL-obj KOBJ_SPOOR slot reached the Dev dispatch (fixed)
- [[fnd-pouchb0-r3-f2]] [P2] client-side poll on a byte-mode /srv connection is neither implemented nor fail-closed: it samples the server's end, and the reply wakes nobody (fixed) — Fixed in full, not fail-closed: POLLNVAL for a client endpoint is indistinguishable from the libc defect 0039 fixes, so the prover could no longer discriminate, and the browser's IPC would still have had nothing to poll. `srvconn_poll(cn, client, events, pw)` gives the two endpoints mirror-image rows, and reading `srvconn.c` end to end found the wake set incomplete in BOTH directions -- a nonblocking server polling for write room was never woken by a client drain either -- so every ring mutation now walks the one hook list. That is sound only with a second fix nobody had asked for: `sys_poll_for_proc` returned 0 when a post-wake re-sample found nothing, so a timed poll "timed out" in microseconds and `poll(-1)` could return 0. `specs/poll.tla` modelled readiness as a one-way edge and could not see it; the spec was extended first. The libc half is pouch 0041 (the stream-socket shape at EOF). Not covered and recorded: an ACCEPTED socket, untagged by 0006's design.
- [[fnd-pouchb0-r4-f1]] [P1] a console poller registered during a trusted episode is stranded on the episode list after END: the re-arm kept hooks where the first scan put them (fixed) — Fixed differently. The suggested fix leaves the secondary as a side channel (each wake is a kernel pass whose CPU cost the caller can time -- the cadence of the secret), so the spec was extended first (`cons_poll.tla` NoSecretCadence) and the poll loop re-registers EVERY pass: unhook all, put all, clear, then each `.poll` WITH its hook. The Dev re-chooses its list each pass; the fd re-resolves with its hook (the parked S1 item). Both real-poller tests fail on the sabotaged sample-only loop.
- [[fnd-pouchb0-r4-f2]] [P2] the poll re-arm loop never checks death or stop itself, and tsleep's checks sit behind its cond test: a noise-driven poll(-1) is unkillable and unstoppable (fixed) — Fixed as suggested, spec first: each pass checks `thread_die_pending` and parks on `proc_stop_sleeper_park` with every hook off (DEATH WINS: the park returns SLEEP_INTR), and a noise pass `sched_yield_hint`s so queued work runs. RESIDUE, the operator's: with nothing else runnable the loop still spins with interrupts off, because syscalls run IRQ-masked end to end -- a preemption-model question recorded for the F3-F9 kernel-design conversation, not invented here.
- [[fnd-pouchb0-r4-f3]] [P1] an IRQ-edge ABBA between the console hook lists and g_cons.lock: the list lock was plain, nested under an interrupt-taken lock, and held with IRQs on by console_mgr (fixed) — Fixed: every list op irqsave. Pre-existing since LS-8. The vault lock note had said "never widen this lock to irqsave"; the half that stands is that no IRQ handler WALKS a list (O(pollers) work) -- the lock is masked for the nesting, not to license an IRQ walk.
- [[fnd-rw2-2cf1]] [P1] A registered poll waiter outlives the obj ref — sibling-close mid-sleep frees the hook list (fixed)
- [[fnd-rw2-r2poll-f1]] [P3] The retain is INERT for KObj_Srv — listener-poll safety rests on the boot registry's immortality (documented) — The overclaiming comment fixed; the obligation tracked as
<!-- generated:end -->
