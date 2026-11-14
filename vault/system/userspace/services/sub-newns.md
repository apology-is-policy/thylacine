---
id: sub-newns
type: sub
title: "newns -- the session starter: the session's namespace, then its program"
parent: moc-userspace
code: [usr/newns/Cargo.toml, usr/newns/src/main.rs]
audit: hard
guarded-by: [inv-i22, inv-i27, inv-i1]
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/MAC-RUNNER.md"]
created: 2026-11-14
updated: 2026-11-14
---
## Purpose

Start every session's program in a namespace that already holds the session's own mounts, made
as the session's user (`docs/MAC-RUNNER.md` 4.4, vote M13). The kernel stamps a `/srv` attach
with the principal of the Proc that opens the service, and login runs as SYSTEM, so a mount whose
files must show as the user's has to be made by a Proc that already is the user. newns is that
Proc: login spawns it as the user, it builds the namespace, then it becomes the session's
program. Plan 9's newns stands in the same place, between login and rc.

## Contract

- `newns <program> [args...]`. login runs `newns /bin/ut --home /home/<user> [--consctl-fd 3]`,
  or `newns /bin/halcyond --session --home /home/<user>` in the Halcyon session.
- It opens `/srv/mac`. That is a 9P-mode service, so the open is a fresh attach made with
  newns's principal, the user's. It mounts the attach's root at `/mnt/mac` (MREPL) in its own
  namespace and closes its descriptor, so the program inherits the mount and not the descriptor.
- With no `/srv/mac` (ENOENT: not on a Mac, or the driver posted nothing) it says nothing. Any
  other failure to open or mount is one line on stderr, `newns: /mnt/mac not mounted: <error>`,
  and newns goes on. An open with no app on the line fails at once, because the driver hangs it
  up ([[sub-mac-line]]).
- Then it execs `<program>` with `<args>` in place. If the exec fails it says
  `newns: <program>: <error>` and exits 127 (not found) or 126, and login sees the session end.
- The exec keeps everything login gave the session program. It keeps the Proc: the pid, the
  session, and the console owner, which the kernel records as the Proc. It keeps the identity,
  the caps and the perm bits; exec clears only the note-table and self-managing-notes bits
  (`proc_exec_drop_image_state`). It keeps every descriptor spawn installed, since none is
  close-on-exec (the spawn thunk installs them with `handle_alloc`). It keeps the memory
  account, which is membership rather than a handle, the one-way seal, `/env`, and the
  namespace with the new mount.

## Mechanism

- login makes the mount point `/mnt/mac`, because it owns the pool root and newns, as the user,
  cannot create there. The mkdir is idempotent because the pool persists. A mount needs only
  search permission on its path (`sys_mount_for_proc` checks no owner), so the user mounts over
  SYSTEM's empty directory, in its own namespace only.
- login routes `mac` as a resident name of the session's `/srv` registry, so the open reaches the
  boot registry's post, and no program of the session can post a `/srv/mac` of its own to stand
  in for the line.
- The namespace is copied at spawn, so the session's program and everything it starts have the
  mount, and nothing outside the session does.

## Data structures

None.

## Concurrency

One thread; the exec is its last act.

## Invariants enforced

- I-22: newns holds no authority of its own. It runs with the user's identity, the shell's cap
  mask and the perms login granted, and adds and drops none of them.
- I-27: CONSOLE_OWNER belongs to the Proc and stays with it; newns never gains console attach.
- I-1: the mount is made in the session's namespace, never in login's or another session's.

## Error paths

See the Contract: a missing service is silent, any other mount failure is one line and the
session starts anyway, and a failed exec ends the session.

## Performance

One extra exec per login and, with the line up, one attach. Not measured.

## Prosecution

The kernel facts this rests on were read for the build:
- exec keeps `proc_flags` except the two note bits (`kernel/proc.c`
  `proc_exec_drop_image_state`);
- it closes only close-on-exec descriptors, accounts and private rings (`kernel/handle.c`
  `handle_close_on_exec`);
- spawn installs inherited descriptors without close-on-exec (`kernel/syscall.c`, the spawn
  thunk's `handle_alloc`);
- the mount checks no owner.

`ls-mac-line` covers it in two legs:
- leg 2: the attach made at login carries the principal `id` names, `cat /mnt/mac/snarf` reads
  the app's text, and `stat` shows the user as the owner;
- leg 4: with the app gone, the session's mount fails.

The scenario ran against a scripted guest (aux-gate `scendry-mac`): 2 runs green, and 11 faults
red, each at its own check. It has NOT YET RUN IN A GUEST, and newns is not yet audited (round
R5).

## Seams

- [[sub-stratum-session]] holds login's side: the spawn, the resident `mac`, the mount point.
- The per-user attach of the pool (`docs/IDENTITY-DESIGN.md`, M7) can join the namespace here.

## Caveats

- A session that starts while the app is away has no `/mnt/mac`. A remount mends only the
  program that remounts and its later children (4.4, "When the app goes away").
- Between the spawn and the exec the Proc is newns. A Ctrl-C in that window reaches newns, the
  console owner, as it would reach ut before ut handles it, and ends the session.

## Provenance

Written 2026-11-14 (aux) with the first build, from vote M13
([[dec-2026-11-14-mac-session-starter]]).
(generated -- incoming `touched` backlinks, newest first; never hand-written)
