/* The native C adapter against the simulated kernel (sim_kernel.c), test for
 * test the C twin of loom-client's tests/native.rs. Every test also ends with
 * the ring's storage idle and every region detached. */
#include "sim_kernel.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <thyla/poll.h>
#include <unistd.h>

#define MS 1000000ull
typedef T_LOOM_RING_STORAGE(1) ring1;
typedef T_LOOM_RING_STORAGE(2) ring2;
typedef T_LOOM_RING_STORAGE(4) ring4;
typedef T_LOOM_RING_STORAGE(8) ring8;

enum { K_CONNECT = 1, K_WALK, K_OPEN, K_READ, K_WRITE };
struct res {
    int32_t result;
    struct t_loom_scope *scope;
    struct t_loom_fid *fid;
    struct t_fixed_read_result *read;
};
struct fx {
    struct sim *sim;
    struct t_loom_kernel k;
    void *storage;
    struct t_loom_ring *base, *r;
};

static struct sim *new_sim(void) {
    struct sim *s = calloc(1, sizeof(*s));
    assert(s);
    sim_init(s);
    return s;
}
static int setup_on(struct fx *f, uint32_t entries, const size_t *lengths, size_t n) {
    f->k = sim_kernel(f->sim);
    switch (entries) {
#define CASE(e)                                                                    \
    case e: {                                                                      \
        ring##e *st = calloc(1, sizeof(*st));                                      \
        assert(st);                                                                \
        f->storage = st;                                                           \
        f->base = &st->base;                                                       \
        return T_LOOM_RING_SETUP(st, &f->k, entries, lengths, n, &f->r);          \
    }
        CASE(1) CASE(2) CASE(4) CASE(8)
#undef CASE
    }
    assert(0);
    return 0;
}
static struct fx ring_of(uint32_t entries, const size_t *lengths, size_t n) {
    struct fx f = { .sim = new_sim() };
    assert(setup_on(&f, entries, lengths, n) == 0);
    return f;
}
/* The ring handle is gone: the storage must be idle and every region detached. */
static void finish(struct fx *f) {
    assert(t_loom_idle(f->base));
    assert(f->sim->nregions == 0);
    free(f->storage);
    free(f->sim);
}
static struct t_loom_service_pool_member range(uint32_t index, uint64_t offset, uint64_t length) {
    return (struct t_loom_service_pool_member){ index, 0, offset, length };
}
static int complete(unsigned kind, struct t_loom_op *op, struct res *o) {
    switch (kind) {
    case K_CONNECT: return t_loom_connect_done(op, &o->result, &o->scope);
    case K_WALK: return t_loom_walk_done(op, &o->result, &o->fid);
    case K_OPEN: return t_loom_open_done(op, &o->result);
    case K_READ: return t_loom_read_done(op, &o->result, &o->read);
    default: return t_loom_write_done(op, &o->result);
    }
}
/* The C form of Ring::block_on: turn until the op answers; a ring error is
 * returned, after asking the op once more. */
