---
id: msr-thread-storm
type: msr
title: "Thread spawn+join throughput (cpubench storm)"
metric: "cpubench storm 16 1000 thread spawn+join rate, device"
unit: threads/s
created: 2026-10-22
updated: 2026-10-22
---
## Method

`cpubench storm 16 1000` on the ci image: 1000 rounds of {spawn 16 trivial
workers, release them, join all}, so 16,000 thread spawn+joins in one Proc,
reported as threads/s. Two shapes, three rounds each, in one boot (hvf,
`-smp 4`, `-m 2048`): one storm alone, then four storms at once in four
Procs (`cpubench storm 16 1000 &;` four times, then `wait`). A row gives the
median, the range, and the image it was measured on; the pre-change image is
measured in the same session, so each step is a same-day A/B pair.

Caveats. One boot per image, so boot-to-boot variation is not sampled. Four
concurrent storms print their result lines at once and can interleave
mid-line, so a parser must match the rate, not the whole line. On both images
a storm runs faster beside three others than alone (about 1.6x on the
pre-C3 tip); the cause is not measured.

## Series
| date | value | chg |
|---|---|---|
| 2026-10-22 | alone 61,309 (57,222-62,766); four at once, per storm 96,613 (85,601-102,029); pre-C3 tip a0128885b, ELF sha256 59ca8a56 | [[chg-2026-10-22-thread-model-c3]] (baseline) |
| 2026-10-22 | alone 63,334 (60,990-68,567); four at once, per storm 146,176 (131,287-148,185), 1.51x; C3 e903aa208, ELF sha256 d7c5e853 | [[chg-2026-10-22-thread-model-c3]] |
