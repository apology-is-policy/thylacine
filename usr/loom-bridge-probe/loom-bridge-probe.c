// loom-bridge-probe -- the EL0 leg of the private Loom syscall bridge
// (docs/ASYNC-SERVICE-ABI.md, "The EL0 probe"). The kernel test
// loom.bridge_probe spawns it and requires PASS_DEFAULT or PASS_QUAL according to
// its own compile-time lever, so a probe that guesses its mode wrong fails.
//
// Fault pages come from one reservation cut into RW | R | none and never
// unmapped: a hole could be filled by the very ring a setup creates, and the
// copyout meant to fault would then succeed.

#include <stdint.h>
#include <stddef.h>
#include <thyla/syscall.h>
#include <thyla/poll.h>
#include <thyla/loom.h>

#define PASS_DEFAULT 70
#define PASS_QUAL    71

// The kernel's errno registry (kernel/include/thylacine/errno.h).
#define E_NOENT      2
#define E_BADF       9
#define E_FAULT     14
#define E_BUSY      16
#define E_INVAL     22
#define E_OPNOTSUPP 95

#define PAGE      4096ul
#define KERNEL_VA 0xffff000000001000ul
#define P         T_LOOM_SETUP_PRIVATE_SERVICE
#define B         T_LOOM_SETUP_SERVICE_BUFFERS

static struct t_loom_params g_params;
static uint8_t g_rec[sizeof(struct t_loom_service_pool_create)] __attribute__((aligned(8)));
static uint8_t *g_p0, *g_p1, *g_p2;

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
    t_putstr("loom-bridge-probe: FAIL line ");
    put_dec(line);
    t_putstr(": ");
    t_putstr(why);
    t_putstr(" (got ");
    put_dec(got);
    t_putstr(")\n");
    t_exits(1);
}
#define CHECK(cond, why, got) do { if (!(cond)) fail(__LINE__, (why), (long)(got)); } while (0)

// Byte-wise so no libc call is emitted; native C links none.
static void fill(void *p, uint8_t v, size_t n) {
    volatile uint8_t *b = p;
    for (size_t i = 0; i < n; i++) b[i] = v;
}
static void put16(void *p, size_t off, uint16_t v) {
    volatile uint8_t *b = (uint8_t *)p + off;
    for (int i = 0; i < 2; i++) b[i] = (uint8_t)(v >> (8 * i));
}
static void put32(void *p, size_t off, uint32_t v) {
    volatile uint8_t *b = (uint8_t *)p + off;
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
}
static void put64(void *p, size_t off, uint64_t v) {
    volatile uint8_t *b = (uint8_t *)p + off;
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
}
static void copy_params(struct t_loom_params *to, const struct t_loom_params *from) {
    volatile uint8_t *d = (uint8_t *)to;
    const volatile uint8_t *f = (const uint8_t *)from;
    for (size_t i = 0; i < sizeof(*to); i++) d[i] = f[i];
}
static uint32_t get32(const void *p, size_t off) {
    const volatile uint8_t *b = (const uint8_t *)p + off;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)b[i] << (8 * i);
    return v;
}
static uint64_t get64(const void *p, size_t off) {
    const volatile uint8_t *b = (const uint8_t *)p + off;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)b[i] << (8 * i);
    return v;
}

static void record(void *r, uint32_t size) {
    fill(r, 0, size);
    put32(r, 0, size);
    put16(r, 4, (uint16_t)T_LOOM_SERVICE_ABI_VERSION);
}
static void ref_into(void *r, size_t off, uint32_t slot, uint64_t incarnation) {
    put32(r, off, slot);
    put32(r, off + 4, 0);
    put64(r, off + 8, incarnation);
}

static void p1_writable(int rw) {
    long rc = t_burrow_protect((unsigned long)g_p1, PAGE,
                               rw ? T_BURROW_PROT_READ | T_BURROW_PROT_WRITE
                                  : T_BURROW_PROT_READ, 0);
    CHECK(rc == 0, "protect the middle fault page", rc);
}

