// Host double of arch/arm64/atomic_lse.h, for host fixtures that compile real
// kernel sources whose headers reach <atomic_lse.h> (spinlock.h, by way of
// mm/slub.h). The same primitives at the same memory orders on the compiler's
// __atomic builtins; the LL/SC and LSE forms and their boot-time patching are
// not modelled.
#ifndef THYLACINE_ARM64_ATOMIC_LSE_H
#define THYLACINE_ARM64_ATOMIC_LSE_H

#include <thylacine/types.h>

static inline u32 t_atomic_xchg_acq_u32(u32 *ptr, u32 val) {
    return __atomic_exchange_n(ptr, val, __ATOMIC_ACQUIRE);
}
static inline u32 t_atomic_fetch_add_relaxed_u32(u32 *ptr, u32 val) {
    return __atomic_fetch_add(ptr, val, __ATOMIC_RELAXED);
}
static inline int t_atomic_fetch_add_relaxed_int(int *ptr, int val) {
    return __atomic_fetch_add(ptr, val, __ATOMIC_RELAXED);
}
static inline int t_atomic_fetch_add_acqrel_int(int *ptr, int val) {
    return __atomic_fetch_add(ptr, val, __ATOMIC_ACQ_REL);
}
static inline int t_atomic_fetch_sub_relaxed_int(int *ptr, int val) {
    return t_atomic_fetch_add_relaxed_int(ptr, -val);
}
static inline int t_atomic_fetch_sub_acqrel_int(int *ptr, int val) {
    return t_atomic_fetch_add_acqrel_int(ptr, -val);
}

#endif // THYLACINE_ARM64_ATOMIC_LSE_H
