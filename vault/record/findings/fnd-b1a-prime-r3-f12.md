---
id: fnd-b1a-prime-r3-f12
type: fnd
title: "The copy-on-write copy branch releases its share of the original while the faulter's own stale read-only leaf still translates to it -- the other holder, sole, can take the page in place and write into what a sibling thread still reads, or exit and free it under a live leaf"
round: adt-b1a-prime-r3
severity: P1
status: fixed
surface: [sub-kernel-fault, sub-kernel-mmu]
threatens: [inv-i44, inv-i13]
fixed-by: chg-2026-09-23-b1a-prime-close-r3
regression: "cow.break_copy_pins_the_original_until_replaced"
created: 2026-09-23
---
## Prosecution

The round-2 fix (F9) removed the unconditional `mmu_uninstall_user_pte` that
ran at the top of the write arm and replaced it with a leaf replace at the END
of the fault. Between the two the faulting address space still holds the
read-only leaf a preceding read installed, and peer CPUs may hold it in their
TLBs -- and the copy branch put its share of the original right after the
slot swap. Chain 1 (I-44): P has threads T1 and T2, P forked C, page X shared
(count 2); T2 read X earlier (a valid RO leaf naming X in P's tree); T1 writes,
not sole, copies, swaps, puts -- count 1, C is sole; before T1's replace, C's
thread writes X: sole, takes in place, writes a secret into X; T2 reads P's
VA through the still-valid RO leaf and sees C's private write. Nothing in P's
fault path stops T2: a read of a valid leaf never enters the kernel. Chain 2
(I-13): C exits between T1's decide and its put; C's drain puts X to 1; T1's
own put then frees X while P's L3 still holds a valid RO leaf naming it and
T2's TLB may too -- any CPU allocating in the window gets X for kernel use.
Under the old flow both chains were impossible by construction: the
uninstall and its TLBI ran before the decide, so a peer read faulted and
serialized on `as->lock`. A regression introduced by the F9 fix.

## Fix

The share is PINNED until the leaf has been replaced -- the order Linux's
`wp_page_copy` keeps (`put_page(old_page)` after `set_pte_at`). `struct page
*cow_release` beside the arm's flags; the copy branch sets `cow_release =
resident` where it used to put; after step 5, on the failed-replace path
too, `if (cow_release && cow_page_put(cow_release)) free_pages(cow_release,
0)`. With the count held at two across the window C cannot take X in place
and X cannot be freed; a peer read through the stale RO leaf sees the
pre-copy bytes, which equal the copy's until the faulter writes after the
replace. A tests-only probe (`g_cow_copy_probe_for_test`, `KERNEL_TESTS`)
fires between the swap and the replace; the regression test asserts the
CONTROL (the faulter's leaf still names the original at the probe -- the
window is real) and the claim (the share is still two there, one after).
RED `nocowpin` (the early put restored) reddens it.
