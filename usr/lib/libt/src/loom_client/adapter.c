// The native C adapter (docs/ASYNC-SERVICE-ABI.md, "Native adapter"): the C
// twin of loom-client's native module, over a table of raw kernel calls.
#include <thyla/loom_client/adapter.h>
#include <thyla/poll.h>
#include <stdatomic.h>

enum { E_NOENT = 2, E_INTR = 4, E_IO = 5, E_BADF = 9, E_BUSY = 16, E_INVAL = 22, E_NOSPC = 28,
       E_CANCELED = 125 };
enum { PAGE = 4096, NAME_CELL = 256, MAX_REG_BUFFERS = 64 };
enum { R_OPEN = 1, R_RETIRING, R_CLOSED, R_FAILED };
enum { S_TARGET = 1, S_SCOPE, S_FID };
enum { S_LIVE = 1, S_CLUNK, S_CLUNKING, S_REAP };
enum { OP_CONNECT = 1, OP_WALK, OP_OPEN, OP_READ, OP_WRITE, OP_STREAM };

static const struct t_loom_service_ref ZERO_REF;

static int same_ref(struct t_loom_service_ref a, struct t_loom_service_ref b) {
    return a.slot == b.slot && a.reserved == b.reserved && a.incarnation == b.incarnation;
}
static int minted(struct t_loom_service_ref r) {
    return r.slot < 64 && !r.reserved && r.incarnation;
}
// Zero means "no deadline" to the kernel, so an absolute zero becomes one.
static uint64_t deadline_word(int has_deadline, uint64_t deadline) {
    return has_deadline ? (deadline ? deadline : 1) : 0;
}

// ---- What the ring holds in the address space: descriptor, mapping, buffers.

static void close_fd(struct t_loom_ring *r) {
    int32_t fd = r->fd;
    r->fd = -1;
    if (fd >= 0) r->k.close(r->k.context, fd);
}
// The ring was closed under the adapter. Its descriptor is the only one (a
// private ring is never duplicated or inherited, and exec latches it only past
// the point of no return), so another thread closed it and the number may
// already name another file: it is dropped, never closed.
static void forget_fd(struct t_loom_ring *r) {
    r->fd = -1;
    r->lost = 1;
}
// The last of the ring, the core backend and the mapping backend is gone: the
// mapping (only once validated) and every region are detached, and the storage
// is idle. Nothing touches the ring after this.
static void release_holder(struct t_loom_ring *r) {
    T_LOOM_CHECK(r->holders);
    if (--r->holders) return;
    close_fd(r);
    if (r->ring_valid) r->k.detach(r->k.context, r->ring_va, r->ring_len);
    for (size_t i = 0; i < r->regions; ++i) r->k.detach(r->k.context, r->region_va[i], r->region_len[i]);
    t_bytes_zero(r, sizeof(*r));
}

static struct t_loom_service_snapshot snapshot(struct t_loom_service_ref object) {
    return (struct t_loom_service_snapshot){ .size = 64, .version = 1, .object = object };
}
// Whether the descriptor still names the ring, asked with a QUERY of the zero
// reference: a live ring answers -ENOENT, a dead number -EBADF.
static int gone(struct t_loom_ring *r) {
    if (r->fd < 0) return (int)r->lost;
    struct t_loom_service_snapshot s = snapshot(ZERO_REF);
    int64_t rc = r->k.reg(r->k.context, r->fd, T_LOOM_REGISTER_QUERY_SERVICE_SLOT, &s, 1);
    if (rc == -E_BADF) forget_fd(r);
    return rc == -E_BADF;
}
// One versioned record; after close the ring is gone and nothing is sent. The
// kernel checks the ring's descriptor before it reads the record, so -EBADF
// says the number is closed or names another kind of file: it is forgotten, and
// the call answers -ECANCELED as a closed ring does. A TARGET's registry
// descriptor earns the same answer, so a probe decides.
static int64_t reg(struct t_loom_ring *r, uint32_t op, void *record) {
    if (r->fd < 0) return -E_CANCELED;
    int64_t rc = r->k.reg(r->k.context, r->fd, op, record, 1);
    if (rc == -E_BADF && (op != T_LOOM_REGISTER_SERVICE_TARGET || gone(r))) {
        forget_fd(r);
        return -E_CANCELED;
    }
    return rc;
}
static int64_t control(struct t_loom_ring *r, uint32_t op, struct t_loom_service_ref object) {
    struct t_loom_service_control c = { .size = 32, .version = 1, .object = object };
    return reg(r, op, &c);
}
static int64_t return_buffer(struct t_loom_ring *r, struct t_loom_service_buffer_receipt receipt) {
    struct t_loom_service_buffer_return b = { .size = 40, .version = 1, .receipt = receipt };
    return reg(r, T_LOOM_REGISTER_RETURN_SERVICE_BUFFER, &b);
}
static int reserve_once(struct t_loom_ring *r, uint32_t kind, struct t_loom_service_ref scope,
                        struct t_loom_service_ref *out) {
    struct t_loom_service_reserve rec = { .size = 48, .version = 1, .kind = kind, .scope = scope };
    int64_t rc = reg(r, T_LOOM_REGISTER_SERVICE_SLOT, &rec);
    if (rc < 0) return (int)rc;
    if (!minted(rec.result)) return -E_IO;
    *out = rec.result;
    return 0;
}
static int query_state(struct t_loom_ring *r, struct t_loom_service_ref object, uint32_t *state) {
    struct t_loom_service_snapshot s = snapshot(object);
    int64_t rc = reg(r, T_LOOM_REGISTER_QUERY_SERVICE_SLOT, &s);
    if (rc < 0) return (int)rc;
    *state = s.state;
    return 0;
}
static struct t_loom_slot *track(struct t_loom_ring *r, struct t_loom_service_ref ref, unsigned kind,
                                 struct t_loom_service_ref scope) {
    struct t_loom_slot *e = &r->slots[ref.slot];
    // The kernel minted a slot the adapter still holds.
    T_LOOM_CHECK(!e->kind);
    *e = (struct t_loom_slot){ .ref = ref, .scope = scope, .kind = kind, .state = S_LIVE };
    return e;
}
static struct t_loom_slot *entry(struct t_loom_ring *r, struct t_loom_service_ref ref) {
    if (ref.slot >= 64) return NULL;
    struct t_loom_slot *e = &r->slots[ref.slot];
    return e->kind && same_ref(e->ref, ref) ? e : NULL;
}
static void set_state(struct t_loom_ring *r, struct t_loom_service_ref ref, unsigned state) {
    struct t_loom_slot *e = entry(r, ref);
    if (e) e->state = state;
}
static void free_slot(struct t_loom_slot *e) {
    T_LOOM_CHECK(!e->handle && !e->refs && !e->clunking);
    t_bytes_zero(e, sizeof(*e));
}
// ABORT_SCOPE: the whole connection, every sibling operation included.
static void abort_scope(struct t_loom_ring *r, struct t_loom_service_ref scope) {
    struct t_loom_slot *e = entry(r, scope);
    if (!e || e->aborted) return;
    e->aborted = 1;
    control(r, T_LOOM_REGISTER_ABORT_SCOPE, scope);
}
// REAP every slot waiting on it. A scope's fids pin it, so passes repeat while
// one frees a slot. A busy slot is retried on a later turn; a refusal other than
// EBUSY leaves it for close to reclaim.
static void reap_pass(struct t_loom_ring *r) {
    int freed;
    do {
        freed = 0;
        for (size_t i = 0; i < 64; ++i) {
            struct t_loom_slot *e = &r->slots[i];
            if (!e->kind || e->state != S_REAP) continue;
            int64_t rc = control(r, T_LOOM_REGISTER_REAP_SERVICE_SLOT, e->ref);
            if (rc == 0 || rc == -E_NOENT) {
                free_slot(e);
                freed = 1;
            } else if (rc != -E_BUSY) {
                free_slot(e);
            }
        }
    } while (freed);
}

