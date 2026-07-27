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
| `/self/cwd` | the working directory, **bare** | `/proc/<pid>/cwd` (V-4b-1) |
| `/self/maps` | the address space, Linux column layout | `/proc/<pid>/maps` (V-4b-2) |
| `/self/cmdline` | `argv[0]`, NUL-terminated | derived from `exe` |
| `/self/status` | `Name`/`Pid`/`Uid`/`Gid`/`Threads`/`VmRSS` | peer + `/proc/<pid>/status` |
| `/meminfo` | `MemTotal`/`MemFree`/`MemAvailable` in kB | `/ctl/memory` page counts |
| `/uptime` | `<up> <idle>` seconds | `CLOCK_MONOTONIC` |

Honest gaps, deliberately not faked:

- **`exe` is a regular file, where Linux has a symlink** — and this one is a real
  fidelity gap, not a cosmetic one. On Linux `readlink("/proc/self/exe")` yields
  the path while `open()`+`read()` yields the executable's *bytes*; here the file's
  *contents* are the path, so `readlink()` fails with `EINVAL`. LLVM's
  `getMainExecutable` — the consumer VIVARIUM §6.3 names as the reason this file is
  load-bearing — calls exactly `readlink`, so it still falls through to `argv[0]`.
  The blocker is structural: **there is no `SYS_READLINK`**; Thylacine has no EL0
  symlink surface (the 9P `Treadlink` ops serve only the kernel's own client and
  Loom). The likely fix is to translate `readlink()` in the *phenotype* rather than
  grow a symlink surface. Tracked; §6.3 carries the correction.
- `cmdline` serves `argv[0]` only — a running Proc retains no argv (`SYS_SPAWN`'s
  is consumed at exec). `argv[0] == the path` is the universal convention and is
  *derived*, not invented.
- `status` prints the same principal in all four Uid/Gid columns; Thylacine has
  one principal, not Linux's real/effective/saved/fs quartet.
- `uptime`'s idle field is `0.00` — there is no aggregate idle accounting here,
  and Linux itself reports 0 on some virtualized configurations.
- `MemAvailable` equals `MemFree`: without a reclaim model, any other number would
  be a fabrication.

Deferred with their kernel prerequisites: `/cpuinfo`, `/stat`,
`/self/{fd,environ,auxv}` (see VIVARIUM §6.10 — `fd` is blocked on the #66c
handle-table lifetime, and `environ`/`auxv` each need a kernel source).

### The rest of the tree (V-4b-3)

| Path | Content | Native source |
|---|---|---|
| `/<pid>/{exe,cmdline,status,cwd,maps}` | the same five files, for any live Proc | `/proc/<pid>/*` |
| `/sys/kernel/ostype` | `Linux` | — the phenotype (see below) |
| `/sys/kernel/osrelease` | `6.1.0-thylacine` | — the phenotype |
| `/sys/kernel/version` | `#1 SMP Thylacine` | — the phenotype |
| `/sys/kernel/hostname` | `(none)` | matches native `uname -n` |

