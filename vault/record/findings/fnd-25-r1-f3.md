---
id: fnd-25-r1-f3
type: fnd
title: "larder_page_invalidate is O(page_cap) — a full-array scan per own-write"
round: adt-25-r1
severity: P3
status: fixed
surface: [sub-kernel-larder]
threatens: []
fixed-by: chg-2026-07-11-fid-lifecycle
regression: larder.page_invalidate_multifile
created: 2026-07-31
---
## Prosecution

The own-write invalidate scanned every slot for the file's qid.path —
O(page_cap) per write. Tolerable at 512, noticeable at 8192, and a
~193-million-slot-scan tax at the coming 32768 cap on the write-heavy
cold path (gofmt-cold write ≈ 5.9k invalidates).

## Disposition

Dispositioned deferred at the round; CLOSED before the keeper committed
by the task-#29 secondary index (`page_qhash` keyed by qid.path alone +
the `qnext` chain — every page of one file shares one qbucket, so the
invalidate walks O(pages-of-file), independent of cap; the "in qhash IFF
in page_hash" lockstep invariant audited at adt-29-r1). Both the #25
rewrite and the #29 fix landed together in the fid-lifecycle keeper
commit. The index is what made the 128 MiB cap affordable.