static int wait_op(struct t_loom_ring *r, unsigned kind, struct t_loom_op *op, struct res *o) {
    int rc;
    while (!(rc = complete(kind, op, o))) {
        int64_t t = t_loom_turn(r, 0, 0);
        if (t < 0) {
            rc = complete(kind, op, o);
            return rc ? rc : (int)t;
        }
    }
    return rc;
}
/* run(): the op finishes (1) with the ring up; its result is returned. */
static int32_t run(struct t_loom_ring *r, unsigned kind, struct t_loom_op *op, struct res *o) {
    memset(o, 0, sizeof(*o));
    assert(wait_op(r, kind, op, o) == 1);
    return o->result;
}
static struct t_loom_target *target_ok(struct t_loom_ring *r, const char *name) {
    struct t_loom_target *t = NULL;
    assert(t_loom_target(r, 5, name, strlen(name), &t) == 0 && t);
    return t;
}
static struct t_loom_scope *connect_ok(struct t_loom_ring *r, struct t_loom_target *t) {
    struct t_loom_op *op;
    struct res o;
    assert(t_loom_connect(t, 0, 0, &op) == 0);
    assert(run(r, K_CONNECT, op, &o) >= 0 && o.scope);
    return o.scope;
}
static struct t_loom_op *walk_op(struct t_loom_scope *s, const char *name) {
    struct t_loom_op *op = NULL;
    assert(t_loom_walk(s, name, strlen(name), 0, 0, &op) == 0 && op);
    return op;
}
static struct t_loom_fid *walk_ok(struct t_loom_ring *r, struct t_loom_scope *s, const char *name) {
    struct res o;
    assert(run(r, K_WALK, walk_op(s, name), &o) >= 0 && o.fid);
    return o.fid;
}
static int32_t open_rc(struct t_loom_ring *r, struct t_loom_fid *f, uint32_t mode) {
    struct t_loom_op *op;
    struct res o;
    assert(t_loom_open(f, mode, 0, 0, &op) == 0);
    return run(r, K_OPEN, op, &o);
}
static struct t_loom_op *read_op(struct t_loom_fid *f, struct t_loom_service_pool_member m, int has_deadline,
                                 uint64_t deadline) {
    struct t_loom_op *op = NULL;
    assert(t_loom_read(f, m, 0, has_deadline, deadline, &op) == 0 && op);
    return op;
}
/* A read's bytes, compared and dropped. */
static void read_is(struct t_loom_ring *r, struct t_loom_fid *f, struct t_loom_service_pool_member m,
                    const char *want) {
    struct res o;
    assert(run(r, K_READ, read_op(f, m, 0, 0), &o) == (int32_t)strlen(want) && o.read);
    size_t len = 0;
    const unsigned char *bytes = t_loom_read_bytes(o.read, &len);
    assert(len == strlen(want) && !memcmp(bytes, want, len));
    t_loom_read_drop(o.read);
}
static struct t_loom_op *stream_op(struct t_loom_fid *f, struct t_loom_pool *p, int multishot) {
    struct t_loom_op *op = NULL;
    assert(t_loom_stream(f, p, 0, 64, multishot, 0, 0, &op) == 0 && op);
    return op;
}
/* The stream's next shot: 1 with a payload, 0 with its end in *end_rc. */
static int next_shot(struct t_loom_ring *r, struct t_loom_op *op, struct t_ring_payload **p, int32_t *end_rc) {
    for (;;) {
        int end = 0;
        int rc = t_loom_stream_next(op, p, &end, end_rc);
        if (rc == 1) return !end;
        assert(rc == 0);
        assert(t_loom_turn(r, 0, 0) >= 0);
    }
}
static struct t_loom_pool *pool_ok(struct t_loom_ring *r, const struct t_loom_service_pool_member *m, size_t n) {
    struct t_loom_pool *p = NULL;
    assert(t_loom_pool(r, m, n, &p) == 0 && p);
    return p;
}
static void file(struct sim *s, const char *name, const char *bytes) { sim_file(s, name, bytes, strlen(bytes)); }
/* A connected scope, a walked and opened fid of `name`. */
static void opened(struct fx *f, const char *name, struct t_loom_target **t, struct t_loom_scope **s,
                   struct t_loom_fid **fid) {
    *t = target_ok(f->r, "clip");
    *s = connect_ok(f->r, *t);
    *fid = walk_ok(f->r, *s, name);
    assert(open_rc(f->r, *fid, 0) == 0);
}
static void drop_all(struct fx *f, struct t_loom_fid *fid, struct t_loom_scope *s, struct t_loom_target *t) {
    if (fid) t_loom_fid_drop(fid);
    if (s) t_loom_scope_drop(s);
    if (t) t_loom_target_drop(t);
    t_loom_ring_drop(f->r);
}
static void turn_until(struct t_loom_ring *r, uint64_t deadline) { assert(t_loom_turn(r, 1, deadline) >= 0); }