static void fault_pages(void) {
    long base = t_burrow_reserve(3 * PAGE, T_BURROW_PROT_READ | T_BURROW_PROT_WRITE, 0);
    CHECK(base > 0, "reserve three fault pages", base);
    g_p0 = (uint8_t *)base;
    g_p1 = g_p0 + PAGE;
    g_p2 = g_p1 + PAGE;
    long rc = t_burrow_protect((unsigned long)g_p2, PAGE, T_BURROW_PROT_NONE, 0);
    CHECK(rc == 0, "protect the last fault page to none", rc);
    p1_writable(0);
}

// Callers pre-fill params with 0xA5: the kernel reads only flags, so every other
// byte read back as zero was written by the kernel.
static long setup_at(void *params, uint32_t flags, unsigned long entries) {
    put32(params, 0, flags);
    return t_loom_setup(entries, params);
}

static int regions_disjoint(const struct t_loom_params *q, int receipts) {
    uint64_t at[5] = { q->hdr_off, q->sq_array_off, q->sqe_off, q->cqe_off,
                       receipts ? q->_resv1[0] : 0 };
    uint64_t len[5] = { sizeof(struct t_loom_ring_hdr), q->sq_array_size, q->sqe_size,
                        q->cqe_size, receipts ? q->_resv1[1] : 0 };
    int n = receipts ? 5 : 4;
    for (int i = 0; i < n; i++) {
        if (at[i] % 64 || at[i] + len[i] > q->ring_size) return 0;
        for (int j = 0; j < i; j++)
            if (at[i] < at[j] + len[j] && at[j] < at[i] + len[i]) return 0;
    }
    return 1;
}

// Every geometry field is the kernel's own write over the 0xA5 fill, so the
// mirror's offsets are checked against it, not against a constant. flags is the
// probe's own IN word, so its check pins the grant, not the copyout. A fresh
// ring's header is zeroed memory: its zero words (hdr.flags and overflow above
// all, which no step here makes non-zero) rest on the static asserts and
// tools/check-loom-service-abi.py.
static volatile struct t_loom_ring_hdr *check_ring(const struct t_loom_params *q,
                                                   uint32_t flags, uint32_t entries) {
    int receipts = (flags & B) != 0;
    CHECK(q->flags == flags, "params echo the granted flags", q->flags);
    CHECK(q->sq_entries == entries && q->cq_entries == 2 * entries,
          "params carry the ring's entries", q->cq_entries);
    CHECK(q->ring_size && q->ring_size % PAGE == 0, "a page-rounded ring", q->ring_size);
    CHECK(q->ring_va && q->ring_va % PAGE == 0, "a page-aligned ring address", q->ring_va);
    CHECK(q->sq_array_size == entries * 4u &&
          q->sqe_size == entries * sizeof(struct t_loom_sqe) &&
          q->cqe_size == 2 * entries * sizeof(struct t_loom_cqe),
          "region sizes match the mirrored records", q->sqe_size);
    CHECK(!q->_resv0 && !q->_resv1[3], "reserved params are zero", q->_resv0);
    if (receipts)
        CHECK(q->_resv1[1] == 2 * entries * sizeof(struct t_loom_service_buffer_receipt) &&
              q->_resv1[2] == sizeof(struct t_loom_service_buffer_receipt),
              "params carry the receipt geometry", q->_resv1[1]);
    else
        CHECK(!q->_resv1[0] && !q->_resv1[1] && !q->_resv1[2],
              "no receipt geometry without SERVICE_BUFFERS", q->_resv1[1]);
    CHECK(regions_disjoint(q, receipts), "regions aligned, inside the ring, disjoint", 0);
    volatile struct t_loom_ring_hdr *h = (void *)(uintptr_t)(q->ring_va + q->hdr_off);
    CHECK(!h->sq_head && !h->sq_tail && h->sq_mask == entries - 1 &&
          h->sq_entries == entries, "the SQ header words", h->sq_entries);
    CHECK(!h->cq_head && !h->cq_tail && h->cq_mask == 2 * entries - 1 &&
          h->cq_entries == 2 * entries, "the CQ header words", h->cq_entries);
    CHECK(!(h->flags & ~T_LOOM_RING_SQ_NEED_WAKEUP) && !h->dropped && !h->overflow,
          "the runtime header words start clear", h->dropped);
    return h;
}

static void close_ring(long fd, const struct t_loom_params *q) {
    long rc = t_close(fd);
    CHECK(rc == 0, "close the ring", rc);
    rc = t_burrow_detach(q->ring_va, q->ring_size);
    CHECK(rc == 0, "detach the ring", rc);
}

