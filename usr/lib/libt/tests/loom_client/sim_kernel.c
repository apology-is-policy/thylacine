/* The simulated kernel: see sim_kernel.h. */
#include "sim_kernel.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <thyla/poll.h>

static const struct t_loom_service_buffer_receipt ZERO_RECEIPT;

static int name_eq(const unsigned char *a, size_t alen, const unsigned char *b, size_t blen) {
    return alen == blen && !memcmp(a, b, alen);
}
void sim_init(struct sim *s) {
    memset(s, 0, sizeof(*s));
    s->clock = 1000000000;
    s->next_inc = 1;
    s->next_lease = 1;
    memcpy(s->services[0], "clip", 4);
    s->service_len[0] = 4;
    s->nservices = 1;
    s->registry_fd = 5;
}
size_t sim_live_slots(const struct sim *s) {
    size_t n = 0;
    for (size_t i = 0; i < 64; ++i) n += s->slots[i].used;
    return n;
}
int sim_closed(const struct sim *s, int32_t fd) {
    for (size_t i = 0; i < s->nclosed; ++i)
        if (s->closed_fds[i] == fd) return 1;
    return 0;
}
static _Atomic uint32_t *word(struct sim *s, size_t i) {
    assert(s->has_ring);
    return (_Atomic uint32_t *)(void *)(s->ring_va + s->p.hdr_off + 4 * i);
}
uint32_t sim_hdr_word(struct sim *s, size_t i) { return atomic_load_explicit(word(s, i), memory_order_acquire); }
void sim_poke_hdr(struct sim *s, size_t i, uint32_t v) { atomic_store_explicit(word(s, i), v, memory_order_release); }

static struct sim_file *file_of(struct sim *s, const unsigned char *name, size_t len, int create) {
    for (size_t i = 0; i < s->nfiles; ++i)
        if (name_eq(s->files[i].name, s->files[i].name_len, name, len)) return &s->files[i];
    if (!create) return NULL;
    assert(s->nfiles < 16 && len <= SIM_NAME);
    struct sim_file *f = &s->files[s->nfiles++];
    memcpy(f->name, name, len);
    f->name_len = len;
    f->len = 0;
    return f;
}
void sim_file(struct sim *s, const char *name, const void *bytes, size_t len) {
    struct sim_file *f = file_of(s, (const unsigned char *)name, strlen(name), 1);
    assert(len <= sizeof(f->data));
    memcpy(f->data, bytes, len);
    f->len = len;
}
const struct sim_file *sim_find_file(const struct sim *s, const char *name) {
    return file_of((struct sim *)s, (const unsigned char *)name, strlen(name), 0);
}
static struct sim_shots *shots_of(struct sim *s, const unsigned char *name, size_t len, int create) {
    for (size_t i = 0; i < s->nshots; ++i)
        if (name_eq(s->shots[i].name, s->shots[i].name_len, name, len)) return &s->shots[i];
    if (!create) return NULL;
    assert(s->nshots < 8 && len <= SIM_NAME);
    struct sim_shots *q = &s->shots[s->nshots++];
    memset(q, 0, sizeof(*q));
    memcpy(q->name, name, len);
    q->name_len = len;
    return q;
}
void sim_shot(struct sim *s, const char *name, const void *bytes, size_t len) {
    struct sim_shots *q = shots_of(s, (const unsigned char *)name, strlen(name), 1);
    assert(q->count < 64 && len <= 256);
    size_t at = (q->head + q->count++) % 64;
    memcpy(q->q[at].b, bytes, len);
    q->q[at].len = len;
}

