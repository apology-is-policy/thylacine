---
id: gate-interactive
type: gate
title: "LS-CI — the interactive console E2E net"
proves: "That a real terminal driven into the live console produces the expected rendered output: login succeeds (every keystroke was received), command stdout/stderr reach the screen, and the ~30 scenario set covers the LS + graphics surfaces. It is the ONLY harness that can type -- CI's piped stdin EOFs the chardev, so no other gate delivers a keystroke."
blind-to: "Anything HVF-specific (it defaults to TCG -- a DIFFERENT CPU from test.sh's `-cpu host`, so LSE-present behavior and HVF timing are invisible); anything requiring an optional host artifact (those SKIP, and a SKIP is not coverage); the legs after a cut (an INFRA or HARNESS failure LOSES coverage -- the gate goes red but the unrun legs proved nothing); anything below the rendered-text layer (it asserts what the screen shows, not why)."
invocation: "tools/test-interactive.sh [scenario...] (make test-interactive). Optional gate: SKIPs exit-0 without `expect`. Env: THYLACINE_ACCEL, LS_CI_BOOT_TIMEOUT (300 with GOROOT baked, else 180), LS_CI_CMD_TIMEOUT (30), LS_CI_ATTEMPTS (3), LS_CI_POOL_RESTORE."
created: 2026-08-01
updated: 2026-08-01
---
## Method

Boot under QEMU, bridge `-serial mon:stdio` through `serial-bridge.py` into
`expect` (itself run under `script(1)` for a controlling PTY), log in as a
seeded user, and assert on rendered command OUTPUT — never on typed input,
which the `ut` line editor redraws per keystroke and makes unmatchable.
Fixtures are restored from pristine twins per ATTEMPT. A host-only relay
differential runs as a preflight before anything boots.

## Classification rules

Four outcomes; three of them are not the guest, and two of those still make
the gate RED because coverage was lost:

- **guest FAIL** — all attempts failed with no harness fingerprint. A real
  regression: a genuine break fails every attempt deterministically.
- **INFRA-FAIL** (red) — the VM never started; QEMU's own refusal is
  recorded under an `INFRA:` marker.
- **HARNESS-FAIL** (red) — EVERY attempt was cut by the relay losing its
  reader while the VM was ALIVE (#60: `reason=stdout-broken` + a live
  `stat=`). Requiring every attempt to carry the fingerprint is what keeps
  this from failing open.
- **SKIP (77)** — the scenario declined for a missing optional host
  artifact. Not a guest result, and explicitly **not coverage**.

## History

Exists because LS-1 (UART never master-enabled for RX) and LS-2 (external
command output dropped) both shipped through a fully green suite — the
in-kernel harness structurally cannot type.

Its own corrections are the more instructive record. #72 retracted an
unmeasured claim that lost boots were "host timing"; ground truth (N=10,
instrumented) found the VM alive in all five losses and the relay dead of
SIGPIPE. #78 found the relay's blocking write was *causing* the drops it was
meant to prevent, by back-pressuring the guest into the kernel's TX
deadline. #85 found ~74 MB of accumulated pool contamination producing both
a false RED and a false GREEN. #59 found its own reap pattern shooting a
sibling worktree's live VM.

Caught, most recently: the #76 console-write interleave, where a login
prompt was shredded byte-for-byte by a concurrent writer on the trusted
path — visible only because something was reading the rendered screen.
