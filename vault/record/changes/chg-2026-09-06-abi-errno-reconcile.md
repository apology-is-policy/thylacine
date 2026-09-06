---
id: chg-2026-09-06-abi-errno-reconcile
type: chg
title: "abi-errno registry reconcile: +16 missing codes (the whole V-5 socket family + INTR/2BIG/CHILD/NOTTY/MFILE/NODEV/NOTDIR/ISDIR/LOOP), the stale self-counts (19->35 non-zero, 20->36 asserts), and the err.rs mirror analysis"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - abi-errno
established: []
closed: []
opened: []
mirrors-checked:
  - usr/lib/libthyla-rs/src/err.rs
  - usr/lib/pouch/patches/0001-pouch-syscall-seam.patch
depth: skeletal
created: 2026-09-06
---
[[abi-errno]] (updated 2026-08-02, `stability: append-only`) had fallen a third
behind the ABI it registers. aux flagged one owed code on the yip channel
(0036: "still owed from the C2-kernel close ... abi-errno T_E_NOTTY=25"); a full
reconciliation against `kernel/include/thylacine/errno.h` found the gap was much
larger. This is a registry the stale tool does not flag (it tracks sub-dossier
`code:` churn, not registry drift), so the peer's flag was the only signal.

Ground-truthed against errno.h (every value + POSIX name + the tree-specific
meaning taken from the define's own comment) and against `err.rs`:

- **Table: 19 -> 35 non-zero rows.** Added, in value order: `INTR` (4),
  `2BIG` (7), `CHILD` (10), `NOTDIR` (20), `ISDIR` (21), `MFILE` (24),
  `NOTTY` (25), `LOOP` (40), and the eight-code V-5 socket family
  `NOTSOCK` (88) / `PROTONOSUPPORT` (93) / `AFNOSUPPORT` (97) / `ADDRINUSE`
  (98) / `CONNABORTED` (103) / `ISCONN` (106) / `NOTCONN` (107) /
  `CONNREFUSED` (111). Each meaning is the errno.h comment's tree-specific
  description (e.g. NOTDIR precedes the X-check so 0755==0644; ADDRINUSE is
  reported by listen() not bind()), not a generic POSIX gloss.
- **Self-counts corrected.** `pinned-by` 20 -> 36 asserts (measured:
  `grep -c "_Static_assert(T_E_"` = 36). The prose "19 non-zero values" the
  mirror section rested on is gone.
- **err.rs mirror analysis rewritten.** It now names 18 of the 35 (measured
  from `as_errno`), missing 17 -- dominated by the socket family, plus
  INTR/2BIG/CHILD/NOTTY. The old "missing four (SRCH/NODEV/OPNOTSUPP/CANCELED)"
  was itself stale: the mirror GAINED NotADirectory/IsADirectory/SymlinkLoop
  while the socket family appended past it. Recorded the reverse asymmetry too
  -- err.rs names `DirectoryNotEmpty` -> 39 (ENOTEMPTY, #80), which has NO
  `T_E_*` in errno.h (server-originated via the Rlerror passthrough).

Both mirrors checked (R6). `err.rs`: analyzed above (18 of 35, drift recorded).
`0001-pouch-syscall-seam.patch`: a range contract, not a value list -- every
added code negates within `[-4095, -2]` (the largest, `CONNREFUSED` -> -111),
so `__syscall_ret`'s passthrough already carries them unchanged; no patch edit
owed.

Not a phantom: `T_E_DEVGONE` appears in errno.h only as a COMMENT naming the
NODEV device-gone class, not a define -- excluded. `T_E_2BIG` (7) is a real
define but comment-marked "accidental"/environment-bounds-only; documented as
such. Append-only respected: no value renumbered, only rows added for values
errno.h already pins. `updated:` -> 2026-08-02 -> 2026-09-06.
