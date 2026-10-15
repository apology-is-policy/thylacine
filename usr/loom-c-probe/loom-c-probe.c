// loom-c-probe -- the C native Loom adapter driven from EL0 against
// /bin/loom-fixture (docs/ASYNC-SERVICE-ABI.md, NA-4d). The kernel test
// loom.native_probe_c starts both and requires 74 from the default image, where
// setup refuses -EOPNOTSUPP, or 75 from the qualification image, where every
// leg below passed. The legs are loom-native-probe's, through the C interface,
// and each one reads the fixture's stats file for what the server saw.
//
// fd 0 is the registry the fixture posts in, fd 1 the control pipe (closed at
// exit, which ends the fixture) and fd 2 the notify pipe.

#include <stdint.h>
#include <stddef.h>
#include <thyla/syscall.h>
#include <thyla/poll.h>
#include <thyla/loom_client/adapter.h>

#define PASS_DEFAULT 74
#define PASS_QUAL    75
#define REGISTRY     0
#define NOTIFY       2

// The kernel's errno registry (kernel/include/thylacine/errno.h).
#define E_NOENT      2
#define E_ACCES     13
#define E_OPNOTSUPP 95
#define E_TIMEDOUT 110
#define E_CANCELED 125

#define PAGE       4096u
#define ENTRIES    2u
#define CQ_ENTRIES 4u
#define WAIT_NS    5000000000ull

// The fixture's files and its stats counters (usr/loom-fixture).
#define DATA_LEN   (3u * 4096u + 1000u)
#define STREAM_LEN 2600u
#define SHOT       512u
#define ACCEPTED   0
#define ENDED      1
#define VIOLATIONS 2
#define STALLED    3
#define STAT_COUNT 11

static T_LOOM_RING_STORAGE(ENTRIES) g_store;
static struct t_loom_ring *g_ring;
static struct t_loom_kernel g_kernel;
static unsigned char g_got[STREAM_LEN];

static void put_dec(long v) {
    char b[24];
    int n = 0;
    unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
    do { b[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 22);
    char o[24];
    int k = 0;
    if (v < 0) o[k++] = '-';
    while (n) o[k++] = b[--n];
    o[k] = 0;
    t_putstr(o);
}

__attribute__((noreturn))
static void fail(int line, const char *why, long got) {
    t_putstr("loom-c-probe: FAIL line ");
    put_dec(line);
    t_putstr(": ");
    t_putstr(why);
    t_putstr(" (got ");
    put_dec(got);
    t_putstr(")\n");
    t_exits(1);
}
#define CHECK(cond, why, got) do { if (!(cond)) fail(__LINE__, (why), (long)(got)); } while (0)

static void say(const char *what) {
    t_putstr("loom-c-probe: ");
    t_putstr(what);
    t_putstr("\n");
}

// Byte-wise and volatile, so no libc call is emitted; native C links none.
static int matches(const unsigned char *b, size_t len, size_t from) {
    const volatile unsigned char *v = b;
    for (size_t i = 0; i < len; i++)
        if (v[i] != (unsigned char)((from + i) % 251)) return 0;
    return 1;
}
static size_t append(size_t at, const unsigned char *b, size_t len) {
    const volatile unsigned char *v = b;
    volatile unsigned char *d = g_got;
    for (size_t i = 0; i < len && at < sizeof(g_got); i++) d[at++] = v[i];
    return at;
}
static size_t name_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static struct t_loom_service_pool_member range(uint32_t buffer, uint64_t offset, uint64_t length) {
    struct t_loom_service_pool_member m;
    m.buffer_index = buffer;
    m.reserved = 0;
    m.offset = offset;
    m.length = length;
    return m;
}

static uint64_t soon(void) {
    return t_loom_now(g_ring) + WAIT_NS;
}

// Sleeps up to ms on the notify pipe, which the fixture writes only once.
static void nap(long ms) {
    struct pollfd p = { .fd = NOTIFY, .events = POLLIN, .revents = 0 };
    (void)t_poll(&p, 1, ms);
}

// Turns the ring while a completion call answers 0 (pending); it must then
// answer 1 (done).
#define AWAIT(call, why) do { \
    uint64_t until_ = soon(); \
    int answer_; \
    while ((answer_ = (call)) == 0) { \
        CHECK(t_loom_now(g_ring) < until_, why ": no completion", 0); \
        int64_t turned_ = t_loom_turn(g_ring, 1, until_); \
        CHECK(turned_ >= 0, why ": a turn", turned_); \
    } \
    CHECK(answer_ == 1, why ": the completion call", answer_); \
} while (0)

