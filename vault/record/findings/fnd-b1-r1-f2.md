---
id: fnd-b1-r1-f2
type: fnd
title: "Cached-open comments overclaimed 'server-fresh'/'REFILLS' for loose clients"
round: adt-b1-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-dev9p]
threatens: []
fixed-by: chg-2026-07-11-b1-loose
regression: ""
created: 2026-07-31
---
## Prosecution

The resolver-side cached-open comment and the dev9p step-1 comment both
asserted a server-fresh revalidation unconditionally — true for strict
mounts, false for the new loose mode (whose whole point is skipping the
forced-wire hint). A reader auditing coherence from the comments would
credit loose mounts with a guarantee they deliberately trade away.

## Disposition

Fixed: both comments scoped to the strict path. The claim-in-a-comment
class — exactly as unverified as a claim in chat.
