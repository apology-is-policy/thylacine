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
> **This is the aux-track copy.** `aux-2` had no journal until 2026-08-17; this
> file carries the same header as main's so a merge is a concatenation (both
> are newest-first). Main's entries are not duplicated here.

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

## 2026-08-17 (aux) — the interactive `viv run` that never ran; the diorama channel goes private

The R5-F9 experiment needed an interactive Alpine ash under `viv`, and the
first attempt to drive one from a `ptyhost`ed `ut` produced nothing: no ash
prompt, no output from a `sh -c 'echo …'` bisect, viv back at the outer prompt
at once. The resume note going into this watch carried a hypothesis — viv's
`stdio_born` gate (`fstat` on fds 0/1/2) fails on a pts, so the entrypoint is
spawned fd-less — and the instruction to VERIFY before fixing. The verification
took one grep. Every one of the four logs has the console line
`viv: spawn /bin/diorama` right after the `viv run` echo, and that line is not
progress: it is `Err(String::from("spawn /bin/diorama"))` reaching `say`. The
container never existed. The line reads like an announcement, and the previous
watch read it as one; the source says it is the report that the announced thing
failed. **A line that names an action may be the report that the action
failed — grep the source for the string before reading it as progress.**

The mechanism took a few more. `viv` requests `SPAWN_PERM_MAY_POST_SERVICE`
for its per-container diorama, because the diorama posted the fixed name
`/srv/viv-dio`; `spawn_perm_grant_check` grants that bit only to a
console-attached granter or an existing holder; joey confers it on login, login
confers `CONSOLE_OWNER` on `ut` and nothing more, `ut` confers nothing on its
externals — so no session shell's `viv` was ever a holder, and every boot-gate
`viv` was joey-spawned WITH the bit, so no gate ever ran the path a person
runs. The V-7 commit body had listed exactly this as a "known seam" and moved
on. A bug that lives only in prose is a bug being walked past in slow motion;
this one waited eighteen days for someone to type `viv run` at a prompt.

Two fixes were possible and the research collapsed them to one. Widening the
privilege — login confers the bit on `ut`, `ut` on every command — puts every
user program in a position to squat names in the ONE shared boot registry
(`/srv/home-<user>` before that user logs in; a tombstoned `/srv/net` after
netd dies is re-postable by any marked Proc), and leaves the fixed-name
collision in place. The other reading is Plan 9's own: a 9P server a process
starts for itself is reached by `mount(fd)` over a pipe, and `srv(3)` exists to
publish fds to strangers — this channel has no strangers. The kernel had the
primitive since Phase 5: `SYS_ATTACH_9P(tx, rx)` over two Plan 9 pipes, the
`stub-driver` shape, tested by `test_attach_probe` and used in production by
nothing. Now `viv` makes two pipes, spawns `diorama --vivarium <pid>` with the
server ends as the child's fds 0/1 and nothing else, attaches the client ends,
mounts at `/dio` as before; the diorama serves that one connection until EOF.
No name, no privilege, no collision. Three seams closed by removing a
mechanism rather than adding one: the interactive path works, concurrent
containers move from OUT to IN, and the V-8 F3 attach gate — a peer-pid check
in `h_attach` plus a joey deny leg — becomes structural (nobody but the runner
holds an end) and comes out. What the diorama still checks is its one scoping
premise, that the argv runner is its parent, read off its own status file's
`ppid` line. Its `self` is derived rather than kernel-stamped now (there is no
`SYS_SRV_PEER` on a pipe): the runner's pid, its own uid/gid, a native
liveness resolve — the same content the stamp gave, since the peer was always
the mounter (#90 unchanged).

The gates were rebuilt around the new shape rather than deleted with the old
one. The `#101` deny leg became the `viv-channel` leg: two spawns of `diorama
--vivarium` one variable apart — joey's own pid (the attach must succeed, and
with the server provably up `/srv/viv-dio` must NOT resolve, and closing the
attach root must make the diorama exit on EOF within a bounded wait) and a pid
joey is not (the diorama must exit at its parent check before Tversion, so the
attach must fail). Neither branch can pass for the wrong reason: a diorama that
cannot start satisfies only the refusal, a diorama with no parent check
satisfies only the serve. And the V-7 leg now spawns two `viv run
/vivarium/probe` concurrently: each probe asserts its pid view is exactly
`{self}`, so two live containers prove from the inside what `#101` proved from
outside, under the concurrency the old design refused. Every boot `viv run`
was also stripped of the perm bit it no longer needs, so the gates run the
interactive path instead of a privileged twin of it — the same shape as the
L26 premise trap a stanza above: a gate that runs a different path than the
user does is a gate on the wrong thing.