// ---- The client cores' backend: buffers, pools and whole-ring retirement.

static size_t b_count(void *v) { return ((struct t_loom_ring *)v)->buffers; }
static size_t b_length(void *v, size_t i) { return ((struct t_loom_ring *)v)->region_len[i]; }
static const unsigned char *b_buffer(void *v, size_t i) {
    return (const unsigned char *)((struct t_loom_ring *)v)->region_va[i];
}
static int64_t b_return(void *v, struct t_loom_service_buffer_receipt receipt) { return return_buffer(v, receipt); }
// The kernel has no pool retirement; the core's own latch stops new reads, and
// REAP (destroy_pool) frees the slot once its streams have ended.
static void b_retire_pool(void *v, struct t_loom_service_ref pool) { (void)v; (void)pool; }
static int64_t b_destroy_pool(void *v, struct t_loom_service_ref pool) {
    return control(v, T_LOOM_REGISTER_REAP_SERVICE_SLOT, pool);
}
static void b_begin_retire(void *v) {
    struct t_loom_ring *r = v;
    r->admission = 0;
    for (size_t i = 0; i < 64; ++i)
        if (r->slots[i].kind == S_SCOPE) abort_scope(r, r->slots[i].ref);
}
// Each SQE the kernel consumes ends in exactly one terminal CQE (I-29), so once
// every one pushed has had its terminal taken, none is still to come. RETIRED
// alone does not say that: the engine retires a scope before it posts the
// scope's last terminal, and ENTER can hold a refusal back against a full CQ
// that no request record counts.
static int b_retired(void *v) {
    struct t_loom_ring *r = v;
    if (r->admission || r->fd < 0) return 0;
    if (r->submitted != r->ended) return 0;
    for (size_t i = 0; i < 64; ++i) {
        if (r->slots[i].kind != S_SCOPE) continue;
        uint32_t state = 0;
        int rc = query_state(r, r->slots[i].ref, &state);
        if (rc < 0 ? rc != -E_NOENT : state != T_LOOM_SERVICE_RETIRED) return 0;
    }
    return 1;
}
static void b_close(void *v) { close_fd(v); }
static void b_destroy(void *v) { release_holder(v); }
static int64_t b_register_pool(void *v, const struct t_loom_service_pool_member *ranges, size_t n,
                               struct t_loom_service_ref *out) {
    // Zeroed by loop: an aggregate initializer this large compiles to memset,
    // which libt does not have.
    struct t_loom_service_pool_create rec;
    t_bytes_zero(&rec, sizeof(rec));
    rec.size = 1568;
    rec.version = 1;
    rec.count = (uint32_t)n;
    for (size_t i = 0; i < n; ++i) rec.members[i] = ranges[i];
    int64_t rc = reg(v, T_LOOM_REGISTER_SERVICE_POOL, &rec);
    *out = rec.result;
    return rc;
}
static void b_abort_scope(void *v, struct t_loom_service_ref scope) { abort_scope(v, scope); }

// The ring mapping as the paired CQ reader's owner sees it. Closing closes the
// descriptor; the ring detaches the mapping once nothing reads it.
static void *m_base(void *v) { return (void *)((struct t_loom_ring *)v)->ring_va; }
static size_t m_length(void *v) { return ((struct t_loom_ring *)v)->ring_len; }
static void m_close(void *v) { close_fd(v); }
static void m_destroy(void *v) { release_holder(v); }

// ---- The SQ's only producer.

static _Atomic uint32_t *word(struct t_loom_ring *r, size_t offset) {
    return (_Atomic uint32_t *)(void *)(r->hdr + offset);
}
static uint32_t sq_pending(struct t_loom_ring *r) {
    uint32_t tail = atomic_load_explicit(word(r, offsetof(struct t_loom_ring_hdr, sq_tail)), memory_order_relaxed);
    return tail - atomic_load_explicit(word(r, offsetof(struct t_loom_ring_hdr, sq_head)), memory_order_acquire);
}
static int sq_space(struct t_loom_ring *r) { return sq_pending(r) < r->sq_entries; }
// The slot the next SQE takes, and with it the name cell a WALK uses.
static uint32_t sq_next_slot(struct t_loom_ring *r) {
    return atomic_load_explicit(word(r, offsetof(struct t_loom_ring_hdr, sq_tail)), memory_order_relaxed) &
           (r->sq_entries - 1);
}
// sq_space held, and nothing fallible remains for this submission: the release
// store publishes it to the kernel.
static void sq_push(struct t_loom_ring *r, const struct t_loom_sqe *sqe) {
    _Atomic uint32_t *tail_word = word(r, offsetof(struct t_loom_ring_hdr, sq_tail));
    uint32_t tail = atomic_load_explicit(tail_word, memory_order_relaxed);
    uint32_t slot = tail & (r->sq_entries - 1);
    r->sqes[slot] = *sqe;
    r->sq_array[slot] = slot;
    atomic_store_explicit(tail_word, tail + 1, memory_order_release);
}
static int cq_ready(struct t_loom_ring *r) {
    return atomic_load_explicit(word(r, offsetof(struct t_loom_ring_hdr, cq_tail)), memory_order_acquire) !=
           atomic_load_explicit(word(r, offsetof(struct t_loom_ring_hdr, cq_head)), memory_order_relaxed);
}

// ---- Ring state.

