# TyrQuake vendored source — prune manifest

Upstream: TyrQuake 0.71 (Kevin Shanahan's maintained Quake port; GPLv2 —
`gnu.txt`; the software renderer kept alive, which is exactly why it is
the G-7 gate title: TAPESTRY.md §9/§17 "software-Quake").
Tarball: `tyrquake-0.71.tar.gz` from
`https://disenchant.net/files/engine/tyrquake-0.71.tar.gz`
sha256 `ccdfed38e6258af4778b9f03f6a9e9d323b00548927aba7e7afb0959d8f263d3`.

Every file KEPT is byte-pristine from that tarball. PRUNED (whole
files/dirs removed, none edited) to the NQ software-renderer SDL build
the Thylacine pouch cross-build compiles (`build_tyrquake()`; the
curated object list mirrors the upstream Makefile's
COMMON/CL/SV/NQCL/SW groups + the sdl/null driver selections).

## Removed (relative to the tarball root)

- `QW/` (the QuakeWorld client/server — the NQ single-player build is
  the gate; QW is a network game needing infrastructure out of scope).
- `launcher/` (GTK launcher), `wine-dx/` + `wine-dx.patch`, `icons/`,
  `man/`, `scripts/`, `NQ/3dfx.txt`.

## Kept

- `common/` + `NQ/` + `include/` + `external/` (the compile set; the
  x86-asm `.S` files stay unbuilt — `USE_X86_ASM` is never defined on
  aarch64), `Makefile` (the object-list reference the build.sh list is
  curated from), `changelog.txt`, `readme*.txt`, `gnu.txt` (GPLv2).

## Game data is NOT here

The shareware `pak0.pak` is fetched at BUILD time into `build/` (never
committed) and baked into the pool fixture at `/usr/share/quake/id1/`.
