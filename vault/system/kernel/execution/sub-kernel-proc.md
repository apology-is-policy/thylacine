---
id: sub-kernel-proc
type: sub
title: "The Proc: table, lineage, creation, and wait"
parent: moc-kernel-execution
code: ["kernel/proc.c", "kernel/include/thylacine/proc.h"]
audit: hard
guarded-by: [inv-i1, inv-i32, inv-i33, inv-i44]
validated-by: [gate-smp]
locks: [lock-proc-table]
design: ["docs/ARCHITECTURE.md", "docs/IDENTITY-DESIGN.md", "docs/LINEAGE.md"]
created: 2026-08-01
updated: 2026-09-05
---
## Purpose

A `Proc` is the unit of isolation: one address space, one Territory, one
handle table, one identity, one note queue, one or more Threads. This
dossier owns everything about a Proc that is not its death — the table it
lives in, how it is created, what it inherits, what bounds it, and how its
parent collects it.

The table is not a table. Every Proc is reachable from `kproc` (pid 0)
through `children`/`sibling` pointers, and every lookup is a DFS over that
tree (`proc_find_by_pid_walk`, `proc_for_each_walk`). This is load-bearing
in a way an array would not be: reparenting an orphan is a splice, not a
re-index, and the orphan-adopter fallback (`init`, else `kproc`) is what
keeps the tree rooted and therefore keeps every Proc findable.

## Contract

| Surface | Shape |
|---|---|
| `proc_alloc` / `proc_free` | allocate a KP_ZERO'd Proc with a fresh pid + stripes + pgtable + handle table + note queue; free one that is ZOMBIE with no threads and no children |
| `rfork` / `rfork_with_caps` / `rfork_forked` / `rfork_forked_with_caps` | the sole Proc-creation chokepoint; `RFPROC` **or** `RFPROC\|RFMEM`, every other flag **extincts**. The `_forked_with_caps` variant is the Linux `clone`'s (syscall.c), which passes `caps_mask = CAP_ALL` -- a clone has no caps argument, so the child inherits the parent's full set minus the elevation strip |
| `proc_find_by_pid` / `proc_for_each` | DFS from `kproc`; the callback runs under [[lock-proc-table]] |
| `wait_pid_for(want_pid, flags, status_out)` | reap a ZOMBIE child, or (PTY-1e) *report* a stopped/continued one; pid/pgrp selectors + `WNOHANG` |
| `proc_setsid` / `setpgid` / `getpgid` / `getsid` | the POSIX session + process-group cores ([[sub-kernel-pts]] and [[sub-kernel-jobctl]] are what read them) |
| `proc_page_charge` / `vma_charge` / `shared_map_charge` (+ uncharges) | **policy only** since L-2 — "has an address space" and "is exempt"; the counters and their arithmetic live on the `AddrSpace` ([[lock-vma]] for what the lock does and does not buy) |
| `proc_thread_cap_ok` / `proc_child_cap_ok` | the I-32 creation gates (take the table lock themselves) |
| `proc_apply_identity` | the single audited identity-mutation site |
| `proc_mark_*` / `proc_is_*` | the one-way `proc_flags` stamps and their fail-closed readers |

`proc_stripes` is the unforgeable per-Proc identity a `/srv` peer query
resolves against — a fresh monotonic u64 per `proc_alloc`, never inherited,
`0` reserved as the fail-closed sentinel.

## Mechanism

**Three shapes, one discriminator.** Since the fork arc there are exactly three
answers to "what address space does the child get", and each *is* what its shape
means:

| Shape | Address space | What it is |
|---|---|---|
| kernel entry, no `RFMEM` | a fresh empty one | spawn — the child starts at a kernel entry point and execs, so copying would be waste thrown away at the exec |
| `RFMEM` | the parent's, **shared** | vfork |
| a fork context, no `RFMEM` | a copy-on-write clone | stock `fork()` |