static void lost_ring(struct t_loom_ring *r);
static void learn(struct t_loom_ring *r) {
    if (r->lost && (r->state == R_OPEN || r->state == R_RETIRING)) lost_ring(r);
}
static int check(struct t_loom_ring *r) {
    learn(r);
    return r->state == R_CLOSED ? -E_CANCELED : r->state == R_FAILED ? -E_IO : 0;
}
static int admit(struct t_loom_ring *r) {
    learn(r);
    return r->state == R_OPEN ? 0 : r->state == R_FAILED ? -E_IO : -E_CANCELED;
}
// A client core's code as an errno. Kernel errnos pass through.
static int errno_of(int64_t code) {
    if (code == T_FIXED_CLOSING || code == T_LEASE_STATE) return -E_CANCELED;
    if (code == T_LOOM_CLIENT_ENOSPC || code == T_REQ_EXHAUSTED) return -E_NOSPC;
    if (code == T_LEASE_BUSY) return -E_BUSY;
    if (code == T_REQ_GEOMETRY || code == T_LEASE_GEOMETRY || code == T_LEASE_RECEIPT || code == T_LEASE_EXHAUSTED)
        return -E_INVAL;
    if (code < 0 && code > -1000) return (int)code;
    return -E_IO;
}
// A request's error as its owner sees it: a failed ring is -EIO.
static int fault(struct t_loom_ring *r, int64_t code) {
    return r->state == R_FAILED ? -E_IO : errno_of(code);
}
static void shutdown(struct t_loom_ring *r) {
    t_fixed_close(r->driver);
    for (size_t i = 0; i < 64; ++i) {
        struct t_loom_slot *e = &r->slots[i];
        if (e->clunking) {
            e->clunking = 0;
            t_fixed_book_drop(&e->clunk);
        }
    }
}
static void fail(struct t_loom_ring *r) {
    r->state = R_FAILED;
    shutdown(r);
}
static void close_ring(struct t_loom_ring *r) {
    if (r->state == R_OPEN || r->state == R_RETIRING) r->state = R_CLOSED;
    shutdown(r);
}
// The kernel closed the ring under the adapter: forget its descriptor.
static void lost_ring(struct t_loom_ring *r) {
    forget_fd(r);
    close_ring(r);
}

// ENTER(NONBLOCK, min_complete 0) for every queued SQE. A short count leaves the
// rest queued for a later turn; none is ever discarded.
static void enter_ring(struct t_loom_ring *r) {
    if (r->fd < 0) return;
    int64_t rc = r->k.enter(r->k.context, r->fd, sq_pending(r), 0, T_LOOM_ENTER_NONBLOCK);
    // The adapter writes only in-range indices: a dropped SQE is a broken invariant.
    T_LOOM_CHECK(atomic_load_explicit(word(r, offsetof(struct t_loom_ring_hdr, dropped)), memory_order_acquire) ==
                 r->dropped);
    // A ring latched closing answers -ECANCELED, a dead number -EBADF. Any other
    // refusal repeats on every ENTER, so the queued SQEs could never run: the
    // ring fails rather than hang its operations.
    if (rc == -E_CANCELED || rc == -E_BADF)
        lost_ring(r);
    else if (rc < 0)
        fail(r);
}
// Publish one SQE. The book or fixed publish activates its request; nothing
// fallible may follow it, so the space check comes first and the push cannot
// fail.
static int submit(struct t_loom_ring *r, struct t_loom_sqe *sqe, const void *name, size_t len,
                  struct t_fixed_book *book, struct t_fixed_request *fixed, uint64_t *cookie) {
    int rc = admit(r);
    if (rc) return rc;
    if (!sq_space(r)) return -E_NOSPC;
    uint64_t c = 0;
    rc = book ? t_fixed_book_publish(book, &c) : t_fixed_publish(fixed, &c);
    if (rc) return errno_of(rc);
    sqe->user_data = c;
    if (len) {
        // The kernel copies the component while the ENTER that consumes this
        // slot runs, and this thread is the only one that enters: the cell is
        // free again before the slot is.
        uint32_t slot = sq_next_slot(r);
        t_bytes_copy(r->names + (size_t)slot * NAME_CELL, name, len);
        sqe->buf_idx_or_off = r->name_buffer;
        sqe->_resv1[0] = (uint64_t)slot * NAME_CELL;
        sqe->len = (uint32_t)len;
    }
    sq_push(r, sqe);
    ++r->submitted;
    enter_ring(r);
    if (cookie) *cookie = c;
    return 0;
}
static struct t_loom_sqe make_sqe(uint8_t opcode, struct t_loom_service_ref source, int has_deadline,
                                  uint64_t deadline) {
    struct t_loom_sqe s = { .opcode = opcode, .handle_idx = source.slot };
    s._resv1[1] = source.incarnation;
    s._resv1[3] = deadline_word(has_deadline, deadline);
    return s;
}

// ---- Scopes, pools, operations and the payload queue.

static void scope_ref(struct t_loom_ring *r, uint32_t slot) {
    T_LOOM_CHECK(r->slots[slot].kind == S_SCOPE);
    ++r->slots[slot].refs;
}
// The last holder of a scope is gone: it is aborted and its slot reaped.
static void scope_unref(struct t_loom_ring *r, uint32_t slot) {
    struct t_loom_slot *e = &r->slots[slot];
    T_LOOM_CHECK(e->kind == S_SCOPE && e->refs);
    if (--e->refs) return;
    abort_scope(r, e->ref);
    e->state = S_REAP;
}
static void pool_unref(struct t_loom_ring *r, uint32_t cell) {
    struct t_loom_pool_cell *p = &r->pools[cell];
    T_LOOM_CHECK(p->refs);
    if (--p->refs) return;
    struct t_ring_pool *core = p->core;
    t_bytes_zero(p, sizeof(*p));
    t_ring_pool_drop(core);
}
static struct t_loom_op *claim_op(struct t_loom_ring *r) {
    for (size_t i = 0; i < r->op_count; ++i)
        if (!r->ops[i].kind) return &r->ops[i];
    return NULL;
}
static void free_op(struct t_loom_op *op) { t_bytes_zero(op, sizeof(*op)); }
static void queue_push(struct t_loom_ring *r, struct t_loom_op *op, struct t_ring_payload *payload) {
    for (size_t i = 0; i < 64; ++i) {
        if (r->shots[i].op) continue;
        r->shots[i] = (struct t_loom_shot_cell){ op, ++r->shot_seq, payload };
        return;
    }
    // Every queued payload holds one of the bank's 64 members.
    t_loom_client_fatal(__func__, __LINE__);
}
static struct t_ring_payload *queue_pop(struct t_loom_ring *r, struct t_loom_op *op) {
    struct t_loom_shot_cell *first = NULL;
    for (size_t i = 0; i < 64; ++i)
        if (r->shots[i].op == op && (!first || r->shots[i].seq < first->seq)) first = &r->shots[i];
    if (!first) return NULL;
    struct t_ring_payload *payload = first->payload;
    *first = (struct t_loom_shot_cell){ 0 };
    return payload;
}
// The release of every caller handle: once the ring handle and the last of them
// are gone, the driver and the paired reader go, and with them the ring's hold.
static void handle_gone(struct t_loom_ring *r) {
    T_LOOM_CHECK(r->handles);
    --r->handles;
    if (r->root || r->handles) return;
    close_ring(r);
    for (size_t i = 0; i < r->op_count; ++i) {
        struct t_loom_op *op = &r->ops[i];
        if (!op->kind) continue;
        T_LOOM_CHECK(op->kind == OP_STREAM && op->route && !op->handle);
        uint32_t pool = op->pool;
        free_op(op);
        pool_unref(r, pool);
    }
    t_fixed_driver_drop(r->driver);
    t_mapping_destroy(&r->map);
    release_holder(r);
}

