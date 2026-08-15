---
id: sub-substrate-interactive
type: sub
parent: moc-substrate
title: "LS-CI — the only harness that can type, and its fault taxonomy"
code:
  - tools/test-interactive.sh
  - tools/interactive/lib.exp
  - tools/interactive/serial-bridge.py
  - tools/interactive/serial-listen.py
  - tools/interactive/test-serial-bridge.py
audit: none
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
abis: []
design: ["docs/LIFE-SUPPORT.md"]
created: 2026-08-01
updated: 2026-08-15
---
## Purpose

Boot Thylacine, drive a **real PTY** into `-serial mon:stdio`, log in, and
assert the rendered output. CI feeds QEMU a piped stdin, which hits EOF and
closes the chardev — so no keystroke is ever delivered, and two interactive
regressions shipped silently through a fully green suite: LS-1 (the UART was
never master-enabled for RX) and LS-2 (external command stdout/stderr were
dropped). This is the only harness that can catch that class.

## Contract

- `tools/test-interactive.sh [scenario...]` runs every `tools/interactive/*.exp`
  (or the named ones). Exit 0 iff no scenario failed.
- **Optional gate**: SKIPs (exit 0) when `expect` is absent.
- Default `THYLACINE_ACCEL=tcg` — the portable compat run, and a *different
  CPU* from `test.sh`'s HVF `-cpu host`. **Since G-3 it is only a default**:
  14 graphics scenarios override it to `hvf` in the `.exp` itself, and the
  timings table reports what actually **booted** rather than what this
  variable said.
- `LS_CI_JOBS` scenarios run at once (**3** by default — but see the
  caveats, the file says otherwise twice).
- Per-scenario bounded retry (`LS_CI_ATTEMPTS`, default 3); a scenario fails
  only if ALL attempts fail.
- Scenario exit 77 = SKIP (a missing optional host artifact).

## Mechanism

**`expect` must run under `script(1)`.** macOS expect 5.45 corrupts its own
std channels inside `spawn` when its stdout is not a tty — either a `>file`
redirect or a pipe. It aborts with `Tcl_RegisterChannel: duplicate channel
names` (SIGABRT) or breaks `puts` with `bad file number`. `script -q
<transcript> expect -f <scen>` gives it a controlling PTY, captures the
session, and propagates the exit code.

**`global spawn_id` in any proc that spawns.** `spawn` writes `spawn_id` in
the *current* scope; without the declaration in `lc_boot` the spawn is
proc-local and every later proc reports a spurious immediate EOF.

**Match command OUTPUT, never typed input.** The `ut` line editor redraws
the prompt on every keystroke via cursor positioning, so the typed line
never appears as contiguous bytes and is unmatchable. Scenarios assert on
output tokens that cannot appear in the input anyway — a `tr a-z A-Z`
upper-cased token, a `cat:` stderr prefix.

**The relay is `serial-bridge.py`, never `nc`, and it spools.** Two distinct
bugs wore one symptom here:

1. BSD `nc -U` dies of SIGPIPE under a full boot burst — measured 5 of 10
   boots lost, VM alive each time, `bridge exit=141`.
2. expect's default `match_max` is **2000 bytes** against a ~110 KB boot,
   forcing ~55 discard-and-rescan cycles; under that churn expect closes its
   read end mid-stream and the relay dies as a *consequence*. Swapping the
   relay alone left this at 2/10; `match_max 200000` took it to 0/10.

