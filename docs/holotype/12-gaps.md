# HOLOTYPE RW-12 — Cross-cut: Gaps (workload-driven W1-W6)

**Status**: CLOSED (see §7 for the verification posture)
**Tier**: STANDARD. **Inventory + registration only — ZERO in-arc edits.** Unlike
RW-11 (which rode 6 trivially-mechanical, decision-free honesty fixes), every
scripture-honesty fix this round surfaced is *coupled to an RW-13 scope or emerge
decision* (is the native toolset THE v1.0 Utopia? which phase is the network? does
the COMPARISON container ✓ become ○? is per-Proc-9P-connection descoped to v1.x?).
Those are scripture-altering decisions that warrant the user's vote — explicitly
RW-13's charter ("re-plan the forward roadmap... lands as a scripture commit with
user signoff"). So RW-12 registers everything and edits nothing; the report IS the
deliverable, and its §5 is the RW-13 input.
**Tip at review start**: `7504e12`.
**Reviewers**: 4 × Fable `holotype-reviewer` (R1 W1 file-service + W2 build-storm /
R2 W3 interactive + W6 editor-TUI / R3 W4 network / R4 W5 containers+limits; all
`claude-fable-5` MODEL start==end, no fallback) + the main-loop self-audit
(SA-1..SA-7, merged at equal authority) + the aux track's `DOC-GAP-REPORT.md`
(G01-G18, the author-against-docs empirical leg).
**Headline**: **60 gap findings, 0 H1** — nothing re-plans the arc immediately, and
the two designed-carrier phases (the network phase, the hardening/rc phase) are
intact. But the round surfaces **two dominant cross-cutting themes** that are the
load-bearing input to RW-13:

1. **Present-tense scripture over deferred surface + pervasive phase-record
   incoherence.** Every band tripped on it. VISION/NOVEL/COMPARISON/ARCH present-tense
   capabilities the tree never built (per-Proc 9P connections, union mounts, advisory
   locks, xattrs, the `/ctl/9p` pipeline observability, the nine-specs claim), and the
   phase tables disagree with each other after the LS arc + the convergence detours +
   the Pouch renumbering re-scoped work without reconciling the older records — the
   network phase is named **five different ways** (Phase 6/7/8) with VISION pointing
   the stack decision at an ARCH §13 that is now "MMIO/VirtIO"; ARCH §23.8 lists Helix
   + PTY + `/tmp`-tmpfs as **Phase-7-exit "must haves"** that LIFE-SUPPORT re-scoped to
   Phase 8/LS; and ARCH §28 **I-20 says PTY "lands with LS-8"** when LS-8's own scope
   *excludes* the master/slave mechanism. This is the RW-10 fossil class, but about
   *phases and capability claims* rather than invariants — and it is exactly what the
   RW-13 consolidation + emerge decision must reconcile.

2. **Two keystone capabilities are unbuilt with no/weak re-entry, and they gate the
   marquee claims.** (a) **exec-from-namespace** — all five spawn variants load the
   binary from the boot cpio (`syscall.c:3612/3798/3868/3969/4401`, `devramfs_lookup`),
   bypassing the territory/stalk entirely, so a container's (or any mounted FS's)
   binaries are *categorically unexecutable*; this is the keystone blocker of the
   COMPARISON "Container = territory ✓" / "OCI ✓" claims, and it is a reverse
   visibility leak (a confined Proc can name+spawn any boot-cpio binary its territory
   cannot hide). (b) the **Resource/DoS floor** — IDENTITY-DESIGN §769 commits it under
   **BUILD** ("a fork/thread/memory bomb is bounded, not box-extincting"); it was never
   built, has no task, and the v1.x quota seam textually *depends* on its per-Proc
   counters, so memory/thread/proc bombs are unbounded today and the seam above it is
   incoherent until the floor exists.

The third notable thread: **namespace introspection is structurally hollow** — the
`/proc/<pid>/ns` file is a `binds: N` stub, and `struct Spoor` retains **no name
field** (verified field-by-field; mounts key on `(dc, devno, qid.path)`), so the
Plan 9 signature tool `ns` cannot be rendered by *any* present-or-future accessor
without a mount-name-retention data-model change. "Discoverable via `ls`"
(COMPARISON §4.1) is hollow until this lands.

