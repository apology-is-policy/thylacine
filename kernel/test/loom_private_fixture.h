// Empty private-owner infrastructure; no scope protocol or public setup claim.
//
// The checks here are the reviewed set from the paused owner-integration draft
// (astra, yip 0161), reformatted to this tree's style, plus the final/nonfinal
// retirement discrimination at the end -- which the draft's set did not cover,
// because every retirement in it ends the ring's occupancy and so refunds the
// whole charge. Scheduling is FORCED by this fixture (handles are opened and
// closed directly, and lp_wait spins on the retirer's counter); none of it
// demonstrates reachability from a syscall pair, and no claim of that is made.
#ifndef LOOM_PRIVATE_FIXTURE_H
#define LOOM_PRIVATE_FIXTURE_H

#include <thylacine/addrspace.h>
#include "../../arch/arm64/uart.h"
#include <thylacine/vma.h>
// A VA for the surviving-mapping leg, clear of the 0x140000000 range the
// SQPOLL fixtures in this file use.
#define LP_RING_VA 0x150000000ull

#define LP_CHECK(x, msg) do { if (!(x)) { error = msg; goto done; } } while (0)

// The charge-settling range detach, with the discipline vma_detach_range_in
// requires: as->lock held across the call, and the returned chain of Burrows
// whose last mapping went handed to burrow_free_deferred AFTER the unlock,
// because a FILE Burrow's free may sleep. `payer` is what makes this settle at
// all -- passing NULL settles nothing and leaves an eager region charged, which
// is the safe direction and the wrong one for this fixture.
static bool lp_detach_settling(struct Proc *p, u64 vaddr, u32 length) {
    struct Burrow *dead = NULL;
    spin_lock(&p->as->lock);
    int rc = vma_detach_range_in(p->as, proc_resource_exempt(p), p,
                                 vaddr, (u64)length, 0, &dead);
    spin_unlock(&p->as->lock);
    burrow_free_deferred(dead);
    return rc == 0;
}

// The retirer is a separate thread, so every retirement here is observed by
// waiting on its monotonic counter rather than assumed complete on return.
static bool lp_wait(u64 target) {
    u64 deadline = timer_now_ns() + 5000000000ull;
    while (loom_private_retired() < target && timer_now_ns() < deadline) sched();
    return loom_private_retired() >= target;
}

