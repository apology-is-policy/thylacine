# The autonomous-run journal

**What this is for.** After a long autonomous run the operator needs to
reconstruct what happened without stitching together `git log`, six phase-status
rows, and a memory directory. This is that single thread: what landed, in order,
why, what it cost, and what it left open.

**What it is NOT.** Not a changelog — `git log` already has the commits, and
duplicating them here would rot. Not a status doc — `docs/phaseN-status.md` owns
per-chunk rows. What lives here is the *narrative*: the reasoning, the wrong
turns, the findings that were not in anyone's plan, and the decisions that
needed the operator.

**Conventions.**

- Newest run first. Within a run, chronological.
- Every claim carries its evidence: a hash, a measured number, a file:line.
- **A wrong turn is worth more than a win** — record the ones that were caught
  and how, because those are the reusable part.
- **Say what is still open, and be exact about what "fixed" covers.** A half a
  defect closed is written as a half.

---

## 2026-08-17 — the aux-2 merge: two tracks fixed one UAF, and 23 conflicts said which one to keep

Resumed from the self-compaction at `a9a4a4fe` (Warp-C closed). The note said
"merge aux-2 first", and the reason it was first is the interesting part: the
main#243 Fable round had found a P1 (exec leaves `in_handler` set) plus two P2s,
and every one of them was ALREADY FIXED on aux-2 — aux had found the same UAF
(`#254`) the same week, from the other direction. Two independent proofs of the
same defect are worth more than one; two independent FIXES of it are a merge
conflict, and the conflict is where the decision lives.

### The merge itself (`8a58112d`)

104 aux commits over the common base `72ab319d`; 216 main commits the other
way; 23 conflicted files. The rule for every conflict was "which side's version
is the RATIFIED one", not "which is mine":

- **The sigtab UAF, twice.** main `a41fc9eb` reset the table in place through a
  public `proc_exec_reset_dispositions`; aux `c2a09473` + `8690cfb3` + `d3a11c8e`
  did the same through a static `proc_exec_drop_image_state` that ALSO clears
  the in-handler latch (#247 = main F1) and applies the operator-voted
  phenotype rule (F4). Aux's is the superset and is kept as THE one place; main's
  function is gone. What main had that aux did not was the per-8-byte-FIELD
  paragraph and an every-byte-zero test — folded into aux's comment, and the test
  ported onto aux's `_for_test` hook rather than deleted, because it asserts a
  property aux's test does not (a reset that stops early passes aux's).
- **`cons.c`'s mode write.** main's side was a COMMENT change (#233: login must
  set the mode before the prompt); aux's was a semantics change ratified in
  PTY-DESIGN and audited (a write clearing ICANON DELIVERS the pending line).
  Aux's code, plus main's corollary — the disclosure half of #233's race exists
  under either semantics, so the sentence still binds.
- **The bin lists** (`tools/build.sh`, `usr/Cargo.toml`): the union, verified
  programmatically against the base — no member dropped by either side.
- **AUDIT-TRIGGERS.md** was an add/add (both trees created it from CLAUDE.md's
  table on the same day and each appended rows): resolved ROW BY ROW against the
  base row, so main's vault-#170 path fixes and pipe escapes and aux's addenda
  both survive; the LS-8 row carries both sides' addenda in order.
- **147-execve.md's sigtab row** was stale on BOTH sides (main said "zeroed in
  place", aux said "zeroing is exact POSIX because SIG_DFL == 0" — aux's own later
  commit had made the reset phenotype-conditional). Rewritten to the MERGED rule
  rather than picking a stale side; the note-mask and in-handler rows added.
- **Seven ragged doc rows** (six pre-existing on both tips, one in aux's newest
  addendum) escaped with the two controls `85c1ee9c` used: the checker to zero,
  and de-escaped-line == original with only the named lines differing.

**One thing the resume note did not say and the build did:** aux's DISTRO gates
are pool-resident and SOFT-SKIP without the Alpine tarball, which main's cache
did not have. A green `tools/test.sh` with two skipped arc gates is a gate not
run — so the fixtures were copied from aux's cache and the pool + ramfs re-baked
PAIRED (`PRESERVE=0`, fresh key both sides). `arc gates: 2/2 ran -- L-6c=PASS
D-5=PASS` on the merged tree; suite 1424/1424; clade 3/3.

### The main#243 residuals, on the merged tree (F2/F5/F6/F7/F8)

The round's F6 was the sharpest: the 8-byte store width that the whole lock-free
argument rests on was a MEASURED codegen property (a struct assignment happened
to give `stp`), not a construction. It is a construction now — every entry field
is one `__atomic_*` op on an aligned u64 (`_Static_assert`ed), the install
publishes `handler` last with release and readers acquire it, the reset zeroes
`handler` first; objdump shows `str xzr` per field and `stlr`/`ldar` on the
gate. F2 wrote the load-bearing sentence AT `notes_proc_has_live_handler`
("a cross-Proc reader that acts on `handler` alone; the copy is discarded"),
which is the sentence the three earlier statements of the argument had each
left implicit. F5's discrimination was checked the only way that counts: two
sabotages (a reset one entry short; the gate field only) each went RED on the
named assertions, and the tree was reverted with text replacement, not
`git checkout`. F8 clears `clear_child_tid` at exec beside `in_handler`. F7
retired four stale sentences (three of them "X is not a table row" claims that
the LINEAGE arc had falsified without anything failing).

### Meanwhile

The C-0d Fable re-prosecution — the #240 detector's first read from a
different lineage after three Opus rounds — was spawned under the standing
permission while the merge built; its verdict is the next entry's business.

---

> **Two tracks, one thread.** Entries marked `(aux)` were written on `aux-2`
> and merged into this file when aux-2 merged into main (2026-08-17); the two
> tracks ran concurrently, so a main run entry and the aux entries beside it
> overlap in wall-clock time. The `(aux)` block below is in the order aux
> wrote it -- oldest first, `c8ab2744` to `01f076f2`; main's run entries
> below it are newest-first as the convention says.

---

## 2026-08-17 (aux) — the c8ab2744 audit close, and the positive control that caught a second bug

Resumed from aux's **first** self-compaction (the change-of-watch scripts had
been main-only until `4525023a`; the operator had compacted this track by
hand). The nudge fired and the resume note said, correctly, "execute the plan;
do not re-derive it" — the Fable 5 round on `c8ab2744` had reported the audited
change CLEAN and four PRE-EXISTING findings three lines above it, and the fix
plan was already written in `memory/audit_15_closed_list.md`.

### The four fixes (`93a91c6c`)

- **F1 [P1] — both class scans read the sigtab per note.** The terminate scan
  gated on `handler_va` (0 for every Linux guest) and returned the first
  latch-class name at ANY index, so a `SIG_DFL` candidate that fell through
  from the phenotype branch let it name a CAUGHT `tty:hup`/`interrupt` behind
  it and the guest died with its handler installed. #251's per-Proc predicate
  had reached three sites and not this one — the fourth "site N+1" on the row
  (V-8 F2 → #251 → maskstop → F1). Fix: `notes_proc_default_applies(p, name)`
  INSIDE both scans; the fixed-name outer gate on the stop scan retired.
- **F2 [P2] — a `SIG_DFL` `pipe` on PHENO_LINUX reached no arm** (no native
  latch, #237) and sat as the dispatcher candidate for life. Fix,
  phenotype-scoped: `viv_signote_default_is_terminate` + `exits(canonical)`
  from the phenotype branch on the candidate. Native `pipe` untouched; #237
  stays the ABI question it is.
- **F3/F4 [P3]** — the dead drain call deleted with its reasoning; three "an
  uncaught susp is never queued" sentences reworded (caught / all-masked /
  thread-less).

### The wrong turn worth recording: J and L passed on an empty capture

The E2E for F2 is three L-6c legs sharing one fixture — `err=$( { WRITER 2>&3
| head -n 1 ...; } 3>&1 )` — J and L asserting the writer printed NOTHING (killed
by SIGPIPE), K the positive control (`trap "" PIPE` in the writer's own process
→ EPIPE returned → `write error` reported). Boot A: **J green, L green, K red,
`L6C-K-RAW:` empty**, and once per leg on the console:
`/gate/run.sh: line 9: fcntl(3,F_DUPFD,10): No file descriptors available`.

busybox ash's `redirect()` probes the TARGET fd of every `N>&M` with
`fcntl(N, F_DUPFD, 10)` to learn whether N is open — `EBADF` means "not open,
nothing to save"; anything else is "strange" and aborts the command. The
vivarium's `VIV_FCNTL_DUPFD` arm answered `EMFILE` for BOTH of
`handle_dup_posix`'s folded failures, on a comment arguing that a guest which
just used the fd knows it exists. True about the wrong caller. So the whole
capture never ran, the substitution yielded "", and two negatives were
satisfied by a broken fixture — aux#215's class, caught by the remedy aux#215
prescribes. Without K this would have shipped as two green legs proving
nothing. Fixed in the same commit (a liveness re-check after a failed dup:
closed → `EBADF`, residual → `EMFILE`; `vivarium.fcntl_dupfd_errnos`).

Boot A2 then showed a second fixture wart: `head -n 1 >/dev/null` printed
`can't create /dev/null: Function not implemented` — ash opens `>` with
`O_CREAT|O_TRUNC` and `O_CREAT` is a KNOWN unserved openat flag (#201, designed
around). The legs still measured SIGPIPE correctly (the reader slot died before
reading instead of after one line), but a fixture must not lean on a known
gap: the reader now writes its one line INTO the capture, so J's assertion is
the sharper "the capture is EXACTLY `y`" — the reader really read, the writer
was silent.

### The bar

Suite 1405/1405 (+2). Sabotages, each reddening its named assertion and
nothing else: S1 (terminate gate dropped) → `A: the terminate scan does NOT
name the CAUGHT interrupt`; S2 (stop gate dropped) → `D: the stop PREDICATE
declines a caught susp`; S3 (phenotype `exits()` disabled) → suite green,
L-6c `first-missing=L6C-J`, L missing, K present. pty + pty_stop: 4 clean/
liveness cfgs green, 6 buggy cfgs violate (rc 12/13) — after fixing the runner,
which first "passed" all ten legs in 0 s because `/usr/bin/java` is the macOS
stub and every rc was 1 for the wrong reason (the buggy legs read as
violations). Keyed on the exit code AND the `TLC2 Version` banner now. SMP gate
40/40 (default+UBSan × smp4/smp8, N=10, 0 corruption). LS-CI 33 PASS + 2 SKIP (GL not
baked) — and pty-4 burned a retry AGAIN, this time INTO the failure-time probe
landed at `11173762`: see the next entry, because the probe answered.

### Still open leaving this run

- #237 (native `pipe` has no latch) is sharper, not closed: the phenotype
  answers SIG_DFL SIGPIPE for its own Procs; a native handler-less, fd-less
  program still keeps a stranded `pipe` note.
- The tail's delivery-time SIG_IGN discard arm is reached by nothing (second
  unconstructed state on this row); its own chunk.
