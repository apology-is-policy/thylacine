---
id: sub-diorama
type: sub
title: "diorama — the synthetic Linux world, and a reformatter that must never become an authority"
parent: moc-userspace
code:
  - usr/diorama/src/server.rs
  - usr/diorama/src/main.rs
  - usr/diorama/Cargo.toml
audit: hard
guarded-by: [inv-i43]
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/VIVARIUM.md"]
created: 2026-08-04
updated: 2026-08-04
---
## Purpose

An unmodified Linux binary reads `/proc/self/exe` to find itself,
`/proc/cpuinfo` to size a thread pool, `/sys/devices/system/cpu/online`
to count cores. Thylacine has all that state and none of those shapes.
The diorama supplies the shapes.

It is a device-less 9P server posting `/srv/diorama` — the fifth native
server, and the third with no hardware (the [[sub-ptyfs]] and
[[sub-corvus]] precedent), so it is not warden-bound. A container binds
its `proc` and `sys` subtrees where Linux expects them.

**What it is not is the whole design.** It is a *reformatter*: every byte
it serves is derived from a source the calling process could have read
itself. That single property is [[inv-i43]], and it is what keeps a
compatibility layer from quietly becoming a privilege — the failure this
whole class of software is prone to, because each individual step toward
it looks like an improvement.

## Contract

A read-only 9P2000.L tree whose root is the synthetic *world*, not
`/proc`. Its children are named for Linux's mount points, and a container
binds each where it belongs.

Under `proc`: `self/{exe,cmdline,status,cwd,maps,environ}`, the same file
set under every live `<pid>` (minus `environ`), plus `meminfo`, `uptime`,
`stat`, `cpuinfo` and the `sys/kernel/{ostype,osrelease,version,hostname}`
sysctls. Under `sys`: `devices/system/cpu/{online,possible,present}`, one
`cpuN` directory per CPU, and each one's
`cache/index0/coherency_line_size`.

**Read-only, refused at the protocol edge.** Every write is `EPERM`
before any renderer is reached, and an open asking for write access is
refused at open rather than at write, so a caller learns where it can act
on it. That one decision removes most of the surface a `/proc` would
carry.

**`self` means the connection's peer** — the process that opened it,
which for a mounted tree is the *mounter*. So the tree belongs in a
per-container territory, and that is not a limitation to engineer around:
a container mounts it privately, so it gets itself by construction. The
alternative, letting a client name a pid, is the failure mode and is
deliberately not offered.

## Mechanism

**Existence is decided by a native read, never by a table.** A numeric
component under `proc` resolves only if opening `/proc/<pid>` natively
succeeds; a `cpuN` only if the kernel's own CPU count reaches that index.
This is the invariant's mechanism and it buys a second thing: a dead pid
is an honest `ENOENT` rather than a directory of empty files, which is
how every Linux consumer detects that a process is gone.

**Three qid families, mutually non-aliasing, and the separation is
asserted.** Static nodes use their table index; a per-pid qid packs
`pid << 32 | kind`; a cpu qid sits at bit 24 with the index in the low
byte and a kind above it. Since pid 0 is never live and every static
index is small, the ranges cannot collide — and the selftest checks all
three pairings rather than trusting the arithmetic.

**Every render is bounded and truncates at a row boundary.** One fixed
buffer, every push cap-checked. The maps renderer marks its position
before each row and rewinds if the row does not fit, so the output always
ends where a parser expects.

**The `/proc/*/maps` translation is where reformatting is most visible.**
Six native columns become six Linux ones, and the interesting parts are
where the systems genuinely differ: Thylacine's device number is flat
with no major/minor split, so it renders as a minor under major zero —
which is exactly how Linux renders any filesystem with no backing block
device, so the shape is honest rather than approximated. A protection-none
guard VMA renders `---p` with no pathname and is *emitted*, because
dropping it would make the map claim the range is free.

