# 33 — devctl (/ctl) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-devctl-absorb`). The
`/ctl` device renders kernel state most of which is *owned elsewhere*, so it
redirects to the dossiers that hold each piece:

- the **`/ctl` device itself** — the leaf table, the default-allow gate and the
  `kernel-base` `CAP_HOSTOWNER`-no-owner-axis special case, the overflow-bounded
  process-list lock hold, the offline-CPU short row, and the `/ctl/procs` STATE
  column (job-stop renders `STOPPED`; the debugger's `debug_stop_req` is
  deliberately hidden as its private I-39 view):

      vault/system/kernel/introspection/sub-kernel-devctl.md

- the **`/ctl/cons` counter semantics** — the receive back-pressure taxonomy
  (a raw byte or cooked flush refused for room is back-pressure, not loss; a
  byte past the line limit is a real drop; the third counter that stays zero as
  an invariant witness), the transmit room-waits and the drain's drop-oldest:

      vault/system/kernel/console-gfx/sub-kernel-cons.md

- the **`/ctl/9p-sessions` diagnostic** — the ring conservation law and the
  wedge discriminator:

      vault/system/kernel/srv/sub-kernel-srvconn.md

**What this file got WRONG or MISSED by the time it was absorbed** (the reason
the dossiers are written from the code):

- It reproduces per-leaf **render layouts** (the `/ctl/procs` column order, the
  `/ctl/cons` field list, the `/ctl/9p-sessions` roster) — append-only by
  contract (the userspace reader matches the leading tokens positionally), so
  the byte layout is the code's to pin (`kernel/devctl.c`) and the dossiers
  state the semantics rather than freezing a column set that grows.
- Its diagnostic content is split across the three dossiers above because the
  `/ctl` device only *renders*: the counter meanings belong to the subsystem
  that owns each counter (cons, srvconn), not to the render surface.
