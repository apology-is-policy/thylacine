---
id: fnd-c10-r1-f4
type: fnd
round: adt-c10-r1
severity: P3
status: fixed
title: "The guard rested on -fstack-clash-protection passed through -Xclang"
surface: [sub-kernel-thread]
threatens: []
fixed-by: chg-2026-11-04-c10-r1
regression: "measured: 79 of 79 prologues moving SP by 4 KiB or more carry the probe"
created: 2026-11-04
---
## Prosecution

A move of nearly 16 KiB with no probe lands the frame in the slot below.

## Disposition

`-Wframe-larger-than=8192` with `-Werror`, and the probes measured in the ELF.
