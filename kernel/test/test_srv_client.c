// P5-corvus-srv-impl-b2 — kernel-internal tests for the /srv CLIENT
// side: the per-Proc cap, the KObj_Srv r/w dispatch in SYS_READ /
// SYS_WRITE, and the handshake-drive's failure path.
//
// What this chunk covers:
//
//   srv_client.per_proc_cap_blocks_second_open
//     A Proc that already holds a /srv client-connection handle cannot
//     mint a second. The check fires BEFORE srvconn_create — no allocation
//     burned on the rejected attempt. The global SRV_MAX_CONNS cap is a
//     separate, weaker bound (set high; not hit here).
//
//   srv_client.handle_close_decrements_cap
//     Closing the connection handle drops p->srv_conn_count back to 0;
//     a subsequent open for the same Proc succeeds.
//
//   srv_client.read_write_fail_pre_handshake
//     Before srvconn_drive_client_handshake completes, sys_read_for_proc
//     and sys_write_for_proc on the KObj_Srv handle fail-close (-1). The
//     gate is in srvconn_client_read / srvconn_client_write; without it,
//     a pre-handshake read would Tread on fid 0 (the never-bound default)
//     and the kernel client would emit a malformed wire request.
//
//   srv_client.handshake_times_out_without_responder
//     With no server processing Tversion, srvconn_drive_client_handshake
//     hits its deadline (caller sets srvconn_set_client_deadline) and
//     returns -1, leaving client_handshake_done = false — the
//     server-hang resilience documented in CORVUS-DESIGN §6.2.
//
//   srv_client.sys_srv_connect_unknown_service
//     SYS_SRV_CONNECT against a name that no Proc has posted returns -1
//     with no SrvConn / handle created (per-Proc cap untouched).
//
//   srv_client.sys_srv_connect_per_proc_cap
//     Two SYS_SRV_CONNECT calls from one Proc — second returns -1 (the
//     production path inherits the cap from srv_conn_open_for_proc).
//
// The full handshake-success path (where the responder synthesizes
// Rversion / Rattach / Rwalk / Rlopen and the kernel client's Tread /
// Twrite round-trip data) lands in b3 against the real corvus 9P server
// — duplicating that responder here would re-implement b3's work.

#include "test.h"

#include <thylacine/devsrv.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/srvconn.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/timer.h"   // timer_now_ns — deadline for the timeout test

// Testable cores defined in kernel/syscall.c (non-static — same pattern
// as the other -for_proc helpers).
extern int sys_post_service_for_proc(struct Proc *p, const char *name,
                                     size_t name_len);
extern int sys_srv_connect_for_proc(struct Proc *p,
                                    const char *name, size_t name_len,
                                    const u8 *path, size_t path_len);
extern s64 sys_read_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len);
extern s64 sys_write_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf,
                              u64 len);

// Test-support: registry wipe (non-static in kernel/devsrv.c, no
// production caller).
extern void srv_registry_reset(void);

void test_srv_client_per_proc_cap_blocks_second_open(void);
void test_srv_client_handle_close_decrements_cap(void);
void test_srv_client_read_write_fail_pre_handshake(void);
void test_srv_client_handshake_times_out_without_responder(void);
void test_srv_client_sys_srv_connect_unknown_service(void);
void test_srv_client_sys_srv_connect_per_proc_cap(void);

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

static struct Proc *make_test_proc(void) {
    return proc_alloc();
}

static struct Proc *make_marked_test_proc(void) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    proc_mark_may_post_service(p);
    return p;
}

