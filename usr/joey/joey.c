// /joey — first userspace process; the long-running init.
//
// joey orchestrates the production boot path and, since
// P5-corvus-bringup-b, drives an end-to-end test of /sbin/corvus over a
// kernel-owned transport. At P5-corvus-srv-impl-b3b the transport
// switched from a pipe pair (corvus's fd 0/1) to /srv/corvus (corvus's
// 9P2000.L server reached via t_srv_connect). The verb exchange is
// unchanged at the dispatch layer:
//
//   USER_CREATE michael → USER_CREATE susan → AUTH(wrong) → AUTH(ok)
//   → WRAP(users/michael, dek) → UNWRAP(users/michael) [DEK round-trip]
//   → UNWRAP(users/susan)  [C-7: PermissionDenied]
//   → UNWRAP(users/ghost)  [unknown dataset: NotFound]
//   → UNWRAP(users/michael, wrong key_id)  [InternalError]
//   → UNWRAP(users/michael, malformed envelope)  [BadFormat]
//   → SESSION_CLOSE.
//
// Plus the Q11 negative regression (P5-corvus-srv-impl-b1's deferred
// case): a second connection with a verb-frame carrying a bad
// protocol_version. corvus must reply BadFormat and tear the connection
// down (Q11); joey reconnects to verify clean recovery and authenticates
// again.
//
// joey returns non-zero on any failed assertion — that is the boot-path
// regression signal.

#include <thyla/syscall.h>
#include <thyla/poll.h>

static const char *itoa_dec(long v, char *buf, size_t buf_sz) {
    if (buf_sz == 0) return "";
    if (v == 0) {
        if (buf_sz < 2) return "";
        buf[0] = '0'; buf[1] = '\0';
        return buf;
    }
    int negative = 0;
    unsigned long u;
    if (v < 0) { negative = 1; u = (unsigned long)(-(v + 1)) + 1; }
    else       { u = (unsigned long)v; }
    char tmp[24];
    size_t n = 0;
    while (u && n < sizeof(tmp)) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    size_t need = n + (negative ? 1 : 0) + 1;
    if (need > buf_sz) return "";
    size_t i = 0;
    if (negative) buf[i++] = '-';
    while (n > 0) buf[i++] = tmp[--n];
    buf[i] = '\0';
    return buf;
}

// =============================================================================
// corvus wire helpers — CORVUS-DESIGN.md §6.4 binary frames over a
// single /srv/corvus connection handle.
// =============================================================================

