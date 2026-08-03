---
id: sub-kernel-proc
type: sub
title: "The Proc: table, lineage, creation, and wait"
parent: moc-kernel-execution
code: ["kernel/proc.c", "kernel/include/thylacine/proc.h"]
audit: hard
guarded-by: [inv-i1, inv-i32, inv-i33]
validated-by: [gate-smp]
locks: [lock-proc-table]
design: ["docs/ARCHITECTURE.md", "docs/IDENTITY-DESIGN.md"]
created: 2026-08-01
updated: 2026-08-01
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
| `rfork` / `rfork_with_caps` | the sole Proc-creation chokepoint; `RFPROC` only, all other flags **extinct** |
| `proc_find_by_pid` / `proc_for_each` | DFS from `kproc`; the callback runs under [[lock-proc-table]] |
| `wait_pid_for(want_pid, flags, status_out)` | reap a ZOMBIE child, or (PTY-1e) *report* a stopped/continued one; pid/pgrp selectors + `WNOHANG` |
| `proc_setsid` / `setpgid` / `getpgid` / `getsid` | the POSIX session + process-group cores ([[sub-kernel-pts]] and [[sub-kernel-jobctl]] are what read them) |
| `proc_page_charge` / `vma_charge` / `shared_map_charge` (+ uncharges) | the I-32 counters; the **caller must hold `p->vma_lock`** |
| `proc_thread_cap_ok` / `proc_child_cap_ok` | the I-32 creation gates (take the table lock themselves) |
| `proc_apply_identity` | the single audited identity-mutation site |
| `proc_mark_*` / `proc_is_*` | the one-way `proc_flags` stamps and their fail-closed readers |

`proc_stripes` is the unforgeable per-Proc identity a `/srv` peer query
resolves against — a fresh monotonic u64 per `proc_alloc`, never inherited,
`0` reserved as the fail-closed sentinel.

## Mechanism

**Creation.** `rfork_internal` is the one path, and its body is best read as
a ledger of three columns:

- **Inherited** — identity (`principal_id`, `primary_gid`, `supp_gids`),
  session and group (`sid`, `pgid` — POSIX fork semantics, overwriting
  `proc_alloc`'s own-session default), the legate scope tag, the hardware
  allowance (deep-cloned), the environment group (deep-copied), the
  phenotype, `exe_path`, and the CL-5 `page_budget`.
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

## Data structures

`struct Proc` is 400 bytes, grown strictly by appending, with **every**
load-bearing offset individually `_Static_assert`ed and each assert carrying
the reason its field landed where it did. `magic` is at offset 0 so SLUB's
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
guarded by a *different* lock — the I-32 page/VMA/shared-map counters are
exact precisely because their callers hold `p->vma_lock`, the lock that
already serializes the attach/detach path, so check-and-charge is atomic
against a sibling attach.

The child and thread caps are the deliberate exceptions: they read under the
table lock and the increment happens at a later, separate hold, so they
carry a bounded TOCTOU overshoot of at most `ncpus-1` concurrent spawners.
That is stated in the code as acceptable *for a floor* — a bound, not an
accountant.

## Invariants enforced

- [[inv-i1]] — a child gets a *cloned* Territory (`territory_clone`), never
  a shared one; `RFNAMEG` is unimplemented, so per-Proc namespace isolation
  holds by construction at this layer.
- [[inv-i32]] — the four axes charged here (pages, VMAs, shared-in pages,
  children/threads) plus the unforgeable `PRINCIPAL_SYSTEM` exemption.
- The I-2 capability strip — `& ~CAP_ELEVATION_ONLY` on every fork.
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
- The elevation strip is unconditional and must stay so; `caps_mask` alone
  cannot enforce it.
- The `proc_flags` never-inherited rule is what stops a remote-login chain
  from inheriting the local-console trust anchor. A new flag added to the
  word must be atomic-RMW (the word is multi-writer since the SAK).
- `wait_pid_for`'s register-then-observe: the waiter registration and the
  no-zombie scan must stay in **one** critical section.
- The I-32 charge helpers' `p->vma_lock` precondition is what makes the caps
  exact; a caller that charges without it silently degrades them to
  best-effort.
- `proc_find_by_pid` returns an **unrefcounted** pointer
  ([[seam-proc-find-no-refcount]]) — safe today only because every consumer
  either holds the table lock across its use or lets only *values* escape
  (`proc_peer_snapshot_by_stripes` is the model to copy).

## Seams

- [[seam-rfork-flags-unimplemented]] — eight of the nine Plan 9 rfork flags
  extinct.
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
