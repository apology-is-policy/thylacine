// Bounded payload-lease state machine. See loom_service_pool.h for lock/pin
// obligations. Private syscall dispatch is not enabled by this module.
#include <thylacine/loom_service_pool.h>
#include <thylacine/errno.h>

static bool same_ref(struct loom_service_ref a, struct loom_service_ref b) {
    return a.slot == b.slot && a.incarnation == b.incarnation;
}
static bool valid_ref(struct loom_service_ref r) {
    return !r.reserved && r.slot < LOOM_SERVICE_SLOT_COUNT && r.incarnation;
}
static bool valid_extent(struct loom_pool_extent e) {
    return e.backing && e.length && e.offset <= UINT64_MAX - e.length;
}
static bool overlaps(struct loom_pool_extent a, struct loom_pool_extent b) {
    // Both extents are validated, so their exclusive ends cannot overflow.
    return a.backing == b.backing && a.offset < b.offset + b.length &&
           b.offset < a.offset + a.length;
}
static bool owns(const struct loom_pool_cell *c, const struct loom_pool *p) {
    return c->phase != LMEMBER_FREE && same_ref(c->pool, p->ref);
}
static const struct loom_pool_cell *member(const struct loom_pool_bank *b,
                                           const struct loom_pool *p, u32 n) {
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i)
        if (owns(&b->cells[i], p) && b->cells[i].ordinal == n)
            return &b->cells[i];
    return NULL;
}
static int receipt_cell(struct loom_pool_bank *b, const struct loom_pool *p,
                        const struct loom_service_buffer_receipt *r,
                        u32 phase, struct loom_pool_cell **out) {
    if (r->reserved || !valid_ref(r->pool) || r->member >= p->count)
        return -T_E_INVAL;
    if (p->phase != LPOOL_LIVE || !same_ref(r->pool, p->ref) || !r->lease)
        return -T_E_NOENT;
    struct loom_pool_cell *c = (struct loom_pool_cell *)member(b, p, r->member);
    if (!c || c->phase != phase || c->nonce != r->lease) return -T_E_NOENT;
    *out = c;
    return 0;
}
bool loom_pool_conflicts(const struct loom_pool_bank *b, struct loom_pool_extent e) {
    // Invalid intervals must never be used as an exclusion bypass.
    if (!valid_extent(e)) return true;
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i)
        if (b->cells[i].phase != LMEMBER_FREE && overlaps(e, b->cells[i].extent))
            return true;
    return false;
}
int loom_pool_prepare(struct loom_pool_bank *b, struct loom_pool *p,
                      struct loom_service_ref ref, const struct loom_pool_extent *e,
                      u32 count) {
    if (p->phase != LPOOL_EMPTY) return -T_E_BUSY;
    if (!valid_ref(ref) || !e || !count || count > LOOM_SERVICE_POOL_MEMBERS)
        return -T_E_INVAL;
    u32 free = 0;
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i) {
        if (b->cells[i].phase == LMEMBER_FREE) ++free;
        else if (same_ref(b->cells[i].pool, ref)) return -T_E_INVAL;
    }
    if (free < count) return -T_E_NOSPC;
    for (u32 i = 0; i < count; ++i) {
        if (!valid_extent(e[i])) return -T_E_INVAL;
        if (loom_pool_conflicts(b, e[i])) return -T_E_BUSY;
        for (u32 j = 0; j < i; ++j)
            if (overlaps(e[i], e[j])) return -T_E_INVAL;
    }
    // All validation precedes mutation. Provisional cells immediately exclude
    // both competing registrations and ordinary fixed-buffer admissions.
    u32 n = 0;
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS && n < count; ++i) {
        struct loom_pool_cell *c = &b->cells[i];
        if (c->phase != LMEMBER_FREE) continue;
        *c = (struct loom_pool_cell){ .pool = ref, .extent = e[n],
                     .ordinal = n, .phase = LMEMBER_AVAILABLE };
        ++n;
    }
    *p = (struct loom_pool){ .ref = ref, .phase = LPOOL_RESERVED, .count = count };
    return 0;
}
int loom_pool_publish(struct loom_pool *p) {
    if (p->phase != LPOOL_RESERVED) return -T_E_INVAL;
    p->phase = LPOOL_LIVE;
    return 0;
}
static void remove_pool(struct loom_pool_bank *b, struct loom_pool *p) {
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i)
        if (owns(&b->cells[i], p)) b->cells[i] = (struct loom_pool_cell){0};
    // Pins are the caller's responsibility: drop them outside the ring lock
    // only after this exclusion removal and after all local writers ended.
    *p = (struct loom_pool){0};
}
int loom_pool_rollback(struct loom_pool_bank *b, struct loom_pool *p) {
    if (p->phase != LPOOL_RESERVED) return -T_E_INVAL;
    remove_pool(b, p);
    return 0;
}
int loom_pool_stream_get(struct loom_pool *p) {
    if (p->phase != LPOOL_LIVE) return -T_E_NOENT;
    if (p->streams == UINT32_MAX) return -T_E_NOSPC;
    ++p->streams;
    return 0;
}
int loom_pool_stream_put(struct loom_pool *p) {
    if (p->phase != LPOOL_LIVE || !p->streams) return -T_E_INVAL;
    --p->streams;
    return 0;
}
int loom_pool_claim(struct loom_pool_bank *b, struct loom_pool *p, u32 maximum,
                    struct loom_service_buffer_receipt *out) {
    if (p->phase != LPOOL_LIVE) return -T_E_NOENT;
    if (!p->streams || !maximum || maximum > INT32_MAX) return -T_E_INVAL;
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i)
        if (owns(&b->cells[i], p) && maximum > b->cells[i].extent.length)
            return -T_E_INVAL;
    // Exhaustion is terminal even with an empty pool; do not wait for a return
    // that can never make another nonce available.
    if (b->next_nonce == UINT64_MAX) return -T_E_NOSPC;
    for (u32 n = 0; n < p->count; ++n) {
        u32 ordinal = (p->cursor + n) % p->count;
        struct loom_pool_cell *c = (struct loom_pool_cell *)member(b, p, ordinal);
        if (!c || c->phase != LMEMBER_AVAILABLE) continue;
        c->nonce = ++b->next_nonce;
        c->phase = LMEMBER_BUSY;
        c->requested = maximum;
        c->result = 0; c->user_data = 0; c->more = false;
        p->cursor = (ordinal + 1) % p->count;
        *out = (struct loom_service_buffer_receipt){ .pool = p->ref,
                                     .member = ordinal, .lease = c->nonce };
        return 0;
    }
    return -T_E_AGAIN; // wait locally; caller must not emit a Tread
}
int loom_pool_release_busy(struct loom_pool_bank *b, const struct loom_pool *p,
                           const struct loom_service_buffer_receipt *r) {
    struct loom_pool_cell *c;
    int err = receipt_cell(b, p, r, LMEMBER_BUSY, &c);
    if (err) return err;
    c->phase = LMEMBER_AVAILABLE; // nonce remains burned
    return 0;
}
int loom_pool_commit(struct loom_pool_bank *b, const struct loom_pool *p,
                     const struct loom_service_buffer_receipt *r, u32 length,
                     u64 user_data, bool more) {
    struct loom_pool_cell *c;
    int err = receipt_cell(b, p, r, LMEMBER_BUSY, &c);
    if (err) return err;
    if (!length || length > c->requested) return -T_E_INVAL;
    c->result = length; c->user_data = user_data; c->more = more;
    c->phase = LMEMBER_PENDING;
    return 0;
}
int loom_pool_peek(const struct loom_pool_bank *b, const struct loom_pool *p,
                   u32 ordinal, struct loom_pool_result *out) {
    if (p->phase != LPOOL_LIVE) return -T_E_NOENT;
    if (ordinal >= p->count) return -T_E_INVAL;
    const struct loom_pool_cell *c = member(b, p, ordinal);
    if (!c || c->phase != LMEMBER_PENDING) return -T_E_NOENT;
    *out = (struct loom_pool_result){
        .receipt = { .pool = p->ref, .member = ordinal, .lease = c->nonce },
        .user_data = c->user_data, .length = c->result, .more = c->more };
    return 0;
}
int loom_pool_deliver(struct loom_pool_bank *b, const struct loom_pool *p,
                      const struct loom_service_buffer_receipt *r) {
    struct loom_pool_cell *c;
    int err = receipt_cell(b, p, r, LMEMBER_PENDING, &c);
    if (err) return err;
    c->phase = LMEMBER_LEASED;
    return 0;
}
int loom_pool_return(struct loom_pool_bank *b, const struct loom_pool *p,
                     const struct loom_service_buffer_receipt *r) {
    struct loom_pool_cell *c;
    int err = receipt_cell(b, p, r, LMEMBER_LEASED, &c);
    if (err) return err;
    c->phase = LMEMBER_AVAILABLE;
    return 0;
}
int loom_pool_snapshot(const struct loom_pool_bank *b, const struct loom_pool *p,
                       struct loom_service_pool_snapshot *out) {
    if (p->phase != LPOOL_LIVE) return -T_E_NOENT;
    *out = (struct loom_service_pool_snapshot){ .size = sizeof(*out),
         .version = LOOM_SERVICE_ABI_VERSION, .pool = p->ref,
         .members = p->count, .streams = p->streams };
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i) {
        const struct loom_pool_cell *c = &b->cells[i];
        if (!owns(c, p)) continue;
        switch (c->phase) {
        case LMEMBER_AVAILABLE: ++out->available; break;
        case LMEMBER_BUSY: ++out->busy; break;
        case LMEMBER_PENDING: ++out->pending; break;
        case LMEMBER_LEASED: ++out->leased; break;
        default: return -T_E_IO;
        }
    }
    return 0;
}
int loom_pool_reap(struct loom_pool_bank *b, struct loom_pool *p) {
    if (p->phase != LPOOL_LIVE) return -T_E_NOENT;
    if (p->streams) return -T_E_BUSY;
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i)
        if (owns(&b->cells[i], p) && b->cells[i].phase != LMEMBER_AVAILABLE)
            return -T_E_BUSY;
    remove_pool(b, p);
    return 0;
}
