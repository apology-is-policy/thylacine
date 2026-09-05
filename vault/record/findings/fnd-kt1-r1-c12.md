---
id: fnd-kt1-r1-c12
type: fnd
title: "a lever-on image with no compositor (serial-only / THYLACINE_DISPLAY=none) has NO shell -- login spawns halcyond --session, it exits 1 after CONNECT_TRIES x 25 ms, login treats it as logout, getty re-prompts, forever"
round: adt-kt1-r1
severity: P3
status: deferred
surface: [sub-stratum-session]
threatens: []
regression: "seam-login-halcyond-fallback"
seam: seam-login-halcyond-fallback
created: 2026-09-05
---
## Prosecution

**File**: usr/login/src/main.rs:1270-1271, :1352-1353, usr/halcyond/src/session.rs:46-47, :348-371
**Prosecution**: `read_session_lever()` does not consult the display posture; `connect()` fails after 200 x 25 ms -> `run()` returns 1 -> `child.wait()` returns -> logout. The d-1a commit recorded "login-loop on session-halcyond failure (a fallback-to-ut is more robust)" as a consideration; it is not enqueued. **Fix**: fall back to the ut path when `/srv/tapestry` is absent or the session halcyond exits non-zero within N seconds of spawn.

## Disposition

Deferred: login treats halcyond's exit as logout regardless of status, so a lever-on image with no compositor re-prompts forever. The round-2 close removes the client-triggerable path into this loop (halcyond runs UNDECLARED instead of exiting on a refused declaration), but the no-compositor image still needs the fallback (halcyond exits non-zero within N seconds -> `ut` on /dev/cons). Owed at [[seam-login-halcyond-fallback]].
