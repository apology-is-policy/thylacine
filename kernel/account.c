// The memory account tree and its physical ledger, MM-1: see <thylacine/memaccount.h>.
#include <thylacine/memaccount.h>

#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>

#include "../mm/phys.h"
#include "../mm/slub.h"

static u64 g_account_unpaired;
// Sponsored pages outside the capacity pool, which is what separates the two totals.
static u64 g_account_unpooled;
static struct kmem_cache *g_account_cache;

static struct MemoryAccount g_account_root = {
    .incarnation = 1,
    .parent = NULL,
    .refs = 1,
    .depth = 0,
};

static struct MemoryAccount g_account_system_cache = {
    .incarnation = 2,
    .parent = &g_account_root,
    .refs = 1,
    .depth = 1,
};

struct MemoryAccount *account_root(void) { return &g_account_root; }
struct MemoryAccount *account_system_cache(void) { return &g_account_system_cache; }

struct MemoryAccount *account_current(void) {
    struct Thread *t = current_thread();
    if (!t || !t->proc || !t->proc->account) return &g_account_root;
    return t->proc->account;
}

void account_init(void) {
    if (g_account_cache) extinction("account_init called twice");
    g_account_cache = kmem_cache_create("memory_account", sizeof(struct MemoryAccount), 8, 0);
    if (!g_account_cache) extinction("kmem_cache_create(memory_account) returned NULL");
}

#ifdef KERNEL_TESTS
// 1 and 2 are the two static accounts. The counter is 64 bits and only ever rises, so
// no account in a boot's life shares an incarnation with one that came before it.
static u64 g_account_incarnation = 3;

struct MemoryAccount *account_child(struct MemoryAccount *parent) {
    if (parent->depth + 1u >= ACCOUNT_MAX_DEPTH) return NULL;
    struct MemoryAccount *a = kmem_cache_alloc(g_account_cache, KP_ZERO);
    if (!a) return NULL;
    a->incarnation = __atomic_fetch_add(&g_account_incarnation, 1u, __ATOMIC_RELAXED);
    a->parent = parent;
    a->refs = 1;
    a->depth = parent->depth + 1u;
    account_ref(parent);
    return a;
}
#endif

void account_ref(struct MemoryAccount *a) {
    if (__atomic_fetch_add(&a->refs, 1u, __ATOMIC_RELAXED) == 0u)
        extinction("account_ref of a dead account");
}

// A loop, so a chain of last references costs no stack. The static two start with a
// reference nothing drops, so neither reaches the free.
void account_unref(struct MemoryAccount *a) {
    while (a) {
        u32 was = __atomic_fetch_sub(&a->refs, 1u, __ATOMIC_ACQ_REL);
        if (was == 0u) extinction("account_unref of a dead account");
        if (was != 1u) return;
        struct MemoryAccount *parent = a->parent;
        for (unsigned cls = 0; cls < ACCOUNT_CLASSES; cls++) {
            if (__atomic_load_n(&a->used[cls], __ATOMIC_RELAXED) != 0u)
                __atomic_fetch_add(&g_account_unpaired, 1u, __ATOMIC_RELAXED);
        }
        kmem_cache_free(g_account_cache, a);
        a = parent;
    }
}

void account_charge(struct MemoryAccount *a, unsigned cls, u64 pages) {
    for (; a; a = a->parent)
        __atomic_fetch_add(&a->used[cls], pages, __ATOMIC_RELAXED);
}

