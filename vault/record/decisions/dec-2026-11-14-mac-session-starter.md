---
id: dec-2026-11-14-mac-session-starter
type: dec
title: "The /mnt/mac attach carries the user: a session starter, newns, mounts it as the user and execs the session program"
date: 2026-11-14
status: standing
decided-by: user-vote
affects: []
created: 2026-11-14
---
## Fork

`docs/MAC-RUNNER.md` 4.4 (vote M10, [[dec-2026-11-13-mac-interop]]) had login mount `/srv/mac`
in each session, attached as the session user. Building the virtio-serial driver found that it
cannot happen as written. The kernel stamps a `/srv` 9P attach with the principal of the Proc
that opens the service (`kernel/9p_attach.c:406`, the opener taken at `kernel/devsrv.c:1345`).
login runs as PRINCIPAL_SYSTEM (`usr/login/src/main.rs:24`). Spawn copies the namespace
(`kernel/proc.c:1965-1971`), so a helper spawned as the user cannot mount into login's namespace
either. A mount login made would show every file owned by SYSTEM, and the user, who gets the
owner bits only, could neither read nor write it.

## Research

- Plan 9: `auth/login` becomes the user, calls `newns(user)`, which builds the namespace with
  the mounts made as that user, then execs `rc`.
- `docs/IDENTITY-DESIGN.md` (M7) already names per-user attach, Plan 9's newns, the faithful
  long-term shape for the pool.
- The tree: login spawns one session program, ut or halcyond, with `.identity(...)` and the
  shell's capabilities (`main.rs:1420-1467`), and `t_execve` replaces a Proc's image in place
  (LINEAGE L-2).

## Options

1. A session starter: login spawns `newns` as the user; it opens `/srv/mac`, mounts it at
   `/mnt/mac` in its own namespace, then execs the session program in place.
2. ut and halcyond each mount it as their first act.
3. A kernel attach flag letting a CAP_SET_IDENTITY holder stamp a chosen principal, so login
   keeps the mount.

## The call

The operator, 2026-11-14: option 1, as recommended (vote M13).

## Rationale

It is Plan 9's shape and needs no kernel interface. One program builds every session's
namespace, whatever program the session runs, and a later per-user attach of the pool can join
it there. The build must show that the spawn's perms (CONSOLE_OWNER, SESSION_HANGUP) and the
inherited consctl descriptor survive the exec.
