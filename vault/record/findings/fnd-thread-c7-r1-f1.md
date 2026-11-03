---
id: fnd-thread-c7-r1-f1
type: fnd
round: adt-thread-c7-r1
severity: P1
status: fixed
title: "A cloned thread did not inherit its creator's note mask, and the first entry now delivers before its first instruction"
surface: [sub-kernel-syscall-dispatch, sub-kernel-notes]
threatens: [inv-i19]
fixed-by: chg-2026-10-29-thread-model-c7
regression: "first_entry.newborn_inherits_the_mask (both arms x masked/control); sabotages S11 (clone) and S15 (native) RED at phase e"
created: 2026-10-29
---
## Prosecution

`viv_clone_thread` never copied `note_mask` (the thread is KP_ZERO; the only copy was rfork's PHENO_LINUX fork arm), while ARCH 7.6.2 said clone copies it. musl blocks every signal across clone and the child restores its mask as its first act; since C7b every first entry takes the EL0-return tail, so a note every thread blocked was delivered to the newborn before that restore -- deterministically, where before it needed an interrupt inside a few instructions. Fixed by copying the mask before the admit links the thread; the native arm had the same exposure by documented contract, which operator vote V5 closed the same way ([[dec-2026-10-29-thread-inherits-mask]]).