// The stream a pooled delivery names: its route is the only holder of its pool
// once the stream handle is gone.
static struct t_loom_op *route_of(struct t_loom_ring *r, uint64_t cookie) {
    for (size_t i = 0; i < r->op_count; ++i)
        if (r->ops[i].kind == OP_STREAM && r->ops[i].route && r->ops[i].cookie == cookie) return &r->ops[i];
    return NULL;
}
static void clunk_done(struct t_loom_ring *r, uint64_t cookie) {
    for (size_t i = 0; i < 64; ++i) {
        struct t_loom_slot *e = &r->slots[i];
        if (!e->clunking || e->clunk.cookie != cookie) continue;
        struct t_owned_cqe c;
        e->clunking = 0;
        if (t_fixed_book_take(&e->clunk, &c) != 1) t_fixed_book_drop(&e->clunk);
        e->state = S_REAP;
        return;
    }
}
static int deliver(struct t_loom_ring *r, const struct t_paired_record *record) {
    struct t_fixed_routed routed;
    if (t_fixed_route(r->driver, record, &routed)) {
        fail(r);
        return -E_IO;
    }
    if (routed.kind == T_ROUTED_CONTROL) clunk_done(r, routed.cookie);
    if (routed.kind != T_ROUTED_POOLED) return 0;
    struct t_request_delivery *d = &routed.delivery;
    struct t_loom_op *op = route_of(r, d->cookie);
    if (!op) {
        fail(r);
        return -E_IO;
    }
    struct t_ring_pool *pool = r->pools[op->pool].core;
    struct t_ring_payload *payload = NULL;
    int rc = t_ring_accept(pool, d->record.cqe.result, d->record.cqe.flags, d->record.receipt, &payload);
    if (rc == 1) {
        if (op->handle)
            queue_push(r, op, payload);
        else
            t_ring_abandon(payload); // An abandoned stream's lease goes straight back.
    } else if (rc == T_LEASE_STATE) {
        // A retiring ring's pools take no new lease, but a shot can still
        // arrive: one already in the CQ, or one the kernel held for CQ room and
        // delivers before the abort's terminal. Unreturned, its member stays
        // leased and REAP refuses the pool. The ledger checks it first: a
        // replayed record must never RETURN a member a live payload still reads.
        struct t_loom_service_buffer_receipt receipt;
        int refused = t_ring_refuse(pool, d->record.cqe.result, d->record.cqe.flags, d->record.receipt, &receipt);
        if (refused < 0) {
            fail(r);
            return -E_IO;
        }
        if (refused == 1) {
            int64_t back = return_buffer(r, receipt);
            // REGISTER found the descriptor gone and forgot it.
            if (back == -E_CANCELED) {
                lost_ring(r);
                return -E_CANCELED;
            }
            if (back != 0 && back != -E_NOENT) {
                fail(r);
                return -E_IO;
            }
        }
    } else if (rc != 0) {
        fail(r);
        return -E_IO;
    }
    if (d->terminal) {
        uint32_t cell = op->pool;
        op->route = 0;
        if (!op->handle) free_op(op);
        pool_unref(r, cell);
    }
    return 0;
}
// Background CLUNKs for fids whose handles are gone. A fid of an aborted scope,
// or of a retiring ring, retires with its scope and is only reaped.
static void clunk_pass(struct t_loom_ring *r) {
    for (size_t i = 0; i < 64; ++i) {
        struct t_loom_slot *e = &r->slots[i];
        if (!e->kind || e->state != S_CLUNK) continue;
        struct t_loom_slot *scope = entry(r, e->scope);
        if (!scope || scope->aborted || scope->state != S_LIVE || admit(r)) {
            e->state = S_REAP;
            continue;
        }
        if (!sq_space(r) || check(r)) return;
        struct t_request_spec spec = { .kind = T_REQ_CONTROL };
        if (t_fixed_book_reserve(r->driver, e->scope, spec, &e->clunk)) return;
        struct t_loom_sqe sqe = make_sqe(T_LOOM_OP_CLUNK, e->ref, 0, 0);
        int rc = submit(r, &sqe, NULL, 0, &e->clunk, NULL, NULL);
        // A ring that closed inside the submit already shut every CLUNK down.
        if (!rc && !check(r)) {
            e->state = S_CLUNKING;
            e->clunking = 1;
        } else {
            t_fixed_book_drop(&e->clunk);
        }
    }
}
static void begin_retire(struct t_loom_ring *r) {
    if (r->state != R_OPEN) return;
    r->state = R_RETIRING;
    t_fixed_begin_retire(r->driver);
}

// ---- The loop turn.

static int64_t service(struct t_loom_ring *r) {
    int rc = check(r);
    if (rc) return rc;
    if (sq_pending(r)) {
        enter_ring(r);
        // A close learnt here must not reach the routing below as a failure.
        if ((rc = check(r))) return rc;
    }
    size_t routed = 0;
    while (routed < r->op_count) {
        struct t_paired_record record;
        int got = t_mapping_reap(&r->map, &record);
        if (!got) break;
        if (got < 0) {
            fail(r);
            return -E_IO;
        }
        if (!(record.cqe.flags & T_LOOM_CQE_MORE)) ++r->ended;
        if ((rc = deliver(r, &record))) return rc;
        ++routed;
    }
    // Freed CQ space lets the kernel post what it deferred; ENTER is the kick.
    if (routed) enter_ring(r);
    r->retired = (unsigned)t_fixed_progress(r->driver);
    clunk_pass(r);
    reap_pass(r);
    if ((rc = check(r))) return rc;
    return (int64_t)routed;
}
// The ring's wait: poll(2) on the ring descriptor. The kernel registers the
// waiter before it samples the CQ, which is the atomic compare a wait needs.
// Queued SQEs are not readiness: the kernel stops consuming only for a full CQ,
// which service drains, or a pending rejection, which arrives as a CQE.
static int wait_ring(struct t_loom_ring *r, int has_deadline, uint64_t deadline) {
    int rc = check(r);
    if (rc) return rc;
    if (has_deadline && r->k.now(r->k.context) >= deadline) return 0;
    uint32_t ticket =
        atomic_load_explicit(word(r, offsetof(struct t_loom_ring_hdr, cq_tail)), memory_order_acquire);
    if (cq_ready(r) || (has_deadline && r->k.now(r->k.context) >= deadline)) return 0;
    if (atomic_load_explicit(word(r, offsetof(struct t_loom_ring_hdr, cq_tail)), memory_order_acquire) != ticket)
        return 0;
    if (r->fd < 0) return -E_CANCELED;
    // Rounded up: poll never returns before the deadline it was given.
    int32_t timeout = -1;
    if (has_deadline) {
        uint64_t now = r->k.now(r->k.context), left = deadline > now ? deadline - now : 0;
        uint64_t ms = left / 1000000 + (left % 1000000 != 0);
        timeout = ms > INT32_MAX ? INT32_MAX : (int32_t)ms;
    }
    int64_t events = r->k.poll(r->k.context, r->fd, POLLIN, timeout);
    if (events < 0) return events == -E_INTR ? 0 : (int)events;
    if (!events) return 0;
    if (events & (POLLHUP | POLLNVAL)) {
        // The ring was closed under us; nothing more will arrive.
        lost_ring(r);
        return 0;
    }
    // The kernel answers POLLERR only for a Loom object it no longer
    // recognises: nothing on this ring can be trusted.
    if (events & POLLERR) {
        fail(r);
        return -E_IO;
    }
    return 0;
}

