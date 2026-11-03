// loom-c-probe -- the C native Loom adapter driven from EL0 against
// /bin/loom-fixture (docs/ASYNC-SERVICE-ABI.md, NA-4d). The kernel test
// loom.native_probe_c starts both and requires 74 from the default image, where
// setup refuses -EOPNOTSUPP, or 75 from the qualification image, where every
// leg below passed. The legs are loom-native-probe's, through the C interface,
// and they read the fixture's stats file for what the server saw.
//
// fd 0 is the registry the fixture posts in, fd 1 the control pipe (closed at
// exit, which ends the fixture) and fd 2 the notify pipe. A 'Q' on fd 1 asks the
// fixture for a 'Z' on fd 2 once it holds no session.

#include <stdint.h>
#include <stddef.h>
#include <thyla/syscall.h>
#include <thyla/poll.h>
#include <thyla/loom_client/adapter.h>

#define PASS_DEFAULT 74
#define PASS_QUAL    75
#define REGISTRY     0
#define CONTROL      1
#define NOTIFY       2

// The kernel's errno registry (kernel/include/thylacine/errno.h).
#define E_NOENT      2
#define E_IO         5
#define E_BADF       9
#define E_ACCES     13
#define E_BUSY      16
#define E_INVAL     22
#define E_NOSPC     28
#define E_OPNOTSUPP 95
#define E_TIMEDOUT 110
#define E_CANCELED 125

#define PAGE       4096u
#define ENTRIES    2u
#define CQ_ENTRIES 4u
#define WAIT_NS    5000000000ull
// Targets, scopes, fids and pools share the kernel's LOOM_SERVICE_SLOT_COUNT
// slots (kernel/include/thylacine/loom_service_abi.h).
#define SLOTS      64u
// LOOM_SERVICE_POOL_MEMBERS: the most members one pool may name.
#define POOL_MEMBERS 64u

// The fixture's files and its stats counters (usr/loom-fixture).
#define DATA_LEN   (3u * 4096u + 1000u)
#define LEAF_LEN   600u
#define STREAM_LEN 2600u
#define SHOT       512u
#define ACCEPTED   0
#define ENDED      1
#define VIOLATIONS 2
#define STALLED    3
#define CLUNKS     5
#define GETATTRS   6
#define WALKS      7
#define FAULTS     11
#define STAT_COUNT 12

static T_LOOM_RING_STORAGE(ENTRIES) g_store;
static T_LOOM_RING_STORAGE(ENTRIES) g_store2;
static struct t_loom_ring *g_ring;
static struct t_loom_ring *g_ring2;
// The ring the helpers below turn and read the clock of.
static struct t_loom_ring *g_cur;
static struct t_loom_kernel g_kernel;
static unsigned char g_got[STREAM_LEN];
static struct t_loom_fid *g_held[SLOTS];

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
// Copies what fits in g_got and returns the true total, so surplus bytes show.
static size_t append(size_t at, const unsigned char *b, size_t len) {
    const volatile unsigned char *v = b;
    volatile unsigned char *d = g_got;
    for (size_t i = 0; i < len && at + i < sizeof(g_got); i++) d[at + i] = v[i];
    return at + len;
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
    return t_loom_now(g_cur) + WAIT_NS;
}

// Sleeps up to ms on the notify pipe: after the fixture's 'R', nothing arrives
// there until the probe asks for the 'Z'.
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
        CHECK(t_loom_now(g_cur) < until_, why ": no completion", 0); \
        int64_t turned_ = t_loom_turn(g_cur, 1, until_); \
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

// A WALK refused at submit: the kernel reserves its FID first, and no FID
// reserves under a scope that is no longer READY.
static void walk_refused(struct t_loom_scope *s, const char *name, int want, const char *why) {
    struct t_loom_op *op = NULL;
    int rc = t_loom_walk(s, name, name_len(name), 1, soon(), &op);
    CHECK(rc == want && !op, why, rc);
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

// A READ the kernel refuses with `want` and no bytes.
static void read_refused(struct t_loom_fid *fid, int32_t want, const char *why) {
    struct t_loom_op *op;
    int rc = t_loom_read(fid, range(0, 0, PAGE), 0, 1, soon(), &op);
    CHECK(rc == 0, "submit a READ", rc);
    int32_t res = 0;
    struct t_fixed_read_result *rr = NULL;
    AWAIT(t_loom_read_done(op, &res, &rr), "a refused READ");
    CHECK(res == want && !rr, why, res);
}

// A dropped operation keeps its entry in the request book until a turn routes its
// terminal, so a read right after a drop may first have to make that room.
static void stats(struct t_loom_fid *fid, uint32_t v[STAT_COUNT]) {
    const unsigned char *b;
    size_t len;
    uint64_t until = soon();
    struct t_loom_op *op;
    int rc;
    while ((rc = t_loom_read(fid, range(2, 0, 64), 0, 1, soon(), &op)) == -E_NOSPC) {
        CHECK(t_loom_now(g_cur) < until, "room for a stats read", rc);
        int64_t turned = t_loom_turn(g_cur, 1, t_loom_now(g_cur) + 10000000ull);
        CHECK(turned >= 0, "a turn with the book full", turned);
    }
    CHECK(rc == 0, "submit a stats read", rc);
    int32_t res = -1;
    struct t_fixed_read_result *rr = NULL;
    AWAIT(t_loom_read_done(op, &res, &rr), "a stats read");
    CHECK(res >= 0 && rr, "a stats read", res);
    b = t_loom_read_bytes(rr, &len);
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
        CHECK(t_loom_now(g_cur) < until, "a fixture counter reaches its value", at);
        nap(5);
    }
}

