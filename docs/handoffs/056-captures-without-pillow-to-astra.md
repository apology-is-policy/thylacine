# 056 -- the scenario captures no longer need Pillow (to Astra)

From corona, 2026-11-13, operator vote 96 ("drop the Pillow dependency").
Branch `corona/restore`, a commit of its own after the restore's. Written and
NOT YET RUN IN A GUEST when committed; the S half of work/nov03-mm1's restore
gate runs `ls-halcyon-pointer` with no Pillow on the host to prove it.

## What changed

- `tools/interactive/vnc_capture.py` (yours, from the pointer work) imports no
  Pillow. It keeps the frame as a `bytearray` of RGB, writes its PNGs itself
  (`write_png`, filter 0, zlib), and composes the cursor with Pillow's own
  arithmetic: the straight colour is `premultiplied * 255 // alpha`, and each
  channel is `((mix >> 8) + mix) >> 8` with `mix = dst * (255 - a) + src * a +
  128`. On three seeded frames from a scripted RFB server the framebuffer PNG,
  the cursor PNG, the composed PNG and the JSON witness were byte-identical to
  the Pillow version's (a capture without `--cursor-at` too), and a mismatched
  pair differed, so the comparison could fail.
- `tools/interactive/png_extrema.py` (new): per-channel (min, max) of a box,
  printed as Pillow's `getextrema()` prints it, read with gfx_fp's `read_png`.
  `ls-halcyon-pointer` uses it for the flat-scene check instead of an inline
  Pillow program.
- `lc_require_host_pil` (lib.exp) is gone, with its calls in
  `ls-halcyon-pointer` and the six SAK scenarios that capture over EGL.

## If you change this

A scenario's host tools stay on the standard library. A tool that cannot
must SKIP (exit 77) before a VM starts, as the dossier's rule 7 says
(vault/system/substrate/sub-substrate-interactive.md).