static int cq_room(struct sim *s) {
    return s->cq_tail - atomic_load_explicit(word(s, 4), memory_order_acquire) < s->p.cq_entries;
}
/* Post deferred completions in order while the CQ has room. */
static void flush(struct sim *s) {
    while (s->ndeferred && cq_room(s)) {
        struct sim_cqe c = s->deferred[0];
        memmove(s->deferred, s->deferred + 1, --s->ndeferred * sizeof(s->deferred[0]));
        if (s->has_forge) {
            c.user_data = s->forge_next;
            s->has_forge = 0;
        }
        uint32_t at = s->cq_tail & (s->p.cq_entries - 1);
        unsigned char *base = (unsigned char *)s->ring_va;
        struct t_owned_cqe cqe = { c.user_data, c.result, c.flags };
        memcpy(base + s->p.cqe_off + (size_t)at * 16, &cqe, 16);
        memcpy(base + s->p.receipt[0] + (size_t)at * 32, &c.receipt, 32);
        ++s->cq_tail;
        atomic_store_explicit(word(s, 5), s->cq_tail, memory_order_release);
    }
}
static void post(struct sim *s, int has_scope, size_t scope, uint64_t user_data, int32_t result, uint32_t flags,
                 struct t_loom_service_buffer_receipt receipt) {
    struct sim_cqe c = { has_scope, scope, user_data, result, flags, receipt };
    if (flags & T_LOOM_CQE_SERVICE_BUFFER) {
        s->last_shot = c;
        s->has_last_shot = 1;
    }
    assert(s->ndeferred < SIM_MAX);
    s->deferred[s->ndeferred++] = c;
    flush(s);
}
void sim_release_rejections(struct sim *s) {
    s->hold_rejections = 0;
    size_t n = s->nrejected;
    s->nrejected = 0;
    for (size_t i = 0; i < n; ++i) post(s, 0, 0, s->rejected[i].user_data, s->rejected[i].result, 0, ZERO_RECEIPT);
}
void sim_release_terminals(struct sim *s) {
    s->hold_terminals = 0;
    size_t n = s->nheld;
    s->nheld = 0;
    for (size_t i = 0; i < n; ++i)
        post(s, 1, s->held[i].scope, s->held[i].user_data, s->held[i].result, 0, ZERO_RECEIPT);
}
void sim_replay_last_shot(struct sim *s) {
    assert(s->has_last_shot);
    struct sim_cqe c = s->last_shot;
    post(s, c.has_scope, c.scope, c.user_data, c.result, c.flags, c.receipt);
}
static int mint(struct sim *s, struct sim_slot slot, struct t_loom_service_ref *out) {
    for (size_t i = 0; i < 64; ++i) {
        if (s->slots[i].used) continue;
        slot.used = 1;
        slot.inc = s->next_inc++;
        s->slots[i] = slot;
        *out = (struct t_loom_service_ref){ (uint32_t)i, 0, slot.inc };
        return 1;
    }
    return 0;
}
static struct sim_slot *live(struct sim *s, struct t_loom_service_ref r) {
    if (r.slot >= 64 || r.reserved) return NULL;
    struct sim_slot *sl = &s->slots[r.slot];
    return sl->used && sl->inc == r.incarnation ? sl : NULL;
}
static void remove_op(struct sim *s, size_t i) {
    memmove(s->ops + i, s->ops + i + 1, (s->nops - i - 1) * sizeof(s->ops[0]));
    --s->nops;
}
static void cancel_scope(struct sim *s, size_t scope, int32_t result) {
    for (size_t i = 0; i < s->nops;) {
        if (s->ops[i].scope != scope) {
            ++i;
            continue;
        }
        uint64_t ud = s->ops[i].user_data;
        remove_op(s, i);
        if (s->hold_terminals) {
            assert(s->nheld < SIM_MAX);
            s->held[s->nheld++] = (struct sim_cqe){ 1, scope, ud, result, 0, ZERO_RECEIPT };
        } else {
            post(s, 1, scope, ud, result, 0, ZERO_RECEIPT);
        }
    }
    struct sim_slot *sl = &s->slots[scope];
    if (sl->used && sl->state != SIM_RETIRED && sl->state != SIM_RESERVED) sl->state = SIM_ABORTING;
}
static void abort_all(struct sim *s) {
    for (size_t i = 0; i < 64; ++i)
        if (s->slots[i].used && s->slots[i].kind == SIM_SCOPE) cancel_scope(s, i, SIM_ECANCELED);
}
void sim_close_under(struct sim *s) {
    assert(s->nclosed < 16);
    s->closed_fds[s->nclosed++] = s->ring_fd;
    s->closing = 1;
    abort_all(s);
}
void sim_latch_under(struct sim *s) {
    s->closing = 1;
    abort_all(s);
}
static int busy_scope(struct sim *s, size_t i) {
    for (size_t j = 0; j < s->nops; ++j)
        if (s->ops[j].scope == i) return 1;
    return 0;
}
/* Retire aborted scopes with nothing in flight. A fid never retires with its
 * scope: REAP retires it (loom_fid_reap_retired). */
static void settle(struct sim *s) {
    for (size_t i = 0; i < 64; ++i) {
        struct sim_slot *sl = &s->slots[i];
        if (sl->used && sl->kind == SIM_SCOPE && sl->state == SIM_ABORTING && !busy_scope(s, i))
            sl->state = SIM_RETIRED;
    }
}
void sim_engine_visit(struct sim *s) { settle(s); }
/* loom_private_service_reap: a target or pool once unborrowed; a scope once
 * unused or RETIRED and no fid pins it; a fid once unused, clunked, or of a
 * RETIRED scope, and unborrowed. */
