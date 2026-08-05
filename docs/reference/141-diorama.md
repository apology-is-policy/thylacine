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

## The root is the WORLD, not `/proc` (V-4c-1)

Since V-4c-1 the served root is the synthetic Linux **world**, and its children
are named for the mount points Linux expects. A container **binds** each where it
belongs; the diorama itself has no opinion about where it is mounted.

```
/            the world
/proc/…      -> bind at /proc
/sys/…       -> bind at /sys
```

**Why bind rather than a second `Tattach` aname.** 9P's own answer for one server
exporting two trees is an aname (Stratum's `ds:<name>`), and that route is
**closed here**: `devsrv_open_connect` attaches a 9P-mode `/srv` service with a
hardcoded *empty* aname, and `SYS_ATTACH_9P_SRV` — which does carry one — is
byte-mode-gated and rejects a 9P-mode conn (soundly: such a conn already has a
kernel-owned `p9_client` on its rings, and a second would interleave frames). An
aname would therefore need a new kernel ABI. Binding needs none: `SYS_MOUNT`
accepts **any readable Spoor**, a subdirectory included (`sys_mount_for_proc`
gates on `RIGHT_READ` alone). It is also the mechanism VIVARIUM §6.15 already
chose for `/dev`, so both halves of Tier 3 arrive the same way.

**A bound subtree is sealed.** The server records `/sys`'s parent as the world
root, so the obvious question is whether a container can walk `/sys/..` into
`/proc`. It cannot: `stalk` resolves `..` by **popping its own trail**
(`kernel/stalk.c`) and never sends a `Twalk("..")`, so `<mount>/..` lands on the
mount point's parent *in the client's namespace*. The server-side parent link is
unreachable through a bind. (Same property that contains `..` at `root_spoor` for
I-28.)

**Shorthand used below.** Outside the tree tables this doc writes `/self/maps`,
`/<pid>/environ` and the like — the *node* names, as the server's own code does.
Their served paths all carry the `/proc` prefix since V-4c-1. Kept deliberately:
those sentences are about renderers, and prefixing every one would add noise
without adding clarity.

## The `/proc` tree (V-4a, Tier 1)

| Path | Content | Native source |
|---|---|---|
| `/proc/self/exe` | the executable's path, **bare** (no NUL, no newline) | `/proc/<pid>/exe` (V-4a-0) |
| `/proc/self/cwd` | the working directory, **bare** | `/proc/<pid>/cwd` (V-4b-1) |
| `/proc/self/maps` | the address space, Linux column layout | `/proc/<pid>/maps` (V-4b-2) |
| `/proc/self/cmdline` | `argv[0]`, NUL-terminated | derived from `exe` |
| `/proc/self/status` | `Name`/`Pid`/`Uid`/`Gid`/`Threads`/`VmRSS` | peer + `/proc/<pid>/status` |
| `/proc/meminfo` | `MemTotal`/`MemFree`/`MemAvailable` in kB | `/ctl/memory` page counts |
| `/proc/uptime` | `<up> <idle>` seconds | `CLOCK_MONOTONIC` |

Honest gaps, deliberately not faked:

- **`exe` is a regular file, where Linux has a symlink** — a real fidelity gap, not
  a cosmetic one. On Linux `readlink("/proc/self/exe")` yields the path while
  `open()`+`read()` yields the executable's *bytes*; here the file's *contents* are
  the path. **CLOSED at V-4b-4, in the phenotype rather than here**: there is no
  `SYS_READLINK` and Thylacine has no EL0 symlink surface to grow (the 9P
  `Treadlink` ops serve only the kernel's own client and Loom), so the pouch
  boundary-line translates instead — `readlink` on the four link-shaped paths
  (`/proc/{self,<pid>}/{exe,cwd}`) is an open + read of exactly this file. **The
  diorama is unchanged and deliberately so**: serving these as regular files whose
  contents are a path is the *native* shape, and it is the phenotype's job to
  re-present it. See VIVARIUM §6.11 and `78-pouch.md`. Note the shim rewrites
  `self` → the caller's own pid, so it does not depend on this server's `self`
  resolution (nor on this server being mounted at all).
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

### The rest of the `/proc` tree (V-4b-3)

| Path | Content | Native source |
|---|---|---|
| `/proc/<pid>/{exe,cmdline,status,cwd,maps}` | the same five files, for any live Proc | `/proc/<pid>/*` |
| `/proc/sys/kernel/ostype` | `Linux` | — the phenotype (see below) |
| `/proc/sys/kernel/osrelease` | `6.1.0-thylacine` | — the phenotype |
| `/proc/sys/kernel/version` | `#1 SMP Thylacine` | — the phenotype |
| `/proc/sys/kernel/hostname` | `(none)` | matches native `uname -n` |

`/proc` also **enumerates** the live pids (from `/ctl/procs`' first column), so a
`ps` that readdirs it sees the numeric dirs Linux puts there.

Note `/proc/sys/…` is Linux's **sysctl** tree — a different thing from the `/sys`
below that happens to share a name. The node table disambiguates them by parent.

### The `/sys` tree (V-4c-1)

| Path | Content | Native source |
|---|---|---|
| `/sys/devices/system/cpu/online` | the online cpulist (`0-3`, `0,2-3`) | `/ctl/cpu` rows |
| `/sys/devices/system/cpu/possible` | the declared cpulist | `/ctl/cpu` `cpus:` header |
| `/sys/devices/system/cpu/present` | the declared cpulist | `/ctl/cpu` `cpus:` header |
| `/sys/devices/system/cpu/cpuN` | one dir per CPU | `/ctl/cpu` `cpus:` header |
| `.../cpuN/cache/index0/coherency_line_size` | the D-cache line, bytes (V-4c-2c) | `/ctl/cpu` `cacheline` column |

`/ctl/cpu` maps onto Linux's present-vs-online distinction exactly, and for free:
its `cpus:` header is `smp_cpu_count()` — every CPU the **DTB declared**, including
one that failed PSCI bring-up, which the kernel keeps counting (prowl-5 F2) — and
its per-row `offline` marker is precisely the online subset. So all three files are
*sourced* rather than guessed. On QEMU-virt the two sets coincide (PSCI never
fails there); the distinction is real on a board where it can.

An unreadable `/ctl/cpu` renders these **empty**, never `0` — a consumer reading
`0` would conclude one CPU exists.

**Deliberately absent, each for a stated reason:**

| Missing | Why |
|---|---|
| `kernel_max` | Linux sources it from a compile-time `NR_CPUS`; Thylacine's `DTB_MAX_CPUS` is on no EL0-readable surface |
| `cpuN/topology/` | core/cluster identity is not derivable from `MPIDR` alone on a board whose DTB the diorama does not re-read |

`cpuN/cache/…` **was** in this table at V-4c-1, on the grounds that `CTR_EL0` is
EL0-trapped (`SCTLR_EL1.UCT` is clear in `INIT_SCTLR_EL1_MMU_OFF`) exactly as
`MIDR_EL1` is. §6.17 took the other branch of its own rule — *give the kernel a
source* — so V-4c-2b reads `CTR_EL0` per-CPU at bring-up and surfaces the decoded
line size as a `/ctl/cpu` column, and V-4c-2c serves the leaf.

That makes a **third** instance of the per-field question VIVARIUM §6.15 raised
for `cpuinfo` and `stat`, with an identical shape. All three await **one**
decision — omit the unsourced fields, or give the kernel a source — made once
rather than piecemeal (V-4c-2). The `cpuN` dirs themselves are not fabrications:
each names a CPU the kernel reports, and their existence is what the legacy
"count the `cpuN` entries" path reads (busybox `nproc`, older glibc
`_SC_NPROCESSORS_CONF`). Modern consumers read the range files one level up,
which is why those were the ones worth sourcing first.

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
  `/proc/01`), so one Proc never gets two names. **Native devproc used to accept
  them**, so the two renderings of one system disagreed about which paths exist —
  a §6.2 coherence break, closed at V-4b-5 by teaching `parse_decimal` the same
  rule. The diorama's side was right all along and did not change.
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
  cap-checked against `RENDER_MAX` (4096); an over-long render truncates rather
  than overruns. (This line said 1024 until #72 — the constant moved and the
  prose did not, the same stale-comment class the V-4c-3 round found twice.)
- **`render` takes the read offset.** Its contract is one sentence for every
  node: *the bytes at `[off, ...)`*. Almost every renderer builds its whole file
  and `render` drops the first `off` bytes at the end, because their sources are
  bounded far under `RENDER_MAX`. `environ` is the exception and the reason the
  parameter exists — it renders that window directly (#72, below). `h_read` does
  no offset arithmetic of its own, so it cannot get the two cases confused.
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

V-4c-1 adds to the selftest: the world-root structure and the **sibling isolation**
(`/proc` must not reach the sysfs tree, `/sys` must not reach proc's), pids
resolving under `/proc` and *not* at the world root, the `/sys` walk, the cpu qid
encoding (round-trip, and that it can alias neither a static index nor a pid),
`parse_cpu_name`'s rejections, the `/ctl/cpu` count + online-mask parse including
the header skip and the **fail-safe** on a truncated render, the cpulist run
encoder, and that `kernel_max` does *not* resolve. It adds to the probe: the
cpulists read in place, `cpu0` walkable-but-empty, and the **composition proof** —
binding `/dio/sys` at a second path and reading identical bytes through the new
name, which is what V-7 depends on.

Every V-4b-3 mechanism was **revert-probed**, each failing at its own leg: dropping
the existence check → `selftest FAIL: resolved a nonexistent pid`; disabling the
per-pid id parse → `FAIL per-pid status has no Uid`; skipping the root's pid phase
→ `FAIL root readdir did not list our own pid`. Both V-4c-1 legs likewise: breaking
the online/offline distinction → `selftest FAIL: cpu online mask`; re-parenting a
sysfs node under `/proc` → `FAIL: /proc leaked the sysfs tree`. In every case
`/sbin/diorama` never posts and the boot fails, so these gates are boot-fatal
rather than decorative.

## Status

V-4a + V-4b **closed**: `environ` built (V-4b-6, below), `auxv` weighed and
deliberately **not built** (VIVARIUM §6.14 — zero live readers, and a `viv`-launched
binary receives its auxv on the stack by construction), `fd` blocked on #66c (the
#926 handle-table lifetime restructure, a kernel chunk).

V-4c-1 landed the `/sys` tree and the world-root restructure (§6.16). Kernel
byte-unchanged by V-4a, V-4b-3 and V-4c-1; the V-4b-1 / V-4b-2 / V-4b-5 / V-4b-6
kernel sources landed with their own sub-chunks.

V-4c-2 landed in three parts: **2a** decided the per-field question once (§6.17),
**2b** built the kernel sources, **2c** served them. The decision that covers all
of it: *give the kernel a source, per-CPU, in the kernel's own shape — and omit
only what has no truth to tell.*

**Owed:** **V-4c-3, the arc's focused audit** — V-4b-1..6 and V-4c all landed on
self-audit only, devproc/devenv are ARCH §25.4 trigger surfaces, and V-4c-2b put
two new counters on two more (the scheduler switch chokepoint and GIC dispatch).
The formal round is a **merge gate** for `gfx-4 → main`.


## The two Tier-1 stragglers (V-4c-2c)

| Path | Content | Native source |
|---|---|---|
| `/proc/stat` | `cpu`/`cpuN` jiffies, `intr`, `ctxt`, `btime`, `processes` | `/ctl/cpu` columns + `/ctl/sched` `created:` + the two clocks |
| `/proc/cpuinfo` | one block per **online** CPU, aarch64 shape | `/ctl/cpu` `hwcap:` + `midr` |

`Features` is the `AT_HWCAP` word, already carried in arm64 *uapi* numbering for
the exec auxv, so the names map one-to-one — the CF-4 chunk paid for the field
capability-detecting consumers actually parse. The four identity lines are
`MIDR_EL1`'s fields, which is exactly what Linux prints there.

**Omitted, each for a stated reason:** `BogoMIPS` (a calibration artifact of a
loop Thylacine does not run, and meaningless on Linux too); `procs_running` /
`procs_blocked` (a live state census).

**`CPU implementer` is legitimately `0x00` on some targets.** QEMU's TCG
`-cpu max` reports `MIDR_EL1 = 0x000f0510` — it deliberately does not claim to be
an ARM-implemented part — and that is the CPU `tools/test-interactive.sh` runs by
default. Do not treat a zero implementer as a fault or as evidence the register
was unread; ARMv8 requires only that `MIDR.Architecture` (19:16) read `0xF`, and
an *unread* record is all-zero. This cost a boot-fatal `EXTINCTION` when a kernel
test asserted the opposite: `tools/test.sh` runs HVF with `-cpu host` (Apple) and
the interactive harness runs TCG with `-cpu max`, so **a green `test.sh` is not a
sufficient gate for any assertion about a hardware register** — the two harnesses
disagree about the hardware.

### The one stated premise

`/proc/stat`'s `cpu` line reports **all non-idle time as `system`**, and this is
the single place in the arc where a value is knowingly not what its column
claims. Thylacine has no EL0-vs-EL1 time accounting anywhere, so the user/system
split has no source *and no material*. Unlike every other unsourced field, it
**cannot be omitted**: the columns are positional, so a missing middle column is
a *wrong* answer rather than an absent one. Every available choice is wrong for a
reader who wants the split; only the shape of the wrongness is ours to pick.

Utilization (`1 − idle/total`) — what essentially every consumer computes — is
exactly right either way, and a reader who specifically wants the split gets a
degenerate answer rather than a plausible fabricated distribution. `iowait`,
`steal` and `guest` are **honest** zeros (no block-wait accounting, not a guest).
Revisit the day per-mode accounting lands.

There *is* material for a plausible-looking split — attribute kernel threads'
`run_ns` to `system` and user threads' to `user` — and it is rejected for the
same reason a forwarded-IRQ-only counter cannot be called `intr`: it would be a
different quantity wearing the field's name.

### Trap: an early-returning arm makes a later one unreachable

`walk_child` already had an `is_cpu_node(dir)` arm that returned early. Appending
a second arm lower in the same function for the cache subtree compiled cleanly
and would have been **dead code** — the feature would have shipped resolving
nothing. The same arm also hardcoded `..` → `.../system/cpu`, correct only while
`cpuN` was a leaf dir; unchanged, `cache/..` would have skipped a level.

Both are one shape: **V-4c-1's code was right for V-4c-1's tree, and extending
the tree turns previously-correct shortcuts into bugs.** Neither is caught by
adding code and watching it compile.


## `/self/environ` -- and the first gated proxy (V-4b-6)

The environment as Linux serves it: NUL-terminated `NAME=VALUE` records back to
back, straight from the kernel's `/proc/<pid>/environ`. A **passthrough**, unique
among these renderers -- the kernel source is already in Linux's exact shape,
because Thylacine's `/env` has no flat form for it to be in a *different* shape
from. The block is synthesized for this purpose, so there is no translation to get
wrong and adding one would only add a place to lose bytes.

There is no local work left at all. Since #72 (below) the read is a **window**:
`render_environ` `t_pread`s the native file at the requested offset and pushes
the bytes verbatim, so a record straddling a buffer boundary simply continues at
the next offset, exactly as it does through the native file.

Until #72 it read the first 4 KiB and backed up to the last NUL — a
whole-record trim that was locally correct and globally the bug, because
everything past the trim was *gone*, not deferred. `trim_to_last_record` is
retired with the cap that made it necessary.

### Why it is `/self` only

This is the one asymmetry in the tree, and it is the whole of section 6.2.

`/proc/<pid>/environ` is owner-or-`CAP_HOSTOWNER` (unlike the `0444` siblings),
because nothing else in the system discloses another Proc's environment. That gate
keys on the **reader** -- which is this server, not its client.

* **`/self/environ` is sound.** The target is the connection's own peer, so a read
  the kernel allows is a read of the CLIENT's own environment, which the client
  could have performed itself. A read the kernel denies (a user-principal peer,
  since the shared boot diorama runs as SYSTEM) renders empty. Either way the
  client gains nothing it did not already have.
* **`/<pid>/environ` would NOT be sound, and is absent.** This server is
  `PRINCIPAL_SYSTEM`, so the kernel would ALLOW it to read any SYSTEM Proc's
  environ -- and it would hand those bytes to a client of any principal, who
  natively would have been denied. `/srv` is the shared immortal boot registry
  re-grafted post-pivot, so a logged-in user Proc can mount this server: the leak
  was reachable, not theoretical. It is the deputy-as-authority failure section
  6.2 forbids, and it surfaces now rather than at V-4b-1..5 because environ is the
  first proxied file whose native gate is anything but "everyone".

A walk to `/<pid>/environ` is an honest ENOENT, and both the selftest and
`/bin/diorama-probe` assert the miss (a resolution is boot-fatal -- the selftest
runs before the `/srv/diorama` post, so a regression takes the server down rather
than shipping the leak).

Replicating the kernel's owner check against `peer.principal_id` was considered
and rejected: it would work, but it turns a component whose entire design property
is *having no policy* into a policy point, to serve a file no v1.0 consumer reads.
Two things make the per-pid variant servable, neither a change here -- a
per-container diorama running as its container's principal (V-7), where server and
client authority coincide by construction, or MANDATE (I-35), which would let a
deputy act with its client's authority instead of its own.

**The rule to carry forward:** before proxying a file, ask not only "could the
client read this natively" but "could the client read this natively *for this
target*". A deputy with more authority than its client is as much a section 6.2
violation as one that invents an answer.

## The V-4c-3 audit close -- what the formal round changed

The arc's focused round (Fable 5 at max effort; `MODEL(start) == MODEL(end)`, no
fallback) closed **0 P0 / 1 P1 / 2 P2 / 5 P3**. The P1 is kernel-side and is
recorded with `proc_set_exe_path`; the two P2s are here.

### `msize` is negotiated without a floor, so the size caps must saturate

`h_version` sets `msize = min(client_msize, SRV_MSIZE)` and `parse_tversion`
accepts any `u32` -- **`0` included**. `h_read` and `h_readdir` then size their
reply against `msize - P9_HDR_LEN - 4`, and `P9_HDR_LEN` is 7, so any negotiated
`msize < 11` **underflows**.

That is not a wrap. `usr/Cargo.toml` builds this crate with
`overflow-checks = true` and `panic = "abort"`, and libthyla-rs' panic handler
tail-calls `t_exits(1)` -- so the underflow **terminates the server**, and `/dio`
dies for every mount on the box (`main.rs` has no restart). The reaching sequence
is three messages from any Proc that can `open("/srv/diorama")`:
`Tversion{msize:0}` -> `Tattach` (the root is a directory) -> `Treaddir`. No walk
required.

Both sites now use `(self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4)`,
which is the spelling netd, ptyfs and corvus already share at exactly this
expression -- the diorama was the outlier, not the innovator. A degenerate
session then yields a zero-capacity read rather than a dead server: useless, but
useless is a legal answer and death is not.

A negotiation floor was considered and **declined**: saturating already makes the
arithmetic safe, and adding a floor none of the three sibling servers have would
buy nothing while making this one behave differently from the rest.

### Walk-by-name and enumeration are DIFFERENT surfaces

The whole `cpuN/cache/index0` subtree shipped **readdir-invisible**, and every
test passed.

`h_readdir` runs the static-node loop first. A cpu qid is `>= CPU_BASE` (`1<<24`)
and every static node's parent is `< N_COUNT` (26), so no entry can ever match:
the loop takes its `continue` on every iteration and leaves its cursor `child` at
26. The cache-chain arm below was gated on `child == 0` -- so it could never run,
on the first call or any other. `readdir("/sys/devices/system/cpu/cpu0")` was an
empty directory, as were `cache/` and `cache/index0/`.

The comment above that arm ("the cookie is simply 'have I emitted it yet' -- 0
means not") described **`a.offset`**, the client's cursor. `child` is the *static
loop's* cursor and had already been advanced past it. The fix is to gate on
`a.offset`, which is what the comment always meant.

Why nothing caught it is the durable part. `walk` resolved every level by name
perfectly well -- the selftest drives `walk_child`, and `diorama-probe` opened the
leaf by literal path. **Neither ever issued a `Treaddir` on a cpu node.** So the
subtree was provably reachable and provably unlistable at the same time, and a
consumer that enumerates to find `index*` (the portable way -- cache-level
numbering is not fixed) saw nothing.

`diorama-probe` now carries three `dir_lists()` legs asserting each level
enumerates its single child. **Revert-probed**: restoring `child == 0` fails the
boot with `diorama-probe: FAIL cpu0 readdir does not list 'cache' (V-4c-3 F3)`.

**The rule to carry forward:** proving a path resolves says nothing about whether
its parent lists it. A tree needs a test on both surfaces, and a probe that only
ever opens literal paths is structurally blind to half of it.

---

## The V-4c-3 P3 hygiene pass (tasks #74 + #75)

Two findings the audit deliberately tracked rather than fixed mid-close, both
about *consistency* with disciplines this file already applies elsewhere.

### The renderers had opted out of the vDSO without saying so (SA-4)

`clock_pair_ns` and `render_uptime` each declared a private `t_timespec` mirror
and called `libthyla_rs::t_clock_gettime` raw. Correct, and correctness was
never the issue: `libthyla_rs::time` takes the #343 vDSO page first (a
`CNTVCT_EL0` read, no syscall), so going around it cost two syscalls per
`/proc/stat` render and one per `/proc/uptime` — on precisely the files a
monitoring tool reads in a loop.

The reason it happened is worth keeping. `render_uptime` **documented** its
workaround ("`libthyla_rs::time` keeps its `TimeSpec` private and `Instant`
exposes no 'since boot' accessor"), and that was a true statement of a real API
gap. What it could not document was the cost, because the fast path lived behind
the API being bypassed. The author solved the problem they could see.

Fixed in libthyla-rs, not here — patching it diorama-side would only have
relocated the duplication. `time::monotonic_realtime_ns()` and
`Instant::since_boot()` now serve both sites, and the pair reader derives both
clocks from **one** counter sample, which is strictly better than what the
bypass could do: `btime` is `realtime - monotonic`, and two separately-taken
samples let a preemption between them leak into that difference. Three
`t_timespec` mirrors in the Rust tree are back down to one.

### Wrapping accumulation in a `panic = "abort"` server (F7)

`parse_kv_dec` and `parse_dec_after` accumulated digits with
`wrapping_mul(10).wrapping_add(..)` and no length bound, so a long enough digit
run silently produced *some* `u64`. `parse_u64`, ten lines away, had used
`checked_mul`/`checked_add` all along — the audit's claim that the parsers were
bounds-checked was scoped to `parse_u64` only.

The consequence lands where the values are consumed: `VmRSS` and the three
`meminfo` fields multiply by 4 to convert pages to kB, and under
`overflow-checks = true` an unchecked `* 4` **panics** — which with
`panic = "abort"` is the same server death the `msize` underflow produced. Not
reachable: both sources are kernel-generated and bounded (`/ctl/memory` by
`phys_total_pages()`, `/proc/<pid>/status` pages by `PROC_PAGE_MAX`). It was
tracked because an inconsistency with the file's own saturating/checked
discipline is the kind of thing that stops being unreachable quietly.

Both parsers now use the `checked_*` form and return `None` — an unrepresentable
number is not a value, and "absent" is the honest reading — and the four `* 4`
sites use `saturating_mul(4)`, matching `sum_cpu_col` / `clock_pair_ns` /
`render_stat`. The selftest covers it, **revert-probed**: restoring the wrapping
form fails the boot with `diorama: selftest FAIL: parse_kv_dec wrapped an
over-long digit run into a value`. `u64::MAX` itself still parses, and an
overflowing key does not poison a later well-formed one.

## The V-4c-3 shape pass (#71 + #72)

The round's last two P3s, closed together because both are the same kind of
claim: the diorama answers as a **9P server**, and its shape has to be right at
that surface rather than at "whatever our own kernel happens to ask for".

### #71 — nothing is a child of a file

SA-2 reported that `h_walk` does not gate on the source fid being a directory, so
`/dio/proc/self/exe/..` resolves where Linux gives `ENOTDIR`. **That example was
wrong**, and the scope with it: `walk_child` already gated in two of its three
arms — the per-pid arm on `qid_kind != PK_DIR`, the static arm on
`!NODES[dir].is_dir` — so both `exe/..` and `<pid>/maps/..` already missed. Only
the **cpu arm** was ungated, which made the cache leaf
`cpuN/cache/index0/coherency_line_size` the single node in the whole tree from
which `..` (and `.`) walked. The note generalized one deliberate exemption into a
missing gate; reading `h_walk` alone it is true that the gate is not *there*, but
it does not follow that there is no gate.

The fix hoists **one** gate to the top of `walk_child` and deletes the two
per-arm ones it subsumes. The exemption goes away rather than growing a special
case, and `dir < N_COUNT` becomes a precondition of the static arm — which is
what keeps its `NODES` index in bounds.

The cpu `..` match moves out to `cpu_parent(n, kind)`, **total over every kind
including the leaf**. The gate now makes the leaf case unreachable, and that is
exactly why the arm has to stay correct: V-4c-2c's warning was that a `_ =>`
catch-all would answer `.../system/cpu` from the leaf and skip *two* levels — a
wrong answer rather than an error — and a gate with a wrong answer behind it is
one edit away from being a wrong answer again.

`h_walk` additionally answers **`ENOTDIR`** rather than the `ENOENT` an empty
partial walk produced, scoped to `nwname > 0` because a zero-length walk is how a
client clones a fid and cloning a fid that names a file is legal 9P.

**What is and is not reachable in-guest.** Neither half is observable through
Thylacine's own resolver, and the reason is worth recording:

* `stalk` normalizes `..` itself — it pops its trail (`kernel/stalk.c:237`), which
  is what contains resolution at `root_spoor` for I-28 — so `..` never reaches a
  Dev. Only a direct 9P client sees it.
* `Dev.walk` returns a `Walkqid *` with no errno channel, so stalk's
  single-component arm pins `err = T_E_NOENT` (`stalk.c:522/551`) and the new
  `ENOTDIR` is collapsed on the way out. That is a kernel-side gap affecting
  every Dev, not a diorama one — **task #79**, the same class as the tracked
  unlink-path and #99 create-path errno losses.

So the coverage is the selftest, revert-probed twice: disabling the gate fails
the boot with `diorama: selftest FAIL: walked into a file`, and folding
`cpu_parent`'s leaf arm into the catch-all fails it with `cpu_parent skipped a
level`.

### #72 — `environ` is a window, not a slice

SA-3 reported that `h_read` renders whole-file into a 4096-byte `Render` and
slices, capping **every** file at `RENDER_MAX`. For the bounded ones that is far
above the content; for `environ` it was the binding constraint, and it re-imposed
precisely the failure V-4b-6's offset-aware `env_render_environ` exists to
prevent — an `Env` holds up to `ENV_MAX_ENTRIES × ENV_VALUE_MAX`, and past 4 KiB
the tail vanished. Vanished *silently*: a dropped variable does not read as a
truncated file, it reads as a variable that was never set.

`render` now takes the offset. `environ` fetches its window with `read_native_at`
(a `t_pread` reader, **not** a drop-in for `read_native` — devctl is not
`.seekable`, so every `/ctl` source in the file must keep the cursor reader or
take `ESPIPE`), and every other renderer keeps building whole and gets
`Render::drop_prefix(off)` applied for it.

`h_getattr` reports **size 0** for `environ` alone. The self-sufficient reason is
that its content is a window: there is no total to report without reading the
whole file on every stat, and the pre-#72 answer — the *truncated* length — was a
lie the moment an environment passed 4 KiB. 0 is the only number here that is not
a guess, and it says the true thing: read to EOF.

It also matches the source. `devproc_stat_native` zeroes the whole `t_stat` and
never sets size for **any** `/proc` file (verified in `kernel/devproc.c`, not
assumed), so this reports what the surface it re-presents reports. Linux is
understood to do the same for `/proc/*/environ`, generated rather than stored;
that corroborates the choice but is not what it rests on. Deliberately not
extended to the rest: their sources are bounded, one render measures them
exactly, and a stat that agrees with its read is worth more than symmetry with a
zero.

The latent sibling SA-3 also named is closed: `render_cpuinfo` and
`render_stat`'s per-CPU loop now **commit whole units** (the `render_maps` row
idiom), so an overflow drops a whole block or line rather than cutting one in
half. Unreachable at `DTB_MAX_CPUS = 8` either way.

Proven in-guest by a new probe leg that builds a deliberately >4 KiB environment
through `/env` and checks the last record survives: `diorama-probe: environ>cap
bytes=5055`. Revert-probed — restoring the whole-render path fails the boot with
`diorama-probe: FAIL environ stopped at the render cap -- the >4 KiB tail was
dropped`. The pre-existing `environ bytes=19` leg cannot see this, because at
that size a whole-file render and a windowed one are the same bytes.