The discriminator is the fork context — the saved caller frame the child will
return onto — and **the handle table two hundred lines below is discriminated by
the same thing, deliberately rather than by coincidence.** A fork context means
the child *is* the parent, continuing on its frame, so it must see the parent's
memory *and* the parent's descriptors or its very next instruction reads
something that is not there. One fact, two consequences.

What is **not** conditional on `RFMEM` is the point Plan 9's flag word exists to
make: the Territory, the note queue, the environment and the handle table remain
the child's own, because each is governed by its own flag and all of those are
still refused. "Shares memory" and "shares file descriptors" are independent
claims — and the Linux shape this eventually serves depends on the separation,
since `posix_spawn` passes the memory-sharing flag *without* the
descriptor-sharing one precisely so the child's descriptor manipulation cannot
disturb the parent.

The clone's reference accounting is worth stating because it makes the failure
paths need nothing bespoke: the clone is born with one reference (the creator's),
allocation takes the child's, the creator drops its own — so an allocation
rollback drops the child's and the creator's unref then takes the last one.

**Creation.** `rfork_internal` is the one path, and its body is best read as
a ledger of three columns:

- **Inherited** — identity (`principal_id`, `primary_gid`, `supp_gids`),
  session and group (`sid`, `pgid` — POSIX fork semantics, overwriting
  `proc_alloc`'s own-session default), the legate scope tag, the hardware
  allowance (deep-cloned), the environment group (deep-copied), the
  phenotype, `exe_path`, and the CL-5 `page_budget`. A **PHENO_LINUX** fork
  additionally gives the child thread the calling thread's **note mask** (#127
  — POSIX fork's signal-mask inheritance; a native fork keeps the zero mask, the
  rfork rule) and preserves the handler-execution snapshot, so a `fork()` issued
  from inside a signal handler produces a child whose saved user context agrees
  with its stack rather than a `KP_ZERO` "not in a handler" that would make its
  handler-return silent UB.
- **Fresh** — pid, `stripes`, pgtable root, handle table, note queue,
  Territory (deep clone), and every I-32 counter.
- **Stripped** — `caps` are `(parent_caps & caps_mask) & ~CAP_ELEVATION_ONLY`
  (the unconditional strip is the load-bearing half: a caller may pass a mask
  containing an elevation-only bit and must still not get it), and
  `proc_flags` are *not copied at all* — so console-attach, may-post-service,
  legate-root, self-managing-notes and console-renderer each grow only
  through their explicit `proc_mark_*`.

Two gates run **before** the expensive allocations: the I-32 child cap, and
`allowance_is_narrowed(parent)` — a hardware-allowance-narrowed Proc may not
create a child at all (Menagerie §13.2: drivers are leaves, so no
hw-capable grandchild can outlive its parent's revoke). Every later failure
rolls back through `state = ZOMBIE; proc_free(child)`, which is why
`proc_free` tolerates NULL territory / NULL env / NULL allowance.

**Allocation ordering** is deliberately fallible-first: the pid and the
`stripes` tag are consumed **last**, after the handle table, pgtable and
note queue have all succeeded, so a rolled-back allocation never sparsifies
either space (R5-H F89's discipline, extended to the tag space).

**Wait.** `wait_pid_for` is one loop with a single authoritative scan under
the table lock. Per iteration it walks `p->children` once, applying the
selector (`-1` any / `>0` that pid / `0` the caller's group / `< -1` group
`-want_pid`) and collecting three facts: is there any matching child, is
there a matching ZOMBIE, is there a matching child with a PTY-1e report
latch. Precedence is exit > continue > stop. Then:

- no match → `-1` (the POSIX `ECHILD` shape);
- zombie → unlink under the lock, then **outside** it spin each Thread's
  `on_cpu` and `thread_free` it, then `proc_free`;
- reportee → return the pid and packed status, consume the latch, and run
  **none** of the teardown (report-is-not-reap, PTY-1e R2-F6);
- `WNOHANG` → `0`, an unambiguous sentinel because pid 0 is never a child;
- otherwise register a stack `poll_waiter` on `child_waiters` *inside the
  same critical section as the no-zombie scan*, release, and park on the
  caller's own private rendez.

That last step is the whole of the #344 multi-waiter lift. The predicate
(`child_wait_ready_cond`) reads **only** `pw->ready` — it touches no lineage
state — which is what dissolved the old `r->lock → proc_table_lock`
inversion candidate and let the `wait_active` guard (which had to *refuse*
a second concurrent waiter, breaking parallel `go build`) be retired.

**The by-pid selector had to reach the kernel's own caller too (#94).**
U-7-pre built the filter and converted the userspace callers, but left the
site that *motivated* it — `kernel/joey.c`'s kproc-waits-for-joey — on the
reap-any form, so the hazard the filter exists to close stayed live in the
kernel. It was reachable, not hypothetical, and the ordering is the whole
bug: when joey exits early its daemon children re-parent to kproc,
`proc_reparent_children` splices each onto the **front** of
`kproc->children` (ahead of joey), and the scan breaks on the first ZOMBIE
it meets. So an already-dead daemon was reaped *instead of* joey, and the
`reaped != pid` check extincted with "wrong pid" — discarding joey's exit
status, i.e. the boot-failure diagnostic, on precisely the branches where a
daemon dying is *why* joey is exiting. kproc now waits by pid. Orphan fate
is unchanged: kproc has never reaped orphans as a service (init is the
reaper; kproc adopts only once init is gone), and the site performed a
single incidental reap before extincting on it. Pinned by
`proc.wait_pid_for_skips_adopted_orphan_zombie`, which builds the boot's
exact arrangement — an adopted orphan ZOMBIE ahead of the target, with
distinct statuses so the pid *and* the status each discriminate.

### Image replacement: what must be reset, and why those things

`proc_exec_replace` swaps a live process's address space in place. The rule
governing everything it clears is one sentence: **the image is gone, so anything
holding an address into it, or a disposition installed by it, is now a pointer
into someone else's program.**

Three such things are reset, and they are worth reading together because the
second and third share a failure mode the first does not.

- **The registered handler entry points.** Their addresses were the old image's.
  The fd-shaped delivery path survives, because it names no address; only the
  registered entries go.
- **The pending-note mask.**
- **The hardware breakpoint and watchpoint slots.**

**The debug slots are the instructive one.** Nothing else disarms them — the
debug state lives until the process is freed — and the context-switch path
**re-arms unconditionally** from the stored counts at every switch. So a
surviving slot fires on whatever now occupies that virtual address and delivers a
stop **in a program the debugger never set a breakpoint in.**

The *attachment* deliberately survives, matching the reference system, which
clears the slots and keeps the tracer. So the reset is per-image, not
per-relationship.

**The pattern: the state that bites at exec is the state some other mechanism
re-arms without asking.** A field nobody touches again is merely stale; a field a
periodic path restores from a count is actively re-installed into the new image,
every switch, forever. When auditing an exec path, enumerate by *who re-arms
this*, not by *what looks like it belongs to the image*.

### The disposition table is reset in place and never freed

The full account of why lives on [[sub-kernel-vivarium]] — the lock-free
cross-process readers, the earlier comment that was true about threads and false
about processes, the store-width hazard.

What belongs here is the boundary it draws around this function's own guarantee.
`proc_exec_alone` bounds **the threads of this process**. It says nothing
whatever about other processes, and that is exactly the half the superseded
comment got wrong. The same gate correctly covers the same-process readers and
covers nothing else.

And the reset is explicitly **not a snapshot**: a lock-free reader on another
processor can see an arbitrary mix of pre- and post-reset entries. Sound, because
every entry it sees was either genuinely installed or the default — but it is a
per-field guarantee, and the first version of the comment claiming it overclaimed
by saying reader-set growth was simply "safe by default".

**The helper's precondition is unenforced, and the reason is a test.** The
production caller extincts on a non-self target; the split-out helper does not,
because the kernel test drives it directly on a process it built and never
scheduled. Stated in the header rather than hidden, which is the right
disposition — but it means the check that protects the production path does not
protect a future second caller.

### The phenotype is committed here — the one store among the clears (Design D)

Everything above is *reset* because the old image's addresses are gone; the
phenotype is the opposite — a value *written*, the new image's ABI shape, decided
by the resolver ([[sub-kernel-stalk]]) and threaded in to `proc_exec_replace` as
`new_pheno`. It is the ONE store of the phenotype, in the infallible commit
region and nowhere earlier: before the address-space swap the load could still
fail and return the caller to its OLD image, which must keep decoding under its
own ABI (the review F1 Leg-B correction). The store is RELEASE — it orders the
swap and the close-on-exec sweep ahead of it for an acquiring reader — but it
deliberately does NOT order the signal reset above, whose plain/relaxed stores a
lock-free cross-Proc reader (notes.c's SIG_IGN hook, the default-disposition
query, the job-stop fan) may observe before OR after this one. So all four
(phenotype, reset-state) combinations are observable, and each is a legitimate
state of ONE image: a native Proc never consults the sigtab; a Linux Proc reads
either the reset all-`SIG_DFL` table (the new image's initial state) or the old
dispositions (the POSIX latitude for a `sigaction` racing an in-flight signal).
This is the commit half of Design D — the decision half is
[[sub-kernel-syscall-dispatch]]'s execve, the resolver seed [[sub-kernel-stalk]]'s.

## Data structures

`struct Proc` is 392 bytes and no longer holds a page table at all — it holds a
pointer to a refcounted address space, which is what makes the vfork and
copy-on-write shapes above expressible. That extraction is the **only change in
the struct's recorded history that ever made it smaller**: 408 → 376 in one
commit, against a run of appends before and after (it is back to 392 through a
socket table and a ring-poll field). Worth naming because the struct is
otherwise grown strictly by appending, with **every** load-bearing offset
individually `_Static_assert`ed and each assert carrying the reason its field
landed where it did. `magic` is at offset 0 so SLUB's
freelist write clobbers it — a double `proc_free` reads the clobbered value
and extincts with a clear diagnostic rather than corrupting.

Notable residents: `child_waiters` (a `poll_waiter_list` that is
byte-identical in layout to the `Rendez` it replaced, which is why the #344
swap kept every following offset stable); `_reserved0` (the retired
`wait_active` guard, kept solely to hold the offsets); `page_peak` and
`phenotype` and `shared_map_pages`, each of which fills an existing tail pad
rather than growing the struct.

The three lifecycle states are `INVALID(0)` / `ALIVE` / `ZOMBIE`, with
`INVALID == 0` asserted so a zero-initialized Proc is detectably unusable.
There is no REAPED state — by the time `wait_pid` returns the pid, the
descriptor is freed and its magic clobbered.

## Concurrency

One lock: [[lock-proc-table]], `g_proc_table_lock`, file-static in `proc.c`
and exposed to `thread.c` only as an acquire/release pair. It guards the
children lists, the `parent` pointers, the ALIVE→ZOMBIE transitions, the
exit status/msg, the companion Thread's EXITING commit, the `sid`/`pgid`
mutations, the report latches, the console-role pointers, and
`g_init_proc`.

Everything else on a Proc is either (a) written once before the Proc runs at
EL0 and read plainly thereafter (identity, `stripes`), (b) an atomic RMW
because it became multi-writer (`proc_flags`, once the SAK kthread started
mutating the console bit from a different thread than the owner), or (c)
guarded by a *different* lock — or, for the I-32 page/VMA/shared-map
counters, (d) **no longer on this structure at all**. Those three moved to the
address space with the mapping list they account for, and their arithmetic is a
compare-and-swap loop that assumes no lock, because the uncharge runs where the
pages actually free — reachable from a handle close, which holds no
address-space lock. What [[lock-vma]] still buys is the *cap decision*: held
across check-then-charge it makes the bound exact against another charge on the
same address space, and charges from outside it can overshoot by at most the
smaller. A floor, not an accountant.

The child and thread caps are the deliberate exceptions: they read under the
table lock and the increment happens at a later, separate hold, so they
carry a bounded TOCTOU overshoot of at most `ncpus-1` concurrent spawners.
That is stated in the code as acceptable *for a floor* — a bound, not an
accountant.

## Invariants enforced

- [[inv-i1]] — a child gets a *cloned* Territory (`territory_clone`), never
  a shared one; `RFNAMEG` is unimplemented, so per-Proc namespace isolation
  holds by construction at this layer. **This survived the address-space
  sharing intact, and that is the flag word earning its keep**: `RFMEM` shares
  memory and nothing else, so two Procs on one address space still hold two
  independent namespaces.
- [[inv-i44]] — an address space now outlives any single Proc and is
  reference-counted, so this file's contribution is the three-shape decision
  above plus the reference discipline around it. The teardown moved with it: the
  VMA drain happens at the address space's last drop, not at Proc free, which is
  what makes the vfork release comparison safe against recycling
  ([[sub-kernel-death]]).
- [[inv-i32]] — the four axes charged here (pages, VMAs, shared-in pages,
  children/threads) plus the unforgeable `PRINCIPAL_SYSTEM` exemption.
- The I-2 capability strip — `& ~CAP_ELEVATION_ONLY` on every fork, the Linux
  `clone` included (`rfork_forked_with_caps` with `caps_mask = CAP_ALL`): a clone
  inherits the parent's full set, but the elevation bits are stripped
  unconditionally, so monotonic reduction holds on the phenotype path too.
- I-43 — `proc_exec_replace` commits the new image's phenotype (above): it sets
  the ABI *shape* the process presents and confers no authority. The decision is
  the resolver's and the dispatcher's; this file only *stores* it, in the
  infallible region, so shape and address space flip together.
- I-22 — `proc_apply_identity` extincts on an attempt to *stamp*
  `PRINCIPAL_SYSTEM` or the INVALID sentinel, so the TCB identity cannot be
  reached from the spawn path; identity confers no caps.
- [[inv-i33]] — `exe_path` is a ref-held `Path` that nothing resolves
  through; NULL is a valid state.

## Error paths

`proc_alloc` returns NULL on any allocation failure (each rolling back
through `proc_free`). `rfork_internal` returns `-1` uniformly: over the
child cap, narrowed-allowance parent, or any allocation failure.
`wait_pid_for` returns `-1` for no-match **and** for "this Proc is
group-terminating" (a `SLEEP_INTR` unwind) — the caller cannot distinguish,
which is correct because in both cases the right move is to stop waiting.
The POSIX cores use `-T_E_ACCES` for every EPERM contour (`-T_E_PERM` would
alias the bare `-1` sentinel), `-T_E_SRCH` for no-such-process,
`-T_E_INVAL` for a negative pgid.

Contract violations extinct rather than returning: `proc_free` of a
non-ZOMBIE Proc, with live threads, or with live children; `rfork` with a
non-`RFPROC` flag; a `proc_mark_*` on a non-ALIVE Proc; `proc_apply_identity`
with an out-of-range gid count.

## Performance

The DFS walkers are O(procs) and run under a global IRQ-off lock, which is
fine for the paths that use them (death, `/proc` reads, the pgrp fans) and
would not be for a hot path — none exists. The orphan rule's walks are
O(procs) *per candidate group*, explicitly justified in the code by death
not being hot. `proc_alloc`'s fallible-first ordering costs nothing;
`wait_pid_for`'s re-scan per wake is O(children), bounded by
`PROC_CHILD_MAX`.

## Prosecution

- The `rfork` ledger: every field is inherited, freshened or stripped
  *deliberately* — a new `struct Proc` field silently defaults to
  KP_ZERO-fresh, which is the safe direction for authority but the **wrong**
  direction for anything POSIX expects to inherit (the `sid`/`pgid` pair is
  the worked example of a field that had to be added to the inherit block).
- **The exec ledger is the same obligation on a different axis, and it is
  enumerated by WHO RE-ARMS.** A new per-process field holding an address into
  the image, or a disposition installed by it, must be reset at image
  replacement. The ones that bite are the ones another mechanism restores
  unconditionally — the hardware debug slots are re-armed from their counts at
  *every* context switch, so a slot left set is not stale, it is actively
  re-installed into the new image forever.
- **`proc_exec_alone` bounds threads, not processes.** Any argument that reaches
  for it as an exclusivity guarantee owes a check on whether the racer is a peer
  thread or another process. It has been misread in that direction once already,
  in a comment that stood for the life of the feature.
- The elevation strip is unconditional and must stay so; `caps_mask` alone
  cannot enforce it.
- The `proc_flags` never-inherited rule is what stops a remote-login chain
  from inheriting the local-console trust anchor. A new flag added to the
  word must be atomic-RMW (the word is multi-writer since the SAK).
- `wait_pid_for`'s register-then-observe: the waiter registration and the
  no-zombie scan must stay in **one** critical section.
- The I-32 charge helpers hold **no** counter state here; they route to the
  address space and decide only exemption. A caller that charges without
  [[lock-vma]] does not corrupt the count — the compare-and-swap prevents a lost
  update — it degrades the *cap decision* to a bounded overshoot.
- `proc_find_by_pid` returns an **unrefcounted** pointer
  ([[seam-proc-find-no-refcount]]) — safe today only because every consumer
  either holds the table lock across its use or lets only *values* escape
  (`proc_peer_snapshot_by_stripes` is the model to copy).

## Seams

- [[seam-rfork-flags-unimplemented]] — **seven** of the nine Plan 9 rfork flags
  now extinct; `RFMEM` was implemented by the fork arc and each of the rest is
  reserved to make its own case rather than inheriting approval from that one.
- [[seam-proc-find-no-refcount]] — no Proc refcounting; lookup returns a
  bare pointer.
- [[seam-legate-member-sweep-race]] — a member spawned racing the legate
  teardown walk is missed (benign: unelevated).
- [[seam-sak-revoke-note]] — the SAK revoke has no note of its own.

## Caveats

- **`proc_free` is not the only teardown.** Since #926/#68 the handle table
  is normally closed at *exit* and `proc_free`'s `handle_table_free(NULL)`
  no-ops; `proc_free` remains the real close only for the direct
  `state=ZOMBIE; proc_free()` rollback and orphan paths. Reading `proc_free`
  alone gives the wrong picture of when a Proc's fds shut. See
  [[sub-kernel-death]].
- **`thread_count` is not "live threads".** It counts *unreaped* threads and
  decrements only at reap, so a joined-then-exits multi-thread Proc has
  `thread_count > 1` with zero live peers. Mistaking one for the other was
  #68 round-2 F2 ([[fnd-68-r2-f2]]); the live count is
  `proc_count_live_peers_locked`.
- **`sizeof(struct Proc)` has been wrong in the reference doc since P2.**
  The absorbed `14-process-model.md` asserted 296.
- `proc_set_exe_path` takes the table lock — added by V-4c-3 F1 after the
  original justification enumerated only *writers* and was silent on the
  cross-Proc `/proc` reader the same commit introduced. A lock the writer
  never takes cannot serialize anything.
- **`proc_setsid`'s comment describes work it does not do, for a case that
  cannot occur.** It says that once the pts registry exists the call "also
  clears any binding owned by the OLD session iff the caller was its leader
  (wired at PTY-1d)". The registry exists, PTY-1d landed, and `setsid`
  touches no registry state — nor does it need to, since a session leader's
  group id is pinned equal to its pid and `setsid` refuses exactly that
  caller, making the stated condition unreachable. The property the design
  relies on holds by a different mechanism entirely; see [[sub-kernel-pts]]
  and task #69.

## Provenance

[[chg-2026-08-15-proc-lineage]] is the re-sweep after the LINEAGE arc: the
address-space extraction, the second accepted rfork shape, and the three-shape
creation decision.
[[chg-2026-09-05-proc-pheno-fork-exec]] brings it current through the phenotype
fork+exec work: `rfork_forked_with_caps` (the Linux clone), the PHENO_LINUX
note-mask inheritance (#127), and Design D's phenotype commit in
`proc_exec_replace`.
