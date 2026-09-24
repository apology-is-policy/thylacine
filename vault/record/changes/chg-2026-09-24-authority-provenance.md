---
id: chg-2026-09-24-authority-provenance
type: chg
title: "Retain authority transaction provenance in the policy ledger"
date: 2026-09-24
arc: arc-user-authority
commits: ["*(pending)*"]
touched: [sub-corvus-authority, inv-i35]
established: []
closed: []
opened: []
mirrors-checked: [abi-user-authority]
depth: skeletal
created: 2026-09-24
---
Mandate now owns its transaction ID throughout policy insertion and both
revocation phases, eliminating a separate codec wrapper that lost attribution
when inserted into the ledger. MDTM v1 bytes are unchanged.49 host tests pass,
bare-target check and Clippy pass. No runtime publication or durability claim.
