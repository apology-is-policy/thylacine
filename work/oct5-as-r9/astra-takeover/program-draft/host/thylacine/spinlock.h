#ifndef THYLACINE_SPINLOCK_H
#define THYLACINE_SPINLOCK_H
#include <thylacine/types.h>
#include <stdlib.h>
typedef u64 irq_state_t;
typedef struct spin_lock {u32 value;} spin_lock_t;
#define SPIN_LOCK_INIT ((spin_lock_t){0})
static inline void spin_lock_init(spin_lock_t *l){l->value=0;}
static inline void spin_lock(spin_lock_t *l){if(l->value)abort();l->value=1;}
static inline void spin_unlock(spin_lock_t *l){if(!l->value)abort();l->value=0;}
#endif
