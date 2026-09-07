---
id: chg-2026-09-07-login-console-fallback
type: chg
title: "login degrades to the console shell when the session compositor cannot start -- closes seam-login-halcyond-fallback"
date: 2026-09-07
arc: arc-vault
commits: ["70490e20"]
touched:
  - sub-stratum-session
established: []
closed:
  - seam-login-halcyond-fallback
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
main @36cb83d8 ("login: fall back to the console ut shell on session-compositor
failure") closes [[seam-login-halcyond-fallback]], the KT-1 round-1 C-F12 debt.

## The lock-out

A session-lever image (`/lib/halcyon/session == "on"`) booted console-mode was a
real seat lock-out. `THYLACINE_DISPLAY=console` drops the GPU stack, so tapestryd
never starts, but the pool still carries the lever, so login spawns
`halcyond --session`, which finds no compositor and exits 1. Pre-fix login
treated that exit as a clean logout ("its exit IS logout, regardless of status")
and returned -- so joey's getty respawned login, which re-ran halcyond, which
failed identically: an endless login loop with NO usable shell on any box where
the compositor cannot come up (console-mode, headless, a GPU that never binds).
Not a dev-config quirk -- a real lock-out until reboot.

## The fix, and where it beat the seam's plan

login distinguishes a bootstrap failure from a logout by the child's EXIT STATUS,
which is cleaner than the "halcyond exiting non-zero within N seconds of spawn"
timing window this seam predicted -- no race, no N to tune. `session_failed =
!status.success()` (a `wait()` error also counts as failed); on
`session_halcyon && session_failed`, login prints "login: session compositor
unavailable -- console shell fallback" and degrades to the console shell. The `ut`
path is unchanged (its exit is logout regardless of status); a CLEAN last-tile
logout (status 0) still returns to the getty prompt.

The fallback reuses the already-constructed `shell_cmd` -- the default no-lever
image's exact path, built with `CONSOLE_OWNER`, the consctl fd, `--home`, the
stamped user identity, and the masked `SHELL_CAPS` -- so it grants NO new
authority (in particular no `CAP_SET_IDENTITY` leak; it inherits the C-F1/C-F7
identity masking on the same dossier), and it enters the same `/home/<user>`
bind. `session_failed` covers both a connect-fail and a mid-session compositor
DEATH (both non-zero), which is why the fix already satisfies the run-40
prosecutor's F2 [P3] "a mid-session death should degrade too".

## Witness

Console-mode boot-verify (hvf): a session-lever image booted
`THYLACINE_DISPLAY=console` produced "login: session compositor (halcyond)
spawned" -> "halcyond: FAIL session connect" -> "login: session compositor
unavailable -- console shell fallback" -> the ut banner -> an echo round-trip,
with NO second "Thylacine login:" (no loop). ls-gfx-session (graphical)
unaffected -- a clean last-tile logout returns 0 and skips the fallback.

## Fold

sub-stratum-session (audit:hard) gains the "login degrades to the console shell"
subsection at the identity-masking site (the fallback's no-new-authority claim
rides that subsection's masking); its open [[seam-login-halcyond-fallback]]
reference is flipped to closed. `updated:` 09-05 -> 09-07 (clears the STALE).
