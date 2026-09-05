# third_party — vendored upstream source

This directory holds **pristine, unmodified upstream source** vendored into the
Thylacine tree. Each subdirectory is a byte-for-byte copy of a published
upstream release, governed by that project's own license.

## Discipline

- **Do not edit a vendored tree in place.** Thylacine's changes to a vendored
  project live as a *patch series* outside `third_party/` — never as direct
  edits. This keeps the vendored copy identical to the published release, so an
  upstream security release can be re-vendored and the patch series rebased,
  and so the patch series' size is the honest measure of Thylacine's divergence.
- A vendored tree is updated only by re-vendoring a newer pinned upstream
  release: download the tarball, verify its checksum, replace the tree, rebase
  the patch series.
- Build artifacts never land here. Vendored builds run **out-of-tree** under
  `build/` (gitignored) — the vendored source stays clean.
- `third_party/` is excluded from the Thylacine line-of-code count (`loc.sh`):
  vendored code is not code we wrote.

## Vendored packages

### musl 1.2.5 — `third_party/musl/`

The portable upper half of **pouch**, Thylacine's POSIX libc (execution
Phase 6 — see `docs/POUCH-DESIGN.md`). pouch keeps musl's portable upper half
(string / stdio / stdlib / math — pure computation) near-unmodified and
replaces musl's OS-boundary lower half (sockets, threads, signals, the syscall
seam) with Thylacine-native code. That replacement is the patch series at
`usr/lib/pouch/patches/`.

| Field | Value |
|---|---|
| Version | 1.2.5 (released 2024-03-01) |
| Source | `https://musl.libc.org/releases/musl-1.2.5.tar.gz` |
| sha256 | `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4` |
| License | MIT — see `third_party/musl/COPYRIGHT` |
| Patch series | `usr/lib/pouch/patches/` |
| Boundary-line inventory | `docs/reference/78-pouch.md` |

The tree is byte-identical to the published release. To re-vendor on a musl
security release: download + verify the new tarball, replace `third_party/musl/`
in full, then rebase each patch in `usr/lib/pouch/patches/`.

**Re-vendor footgun.** musl ships its own `.gitignore` (`third_party/musl/.gitignore`),
and git honors nested `.gitignore` files — its `config.mak` pattern (line 6)
shadows the pristine, shipped file `third_party/musl/dist/config.mak`. That one
file must be force-staged so the vendored tree stays byte-complete:
`git add -f third_party/musl/dist/config.mak`. Any re-vendor must do the same.

### LLVM compiler-rt builtins 22.1.4 — `third_party/compiler-rt/`

The **compiler runtime** for **pouch** — the LLVM analogue of GCC's `libgcc`.
A complete C cross-toolchain is four parts: compiler + libc + CRT objects +
*compiler runtime*. compiler-rt's `builtins` component supplies the low-level
support routines clang emits calls to when the target ISA lacks an operation —
notably `binary128` soft-float (`long double` on aarch64 is IEEE `binary128`,
which has no hardware support), referenced by `printf`'s `vfprintf` path
(`__addtf3`, `__eqtf2`, `__extenddftf2`, …). Homebrew LLVM ships compiler-rt
for the Darwin host only, so pouch builds its own for `aarch64-thylacine`:
`tools/build.sh sysroot` → `build/sysroot/lib/libclang_rt.builtins.a`.

| Field | Value |
|---|---|
| Version | LLVM 22.1.4 — matches the pinned `clang` / `llvm-ar` toolchain |
| Source | `https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.4/llvm-project-22.1.4.src.tar.xz` |
| sha256 | `3e68c90dda630c27d41d201e37b8bbf5222e39b273dec5ca880709c69e0a07d4` |
| Vendored subtree | `compiler-rt/lib/builtins/` → `third_party/compiler-rt/builtins/` (443 files) |
| License | Apache-2.0 WITH LLVM-exception — see `third_party/compiler-rt/LICENSE.TXT` |
| Patch series | none — compiler-rt is pure computation; pouch does not patch it |
| Built by | `tools/build.sh sysroot` (`build_compiler_rt`); see `docs/reference/78-pouch.md` |

The `builtins/` tree is byte-identical to the published release. Unlike musl,
compiler-rt has **no patch series** — none of it is OS-boundary code, so there
is nothing to retarget; `build_compiler_rt` compiles the vendored source
directly, out-of-tree under `build/`.

