---
id: chg-2026-09-06-warden-230-fixture-split
type: chg
title: "warden de-stale: #230 -- the broker runs in every build shape (not init's probe ladder), and its bind database splits into production BUILTIN_MANIFESTS vs opt-in FIXTURE_MANIFESTS (--with-fixtures)"
date: 2026-09-06
arc: arc-vault
commits: ["204363fa"]
touched:
  - sub-warden
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
[[sub-warden]] was brought current at 2026-09-02 for the H-4b-1 csprng cap chain
(its Grant section already covers `caps = ["csprng"]`), but `5a246371` (#230,
"the warden is not a probe") landed on `usr/warden/src/main.rs` after that dossier
commit and it did not cover it. Ground-truthed by diffing `e2aa1270..HEAD`
(+105/-34) and reading the current source. One substantive change folded:

**#230: the warden runs in EVERY build shape, and its bind database is now two
sets.** It entered the tree as a bind-loop PROOF, gated inside joey's
`THYLA_BOOT_PROBES` ladder; netd landed a day later and made it the network +
compositor bring-up, but nothing revisited the gate -- so the lean production
image booted with no drivers, no network, no compositor. A hardware broker is not
a test. The fix runs it unconditionally, pre-pivot, and SPLITS the manifests:
`BUILTIN_MANIFESTS` (production real hardware -- NIC/tapestryd/netd, every shape)
vs `FIXTURE_MANIFESTS` (`menagerie-probe` the 5c grant/narrowing proof +
`crash-probe` the 5e-2 restart demo, bound ONLY under `--with-fixtures`, which joey
passes). The split is a SAFETY boundary, not cosmetics: `menagerie-probe` binds
`arm,pl061`, a node QEMU-virt really provides, so leaving it production-side would
spawn a fixture on a shipped boot. Kept in ONE binary (not a cargo feature)
deliberately -- production then runs the SAME code the gates exercise, differing
only in the database length -- and every run LOGS which set it used, since a boot
that silently bound a different device set than the reader assumes is the whole
class of bug this arc closed. The synthetic restart-test node is still appended
unconditionally; only the manifest that binds it (crash-probe) moved to fixtures,
so a production boot discovers it and matches nothing.

Folded into Purpose (the #230 run-shape), Mechanism (Bind: the two-database split;
Discover: the restart ladder now fixture-gated), Data structures. `updated:` ->
2026-09-06. Stale backlog 38 -> 37.
