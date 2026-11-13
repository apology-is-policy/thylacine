---
id: sub-kernel-devctl
type: sub
parent: moc-kernel-introspection
title: "/ctl — machine-wide stats, two gated leaves, and counters with owners"
code:
  - kernel/devctl.c
  - kernel/include/thylacine/devctl.h
  - kernel/test/test_devctl.c
audit: light
guarded-by: []
validated-by: [prose, gate-smp]
locks: [lock-proc-table]
abis: []
design: ["docs/ARCHITECTURE.md section 9.4", "docs/PROWL-DESIGN.md section 3.4", "docs/VIVARIUM.md section 6.17", "docs/IMPERIUM-DESIGN.md section 11.3 item 10"]
created: 2026-08-02
updated: 2026-11-13
---
## Session transport accounting

`/ctl/9p-sessions` begins with a bounded admission summary: global/session/boot
connection counts, global and session credit usage/limits, boot credits,
per-domain limit, retained domains and bytes per credit. It precedes the detail
walk, so a cut read still holds the global summary. Each connection row carries
its own admission domain's two figures between `msize` and the kind: the
domain's live connection count (`session=` for a login session's domain,
`boot=` for the boot class) and its credits with their limit. The global
figures are the summary's alone; a row does not repeat them. Counts are never
silently reinterpreted as credits.

One read is the whole file. The render is bounded by the 4 KiB read buffer,
connection rows first and session rows last. A row that does not fit is left
out whole, and a render that left any row out ends with the line
`(cut: more rows than one read holds)`, whose room no row may take, so a
missing row is told apart from a row that does not exist. The line does not
say how many are missing. The summary line is 133 bytes, a connection row 80
to 110, a session row 105 to 115; the gate image's boot (15 connections, 10
sessions) renders in about 2.5 KiB.

Each snapshot is coherent under the admission lock. Summary and detail rows
are sampled at different instants, not a transaction or a peak measurement.
The detail walk retains the existing diagnostic-list-before-admission lock
order. The summary releases admission before starting that walk. These are
observations, not authorization inputs; see [[sub-kernel-srvconn]].

The admission figures (the summary line, and each connection row's two) are
not among the counters IMPERIUM-DESIGN 11.3 item 10 withholds: none of them
moves per message, per key or per byte. A connection's figures move when a
connection is reserved or freed; the summary's `domains=` moves when a session
domain is created or retired. Every reader who sees a row sees them. They tell
such a reader which rows are a login session's and which the boot class's, how
many live connections and credits that row's domain holds, and how many
session domains are retained.

A reader running as none reads an empty file. The none wall gives it no row of
this file (`docs/AUDIT-TRIGGERS.md`, the none-wall row, item (c);
[[dec-2026-10-06-none-owns-nothing]]), and the summary is part of the file.
`format_9p_sessions` asks `ctl_9p_row_hidden` once, before any emitter runs, so
a line added to the file stands behind the wall without a check of its own.

## Purpose

The `/ctl` Dev (`dc='C'`, uppercase to leave `c` for the console): eight flat
text files rendering machine-wide state — the process list, physical memory,
the registered Devs, the KASLR base, scheduler stats, per-CPU meters, the
console's admission and loss counters (`/ctl/cons`, `rx_drop_side` among them
since vote 73), and the live 9P sessions.

Read-only. Admin *commands* were deferred and never landed; `write` refuses
unconditionally.

## Contract

Walk `/ctl/<leaf>`, read text. Single-level: `..` from anywhere is the apex, and
a walk from a leaf has no meaning. A read renders its leaf afresh into a 4 KiB
buffer and answers from the caller's offset, except `procs`, which is rendered
once per open file (below).

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

**The header.** Nothing outside devctl.c calls into /ctl but the in-kernel
tests, so `devctl.h` holds only their reach, under `#ifdef KERNEL_TESTS`
([[sub-kernel-declarations]]); the Dev is declared with the others in `dev.h`.

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

