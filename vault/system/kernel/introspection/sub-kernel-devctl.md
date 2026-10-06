---
id: sub-kernel-devctl
type: sub
parent: moc-kernel-introspection
title: "/ctl — machine-wide stats, two gated leaves, and counters with owners"
code:
  - kernel/devctl.c
  - kernel/test/test_devctl.c
audit: light
guarded-by: []
validated-by: [prose, gate-smp]
locks: [lock-proc-table]
abis: []
design: ["docs/ARCHITECTURE.md section 9.4", "docs/PROWL-DESIGN.md section 3.4", "docs/VIVARIUM.md section 6.17", "docs/IMPERIUM-DESIGN.md section 11.3 item 10"]
created: 2026-08-02
updated: 2026-10-06
---
## Purpose

The `/ctl` Dev (`dc='C'`, uppercase to leave `c` for the console): eight flat
text files rendering machine-wide state — the process list, physical memory,
the registered Devs, the KASLR base, scheduler stats, per-CPU meters, the
console's admission counters, and the live 9P sessions.

Read-only. Admin *commands* were deferred and never landed; `write` refuses
unconditionally.

## Contract

Walk `/ctl/<leaf>`, read text. Single-level: `..` from anywhere is the apex, and
a walk from a leaf has no meaning. Offset-aware reads over a freshly generated
2 KiB snapshot.

`stat_native` reports the apex as a directory and each leaf as a regular file,
**with size 0** — deliberately. Every file is generated at read time from live
state, so any length measured at stat is already stale, and a caller that
trusted it (stat, allocate, read exactly that many bytes) would silently truncate
a growing table. Linux reports 0 for `/proc/meminfo` for the same reason and the
world's readers loop to EOF. Sibling Devs that report *real* sizes are correct to
do so: their content is a static device-tree property or a config register, which
does not move between the stat and the read.

`procs` renders nine columns per process — `PID PPID NAME STATE THREADS PAGES
TABLES CHILDREN CPU_NS` — the counters as atomic loads, since a cross-Proc
reader holds no per-Proc lock. `TABLES` (prowl-6) is the page-table share of
`PAGES`, the holder count, so a reader takes the data view without opening
`/proc/<pid>/status`. The layout has consumers that parse by count (prowl) and
from the end (`ps`, whose beacon table also carries one alignment per column)
and one that checks the frame (coreutil-smoke); a column change moves all
three in the same commit, while the leading-column readers (Halcyon's loaded
systems, diorama) need nothing.

## Mechanism

### One table drives everything

A single leaf table carries `{name, kind, formatter}`, and walk, stat and read
all resolve through it. That is structurally better than the sibling `/proc`,
where adding a file means four separate registrations — here there is one, and a
leaf that is in the table is automatically walkable, stattable and readable.

