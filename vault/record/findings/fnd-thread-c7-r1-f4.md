---
id: fnd-thread-c7-r1-f4
type: fnd
round: adt-thread-c7-r1
severity: P3
status: fixed
title: "The manual's thread-listing gate overstated CAP_DEBUG"
surface: [sub-kernel-devproc]
threatens: [inv-i39]
fixed-by: chg-2026-10-29-thread-model-c7
created: 2026-10-29
---
## Prosecution

Manual 13 said a CAP_DEBUG holder may list any process's threads; the gate's debug axis refuses kproc and NOTRACE, so it lists only a process it could debug. Fixed. The finding's second half -- that kstack also refuses a sealed process -- is WITHDRAWN: `devproc_kind_is_image` does not list PQS_KSTACK, so the seal check in the kstack walk returns false; kstack belongs to NOTRACE, which the manual already says (round 2 re-derived this).
