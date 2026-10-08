SUPERSEDED 2026-10-08: astra t71 approved a REVISED form (adds an explicit disarm, an armed-early-refusal check,
separate CHARGE/GUARD assertions, independent mutants and a KERNEL_TESTS-off shape measurement). The
implementation in kernel/loom.c + kernel/test/loom_private_fixture.h + layout-leg-run.sh is the record; this
sketch is kept only as the proposal she reviewed.

(b) SEAM PROPOSAL -- DRAFT, to bring to astra only AFTER (a) has run (her t67 order)

The edge: loom_create_private's caller-side unwind when loom_create_layout returns
NULL after the charge: uncharge(metadata + backing), then addrspace_private_end.
loom_create_layout's OWN inner unwind (kfree(l) when burrow_create_anon fails) is
shared with public loom_create and is NOT this edge.

Shape, following the tree's precedent (#ifdef KERNEL_TESTS test support in
production files: sched.c need_resched accessors, exec.c icache recorder,
irqfwd.c dispatch) -- compiled OUT of the production shape:

  #ifdef KERNEL_TESTS
  static bool g_loom_private_layout_fault;
  void loom_private_fail_next_layout_for_test(void);   // arm, one shot
  bool loom_private_layout_fault_armed_for_test(void); // observe (reset proof)
  static bool loom_private_layout_fault_take(void) {   // consume
      return __atomic_exchange_n(&g_loom_private_layout_fault, false, __ATOMIC_RELAXED);
  }
  #else
  static inline bool loom_private_layout_fault_take(void) { return false; }
  #endif

  In loom_create_private: `bool layout_fault = loom_private_layout_fault_take();`
  as the FIRST statement (consumed by this call whether or not it gets as far as
  the layout, so an earlier refusal cannot leave it armed for a later call), and
  `struct Loom *l = layout_fault ? NULL : loom_create_layout(...)` at the site.

ISOLATION, argued and then asserted:
  - only loom_create_private reads it; loom_create / loom_create_with_receipts
    (public) never do; no kmalloc, slab, burrow or phys path is touched;
  - only the fixture calls loom_create_private (no syscall reaches it), so no
    unrelated admission can consume the shot;
  - the fixture asserts the flag is CLEAR after the call (consumed), and
    `done:` disarms unconditionally on every exit -- reset on all paths.

THE LEG: arm; admit on a fresh, single-owner, non-exempt space with ample cap;
assert NULL; assert page_count, private_rings, addrspace_ref_count and the owner
count all at baseline; assert the shot consumed.

RED: TWO one-site mutants, each run separately, because the unwind has two halves
and either alone leaks differently:
  M1 delete the uncharge  -> page_count stays up by metadata + backing
  M2 delete private_end   -> private_rings 1 and the lifetime ref leaked
Each must FAIL the leg's own baseline assertion by its exact message; the
refusal-leg oracle generalises (WANT per mutant).

OPEN QUESTIONS FOR ASTRA: is one combined baseline assertion acceptable, or does
she want the charge and the guard asserted separately so M1 and M2 fail with
DIFFERENT messages (stronger attribution, my lean)? And does the production-shape
claim need a measured byte comparison of loom.o with KERNEL_TESTS off, or is the
compiled-out #else sufficient?
