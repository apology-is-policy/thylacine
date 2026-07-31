---
id: view-audit-triggers
type: view
title: "Audit-trigger surfaces"
query: audit-triggers
---
# Audit-trigger surfaces

Generated from note fields — do not edit between the markers
(`quaestor render`). Replaces: the CLAUDE.md trigger table + the ARCH section-25.4 twin.

<!-- generated:begin -->
| surface | code | invariants | prosecution |
|---|---|---|---|
| [[sub-kernel-ninep-attach]] | kernel/9p_attach.c, kernel/include/thylacine/9p_attach.h |  | - **The failure-path ledger**: every exit must leave (adapter ref × |
| [[sub-kernel-ninep-client]] | kernel/9p_client.c, kernel/9p_session.c, kernel/9p_transport.c, kernel/9p_srvconn_transport.c, kernel/9p_transport_mq.c, kernel/9p_attach.c, kernel/include/thylacine/9p_client.h | inv-i9, inv-i10, inv-i11 | What an auditor attacks here (the single home of the trigger-row content for |
| [[sub-kernel-ninep-dev9p]] | kernel/dev9p.c, kernel/include/thylacine/dev9p.h | inv-i38 | - **The coherence pairing**: every mutation path must carry its exact |
| [[sub-kernel-ninep-dev9p-poll]] | kernel/dev9p_poll.c | inv-i9 | - **The I-9 window**: any reordering of register-hook / ensure-probe / |
| [[sub-kernel-ninep-session]] | kernel/9p_session.c, kernel/include/thylacine/9p_session.h | inv-i10, inv-i11 | - **The retirement matrix**: any new path that clears an `awaiting_flush` |
| [[sub-kernel-ninep-transport]] | kernel/9p_transport.c, kernel/9p_spoor_transport.c, kernel/9p_srvconn_transport.c, kernel/9p_transport_loopback.c, kernel/9p_transport_mq.c, kernel/include/thylacine/9p_transport.h |  | - **The EAGAIN classification boundary**: EAGAIN accepted anywhere past |
| [[sub-kernel-ninep-wire]] | kernel/9p_wire.c, kernel/include/thylacine/9p_wire.h |  | What an auditor attacks here (changes to this surface ride the |
<!-- generated:end -->
