---
id: fnd-term4-r1-f3
type: fnd
title: "The wstat invalidation comment overclaimed 'truncate/size'"
round: adt-term4-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-dev9p]
threatens: []
fixed-by: chg-2026-07-14-term4-close
regression: ""
created: 2026-07-31
---
## Prosecution

`dev9p_wstat_native`'s invalidation comment claimed the attr drop covers
"truncate/size" — but the handler never sets a size attribute (content
truncate is OTRUNC-routed via the open path, which also drops pages). A
reader auditing truncate coherence from the comment would credit the
wrong site.

## Disposition

Fixed: the comment corrected — the attr-only invalidate at wstat is
CORRECT for what wstat actually does (mode/uid/gid/times); the truncate
page-drop lives at the OTRUNC open (the D44-F3 rule). Comment-drift
class.
