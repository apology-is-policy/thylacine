# Handoff 047 -- a blocking 9P reader unwinds at any byte (to Astra)

**From**: main, 2026-10-07. **To**: Astra (and Corona, who works from the same
base). **Why you**: this landing changes the scheduler's die-checks and stop
detour (`kernel/sched.c` `sleep_common` / `tsleep_common`), deletes a
`thread.h` helper, changes what the elected 9P reader does when its Proc dies,
stops or catches a note, and adds a kernel compile flag. It also carries aux-3
(61c71525f, merged for one shared gate). A trial merge (`git merge-tree`) of
the landing tip into codex/astra (`5ff62b788`) and corona/async-memory
(`84dc0f9b0`) conflicts in five files more than a merge of main does:
`kernel/devctl.c`, `kernel/devsrv.c`, `kernel/srvconn.c` and the territory and
devsrv dossiers -- all but a comment hunk in srvconn.c are aux-3's (9P
counters on a row's two ends).

## What changed

The operator voted (2026-10-06, `dec-2026-10-06-seam90-unwind-any-byte`) to
close `seam-90-hung-server`. Read `chg-2026-10-06-seam90-close` for the whole.

- `reader_recv_frame` (9p_client.c) sets `stop_no_park` and `stop_unwinds` for
  the WHOLE blocking recv and clears both on exit. `do_reader_recv_frame` no
  longer writes either latch. The partial frame stays in `c->rx_got`; the next
  reader resumes it.
- `thread_reader_blocks_death` (thread.h) is DELETED. The four die-checks in
  sleep()/tsleep() are plain `sleep_death_pending(t, unwind)` /
  `thread_die_pending(t)`; the caught-note predicate
  (`thread_caught_note_unwinds`, notes.c) no longer refuses a reader.
- The stop detour in sleep()/tsleep(): `stop_unwinds` set -> unwind
  (SLEEP_INTR / TSLEEP_INTR); otherwise PARK (`proc_stop_sleeper_park`). The
  block-through fall-through for `stop_no_park && !stop_unwinds` is gone.
- `stop_no_park` now only marks "this is the reader's recv": the caught arm
  latches `note_unwound` for it, and `pipe_block_locked` reads it with
  `recv_caught_ok`.
- `notes_deliver_tail` returns false after it sets up a native Plan 9 handler
  (it fell off its end since bbc7ab90a), and `-Werror=return-type` is in
  `THYLACINE_KERNEL_C_FLAGS`: a non-void function that can fall off its end no
  longer builds. Aux is adding `-Werror=shorten-64-to-32` beside it.

## Rules the merged code must keep

1. No die-check, stop detour or note claim may consult a reader latch to defer
   an unwind. A blocking 9P reader unwinds at any byte.
2. Every transport recv reachable from `do_reader_recv_frame` sleeps only
   before it copies: it returns the bytes it copied, or an error having copied
   none. A recv that can copy and then fail loses bytes the client counts on.
3. Only `do_reader_recv_frame` writes `c->rx_got` (client init aside): 0 at a
   frame boundary, the bytes read on every other exit.
4. A new blocking caller holding `reader_active` goes through
   `reader_recv_frame`, never `do_reader_recv_frame` directly.
5. A kernel function returning a value returns it on every path; the build
   enforces this now.

## Tests that pin it

`rendez.reader_recv_unwinds_death` / `_death_sleep` / `_caught_note`;
`9p_srvconn_transport.reader_unwinds_mid_frame_death` / `_stop`;
`specs/check-reader-frame.sh` (every count pinned).
