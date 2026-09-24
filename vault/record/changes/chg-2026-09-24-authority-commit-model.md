---
id: chg-2026-09-24-authority-commit-model
type: chg
title: "Model immutable authority commit and measure dense policy"
date: 2026-09-24
arc: arc-user-authority
commits: ["*(pending)*"]
touched: [sub-corvus-authority, inv-i35, spec-mandate]
established: [spec-mandate-commit]
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
Model separate trusted admission and durable publication, immutable intent,
group-dependent preview, expiry/client/issuer lifetime, suspend/resume and
restrictive replay before the corresponding transaction implementation. Clean
3768 states; all seven new mutants violate their named invariants; original
model154 states plus seven mutants also rechecked. Self-review only.

Add dense maximum-policy fixture:36 host tests pass, host Clippy clean.
4096 records retain4,127,808 bytes of Vec payload; closure about15ms on this Mac
in a debug build. Excludes allocator metadata, guest RSS and backend revocation.
First test invocation used --manifest-path from the repo root, selecting a local
usr/target instead of workspace config; reran from usr/ with --offline and kept
both logs. No dependency/lockfile change. Canonical evidence:
work/ua-policy/dense-workspace.log; model evidence:
work/ua-model/20260924T110752.927464Z. Kernel/runtime integration still pending.
