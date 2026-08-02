---
id: seam-791-smp1-joey
type: seam
title: "joey exits non-zero in ~45% of `-smp 1` boots"
status: open
surface: [sub-substrate-machine, sub-substrate-gates]
opened-by: chg-2026-08-01-substrate-sweep
tracker: "#791"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

Root-cause and fix for an smp1-specific init failure. It is a real,
reproducible guest defect at a documented rate, not a harness artifact.

## Why it matters more than its configuration suggests

`-smp 1` is otherwise the attractive CI configuration: it is fast and it has
**no boot-time variance at all** (0.39 s spread, versus the bimodal
19–26 s / 33–37 s distribution at `-smp 4` caused by macOS placing TCG vCPU
threads across P-cores and E-cores). That variance is what forces the 90 s
default timeout.

So this bug is what keeps CI on the slower, noisier configuration — and the
second reason is independent and permanent: `-smp 1` loses in-kernel SMP
coverage entirely, which is the coverage the whole multi-boot gate exists
to provide.

## Risk while open

`THYLACINE_TEST_CPUS=1` is documented as available but explicitly not to be
relied on for CI. Anyone reaching for it to escape boot-time variance will
see a ~45% failure rate that is not their change.
