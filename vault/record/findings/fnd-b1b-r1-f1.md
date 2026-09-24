---
id: fnd-b1b-r1-f1
type: fnd
title: "The found bug was a mis-attribution: __init_tls's raw six-argument SYS_mmap2 never killed a Pouch program"
round: adt-b1b-r1
severity: P2
status: fixed
surface: [sub-pouch-mem, sub-pouch-seam]
threatens: []
fixed-by: chg-2026-09-23-b1b-pouch-memory
regression: "none: no code behaviour changed; the evidence is CL-4's on-device clang++ and the unit pin in test_sys_burrow.c"
created: 2026-09-24
---
## Prosecution

B-1b announced patch 0046 as the fix for a defect it had found: `__init_tls`
issued `SYS_mmap2` raw with Linux's six arguments, which the kernel's syscall
83 was said to misread. The kernel's arm has read exactly that shape since Clade
CL-4 (`burrow_lazy_len_from_args`, kernel/syscall.c, pinned by
test_sys_burrow.c), so no Pouch program ever died of it. The claim sat in binding
scripture (ARCH 6.5, POUCH-DESIGN 8.1), the audit-trigger row's title, the
journal, two dossiers, the change note, the patch series and their headers, the
in-file comments and memory, and the RED run scored as its witness measured
0044 without 0046 rather than the claimed defect.

## Disposition

Fixed in the close. Every site now says what is true: 0046 is required by
0044's parking of `__NR_mmap`, not by a kernel misreading. The CL-4 arm is kept,
since a Clade stage built before 0046 still sends the shape, with a comment that
says so. No code behaviour changed, so there is no regression test; retiring the
arm would be an ABI-shape change and is the operator's call. The lesson: a
boundary defect read from one side is a hypothesis until the other side's
handler for that exact call has been read.