One residual, recorded and not built here because it is kernel-side on the
Pipe audit row: `devpipe_write` posts a `pipe` note to the WRITING Proc when a
ring's reader is gone, and the kernel 9P client's spoor transport writes in the
syscalling Proc's context — so a container Proc that touches `/proc` after its
diorama has died (an orphan outliving its runner; a diorama crash) gets a
`SIGPIPE`-shaped note where the `/srv` transport gave only an error. Linux's
kernel 9P client signals nothing (it writes from a workqueue); the fix is a
`MSG_NOSIGNAL`-shaped kernel-internal transport write.

### The bar over the tip

First boot after the change, no retries: kernel suite 1413/1413 (the kernel
binary was not rebuilt — no `kernel/`/`arch/`/`mm/` file changed);
`joey: V-7 viv-probe (containered, x2 concurrent) PASS`; `joey: viv-channel:
private pair serves, no /srv name, EOF-exit`; `diorama: --vivarium pid is not
my parent` then `joey: viv-channel: non-parent runner REFUSED`; V-1b + L-6c +
D-5 PASS; `Thylacine boot OK`. LS-CI `viv-run` (the new scenario): PASS on
attempt 1 — the console `viv run /vivarium/probe` printed the probe's leg-6
line, the `ptyhost`ed `viv run /vivarium/alpine-ash` showed `/ $ ` on the pts
and answered `ASH-ALIVE` through it (a pts trio passes `stdio_born` and flows
both ways — the question the retracted hypothesis raised, answered by the
witness rather than the fix), `exit` returned to `ut` twice over. Landed as
`437213c4`; the full LS-CI run rode the next chunk's bar (the ^C finding
below), one bar for both.

## 2026-08-18 (aux) — the first ^C killed the runner; and a hosted `ut` loses a line after two

With the channel fixed, the R5-F9 experiment finally reached an interactive
phenotype ash — and its first ^C at the prompt produced not the ash prompt but
`proc: orphan pid=N name="sh" (parent viv exiting)`, then the same line for the
diorama, then a terminal in which nothing answered coherently. Three of three
attempts. Not the R5-F9 wedge at all: the pts's ISIG cooks 0x03 into an
`interrupt` for the FOREGROUND PGRP, and `viv` runs as `ut`'s foreground job,
so its pgrp is `viv` + its diorama + every container Proc. The container's
shell sees SIGINT and handles it; the two NATIVE members have no handler and no
notes fd and die of an uncaught `interrupt` — LS-5's default, working exactly
as designed on the wrong recipients. The orphaned shell then kept reading the
same pts as the outer `ut` and split every later keystroke with it, which is
why the control legs saw nothing: PTY-4's "no TTIN arbitration" footnote seen
from the other side.