// ---- The ring.

static size_t round_page(size_t len) { return (len + (PAGE - 1)) & ~(size_t)(PAGE - 1); }

int t_loom_ring_setup(struct t_loom_ring *r, struct t_request_entry *book, struct t_loom_op *ops, size_t cq,
                      const struct t_loom_kernel *k, uint32_t entries, const size_t *lengths, size_t n,
                      struct t_loom_ring **out) {
    T_LOOM_CHECK(r && book && ops && k && out && k->setup && k->reg && k->enter && k->poll && k->close &&
                 k->attach && k->detach && k->now);
    // Reusing live storage is a broken invariant.
    T_LOOM_CHECK(!r->used);
    *out = NULL;
    if (!entries || entries > T_LOOM_MAX_ENTRIES || (entries & (entries - 1))) return -E_INVAL;
    if (cq != 2 * (size_t)entries) return -E_INVAL;
    if (!lengths || !n || n >= MAX_REG_BUFFERS) return -E_INVAL;
    size_t len[MAX_REG_BUFFERS];
    for (size_t i = 0; i < n; ++i) {
        if (!lengths[i] || lengths[i] > PTRDIFF_MAX - (PAGE - 1)) return -E_INVAL;
        len[i] = round_page(lengths[i]);
    }
    len[n] = round_page((size_t)entries * NAME_CELL);
    t_bytes_zero(r, sizeof(*r));
    t_bytes_zero(ops, cq * sizeof(*ops));
    r->k = *k;
    r->fd = -1;
    r->used = 1;
    r->admission = 1;
    r->holders = 1;
    // Attach every region, or none: a refusal detaches what was attached.
    for (size_t i = 0; i <= n; ++i) {
        int64_t va = k->attach(k->context, len[i]);
        if (va <= 0 || va % PAGE) {
            if (va > 0) k->detach(k->context, (uintptr_t)va, len[i]);
            release_holder(r);
            return va < 0 ? (int)va : -E_IO;
        }
        r->region_va[i] = (uintptr_t)va;
        r->region_len[i] = len[i];
        r->regions = i + 1;
    }
    r->buffers = n;
    struct t_owned_setup p = { .flags = T_LOOM_SETUP_PRIVATE_SERVICE | T_LOOM_SETUP_SERVICE_BUFFERS };
    int64_t fd = k->setup(k->context, entries, &p);
    if (fd < 0 || fd > INT32_MAX) {
        release_holder(r);
        return fd < 0 ? (int)fd : -E_IO;
    }
    r->fd = (int32_t)fd;
    if (!p.ring_va) {
        release_holder(r);
        return -E_IO;
    }
    r->ring_va = (uintptr_t)p.ring_va;
    r->ring_len = p.ring_size;
    // Validates every region, the header and the receipts; closes on refusal.
    ++r->holders;
    if (t_mapping_create((struct t_mapping_backend){ r, m_base, m_length, m_close, m_destroy }, p, &r->map)) {
        release_holder(r);
        return -E_IO;
    }
    r->ring_valid = 1;
    if (p.sq_entries != entries || p.cq_entries != cq) {
        t_mapping_destroy(&r->map);
        release_holder(r);
        return -E_IO;
    }
    struct t_loom_buf_reg table[MAX_REG_BUFFERS];
    for (size_t i = 0; i < r->regions; ++i) table[i] = (struct t_loom_buf_reg){ r->region_va[i], r->region_len[i] };
    int64_t rc = k->reg(k->context, r->fd, T_LOOM_REGISTER_BUFFERS, table, (uint32_t)r->regions);
    if (rc < 0) {
        t_mapping_destroy(&r->map);
        release_holder(r);
        return (int)rc;
    }
    ++r->holders;
    struct t_ring_backend b = { r, b_count, b_length, b_buffer, b_return, b_retire_pool, b_destroy_pool,
                                b_begin_retire, b_retired, b_close, b_destroy, b_register_pool, b_abort_scope };
    int created = t_fixed_create(b, &r->client, book, cq, &r->driver);
    if (created) {
        t_mapping_destroy(&r->map);
        release_holder(r);
        return errno_of(created);
    }
    unsigned char *base = (unsigned char *)r->ring_va;
    r->hdr = base + p.hdr_off;
    r->sq_array = (uint32_t *)(void *)(base + p.sq_array_off);
    r->sqes = (struct t_loom_sqe *)(void *)(base + p.sqe_off);
    r->sq_entries = entries;
    r->names = (unsigned char *)r->region_va[n];
    r->name_buffer = (uint32_t)n;
    r->ops = ops;
    r->op_count = cq;
    r->book = book;
    r->root = 1;
    r->state = R_OPEN;
    *out = r;
    return 0;
}
int t_loom_idle(const struct t_loom_ring *r) { return !r->used; }
uint64_t t_loom_now(struct t_loom_ring *r) { return r->k.now(r->k.context); }
int64_t t_loom_turn(struct t_loom_ring *r, int has_deadline, uint64_t deadline) {
    T_LOOM_CHECK(r->root);
    int64_t routed = service(r);
    if (routed || r->retired) return routed;
    int rc = wait_ring(r, has_deadline, deadline);
    if (rc) return rc;
    return service(r);
}
void t_loom_retire(struct t_loom_ring *r) {
    T_LOOM_CHECK(r->root);
    begin_retire(r);
}
int t_loom_retired(const struct t_loom_ring *r) { return (int)r->retired; }
void t_loom_close(struct t_loom_ring *r) {
    T_LOOM_CHECK(r->root);
    close_ring(r);
}
void t_loom_ring_drop(struct t_loom_ring *r) {
    T_LOOM_CHECK(r->root);
    close_ring(r);
    r->root = 0;
    ++r->handles;
    handle_gone(r);
}

// ---- Targets and pools.

