---
id: chg-2026-09-06-vt-doc-absorb
type: chg
title: "absorb docs/reference/150-vt (the shared VT/xterm parser): clean redirect to sub-lib-vt + the untrusted-ingest boundary"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
usr/lib/vt -- the shared VT/xterm escape-sequence parser. sub-lib-vt (audit:light,
updated 2026-09-05) LAPS the doc; verified atom-by-atom.

ALREADY COVERED (spot-checked, not assumed):
- The span serial (monotonic span_serial, OSC-1936 advances it, copied into
  Cell.span; recovers markup without a second Beacon parser, sub-lib-vt:80-89).
- The OSC 7770 in-band settings channel allowlisted twice, control bytes rejected
  in key AND value (the cfg-3 F1 laundering guard, :107-109/:184-185/:225).
- The wide-glyph model (char_width, ATTR_WIDE left + pen-blank right, combining
  zero-column, cols<2 degrade, :117-123/:187-188/:228/:277).
- DECSTBM/DECOM/SU/SD/SGR (incl. 22-clears-both) + the alt-screen 1049 autowrap
  (:117-121/:125/:246).
- TOTALITY by construction: CSI params saturate (no panic, :199), the cols<2
  degrade "must stay" (:228). The parser's contract is no-panic/no-OOB/no-wedge.

Zero-fold. AUDIT-CLASS SPLIT noted honestly: totality is the parser's by-
construction property (sub-lib-vt); the format-fuzz AUDIT ROUNDS run at the
untrusted-INGEST boundary (kaua-term audit:hard + halcyond's transcript parser),
NOT on the pure-logic crate -- which is why sub-lib-vt is audit:light. Redirect
stub.
