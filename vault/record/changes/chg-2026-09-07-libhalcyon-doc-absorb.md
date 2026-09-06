---
id: chg-2026-09-07-libhalcyon-doc-absorb
type: chg
title: "absorb docs/reference/151-libhalcyon (the Halcyon environment library): fold the UNOWNED halcyon tool + tag.rs into sub-libhalcyon"
date: 2026-09-07
arc: arc-vault
commits: ["f41a0a65"]
touched: [sub-libhalcyon]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
libhalcyon (theme/layout/skeleton/tag) + the halcyon tool. quaestor owner: the crate
sources -> sub-libhalcyon (audit:light, fresh 2026-09-06); usr/halcyon/{lib,main}.rs
-> UNOWNED; usr/lib/libhalcyon/src/tag.rs -> UNOWNED (missing from the code list).
Verified atom-by-atom.

ALREADY COVERED (verified, sub-libhalcyon fresh + deep): the theme single-token-source
(DAYLIGHT scripture, four-bevel-one-derivation, hairline==header, daylight_palette
agrees with the Sheet); the halcyon-layout v1 format + bounded no-panic parser +
from_render_text; the skeleton nest-vs-flatten MODEL + refs-not-ids + focus replay;
prune_env; the divergence-obligation (Invariants: the plan must model the compositor
faithfully). The H-4b F1 build-then-fill window + the Session(principal) authority
gate -> sub-tapestryd (audit:hard). TAPESTRY_CLAIM auto-consume -> sub-libtapestry.
The save durability pattern -> sub-aurora (config::save).

THE FOLD (genuine gap -> sub-libhalcyon, depth rich; updated 09-06 -> 09-07):
- The halcyon TOOL (usr/halcyon) was UNOWNED and its executor-side atoms homeless
  because sub-libhalcyon EXPLICITLY scopes itself to "the pure, authority-free planner
  + save format" (Invariants). Folded as "The halcyon tool" section: name_is_valid
  (one component, [A-Za-z0-9._-], no leading -/dot, no .tmp -> traversal closed by
  construction); the save = the aurora write-tmp/content-fsync/atomic-rename/
  metadata-fsync discipline; the restore executor = a Session(principal) peer (acts
  judged as the user's; shared /dev/tapestry used only for reads) driving
  skeleton::plan with a live-dump id-binding that VERIFIES each split's predicted
  nest/flatten and ABORTS rather than misplace; the one-shot TAPESTRY_CLAIM seeding;
  the H-4d-1 session-compositor anchor placement; the H-4b F1 window referenced to
  sub-tapestryd. Added usr/halcyon/{src/lib.rs,src/main.rs,Cargo.toml} + the missing
  usr/lib/libhalcyon/src/tag.rs to code:. The tool takes no capability/SPAWN_PERM/
  server verb (authority is the user's principal), so audit:light holds -- the gate is
  sub-tapestryd's. Render+lint verified NO double-claim.

Redirect stub. Zero code change.
