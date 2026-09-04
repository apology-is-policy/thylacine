/* thyla_jit.h -- the I-42 JIT syscall trio for a pouch C/C++ consumer.
 *
 * Companion to thyla_capjit.h (which ACQUIRES CAP_JIT by walking the corvus
 * clearance). This header carries the three syscalls that operate a dual-mapped
 * code Burrow once the capability is held:
 *
 *   thyla_jit_create(len, &region)  -- SYS_JIT_CREATE (101), CAP_JIT-gated.
 *       Installs BOTH aliases of one code region: a WRITER alias (RW, emit here)
 *       and an EXEC alias (RX, branch here), each a separate VMA over the same
 *       physical pages. No PTE is ever W-and-X, so I-12 holds. The two aliases
 *       sit at unrelated VAs but an instruction has the SAME offset in each, so
 *       exec = writer + (exec_va - writer_va) is a constant delta.
 *   thyla_jit_icache_sync(va, len)  -- SYS_ICACHE_SYNC (103), not gated.
 *       Publishes emitted bytes: the dc-cvau / ic-ivau dance the architecture
 *       requires between a write through the writer alias and an instruction
 *       fetch through the exec alias. The range may name EITHER alias (both map
 *       the same physical pages). MANDATORY before executing freshly-emitted
 *       code -- Thylacine runs EL0 with SCTLR_EL1.UCI=0, so a userspace
 *       `dc cvau`/`ic ivau` (what __builtin___clear_cache lowers to) TRAPS;
 *       the kernel performs the maintenance on its own direct map instead.
 *   thyla_jit_destroy(writer_va)    -- SYS_JIT_DESTROY (102), not gated.
 *       Tears down both aliases, named by the writer VA. Not needed by a JIT
 *       that holds its region for the life of the process (proc teardown frees
 *       it); provided for completeness.
 *
 * The numbers are ABI (kernel/include/thylacine/syscall.h). Issued as inline
 * SVC rather than through libc: musl carries no wrapper for a Thylacine-private
 * number, and syscall(3) would put the call at the mercy of the pouch seam's
 * number mapping. Same rationale + shape as thyla_capjit.h and the ORC
 * DualMapMemoryMapper (llvm patch 0007).
 */
#ifndef THYLA_JIT_H
#define THYLA_JIT_H

#include <stddef.h>
#include <stdint.h>

#include "thyla_capjit.h" /* thyla_acquire_cap_jit() */

#define THYLA_SYS_JIT_CREATE  101L
#define THYLA_SYS_JIT_DESTROY 102L
#define THYLA_SYS_ICACHE_SYNC 103L
#define THYLA_JIT_REGION_MAX  (64u * 1024u * 1024u)

/* struct t_jit_region -- the SYS_JIT_CREATE out-parameter. Layout pinned by
 * _Static_assert on the kernel side (writer_va@0, exec_va@8, size 16). */
struct thyla_jit_region {
    uint64_t writer_va;
    uint64_t exec_va;
};

/* SYS_JIT_CREATE. Returns 0 and fills *out on success; -errno otherwise
 * (-13/EACCES = no CAP_JIT, -22/EINVAL = length 0 or > JIT_REGION_MAX,
 * -12/ENOMEM = budget/VA/allocator, -14/EFAULT = out unwritable). */
static inline long thyla_jit_create(size_t length, struct thyla_jit_region *out)
{
    register long x0 __asm__("x0") = (long)length;
    register long x1 __asm__("x1") = (long)(uintptr_t)out;
    register long x8 __asm__("x8") = THYLA_SYS_JIT_CREATE;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory", "cc");
    return x0;
}

/* SYS_ICACHE_SYNC. `va` may point into either alias of a live code region.
 * Returns 0 or -errno (-22/EINVAL = empty/wrapping/uncontained range). */
static inline long thyla_jit_icache_sync(void *va, size_t length)
{
    register long x0 __asm__("x0") = (long)(uintptr_t)va;
    register long x1 __asm__("x1") = (long)length;
    register long x8 __asm__("x8") = THYLA_SYS_ICACHE_SYNC;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory", "cc");
    return x0;
}

/* SYS_JIT_DESTROY. `writer_va` must be a region's writer-alias base. */
static inline long thyla_jit_destroy(uint64_t writer_va)
{
    register long x0 __asm__("x0") = (long)writer_va;
    register long x8 __asm__("x8") = THYLA_SYS_JIT_DESTROY;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

#endif /* THYLA_JIT_H */