static int reapable(struct sim *s, size_t i) {
    struct sim_slot *sl = &s->slots[i];
    for (size_t j = 0; j < s->nops; ++j) {
        struct sim_op *o = &s->ops[j];
        if (o->source == i || o->scope == i || (o->kind == SIM_OP_CONNECT && o->target == i) ||
            (o->kind == SIM_OP_POOLED && o->pool == i) ||
            /* loom_fid_claim_walk pins the destination until the WALK ends. */
            (o->kind == SIM_OP_WALK && o->dest == i))
            return 0;
    }
    switch (sl->kind) {
    case SIM_TARGET:
        return 1;
    case SIM_POOL:
        for (size_t m = 0; m < s->pool_members[i]; ++m)
            if (s->pools[i][m].leased) return 0;
        return 1;
    case SIM_SCOPE:
        for (size_t j = 0; j < 64; ++j)
            if (j != i && s->slots[j].used && s->slots[j].kind == SIM_FID && s->slots[j].scope == i) return 0;
        return sl->state == SIM_RESERVED || sl->state == SIM_RETIRED;
    default:
        return sl->state == SIM_RESERVED || sl->state == SIM_RETIRED ||
               (s->slots[sl->scope].used && s->slots[sl->scope].state == SIM_RETIRED);
    }
}
static unsigned char *buffer(struct sim *s, uint32_t index, uint64_t at, uint64_t len) {
    if (index >= s->nbuffers) return NULL;
    if (at > UINT64_MAX - len || at + len > s->buffers[index].len) return NULL;
    return (unsigned char *)(s->buffers[index].va + (size_t)at);
}
/* Answer one operation; 1 means it is still running. */
static int answer(struct sim *s, struct sim_op *op) {
    struct sim_slot *src = &s->slots[op->source];
    int has_file = src->has_name;
    unsigned char file[SIM_NAME];
    size_t file_len = src->name_len;
    memcpy(file, src->name, file_len);
    int64_t rc = 0;
    switch (op->kind) {
    case SIM_OP_CONNECT: {
        struct sim_slot *t = &s->slots[op->target], *scope = &s->slots[op->scope];
        int found = 0;
        for (size_t i = 0; i < s->nservices; ++i) found |= name_eq(s->services[i], s->service_len[i], t->name, t->name_len);
        if (found) {
            scope->state = SIM_READY;
            rc = (int64_t)op->scope;
        } else {
            scope->state = SIM_RETIRED;
            rc = SIM_ENOENT;
        }
        break;
    }
    case SIM_OP_WALK:
        if (has_file && op->name_len) {
            rc = SIM_ENOTDIR;
        } else if (!op->name_len || file_of(s, op->name, op->name_len, 0)) {
            struct sim_slot *d = &s->slots[op->dest];
            d->state = SIM_READY;
            if (op->name_len) {
                memcpy(d->name, op->name, op->name_len);
                d->name_len = op->name_len;
                d->has_name = 1;
            } else {
                memcpy(d->name, file, file_len);
                d->name_len = file_len;
                d->has_name = has_file;
            }
            rc = (int64_t)op->dest;
        } else {
            rc = SIM_ENOENT;
        }
        break;
    case SIM_OP_OPEN:
        src->open = 1;
        break;
    case SIM_OP_READ: {
        /* The root is the service's only directory: a byte READ of it fails. */
        if (!has_file) {
            rc = SIM_EISDIR;
            break;
        }
        struct sim_file *f = file_of(s, file, file_len, 0);
        size_t flen = f ? f->len : 0, start = op->offset < flen ? (size_t)op->offset : flen;
        size_t n = flen - start < op->len ? flen - start : op->len;
        unsigned char *dst = buffer(s, op->buf, op->at, op->len);
        assert(dst);
        if (n) memcpy(dst, f->data + start, n);
        rc = (int64_t)n;
        break;
    }
    case SIM_OP_WRITE: {
        unsigned char *src_bytes = buffer(s, op->buf, op->at, op->len);
        assert(src_bytes && has_file);
        struct sim_file *f = file_of(s, file, file_len, 1);
        size_t end = (size_t)op->offset + op->len;
        assert(end <= sizeof(f->data));
        if (f->len < end) {
            memset(f->data + f->len, 0, end - f->len);
            f->len = end;
        }
        memcpy(f->data + op->offset, src_bytes, op->len);
        rc = op->len;
        break;
    }
    case SIM_OP_POOLED:
        assert(has_file);
        for (;;) {
            unsigned char shot[256];
            size_t shot_len = 0;
            int got = 0;
            struct sim_shots *q = shots_of(s, file, file_len, 0);
            if (op->multi) {
                if (q && q->count) {
                    shot_len = q->q[q->head].len;
                    memcpy(shot, q->q[q->head].b, shot_len);
                    q->head = (q->head + 1) % 64;
                    --q->count;
                    got = 1;
                }
            } else {
                struct sim_file *f = file_of(s, file, file_len, 0);
                size_t flen = f ? f->len : 0, start = op->offset < flen ? (size_t)op->offset : flen;
                size_t n = flen - start < op->limit ? flen - start : op->limit;
                assert(n <= sizeof(shot));
                if (n) {
                    memcpy(shot, f->data + start, n);
                    shot_len = n;
                    got = 1;
                }
            }
            if (!got) {
                post(s, 1, op->scope, op->user_data, 0, 0, ZERO_RECEIPT);
                return 0;
            }
            assert(shot_len <= op->limit);
            uint64_t lease = s->next_lease;
            size_t m = 0;
            while (m < s->pool_members[op->pool] &&
                   (s->pools[op->pool][m].leased || s->pools[op->pool][m].range.length < shot_len))
                ++m;
            if (m == s->pool_members[op->pool]) {
                /* No free member: the stream waits for a RETURN. */
                if (op->multi) {
                    q->head = (q->head + 63) % 64;
                    ++q->count;
                    memcpy(q->q[q->head].b, shot, shot_len);
                    q->q[q->head].len = shot_len;
                }
                return 1;
            }
            struct sim_member *mem = &s->pools[op->pool][m];
            mem->leased = 1;
            mem->lease = lease;
            ++s->next_lease;
            unsigned char *dst = buffer(s, mem->range.buffer_index, mem->range.offset, shot_len);
            assert(dst);
            memcpy(dst, shot, shot_len);
            struct t_loom_service_buffer_receipt receipt = {
                { (uint32_t)op->pool, 0, s->slots[op->pool].inc }, (uint32_t)m, 0, lease };
            post(s, 1, op->scope, op->user_data, (int32_t)shot_len,
                 T_LOOM_CQE_SERVICE_BUFFER | (op->multi ? T_LOOM_CQE_MORE : 0), receipt);
            if (!op->multi) return 0;
        }
    case SIM_OP_CLUNK:
        ++s->clunks;
        src->state = SIM_RETIRED;
        break;
    }
    post(s, 1, op->scope, op->user_data, (int32_t)rc, 0, ZERO_RECEIPT);
    return 0;
}
/* The server's turn: deadlines first, then every pending answer. */
void sim_step(struct sim *s) {
    uint64_t now = s->clock;
    size_t expired[SIM_MAX], nexpired = 0;
    for (size_t i = 0; i < s->nops; ++i)
        if (s->ops[i].deadline && s->ops[i].deadline <= now) expired[nexpired++] = s->ops[i].scope;
    for (size_t e = 0; e < nexpired; ++e) {
        /* Any operation timeout terminally aborts its whole scope. */
        size_t scope = expired[e];
        for (size_t i = 0; i < s->nops;) {
            if (s->ops[i].scope != scope) {
                ++i;
                continue;
            }
            struct sim_op op = s->ops[i];
            remove_op(s, i);
            post(s, 1, scope, op.user_data, op.deadline && op.deadline <= now ? SIM_ETIMEDOUT : SIM_ECANCELED, 0,
                 ZERO_RECEIPT);
        }
        if (s->slots[scope].used) s->slots[scope].state = SIM_ABORTING;
    }
    if (!s->hold) {
        size_t n = s->nops;
        for (size_t k = 0; k < n; ++k) {
            struct sim_op op = s->ops[0];
            remove_op(s, 0);
            if (answer(s, &op)) s->ops[s->nops++] = op;
        }
    }
    settle(s);
    flush(s);
}

