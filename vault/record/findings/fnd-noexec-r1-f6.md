---
id: fnd-noexec-r1-f6
type: fnd
round: adt-noexec-r1
severity: P3
status: fixed
title: "The parent's look passed an ET_EXEC with PT_DYNAMIC, which no load takes"
surface: [sub-kernel-exec]
threatens: []
fixed-by: chg-2026-11-04-noexec-r1
regression: "execve.image_check_answers_for_the_parent: an ET_EXEC with PT_DYNAMIC"
created: 2026-11-04
---
## Prosecution

`exec_image_check` passed `ELF_LOAD_HAS_DYNAMIC` on the stated ground that its interpreter was the child's to resolve. It has none.

## Disposition

Refused. The comment and the dossier now say the look covers the header and `elf_load` only.
