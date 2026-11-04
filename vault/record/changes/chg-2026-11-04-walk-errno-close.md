---
id: chg-2026-11-04-walk-errno-close
type: chg
title: "The walk errno close: rounds 1 and 2, the first boots, 32 sabotages"
date: 2026-11-04
arc: arc-phase6-pouch
commits: ["5d0028b85", "b8c8116c5", "6ca029353"]
touched:
  - sub-kernel-stalk
  - sub-kernel-dev
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-04
---
The close of [[chg-2026-11-03-walk-errno]], whose record fields were fixed when it was committed. Audit rounds 1 and 2 (Fable 5.1, start == end, each on a frozen copy): 0/0/1/4 and 0/0/0/1. Two first boots stopped on stale pins of the flat -1 this chunk replaced, fixed in 5d0028b85 and b8c8116c5. Lease 1 on b8c8116c5: suite 2201/0; the Pouch spawn leg and the Linux phenotype leg pass; 27 of 29 kernel sabotages red at their named lines; G01 at viv-pheno-probe L351; P01 at posix_spawn(miss). Green, each read: E07 has no observable difference, since stalk_union_child is static and its one caller reads the reason only on a NULL; E11 was a test gap, closed by 6ca029353. Lease 2, 10:30:54Z to 10:34:12Z, on 6ca029353: suite rc 0, 2201 tests, with the new check in; E11 red at `stalk.walk_failure_keeps_its_errno`, "the next one's miss has no reason left over". G02 went red in the kernel tests and so never reached its guest line. P01 is what shows that line can fail. Regreen ELF == green ELF. The SMP subset did not run on this chunk: its one new piece of state is a per-thread slot, and C8a carries the code and runs it by vote 27. The full SMP matrix, tools/test-fault.sh, the interactive fleet and check-production beyond what tools/test.sh does first did not run; they are owed at the merge to main under the operator's hold.
