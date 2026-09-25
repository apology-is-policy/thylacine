---
id: chg-2026-09-25-pty-observer-client
type: chg
title: "Add typed native terminal observer client"
date: 2026-09-25
arc: arc-halcyon-interaction
commits: ["*(pending)*"]
touched: [sub-libthyla-rs, sub-kernel-syscall-abi, sub-halcyond]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-25
---
Adds typed native wrappers for the existing terminal ownership operations,
keeping binding locators distinct from role authority and explicit unbinding
distinct from RAII watch closure. Halcyon seals its terminal host at spawn;
ordinary slave-side children retain their default unsealed spawn.

AArch64 checks pass for the library, Halcyon and probe. The full production image
check passes with test-only kernel symbols absent. The new pty-observer interactive
scenario passes in HVF: actual EL0 records, watch readiness/retirement and spawn
seal behavior, followed by a live shell pipeline. Positive observer ACK/CHECK and
the actual Halcyon tile launch remain part of live integration. The preceding
kernel checkpoint separately passed all 50 SMP/UBSan boots. Applications are not
yet wired to clipboard service; neither result establishes a live clipboard.
