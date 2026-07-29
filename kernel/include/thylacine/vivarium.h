// VIVARIUM V-2 — the Linux syscall translation table (docs/VIVARIUM.md §4/§5.3).
//
// This header + kernel/vivarium.c are the in-kernel half of Option C (the
// hybrid, user-voted 2026-07-23): the Linux calls whose translation is TOTAL and
// STATELESS are decoded here by table; everything else forwards to the userspace
// supervisor or fails ENOSYS.
//
// WHY A TABLE AND NOT LOGIC. §4's whole defence of Option C is that "the
// in-kernel part is a TABLE, not logic — auditable by inspection, and it adds no
// new kernel *semantics*, only a decode." That is a real constraint on this file,
// not a description of it: `vivarium_translate` is PURE — it takes a syscall
// number and six argument words, and returns a verdict plus a rewritten call. It
// touches no Proc, no handle table, no user memory, no lock, and allocates
// nothing. It is unit-testable with zero kernel plumbing, and that is the point.
//
// THE ADMISSION RULE (§4, binding). A Linux call may occupy a TABLE row iff its
// translation is *total and stateless*: a pure renumber plus an argument-order or
// flag-bit mapping onto exactly one existing `sys_*_for_proc`, with **no new
// kernel state, no new error semantics, and no policy**. The instant a call needs
// state the kernel does not already own — socket tables, signal dispositions,
// /proc content, ioctl dispatch — it forwards.
//
// "TOTAL" IS THE WORD THAT DOES THE WORK, and it is easy to misread as "the
// arguments line up". It does not mean that. It means the translation is correct
// for EVERY input the Linux call accepts. The worked counterexample is `munmap`:
// Linux `munmap(addr, len)` and `SYS_BURROW_DETACH(vaddr, length)` take the same
// two words in the same order and look like a free row — but burrow_detach
// requires an EXACT VMA match ("no partial detach at v1.0", syscall.h:611) while
// Linux explicitly permits partial and multi-mapping unmaps. The renumber would
// therefore be silently wrong for a legal class of inputs, so `munmap` is NOT a
// table row. Arguments aligning is not semantics aligning; check the contract.
//
// WHAT IS DELIBERATELY ABSENT. Nothing here is wired into `syscall_dispatch`.
// V-1a landed `Proc.phenotype` but NOTHING can set it to PHENO_LINUX — verified:
// `exec.c` never touches the field, the only assignment in the tree is the rfork
// inherit, and `PHENO_LINUX` is referenced nowhere outside its own enum. So the
// dispatch branch would today be branching on a field that is provably always 0.
// The branch + the declaration that makes it reachable are V-1b, and the
// declaration's correct SHAPE depends on V-7's container object (§5.2: "the fused
// container+phenotype object is the right granularity"; every peer system except
// FreeBSD has the CONTAINER declare). Building the table first is the reversible
// half: a table is data and can be rewritten freely, whereas a syscall signature
// is append-only ABI.

#ifndef THYLACINE_VIVARIUM_H
#define THYLACINE_VIVARIUM_H

#include <thylacine/types.h>

// A Linux aarch64 syscall carries at most 6 argument words (x0..x5).
#define VIV_NARGS 6

// The verdict of a translation attempt.
enum viv_verdict {
    // A TABLE row matched: dispatch `out->nr` with `out->args` natively.
    VIV_TRANSLATED = 0,

    // Total-and-stateless does not hold: the call needs state, policy, or
    // judgement the kernel does not already own. V-3's userspace supervisor
    // handles it. Until V-3 exists the caller's disposition is its own choice —
    // this function only classifies.
    VIV_FORWARD = 1,

    // No Thylacine counterpart exists at all (`brk`: the heap is Burrow-based,
    // there is no break pointer to move). Honest ENOSYS, not a silent lie.
    VIV_ENOSYS = 2,
};

// A translated call: the Thylacine syscall number + its argument vector.
struct viv_call {
    u64 nr;
    u64 args[VIV_NARGS];
};

// Translate one Linux aarch64 syscall.
//
// PURE: no Proc, no uaccess, no locks, no allocation, no globals touched. Safe to
// call from anywhere, including a unit test with a synthetic argument vector.
//
// `args_in` must point to VIV_NARGS words. `out` is written only when the return
// is VIV_TRANSLATED; on FORWARD/ENOSYS it is left untouched, so a caller that
// ignores the verdict cannot accidentally dispatch a garbage number.
//
// A NULL `args_in` or `out` yields VIV_ENOSYS (fail closed, never a dispatch).
enum viv_verdict vivarium_translate(u64 linux_nr, const u64 *args_in,
                                    struct viv_call *out);

// The Linux aarch64 numbers this table knows. Named so the tests assert against
// symbols rather than magic numbers, and so a future row addition is a one-line
// diff next to its number. (Linux's aarch64 table is stable ABI — these values
// are fixed for all time by the Linux kernel's own compatibility promise.)
enum {
    VIV_LINUX_OPENAT     = 56,
    VIV_LINUX_CLOSE      = 57,
    VIV_LINUX_LSEEK      = 62,
    VIV_LINUX_READ       = 63,
    VIV_LINUX_WRITE      = 64,
    VIV_LINUX_FSTAT      = 80,
    VIV_LINUX_BRK        = 214,
    VIV_LINUX_MUNMAP     = 215,
    VIV_LINUX_MMAP       = 222,
    VIV_LINUX_EXIT_GROUP = 94,
};

#endif // THYLACINE_VIVARIUM_H
