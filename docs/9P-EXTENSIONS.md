# The 9P extension-op registry (Thylacine + Stratum)

**This file is the single allocation authority for 9P message-type
numbers above the standard 9P2000.L set, across BOTH projects.** Before
assigning a new T/R pair anywhere (Thylacine `kernel/include/thylacine/
9p_wire.h`, libthyla-rs `ninep.rs`, Stratum `include/stratum/9p.h`),
check this table and extend it in the same change. Allocating from one
project's enum alone is how #371 happened (see History).

Rules:
- T/R pairs are adjacent even/odd (`R = T + 1`), the 9P convention.
- A number is BURNED once it has ever been on a wire in a release or a
  landed commit — renumbering is a wire-ABI change requiring user
  signoff and is possible only while both endpoints are in-tree and
  nothing persists the number.
- "Domain" records which client↔server pairs actually carry the op —
  collisions across disjoint domains are still forbidden (that was
  #371's latent state; disjointness is an accident of today's wiring,
  not a guarantee).

## Standard 9P2000.L (reference; not allocatable)

6..127 per the 9P2000.L spec as used here — notably Tgetattr 24,
Treaddir 40, Tfsync 50, Tversion 100, Tflush 108, Twalk 110, Tread 116,
Twrite 118, Tclunk 120. Legacy 9P2000 Tstat 124 / Twstat 126 are unused
by the .L dialect, which is why Stratum repurposed 124-127 (below).

## The extension registry

| T  | R  | Op | Owner | Domain | Defined in |
|----|----|----|-------|--------|------------|
| 124 | 125 | Tbind / Rbind | Stratum | any client ↔ stratumd | `stratum include/stratum/9p.h` |
| 126 | 127 | Tunbind / Runbind | Stratum | any client ↔ stratumd | same |
| 128 | 129 | Tsync / Rsync | Stratum | any client ↔ stratumd | same; mirrored in thyla `9p_wire.h` |
| 130 | 131 | Treflink / Rreflink | Stratum | any client ↔ stratumd | same; mirrored |
| 132 | 133 | Tfallocate / Rfallocate | Stratum | any client ↔ stratumd | same; mirrored |
| 134 | 135 | Tfadvise / Rfadvise | Stratum | any client ↔ stratumd | `stratum include/stratum/9p.h` |
| 136 | 137 | Tpin / Rpin | Stratum (reserved; ENOSYS at v2.0) | any client ↔ stratumd | same |
| 138 | 139 | Tunpin / Runpin | Stratum (reserved; ENOSYS at v2.0) | any client ↔ stratumd | same |
| 140 | 141 | Twalkgetattr / Rwalkgetattr | SHARED (POUNCE; Thylacine-designed, Stratum-implemented) | kernel client ↔ stratumd (netd answers ENOSYS → the dev9p per-session latch) | both: thyla `9p_wire.h` + `stratum include/stratum/9p.h` |
| 142 | 143 | Tweft / Rweft | Thylacine | kernel client ↔ netd ONLY (never stratumd) | thyla `9p_wire.h` + `ninep.rs` |
| 144 | 145 | Tweftio / Rweftio | Thylacine | kernel client ↔ netd ONLY | same |

**Next free pair: 146/147.**

## History

- **#371 (discovered at POUNCE P-2, 2026-07-07; resolved same day,
  user-approved):** the Weft family was born on 134-137, allocated
  believing the Stratum enum ended at Tfallocate 132/133 — but Stratum
  also assigns Tfadvise 134/135 + Tpin 136/137 + Tunpin 138/139. The
  collision was latent (Weft ops go kernel→netd only; Tfadvise/Tpin go
  to stratumd only; no session carries both), but the same number
  meaning two ops depending on the server is a standing hazard for any
  future proxy or multiplexed session. Resolution: the Weft quartet
  renumbered to 142-145 (both endpoints in-tree, no persistence;
  Stratum's 134-139 are shipped ABI and stay); POUNCE had already taken
  140/141 — the first pair free in both registries — at P-2.
- Stratum's 124-127 (Tbind/Tunbind) repurpose the legacy 9P2000
  Tstat/Twstat numbers, which the .L dialect leaves unused.

## Cross-references

- Thylacine: `kernel/include/thylacine/9p_wire.h` (the kernel wire
  enum), `usr/lib/libthyla-rs/src/ninep.rs` (the userspace codec netd
  serves with), `docs/POUNCE-DESIGN.md` §3, `docs/NET-THROUGHPUT.md` §6.
- Stratum: `include/stratum/9p.h` (the `STM_9P_*` enum),
  `docs/reference/20-9p.md`.
