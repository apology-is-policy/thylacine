---
id: dec-2026-10-18-create-speaks-for-a-session-end
type: dec
title: "A create through a 9P session is admitted only to a principal at one of its two ends"
date: 2026-10-18
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-dispatch, sub-kernel-ninep-dev9p, sub-kernel-ninep-client, sub-kernel-ninep-attach, sub-kernel-perm, sub-viv, sub-substrate-build]
created: 2026-10-18
---
## Fork

A file an ordinary user creates on the shared system pool is owned by
system. The pool is one 9P session that the boot chain attaches, and
Stratum stamps every create with its connection's `SO_PEERCRED` uid.
A probe on 2026-10-15 showed the result: uid 1000's `topfile` in
`/var/lib_tests/libxml2/build` was owned by system.

IDENTITY-DESIGN 9.7 M6 accepted the shared mount on one premise:
`perm_check` stops a non-system Proc from creating in a directory that
system owns. Three `tools/build.sh` sites stamp 1777 on the pool, and
the premise fails in every one of them:
- every `/vivarium` bundle's `rootfs/tmp`;
- ICU's `testdata/out`;
- the `lib_tests` build trees.

Through any of them a user can make system-owned files with whatever
content they choose. B-3e's libxml2 tests make nested directories
there, and fail because the first directory is system's.

## Research

- **Plan 9** never creates through another identity's attach. devmnt's
  Tattach carries `up->user`, and a cpu server builds each user a
  namespace (newns) that attaches the file server as that user.
- **Fuchsia and Genode** have no ambient uid. A component writes only
  through the storage capabilities it was handed, which is Thylacine's
  I-23.
- **Linux v9fs** with `access=user` attaches one fid per uid on a
  shared session, and the server trusts `n_uname`. Stratum ignores
  `n_uname` (IDENTITY-DESIGN 9.7).
- **A session already records both of its ends.** A `/srv` connection
  holds its opener's principal and its poster's
  (`SrvConn.peer_principal` and `.server_principal`, stamped at
  `kernel/devsrv.c:1060-1063` and `:434`), and `/ctl/9p-sessions` shows
  a row to either end.
- **A home's ends are system and the user.** login attaches each user's
  home as system (`usr/login/src/main.rs:929`). The home's server is
  the per-user proxy, which runs as the user and posts
  `/srv/home-<user>` itself (`:873-889`).

## Options

- **Per-user attach (Plan 9's newns).** Each login attaches the pool
  through its own Stratum connection, so the server stamps and checks
  the real principal. This is the faithful answer, and the largest: it
  touches login's namespace, the proxy's dataset scope, and caching per
  session.
- **The kernel stamps the creator.** After a create through a session
  that another principal attached, the kernel sends a Tsetattr to the
  creator. This is not atomic, and Stratum's own create checks still
  run as system.
- **Fail closed.** The kernel refuses such a create, and the three 1777
  stamps go.

The fail-closed rule was first put as "the creator must be the
attacher". That would refuse every user's create in their own home,
because login attaches the home as system. A second question offered
three rules:
- either end of the session;
- the attacher only, with login's home attach moved into the user's
  session;
- the attacher only, with an attach-as flag for the TCB, which would be
  an ABI change.

## The call

Fail closed now, with per-user attach recorded as the long-term shape
(operator vote, 2026-10-18 19:24Z). The rule is the first of the three
above: a create through a 9P session is admitted only to a principal at
either end of the session (operator vote, 19:35Z). IDENTITY-DESIGN 9.7
M7 states it.

## Rationale

The server attributes a create to the session, never to the Proc that
asked. It uses the connection's credential, or its own when it relays,
so a Proc outside the session's ends could create files as someone it
is not. Admitting only the ends means that every create through a
session is made by an identity the session speaks for:
- the pool's ends are both system;
- a home's server end is the user it stamps.

The refusal is honest where the old behaviour misattributed silently.
It needs no protocol or syscall change, and it restores the premise M6
relied on. Per-user attach remains the shape that would let users
create on the pool as themselves.
