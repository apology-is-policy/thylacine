---
id: dec-2026-09-28-ns-session-root-names
type: dec
title: "/proc/<pid>/ns names a 9P session root by the file its session came over"
date: 2026-09-28
status: standing
decided-by: user-vote
affects: [sub-kernel-territory, sub-kernel-ninep-dev9p, sub-kernel-syscall-dispatch, sub-coreutils-presenters, sub-haul]
created: 2026-09-29
---
## Fork

`/proc/<pid>/ns` rendered every 9P session root's source as `/`: login's home
read `mount /home/michael /`, and the shell's mount of a posted Haul service
`mount /tmp/x / remote`. That is the name every session root is born with
(`dev9p_attach_client`, #66). It is right for the root as a namespace root
(joey pivots to one), but on a mount line it reads like the system root and
says nothing about which session is mounted there. The LR-1 audit (F1) found
it. The rendered text of `/proc/<pid>/ns` is an interface, so the choice went
to the operator, in one batch of four questions asked after FL-1 landed.

## Research

- Plan 9 (`devproc.c`, the ns reader): a mount prints as
  `mount [-flags] <name> <mount point> [spec]`, where the name is the `/srv`
  entry the mount's channel is posted under (`srvname`), else the channel's
  own path. The file the session came over names the mount, never the tree's
  root: a mount over a pipe reads the pipe's name (`#|/data1`), one over a
  network connection its data file.
- Linux: `/proc/<pid>/mountinfo` carries a mount-source field beside the mount
  point (the device, or `server:/export` for NFS), never `/` for a remote root.
- Tree facts: `dev9p_attach_client` seeds every session root `/`; the stalk
  adoption arm transplants an opened `/srv` path onto a 9P-mode service's root,
  so `/net` already read `mount /net /srv/net`; `territory_format_ns` renders
  `mount <pt> <src>[ flags]` with the source Spoor's name, else `#<dc>`; I-33
  makes every name display only, and a pivot never re-stamps a published Spoor.

## Options

1. **The file it came from** (recommended): Plan 9's form,
   `mount /home/michael /srv/home-michael`; a Haul mount names its dialed
   connection. Stamped once at the mount from the fd's own name (display only,
   I-33); the first two fields keep their meaning; the ns and stat consumers are
   updated.
2. **The file plus the aname**: as 1, plus the attach name as a marked suffix
   (`spec=ds:michael`), so two mounts of one server with different anames read
   apart; one more field for parsers.
3. **`#9` for a session root**: the device spec, like every other device root
   (`mount /home/michael #9`); it says the source is a 9P session, not which one.
4. **Keep `/`** and document it.

## The call

Option 1 (operator, 2026-09-28 ~18:30Z). As built, reading "from the fd's own
name": the name is the transport file's own. `SYS_ATTACH_9P_SRV` takes its
connection's namespace name, which the stalk adoption arm gave the connection
when it was opened (`/srv/home-michael`; `/srv/NAME` for the shell's mount of a
posted Haul service); `SYS_ATTACH_9P` takes its transmit fd's. A file with no
name contributes its device spec. Haul's private form attaches over two pipes,
so its line reads `#|`: the TCP connection Haul dialed belongs to Haul, not to
the kernel's session, so "a Haul mount names its dialed connection" holds for
the post form (the posted service is that connection, published) and reads `#|`
for the private one. The name rides the root's `dev9p_priv` (`origin`), stamped
by the attach handler before the root is published; the root keeps its own name
`/`, so a pivot to a session root and a bind of the tree read as before (a
bind of `/` at `/n` still reads `mount /n /`). The attach name is not rendered.

## Rationale

It is Plan 9's form and the only option that answers "which session is this"
from the line itself. The line keeps its shape (the source is one word, so the
first two fields keep their meaning), the name is display only (I-33: the
resolver never reads a name), and no syscall or wire changes: both transport
files are in hand at the attach. `#9` says no more than the device character
already does, and `/` keeps a line that reads like the system root.
