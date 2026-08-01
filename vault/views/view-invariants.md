---
id: view-invariants
type: view
title: "Invariant matrix"
query: invariants
---
# Invariant matrix

Generated from note fields — do not edit between the markers
(`quaestor render`). Replaces: the ARCH section-28 condensed table.

<!-- generated:begin -->
| # | invariant | strength | guards | validated by |
|---|---|---|---|---|
| I-1 | [[inv-i1]] | spec | sub-kernel-territory, sub-kernel-devsrv | spec-territory, gate-smp |
| I-10 | [[inv-i10]] | spec | sub-kernel-ninep-client, sub-kernel-ninep-session | spec-9p-client |
| I-11 | [[inv-i11]] | spec | sub-kernel-ninep-client, sub-kernel-ninep-session | spec-9p-client |
| I-28 | [[inv-i28]] | prose | sub-kernel-stalk | gate-smp |
| I-3 | [[inv-i3]] | spec | sub-kernel-territory | spec-territory, gate-smp |
| I-33 | [[inv-i33]] | prose | sub-kernel-path, sub-kernel-stalk, sub-kernel-territory | gate-smp |
| I-38 | [[inv-i38]] | spec | sub-kernel-ninep-dev9p, sub-kernel-larder | spec-fs-cache |
| I-9 | [[inv-i9]] | spec | sub-kernel-ninep-client, sub-kernel-ninep-dev9p-poll, sub-kernel-srvconn, sub-netd-server | spec-reader-frame, spec-9p-client, spec-net-poll, spec-net-poll-teardown, gate-smp |
<!-- generated:end -->
