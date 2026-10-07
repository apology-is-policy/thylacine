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
    // MUTANT -- remove the lifetime_get in addrspace_private_begin and the
    // matching put in addrspace_private_end, and the retirer reaches a freed
    // descriptor through this leg while every leg above stays green.
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
    test_proc_drop(p); p = NULL; fd = -1;
    LP_CHECK(lp_wait(goal),
             "a reaped creator's ring retires on the ring's own image reference");

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
    if (p) test_proc_drop(p);
    if (!lp_wait(goal) && !error) error = "private fixture cleanup retirement timed out";
    if (pin) addrspace_unpin(pin);
    return error;
}

#undef LP_CHECK
#endif