The fix is a mask and only a mask. `viv` masks `interrupt`: the container needs
nothing forwarded (it is in the pgrp; the note reaches it directly) and
inherits nothing (a spawned child starts with a zero mask — `rfork_internal`
copies `note_mask` only when the PARENT is `PHENO_LINUX`, and the native
exec-image reset zeroes it). The tty family stays UNMASKED in `viv` on purpose:
^Z must STOP `viv` with the container, or `ut`'s `wait_pid(WUNTRACED)` on the
job never sees it stop and the terminal is never handed back; a hangup ends
`viv` with the container; ^\ still kills the runner and detaches a running
container, as `docker run` does under SIGQUIT. The diorama has no such
constraint — nothing waits on it as a job — so it masks both families: a
server never dies of a keystroke, and its lifetime is its channel's. Two kernel
facts were read rather than assumed first: the terminate LATCH is armed at post
regardless of the mask, but both its consumers — the EL0 tail's terminate scan
and the #811 sleep predicate — honour the per-thread mask, so a masked
`interrupt` neither delivers nor unwinds the blocked `wait_pid`; and the
ldisc's post is `synthetic`, so repeated ^C coalesce rather than fill the
queue.

`viv-run.exp` gained the leg, with a witness that names WHICH shell answered:
`uname -s | tr a-z A-Z` says `LINUX` from the phenotype ash and `THYLACINE`
from `ut`'s coreutil, so a runner that died and handed the terminal back could
not pass it by `ut` executing the line. PASS on attempt 1 on the build whose
only change from `437213c4` is the two masks.

And the "unexplained" note from the previous watch reproduced on its first
try. `scratchpad/r5f9/ctrlc-idle.exp`: two ^C at the console `ut`'s idle
prompt, then a command — answered; ONE ^C at a `ptyhost`ed `ut`'s idle prompt,
then a command — answered; TWO ^C there, then a command — echoed, not executed
within 30 s; an Enter, then a command — answered
(`outer-cc=1 inner-c=1 inner-cc=0 recover=1`). The rendering suggests the
hosted `ut`'s `interrupt` arrives late and its line-discard eats characters of
the NEXT line; with two, the second discard lands around Enter and takes the
whole line. That is aux's own line — notes, job control, the pts — and it is
enqueued as item 10 (`memory/bug_hosted_ut_double_ctrlc_idle.md`) rather than
folded in here: it is a different mechanism from this chunk's, and it wants its
own root cause before its scenario joins LS-CI. Every gate we had covers ^C on
a foreground JOB; none covered ^C at an idle hosted prompt.

The R5-F9 question itself — does busybox ash's `raise_interrupt` longjmp out of
its SIGINT handler and wedge `in_handler` — ran next, on the runner that now
survives the ^C it needs, and came back INCONCLUSIVE on the wedge and
conclusive on something underneath it: after ash's prompt (blocked in `read`)
a ^C produced NOTHING for ten seconds, twice; the next typed line was then
discarded (`^C`, newline, reprompt AFTER the line) — the SIGINT was delivered
exactly when the read completed. Read in the kernel: `thread_die_pending` is
the only sleep-unwind predicate and it fires for group death and the UNCAUGHT
terminate latches only, so a CAUGHT note never wakes a thread blocked in a
syscall wait; Plan 9 (`Eintr`) and Linux (`EINTR`/`ERESTARTSYS`) both do. A
note that arrives with the next line is not late, it is undelivered. That is a
kernel design fork (item 11, `memory/design_caught_notes_do_not_interrupt_
waits.md`), the operator's to vote, and it very likely owns item 10 too.

### The bar over the tip (`5336c894`: the channel + the ^C masks)

Boot (first after the change, no retries): kernel suite 1413/1413 — the kernel
binary was not rebuilt (no `kernel/`/`arch/`/`mm/` file changed), so no SMP
gate; the joey `viv-channel` legs + `V-7 viv-probe (containered, x2
concurrent) PASS`; V-1b + L-6c + D-5 PASS; `Thylacine boot OK`. LS-CI **34
PASS + 2 SKIP (GL not baked) over 36 scenarios, 0 retries** (TCG, sequential,
~55 min), the new `viv-run` among them. Pushed to both mirrors after the
fixup; main ratified the channel design on the line (call 0025) and had merged
aux-2 @13149152 into main (8a58112d) meanwhile.
