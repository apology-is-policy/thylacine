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