**Where a field has no source, the file says so — and the one place that
could not omit is stated at the site.** BogoMIPS is absent because
there is no truth to tell. `procs_running` would need a live census, so
it is omitted rather than zeroed. But `/proc/stat`'s CPU line is
*positional*, so a missing middle column is a wrong answer rather than an
absent one — Thylacine has no user-versus-kernel time accounting, so all
non-idle time is reported as system, with the premise written at the
function and the note that utilization (what essentially every consumer
computes) is exactly right either way.

**The constants are the deliberate exception, and the discriminator is
one sentence.** A value derived from kernel state needs a native source,
no exceptions; a constant declaring which ABI the caller is looking at is
the phenotype speaking about itself. `osrelease` is the one with teeth:
glibc refuses to start below a minimum kernel version, so it declares
6.1 — with a `-thylacine` suffix, because Linux's own convention carries
local suffixes and anything that prints the string then tells the truth.

## Data structures

A static node table of 26 entries, each a name, a parent index and a
directory flag — the index *is* the qid path, so a static node can never
dangle. Two dynamic families hang off it (pids, CPUs), neither of which
can be a static index because both are runtime facts.

`Render` is the bounded buffer: a fixed array, a length, and push helpers
for bytes, decimal and zero-padded hex. `Conn` is the per-connection fid
table — a fixed array of `(fid, node, opened)`, an input and an output
buffer, and the negotiated message size.

## Concurrency

None. Single-threaded, one poll loop, no locks, no shared mutable state.

