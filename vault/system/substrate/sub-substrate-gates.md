---
id: sub-substrate-gates
type: sub
parent: moc-substrate
title: "The gates — boot verdict, multi-boot classification, the v8.0 floor"
code:
  - tools/test.sh
  - tools/smp-multiboot.sh
  - tools/ci-smp-gate.sh
  - tools/check-v80-floor.py
  - tools/screendump.sh
audit: none
guarded-by: []
validated-by: [prose, gate-smp, gate-v80-floor]
locks: []
abis: [abi-boot-banner]
design: ["docs/TOOLING.md", "docs/PORTABILITY.md", "docs/DEBUGGING-PLAYBOOK.md"]
created: 2026-08-01
updated: 2026-08-16
---
## Purpose

The non-interactive verdicts. `test.sh` decides whether ONE boot passed;
`smp-multiboot.sh` decides what a failure MEANS; `ci-smp-gate.sh` decides
whether the matrix as a whole is clean; `check-v80-floor.py` decides whether
what shipped can run on the baseline CPU.

## Contract

- `test.sh` → exit 0 iff the boot reached [[abi-boot-banner]] with no
  extinction, and every enforced post-boot gate passed.
- `smp-multiboot.sh <label> <cpus> <N> [sanitizer]` → exit 0 iff
  0 CORRUPTION, 0 EXTERNAL-KILL **and** 0 OTHER across N boots.
- `ci-smp-gate.sh` → builds each kernel once, runs the matrix, aggregates.
- `check-v80-floor.py` → exit non-zero if any tracked build input asks above
  ARMv8.0-A, or any shipped userspace binary carries an ungated LSE.

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
timing), across four configs. UBSan-smp4 is the amplifier: on the broken
bringup it crashed 33–43% of boots and 0% of a single lucky one.

**The classifier has FIVE classes, and three of them fail the gate.** The
ladder is ordered, so each row is also "everything above it did not match".

| Class | Fails? | Anchored on |
|---|---|---|
| CORRUPTION | yes | exact extinction strings (invalid prev state, stack canary mismatch, kernel stack overflow, already on_cpu, #860, …) |
| EXTERNAL-KILL | **yes** | QEMU's own `terminating on signal N from pid M` (#88) |
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

**EXTERNAL-KILL is sound because of what CANNOT produce its evidence.** The
message names a signal and a sender pid, but that content is not the argument.
The argument is negative space: a guest cannot signal its own hypervisor, and
the harness's only kill is `kill -KILL` — **uncatchable, so QEMU prints
nothing for it.** The one killer that would make the line ambiguous is
structurally incapable of writing it. Existence of the line is therefore the
evidence; the signal number and pid are a bonus the classifier had been
discarding.

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
more `-march` sites have appeared since.

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

## Data structures

None persistent. `build/multiboot-fails/` accumulates captured logs, cleared
**per label** (not whole-dir) so a matrix running several labels
back-to-back does not wipe a sibling's evidence, while stale captures from a
since-fixed run cannot masquerade as current findings. Each failing class
writes BOTH streams (`$LABEL-$i-<CLASS>.log` guest serial, `-harness.log` the
harness side), and EXTERNAL-KILL appends its resolved sender record to the
harness capture.

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

## Provenance

[[chg-2026-08-01-substrate-sweep]]; [[chg-2026-08-16-gates-external-kill]] the
fifth class and the G-2 socket override.