// Neither route is chosen before the flags are read, in either image.
static void pre_route_refusals(void) {
    long rc = t_loom_setup(2, (struct t_loom_params *)KERNEL_VA);
    CHECK(rc == -1, "params at a kernel address: the legacy -1", rc);
    rc = t_loom_setup(2, (struct t_loom_params *)(void *)g_p2);
    CHECK(rc == -1, "unreadable params: the legacy -1", rc);
}

static int run_default(long fd0, uint64_t va0) {
    static const uint32_t refused[] = { P, P | B, B, P | T_LOOM_SETUP_SQPOLL };
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        fill(&g_params, 0xA5, sizeof(g_params));
        long rc = setup_at(&g_params, refused[i], 2);
        CHECK(rc == -E_OPNOTSUPP, "the default image refuses a private setup", rc);
        for (size_t b = 4; b < sizeof(g_params); b++)
            CHECK(((uint8_t *)&g_params)[b] == 0xA5, "a refused setup writes no params", b);
    }
    pre_route_refusals();
    record(g_rec, sizeof(g_rec));
    for (unsigned long op = 2; op <= 9; op++)
        CHECK(t_loom_register(999, op, g_rec, 1) == -E_OPNOTSUPP,
              "the default image refuses a private register op before any lookup", op);

    fill(&g_params, 0xA5, sizeof(g_params));
    long fd = setup_at(&g_params, 0, 2);
    CHECK(fd == fd0 && g_params.ring_va == va0,
          "the refusals left no handle and no mapping", fd);
    check_ring(&g_params, 0, 2);
    for (unsigned long op = 2; op <= 9; op++)
        CHECK(t_loom_register(fd, op, g_rec, 1) == -E_OPNOTSUPP,
              "the default image refuses a private op on an ordinary ring", op);
    CHECK(t_loom_register(fd, 10, g_rec, 1) == -E_INVAL, "an unknown register op is -EINVAL", 0);
    close_ring(fd, &g_params);
    // A client tells a ring closed under it from its own error by -EBADF.
    CHECK(t_loom_enter(fd, 0, 0, 0) == -E_BADF, "ENTER on a closed ring's number is -EBADF", 0);
    CHECK(t_loom_register(fd, 0, 0, 0) == -E_BADF,
          "REGISTER on a closed ring's number is -EBADF", 0);
    t_putstr("loom-bridge-probe: default image refuses every private route\n");
    return PASS_DEFAULT;
}

