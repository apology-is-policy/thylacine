---
id: chg-2026-09-18-npxf-host
type: chg
title: "Supported OpenSSL npxf hosts and pending graphical SAK ownership review"
date: 2026-09-18
arc: arc-tapestry
commits: []
touched: [sub-haul, sub-corvus, sub-substrate-interactive]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-18
---

The separate npxf repository at `apology-is-policy/npxf`, published `cd35c64`,
now builds with OpenSSL 3 and CMake on Linux and macOS. [[sub-haul]] keeps
its existing NPXF v1 wire and RustCrypto implementation. Its operator section
now gives host setup, a random token and read-only export commands.

Native CTest passes on both hosts and macOS LLVM ASan/UBSan. All 53 Haul host
tests pass including live interoperability; the actual CI-profile guest passes
`haul-npxf` and `haul-post` against the native macOS server (56s each). See
[[sub-substrate-interactive]] for the explicit serial-profile requirement and
`docs/JOURNAL.md` for failed attempts and isolated sanitizer runtime limitations.

[[sub-corvus]] records the requested graphical implementation and the pending
hardware ownership decision. The existing serial episode is still the only
enforced sink. `docs/GRAPHICAL-SAK-OWNERSHIP.md` is a proposal, not a ratified
replacement of the kernel-owned graphics contract. The approved visual mockup
alone does not authorize that separate trusted-computing-base change.