static void setup_registers_every_buffer_and_drop_detaches_every_region(void) {
    size_t b[] = { 4096, 5000 };
    struct fx f = ring_of(8, b, 2);
    /* Two caller buffers (the second rounded up to two pages), the name arena,
     * and the ring mapping itself. */
    assert(f.sim->nregions == 4);
    t_loom_ring_drop(f.r);
    assert(f.sim->nclosed == 1 && f.sim->closed_fds[0] == 7);
    finish(&f);
}
static void a_refused_attach_detaches_what_was_attached(void) {
    struct fx f = { .sim = new_sim() };
    f.sim->attach_fail_at = 2;
    size_t b[] = { 4096, 4096 };
    assert(setup_on(&f, 8, b, 2) == SIM_ENOMEM && !f.r);
    finish(&f);
}
static void connect_walk_open_write_then_read_round_trips(void) {
    size_t b[] = { 4096, 4096 };
    struct fx f = ring_of(8, b, 2);
    file(f.sim, "text", "hello");
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    struct t_loom_fid *fid = walk_ok(f.r, s, "text");
    assert(open_rc(f.r, fid, 2) == 0);
    struct t_loom_op *op;
    struct res o;
    assert(t_loom_write(fid, range(0, 0, 5), 0, "HELLO", 5, 0, 0, &op) == 0);
    assert(run(f.r, K_WRITE, op, &o) == 5);
    read_is(f.r, fid, range(1, 16, 5), "HELLO");
    const struct sim_file *sf = sim_find_file(f.sim, "text");
    assert(sf->len == 5 && !memcmp(sf->data, "HELLO", 5));
    drop_all(&f, fid, s, t);
    finish(&f);
}
static void a_failed_connect_reaps_its_scope(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    struct t_loom_target *t = target_ok(f.r, "absent");
    struct t_loom_op *op;
    struct res o;
    assert(t_loom_connect(t, 0, 0, &op) == 0);
    assert(run(f.r, K_CONNECT, op, &o) == SIM_ENOENT && !o.scope);
    turn_until(f.r, t_loom_now(f.r));
    assert(sim_live_slots(f.sim) == 1); /* only the target remains */
    t_loom_target_drop(t);
    turn_until(f.r, t_loom_now(f.r));
    assert(sim_live_slots(f.sim) == 0);
    t_loom_ring_drop(f.r);
    finish(&f);
}
static void walks_read_their_files(struct fx *f, struct t_loom_scope *s, struct t_loom_op *wa, struct t_loom_op *wb) {
    struct res o;
    assert(run(f->r, K_WALK, wa, &o) >= 0);
    struct t_loom_fid *a = o.fid;
    assert(run(f->r, K_WALK, wb, &o) >= 0);
    struct t_loom_fid *b = o.fid;
    assert(open_rc(f->r, a, 0) == 0 && open_rc(f->r, b, 0) == 0);
    read_is(f->r, a, range(0, 0, 16), "first");
    read_is(f->r, b, range(0, 0, 16), "second");
    t_loom_fid_drop(a);
    t_loom_fid_drop(b);
    (void)s;
}
static void each_walk_name_is_read_inside_the_enter_that_consumes_it(void) {
    /* One SQ slot, so both walks use the same name cell. The kernel copies the
     * component before ENTER returns, which is what lets the cell be reused. */
    size_t b[] = { 4096 };
    struct fx f = ring_of(1, b, 1);
    file(f.sim, "a", "first");
    file(f.sim, "b", "second");
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    struct t_loom_op *wa = walk_op(s, "a"), *wb = walk_op(s, "b");
    walks_read_their_files(&f, s, wa, wb);
    drop_all(&f, NULL, s, t);
    finish(&f);
}
static void queued_walks_keep_their_own_name_cells(void) {
    /* Neither SQE is consumed until both are queued, so each name must still be
     * in its own slot's cell when the kernel reads it. */
    size_t b[] = { 4096 };
    struct fx f = ring_of(4, b, 1);
    file(f.sim, "a", "first");
    file(f.sim, "b", "second");
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    f.sim->has_enter_limit = 1;
    f.sim->enter_limit = 0;
    struct t_loom_op *wa = walk_op(s, "a"), *wb = walk_op(s, "b");
    f.sim->has_enter_limit = 0;
    walks_read_their_files(&f, s, wa, wb);
    drop_all(&f, NULL, s, t);
    finish(&f);
}
static void a_multishot_stream_runs_on_out_of_order_returns(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "events", "");
    const char *shots[] = { "one", "two", "three", "four", "five", "six" };
    for (size_t i = 0; i < 6; ++i) sim_shot(f.sim, "events", shots[i], strlen(shots[i]));
    struct t_loom_service_pool_member m[] = { range(0, 0, 64), range(0, 64, 64), range(0, 128, 64) };
    struct t_loom_pool *p = pool_ok(f.r, m, 3);
    struct t_loom_target *t;
    struct t_loom_scope *s;
    struct t_loom_fid *fid;
    opened(&f, "events", &t, &s, &fid);
    struct t_loom_op *stream = stream_op(fid, p, 1);
    struct t_ring_payload *held[3], *pl;
    size_t nheld = 0, seen = 0;
    int32_t end = -1;
    while (next_shot(f.r, stream, &pl, &end)) {
        size_t len;
        const unsigned char *bytes = t_loom_payload_bytes(pl, &len);
        assert(len == strlen(shots[seen]) && !memcmp(bytes, shots[seen], len));
        ++seen;
        held[nheld++] = pl;
        /* Three members: hold them all, then give back the newest first, so the
         * server's next member is not the oldest one. */
        if (nheld == 3) {
            assert(t_loom_payload_return(held[2]) == 0);
            t_loom_payload_drop(held[0]);
            held[0] = held[1];
            nheld = 1;
        }
    }
    assert(end == 0 && seen == 6);
    for (size_t i = 0; i < nheld; ++i) t_loom_payload_drop(held[i]);
    assert(f.sim->returns == 6);
    t_loom_op_drop(stream);
    t_loom_pool_drop(p);
    drop_all(&f, fid, s, t);
    finish(&f);
}
static void a_deadline_ends_the_operation_and_aborts_its_scope(void) {
    size_t b[] = { 4096, 4096 };
    struct fx f = ring_of(8, b, 2);
    file(f.sim, "slow", "never");
    struct t_loom_target *t;
    struct t_loom_scope *s;
    struct t_loom_fid *fid;
    opened(&f, "slow", &t, &s, &fid);
    f.sim->hold = 1;
    uint64_t deadline = t_loom_now(f.r) + 5 * MS;
    struct t_loom_op *late = read_op(fid, range(0, 0, 8), 1, deadline);
    struct t_loom_op *sibling = read_op(fid, range(1, 0, 8), 0, 0);
    struct res o;
    assert(run(f.r, K_READ, late, &o) == SIM_ETIMEDOUT && !o.read);
    assert(run(f.r, K_READ, sibling, &o) == SIM_ECANCELED && !o.read);
    assert(t_loom_now(f.r) >= deadline);
    drop_all(&f, fid, s, t);
    finish(&f);
}
static void poll_rounds_the_time_left_up_to_a_whole_millisecond(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    uint64_t deadline = t_loom_now(f.r) + 1500000;
    assert(t_loom_turn(f.r, 1, deadline) == 0);
    assert(f.sim->npolls == 1 && f.sim->polls[0] == 2);
    assert(t_loom_now(f.r) >= deadline);
    t_loom_ring_drop(f.r);
    finish(&f);
}
static void dropping_an_operation_aborts_its_scope_and_settles_its_entry(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "f", "data");
    struct t_loom_target *t;
    struct t_loom_scope *s;
    struct t_loom_fid *fid;
    opened(&f, "f", &t, &s, &fid);
    f.sim->hold = 1;
    struct t_loom_op *read = read_op(fid, range(0, 0, 4), 0, 0);
    size_t before = f.sim->naborts;
    t_loom_op_drop(read);
    assert(f.sim->naborts == before + 1); /* one ABORT for the drop */
    /* The cancelled READ's terminal arrives and is discarded; nothing waits. */
    turn_until(f.r, t_loom_now(f.r) + MS);
    assert(f.sim->nops == 0);
    /* The scope is gone: new work on it ends -ECANCELED. */
    f.sim->hold = 0;
    struct res o;
    assert(run(f.r, K_READ, read_op(fid, range(0, 0, 4), 0, 0), &o) == SIM_ECANCELED);
    drop_all(&f, fid, s, t);
    finish(&f);
}
static void a_full_cq_defers_completions_and_loses_none(void) {
    /* Two SQ slots, four CQ slots, eight shots: the kernel defers what does not
     * fit, and ENTER after each drain lets it post the rest. */
    size_t b[] = { 4096 };
    struct fx f = ring_of(2, b, 1);
    file(f.sim, "burst", "");
    char shots[8][4];
    for (int i = 0; i < 8; ++i) {
        memset(shots[i], 'a' + i, 4);
        sim_shot(f.sim, "burst", shots[i], 4);
    }
    struct t_loom_service_pool_member m[8];
    for (int i = 0; i < 8; ++i) m[i] = range(0, (uint64_t)i * 64, 64);
    struct t_loom_pool *p = pool_ok(f.r, m, 8);
    struct t_loom_target *t;
    struct t_loom_scope *s;
    struct t_loom_fid *fid;
    opened(&f, "burst", &t, &s, &fid);
    struct t_loom_op *stream = stream_op(fid, p, 1);
    struct t_ring_payload *held[8], *pl;
    size_t got = 0;
    int32_t end = -1;
    while (next_shot(f.r, stream, &pl, &end)) {
        size_t len;
        const unsigned char *bytes = t_loom_payload_bytes(pl, &len);
        assert(got < 8 && len == 4 && !memcmp(bytes, shots[got], 4));
        held[got++] = pl;
    }
    assert(end == 0 && got == 8);
    for (size_t i = 0; i < got; ++i) t_loom_payload_drop(held[i]);
    t_loom_op_drop(stream);
    t_loom_pool_drop(p);
    drop_all(&f, fid, s, t);
    finish(&f);
}
static void a_short_enter_leaves_the_rest_queued_for_a_later_turn(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(4, b, 1);
    file(f.sim, "f", "x");
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    f.sim->has_enter_limit = 1;
    f.sim->enter_limit = 0;
    struct t_loom_op *walks[3];
    for (int i = 0; i < 3; ++i) walks[i] = walk_op(s, "f");
    assert(sim_hdr_word(f.sim, 1) - sim_hdr_word(f.sim, 0) == 3); /* all three queued */
    f.sim->enter_limit = 1;
    for (int i = 0; i < 3; ++i) {
        struct res o;
        assert(run(f.r, K_WALK, walks[i], &o) >= 0);
        t_loom_fid_drop(o.fid);
    }
    assert(sim_hdr_word(f.sim, 1) == sim_hdr_word(f.sim, 0)); /* every SQE consumed */
    drop_all(&f, NULL, s, t);
    finish(&f);
}
static void a_full_sq_answers_enospc(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(2, b, 1);
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    f.sim->has_enter_limit = 1;
    f.sim->enter_limit = 0;
    struct t_loom_op *a = walk_op(s, ""), *b2 = walk_op(s, ""), *c = NULL;
    assert(t_loom_walk(s, "", 0, 0, 0, &c) == -28 && !c);
    t_loom_op_drop(a);
    t_loom_op_drop(b2);
    drop_all(&f, NULL, s, t);
    finish(&f);
}
static void a_rejected_sqe_ends_its_operation_with_the_kernels_errno(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "f", "x");
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    struct t_loom_fid *fid = walk_ok(f.r, s, "f");
    assert(open_rc(f.r, fid, 7) == SIM_EINVAL);
    struct res o;
    assert(run(f.r, K_WALK, walk_op(s, ".."), &o) == SIM_EINVAL && !o.fid);
    drop_all(&f, fid, s, t);
    finish(&f);
}
extern const char *t_test_expected_fatal;
static void expect_fatal(const char *func, void (*misuse)(void)) {
    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        t_test_expected_fatal = func;
        misuse();
        _exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 77) {
        fprintf(stderr, "%s: no fatal there (status %d)\n", func, status);
        assert(0);
    }
}
static void drop_an_sqe(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    sim_poke_hdr(f.sim, 9, 1);
    struct t_loom_op *op;
    (void)t_loom_connect(target_ok(f.r, "clip"), 0, 0, &op);
}
static void a_dropped_sqe_ends_the_process(void) { expect_fatal("enter_ring", drop_an_sqe); }
static void a_forged_completion_fails_the_ring(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    struct t_loom_target *t = target_ok(f.r, "clip");
    f.sim->has_forge = 1;
    f.sim->forge_next = 0xdead;
    struct t_loom_op *op;
    assert(t_loom_connect(t, 0, 0, &op) == 0);
    assert(t_loom_turn(f.r, 0, 0) == SIM_EIO);
    struct res o = { 0 };
    assert(wait_op(f.r, K_CONNECT, op, &o) == SIM_EIO);
    assert(f.sim->nclosed == 1 && f.sim->closed_fds[0] == 7); /* a failed ring is closed */
    t_loom_op_drop(op);
    t_loom_target_drop(t);
    t_loom_ring_drop(f.r);
    finish(&f);
}
static void closing_with_leases_held_keeps_their_bytes_until_they_drop(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "f", "kept");
    struct t_loom_service_pool_member m[] = { range(0, 0, 64) };
    struct t_loom_pool *p = pool_ok(f.r, m, 1);
    struct t_loom_target *t;
    struct t_loom_scope *s;
    struct t_loom_fid *fid;
    opened(&f, "f", &t, &s, &fid);
    struct t_loom_op *stream = stream_op(fid, p, 0);
    struct t_ring_payload *pl;
    int32_t end;
    assert(next_shot(f.r, stream, &pl, &end) == 1);
    t_loom_op_drop(stream);
    t_loom_pool_drop(p);
    drop_all(&f, fid, s, t);
    assert(f.sim->nclosed == 1 && f.sim->closed_fds[0] == 7);
    /* The buffer, the name arena and the ring mapping, all still attached. */
    assert(f.sim->nregions == 3 && !t_loom_idle(f.base));
    size_t len;
    const unsigned char *bytes = t_loom_payload_bytes(pl, &len);
    assert(len == 4 && !memcmp(bytes, "kept", 4));
    t_loom_payload_drop(pl);
    finish(&f);
}
/* A ring closed under the adapter cancels everything, and its descriptor is
 * never closed again: another thread closed it, so the number may name another
 * file. `latch` is a close that races the adapter's call. Answers how many
 * closes the simulated kernel saw. */
