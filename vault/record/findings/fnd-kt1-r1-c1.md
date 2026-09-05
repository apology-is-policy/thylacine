---
id: fnd-kt1-r1-c1
type: fnd
title: "the per-user session compositor -- and every program in every session tile -- inherits login's CAP_SET_IDENTITY (cross-user identity escalation; the 14.12 'zero identity delegation' property is false by construction)"
round: adt-kt1-r1
severity: P0
status: fixed
surface: [sub-stratum-session, sub-tapestryd]
threatens: [inv-i22]
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "ls-gfx-session caps-probe leg (plain OK + identity REFUSED)"
created: 2026-09-05
---
## Prosecution

**File**: usr/login/src/main.rs:1316-1321 (the hal spawn), :1280-1287 (the ut spawn it was copied from), usr/lib/libthyla-rs/src/process.rs:231, usr/halcyond/src/session.rs:167-175, usr/lib/ptyhold/src/lib.rs:139-146, usr/joey/joey.c:3060, kernel/syscall.c:8230, :9105-9112, :8638-8641
**Invariant**: I-22 (no ambient super-authority; elevation only via the legate), I-2 (caps monotone; here the mask that was supposed to narrow is missing), HALCYON.md 14.12 "Capabilities (I-22 clean)"; the d-1a commit's own claim "no extra cap".
**Prosecution**:
1. joey grants login `LOGIN_CAPS (T_CAP_SET_IDENTITY | T_CAP_LOCK_PAGES | T_CAP_CSPRNG_READ)` (joey.c:3060). The ut path deliberately narrows: `shell_cmd.identity(pid, gid, &supp).caps(SHELL_CAPS)` (login:1282-1283; `SHELL_CAPS = T_CAP_LOCK_PAGES | T_CAP_CSPRNG_READ`, login:74) -- "The shell gets the user's identity (SPAWN_IDENTITY_SET) but NOT CAP_SET_IDENTITY" (login:1274-1275).
2. The new session path omits the mask: `hal.arg("--session").identity(pid, gid, &supp).stdin(Stdio::Inherit).stdout(Stdio::Inherit).stderr(Stdio::Inherit);` (login:1317-1321) -- no `.caps(...)`.
3. libthyla-rs `Command` defaults to `cap_mask: !0u64, // inherit all caps; kernel intersects with parent` (process.rs:231), and the kernel computes `child->caps = parent->caps & cap_mask` (syscall.c:8230); `CAP_SET_IDENTITY` is FORK-GRANTABLE, "a member of CAP_ALL" (caps.h:80-88), not in CAP_ELEVATION_ONLY. So `halcyond --session` runs as the user WITH CAP_SET_IDENTITY.
4. It propagates: session.rs spawns each tile with `Command::new("/bin/kaua-term").arg(..).arg("/bin/ut").stdin(Stdio::Piped).stdout(Stdio::Piped).stderr(Stdio::Inherit).spawn()` (session.rs:167-175, no `.caps`); kaua-term spawns ut through ptyhold's `spawn_on_slave` -> `let mut cmd = Command::new(argv[0].clone()); ... cmd.spawn()` (ptyhold/lib.rs:139-146, no `.caps`). Every command the user runs in any tile therefore holds CAP_SET_IDENTITY.
5. Any such program calls `Command::new(x).identity(VICTIM_UID, ANY_GID, &[..]).spawn()` (process.rs:297 is public API). The kernel's only gate is `if (!(my_caps & CAP_SET_IDENTITY)) return -1;` (syscall.c:9112) plus `spawn_identity_id_ok`: `return id == PRINCIPAL_NONE || (id != PRINCIPAL_INVALID && id != PRINCIPAL_SYSTEM);` (syscall.c:8638-8641) -- every other principal id and every non-SYSTEM gid is accepted.
6. Observes: a process running as user A spawns a process whose kernel-stamped principal is user B (or NONE) with arbitrary primary/supplementary gids -> B's file authority under the A-2d rwx layer, the I-26 owner-axis kill of B's processes, `Actor::Session(B)` in tapestryd (server.rs:15448 keys on `peer_principal`, which the kernel stamps from the spawn identity), the corvus per-user surfaces keyed on principal. This is exactly the "I-22 ambient-authority hole" 14.12 says the design was chosen to avoid, and the d-1a commit message asserts the opposite ("no extra cap ... Zero identity delegation (I-22 clean)"). Reachable on every THYLACINE_HALCYON_SESSION=1 image; the default image is unaffected (the ut path masks). No gate constructs it: ls-gfx-session never reads the session's caps.
**Suggested fix**: `hal.caps(SHELL_CAPS)` on the session spawn (the exact mask the ut path applies), plus defense in depth at the two inheritance hops (session.rs kaua-term spawn and ptyhold::spawn_on_slave: `.caps(caps_of_self & !T_CAP_SET_IDENTITY)` or an explicit mask), and a witness: the session E2E (or a boot-test probe) reads `/proc/<halcyond>/caps` (or attempts an identity spawn from a tile) and asserts SET_IDENTITY absent. Consider making libthyla-rs `Command`'s default mask drop CAP_SET_IDENTITY unless `.caps()` names it -- the inherit-all default is the footgun (see F7).

## Disposition

Fixed in 062efe18 (round 1): `Command` defaults to inherit-all, and login masked only the `ut` path. login now spawns halcyond with `.caps(SHELL_CAPS)` (LOCK_PAGES | CSPRNG_READ -- SET_IDENTITY dropped) and aurora-push with `.caps(0)` (C-F7); the session compositor masks `!T_CAP_SET_IDENTITY` on every kaua-term spawn (the second hop's own guard); the kernel intersects, so both hops are monotone. Witness: `/bin/caps-probe` typed into a session tile by ls-gfx-session -- a plain spawn (the control) succeeds and the SAME spawn with an identity request is REFUSED, one variable apart. B-F1 is the same defect from the seam prosecutor.
