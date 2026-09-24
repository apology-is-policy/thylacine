---
id: sub-substrate-gates
type: sub
parent: moc-substrate
title: "The gates — boot verdict, multi-boot classification, the v8.0 floor, the host tests"
code:
  - tools/test.sh
  - tools/smp-multiboot.sh
  - tools/test-smp-classify.sh
  - tools/ci-smp-gate.sh
  - tools/check-v80-floor.py
  - tools/screendump.sh
  - tools/test-rust.sh
audit: none
guarded-by: []
validated-by: [prose, gate-smp, gate-v80-floor]
locks: []
abis: [abi-boot-banner]
design: ["docs/TOOLING.md", "docs/PORTABILITY.md", "docs/DEBUGGING-PLAYBOOK.md"]
created: 2026-08-01
updated: 2026-09-24
---
## Purpose

The non-interactive verdicts. `test.sh` decides whether ONE boot passed;
`smp-multiboot.sh` decides what a failure MEANS; `ci-smp-gate.sh` decides
whether the matrix as a whole is clean; `check-v80-floor.py` decides whether
what shipped can run on the baseline CPU. `test-rust.sh` decides whether the
userspace Rust crates' host unit tests pass -- and, as important, how many of
them run on no machine at all.

## Contract

- `test.sh` → exit 0 iff the boot reached [[abi-boot-banner]] with no
  extinction, and every enforced post-boot gate passed.
- `smp-multiboot.sh <label> <cpus> <N> [sanitizer]` → exit 0 iff
  0 CORRUPTION, 0 EXTERNAL-KILL **and** 0 OTHER across N boots.
- `ci-smp-gate.sh` → builds each kernel once, runs the matrix, aggregates.
- `check-v80-floor.py` → exit non-zero if any tracked build input asks above
  ARMv8.0-A, or any shipped userspace binary carries an ungated LSE.
- `test-rust.sh [crate...]` (`make test-rust`) → exit 0 iff no crate FAILs:
  its lib tests failed in any pass, or its test output could not be reconciled
  with cargo's own count. Prints per crate the bucket, the tests run, the
  quarantined ignores, the compiler warnings, and STRANDED -- the `#[test]`s the
  sources declare that compiled into no pass. Stranded tests and warnings are
  reported, never fatal.

## Mechanism

**Extinction outranks the banner, and the banner no longer ends the boot.**
Since A-5a joey persists past `SYS_BOOT_COMPLETE` (it getty-loops
`/sbin/login`), so post-banner code can fault. `test.sh` therefore checks
`EXTINCTION:` FIRST on every poll, and on banner-observed it watches a
`BANNER_GRACE` window (default 3 s) for a post-banner crash before declaring
PASS. A pass is "banner, and still healthy a moment later."

**Every log grep is `grep -a`.** Boot logs carry binary spill; without `-a`
grep decides the file is binary and reports only "binary file matches",
which a `-q` test then reads as a match and a `!` test reads as its
opposite. This is uniform across all four scripts.

**A single boot is not a gate.** The #788/#806/#860 context-corruption races
are layout- and timing-sensitive and pass most single boots — a one-shot
`test.sh` "is the thing that masked #860 for weeks." The gate is therefore
N≥10 boots per config against ONE built kernel (host jitter varies the
timing), across five configs. UBSan-smp4 is the amplifier: on the broken
bringup it crashed 33–43% of boots and 0% of a single lucky one.

**One of those five rows boots ONE CPU, and that is a control, not a
contradiction (`default-smp1`, 2026-09-22).** A peer CPU is not only extra
concurrency -- it is a RESCUE MECHANISM, and a hazard a rescue mechanism hides
is one a matrix of smp4/smp8 rows cannot observe. Every row booted 4 or 8 CPUs
(and `test.sh` defaults to `-smp 4`), so the one configuration in which a
thread spinning on another thread's write cannot be rescued was the one nothing
booted. It cost a 100% boot hang that ran unseen for 19 days: `loom_free` joined
the SQPOLL kthread with a spin inside a syscall body, which is non-preemptible,
so at `-smp 4` a peer ran the kthread and the spin ended, and at `-smp 1` every
boot wedged at `loom-smoke`'s exit. "Single boots lie" is true about SMP races;
it was read as licence to stop booting single CPUs at all, which is a different
claim. The row is cheap -- 1-vCPU boots are the fastest in the matrix, with no
bimodal P-core/E-core spread. **If it goes red on a joey non-zero exit rather
than a CORRUPTION, suspect task #791 and measure before concluding:** `test.sh`'s
header records #791's ~45% `-smp 1` joey failure (2026-05-30) and, beside it,
that the rate did NOT reproduce on 2026-09-22 (5/5 clean) -- evidence it
changed, not proof it is gone, since 0.55^5 is about 5%.