// The kernel must accept a lease's return.
static void give_back(struct t_ring_payload *p, const char *why) {
    int rc = t_loom_payload_return(p);
    CHECK(rc == 0, why, rc);
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
            give_back(p, "return the newer lease first");
            give_back(held, "then the older one");
            held = NULL;
        } else if (swap) {
            held = p;
        } else {
            n = append(n, b, len);
            give_back(p, "return a lease");
        }
    }
    if (held) {
        size_t hl;
        const unsigned char *hb = t_loom_payload_bytes(held, &hl);
        n = append(n, hb, hl);
        give_back(held, "return the last lease");
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

// Walks data under s until the slot table is full, which refuses the next WALK
// -ENOSPC at submit, then drops every fid it walked. Returns how many fit.
static unsigned fill(struct t_loom_scope *s) {
    unsigned n = 0;
    for (;;) {
        struct t_loom_op *op = NULL;
        int rc = t_loom_walk(s, "data", 4, 1, soon(), &op);
        if (rc != 0) {
            CHECK(rc == -E_NOSPC && !op, "a full slot table refuses the WALK at submit", rc);
            break;
        }
        int32_t res = 0;
        struct t_loom_fid *fid = NULL;
        AWAIT(t_loom_walk_done(op, &res, &fid), "a WALK into a free slot");
        CHECK(res >= 0 && fid, "a WALK into a free slot", res);
        CHECK(n < SLOTS, "the slot table holds at most its slots", n + 1);
        g_held[n++] = fid;
    }
    for (unsigned i = 0; i < n; i++) t_loom_fid_drop(g_held[i]);
    return n;
}

// A service with one fault in it (the fixture's HOSTILE table).
static struct t_loom_target *hostile_target(const char *name) {
    struct t_loom_target *t = NULL;
    int rc = t_loom_target(g_cur, REGISTRY, name, name_len(name), &t);
    CHECK(rc == 0 && t, "TARGET of a hostile service", rc);
    return t;
}

// The CONNECT reaches the fault and stops there, and dropping it must still end
// the session.
static void hold_connect(struct t_loom_fid *sf, const char *name) {
    uint32_t base[STAT_COUNT], v[STAT_COUNT];
    struct t_loom_target *t = hostile_target(name);
    stats(sf, base);
    struct t_loom_op *op;
    int rc = t_loom_connect(t, 0, 0, &op);
    CHECK(rc == 0, "submit a CONNECT the server holds", rc);
    stats_until(sf, FAULTS, base[FAULTS] + 1, v);
    t_loom_op_drop(op);
    stats_until(sf, ENDED, base[ENDED] + 1, v);
    t_loom_target_drop(t);
}

// The handshake's reply is malformed: the CONNECT answers -EIO by itself, and the
// session ends with no abort from here.
static void broken_connect(struct t_loom_fid *sf, const char *name) {
    uint32_t base[STAT_COUNT], v[STAT_COUNT];
    struct t_loom_target *t = hostile_target(name);
    stats(sf, base);
    struct t_loom_op *op;
    int rc = t_loom_connect(t, 1, soon(), &op);
    CHECK(rc == 0, "submit a CONNECT", rc);
    int32_t res = 0;
    struct t_loom_scope *s = NULL;
    AWAIT(t_loom_connect_done(op, &res, &s), "a malformed handshake");
    CHECK(res == -E_IO && !s, "a malformed handshake reply fails the CONNECT", res);
    stats_until(sf, ENDED, base[ENDED] + 1, v);
    CHECK(v[FAULTS] == base[FAULTS] + 1, "the fault was the service's", v[FAULTS]);
    t_loom_target_drop(t);
}

// Past the handshake, the first Tgetattr or Twalk of a WALK is held.
static void hold_walk(struct t_loom_fid *sf, const char *name) {
    uint32_t base[STAT_COUNT], v[STAT_COUNT];
    struct t_loom_target *t = hostile_target(name);
    struct t_loom_scope *s = connect(t);
    stats(sf, base);
    struct t_loom_op *op;
    int rc = t_loom_walk(s, "data", 4, 0, 0, &op);
    CHECK(rc == 0, "submit a WALK the server holds", rc);
    stats_until(sf, FAULTS, base[FAULTS] + 1, v);
    t_loom_op_drop(op);
    t_loom_scope_drop(s);
    stats_until(sf, ENDED, base[ENDED] + 1, v);
    t_loom_target_drop(t);
}

// The Tread's reply is malformed or the peer hung up: the READ answers -EIO by
// itself, and the whole scope goes with it.
static void broken_read(struct t_loom_fid *sf, const char *name) {
    uint32_t base[STAT_COUNT], v[STAT_COUNT];
    struct t_loom_target *t = hostile_target(name);
    struct t_loom_scope *s = connect(t);
    struct t_loom_fid *f = walk(s, "data", 0);
    open_as(f, 0, 0);
    stats(sf, base);
    struct t_loom_op *op;
    int rc = t_loom_read(f, range(4, 0, PAGE), 0, 1, soon(), &op);
    CHECK(rc == 0, "submit a READ", rc);
    int32_t res = 0;
    struct t_fixed_read_result *rr = NULL;
    AWAIT(t_loom_read_done(op, &res, &rr), "a malformed READ reply");
    CHECK(res == -E_IO && !rr, "a malformed READ reply fails the READ", res);
    stats_until(sf, ENDED, base[ENDED] + 1, v);
    CHECK(v[FAULTS] == base[FAULTS] + 1, "the fault was the service's", v[FAULTS]);
    walk_refused(s, "data", -E_NOENT, "no FID reserves under the scope the peer broke");
    t_loom_fid_drop(f);
    t_loom_scope_drop(s);
    t_loom_target_drop(t);
}

static void hostile_peers(struct t_loom_fid *sf, struct t_loom_fid *data) {
    uint32_t base[STAT_COUNT], v[STAT_COUNT];
    struct t_loom_op *op;
    int rc;
    int32_t res;

    say("a peer that holds the handshake");
    struct t_loom_target *t = hostile_target("hold-version");
    stats(sf, base);
    uint64_t deadline = t_loom_now(g_cur) + 3000000000ull;
    rc = t_loom_connect(t, 1, deadline, &op);
    CHECK(rc == 0, "submit a CONNECT the server holds", rc);
    stats_until(sf, FAULTS, base[FAULTS] + 1, v);
    res = 0;
    struct t_loom_scope *none = NULL;
    AWAIT(t_loom_connect_done(op, &res, &none), "the held CONNECT");
    CHECK(res == -E_TIMEDOUT && !none, "the deadline ends the held CONNECT", res);
    stats_until(sf, ENDED, base[ENDED] + 1, v);
    t_loom_target_drop(t);
    hold_connect(sf, "hold-version");
    hold_connect(sf, "hold-attach");
    hold_connect(sf, "torn-version");

    say("a peer that garbles the handshake");
    broken_connect(sf, "garbled-version");
    broken_connect(sf, "wrong-version");

    say("a service that never accepts");
    t = hostile_target("unaccepted");
    stats(sf, base);
    deadline = t_loom_now(g_cur) + 200000000ull;
    rc = t_loom_connect(t, 1, deadline, &op);
    CHECK(rc == 0, "submit a CONNECT nobody accepts", rc);
    res = 0;
    none = NULL;
    AWAIT(t_loom_connect_done(op, &res, &none), "the unaccepted CONNECT");
    CHECK(res == -E_TIMEDOUT && !none, "the deadline ends the unaccepted CONNECT", res);
    rc = t_loom_connect(t, 0, 0, &op);
    CHECK(rc == 0, "submit a CONNECT nobody accepts", rc);
    nap(20);
    t_loom_op_drop(op);
    stats(sf, v);
    CHECK(v[ACCEPTED] == base[ACCEPTED], "the server accepted nothing", v[ACCEPTED]);
    t_loom_target_drop(t);

    say("a peer that holds a walk or an open");
    hold_walk(sf, "hold-getattr");
    hold_walk(sf, "hold-walk");
    t = hostile_target("hold-open");
    struct t_loom_scope *s = connect(t);
    struct t_loom_fid *f = walk(s, "data", 0);
    stats(sf, base);
    rc = t_loom_open(f, 0, 0, 0, &op);
    CHECK(rc == 0, "submit an LOPEN the server holds", rc);
    stats_until(sf, FAULTS, base[FAULTS] + 1, v);
    t_loom_op_drop(op);
    t_loom_fid_drop(f);
    t_loom_scope_drop(s);
    stats_until(sf, ENDED, base[ENDED] + 1, v);
    t_loom_target_drop(t);

    say("a peer that tears a reply");
    t = hostile_target("torn-read");
    s = connect(t);
    f = walk(s, "data", 0);
    open_as(f, 0, 0);
    stats(sf, base);
    rc = t_loom_read(f, range(4, 0, PAGE), 0, 0, 0, &op);
    CHECK(rc == 0, "submit a READ the server tears", rc);
    stats_until(sf, FAULTS, base[FAULTS] + 1, v);
    t_loom_op_drop(op);
    t_loom_fid_drop(f);
    t_loom_scope_drop(s);
    stats_until(sf, ENDED, base[ENDED] + 1, v);
    t_loom_target_drop(t);

    say("a peer that garbles a reply or hangs up");
    broken_read(sf, "garbled-read");
    broken_read(sf, "wrong-read");
    broken_read(sf, "hangup-read");

    // None of it touched a sibling scope, and none of it was the client's fault.
    const unsigned char *bytes;
    size_t len;
    struct t_fixed_read_result *rr = read_at(data, range(0, 0, PAGE), 2 * PAGE, &bytes, &len);
    CHECK(len == PAGE && matches(bytes, len, 2 * PAGE), "a scope beside the hostile ones still reads", len);
    t_loom_read_drop(rr);
    stats(sf, v);
    CHECK(v[VIOLATIONS] == 0, "no hostile leg made the client break the protocol", v[VIOLATIONS]);
}

// Waits for a stream to fill the CQ with nothing reaping it.
static void cq_full(void) {
    uint64_t until = soon();
    while (t_loom_backlog(g_cur) < CQ_ENTRIES) {
        CHECK(t_loom_now(g_cur) < until, "the CQ fills while no turn reaps it", t_loom_backlog(g_cur));
        nap(1);
    }
}

// A stream keeps delivering while another scope stalls and is cancelled, and that
// cancel needs no room in a full CQ.
static void independent_scopes(struct t_loom_fid *sf, struct t_loom_target *t) {
    uint32_t base[STAT_COUNT], v[STAT_COUNT];
    struct t_loom_op *op;
    int rc;
    static struct t_loom_service_pool_member members[8];
    for (unsigned i = 0; i < 8; i++) members[i] = range(5, i * SHOT, SHOT);
    struct t_loom_pool *pool = NULL;
    rc = t_loom_pool(g_cur, members, 8, &pool);
    CHECK(rc == 0 && pool, "pool D", rc);

    say("a stream beside a stalled scope");
    struct t_loom_scope *media = connect(t);
    struct t_loom_fid *shots = walk(media, "stream", 0);
    open_as(shots, 0, 0);
    struct t_loom_op *st = stream(shots, pool);
    size_t got = 0, len;
    struct t_ring_payload *p = shot(st);
    const unsigned char *pb = t_loom_payload_bytes(p, &len);
    got = append(got, pb, len);
    give_back(p, "return a lease");
    struct t_loom_scope *stalled = connect(t);
    struct t_loom_fid *held = walk(stalled, "stall", 0);
    open_as(held, 0, 0);
    stats(sf, base);
    rc = t_loom_read(held, range(4, 0, PAGE), 0, 0, 0, &op);
    CHECK(rc == 0, "submit a READ the server holds", rc);
    stats_until(sf, STALLED, base[STALLED] + 1, v);
    p = shot(st);
    pb = t_loom_payload_bytes(p, &len);
    got = append(got, pb, len);
    give_back(p, "a stream delivers while a sibling scope stalls");
    t_loom_op_drop(op);
    t_loom_fid_drop(held);
    t_loom_scope_drop(stalled);
    stats_until(sf, ENDED, base[ENDED] + 1, v);
    CHECK(v[ENDED] == base[ENDED] + 1, "only the stalled scope's session ended", v[ENDED]);
    // drain() fills g_got from its start, so the two shots above go first.
    static unsigned char first[2 * SHOT];
    CHECK(got <= sizeof(first), "two shots fit their copy", got);
    for (size_t i = 0; i < got; i++) ((volatile unsigned char *)first)[i] = ((volatile unsigned char *)g_got)[i];
    size_t rest = drain(st, 0);
    CHECK(got + rest == STREAM_LEN && matches(first, got, 0) && matches(g_got, rest, got),
          "the stream lost no shot to its sibling", got + rest);
    t_loom_op_drop(st);
    t_loom_fid_drop(shots);
    t_loom_scope_drop(media);

    say("a scope aborted with the CQ full");
    stalled = connect(t);
    held = walk(stalled, "stall", 0);
    open_as(held, 0, 0);
    struct t_loom_scope *filler = connect(t);
    shots = walk(filler, "stream", 0);
    open_as(shots, 0, 0);
    stats(sf, base);
    rc = t_loom_read(held, range(4, 0, PAGE), 0, 0, 0, &op);
    CHECK(rc == 0, "submit a READ the server holds", rc);
    stats_until(sf, STALLED, base[STALLED] + 1, v);
    st = stream(shots, pool);
    cq_full();
    t_loom_op_drop(op);
    t_loom_fid_drop(held);
    t_loom_scope_drop(stalled);
    // Long enough for an engine that posted the cancel's terminal past a full CQ.
    nap(100);
    CHECK(t_loom_backlog(g_cur) == CQ_ENTRIES, "a cancel posts nothing past a full CQ", t_loom_backlog(g_cur));
    got = drain(st, 0);
    CHECK(got == STREAM_LEN && matches(g_got, got, 0), "no shot lost behind the full CQ", got);
    stats_until(sf, ENDED, base[ENDED] + 1, v);
    t_loom_op_drop(st);
    t_loom_fid_drop(shots);
    t_loom_scope_drop(filler);
    t_loom_pool_drop(pool);
}

// The bounds an honest program can reach, each refused at its own gate.
static void quotas(struct t_loom_fid *sf, struct t_loom_target *t, struct t_loom_service_pool_member pool_a) {
    uint32_t base[STAT_COUNT], v[STAT_COUNT];
    int rc;
    say("a pool past its bound, and one over a member in use");
    static struct t_loom_service_pool_member many[POOL_MEMBERS + 1];
    for (unsigned i = 0; i < POOL_MEMBERS + 1; i++) many[i] = range(5, i * 16, 16);
    struct t_loom_pool *pool = NULL;
    rc = t_loom_pool(g_cur, many, POOL_MEMBERS + 1, &pool);
    CHECK(rc == -E_INVAL && !pool, "a pool of more than the most members", rc);
    rc = t_loom_pool(g_cur, &pool_a, 1, &pool);
    CHECK(rc == -E_BUSY && !pool, "a pool over a member a live pool holds", rc);

    say("the bound on requests in flight");
    struct t_loom_scope *s = connect(t);
    struct t_loom_fid *held = walk(s, "stall", 0);
    open_as(held, 0, 0);
    stats(sf, base);
    static struct t_loom_op *reads[2 * CQ_ENTRIES + 1];
    unsigned n = 0;
    for (;;) {
        rc = t_loom_read(held, range(4, n * 16u, 16), 0, 0, 0, &reads[n]);
        if (rc != 0) {
            CHECK(rc == -E_NOSPC, "the first read past the bound refuses at submit", rc);
            break;
        }
        n++;
        CHECK(n <= 2 * CQ_ENTRIES, "requests in flight are bounded", n);
    }
    t_putstr("loom-c-probe: ");
    put_dec((long)n);
    t_putstr(" reads in flight\n");
    CHECK(n >= CQ_ENTRIES - 1, "the bound admits a full CQ's requests", n);
    for (unsigned i = 0; i < n; i++) t_loom_op_drop(reads[i]);
    t_loom_fid_drop(held);
    t_loom_scope_drop(s);
    stats_until(sf, ENDED, base[ENDED] + 1, v);
}

static int run(void) {
    unsigned char b = 0;
    long n = t_read(NOTIFY, &b, 1);
    CHECK(n == 1 && b == 'R', "the fixture has posted its service", n);
    g_kernel = t_loom_kernel_native();
    static const size_t lengths[6] = { PAGE, PAGE, PAGE, PAGE, PAGE, PAGE };
    // 0: fixed data and sink I/O; 1: pool A; 2: stats; 3: pool B; 4: stalled reads.
    int rc = T_LOOM_RING_SETUP(&g_store, &g_kernel, ENTRIES, lengths, 6, &g_ring);
    if (rc == -E_OPNOTSUPP) {
        say("default image: setup refuses -EOPNOTSUPP");
        return PASS_DEFAULT;
    }
    CHECK(rc == 0 && g_ring, "a private ring in the qualification image", rc);
    g_cur = g_ring;

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
    stats(sf, v);
    CHECK(v[WALKS] == base[WALKS] + 1 && v[VIOLATIONS] == base[VIOLATIONS],
          "the server answered that Twalk itself", v[WALKS]);
    struct t_loom_fid *data = walk(s1, "data", 0);
    open_as(data, 1, -E_ACCES);
    open_as(data, 0, 0);
    struct t_loom_fid *sink = walk(s1, "sink", 0);
    open_as(sink, 2, 0);

    say("a walk from a fid below the root");
    struct t_loom_fid *dir = walk(s1, "dir", 0);
    struct t_loom_op *op;
    rc = t_loom_walk_fid(dir, "leaf", 4, 1, soon(), &op);
    CHECK(rc == 0, "submit a WALK from a directory fid", rc);
    int32_t res = -1;
    struct t_loom_fid *leaf = NULL;
    AWAIT(t_loom_walk_done(op, &res, &leaf), "a WALK from a directory fid");
    CHECK(res >= 0 && leaf, "a WALK from a directory fid", res);
    open_as(leaf, 0, 0);
    const unsigned char *bytes;
    size_t len;
    struct t_fixed_read_result *rr = read_at(leaf, range(0, 0, PAGE), 0, &bytes, &len);
    CHECK(len == LEAF_LEN && matches(bytes, len, 0), "the file below a directory", len);
    t_loom_read_drop(rr);

    say("a walk from an opened fid");
    // The fixture refuses this WALK with the same errno, so only its counters
    // tell the kernel's refusal at submit from the server's.
    struct t_loom_fid *odir = walk(s1, "dir", 0);
    open_as(odir, 0, 0);
    stats(sf, base);
    rc = t_loom_walk_fid(odir, "leaf", 4, 1, soon(), &op);
    CHECK(rc == 0, "submit a WALK from an opened fid", rc);
    res = 0;
    struct t_loom_fid *child = NULL;
    AWAIT(t_loom_walk_done(op, &res, &child), "a WALK from an opened fid");
    CHECK(res == -E_BADF && !child, "the kernel refuses a WALK from an opened fid", res);
    stats(sf, v);
    uint32_t reached = (v[WALKS] - base[WALKS]) + (v[GETATTRS] - base[GETATTRS]) + (v[VIOLATIONS] - base[VIOLATIONS]);
    CHECK(reached == 0, "no Tgetattr or Twalk reached the server", reached);

    say("fixed reads and writes");
    rr = read_at(data, range(0, 0, PAGE), 0, &bytes, &len);
    CHECK(len == PAGE && matches(bytes, len, 0), "a fixed READ's bytes", len);
    t_loom_read_drop(rr);
    rr = read_at(data, range(0, 0, PAGE), 3 * PAGE, &bytes, &len);
    CHECK(len == DATA_LEN - 3 * PAGE && matches(bytes, len, 3 * PAGE), "a short READ at the end", len);
    t_loom_read_drop(rr);
    static unsigned char msg[100];
    volatile unsigned char *m = msg;
    for (int i = 0; i < 100; i++) m[i] = (unsigned char)(i * 7 + 1);
    rc = t_loom_write(sink, range(0, 0, 100), 0, msg, 100, 1, soon(), &op);
    CHECK(rc == 0, "submit a WRITE", rc);
    res = -1;
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
    CHECK(t_loom_backlog(g_ring) == 0, "the CQ is empty before the stream", t_loom_backlog(g_ring));
    st = stream(second, pool_b);
    // Submitting entered the SQE; the executor runs without a turn.
    uint64_t until = soon();
    while (t_loom_backlog(g_ring) < CQ_ENTRIES) {
        CHECK(t_loom_now(g_ring) < until, "the CQ fills while no turn reaps it", t_loom_backlog(g_ring));
        nap(1);
    }
    // Long enough for an engine that ignored the full CQ to post past it.
    nap(100);
    CHECK(t_loom_backlog(g_ring) == CQ_ENTRIES, "a full CQ holds exactly its entries", t_loom_backlog(g_ring));
    int64_t routed = t_loom_turn(g_ring, 0, 0);
    CHECK(routed == CQ_ENTRIES, "one turn routes the whole CQ", routed);
    got = drain(st, 0);
    CHECK(got == STREAM_LEN && matches(g_got, got, 0), "no shot lost behind the full CQ", got);
    t_loom_op_drop(st);

    say("a deadline aborts its scope");
    struct t_loom_scope *s2 = connect(t);
    struct t_loom_fid *near2 = walk(s2, "data", 0);
    open_as(near2, 0, 0);
    struct t_loom_fid *stall2 = walk(s2, "stall", 0);
    open_as(stall2, 0, 0);
    stats(sf, base);
    uint64_t deadline = t_loom_now(g_ring) + 3000000000ull;
    rc = t_loom_read(stall2, range(4, 0, PAGE), 0, 1, deadline, &op);
    CHECK(rc == 0, "submit a READ the server holds", rc);
    stats_until(sf, STALLED, base[STALLED] + 1, v);
    rr = NULL;
    res = 0;
    AWAIT(t_loom_read_done(op, &res, &rr), "the held READ");
    CHECK(res == -E_TIMEDOUT && !rr, "the deadline ends the held READ", res);
    // The abort latched the scope out of READY: admission refuses a sibling's
    // operation, and no FID reserves under the scope.
    read_refused(near2, -E_CANCELED, "the deadline aborted the open sibling too");
    walk_refused(s2, "data", -E_NOENT, "no FID reserves under the scope the deadline aborted");
    stats_until(sf, ENDED, base[ENDED] + 1, v);

    say("cancellation by drop");
    struct t_loom_scope *s3 = connect(t);
    struct t_loom_fid *near3 = walk(s3, "data", 0);
    open_as(near3, 0, 0);
    struct t_loom_fid *stall3 = walk(s3, "stall", 0);
    open_as(stall3, 0, 0);
    stats(sf, base);
    rc = t_loom_read(stall3, range(4, 0, PAGE), 0, 0, 0, &op);
    CHECK(rc == 0, "submit a READ the server holds", rc);
    stats_until(sf, STALLED, base[STALLED] + 1, v);
    t_loom_op_drop(op);
    read_refused(near3, -E_CANCELED, "dropping a pending READ aborted the open sibling too");
    walk_refused(s3, "data", -E_NOENT, "no FID reserves under the scope the drop aborted");
    stats_until(sf, ENDED, base[ENDED] + 1, v);
    rr = read_at(data, range(0, 0, PAGE), PAGE, &bytes, &len);
    CHECK(len == PAGE && matches(bytes, len, PAGE), "a scope beside the aborted ones still reads", len);
    t_loom_read_drop(rr);

    say("a stream dropped before its end");
    struct t_loom_scope *s4 = connect(t);
    struct t_loom_fid *fourth = walk(s4, "stream", 0);
    open_as(fourth, 0, 0);
    stats(sf, base);
    st = stream(fourth, pool_a);
    struct t_ring_payload *p = shot(st);
    t_loom_op_drop(st);
    const unsigned char *pb = t_loom_payload_bytes(p, &len);
    CHECK(len == SHOT && matches(pb, len, 0), "a lease outlives its dropped stream", len);
    give_back(p, "a dropped stream's lease returns");
    stats_until(sf, ENDED, base[ENDED] + 1, v);
    // Pool A has two members: a stream drains only if both came back.
    struct t_loom_fid *fifth = walk(s1, "stream", 0);
    open_as(fifth, 0, 0);
    st = stream(fifth, pool_a);
    got = drain(st, 1);
    CHECK(got == STREAM_LEN && matches(g_got, got, 0), "the dropped stream's pool streams again", got);
    t_loom_op_drop(st);

    say("an explicit abort");
    struct t_loom_scope *s5 = connect(t);
    struct t_loom_fid *near5 = walk(s5, "data", 0);
    open_as(near5, 0, 0);
    stats(sf, base);
    t_loom_scope_abort(s5);
    read_refused(near5, -E_CANCELED, "an explicit abort refuses the open fid");
    walk_refused(s5, "data", -E_NOENT, "no FID reserves under the scope abort ended");
    stats_until(sf, ENDED, base[ENDED] + 1, v);

    say("dropped fids, scopes, pools and targets are reaped");
    struct t_loom_target *t2 = NULL;
    rc = t_loom_target(g_ring, REGISTRY, "fixture", 7, &t2);
    CHECK(rc == 0 && t2, "a second TARGET of the fixture", rc);
    // Held: two targets; s1 and its nine fids; pools A and B; s2, s3 and s5
    // with their two, two and one fids; s4 and its one.
    const unsigned held = 2 + 1 + 9 + 2 + 3 + 3 + 2 + 2;
    stats(sf, base);
    unsigned f1 = fill(s1);
    CHECK(f1 == SLOTS - held, "the free slots are those no live handle holds", f1);
    stats_until(sf, CLUNKS, base[CLUNKS] + f1, v);
    struct t_loom_fid *gone[] = { near2, stall2, near3, stall3, fourth, near5 };
    for (unsigned i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) t_loom_fid_drop(gone[i]);
    t_loom_scope_drop(s2);
    t_loom_scope_drop(s3);
    t_loom_scope_drop(s4);
    t_loom_scope_drop(s5);
    t_loom_pool_drop(pool_b);
    t_loom_target_drop(t2);
    const unsigned dropped = 12;
    unsigned f2 = fill(s1);
    CHECK(f2 == f1 + dropped, "every dropped handle's slot came back", f2);
    stats_until(sf, CLUNKS, base[CLUNKS] + f1 + f2, v);
    CHECK(v[CLUNKS] == base[CLUNKS] + f1 + f2 && v[VIOLATIONS] == base[VIOLATIONS],
          "a Tclunk for each live scope's dropped fid, and no other", v[CLUNKS]);

    hostile_peers(sf, data);
    independent_scopes(sf, t);
    quotas(sf, t, range(1, 0, SHOT));

    say("closing a ring with leases held");
    rc = T_LOOM_RING_SETUP(&g_store2, &g_kernel, ENTRIES, lengths, 5, &g_ring2);
    CHECK(rc == 0 && g_ring2, "a second private ring", rc);
    struct t_loom_target *target2 = NULL;
    rc = t_loom_target(g_ring2, REGISTRY, "fixture", 7, &target2);
    CHECK(rc == 0 && target2, "TARGET from the second ring", rc);
    stats(sf, base);
    g_cur = g_ring2;
    struct t_loom_scope *s6 = connect(target2);
    struct t_loom_fid *third = walk(s6, "stream", 0);
    open_as(third, 0, 0);
    struct t_loom_fid *data6 = walk(s6, "data", 0);
    open_as(data6, 0, 0);
    struct t_loom_pool *pool_c = NULL;
    rc = t_loom_pool(g_ring2, two, 2, &pool_c);
    CHECK(rc == 0 && pool_c, "pool C", rc);
    st = stream(third, pool_c);
    struct t_ring_payload *p1 = shot(st);
    struct t_ring_payload *p2 = shot(st);
    rr = read_at(data6, range(0, 0, PAGE), 0, &bytes, &len);
    t_loom_close(g_ring2);
    g_cur = g_ring;
    size_t l1, l2;
    const unsigned char *b1 = t_loom_payload_bytes(p1, &l1);
    const unsigned char *b2 = t_loom_payload_bytes(p2, &l2);
    CHECK(l1 == SHOT && matches(b1, l1, 0) && l2 == SHOT && matches(b2, l2, SHOT) &&
          len == PAGE && matches(bytes, len, 0), "leases keep their bytes after the ring closes", l1);
    struct t_loom_target *late = NULL;
    rc = t_loom_target(g_ring2, REGISTRY, "fixture", 7, &late);
    CHECK(rc == -E_CANCELED && !late, "a closed ring admits no TARGET", rc);
    op = NULL;
    rc = t_loom_walk(s6, "data", 4, 0, 0, &op);
    CHECK(rc == -E_CANCELED && !op, "a closed ring admits no WALK", rc);
    // The close reached the kernel: the server saw the session end while this
    // program still runs.
    stats_until(sf, ENDED, base[ENDED] + 1, v);
    t_loom_payload_drop(p1);
    t_loom_payload_drop(p2);
    t_loom_read_drop(rr);
    t_loom_op_drop(st);
    t_loom_fid_drop(data6);
    t_loom_fid_drop(third);
    t_loom_scope_drop(s6);
    t_loom_pool_drop(pool_c);
    t_loom_target_drop(target2);
    t_loom_ring_drop(g_ring2);
    CHECK(t_loom_idle(&g_store2.base) == 1, "every handle dropped leaves the closed ring's storage idle", 0);

    say("retiring a ring");
    stats(sf, v);
    CHECK(v[VIOLATIONS] == 0, "the fixture saw no protocol violation", v[VIOLATIONS]);
    t_loom_retire(g_ring);
    until = soon();
    while (!t_loom_retired(g_ring)) {
        CHECK(t_loom_now(g_ring) < until, "every scope retires and every terminal is routed", 0);
        int64_t turned = t_loom_turn(g_ring, 1, t_loom_now(g_ring) + 10000000ull);
        CHECK(turned >= 0, "a turn while the ring retires", turned);
    }
    rc = t_loom_target(g_ring, REGISTRY, "fixture", 7, &late);
    CHECK(rc == -E_CANCELED && !late, "a retired ring admits no TARGET", rc);
    // The fixture serves only this program: once the ring retired, it holds
    // no session.
    n = t_write(CONTROL, "Q", 1);
    CHECK(n == 1, "ask the fixture to answer once it holds no session", n);
    struct pollfd idle = { .fd = NOTIFY, .events = POLLIN, .revents = 0 };
    n = t_poll(&idle, 1, 5000);
    CHECK(n == 1, "the fixture holds no session once the ring retired", n);
    n = t_read(NOTIFY, &b, 1);
    CHECK(n == 1 && b == 'Z', "the fixture's answer", n);
    struct t_loom_fid *fids[] = { fifth, second, first, odir, leaf, dir, sink, data, sf };
    for (unsigned i = 0; i < sizeof(fids) / sizeof(fids[0]); i++) t_loom_fid_drop(fids[i]);
    t_loom_scope_drop(s1);
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
