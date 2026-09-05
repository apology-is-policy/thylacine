---
id: fnd-66a-r1-f1
type: fnd
title: "#66a F1: the fd2path 'never wrong' doc overclaim"
round: adt-66a-r1
severity: P3
status: fixed
surface: [sub-kernel-path]
threatens: []
fixed-by: chg-2026-06-12-66a-spoor-path
created: 2026-08-01
---
## Prosecution

fd2path returns the path the Spoor was REACHED by, not a live lookup —
a later rename/unmount leaves it STALE, so "never wrong" invited the
re-open-by-string TOCTOU footgun (resolve the returned string and get a
different object).

## Disposition

Fixed (doc): softened across syscall.h + libt + libthyla-rs to "may be
unknown OR stale; not a re-open key" — I-33's honest envelope, now the
[[sub-kernel-path]] Contract line.