- `>/dev/null` from a Linux shell under viv fails on `O_CREAT` (#201) — the
  most common redirection in existence; the L-6c fixture routes around it.
- pty-4's burned retry: instrumented, not diagnosed.

## 2026-08-17 (aux) — pty-4's burned retry, diagnosed on the probe's first miss: the ldisc flushed type-ahead

The failure-time probe landed at `11173762` the day before, on the theory that
INPUT truncation and OUTPUT loss are indistinguishable in a plain capture and
only the guest can say which. Its first miss (LS-CI batch 6 of the c8ab2744
close bar) said, in order: `[listen]` — the raw stream showed `sle` as PLAIN
echoed text after `PTY-INNER`, then only SIX empty editor redraws where the
passing attempt shows NINE (`sleep 30\r`); `[jobs]` — nothing listed;
`[channel alive?]` — the editor answered; VM alive, bridge alive. The editor
never echoes typed text (the harness header says so), so plain `sle` can only be
the pts line discipline echoing in cooked mode.

So: `lc_run_expect` returns the instant `PTY-INNER` is SEEN — before `ut` has
reaped the pipeline, restored PROMPT_MODE and redrawn — and `lc_send "sleep 30"`
fires at once. On TCG the window is sometimes wide enough that `s`,`l`,`e` land
in CHILD_MODE (+icanon +echo): assembled, echoed, then ut writes PROMPT_MODE and
ptyfs `ctl_apply` does `p.line_len = 0; // TCSAFLUSH: a mode change resets the
assembly` — the three bytes are gone and `ep 30\r` reaches the raw editor. A
race, and a real one — but the DEFECT is the guest's: Plan 9's `devcons` `rawon`
pushes the partial line to the reader ("flush output on rawoff -> rawon", the
clumsy-hack zero byte), Linux's `n_tty_set_termios` never discards on a canon
change, and TCSAFLUSH is a caller-chosen flush that bash/readline deliberately
do NOT use (`TCSADRAIN`). Thylacine's ctl grammar offered no choice: every mode
write flushed. Type-ahead across a job's end — a paste of two lines, a script
driving a pts, LS-CI — lost the HEAD of the next line and executed the TAIL.

The posture came from the LS-8b audit's F1 remedy ("a fragment stranded across
canonical→raw→canonical prepends the next line"), copied per-pts by PTY-2c, on
the stated premise that "no current consumer flips mid-line". The premise was
falsified by the one consumer that flips around every foreground job. Both
ldiscs now DELIVER on ICANON-clear and touch nothing otherwise
(`c62eb738` scripture, PTY-DESIGN "Mode writes deliver, never discard"; the impl
`ccb597b8`): the F1 hazard stays closed because canonical→raw delivers, so nothing is
stranded, and I-20's byte conservation now holds across a mode write. A
delivery into a full ring is a real drop under a new counter
(`rx_drop_modeflush`, the #95 rule). Not built: an explicit flush verb — pouch's
`TCSETS/SW/SF` all map to the one write, which now behaves like `TCSANOW`.

Two things worth keeping from this: (1) the instrument earned its keep on its
FIRST miss, and the reason it could is that it asked the guest in a fixed order
with a control at the end (`channel alive?`); (2) a "posture" chosen as an audit
remedy is still a claim about consumers, and consumers change — the sentence
"no current consumer flips mid-line" was true when written and had no test.

## 2026-08-17 (aux) — the "reached by nothing" discard arm, and why the right fix moved the mechanism instead of reaching it

Resumed from aux's **second** self-compaction (`05708496`). The resume note's
first item was to ask the operator for the owed prosecutor round on `ccb597b8`;
the ready-to-paste prompt was written first
(`memory/audit_ccb597b8_prosecutor_prompt.md`), my own self-audit of the
mode-write delivery found nothing, the operator said yes, and the round is
running in the background while this chunk lands (its files — `cons.c`,
ptyfs — are disjoint from this one's).

### The chunk: Stream 4's "delivery-time SIG_IGN discard arm is reached by nothing"

The open item was the second member of the unconstructed-state class found by
sweeping after the maskstop one: `notes_deliver_at_el0_return`'s phenotype
branch discards a candidate whose disposition is `SIG_IGN`, and its own comment
named the only way in — "a note queued BEFORE the install is still sitting
here … this is the only place that can happen." The post-time hook prevents the
state in every ordinary ordering, so the arm needed a CONSTRUCTED one.

Constructing it in-guest turned out to be easy — `viv-pheno-probe` has raised
SIGPIPE at will since V-6c (fd 0 is a reader-less pipe write end), so
`block → write → SIG_IGN → unblock` is deterministic. But writing the legs
forced the question the arm's comment had skated over: **what does POSIX say
happens to a pending, blocked signal when its disposition becomes `SIG_IGN`?**
It is discarded AT THE INSTALL, "whether or not it is blocked" (2.4.3; Linux
`do_sigaction` → `flush_sigqueue_mask`). Thylacine discarded at the next
delivery instead. Same answer for `pending → SIG_IGN → unblock`; a DIFFERENT
answer for `pending → SIG_IGN → handler → unblock` — Linux fires nothing, the
tail ran the handler for a signal POSIX says had died. So the arm was not merely
uncovered; the mechanism it implemented was wrong on the ordering nobody had
tested, and the honest fix is not to reach the arm but to move the discard.

What landed: `notes_discard_name(p, name)` — under `q->lock`, remove every
queued note of one name, mask-blind, each removal draining the class latch as a
dequeue does (an `interrupt` armed under `SIG_DFL`, then ignored while blocked,
must not leave a Proc whose every sleep is `*_INTR`), `kill` refused; the
phenotype `rt_sigaction` shell calls it after the store whenever the new
disposition ignores (`SIG_IGN`, or `SIG_DFL` for a default-ignore signal — the
no-table `SIG_DFL` shortcut now skips only the store); and `notes_post`'s
disposition read moved UNDER `q->lock`, so store-then-lock against
read-under-lock leaves no interleaving with a stale ignored note. The tail's
arm stays as defense-in-depth — its absence would hand a stale note to the
`SIG_DFL`-terminate arm — with its comment rewritten to say exactly that.

The proof: `notes.discard_name_purges_pending` (mask-blind, per-CLASS latch
drain — tty:hup out leaves the TTY latch armed for tty:quit — survivor order,
`kill` refused, a purged FULL ring really empty: 16 out, 16 in) and probe legs
L205–L216. Round A: pending → `SIG_IGN` → unblock survives with nothing fired
(L209 is PRE-STAMPED and rewound so a death names its leg instead of leaving
joey's `??` — the marker channel is fail-only by design, and this is the one
place a marker is written before the verdict is known), then a handler
installed after is not handed a stale note (L210). Round B: pending →
`SIG_IGN` → handler → unblock fires NOTHING (L215 — the install-vs-delivery
leg; red on the tree before this chunk). Each round ends with a fresh SIGPIPE
delivered exactly once, so a queue wedged by the experiment cannot read as
"nothing fired".

### Found on the way, enqueued not fixed

Reading `proc_exec_drop_image_state` for the exec-time sigtab reset: it zeroes
every row and the mask, and its comment says "Zeroing is exact POSIX". True of
CAUGHT handlers; false of `SIG_IGN` and of the blocked mask, both of which POSIX
and Linux keep across `execve` (`nohup`, `sh -c 'cmd &'`, `trap '' INT; exec`
all depend on it). ARCH §7.6 names the clear as the NATIVE rule, so the fix is
phenotype-conditional and a scripture decision — surfaced with options in
`memory/bug_exec_resets_sigign_and_mask_phenotype.md`; recommendation:
phenotype keeps `SIG_IGN` + mask.

### The bar (`7580c1f7`)

Suite 1406/1406 (+1); V-1b PASS (L205–L216 green); L-6c PASS. Sabotages, each
reddening exactly its named assertion: S1 (the shell never purges) → V-1b
`marker=L215` — and NOT L209, because the tail's arm still saved that ordering,
which is the whole reason the arm stays; S2 (S1 + the tail's `SIG_IGN` disjunct
deleted) → `marker=L209` — the guest died at the unblock and the pre-stamp named
the leg; S3 (purge without the latch drain) → the unit test at "removing the
last interrupt drained the latch", 1405/1406. SMP gate + LS-CI ran over the tip
together with the round close below (see the fixup).

## 2026-08-17 (aux) — the ccb597b8 round came back: sound delivery, an unwitnessed counter

The operator said yes to the round while the chunk above was being built; the
prosecutor (Fable 5, read-only) ran ~20 minutes and reported 0 P0 / 0 P1 / 2 P2
/ 6 P3 — every finding on the NEW DROP SITE's witness, none on the delivery it
was asked to break. It re-derived the I-9 wake pairing, the poll relay, the SMP
ordering under `g_cons.lock`, the hook/production parity and ptyfs's
single-threaded ordering line by line and found them as claimed.

What it found instead is worth keeping. **F1**: the fifth drop site's counting
path had only a NEGATIVE test in both ldiscs — leg B "it fit, no drop counted"
against an empty ring — so a misattribution to `rx_drop_ring` (the must-stay-
zero witness) or not counting at all read green. The tree's own
`test_cons_rx_drop_counters` header says exactly why that is worse than no
counter, and I had shipped one anyway because the negative FELT like coverage.
Legs (d)/(e) now drive the site (512 filler + 10 pending → 10 counted here,
every sibling asserted unmoved, filler intact; 507 + 10 → the 5-byte PREFIX
delivered in order); the ptyfs selftest drives its site on a fresh pts.
**F2**: ptyfs had folded that drop into `drop_flush` — against PTY-DESIGN,
which named "its own counter" for BOTH ldiscs, and against `drop_flush`'s own
documented shape (a short cooked flush loses tail + newline so the line never
runs; a short mode-flush loses the tail and the terminator arrives raw, so the
truncated command RUNS — #95's exact shape, hidden under a name whose doc said
it could not produce it). One of two twins diverged from a rule written for
both, and a re-read of the scripture would have caught it. **F3/F4/F6/F7**: the
one-shot report did not name the new site; the "reachable only by a wedged
reader" claim was false (ut re-arms before it drains, so a paste can reach it);
three comments still said TCSAFLUSH; 111-cons.md carried the deleted test with
the reversed semantics. **F8**: pty-4's type-ahead leg had no ARMED witness —
bytes landing raw before CHILD_MODE or after the re-arm satisfied the cursor-35
anchor too, under the old posture as well; it now first requires the pts's
cooked echo as plain text directly after the CRLF, which only CHILD_MODE cooking
produces. **F5** stays open as a scripture vote: an ISIG-consumed ^C/^\/^Z does
not flush the pending canonical line (POSIX and Linux do; Plan 9 does not) —
the old reset masked it, delivery makes it visible; recommendation: adopt POSIX
in both ldiscs.

Closed at `56b5a412`: suite 1406/1406; S7 (kernel misattributes to
`rx_drop_ring`) → "(d) modeflush counts exactly the 10 bytes the full ring could
not take"; S8 (ptyfs folds into `drop_flush`) → `ptyfs: selftest FAIL:
modeflush-drop-not-counted`, boot-fatal.

### The bar over the tip (`56b5a412`, both commits)

One run for both (disjoint surfaces): SMP gate 40/40 — default + UBSan ×
smp4/smp8, N=10, 0 corruption / 0 external-kill / 0 other, in two halves —
then LS-CI in six batches on TCG: 33 PASS + 2 SKIP (the GL half is not baked
into this pool; not a guest result, not coverage). pty-4 passed WITH the new
armed witness (the pts's cooked echo matched before the cursor-35 anchor — the
delivery path was exercised, not merely reached). Pushed to both mirrors after
the fixup.

## 2026-08-17 (aux) — the votes came back: ISIG discards, fork/exec goes POSIX, and the 7580c1f7 round

The operator answered all three questions in one round: spawn the 7580c1f7
round (yes), F5 (adopt POSIX — an ISIG character discards the pending line in
both ldiscs), and the exec item (the phenotype keeps `SIG_IGN` + the mask). Each
landed scripture-first.

**F5** (`e69e9baf` scripture, `4df51c30` impl): the kernel ISIG arm and the
ptyfs ISIG arm zero the pending assembly when ICANON is set — a disposition like
an erase, not a counted drop, deliberately narrower than POSIX's full flush
(committed lines in the ring stay; output is never flushed — the console TX ring
carries kernel diagnostics). The PTY-3 pouch probe's leg H had pinned the OLD
posture (`x` ^C `y` CR → `xy\n`) and went red on the first boot — the fixture
that encoded the divergence, found by the change that removed it; updated to
`y\n` as on Linux. Sabotages S9/S10 each red on the named check.

**fork/exec** (`c484a7d1` scripture): reading `proc_exec_drop_image_state` for
the exec half surfaced the fork half too — task #127, recorded at L-3d as "two
behaviours and a design decision", never landed. So the chunk is the pair:
`rfork` copies the parent's sigtab into the child's OWN table (before the child
is postable) plus the caller's `note_mask`; `execve` resets caught rows only and
keeps `SIG_IGN` + the mask; native keeps the Plan 9 clear. Probe legs L217–L228
drive a real fork and a real exec (the children name the first wrong fact
through the report dup); the unit test pins the two primitives.

**The 7580c1f7 round** (Fable 5, 0/0/0/4) re-derived the install-time discard
SOUND — the linearization, the primitive, the shell, the pre-stamp arithmetic —
and found the one ordering nobody had tested: `block; SIG_IGN; raise; handler;
unblock`. Linux queues a blocked ignored signal ("the handler may change by the
time it is unblocked") and discards at dequeue; Thylacine drops at generation,
mask-blind. POSIX 2.4.1 permits both, so it is recorded as a stated divergence
rather than matched — but the docs had said "exactly as Linux", and the lesson
worth keeping is that "exactly as X" is a claim about every ordering. F1: the
SIG_DFL/default-ignore purge disjunct had no driver → L229–L232 with a positive
control (S13 reddens only the negative). F2/F4: an over-claiming comment and two
stale sentences.

### The bar over the tip (`d3a11c8e`: F5 + fork/exec + the round close)

SMP gate 40/40 (default + UBSan × smp4/smp8, N=10, 0 corruption / 0
external-kill / 0 other, two halves); LS-CI 33 PASS + 2 SKIP (GL not baked);
suite 1408/1408 per commit; sabotages S9/S10 (F5) and S11–S15 (fork/exec)
each red on the named check — S14/S15 are the WIRING witnesses (the unit test
cannot see proc.c; the probe legs L223/L226 can, and they went red). Pushed to
both mirrors after the fixup.

## 2026-08-17 (aux) — the d3a11c8e round: the fork rule was one field short

The operator said spawn; the round (Fable 5, read-only, 0/0/1/6) re-derived
both mechanisms sound — the fork copy is published before the child is
reachable and aliases nothing, the exec reset uses the same "caught" predicate
delivery uses, the ISIG discard is one field under the right lock in both ldiscs
— and found the one place the voted RULE was short. "fork copies everything
(POSIX fork(2))" copied what POSIX names: dispositions and mask. This design has
a third piece of thread signal state POSIX never has to name, because Linux
keeps it on the user stack: the kernel-side handler-execution snapshot (the
sigframe here is written for reading; `rt_sigreturn` restores from the
per-Thread save block). A `fork()` issued from INSIDE a handler — async-signal-
safe, POSIX-permitted — therefore produced a child whose user stack said "in a
handler" while its KP_ZERO thread said "not"; its handler return was refused
and it ran on past the svc into whatever followed the restorer (musl: silent UB;
the probe: `brk #0`). Fork+exec and fork+`_exit` from a handler were fine, which
is why nothing had surfaced. Fixed by copying the block with the mask
(`in_handler` written last, before `ready()`); phenotype only — a Plan 9 child
is not notified. Lesson: enumerate what the RESTORE path reads, not what the
standard lists.

The witness leg cost two extra boots for a reason worth keeping: its first
draft had the child exit 3 and the parent reap "exactly 3", and it went red on a
WORKING fix — v1.0's phenotype exit path collapses every non-zero
`exit_group(N)` to 1 (VIVARIUM task #91, "`exit(N)` is boolean"). A diag with
`exit(5)` read as 1 too. So the oracle is exit 0 versus anything else, and the
child's own marker (re-emitted by the parent on failure) carries the why. A
status oracle must be a value the status channel can carry.

Six P3s: a pre-#254 "known hazard" paragraph in `proc_exec_replace` that
contradicted the in-place reset it now calls; a phantom `viv_sigtab_copy_into`
in 145; PTY-DESIGN naming leg (f) for (e4); the ptyfs (e4) leg with no witness
for "m2s/s2m are NOT flushed" (both were EMPTY at the VINTR, so an over-broad
discard passed — it now commits `x\n` unread and leaves the echoes unread and
asserts both survive); the fcntl test's header comment migrated onto the sigtab
test; and the ISIG-DISCARD + ccb597b8-ROUND addenda living only on the
AUDIT-TRIGGERS rows that declare ARCH 25.4 authoritative (mirrored). Enqueued
from the observations: `Proc.socktab` is not cloned at fork (the fork half of
the LINEAGE dup3 note — a real L-6 gap for fork-per-connection servers), the
handler mask discipline (sa_mask|sig never applied during a handler; sigreturn
does not restore the mask), and `pty.tla`'s CookSignal echoing a char neither
ldisc echoes.

## 2026-08-17 (aux) — the console TX ring pushes UNITS now

Main handed over the byte-atomic tear it measured on thyla-pi: `proc: orphan
pid=2119 name="ttaappeessttrryydd"` — the kernel's orphan-adoption burst and
tapestryd's posture line on another CPU, byte for byte, because every producer
pushed each byte under its own `g_cons_tx.lock` hold and the writer role cannot
serialize a diagnostic emitter (IRQ context; the role sleeps). ARCH 23.5.2 had
already named the missing piece — "full echo-exclusion via a bulk-push fast
path" was #79, a documented v1.x item withdrawn from an earlier draft because it
"carries a two-ring lock-ordering design". The design point resolved as: never
nested. Tap under the drain lock, release, push under the ring lock, release.

The rule now: every producer pushes a UNIT under one hold. A kernel diagnostic
is a line assembled on the caller's stack (`struct cons_diag_line`) and pushed
once, all-or-nothing — the per-token trio is gone, because a per-token API
cannot be line-atomic without hidden state, and a per-CPU accumulator would
splice an IRQ handler's line into the process-context line half-assembled below
it; a caller-owned object is nesting-safe by construction. Echo pushes its
staged unit whole (half a `\b \b` walks the cursor over the prompt). The role
writer stages a 512-byte chunk, cuts it back to the last NL when the input
continues, pushes what fits and room-waits for the rest — so a ring-fitting
write, which is every console line, is whole against every producer. The
residual is named and Linux-equivalent: a long write spans chunks; a FULL ring
splits at a chunk boundary, because progress beats atomicity under congestion.

Three tests, one of them the tear's own witness: two kthreads hammer a STALLED
ring with 64-byte units from two CPUs, the ring is read back through a new peek
hook and parsed as frames, and every frame must be one producer's unit — with an
overlap witness so the test says whether the interleave was exercised (it was).
The other two pin the boundary deterministically on one CPU: room = len-1 moves
the count by zero and `dropped` by exactly len; room = len lands whole.

### The bar over the tip (`277b02cc`: the round close + the TX-ring unit)

SMP gate 40/40 (default + UBSan × smp4/smp8, N=10, 0 corruption / 0
external-kill / 0 other, two halves — the kernel byte-changed, so the whole
matrix re-ran); LS-CI 33 PASS + 2 SKIP (GL not baked; six batches, TCG); suite
1408/1408 (`920bbfca`) and 1411/1411 (`277b02cc`) per commit; sabotages
SF1/S16/S17/SP5 (the round close) and S1–S3 (the unit rule) each red on the
named check. Pushed to both mirrors after the fixup.

A number corrected on the way: three earlier bar stanzas and four status rows
said "LS-CI 34 PASS + 2 SKIP". Every bar today measured 33 + 2 over the same 35
scenarios, and so did the two before it; the 34 came from the c8ab2744 close's
"36 scenarios" — an `ls tools/interactive/*.exp` count that included `lib.exp`
— minus the two SKIPs. A derived figure propagated as a measured one, six
times, before a run's own tally was set beside it. The tally is now taken from
the harness's `==> LS-CI:` lines only.

## 2026-08-17 (aux) — the handler-time mask is Linux's; three socket findings; a file count that was not a scenario count

Item 7 of the notes line was the smallest thing on the queue and the only one
without a vote in front of it (the #237 `pipe` default and the socktab posture
both alter user-signed scripture), so it went first while the votes ride the
report. The d3a11c8e round had recorded two permissive-direction divergences:
delivery never applied `sa_mask | sig` while a handler ran — N-3's blanket
`in_handler` guard stood in for it — and `rt_sigreturn` did not restore
`note_mask`, so a handler's own `rt_sigprocmask` outlived the handler, and an
`execve` from inside a handler handed the image the PRE-handler mask where
Linux hands it mask | sa_mask | sig.

The change is three lines and a field. `notes_deliver_linux_locked` saves the
pre-handler mask into a new `Thread.note_saved_mask` and stores Linux's
`signal_delivered` value — mask | sa_mask | sig, sig omitted under
`SA_NODEFER`, both additions through the same coarse translation as
`rt_sigprocmask` (a tty-family `sa_mask` entry blocks the family; SIGKILL is
dropped); the phenotype's `rt_sigreturn` restores the saved mask, gated on
`t->proc->phenotype` because a PHENO_LINUX Proc reaches delivery only through
the Linux path and a native Proc never does; and the fork-from-inside-a-handler
copy from the round's F1 gained the field — the round's own lesson, "enumerate
what the restore reads", applied to the next field. Delivery is untouched: the
guard still holds every note for the handler's duration (VIVARIUM 6.22's stated
conservative imprecision), so what changed is the mask a handler OBSERVES and
PASSES ON. The frame's `uc_sigmask` still carries the pre-handler mask and is
written for reading — a handler that edits it changes nothing, which Linux
would honour; recorded as the conservative-direction divergence of this frame
design. Native `noted` keeps the as-built rule.

Two things the witness taught. A signal with no note (SIGUSR1/2) reads back
CLEAR whatever is blocked — the translation has nothing to set — so a
`sa_mask = {SIGUSR1}` witness would have proved nothing; the legs use SIGINT,
SIGCHLD, SIGWINCH and SIGPIPE, one note bit each. And the pre-handler mask is
{SIGCHLD}, non-zero on purpose: a restore that puts back ZERO is
indistinguishable from a correct one against an empty pre-handler mask, and
the fork leg (the child forked from inside the handler restores at ITS
sigreturn) is exactly the leg a missing copy would pass with zero. The Thread
grew by a u64 and its size did not change — the 8 bytes landed in the pad
before the 16-aligned FP area — and that was measured with
`-fdump-record-layouts` before the size assert's message said so, not derived.

The first boot reddened the "handler's own block undone" leg on a WORKING
restore, and the reason is the reusable part: probe leg L26, far above, blocks
SIGWINCH to assert the tty family's honest over-report — and nothing since
unblocks it. So the pre-handler mask carried the tty bit, the restore put it
back exactly as it should, and the leg read that as "the block persisted". A
premise assumed is a premise that can be false without anyone's fault; it is
now asserted as its own leg (L237: the pre-handler mask is exactly {SIGCHLD}),
with the tty family unblocked first and re-blocked after so the legs below run
under the state they always had.

Sabotages, each red on exactly its named check: SM1 (no handler-time store) →
probe L239 (the mask inside lacks sa_mask|sig; 1413/1413); SM2 (no restore) →
`notes.phenotype_sigreturn_restores_mask` leg A (1412/1413 — the suite fails
first, so the probe is not reached; L240/L241 had already shown they
discriminate, on the premise failure above); SM3 (the fork copy skips the
field) → probe L244 only (the child forked from inside the handler restores
zero, and zero is not {SIGCHLD}).

### Three socket findings, from reading before touching

The socktab item (fork does not clone it) was researched instead of started,
and the research moved it. The enqueued plan said "a refcounted entry"; a
refcounted ENTRY cannot carry the ctl->data handle swap `connect` performs in
one table, so it reproduces Linux no better than a per-process copy — and a
per-process copy is Plan 9 APE's own posture (rocks live in process memory;
fork copies them). Every fork shape that occurs (accept-then-fork,
prefork-accept) works under a copy; the divergence — a state mutation through
one alias not seen through another — is the one LINEAGE already published for
dup3. VIVARIUM 5.5.2 states today's "not rfork-inherited" as design, so the
flip is the operator's vote (`memory/design_socktab_across_images.md`).

Alongside it, two defects verified in the tree. `handle_close_on_exec` closes
a close-on-exec socket handle and pays no socktab drop, and `fcntl(F_SETFD,
FD_CLOEXEC)` is a served row — so `socket; fcntl; execve` leaves a stale
(proto, N) entry keyed on a number the new image's next fd-creating call is
handed: the "dial verb to a stranger" class the V-5 header names as the
sharpest this table can have, reached through exec rather than dup. And the
reach is wide, because of the third finding: `socket()` answers EINVAL for
`SOCK_CLOEXEC|SOCK_NONBLOCK` "rather than masking them off", and EINVAL is
exactly musl 1.2.5's fallback trigger (`third_party/musl/src/network/socket.c`):
it retries without the flags, then issues `fcntl(F_SETFD, FD_CLOEXEC)` — served,
so every musl `SOCK_CLOEXEC` socket reaches the stale-entry path — and
`fcntl(F_SETFL, O_NONBLOCK)` — unserved, ENOSYS, and musl ignores the result. The
guest ends up holding a BLOCKING socket it believes non-blocking, the very
failure the refusal's comment says it prevents. A refusal is only as honest as
the libc that receives it; the claim was verified on the artifact, not on the
kernel's return value. Both enqueued (memory + AUX-ROADMAP), main told
(V-5 is theirs).

Also to verify, not yet verified: holotype R5-F9 (longjmp out of a handler
wedges `in_handler`) was registered against pouch programs, but busybox ash's
`raise_interrupt` longjmps out of the SIGINT handler when interrupts are
enabled, and the phenotype population is every musl-static shell. One VM
experiment settles it; if real it is a P1 for interactive shells and needs an
abandoned-frame rule (design).

### The count that was a file count

The push bar over `277b02cc` measured LS-CI at 33 PASS + 2 SKIP; the record —
three JOURNAL stanzas, four status rows, this session's own resume note — said
34 + 2. Every bar today measured 33 + 2 over the same 35 scenarios, and the two
full runs before them said "32/34; 2 SKIPPED" in the harness's own words. The
34 was the c8ab2744 close message's "36 scenarios", an `*.exp` count that
included `lib.exp`, minus the two SKIPs: a derived figure that propagated as a
measured one six times before a run's tally was set beside it. Corrected
everywhere; the tally now comes from the harness's `==> LS-CI:` lines only.

### The bar over the tip (`01f076f2`: the handler-time mask)

SMP gate 40/40 (default + UBSan × smp4/smp8, N=10, 0 corruption / 0
external-kill / 0 other, two halves — the kernel byte-changed); LS-CI 33 PASS +
2 SKIP over 35 (GL not baked; six batches, TCG); suite 1413/1413; sabotages
SM1/SM2/SM3 each red on the named check. Pushed to both mirrors after the
fixup.

---

## 2026-08-16 — Warp-C C-1, the per-slot decision, and one third of the extinction tear

Resumed from a self-compaction at the 600k checkpoint. **The nudge fix worked
on its first live test** — the detached watcher fired behind `/compact` and the
far side woke itself, which is the loop the operator had been closing by hand at
every boundary.

### Warp-C C-1 — the composed present, modelled (`ee581fbd`, fixup `ae9a25df`)

GPU-DESIGN §4.5.6 is binding here: `tapestry_present.tla` is model-first, so the
model is extended *before* the impl. Added the GPU-composed present behind
`ALLOW_COMPOSE` — `Attach`/`Detach` (P1b's authority-conferral point),
`ComposeBlit`/`ComposeComplete`, `DrainedOfBlits` on `ServerRelease` + `Free`,
and two invariants repeating T-1's own LIFETIME/CONTENT split: `NoTornCompose`
and `NoStaleCompose`. Eleven cfgs, gated by the new `specs/check-tapestry.sh`.

**The control was set before the work, which is the only reason it meant
anything.** I recorded every cfg's distinct-state count *before* touching the
module, so "this extension is additive" became checkable: with `ALLOW_COMPOSE =
FALSE` the six pre-existing cfgs must reproduce 5413 exactly. They do — and the
check earned its keep, catching that tracking `filled` unconditionally cost the
direct path 5413 → 10413 states.

**Two measurement traps, both mine, both caught by controls rather than by
reasoning:**

- My first comparison harness reported all six cfgs as DIFFERING. The harness
  was broken (`set --` inside the loop clobbered the positionals, lagging every
  expectation by one row). But under the bad labels the raw numbers still said
  something real, and chasing *that* was the right move.
- The buggy cfgs genuinely did differ — and it turned out **the metric was of
  the instrument**. A buggy cfg halts at the first violation, so with parallel
  workers "states explored before tripping" is scheduler noise: measured
  129/141/155 across three *identical* runs. Buggy cfgs are now judged on exit
  status plus the *name* of the invariant reported. (Never on TLC's prose — it
  writes both "is violated" and "was violated" depending on property kind.)

**Then TLC refuted my model, and the tree refuted the premise under it.** I had
carried the in-flight blit as the *slot* it reads, reasoning that a client
filling a *different* slot during a composition is legitimate pipelining — and I
wrote that justification into the module header as though it were established.
It is false. `usr/tapestryd/src/gpu.rs:1515-1518`: tapestryd allocates one 2D
resource per surface, attaches the whole weave as backing, and transfers at a
per-present *offset* that selects the slot. Guest-side slots buy **no** host-side
concurrency. The guard also had the shape of a known trap — `intransfer = 0` is
a gauge reading zero, equally true of "the fill landed" and "no fill was ever
issued" — now closed by an explicit `filled`.

The exclusion is symmetric, so it gets a sabotage *per direction*
(`buggy_blit_during_fill`, `buggy_fill_during_blit`) rather than one flag opening
both gates, which would only ever demonstrate whichever end TLC reached first.

Non-vacuity was measured, not assumed: coverage shows the composed actions fire
`0:0` with the switch off and `ComposeBlit` 2264 / `ComposeComplete` 7328 with it
on, so the green sits over a constructed state.

**Verification:** 32 spec modules green + the 11-cfg tapestry gate. `corvus` and
`handles` deliberately not re-run — 87 minutes, and nothing `EXTENDS`
`tapestry_present`, so they cannot be reached by this change. Zero build inputs
changed (proved by `git diff --name-only`), so the full bar's other legs carry
from `ca50a164` by construction rather than by assertion.

### The design fork it forced — and the operator's vote (`14f8c1ed`)

C-1 surfaced an obligation **the prose did not have**: the D1 recycle gate does
not survive the composed path unchanged. In the direct path a present's terminal
CQE genuinely means "the host has finished reading" — until the compositor
becomes a second, async reader of that one host resource, at which point the CQE
stops meaning the resource is free and nothing in the old rule notices.

Researched before posing it (Wayland `wl_buffer.release` + `drm_syncobj`, Android
BufferQueue acquire/release fences, Fuchsia buffer collections), which showed the
SOTA answer is *two* mechanisms, not one: buffer-release semantics for software
clients, explicit fences for GPU ones. Posed the fork with that attached.

**Operator chose one host resource per slot (3×).** Landed as a scripture commit
with no code, per the design-conversation pattern: GPU-DESIGN §4.5.8, with the
two rejected alternatives and their reasons, and the cost stated rather than
buried (3× host VRAM; ~100 MB at 4K, against a 64-MiB weave cap that already
cannot hold a triple-buffered 4K weave). The landed model does not change with
the vote — `NoStaleCompose` is whole-generation, correct today and merely
conservative once slots become distinct host objects.

### The extinction tear — one third of it (`44a8d53f`)

A surfaced soundness defect outranks the perf arc, so I stopped C-2 and took
this. The `EXTINCTION:` ABI line is emitted as four separate unlocked
`uart_puts` calls; every consumer anchors its match (`^EXTINCTION:` in
`tools/test-fault.sh`, and bare-token matchers elsewhere). A torn banner is
therefore not cosmetic — it is **a real extinction the harness cannot see**,
fail-open on the one channel the whole test discipline trusts.

**The vault already carried an adjacent seam, and I nearly conflated them.**
There are **three** tearing sources with confusingly close names:

1. extinction vs extinction — the re-entrancy guard is per-CPU *by design*, so
   two dying CPUs both print. **Fixed** (`extinction_claim_console`).
2. extinction vs a peer's *normal* console write — the vault's
   `seam-extinction-line-unserialized`. **Open.**
3. `IPI_HALT` — would subsume both. **Open**, a commented-out reservation.

The fix is one `__atomic_exchange_n`: a raw atomic rather than a kernel spinlock
(this runs on a dying machine, often inside a fault handler, and a primitive
carrying lock-order assertions could itself fault), try-once rather than spin
(the winner never releases, since every path ends in `_torpor`), and losers park
emitting nothing — because the failure modes are asymmetric: a torn line can be
read as a clean boot, a missing one leaves the guest visibly hung. Take the loud
failure.

**The fix introduces its own fail-open, and that is what most of the design
guards.** Nothing releases the console, so anything claiming it spuriously
silences every later extinction in the boot — the same defect from the other
side. Hence the deliberate interface split: the claim core is exported to be run
on a *caller-supplied* word, and nothing exports a way to claim the live one. A
test that took the real console would disable extinction reporting for every
test after it, silently.

**Both new tests were sabotage-verified** (1367/1367 → 1365/1367, each failing
on its own distinct assertion message). And the first one is documented for what
it does *not* cover: it is sequential and the property is a race, so a non-atomic
`if (*w) return 0; *w = 1; return 1;` passes it identically. Covering the real
regression needs a multi-CPU fault-injection arm with a **forced** interleaving —
without forcing it the pre-fix build garbles only sometimes, and a discriminator
that fails only sometimes is not a regression test. Tracked, not skipped quietly.

Also corrected a phantom that had propagated into two files: both
`kernel/extinction.c` and the header told readers to co-update
`tools/agent-protocol.md`, which was planned in Phase 1 and never written, and
`tools/run-vm.sh`, which matches neither literal because it only launches QEMU
and never reads boot output. Both now point at the vault's `abi-boot-banner`
mirror set instead of a transcribed list.

**Verification (the full bar, since this is a kernel change):** build clean;
suite 1367/1367 (was 1365; +2); SMP gate 40/40 with 0 corruption across
default-smp4/smp8 + ubsan-smp4/smp8; LS-CI 35/35 PASS; v8.0 floor OK.

**A killed gate is not a green gate.** The first LS-CI run was stopped by the
harness (`Terminated: 15` on its scenario subprocesses) after I ended a turn
while it ran; the SMP gate had survived the identical foreground → background
migration earlier in the same run, so what differed was ending the turn. Re-run
as a tracked background task, staying in-turn.

**And then I got the reasoning for that right conclusion wrong, twice, the same
way.** I first wrote that the killed run "recorded zero verdicts", inferring it
from a stdout log containing only `==> start:` lines. Then, waiting on the
re-run, I read the same channel and concluded it had produced no results after
eight minutes. Both readings were of the wrong channel:
`tools/test-interactive.sh` says so in its own comment — *"The verdict is a
FILE, not a counter"* — and writes results to per-slot `timings.tsv`, never to
stdout. The re-run was healthy the whole time (`go8d PASS` already on disk).

So: **a pattern that matches the wrong thing returns a confident wrong answer,
never an error** — a lesson already pinned in memory, re-learned twice in one
hour on one command. What makes it worth writing down again is that the wrong
instrument produced a *plausible* story both times (a killed gate really had
been killed; a slow gate really can be slow), which is precisely why it was not
self-correcting. The fix is to find where a tool actually writes its verdict
before reading any verdict from it.

### Before C-2 wrote a line: the composed path cannot run on the dev loop

Checked the precondition rather than assuming it, and it changed the arc. The
boot log of the very run I had just gated says
`tapestryd: gpu up -- 1280x800, pci intid=35, virgl=0 capsets=0`, and
`tools/run-vm.sh` defaults to `virtio-gpu-pci` — a device with no GL. So
`CTX_CREATE` / `RESOURCE_CREATE_3D` / `SUBMIT_3D` are unavailable on the primary
dev loop, and with them every mechanism §4.5 describes.

Three consequences, recorded as GPU-DESIGN §4.5.9. C-2/C-3 must be verified on
**thyla-pi**, not here. The composed path must be capability-gated on the
negotiated feature bit — a tapestryd that assumed GL would take the console dark
on the default device. And the third corrects the roadmap: **"C-4 retire the
readback path" cannot mean delete it.** That is forced twice over — by the plain
`virtio-gpu` that is the default here, and more fundamentally by bare metal,
where there is no virtio-gpu at all and virgl is a *virtualization* transport
with nothing to negotiate. The CPU path is the universal one; GPU composition is
the accelerated path where a GPU seam exists.

The cost is stated rather than left to be discovered: tapestryd carries **two
composition paths permanently**, and they must stay behaviourally identical from
the outside or the gate that proves one is silent about the other.

### The C-2 verification host, proven rather than assumed

Having established the dev loop *cannot* run the composed path, the next
question was whether anything can. Synced HEAD to thyla-pi (all 80 pool chunks
hash-verified, artifacts paired) and booted `virtio-gpu-gl-pci` under KVM on
real V3D:

```
tapestryd: gpu virgl -- num_scanouts=1 num_capsets=2
tapestryd: gpu capset[1] id=2 max_version=2 max_size=1384
tapestryd: gpu up -- 1280x800, pci intid=35, virgl=1 capsets=2
CAPSET GATE: VERIFIED
```

So C-2 has a working verification host, and the two figures — `virgl=0` here,
`virgl=1` there — are the whole argument for §4.5.9 in one line each. Worth
doing before the implementation rather than after: had C-2 been written first,
its first symptom on the dev loop would have been a dark console, which is a
long way from its cause.

### C-2a — the capability gate and the compositor context

The first landable piece of C-2: a reserved compositor virgl context
(`COMPOSITOR_CTX = 0x100`, far above the client `slot + 1` range so a client's
stream can never author against the screen), minted only where `virgl`
negotiated, and a startup line reporting which composition path the host can
actually take.

**The first cut reported nothing, and the boot passed anyway.** I had hung the
posture report off `ensure_screen`, beside the other display resources — but
`ensure_screen` runs only under `Scanout::Composed`, a state a normal boot never
enters, so the line sat behind an unconstructed state and printed on neither
host. The suite went 1367/1367 with the feature effectively absent. Which
composition path is *available* is a property of the HOST, fixed at feature
negotiation, so it now reports where the host is brought up.

**Verified on both arms, differing in exactly one variable** — a negative
assertion alone would have been satisfied by a broken fixture:

| Host | Negotiation | Posture |
|---|---|---|
| dev loop, `virtio-gpu-pci` | `virgl=0` | `composed path = CPU (virgl=0)` |
| thyla-pi, `virtio-gpu-gl-pci` | `virgl=1 capsets=2` | `compositor ctx 256 up` → `composed path = GPU` |

Getting the positive arm took one correction of its own: the `capset` verb
filters its output at the capset markers, so the Pi run *looked* like it lacked
the line when it had simply not been shown it — `boot-probe.sh` keeps the full
log on the host, and the line was there. A truncated capture and a missing
feature are the same reading until you check which one you have.

### C-2b — the 3D screen, landed gated and HONESTLY UNPROVEN on its own arm

The screen becomes a host-side 3D resource attached to the compositor context
where GL exists, falling back to the 2D resource everywhere else. Guest backing
stays on both paths, because at C-2b the screen is still CPU-filled — only its
host-side representation changes. `screen_push` grows a 3D arm, and there the
sync transfer moves the whole surface rather than the damage rect: a deliberate
trade, since C-3 deletes the CPU fill outright and building a rect path for a
mechanism already scheduled for removal is waste.

**What is verified, and what is not — stated because the gap is the finding.**
The FALLBACK arm is verified: suite 1367/1367, and LS-CI 35/35 where the
`ls-gfx` scenarios assert exact pixels via screendump and therefore cannot pass
without a working composed screen. **The 3D arm has never executed.**
`alloc_screen` runs only under `Scanout::Composed`, and neither the dev-loop
boot nor the Pi's `capset` boot enters it, so `screen res N 3D (compositor ctx)`
printed on neither host. `prove` produced no new boot log to grep.

So this lands **gated off on every host I could exercise** — dead on the dev
loop by capability, unproven on the Pi by opportunity — and the commit says so
rather than calling a clean boot a verification. Booting green proves the gate
did not fire, which is exactly what an `if (false)` would also prove.

**Then I found why, and it is a tooling gap rather than a code problem.** The
Pi logs say `tapestryd: scanout direct 0 (1280x800)`: every existing Pi verb
drives a SINGLE display-sized GL client, and that takes the **Direct** path —
scanning out the client's own resource and bypassing the compositor screen
entirely. §4.5.1 spells out the condition: Direct demands one visible surface
AND one visible leaf AND an exactly display-sized surface. So composed scanout
needs two surfaces, or one smaller than the display, and **no verb in
`warp-host.sh` produces either.** `capset` and `smoke` both land in Direct;
`tri` and `prove` left no new boot log at all.

That is worth more than a failed check: it says the composed path — the entire
subject of the Warp-C arc — has no driver on the only host that can run its GPU
half. Building one (two surfaces, or a mode change that un-sizes a single one,
which is what `ls-gfx-mode` does locally) is the next task, and it gates C-2b,
C-3, and the arc's exit criterion alike.

### The driver — C-2b's 3D arm finally executes, and my own note was wrong

The task I left myself was "build a Pi driver that forces Composed scanout."
Before building anything I checked the claim under it, and **it was false**. The
section above says "no verb in `warp-host.sh` produces either" — but
`glq-virgl.exp`, which `quake` runs, opens GLQuake in a window and its very
first assertion is `-re {scanout composed \((\d+)x(\d+)\)}` with the label
"composed entry (two leaves)". `decomp` and `wedge` split the layout too. What
was actually true is narrower and duller: the verbs I had *read the boot logs
of* — `capset`, `smoke` — boot with no client at all, so aurora alone is
display-sized and lands in Direct. I generalised from the two logs I had to a
claim about all ten verbs, and wrote it into two documents.

Worth noting how cheap the catch was: one grep for `composed` across
`tools/warp/*.exp`, run because the note asserted a negative over a set I had
not enumerated. **The evidence that a thing is absent has to come from the whole
set, not from the members that happened to be in front of me** — and a note
written confidently at a compaction boundary is exactly where that error
survives, because the far side inherits it as established fact.

I still did not use `quake`. It drags in the pool's `tyr-glquake`, S3TC quirks
(#216), the #198 storm, and 900-second timeouts — a lot of machinery that can
fail for reasons having nothing to do with C-2b. `/bin/tapestry-battery` brings
up two surfaces, lives in the ramfs, and needs no GL of its own, so **the only
GL object in the experiment is the compositor's own screen**. That isolation is
the reason to pick it, not availability.

`tools/warp/composed-screen.exp` boots, takes the posture line between boot and
login (it prints at bringup, which is where a host property belongs — a lesson
this arc already paid for), runs the battery, and asserts the screen mint. **The
control is the device**, which is why the scenario takes one as a parameter
instead of hardcoding the GL model: two legs, one host, one variable, each
asserting the other's outcome is wrong.

```
virtio-gpu-gl-pci -> composed path = GPU -> screen res 67 3D (compositor ctx) (1280x800)
virtio-gpu-pci    -> composed path = CPU -> screen res 67 2D (1280x800)
```

**C-2b's 3D arm has now executed**, on real V3D silicon through virgl. The
second line is what makes the first mean something: a GL-only leg would pass
identically against a tapestryd that ignored the negotiated bit and always
minted 3D. Two legs that *disagree* are stronger evidence than two that both
pass — the control produced a different answer rather than merely staying quiet.
Both legs minting `res 67` is a small corroboration on the side: everything
upstream of the branch is identical, so the arm is the only thing that moved.

The gate keeps two claims separate rather than collapsing them — posture matches
the device, screen arm matches the posture — so a host that had silently lost
its GL could not satisfy the second by making both sides equally wrong. And
`tools/warp-host.sh composed` requires each leg's scenario-completion line as
well as its screen line, because a leg that died immediately after printing its
screen line would otherwise still show the gate everything it greps for. That
term is not hypothetical caution: the `reject` verb in this same file shipped
grepping `C0-REJECT` while its producer printed `C0-DETECT`, and exited 0 on the
exact failure it existed to catch.

### Then C-2d refuted itself before it wrote a line (§4.5.8a, OPEN)

With the driver landed I went to implement §4.5.8 — the per-slot host resources
the operator voted for — and read the present path first. The decision does not
survive it, for a reason nobody had in view at the vote.

Three facts, each one grep:

1. Every client rotates slots on every present: `cur_slot = (cur_slot + 1) %
   nslots`, `libtapestry/src/lib.rs:525`, unconditional, both scanout modes.
2. Nothing copies content from slot *N* to slot *N+1*. `pixels()` hands back
   the raw current slot; there is no carry-forward anywhere.
3. **The single per-generation host resource is therefore doing a job nobody
   wrote down: it is the accumulation buffer.** A damage-only present transfers
   only its rect, so the host resource keeps the rest of the previous frame and
   the stale guest slots never reach the host.

Give each slot its own host resource and that job has no owner. A damage-only
present would render a three-frames-stale background around each fresh rect —
in Direct immediately, and in Composed at C-3. And the client this lands on is
**aurora**: it repaints only rows `r0..r1` and presents that rect
(`aurora/src/main.rs:1027-1038`), and it is the default Direct client on every
boot. The very line I have been reading all session, `scanout direct 0
(1280x800)`, is that client.

What makes this worth recording is not the catch but where the load was.
§4.5.8's analysis compared 3× / 2× / 1× VRAM and serialization — a complete
comparison of the properties anyone had *named*. The single resource's real
function was invisible because nothing declared it; it was an emergent
consequence of "transfer only the damage rect", and it had been load-bearing
for the console for as long as the console has existed. **A design comparison
can be sound over every property you listed and still miss the one the code is
actually relying on.** Only reading the path surfaces those.

I recorded it as **§4.5.8a** with four options rather than picking one, because
the vote is the operator's and this changes the terms they voted on. The
recommendation is buffer age — `EGL_EXT_buffer_age` and Wayland's
`wl_surface.damage_buffer` exist for this exact problem, Android's BufferQueue
exposes the same, and it keeps the per-slot vote intact at no VRAM cost while
retiring the latent hazard instead of routing around it. C-2c and C-3 both wait
on the answer: every option changes what gets attached and what gets blitted.

### The vote, and C-2d-a (`0a0e0fbb`, `931bf15a`)

The operator picked buffer age. Implementing it immediately hit a constraint the
option sketch had assumed away: I had written "present CQE now carries: age",
and it cannot. A present is a 9P write over the Loom ring, so its CQE is
**kernel-owned** — `result` is the write's byte count, `flags` is `LOOM_CQE_*`,
and `struct loom_cqe` is `_Static_assert`-pinned at 16 bytes. Putting a
compositor payload there is a kernel ABI break for a compositor convenience.

The way out was to notice who already owns the information. `libtapestry` owns
the rotation — `cur_slot` advances only after a present's own CQE — so it knows
exactly when each slot was last presented and can derive the age itself. A
`TEV_AGE` event was rejected (async to the present, so it races the rotation) and
a control word in the weave was rejected (a client-visible layout change for
something the client can compute).

**The interesting part is what the derivation costs, because it is the same
trap again.** A derived age is correct only if the client hears about every
server-side invalidation — which is exactly the kind of undeclared dependency
that produced §4.5.8a two hours earlier. So it is written down as a named
invariant this time rather than left to be rediscovered: tapestryd must not skip
a transfer without the client subsequently getting a redraw request, and a
redraw invalidates **every** slot, so the client repaints full for `nslots`
presents, not one. Both arms are wired in `libtapestry`.

Then aurora handed back independent corroboration of §4.5.8a. `main.rs:988`
already routes any OSD pass through the full-frame branch, with the comment
*"a partial rect could transfer stale panel pixels from an older slot"*. The
symptom had been understood locally, for one widget, and worked around — the
general statement just never got made. That is what an emergent load-bearing
property looks like from the inside: not unknown, merely un-generalized.

I split the chunk, because the halves are not symmetric: per-slot resources
without age break every accumulator, but age without per-slot resources is inert
and harmless. So the client half went first — and **its honest gate is that
nothing changed.** `ls-gfx` PASS, `ls-gfx-panes` PASS (exact pane-centre
pixels), suite 1367/1367. Its actual effect is unobservable until C-2d-b removes
the accumulator, and the commit says so rather than dressing a green boot up as
verification.

**Then I got the prerequisite list wrong, in the commit message, within twenty
minutes of writing the lesson that prevents it.** I swept for clients that
present partial damage with `grep 'present(Some\|present_rects'` and reported
three. That greps **API shape**, not the property that matters — *damage
smaller than the full surface*. Checked properly, it is one:

- `tapestry-battery` needs **nothing**. Every present is `present(None)`, and
  its one `present_rects` tiles the whole surface with two rects after writing
  every pixel. Its own header says so: *"presents FULL-FRAME only."* I had
  called it "the one with teeth."
- `tapestry-demo` is the real one, and is the sharpest example in the tree: it
  paints the quadrant background **into slot 0 only**, at frame 0, then draws
  just the plasma box into *rotating* slots forever after. Slots 1 and 2 never
  receive the background at all — they hold alloc-time zeros. Under per-slot
  resources, two frames in three would show black around the plasma.

"A pattern that matches the wrong thing returns a confident wrong answer, never
an error" is pinned at the top of my own memory index. It still went into a
commit message, a scripture section and the handoff, because a grep that
*returns results* feels like a sweep that *finished*. Corrected in §4.5.8b and
the handoff; the commit body stands as written, with this as its correction.

### The stop hook guarded the wrong stop, and the guard was never needed (`b3632942`, `cd0b3390`, `b61ca929`)

The operator noticed the Stop hook fired once in the long run and then went
quiet at a second stop it should have caught, and asked aux and me to work out
why. It is the third instance this week of the same family, and the sharpest.

**The measurement.** Replaying the hook's own parser over the real 805 MB
transcript: the silent stop sat at **530k / 73 turns** — inside the window on
both axes. So "it was correctly silent above the checkpoint" is dead. Isolating
the logic with synthetic input showed it behaves exactly as written. The cause
was upstream, and the pattern repeats: every firing is followed by silence for
the rest of the continuation, re-arming only when the user speaks or a
compaction lands.

**What I got wrong, and it was not the code.** `stop_hook_active` means "this
hook already triggered a continuation" — per-continuation by definition. I
exited early on it, which made the hook a once-per-*run* nudge guarding the
first stop and nothing after, i.e. the stop most likely to be earned and none
of the ones that follow. I kept that early exit because I believed it was the
loop guard.

**It never was.** aux fetched the contract: the harness overrides a Stop hook
after **eight consecutive blocks** (`CLAUDE_CODE_STOP_HOOK_BLOCK_CAP`). The
belay already existed one level up. So I had built a guard against a loop
something else was already preventing, and paid for it with the exact behaviour
the hook exists to provide. That is a different failure from a bug: **the code
did what I meant; what I meant rested on a contract I had not read.** No amount
of testing my own intent would have found it — only reading someone else's.

**The instrument came before the fix, and earned it twice.** The hook had nine
silent exits, so "correctly silent", "suppressed", and "crashed" were one
observation and any diagnosis could only be a guess — the same shape that had
just cost the vault a stranded day. So a ledger row on every path landed first.
Then it caught two things I would not have:

- Its own blind spot: the `stop_hook_active` parser printed `"1"` on exception,
  so a malformed stdin logged as `silent-stop-hook-active`. **The instrument
  built to separate those two causes could not separate those two causes.** The
  malformed-stdin test leg printed the wrong row, which is the only reason I
  looked.
- On its first *real* output: three rows in 24 seconds with incoherent context
  jumps, because the ledger is shared by main/aux/vault and I had dropped the
  session field from aux's spec. An interleaved log with no writer is worse
  than no log — it invites a confident reconstruction of one impossible session
  out of three real ones.

**And the fix validated itself in production before I finished writing it up:**
the reworded stem ("fires once per stop") came back in a live firing that
re-armed mid-continuation after real work — something the old version could not
do — with the ledger row `588458ctx/44t/27b/flag1` showing exactly why.

### C-2d-b landed, and the sabotage that proved it unverified (`f86177b6`)

The server half went in as voted: each generation mints `WEAVE_SLOTS` host
resources instead of one, backed per-slot instead of whole-weave. The
consequences were all followed rather than found later — `res_stale` becomes
per-slot; Direct binds the presented slot's resource and therefore rebinds every
frame (a KMS page flip, carrying the #57 post-bind flush); transfer offsets lose
their slot base, which the compiler confirmed by reporting `slot_stride` newly
unused; retire and `release_gen` unref all three or leak two per surface in the
process that IS the console.

`Held::Direct(Rect)` was the one that needed design rather than editing, and it
is why I stopped the first attempt at it. A rect union is well-defined only
while every held present lands on one resource; presents rotate slots, so two
held presents sit on different resources and `release` must flush each against
its own. Now `[Rect; WEAVE_SLOTS]` — bounded by construction, since a client
cannot hold more presents than it has slots.

**Then the sabotage passed, and that is the result worth the whole chunk.** I
disabled aurora's age handling with per-slot resources live — `stale_slot =
false`, `back = 0`, exactly the pre-C-2d-a client against a non-accumulating
server — and **`ls-gfx` still reported PASS.**

So the two gates I had been treating as verification are not. `ls-gfx` asserts
the frame *looks like* a console and that dumps *differ* after a command;
neither notices a stale background around fresh rows. `ls-gfx-panes` drives the
battery, which presents full-frame only and never exercises the accumulator path
at all. Between them they cover everything about the compositor **except the
property C-2d changes.**

That is the same trap as C-2b at the start of this run — a green result that
proves the gate did not fire — except this time I was the one about to be
fooled by it, having written the C-2b version into scripture that morning. The
difference between the two is not insight, it is that I ran the sabotage. Had I
not, this would have landed as "green on both pixel gates", which is *true* and
means nothing.

C-2d is therefore **implemented, not verified**, and the commit says so. §4.5.8c
records what the missing gate has to do: paint a region, damage a *different*
region, rotate all slots, sample the first region. `ls-gfx-panes` already has
the sampling machinery, so it is a scenario to write, not an instrument. The
focused audit is owed too — `usr/tapestryd` is an I-40 trigger surface and this
is the live scanout path — and could not run here because agent spawning is off.

### The self-compaction slot had two keys that did not agree (`7061115a`)

aux found this by reading the ledger nobody reads, and it is the best kind of
find: the mechanism had been quietly half-broken since it was built, and the
evidence had been sitting in a file the whole time.

`~/.claude/thyla-selfcompact/log.tsv` has vault's `allow` at 2026-08-16
10:44:32Z with **no `consumed` and no `nudge`**, and its `.note.pending` still
in the slot dir a day later. Every `main` row is paired; only vault's is
orphaned. That session compacted itself and was never handed its own resume
note — it sat at a prompt for the rest of the day.

The cause is a key mismatch, and **the comment is the interesting part.**
`tools/thyla-selfcompact.sh` said, in as many words: *"Two independent
derivations of one key, no shared config to drift."* The producer keys on `git
rev-parse --show-toplevel`; the consumer on `basename(dirname(transcript))`,
which is where the session was **launched**. Those coincide for main and aux
and do not for vault, which is launched from the thylacine tree and works in
thylacine-vault. So the comment **named the hazard and then asserted it away**,
and that assertion is what kept it unexamined for the mechanism's whole life.
It is every "keep these in sync" note that has ever rotted, except this one had
the confidence of sounding like an argument.

The fix needed no new identity, because one was already there and unused: the
arming script has always stamped `pane=$TMUX_PANE` into the meta, and a hook is
a child of the same claude, so it reads the same value. Pane match first, path
key as fallback.

**But the half that mattered was the silence.** The old failure was not doing
the wrong thing — it did *nothing*, and left no evidence, so `allow` without
`consumed` was the only trace. There is now an `orphan-note` row whenever a
pending slot goes unmatched, plus a 30-minute staleness discard.

**Then the test caught a bug in the fix that was worse than the bug.** The
first age check used `time.mktime` on a UTC stamp — `mktime` reads a
`struct_time` as *local* — so a note stamped that same second measured as an
hour old and was **discarded**. In any non-UTC zone that breaks every
legitimate resume: the repair would have converted a vault-only silent miss
into a universal one. I saw it only because leg 1 of the test printed
`stale-discarded` on a note written a moment earlier. Four legs, with legs 3–4
as the controls that make leg 1 mean anything — same note, same path-key
mismatch, only the pane varies:

```
1 pane matches, fresh    -> INJECTED,     consumed
2 pane matches, 25h old  -> not injected, stale-discarded
3 CONTROL no TMUX_PANE   -> not injected, orphan-note
4 CONTROL wrong pane     -> not injected, orphan-note
```

aux also retracted something in the same message, which is worth recording
because the retraction is worth more than the claim was: the "fourth
unregistered session" cited in the yip lease rationale **was aux itself** —
`ps -o ppid` on its own tool shell resolved to the process it had been reading
as a stranger. A census needs a control, and the control was its own identity.
Same family as `ps` matching its own command line, from the other end.

### Found in passing: `docs/REFERENCE.md`'s snapshot block died in Phase 5

The doc-update step sent me to `docs/REFERENCE.md` to refresh its Snapshot
block, which `CLAUDE.md` calls non-negotiable per chunk. **The newest "Tip"
bullet in it is a Phase 5 chunk** (`P5-stratumd-stub-bringup` audit close), and
there are 101 bullets behind it. The file's last commit of any kind is
`418688cf`, 2026-08-01. It contains **zero** occurrences of "Warp", "Tapestry",
"Clade" or "PTY-" — three whole arcs and a subsystem that do not exist as far as
the as-built technical reference is concerned.

So a binding per-PR obligation has been quietly unmet across roughly two phases,
including by me, several times this week. It is the "*a status field whose flip
is nobody's step stays unflipped*" shape: every chunk's author is told to
refresh it, no chunk's work makes them, and nothing fails when they do not.

**I deliberately did NOT patch my own bullet onto the top.** A dead list with
one fresh entry reads as maintained, which is worse than one that visibly
stopped — the reader trusts it again. The real question is what that block is
*for* now that `docs/phaseN-status.md` carries per-chunk rows and this journal
carries the narrative; answering it is a scripture-shaped decision, not a doc
edit to slip into a tooling commit. Enqueued rather than fixed in passing, and
enqueued in memory because the tracker is down this session.

### The gate that sees C-2d, red under both sabotages — and the defect building it found (after the self-compaction at `a733402e`)

Resumed from my own note with one instruction: build the §4.5.8c gate on aurora
in Direct, and validate it by re-running the sabotage that had passed `ls-gfx`
and requiring red. That is what happened, with two things the note did not
anticipate.

**The gate** (`tools/interactive/ls-gfx-age.exp` + `gfx_region.py`). Fill three
times with `yes … | head -n 200` so every slot carries glyphs; a POSITIVE
control — the same region assert, four keystroke-rotated dumps, each must show
text (a negative with no positive twin is satisfied by a broken fixture); then
`clear`, which blanks every cell in one all-rows present into ONE slot; then
eight rounds of keystrokes + dump, region exactly Bonfire, every pixel read.
The region is in cells (rows 6..rows-3, cols 2..cols/2) off aurora's own
`console up` line, so a font change moves it rather than breaks it.

**What the note left to the author, and how it was decided.** The detector is
slot-phased: the screen shows the slot presented LAST, so one dump samples one
slot. I had written "probabilistic — require N consecutive dumps". Working it
through, the honest model is *driven*, not sampled: each keystroke is a
row-0-only redraw, i.e. one present into the next slot, so the rounds advance
the phase deterministically plus whatever blink presents fall in the round.
That reframing exposed the real trap: **a broken client can have ONE stale
slot, not two** — an off-by-one in the union (`back = age-2`) leaves exactly one
— and the 1,2,3,1,2,3,… key pattern I first sketched (meant to break any
phase-lock with the blink) visits residues 1,0,0,1,0,0,1,0 under `b=0`: it never
reaches residue 2 and would pass an off-by-one every time. A plain one key per
round does reach it (1,2,0,1,2,0…) but is the pattern a 60 Hz blink can
phase-lock. So the negative leg types 1,1,2,1,1,2,1,1 keys, which visits all
three residues for *any* constant blink count per round (checked for b=0,1,2 in
the header); the
independence bounds — 3^-8 for the no-age class, (2/3)^8 = 3.9% for the
one-stale-slot class — are the fallback if the blink rate varies mid-leg, and
the header says which claim is load-bearing.

**Measured** (HVF, 128×36 cells, region 368 280 px). Fixed build: positive
63 882/368 280 non-bg on 4/4 dumps (identical counts — every slot holds the
same fill, as a correct client guarantees), negative **0/368 280 on 8/8**,
43 s. **S1** — the §4.5.8c sabotage, `stale_slot = false` + `back = 0`: **red
3/3 attempts**, at rounds 2, 1, 2 (63 882 stale px, i.e. the pre-clear fill
verbatim). **S2** — `back` off by one: **red 3/3**, at rounds 2, 5, 2. The
five-round attempt is the 1,1,2 pattern paying for itself: four dumps landed on
the two good slots before the fifth reached the one stale one. Restore green.
Both sabotages applied and reverted with `Edit`, and `grep SABOTAGE` empty
before the restore build.

**The defect the gate found — in C-2d-a, not C-2d-b.** Reading aurora's damage
branch to predict the sabotage outcomes, I traced what `931bf15a` records into
`dmg_hist`: **the WIDENED range** ("this is what actually reached the slot, and
the next union reads it"). That reasoning conflates *repaint* with *damage*.
The union answers "what changed since slot X was last presented"; what changed
between two presents is the dirty span, and the widening only says how much of
it THIS slot had to catch up on. Recording the widened range makes any
full-rows entry — every scroll — re-enter every later union, so every present
after it repaints all rows, forever. Aurora has been repainting the whole grid
on every cursor blink since C-2d-a landed: correct pixels, dead damage path.
Fixed to record the dirty span (`dirty0, dirty1` captured before the widening);
a full entry now falls out of the window after `nslots` presents. Two things
follow that are worth having in writing: S2 is a sabotage only against the
*fixed* recording — under the widened one an off-by-one is masked, since any
`back ≥ 1` propagates the full-rows entry (the old code had slack precisely
because it had no damage path); and the tight recording is guarded by the gate
that was built in the same chunk, which is the right order.

**Wrong turns, caught:** the first run failed on my own Tcl (`gfx_dump` takes
two args and I passed one) — three attempts, ~30 s each, all on the harness
side, before a pixel was read. And the resume note's "the sampling machinery is
in `ls-gfx-panes`" was true and unhelpful: `ppm-sample.py` reads one pixel; the
gate needs a region census with a positive control, which is a 40-line tool.

**Owed, unchanged:** the focused audit on `usr/tapestryd` (I-40; agent spawning
still off). The vault-owned prose (`sub-aurora`, `sub-libtapestry`,
`sub-tapestryd`) for C-2d and the recording fix goes over yip; the local
reference carries the gate.

### The device's OK was never the renderer's verdict — C-2b's "3D" word re-earned

Found while designing C-2c's gate, and by the one move that keeps saving this
arc: reading the source of the thing making the claim before repeating the
claim. My C-2c draft was about to say, for the third time in a week, that a
`CTX_ATTACH_RESOURCE` answered OK "attests the host accepted it". Before
writing that I fetched QEMU v10.0.0 `hw/display/virtio-gpu-virgl.c` (thyla-pi
runs 10.0.11) and read the handlers. **They ignore the `virgl_renderer_*` return
value** — for `CTX_CREATE`, `RESOURCE_CREATE_2D/3D`, `CTX_ATTACH/DETACH`,
`TRANSFER_TO_HOST_3D`, `SUBMIT_3D`, `CTX_DESTROY`; `ATTACH_BACKING` checks it
only to clean up the iov. `RESP_OK_NODATA` means "QEMU parsed it": nonzero,
non-duplicate id, valid iov. Only `SET_SCANOUT` (`resource_get_info_ext`) and
`RESOURCE_UNREF` (QEMU-side existence) consult anything.

**So three of my own documents were false in the same sentence.** C-2b's gate
header, `149-warp.md` and (by reference) the status row said the screen's "3D"
word was "the conjunction of four response-checked round trips the host
answered OK — a claim about the host accepting the object". Those four are
exactly the ignored ones. And it was not only prose: `alloc_screen`'s "a 3D
failure is NOT fatal — it falls back to 2D" was dead for a renderer-side
refusal — `is3d` reduced to `comp_ctx`, "3D" printed, and the failure landed
later, silently, as `INVALID_RESOURCE_ID` at the composed `SET_SCANOUT`, whose
result the code dropped after printing "scanout composed" *before* the bind.
The display would have kept the previous scanout, and the C-2b gate would have
said VERIFIED. #240 had measured this exact shape for `SUBMIT_3D` four days
earlier; the finding was filed against one command and never checked against
its family — the same lesson as the C-2d gate pattern that morning, one level
up.

**The repair is #240's own technique**: make the producer prove it with pixels.
`alloc_screen` writes 16 sentinel pixels into the fresh screen's backing,
`TRANSFER_TO_HOST_3D`s them through the compositor context, clobbers the
backing, `TRANSFER_FROM_HOST_3D`s back, compares, restores the zeros. Only a
resource the renderer holds, has attached to `COMPOSITOR_CTX`, and moves pixels
through can pass; a refused create or attach makes both transfers renderer-side
no-ops and the clobber survives. A refusal now falls back to 2D for real, the
screen line says why, the composed line prints after the bind with its verdict,
and `composed-screen.exp` grew a fifth term (the bound resource IS the minted
screen; the verb requires it on both legs).

**Measured on thyla-pi** (KVM, real V3D, boot-ms ~212 000), one variable —
the format the renderer will accept — two runs. *Sabotage*, `VIRGL_FORMAT`
`0x7FFF` in the 3D create: GL leg `screen res 71 2D (1280x800) -- 3D refused:
renderer round trip`, then `scanout composed (1280x800) res 71 bound` — so
`CREATE_3D`, `CTX_ATTACH_RESOURCE` and `ATTACH_BACKING` all came back OK from
the device under a format the renderer cannot accept (the reason would have
named the step otherwise), the renderer refused, the fallback was real and the
display got a working screen; the scenario went RED on the arm and the verb
reported three GATE FAIL terms; the non-GL leg was unaffected. *Clean*: GL leg
**`screen res 71 3D (compositor ctx) (1280x800)`** + `res 71 bound`, non-GL
`2D` + `res 71 bound`, all five terms, rc 0. The half that says the OLD code
would have printed 3D under the sabotage is inferred from the measured OKs and
the old boolean (`comp_ctx && create.is_ok() && attach.is_ok()`), not itself
measured — I chose not to spend a third Pi cycle on a one-line inference and
say so here.

**What this changes downstream**: `CTX_ATTACH_RESOURCE`'s response witnesses
nothing, so C-2c cannot be verified by its attach at all — its gate is P1b's two
arms in-guest (attach + one blit + readback; no-attach control red), which means
C-2c lands WITH the first blit witness. The C-2c design draft
(compositor-side import on host, bounded by hosting, no client verb — every
compositor in the prior art does it that way) is written and waits on that
correction; it goes into GPU-DESIGN as §4.5.10 with the next chunk.

### C-2c — the compositor imports what it composes, and the import is witnessed (after the self-compaction at `8c20b1f8`)

Resumed from the second self-compaction of the run (`8c20b1f8`, all pushed;
the note said "next is C-2c WITH its blit witness", and that is what this is).

**What C-2c is, in one line:** at `alloc_weave` tapestryd now
`CTX_ATTACH_RESOURCE`s every slot resource of a generation into
`COMPOSITOR_CTX`, and at `present-to` it imports the GL adoption's consented
BO — the client handing its buffer to the compositor is the whole grant, no
client verb (§4.5.10) — and every import is revoked BEFORE the resource's
unref on every death path (`release_gen`, `retire`, `wbo_retire`, `present-to
off`/replace, the consented surface's retire).

**The witness, and why it is not the one the design paragraph drew.**
§4.5.4c had already established that `CTX_ATTACH_RESOURCE`'s OK attests
nothing, so C-2c had to land with a pixel witness. The design said "blit a box
of the slot into the screen and read the screen back". Built instead: the
compositor context's own #240 mark/sentinel pair (`warp_probe_build
(COMPOSITOR_CTX)`, minted with the ctx), and per slot: seed tokens into the
slot's host copy through the present path's own `TRANSFER_TO_HOST_2D` (the
guest pixels are borrowed while NO client mapping of the weave exists yet —
`alloc_weave` runs before the Tweft that maps it is answered — then zeroed),
poison the sentinel, `RESOURCE_COPY_REGION` slot → sentinel inside
`COMPOSITOR_CTX`, read the sentinel back. A 1×1 compositor-owned target
instead of the screen: same claim (pixels through the compositor context or
nothing), the direction C-3 will use (the slot as SOURCE), no screen pixels
to save/restore, no question about the screen's coordinates — and it made
import time the natural site, since the reason the design gave for composed
entry ("the screen may not exist yet at import") no longer applied.

**A health copy runs before every witness, and the reason is the latch.** A
copy naming a resource the renderer does not hold in the context reports
`ILLEGAL_RESOURCE`, and vrend then refuses every later command buffer on that
context (§4.5.4a). So a genuinely refused import kills GPU composition for the
process lifetime, silently — which is (a) why `comp_attached` fails closed and
C-3 must never blit from a resource without it, (b) why the mark → sentinel
health copy runs first, so a REFUSED is attributable to THAT import and later
generations read `SKIPPED (compositor ctx unhealthy)` as a measured state, and
(c) why the witness runs at a rare structural moment (~16 controlq round trips
per generation) and never per frame.

**What the Pi taught before it answered the question it was asked** (six
`composed` cycles; the sixth is the one that counts). (1) The clean build read
`REFUSED (slot 0 copy did not land)` on its first run — the witness's own
seed was at guest row 0 and the compositor's copy of a y=0 box on a `Y_0_TOP`
source lands from texel row **h−1** (vrend's FBO copy path measures such boxes
from the bottom; the texel-exact copy-image path was not the one taken). The
instrument needed a control of its own: it now seeds rows 0 and h−1 with
distinct tokens and REPORTS which came back — `witnessed 3/3 (copy read texel
row 799)` — a measured convention C-3's blit boxes inherit rather than a
guess. (2) The posture anchor came out `ttaappeessttrryydd`: the kernel's
`proc: orphan` burst at warden's exit and tapestryd's SYS_PUTS interleaved
BYTE for BYTE — the console TX ring is byte-atomic, not line-atomic, and my
probe mint had moved the anchor into the burst. Not fixed here (LS-8 surface,
aux mid-change in `cons.c`, and it costs the kernel-byte-unchanged property);
the anchor is printed first again, the armed state moved to its own line, the
defect enqueued (`bug_console_tx_ring_byte_atomic.md`) and handed to aux on
yip. (3) The gate script then cost three cycles of its own: a say-line format
change under an anchored regexp; three `-re` arms — pattern ORDER beats buffer
position, so the arm listed first ate a later comp-attach line and discarded
the screen/composed pair before it; and one ordered pattern that matched
PARTIAL lines (serial arrives in chunks) — three GL-leg hangs ending on the
battery's own later FAIL, while an offline replay of the same log passed. The
anchored single-pattern form went green: `WARP-COMPOSED ATTACH: witnessed 2
surfaces (copy read texel rows: 799 797)`, both legs PASS, verb VERIFIED on
seven terms.

**The sabotage measured more than it was asked to.** Skipping the slot
attaches: the first import `REFUSED (slot 0 copy did not land)`, then every
later import `SKIPPED (compositor ctx unhealthy)` — the latch is now a
measurement, not a recollection of vrend — **and the screen's own 3D mint fell
back**: `screen res 73 2D (1280x800) -- 3D refused: renderer round trip`. The
§4.5.4c fallback, built two chunks ago against a hypothetical, ran for real:
the display kept working on the CPU/2D arm while GPU composition was loudly
gone. Verb RED, 2D leg unaffected.

**The quake gate found a C-2d-b leftover.** `glq-virgl.exp`'s eviction leg
waits for `scanout direct N (WxH)`; C-2d-b (`f86177b6`) changed that say line
to `scanout direct N slot S (WxH)` and the check made then enumerated the
`scanout composed` consumers and missed the `scanout direct` ones — five
patterns across `glq-virgl` / `glq-decomp` / `glq-wedge-probe`, all silently
broken since, all failing CLOSED (a false RED on the console-restore leg after
^C, the first time any of them ran after that commit). Fixed to take the
`slot S` token as optional. #230's lesson again: a mirror set is enumerated by
what its members MEAN, not by the substring one happened to grep.

**Gates.** `composed-screen.exp` grew a third claim (GL leg: ≥ 2 per-surface
`witnessed n/n` lines — the battery's two surfaces — none refused; 2D leg: the
import declared skipped, no per-surface line — the control), the `composed`
verb terms six/seven, and `glq-virgl.exp` gates the ctl census (`comp-attach
witnessed W refused R`: R must be 0) after the game dies — the BO import
through the SDL shim's real `present-to`.

**Coordination.** Aux held the mac all afternoon (its pty-4 root-cause fix:
builds + suite + LS-CI + the SMP halves); the C-2c cargo check/build ran at
`-j2` under an explicit yes on yip 0024, everything else waited for the
release; the Pi lease was mine (`hold pi`) for the whole verification.

### C-3 — the compositor composes by blit, and the pixel oracle caught the model on its first probe (`7296bf07`; after the self-compaction at `115cbc5a`)

Resumed from the third self-compaction (`115cbc5a`, everything pushed; the
note said "next is C-3, a large chunk", and it was).

**What C-3 is** (`usr/tapestryd/src/server.rs` + `gpu.rs`; GPU-DESIGN §4.5.11).
Where the host has GL, a Composed present of a software surface no longer
fills the screen on the CPU: it transfers its damage into the presented
slot's own resource (the direct arm's transfer, per slot since C-2d-b) and
composes by `VIRGL_CCMD_BLIT` slot → screen inside `COMPOSITOR_CTX`, then
flushes; a witnessed GL adoption composes by one blit BO → screen — no
readback, no CPU pass, no upload. The blits ride the compositor context's
SYNC slot (`submit_blits`, chunked at the widened `REQ_REGION_LEN`), so a
present is still one dispatch and `ComposeBlit`/`ComposeComplete` close
inside it: the in-flight blit set is empty at every retire point by
construction, exactly the shape stage-0 synchrony gave `intransfer = 0`, and
detach-before-unref (C-2c) stays the whole ordering. The pipelined form
(fenced blits, flush riding fence completion, a real drain) is the C-4+
evolution the spec is cut for; §4.5.11 records why the sync form was chosen
(µs per present against the ~8 MB round trip it deletes; the GL-completion
residual is P2, measured 0/500) and what a FENCE-flagged sync command would
buy if it is ever needed. Chrome stays CPU-painted and uploaded on damage on
both paths — a focus-only repaint now uploads only the frame/strip rects,
because on the GPU path the screen buffer holds chrome and not client pixels
(the whole-buffer push that used to serve focus changes would have blanked
every pane). `Held::Composed` splits into `cpu` (upload + flush at release)
and `gpu` (flush only) regions. The compositor runs its own #240 health copy
once per tick after a GPU-composed present and latches GPU composition OFF,
sticky, with a structural repaint deferred to the next tick (never inline
in the dispatch: the CONFIGURE fan can wedge-retire the surface mid-present).
`res_stale[slot] = !covers_full` on the GPU arm, decided per §4.5.8c rather
than ported. The CPU path is untouched wherever the GPU one does not apply.

**The screen is `Y_0_TOP` now, and C-2b's flags-0 screen was displaying
inverted.** Every 2D resource QEMU creates carries `Y_0_TOP` and is flipped at
scanout (Linux fbcon upright under egl-headless); a flags-0 resource is shown
unflipped (Weston upright). C-2b minted the 3D screen flags 0 and filled it
top-down from the CPU — inverted on a GL display, from the day it landed, and
nothing could see it (#195, and a gate that read a say line). Named in
§4.5.11 as the defect it was; the display half stays an anchor, since the
oracle reads the resource, not the display.

**Conventions are measured, and the measurement was wrong once — the oracle
caught it on the first probe.** A blit box is a request in the renderer's
coordinates; C-2c had measured that a copy box on a `Y_0_TOP` source counts
from the bottom here. So C-3 measures at bring-up, on throwaway contexts
(`CONV_PROBE_CTX_BASE`+, one fresh per attempt — a refused request latches
its context, and the probe tries requests whose acceptance is the question),
with seeded 1×4/1×16 probes of each kind. The first probe measured ONE
request — unscaled, 1×2 → 1×2 — derived flips (both sides), confirmed them
(unscaled again), and applied them to every blit. The battery's panes are
both SCALED (A 1280×800 → 638×398, B 640×400 → 636×398 — the 1-px frame inset
makes every "matching" pane the scaled class), and virglrenderer routes an
unscaled same-format nearest RGBA blit to the texel-exact copy-image path
and a scaled one to `glBlitFramebuffer`, which hold OPPOSITE conventions for
a `Y_0_TOP` pair whose transfers invert rows: copy-image wants both boxes
flipped, blit applies the flip itself and wants the raw boxes. Run 1: the
panes composed vertically swapped; the first `probe-screen` read `(960,200) =
#0000ff` for A's red — `LS-CI FAIL` — while the probe's own confirmation had
read CONFIRMED. The measurement of the renderer was right about the class it
measured; the measurement of the SYSTEM (the battery at real geometry + the
oracle) is what caught it. Redesigned per (source shape: `Y_0_TOP` slot /
flags-0 BO) × (size class: unscaled / scaled ×2), request variants tried in
order (plain, negative source height, negative destination height) until the
landing has the ORDER the shape needs (slot straight; BO mirrored — its GL
row H−1 is its visual top), flips read off WHERE it landed and WHICH rows it
carried, each CONFIRMED at an asymmetric offset, each fail-closed per class,
every landing SAID as a 16-character row map. Run 2 on V3D: `slot U plain
sf1 df1, S plain sf0 df0; bo U plain sf0 df1, S src-neg sf0 df0` — the plain
scaled BO request landed straight (`.0011…`), the negative-source-height
idiom mirrors it — all four CONFIRMED, then 9/9 pixel probes exact. The
compose path picks the class by the op's own box sizes (the renderer's
predicate) and issues through the same builder the probe used. Lesson filed
(`memory/bug_c3_convention_per_request_class.md`): a convention measured on
one request class is not a convention; two recollections of vrend/QEMU's flip
code were wrong in opposite directions this arc, and the measurements were
right both times.

**The oracle.** `probe-screen X Y` (tapestry global ctl; test-mode, ungated
like the determinism verbs, rate-limited) makes the compositor read texel
(X,Y) of the SCREEN back and say it — `via readback` (TRANSFER_FROM_HOST_3D
through the compositor ctx, the only place a GPU-composed pixel exists) on
the 3D screen, `via backing` on the 2D one, with the scanout mode and the
`composed gpu G cpu C` census. The battery probes its own sample points at
every pixel stage and grew `multirect-v` (B split TOP/BOTTOM green over yellow
— the vertical asymmetry a mirrored or displaced box cannot fake, which a
solid fill and a left/right split never show) and `tab-cycled ready` (A
hidden by the tab, revealed by the cycle, presented red, probed — the C-2d
redraw contract on the composed path). `composed-screen.exp` claim 4 + verb
terms eight/nine: 9/9 exact `via readback` with `gpu ≥ 1` on the GL leg (a
build whose GPU path silently routed everything to the CPU one composes
CORRECT pixels; only the census tells that apart), 9/9 exact `via backing`
with `gpu 0` on the non-GL leg — the same coordinates and colours on both,
the first pixel witness that the two composition paths agree from outside.

**Measured (thyla-pi, KVM, V3D).** Run 3, the final binary, both legs:
`WARP-COMPOSED PIXELS: 9 probes via readback ok (composed gpu 34 cpu 0)` /
`… via backing ok (composed gpu 0 cpu 27)`, `C-2b/C-2c/C-3 COMPOSED-SCREEN
GATE: VERIFIED` (nine terms). Sabotages, GL leg: **S1** — the blit never
submitted, every other GPU-path step intact — `screen-probe (960,200) =
#101014` (the pane background) with `composed gpu 10`, RED on the first
probe; **S2** — every present routed to the CPU path — all nine pixels exact
`via readback` (so the CPU upload into the 3D screen composes right as well)
but `composed gpu 0 cpu 31`, RED on the census term, which is exactly the
sabotage the census exists for. Run 1 stands as the third: the natural
convention error, RED at the first pixel. Then `quake` and `decomp gl` on the
final binary — the standing GL gates and the only driver of the BO composed
arm: `quake` `WARP-4 GATE: VERIFIED` (969 frames, 44.9 fps; `comp-attach ctx 1
bo 1 res 82 -> surface 1: witnessed`, and — the BO arm's first live execution
— `surface 1 composed via GPU blit (BO res 82 -> screen res 76)` in the
Composed window before the direct switch); `decomp gl`: composed **36.9 fps
(969 frames, 26.3 s)** against the **25.4 fps (38.1 s)** measured 2026-08-10
on the same host and demo — the direct arm reads the identical 44.4 fps both
days, so the arms are comparable — the composed present's cost fell from
16.8 ms to 4.6 ms per frame (39.3 → 27.1 ms/frame), the windowed-GL overhead
from 1.75× to 1.20×. What is left in the 4.6 ms is the C-4 question (the blit
+ flush round trips, the per-tick health copy, the display readback under
egl-headless), to be decomposed rather than guessed.

### C-4 — the residual decomposed, and it was neither of the two things named first (after the self-compaction at `d591c35e`)

Resumed from the third self-compaction of the day; the note said "next is
C-4: decompose the remaining 4.6 ms, retire the readback where GL exists, the
fenced form if the sync round trips are what is left." Read §4.5.11 + §4.5.9 +
149-warp's #196/#215 decomposition first, as the note demanded, then built
the instrument before touching the mechanism.

**The instrument** (`Cost` in `server.rs`): every synchronous device step of
the present path timed where it is issued, every present dispatch timed
whole and attributed to its arm, cumulative `cost <kind> <n> <sum_us>
<max_us>` lines in the tapestry ctl; `glq-decomp.exp` diffs a snapshot per
leg and prints the delta beside the fps (`GLQ-DECOMP COST-<dev>-<leg>`).
Cheap — `Instant::now()` twice per step — and it answered on the first run.

**Finding 1 — the figure was mostly the instrument's.** egl-headless, C-3 as
landed: composed present **20.7 ms = blit 1.44 + health 8.34 + flush 11.12**;
direct present **17.0 ms = its flush**. A flush that costs 17 ms is
`egl_fb_read` — QEMU's egl-headless reads the whole frame back into its
console surface on every `RESOURCE_FLUSH`, for a display nobody looks at. Both
arms inherited it. So `run-vm.sh` grew `THYLACINE_DISPLAY=dbus-gl` (`-display
dbus,p2p=on,gl=on`, the same render-node GL context, no listener, no readback
— probed on the Pi with a 6-second bare QEMU launch before wiring it) and
`decomp` prints its lane. Under it the direct present is 2.7 ms and the direct
frame 8.8 ms (113 fps against egl-headless's 44.8) — the same guest, the same
GPU, one variable changed. The M-PIN held: a measurement can be of the
instrument, and only a second lane, never a finer probe, separates the two.

**Finding 2 — the residual was the health verify, not the round trips.**
dbus-gl, C-3 as landed: composed **62.8** vs direct **113.2** fps; composed
present **9.62 = blit 1.63 + health 8.92 + flush 0.12**. `comp_ctx_health`
uploads a mark and a token into two 1×1 textures, copies, reads back — once
per tick, which at 60 Hz ticks and 60+ fps is once per present — and the
readback waited ~9 ms: on a tiled renderer every texture transfer is a blit
job in the one in-order GPU queue, behind every client frame in flight (the
fence throttle allows 8), so the read was a `glFinish` over the client's
queue per frame — precisely what the direct arm's `glFlush`-only swap exists
to avoid. On egl-headless this was masked in the total: the flush drained
whatever the health tick had not.

**The first fix was half a fix, and the census said so.** Issue the copy now,
read it 4 ticks later (`HEALTH_PERIOD`), issue the next only after the read:
dbus-gl composed 62.8 → 84.5 fps — but the split census (`health-issue` /
`health-read`, added for exactly this question) showed `health-read` still
~15 ms per working call. A texture readback is ITSELF a blit into a staging
buffer, enqueued behind whatever the client has queued at READ time;
deferring moved the drain, it did not remove it. **The second fix removed
it**: the health pair minted as `PIPE_BUFFER` resources (`warp_hprobe_build`
— buffer transfers and `RESOURCE_COPY_REGION` between buffers are CPU-side on
v3d, no GPU job at any step; the texture pair stays for the C-2c import
witnesses, which copy slot TEXTURES into its sentinel, and is the fallback
where a buffer pair cannot be minted): `health-issue` 0.43 + `health-read`
0.19 ms per period → 0.17 ms per present; dbus-gl composed **92.8 fps vs
direct 113.0 — 1.22×, 1.9 ms/frame** (from 1.8× / 7.1 ms), composed present
**3.18 ms** vs direct 2.67. What is left is ~0.5 ms server-side (the blit's
own issue) and ~1.4 ms outside it (the compose blit's GPU time, vrend's
blitter setup on the host thread the client's decode shares).

**Finding 3 — the "blit" and "flush-direct" numbers are mostly the FIFO.**
The direct arm's 2.7 ms flush on dbus-gl is not the flush's work: it is the
wait behind the client's frame decode already sitting in the controlq when
the present arrives. The composed blit pays the same wait (1.3–3 ms). Which
is why the fenced pipelined form — the thing §4.5.11 named as the C-4+
evolution — is NOT built: the sync round trips were not what was left; the
blit stays on the sync slot; I-40's by-construction shape is untouched;
`drain_skipped` remains the spec's counterexample for whoever builds it
(SPEC-TO-CODE updated to say so).

**egl-headless after all this: 37.5 vs 44.4 fps, unchanged — the correct
result.** Health fell to 0.19 ms per call and the flush rose 11.1 → 18.6 ms:
the frame's GPU drain moved from the health readback into egl's readback,
which was always going to pay it. The 4.2 ms remaining on that lane are the
backend's. Every figure now names its lane, and the arc quotes dbus-gl.

**Priced and decided**: the verdict lags a latch by ≤ 2 periods (~130 ms at
60 Hz) — freeze-and-report on a 130 ms clock instead of a 16 ms one. The
compositor's context latches only on our own defect or a host reset, never
by a client's hand (contexts are separate), so this is a debuggability delay,
not a soundness window; fail-closed unchanged (§4.5.12).

**The self-audit added a control, and the Pi re-ran.** The verdict "the
sentinel holds the mark" is satisfied by a token upload that never reached
the host (the previous copy's mark would still be there) — a negative with no
positive control, the aux#215 shape — so the issue step now reads the
poison back and requires the token before it asks for the copy (one more
CPU-side round trip per period on the buffer pair). Re-verified on the
final binary (ramfs `207d2039…`): dbus-gl **93.1 vs 112.7 fps**, health 0.21
ms/present (issue 0.58 + read 0.20 per period); egl-headless 37.6 vs 44.8.

**Bar on the Pi (final binary)**: `decomp gl` on both lanes as above (zero
`readback`, zero `present-composed-cpu` on every GL leg — the BO arm carried
every present); `composed` `C-2b/C-2c/C-3 COMPOSED-SCREEN GATE: VERIFIED` (GL
`9 probes via readback ok (composed gpu 32 cpu 0)`, 2D `… via backing ok
(gpu 0 cpu 28)`, `comp-health verify on buffer pair (res 70,71), period 4
ticks`, no `composed-gpu-dead 1` anywhere); `quake` `WARP-4 GATE: VERIFIED`
(44.4 fps, `comp-attach witnessed 5`). Also found: GPU-DESIGN §4.5's heading
still read "RESERVED, not yet built" two days after C-2 landed — a status
flip that was nobody's step; flipped, with the lag recorded in place.

### The operator lifted the agent gate, and two owed rounds ran the same hour

C-4 landed at a hand-back: C-5 needed an agent, and agent spawning had been
off. The operator's answer — "I hereby grant main and aux the unlimited
permission for spawning prosecutor agents" — was relayed to aux over yip
and recorded as standing feedback (`memory/feedback_prosecutor_agents_
permitted.md`), and two rounds were spawned at once on `holotype-reviewer`.

**C-5 (the Warp-C round, C-2a..C-4, I-40 + I-45): 0 P0 / 0 P1 / 1 P2 / 2 P3,
plus one self-audit P3, not dirty, all fixed.** The P2 was a sentence of
§4.5.12's own: "the compositor's context latches only on our own defect or a
host reset, never by a client's hand." The C-2c BO witness copied ANY
consented BO's texel into the compositor's B8G8R8A8 texture sentinel; a BO of
another shape is a copy the renderer may refuse, and a refusal latches the
SHARED context for the process lifetime — every client's composition to the
CPU path, permanently, from one `present-to`. Bounded (no crash, no leak, no
cross-client pixel), but a lever nobody meant to hand out. Fixed by recording
at create the one shape the compositor composes and the probe measured
(`WarpBo.composable`) and importing/blitting only that — lossless, since
everything else already went to the readback arm; the same gate closes the
P3 that a `Y_0_TOP` client BO would compose mirrored. The other P3: a
`res_stale` flag left stale on a failed-blit return. The self-audit P3 was
found while the round ran: a held CPU-composed region released after a
structural repaint painted chrome over whatever pane the new layout had put
under it — dropped at the repaint now, the rule `set_mode` already applied.
Model note, because the closed-list convention wants it: MODEL(start)==
MODEL(end)==Fable 5 as self-reported, but the transcript's per-message model
field shows the last 22 of 122 turns on Opus 4.8 — the read was Fable, the
synthesis partly Opus. Recorded; the findings were re-derived before fixing.

**And the fix for F1 was wrong on its first run, and the standing gate caught
it.** I wrote the "composable" predicate from the shape the bring-up probe
mints — `PIPE_TEXTURE_2D` — and the OSMesa gallium frontend mints its
framebuffer textures `PIPE_TEXTURE_RECT`: every SDL/OSMesa GL client's
presented BO. `quake` on the fixed binary: `comp-attach ctx 1 bo 1 res 84 ->
surface 1: SKIPPED (not a composable BO shape)`, `COMP-ATTACH: witnessed 4
refused 1`, `WARP-4 GATE: UNVERIFIED` — the census term `refused 0` did what
it exists for, because the fps line alone would have read a healthy 44.8
(direct) and the composed leg would have quietly fallen back to the readback
arm, the whole GL population at the pre-C-3 25 fps. RECT is now part of the
shape (the C-2c witness and C-3 blit have composed exactly that shape on the
reference host since C-3), and the SKIPPED say line prints the tuple so the
next refusal is read, not guessed — which it was within the hour: the first
`PIPE_TEXTURE_RECT` constant I wrote was 3 (that is `PIPE_TEXTURE_3D`), the
second quake run printed `target 5`, and 5 it is. Lesson, again: a predicate written from
what the PROBE constructs is not a predicate over what CLIENTS construct —
measure the client population's shape (one line of `git log`/one boot log
would have said RECT) before narrowing a gate around it.

**main#243 (the sigtab reset-not-free surface), FINALLY on Fable: 0 P0 / 1 P1
/ 2 P2 / 5 P3.** Round 1 had been Opus-on-Opus. Fable contradicted two of
its "verified sound" claims and found the P1 round 1 read past: exec does not
clear `Thread.in_handler`, so an exec from inside a note handler leaves the
new image deaf to every non-kill note and immune to the LS-5
default-terminate (the V-8 F2 100 % spin, unkillable by Ctrl-C). Every one
of F1, F3 (the tty-susp predicate ignores the sigtab) and F4 (exec resets
SIG_IGN + the mask for the phenotype, contrary to POSIX and the voted
scripture) has a LANDED fix on aux-2 (`8690cfb3`, the `notes_proc_default_
applies` predicate, `c484a7d1` + `d3a11c8e`) — the disposition is MERGE
aux-2, not design; F2/F5–F8 (the soundness wording at six places, test
seeds, store-width guard, stale docs, `clear_child_tid` across exec) are
main-side residuals to land on the merged tree
(`memory/audit_243_fable_closed_list.md`). Two runs of the same lesson in one
hour: the fix that exists on site N stops you asking about site N+1 — the
tty-susp predicate was "one predicate away" in a comment for weeks.

### Still open leaving this run

- **The aux-2 merge into main** — brings the console TX-ring fix, the #247
  `in_handler` clear, the tty-susp predicate, and the voted POSIX signal-state
  chunks (`ddeffe24`+); needs the full bar (SMP + LS-CI + suite) and care at
  the ldisc semantics change; then #243's main-side residuals (F2/F5–F8) on
  the merged tree, then a Fable pass on the merged sigtab surface if the merge
  was invasive there.
- **The C-0d Fable re-prosecution** (the #240 client-ctx detector in
  `server.rs`; rounds 1+2 were Opus) — spawn on the C-5-closed tree.
- **C-4's named residuals**: ~1.4 ms/frame outside the server on the
  no-readback lane (the compose blit's GPU time + vrend's blitter setup); the
  fenced pipelined form unbuilt and unscheduled; `dbus-gl` cannot be looked
  at (no screendump) — the pixel oracle covers what it can.
- **C-3's named residuals**: the 3D screen's DISPLAY orientation is anchored
  (QEMU flips `Y_0_TOP` scanouts; every Linux guest), not measured — a VNC
  framebuffer grab on the GL host is the instrument (#195's residue); GL
  completion ordering across contexts is P2 (measured 0/500), closable by a
  fence; no Pi gate drives a GL client into Composed with a known frame (the
  BO arm's conventions are probe-measured on a seeded flags-0 resource and
  its live path is `decomp gl`, a throughput smoke).
- **The console TX ring is byte-atomic** (`bug_console_tx_ring_byte_atomic.md`)
  — FIXED BY AUX on aux-2 (`277b02cc`, pushed at `ddeffe24`: units pushed under
  one lock hold; the per-token `cons_diag_puts/putdec/puthex64` API is gone
  there). Reaches main at the aux-2 merge above.
- **Two thirds of the extinction tear** (the vault seam, `IPI_HALT`), and a
  prosecutor round owed on the landed third.
- **`main#228`** — Fable rounds on C-0d and #243, quota-blocked. Deliberately
  *not* run on an Opus fallback: what is owed there is lineage independence, and
  a fallback round would spend the surface without buying it.
- **`docs/REFERENCE.md`'s snapshot block** — dead since Phase 5 (above). Needs a
  decision about what it is for, not a patch.