static void drop_test_proc(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// Open a /srv connection for `client` against the "corvus" service that
// `corvus` has posted. Returns the KObj_Srv handle (>= 0) or -1. Used by
// every test that needs a posted+opened SrvConn.
static int open_connection(struct Proc *client) {
    return srv_conn_open_for_proc(client, "corvus", 6);
}

// Resolve a KOBJ_SRV handle to its underlying SrvConn (for tests that
// need to flip the handshake_done flag or set a deadline directly).
// Returns NULL on a bad / non-SRV / wrong-magic handle.
static struct SrvConn *conn_of(struct Proc *p, hidx_t h) {
    struct Handle *slot = handle_get(p, h);
    if (!slot || slot->kind != KOBJ_SRV || !slot->obj) return NULL;
    if (*(const u64 *)slot->obj != SRV_CONN_MAGIC)     return NULL;
    return (struct SrvConn *)slot->obj;
}

// ---------------------------------------------------------------------------
// srv_client.per_proc_cap_blocks_second_open
// ---------------------------------------------------------------------------

void test_srv_client_per_proc_cap_blocks_second_open(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "proc_alloc + mark for corvus");
    TEST_ASSERT(sys_post_service_for_proc(corvus, "corvus", 6) >= 0,
        "post \"corvus\"");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "proc_alloc for client");
    TEST_EXPECT_EQ((int)client->srv_conn_count, 0,
        "client starts with srv_conn_count == 0");

    int h1 = open_connection(client);
    TEST_ASSERT(h1 >= 0, "first open succeeds");
    TEST_EXPECT_EQ((int)client->srv_conn_count, 1,
        "srv_conn_count bumped to 1");

    // Second open MUST fail — the cap is 1. No SrvConn allocated for it.
    u64 created_before = srvconn_total_created();
    int h2 = open_connection(client);
    TEST_EXPECT_EQ(h2, -1, "second open refused — per-Proc cap is 1");
    TEST_EXPECT_EQ((int)client->srv_conn_count, 1,
        "cap counter unchanged on refused open");
    TEST_EXPECT_EQ(srvconn_total_created(), created_before,
        "no SrvConn allocation on refused open (fail-fast)");

    // Cleanup. handle_close decrements; teardown waits for the accept-
    // backlog ref to drop (we drain by resetting the registry, which
    // tombstones the service and clears the backlog).
    handle_close(client, h1);
    srv_registry_reset();
    drop_test_proc(corvus);
    drop_test_proc(client);
}

// ---------------------------------------------------------------------------
// srv_client.handle_close_decrements_cap
// ---------------------------------------------------------------------------

void test_srv_client_handle_close_decrements_cap(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "proc_alloc + mark");
    TEST_ASSERT(sys_post_service_for_proc(corvus, "corvus", 6) >= 0,
        "post \"corvus\"");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "proc_alloc");

    int h1 = open_connection(client);
    TEST_ASSERT(h1 >= 0, "first open");
    TEST_EXPECT_EQ((int)client->srv_conn_count, 1, "count == 1");

    // Close — drops back to 0 via the KOBJ_SRV SRV_CONN_MAGIC arm of
    // handle_close.
    TEST_EXPECT_EQ(handle_close(client, h1), 0, "close succeeds");
    TEST_EXPECT_EQ((int)client->srv_conn_count, 0,
        "srv_conn_count back to 0 after close");

    // A second open for the SAME Proc now succeeds (the cap was the only
    // gate; with count == 0 it's lifted).
    int h2 = open_connection(client);
    TEST_ASSERT(h2 >= 0, "reopen after close succeeds");
    TEST_EXPECT_EQ((int)client->srv_conn_count, 1, "count == 1 again");

    handle_close(client, h2);
    srv_registry_reset();
    drop_test_proc(corvus);
    drop_test_proc(client);
}

// ---------------------------------------------------------------------------
// srv_client.read_write_fail_pre_handshake
// ---------------------------------------------------------------------------

void test_srv_client_read_write_fail_pre_handshake(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "proc_alloc + mark");
    TEST_ASSERT(sys_post_service_for_proc(corvus, "corvus", 6) >= 0,
        "post \"corvus\"");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "proc_alloc");

    int h = open_connection(client);
    TEST_ASSERT(h >= 0, "open");
    struct SrvConn *cn = conn_of(client, h);
    TEST_ASSERT(cn != NULL, "conn_of recovers the SrvConn");
    TEST_EXPECT_EQ((int)cn->client_handshake_done, 0,
        "fresh SrvConn has handshake_done = false");

    // Zero-length r/w validate the handle but don't touch the SrvConn —
    // they return 0 to mirror POSIX. Non-zero r/w hits srvconn_client_*
    // which fails-close on !handshake_done.
    u8 buf[16];
    s64 wn = sys_write_for_proc(client, (hidx_t)h, buf, 8);
    TEST_EXPECT_EQ(wn, (s64)-1, "pre-handshake write fails-closed");
    s64 rn = sys_read_for_proc(client, (hidx_t)h, buf, 8);
    TEST_EXPECT_EQ(rn, (s64)-1, "pre-handshake read fails-closed");

    handle_close(client, h);
    srv_registry_reset();
    drop_test_proc(corvus);
    drop_test_proc(client);
}

// ---------------------------------------------------------------------------
// srv_client.handshake_times_out_without_responder
// ---------------------------------------------------------------------------
//
// With no server consuming the c2s ring, the handshake's first blocking
// recv on s2c (waiting for Rversion) hits the caller-supplied deadline
// and returns -1. The SrvConn stays with handshake_done = false; the
// caller's handle is left open (caller is responsible for closing).

