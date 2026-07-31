---
id: fnd-d44-r1-f1
type: fnd
title: "The aligned-read lead arm manufactured a FALSE MID-FILE EOF from a legitimate server short-return"
round: adt-d44-r1
severity: P1
status: fixed
surface: [sub-kernel-ninep-dev9p]
threatens: [inv-i38]
fixed-by: chg-2026-07-11-d44-read-band
regression: dev9p.read_align_short_not_eof
created: 2026-07-31
---
## Prosecution

The aligned wire read fetches from `wire_off <= offset`; the `got <=
lead → return 0` arm assumed a page-aligned Tread only short-returns at
EOF. A single Rread may legitimately short-return MID-FILE (the R-5
ground truth), so `0 < got <= lead` returned 0 — a false mid-file EOF —
to consumers whose loop-termination contract is "0 means end-of-file":
the REVENANT cluster fill breaks and installs its KP_ZERO tail as
RESIDENT executable text pages (silent persistent corruption of exec'd
code); eager segment reads fail an exec spuriously; the ELF header read
truncates; userspace streams see a false EOF (Go io.ReadFull →
ErrUnexpectedEOF). Latent — the loopback fixture never short-returns
mid-file and no measured boot tripped it — hence P1, not P0.

## Disposition

Fixed: the arm split — `got == 0` is a TRUE EOF (a Tread at wire_off
returning nothing proves size <= wire_off <= offset); `0 < got <= lead`
RETRIES UNSHIFTED at the caller's offset (direct client read, no
recursion) and returns that verbatim; the shifted fetch's front-page
heal is already installed either way. Regression injects a one-shot
mid-file short below lead — fails pre-fix by construction. The round's
lesson: an "every consumer loops" sweep is incomplete until each loop's
termination condition is confronted with every value the change can
return.
