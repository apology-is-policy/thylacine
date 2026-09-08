# typelab -- the Halcyon typography lab

Host-only research tooling behind `docs/HALCYON-TYPE.md`: the same Daylight
text (the welcome screen's heading, a prose line with Cornucopia islands,
the dim closing line) rendered through every rendering recipe under study
and next to CoreText on the same IBM Plex Sans files, at 1.0 and 2.0, with
a measurement beside the eye. Not part of any build; run it when a type
rendering question comes up and the numbers must be re-derived rather than
remembered.

## Producers

| Half | What | Builds with |
|---|---|---|
| `src/main.rs` | fontdue (the as-built rasterizer) and skrifa + zeno (outlines: unhinted / autohinter LIGHT / TrueType interpreter; exact placement; outline stroke); every pen, blend and emboldening variant; the probes; the metrics; PNG output; the driver | `cargo build --release` (fetches skrifa, zeno, png from crates.io -- a research instrument, not a vendored build input) |
| `ftdump.c` | FreeType glyph masks: `nohint`, `light`, `light-dark` (stem darkening), `normal` (v40 interpreter); optional third-pixel outline shift | `cc -O2 -o ftdump ftdump.c $(pkg-config --cflags --libs freetype2)` (brew freetype) |
| `ct.swift` | CoreText/Quartz compositing of a spec into a P6 PPM with font smoothing / subpixel positioning each on or off, kerning off | `swiftc -O -o ct ct.swift` |
| `assemble.py` | the comparison page: embeds the selected renders as data URIs, the numbers table, the two Plex faces | python3 (stdlib) |

## Run

```sh
cd tools/typelab
cc -O2 -o ftdump ftdump.c $(pkg-config --cflags --libs freetype2)
swiftc -O -o ct ct.swift
SSL_CERT_FILE=/etc/ssl/cert.pem cargo build --release   # see memory: the stale emsdk certifi
./target/release/typelab                                 # -> out/ (about 2.5 s)
python3 assemble.py out halcyon-type-lab.html
```

`out/<scale>/<sample>/<variant>.png` is the full render, `.crop-<name>.png`
the nearest-neighbour magnification, `out/metrics.tsv` the numbers per
(scale, sample, variant): row-ink roughness (linear and L*), weight (mean
L* ink per row), core continuity, fringe darkness and width. `out/index.tsv`
is what `assemble.py` reads.

## Inputs

`third_party/ibm-plex/ttf/IBMPlexSans-{Text,Bold,TextItalic,Italic}.ttf`
and `~/projects/cornucopia-font/cornucopia-Regular.ttf` (the bake's
provenance, `tools/bake-cornucopia.py`). Paths are constants at the top of
`src/main.rs`.

## Adding a variant

`variants()` in `src/main.rs`: a rasterizer (`Fontdue`, `Skrifa(hint)`,
`Ft(mode)`), a pen (`Int` whole-pixel, `Third`, `Exact`), a blend (`Srgb`
verbatim from `cartoon::blend`, `Linear`, `Mid` = gamma 1.45), a mask
emboldening radius, an outline stroke as a fraction of the em. Add its
one-line description to `DESC` in `assemble.py`.
