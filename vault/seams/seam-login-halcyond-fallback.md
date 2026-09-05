---
id: seam-login-halcyond-fallback
type: seam
title: "a lever-on image with no compositor (or a halcyond that exits non-zero) has no shell: login re-prompts forever"
status: open
surface: [sub-stratum-session]
opened-by: fnd-kt1-r1-c12
tracker: "the KT-1 audit round 1 (C-F12); the d-1a deferral"
created: 2026-09-05
updated: 2026-09-05
---
## Owed

[[fnd-kt1-r1-c12]]: login treats the session compositor's exit as logout regardless of status, so on a lever-on image without a compositor (serial-only, THYLACINE_DISPLAY=none) halcyond exits 1 after its bounded connect and the getty loop re-prompts forever. The round-2 close removed the client-triggerable route into this loop (a refused declaration no longer exits halcyond), so what remains is the no-compositor image.

## What closes it

login distinguishes a bootstrap failure from a logout: halcyond exiting non-zero within N seconds of spawn (or a `/srv/tapestry` that is absent) falls back to `ut` on /dev/cons for that session, said on the console; a serial-only lever-on boot in the gate set proves the shell arrives.

## Risk while open

A misconfigured image (lever on, no display) locks the seat out of any shell until reboot. The lever ships OFF by default; ls-gfx-session bakes it on only with a compositor present.
