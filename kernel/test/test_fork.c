// LINEAGE L-3b: the fork half of rfork(RFPROC|RFMEM) -- child-context
// restoration and the SYS_RFORK argument gate.
//
//   fork.frame_init
//     The DECISION: a child's frame is its parent's frame with exactly two
//     edits (x0 := 0, sp := child_sp) and everything else verbatim. Split out
//     of thread_create_forked precisely so a test can reach it without needing
//     a Proc, an address space, or an EL0 thread.
//
//   fork.rfork_arg_rejection
//     SYS_RFORK refuses every malformed request BEFORE it can create anything.
//     Driven through the REAL handler with a synthetic frame -- the checks live
//     ahead of rfork_forked for exactly this reason, so a kernel test can reach
//     all of them from kproc, which has no address space.
//
// NOT covered here, and this is the chunk's central coverage fact: that the
// child actually RESUMES. Nothing kernel-side can observe an eret to EL0 --
// kproc has no address space by construction (addrspace.kproc_has_none) and no
// EL0 trapframe to fork from, so a "kernel-driven" resume is not merely
// untested but unobservable. The two claims split cleanly:
//
//   the DECISION (this file)          -- what the child's frame should contain
//   the RESUME   (/fork-probe, EL0)   -- that the frame is actually restored
//
// The in-guest probe is the returns-twice shape itself: a parent that calls
// SYS_RFORK and a child that finds itself in the same C function with x0 == 0,
// writing to a variable in the address space they now share. That single probe
// proves resume-at-the-parent's-PC, x0 == 0, its-own-stack, and the sharing all
// at once, and no kernel test can prove any of them.

#include "test.h"

#include <thylacine/burrow.h>
#include <thylacine/handle.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/exception.h"
#include "../../arch/arm64/uaccess.h"

void test_fork_frame_init(void);
void test_fork_rfork_arg_rejection(void);

// The real handler (the sys_pci_claim_handler pattern in test_allowance.c).
extern s64 sys_rfork_handler(struct exception_context *ctx);

// A recognisable non-zero pattern per field, so a copy that silently drops or
// transposes one is visible rather than plausible.
static void fill_frame(struct exception_context *f) {
    for (int i = 0; i < 31; i++) f->regs[i] = 0xA0000000ull + (u64)i;
    f->sp   = 0x0000BEEF00001000ull;
    f->elr  = 0x0000000040001234ull;
    f->spsr = 0x60000000ull;          // NZCV live: EL0t, N+Z set
    f->esr  = 0x56000000ull;          // EC 0x15 == SVC from EL0
    f->far  = 0x0000000000000000ull;
}

void test_fork_frame_init(void) {
    struct exception_context parent;
    struct exception_context child;
    fill_frame(&parent);

    // Poison the destination so "copied" cannot be confused with "left alone".
    for (int i = 0; i < 31; i++) child.regs[i] = 0xDEADull;
    child.sp = child.elr = child.spsr = child.esr = child.far = 0xDEADull;

    const u64 child_sp = 0x0000000050000000ull;
    fork_frame_init(&child, &parent, child_sp);

    // The two edits.
    TEST_EXPECT_EQ((s64)child.regs[0], 0,
                   "x0 := 0 in the CHILD's copy -- the entirety of what makes "
                   "fork return twice");
    TEST_EXPECT_EQ((s64)child.sp, (s64)child_sp,
                   "sp := child_sp -- mandatory, because two Procs sharing an "
                   "address space must not share a stack pointer");

    // Everything else verbatim. x1..x30 carry the parent's C frame: the child
    // continues inside the parent's function, so its callee-saved registers and
    // its return address must be the parent's, not a fresh thread's zeroes.
    for (int i = 1; i < 31; i++) {
        if (child.regs[i] != parent.regs[i]) {
            test_fail("x1..x30 must be copied verbatim -- the child continues "
                      "the parent's C frame");
            return;
        }
    }
    TEST_EXPECT_EQ((s64)child.elr, (s64)parent.elr,
                   "elr verbatim -- the child resumes at the SAME instruction, "
                   "which is what 'returns twice' means");
    TEST_EXPECT_EQ((s64)child.spsr, (s64)parent.spsr,
                   "spsr verbatim -- NZCV is live if a conditional follows the "
                   "syscall, and EL0 cannot forge the mode bits (the hardware "
                   "wrote them on exception entry)");

    // The parent's own frame is untouched: it is still executing on it, and the
    // handler returns its child's pid through this exact regs[0].
    TEST_EXPECT_EQ((s64)parent.regs[0], (s64)(0xA0000000ull),
                   "the SOURCE frame is not modified -- the parent is still "
                   "running on it");
    TEST_EXPECT_EQ((s64)parent.sp, (s64)0x0000BEEF00001000ull,
                   "the source sp is not modified either");
}

