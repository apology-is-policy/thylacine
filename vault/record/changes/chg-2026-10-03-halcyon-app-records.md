---
id: chg-2026-10-03-halcyon-app-records
type: chg
title: "Prepare exact HIN1 replay and shared buffer budgets"
date: 2026-10-03
arc: arc-halcyon-interaction
commits: []
touched: [sub-halcyond, sub-halcyond-service-wire, sub-halcyond-interaction-record]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
Prepare per-fid HIN1 assembly, exact replay and incarnation-bound completion;
retain exact observed terminal foreground for registration. Expose actual
transport capacity and remaining-budget hooks so the application adapter can
charge its caches against the same approved allowance. HI1-R24 remains open
for that complete caller/ledger. Review reproduced and fixed collapsed error
codes (HI1-R25); failed/red harness logs remain in work/oct3-hi-apprecords.

538 host tests and47 intended mutations pass. Preliminary native boot1830/1830
and service-wire36.09s pass; its source delta to the final record error fix is
pinned. The final rebuilt image passes media71.74s and physical F10SAK89.66s;
1280x800 captures inspected. No new native clipboard or freshSMP/sanitizer,
Pi/minimum-display qualification. Public dispatch stays off; Main is unchanged.
The remaining app dispatcher, HSC cache/output barrier, total allocation ledger,
native two-client checks and modal UI remain in the status note. Single-agent
self-review, original protected drafts preserved.