**Two more lines since MM-1b (2026-11-03):** `ledger:` is `account_pool_share`, the
accounts' count of the same pages, and `unpaired:` is `account_unpaired`, the broken
pairings they caught ([[sub-kernel-accounts]]). Both are atomics. The test reads the file
through a larger buffer now, because the two lines can pass 256 bytes.

### The gate is a special case, and it is default-allow

Two leaves are gated, on the SAME `CAP_HOSTOWNER` check and for DIFFERENT
reasons. `kernel-base` discloses the live KASLR slide. `kstack` discloses no
address at all — it is gated because of its COST: its formatter walks every
live Proc under `g_proc_table_lock`, each Proc's threads under its thread lock, with IRQs MASKED, and scans each thread's
16 KiB stack for the poison boundary, recomputed on every `read()` at every
offset, since these leaves are stateless. Left world-readable, a `pread` loop
would hold a CPU masked and block every fork/exit/wait behind the proc-table
lock, defeating on that CPU the very interrupt-latency property ARCH 8.12
exists to establish (I-32). The scan is separately budgeted (1 MiB of loads in
all), not by the thread cap, which no longer bounds it usefully: a Proc's
thread budget is 4096 by default since thread-model C6 (ARCH 7.3.5). The gate
and the bound close different halves and both are wanted. Added by the ARCH 8.12 audit
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
  disagree about a row. A reader running as none sees no row but its own:
  `format_procs_cb` skips every row `devproc_none_walled` refuses (Plan 9's
  `nonone`, [[dec-2026-10-06-none-owns-nothing]]), before the row starts, so
  the rows it does emit stay whole. `/ctl/9p-sessions` shows a none reader no
  row at all and no admission summary (`ctl_9p_row_hidden`): none is no end,
  so it owns no row, and a none hostowner counts as `sys`. A reader refused `/ctl/kernel-base` or
  `/ctl/kstack` reads `EACCES`, not -1.
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
`proc_become_legate_locked`).

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
readable to every reader but one running as none: peer pid, label, msize, a
connection's two admission figures, mode and state.

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
the process table. The bound is in rows, not in time: a row whose `CPU_NS`
the reader may see walks that Proc's threads (`proc_cpu_ns`). And it is for a
reader that is shown rows. One behind the none wall is shown its own row
only, never fills the buffer, and its walk visits every Proc: the cost of any
`/proc/<pid>/status` read. Both are part of the Proc table's locking debt
(#62).

A row is committed whole or not at all, in `/ctl/procs` and `/ctl/kstack`
alike: the callback formats at the buffer's tail and, if any field does not
fit, rolls the offset back to the row's start. A row cut mid-number would
hand a parser a plausible smaller figure, which is worse than a list that
ends early. `/ctl/procs` stops its walk at that row; `/ctl/kstack` walks on
without rows, for its peak (below).

### A cut process table says so (2026-11-12; operator vote 85)

`/ctl/procs` holds the rows that fit one snapshot (`PROCS_SNAP_CAP`, 4092
bytes, about sixty rows), not every Proc. A table that left a row out ends with one
fixed line, `PROCS_CUT_LINE`:

    (cut: more processes than one read holds; /proc lists them all)

A short table with no sign reads as "no other Proc", and a gate that
concludes that from a cut table is wrong without knowing it. The line always
fits, because its room is reserved. `format_procs` renders nothing into a
buffer that cannot hold the header and the line. `format_procs_cb` lets only
the table's last row end inside the reserve (`in_reserve`): when another
visible row follows a row that did, that row is taken back (`s->off =
s->last`) and the table is cut there. So a table that exactly fits the buffer
is whole and carries no line, and a cut one always has room for it. A
`_Static_assert` keeps `DEVCTL_READ_BUF` at least the header plus the line.
Rows a none reader is walled from are skipped before the reserve check, so
they never cut its one-row table.

The line is not a row: it starts with `(`, where a row starts with a pid. A
reader that needs every Proc does not read this file. It lists `/proc`, a
directory with no such bound, and reads each `status`
([[sub-kernel-devproc]]); `ps`, `prowl`, diorama, halcyon and `srvfull-probe`
do that through libthyla-rs's `procs` module ([[sub-libthyla-rs]]).
`/ctl/9p-sessions` ends a cut render the same way, with a line of its own.

`devctl.procs_rows_whole` links two Procs of its own, formats the table into
every buffer size from one byte up, and compares each result's shape with the
whole table's: every line whole, every row nine columns, the rows the whole
table's first rows by pid, the cut line present exactly when a row is
missing, and at least one size that holds rows and the line together. It
compares shape, not bytes, because `CPU_NS` moves between two renders.
`tools/interactive/ps-many.exp` reads the file on a freshly booted guest and
requires no cut line, then with seventy more Procs and requires it after a
whole nine-column row, with nothing of a later rendering after it (below).

### One rendering per open file (2026-11-13; operator votes 99 and 108)

`/ctl/procs` is rendered once per open file. Its first read at offset 0
renders the table into a `procs_snap` (a length and `PROCS_SNAP_CAP` = 4092
bytes, exactly one page: `kmalloc` serves a request past
`SLUB_MAX_OBJECT_SIZE` from `alloc_pages`) and publishes it in `Spoor.aux` by
compare-and-swap. Every read of that open file, at any offset, is served from
it, and a read at or past its end answers end of file. A whole read keeps it
too, as Linux's `seq_file` keeps its buffer: then every reader of the open
file, whatever its buffer and whichever thread it runs on, reads one
rendering. `devctl_close` frees it at the open file's last reference.

Every read used to render the table afresh, and a reader that read to end of
file joined a second rendering's tail to the first: when the second was
longer, `cat` printed its last bytes after the cut line (7 bytes, a cut line's
end, in both of ps-many's boots before the rule). Rows are unpadded decimals
and `CPU_NS` moves every tick, so the length moves between any two reads. Vote
99 first answered with end of file past offset 0 and a table rendered to the
caller's buffer. Audit round 28 F1 showed what that cost a reader that starts
small: `hexdump` (16-byte reads) and Rust std's `read_to_string` (a 32-byte
first probe; the file's size reads 0) were refused, and a 1 KiB reader
(libthyla-rs's `read_to_end`, Pouch stdio) got about thirteen rows and the cut
line. Vote 108 pulled vote 99's per-open snapshot forward for this one file.
No read is refused for its size; were such a refusal to return, its code is
`EINVAL` (vote 109). The other rendered files (devctl's other leaves, devproc,
diorama's Linux view) still render per read, until vote 99's own snapshot
chunk after the clipboard arc.

Three things keep it one open file's and freed once:
- **No clone inherits it.** `spoor_clone` copies `aux` shallowly, so
  `devctl_walk` clears it on every Spoor it returns. The one route by which a
  clone reaches an opened `/ctl/procs` is a mount whose source it is: a mount
  crossing clones the source (`clone_walk_zero`) and opens the clone. The
  O_PATH re-open of a descriptor is not one: a file descriptor answers ENOTDIR
  there.
- **Every open starts at offset 0.** `dev_simple_open` resets the cursor the
  clone copied ([[sub-kernel-dev]]), so that crossing reads its own whole table
  from byte 0 instead of answering end of file at the mounted reader's offset
  (audit round 29 F1).
- **A race publishes one.** Two threads sharing the open file can both read at
  offset 0. Each renders; the first compare-and-swap publishes, and the other
  frees its copy and serves the winner's. The snapshot is immutable once
  published and freed only at the last reference, when no read runs, because
  every read holds a reference. `spoor_read_on` reads the cursor with acquire
  and advances it with release ([[sub-kernel-syscall-dispatch]]), so a read at
  a cursor another thread advanced sees the snapshot that thread published.
  Before that, the advanced cursor could be seen with `aux` still NULL, and a
  read past 0 with nothing kept answers end of file (round 29 F2). That answer
  now belongs to positioned readers alone (`mmap_eager_copy`, exec's header,
  fault reads), which read offset 0 first with at least 4 KiB.

A snapshot costs one page per open file that has been read, uncharged, like a
pipe ring. It lives as long as a reference to the open file does. A descriptor
is one (at most `PROC_HANDLE_MAX`, 1024, per Proc), a mount entry another (at
most `PGRP_MOUNT_CAP`, 1024, per territory), and so is a Loom registration. A
file mapping would be one too, kept in the Image cache past the mapper and past
every close, one per mapping up to 65536 per address space: so devctl sets
`no_file_mapping`, and both mapping cores answer `ENODEV` (a `/ctl` leaf's size
is 0, so no byte of one was ever mappable; audit round 30 F1). When no page is
free, the read fails with `ENOMEM` and nothing is kept.

The table is rendered for the open file's first reader, with that reader's
gates: the none wall and the `CPU_NS` gate. A Proc handed the open file, or
one that inherits it (a fork before a drop to none), reads that rendering
([[sub-kernel-devproc]]'s none wall still holds: no Proc reaches the table
another Proc rendered without that Proc's handing it over). This is Linux's
`f_cred` shape for `seq_file`.

`devctl.procs_snapshot` reads through `devctl_read` with two Procs of its own
linked. A whole read keeps its rendering, and its next read is end of file. A
pieced read in 16-byte reads, with a third Proc linked after its first piece,
reads the table a whole read had just shown, row for row by pid, while a fresh
open shows the third Proc, so the change it missed did happen. It is taken
again until it matches, within four tries, because a Proc can come or go
between the reference read and the pieced one. Both walk shapes, devctl's own
clone and a caller's pre-clone, return a Spoor with no snapshot; a clone found
carrying one is detached before its close, so that mutant fails at the
assertion and not on a page freed twice. A clone of the read-into file opened
the way a mount crossing opens it starts at 0 and reads a whole table of its
own. The open file's last close is watched entering the large free for its
snapshot's page (`kfree_large_watch`). The race is played by a test hook that
runs inside a render, after it renders and before it publishes: a Proc joins,
and a second read of the same open file publishes first. The first read must
serve that table, the joined Proc in it, and leave one snapshot held, the
loser's copy freed. One-byte reads read a whole table, and every last close
frees its snapshot (a held count: allocated, not yet freed). A kernel without
the snapshot fails it at once: vote 99's rule ends the pieced read after 16
bytes, and a kernel that renders per read gives the pieced read the third
Proc's row or a splice on every try.

`devctl.backs_no_mapping` reads one byte of an open `/ctl/procs` (a snapshot
is held), asks both mapping cores to map it through a Proc's handles, drains
and frees that Proc, and closes the file: both cores answer `ENODEV`, and the
held count returns to where it began, so no Image entry kept the open file.
Without the flag a mapping succeeds and the cached entry keeps the snapshot
past the close.

`ps-many.exp`'s second leg is unchanged: `cat`'s first read (8 KiB) holds the
table and its second read finds end of file, so a whole row or a cut line's end
after the cut line fails the leg. Its leg 2b runs `hexdump /ctl/procs`
(16-byte reads) on the same large system, rebuilds the bytes from the hex
columns, and requires one table: the header, whole nine-column rows with no pid
twice, the cut line last, rows at contiguous offsets, and a total of 3000 to
4096 bytes. Vote 99's rule refused that read, and a rule that ended the file
after the first read would leave 16 bytes. A file rendered per read tears only
where a row's length changes between two reads, and nothing in the scenario
makes one change, so the leg does not catch that kernel; `devctl.procs_snapshot`
does, at every run (audit round 30 F2). The leg's own claim is the small
reader's path: `spoor_read_on` and the open file's own cursor, which the kernel
test calls around.

### A cut kernel-stack table says so, and keeps its peak (2026-11-13)

`/ctl/kstack` had the same silent end and a worse one beside it.
`format_kstack` returned at the first row that did not fit, so past about a
hundred Procs the file was a shorter table with no sign and without its
`peak:` line, the one figure the file exists for. The table's end now has its
room reserved before any row: `KSTACK_TAIL_MAX` is the cut line, the
scan-budget line and the peak line with each number at its widest. Rows end
where the end begins, so the end always fits, and a buffer too small for the
header and the end renders nothing. A table that left a row out says, after
its rows:

    (cut: more processes than one read holds)

Unlike `/ctl/procs`, the walk does not stop there. `format_kstack_cb` goes on
without rendering, so the peak is taken over every Proc the walk reached, its
row shown or not, and the rows shown stay the walk's first. The buffer was
one of this walk's two bounds (a table of about 120 rows ended it), and a
walk that goes on past the cut has lost it. The scan budget alone does not
replace it: a scan is charged by the word, and a Proc that has reaped every
thread -- a zombie -- has a recorded peak and nothing to scan, so a farm of
them, which any program may leave behind, would be walked whole under the
table lock with IRQs masked. So every Proc pays `KSTACK_VISIT_WORDS` (128, the
scan of 1 KiB of stack) out of the same budget before it is looked at, and a
Proc that cannot pay ends the walk like a scan that runs out: the table says
its peak is a lower bound. One read visits at most 1024 Procs. A scan cut
short by the budget stopped below the true frontier, so that Proc's number
reads deeper than the truth and is dropped; the peak is over the Procs scanned
whole. A `_Static_assert` keeps the read buffer at least the widest header
plus the end.

`devctl.kstack_ends_whole` links a Proc of its own whose recorded peak is the
whole stack (`reaped_kstack_peak`), formats the table into every buffer size
up to the whole table's and a little past, and requires of each: the peak line
last, every row five columns, the rows the whole table's first, the cut line
exactly when a row is missing, and the peak naming that Proc whether or not
its row was cut, with at least one size where it was. A shallow Proc of the
test's own is linked after it, so a row always comes before the deepest
Proc's and a walk that stopped at its first cut row would lose the peak
whatever kproc shows. `devctl.kstack_says_truncated` formats with a budget of
its choosing (a test hook): no budget shows no row and the budget line; then
it finds a budget that scans the system as it stands whole, links a farm of
thread-less Procs one visit longer than that budget pays for, and requires
the budget line at every buffer size, the peak line still last, with at least
one size both cut and out of budget; without the farm the same budget scans
the system whole.

### The STATE column shows job-stop, never debug-stop

An ALIVE Proc carrying `job_stop_req` (a `/proc/<pid>/ctl` suspend, or a Ctrl-Z
through the pts path) renders `STOPPED` — the Unix `ps` T-state, via
`proc_state_word` ([[sub-kernel-proc]]), the one function
`/proc/<pid>/status` asks too, so the two files cannot disagree about a
Proc. The DEBUG stop (`debug_stop_req`, the attach-gated debugger
stop) is deliberately **not** surfaced: it is the debugger's private I-39 view,
not a job-control state a monitor should expose, so the render reads
`job_stop_req` alone. That flag is read atomically — a cross-Proc reader holds
`g_proc_table_lock` via `proc_for_each` but takes no per-Proc lock. A dying
Proc (`group_exit_msg` set) is never shown `STOPPED`, whatever flag a stop made
before the kill left: its last thread runs its exit close regardless, and a
dying Proc is not stopped to any reader (DEBUG-FS-DESIGN 5g).
`proc.dying_takes_no_stop` asks `proc_state_word` of a live and a dying Proc
with the same flag set.

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

`g_proc_table_lock` for the process list (taken by `proc_for_each`), and each
Proc's [[lock-proc-threads]] for its threads (the CPU-time and stack-peak
helpers take it themselves).

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
- **A cut process table ends with its cut line, and a whole one never carries
  it.** The line's room stays reserved: only the table's last row may end
  inside it.
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
  time left this list on 2026-10-06 and is owner-only; a reader running as none
  sees only its own row, also since 2026-10-06.) This is the Plan 9 posture and is shared with `/proc/<pid>/status`,
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
