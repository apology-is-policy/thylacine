# ibm-plex — prune manifest

Vendored from IBM's `@ibm/plex-sans@1.1.0` release package `ibm-plex-sans.zip`
(sha256 `fb365d910566e6d199cc2c15579a7dd9a267128e18431a394ed81f1970c69200`,
9,921,777 bytes; GitHub publishes no digest for this asset, so the sha256 is
the one measured on 2026-10-21), PRUNED to the faces Halcyon uses plus the
licence. Every kept file is byte-identical to the release copy. The same
seven faces are byte-identical in `@ibm/plex-sans@1.0.0` and in v6.4.0's
`IBM-Plex-Sans.zip`, and differ in v6.3.0's `TrueType.zip`: version 3.005
first ships in v6.4.0.

## Kept

| File | Upstream path |
|---|---|
| `ttf/IBMPlexSans-Regular.ttf` | `ibm-plex-sans/fonts/complete/ttf/IBMPlexSans-Regular.ttf` |
| `ttf/IBMPlexSans-Italic.ttf` | `ibm-plex-sans/fonts/complete/ttf/IBMPlexSans-Italic.ttf` |
| `ttf/IBMPlexSans-Bold.ttf` | `ibm-plex-sans/fonts/complete/ttf/IBMPlexSans-Bold.ttf` |
| `ttf/IBMPlexSans-Medium.ttf` | `ibm-plex-sans/fonts/complete/ttf/IBMPlexSans-Medium.ttf` |
| `ttf/IBMPlexSans-SemiBold.ttf` | `ibm-plex-sans/fonts/complete/ttf/IBMPlexSans-SemiBold.ttf` |
| `ttf/IBMPlexSans-Text.ttf` | `ibm-plex-sans/fonts/complete/ttf/IBMPlexSans-Text.ttf` |
| `ttf/IBMPlexSans-TextItalic.ttf` | `ibm-plex-sans/fonts/complete/ttf/IBMPlexSans-TextItalic.ttf` |
| `OFL.txt` | `ibm-plex-sans/LICENSE.txt` (renamed) |

## Pruned

Everything else in the package: the nine other TrueType faces (Thin,
ExtraLight, Light and the remaining italics), the `otf`, `eot`, `woff` and
`woff2` builds, the `split` subsets, `css` and `scss`.

To re-vendor: download the new `@ibm/plex-sans` release package, record its
sha256, replace the kept files from the same paths, and update this manifest
and the version row in `third_party/README.md`.
