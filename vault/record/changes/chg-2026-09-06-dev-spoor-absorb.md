---
id: chg-2026-09-06-dev-spoor-absorb
type: chg
title: "docs/reference retirement: absorb 30-dev-spoor -- a THREE-dossier redirect (dev + spoor + path), zero-fold (content fully covered across all three, verified no orphan) (51 absorbed / 106 live)"
date: 2026-09-06
arc: arc-vault
commits: ["aa551eb4"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The devices/introspection batch opens (analyzed by a second Explore gap-finder,
alongside the memory batch). 30-dev-spoor is the cleanest: it bundled three
concerns the vault deliberately keeps as three dossiers, and the analysis found
NO content lost -- so it stubs zero-fold, but the stub must redirect to all
three or it would orphan two of them (the split-absorption failure class).

Verified before stubbing: the doc cites exactly kernel/{dev,devnone,path,spoor}.c,
which map to sub-kernel-dev (dev.c + devnone.c: bestiary, dev_register, the 25
vtable slots + the wstat_native/perm_enforced gate, dev_simple_*, devnone),
sub-kernel-path (path.c: copy-on-walk Path, path_addelem, fail-soft NULL, the
3 hooks, FD2PATH), and sub-kernel-spoor (spoor.c: clunk-vs-unref + the shallow-aux
unwind, ref-in-walk, magic-at-0 UAF, flag provenance-vs-state). Spot-checked each
dossier carries its third (28/12/28 coverage hits). No dossier edited.

"What it got wrong": the Dev vtable is stale at 16 slots (now 25); it bundles the
Path/I-33 surface into the Spoor chapter, obscuring the write-only-to-Path property
I-33 rests on; the Chan->Spoor / devtab->bestiary naming asides are dropped.

No code touched; no audit owed. view-absorption re-rendered: 50 -> 51 absorbed,
106 live. The rest of the devices batch (32-devproc ~7 telemetry atoms, 33-devctl
2 taxonomies, 109-devdev revoke-asymmetry, 31-trivial-devs) plus the heavy memory
pair (20-burrow A1-A4 + handle.c sibling, 25-fault-dispatcher exception.c sibling
+ C2/C3/C4) remain the analyzed queue. STANDOUT: arch/arm64/uart.c is an ORPHAN
(no dossier owns it; holds the A-4c-1 I-27 trusted-path RX/BREAK content; already
orphaned from 01-boot's stub, task #32) -- it needs a home BEFORE 31-trivial-devs
can stub.
