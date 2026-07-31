---
id: fnd-294-self-2
type: fnd
title: "any_outstanding_on_fid counted awaiting_flush -- the cancel's own Tflush refused the Tclunk"
round: adt-294-self
severity: P1
status: fixed
surface: [sub-kernel-ninep-session]
threatens: [inv-i10]
fixed-by: chg-2026-06-21-294-cancel-at-close
created: 2026-07-31
---
## Prosecution

The abandon's Tflush leaves the readiness oldtag awaiting_flush targeting
the fid; the SendClunk precondition counted it as live and REFUSED the
immediate Tclunk -- so the slot leaked ANYWAY, production-identical
(dev9p_close abandons then immediately clunks, before any Rflush clears
the tag). BELOW the teardown model's abstraction; the kernel test driving
the real wire path caught it.

## Disposition

Fixed: `any_outstanding_on_fid` EXCLUDES flushed entries -- a cancelled op
does not block a fid op (Tflush-then-Tclunk is the standard
cancel-then-close). Sound for all callers; the tag stays reserved until
its Rflush (I-10 untouched -- alloc_tag keys on `active`, orthogonal to
this LIVE-op precondition). The prosecutor cross-confirmed it IMPROVES
shared-client robustness (a dead Proc's awaiting_flush entry no longer
wedges another Proc's op on a shared fid). The #53-audit later extended
the same exclusion to the `abandoned` bit.