static size_t closed_under(int latch, int in_flight) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "f", "data");
    struct t_loom_target *t;
    struct t_loom_scope *s;
    struct t_loom_fid *fid;
    opened(&f, "f", &t, &s, &fid);
    f.sim->hold = 1;
    /* A READ reserves no slot, so nothing REGISTERs after it ends: only the
     * ENTER that follows its routing can learn of the close. */
    struct t_loom_op *read = in_flight ? read_op(fid, range(0, 0, 4), 0, 0) : NULL;
    if (latch)
        sim_latch_under(f.sim);
    else
        sim_close_under(f.sim);
    if (read) {
        struct res o = { 0 };
        int rc = wait_op(f.r, K_READ, read, &o);
        assert(rc == SIM_ECANCELED || (rc == 1 && o.result == SIM_ECANCELED));
        if (rc != 1) t_loom_op_drop(read);
        /* The ENTER after the cancellation learnt of the close. */
        assert(t_loom_turn(f.r, 1, t_loom_now(f.r)) == SIM_ECANCELED);
    } else {
        /* Nothing to route: the wait learns of it. */
        assert(t_loom_turn(f.r, 0, 0) == SIM_ECANCELED);
    }
    struct t_loom_op *w = NULL;
    assert(t_loom_walk(s, "", 0, 0, 0, &w) == SIM_ECANCELED && !w);
    drop_all(&f, fid, s, t);
    size_t closes = f.sim->nclosed;
    finish(&f);
    return closes;
}
static void an_enter_that_finds_the_descriptor_closed_forgets_it(void) {
    assert(closed_under(0, 1) == 1); /* only the other thread's close */
}
static void an_enter_that_races_the_close_forgets_the_descriptor(void) { assert(closed_under(1, 1) == 0); }
static void a_poll_that_finds_the_descriptor_closed_forgets_it(void) { assert(closed_under(0, 0) == 1); }
static void a_hangup_while_polling_forgets_the_descriptor(void) { assert(closed_under(1, 0) == 0); }
static void retire_waits_until_the_kernel_retires_every_scope(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    assert(!t_loom_retired(f.r));
    t_loom_retire(f.r);
    struct t_loom_op *op = NULL;
    assert(t_loom_connect(t, 0, 0, &op) == SIM_ECANCELED && !op);
    uint64_t deadline = t_loom_now(f.r) + 10 * MS;
    while (!t_loom_retired(f.r) && t_loom_now(f.r) < deadline) turn_until(f.r, deadline);
    assert(t_loom_retired(f.r));
    assert(f.sim->naborts == 1);
    int kernel_retired = 0;
    for (size_t i = 0; i < 64; ++i)
        kernel_retired |= f.sim->slots[i].used && f.sim->slots[i].kind == SIM_SCOPE &&
                          f.sim->slots[i].state == SIM_RETIRED;
    /* retired answered only after the kernel retired the scope */
    assert(kernel_retired);
    drop_all(&f, NULL, s, t);
    finish(&f);
}