int t_loom_target(struct t_loom_ring *r, int32_t registry_fd, const void *name, size_t len,
                  struct t_loom_target **out) {
    T_LOOM_CHECK(r->root && out);
    *out = NULL;
    int rc = admit(r);
    if (rc) return rc;
    if (!name || !len || len > 255) return -E_INVAL;
    struct t_loom_service_target rec;
    t_bytes_zero(&rec, sizeof(rec));
    rec.size = 288;
    rec.version = 1;
    rec.registry_fd = registry_fd;
    rec.name_len = (uint32_t)len;
    t_bytes_copy(rec.name, name, len);
    int64_t got = reg(r, T_LOOM_REGISTER_SERVICE_TARGET, &rec);
    if (got < 0) return (int)got;
    if (!minted(rec.result)) return -E_IO;
    struct t_loom_slot *e = track(r, rec.result, S_TARGET, ZERO_REF);
    e->handle = 1;
    e->target = (struct t_loom_target){ r, rec.result.slot };
    ++r->handles;
    *out = &e->target;
    return 0;
}
void t_loom_target_drop(struct t_loom_target *t) {
    struct t_loom_ring *r = t->ring;
    struct t_loom_slot *e = &r->slots[t->slot];
    T_LOOM_CHECK(e->kind == S_TARGET && e->handle);
    e->handle = 0;
    e->state = S_REAP;
    handle_gone(r);
}
int t_loom_pool(struct t_loom_ring *r, const struct t_loom_service_pool_member *ranges, size_t n,
                struct t_loom_pool **out) {
    T_LOOM_CHECK(r->root && out);
    *out = NULL;
    int rc = admit(r);
    if (rc) return rc;
    struct t_loom_pool_cell *p = NULL;
    for (size_t i = 0; i < 64 && !p; ++i)
        if (!r->pools[i].refs) p = &r->pools[i];
    // A pool holds at least one of the bank's 64 members, so a cell is free
    // whenever the core can register one.
    if (!p) return -E_NOSPC;
    struct t_ring_pool *core = NULL;
    int64_t native = 0, got = t_fixed_register_pool(r->driver, ranges, n, &core, &native);
    if (got == T_RING_UNCERTAIN) return native < 0 && native > -1000 ? (int)native : -E_IO;
    if (got) return errno_of(got);
    *p = (struct t_loom_pool_cell){ .core = core, .ref = t_ring_pool_identity(core), .refs = 1, .members = n,
                                    .handle = 1 };
    p->pool = (struct t_loom_pool){ r, (uint32_t)(p - r->pools) };
    ++r->handles;
    *out = &p->pool;
    return 0;
}
void t_loom_pool_drop(struct t_loom_pool *h) {
    struct t_loom_ring *r = h->ring;
    struct t_loom_pool_cell *p = &r->pools[h->cell];
    T_LOOM_CHECK(p->handle);
    p->handle = 0;
    pool_unref(r, h->cell);
    handle_gone(r);
}

// ---- Submissions.

// A SCOPE or FID reservation. Exhaustion first retries the slots awaiting REAP.
static int reserve(struct t_loom_ring *r, uint32_t kind, struct t_loom_service_ref scope,
                   struct t_loom_service_ref *out) {
    int rc = reserve_once(r, kind, scope, out);
    if (rc != -E_NOSPC) return rc;
    reap_pass(r);
    return reserve_once(r, kind, scope, out);
}
static struct t_loom_op *new_op(struct t_loom_ring *r, unsigned kind, uint32_t scope) {
    struct t_loom_op *op = claim_op(r);
    if (op) *op = (struct t_loom_op){ .ring = r, .kind = kind, .scope = scope };
    return op;
}
static void op_issued(struct t_loom_ring *r, struct t_loom_op *op, uint64_t cookie) {
    op->handle = 1;
    op->cookie = cookie;
    ++r->handles;
}