static struct t_loom_scope *connect(struct t_loom_target *t) {
    struct t_loom_op *op;
    int rc = t_loom_connect(t, 1, soon(), &op);
    CHECK(rc == 0, "submit a CONNECT", rc);
    int32_t res = 0;
    struct t_loom_scope *s = NULL;
    AWAIT(t_loom_connect_done(op, &res, &s), "a CONNECT");
    CHECK(res >= 0 && s, "a CONNECT", res);
    return s;
}

// A WALK whose result must be `want`: 0 for a new fid, or a negative errno.
static struct t_loom_fid *walk(struct t_loom_scope *s, const char *name, int32_t want) {
    struct t_loom_op *op;
    int rc = t_loom_walk(s, name, name_len(name), 1, soon(), &op);
    CHECK(rc == 0, "submit a WALK", rc);
    int32_t res = 0;
    struct t_loom_fid *fid = NULL;
    AWAIT(t_loom_walk_done(op, &res, &fid), "a WALK");
    if (want < 0) CHECK(res == want && !fid, "a WALK's refusal", res);
    else CHECK(res >= 0 && fid, "a WALK", res);
    return fid;
}

static void open_as(struct t_loom_fid *fid, uint32_t mode, int32_t want) {
    struct t_loom_op *op;
    int rc = t_loom_open(fid, mode, 1, soon(), &op);
    CHECK(rc == 0, "submit an LOPEN", rc);
    int32_t res = 1;
    AWAIT(t_loom_open_done(op, &res), "an LOPEN");
    CHECK(res == want, "an LOPEN's result", res);
}

static struct t_fixed_read_result *read_at(struct t_loom_fid *fid, struct t_loom_service_pool_member r,
                                           uint64_t offset, const unsigned char **bytes, size_t *len) {
    struct t_loom_op *op;
    int rc = t_loom_read(fid, r, offset, 1, soon(), &op);
    CHECK(rc == 0, "submit a READ", rc);
    int32_t res = -1;
    struct t_fixed_read_result *rr = NULL;
    AWAIT(t_loom_read_done(op, &res, &rr), "a READ");
    CHECK(res >= 0 && rr, "a READ", res);
    *bytes = t_loom_read_bytes(rr, len);
    CHECK(*len == (size_t)res, "a READ's bytes are its result", *len);
    return rr;
}