/* ---- PART 2 ---- */
static void turns(struct t_loom_ring *r, int n) {
    for (int i = 0; i < n; ++i) turn_until(r, t_loom_now(r) + MS);
}
static void a_dropped_fid_is_clunked_then_reaped(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "f", "x");
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    struct t_loom_fid *fid = walk_ok(f.r, s, "f");
    assert(sim_live_slots(f.sim) == 3);
    t_loom_fid_drop(fid);
    turns(f.r, 3);
    assert(sim_live_slots(f.sim) == 2); /* the fid's slot is free */
    assert(f.sim->naborts == 0);        /* a clunk is not a cancel */
    drop_all(&f, NULL, s, t);
    finish(&f);
}
static void a_dropped_scope_is_reaped_after_its_fids_without_clunking_them(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "f", "x");
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    struct t_loom_fid *a = walk_ok(f.r, s, "f"), *b2 = walk_ok(f.r, s, "");
    assert(open_rc(f.r, a, 0) == 0);
    assert(sim_live_slots(f.sim) == 4);
    /* The abort ends the session, so no fid is clunked. Each fid is reaped once
     * the scope has retired, and the scope after its fids, in the same turn. */
    t_loom_fid_drop(a);
    t_loom_fid_drop(b2);
    t_loom_scope_drop(s);
    turns(f.r, 1);
    assert(sim_live_slots(f.sim) == 1); /* only the target remains */
    assert(f.sim->clunks == 0 && f.sim->naborts == 1);
    drop_all(&f, NULL, NULL, t);
    finish(&f);
}
static void a_failed_walk_dropped_unobserved_is_reaped_without_a_clunk(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    struct t_loom_op *walk = walk_op(s, "missing");
    turns(f.r, 3);
    assert(f.sim->nops == 0); /* the WALK's -ENOENT arrived unobserved */
    t_loom_op_drop(walk);
    turns(f.r, 3);
    assert(sim_live_slots(f.sim) == 2); /* the reservation is reaped */
    assert(f.sim->clunk_sqes == 0);     /* a fid that never walked is not clunked */
    assert(f.sim->naborts == 0);
    drop_all(&f, NULL, s, t);
    finish(&f);
}
static void a_walk_dropped_in_flight_aborts_and_reaps_its_reservation_unclunked(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "f", "x");
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    f.sim->hold = 1;
    struct t_loom_op *walk = walk_op(s, "f");
    assert(sim_live_slots(f.sim) == 3);
    t_loom_op_drop(walk);
    assert(f.sim->naborts == 1); /* one ABORT for the drop */
    turns(f.r, 3);
    assert(f.sim->nops == 0);
    assert(sim_live_slots(f.sim) == 2); /* the destination is reaped once its WALK ends */
    assert(f.sim->clunk_sqes == 0);     /* an aborted scope's fid is not clunked */
    drop_all(&f, NULL, s, t);
    finish(&f);
}
static void an_open_dropped_in_flight_aborts_its_scope(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "f", "x");
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    struct t_loom_fid *fid = walk_ok(f.r, s, "f");
    f.sim->hold = 1;
    struct t_loom_op *open;
    assert(t_loom_open(fid, 0, 0, 0, &open) == 0);
    t_loom_op_drop(open);
    assert(f.sim->naborts == 1); /* one ABORT for the drop */
    turns(f.r, 1);
    assert(f.sim->nops == 0);
    f.sim->hold = 0;
    assert(open_rc(f.r, fid, 0) == SIM_ECANCELED);
    drop_all(&f, fid, s, t);
    finish(&f);
}
static void an_explicit_scope_abort_ends_its_operations_once(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "f", "data");
    struct t_loom_target *t;
    struct t_loom_scope *s;
    struct t_loom_fid *fid;
    opened(&f, "f", &t, &s, &fid);
    f.sim->hold = 1;
    struct t_loom_op *read = read_op(fid, range(0, 0, 4), 0, 0);
    t_loom_scope_abort(s);
    t_loom_scope_abort(s);
    assert(f.sim->naborts == 1); /* the second abort issues nothing */
    struct res o;
    assert(run(f.r, K_READ, read, &o) == SIM_ECANCELED);
    drop_all(&f, fid, s, t);
    finish(&f);
}
static void a_target_dropped_under_its_connect_is_reaped_after_it(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    struct t_loom_target *t = target_ok(f.r, "clip");
    f.sim->hold = 1;
    struct t_loom_op *connect;
    assert(t_loom_connect(t, 0, 0, &connect) == 0);
    t_loom_target_drop(t);
    turns(f.r, 1);
    assert(sim_live_slots(f.sim) == 2); /* the CONNECT still borrows its target */
    f.sim->hold = 0;
    struct res o;
    assert(run(f.r, K_CONNECT, connect, &o) >= 0 && o.scope);
    turns(f.r, 1);
    assert(sim_live_slots(f.sim) == 1); /* only the scope remains */
    drop_all(&f, NULL, o.scope, NULL);
    finish(&f);
}
static void an_exhausted_slot_table_reaps_before_it_refuses(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "f", "x");
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    /* Fill the rest of the table with targets, then let one go unreaped. */
    struct t_loom_target *spare[62];
    for (int i = 0; i < 62; ++i) spare[i] = target_ok(f.r, "clip");
    assert(sim_live_slots(f.sim) == 64);
    t_loom_target_drop(spare[61]);
    struct t_loom_fid *fid = walk_ok(f.r, s, "f"); /* the walk took the reaped slot */
    for (int i = 0; i < 61; ++i) t_loom_target_drop(spare[i]);
    drop_all(&f, fid, s, t);
    finish(&f);
}
static int scope_retired_in_kernel(struct sim *s) {
    for (size_t i = 0; i < 64; ++i)
        if (s->slots[i].used && s->slots[i].kind == SIM_SCOPE && s->slots[i].state == SIM_RETIRED) return 1;
    return 0;
}
static void turn_until_retired(struct t_loom_ring *r) {
    uint64_t deadline = t_loom_now(r) + 10 * MS;
    while (!t_loom_retired(r) && t_loom_now(r) < deadline) turn_until(r, deadline);
    assert(t_loom_retired(r));
}
static void an_operation_held_across_retirement_keeps_its_terminal(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "f", "x");
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    f.sim->hold = 1;
    struct t_loom_op *walk = walk_op(s, "f");
    t_loom_retire(f.r);
    turn_until_retired(f.r);
    struct res o;
    /* The WALK keeps the terminal its abort gave it. */
    assert(run(f.r, K_WALK, walk, &o) == SIM_ECANCELED && !o.fid);
    assert(f.sim->nclosed == 0); /* no operation closes the ring */
    drop_all(&f, NULL, s, t);
    finish(&f);
}
static void a_dropped_read(struct t_loom_fid *fid) { t_loom_op_drop(read_op(fid, range(0, 0, 4), 0, 0)); }
static void retirement_waits_for_terminals_posted_after_the_scope_retires(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "f", "data");
    struct t_loom_target *t;
    struct t_loom_scope *s;
    struct t_loom_fid *fid;
    opened(&f, "f", &t, &s, &fid);
    f.sim->hold = 1;
    f.sim->hold_terminals = 1;
    a_dropped_read(fid);
    t_loom_retire(f.r);
    turns(f.r, 3);
    assert(scope_retired_in_kernel(f.sim)); /* the scope retired first */
    assert(!t_loom_retired(f.r));           /* retired waits for the READ's terminal */
    sim_release_terminals(f.sim);
    turn_until_retired(f.r);
    assert(f.sim->nclosed == 0); /* the late terminal did not fail the ring */
    drop_all(&f, fid, s, t);
    finish(&f);
}
static void an_sqe_not_yet_entered_holds_retirement_back(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "f", "data");
    struct t_loom_target *t;
    struct t_loom_scope *s;
    struct t_loom_fid *fid;
    opened(&f, "f", &t, &s, &fid);
    f.sim->has_enter_limit = 1;
    f.sim->enter_limit = 0;
    a_dropped_read(fid);
    t_loom_retire(f.r);
    turns(f.r, 3);
    assert(sim_hdr_word(f.sim, 1) - sim_hdr_word(f.sim, 0) == 1); /* the READ is still queued */
    assert(scope_retired_in_kernel(f.sim));
    assert(!t_loom_retired(f.r)); /* retired waits for the queued READ's terminal */
    f.sim->has_enter_limit = 0;
    turn_until_retired(f.r);
    assert(f.sim->nclosed == 0);
    drop_all(&f, fid, s, t);
    finish(&f);
}
/* Two SQ slots and four CQ slots: eight shots and the end overflow the CQ, and
 * the ENTER after each drain posts what was deferred. */
