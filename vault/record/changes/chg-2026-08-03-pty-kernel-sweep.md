---
id: chg-2026-08-03-pty-kernel-sweep
type: chg
title: "the PTY kernel seam -- a guard that cannot tell a dead session from a live one, and three claims about what happens when a leader dies"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-kernel-pts
  - sub-kernel-jobctl
  - lock-pts
  - inv-i20
  - spec-pty-stop
  - moc-kernel-execution
  - sub-kernel-proc
  - sub-kernel-death
  - sub-ptyfs
established:
  - sub-kernel-pts
  - sub-kernel-jobctl
  - lock-pts
closed: []
opened: []
depth: skeletal
created: 2026-08-03
---
Batch 32, the fourth sweep off the census: the PTY kernel seam -- `kernel/pts.c`
+ `kernel/include/thylacine/pts.h` (668 lines, the registry and the tty seam)
and the job-control block inside `kernel/proc.c` (~280 lines, [[inv-i20]]'s stop
leg). Main unmoved at `c0c76977`; L-1 absent on the TWENTIETH check. Two
dossiers, one lock note.

The scope grew by one surface on reading. Task #53 named `pts.c`, but the
suspend class does not signal -- it routes into a fan that decides, per member,
whether to stop or merely notify, and that fan lives in the process file
alongside the catchability gate, the report latches and the POSIX orphan rule.
`proc.c` was already two dossiers (the Proc, the death path); the code's own
section boundary makes it three.

**THE SEAM IS THE GOOD PART, AND IT IS WORTH SAYING SO FIRST.** ptyfs's entire
signal power is a call taking `(pts_id, class)`. There is no process-group
parameter to get wrong. The kernel resolves pts -> controlling session ->
foreground group; a server cannot escape its own terminal because it was never
given a way to name anything outside it. [[inv-i20]]'s third clause is enforced
by an **absent argument** rather than by a check -- and the surrounding design
is consistently of that kind: no pts handle exists to dup or leak, the
correlation key is a pointer the resolve never dereferences, and the
controlling-terminal state lives on the kernel's entry so no server bug can
corrupt it.

**F1 -- THE ANTI-STEAL GUARD CANNOT TELL A DEAD SESSION FROM A LIVE ONE.**
`ct_sid` is a bare pid, set at acquisition, cleared only at free. Nothing
clears it when the session leader dies. So `pts_tty_acquire`'s guard
(`pts.c:377`) -- correct and deliberate against a live session, the round-1 F7
close -- refuses **every later acquisition on that pts for the life of the
entry**. A dead session owns the terminal.

Latent: the one in-tree host opens a master, spawns a single child, and exits.
It becomes reachable the moment anything respawns on a pts it already holds --
the ordinary terminal-multiplexer shape -- where the new shell silently gets no
controlling terminal and no job control.

The fix is cheap and already demonstrated *in the same file*: the
foreground-group gate checks membership UNLOCKED before taking the registry
lock, accepting a benign race, precisely because the check walks the process
table. Acquisition could do the same and does not. Task #67.

**F2 -- THREE DOCUMENTS, THREE DIFFERENT ANSWERS TO "WHAT HAPPENS WHEN THE
LEADER DIES".** `PTY-DESIGN.md:31` gives *"SIGHUP-on-leader-death"* as a
motivation for having sessions at all. The audit finding that closed the area
(`:657`) restates it as *"session-leader death fans `tty:hup`+`tty:cont` to
orphaned stopped pgrps"* -- which is the orphan rule, a **strictly narrower**
property. The code implements the narrow one.

All three `tty:hup` posters are the teardown fan, the server's explicit hangup
class, and the orphan rule. **None fires on a controlling process's death.** A
foreground job whose shell dies gets nothing unless it is *also* stopped and
newly orphaned; carrier loss arrives when the master closes instead.

The narrowing is the batch-28 shape again -- a claim's subject quietly smaller
than the claim -- except here it happened *inside an audit close*, which is the
document a later reader trusts most. Task #68.