**The classifier has FIVE classes, and three of them fail the gate.** The
ladder is ordered, so each row is also "everything above it did not match".

| Class | Fails? | Anchored on |
|---|---|---|
| CORRUPTION | yes | exact extinction strings (invalid prev state, stack canary mismatch, kernel stack overflow, already on_cpu, #860, …) |
| EXTERNAL-KILL | **yes** | two arms — QEMU's `terminating on signal N from pid M` (catchable, #88), OR the shell's `line N: PID Killed: 9` + `qemu_alive_at_teardown=0` (SIGKILL, #222) |
| INJECT-MISS | no | the full green-guest proof (below) |
| TIMING | no | EMITTED warn strings only (`[SOFT-WARN]`, the irq-bench budget text) |
| OTHER | **yes** | nothing — an unclassified nonzero exit |

OTHER failing is the load-bearing choice: an unexplained red is surfaced,
never absorbed. There is deliberately no bucket for "probably fine."

**But OTHER-fails is necessary and not sufficient, which is what #88 is
about.** A single boot of forty reported `OTHER fail: <unclassified>` and
failed the whole gate — while the guest was provably healthy (1236/1236
PASS, 0 extinction) and the log's last line was QEMU announcing it had been
signalled from outside. The verdict was not wrong. It was **undifferentiated**,
and that costs twice: a full investigation every occurrence, and camouflage
for any genuine failure landing in the same bucket.

So the dossier's older lesson has a twin. The #362 regression showed that **a
benign catch-all buries failures by passing them**; #88 shows that **a failing
catch-all buries them too, by making the red routine.** Both destroy the
signal; one by silence, one by crying wolf. The fix in each case is the same
shape — take the explainable cause out of the bucket and give it its own honest
label — and in neither case does the verdict change.

**EXTERNAL-KILL has two arms, because catchable and uncatchable signals leave
different evidence (#222).**

*Arm 1 — the QEMU report (catchable: TERM/INT/HUP).* QEMU prints
`terminating on signal N from pid M`, which names the signal AND the sender. Its
soundness is negative space: a guest cannot signal its own hypervisor, so the
line's mere existence is the evidence; the signal number and pid are a bonus the
classifier had been discarding.

*Arm 2 — the shell's job notification (SIGKILL).* SIGKILL is **uncatchable, so
QEMU prints nothing** — which means arm 1 is structurally blind to exactly the
signal that most needs the bucket, and a real external SIGKILL therefore landed
in OTHER (the #200 sightings, the finding that made arm 2 necessary). The only
witness is the harness shell's own job notification, `line N: PID Killed: 9`, in
the harness stream. That notification alone is NOT "someone else did it": bash
emits it for the harness's OWN teardown `kill -KILL` too, whenever a command
boundary elapses before the shell reaps. So arm 2 requires a second conjunct only
`test.sh` can supply — `qemu_alive_at_teardown=0`, meaning the teardown kill hit
an already-dead process, so the signal death cannot have originated here. Arm 2
reports the sender as **NOT RECOVERABLE**, because a SIGKILL carries none — the
premise the whole bucket rests on. The pattern anchors the whole `line N: PID
Signal: M` shape (not a bare `Killed:`) because the harness log EMBEDS the guest
log, so a bare token would be forgeable by guest output; and it is deliberately
NOT widened to `Segmentation fault:` / `Abort trap:`, which are QEMU crashing —
a different class that must not be laundered as external interference.

**The pattern was validated against a fresh specimen, not against the captured
incident** — and that is the whole reason it works. Generating a real kill from
*this host's* QEMU binary revealed a trailing ` (<unknown process>)` suffix
that the original capture did not have, so an end-anchored regex would have
matched the historical log and missed every future one. The pattern
deliberately does not anchor the line end; both forms match, and where QEMU can
resolve the sender the suffix names it for free.

**Ladder position is load-bearing in both directions.** After CORRUPTION, so a
real corruption signature is never masked by a kill that landed afterwards;
before INJECT-MISS, so a killed-but-green boot cannot be absorbed into a
non-failing class. Either move loses a real failure.

**The exit-0 branch checks CORRUPTION and deliberately does not check
EXTERNAL-KILL**, and the asymmetry has a reason on each side. A corruption
marker on a passing boot is a leak worth catching regardless of exit status. A
kill line on a passing boot means the signal arrived *after* the verdict was
computed — the boot passed every gate before dying, so it is a genuine PASS.

**The sender is `ps`'d at CLASSIFY time, not at report time.** Seconds after
the kill a long-lived sender may still be alive; by the time an operator opens
the log the pid is long gone and unresolvable. Volatile evidence has to be
captured at detection, which is a general instrument rule and not a detail of
this class.

The two proving boots were removed from the capture directory afterwards: a
self-inflicted EXTERNAL-KILL capture left in `build/multiboot-fails/` would
read as a real incident to the next operator.

**Two precision rules the classifier learned the hard way.** The corruption
regex uses exact strings because a bare `canary` matched the benign
`canaries` hardening banner and the `canary: initialized` boot line — a
false positive on every healthy boot. And the TIMING regex is anchored on
emitted warn text and **never on test names**: the pre-#362 pattern
contained `stalk.*lifetime`, which matched the PASSING line
`[test] stalk.lifetime_no_leak ... PASS` present in *every* log — making
TIMING a catch-all that silently absorbed any nonzero exit. It buried 23 of
40 inject-misses, "and a real unclassified failure would have been too."

**The classifier is a pure function the ladder is DRIVEN through, not a block of
inline greps (#234).** `classify_boot()` and `harness_result_token()` are
sourceable — `smp-multiboot.sh` `return`s at its `BASH_SOURCE != $0` source-guard
before any side effect — so `tools/test-smp-classify.sh` drives the REAL ladder over real
and synthetic fixtures. A classifier nobody can exercise has arms that are
*assumed* to fire rather than known to; and a test that re-declared the patterns
would only prove the copy agrees with itself (#143), which is why the test
sources the production ladder instead of restating it. The producer cross-check
is the other half: `harness_result_token` maps `test.sh`'s verdict strings to a
capture-name token, and the classify test asserts `test.sh` STILL emits each
literal — renaming a verdict on one side silently voids an arm otherwise, which
is #234 found exactly that way. One arm order is load-bearing: `arc-gates` MUST
precede `pass` (#212), because a post-banner gate fails AFTER `==> PASS` is
already in the log, so a missing arm degrades to `pass` — worse than `unknown` —
and would have labelled the capture OTHER-pass. The OTHER bucket now carries that
token too (`result=<token>`), separating "QEMU vanished" (qemu-exit) from "a
post-banner gate failed", which OTHER used to conflate.

**INJECT-MISS requires proving the guest green, not merely proving the
injection missed.** All five must hold: the `AWAITING_QMP_KEY` sentinel
present, a clean `virtio-input: SKIP`, the banner present, no `EXTINCTION:`,
and no suite FAIL line. A boot that merely *also* missed injection stays
CORRUPTION or OTHER.

**Verify the artifact, not the intent (#101).** `build.sh` re-bakes the pool
from the ambient environment and `THYLACINE_BAKE_CLADE` defaults to 0, so a
bare `build.sh kernel` produced a pool with no `/clade` — and a CL-6 gate
ran 40 boots in which clangd was simply absent (the probe skips an absent
server by design) and still reported 40/40 PASS. "A gate that cannot see the
feature reports success identically to one that verified it." The gate now
defaults the bake on when the tree is configured for it, PRINTS what it
chose, and then checks the produced `pool.img` exists and is ≥ 3 GiB.

**A timeout is a ceiling, not a sleep.** Boots exit early on banner or
extinction (0.1 s poll); only a genuine wedge waits it out. The budgets are
sized for the go4c-enforcing boot — the pre-#362 90/120 s values timed out
HEALTHY boots and produced a 10/10 false-OTHER band.

**Per-boot fixture restore.** Each boot's go4c probes write GOCACHE/`$WORK`
into the pool with ~6× CoW amplification, so N cumulative boots age the
fixture: later boots drift toward the timeout and a long matrix would
eventually ENOSPC into false reds. Every boot starts from the baked snapshot
(`cp -c`, an APFS clonefile), which also makes per-boot timing comparable.
The **key twin is validated coherent first** — the ramfs bakes the key, so
only the pool matching the live key may be restored.

**The guest serial log is not the whole story.** `$LOG` is guest serial
only; a post-banner verdict step (the console gate, the liveness compare)
that fails leaves no trace there. The 2026-07-19 ubsan-smp8 OTHER was
undiagnosable because the harness stream went to `/dev/null`. Both streams
are now captured on every non-PASS.

**The v8.0 floor guard runs two independent checks because they fail
differently.** SOURCE greps every tracked build input for a `-march` above
the floor: cheap, exact, names the file — but sees only inputs it knows to
look for. BINARIES disassembles what shipped: this is the one that matters,
and #71's postmortem says why — `tools/pouch-clang` was not in the first
enumeration (cmake + build.sh + cargo); it was found only because measuring
the output left two ungated instructions in a shipped binary. **"Enumerating
the files you expect is not the same as measuring what shipped."** Three
more `-march` sites have appeared since. `--all` extends the binary scan to the
big pool payloads -- `/clade`, `/goroot`, and since the WebKit arc `/webkit`'s
stage -- which is where the ~6-minute cost comes from.

The gating rule is structural, not symbol-based: an LSE is gated iff a
nearby preceding feature-byte load pairs with a conditional branch whose
target lies past it. That one rule covers both producers (compiler-rt
outline-atomics and the Go runtime) uniformly, and it has to be structural
because the shipped clade toolchain is fully STRIPPED — there are no symbol
names to allowlist.

**The console gate grew a second layer when the first proved blind.** G-4
verifies the Aurora console statistically + exactly (Bonfire bg dominant
≥40%, exact default-fg text ≥200 px) plus a liveness retry-compare — two
dumps must eventually differ, since the 1 Hz cursor blink guarantees change.
But exact bg/fg counts are structurally blind to ANTIALIASED EDGE pixels,
which is exactly where the #35 packed-lane blend bug lived: glyph cores
stayed exact via the `a=255` short-circuit while edge channels scattered, so
G-4 passed a violet-fringed screen. G-5 added the blend-integrity pass —
every pixel 8-adjacent to an exact-fg core must lie inside the per-channel
`[bg,fg]` convex envelope.

**A hardcoded socket path is a serialization constraint wearing a default's
clothes.** `screendump.sh` addressed the QMP control socket at a fixed
`build/qmp.sock`, so two boots in one tree could not run at once — not because
anything forbade it, but because they would have addressed the same file. G-2
made it `THYLACINE_QMP_SOCK`-overridable, and that one-line change is what
unlocked running N interactive scenarios at a time. The same family as the
fixed-host-port trap: **the constraint was invisible precisely because nothing
enforced it**; it only surfaced as a collision when someone tried the
concurrency the tool never said it lacked.

**The lean `--production` shape is built inside the gate loop, not by a target
nobody runs (#228/#229).** #228's root cause was not that `--production` was
broken but that NOTHING BUILT IT, so it stayed broken silently for weeks while
every gate stayed green; a Makefile target does not fix that, building it in the
loop that actually runs does (~2 s, into a scratch dir). It SKIPs — reported as
NOT coverage, the #212 discipline — when `build/generated` is absent (a
kernel-only iteration legitimately has no userspace headers). #230 then made the
warden UNCONDITIONAL (it had been probe-gated, so the lean image had no drivers
at all — that was the defect, not the shape), so the lean image renders a console
and the G-4 gate APPLIES to it; only `debug-probe` stays lean-skipped, keyed on
joey's `ARC-GATES not-built` report — the build SHAPE, never the absence of the
thing a gate looks for, since that absence is also what the corresponding
regression looks like. The G-4 gate also moved to a SECOND QMP monitor
(`THYLACINE_QMP_SOCK2`, #230): one chardev serves one client and the key-injector
holds the first for the whole boot whenever its sentinel never arrives, so a
shared socket made the two consumers ordering-dependent.

**The DISTRO / CLADE arc gates are propagated into the exit status (#212/#232).**
`check-arc-gates.sh` answers for the D-5 / L-6c arc chain and the three clade
gates; both soft-skip when their external Alpine/clade bundle is absent (right on
a fresh clone), but nothing carried the skip into the exit status, so a tree with
a REGRESSED D-1..D-4 chain exited 0 identically to one where the arc ran. The
checker now FAILS on an ABSENT report (a dropped gate — the shape a green boot
hides) and otherwise states what ran; `THYLA_ARC_GATES=require` /
`THYLA_CLADE_GATES=require` make a skip fatal for shapes that ship the fixture.

**netd's boot selftests reach the exit status (2026-09-24).** netd prints a
`netd: <name> PASS/FAIL` line per selftest, and no gate read them. A netd whose
selftest regressed booted green. The TCP-retirement FAIL of 2026-09-21
(`20f73f27`, one boot in ~85) was noticed only because an extinction brought
the log under review. Once netd has started (`netd: up mac=`), the verdict fails if netd prints any
`netd: .*FAIL` line, never serves `/net`, or omits `netd: dial-verdict selftest
PASS` by name (an absent line passes a no-FAIL check). It keys on netd STARTING,
never on serving. Two selftests (resident lo, TCP retirement) end netd before it
posts `/net`, joey treats a missing `/net` as non-fatal, and the banner still
prints, so a check keyed on serving would skip exactly the deterministic
failure. [[seam-242-selftest-nonfatal]] (the other selftests proceed after a
FAIL) is narrowed by this capture, not closed.

### The host tests (`test-rust.sh`)

**No gate ran `cargo test` until 2026-09-22**, so the largest body of tests in
userspace ran only when someone transcribed a command out of a `Cargo.toml`
comment. `test-rust.sh` runs every workspace crate's lib tests on the host --
`cargo test -p <crate> --lib --no-default-features --target <host>`, the triple
read from `rustc -vV`, the crate list from `cargo metadata` so a new crate is
picked up without an edit -- and puts each crate in ONE of five buckets: PASS,
FAIL, NO-LIB (bin-only: every probe, smoke and bench), NO-HOST (links
`libthyla-rs` unconditionally, whose `_start` asm is ELF-only), NO-TESTS. Only
FAIL fails. The five exist because a two-way split was red on its first run: the
first draft called all 93 bin-only crates FAIL.

**STRANDED is derived from the crate, never from its bucket -- and that is a
correction.** Per crate it is the `#[test]`s its `src/` declares minus the tests
that compiled into a pass (passed or ignored). The first version counted NO-HOST
crates only, so the fix that made `libutopia` host-testable moved it into PASS
and took its remaining 91 stranded tests out of the report with it: the summary
said "nothing is stranded" while 101 tests in three crates ran nowhere
(2026-09-23). **A debt reported by CATEGORY disappears the moment a fix changes
the category.** Derived per crate, a PASS crate strands whatever
`--no-default-features` compiles out (libutopia's `backend`-gated modules), a
NO-LIB crate strands every test it declares (aurora's, inside a `no_main`
binary), and a NO-HOST crate strands all of its own.

**The declared count is anchored at the start of a line**, because a `#[test]`
that a comment mentions is not a test -- libhalcyon's `theme.rs` has one, and the
unanchored count called it stranded.

**Every figure comes from parsed test NAMES, and the parse proves itself first.**
Per pass, the names parsed must equal cargo's own passed + ignored, or the crate
FAILs: a parse that silently matched nothing would call every test stranded, and
one that matched some would print a plausible number that is wrong. A name twice
in ONE pass also FAILs, because it is a test registered twice -- and the first
full run found one: `libutopia`'s
`equal_is_assignment_at_statement_start_and_literal_after_a_word` carried two
`#[test]` attributes, so libtest ran it twice and cargo's count (313) was one
more than the crate's distinct tests (312). A count summed from cargo's footer
could never have seen that; a count of distinct names made it a one-line diff.

**`FEATURE_PASSES` decides only what RUNS.** `cornucopia` tests its `scale`
bakes only with the feature and their absence only without it, so any single
pass strands one of the pair. A `crate:features` entry adds a pass, and a test
counts as run if it compiled in any of its crate's passes, by name. The table is
the one name-keyed part of the script and deliberately cannot hide anything: the
stranded figure is derived regardless, so a crate missing from it shows as
stranded, never as quietly passing.

**Compiler warnings are counted per crate and printed, never fatal.** rustc
warned `duplicate_macro_attributes` at that doubled `#[test]` on every build for
a day; the gate keeps each crate's log and prints it only on a FAIL, so the one
line that named the defect was swallowed. A gate that fails on warnings gets
bypassed, and one that swallows them hides the warning that matters. The count
is taken from cargo's per-crate footer, so a dependency's warnings are not
charged to the crate that happened to rebuild it.

## Data structures

None persistent. `build/multiboot-fails/` accumulates captured logs. A label's
prior captures are **ARCHIVED, not deleted** (#223): moved to
`archive/$LABEL-<timestamp>/` at the start of each run, per label (not
whole-dir). Deleting them was sound for the masquerade hazard — a stale fail log
from a since-fixed run must not read as a current finding — but the standard
response to a RARE failure is to re-run that same label in isolation, so a
delete-on-start makes the diagnostic act destroy the only copy of what you were
diagnosing (it cost the #200 sighting-2 harness log, which now survives only as a
commit-message quotation). A move is as effective against the masquerade and
costs a rename. Each failing class writes BOTH streams
(`$LABEL-$i-<CLASS>[-<token>].log` guest serial, `-harness.log` the harness side,
the token being `test.sh`'s result), and arm-1 EXTERNAL-KILL appends its resolved
sender record to the harness capture.

## Concurrency

The matrix is sequential by construction. Two worktrees can gate
concurrently — which is what makes reap-scoping load-bearing
([[sub-substrate-interactive]]). The interactive gate runs N scenarios at a
time within one tree since G-2, which is what the per-scenario QMP socket
override exists for.

## Invariants enforced

None of §28 directly. These gates are how several §28 invariants are
*evidenced*: [[inv-i9]] and [[inv-i21]]'s SMP claims rest on the multi-boot
record, and I-12's W^X holds at build time but the v8.0 floor is what keeps
the shipped userspace runnable on the baseline core.

## Error paths

`test.sh` distinguishes pass / gpu-gate / extinction / qemu-exit / timeout,
each with a targeted log slice on stderr. `ci-smp-gate.sh` exits 2 on an
unknown config label rather than silently running the full matrix.

## Performance

A default boot is ~95–110 s (two real on-device go builds ride every boot);
UBSan is ~150–300 s. A full N=10 four-config matrix is tens of minutes to
hours — "that cost IS the gate." The full matrix sits at the 600 s Bash
ceiling, so it is run as subsets via `SMP_GATE_CONFIGS`.

`test-rust.sh` is ~5 minutes for the whole tree, and most of that is `manual`'s
two bounds tests: 101 s + 42 s in the unoptimized profile `cargo test` uses,
~16 s + ~7 s with `--release` (measured 2026-09-23). They are exhaustive by
design -- 42 worst-case shapes of a 1 MiB section at up to five tier/width
settings, under the guest's first-fit allocator -- and the second one asserts the
parser stays linear. They look like a hang (one test binary at 100% CPU for
minutes) and are not one.

## Prosecution

- Any new classification bucket must be anchored on text the guest EMITS on
  failure, never on a token present in healthy logs. The #362 regression is
  the template.
- A new "benign" class must not be reachable without a positive green-guest
  proof; INJECT-MISS's five conjuncts are the bar.
- **A new class needs its LADDER POSITION argued in both directions** — what
  it must not mask (the rows above) and what must not absorb it (the rows
  below). #88 states both; a class inserted without that argument is a
  reordering of every verdict, not an addition to them.
- **Validate a log pattern against a specimen you generate NOW**, from the
  binary actually in the loop — never against a captured incident. QEMU's kill
  message gained a suffix between the incident and the fix; a pattern fitted to
  the capture would have matched history and nothing else.
- **Capture volatile forensics at DETECTION, not at report.** A sender pid is
  resolvable for seconds and unresolvable by the time anyone reads the log.
- **A red class that fires routinely is as blind as a green one that
  over-matches.** When an explainable cause keeps landing in the catch-all,
  the fix is a new honest label, not a higher bar — the operator has already
  begun discounting the verdict either way.
- Adding a feature to the bake requires adding its verification to the
  chokepoint, or the gate silently stops seeing it (#101).
- `check-v80-floor.py --binaries` must NOT be pointed at the kernel ELF: the
  W1.5 boot patcher rewrites LL/SC into LSE in place, so kernel LSE lives in
  `.altinstr_replacement` with no branch before it and the checker would
  correctly call it ungated.
- A revert-probe is the only proof a gate is live. Every gate here that
  failed did so by passing.
- **Derive a per-item figure from the item, never from the category it was
  sorted into.** `test-rust.sh`'s STRANDED counted a bucket, so the fix that
  moved a crate out of that bucket deleted its debt from the report.
- **A gate that parses a tool's output must reconcile the parse with the tool's
  own count before deriving anything from it** -- and a count of DISTINCT items
  sees what a summed total cannot (the doubled `#[test]`).
- `test-rust.sh`'s six mechanisms were each sabotage-verified on 2026-09-23 --
  a doubled `#[test]` FAILs naming it; a broken name parse FAILs "parsed 0,
  cargo counted 38"; dropping the `FEATURE_PASSES` entry strands cornucopia's
  twin; a `#[cfg(any())]` test reads as 1 STRANDED; an unanchored count strands
  libhalcyon's comment; a planted warning is counted. A change to any of them
  re-owes its sabotage.

## Seams

[[seam-70-tcg-watchpoint]] · [[seam-791-smp1-joey]] ·
[[seam-87-disk-write-proof]].

## Caveats

- `test.sh`'s inline comment on boot-time variance is a genuine, *measured*
  host attribution — the bimodal idle distribution (~19–26 s vs ~33–37 s
  with a clean gap) was proven to be macOS placing TCG vCPU threads across
  P-cores vs E-cores, with `-smp 1` → 0.39 s spread and `taskpolicy -b` →
  170–220 s as the controls. This is what the "no host load" discipline
  actually demands: measure it, or do not claim it.
- The gate's own note [[gate-smp]] described two classes; the code had four
  at the first sweep and has five now. **Both times the count drifted the same
  direction** — the code grew a class and the prose describing it did not — and
  the second drift happened while this dossier was the thing describing it.
  A class count is a fact with no owner: adding one is a change to the
  classifier, and updating everything that states the total is nobody's step.
- The #88 incident's original sender **was never identified**. The two known
  QEMU-killers in the tree were checked and falsified. The classify-time `ps`
  exists precisely so a recurrence names it — the class is a standing trap, not
  a closed case.
- **Every boot's wall clock is recorded, not just failures' (#200).** The open
  question on the SIGKILL sightings is whether the UBSan asymmetry is a sanitizer
  effect or an exposure-time one — a host-side killer cannot care which sanitizer
  built the guest, but a UBSan boot runs longer and so presents a proportionally
  larger kill window. A per-unit-time hazard rate needs these numbers from
  ordinary gate runs, so they ride every boot line (`(Ns)`) rather than a bespoke
  experiment nobody re-runs.
- **`test-rust.sh`'s STRANDED figure is informational**: a newly stranded test
  raises the printed number and does not fail the gate. The tree carries 100
  (libutopia 91, aurora 9) at 2026-09-23. Only a FAIL is red.
- **Tests outside `src/` are not counted, and there are none**: measured
  2026-09-23, zero `.rs` files outside `src/` across the 123 workspace crates,
  against 393 inside. A crate that grows a `tests/` directory would strand it
  silently -- `--lib` never runs it and the declared count never sees it.

## Provenance

[[chg-2026-08-01-substrate-sweep]]; [[chg-2026-08-16-gates-external-kill]] the
fifth class and the G-2 socket override; [[chg-2026-09-06-substrate-gates-detectors]]
the second EXTERNAL-KILL arm (#222), the sourceable classifier + producer
cross-check (#234/#212), archive-not-delete (#223), and the lean-shape / arc-gate
propagation (#228/#229/#230/#232). 2026-09-23: `test-rust.sh` ADOPTED -- it
shipped 2026-09-22 (`9e837771`) with no dossier -- with the per-crate STRANDED
derivation, the name parse's two self-checks and the warning count; and the two
gate changes of 2026-09-22 that landed without a dossier update recorded here:
main's `default-smp1` row (`6e1cda16`, the loom join) and the `/webkit` floor
path (`b70e1bfd`). 2026-09-24: the netd selftest verdict joins the exit status.
