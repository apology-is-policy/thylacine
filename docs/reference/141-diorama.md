# The diorama — the synthetic Linux world (`/sbin/diorama`)

**As-built at VIVARIUM V-4a.** Design: `docs/VIVARIUM.md` §6. Invariant: **I-43**
(a phenotype confers ABI shape, never authority).

## Purpose

A read-only 9P server that presents Thylacine's native introspection surfaces in
the shapes an unmodified Linux binary expects — Linux's `/proc`, rebuilt out of
Thylacine's `/proc` and `/ctl`. It is the *world* half of Phase 8's fourth pole;
the *ABI* half is the phenotype (V-1/V-2).

Native `usr/diorama`, `no_std` libthyla-rs, device-less — the ptyfs/corvus tier,
not a warden-bound driver. joey spawns it with `T_SPAWN_PERM_MAY_POST_SERVICE`;
it runs a selftest, then posts `/srv/diorama`.

## The rule that defines it

> The diorama renders **only** from sources the calling Proc could already reach
> natively. It is a reformatter, never an authority.

That is what makes I-43 structural rather than review-dependent: the kernel's
existing gates run unchanged underneath, so a read the kernel would refuse is a
read the diorama cannot serve. Two corollaries, both load-bearing:

- **Never** source a file through a path a native Proc could not use.
- **Never** accept an answer supplied by the client (a client-named pid is the
  canonical version of this mistake).

When a file has no native source, the fix belongs in the **kernel**. That is what
`Proc.exe_path` (V-4a-0) and `srv_peer_info.pid` (V-4a-0b) are — see
`VIVARIUM.md` §6.5–§6.7, which record both and flag `/self/cwd` + `/self/maps` as
the same shape for V-4b.

## Who is `self` — the sharp edge

`/proc/self/…` is a question about the *caller*. The diorama answers it with
`SYS_SRV_PEER`, which reports the peer of the **9P connection** — i.e. the Proc
that opened it, which for a mounted tree is the **mounter**.

So `self` means *"the Proc that owns this connection"*, and that is correct only
for a **per-Proc or per-container mount**. A shared mount — the way joey mounts
`/net` and `/dev/pts` once for every session — would silently report the
*mounter's* identity to every reader. (Same shape as cfg-3, where the shared
`/dev/tapestry` mount's peer is joey rather than the session.)

This is not a limitation to engineer around; it is why §6 says the diorama is
"mounted into the container's territory only". A vivarium sets up a per-container
mount anyway (V-7), and a Proc's territory is private, so a Proc that mounts the
diorama itself gets itself as `self` by construction.

**joey therefore does not mount it.** It creates the mount point `/dio` and lets
each client mount privately. `/bin/diorama-probe` does exactly that, which is what
makes the V-4a gate meaningful — joey is blob-loaded and has no recorded exe at
all, so a joey-mounted `/self/exe` would read empty.

## The tree (V-4a, Tier 1)

| Path | Content | Native source |
|---|---|---|
| `/self/exe` | the executable's path, **bare** (no NUL, no newline) | `/proc/<pid>/exe` (V-4a-0) |
| `/self/cmdline` | `argv[0]`, NUL-terminated | derived from `exe` |
| `/self/status` | `Name`/`Pid`/`Uid`/`Gid`/`Threads`/`VmRSS` | peer + `/proc/<pid>/status` |
| `/meminfo` | `MemTotal`/`MemFree`/`MemAvailable` in kB | `/ctl/memory` page counts |
| `/uptime` | `<up> <idle>` seconds | `CLOCK_MONOTONIC` |

Honest gaps, deliberately not faked:

- `cmdline` serves `argv[0]` only — a running Proc retains no argv (`SYS_SPAWN`'s
  is consumed at exec). `argv[0] == the path` is the universal convention and is
  *derived*, not invented.
- `status` prints the same principal in all four Uid/Gid columns; Thylacine has
  one principal, not Linux's real/effective/saved/fs quartet.
- `uptime`'s idle field is `0.00` — there is no aggregate idle accounting here,
  and Linux itself reports 0 on some virtualized configurations.
- `MemAvailable` equals `MemFree`: without a reclaim model, any other number would
  be a fabrication.

Deferred to **V-4b** with their kernel prerequisites: `/self/maps`, `/self/cwd`,
`/proc/<pid>/…`, `/cpuinfo`, `/stat`.

## Read-only

There is no write path. `Twrite` returns `E_PERM`, and `Tlopen` refuses any open
requesting write access — so the refusal lands at `open()`, where a caller can act
on it, rather than at `write()`. That single decision removes most of the surface
a `/proc` would otherwise carry, and the probe gates it.

## Implementation notes

- **Static node table.** Every node is known at compile time and a qid path *is*
  the node index, so resolution can never dangle — no dynamic slots, hence none of
  the slot-reuse hazards netd and ptyfs must defend against (net-3d F1).
- **Bounded renders.** Every write goes through `Render::push`, which is
  cap-checked against `RENDER_MAX` (1024); an over-long render truncates rather
  than overruns.
- **Live peer, per use.** `t_srv_peer` is queried per read rather than cached at
  accept: `alive`/`pid` are alive-gated, so a peer that exited renders **empty**
  instead of a stale answer. The selftest pins this (`alive == 0` → empty).
- **Single-threaded.** One serve loop, no locks. The listener leaves the poll set
  when the connection table is full, so a pending 9th client parks the loop
  instead of busy-spinning it (the PTY-2e audit F4 finding).

### Trap: the Rgetattr security trio

`Rgetattr`'s `valid` mask **must** advertise `MODE | NLINK | UID | GID`. The
kernel's dev9p per-component X-search reads that trio, and an unfilled trio
**fails closed** — which presents as: `t_mount` returns 0, and then *every* open
under the mount is denied, with no error pointing at getattr. This cost a
debugging cycle here despite ptyfs carrying the same warning in its own
`h_getattr`. If a mounted tree is suddenly untraversable, check the valid mask
first.

## Tests

- `server::selftest()` — runs **before** the post, so a logic failure gates the
  boot: tree resolution (including `..`, cross-parent isolation, and walking into
  a file), render-buffer boundedness, decimal formatting, the key/value parser
  (including that a key must match at a *line start*, not mid-line), and the
  dead-peer-renders-empty property.
- `/bin/diorama-probe` — the in-guest gate, boot-fatal. Mounts the diorama itself,
  then asserts `/self/exe` == its own path, the `cmdline` NUL shape, the `status`
  `Name` basename, `meminfo`/`uptime` shape, and that a write-open is refused.
  This is the leg no unit test can reach: it exercises the kernel's `exe_path`
  record, the `srv_peer_info.pid` channel, peer resolution, and the 9P path in one
  read.

## Status

V-4a complete. Kernel byte-unchanged by this chunk (the two kernel prerequisites
landed separately as V-4a-0 / V-4a-0b).
