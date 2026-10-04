---
id: chg-2026-10-04-async-owner-lifetime
type: chg
title: "Separate process ownership from asynchronous descriptor retention"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-addrspace, sub-kernel-burrow, sub-kernel-proc, sub-kernel-syscall-dispatch]
established: []
closed: []
opened: []
depth: skeletal
---
# AS-2a: exact owner lifetime for asynchronous retirement

Implementation of approved ASYNC-SERVICE-LIFECYCLE sections 6 and 8. This is an
internal prerequisite; it neither enables private Loom nor replaces the current
shared-map budget. It is not the later MM account hierarchy.

An AddrSpace has process/constructor owners and kernel descriptor pins. `ref`
counts both; `owners` counts only the first. Allocation starts both at one.
Owner acquisition first takes a total reference, then increments owners; it
requires an existing owner and cannot resurrect an ownerless descriptor.
Kernel pin acquisition requires an already live owner or pin. Neither accessor
is a synchronization primitive or a way to acquire an unpinned pointer.

Owner release decrements owners. Only its transition from one to zero drains
VMAs, on the ordinary exit/exec path, retaining that owner's total reference
through the entire drain. FILE Burrow destruction can call a sleeping clunk;
that work must not move to the private service retirement worker. Only after
the drain finishes does the owner release its total reference. Kernel unpin
releases only a total reference. The single final total-reference transition
frees page tables and the descriptor, asserting there are no owners or VMAs.
No pair of independent zero tests decides destruction.

A kernel pin does not authorize VA translation or retain mappings. Existing
no-CPU-under-old-TTBR0 requirements still belong to final owner release. Loom's
registered writable contiguous ANON buffers have independent Burrow references
and stable direct-map addresses; those references, not the address-space VMAs,
keep pending I/O storage valid. The later private implementation must retain
both kinds of reference and retire them in that order.

The three Proc ownership predicates (device quiescence, authority-image join,
image-flag stamping) and the spawn budget's raw-ref predicate all use owners.
A kernel cleanup pin must neither block sole-image elevation nor hide the last
driver from MMIO quiescence. A genuine second Proc must retain both effects.
The appended counter changes AddrSpace's asserted internal size72 to80 without
moving existing members. No userspace record changes.

Burrow charge claim/restore gain exact-AddrSpace forms. Existing Proc wrappers
delegate to them; their behavior and lock order stay the same. The caller owns
the exact descriptor and Burrow reference. Claim precedes Burrow release;
refund follows actual storage destruction; a nonfinal release restores the
claim. No dead Proc pointer or successor image is read. The existing conservative
claim/restore window and the legacy detach's shared-out policy are unchanged.
This does not claim a new globally exact memory ledger: the approved MM arc
will replace those semantics. An ownerless descriptor cannot admit new user
allocations while its pending charges settle.

Primary precedent: Linux separates mmgrab descriptor lifetime from mmget mapping
lifetime ([lifetime helpers](https://github.com/torvalds/linux/blob/master/include/linux/sched/mm.h)). Thylacine retains its own Proc, AddrSpace,
Burrow, namespace and existing accounting rules.

Verification:
actual lifecycle C slice + actual struct declaration, allocator/drain doubles,
serial two-owner/pin cases, final-owner drain paused while a pin releases, and
100 concurrent final-drop schedules. Clean ASan/UBSan and seven intended named
mutations pass. Six complete C translation units compile for ARM64. Existing COW/capacity/Burrow models pass five clean configurations and twelve intended counterexamples. Fresh CI CPU1 boot passes the guest tests: actual process table elevation/sharing; actual last-Proc
VMA-only virtio quiescence; exact-payer claim/refund after Proc death.

Self-review findings corrected before application: total pins cannot retain all
VMAs because their last release could then wait on a filesystem peer; raw-ref
spawn budget consumer was found by a full census; test fixture externs were
missing; device result assertions now follow cleanup. The additional mutation
reached an earlier protection than its first expected label, which was corrected
with original evidence retained. No independent review is claimed (single-agent).

Focused evidence: /Users/northkillpd/projects/thylacine-astra/work/oct4-async-service/as2a/check-1791130311682969000. No AS-2a broad matrix, graphical or Pi claim;
the preceding 50/50 matrix qualified AS-1/TLS before these changes. Private
setup remains rejected. The ownership engine and userspace clients are next.
