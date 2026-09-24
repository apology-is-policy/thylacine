---
id: abi-user-authority
type: abi
title: "User authority record and protocol reservations"
kind: wire
stability: append-only
pinned-by: ["tools/check-authority-abi.py"]
mirrors: ["kernel/include/thylacine/authority_wire.h", "usr/lib/corvus-authority/src/abi.rs", "docs/USER-AUTHORITY-ABI.md"]
created: 2026-09-24
updated: 2026-09-24
---
## Layout / semantics
UA-0 partial reservation: canonical 96-byte mandate header, 32-byte optional
envelope header, total bound 896 bytes; Corvus verbs 21..25; tags, scoped action
masks and limits. Explicit little-endian arrays; unknown/reserved values refused.
No enabled protocol or authority is conferred by these definitions. Request/reply,
LCURv2 and kernel scope/admission layouts remain pending before their consumers.
See USER-AUTHORITY-ABI for the exact field table.

## Change protocol
Update all mirrors together, record mirrors-checked, run the source mirror check
and compile both static-assert layouts. Layout changes require a schema version;
unknown schema versions never silently fall back to old policy.