**The accept loop declines rather than spins, and cites the finding that
taught it.** When the connection table is full it drops the listener from
the poll set entirely, because a full table plus a pending connection
would keep the listener perpetually readable — the accept is skipped,
nothing else changes, and the loop turns at full speed forever. This is
worth naming because [[sub-corvus]] has the same shape and does *not*
have this fix (task #149): the correction reached one sibling.

## Invariants enforced

**[[inv-i43]]** — this is its only enforcer and its whole design. See
that note for how the property is structural rather than checked.

Nothing else directly. The kernel's gates run underneath unchanged, which
is the point: [[inv-i26]]'s two-axis rule on `/proc/<pid>` control, the
capability gate on the kernel base, the per-file permissions — a read the
kernel refuses this server is a read this server cannot serve.

The one place authority *would* have diverged is handled by absence
rather than by a gate. `/proc/<pid>/environ` is owner-or-capability
natively because environments carry secrets by convention; this server
runs as SYSTEM, so it would be *allowed* to read any system process's
environment and would then hand those bytes to a client of any principal.
So `environ` exists under `self` only — where the target is the caller's
own process and the kernel's own answer is the client's own answer — and
the per-pid absence is asserted by the selftest with a failure string
naming what it is protecting.

The rejected alternative is recorded and the reasoning is worth keeping:
replicating the kernel's owner check against the peer would *work*, and
was refused because it turns a component whose entire design property is
having no policy into a policy point, to serve a file no consumer reads.

## Error paths

Uniformly fail-empty rather than fail-loud, and the distinction between
"denied" and "absent" is deliberately not observable: a denied read and
an empty environment both render empty.

A dead peer renders empty for every `self` file — checked once at the
render entry rather than inside each renderer, so a peer that exited
cannot cause a read of a pid that may since have been reused. Six
assertions pin this.

Protocol errors are ordinary 9P error replies. A frame that does not
parse drops the connection; a full message buffer with no complete frame
drops it too.

## Performance

Every read re-renders from scratch, and every render re-reads its native
sources. No caching anywhere. For an introspection surface read a handful
of times per process lifetime this is the right trade, and it is what
makes the freshness argument trivial.

`/proc` enumeration re-reads the live pid list per call, so a process that
exits mid-enumeration can make a pid appear twice or not at all — a
property Linux's own `/proc` readdir shares, since its cookie is a
position in a list that moves.

## Prosecution

- **Every new file needs a native source.** The rule is at the top of the
  file and restated wherever a shortcut would tempt. A file that reads
  state through a path a native process could not use has become an
  authority regardless of its output.
- **Never accept an answer supplied by the client.** Identity comes from
  the kernel-stamped connection peer; a client-named pid is the failure
  mode.
- **A new node keeps the dirent scratch adequate.** A pack failure returns
  false *without* advancing the cursor, so a name that can never fit makes
  a client re-ask for the same entry forever. The scratch is roughly
  double the longest reachable name; a longer name needs a bigger scratch,
  not a silent truncation.
- **The two trees stay siblings.** Hanging the sysfs tree off a root that
  *is* `/proc` would put a directory in a container's namespace that Linux
  has never had. Both directions are asserted.
- **Message-size arithmetic stays saturating.** See the caveats — this one
  has already been a whole-server abort.

## Seams

**No `/proc/self/fd`**, blocked on the kernel side: a cross-process fd
list of a live peer races the at-exit handle-table free. There is no
other native source, and inventing one is the failure this file exists to
avoid.

**No `auxv`** — weighed and not built: no live readers, and a
container-launched binary receives its auxv on the stack anyway.

**No cpu `topology`**, no `kernel_max` — core and cluster identity are not
derivable from the registers available, and Linux's `kernel_max` comes
from a compile-time constant with no readable equivalent. Omitting beats
reporting a different number under a name that means something else.

**Pid visibility is not container-scoped**, because there is no such
scoping natively — the native process list shows every process on the
box. So this server serves exactly what native `/proc` serves to exactly
the same readers. When containment lands, that becomes a real question
about `/proc` and the native process list *first*; scoping it here alone
would be theatre, since a contained process that can reach native `/proc`
would read around it.

## Caveats

- **The saturating-subtraction fix is recent and its absence was fatal.**
  Both read paths cap their reply against the negotiated message size,
  and the negotiation accepts any client-proposed value including zero
  with no floor. A raw subtraction underflows for anything below the
  header length — and this crate builds with overflow checks and
  abort-on-panic, so that is not a wrap, it terminates the server and the
  tree dies for every mount on the box. Three messages from any process
  that can open the service reached it. Its siblings all spell the
  expression saturating; this was the outlier.

- **A whole subtree read as an empty directory, and the tests could not
  see it.** The cache-chain readdir gated on the *static* loop's cursor
  rather than the request offset — and since no static node ever has a
  cpu qid as its parent, that cursor always exits at the table length and
  the guard could never fire. Walk resolved every level by name, so the
  selftest (which drives the walk) and the in-guest probe (which opens the
  leaf by literal path) both passed. A consumer that *enumerates* to find
  the index directory — the portable way, since the numbering is not fixed
  — saw nothing. The lesson is specific: a resolution test and an
  enumeration test are different tests.

- **The environ gate keys on the reader, and the reader is this server.**
  Sound today because the file exists only under `self`. If a per-container
  instance ever runs as its container's principal, or a delegated-authority
  mechanism lands, the per-pid form becomes servable — and until then the
  absence is the whole protection.

- **The maps path column rests on a stated premise.** A file-backed
  mapping renders the executable's path, which is correct only while the
  only file-backed regions in an address space are the executable's own
  segments. The premise is written at the site with its trigger: when a
  file-mapping syscall lands, the kernel's own line must start carrying a
  path and this branch must read it instead of substituting.

- **The proof position is the strongest in the userspace tree, and it is
  not host tests.** The selftest is ~500 lines of assertions that run
  before the service is posted and *gate the boot* — the tree walk, the
  qid families, every parser, the bounded renderer, the maps translation,
  the dead-peer renders, and the negative assertions that protect the
  invariant. It needs no VM state and no harness. Compare [[sub-aurora]],
  whose tests cannot compile, and [[sub-corvus-crypto]], whose run on the
  host but not on the device. Three crates in one batch, three different
  answers to how a thing is proved.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
