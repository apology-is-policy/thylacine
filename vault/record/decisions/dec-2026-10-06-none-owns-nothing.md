---
id: dec-2026-10-06-none-owns-nothing
type: dec
title: "A Proc running as none owns nothing but itself"
date: 2026-10-06
status: standing
decided-by: user-vote
affects: [sub-kernel-devproc, sub-kernel-devctl, sub-kernel-perm]
created: 2026-10-06
---
## Fork

[[dec-2026-10-06-9p-sessions-ends]] made a reader running as `none`
(PRINCIPAL_NONE) no end of a `/ctl/9p-sessions` row, and left the `/proc`
owner predicate's none as its own question. Reading the whole owner family
found it wider than enqueued. Every owner axis compared principals, so two
unrelated Procs running as none were one owner everywhere:
- each could kill the other through `/proc/<pid>/ctl` (I-26);
- each could debug the other whenever its caps covered the other's, which two
  bare none Procs always do (I-39);
- each could read the other's `environ`, `sched`, `imperium` and `cpu_ns`.

No in-tree program runs as none today, so the exposure is a future pre-auth or
network server. It is an invariant-bearing change to I-26 and I-39, so it went
to the operator.

The question offered: Plan 9's `nonone` in full; the owner checks only (the
six per-Proc files every reader may read stay readable to none); the read
checks only (kill and debug unchanged); or record the residual.

## Decision

The operator voted on 2026-10-06: **Plan 9's nonone**. A none Proc reaches no
other Proc's state:
- No owner axis admits it for any Proc but itself: kill, debug, and the
  owner-or-hostowner reads (`environ`, `sched`, `imperium`, the `cpu_ns` of
  `status` and `/ctl/procs`).
- `status`, `cmdline`, `ns`, `exe`, `cwd`, `maps` and the read side of `ctl`
  refuse it for any Proc but itself.
- `/ctl/procs` lists it only its own row.
- A capability axis still admits it, as Plan 9 exempts eve: `CAP_HOSTOWNER`
  for all of the above, `CAP_KILL` for kill, `CAP_DEBUG` for debug.

Decided under the operator's grant, as the vote's own scope:
- kill gains a self arm (the caller itself), which every other owner predicate
  already had, so a none Proc can still kill itself through its own `ctl`;
- a none parent keeps `SYS_POSTNOTE` to its own children, since that is a
  parent test, never an owner test;
- what stays visible is what Plan 9 leaves visible: a pid's existence under
  `/proc` and its stat (owner, mode). `getpgid` and `getsid` also stay
  ungated: they name a group, carry no state, and the kernel's own pty hangup
  path asks them on a server's behalf;
- every refusal answers -T_E_ACCES, the wall's included, and so does every
  other `/proc` authority refusal and `/ctl`'s two gated leaves (`kernel-base`,
  `kstack`). ERRORS.md binds a denial to EACCES, and the bare -1 these
  answered before reads as EPERM in pouch and Go. The other failures (no such
  Proc, not ALIVE, not stopped, a claimed slot) stay -1;
- kill, suspend and debug attach ask the caller's authority before the
  target's liveness, so a refused caller reads EACCES for a dying target as for
  a live one: no liveness bit about a Proc whose status it may not read;
- `/ctl/9p-sessions` shows a none reader no row at all. A none reader is no end
  of any row ([[dec-2026-10-06-9p-sessions-ends]]), and the rows name other
  Procs' connections: peer pid, label, msize, mode, liveness. The first draft
  left them readable, as Plan 9's `/net/*/status` files are; the audit round
  showed that leaf alone told a none reader about other Procs, so it follows
  the rule.

## Rationale

Plan 9 wrote the rule down. Bell Labs `port/devproc.c`, kept by 9front:

    /*
     *  none can't read or write state on other
     *  processes.  This is to contain access of
     *  servers running as none should they be
     *  subverted by, for example, a stack attack.
     */

`nonone()` is called at the open of every other Proc's ctl, mem, args,
noteid, status, wait, regs, fpregs and syscall files. Thylacine's all-pids
posture is Plan 9's, and Plan 9's all-pids posture never covered none.

Linux shares one `nobody` uid, and two daemons running as nobody can signal
and ptrace each other. That is the reason systemd's DynamicUser allocates a
uid per service and OpenBSD gives every daemon its own user. The kernel
cannot assume every future none server will get a principal of its own, so
the wall belongs in the kernel.

The owner checks alone would leave `maps` readable to none: every other
process's memory layout, an ASLR break that a subverted network-facing server
can use against local targets. They would also leave `cmdline`, where
secrets are sometimes passed. Changing only the reads would leave kill and
debug, the two control axes, open between unrelated none Procs. Recording the
residual leaves all of it.
