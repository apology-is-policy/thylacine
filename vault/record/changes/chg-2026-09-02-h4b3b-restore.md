---
id: chg-2026-09-02-h4b3b-restore
type: chg
title: "H-4b-3b: the restore tool + PFK_OWNER + the H-4b arc audit close"
date: 2026-09-02
arc: arc-tapestry
commits: ["3b12f7b4", "e8ca3b84", "589f0735"]
touched: [sub-tapestryd, sub-libtapestry]
established: []
closed: []
opened: []
mirrors-checked: [codeberg, github]
depth: skeletal
created: 2026-09-02
---
H-4b-3b is the H-4b arc's last sub-chunk: `halcyon layout restore` (the
syscalling half of the D decision), plus the tapestryd `pane/<id>/owner` file
(PFK_OWNER, read-only) and the libhalcyon `env` marker + `prune_env` + the pure
`skeleton` restore planner it needs. The tool drives its OWN /srv/tapestry
session (a Session(principal) peer), builds a saved tree via split/mode verbs
with a live-dump-diff id-binding + a nest/flatten divergence check, claims each
built leaf, seeds the one-shot token into its /env, and spawns each tag as the
user; the child's libtapestry auto-claims (H-4b-3a). The NEW ls-gfx-restore E2E
carries H-4b-2's positive cross-process mutual-authority witness (the tool
focuses a peer same-principal process's tile).

The batched holotype over the whole H-4b arc (H-4b-1..3) CLOSED: 0 P0 / 0 P1 /
0 P2 / 2 P3, NOT dirty. Fable was out of credits, so the round ran on the Opus
fallback (a finished fallback round is closed, no Fable re-run owed);
MODEL(start)==MODEL(end)==Opus 4.8. F2 [P3] FIXED (the halcyon tool's env
classifier treated owner 0 as respawnable -- a fail-OPEN arm; now the pure
`owner_is_env` = owner!=me, fail-closed). F1 [P3] documented + tracked as a
v1.x multi-seat seam (tracked in the Thylacine memory, bug-h4b-empty-scaffold-griefable-cross-principal): the
empty restore-scaffold is grief-able cross-principal until filled (the ratified
13.6 "an all-empty subtree is anyone's"), harmless at v1.0's single session.

Pushed to both mirrors (ea0a2ba4..589f0735; codeberg + github verified). The
whole H-4b arc (the placement claim, the Session actor, the auto-claim, the
restore tool) is now landed, audited, and shipped.