// write_all — loop t_write until `len` bytes drained. 0 / -1.
static int write_all(long fd, const unsigned char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        long n = t_write(fd, &buf[sent], len - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

// read_exact — loop t_read until `len` bytes read. 0 / -1 (EOF or
// error).
static int read_exact(long fd, unsigned char *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        long n = t_read(fd, &buf[got], len - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

// corvus_exchange — write a [verb|version|len_lo|len_hi|payload] request
// frame, read a [status|len_lo|len_hi|payload] response frame. On
// transport success returns 0 and fills *status + *resp_len; the
// response payload lands in rx[3 .. 3+*resp_len]. -1 on transport
// error.
#define CORVUS_PROTOCOL_VERSION 1
static int corvus_exchange(long conn_fd,
                           unsigned char verb,
                           const unsigned char *payload, size_t payload_len,
                           unsigned char *rx, size_t rx_cap,
                           unsigned char *status, size_t *resp_len) {
    unsigned char hdr[4];
    hdr[0] = verb;
    hdr[1] = CORVUS_PROTOCOL_VERSION;
    hdr[2] = (unsigned char)(payload_len & 0xff);
    hdr[3] = (unsigned char)(payload_len >> 8);
    if (write_all(conn_fd, hdr, 4) != 0) return -1;
    if (payload_len > 0 && write_all(conn_fd, payload, payload_len) != 0) return -1;
    if (read_exact(conn_fd, rx, 3) != 0) return -1;
    *status = rx[0];
    size_t rlen = (size_t)rx[1] | ((size_t)rx[2] << 8);
    if (3 + rlen > rx_cap) return -1;
    if (rlen > 0 && read_exact(conn_fd, &rx[3], rlen) != 0) return -1;
    *resp_len = rlen;
    return 0;
}

// build_user_create — USER_CREATE payload (verb 5).
//   user_len(1) + user + pass_len(2) + pass + backend(1)
static size_t build_user_create(unsigned char *pl,
                                const char *user, size_t user_len,
                                const char *pass, size_t pass_len) {
    size_t o = 0;
    pl[o++] = (unsigned char)user_len;
    for (size_t i = 0; i < user_len; i++) pl[o++] = (unsigned char)user[i];
    pl[o++] = (unsigned char)(pass_len & 0xff);
    pl[o++] = (unsigned char)(pass_len >> 8);
    for (size_t i = 0; i < pass_len; i++) pl[o++] = (unsigned char)pass[i];
    pl[o++] = 0; // backend = 0 (passphrase)
    return o;
}

// build_auth — AUTH payload (verb 1).
//   user_len(1) + user + pass_len(2) + pass
static size_t build_auth(unsigned char *pl,
                         const char *user, size_t user_len,
                         const char *pass, size_t pass_len) {
    size_t o = 0;
    pl[o++] = (unsigned char)user_len;
    for (size_t i = 0; i < user_len; i++) pl[o++] = (unsigned char)user[i];
    pl[o++] = (unsigned char)(pass_len & 0xff);
    pl[o++] = (unsigned char)(pass_len >> 8);
    for (size_t i = 0; i < pass_len; i++) pl[o++] = (unsigned char)pass[i];
    return o;
}

// build_admin_elevate — ADMIN_ELEVATE payload (verb 7; P5-hostowner-b-b).
//   token(33) + sys_pass_len(2 LE) + sys_passphrase
static size_t build_admin_elevate(unsigned char *pl,
                                  const unsigned char *token,
                                  const char *sys_pass, size_t sys_pass_len) {
    size_t o = 0;
    for (int i = 0; i < 33; i++) pl[o++] = token[i];
    pl[o++] = (unsigned char)(sys_pass_len & 0xff);
    pl[o++] = (unsigned char)(sys_pass_len >> 8);
    for (size_t i = 0; i < sys_pass_len; i++) pl[o++] = (unsigned char)sys_pass[i];
    return o;
}

// build_wrap — WRAP payload (verb 10).
//   token(33) + dataset_len(1) + dataset + key_id(8) + dek_len(2) + dek
static size_t build_wrap(unsigned char *pl,
                         const unsigned char *token,
                         const char *dataset, size_t dataset_len,
                         unsigned long key_id,
                         const unsigned char *dek, size_t dek_len) {
    size_t o = 0;
    for (int i = 0; i < 33; i++) pl[o++] = token[i];
    pl[o++] = (unsigned char)dataset_len;
    for (size_t i = 0; i < dataset_len; i++) pl[o++] = (unsigned char)dataset[i];
    for (int i = 0; i < 8; i++) pl[o++] = (unsigned char)(key_id >> (8 * i));
    pl[o++] = (unsigned char)(dek_len & 0xff);
    pl[o++] = (unsigned char)(dek_len >> 8);
    for (size_t i = 0; i < dek_len; i++) pl[o++] = dek[i];
    return o;
}

// build_unwrap — UNWRAP payload (verb 4).
//   token(33) + dataset_len(1) + dataset + key_id(8) + wrapped_len(2) + wrapped
static size_t build_unwrap(unsigned char *pl,
                           const unsigned char *token,
                           const char *dataset, size_t dataset_len,
                           unsigned long key_id,
                           const unsigned char *wrapped, size_t wrapped_len) {
    size_t o = 0;
    for (int i = 0; i < 33; i++) pl[o++] = token[i];
    pl[o++] = (unsigned char)dataset_len;
    for (size_t i = 0; i < dataset_len; i++) pl[o++] = (unsigned char)dataset[i];
    for (int i = 0; i < 8; i++) pl[o++] = (unsigned char)(key_id >> (8 * i));
    pl[o++] = (unsigned char)(wrapped_len & 0xff);
    pl[o++] = (unsigned char)(wrapped_len >> 8);
    for (size_t i = 0; i < wrapped_len; i++) pl[o++] = wrapped[i];
    return o;
}

// =============================================================================
// stratumd-stub boot pivot demo (P5-stratumd-stub-bringup-c). Production-
// shape orchestration on joey's boot path: t_pipe × 2 → t_spawn_with_fds →
// t_attach_9p → t_mount → t_unmount → t_close × N → t_wait_pid. This is
// the same sequence /stub-driver runs (P5-stratumd-stub-bringup-b), now
// inline in joey so the boot log carries production-shape evidence of the
// 9P attach + mount lifecycle. Real stratumd swaps in for /stratumd-stub
// when the musl sysroot lands (Phase 6 dependency). target_path_id 99
// matches the existing /stub-driver convention to keep the kernel test +
// boot-path numbers aligned.
//
// Failure semantics: any sub-step failure returns -1 with a diagnostic;
// joey's main treats that as a boot regression (return 1).
static int do_stratumd_stub_bringup(void) {
    long c2s_rd = -1, c2s_wr = -1;
    if (t_pipe(&c2s_rd, &c2s_wr) < 0) {
        t_putstr("joey: stub-bringup t_pipe c2s FAILED\n");
        return -1;
    }
    long s2c_rd = -1, s2c_wr = -1;
    if (t_pipe(&s2c_rd, &s2c_wr) < 0) {
        t_putstr("joey: stub-bringup t_pipe s2c FAILED\n");
        return -1;
    }

    const char stub_name[] = "stratumd-stub";
    unsigned int stub_fds[2] = { (unsigned int)c2s_rd, (unsigned int)s2c_wr };
    long stub_pid = t_spawn_with_fds(stub_name, sizeof(stub_name) - 1,
                                     stub_fds, 2);
    if (stub_pid <= 0) {
        t_putstr("joey: stub-bringup t_spawn_with_fds FAILED\n");
        return -1;
    }

    // Drop joey-side refs on the stub's transport fds. Without this,
    // the stub never sees EOF on c2s_rd because joey would still hold
    // an alive reader on the c2s ring.
    if (t_close(c2s_rd) != 0) {
        t_putstr("joey: stub-bringup t_close c2s_rd FAILED\n");
        return -1;
    }
    if (t_close(s2c_wr) != 0) {
        t_putstr("joey: stub-bringup t_close s2c_wr FAILED\n");
        return -1;
    }

    static const char aname[] = "/";
    long attach_fd = t_attach_9p(c2s_wr, s2c_rd, aname, 1, 0);
    if (attach_fd < 0) {
        t_putstr("joey: stub-bringup t_attach_9p FAILED\n");
        return -1;
    }

    if (t_mount(attach_fd, 99, 0) < 0) {
        t_putstr("joey: stub-bringup t_mount FAILED\n");
        return -1;
    }
    if (t_unmount(99) < 0) {
        t_putstr("joey: stub-bringup t_unmount FAILED\n");
        return -1;
    }

    if (t_close(attach_fd) != 0) {
        t_putstr("joey: stub-bringup t_close(attach_fd) FAILED\n");
        return -1;
    }

    // Drop the last joey-side transport refs; c2s_wr last-drop fires
    // write_eof on the c2s ring so the stub's next read returns 0.
    if (t_close(c2s_wr) != 0) {
        t_putstr("joey: stub-bringup t_close c2s_wr FAILED\n");
        return -1;
    }
    if (t_close(s2c_rd) != 0) {
        t_putstr("joey: stub-bringup t_close s2c_rd FAILED\n");
        return -1;
    }

    int status = -1;
    long reaped = t_wait_pid(&status);
    if (reaped != stub_pid) {
        t_putstr("joey: stub-bringup t_wait_pid wrong pid\n");
        return -1;
    }
    if (status != 0) {
        t_putstr("joey: stub-bringup stratumd-stub exited non-zero\n");
        return -1;
    }
    return 0;
}

// connect_corvus — bounded retry to t_srv_connect("corvus", "ctl"),
// yielding between attempts so corvus's startup can race in.
//
// The race: SYS_SPAWN_WITH_PERMS returns the new pid to joey BEFORE
// corvus's first kernel thread runs `sys_spawn_with_fds_thunk` →
// `exec_setup`. So when joey gets back from the spawn syscall and starts
// retrying `t_srv_connect`, corvus's thread is still about to enter
// exec_setup. exec_setup must allocate and zero corvus's 24 MiB BSS
// (linked_list_allocator working memory; the static HEAP_BUF in
// usr/corvus/src/main.rs) via burrow_create_anon + KP_ZERO; the buddy
// allocator rounds this to an order-13 (32 MiB) contiguous block, and
// the KP_ZERO loop writes ~4 million u64 stores. Under emulated QEMU
// AArch64 (no HVF on macOS, TCG-interpreted), this takes upwards of ten
// seconds, sometimes more if scheduling interleaves.
//
// `t_poll` on a never-ready pipe-read end with a small timeout acts as a
// sleep-equivalent — joey's thread enters `tsleep` in the kernel,
// freeing the CPU for corvus's thread on a different vCPU. 60
// iterations × 1000 ms gives 60 seconds of yield budget, comfortably
// over corvus's exec_setup latency on the slowest expected emulator.
// On native ARM64 (HVF/KVM) or hardware, the first iteration typically
// succeeds.
static long connect_corvus(void) {
    long rd_yield, wr_yield;
    if (t_pipe(&rd_yield, &wr_yield) != 0) return -1;
    long result = -1;
    for (int i = 0; i < 60; i++) {
        long h = t_srv_connect("corvus", 6, "ctl", 3);
        if (h >= 0) { result = h; break; }
        struct pollfd pfd = { .fd = (int)rd_yield, .events = POLLIN, .revents = 0 };
        (void)t_poll(&pfd, 1, 1000);
    }
    t_close(rd_yield);
    t_close(wr_yield);
    return result;
}

int main(void) {
    char buf[24];
    t_putstr("joey: hello from /joey (real userspace binary, loaded from ramfs)\n");

    // === /hello orchestration (P5-spawn-wait) ===
    const char hello_name[] = "hello";
    long pid = t_spawn(hello_name, sizeof(hello_name) - 1);
    if (pid <= 0) {
        t_putstr("joey: t_spawn(\"hello\") FAILED\n");
        return 1;
    }
    int status = -1;
    long reaped = t_wait_pid(&status);
    if (reaped != pid || status != 0) {
        t_putstr("joey: /hello orchestration FAILED\n");
        return 1;
    }
    t_putstr("joey: /hello reaped status=0; orchestration verified\n");

    // === /sbin/corvus spawn ===
    //
    // No pipes — corvus posts /srv/corvus and joey reaches it via
    // t_srv_connect. joey grants corvus T_SPAWN_PERM_MAY_POST_SERVICE
    // so corvus can call SYS_POST_SERVICE("corvus") at startup. The
    // perm bit is stamped on the child by the kernel atomically inside
    // the spawn thunk (BEFORE exec_setup; P5-corvus-srv-impl-b3a).
    const char corvus_name[] = "corvus";
    unsigned int no_fds[1] = { 0 };
    // P5-hostowner-b-b: joey grants corvus T_CAP_GRANT_HOSTOWNER so
    // corvus may write /cap/grant on ADMIN_ELEVATE (the kernel cap
    // device gates the grant write on this fork-grantable bit). joey
    // holds it via CAP_ALL; corvus inherits it through the spawn mask.
    long corvus_pid = t_spawn_with_perms(
        corvus_name, sizeof(corvus_name) - 1,
        no_fds, 0,
        T_CAP_LOCK_PAGES | T_CAP_CSPRNG_READ | T_CAP_GRANT_HOSTOWNER,
        T_SPAWN_PERM_MAY_POST_SERVICE);
    if (corvus_pid <= 0) {
        t_putstr("joey: t_spawn_with_perms(\"corvus\") FAILED\n");
        return 1;
    }
    t_putstr("joey: spawned /sbin/corvus pid=");
    t_putstr(itoa_dec(corvus_pid, buf, sizeof(buf)));
    t_putstr("\n");

    long conn_fd = connect_corvus();
    if (conn_fd < 0) {
        t_putstr("joey: t_srv_connect(\"corvus\", \"ctl\") FAILED\n");
        return 1;
    }
    t_putstr("joey: connected /srv/corvus/ctl fd=");
    t_putstr(itoa_dec(conn_fd, buf, sizeof(buf)));
    t_putstr("\n");

    // tx holds the payload under construction (max = UNWRAP ~1274 B);
    // rx holds the response frame (max = WRAP envelope, 3 + 1217 B).
    unsigned char tx[1400];
    unsigned char rx[1300];
    unsigned char st;
    size_t rlen;
    size_t pl;

    const char pass_michael[] = "correct-horse-battery-staple-v1";
    const char pass_susan[]   = "anatomy-trombone-glacier-velvet-42";

    // === USER_CREATE michael === (first user; bootstrap exception
    // — corvus allows USER_CREATE with no caller cap while the user
    // table is empty so the initial hostowner candidate can exist; once
    // any user is created, subsequent USER_CREATEs require the caller
    // to hold CAP_HOSTOWNER, which lands via ADMIN_ELEVATE below.)
    pl = build_user_create(tx, "michael", 7, pass_michael, sizeof(pass_michael) - 1);
    if (corvus_exchange(conn_fd, 5, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: USER_CREATE michael transport FAILED\n");
        return 1;
    }
    if (st != 0 || rlen != 0) {
        t_putstr("joey: USER_CREATE michael returned non-OK status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: USER_CREATE michael ok (bootstrap)\n");

    // === AUTH michael (wrong passphrase) → BadAuth (1) ===
    pl = build_auth(tx, "michael", 7, "wrong-passphrase", 16);
    if (corvus_exchange(conn_fd, 1, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: AUTH(wrong) transport FAILED\n");
        return 1;
    }
    if (st != 1) {
        t_putstr("joey: AUTH(wrong pass) expected BadAuth(1), got status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: AUTH(wrong pass) returned BadAuth (expected)\n");

    // === AUTH michael (correct) → OK + 33-byte session token ===
    pl = build_auth(tx, "michael", 7, pass_michael, sizeof(pass_michael) - 1);
    if (corvus_exchange(conn_fd, 1, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: AUTH transport FAILED\n");
        return 1;
    }
    if (st != 0 || rlen != 33 || rx[3] != 's') {
        t_putstr("joey: AUTH returned bad status/token status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    unsigned char token[33];
    for (int i = 0; i < 33; i++) token[i] = rx[3 + i];
    t_putstr("joey: AUTH ok (token=");
    for (int i = 0; i < 8; i++) {
        char c[2] = { (char)token[i], 0 };
        t_putstr(c);
    }
    t_putstr("...)\n");

    // === ADMIN_ELEVATE michael (P5-hostowner-b-b) === verify console +
    // system passphrase; corvus writes /cap/grant for joey's stripes,
    // returns OK. Then joey redeems via t_cap_use → joey's Proc gets
    // CAP_HOSTOWNER. After this, joey can call admin-gated verbs
    // (USER_CREATE et al.).
    pl = build_admin_elevate(tx, token, "thylacine", 9);
    if (corvus_exchange(conn_fd, 7, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: ADMIN_ELEVATE transport FAILED\n");
        return 1;
    }
    if (st != 0 || rlen != 0) {
        t_putstr("joey: ADMIN_ELEVATE returned non-OK status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: ADMIN_ELEVATE ok\n");

    if (t_cap_use(T_CAP_HOSTOWNER) != 0) {
        t_putstr("joey: t_cap_use(CAP_HOSTOWNER) FAILED\n");
        return 1;
    }
    t_putstr("joey: t_cap_use(CAP_HOSTOWNER) ok (joey now hostowner)\n");

    // === USER_CREATE susan === (gives the cross-user C-7 test a real
    // owner). Now gated on CAP_HOSTOWNER — joey just elevated, so
    // peer_live_caps for joey's conn includes CAP_HOSTOWNER.
    pl = build_user_create(tx, "susan", 5, pass_susan, sizeof(pass_susan) - 1);
    if (corvus_exchange(conn_fd, 5, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: USER_CREATE susan transport FAILED\n");
        return 1;
    }
    if (st != 0 || rlen != 0) {
        t_putstr("joey: USER_CREATE susan (post-elevate) returned non-OK status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: USER_CREATE susan ok (gated on CAP_HOSTOWNER)\n");

    // === WRAP users/michael — wrap a known 32-byte DEK ===
    unsigned char dek[32];
    for (int i = 0; i < 32; i++) dek[i] = (unsigned char)(0x40 + i);
    pl = build_wrap(tx, token, "users/michael", 13, 1, dek, 32);
    if (corvus_exchange(conn_fd, 10, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: WRAP transport FAILED\n");
        return 1;
    }
    if (st != 0 || rlen != 1217) {
        t_putstr("joey: WRAP returned bad status/len status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr(" len=");
        t_putstr(itoa_dec((long)rlen, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    unsigned char envelope[1217];
    for (int i = 0; i < 1217; i++) envelope[i] = rx[3 + i];
    t_putstr("joey: WRAP users/michael ok (envelope 1217 B)\n");

    // === UNWRAP users/michael — DEK round-trip ===
    pl = build_unwrap(tx, token, "users/michael", 13, 1, envelope, 1217);
    if (corvus_exchange(conn_fd, 4, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: UNWRAP transport FAILED\n");
        return 1;
    }
    if (st != 0 || rlen != 32) {
        t_putstr("joey: UNWRAP users/michael returned bad status/len status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    int dek_ok = 1;
    for (int i = 0; i < 32; i++) {
        if (rx[3 + i] != (unsigned char)(0x40 + i)) dek_ok = 0;
    }
    if (!dek_ok) {
        t_putstr("joey: UNWRAP users/michael DEK mismatch — round-trip FAILED\n");
        return 1;
    }
    t_putstr("joey: UNWRAP users/michael ok (DEK round-trip verified)\n");

    // === UNWRAP users/susan — C-7: cross-user must be PermissionDenied ===
    pl = build_unwrap(tx, token, "users/susan", 11, 1, envelope, 1217);
    if (corvus_exchange(conn_fd, 4, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: UNWRAP(susan) transport FAILED\n");
        return 1;
    }
    if (st != 2) {
        t_putstr("joey: UNWRAP users/susan expected PermissionDenied(2), got status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: UNWRAP users/susan refused PermissionDenied (C-7 verified)\n");

    // === UNWRAP users/ghost — unknown dataset must be NotFound ===
    pl = build_unwrap(tx, token, "users/ghost", 11, 1, envelope, 1217);
    if (corvus_exchange(conn_fd, 4, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: UNWRAP(ghost) transport FAILED\n");
        return 1;
    }
    if (st != 3) {
        t_putstr("joey: UNWRAP users/ghost expected NotFound(3), got status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: UNWRAP users/ghost refused NotFound (expected)\n");

    // === UNWRAP users/michael, wrong key_id — F3 regression ===
    pl = build_unwrap(tx, token, "users/michael", 13, 999, envelope, 1217);
    if (corvus_exchange(conn_fd, 4, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: UNWRAP(wrong key_id) transport FAILED\n");
        return 1;
    }
    if (st != 6) {  // STATUS_INTERNAL_ERROR
        t_putstr("joey: UNWRAP wrong key_id expected InternalError(6), got status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: UNWRAP wrong key_id refused InternalError (key_id AAD bind verified)\n");

    // === UNWRAP users/michael, malformed envelope — F8 regression ===
    unsigned char junk[10];
    for (int i = 0; i < 10; i++) junk[i] = 0xff;
    pl = build_unwrap(tx, token, "users/michael", 13, 1, junk, 10);
    if (corvus_exchange(conn_fd, 4, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: UNWRAP(malformed envelope) transport FAILED\n");
        return 1;
    }
    if (st != 5) {  // STATUS_BAD_FORMAT
        t_putstr("joey: UNWRAP malformed envelope expected BadFormat(5), got status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: UNWRAP malformed envelope refused BadFormat (expected)\n");

    // === SESSION_CLOSE ===
    if (corvus_exchange(conn_fd, 3, token, 33, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: SESSION_CLOSE transport FAILED\n");
        return 1;
    }
    if (st != 0 || rlen != 0) {
        t_putstr("joey: SESSION_CLOSE returned non-OK status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: SESSION_CLOSE ok\n");

    // Drop the first connection so corvus closes its session-bearing
    // record + recycles the per-Proc-cap (SRV_CONN_PER_PROC_MAX = 1).
    t_close(conn_fd);

    // === Q11 negative regression — protocol_version mismatch ===
    //
    // Reconnect, send a verb frame with protocol_version=99. corvus
    // must reply BadFormat AND tear down (the wire shape may differ
    // across versions; the stream cannot be safely re-synced — same
    // discipline as oversize payload_len). joey reconnects again and
    // verifies a fresh AUTH succeeds.
    long conn_fd2 = connect_corvus();
    if (conn_fd2 < 0) {
        t_putstr("joey: Q11-negative t_srv_connect FAILED\n");
        return 1;
    }
    {
        unsigned char bad_hdr[4];
        bad_hdr[0] = 1;     // verb_id = AUTH (any verb; the version
                            // gate fires before the verb dispatch)
        bad_hdr[1] = 99;    // protocol_version — UNSUPPORTED
        bad_hdr[2] = 0;
        bad_hdr[3] = 0;
        if (write_all(conn_fd2, bad_hdr, 4) != 0) {
            t_putstr("joey: Q11-negative write FAILED\n");
            return 1;
        }
        unsigned char rh[3];
        if (read_exact(conn_fd2, rh, 3) != 0) {
            t_putstr("joey: Q11-negative read FAILED\n");
            return 1;
        }
        if (rh[0] != 5) {  // STATUS_BAD_FORMAT
            t_putstr("joey: Q11-negative expected BadFormat(5), got status=");
            t_putstr(itoa_dec(rh[0], buf, sizeof(buf)));
            t_putstr("\n");
            return 1;
        }
        size_t bl = (size_t)rh[1] | ((size_t)rh[2] << 8);
        if (bl != 0) {
            t_putstr("joey: Q11-negative bad-format response carries unexpected payload\n");
            return 1;
        }
    }
    t_putstr("joey: Q11-negative refused BadFormat + tore down conn (expected)\n");
    t_close(conn_fd2);

    // === Reconnect after Q11 tear-down — fresh AUTH must succeed ===
    long conn_fd3 = connect_corvus();
    if (conn_fd3 < 0) {
        t_putstr("joey: reconnect t_srv_connect FAILED\n");
        return 1;
    }
    pl = build_auth(tx, "michael", 7, pass_michael, sizeof(pass_michael) - 1);
    if (corvus_exchange(conn_fd3, 1, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: reconnect AUTH transport FAILED\n");
        return 1;
    }
    if (st != 0 || rlen != 33 || rx[3] != 's') {
        t_putstr("joey: reconnect AUTH returned bad status/token status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    unsigned char token2[33];
    for (int i = 0; i < 33; i++) token2[i] = rx[3 + i];
    if (corvus_exchange(conn_fd3, 3, token2, 33, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: reconnect SESSION_CLOSE transport FAILED\n");
        return 1;
    }
    if (st != 0 || rlen != 0) {
        t_putstr("joey: reconnect SESSION_CLOSE returned non-OK status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: reconnect AUTH + SESSION_CLOSE ok (Q11 recovery verified)\n");
    t_close(conn_fd3);

    // corvus exits when its listener torns down — joey doesn't reap it
    // because corvus's main loop continues serving until the listener
    // is unposted. At v1.0 there's no SYS_UNPOST_SERVICE; corvus dies
    // when joey exits (the kernel reaps).
    //
    // For boot-test simplicity, we just exit clean; the kernel-side
    // reaping happens at proc_exit. corvus's main loop exits on the
    // next listener POLLHUP (initiated by joey's proc_exit triggering
    // service cleanup).

    t_putstr("joey: corvus-d hybrid-PKE round-trip verified via /srv/corvus (b3b)\n");

    // === stratumd-stub boot pivot demo (P5-stratumd-stub-bringup-c) ===
    if (do_stratumd_stub_bringup() != 0) return 1;
    t_putstr("joey: stub-bringup ok (pipe + spawn + attach + mount + unmount)\n");

    return 0;
}