**F3 -- A COMMENT DESCRIBING A MECHANISM THAT WAS NEVER BUILT, FOR A CASE THAT
CANNOT HAPPEN.** `proc_setsid`'s header: *"when the registry exists, this core
also clears any binding owned by the OLD session iff the caller was its leader
(wired at PTY-1d)."* The registry exists, PTY-1d landed, and `setsid` touches
no registry state -- there is no `pts_*` call anywhere in `proc.c`.

It does not need to, and neither half of the comment is right about why. A
session leader always has `pgid == pid` (setsid sets both; setpgid refuses a
session-leader target), and setsid refuses exactly that caller -- so **the
caller can never be the old session's leader.** The condition is unreachable.
And the property the design leans on -- *"setsid drops the controlling tty"*,
listed among the round-1 verified-sound items -- holds by a **third** mechanism:
every terminal call compares against the caller's LIVE session id, so changing
it detaches the caller immediately, with nothing to clear.

Three mechanisms in play; the comment names the one that does not exist. Task
#69.

**THE COUNTERWEIGHT, AND IT IS SUBSTANTIAL.** Two things in this batch are the
best of their kind so far.

The **non-completing stop wake**. The death cascade fabricates a *completed*
wait -- immaterial for a Proc about to stop existing. Applied to a stop it was a
real, visible bug: the fabricated success rode back to EL0 at resume and made
every timed wait *finish* instead of continue, so a resumed `sleep` exited, `fg`
reported the job done, and a second Ctrl-Z found no job. The fix distinguishes
the two wakes, and its regression test asserts that a timed wait **survives** a
stop -- a test of a property whose absence produced a shell bug rather than a
crash, which is the hardest kind to think to write.

And `proc.job_stop_orphan_rule`, which carries both polarities in one body: a
suspend on an orphaned group affects nobody and posts nothing, then the *same*
suspend on the *same* group, re-homed under an anchoring parent, stops it. Two
outcomes from one stimulus with only the anchoring changed. That is a test of
the rule rather than of the fan, and it is what the batch-31 finding
(`elf_brand_hint`'s eleven assertions on a function nothing calls) was the
inverse of.

**A SMALLER OBSERVATION WORTH KEEPING.** The self-managing-notes predicate fails
closed toward *not* self-managing, and its comment justifies that as the safe
default for the uncaught-interrupt terminate: an unverifiable Proc must not
dodge being terminated. The suspend gate now asks the same predicate, where the
same answer pushes toward *stopping*. Both land on "act on it", so the default
is safe in both -- but only one of the two reasons is written down. **A
fail-closed default is closed relative to a particular question**, and this one
now answers two.

**PATTERN, NINE BATCHES.** b24 assertions pin values not their description; b25
models pin mechanisms not their own scope; b26 each copy pinned to itself not to
the others; b27 the guard travelled but not its reason; b28 the ledger pins the
areas not the areas to the tree; b29 the enforcement list names a guard that
cannot fire; b30 plus a justification whose stated and real reasons diverged;
b31 the documents are wrong about which code runs; **b32 the guard is right
about the case it was written for and silently wrong about the case nobody
asked it -- and three documents give three different answers to the question
that would have surfaced it.**

The through-line since b29 is sharpening: a mechanism is written, argued, and
tested against the case that motivated it, and the *adjacent* case inherits the
mechanism without inheriting the argument. b27's guard-without-its-reason was
the first instance; F1 here is the same shape with a liveness dimension the
original question did not have.

LEDGER. Corpus 812 -> **816**. Coverage 148 -> **150 owned of 421 (35%)**;
`kernel` 33 unowned -> 31. [[inv-i20]] gains its two kernel guards -- it had
been carrying [[sub-ptyfs]] and [[sub-kernel-proc]] alone since batch 27, which
meant the invariant pointed at the server that enforces one clause and at a
dossier that mentions the fields in passing, and at nothing for the two files
that hold the rest.
