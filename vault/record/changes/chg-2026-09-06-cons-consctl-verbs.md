---
id: chg-2026-09-06-cons-consctl-verbs
type: chg
title: "sub-kernel-cons brought current: the three consctl surfaces since 2026-08-18 -- the beacon-tier verb, the serialsilent display-routing verb, and the C2-k1b termios projection"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-kernel-cons
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
kernel/cons.c moved ~819 lines since the dossier's 2026-08-18 update. Partitioned
by date, the churn splits cleanly: the extinction ring-lock tearing (455c651d /
7dd5be19) is dated 2026-08-18 and IS the update's base -- already covered (the TX
ring "the winner holds the ring lock" prose), borrowed. Three real consctl surfaces
landed AFTER and were absent from the dossier.

## The `beacon <tier>` verb (H-1/H-1a, 7cd1ab94)

The console-side origin of the render-tier chain this session folded on the consumer
end. A renderer advertises its Beacon render-capability via the consctl `beacon
rich|cells|none` verb -> `beacon_tier` (`CONS_BEACON_NONE`/`CELLS`/`RICH`), staged
and applied exactly as `winsize` is (atomic whole-write, under the console lock),
read by `cons_beacon_tier()`, reset to NONE when the renderer goes. It confers
nothing -- a lying tier only changes how consumers FORMAT bytes -- which is what
lets the console expose it to `/dev/beacon` for the shell's `BEACON` export
([[sub-beacon]] / [[sub-utopia-interactive]]) with no capability question. Added to
the control-file section's verb list + a paragraph.

## The `serialsilent <0|1>` verb (DISPLAY-MODES 1b, 19858aec)

When a graphical renderer is the PRIMARY display (`thylacine.display=gpu`), the
display owner sets `serialsilent 1` and EL0 program output to the UART is dropped --
the write SUCCEEDS fully (the program is neither blocked nor errored; only the bytes
are not emitted), read locklessly in the `cons_emit` paths. A display-routing
decision by the display owner, never a termios flag; the SAK path restores serial
output unconditionally (`cons_serial_silent_clear`, the audit F2 fix) so the trusted
path is never left dark. Added as a paragraph.

## The C2-k1b termios projection (05e91a06)

`cons_termios_get` renders the one global termios word as a Linux `struct termios`
for the VIVARIUM `isatty` / `tc[gs]etattr` ioctl -- a read-only projection, still one
global word. Folded into the existing "one global termios word" seam rather than a
new section, since it does not change the seam's claim (per-fd still belongs to
ptyfs).

The Data-structures section is field-agnostic (it names the four statics, not their
fields), so `beacon_tier` / `serial_silent` need no enumeration there. `updated:` ->
2026-09-06; guarded-by unchanged. audit: hard -- every mechanism verified in
cons.c/cons.h before folding.