This is an honest-positioning round: **the kernel/runtime substrate is broad and
sound** (RW-1..11 proved that); the gaps are (a) the *userland* surface a real
workload needs (a toolchain, an editor, termios/PTY, a clock, env, sockets) — almost
all of it on the recorded LS-arc / Phase-8 / Phase-9 horizon — and (b) the
*reconciliation debt* between what scripture says and what the tree does. Nothing
found is unsound; much found is unfinished, and some of the unfinished is mis-recorded.

---

## §1. The H-tally and the classification split

| H-tier | Count | Meaning | What's here |
|---|---|---|---|
| **H1** | **0** | v1.0-blocking, escalate-now, re-plans the arc | — (nothing) |
| H2 | 19 | should land / be reconciled before v1.0-rc | the keystones (exec-from-ns, resource floor, ns substrate, toolchain), the phase-contradiction cluster (editor/PTY/Ctrl-Z/tmp), the per-Proc-conn + union-mount + socket-shim self-contradictions, and the v1.0-arc-owed LS items (login echo, self-id, clock, child-stdin, namespace builtins) |
| H3 | 29 | v1.x roadmap | the bulk of the net design-pass gaps, the FS-extension families (locks/xattr/links/times), container-assembly pieces, capacity ceilings |
| H4 | 12 | note/record only | mouse, NTP, logging convention, statfs-async, the verified-substrate inventory notes |