int t_loom_connect(struct t_loom_target *t, int has_deadline, uint64_t deadline, struct t_loom_op **out) {
    struct t_loom_ring *r = t->ring;
    struct t_loom_slot *target = &r->slots[t->slot];
    T_LOOM_CHECK(target->kind == S_TARGET && target->handle && out);
    *out = NULL;
    int rc = admit(r);
    if (rc) return rc;
    struct t_loom_op *op = new_op(r, OP_CONNECT, 0);
    if (!op) return -E_NOSPC;
    struct t_loom_service_ref scope;
    if ((rc = reserve(r, T_LOOM_SERVICE_SCOPE, ZERO_REF, &scope))) {
        free_op(op);
        return rc;
    }
    track(r, scope, S_SCOPE, scope);
    struct t_loom_sqe sqe = make_sqe(T_LOOM_OP_SERVICE_CONNECT, target->ref, has_deadline, deadline);
    sqe.offset = scope.slot;
    sqe._resv1[2] = scope.incarnation;
    struct t_request_spec spec = { .kind = T_REQ_SLOT, .slot = scope.slot };
    uint64_t cookie = 0;
    if ((rc = t_fixed_book_reserve(r->driver, scope, spec, &op->book))) {
        rc = errno_of(rc);
    } else if ((rc = submit(r, &sqe, NULL, 0, &op->book, NULL, &cookie))) {
        t_fixed_book_drop(&op->book);
    }
    if (rc) {
        set_state(r, scope, S_REAP);
        free_op(op);
        return rc;
    }
    op->scope = scope.slot;
    op->dest = scope;
    op->has_dest = 1;
    op_issued(r, op, cookie);
    *out = op;
    return 0;
}
static int walk(struct t_loom_ring *r, uint32_t scope_slot, struct t_loom_service_ref source, const void *name,
                size_t len, int has_deadline, uint64_t deadline, struct t_loom_op **out) {
    T_LOOM_CHECK(out);
    *out = NULL;
    int rc = admit(r);
    if (rc) return rc;
    if (len > 255 || (len && !name)) return -E_INVAL;
    struct t_loom_op *op = new_op(r, OP_WALK, scope_slot);
    if (!op) return -E_NOSPC;
    struct t_loom_service_ref scope = r->slots[scope_slot].ref, fid;
    if ((rc = reserve(r, T_LOOM_SERVICE_FID, scope, &fid))) {
        free_op(op);
        return rc;
    }
    track(r, fid, S_FID, scope);
    struct t_loom_sqe sqe = make_sqe(T_LOOM_OP_WALK, source, has_deadline, deadline);
    sqe.offset = fid.slot;
    sqe._resv1[2] = fid.incarnation;
    struct t_request_spec spec = { .kind = T_REQ_SLOT, .slot = fid.slot };
    uint64_t cookie = 0;
    if ((rc = t_fixed_book_reserve(r->driver, scope, spec, &op->book))) {
        rc = errno_of(rc);
    } else if ((rc = submit(r, &sqe, name, len, &op->book, NULL, &cookie))) {
        t_fixed_book_drop(&op->book);
    }
    if (rc) {
        set_state(r, fid, S_REAP);
        free_op(op);
        return rc;
    }
    op->dest = fid;
    op->has_dest = 1;
    scope_ref(r, scope_slot);
    op_issued(r, op, cookie);
    *out = op;
    return 0;
}
int t_loom_walk(struct t_loom_scope *s, const void *name, size_t len, int has_deadline, uint64_t deadline,
                struct t_loom_op **out) {
    struct t_loom_slot *e = &s->ring->slots[s->slot];
    T_LOOM_CHECK(e->kind == S_SCOPE && e->handle);
    return walk(s->ring, s->slot, e->ref, name, len, has_deadline, deadline, out);
}
int t_loom_walk_fid(struct t_loom_fid *f, const void *name, size_t len, int has_deadline, uint64_t deadline,
                    struct t_loom_op **out) {
    struct t_loom_slot *e = &f->ring->slots[f->slot];
    T_LOOM_CHECK(e->kind == S_FID && e->handle);
    return walk(f->ring, e->scope.slot, e->ref, name, len, has_deadline, deadline, out);
}
static struct t_loom_slot *fid_of(struct t_loom_fid *f) {
    struct t_loom_slot *e = &f->ring->slots[f->slot];
    T_LOOM_CHECK(e->kind == S_FID && e->handle);
    return e;
}
int t_loom_open(struct t_loom_fid *f, uint32_t mode, int has_deadline, uint64_t deadline, struct t_loom_op **out) {
    struct t_loom_ring *r = f->ring;
    struct t_loom_slot *e = fid_of(f);
    T_LOOM_CHECK(out);
    *out = NULL;
    int rc = admit(r);
    if (rc) return rc;
    struct t_loom_op *op = new_op(r, OP_OPEN, e->scope.slot);
    if (!op) return -E_NOSPC;
    struct t_loom_sqe sqe = make_sqe(T_LOOM_OP_LOPEN, e->ref, has_deadline, deadline);
    sqe.offset = mode;
    struct t_request_spec spec = { .kind = T_REQ_CONTROL };
    uint64_t cookie = 0;
    if ((rc = t_fixed_book_reserve(r->driver, e->scope, spec, &op->book))) {
        free_op(op);
        return errno_of(rc);
    }
    if ((rc = submit(r, &sqe, NULL, 0, &op->book, NULL, &cookie))) {
        t_fixed_book_drop(&op->book);
        free_op(op);
        return rc;
    }
    scope_ref(r, e->scope.slot);
    op_issued(r, op, cookie);
    *out = op;
    return 0;
}
static int fixed_io(struct t_loom_fid *f, unsigned write, struct t_loom_service_pool_member range, uint64_t offset,
                    const void *bytes, size_t len, int has_deadline, uint64_t deadline, struct t_loom_op **out) {
    struct t_loom_ring *r = f->ring;
    struct t_loom_slot *e = fid_of(f);
    T_LOOM_CHECK(out);
    *out = NULL;
    int rc = admit(r);
    if (rc) return rc;
    struct t_loom_op *op = new_op(r, write ? OP_WRITE : OP_READ, e->scope.slot);
    if (!op) return -E_NOSPC;
    rc = write ? t_fixed_write(r->driver, e->scope, range, bytes, len, &op->fixed)
               : t_fixed_read(r->driver, e->scope, range, &op->fixed);
    if (rc) {
        free_op(op);
        return errno_of(rc);
    }
    struct t_loom_sqe sqe = make_sqe(write ? T_LOOM_OP_WRITE : T_LOOM_OP_READ, e->ref, has_deadline, deadline);
    sqe.offset = offset;
    sqe.len = (uint32_t)range.length;
    sqe.buf_idx_or_off = range.buffer_index;
    sqe._resv1[0] = range.offset;
    uint64_t cookie = 0;
    if ((rc = submit(r, &sqe, NULL, 0, NULL, op->fixed, &cookie))) {
        t_fixed_request_drop(op->fixed);
        free_op(op);
        return rc;
    }
    scope_ref(r, e->scope.slot);
    op_issued(r, op, cookie);
    *out = op;
    return 0;
}
int t_loom_read(struct t_loom_fid *f, struct t_loom_service_pool_member range, uint64_t offset, int has_deadline,
                uint64_t deadline, struct t_loom_op **out) {
    return fixed_io(f, 0, range, offset, NULL, 0, has_deadline, deadline, out);
}
int t_loom_write(struct t_loom_fid *f, struct t_loom_service_pool_member range, uint64_t offset, const void *bytes,
                 size_t len, int has_deadline, uint64_t deadline, struct t_loom_op **out) {
    return fixed_io(f, 1, range, offset, bytes, len, has_deadline, deadline, out);
}
int t_loom_stream(struct t_loom_fid *f, struct t_loom_pool *pool, uint64_t offset, uint32_t limit, int multishot,
                  int has_deadline, uint64_t deadline, struct t_loom_op **out) {
    struct t_loom_ring *r = f->ring;
    struct t_loom_slot *e = fid_of(f);
    T_LOOM_CHECK(out);
    *out = NULL;
    int rc = admit(r);
    if (rc) return rc;
    if (pool->ring != r) return -E_INVAL;
    struct t_loom_pool_cell *p = &r->pools[pool->cell];
    T_LOOM_CHECK(p->handle);
    struct t_loom_op *op = new_op(r, OP_STREAM, e->scope.slot);
    if (!op) return -E_NOSPC;
    struct t_request_spec spec = { .kind = T_REQ_POOLED_READ, .limit = limit, .multi = !!multishot, .pool = p->ref };
    if ((rc = t_fixed_book_reserve(r->driver, e->scope, spec, &op->book))) {
        free_op(op);
        return errno_of(rc);
    }
    struct t_loom_sqe sqe = make_sqe(T_LOOM_OP_READ, e->ref, has_deadline, deadline);
    sqe.flags = (uint8_t)(T_LOOM_SQE_BUFFER_SELECT | (multishot ? T_LOOM_SQE_MULTISHOT : 0));
    sqe.offset = offset;
    sqe.len = limit;
    sqe.buf_idx_or_off = p->ref.slot;
    sqe._resv1[2] = p->ref.incarnation;
    uint64_t cookie = 0;
    if ((rc = submit(r, &sqe, NULL, 0, &op->book, NULL, &cookie))) {
        t_fixed_book_drop(&op->book);
        free_op(op);
        return rc;
    }
    op->pool = pool->cell;
    op->route = 1;
    ++p->refs;
    scope_ref(r, e->scope.slot);
    op_issued(r, op, cookie);
    *out = op;
    return 0;
}

// ---- Completions.

