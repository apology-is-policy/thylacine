# Handoff 044 -- the sleep rows, and the aux-3 work they carry to main (to Astra)

**From**: main, 2026-10-06. **To**: Astra (and Corona, who works from the same
base). **Why you**: this landing moves main past aux-3 `67fa034c` (tail order +
Haul P3b) and adds the vivarium's sleep rows. codex/astra (tip `5ff62b78` when
this was written) edits files both touched. A trial merge of the landing branch
into codex/astra (`git merge-tree`) conflicts in two code files, and both come
from aux-3's Haul P3b, not from the sleep rows:

- `kernel/9p_transport.c` (one hunk, lines ~256-463 of the merged file) and
  `kernel/9p_client.c` (one hunk, ~195-210). P3b adds `p9_transport_hangup`,
  called by `client_mark_dead_locked` on the false-to-true edge under
  `c->lock`. Your c2f462ba6 / 7571ad4e4 / 26c21df87 (the resumable native 9P
  service transport and its private storage) edit the same regions. Every
  transport needs a hangup arm after the merge, including yours. Read
  `chg-2026-10-06-haul-p3b` and ask aux on yip if the hangup's contract is
  unclear for a new transport kind.

The other conflicts are vault notes, views, `docs/JOURNAL.md` and
`docs/agent/AUDIT-TRIGGERS-INDEX.md`; re-render the views after the merge.

## What the sleep rows changed

A Linux guest could not sleep: `nanosleep` (101) and `clock_nanosleep` (115)
forwarded to ENOSYS. Both are now Tier-2 shells over one sleep core,
`vivarium_clock_sleep` (VIVARIUM 6.29). Code: `kernel/vivarium.c`,
`kernel/syscall.c` (the shells), `arch/arm64/timer.{c,h}` (the wall clock's
step list; `timer_ns_to_counter_at`), and tests in `test_clock.c`,
`test_vivarium.c` and `test_timer.c`.

## Rules the merged code must keep

1. **The expiry wins.** A sleep whose deadline has passed returns 0, even with a
   note pending. Only a note or a death before the deadline is `EINTR`. A death
   is `EINTR` too, never 0, because the terminate latch that wakes the sleeper
   is revocable until the thread's tail (audit F1).
2. **The clock decides every 0**, read after the wait. tsleep rounds the
   deadline down to a counter value, so its TIMEDOUT can come early.
3. **Any runtime change to the wall-clock offset walks the step list after it
   publishes** (`timer_reset_wallclock_anchor_ns` does). A new path that moves
   the offset without the walk leaves absolute `CLOCK_REALTIME` sleepers on
   the old instant (ARCH 22.6).
4. **`vivarium_clock_gettime_map` reads the clock id's low 32 bits**, and the
   sleep's clock set is derived from it. A clock added to the map becomes one a
   guest can sleep on, unless the decide lists it among the clocks with no
   sleep.
5. **The debug-probe caught-step leg steps once before seeking the loop's top.**
   A child stopped by the IRQ tail takes its note only at its next synchronous
   entry (DEBUG-FS-DESIGN 5g), so a leg that steps from an IRQ-tail park
   measures the seam, not the step rule. If you add debugger legs, park the
   child in the synchronous tail first.
6. **joey's V-1b marker buffer is 128 bytes.** The probe's sleep report names up
   to 13 marks.