**Classification split** (the charter's taxonomy — the *kind* of each gap):

- **SCRIPTURE GAP** (design never considered it — the most serious kind): ~12,
  **concentrated in the network band** (packet filter, DNS/`/net/cs`, TLS cert-bundle,
  net observability, poll-readiness-over-9P, IP config, the listen-side acceptance
  hole, daemon-logging convention) plus the cross-band ones (the `ns`
  mount-name-retention substrate, container pid-visibility scoping, terminal
  mouse/clipboard).
- **SEAM (UNRECORDED)** (deferred but no re-entry written down — the *recordable*
  finding): ~10 (the argv-16 operand ceiling, the child-stdin handoff owner, the
  `/ctl/proc-events/exit` supervision hook, the union-mount deferral, the statfs
  sync-syscall asymmetry, the RTC epoch source under LS-K, the winsize query under
  LS-8b, the pouch-termios boundary-line patch, the principal→name map for `whoami`,
  the general self-caps read).
- **IMPLEMENTATION GAP** (scripture claims it; tree lacks it): ~9 (per-Proc 9P
  connection, union-mount semantics, the `/ctl/9p` observability, the on-system
  toolchain, virtio-net's `/dev/ether0`, the resource floor, the `ns` stub, the
  editor, `/tmp`-`/run` tmpfs).
- **SEAM (recorded)** (deferred *with* a re-entry — fine as-is, inventoried): ~30 (the
  whole LS-6/K/8/9 cluster, the ROADMAP §9 net data-path, the container runner, the
  FS-extension families, the capacity ceilings, the no-seccomp non-goal).

The signal is in the first three categories. The fourth (recorded seams) is the LS
arc + the designed-carrier phases doing their job — most of W3/W6's walls are
correctly recorded LS items.

---

## §2. The workload × gap matrix (what breaks, per band, end-to-end)

| Workload | Runs today? | The walls (finding ids) |
|---|---|---|
| **W1** multi-client file service | **Partially** — 9P client + Stratum + FS-mutation + multi-in-flight pipelining are built and audited; multiple Procs share ONE root client | per-Proc-connection is scripture-vs-reality (F1); union mounts claimed-✓-but-no-op (F2); no shell bind/mount builtins + no ns-read (F3); no advisory locks/xattr/Tbind/reflink (F4); `/ctl/9p` observability + configurable limit + block-at-limit absent (F5); 256-fid/64-handle ceilings are system-wide via F1 (F6); msize-4096 throughput ceiling (F7, ⊇RW-11 #62); the "server library" is a codec, not a framework (F8) |
| **W2** build/compile storm | **No** — the substrate (spawn, pipes, FS-metadata churn, reaping) exists, but there is no compiler/make/git on-system to drive it | no on-system toolchain in any phase's deliverables (F1); no fork/exec for ported code (F2, Phase-8 shim); argv-16 operand ceiling hit in the first hour (F3, UNRECORDED); no wall clock (F4); no atime/mtime setter (F5); no link/symlink/readlink syscalls (F6); sed/awk/find absent with no LS anchor (F7); no env (F8); O_TMPFILE/fallocate/reflink + statfs-async-only (F9) |
| **W3** multi-user interactive | **Yes, single-session** — A-5 login → shell → coreutils → Ctrl-C → logout is built; concurrency is not | login type-blind, no echo/mask (F1, LS-6); no self-identity for id/whoami (F2, LS-K); no wall clock for date (F3, LS-K); no env/PATH (F4, LS-9); no Ctrl-Z/stopped jobs (F5); child stdin unwired — `cat > f` silently empty (F6, orphaned owner); no concurrent sessions (F7, recorded v1.x); no bind/mount builtins (F8, U-8); no self-caps read (F9); aux-staleness housekeeping (F10) |
| **W4** long-running network service | **No** — the virtio-net *driver* (arp/loop/probe) is built; nothing above it | packet filter designed nowhere (F1, SCRIPTURE); socket-shim mapping claimed-vs-ARCH-§11.5-socket-free + "§16 TBD" (F2, CONTRADICTION); DNS/`/net/cs` mechanism undesigned (F3); TLS cert-bundle exit-criterion-only (F4); net observability undesigned (F5); poll-over-9P-net-fds undesigned, dev9p no `.poll` (F6); /net schema+fid-state-machine pre-design, §9.3 spec-waiver mis-scoped (F7); exit criteria 100% client-side, listen-side never proven (F8); phase named 5 ways + ARCH §13 dangling (F9); IP config undesigned (F10); wall-clock LS-K / NTP scripture-gap (F11); supervision recorded but `/ctl/proc-events/exit` hook task-less (F12); daemon-logging convention undesigned (F13); import/exportfs named-only (F14); virtio-net `/dev/ether0` never landed (F15) |
| **W5** container namespace + limits | **Primitive layer only** — territory construction is real + audited; the container story is aspirational | exec-from-namespace — the keystone; container init can never run (F1, ⊃#58); resource/DoS floor BUILD-committed-unbuilt, bombs unbounded (F2); ns introspection is a stub + the Spoor model retains no name (F3); /proc no readdir/enum, stale cmdline, pid-scoping undesigned (F4); restricted /dev unassemblable — no Dev-attach primitive, PGRP_MAX_MOUNTS=8 (F5); thylacine-run/OCI recorded-seam stale-Phase-6 anchor (F6); union mounts MREPL-only (F7, ↔W1-F2); quota seam depends on the unbuilt floor + cites nonexistent `/ctl/mm/` (F8); container /net blocked on W4 (F9); no seccomp — recorded non-goal, coherent once F1 closes (F10) |
| **W6** editor / TUI (termios/PTY) | **No** — the line-editor for the prompt is built; full-screen editing is not | no editor in the tree vs ROADMAP §8.2/ARCH §23.8 (F1, phase contradiction); PTY: one model, three contradictory phase records, I-20 mispointer (F2); no termios/ISIG/echo, ported code can't `isatty()` (F3, LS-8b); cons not pollable (F4, LS-8a); no winsize query (F5, UNRECORDED); TUI baseline inventory (F6, note); mouse/clipboard (F7, scripture-gap, deliberate); `/tmp`-`/run` tmpfs ARCH-§23.8-must-have, unbuilt, no re-entry (F8) |

---

## §3. The keystone findings (the serious gaps — full prosecution)

### Keystone 1 — exec-from-namespace (W5-F1, H2; ⊃ task #58)
All five spawn variants resolve the binary through the **flat global boot-cpio table**,
not the caller's territory: `kernel/syscall.c:3612, 3798, 3868, 3969, 4401`
(`if (devramfs_lookup(name, &cpio_blob, &cpio_size) != 0) return -1;`). There is no
`SYS_EXEC`, no spawn-from-fd, no stalk-resolved spawn (verified: no such entry in the
syscall enum, ceiling 70). Two consequences: **(a)** an OCI image's binaries — or any
binary in a mounted FS — are *categorically unexecutable*, so `thylacine-run` is
unimplementable *in principle* (even the cooperative-shim pattern dies at the final
exec hop), which makes the COMPARISON "OCI ✓" claim unreachable, not merely unfinished;
**(b)** the bypass is a *reverse* visibility leak — a fully confined Proc can name and
spawn ANY boot-cpio binary, because `devramfs_lookup` consults a table the territory
cannot hide, so "what the container sees" does not bound "what it can run" (a direct
dent in "territory isolation is the boundary", ARCH:2351). Registered #58 captured the
authority-argument half (H3, "argue-or-fix at RW-13"); W5 raises it to the **keystone of
the container story** (H2). Disposition: design at RW-13; fold the two consequences into
#58's resolution.

### Keystone 2 — the Resource/DoS floor (W5-F2, H2; no re-entry)
IDENTITY-DESIGN §769-771 lists, **under BUILD (not seam)**: "Resource/DoS floor (minimal
per-Proc page + thread + child caps). Exit: a fork/thread/memory bomb is bounded, not
box-extincting." It was never built. Verified absences: per-Proc memory is bounded only
*per-call* (`BURROW_ATTACH_MAX = 256 MiB`, eager-allocated) with no cumulative
accounting — a loop of attaches drains the buddy; threads have no cap (only the
tid-counter overflow extinction); procs/children have no cap (pids monotonic-never-reused,
`extinction()` at INT_MAX); the *only* bounded axis is fds (`PROC_HANDLE_MAX = 64`).
Grep for `rlimit|cgroup|quota|memory.max` across kernel/mm/arch/usr: zero mechanism.
The threat needs no privilege and no malice (a buggy Pouch port suffices), scripture's
own exit criterion declares the current state ("box-extincting") unacceptable, and the
recorded v1.x quota seam (W5-F8) textually *depends on* "the minimal floor's per-Proc
counters" — so the seam above is incoherent until the floor exists. There is **no task
and no register entry** for the general floor (only the specific fixed instances
HT01.B-F1 / HT02.2B-F1 / HT07.R1-F1). Disposition: register + schedule pre-v1.0-rc.

### Keystone 3 — namespace introspection has no substrate (W5-F3 + W1-F3 + W3-F8, H2)
Three layers deep: **(1)** the file is a stub — `devproc.c::format_ns` emits only
`"binds: <n>"`; **(2)** it renders the wrong table — `binds[]` is the legacy abstract-u32
surface, while the load-bearing namespace is `mounts[]`, which `ns` does not touch;
**(3)** the substrate is absent — `struct Spoor` carries **no name field** (verified
field-by-field, `spoor.h:80-108`: only `qid` + `devno`), and mounts key on Spoor identity
`(dc, devno, qid.path)`, so no path string survives `SYS_MOUNT` — unlike Plan 9, which
keeps `Cname` on every Chan precisely so `ns` can print `bind /a /b`. Therefore **no
accessor, present or future, can render the namespace without a data-model change**
(capture the target path string into `PgrpMount` at mount/chroot time + a
`territory_iter`). The fix is additive today, a sweep later — the cost is monotonically
rising as every new mount consumer lands on the nameless model. This is the deep version
of aux G17 (which, from userspace, could only see "no enumerator"). It pairs with the
self-caps read (G18, W3-F9) under a `/proc/self` introspection cluster, and its
reachability is registered task #57. Disposition: design at RW-13 as the Plan 9 signature
capability (it is also the blocker for the aux `ns` tool, the band's signature deliverable).

---

## §4. The phase-record incoherence cluster (the RW-13 reconciliation input)

The round's dominant cross-cutting theme. These are not new gaps — they are the
*records of existing gaps* being mutually contradictory or stale, which defeats the
purpose of a recorded seam (a future session must be able to find and trust it). Every
one is an RW-13 honesty-pass item; none is fixed in-arc because each requires a scope
decision (which phase? is the claim descoped or scheduled?) that is RW-13's + the user's.

**A. The network phase is named five ways, with a dangling stack-decision pointer.**
- `ROADMAP.md:925` "## 9. Phase 6: Linux compat + network"; `:375` "(Network stack is Phase 7.)"
- `ARCHITECTURE.md:1744` + `NOVEL.md:95` "Lands ROADMAP Phase 8" (ROADMAP Phase 8 is Halcyon)
- `VISION.md:502` "(Phase 7 decision...)"; `VISION.md:315` "ARCHITECTURE.md §13 decides" the stack —
  but ARCH §13 is now "Memory-mapped I/O and VirtIO" (`ARCHITECTURE.md:2097`); ARCH has **no network section** (the §10.1 paragraph is the whole of it)
- `ROADMAP.md:1450` the deliverable map points the network row at "§16 (TBD subsection)"
(W4-F9. Directly affects RW-13 emerge sequencing: "which phase is the network" currently has five answers.)

**B. ARCH §23.8's Phase-7-exit "must haves" were never edited when LIFE-SUPPORT re-scoped to Phase 8/LS.**
- `ARCHITECTURE.md:3281-3285` lists, as Phase-7-exit must-haves: **Helix port**, **PTY
  server (`/dev/ptmx`,`/dev/pts`) + termios via `/dev/consctl`**, **`/tmp`,`/run` as tmpfs**.
- `ROADMAP.md:856` (§8.2 exit) "`hx /etc/hosts` opens Helix"; `:837/:877` PTY "Phase 7 deliverable".
- But `LIFE-SUPPORT.md:377-378, 400-402` re-scopes editors + PTY + Ctrl-Z to LS-7/LS-8/Phase-8,
  and `ROADMAP.md:1214` "PTY is the exception — not yet built (LS-8, #952)".
- There is **no editor and no `usr/pty-server/` in the tree**, and **`/tmp`/`/run` tmpfs has
  no backing, no LS chunk, no task** (W6-F1, W6-F2, W6-F8, W3-F5).

**C. ARCH §28 I-20 mispoints.** I-20 (`ARCHITECTURE.md:3660`) says PTY master/slave
atomicity "lands with LS-8" — but LS-8's enumerated scope (8a pollable cons, 8b
termios/consctl, 8c shell poll loop) contains **no master/slave mechanism**; LS-8's own
text defers `/dev/ptmx`+`/dev/pts` to Phase 8. As written, I-20's validation cannot
happen at LS-8. (Also unresolved: ARCH §25.2 reserves `specs/pty.tla` as a *kernel* spec
while ROADMAP §8.4 places the PTY server in *userspace* — the enforcement home of a §28
kernel invariant inside a userspace 9P server is undecided.) (W6-F2.)

**D. The container-runner anchor is stale.** `thylacine-run` is recorded at ROADMAP §9 +
ARCH §16.5 as "Phase 6" — but the *executed* Phase 6 (Pouch) is CLOSED without it (the
2026-05-04 reorder predates the Pouch renumbering), so the seam points at a phase that
already happened. (W5-F6.)

**E. Present-tense scripture over unbuilt surface** (the RW-10 fossil class, here about
capabilities). A single "claims-honesty" sub-pass over these would close the scripture
half of W1-F1/F2/F4/F5 + W2-F1 + the nine-specs fossil in one commit:
- `ARCHITECTURE.md:2171` "one connection per Proc at v1.0" vs `:2999`-area "a single kernel
  9P client is shared across Procs" (per-Proc-connection self-contradiction; the tree
  shipped the v2.x shared-multiplexed model) — W1-F1.
- `COMPARISON.md:54` "Union mounts native ✓" + `NOVEL.md` Angle #1 "Union mounts via
  bind(MBEFORE|MAFTER)" vs `territory.h:284-285` "only MREPL has distinguished semantics"
  + the `bind_before`/`bind_after` public API that returns success and no-ops — W1-F2/W5-F7.
- `COMPARISON.md:53/105` "Container = territory ✓" / "OCI compat ✓" — aspirational per the
  matrix's own precision standard (W5 verdict); recommend `✓(primitive)` / `○(planned)`.
- `VISION.md:328/333/397` present-tense Tlock/xattr/`cp -a`-xattrs/the full POSIX surface
  vs the kernel client having none of those wire families (W1-F4, W2-F5/F9).
- `NOVEL.md:230-243` "32 (configurable)" + block-at-limit + `/ctl/9p/<session>/*`
  observability vs the compile-time `P9_SESSION_MAX_OUTSTANDING 64` + fail-not-block +
  no `/ctl/9p` (W1-F5; partially reconciled in ARCH §21.10, never propagated to NOVEL).
- The **nine-specs fossil**: `COMPARISON.md:259/271/318` + `NOVEL.md` Angle #8 + `estimate.md:41`
  still say "nine TLA+ specs (... futex, notes, pty)" — the real inventory is 17 modules and
  futex/notes/pty were dropped (2026-05-23). RW-10 fixed the spec-tooling/CLAUDE.md surfaces;
  these positioning docs were out of its invariant-ledger scope. (X-lens overflow → folds here.)

---

## §5. The RW-13 input (consolidated)

What RW-12 hands the consolidation + emerge decision, in priority order:

1. **The scripture reconciliation pass** (theme A-E above) — the single highest-leverage
   RW-13 deliverable. Pick one canonical phase identity for the network; fix the five-way
   naming + the ARCH §13 dangling pointer; edit ARCH §23.8 + ROADMAP §8.2 to move
   editor/PTY/`/tmp`/Ctrl-Z to their LS/Phase-8 homes; repoint I-20 at the real PTY chunk
   and decide its kernel-vs-userspace enforcement home; descope-or-schedule the present-tense
   capability claims (per-Proc conn, union mounts, locks/xattr, `/ctl/9p` obs); sweep the
   nine-specs fossil. **Most of this is the user's scope call**, not a mechanical edit.
2. **The keystones** — exec-from-namespace (#58, raise to H2 + design); the Resource/DoS
   floor (new task, pre-v1.0-rc); the namespace-introspection substrate (the Spoor
   name-retention data-model change + the `ns` render + the `/proc/self` cluster).
3. **The userland-completeness decision** — is the native `ut` + 34 coreutils THE v1.0
   Utopia, with bash/uutils/vim/tmux/ssh/git/gcc/make/python as Phase-8 *additive* ports?
   If so, VISION §13/§397 + NOVEL #1's "Done" + ROADMAP §8.2 need to say it; and the
   toolchain (gcc/make/git) needs a phase home (it is in *none* today, while Phase-9's
   "parallel make" stress criterion presupposes it). + sed/awk/find need an LS anchor.
4. **The W4 net design-pass charter** — convert the seven scripture gaps inside the net
   seam (packet filter, DNS, TLS trust, observability, poll-over-9P, IP config, the
   listen-side acceptance criterion) into *named bullets* of the net phase's design pass,
   so they are owned rather than rediscovered at phase entry; re-scope §9.3's spec-waiver
   to cover the netd fid/dial state machine (the I-9/I-10/I-11 wait/wake class).
5. **The small unrecorded sub-seams** (one line each): the argv-16 operand ceiling (a
   first-hour wall — raise + chunk; LS-9-adjacent); the child-stdin handoff owner (orphaned
   between LS-5 and LS-8; LS-7's editor depends on it); the `/ctl/proc-events/exit`
   supervision hook (task-less since P4-M); the RTC epoch source under LS-K; the winsize
   query under LS-8b; the pouch-termios boundary-line patch; the principal→name map for
   `whoami`; the statfs sync-syscall asymmetry.
6. **The container-runner re-anchor** + the dependency chain it forces (exec-from-ns, the
   /dev attach primitive + PGRP_MAX_MOUNTS sizing, dev-linux/sys-linux/proc-linux, union
   mounts, `/tmp` tmpfs, the COMPARISON ✓→○ honesty).

The LS-arc emerge (LS-6/7/8/K/9) covers W3/W6's recorded walls almost completely; RW-12
confirms its sequencing is sound and adds the sub-seams it should absorb. The net phase
and the container phase are the two big designed-but-undetailed surfaces; RW-12's verdict
is that both are *real recorded seams* whose *interiors* contain the scripture gaps.

---

## §6. Verified PRESENT (what the workloads will stand on — checked, exists, no gap)

So the next reader knows what was covered and survived:

- **FS substrate**: SYS_WALK_CREATE/FSYNC/READDIR/RENAME/UNLINK (54-58), FSTAT/LSEEK/WSTAT
  (50/51/59), atomic-replace rename (FS-gamma audited); dev9p maps server atime/mtime into
  `t_stat` (so mtime *ordering* — make's core comparison — works natively).
- **9P**: multi-in-flight pipelined client (#841 elected-reader, audited); the Loom arc
  COMPLETE (2a-6d); 9P-mode `/srv` posting + open=connect + `SYS_ATTACH_9P_SRV`; corvus is
  the working userspace-9P-server existence proof.
- **Process**: SYS_SPAWN_FULL_ARGV (49); `wait_pid_for` by-pid + WNOHANG (U-7-pre);
  group-terminate (#809/#811); handle-close-at-exit (#926); no fixed Proc-table ceiling
  (ASID rollover removed the 256-Proc extinction); pipes + N-stage shell pipelines.
- **Namespace primitive**: SYS_MOUNT/UNMOUNT/CHROOT/PIVOT_ROOT (path-keyed, Spoor-identity
  mount table → rbind-for-free), I-28 containment + per-component X-search, I-1 isolation,
  displaced-ref teardown; rfork(RFPROC) deep-copies the territory.
- **Session**: A-5 login → identity stamp → per-user encrypted home → shell → logout
  teardown, single-session, complete + audited (RW-6).
- **Interactive**: the `libutopia` line editor (RW-9-sound: CSI parser, history, multi-line,
  UTF-8, Ctrl-R, completion); the console is raw-by-default (raw mode costs nothing today);
  job control (`&`/jobs/fg/bg/wait/kill) landed (U-7); Ctrl-C as a real note (LS-5).
- **Drivers**: virtio-net (arp/loop/probe), virtio-blk/gpu/input; the QEMU user-net + pcap
  harness wired. The driver substrate W4 needs exists; only the stack above it doesn't.
- **Local IPC sockets**: AF_UNIX SOCK_STREAM complete in pouch 0006 with SO_PEERCRED carrying
  the kernel-stamped principal (A-3); the "future SYS_SOCKETPAIR/VSOCK" slot reserved in prose.
- **Self-read (closed aux items)**: `env::args()` (G03/G04 CLOSED); `io::{stdin,stdout,stderr}`
  + `stdio_inherit` (G05/G06 substantially CLOSED); cwd SYS_CHDIR/GETCWD (G07 CLOSED, LS-4);
  static `uname` (G16 half-closed).

---

## §7. Verification posture

- **Reviewers**: 4 × Fable `holotype-reviewer`, all `claude-fable-5`, MODEL start==end (no
  mid-run fallback) — verified on each report's first/last line. R3 (network) + R4
  (containers) read the aux `DOC-GAP-REPORT.md` against the aux worktree (`032b33d`); R1/R2
  consumed it via the main-loop's pre-read (it is absent on `main` — the aux branch is
  unmerged). Full reports captured in the task transcripts.
- **Trust-but-verify** (main loop, independent of reviewers): every keystone claim
  re-confirmed in source before banking — exec-from-cpio (`syscall.c:3612/3798/3868/3969/4401`),
  `struct Spoor` has no name field (`spoor.h:80-108`), the Resource/DoS floor under BUILD
  (`IDENTITY-DESIGN.md:769-771`), `SYS_SPAWN_ARGV_MAX 16`, the per-Proc-connection
  contradiction (`ARCH:2171`), ARCH §23.8's Phase-7 must-haves (`:3281-3285`), the
  socket-free ARCH §11.5 top-50 (`:1922-1937`), the nine-specs fossil (COMPARISON/NOVEL/estimate).
- **Self-audit (SA-1..SA-7)** merged at equal authority; it converged on the Utopia-toolset
  divergence, the ns-introspection stub, the network seam-with-scripture-gaps, the union-mount
  ✓, the no-editor finding, and the `/ctl/9p` observability gap — each deepened by a reviewer
  (R4 found the Spoor name-retention substrate beneath SA-2; R3 found the socket-shim
  contradiction beneath SA-5; R1 found the toolchain-has-no-phase beneath SA-1). SA-3
  (multi-session) was *corrected* by R2: it is a recorded v1.x seam (CORVUS §11), not a
  scripture gap — only the positive v1.0 single-session posture is unrecorded.
- **In-arc edits**: **NONE.** This is a pure inventory/registration round (see the Tier note
  in the header). NOT a dirty close (gaps round; 0 soundness findings; nothing built or edited).
- **Not done**: no boot (gaps are feature-absence, grep-verifiable; the aux track already ran
  the author-against-docs empirical leg); no runtime repro of the unbounded-bomb (W5-F2 rests
  on the verified absence of any bound + scripture's own "box-extincting" framing); the Stratum
  side (whether stratumd honors Tlock/xattr if sent) taken from its docs, not re-read.

**Register**: HT12.* in `docs/holotype/00-register.md`. **Closed list**:
`memory/audit_holotype_rw12_closed_list.md`. **Tasks**: the §5 clusters → #57/#58 updated
+ new tasks for the keystones, the userland decision, the net charter, the LS sub-seams,
and the container re-anchor.
