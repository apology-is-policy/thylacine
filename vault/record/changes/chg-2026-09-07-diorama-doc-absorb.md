---
id: chg-2026-09-07-diorama-doc-absorb
type: chg
title: "absorb docs/reference/141-diorama (the synthetic Linux world): fold the SA-4 vDSO fast-path + the MIDR-0x00 harness lesson + diorama-probe into sub-diorama"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: [sub-diorama]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
diorama (the synthetic Linux /proc + /sys world). quaestor owner:
usr/diorama/src/{server,main}.rs -> sub-diorama (audit:hard, I-43), flagged STALE
(dossier 2026-08-15, code changed 2026-09-05 +324 lines); usr/diorama-probe ->
UNOWNED. Verified atom-by-atom.

CENSUS CORRECTION (a lesson): my first keyword census used `\|` under `grep -E`,
which reads it as a LITERAL not ERE alternation, so it falsely reported the core
security atoms absent (deputy/msize/readdir/self-only all "0"). Reading the FULL
dossier + a corrected ERE census showed sub-diorama is deep AND AHEAD of the
reference doc: it carries the two modes (default + --vivarium/viv-dio V-7 with
ppid-descent membership + the attach gate), the #182 buffer-half-the-kernel
finding, the whole V-4c-3 close (msize-underflow-terminates-server saturating
sub; walk-by-name-vs-enumeration cache-readdir; checked/saturating parsers; #71
walk-into-a-file; #72 environ-window), and the full section-6.2 deputy reasoning
(/self/environ sound vs /<pid>/environ absent). The reference doc treats V-7 as
FUTURE; the dossier has it built.

THE FOLD (genuine code-deltas the +324 lines carry that the 2026-08-15 dossier
lacked; depth rich; updated 2026-08-15 -> 09-07, clearing the STALE flag):
- SA-4 vDSO clock fast-path. Code-verified (server.rs:1170-1177 + 1760-1766):
  clock_pair_ns + render_uptime now go through libthyla_rs::time (the #343 vDSO
  page, a CNTVCT_EL0 read, no syscall) instead of a private t_timespec + raw
  t_clock_gettime, and derive both clocks from ONE counter sample (btime =
  realtime - monotonic; two samples would let a preemption leak in). Folded into
  Performance.
- The MIDR-0x00 legitimate-zero + harness lesson. Code-verified (server.rs cpuinfo
  midr: Option<u64>, MIDR_EL1 EL0-trapped -> /ctl/cpu column). QEMU TCG -cpu max
  reports MIDR 0x000f0510 (implementer 0x00, not a fault); a kernel test asserting
  non-zero cost a boot-fatal EXTINCTION because test.sh runs HVF -cpu host (Apple)
  while the interactive harness runs TCG -cpu max -- "a green test.sh is not a
  sufficient gate for a hardware-register assertion." Folded into Caveats.
- diorama-probe (UNOWNED orphan the selftest+probe proof story relies on) added to
  code:.

Redirect stub (single dossier). Render + lint verified. Zero code change.