static struct t_loom_op *burst(struct fx *f, struct t_loom_pool **p, struct t_loom_target **t,
                               struct t_loom_scope **s, struct t_loom_fid **fid) {
    size_t b[] = { 4096 };
    *f = ring_of(2, b, 1);
    file(f->sim, "burst", "");
    for (int i = 0; i < 8; ++i) {
        char shot[4];
        memset(shot, 'a' + i, 4);
        sim_shot(f->sim, "burst", shot, 4);
    }
    struct t_loom_service_pool_member m[8];
    for (int i = 0; i < 8; ++i) m[i] = range(0, (uint64_t)i * 64, 64);
    *p = pool_ok(f->r, m, 8);
    opened(f, "burst", t, s, fid);
    return stream_op(*fid, *p, 1);
}
static void a_terminal_posted_but_not_yet_routed_holds_retirement_back(void) {
    struct fx f;
    struct t_loom_pool *p;
    struct t_loom_target *t;
    struct t_loom_scope *s;
    struct t_loom_fid *fid;
    struct t_loom_op *stream = burst(&f, &p, &t, &s, &fid);
    /* The server answers every shot; the first four are routed to the stream. */
    turns(f.r, 1);
    t_loom_op_drop(stream);
    t_loom_retire(f.r);
    sim_engine_visit(f.sim);
    assert(scope_retired_in_kernel(f.sim)); /* the scope retired first */
    /* Route four shots; the ENTER that follows posts the end. */
    assert(t_loom_turn(f.r, 1, t_loom_now(f.r) + MS) == 4);
    assert(!t_loom_retired(f.r)); /* retired waits for the end still in the CQ */
    turn_until_retired(f.r);
    assert(f.sim->nclosed == 0); /* the end did not fail the ring */
    /* The four shots routed after retire go back although no pool takes them. */
    assert(f.sim->returns == 8);
    t_loom_pool_drop(p);
    drop_all(&f, fid, s, t);
    finish(&f);
}
static void an_enter_refusal_not_yet_posted_holds_retirement_back(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    file(f.sim, "f", "data");
    struct t_loom_target *t;
    struct t_loom_scope *s;
    struct t_loom_fid *fid;
    opened(&f, "f", &t, &s, &fid);
    f.sim->has_enter_limit = 1;
    f.sim->enter_limit = 0;
    a_dropped_read(fid);
    t_loom_retire(f.r);
    f.sim->hold_rejections = 1;
    f.sim->has_enter_limit = 0;
    turns(f.r, 3);
    assert(sim_hdr_word(f.sim, 1) == sim_hdr_word(f.sim, 0)); /* ENTER consumed the READ */
    assert(scope_retired_in_kernel(f.sim));
    assert(!t_loom_retired(f.r)); /* retired waits for the refusal still to be posted */
    sim_release_rejections(f.sim);
    turn_until_retired(f.r);
    assert(f.sim->nclosed == 0); /* the late refusal did not fail the ring */
    drop_all(&f, fid, s, t);
    finish(&f);
}
static void a_stream_dropped_before_its_end_returns_the_shots_still_arriving(void) {
    struct fx f;
    struct t_loom_pool *p;
    struct t_loom_target *t;
    struct t_loom_scope *s;
    struct t_loom_fid *fid;
    struct t_loom_op *stream = burst(&f, &p, &t, &s, &fid);
    turns(f.r, 1);
    t_loom_op_drop(stream);
    assert(f.sim->returns == 4); /* the four payloads the stream held */
    /* The next four arrive for a stream that is gone. */
    assert(t_loom_turn(f.r, 1, t_loom_now(f.r) + MS) == 4);
    assert(f.sim->returns == 8); /* each late shot's lease went straight back */
    t_loom_pool_drop(p);
    drop_all(&f, fid, s, t);
    finish(&f);
}
static void a_late_shot_after_the_ring_closed_under_forgets_the_descriptor(void) {
    struct fx f;
    struct t_loom_pool *p;
    struct t_loom_target *t;
    struct t_loom_scope *s;
    struct t_loom_fid *fid;
    struct t_loom_op *stream = burst(&f, &p, &t, &s, &fid);
    turns(f.r, 1);
    t_loom_op_drop(stream);
    t_loom_retire(f.r);
    sim_close_under(f.sim);
    /* The first late shot's RETURN finds the descriptor gone. */
    assert(t_loom_turn(f.r, 1, t_loom_now(f.r) + MS) == SIM_ECANCELED);
    t_loom_pool_drop(p);
    drop_all(&f, fid, s, t);
    assert(f.sim->nclosed == 1); /* the lost number is never closed */
    finish(&f);
}
static void a_scope_dropped_after_the_ring_closed_under_never_closes_the_number(void) {
    size_t b[] = { 4096 };
    struct fx f = ring_of(8, b, 1);
    struct t_loom_target *t = target_ok(f.r, "clip");
    struct t_loom_scope *s = connect_ok(f.r, t);
    sim_close_under(f.sim);
    /* Its ABORT is answered -EBADF, and no turn follows before the ring drops. */
    t_loom_scope_drop(s);
    drop_all(&f, NULL, NULL, t);
    assert(f.sim->nclosed == 1); /* only the other thread's close */
    finish(&f);
}

/* ---- PART 3 ---- */
