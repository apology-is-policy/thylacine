---
id: fnd-kt1-r1-c6
type: fnd
title: "the 'session-less display -> byte-identical console path' claim is false for every user-principal client, including the DEFAULT (lever-off) boot; the console becomes unreachable while any user graphical program is hosted, and the affected aux gates were not run"
round: adt-kt1-r1
severity: P2
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "ls-gfx-panes undeclared + declared controls"
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestryd/src/server.rs:199-201 (principal_is_session), :5782-5799 (has_session_tree over ALL hosted leaves), :5859-5872 (the forced focus transfer), usr/tapestryd/src/pane.rs:879-933 (neighbor_dir: `if !ok || ov == 0 { continue; }` -- a zero-rect leaf has ov == 0), :1136-1145 (tab_cycle skips bg), server.rs:6513-6516 (`focus` needs actor_hosts), :7273-7285 (click needs a hit)
**Invariant**: the d-1b/F2 commit messages and HALCYON.md 14.12 step 4 ("with no session leaf present (every pre-session + gfx-test path) nothing backgrounds ... byte-identical"); the checkpoint rule that a behavior change names the gates it moves.
**Prosecution**:
1. `principal_is_session(p)` is true for ANY real user principal (199-201). In the default boot login spawns `ut` as the user on /dev/cons; every graphical program that shell runs (tapestry-demo, DOSBox/SDL2, a Go GUI, `tapestry-battery` -- ls-gfx-panes.exp:75-87 logs in as michael and runs the battery from ut) mints surfaces with `owner_principal` = the user -> `has_session_tree` true -> aurora is backgrounded: zero rect, no FRAME, no CONFIGURE, focus forced off it (5859-5872).
2. Re-derivation of the claim: the set is empty only when NO user-principal surface is hosted anywhere in the tree. The F2 commit itself proves the gfx-test paths are not session-less ("Excluding aurora makes the battery's pane A WIDE ... the battery adaptation") and lists ls-gfx-panes/restore/session/chords + test.sh as run; the aux DX gates (ls-gfx-dosbox-*, ls-gfx-tombraider, the SDL/Quake captures) run a user-principal fullscreen client from the login shell and were NOT run. A pixel probe at a fixed display coordinate, or a layout assertion that expected the two-column tiling, is a regression candidate.
3. Reachability of the console while the app runs: Super+arrows -> `neighbor_dir` skips the zero-rect leaf (ov == 0, 921); Super+Tab -> `tab_cycle` skips bg (1142); a click cannot hit a zero-rect content (433-436); `focus <aurora-leaf>` from a user process -> `actor_hosts` false -> E_PERM (6515). The console is invisible, dormant and unfocusable until the LAST user-principal surface retires -- including a background job (`tapestry-demo &`) the user cannot reach a shell to kill from the GPU keyboard.
4. Observes: a DEFAULT-boot behavior change in the load-bearing fbcon path shipped under a byte-identity claim, with the consumer gates unrun. Whether "any user program outranks the console" is the intended reading of 14.12 step 4 (written for the per-user COMPOSITOR) is an operator decision; as built it is also a usability trap.
**Suggested fix**: run the aux DX gates before the batch push, and either narrow the trigger to a declared session compositor (e.g. a conn-level `session` declaration or a `role=session` on the bootstrap create, renderer-independent) or keep a chord/verb that can re-foreground the console. Correct the two commit claims in docs (139/150/JOURNAL).

## Disposition

Fixed in 062efe18: the display handoff is a DECLARATION -- the session compositor writes `session on` on its own ctl conn before its first surface (Session-principal-gated); `has_session_tree` keys on the declared conn hosting a leaf, so a user program that merely draws never backgrounds the console. Amended again in the round-2 close (C2-F1: the seat is the principal's; takeover; the undeclared fallback). Witness: ls-gfx-panes' undeclared control (the console leaf keeps a real column) + the declared control (the same leaf goes to zero on `session on`, returns on `session off`).