static void qual_enter_and_cq(long fd, volatile struct t_loom_ring_hdr *h,
                              const struct t_loom_params *q) {
    CHECK(t_loom_enter(fd, 0, 1, T_LOOM_ENTER_NONBLOCK) == -E_INVAL,
          "ENTER with min_complete refused", 0);
    CHECK(t_loom_enter(fd, 0, 0, T_LOOM_ENTER_GETEVENTS) == -E_INVAL,
          "ENTER with GETEVENTS refused", 0);
    CHECK(t_loom_enter(fd, 0, 0, 0x80) == -E_INVAL, "ENTER with an unknown flag refused", 0);
    CHECK(t_loom_enter(fd, 0, 0, T_LOOM_ENTER_NONBLOCK) == 0, "a zero-submit kick", 0);
    CHECK(t_loom_enter(fd, 0, 0, 0) == 0, "a zero-submit kick without NONBLOCK", 0);

    struct pollfd pfd = { .fd = (int)fd, .events = POLLIN, .revents = 0 };
    long rc = t_poll(&pfd, 1, 0);
    CHECK(rc == 0, "no CQE yet, so poll reports nothing", rc);

    // Two SQEs the decoder rejects for different reasons, so each CQE's errno
    // shows which mirrored field the kernel read.
    uint8_t *sqes = (uint8_t *)(uintptr_t)(q->ring_va + q->sqe_off);
    volatile uint32_t *sq = (void *)(uintptr_t)(q->ring_va + q->sq_array_off);
    fill(sqes, 0, 2 * sizeof(struct t_loom_sqe));
    struct t_loom_sqe *s = (struct t_loom_sqe *)(void *)sqes;
    s[0].opcode = 0xEE;
    s[0]._resv1[1] = 1;
    s[0].user_data = 0x10AB;
    s[1].opcode = T_LOOM_OP_NOP;
    s[1].user_data = 0x20CD;
    sq[0] = 0;
    sq[1] = 1;
    // Receipts start zeroed; dirty them so only the kernel's post can clear them.
    uint8_t *receipts = (uint8_t *)(uintptr_t)(q->ring_va + q->_resv1[0]);
    fill(receipts, 0xFF, 2 * sizeof(struct t_loom_service_buffer_receipt));
    __atomic_store_n(&h->sq_tail, 2, __ATOMIC_RELEASE);
    rc = t_loom_enter(fd, 2, 0, T_LOOM_ENTER_NONBLOCK);
    CHECK(rc == 2, "ENTER consumes both SQEs", rc);
    CHECK(h->sq_head == 2, "sq_head published past both", h->sq_head);

    pfd.revents = 0;
    rc = t_poll(&pfd, 1, 1000);
    CHECK(rc == 1 && (pfd.revents & POLLIN), "poll reports the posted CQEs", rc);
    uint32_t tail = __atomic_load_n(&h->cq_tail, __ATOMIC_ACQUIRE);
    CHECK(tail == 2, "two CQEs posted", tail);
    const volatile struct t_loom_cqe *c = (void *)(uintptr_t)(q->ring_va + q->cqe_off);
    CHECK(c[0].user_data == 0x10AB && c[0].result == -E_OPNOTSUPP && !c[0].flags,
          "an unknown opcode completes -EOPNOTSUPP with its token", c[0].result);
    CHECK(c[1].user_data == 0x20CD && c[1].result == -E_INVAL && !c[1].flags,
          "a missing incarnation completes -EINVAL with its token", c[1].result);
    for (size_t b = 0; b < 2 * sizeof(struct t_loom_service_buffer_receipt); b++)
        CHECK(!receipts[b], "a rejected CQE's post clears its receipt", b);
    __atomic_store_n(&h->cq_head, tail, __ATOMIC_RELEASE);
    pfd.revents = 0;
    rc = t_poll(&pfd, 1, 0);
    CHECK(rc == 0, "a reaped CQ no longer polls ready", rc);

    // 0xEE's -EOPNOTSUPP would also come from an opcode read as zero; a CLUNK
    // with an offset is refused by the CLUNK arm alone.
    fill(sqes, 0, sizeof(struct t_loom_sqe));
    s[0].opcode = T_LOOM_OP_CLUNK;
    s[0].offset = 1;
    s[0]._resv1[1] = 1;
    s[0].user_data = 0x30EF;
    sq[0] = 0;
    fill(receipts + 2 * sizeof(struct t_loom_service_buffer_receipt), 0xFF,
         sizeof(struct t_loom_service_buffer_receipt));
    __atomic_store_n(&h->sq_tail, 3, __ATOMIC_RELEASE);
    rc = t_loom_enter(fd, 1, 0, T_LOOM_ENTER_NONBLOCK);
    CHECK(rc == 1 && h->sq_head == 3, "ENTER consumes the CLUNK", rc);
    tail = __atomic_load_n(&h->cq_tail, __ATOMIC_ACQUIRE);
    CHECK(tail == 3, "the CLUNK posts one CQE", tail);
    CHECK(c[2].user_data == 0x30EF && c[2].result == -E_INVAL && !c[2].flags,
          "a CLUNK with an offset completes -EINVAL: the opcode byte was read", c[2].result);
    for (size_t b = 0; b < sizeof(struct t_loom_service_buffer_receipt); b++)
        CHECK(!receipts[2 * sizeof(struct t_loom_service_buffer_receipt) + b],
              "the CLUNK's post clears its receipt", b);
    __atomic_store_n(&h->cq_head, tail, __ATOMIC_RELEASE);

    sq[1] = 7;
    __atomic_store_n(&h->sq_tail, 4, __ATOMIC_RELEASE);
    rc = t_loom_enter(fd, 1, 0, T_LOOM_ENTER_NONBLOCK);
    CHECK(rc == 1 && h->dropped == 1 && h->sq_head == 4, "an out-of-range index is dropped", rc);
    CHECK(__atomic_load_n(&h->cq_tail, __ATOMIC_ACQUIRE) == 3, "a dropped SQE posts nothing", 0);
}