The root also **enumerates** the live pids (from `/ctl/procs`' first column), so a
`ps` that readdirs `/proc` sees the numeric dirs Linux puts there.

**Per-pid needed no kernel work**, which is worth stating because V-4b-1 and
V-4b-2 both did: `/self` was *always* a per-pid render with the pid supplied by
the connection's peer instead of by the path. The pid was a parameter from the
start, so the sub-chunk is a generalization, not a new mechanism. The renderers
cannot tell a `/self` read from a `/<pid>` read and must not need to.

Two details that are not obvious:

- **`/<pid>` resolves only for a LIVE Proc**, decided by a native `O_PATH` open of
  `/proc/<pid>` — not by a table this server keeps. That makes a dead or
  never-existent pid an honest `ENOENT`, which is how every Linux consumer detects
  that a process is gone. `parse_pid` also refuses leading zeros (Linux ENOENTs
  `/proc/01`), so one Proc never gets two names.
- **`status` gets its ids from two different places on purpose.** `/self` uses the
  kernel-stamped `srv_peer_info` — unforgeable, no parse, and the V-4a-0b
  mechanism this server's identity story rests on. A per-pid read has no such
  channel and parses `principal:`/`gid:` out of the native render. Same kernel
  fields either way (devproc's `format_status` prints `p->principal_id` /
  `p->primary_gid`, exactly what `srv_peer_info` stamps), so they cannot disagree;
  unifying on the parse would have traded provenance for symmetry.

**Visibility.** The five per-pid files are `0444` with
`devproc.perm_enforced == false` — Plan 9's all-pids-visible posture — so the
diorama serves exactly what native `/proc` serves, to exactly the same readers.
What it does *not* do is scope the pid set to a container, because nothing does
natively yet: `/ctl/procs` lists every Proc on the box. That containment question
is owed at **V-7** and belongs to native `/proc` first — scoping it here alone
would be theatre, since a contained Proc that can reach native `/proc` reads
around us. Recorded in VIVARIUM §7.1.

### `/sys/kernel` — the fourth source (V-4b-3)

These are the first diorama files that do **not** reformat a native source, and
the distinction matters more than the files do. §6.2 exists to stop the diorama
becoming an *authority* — serving what the native surface would refuse. A constant
carries no information about the system, so there is nothing to leak; what it
describes is the phenotype, this server's own property.

> A value **derived from kernel state** needs a native source, no exceptions. A
> **constant declaring which ABI the caller is looking at** is the phenotype
> speaking about itself.

`osrelease` is the one with teeth: glibc-linked programs parse it and some refuse
to start below a minimum kernel, so `6.1` clears every such check while
`-thylacine` keeps the string honest (Linux's own convention carries local
suffixes). The stated tradeoff — a program *could* version-gate a feature on the
number — is the better of two bad options, since declaring low makes those same
programs refuse to run at all.

`hostname` is deliberately **not** in that category: it would be system state if
Thylacine had any. It does not, so the render is the answer the native tool
already gives (`uname -n` hardcodes `(none)` for the same reason) — one answer for
the system, not two. That it also matches real Linux with no hostname set is a
happy accident, not the justification.

### `/self/maps` — where the Linux shape lives (V-4b-2)

This is the first file whose native and Linux renderings differ enough to force
the question of which layer speaks Linux. The answer is the one the design
implies: **the kernel stays Thylacine, the diorama does the phenotype.** The
kernel emits a native six-column table; `render_self_maps` translates it. Letting
the kernel emit Linux's shape would be phenotype leaking into the kernel — the
inversion VIVARIUM exists to prevent. (`status` set the precedent: native
`key: value` in, `Name:`/`Pid:`/`Uid:` out.)

```
00400000-00402000 r-xp 00000000 00:20 32                    /bin/diorama-probe
00402000-00403000 rw-p 00000000 00:00 0
7feff000-7ff00000 ---p 00000000 00:00 0
7ff00000-80000000 rw-p 00000000 00:00 0                    [stack]
c0000000-c0001000 r--p 00000000 00:00 0                    [vdso]
```

Three translations were judgement calls, recorded in `VIVARIUM.md` §6.8 and at
the call sites:

- **`dev` renders `00:<devno>`.** Thylacine's `devno` is flat — no major/minor.
  The `00` major is not fabricated: Linux uses `00:xx` for every filesystem with
  no backing block device (tmpfs, and 9P mounts specifically), which is exactly
  what a Stratum mount is.
- **The pathname comes from `/self/exe`, under a stated premise** — at v1.0 the
  only FILE Burrows in an address space are the exec'd binary's segments
  (`burrow_create_file` has one caller, from exec; there is no file-mmap
  syscall). **When a file-mmap surface lands, the kernel line must carry a path
  and this branch must read it instead of substituting `exe`.**
- **`[vdso]` is emitted, but Thylacine's vdso is a read-only *data* page** (the
  clock struct), so it renders `r--p`, not Linux's `r-xp` code vDSO. The tag
  still helps the consumers that look for it (sanitizers, which use it to
  *exclude* the region), and nothing goes looking for an ELF object there:
  Thylacine publishes no `AT_SYSINFO_EHDR`, only the private `AT_VDSO_CLOCK`.

A guard VMA is emitted, never hidden — `---p` with no pathname is byte-for-byte
how Linux shows a `PROT_NONE` guard page, and dropping the row would make the map
claim the range is free.

A malformed native row is **skipped**, never half-rendered: `maps_row` returns
false and the caller rewinds to the row boundary. `parse_hex` rejects rather than
coercing, because a coerced `0` would render a plausible-looking row for a line
the translator did not understand.

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
  V-4b-3 adds: the `/sys/kernel` tree and its renders, the per-pid qid encoding
  (round-trip, and that no static index can ever look like a pid node), the
  per-pid walk, `parse_pid`'s rejections, the `/ctl/procs` pid-list parse
  including the header skip, and the mid-line `gid:` parse.
- `/bin/diorama-probe` — the in-guest gate, boot-fatal. Mounts the diorama itself,
  then asserts `/self/exe` == its own path, the `cmdline` NUL shape, the `status`
  `Name` basename, `meminfo`/`uptime` shape, and that a write-open is refused.
  This is the leg no unit test can reach: it exercises the kernel's `exe_path`
  record, the `srv_peer_info.pid` channel, peer resolution, and the 9P path in one
  read. V-4b-3 adds the legs that need a *live* pid: reading its OWN numeric dir,
  checking the per-pid `Uid` against `getuid()` (the only place the native id
  parse runs), `ENOENT` for a pid that cannot exist, finding itself in the root
  readdir, and the `/sys/kernel` values.

Every V-4b-3 mechanism was **revert-probed**, each failing at its own leg: dropping
the existence check → `selftest FAIL: resolved a nonexistent pid`; disabling the
per-pid id parse → `FAIL per-pid status has no Uid`; skipping the root's pid phase
→ `FAIL root readdir did not list our own pid`.

## Status

V-4a + V-4b complete except `/self/{fd,environ,auxv}` (VIVARIUM §6.10). Kernel
byte-unchanged by V-4a and V-4b-3; the V-4b-1 / V-4b-2 kernel sources landed with
their own sub-chunks.
