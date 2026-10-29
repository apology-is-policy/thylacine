---
id: dec-2026-10-04-async-memory-lifecycles
type: dec
title: "Approve private async service scopes and durable memory accounts"
date: 2026-10-04
status: standing
decided-by: user-vote
affects: [sub-kernel-loom, sub-kernel-mm-phys, sub-kernel-addrspace]
created: 2026-10-04
---
The operator read ASYNC-SERVICE-LIFECYCLE.md and SHARED-MEMORY-ACCOUNTING.md
and answered "approved including order." Both contracts and the async-first
sequence in ASYNC-MEMORY-DESIGN-REVIEW.md are ratified. Their prior-art review
and rejected alternatives remain in those specifications.

Private scopes are native-service-only initially, with ring-local fids and no
cross-Proc shared address space. Memory accounts preserve physical, allocation,
retention and metadata distinctions; pressure recovery is cooperative. No
implicit process killing or fixed guaranteed per-user partition is added.

Numeric ABI encodings are reserved before consumers; implementation and actual
qualification follow this scripture checkpoint. Existing128MiB enforcement
remains until its replacement is verified. Single-agent review continues under
the standing instruction; no independent audit or new runtime result is claimed.