void test_srv_client_handshake_times_out_without_responder(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "proc_alloc + mark");
    TEST_ASSERT(sys_post_service_for_proc(corvus, "corvus", 6) >= 0,
        "post \"corvus\"");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "proc_alloc");

    int h = open_connection(client);
    TEST_ASSERT(h >= 0, "open");
    struct SrvConn *cn = conn_of(client, h);
    TEST_ASSERT(cn != NULL, "conn_of recovers");

    // 5 ms deadline — far less than any reasonable handshake; with no
    // responder, the kernel client's first transport recv hits it.
    srvconn_set_client_deadline(cn, timer_now_ns() + 5u * 1000u * 1000u);

    int rc = srvconn_drive_client_handshake(cn, client->pid, NULL, 0);
    TEST_EXPECT_EQ(rc, -1, "handshake times out without a responder");
    TEST_EXPECT_EQ((int)cn->client_handshake_done, 0,
        "handshake_done stays false on timeout");
    TEST_EXPECT_EQ((int)srvconn_client_timed_out(cn), 1,
        "client_timed_out latched true");

    handle_close(client, h);
    srv_registry_reset();
    drop_test_proc(corvus);
    drop_test_proc(client);
}

// ---------------------------------------------------------------------------
// srv_client.sys_srv_connect_unknown_service
// ---------------------------------------------------------------------------

void test_srv_client_sys_srv_connect_unknown_service(void) {
    srv_registry_reset();

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "proc_alloc");

    // No service posted under "corvus" — registry is empty.
    int rc = sys_srv_connect_for_proc(client, "corvus", 6, NULL, 0);
    TEST_EXPECT_EQ(rc, -1, "SYS_SRV_CONNECT to unposted name → -1");
    TEST_EXPECT_EQ((int)client->srv_conn_count, 0,
        "cap counter untouched by refused connect");

    srv_registry_reset();
    drop_test_proc(client);
}

// ---------------------------------------------------------------------------
// srv_client.sys_srv_connect_per_proc_cap
// ---------------------------------------------------------------------------
//
// The production-call path (SYS_SRV_CONNECT) inherits the cap from
// srv_conn_open_for_proc — a second SYS_SRV_CONNECT from the same Proc
// is refused with no SrvConn allocated. We don't drive a real handshake
// here (no responder), so the FIRST call still fails at the handshake
// step (deadline reached or, more practically here, a -1 from
// p9_client_handshake when the kernel client's transport recv returns
// an error sentinel). The crucial assertion is: a second open fails
// either way — under the per-Proc cap when count == 1, or under the
// post-cleanup count == 0 after the first call rolled back.
//
// To get a deterministic cap test from the SYS_SRV_CONNECT entry point,
// we set up an INTERMEDIATE state: a manually-installed connection (via
// srv_conn_open_for_proc, count → 1), then attempt SYS_SRV_CONNECT and
// observe the rejection.

void test_srv_client_sys_srv_connect_per_proc_cap(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "proc_alloc + mark");
    TEST_ASSERT(sys_post_service_for_proc(corvus, "corvus", 6) >= 0,
        "post \"corvus\"");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "proc_alloc");

    // First open via the bare srv_conn_open_for_proc — no handshake
    // (so the test doesn't need a responder), count goes 0 → 1.
    int h1 = open_connection(client);
    TEST_ASSERT(h1 >= 0, "first open succeeds");
    TEST_EXPECT_EQ((int)client->srv_conn_count, 1, "count == 1");

    // SYS_SRV_CONNECT from the same Proc — refused at the cap gate
    // inside srv_conn_open_for_proc, BEFORE the handshake step. Returns
    // -1 with no second SrvConn allocated.
    u64 created_before = srvconn_total_created();
    int rc = sys_srv_connect_for_proc(client, "corvus", 6, NULL, 0);
    TEST_EXPECT_EQ(rc, -1, "SYS_SRV_CONNECT refused by per-Proc cap");
    TEST_EXPECT_EQ((int)client->srv_conn_count, 1,
        "cap counter unchanged on refused SYS_SRV_CONNECT");
    TEST_EXPECT_EQ(srvconn_total_created(), created_before,
        "no SrvConn allocation on cap-refused SYS_SRV_CONNECT");

    handle_close(client, h1);
    srv_registry_reset();
    drop_test_proc(corvus);
    drop_test_proc(client);
}