static uint64_t reserve_scope(long fd, uint32_t *slot) {
    record(g_rec, sizeof(struct t_loom_service_reserve));
    put32(g_rec, 8, T_LOOM_SERVICE_SCOPE);
    long rc = t_loom_register(fd, T_LOOM_REGISTER_SERVICE_SLOT, g_rec, 1);
    CHECK(rc == 0, "a scope reservation", rc);
    *slot = get32(g_rec, 32);
    uint64_t incarnation = get64(g_rec, 40);
    CHECK(incarnation, "the reservation's result is copied out", 0);
    return incarnation;
}

static void control(long fd, unsigned long op, uint32_t slot, uint64_t incarnation,
                    long want, const char *why) {
    record(g_rec, sizeof(struct t_loom_service_control));
    ref_into(g_rec, 8, slot, incarnation);
    long rc = t_loom_register(fd, op, g_rec, 1);
    CHECK(rc == want, why, rc);
}

static void qual_register(long fd) {
    CHECK(t_loom_register(fd, T_LOOM_REGISTER_HANDLES, NULL, 0) == -E_OPNOTSUPP,
          "HANDLES on a private ring refused", 0);
    uint32_t s1, s3;
    uint64_t i1 = reserve_scope(fd, &s1);

    // A read-only record copies in and then cannot take its result: the
    // reservation is burned. A record running into an unreadable page faults on
    // the copy in, before anything is reserved.
    p1_writable(1);
    record(g_p1 + 128, sizeof(struct t_loom_service_reserve));
    put32(g_p1 + 128, 8, T_LOOM_SERVICE_SCOPE);
    uint8_t *straddle = g_p2 - 24;
    record(straddle, 24);
    put32(straddle, 0, sizeof(struct t_loom_service_reserve));
    put32(straddle, 8, T_LOOM_SERVICE_SCOPE);
    p1_writable(0);
    long rc = t_loom_register(fd, T_LOOM_REGISTER_SERVICE_SLOT, g_p1 + 128, 1);
    CHECK(rc == -E_FAULT, "a read-only record's result copyout faults", rc);
    rc = t_loom_register(fd, T_LOOM_REGISTER_SERVICE_SLOT, straddle, 1);
    CHECK(rc == -E_FAULT, "a record running into an unreadable page faults", rc);
    uint64_t i3 = reserve_scope(fd, &s3);
    CHECK(i3 == i1 + 2, "the copyout fault burned one incarnation, the copyin fault none", i3 - i1);

    record(g_rec, sizeof(struct t_loom_service_reserve));
    put32(g_rec, 8, T_LOOM_SERVICE_SCOPE);
    CHECK(t_loom_register(fd, T_LOOM_REGISTER_SERVICE_SLOT, g_rec, 0) == -E_INVAL, "nargs 0", 0);
    CHECK(t_loom_register(fd, T_LOOM_REGISTER_SERVICE_SLOT, g_rec, 2) == -E_INVAL, "nargs 2", 0);
    CHECK(t_loom_register(fd, T_LOOM_REGISTER_SERVICE_SLOT, NULL, 1) == -E_INVAL, "a null record", 0);
    CHECK(t_loom_register(fd, T_LOOM_REGISTER_SERVICE_SLOT, (void *)KERNEL_VA, 1) == -E_FAULT,
          "a record at a kernel address", 0);
    CHECK(t_loom_register(999, T_LOOM_REGISTER_SERVICE_SLOT, g_rec, 1) == -E_BADF, "a closed fd", 0);
    long rd = -1, wr = -1;
    rc = t_pipe(&rd, &wr);
    CHECK(rc == 0, "a pipe", rc);
    CHECK(t_loom_register(rd, T_LOOM_REGISTER_SERVICE_SLOT, g_rec, 1) == -E_BADF,
          "a fd that is not a ring", 0);
    t_close(rd);
    t_close(wr);
    put16(g_rec, 4, 2);
    CHECK(t_loom_register(fd, T_LOOM_REGISTER_SERVICE_SLOT, g_rec, 1) == -E_INVAL,
          "an unknown record version", 0);
    CHECK(t_loom_register(fd, 10, g_rec, 1) == -1, "op 10 is not a private op", 0);

    record(g_rec, sizeof(struct t_loom_service_snapshot));
    ref_into(g_rec, 8, s1, i1);
    rc = t_loom_register(fd, T_LOOM_REGISTER_QUERY_SERVICE_SLOT, g_rec, 1);
    CHECK(rc == 0 && get32(g_rec, 24) == T_LOOM_SERVICE_SCOPE &&
          get32(g_rec, 28) == T_LOOM_SERVICE_RESERVED, "QUERY reports the reserved scope", rc);
    p1_writable(1);
    record(g_p1 + 256, sizeof(struct t_loom_service_snapshot));
    ref_into(g_p1 + 256, 8, s1, i1);
    p1_writable(0);
    rc = t_loom_register(fd, T_LOOM_REGISTER_QUERY_SERVICE_SLOT, g_p1 + 256, 1);
    CHECK(rc == -E_FAULT, "a read-only QUERY record faults on its copyout", rc);

    control(fd, T_LOOM_REGISTER_REAP_SERVICE_SLOT, s1, i1, 0, "REAP an unused scope");
    control(fd, T_LOOM_REGISTER_ABORT_SCOPE, s1, i1, -E_NOENT, "ABORT of a reaped scope");
    control(fd, T_LOOM_REGISTER_REAP_SERVICE_SLOT, s3, i3, 0, "REAP the second scope");

    record(g_rec, sizeof(struct t_loom_service_target));
    put32(g_rec, 8, 900);
    put32(g_rec, 12, 3);
    g_rec[16] = 'a'; g_rec[17] = 'b'; g_rec[18] = 'c';
    rc = t_loom_register(fd, T_LOOM_REGISTER_SERVICE_TARGET, g_rec, 1);
    CHECK(rc == -E_BADF, "TARGET names a registry fd that does not exist", rc);
}