// 1 took the terminal, 0 pending, or the ring's errno.
static int take_book(struct t_loom_op *op, struct t_owned_cqe *c) {
    int rc = t_fixed_book_take(&op->book, c);
    return rc == 1 || rc == 0 ? rc : fault(op->ring, rc);
}
// An operation's handle is consumed: its scope reference goes, then the slot.
static void consumed(struct t_loom_ring *r, struct t_loom_op *op, int holds_scope) {
    uint32_t scope = op->scope;
    free_op(op);
    if (holds_scope) scope_unref(r, scope);
    handle_gone(r);
}
int t_loom_connect_done(struct t_loom_op *op, int32_t *result, struct t_loom_scope **scope) {
    T_LOOM_CHECK(op->handle && result && scope);
    if (op->kind != OP_CONNECT) return -E_INVAL;
    struct t_loom_ring *r = op->ring;
    struct t_owned_cqe c;
    int rc = take_book(op, &c);
    if (rc != 1) return rc;
    *result = c.result;
    *scope = NULL;
    struct t_loom_slot *e = entry(r, op->dest);
    T_LOOM_CHECK(e);
    if (c.result >= 0) {
        // The handle passes from the operation to the scope.
        e->refs = 1;
        e->handle = 1;
        e->scope_handle = (struct t_loom_scope){ r, op->dest.slot };
        *scope = &e->scope_handle;
        free_op(op);
        return 1;
    }
    abort_scope(r, op->dest);
    e->state = S_REAP;
    consumed(r, op, 0);
    return 1;
}
int t_loom_walk_done(struct t_loom_op *op, int32_t *result, struct t_loom_fid **fid) {
    T_LOOM_CHECK(op->handle && result && fid);
    if (op->kind != OP_WALK) return -E_INVAL;
    struct t_loom_ring *r = op->ring;
    struct t_owned_cqe c;
    int rc = take_book(op, &c);
    if (rc != 1) return rc;
    *result = c.result;
    *fid = NULL;
    struct t_loom_slot *e = entry(r, op->dest);
    T_LOOM_CHECK(e);
    if (c.result >= 0) {
        // The handle and its scope reference pass from the operation to the fid.
        e->handle = 1;
        e->fid = (struct t_loom_fid){ r, op->dest.slot };
        *fid = &e->fid;
        free_op(op);
        return 1;
    }
    e->state = S_REAP;
    consumed(r, op, 1);
    return 1;
}
int t_loom_open_done(struct t_loom_op *op, int32_t *result) {
    T_LOOM_CHECK(op->handle && result);
    if (op->kind != OP_OPEN) return -E_INVAL;
    struct t_owned_cqe c;
    int rc = take_book(op, &c);
    if (rc != 1) return rc;
    *result = c.result;
    consumed(op->ring, op, 1);
    return 1;
}
int t_loom_read_done(struct t_loom_op *op, int32_t *result, struct t_fixed_read_result **done) {
    T_LOOM_CHECK(op->handle && result && done);
    if (op->kind != OP_READ) return -E_INVAL;
    struct t_loom_ring *r = op->ring;
    struct t_fixed_read_result *got = NULL;
    int rc = t_fixed_read_complete(op->fixed, &got);
    if (rc == 0) return 0;
    if (rc < 0) return fault(r, rc);
    *result = t_fixed_read_status(got);
    *done = NULL;
    if (*result < 0)
        t_fixed_read_drop(got);
    else
        *done = got;
    consumed(r, op, 1);
    return 1;
}
int t_loom_write_done(struct t_loom_op *op, int32_t *result) {
    T_LOOM_CHECK(op->handle && result);
    if (op->kind != OP_WRITE) return -E_INVAL;
    struct t_owned_cqe c;
    int rc = t_fixed_write_complete(op->fixed, &c);
    if (rc == 0) return 0;
    if (rc < 0) return fault(op->ring, rc);
    *result = c.result;
    consumed(op->ring, op, 1);
    return 1;
}
int t_loom_stream_next(struct t_loom_op *op, struct t_ring_payload **payload, int *end, int32_t *result) {
    T_LOOM_CHECK(op->handle && payload && end && result);
    if (op->kind != OP_STREAM || op->ended) return -E_INVAL;
    *payload = queue_pop(op->ring, op);
    *end = 0;
    if (*payload) return 1;
    struct t_owned_cqe c;
    int rc = take_book(op, &c);
    if (rc == 0) return 0;
    op->ended = 1;
    if (rc < 0) return rc;
    *end = 1;
    *result = c.result;
    return 1;
}
void t_loom_op_drop(struct t_loom_op *op) {
    struct t_loom_ring *r = op->ring;
    T_LOOM_CHECK(op->handle);
    switch (op->kind) {
    case OP_CONNECT:
        abort_scope(r, op->dest);
        set_state(r, op->dest, S_REAP);
        if (op->book.driver) t_fixed_book_drop(&op->book);
        consumed(r, op, 0);
        return;
    case OP_WALK: {
        // Only a WALK that succeeded unobserved left a live fid to clunk; a
        // failed one left the reservation, which REAP frees.
        struct t_owned_cqe c;
        int taken = t_fixed_book_take(&op->book, &c);
        set_state(r, op->dest, taken == 1 && c.result < 0 ? S_REAP : S_CLUNK);
        uint32_t scope = op->scope;
        if (taken != 1) t_fixed_book_drop(&op->book);
        free_op(op);
        scope_unref(r, scope);
        handle_gone(r);
        return;
    }
    case OP_OPEN:
        t_fixed_book_drop(&op->book);
        consumed(r, op, 1);
        return;
    case OP_READ:
    case OP_WRITE:
        t_fixed_request_drop(op->fixed);
        consumed(r, op, 1);
        return;
    case OP_STREAM: {
        struct t_ring_payload *held;
        op->handle = 0;
        while ((held = queue_pop(r, op))) t_ring_abandon(held);
        if (op->book.driver) t_fixed_book_drop(&op->book);
        uint32_t scope = op->scope;
        if (!op->route) free_op(op);
        scope_unref(r, scope);
        handle_gone(r);
        return;
    }
    }
    t_loom_client_fatal(__func__, __LINE__);
}

// ---- Results, payloads, scopes and fids.

const unsigned char *t_loom_read_bytes(const struct t_fixed_read_result *done, size_t *len) {
    const unsigned char *bytes = NULL;
    T_LOOM_CHECK(len && t_fixed_read_bytes(done, &bytes, len) == 0);
    return bytes;
}
void t_loom_read_drop(struct t_fixed_read_result *done) { t_fixed_read_drop(done); }
const unsigned char *t_loom_payload_bytes(const struct t_ring_payload *payload, size_t *len) {
    return t_ring_bytes(payload, len);
}
int t_loom_payload_return(struct t_ring_payload *payload) {
    int64_t rc = t_ring_return(payload);
    if (!rc) return 0;
    return rc < 0 && rc > -1000 ? (int)rc : -E_IO;
}
void t_loom_payload_drop(struct t_ring_payload *payload) { t_ring_abandon(payload); }
void t_loom_scope_abort(struct t_loom_scope *s) {
    struct t_loom_slot *e = &s->ring->slots[s->slot];
    T_LOOM_CHECK(e->kind == S_SCOPE && e->handle);
    abort_scope(s->ring, e->ref);
}
void t_loom_scope_drop(struct t_loom_scope *s) {
    struct t_loom_ring *r = s->ring;
    uint32_t slot = s->slot;
    struct t_loom_slot *e = &r->slots[slot];
    T_LOOM_CHECK(e->kind == S_SCOPE && e->handle);
    e->handle = 0;
    scope_unref(r, slot);
    handle_gone(r);
}
void t_loom_fid_drop(struct t_loom_fid *f) {
    struct t_loom_ring *r = f->ring;
    struct t_loom_slot *e = fid_of(f);
    e->handle = 0;
    e->state = S_CLUNK;
    scope_unref(r, e->scope.slot);
    handle_gone(r);
}