// Drive the real SYS_RFORK handler with a synthetic frame. Every rejection
// below lands BEFORE rfork_forked, so running as kproc (no address space) does
// not mask any of them -- which is why the validation lives in the handler
// rather than inside rfork_internal.
static s64 rfork_call(unsigned flags, u64 child_sp, u64 child_tls, u64 self_sp) {
    struct exception_context ctx;
    fill_frame(&ctx);
    ctx.regs[0] = (u64)flags;
    ctx.regs[1] = child_sp;
    ctx.regs[2] = child_tls;
    ctx.sp      = self_sp;
    return sys_rfork_handler(&ctx);
}

void test_fork_rfork_arg_rejection(void) {
    const u64 ok_sp   = 0x0000000050000000ull;   // nonzero, 16-aligned, user VA
    const u64 self_sp = 0x0000000060000000ull;

    // RFPROC alone is REFUSED, not served. The child would get a fresh EMPTY
    // address space and then be resumed at its parent's PC -- an instruction
    // fetch fault on the first cycle. Serving it would be a fork that cannot
    // work; refusing says so until COW exists (L-4).
    TEST_EXPECT_EQ(rfork_call(RFPROC, ok_sp, 0, self_sp), -(s64)T_E_INVAL,
                   "RFPROC alone must be refused -- a private child address "
                   "space means copy-on-write, which is not built yet");

    // Every other Plan 9 flag stays reserved rather than ignored.
    TEST_EXPECT_EQ(rfork_call(RFPROC | RFMEM | 0x0004u, ok_sp, 0, self_sp),
                   -(s64)T_E_INVAL,
                   "an unsupported flag must be refused, never silently "
                   "dropped -- 'honoured' and 'ignored' must not look alike");

    // child_sp is MANDATORY. A zero SP is the shape a caller lands on if it
    // thinks fork defaults the stack the way POSIX fork does.
    TEST_EXPECT_EQ(rfork_call(RFPROC | RFMEM, 0, 0, self_sp), -(s64)T_E_INVAL,
                   "child_sp == 0 must be refused -- sharing an address space "
                   "with no separate stack corrupts both frames at once");

    TEST_EXPECT_EQ(rfork_call(RFPROC | RFMEM, ok_sp + 8, 0, self_sp),
                   -(s64)T_E_INVAL,
                   "a misaligned child_sp must be refused -- AAPCS64 requires "
                   "16-byte SP alignment and an unaligned SP_EL0 faults");

    TEST_EXPECT_EQ(rfork_call(RFPROC | RFMEM, UACCESS_USER_VA_TOP, 0, self_sp),
                   -(s64)T_E_INVAL,
                   "a child_sp outside the user range must be refused");

    // The one overlap case that is free to see. Not a safety property -- an SP
    // that overlaps the parent's stack without equalling it is just as fatal
    // and is not detectable here -- but refusing the visible mistake is free.
    TEST_EXPECT_EQ(rfork_call(RFPROC | RFMEM, self_sp, 0, self_sp),
                   -(s64)T_E_INVAL,
                   "handing the child the caller's OWN live SP must be refused");

    // And the well-formed request still fails, but for the RIGHT reason and
    // from a LATER gate: kproc has no address space, so L-3a's RFMEM refusal
    // fires inside rfork. Without this leg every assertion above would also
    // pass if the handler simply rejected everything.
    TEST_EXPECT_EQ(rfork_call(RFPROC | RFMEM, ok_sp, 0, self_sp),
                   -(s64)T_E_AGAIN,
                   "a WELL-FORMED request must get past the argument gate and "
                   "fail later (kproc has no address space to share) -- "
                   "otherwise 'validated' and 'refuses everything' look alike");
}

// ---------------------------------------------------------------------------
// fork.table_copy -- LINEAGE L-3c
//
// The DECISION half of fd inheritance: which slots cross into the child, at
// which indices, carrying which rights. Reachable from a kernel test because
// handle_table_copy_into takes two Procs and nothing else -- the same split
// frame_init uses, and for the same reason: rfork_internal's copy call sits
// behind an RFMEM gate that kproc (no address space) can never pass, so the
// WIRING is proven at EL0 by /fork-probe and the RULE is proven here.
//
// The layout is chosen so that index preservation and slot compaction are
// DISTINGUISHABLE. A skipped handle sits BETWEEN two copied ones, so a copy
// written as a loop over the first free slot -- which is what handle_dup does,
// and the obvious way to write this -- lands the last handle one index low and
// fails on exactly that assertion.
// ---------------------------------------------------------------------------