void account_refund(struct MemoryAccount *a, unsigned cls, u64 pages) {
    for (; a; a = a->parent) {
        u64 cur = __atomic_load_n(&a->used[cls], __ATOMIC_RELAXED);
        do {
            if (cur < pages) {
                __atomic_fetch_add(&g_account_unpaired, 1u, __ATOMIC_RELAXED);
                return;
            }
        } while (!__atomic_compare_exchange_n(&a->used[cls], &cur, cur - pages, true,
                                              __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    }
}

VISIBLE_IF_KERNEL_TESTS
u64 account_used(const struct MemoryAccount *a, unsigned cls) {
    return __atomic_load_n(&a->used[cls], __ATOMIC_RELAXED);
}

// Section 4's unconfined default: a RAM class may reach the pool, and nothing bounds
// device storage here. An account has no limits of its own until something sets them.
static u64 class_limit_bytes(unsigned cls) {
    if (cls == CLASS_DEVICE) return ~0ull;
    return (u64)capacity_pool_pages() << PAGE_SHIFT;
}

// Field by field: the tree has no memset for the compiler to call on a large aggregate.
void account_query_fill(const struct MemoryAccount *a, struct account_query *q) {
    q->h.version = ACCOUNT_VERSION;
    q->h.size = (u32)sizeof(*q);
    q->h.flags = 0u;
    q->h.pad = 0u;
    q->h.incarnation = a->incarnation;
    q->generation = 0u;
    q->level = ACCOUNT_NORMAL;
    q->limiting = CLASS_ALLOC;
    q->reclaim_target = 0u;
    q->retained_after_exit = 0u;
    q->awaiting_device = 0u;
    for (unsigned cls = 0; cls < ACCOUNT_CLASSES; cls++) {
        q->c[cls].used = account_used(a, cls) << PAGE_SHIFT;
        q->c[cls].reserved = 0u;
        q->c[cls].limit = class_limit_bytes(cls);
    }

    u64 tightest = ~0ull;
    for (const struct MemoryAccount *up = a; up; up = up->parent) {
        for (unsigned cls = 0; cls < ACCOUNT_CLASSES; cls++) {
            u64 limit = class_limit_bytes(cls);
            u64 used = account_used(up, cls) << PAGE_SHIFT;
            u64 room = limit > used ? limit - used : 0;
            if (room < tightest) {
                tightest = room;
                q->limiting = cls;
            }
        }
    }

    u64 pool = (u64)capacity_pool_pages() << PAGE_SHIFT;
    u64 charged = (u64)capacity_pool_charged() << PAGE_SHIFT;
    u64 pool_room = pool > charged ? pool - charged : 0;
    q->headroom = pool_room < tightest ? pool_room : tightest;
}

u64 account_unpaired(void) {
    return __atomic_load_n(&g_account_unpaired, __ATOMIC_RELAXED);
}

#ifdef KERNEL_TESTS
void account_unpaired_rewind_for_test(u64 count) {
    __atomic_store_n(&g_account_unpaired, count, __ATOMIC_RELAXED);
}
#endif

// The figures are read a moment apart, so a DMA block landing between them can make
// unpooled the larger of the two for this one reading.
u64 account_pool_share(void) {
    u64 alloc = account_used(&g_account_root, CLASS_ALLOC);
    u64 unpooled = __atomic_load_n(&g_account_unpooled, __ATOMIC_RELAXED);
    return (alloc > unpooled ? alloc - unpooled : 0u) + account_used(&g_account_root, CLASS_META);
}

void account_sponsor_block(struct page *head, struct MemoryAccount *a) {
    u64 pages = 1ull << head->order;
    account_ref(a);
    account_charge(a, CLASS_ALLOC, pages);
    if (!(head->flags & PG_USER))
        __atomic_fetch_add(&g_account_unpooled, pages, __ATOMIC_RELAXED);
    head->holder.sponsor = a;
    head->flags |= PG_SPONSORED;
}

// The caller's order is only compared. The block's own is what was charged, and the pool
// refunds the caller's, so a wrong order leaves the two totals apart as well as counted.
void account_release_block(struct page *head, unsigned order) {
    struct MemoryAccount *a = head->holder.sponsor;
    u64 pages = 1ull << head->order;
    if (order != head->order)
        __atomic_fetch_add(&g_account_unpaired, 1u, __ATOMIC_RELAXED);
    head->holder.sponsor = NULL;
    head->flags &= ~PG_SPONSORED;
    if (!(head->flags & PG_USER))
        __atomic_fetch_sub(&g_account_unpooled, pages, __ATOMIC_RELAXED);
    account_refund(a, CLASS_ALLOC, pages);
    account_unref(a);
}