static int64_t shape(const struct t_loom_sqe *q) {
    int no_buffer = !q->len && !q->buf_idx_or_off && !q->_resv1[0];
    int fixed = q->buf_idx_or_off < 64 && q->_resv1[0] <= UINT64_MAX - q->len;
    int dest = q->offset < 64 && q->_resv1[2] && q->offset != q->handle_idx;
    if (q->_resv0 || q->handle_idx >= 64 || !q->_resv1[1]) return SIM_EINVAL;
    int pooled = q->opcode == T_LOOM_OP_READ && (q->flags & T_LOOM_SQE_BUFFER_SELECT);
    unsigned allowed = pooled ? T_LOOM_SQE_BUFFER_SELECT | T_LOOM_SQE_MULTISHOT : 0;
    if (q->flags & ~allowed) return SIM_EINVAL;
    switch (q->opcode) {
    case T_LOOM_OP_SERVICE_CONNECT:
        return !no_buffer || !dest ? SIM_EINVAL : 0;
    case T_LOOM_OP_WALK:
        return !dest || q->len > 255 || (q->len ? !fixed : !no_buffer) ? SIM_EINVAL : 0;
    case T_LOOM_OP_LOPEN:
        return q->offset > 2 || q->_resv1[2] || !no_buffer ? SIM_EINVAL : 0;
    case T_LOOM_OP_READ:
        if (pooled) return !q->len || q->buf_idx_or_off >= 64 || !q->_resv1[2] || q->_resv1[0] ? SIM_EINVAL : 0;
        return q->_resv1[2] || !fixed ? SIM_EINVAL : 0;
    case T_LOOM_OP_WRITE:
        return q->_resv1[2] || !fixed ? SIM_EINVAL : 0;
    case T_LOOM_OP_CLUNK:
        return q->offset || q->_resv1[2] || !no_buffer ? SIM_EINVAL : 0;
    default:
        return -95;
    }
}
/* ENTER's admission of one copied SQE: shape, then the slots it names. */
static int64_t admit_sqe(struct sim *s, const struct t_loom_sqe *q) {
    int64_t rc = shape(q);
    if (rc) return rc;
    struct t_loom_service_ref source = { q->handle_idx, 0, q->_resv1[1] };
    struct sim_slot *src = live(s, source);
    if (!src) return SIM_ENOENT;
    struct t_loom_service_ref dest = { (uint32_t)q->offset, 0, q->_resv1[2] };
    struct sim_op op = { .user_data = q->user_data, .source = source.slot, .offset = q->offset,
                         .deadline = q->_resv1[3] };
    if (q->opcode == T_LOOM_OP_SERVICE_CONNECT) {
        struct sim_slot *d = live(s, dest);
        if (!d || src->kind != SIM_TARGET || d->kind != SIM_SCOPE || d->state != SIM_RESERVED) return SIM_ENOENT;
        op.scope = dest.slot;
        op.kind = SIM_OP_CONNECT;
        op.target = source.slot;
    } else {
        if (q->opcode == T_LOOM_OP_CLUNK) ++s->clunk_sqes;
        size_t scope;
        if (src->kind == SIM_SCOPE)
            scope = source.slot;
        else if (src->kind == SIM_FID)
            scope = src->scope;
        else
            return SIM_ENOENT;
        /* The kernel's order (loom_fid_borrow, loom_fid_begin_clunk): the
         * scope first, then the FID itself. */
        if (s->slots[scope].state != SIM_READY) return SIM_ECANCELED;
        if (src->state != SIM_READY) return SIM_ENOENT;
        op.scope = scope;
        if (q->opcode == T_LOOM_OP_WALK) {
            struct sim_slot *d = live(s, dest);
            if (!d || d->kind != SIM_FID || d->state != SIM_RESERVED || d->scope != scope) return SIM_ENOENT;
            if (q->len) {
                unsigned char *p = buffer(s, q->buf_idx_or_off, q->_resv1[0], q->len);
                if (!p) return SIM_EINVAL;
                /* Copied here, inside ENTER, as loom_private_io_prepare does. */
                memcpy(op.name, p, q->len);
                op.name_len = q->len;
            }
            for (size_t i = 0; i < op.name_len; ++i)
                if (!op.name[i] || op.name[i] == '/') return SIM_EINVAL;
            if ((op.name_len == 1 && op.name[0] == '.') || (op.name_len == 2 && op.name[0] == '.' && op.name[1] == '.'))
                return SIM_EINVAL;
            op.kind = SIM_OP_WALK;
            op.dest = dest.slot;
        } else if (q->opcode == T_LOOM_OP_LOPEN) {
            op.kind = SIM_OP_OPEN;
        } else if (q->opcode == T_LOOM_OP_CLUNK) {
            op.kind = SIM_OP_CLUNK;
        } else if (q->flags & T_LOOM_SQE_BUFFER_SELECT) {
            struct t_loom_service_ref pool = { q->buf_idx_or_off, 0, q->_resv1[2] };
            struct sim_slot *p = live(s, pool);
            if (!p || p->kind != SIM_POOL) return SIM_ENOENT;
            op.kind = SIM_OP_POOLED;
            op.pool = pool.slot;
            op.limit = q->len;
            op.multi = !!(q->flags & T_LOOM_SQE_MULTISHOT);
        } else {
            if (!buffer(s, q->buf_idx_or_off, q->_resv1[0], q->len)) return SIM_EINVAL;
            op.kind = q->opcode == T_LOOM_OP_READ ? SIM_OP_READ : SIM_OP_WRITE;
            op.buf = q->buf_idx_or_off;
            op.at = q->_resv1[0];
            op.len = q->len;
        }
    }
    assert(s->nops < SIM_MAX);
    s->ops[s->nops++] = op;
    return 0;
}

