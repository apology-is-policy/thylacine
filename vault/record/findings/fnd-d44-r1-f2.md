---
id: fnd-d44-r1-f2
type: fnd
title: "The attr-served EOF extends close-to-open to the EOF determination (external-writer window)"
round: adt-d44-r1
severity: P3
status: documented
surface: [sub-kernel-larder, sub-kernel-ninep-dev9p]
threatens: [inv-i38]
regression: ""
created: 2026-07-31
---
## Prosecution

`larder_attr_fresh_size` answers a plain-file read at `offset >= size`
with 0 RPC-free. Under a hypothetical EXTERNAL writer (outside the I-38
single-writer premise) an out-of-band append leaves the cached open-time
size divergent from a fresh RPC until the next revalidation — a reader
polling for growth at EOF would see it late.

## Disposition

Documented — within-premise sound, and EXACTLY the same close-to-open
window the page serve already carries (the cvers gate bounds both to
one open episode). Not a new coherence class, a new member of the
accepted one; noted beside the page-cache close-to-open record. No fix
owed beyond the premise itself.