The relay must also never back-press the guest (#78). The original wrote
stdout *blocking*, on the theory that a full pipe back-pressures the socket
read and drops nothing. That reasoning was exactly inverted: a blocked
stdout write stops the relay draining QEMU's serial socket → QEMU's send
buffer fills → the guest UART TX ring fills → the guest **drops** the
remainder of its console write on the kernel's #75 TX deadline, silently
losing whatever token expect was waiting for. It now drains aggressively
into an in-process spool and writes out non-blocking. Proven by a host-only
differential with no QEMU: against a paused reader the blocking relay stalls
at ~80 KB, the spool relay accepts a full 4 MB burst.

**But non-blocking only protects while the relay is running — and a relay that
stops draining SUSPENDS the guest** (#125). This narrows the claim above: the
spool cannot drain a socket while it is off-CPU, and the socket itself holds
only **8192 bytes** on macOS against ~117–198 KiB of console output per boot,
so roughly 4% of one boot is the entire slack. Past that QEMU's serial write
blocks and **QEMU stops executing the guest at all** — measured by SIGSTOPping
the relay and sampling host CPU from outside QMP (QMP is served by the same
stalled QEMU, so it cannot be the instrument): 100% → **2.4%** within ~2 s,
held for the whole freeze, then 167% catching up.

The consequence is epistemic and belongs beside the #72 retraction: **a guest
that stopped making progress in an LS-CI log is not evidence of a guest defect
until the consumer has been exonerated.** From inside, a suspended guest and a
hung guest are indistinguishable. Read the relay's `stalls=` record first.

**And the obvious fix is vacuous, which is the durable lesson.** Capacity is
governed by the *writer's* send buffer, and the relay is the *reader* — setting
a receive buffer on the relay measurably changes nothing (8192 either way;
measured by writing until the writer blocks). What works is owning the
**listener**, because an accepted connection inherits its options: a small
wrapper creates the socket, sets the option before listening, and `exec`s
through to the VM, which takes it as an inherited descriptor. 8192 B → 8 MiB
(macOS clamps there; asking for 64 MiB still yields 8). It wraps the canonical
launcher rather than editing it, so the launcher stays byte-identical for the
other two gates and for manual boots. A/B through a real boot with nobody
reading for 60 s: **44221 B and no login** — byte-identical to its own 12 s
figure, so a hard stop rather than slowness — against **128183 B and login
reached**. The regression measures capacity directly, in about two seconds,
with no VM.

**That differential runs as a preflight, before anything boots.** The
relay's two load-bearing properties are provable without a VM, and left
unrun they rot — "the exit-record check exists precisely because
`stdout-broken` was read as a diagnosis for three sessions of #78." It
hard-fails; a broken relay would otherwise surface as a mysterious guest
failure in every scenario.

**Evidence before the kill, and the `ps` state is the discriminator.**
`lc_fail` records `vm-at-fail: pid N stat=<S>` *before* killing anything.
The state splits "qemu exited" (`Z*` — Tcl lazy-reaps, so a dead child
lingers as a zombie and `kill -0` would **lie**) from "the relay died under
a LIVE VM" (`R`/`S`/`U`). Without it both surface at the expect layer as one
indistinguishable EOF — which is how the bridge-death class hid behind a
"qemu exited before login prompt" message for so long. The bridge's own exit
record lands beside it as `bridge-at-fail`, and its `reason=` field
separates `stdout-broken` (reader closed) from `socket-eof` (guest gone) —
"the difference between chasing the relay and chasing expect."

**Four outcomes, and three of them are not the guest.**

| Outcome | Counts as | Meaning |
|---|---|---|
| guest FAIL | red | all attempts failed, no harness fingerprint — a real regression |
| INFRA-FAIL | red | the VM never started; QEMU's own words recorded under `INFRA:` |
| HARNESS-FAIL | red | every attempt cut by the relay losing its reader while the VM was ALIVE (#60) |
| SKIP (77) | not a result | the scenario declined; retrying cannot change it |

INFRA and HARNESS still make the gate **RED**: "the scenario's remaining
legs never ran, so coverage was LOST. It is only attributed honestly."
Requiring EVERY attempt to carry the fingerprint is what keeps HARNESS from
failing open — one timeout, EXTINCTION, or genuine qemu exit among the
attempts drops it back to the real-regression branch.

**A retry is a tolerance, never a diagnosis (#72).** This block once
justified itself by asserting that an unexpected qemu exit before a terminal
verdict "is a host-timing artifact — TCG-under-oversubscription, never a
kernel fault." That was wrong and never measured. Ground truth, N=10
instrumented: 5 of 10 boots lost, and in **all five** the VM was still alive
while the relay had died of SIGPIPE. It was never a qemu exit at all. The
retry survives as belt-and-braces only.

**Failed-attempt evidence survives the retry.** The per-attempt truncation
used to destroy the very transcript a retry was retrying over, so a
"flake" claim could never be checked against its own evidence — the
no-host-load discipline needs the artifact to look at. Each failed attempt
is archived as `ls-ci-<name>.attempt<N>.{log,steps}`, cleared per SCENARIO,
not per attempt.

**Fixtures are restored per ATTEMPT (#85).** Every scenario boots against
the same `pool.img` and mutates it, and nothing reset it — measured
contamination at the time: **73,911,951 bytes**. It failed in both
directions: a failed `ls-gfx-mode` left `mode 1600 900` in the config and
every later boot inherited a 1600×900 display (false RED, blamed on a merge
for a day); and a `config.cfg` written by some earlier run satisfied
`ls-gfx-play`'s assertion trivially, so that leg kept passing for five days
after the path it asserted stopped being written (false GREEN — strictly
worse). The scope is the ATTEMPT because a failed attempt's mutations must
not poison its own retry; scenarios that deliberately persist state across
boots do so INSIDE one attempt and are untouched by construction. The
restore is strictly AFTER the reap and settle — "overwriting an image out
from under a live VM is how you manufacture the corruption this gate exists
to detect."

**The restores fail CLOSED, with one deliberate exception.** A restore that
fails part-way leaves a truncated fixture, and booting on it would surface
as guest corruption — the fail-open shape where the harness's own fault gets
read as a Thylacine defect. So a failed copy is FATAL. A *missing python3*
is different and degrades silently to the shared fixture: that is "merely
the old behaviour, not an unknown one."

**Reaping is scoped to this tree's build dir.** `pkill -9 -f
"qemu-system-aarch64.*$BUILD_DIR/"`. The old pattern
(`qemu-system-aarch64.*thylacine`) matched every thylacine worktree, so two
sessions gating concurrently shot each other's live VMs — "qemu GONE, guest
healthy" mid-scenario failures on both sides (#59). The intra-tree half of
that same bug is closed by refusal rather than by narrowing; see
Concurrency.

**Accel is not a speed knob, and G-3's anchor set is the best structure in
the file.** `run-vm.sh` derives the CPU model *and* the GIC version from
it: hvf gives `-cpu host` + GICv2, tcg gives `-cpu max` + GICv3 (HVF cannot
do v3 here at all — its emulated GICv3 distributor trips an `isv`
data-abort assert). So every scenario moved to HVF stops covering GICv3 and
`-cpu max`, and #166 is the standing proof that a scenario can go **inert
under HVF while still reporting PASS** — the worst outcome available, a
green test that quietly stopped testing.

Seven scenarios therefore stay on TCG, and the list is **mechanical rather
than "whatever nobody got round to flipping"** — each has a recorded reason
and several are the *only* remaining coverage of an open bug: two whose
trigger IS TCG's serialized vCPU, one pinning tcg for a deterministic
split, one that breaks under HVF outright, the tickless-idle guard (whose
HVF side is covered by a separate gate), a TCG-only watchpoint livelock
whose regression would otherwise be retired, and a bug that reproduces only
under TCG gate load. Together they keep GICv3 and `-cpu max` live in every
run.

**And it is enforced, not documented**: an hvf directive inside an anchor's
`.exp` is *refused* with a message naming the coverage it would drop,
never silently honoured. The comment states the principle exactly — "a
coverage rule that depends on nobody editing the wrong file is not a rule."
Read the caveats for where that principle is not applied.

**Timings are recorded per scenario and per attempt (G-1).** A TSV plus a
sorted on-screen summary, and the accel column is read out of the boot
artifact rather than taken from the harness's own environment — because 14
scenarios override it, and a timings table whose accel column is wrong is
*worse* than one with no accel column, since it invites the tcg-vs-hvf
comparison it exists to prevent. The instrument came first on purpose: G-2
needed a per-scenario cost to pack slots and a before/after to prove it
gained anything, and G-3 allocates a scarce riskier resource **by** cost.

## Data structures

Per scenario: a `.log` transcript (full PTY session) and a `.steps` file
(the flush-immune live view, where `lc_step` writes the `INFRA:`,
`vm-at-fail:` and `bridge-at-fail:` markers the wrapper keys on).

## Concurrency

~~One VM at a time within a run.~~ **Three, by default, since G-2/G-3** —
the gate now runs `LS_CI_JOBS` scenarios at once, and every shared thing it
touched became per-slot to allow it: the pool fixture, the QMP socket, the
reaper. Measured on the full set: 4908 s serial → 2925 s wall.

**Concurrency here is RAM-bound, not core-bound.** Each VM takes
`THYLACINE_MEM_MIB`, so ~3 is the honest ceiling on an 8 GB host regardless
of its 8 cores. Overcommitting swaps, and a swapping host "makes every
timeout in the suite marginal — which would then get read as guest
flakiness."

**Budgets scale with the job count, and the reasoning is the fail-open
one.** Under contention the same *healthy* guest is legitimately slower —
each VM gets 4 vCPUs, so N VMs oversubscribe the host and TCG is CPU-bound.
Measured: three scenarios that take ~190 s serially take ~400 s each,
blowing a fixed 300 s budget while their logs show the guest still counting
up the boot ladder — a harness fault reported as a guest regression. The
boot timeout exists to catch a **wedge**, not to enforce an SLA, so erring
generous is the safe direction: an over-large budget only delays declaring
a wedged guest dead, while an undersized one reports failures that do not
exist. An explicitly pinned budget always wins; the scaling touches only
the default, and whether the caller pinned it is recorded *before* the
defaults make that unknowable.

**Two reapers, and the second exists because `bash` surprised someone.**
The tree-wide reaper is correct serially and catastrophic in parallel — a
scenario finishing its first boot would shoot down every neighbour. So each
forked scenario re-traps to its own slot before doing anything, and the
reason is **measured, not assumed**: bash resets *signal* traps in a
subshell but the `EXIT` trap still fires when that subshell exits, so a
forked scenario inherits the tree-wide reaper and runs it on the way out.
That is #59's cross-tree shootout reproduced *inside* one tree, presenting
as "qemu GONE, guest healthy" — indistinguishable from a guest fault, and
exactly the shape this project keeps mistaking for load.

**In-tree concurrency with any other VM is refused up front (#217).** The
`EXIT` trap is still tree-wide, so a VM this script never started — an SMP
gate, a `test.sh` boot, a manual run — would die uncatchably on the way
out, its log simply stopping: the misread-as-flake shape again. Refusal
beats narrowing, because in-tree concurrency is unsafe for a second reason
anyway (both gates restore the same `pool.img`, and a restore under a live
VM manufactures exactly the corruption the gates exist to detect), so a
named operator error beats a silent mutual-corruption race.

**The check must precede the trap install**, and that ordering is a scar:
install-then-refuse fires the reaper on the refusal's own exit, killing the
VM it just declined to disturb.

## Invariants enforced

None of §28. It is the *only* evidence for the interactive half of I-27's
trusted path (no registry note yet; the console surface is unswept) and the
LS-5/LS-8 console behavior — properties no in-kernel test can reach, because
the in-kernel harness cannot type either.

## Error paths

Every non-pass prints the steps file, a transcript tail, and the path to
every attempt's preserved evidence. The summary line breaks the count into
guest / INFRA / HARNESS and states plainly that only the guest failures say
anything about Thylacine.

## Performance

TCG boot ≤300 s with the GOROOT baked (≤180 s without); ~30 scenarios. The
per-attempt fixture restore costs ~30 ms measured on the real shape (2.5 GB
image, ~70 MiB divergence, over an EXISTING destination) — note the ~2 ms
figure is for a clone to a FRESH path and does not apply here. ≤96 restores
per full gate ≈ 3 s against a run measured in tens of minutes.

## Prosecution

- A new scenario must match output, not input; must not assume the relay
  delivers a burst atomically; and must raise `match_max` before any large
  expect.
- A new failure attribution must require the fingerprint on EVERY attempt,
  or it fails open.
- Any new shared mutable fixture needs a pristine twin and a per-attempt
  restore, or it will disarm some proof — the two directions of #85 are the
  template, and #87 is the one still open.
- The preflight must stay ahead of the first boot. A harness that fails open
  is the #74 lesson.
- The console socket must be created by us, not by QEMU. The widening is
  inherited from the listener, so anything that goes back to letting QEMU
  create it silently restores an 8 KiB budget — and the symptom is a guest
  that looks hung.
- The shutdown path must be verified **positively** (`EOF clean` in the steps
  file). A dead monitor still PASSes: expect simply times out and the wrapper
  reaps QEMU, so absence of a complaint proves nothing here.
- A forked scenario must re-trap to its own slot before doing anything. The
  inherited `EXIT` trap is tree-wide, and a scenario that runs it takes down
  every neighbour — presenting as a guest fault.
- Moving a scenario to HVF drops GICv3 and `-cpu max` coverage, and #166
  proves a scenario can go **inert under HVF and still report PASS**. The
  anchor gate refuses this mechanically; removing a name from the anchor
  list must say why in the same commit.
- A timing without its accel is unusable. Read the accel from the boot
  artifact, never from the harness's environment — 14 scenarios override it.

## Seams

[[seam-87-disk-write-proof]] · [[seam-expect-channel-close]].

## Caveats

- `disk.img` restore is a MITIGATION, not the fix: it makes every LS-CI boot
  a "boot 1" so the write-proof can fail again here, but `test.sh` and
  `smp-multiboot.sh` share the same fixture and stay exposed
  ([[seam-87-disk-write-proof]]).
- The residual `reason=stdout-broken` with the guest `R+` during small
  post-login output under heavy host load is macOS expect 5.45 closing its
  channel spuriously — not the relay and not the guest (the old relay
  reproduces it identically). `match_max` narrowed it; it is not eradicated.
- The absorbed `09-test-harness.md` numbered this section's "Four
  portability facts are load-bearing" and then listed **six** — the list grew
  and its header did not.

- **The job-count default is stated three times and two are wrong — in the
  file that argues a rule nothing enforces is not a rule.** The usage block
  says *"(default 1)"*; the code is `${LS_CI_JOBS:-3}`; and a third copy
  three hundred lines down still says *"the default stays 1 until a
  parallel run has been proven green."* G-2 landed all three consistent at
  1; G-3 flipped the code and updated neither prose copy — so the surviving
  policy sentence names a precondition as unmet, three hundred lines below
  the comment citing the very run that met it.

  Not cosmetic, because the parallel branch changes behaviour a reader
  would not go looking for: the boot budget an unconfigured run actually
  gets is 900 s rather than 300, the command budget 90 rather than 30, and
  three VMs start on a host the reader believes is running one. The file
  itself notes that a swapping host "makes every timeout in the suite
  marginal — which would then get read as guest flakiness," which is the
  failure this dossier exists to keep legible. Task #183.

  The fix is to derive rather than restate. The pointed part is that the
  anchor gate 300 lines above already knows this: *"Enforced, not
  documented… a coverage rule that depends on nobody editing the wrong file
  is not a rule."* Right principle, not applied to its own neighbour.

  Method note for anyone re-checking: `git log -S 'LS_CI_JOBS:-'` does
  **not** find the flip. `-S` counts occurrences of the string, and
  `:-1` → `:-3` leaves the prefix count unchanged, so the commit that made
  the change is invisible to the search most likely to be reached for.

## Provenance

[[chg-2026-08-01-substrate-sweep]].