static int header_ok(uint32_t size, uint32_t want, uint16_t version, uint16_t flags) {
    return size == want && version == 1 && !flags;
}
static void *region(struct sim *s, size_t len) {
    void *va = NULL;
    assert(!posix_memalign(&va, 4096, len));
    memset(va, 0, len);
    assert(s->nregions < 128);
    s->regions[s->nregions++] = (struct sim_region){ (uintptr_t)va, len };
    return va;
}
static int64_t k_setup(void *v, uint32_t entries, struct t_owned_setup *p) {
    struct sim *s = v;
    if (p->flags != 12 || !entries || entries > 4096 || (entries & (entries - 1))) return SIM_EINVAL;
    uint32_t cq = entries * 2;
#define UP(n) (((n) + 63u) & ~63u)
    uint32_t sq_array_off = 64, sqe_off = UP(sq_array_off + entries * 4), cqe_off = UP(sqe_off + entries * 64);
    uint32_t receipt_off = UP(cqe_off + cq * 16), size = (receipt_off + cq * 32 + 4095) & ~4095u;
#undef UP
    void *va = region(s, size);
    *p = (struct t_owned_setup){ .flags = 12, .sq_entries = entries, .cq_entries = cq, .ring_size = size,
                                 .ring_va = (uintptr_t)va, .hdr_off = 0, .sq_array_off = sq_array_off,
                                 .sqe_off = sqe_off, .cqe_off = cqe_off, .sq_array_size = entries * 4,
                                 .sqe_size = entries * 64, .cqe_size = cq * 16,
                                 .receipt = { receipt_off, (uint64_t)cq * 32, 32, 0 } };
    s->has_ring = 1;
    s->ring_fd = 7;
    s->ring_va = (uintptr_t)va;
    s->p = *p;
    s->closing = 0;
    s->sq_head = 0;
    s->cq_tail = 0;
    sim_poke_hdr(s, 2, entries - 1);
    sim_poke_hdr(s, 3, entries);
    sim_poke_hdr(s, 6, cq - 1);
    sim_poke_hdr(s, 7, cq);
    return 7;
}
static int64_t k_reg(void *v, int32_t fd, uint32_t op, void *arg, uint32_t nargs) {
    struct sim *s = v;
    if (!s->has_ring || s->ring_fd != fd || sim_closed(s, fd)) return SIM_EBADF;
    if (op == T_LOOM_REGISTER_BUFFERS) {
        int used = 0;
        for (size_t i = 0; i < 64; ++i) used |= s->pool_members[i] != 0;
        if (nargs > 64) return SIM_EINVAL;
        if (used) return SIM_EBUSY;
        const struct t_loom_buf_reg *table = arg;
        for (uint32_t i = 0; i < nargs; ++i) {
            int inside = 0;
            for (size_t j = 0; j < s->nregions; ++j)
                inside |= table[i].va >= s->regions[j].va &&
                          table[i].va + table[i].len <= s->regions[j].va + s->regions[j].len;
            if (!inside || table[i].va == s->ring_va) return SIM_EINVAL;
        }
        for (uint32_t i = 0; i < nargs; ++i) s->buffers[i] = (struct sim_region){ table[i].va, table[i].len };
        s->nbuffers = nargs;
        return 0;
    }
    if (nargs != 1) return SIM_EINVAL;
    int closing = s->closing;
    switch (op) {
    case T_LOOM_REGISTER_SERVICE_TARGET: {
        struct t_loom_service_target *t = arg;
        if (!header_ok(t->size, 288, t->version, t->flags) || !t->name_len || t->name_len > 255) return SIM_EINVAL;
        /* loom_private_target_prepare resolves the registry first. */
        if (t->registry_fd != s->registry_fd) return SIM_EBADF;
        if (closing) return SIM_ECANCELED;
        for (uint32_t i = 0; i < t->name_len; ++i)
            if (!t->name[i] || t->name[i] == '/') return SIM_EINVAL;
        struct sim_slot slot = { .kind = SIM_TARGET, .state = SIM_READY, .has_name = 1 };
        assert(t->name_len <= SIM_NAME);
        memcpy(slot.name, t->name, t->name_len);
        slot.name_len = t->name_len;
        if (!mint(s, slot, &t->result)) return SIM_ENOSPC;
        return 0;
    }
    case T_LOOM_REGISTER_SERVICE_SLOT: {
        struct t_loom_service_reserve *rec = arg;
        if (!header_ok(rec->size, 48, rec->version, rec->flags) || rec->reserved) return SIM_EINVAL;
        if (closing) return SIM_ECANCELED;
        struct sim_slot slot = { .state = SIM_RESERVED };
        if (rec->kind == T_LOOM_SERVICE_SCOPE && !rec->scope.slot && !rec->scope.reserved && !rec->scope.incarnation) {
            slot.kind = SIM_SCOPE;
        } else if (rec->kind == T_LOOM_SERVICE_FID) {
            struct sim_slot *sc = live(s, rec->scope);
            if (!sc || sc->kind != SIM_SCOPE || sc->state != SIM_READY) return SIM_ENOENT;
            slot.kind = SIM_FID;
            slot.scope = rec->scope.slot;
        } else {
            return SIM_EINVAL;
        }
        if (!mint(s, slot, &rec->result)) return SIM_ENOSPC;
        if (slot.kind == SIM_SCOPE) s->slots[rec->result.slot].scope = rec->result.slot;
        return 0;
    }
    case T_LOOM_REGISTER_ABORT_SCOPE: {
        const struct t_loom_service_control *c = arg;
        struct sim_slot *sl = live(s, c->object);
        if (!sl) return SIM_ENOENT;
        if (sl->kind != SIM_SCOPE) return SIM_EINVAL;
        assert(s->naborts < 256);
        s->aborts[s->naborts++] = c->object.slot;
        if (sl->state == SIM_ABORTING || sl->state == SIM_RETIRED) return 1;
        if (sl->state == SIM_RESERVED) sl->state = SIM_RETIRED;
        cancel_scope(s, c->object.slot, SIM_ECANCELED);
        return 0;
    }
    case T_LOOM_REGISTER_QUERY_SERVICE_SLOT: {
        struct t_loom_service_snapshot *q = arg;
        struct sim_slot *sl = live(s, q->object);
        if (!sl) return SIM_ENOENT;
        q->state = sl->state;
        if (sl->kind == SIM_SCOPE) {
            /* loom_private_service_query: operations not yet terminal, and
             * terminals not yet posted. */
            size_t i = q->object.slot, active = 0, pending = 0;
            for (size_t j = 0; j < s->nops; ++j) active += s->ops[j].scope == i;
            for (size_t j = 0; j < s->ndeferred; ++j)
                pending += s->deferred[j].has_scope && s->deferred[j].scope == i &&
                           !(s->deferred[j].flags & T_LOOM_CQE_MORE);
            for (size_t j = 0; j < s->nheld; ++j) pending += s->held[j].scope == i;
            q->active_ops = (uint32_t)active;
            q->pending_terminals = (uint32_t)pending;
        }
        q->kind = sl->kind;
        return 0;
    }
    case T_LOOM_REGISTER_REAP_SERVICE_SLOT: {
        const struct t_loom_service_control *c = arg;
        if (!live(s, c->object)) return SIM_ENOENT;
        size_t i = c->object.slot;
        if (!reapable(s, i)) return SIM_EBUSY;
        memset(&s->slots[i], 0, sizeof(s->slots[i]));
        s->pool_members[i] = 0;
        assert(s->nreaps < 256);
        s->reaps[s->nreaps++] = i;
        return 0;
    }
    case T_LOOM_REGISTER_SERVICE_POOL: {
        struct t_loom_service_pool_create *p = arg;
        if (!header_ok(p->size, 1568, p->version, p->flags) || !p->count || p->count > 64) return SIM_EINVAL;
        if (closing) return SIM_ECANCELED;
        for (uint32_t i = 0; i < p->count; ++i)
            if (!buffer(s, p->members[i].buffer_index, p->members[i].offset, (uint32_t)p->members[i].length))
                return SIM_EINVAL;
        struct sim_slot slot = { .kind = SIM_POOL, .state = SIM_READY };
        if (!mint(s, slot, &p->result)) return SIM_ENOSPC;
        size_t i = p->result.slot;
        for (uint32_t m = 0; m < p->count; ++m) s->pools[i][m] = (struct sim_member){ p->members[m], 0, 0 };
        s->pool_members[i] = p->count;
        return 0;
    }
    case T_LOOM_REGISTER_RETURN_SERVICE_BUFFER: {
        const struct t_loom_service_buffer_return *b = arg;
        if (!header_ok(b->size, 40, b->version, b->flags)) return SIM_EINVAL;
        if (s->has_refuse_return) {
            s->has_refuse_return = 0;
            return s->refuse_return;
        }
        struct t_loom_service_buffer_receipt r = b->receipt;
        if (!live(s, r.pool)) return SIM_ENOENT;
        if (r.member >= s->pool_members[r.pool.slot]) return SIM_EINVAL;
        struct sim_member *m = &s->pools[r.pool.slot][r.member];
        if (!m->leased || m->lease != r.lease) return SIM_EINVAL;
        m->leased = 0;
        ++s->returns;
        return 0;
    }
    default:
        return SIM_EINVAL;
    }
}
static int64_t k_enter(void *v, int32_t fd, uint32_t to_submit, uint32_t min_complete, uint32_t flags) {
    struct sim *s = v;
    if (!s->has_ring || s->ring_fd != fd || sim_closed(s, fd)) return SIM_EBADF;
    if (min_complete || flags != T_LOOM_ENTER_NONBLOCK) return SIM_EINVAL;
    if (s->closing) return SIM_ECANCELED;
    if (s->has_enter_errno) return s->enter_errno;
    ++s->enters;
    flush(s);
    uint32_t entries = s->p.sq_entries, budget = to_submit < entries ? to_submit : entries;
    if (s->has_enter_limit && s->enter_limit < budget) budget = s->enter_limit;
    int64_t consumed = 0;
    unsigned char *base = (unsigned char *)s->ring_va;
    for (uint32_t k = 0; k < budget; ++k) {
        uint32_t tail = sim_hdr_word(s, 1);
        if (!cq_room(s) || s->ndeferred || s->nrejected || tail == s->sq_head) break;
        uint32_t head = s->sq_head;
        uint32_t index = atomic_load_explicit(
            (_Atomic uint32_t *)(void *)(base + s->p.sq_array_off + 4 * (head & (entries - 1))), memory_order_acquire);
        struct t_loom_sqe copy;
        int valid = index < entries;
        if (valid)
            memcpy(&copy, base + s->p.sqe_off + 64 * (size_t)index, sizeof(copy));
        else
            sim_poke_hdr(s, 9, ++s->dropped);
        s->sq_head = head + 1;
        sim_poke_hdr(s, 0, head + 1);
        ++consumed;
        if (valid) {
            int64_t rc = admit_sqe(s, &copy);
            if (rc < 0) {
                if (s->hold_rejections) {
                    assert(s->nrejected < SIM_MAX);
                    s->rejected[s->nrejected++] = (struct sim_cqe){ 0, 0, copy.user_data, (int32_t)rc, 0, ZERO_RECEIPT };
                } else {
                    post(s, 0, 0, copy.user_data, (int32_t)rc, 0, ZERO_RECEIPT);
                }
            }
        }
    }
    return consumed;
}
static int ready(struct sim *s) { return sim_hdr_word(s, 5) != sim_hdr_word(s, 4); }
static int64_t k_poll(void *v, int32_t fd, int16_t events, int32_t timeout_ms) {
    struct sim *s = v;
    assert(events == POLLIN);
    assert(s->npolls < 1024);
    s->polls[s->npolls++] = timeout_ms;
    if (!s->has_ring || s->ring_fd != fd || sim_closed(s, fd)) return POLLNVAL;
    if (s->closing) return POLLHUP;
    if (s->has_poll_answer) {
        s->has_poll_answer = 0;
        return s->poll_answer;
    }
    if (ready(s)) return POLLIN;
    sim_step(s);
    if (ready(s)) return POLLIN;
    /* Sleep until the timeout or the earliest operation deadline, whose expiry
     * the kernel posts as a completion. */
    int has_due = 0;
    uint64_t due = 0;
    for (size_t i = 0; i < s->nops; ++i)
        if (s->ops[i].deadline && (!has_due || s->ops[i].deadline < due)) {
            due = s->ops[i].deadline;
            has_due = 1;
        }
    uint64_t wake;
    if (timeout_ms >= 0) {
        uint64_t until = s->clock + (uint64_t)timeout_ms * 1000000;
        wake = has_due && due < until ? due : until;
    } else {
        /* poll would block forever: nothing in flight can complete. */
        assert(has_due);
        wake = due;
    }
    if (wake > s->clock) s->clock = wake;
    sim_step(s);
    return ready(s) ? POLLIN : 0;
}
static int64_t k_close(void *v, int32_t fd) {
    struct sim *s = v;
    /* A second close of a number, which may name another file now. */
    assert(!sim_closed(s, fd));
    if (!s->has_ring || s->ring_fd != fd) return SIM_EBADF;
    assert(s->nclosed < 16);
    s->closed_fds[s->nclosed++] = fd;
    s->closing = 1;
    abort_all(s);
    return 0;
}
static int64_t k_attach(void *v, size_t len) {
    struct sim *s = v;
    if (++s->attaches == s->attach_fail_at) return SIM_ENOMEM;
    /* The adapter attaches whole pages. */
    assert(len && !(len % 4096));
    return (int64_t)(uintptr_t)region(s, len);
}
static int64_t k_detach(void *v, uintptr_t va, size_t len) {
    struct sim *s = v;
    for (size_t i = 0; i < s->nregions; ++i) {
        if (s->regions[i].va != va) continue;
        /* Detach names the whole region. */
        assert(s->regions[i].len == len);
        free((void *)va);
        s->regions[i] = s->regions[--s->nregions];
        return 0;
    }
    /* Detach names an attached region. */
    assert(0);
    return SIM_EINVAL;
}
static uint64_t k_now(void *v) { return ((struct sim *)v)->clock; }
struct t_loom_kernel sim_kernel(struct sim *s) {
    return (struct t_loom_kernel){ s, k_setup, k_reg, k_enter, k_poll, k_close, k_attach, k_detach, k_now };
}