void test_fork_table_copy(void) {
    struct Proc *parent = proc_alloc();
    TEST_ASSERT(parent != NULL, "parent proc");
    struct Proc *child = proc_alloc();
    TEST_ASSERT(child != NULL, "child proc");

    // A real Burrow, so the refcount claim is measurable rather than asserted.
    // burrow_create_anon's count of 1 is CONSUMED by handle_alloc (the Burrow
    // convention), so the parent's handle is that one count.
    struct Burrow *b = burrow_create_anon(PAGE_SIZE);
    TEST_ASSERT(b != NULL, "burrow_create_anon");

    hidx_t h_burrow = handle_alloc(parent, KOBJ_BURROW, RIGHT_READ | RIGHT_WRITE, b);
    TEST_ASSERT(h_burrow == 0, "first alloc lands at slot 0");
    hidx_t h_proc   = handle_alloc(parent, KOBJ_PROCESS, RIGHT_READ, parent);
    TEST_ASSERT(h_proc == 1, "second alloc lands at slot 1");
    // The skip. NULL obj is what test_handle.c uses for hw kinds -- and it is
    // safe HERE for a reason worth stating: the correct code never calls
    // handle_acquire_obj on a skipped slot, so there is no object to be real.
    hidx_t h_mmio   = handle_alloc(parent, KOBJ_MMIO, RIGHT_READ, NULL);
    TEST_ASSERT(h_mmio == 2, "the hw handle lands at slot 2");
    hidx_t h_thread = handle_alloc(parent, KOBJ_THREAD, RIGHT_READ, NULL);
    TEST_ASSERT(h_thread == 3, "fourth alloc lands at slot 3");

    TEST_EXPECT_EQ(burrow_handle_count(b), 1, "the parent holds the Burrow once");

    int copied = handle_table_copy_into(child, parent);
    TEST_EXPECT_EQ(copied, 3, "3 of the parent's 4 handles cross (the hw one does not)");
    TEST_EXPECT_EQ(handle_table_count(child->handles), 3, "the child's table agrees");
    TEST_EXPECT_EQ(handle_table_count(parent->handles), 4,
                   "the parent is UNCHANGED -- a copy, not a move");

    // Index preservation, including the hole. This is the POSIX property: the
    // parent's fd N is the child's fd N.
    struct Handle got;
    TEST_ASSERT(handle_get(child, 0, &got) == 0, "child slot 0 occupied");
    TEST_EXPECT_EQ((int)got.kind, (int)KOBJ_BURROW, "child slot 0 is the Burrow");
    TEST_EXPECT_EQ(got.rights, (rights_t)(RIGHT_READ | RIGHT_WRITE),
                   "rights carried VERBATIM, not narrowed");
    TEST_ASSERT(got.obj == (void *)b, "the child names the SAME Burrow object");
    handle_put(&got);

    TEST_ASSERT(handle_get(child, 1, &got) == 0, "child slot 1 occupied");
    TEST_EXPECT_EQ((int)got.kind, (int)KOBJ_PROCESS, "child slot 1 is the Process handle");
    handle_put(&got);

    // The hole. I-5: a hardware handle is pinned to the Proc that created it,
    // so the child gets nothing at that index -- NOT a shifted-down copy of
    // what came after it.
    TEST_EXPECT_EQ(handle_get(child, 2, &got), -1,
                   "the hw slot leaves a HOLE in the child (I-5)");

    TEST_ASSERT(handle_get(child, 3, &got) == 0,
                "child slot 3 occupied -- the skip left a hole rather than "
                "compacting slot 3 down into slot 2");
    TEST_EXPECT_EQ((int)got.kind, (int)KOBJ_THREAD, "child slot 3 is the Thread handle");
    handle_put(&got);

    // The refcount claim, measured on both edges.
    TEST_EXPECT_EQ(burrow_handle_count(b), 2, "the copy took its OWN reference");

    child->state = PROC_STATE_ZOMBIE;
    proc_free(child);
    TEST_EXPECT_EQ(burrow_handle_count(b), 1,
                   "the child's death released exactly its own reference -- "
                   "the parent's Burrow handle is still live");

    struct Handle still;
    TEST_ASSERT(handle_get(parent, 0, &still) == 0,
                "the parent's handle survives the child entirely");
    handle_put(&still);

    parent->state = PROC_STATE_ZOMBIE;
    proc_free(parent);
}
