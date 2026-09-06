---
id: chg-2026-09-06-docs-reference-retirement-flip
type: chg
title: "SCRIPTURE: retire docs/reference into the vault -- CLAUDE.md step 0 routing flipped so new technical-reference prose goes to a dossier, never a new docs/reference section; the legacy tree is frozen + absorbed into redirect stubs"
date: 2026-09-06
arc: arc-vault
commits: []
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
Operator-ratified 2026-09-06 (AskUserQuestion, direction 2 of the two the
operator chose on return: "docs/reference retirement" -> approach "Flip now").
The design-conversation pattern: this is the SCRIPTURE COMMIT (CLAUDE.md only,
no code), landed for the operator's review before the execution (the ~120-file
absorption) proceeds.

THE DECISION: `docs/reference` (the technical reference) is retired into the
vault. CLAUDE.md's doc-update discipline (step 0) is flipped: an unowned surface
no longer says "write the reference section as today" -- it says a new DOSSIER is
owed (ring vault to author it, the same delegation exit 0 already used for owned
surfaces). So the answer at the doc step is now ALWAYS the vault, covered or not;
the code tracks never write technical-reference prose again.

WHY NOW (not after full coverage): the retirement mechanism is already settled by
PRECEDENT -- the sweep has been absorbing subsystems into redirect stubs since the
start (docs/reference/18-territory.md is a 51-line `[ABSORBED INTO THE VAULT]`
pointer + "what the old file got wrong"). ~32 of 157 files are absorbed; ~120
carry full parallel content. Flipping the routing NOW stops the parallel tree
growing today (the two-sources-of-truth divergence the vault exists to close),
rather than letting it accrete until coverage completes. Alternatives rejected:
(B) flip after full coverage -- leaves the divergence open longer and keeps the
tree growing; (C) don't flip -- abandons the retirement the operator chose.

CLAUDE.md edits (all in "Reference documentation discipline" + the scripture
table): Part A reframed (the vault IS the technical reference; docs/reference is
frozen legacy being absorbed; the per-file template kept as the depth bar a
dossier meets); step 0 routing flipped (always the vault); step 1 flipped (extend
or create the owning dossier; docs/reference frozen); the doc-table row marked
LEGACY/retiring; the audit-policy precedence updated (technical reference = the
vault dossier, or the legacy section until its subsystem is absorbed); the "Why
two references" framing clarified (the split is unchanged -- vault (technical) +
docs/manual (user); docs/manual is a separate track, unaffected, itself deferred
to v1.0-rc). The stub-vs-eventual-delete end-state for the absorbed files is a
later, deferrable call (stubs redirect correctly now; deleting them needs the
external citations -- ARCH section 28 rows, REFERENCE.md index -- repointed first).

NEXT (execution, post-signoff): (1) coordinate the flip to main + aux via yip
(they stop writing new reference docs); (2) the ~120-file coverage survey -- which
remaining full reference files map to an existing dossier (stub now) vs need a
dossier authored first (absorption work) -- producing the sweep's work queue.
