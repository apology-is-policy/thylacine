# Handoff 043 -- signal7: what changed under codex/astra (to Astra)

**From**: main, 2026-10-05. **To**: Astra. **Why you**: codex/astra (base
`8746a8a2`, tip `5ff62b78` when this was written) edits files this arc
rewrote: `kernel/cons.c`, `kernel/proc.c`, `kernel/syscall.c`, `proc.h`,
`poll.h`, `cons.h`, `kernel/test/test_cons.c` and `test.c`, `usr/joey/joey.c`,
and five of the dossiers it touched. A trial merge of this arc into your tip
(`git merge-tree`) conflicts in no code file. Every hunk lands in the function
it was written for, with the same lines. The conflicts you will see are in
vault notes, views, `docs/JOURNAL.md` and `docs/agent/AUDIT-TRIGGERS-INDEX.md`.
Handoff 042's arc already causes most of them. This arc adds
`sub-kernel-syscall-dispatch`, `sub-kernel-vivarium` and `sub-stratum-boot`.
This note says what the merged code must keep.

## What the arc fixed

VIV-EINTR (in your base) built the call's half of the operator's vote of
2026-09-29: the vivarium marks a call on signal(7)'s list
`note_interruptible`. The wait's half was missing. Only the two 9P waits
opted in, so a listed Linux call blocked in a pipe, the console, `poll`,
`wait4` or a futex rode a caught note out, and musl's `pause()` never
returned for one. Every such wait now opts in (ARCH 8.8.3; VIVARIUM 6.22).

## Rules the merged code must keep

1. **One predicate decides.** `thread_caught_note_unwinds(t)` is true for a
   Linux thread in a listed call that can claim the note. It is false for a
   native thread and for a kernel caller. The four caught arms of the sleep
   cores and poll's verdict consult it, and nothing else does. A new wait
   that a listed Linux call can reach opts in (`sleep_noteintr` /
   `tsleep_noteintr`) or joins ARCH 8.8.3's named exclusions: the 9P send
   side, poll's settle, and the notes fd's read. It never rides the note out
   unnamed (AUDIT-TRIGGERS row 57, prosecute (g)). Your debugger stop and
   birth parks use plain `sleep`, which is right: they are not a listed
   call's wait.
2. **Every console wait is `noteintr` for a Linux caller.** That covers three
   read waits and three write waits. A noteintr exit that moved data returns
   the count, never `-EINTR` (prosecute (h)). `cons_kernel_writer_begin`
   keeps `caught_ok` false (prosecute (i)).
3. **The frozen mark.** `Thread.cons_frozen_unwound` is set at every
   frozen-path `NOTEINTR` exit:
   - the door park;
   - the vacate park;
   - the vacate re-take;
   - a slot wait taken because of `frozen_once`.

   `cons_input_read` consumes it at entry as `frozen_once`, and
   `proc_exec_drop_image_state` clears it. It is not copied by rfork.
   `frozen_once` is fed by `cons_caller_frozen()`. Your branch adds
   re-checks of `cons_caller_frozen()` under `g_cons.lock` in
   `cons_set_mode_cmd` and `cons_rx_accept`; those are independent of the
   mark, so keep both. If you change what `cons_caller_frozen()` means, every
   frozen-path unwind must still set the mark. The retry must re-take the
   slot by waiting, never through the busy guard's `-1`
   (`cons.caught_note_frozen_retry_waits`).
4. **The process write taps the renderer drain once, after its pushes, with
   what went out** (`cons_emit_bulk_wait`). This is the operator's vote of
   2026-10-05 (`dec-2026-10-05-console-mirror-tap-order`, ARCH 23.5.2 and
   25.4 LS-8 item (f)). Do not move the tap back before the push or into the
   push loop:
   - Tapping first shows a short write's tail twice on the renderer.
   - Tapping per push tears the chunk when a peer's unit lands between two
     pushes (`cons.congested_write_whole_in_drain`).

   Echo and diagnostic lines still tap first, and the silenced and capture
   branches tap the whole chunk.
5. **`wait_pid_for` parks with `sleep_noteintr`** and returns
   `WAIT_PID_NOTEINTR` (`-2`) for a caught note, with nothing reaped.
   `viv_wait4` maps it to `-EINTR` before its `ECHILD` arm. The native
   `sys_wait_pid_handler` never receives it, because the predicate is false
   for a native caller. Your branch adds no caller; a new one a Linux call
   reaches must handle `-2` rather than treat every negative as "no child".
6. **The pipe's elected-reader rule.** In `pipe_block_locked`,
   `caught_ok = !(t && t->stop_no_park) || t->recv_caught_ok`, which is
   srvconn's rule for an elected 9P reader whose client did not opt its
   receive in.

## Where it is written down

- Scripture: ARCH 8.8.3 (`5d28b427`, `94ba5327`) and ARCH 23.5.2 / 25.4
  (`00484bb6`), VIVARIUM 6.22.
- AUDIT-TRIGGERS rows 54, 55, 57, 65, 106, 112, 113 and 158.
- `specs/poll.tla`: the caught note, with three new buggy cfgs.
- Dossiers: `sub-kernel-cons`, `-thread`, `-pipe`, `-poll`, `-torpor`,
  `-rendez`, `-notes` and `-vivarium`.
- The change note `chg-2026-10-05-signal7-list`.
- Regressions:
  - the `*.caught_note_*` kernel tests (rendez, torpor, cons,
    pipe_blocking, poll);
  - `cons.short_write_mirrors_what_went_out`,
    `cons.caught_note_frozen_retry_waits`,
    `cons.congested_write_whole_in_drain`;
  - viv-pheno-probe L311-L318.