**Why the monorepo tarball.** LLVM 22 stopped publishing per-component source
tarballs — `compiler-rt-22.1.4.src.tar.xz` no longer exists. The only source
tarball the `llvmorg-22.1.4` release ships is the full monorepo
`llvm-project-22.1.4.src.tar.xz` (159 MiB); pouch vendors only the
`compiler-rt/lib/builtins/` subtree from it. The pinned sha256 above is the
**monorepo tarball's** — a re-vendor downloads + verifies it, then re-extracts
that one subtree.

### libsodium 1.0.20 — `third_party/libsodium/`

The **first cross-compiled C library against pouch** (execution Phase 6,
sub-chunk 14 — see `docs/POUCH-DESIGN.md §14`). libsodium is a modern
crypto library — chacha20-poly1305 AEAD, BLAKE2b, SHA-2, ed25519 / x25519,
argon2 — and is the consumer Stratum's key agent relies on. Sub-chunk 14
proves the pouch cross-toolchain by building libsodium against pouch's
sysroot and running a known-answer-test (KAT) round-trip in Thylacine.

| Field | Value |
|---|---|
| Version | 1.0.20 (released 2026-01-06) |
| Source | `https://download.libsodium.org/libsodium/releases/libsodium-1.0.20.tar.gz` |
| sha256 | `ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19` |
| License | ISC — see `third_party/libsodium/LICENSE` |
| Patch series | none — libsodium is portable C; pouch does not patch it |
| Built by | `tools/build.sh sysroot` (`build_libsodium`); see `docs/reference/84-pouch-libsodium.md` |

The tree is byte-identical to the published release. Like compiler-rt,
libsodium has **no patch series** — none of it is OS-boundary code, so
there is nothing to retarget; the OS dependency (the CSPRNG) reaches the
kernel through `getentropy(3)` / `getrandom(2)` already wired by sub-chunks
4 + 11. `build_libsodium` compiles the vendored source directly, out-of-tree
under `build/pouch/libsodium-obj/`, with the `HAVE_*` configuration macros
that `./configure` would normally generate supplied via `-D` flags on the
compile command line. `version.h` is generated at build time from
`version.h.in` (placeholder substitution); the vendored tree stays clean.

### DejaVu fonts 2.37 (pruned) — `third_party/dejavu-fonts/`

Halcyon's proportional face (HALCYON.md §3: **DejaVu Sans Condensed**,
operator-chosen; the transcript body renders in it, with Cornucopia as the
monospace counterpart). Vendored PRUNED: only the four Condensed faces plus
the license documents — see `third_party/dejavu-fonts/PRUNE-MANIFEST.md`
for the exact kept/pruned split and the re-vendor recipe.

| Field | Value |
|---|---|
| Version | 2.37 (released 2016-04-25; the current upstream release) |
| Source | `https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2` |
| sha256 | `fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7` (the full tarball; kept files byte-identical to its contents) |
| License | Bitstream Vera + Arev public-domain terms — see `third_party/dejavu-fonts/LICENSE` |
| Patch series | none — font data is consumed as-is |
| Consumed by | `usr/halcyond` (`include_bytes!` at build; rasterized by the vendored fontdue) |

### Rust registry vendor — `third_party/rust/`

The **entire crates.io closure of the `usr/` workspace**, vendored via
`cargo vendor` (139 crates at creation). Landed at H-2c, whose scripture
(HALCYON.md §13.5) requires fontdue vendored "like every remote input" —
and cargo source replacement is all-or-nothing per registry, so the
honest mechanism vendors the whole closure: the workspace's pre-existing
registry deps (the RustCrypto family under `corvus-crypto`, smoltcp's
`defmt`, platform stubs) become hermetic in the same motion. Builds are
offline + reproducible; `usr/.cargo/config.toml` pins the source
replacement.

| Field | Value |
|---|---|
| Contents | one directory per crate release, exactly as `cargo vendor` wrote it |
| Integrity | per-crate `.cargo-checksum.json` (per-file sha256; cargo REFUSES a tampered tree) |
| Licenses | per-crate (each carries its upstream license files) |
| Patch series | none — **never hand-edit a vendored crate**; any edit is a checksum failure by design |
| Regenerate | edit the consumer's `Cargo.toml`, then from `usr/`: `cargo vendor ../third_party/rust` (network); commit the regenerated tree + `Cargo.lock` |

Unlike the tarball vendors above there is no single upstream checksum:
`usr/Cargo.lock` is the pin (crate versions + registry checksums), and
the `.cargo-checksum.json` files carry the byte-level integrity.
