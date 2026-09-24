---
id: fnd-b1c-r1-sa4
type: fnd
title: "The streaming filters kept reading after their reader left: yes | grep y | head -1 never ended"
round: adt-b1c-r1
severity: P2
status: fixed
surface: [sub-coreutils-lib, sub-coreutils-filters, sub-coreutils-presenters]
threatens: []
fixed-by: chg-2026-09-24-b1c-round1-close
regression: "coreutil-smoke's reader-leaves checks: the filter must exit within 20 s of its reader leaving an endless producer; the stop half sabotaged (grep, cut and uniq ignoring the failed write, tee never breaking) RED by name in four checks, cat unsabotaged as the control; stream.a_stop_reads_no_further, stream.a_stopped_run_is_the_last"
created: 2026-09-24
---
## Prosecution

Found by the main session's self-audit, beside F2. `grep`, `cut` and `uniq`
read through `stream::lines`, whose closures returned true whatever happened to
stdout, so after the reader left they kept reading; on an endless producer they
never ended, and `yes | grep y | head -1` hung where main, holding the slurp's
cap, had ended at 2 MiB with an error. `OutSink` stopped writing at the first
failure, but nothing stopped the reading.

## Disposition

Fixed with F2. Every streaming closure returns `!out.failed()`, the file loops
break on it, and `tee` breaks once stdout and every file have failed. uniq's
grouping moved into the library as `stream::runs`, where a stop is host-tested.
