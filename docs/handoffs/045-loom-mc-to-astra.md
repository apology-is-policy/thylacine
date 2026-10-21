# Handoff 045 -- waiters fan in, and the aux-3 work it carries to main (to Astra)

**From**: main, 2026-10-06. **To**: Astra (and Corona, who works from the same
base). **Why you**: this landing moves main past aux-3 `ab26a9805` (served links,
the CPU-time gate) and changes how every 9P transport is read. codex/astra (tip
`5ff62b788` when this was written) edits files both touched. A trial merge of the
landing branch into codex/astra (`git merge-tree`) conflicts in 26 files; 22 of
them already conflict in a merge of main as it stood (9dc80bb37). The four new
ones:

- `kernel/include/thylacine/9p_client.h` (this chunk): the deadline pump API is
  gone (`p9_client_reader_pump_once`, `_pump_once_deadline`,
  `p9_client_recv_is_deadline_capable`); `p9_client_reader_pump_ready`,
  `p9_client_reader_hook` / `_unhook` and `enum p9_pump_result` replace it, and
  `struct p9_client` gains `rx_got`.
- `docs/manual/13-processes.md`, `vault/system/kernel/entry/sub-kernel-syscall-abi.md`
  and `vault/system/userspace/tools/sub-imperium.md` (aux-3's commits, not this
  chunk's): take aux-3's text and re-apply yours.

`kernel/9p_client.c` already conflicts with main; this chunk adds hunks around
the frame reader (`do_reader_recv_frame`, now resumable) and the pump.
Your `p9_transport_try_ops` (`p9_srvconn_progress_ops`, c2f462ba6) is a separate
type that never passes `p9_transport_init`, so the new mandatory ops below do
not apply to it; checked, not merged.

## What changed

LOOM.md 8.6 and ARCH 21.10 (scripture `78d6714b9`; the operator's vote "waiters
fan in"). A Loom ring's waiters read every 9P client it has an op on; the dev9p
poll kthread reads every QTPOLL client, with no cap. Read
`chg-2026-10-06-loom-multiclient` for the whole of it.

## Rules the merged code must keep

1. **Every `p9_transport_ops` supplies `recv_ready` and `recv_now`.**
   `p9_transport_init` refuses a table without either. `recv_ready(ctx, pw)`
   says a recv would not block, and files `pw` on the backend's readiness list
   atomically with the sample. `recv_now` reads what is waiting and NEVER
   sleeps (`P9_TRANSPORT_EAGAIN` when nothing is). The two must agree: when
   `recv_ready` says yes, `recv_now` returns bytes, 0 (EOF) or -1, never
   EAGAIN, or a pumper loops. `set_recv_deadline` and `recv_timed_out` are
   deleted; drop them from any table of yours.
2. **The partial frame is the client's.** `do_reader_recv_frame` resumes at
   `c->rx_got` and leaves what it read there when it returns without a whole
   frame. A new reader of the stream must go through it, never read into a
   buffer of its own (`haz-shared-stream-desync`).
3. **Only `p9_client_reader_pump_ready` reads for others, and only with
   `recv_now`.** No kthread may block in a recv: one kthread pumps every QTPOLL
   session, and any process can serve a 9P mount over pipes.
4. **One hook per client, never on both lists**: the role list while the role
   is held, the readiness list while it is free (`loom_role.tla`
   BUGGY_READY_HOOK_WHEN_HELD).
5. **A change to what a Loom waiter must drive bumps `drive_gen` under
   `l->lock`**: a CQE post, a completion's state update, an op linked in
   flight, a re-arm claimed (`loom_drive_moved_locked`).
6. **A closed transport is a dead one to a pumper** (`pump_ready` DEAD, the
   hook `-P9_E_IO`).

Open and owned, not yours to act on: `seam-90-hung-server`. A blocking sync
reader still finishes a frame before a death or stop unwinds it (ARCH 8.8.1.1,
voted); now that the partial frame persists, closing the seam is one rule
change, and it is the operator's decision.