static void pool_record(void *r) {
    record(r, sizeof(struct t_loom_service_pool_create));
    put32(r, 8, 1);
    put64(r, 32 + 16, PAGE);
}

static void qual_pool(long fd, long buf) {
    struct t_loom_buf_reg br = { .va = (uint64_t)buf, .len = PAGE };
    CHECK(t_loom_register(fd, T_LOOM_REGISTER_BUFFERS, &br, 1) == 0, "private buffer registration", 0);
    p1_writable(1);
    pool_record(g_p1 + 512);
    p1_writable(0);
    long rc = t_loom_register(fd, T_LOOM_REGISTER_SERVICE_POOL, g_p1 + 512, 1);
    CHECK(rc == -E_FAULT, "a read-only pool record faults on its result copyout", rc);
    CHECK(t_loom_register(fd, T_LOOM_REGISTER_BUFFERS, &br, 1) == 0,
          "no pool survives the faulted create", 0);
    pool_record(g_rec);
    rc = t_loom_register(fd, T_LOOM_REGISTER_SERVICE_POOL, g_rec, 1);
    CHECK(rc == 0, "pool create", rc);
    uint32_t slot = get32(g_rec, 16);
    uint64_t incarnation = get64(g_rec, 24);
    CHECK(incarnation, "the pool's result is copied out", 0);
    rc = t_loom_register(fd, T_LOOM_REGISTER_BUFFERS, &br, 1);
    CHECK(rc == -E_BUSY, "the published pool holds the buffer table", rc);

    record(g_rec, sizeof(struct t_loom_service_pool_snapshot));
    ref_into(g_rec, 8, slot, incarnation);
    rc = t_loom_register(fd, T_LOOM_REGISTER_QUERY_SERVICE_POOL, g_rec, 1);
    CHECK(rc == 0 && get32(g_rec, 24) == 1 && get32(g_rec, 28) == 1,
          "POOL QUERY: one member, available", rc);
    p1_writable(1);
    record(g_p1 + 2560, sizeof(struct t_loom_service_pool_snapshot));
    ref_into(g_p1 + 2560, 8, slot, incarnation);
    p1_writable(0);
    rc = t_loom_register(fd, T_LOOM_REGISTER_QUERY_SERVICE_POOL, g_p1 + 2560, 1);
    CHECK(rc == -E_FAULT, "a read-only POOL QUERY record faults on its copyout", rc);
    record(g_rec, sizeof(struct t_loom_service_buffer_return));
    ref_into(g_rec, 8, slot, incarnation);
    put64(g_rec, 32, 1);
    rc = t_loom_register(fd, T_LOOM_REGISTER_RETURN_SERVICE_BUFFER, g_rec, 1);
    CHECK(rc == -E_NOENT, "RETURN of a lease never issued", rc);
    control(fd, T_LOOM_REGISTER_REAP_SERVICE_SLOT, slot, incarnation, 0, "REAP the pool");
    CHECK(t_loom_register(fd, T_LOOM_REGISTER_BUFFERS, NULL, 0) == 0, "the buffer table released", 0);
}

