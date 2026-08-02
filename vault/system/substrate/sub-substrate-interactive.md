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
updated: 2026-08-02
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
  CPU* from `test.sh`'s HVF `-cpu host`.
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
healthy" mid-scenario failures on both sides (#59).

## Data structures

Per scenario: a `.log` transcript (full PTY session) and a `.steps` file
(the flush-immune live view, where `lc_step` writes the `INFRA:`,
`vm-at-fail:` and `bridge-at-fail:` markers the wrapper keys on).

## Concurrency

One VM at a time within a run; the residual expect-channel bug is
load-sensitive, so interactive gates want an otherwise-idle host.

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

## Provenance

[[chg-2026-08-01-substrate-sweep]].
