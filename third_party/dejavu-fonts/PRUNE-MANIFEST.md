# dejavu-fonts — prune manifest

Vendored from the upstream release `dejavu-fonts-ttf-2.37.tar.bz2`
(sha256 `fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7`),
PRUNED to the faces Halcyon uses (HALCYON.md §3: DejaVu Sans Condensed is
the operator-chosen proportional face) plus the license documents. Every
kept file is byte-identical to the release copy.

## Kept

| File | Upstream path |
|---|---|
| `ttf/DejaVuSansCondensed.ttf` | `dejavu-fonts-ttf-2.37/ttf/DejaVuSansCondensed.ttf` |
| `ttf/DejaVuSansCondensed-Bold.ttf` | `dejavu-fonts-ttf-2.37/ttf/DejaVuSansCondensed-Bold.ttf` |
| `ttf/DejaVuSansCondensed-Oblique.ttf` | `dejavu-fonts-ttf-2.37/ttf/DejaVuSansCondensed-Oblique.ttf` |
| `ttf/DejaVuSansCondensed-BoldOblique.ttf` | `dejavu-fonts-ttf-2.37/ttf/DejaVuSansCondensed-BoldOblique.ttf` |
| `LICENSE` | `dejavu-fonts-ttf-2.37/LICENSE` |
| `AUTHORS` | `dejavu-fonts-ttf-2.37/AUTHORS` |

## Pruned

Everything else in the release: the 17 other faces (Sans, Serif, Mono and
their variants — Thylacine's monospace is Cornucopia, not DejaVu Mono),
`README.md`, `NEWS`, `BUGS`, `langcover.txt`, `unicover.txt`, `status.txt`.

To re-vendor (an upstream security/metrics release): download the new
`dejavu-fonts-ttf-X.Y.tar.bz2`, verify its published sha256, replace the
kept files from the same upstream paths, update this manifest and the
version row in `third_party/README.md`.
