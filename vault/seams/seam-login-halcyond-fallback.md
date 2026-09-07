---
id: seam-login-halcyond-fallback
type: seam
title: "a lever-on image with no compositor (or a halcyond that exits non-zero) has no shell: login re-prompts forever"
status: closed
surface: [sub-stratum-session]
opened-by: fnd-kt1-r1-c12
closed-by: chg-2026-09-07-login-console-fallback
tracker: "the KT-1 audit round 1 (C-F12); the d-1a deferral"
created: 2026-09-05
updated: 2026-09-07
---
## Owed

[[fnd-kt1-r1-c12]]: login treats the session compositor's exit as logout regardless of status, so on a lever-on image without a compositor (serial-only, THYLACINE_DISPLAY=none) halcyond exits 1 after its bounded connect and the getty loop re-prompts forever. The round-2 close removed the client-triggerable route into this loop (a refused declaration no longer exits halcyond), so what remains is the no-compositor image.

## What closed it

[[chg-2026-09-07-login-console-fallback]] -- main @36cb83d8. login distinguishes a bootstrap failure from a logout by the child's EXIT STATUS (cleaner than the "within N seconds of spawn" timing window this seam predicted -- no race): `session_failed = !status.success()` (a `wait()` error also counts), and on `session_halcyon && session_failed` login prints "session compositor unavailable -- console shell fallback" and degrades to the pre-built console `shell_cmd` (`CONSOLE_OWNER` + consctl fd + `--home` + the stamped identity + the masked `SHELL_CAPS`, so no new authority) in the same `/home/<user>` bind. `session_failed` covers both connect-fail AND mid-session compositor death (the run-40 F2 P3). A clean last-tile logout (status 0) still returns to the getty prompt. Witnessed console-mode: the fallback line, then an interactive `ut` shell echo round-trip, no second login prompt.

## Risk while open

A misconfigured image (lever on, no display) locks the seat out of any shell until reboot. The lever ships OFF by default; ls-gfx-session bakes it on only with a compositor present.