**`/ctl/memory` carries the user pool since B-1a'** (2026-09-23): after the
physical totals, `format_memory` emits three more lines -- `reserve:` (the TCB
reserve, `capacity_reserve_pages`), `pool:` (RAM minus it,
`capacity_pool_pages`: every Proc's default budget and hard maximum) and
`charged:` (`capacity_pool_charged`: what every address space together holds
of it, exempt ones included) -- all in pages, in the same `key:   value pages`
shape as the lines above them. Two are boot-static and one is an atomic, so
nothing here takes a lock ([[sub-kernel-addrspace]]).

### The gate is a special case, and it is default-allow

Two leaves are gated, on the SAME `CAP_HOSTOWNER` check and for DIFFERENT
reasons. `kernel-base` discloses the live KASLR slide. `kstack` discloses no
address at all — it is gated because of its COST: its formatter walks every
live Proc under `g_proc_table_lock` with IRQs MASKED and scans each thread's
16 KiB stack for the poison boundary, recomputed on every `read()` at every
offset, since these leaves are stateless. Left world-readable, a `pread` loop
would hold a CPU masked and block every fork/exit/wait behind the proc-table
lock, defeating on that CPU the very interrupt-latency property ARCH 8.12
exists to establish (I-32). The scan is separately budgeted; the gate and the
bound close different halves and both are wanted. Added by the ARCH 8.12 audit
round (F2); `devctl.kstack_gated` is the deny-path regression, and it is a
deny probe through the REAL read path because the predicate is shared and a
predicate-only test would pass whether or not the gate were WIRED for a kind.

The gate is a `CAP_HOSTOWNER` check with **no owner axis** — the kernel has no
owner-principal, so the capability axis is the only one that could exist. A
logged-in user is stripped of the elevation-only capabilities at fork, so it
cannot read the slide and defeat the mitigation.

Everything else is world-readable Plan 9 introspection: the full process list
with names, parents, states, thread counts and page counts, visible to any Proc
that can name `/ctl`. That is the deliberate posture, not an oversight — but see
Caveats for the shape it leaves behind. CPU time and the scheduler's event
counters are the exception, and they are gated per field rather than per leaf
(next section).

The mode reported by `stat_native` follows the gate (0400 for `kernel-base`
and `kstack`, 0444 elsewhere) so the advertised mode does not lie about a file the caller
cannot in fact read — but as with `/proc`, `perm_enforced` is false, so that mode
is documentation and the check at the read site is the enforcement.

### CPU time and the scheduler's counters have owners

Since 2026-10-06 (IMPERIUM-DESIGN 11.3 item 10, [[dec-2026-10-06-cpu-time-gate]])
three leaves carry fields that render `-` to a reader who does not own them.
The leaf stays readable and the row keeps its shape; only the number is
withheld. The reason is the trusted episode. While a user types a secret into
the attached authority (corvus, during a login), every key wakes that authority:
it accrues CPU time, a CPU switches context and takes an interrupt, and a CPU
leaves idle. On a quiet machine each of those counters moves once per key, so a
reader that polls them recovers the secret's length and the timing of its keys,
the channel of Peeping Tom (USENIX Security 2009) and of Diao et al. (IEEE S&P
2016).

There are two owners:

- **A Proc's CPU time is that Proc's.** The trailing `CPU_NS` column of
  `/ctl/procs` shows the number only when `devproc_owner_or_hostowner(reader,
  row)` holds. That is the predicate `/proc/<pid>/sched` and the `cpu_ns` line
  of `/proc/<pid>/status` use ([[sub-kernel-devproc]]), so the two Devs cannot
  disagree about a row.
- **The machine-wide counters are the system principal's.** In `/ctl/cpu`, the
  `idle_ns`, `ctxt` and `intr` columns; in `/ctl/sched`, the `runnable:`,
  `wc:` and `wc-tickless:` values. They are shown only to a reader whose
  principal is `PRINCIPAL_SYSTEM` or who holds `CAP_HOSTOWNER`
  (`devctl_system_counters_readable`). The SYSTEM leg is there because joey's
  boot benchmarks run as SYSTEM without `CAP_HOSTOWNER`: kproc's `CAP_ALL`
  excludes the elevation-only set. `cpus:`, `created:`, the capacity class, the
  cache line and the MIDR stay visible to everyone. No key moves them: they are
  hardware description or a total that only counts creations.

`devctl_read` resolves the reader once per read, as the calling thread's Proc,
and every formatter takes it. A leaf that needs no gate says `(void)reader`;
none lacks the means to ask. The principal and the capabilities are read with
acquire loads, because other threads write both (`proc_apply_identity`,
`proc_become_legate`).

A withheld value renders `-`, never `0`. Zero is a plausible reading (an idle
CPU's interrupt delta, a new Proc's CPU time), so a consumer could not tell it
from the truth. No counter takes the value `-`, and every consumer learned it
in the same change: ps, prowl and diorama. A withheld value is not computed
either. The formatter passes 0 to the emit and skips `sched_wc_stats`
altogether, so the secret never reaches the render buffer.

The reader is whoever calls `read`, not whoever opened the file. A descriptor
handed to another Proc reads with that Proc's authority, as with every Plan 9
file generated at read time. So a SYSTEM service that relays these counters to
other principals must withhold them itself. The shared instance of
[[sub-diorama]] does.

`devctl.counters_gated` is the witness. A forked child takes an ordinary
principal and no `CAP_HOSTOWNER`, reads every gated surface through the real
read path and requires `-` in each, with its own row's `CPU_NS` a number. It
then takes `CAP_HOSTOWNER` and requires numbers. The parent reads as SYSTEM,
and the predicate's legs are checked on their own.

### A 9P connection's counters belong to its two ends

`/ctl/9p-sessions` (#210's loss discriminator) renders one `conn` row per live
`/srv` connection and one `sess` row per attached 9P session. Since 2026-10-06
([[dec-2026-10-06-9p-sessions-ends]], IMPERIUM-DESIGN 11.3 item 10) a row's
counters are shown only to the principals at its two ends, to the system
principal and to a hostowner. The counters are the ring byte counts, the
server's frame count, the demux counters, the reader flag, the send waiters
and the in-flight tags. Each moves once per message, and a pty-served terminal
carries a message per key. Everyone else reads `-`. The row itself stays
world-readable: peer pid, label, msize, mode and state.

The ends travel with the row. A `conn` row carries the connecting Proc's
principal and the poster's, both stored in the `SrvConn` by value at the
connect ([[sub-kernel-srvconn]], [[sub-kernel-devsrv]]). A `sess` row carries
its attaching Proc and, over a `/srv` connection, that connection's server
([[sub-kernel-ninep-attach]]). `ctl_9p_shown` compares the reader's principal,
loaded once per read with acquire, against the two. `PRINCIPAL_INVALID` marks
an end the kernel does not know, such as the server behind a caller-supplied
transport or a session not yet stamped, and it matches no reader, not even
one whose own principal is unset. A reader running as `none`
(`PRINCIPAL_NONE`) matches no end either: Procs that run as none are
unrelated (a pre-auth server runs as none, one per remote client), so none is
nobody's end. The `/proc` owner predicate still treats none as one owner; that
question is enqueued on its own. An end already sees every message on its
row, so the rule discloses nothing new to it. It keeps a user's view of their
own connections, which the wedge autopsy (`tools/warp/glq-wedge-probe.exp`,
run as michael) reads.

A row that does not fit the buffer is rolled back to its start, as
`/ctl/procs` rows are, so a reader gets whole rows.

`devctl.read_9p_sessions_format` and `p9_attached.ctl_registry` check each
reader class against a synthetic row through `devctl_format_9p_sessions_for_test`:
an unrelated principal, either end, an unset principal against an unknown end,
the system principal and a hostowner. `devsrv.conn_ends` checks the ends a real
post and connect record, and that a poster's later identity is not one of
them. `cpu-gate.exp` reads the file as michael and requires one withheld row
and one counted row of each kind. The counted ones are michael's home proxy's.

### Zero means overflow, and an empty string writes zero bytes

The emit macros are the file's whole formatting discipline: call a helper, and
**treat a return of zero as "the buffer is full"** — set the full flag, abandon
the row, return. Every field in every row goes through them.

An empty string writes zero bytes. So emitting one is indistinguishable from
running out of space, and the format aborts at that point.

This is not hypothetical: the newest leaf carried a conditional suffix written as
a ternary with an empty alternative, and every read of that file truncated at
**exactly** the same offset — deterministically, which is what ruled out
interleaving and pointed straight at the format rather than the console. One
partial row and nothing after it, on a file whose entire purpose was diagnosing
something else.

**A success that produces nothing is indistinguishable from a failure that
produces nothing.** The convention has no room to say "wrote zero bytes, on
purpose" — which is the same shape as a gauge reading zero because the thing
never started.

The repair is stated as a rule at the site and generalizes past the literal that
caused it: never route a possibly-empty value through an emit. Conditional
suffixes are guarded by an `if` instead of a ternary with an empty arm, and a
**runtime-computed** value that could be empty — a session label — emits a
placeholder rather than nothing. The surrounding ternaries had always had two
non-empty arms, which is why only the new code broke; the rule was being followed
before anyone had written it down.

**The test passed through all of it.** It asserted a *prefix* of the row, and the
prefix sat before the truncation point — so the assertion could not observe the
failure it was there to catch. It now asserts through the row's tail, which is
the only version a mid-row abort cannot satisfy.

### The process list bounds its own lock hold

The per-Proc formatting callback returns *stop* on the first overflow, so the
walk ends when the output buffer fills rather than visiting every process under
the global lock with interrupts off. Once `/ctl` became reachable from userspace
this stopped being a formatting nicety: it is what bounds an unprivileged
tight-loop reader's lock hold to the size of the buffer instead of the size of
the process table.

A row is committed whole or not at all, in `/ctl/procs` and `/ctl/kstack`
alike: the callback formats at the buffer's tail and, if any field does not
fit, rolls the offset back to the row's start before it stops. A row cut
mid-number would hand `ps` and `prowl` a plausible smaller figure, which is
worse than a list that ends early. `devctl.procs_rows_whole` formats the list
into every buffer size from one byte up and requires each result to end on a
whole row of nine columns.

### The STATE column shows job-stop, never debug-stop

An ALIVE Proc carrying `job_stop_req` (a `/proc/<pid>/ctl` suspend, or a Ctrl-Z
through the pts path) renders `STOPPED` — the Unix `ps` T-state, via
`procs_state_name`. The DEBUG stop (`debug_stop_req`, the attach-gated debugger
stop) is deliberately **not** surfaced: it is the debugger's private I-39 view,
not a job-control state a monitor should expose, so the render reads
`job_stop_req` alone. That flag is read atomically — a cross-Proc reader holds
`g_proc_table_lock` via `proc_for_each` but takes no per-Proc lock. A dying
Proc (`group_exit_msg` set) is never shown `STOPPED`, whatever flag a stop made
before the kill left: its last thread runs its exit close regardless, and a
dying Proc is not stopped to any reader (DEBUG-FS-DESIGN 5g). The test hook
`devctl_procs_state_name_for_test` exposes the word to `proc.dying_takes_no_stop`.

### Offline CPUs render as a short row, not as a busy one

A CPU declared by the device tree that never came online has an idle time of
zero forever — which through the meter's arithmetic (`1 - idle/wall`) is
**indistinguishable from a permanently pegged core**. So the per-CPU renderer
gates on the online flag and emits a two-token `offline` row that the reader's
three-token parse skips, rather than a number that would draw a dead core as a
full meter.

The row format is append-only by contract: the userspace reader matches the
first three tokens positionally and ignores the rest, so columns may be added.

## Data structures

None owned. The leaf table is static and const; all state is read live from the
process table, the physical allocator, the Dev registry, the KASLR module and the
per-CPU scheduler meters.

## Concurrency

Only `g_proc_table_lock`, and only for the process list (taken by
`proc_for_each`; the CPU-time helper walks a Proc's threads under it).

The CPU-time gates read the reader's principal and capabilities with acquire
loads and take no lock.

Everything else is read without a lock, and each has its own reason: the physical
counters and scheduler stats are coherent atomic snapshots of a single writer;
per-CPU capacity, cache-line size and MIDR are boot-static; the Dev registry is
boot-immutable. The job-stop flag surfaced in the state column is read atomically,
because a cross-Proc reader holds no per-Proc lock.

## Invariants enforced

**I-16** (the KASLR slide is a secret) — the `kernel-base` gate is one of its two
enforcement sites, the other being the `/proc` kernel-stack raw/symbolic split.

**I-27** (the trusted path): the CPU-time gates keep the trusted episode's
key cadence from the counters (IMPERIUM-DESIGN 11.3 item 10); the 9P
sessions' ends rule keeps a pty-typed secret's cadence from other principals.

Composes **I-1**: reachability is namespace visibility.

## Error paths

`-1` for: a read of the apex directory, a qid this Dev does not serve, a denied
`kernel-base` read, and every write. No errno distinction, matching the sibling
Dev.

## Performance

One 2 KiB stack buffer, regenerated on every read — so a paginated read of a
large process list re-renders the whole list per call. Fine at the current scale;
the same offset-aware multi-read that `/proc` wants would fix both.

## Prosecution

- **A new leaf that discloses a secret must add its own gate.** The gate is a
  per-leaf special case in an otherwise world-readable file, so the default for a
  new leaf is *readable by everyone*.
- **The `kernel-base` gate must stay capability-only.** There is no owner to
  admit.
- **A counter that moves when another principal's work runs gets a gate.** The
  question is not whether the number is secret. It is whether the number changes
  once per event in someone else's Proc, because such a counter publishes the
  cadence of a secret being typed. A new field of that kind goes through
  `fmt_gated_udec` with the right owner: the row's Proc for a per-Proc value,
  the system principal for a machine-wide one.
- **A per-connection counter goes to the connection's ends.** A new
  `/ctl/9p-sessions` field that moves per message is emitted under the row's
  `shown`, never under a reader-wide flag. An end the kernel cannot name stays
  `PRINCIPAL_INVALID`, never a guess.
- **A withheld field renders `-` and is never computed.** A `0` is a lie no
  consumer can detect. A value computed and then hidden can reach the buffer
  by a later edit.
- **The process-list callback must keep stopping on overflow**, or an
  unprivileged reader re-acquires an unbounded global-lock hold.
- **Sizes must stay 0** for generated leaves. Reporting a measured length invites
  the stat-then-read truncation the current shape exists to prevent.
- **The offline-CPU row must stay short.** Emitting a numeric idle time for a
  never-online CPU renders a dead core as a pegged one.
- **Row columns are append-only.** Readers parse positionally.
- **Never emit a possibly-empty value.** Zero bytes written *is* the overflow
  sentinel, so an empty string aborts the whole file. Guard conditional suffixes
  with a branch rather than a ternary carrying an empty arm, and give any
  runtime-computed field a placeholder.
- **A row's test must assert through its tail.** A prefix assertion sits before
  wherever a mid-row abort would land, so it passes on exactly the failure it
  exists to catch.

## Seams

- **Writes.** The admin command surface (scrub, allocator controls, scheduler
  tunables) was deferred to the phase that would expose `/ctl` to operators and
  has not landed; `write` refuses.
- **Nested directories.** The architecture describes `/ctl/kernel/...`; the
  as-built layout is flat, pending a Dev that walks more than one level.
- **The whole-file re-render per read**, shared with `/proc`.

## Caveats

- **The read gate is default-ALLOW, and the project has already chosen
  default-DENY elsewhere.** The compositor's control surface was restructured so
  that its gate denies everything except an explicitly enumerated ungated set,
  precisely so a newly added verb is gated by construction. `/ctl` has the
  opposite shape: a new leaf is world-readable unless someone remembers a line.
  That is defensible for a surface whose *posture* is Plan 9 all-visible
  introspection, and the two surfaces differ (reads here, authority writes
  there) — but the asymmetry is a decision, and a leaf carrying a secret is one
  forgotten line from disclosure.
- **The formatters ignore a failed numeric append.** Several leaves add a
  number without checking whether it fit, then check the following literal. Each
  append is independently bounds-checked so there is no overflow; the visible
  effect of an exhausted buffer is a line missing its value rather than a
  truncated file. The sibling Dev checks both.

  **This caveat named the right convention and only one of its two directions.**
  It warned about a genuine failure being *ignored*. The defect that actually
  landed was the mirror image — a genuine success being *read as failure*,
  because zero is the overflow sentinel and an empty string writes zero bytes.
  Same fragile convention, opposite direction, and worse in effect: the ignored
  failure loses a field, the invented failure loses the rest of the file. Having
  enumerated one direction made the write-up read as though the hazard had been
  covered.
- **The default-allow read gate has been exercised, and the first verdict on
  it was wrong.** When `/ctl/9p-sessions` landed world-readable, this caveat
  judged its content to be ordinary introspection: peer identifiers, buffer
  counters, frame counts. It was not. The buffer and frame counters move once
  per message, and a pty carries a message per key, so another principal could
  time a secret typed into a terminal. The CPU-time gate's audit found it
  (round 1 F4), and the counters now belong to each row's ends. The judgement
  asked whether the numbers were secret. The question that catches this class
  is whether a number moves once per event in someone else's Proc.
- **The process list is a full-system disclosure.** Names, parents, states,
  thread, page and page-table counts for every process, to any reader. (CPU
  time left this list on 2026-10-06 and is owner-only.) This is the Plan 9 posture and is shared with `/proc/<pid>/status`,
  but it is worth
  stating plainly rather than leaving implied: `/ctl/procs` is the broadest
  ambient disclosure either introspection Dev makes.
- **The formatting helpers are duplicated** from the sibling Dev, noted in the
  code as deliberate chunk independence. They have since drifted slightly (the
  hex helper differs in prefix handling), so a fix to one does not reach the
  other.

## Provenance

[[chg-2026-08-02-introspection-sweep]], [[chg-2026-08-16-devctl-empty-emit]],
[[chg-2026-10-06-cpu-time-gate]], [[chg-2026-10-06-9p-sessions-ends]].