static void stats(struct t_loom_fid *fid, uint32_t v[STAT_COUNT]) {
    const unsigned char *b;
    size_t len;
    struct t_fixed_read_result *rr = read_at(fid, range(2, 0, 64), 0, &b, &len);
    CHECK(len == 4 * STAT_COUNT, "the stats file holds one u32 per counter", len);
    for (int i = 0; i < STAT_COUNT; i++)
        v[i] = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 |
               (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
    t_loom_read_drop(rr);
}

// Reads the stats until counter `at` reaches `want`.
static void stats_until(struct t_loom_fid *fid, int at, uint32_t want, uint32_t v[STAT_COUNT]) {
    uint64_t until = soon();
    for (;;) {
        stats(fid, v);
        if (v[at] >= want) return;
        CHECK(t_loom_now(g_ring) < until, "a fixture counter reaches its value", at);
        nap(5);
    }
}

// Every byte a multishot stream yields, in arrival order, after checking it
// ends with the 0 of its EOF read. With swap it holds one lease and returns the
// newer of each pair first, the opposite order to their arrival.
static size_t drain(struct t_loom_op *st, int swap) {
    size_t n = 0;
    struct t_ring_payload *held = NULL;
    for (;;) {
        struct t_ring_payload *p = NULL;
        int end = 0;
        int32_t res = -1;
        AWAIT(t_loom_stream_next(st, &p, &end, &res), "the next shot");
        if (end) {
            CHECK(res == 0, "a multishot stream ends with its EOF read's 0", res);
            break;
        }
        size_t len;
        const unsigned char *b = t_loom_payload_bytes(p, &len);
        CHECK(len <= SHOT, "a shot fits its member", len);
        if (held) {
            size_t hl;
            const unsigned char *hb = t_loom_payload_bytes(held, &hl);
            n = append(n, hb, hl);
            n = append(n, b, len);
            int rc = t_loom_payload_return(p);
            CHECK(rc == 0, "return the newer lease first", rc);
            rc = t_loom_payload_return(held);
            CHECK(rc == 0, "then the older one", rc);
            held = NULL;
        } else if (swap) {
            held = p;
        } else {
            n = append(n, b, len);
            int rc = t_loom_payload_return(p);
            CHECK(rc == 0, "return a lease", rc);
        }
    }
    if (held) {
        size_t hl;
        const unsigned char *hb = t_loom_payload_bytes(held, &hl);
        n = append(n, hb, hl);
        int rc = t_loom_payload_return(held);
        CHECK(rc == 0, "return the last lease", rc);
    }
    return n;
}

static struct t_ring_payload *shot(struct t_loom_op *st) {
    struct t_ring_payload *p = NULL;
    int end = 0;
    int32_t res = -1;
    AWAIT(t_loom_stream_next(st, &p, &end, &res), "a shot");
    CHECK(!end && p, "a stream ended before its shot", res);
    return p;
}

static struct t_loom_op *stream(struct t_loom_fid *fid, struct t_loom_pool *pool) {
    struct t_loom_op *op;
    int rc = t_loom_stream(fid, pool, 0, SHOT, 1, 0, 0, &op);
    CHECK(rc == 0, "a multishot stream", rc);
    return op;
}

static int run(void) {
    unsigned char b = 0;
    long n = t_read(NOTIFY, &b, 1);
    CHECK(n == 1 && b == 'R', "the fixture has posted its service", n);
    g_kernel = t_loom_kernel_native();
    static const size_t lengths[5] = { PAGE, PAGE, PAGE, PAGE, PAGE };
    // 0: fixed data and sink I/O; 1: pool A; 2: stats; 3: pool B; 4: stalled reads.
    int rc = T_LOOM_RING_SETUP(&g_store, &g_kernel, ENTRIES, lengths, 5, &g_ring);
    if (rc == -E_OPNOTSUPP) {
        say("default image: setup refuses -EOPNOTSUPP");
        return PASS_DEFAULT;
    }
    CHECK(rc == 0 && g_ring, "a private ring in the qualification image", rc);

    say("connect, walk, open");
    struct t_loom_target *t = NULL;
    rc = t_loom_target(g_ring, REGISTRY, "nosuch", 6, &t);
    CHECK(rc == -E_NOENT && !t, "TARGET of a name nobody posted", rc);
    rc = t_loom_target(g_ring, REGISTRY, "fixture", 7, &t);
    CHECK(rc == 0 && t, "TARGET of the fixture", rc);
    struct t_loom_scope *s1 = connect(t);
    struct t_loom_fid *sf = walk(s1, "stats", 0);
    open_as(sf, 0, 0);
    uint32_t v[STAT_COUNT], base[STAT_COUNT];
    stats(sf, base);
    CHECK(base[ACCEPTED] == 1 && base[ENDED] == 0, "the fixture holds one session", base[ACCEPTED]);
    walk(s1, "nosuch", -E_NOENT);
    struct t_loom_fid *data = walk(s1, "data", 0);
    open_as(data, 1, -E_ACCES);
    open_as(data, 0, 0);
    struct t_loom_fid *sink = walk(s1, "sink", 0);
    open_as(sink, 2, 0);

    say("fixed reads and writes");
    const unsigned char *bytes;
    size_t len;
    struct t_fixed_read_result *rr = read_at(data, range(0, 0, PAGE), 0, &bytes, &len);
    CHECK(len == PAGE && matches(bytes, len, 0), "a fixed READ's bytes", len);
    t_loom_read_drop(rr);
    rr = read_at(data, range(0, 0, PAGE), 3 * PAGE, &bytes, &len);
    CHECK(len == DATA_LEN - 3 * PAGE && matches(bytes, len, 3 * PAGE), "a short READ at the end", len);
    t_loom_read_drop(rr);
    static unsigned char msg[100];
    volatile unsigned char *m = msg;
    for (int i = 0; i < 100; i++) m[i] = (unsigned char)(i * 7 + 1);
    struct t_loom_op *op;
    rc = t_loom_write(sink, range(0, 0, 100), 0, msg, 100, 1, soon(), &op);
    CHECK(rc == 0, "submit a WRITE", rc);
    int32_t res = -1;
    AWAIT(t_loom_write_done(op, &res), "a WRITE");
    CHECK(res == 100, "a fixed WRITE's count", res);
    rr = read_at(sink, range(0, 0, PAGE), 0, &bytes, &len);
    CHECK(len == 100, "the sink reads back what was written", len);
    for (int i = 0; i < 100; i++) CHECK(bytes[i] == msg[i], "the sink's bytes", i);
    t_loom_read_drop(rr);

    say("a multishot stream with out-of-order returns");
    struct t_loom_service_pool_member two[2] = { range(1, 0, SHOT), range(1, SHOT, SHOT) };
    struct t_loom_pool *pool_a = NULL;
    rc = t_loom_pool(g_ring, two, 2, &pool_a);
    CHECK(rc == 0 && pool_a, "pool A", rc);
    struct t_loom_fid *first = walk(s1, "stream", 0);
    open_as(first, 0, 0);
    struct t_loom_op *st = stream(first, pool_a);
    size_t got = drain(st, 1);
    CHECK(got == STREAM_LEN && matches(g_got, got, 0), "every shot, in order", got);
    t_loom_op_drop(st);

    say("a full CQ");
    struct t_loom_service_pool_member eight[8];
    for (uint32_t i = 0; i < 8; i++) eight[i] = range(3, (uint64_t)i * SHOT, SHOT);
    struct t_loom_pool *pool_b = NULL;
    rc = t_loom_pool(g_ring, eight, 8, &pool_b);
    CHECK(rc == 0 && pool_b, "pool B", rc);
    struct t_loom_fid *second = walk(s1, "stream", 0);
    open_as(second, 0, 0);
    st = stream(second, pool_b);
    // Submitting entered the SQE; the executor runs without a turn.
    uint64_t until = soon();
    while (t_loom_backlog(g_ring) < CQ_ENTRIES) {
        CHECK(t_loom_now(g_ring) < until, "the CQ fills while no turn reaps it", t_loom_backlog(g_ring));
        nap(1);
    }
    CHECK(t_loom_backlog(g_ring) == CQ_ENTRIES, "a full CQ holds exactly its entries", t_loom_backlog(g_ring));
    int64_t routed = t_loom_turn(g_ring, 0, 0);
    CHECK(routed == CQ_ENTRIES, "one turn routes the whole CQ", routed);
    got = drain(st, 0);
    CHECK(got == STREAM_LEN && matches(g_got, got, 0), "no shot lost behind the full CQ", got);
    t_loom_op_drop(st);

    say("a deadline aborts its scope");
    struct t_loom_scope *s2 = connect(t);
    struct t_loom_fid *stall2 = walk(s2, "stall", 0);
    open_as(stall2, 0, 0);
    stats(sf, base);
    uint64_t deadline = t_loom_now(g_ring) + 1000000000ull;
    rc = t_loom_read(stall2, range(4, 0, PAGE), 0, 1, deadline, &op);
    CHECK(rc == 0, "submit a READ the server holds", rc);
    stats_until(sf, STALLED, base[STALLED] + 1, v);
    rr = NULL;
    res = 0;
    AWAIT(t_loom_read_done(op, &res, &rr), "the held READ");
    CHECK(res == -E_TIMEDOUT && !rr, "the deadline ends the held READ", res);
    walk(s2, "data", -E_CANCELED);
    stats_until(sf, ENDED, base[ENDED] + 1, v);

    say("cancellation by drop");
    struct t_loom_scope *s3 = connect(t);
    struct t_loom_fid *stall3 = walk(s3, "stall", 0);
    open_as(stall3, 0, 0);
    stats(sf, base);
    rc = t_loom_read(stall3, range(4, 0, PAGE), 0, 0, 0, &op);
    CHECK(rc == 0, "submit a READ the server holds", rc);
    stats_until(sf, STALLED, base[STALLED] + 1, v);
    t_loom_op_drop(op);
    walk(s3, "data", -E_CANCELED);
    stats_until(sf, ENDED, base[ENDED] + 1, v);
    rr = read_at(data, range(0, 0, PAGE), PAGE, &bytes, &len);
    CHECK(len == PAGE && matches(bytes, len, PAGE), "a scope beside the aborted ones still reads", len);
    t_loom_read_drop(rr);

    say("closing the ring with leases held");
    stats(sf, v);
    CHECK(v[VIOLATIONS] == 0, "the fixture saw no protocol violation", v[VIOLATIONS]);
    struct t_loom_fid *third = walk(s1, "stream", 0);
    open_as(third, 0, 0);
    st = stream(third, pool_a);
    struct t_ring_payload *p1 = shot(st);
    struct t_ring_payload *p2 = shot(st);
    rr = read_at(data, range(0, 0, PAGE), 0, &bytes, &len);
    t_loom_close(g_ring);
    size_t l1, l2;
    const unsigned char *b1 = t_loom_payload_bytes(p1, &l1);
    const unsigned char *b2 = t_loom_payload_bytes(p2, &l2);
    CHECK(l1 == SHOT && matches(b1, l1, 0) && l2 == SHOT && matches(b2, l2, SHOT) &&
          len == PAGE && matches(bytes, len, 0), "leases keep their bytes after the ring closes", l1);
    struct t_loom_target *late = NULL;
    rc = t_loom_target(g_ring, REGISTRY, "fixture", 7, &late);
    CHECK(rc == -E_CANCELED && !late, "a closed ring admits no TARGET", rc);
    rc = t_loom_walk(s1, "data", 4, 0, 0, &op);
    CHECK(rc == -E_CANCELED, "a closed ring admits no WALK", rc);
    t_loom_payload_drop(p1);
    t_loom_payload_drop(p2);
    t_loom_read_drop(rr);
    t_loom_op_drop(st);
    struct t_loom_fid *fids[] = { third, stall3, stall2, second, first, sink, data, sf };
    for (unsigned i = 0; i < sizeof(fids) / sizeof(fids[0]); i++) t_loom_fid_drop(fids[i]);
    t_loom_scope_drop(s3);
    t_loom_scope_drop(s2);
    t_loom_scope_drop(s1);
    t_loom_pool_drop(pool_b);
    t_loom_pool_drop(pool_a);
    t_loom_target_drop(t);
    t_loom_ring_drop(g_ring);
    CHECK(t_loom_idle(&g_store.base) == 1, "every handle dropped leaves the storage idle", 0);
    say("qualification image drives every leg through the C adapter");
    return PASS_QUAL;
}

int main(void) {
    return run();
}
