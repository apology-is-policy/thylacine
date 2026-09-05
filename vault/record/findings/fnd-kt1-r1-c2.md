---
id: fnd-kt1-r1-c2
type: fnd
title: "structural transparency lets an unprivileged session process `close` a container holding the console renderer -- aurora receives TEV_CLOSE and exits; the graphical console is gone until reboot"
round: adt-kt1-r1
severity: P1
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "ls-gfx-panes: 'declared session cannot close the console's container' (E_PERM)"
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestryd/src/server.rs:6294-6315 (actor_owns_subtree Session arm), :6497-6512 (pane_cmd close), usr/tapestryd/src/pane.rs:612-653 (close_inner), usr/aurora/src/main.rs:806-809
**Invariant**: the compositor authority model (HALCYON.md 13.6; server.rs:167-172 "The console (SYSTEM principal) and other users (another principal) stay protected"); the F2 commit's own guard text ("else a session could close/mode the console renderer"); I-27-adjacent (the graphical login prompt is rendered by aurora).
**Prosecution**:
1. State: a session is up. The common tree after the first tile is `root = SplitH [aurora_leaf(bg), A(session)]` (host() splits the occupied aurora leaf; pane.rs:524-535). `Pane.backgrounded` is true on aurora_leaf (reconcile 5782-5799).
2. Any same-principal process (the ratified `Actor::Session(p)` -- "a program running as you", server.rs:169-171) writes `close <root-id>` to `layout` (or `close` to `pane/<root-id>/ctl`). `layout_cmd` -> `pane_cmd(actor, id, "close")` -> `if !self.actor_owns_subtree(actor, slot) { return Err(p9::E_PERM); }` (6498).
3. The Session arm: `let hosted = self.layout.subtree_hosted(slot); ... for &(leaf, n) in &hosted { if self.layout.is_bg_leaf(leaf) { continue; } match self.surf(n) { Some(s) if s.owner_principal == p => owned_any = true, _ => return false, } } owned_any` (6300-6314). aurora_leaf is skipped; A is owned -> returns TRUE. Pre-F2 this was `.all(|&n| ... owner_principal == p)` -> false -> E_PERM.
4. `let unhosted = self.layout.close(slot); for n in unhosted { self.send_close(n); }` (6509-6512). `close_inner` collects EVERY hosted surface in the subtree -- `self.collect_surfaces(slot, unhosted);` (pane.rs:614) -- including aurora's, resets the root to an empty leaf and frees every other pane (616-629). aurora is unhosted ("hosting is once-per-life", 6502) and gets TEV_CLOSE.
5. aurora: `TEV_CLOSE => { say!("aurora: CLOSE received; exiting"); return 0; }` (aurora/main.rs:806-809). joey does not respawn the renderer ("every other joey child is reaped by-pid at its spawn site", joey.c:72). The session's tile also got CLOSE -> halcyond reaps -> logout -> getty -> the login prompt has no renderer; the layout is an empty root -> reconcile's Off arm `display_disable()` (5940). The GPU console is dark until reboot; only serial works.
6. The guard only covers an aurora-ONLY subtree. `mode`/`move`/`tab` on a mixed subtree are harmless (they do not destroy the transparent leaf); `close` is the one verb whose effect reaches the transparent leaf, and it is not special-cased. The legitimate session compositor only ever closes leaf ids (session.rs:611 `close {leaf}`), so no gate constructs this.
**Suggested fix**: transparency must not extend to destructive verbs: in `pane_cmd` "close" (and any future subtree-destroying verb) require the pre-F2 full-ownership predicate (`subtree_surfaces(slot).all(owner == p)`), or make `close_inner` re-home (detach + reinsert beside the parent) any leaf the actor does not own instead of unhosting it. Add a battery leg: a Session `close <root>` while the console leaf is hosted -> E_PERM, console surface still hosted.

## Disposition

Fixed in 062efe18: the destructive verb no longer uses the transparent view. `close` runs `actor_owns_subtree_all` (a Session owns a subtree only if EVERY hosted surface in it is its own -- a backgrounded SYSTEM leaf blocks it); `split`/`focus`/`zoom`/`tab`/`move` keep the guarded transparency. Regression (round-2 close): the ls-gfx-panes declared control, where the battery's declared conn writes `close` on the container holding the console leaf and must read E_PERM with the console leaf undisturbed.
