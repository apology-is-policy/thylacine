# Handoff 042 -- waiters-stops: what changed under codex/astra (to Astra)

**From**: main, 2026-10-05. **To**: Astra. **Why you**: codex/astra (base
`8746a8a2`) edits the files this arc rewrote: `kernel/9p_client.c`,
`9p_client.h`, `kernel/loom.c`, `docs/DEBUG-FS-DESIGN.md`,
`specs/SPEC-TO-CODE.md`, and both client dossiers. Your next merge of `main`
will conflict in those files. This note says what to keep.

## What the arc fixed

Two defects of one class: the elected 9P reader's handoff (ARCH 21.10) gave
the reader role to a waiter that could not read. A waiter stopped by ^Z or a
debugger was parked in place, skipped, and slept on after its resume. A Loom
ENTER whose pump found the role held slept on its ring's CQ list only, so
when a foreign sync reader left, nobody read its reply.

## Rules the merged code must keep

1. **`p9_rpc.owner` is gone.** The handoff and `client_tag_owed_locked` read
   `rpc->stop_parked` instead. Only `client_debug_stop_park` writes that
   field, under `c->lock`, for exactly the span of the park. Do not bring
   back `r->owner && proc_stop_requested(r->owner)`: the Proc's stop flags
   flip on a resume and a re-stop while the thread never runs.
2. **Every sleep inside the client sets `stop_unwinds`.** This covers the
   reader's recv, the non-reader rpc sleep in `client_wait`, and
   `client_park_for_progress_locked`. A stop unwinds the sleep, and the
   caller's loop parks the thread in `client_debug_stop_park`. A new sleep
   without it parks in place, and the stopped waiter re-sleeps after its
   resume without re-electing.
3. **Every `c->reader_active = false` runs `client_handoff_reader_locked`
   in the same `c->lock` hold.** There are four such sites. When the handoff
   finds no op to designate, it wakes the new role-waiter list
   (`p9_client_role_wait_register` / `_unregister`), and so does
   `client_mark_dead_locked`. A Loom ENTER whose pump returns 0 hooks that
   list beside its CQ hook and sleeps on `loom_cqw_role_cond`.
4. **Your `c->progress` clients are untouched.** The pumps refuse a progress
   client before taking the role, so no ENTER reaches the register on one,
   and no progress client takes the role. The conflicts with your private
   protocol work should be textual only. If a progress path ever adds a
   reader-role release, rule 3 applies to it.

## Where it is written down

DEBUG-FS-DESIGN 5c.6 (the waiters-and-stops amendment), LOOM.md 8.6 item 2,
ARCH 21.10, AUDIT-TRIGGERS rows 53 and 103, `specs/loom_role.tla` (the
role-waiter wake and both stop rules, with five buggy cfgs), and the
regressions `9p_client.{stopped_waiter_elects_on_resume,
stop_parked_owner_not_owed, note_flush_stop_parked_staging_not_owed,
handoff_skips_restopped_owner, handoff_skips_stop_parked, role_wait_contract,
loom_enter_wakes_when_role_frees, resumed_waiter_is_designated}`.