// Four private rings are created here, and the kernel test waits for exactly
// that many to retire: the first, the two rolled back, and the one used last.
static int run_qual(long fd, long fd0, uint64_t va0, long buf) {
    CHECK(fd == fd0 && g_params.ring_va == va0,
          "the first private ring takes the lowest fd and gap", fd);
    static struct t_loom_params first;
    copy_params(&first, &g_params);
    check_ring(&first, P | B, 2);

    fill(&g_params, 0xA5, sizeof(g_params));
    CHECK(setup_at(&g_params, B, 2) == -E_INVAL, "SERVICE_BUFFERS alone refused", 0);
    CHECK(setup_at(&g_params, P | T_LOOM_SETUP_SQPOLL, 2) == -E_INVAL, "SQPOLL refused", 0);
    CHECK(setup_at(&g_params, P | 0x100u, 2) == -E_INVAL, "an unknown setup bit refused", 0);
    CHECK(setup_at(&g_params, P, 0) == -E_INVAL && setup_at(&g_params, P, 3) == -E_INVAL &&
          setup_at(&g_params, P, 2 * T_LOOM_MAX_ENTRIES) == -E_INVAL,
          "zero, non-power-of-two and oversized entries refused", 0);
    pre_route_refusals();

    // A params copyout that faults rolls the whole setup back: the next setup
    // gets the same fd and the same ring address.
    close_ring(fd, &first);
    p1_writable(1);
    fill(g_p1, 0, sizeof(struct t_loom_params));
    put32(g_p1, 0, P | B);
    p1_writable(0);
    long rc = t_loom_setup(2, (struct t_loom_params *)(void *)g_p1);
    CHECK(rc == -E_FAULT, "read-only params: the copyout faults", rc);
    uint8_t *split = g_p1 - 16;
    fill(split, 0xA5, 16);
    rc = setup_at(split, P | B, 2);
    CHECK(rc == -E_FAULT, "params running into a read-only page: the copyout faults", rc);
    CHECK(get32(split, 0) == (P | B) && get32(split, 4) == 2 && get32(split, 8) == 4,
          "the fault came mid-copyout, after the ring existed", get32(split, 4));
    fill(&g_params, 0xA5, sizeof(g_params));
    fd = setup_at(&g_params, P | B, 2);
    CHECK(fd == fd0 && g_params.ring_va == va0,
          "both rollbacks closed the handle and unmapped the ring", fd);
    volatile struct t_loom_ring_hdr *h = check_ring(&g_params, P | B, 2);

    qual_enter_and_cq(fd, h, &g_params);
    qual_register(fd);
    qual_pool(fd, buf);
    close_ring(fd, &g_params);
    t_putstr("loom-bridge-probe: qualification image routes every private syscall from EL0\n");
    return PASS_QUAL;
}

int main(void) {
    fault_pages();
    long buf = t_burrow_attach(PAGE);
    CHECK(buf > 0, "a buffer page", buf);

    // The lowest fd and gap, taken by an ordinary ring and given back.
    fill(&g_params, 0xA5, sizeof(g_params));
    long fd0 = setup_at(&g_params, 0, 2);
    CHECK(fd0 >= 0, "an ordinary ring", fd0);
    static struct t_loom_params ordinary;
    copy_params(&ordinary, &g_params);
    check_ring(&ordinary, 0, 2);
    uint64_t va0 = ordinary.ring_va;
    close_ring(fd0, &ordinary);

    fill(&g_params, 0xA5, sizeof(g_params));
    long fd = setup_at(&g_params, P | B, 2);
    if (fd == -E_OPNOTSUPP) return run_default(fd0, va0);
    CHECK(fd >= 0, "a private ring in the qualification image", fd);
    return run_qual(fd, fd0, va0, buf);
}