static const char *loom_private_fixture(void) {
    const char *error = NULL;
    struct Proc *p = test_proc_make();
    struct Proc *tight = NULL;
    struct Loom *refused = NULL;
    struct Loom *l = NULL;
    struct Handle borrow = {0}, second = {0};
    struct AddrSpace *pin = NULL;
    hidx_t fd = -1, rd = -1, wr = -1;
    u64 mapped_va = 0;
    u32 mapped_len = 0;
    u64 goal = loom_private_retired();

    LP_CHECK(p, "private creator allocated");
    u32 initial = p->as->page_count;

    // Rollback: a refused geometry must leave neither a charge nor a guard.
    LP_CHECK(!loom_create_private(p, 3, 4, true),
             "invalid private geometry refuses before charge");
    LP_CHECK(p->as->page_count == initial && !p->as->private_rings,
             "bad geometry has no guard or charge");

    // The image must be exclusively owned at admission.
    LP_CHECK(addrspace_try_ref(p->as), "second image owner acquired");
    l = loom_create_private(p, 2, 2, true);
    addrspace_unref(p->as);
    LP_CHECK(!l, "shared image refuses private owner");

    l = loom_create_private(p, 2, 2, true);
    if (l) goal++;
    LP_CHECK(l, "empty private owner admitted");
    u32 charged = p->as->page_count;
    LP_CHECK(charged == initial + l->service_metadata_pages +
                        (u32)burrow_backing_pages(l->ring_size) + 1u,
             "ring and metadata charged before publication");

    // Charge refusal: the guard addrspace_private_begin took must be released
    // on the refusal, BOTH halves of it. A leaked guard also holds a lifetime
    // reference, so the final drop's private-ring check never runs and nothing
    // else would notice; a split defect could clear the count and keep the
    // reference, so the reference is checked on its own. Every precondition
    // that routes this call to the charge branch is asserted, and the bound is
    // checked against the admission just measured above, not a hand count: the
    // same geometry was admitted on p, so only the cap differs.
    tight = proc_alloc_in(NULL, 1);
    LP_CHECK(tight && tight->as, "budget-bound creator allocated");
    u32 tight_initial = tight->as->page_count;
    int tight_refs = addrspace_ref_count(tight->as);
    LP_CHECK(addrspace_owner_count(tight->as) == 1 && !tight->as->private_rings &&
             !proc_resource_exempt(tight),
             "budget-bound creator is a fresh, single-owner, non-exempt image");
    LP_CHECK(tight_initial + (charged - initial) > tight->as->page_budget,
             "the bound cannot cover the admission it must refuse");
    refused = loom_create_private(tight, 2, 2, true);
    // Counted before the check, as every other ring here is: an admission that
    // wrongly succeeds still retires at cleanup, and the wait must cover it.
    if (refused) goal++;
    LP_CHECK(!refused, "over-budget private owner refused");
    LP_CHECK(tight->as->page_count == tight_initial && !tight->as->private_rings &&
             addrspace_ref_count(tight->as) == tight_refs &&
             addrspace_owner_count(tight->as) == 1,
             "refused charge leaves no guard, reference or charge");

    bool shared = addrspace_try_ref(p->as);
    if (shared) addrspace_unref(p->as);
    LP_CHECK(p->as->private_rings == 1 && !shared,
             "live private image refuses sharing");
    LP_CHECK(l->service_as == p->as && l->service_creator == p->stripes,
             "exact image and permanent creator captured");

    // Public setup and legacy execution stay refused on a private owner.
    LP_CHECK(loom_register_handles(l, NULL, NULL, 0) == -T_E_OPNOTSUPP &&
             loom_register_buffers(l, p, NULL, 0) == -T_E_OPNOTSUPP &&
             loom_start_sqpoll(l) == -T_E_OPNOTSUPP &&
             loom_enter(l, 0, 0, 0) == -T_E_OPNOTSUPP,
             "private owner refuses legacy execution");

    fd = handle_alloc(p, KOBJ_LOOM, RIGHT_READ | RIGHT_WRITE, l);
    LP_CHECK(fd >= 0, "private table owner installed");
    l = NULL;

    // A returned BORROW is not a close: the distinction the release wrapper
    // rests on, asserted rather than left to the call graph.
    LP_CHECK(!handle_get(p, fd, &borrow) && !handle_get(p, fd, &second),
             "private borrowed refs retained");
    l = borrow.obj;
    handle_put(&second);
    LP_CHECK(!l->service_closing, "borrow release does not close admission");

    LP_CHECK(!handle_close(p, fd), "table owner close");
    fd = -1;
    LP_CHECK(l->service_closing && loom_enter(l, 0, 0, 0) == -T_E_CANCELED &&
             (loom_poll(l, POLLIN, NULL) & POLLHUP),
             "owner close latches and exposes hangup");
    LP_CHECK(p->as->page_count == charged && p->as->private_rings == 1,
             "borrow retains charge and sharing guard after close");
    l = NULL;
    handle_put(&borrow);
    LP_CHECK(lp_wait(goal) && p->as->page_count == initial &&
             !p->as->private_rings,
             "last borrow retires and refunds exact image");

    // dup2 destination removal is ownership release, even though its source is
    // an ordinary pipe; the source object itself keeps its usual semantics.
    l = loom_create_private(p, 2, 2, false);
    if (l) goal++;
    LP_CHECK(l, "private dup destination created");
    fd = handle_alloc(p, KOBJ_LOOM, RIGHT_READ | RIGHT_WRITE, l);
    LP_CHECK(fd >= 0, "dup destination installed");
    l = NULL;
    LP_CHECK(!handle_get(p, fd, &borrow), "dup destination borrowed");
    LP_CHECK(!sys_pipe_for_proc(p, &rd, &wr), "dup pipe source created");
    LP_CHECK(handle_dup_to(p, rd, fd, false) == fd &&
             ((struct Loom *)borrow.obj)->service_closing,
             "dup overwrite closes private destination");
    handle_put(&borrow);
    LP_CHECK(lp_wait(goal), "overwritten private owner retires");
    handle_close(p, fd); fd = -1;
    handle_close(p, rd); rd = -1;
    handle_close(p, wr); wr = -1;

    // Exec closes a private ring whether or not userspace marked it CLOEXEC.
    l = loom_create_private(p, 2, 2, true);
    if (l) goal++;
    LP_CHECK(l, "private exec fixture created");
    fd = handle_alloc(p, KOBJ_LOOM, RIGHT_READ | RIGHT_WRITE, l);
    LP_CHECK(fd >= 0, "exec private owner installed");
    l = NULL;
    LP_CHECK(!handle_set_cloexec(p, fd, false) && !handle_get(p, fd, &borrow),
             "exec borrow with CLOEXEC cleared");
    handle_private_exec_latch(p);
    LP_CHECK(((struct Loom *)borrow.obj)->service_closing,
             "pre-swap exec latch closes admission");
    LP_CHECK(handle_close_on_exec(p) == 1,
             "private exec removal ignores cleared CLOEXEC");
    fd = -1;
    handle_put(&borrow);
    LP_CHECK(lp_wait(goal), "exec private owner retires");

    // Actual Proc destruction while a transient object borrow remains. The AS
    // descriptor pin is kept separately only to inspect its final charge.
    l = loom_create_private(p, 2, 2, true);
    if (l) goal++;
    LP_CHECK(l, "late borrow creator allocated");
    fd = handle_alloc(p, KOBJ_LOOM, RIGHT_READ | RIGHT_WRITE, l);
    LP_CHECK(fd >= 0, "late borrow owner installed");
    l = NULL;
    LP_CHECK(!handle_get(p, fd, &borrow), "late borrow acquired");
    pin = p->as;
    addrspace_pin(pin);
    test_proc_drop(p); p = NULL; fd = -1;
    LP_CHECK(!addrspace_owner_count(pin) && pin->private_rings == 1 &&
             ((struct Loom *)borrow.obj)->service_closing,
             "creator reaped before private borrow");
    handle_put(&borrow);
    LP_CHECK(lp_wait(goal) && !pin->private_rings && !pin->page_count,
             "late retirement refunds original image only");
    addrspace_unpin(pin); pin = NULL;

    // ---- The image reference the RETIRER depends on, with nothing else
    // holding it up. ----
    // The "creator reaped before private borrow" leg above keeps its own
    // addrspace_pin across the whole window, and it has to: it inspects
    // pin->page_count and pin->private_rings after the reap. But that pin is the
    // SAME addrspace_lifetime_get that addrspace_private_begin takes, so it
    // MASKS the property the retirer actually depends on -- with a second
    // lifetime reference held, a ring that took none would still find its image
    // addressable. This leg holds NONE, which is the only way to put the ring's
    // own reference under load.
    //
    // So it asserts nothing whatever about the image: touching `as` here would
    // reintroduce exactly the reference being tested. It observes the
    // retirement only through the monotonic counter, and its witness is the
    // MUTANT -- but the mutant's expected outcome is a NAMED invariant failure,
    // not an arbitrary crash (astra 0161 t53, checked against the source).
    // Remove the lifetime_get in addrspace_private_begin and the matching put
    // in _end while keeping ++private_rings, and the owner's drop inside
    // proc_free becomes the FINAL lifetime drop with a private ring still
    // guarded -- which addrspace_lifetime_put extincts on by name, "AddrSpace
    // final lifetime drop with private rings" (addrspace.c:127). The guard
    // fires in the DYING PROC, before the retirer could reach a freed
    // descriptor.
    //
    // Which says what this leg's content really is: it is the ONLY leg where
    // the owner's drop IS the final lifetime drop while a ring is outstanding.
    // Under the pinned leg above, the fixture's own reference makes that drop
    // non-final, so the guard cannot fire there and the mutant stays invisible.
    // WHAT THIS LEG DOES NOT WITNESS. The mutant below proves the ring TAKES an
    // image reference; nothing here proves it RELEASES one. A page-granular
    // gauge looked like the answer -- the final lifetime drop is also what runs
    // proc_pgtable_destroy, so a reference never released strands whole PAGE
    // TABLES, which phys_free_pages does see -- and it was written, then taken
    // back out, because its instrument cannot be made sound HERE. phys_free_pages
    // reports g_zone0.total_free_pages alone (mm/phys.c:267) and an order-0 free
    // goes to a per-CPU magazine instead, flags cleared, "magazine ownership, not
    // free list" (mm/magazines.c:134), so the reading exists only after
    // magazines_drain_all -- which walks EVERY CPU's magazine with no lock and no
    // IRQ mask (mm/magazines.c:154). The tree's other page-accounting tests free
    // on the CPU they measure from, so for them that cross-CPU pass is a hazard
    // only. This leg cannot: the retirer frees the dying image on whatever CPU it
    // ran on, which makes the cross-CPU pass LOAD-BEARING for the reading, in a
    // suite that runs after smp_init (main.c:689 vs :874) with kthreads runnable.
    // A measurement whose instrument needs a quiescence this fixture cannot
    // establish is not a weaker witness but a different claim. So release stays
    // OPEN and recorded, and restoring it wants a quiescent drain or a production
    // counter, plus an omitted-put-only mutant to show the gauge can redden at
    // all -- the balanced mutant tests acquisition (astra, yip 0161 t55; the
    // symmetric discipline the tree does encode is test_cow.c:249-253).
    p = test_proc_make();
    LP_CHECK(p, "unpinned-reap creator allocated");
    l = loom_create_private(p, 2, 2, true);
    if (l) goal++;
    LP_CHECK(l, "unpinned-reap private owner admitted");
    fd = handle_alloc(p, KOBJ_LOOM, RIGHT_READ | RIGHT_WRITE, l);
    LP_CHECK(fd >= 0, "unpinned-reap owner installed");
    l = NULL;
    // proc_free releases the image BEFORE it frees the handle table, so from
    // this call onward the ring's own reference is the only thing keeping the
    // descriptor addressable -- and the drop that ends the ring's occupancy is
    // the table teardown's, on the dying Proc, not a close on a live one.
    // The delta is taken from the counter, with its precondition asserted
    // rather than assumed: every earlier leg ends in its own lp_wait, so
    // nothing should be in flight here, and if something is then the delta
    // below would be satisfied by someone else's retirement.
    u64 before = loom_private_retired();
    LP_CHECK(before + 1 == goal, "no retirement in flight at the snapshot");
    // ARRIVAL MARKER. Line order is not execution order: LP_CHECK is `goto
    // done`, and the cleanup at `done:` unrefs the ring and drops the owner too,
    // so under a mutant that strips the ring's image reference ANY earlier check
    // failure reaches an owner drop with a ring outstanding and produces the
    // SAME named extinction -- hiding the check that actually failed. A mutant
    // run therefore cannot attribute its death to this operation by position.
    // This marker is the proof of arrival (astra, yip 0161 t61).
    uart_puts("[lp-mark] unpinned-reap-owner-drop\n");
    test_proc_drop(p); p = NULL; fd = -1;
    LP_CHECK(lp_wait(goal),
             "a reaped creator's ring retires on the ring's own image reference");
    // lp_wait only establishes `>= goal`, which is EVENTUAL retirement. Exactly
    // once needs the delta, and it has to come from the counter: the image is
    // gone, and reading it is the masking this leg exists to avoid.
    LP_CHECK(loom_private_retired() == before + 1,
             "the reaped creator's ring retires exactly once");

    // ---- The final / nonfinal ring-drop discrimination. ----
    // Every retirement above ends the ring's occupancy, so each refunds the
    // whole charge -- which means an implementation that refunded
    // UNCONDITIONALLY satisfies all of them. This leg is the one that separates
    // the two: a user mapping of the ring outlives the fd, so the handle drop at
    // retirement is NONFINAL, the settled drop must report a zero refund, and
    // only the metadata may come back. The ring's own charge stays recorded on
    // the Burrow until the mapping teardown settles it.
    p = test_proc_make();
    LP_CHECK(p, "nonfinal-drop creator allocated");
    u32 base = p->as->page_count;
    l = loom_create_private(p, 2, 2, true);
    if (l) goal++;
    LP_CHECK(l, "nonfinal-drop private owner admitted");
    u32 meta_pages = l->service_metadata_pages;
    mapped_len = l->ring_size;
    LP_CHECK(burrow_map(p, l->ring, LP_RING_VA, mapped_len, VMA_PROT_RW) == 0,
             "ring mapped into the creator image");
    mapped_va = LP_RING_VA;
    // Measured, not predicted: whether burrow_map itself charges is not this
    // fixture's subject, so the baseline is taken after the map.
    u32 with_mapping = p->as->page_count;
    fd = handle_alloc(p, KOBJ_LOOM, RIGHT_READ | RIGHT_WRITE, l);
    LP_CHECK(fd >= 0, "nonfinal-drop owner installed");
    l = NULL;
    LP_CHECK(!handle_close(p, fd), "nonfinal-drop owner close");
    fd = -1;
    LP_CHECK(lp_wait(goal), "nonfinal-drop owner retires");
    // THE discriminating assertion: the metadata came back, the ring's pages did
    // not, because they still carry a mapping. An unconditional refund lands
    // here one ring's worth of pages lower.
    LP_CHECK(p->as->page_count == with_mapping - meta_pages,
             "a nonfinal ring drop refunds the metadata only");
    LP_CHECK(!p->as->private_rings,
             "the image pin is released at retirement even when the ring lives");
    // The detach must be the CHARGE-SETTLING one. burrow_unmap passes no payer,
    // so it reaches vma_free_freed and settles nothing: the last mapping would
    // free the ring and leave the recorded backing charged forever, and this
    // leg's closing assertion would fail for a reason that has nothing to do
    // with the retirement under test (astra, yip 0161 note 32). burrow_unmap's
    // semantics are deliberately NOT changed to suit this fixture -- the JIT and
    // the other callers settle separately and rely on exactly that.
    LP_CHECK(lp_detach_settling(p, mapped_va, mapped_len),
             "the surviving ring mapping detaches through the settling path");
    mapped_va = 0;
    LP_CHECK(p->as->page_count == base,
             "the mapping teardown settles the ring charge exactly once");

done:
    // l sometimes aliases borrow only for inspection; never drop it twice.
    if (l && borrow.magic == HANDLE_MAGIC && l == borrow.obj) l = NULL;
    if (l) loom_unref(l);
    handle_put(&second);
    handle_put(&borrow);
    if (p && mapped_va) (void)lp_detach_settling(p, mapped_va, mapped_len);
    // The cleanup's own drop, marked and ATTRIBUTED: if a mutant dies here the
    // extinction is the cleanup's, not the leg's, and the check that sent us to
    // `done:` would otherwise never reach the log because the boot ends before
    // the suite can report `error`.
    // `done:` is ALSO the normal fallthrough from the last check, so the marker
    // says WHICH of the two arrivals this is. An oracle that distinguished them
    // by the ABSENCE of a suffix reads a healthy run as a failing one, which is
    // exactly what the first version did (astra, yip 0161 t63).
    uart_puts("[lp-mark] cleanup-owner-drop ");
    if (error) { uart_puts("after-check-failure: "); uart_puts(error); }
    else uart_puts("normal-fallthrough");
    uart_puts("\n");
    if (refused) loom_unref(refused);
    // A refusal that leaked its whole guard is released here, so the defect
    // fails its own assertion and cannot outlive the leg into later tests. A
    // reference leaked WITHOUT the count is not repaired; it leaks the space,
    // which is the safe direction.
    if (tight && !refused && tight->as && tight->as->private_rings)
        addrspace_private_end(tight->as);
    if (tight) test_proc_drop(tight);
    if (p) test_proc_drop(p);
    if (!lp_wait(goal) && !error) error = "private fixture cleanup retirement timed out";
    if (pin) addrspace_unpin(pin);
    return error;
}

#undef LP_CHECK
#endif
