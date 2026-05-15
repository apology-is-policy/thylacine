// /joey — first userspace process; the long-running init.
//
// joey orchestrates the production boot path and, since
// P5-corvus-bringup-b, drives an end-to-end test of /sbin/corvus over a
// pipe pair. At P5-corvus-bringup-d the corvus exchange exercises the
// full key-agent surface:
//
//   USER_CREATE michael → USER_CREATE susan → AUTH(wrong) → AUTH(ok)
//   → WRAP(users/michael, dek) → UNWRAP(users/michael) [DEK round-trip]
//   → UNWRAP(users/susan)  [C-7: PermissionDenied]
//   → UNWRAP(users/ghost)  [unknown dataset: NotFound]
//   → SESSION_CLOSE.
//
// The UNWRAP(users/susan) case is the spec-pinned C-7 test: michael's
// session must be refused a dataset owned by another user (specs/
// corvus.tla UnwrapOwnerOnly / the BuggyUnwrapCrossUser negative).
//
// joey returns non-zero on any failed assertion — that is the boot-path
// regression signal.

#include <thyla/syscall.h>

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
// corvus wire helpers — CORVUS-DESIGN.md §6.4 binary frames.
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

// read_exact — loop t_read until `len` bytes read. 0 / -1 (EOF or error).
static int read_exact(long fd, unsigned char *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        long n = t_read(fd, &buf[got], len - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

// corvus_exchange — write a [verb|len|payload] frame, read the response.
// On transport success returns 0 and fills *status + *resp_len; the
// response payload lands in rx[3 .. 3+*resp_len]. -1 on transport error.
static int corvus_exchange(long wfd, long rfd,
                           unsigned char verb,
                           const unsigned char *payload, size_t payload_len,
                           unsigned char *rx, size_t rx_cap,
                           unsigned char *status, size_t *resp_len) {
    unsigned char hdr[3];
    hdr[0] = verb;
    hdr[1] = (unsigned char)(payload_len & 0xff);
    hdr[2] = (unsigned char)(payload_len >> 8);
    if (write_all(wfd, hdr, 3) != 0) return -1;
    if (payload_len > 0 && write_all(wfd, payload, payload_len) != 0) return -1;
    if (read_exact(rfd, rx, 3) != 0) return -1;
    *status = rx[0];
    size_t rlen = (size_t)rx[1] | ((size_t)rx[2] << 8);
    if (3 + rlen > rx_cap) return -1;
    if (rlen > 0 && read_exact(rfd, &rx[3], rlen) != 0) return -1;
    *resp_len = rlen;
    return 0;
}

// build_user_create — USER_CREATE payload (verb 5). Returns length.
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

// build_auth — AUTH payload (verb 1). Returns length.
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

// build_wrap — WRAP payload (verb 10). Returns length.
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

// build_unwrap — UNWRAP payload (verb 4). Returns length.
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

    // === /sbin/corvus spawn (P5-corvus-bringup-b) ===
    //
    // Two pipes: c2s (joey → corvus, corvus's fd 0) and s2c (corvus →
    // joey, corvus's fd 1). t_spawn_full installs [c2s_rd, s2c_wr] as
    // corvus's slots 0/1; joey then drops its copies of the child-side
    // fds and keeps c2s_wr (write to corvus) + s2c_rd (read from corvus).
    long c2s_rd, c2s_wr, s2c_rd, s2c_wr;
    if (t_pipe(&c2s_rd, &c2s_wr) != 0 || t_pipe(&s2c_rd, &s2c_wr) != 0) {
        t_putstr("joey: t_pipe FAILED\n");
        return 1;
    }
    const char corvus_name[] = "corvus";
    unsigned int corvus_fds[2] = { (unsigned int)c2s_rd, (unsigned int)s2c_wr };
    long corvus_pid = t_spawn_full(
        corvus_name, sizeof(corvus_name) - 1,
        corvus_fds, 2,
        T_CAP_LOCK_PAGES | T_CAP_CSPRNG_READ);
    if (corvus_pid <= 0) {
        t_putstr("joey: t_spawn_full(\"corvus\") FAILED\n");
        return 1;
    }
    t_close(c2s_rd);
    t_close(s2c_wr);
    t_putstr("joey: spawned /sbin/corvus pid=");
    t_putstr(itoa_dec(corvus_pid, buf, sizeof(buf)));
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

    // === USER_CREATE michael ===
    pl = build_user_create(tx, "michael", 7, pass_michael, sizeof(pass_michael) - 1);
    if (corvus_exchange(c2s_wr, s2c_rd, 5, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: USER_CREATE michael transport FAILED\n");
        return 1;
    }
    if (st != 0 || rlen != 0) {
        t_putstr("joey: USER_CREATE michael returned non-OK status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: USER_CREATE michael ok\n");

    // === USER_CREATE susan === (gives the cross-user C-7 test a real owner)
    pl = build_user_create(tx, "susan", 5, pass_susan, sizeof(pass_susan) - 1);
    if (corvus_exchange(c2s_wr, s2c_rd, 5, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: USER_CREATE susan transport FAILED\n");
        return 1;
    }
    if (st != 0 || rlen != 0) {
        t_putstr("joey: USER_CREATE susan returned non-OK status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: USER_CREATE susan ok\n");

    // === AUTH michael (wrong passphrase) → BadAuth (1) ===
    pl = build_auth(tx, "michael", 7, "wrong-passphrase", 16);
    if (corvus_exchange(c2s_wr, s2c_rd, 1, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
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
    if (corvus_exchange(c2s_wr, s2c_rd, 1, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
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

    // === WRAP users/michael — wrap a known 32-byte DEK ===
    unsigned char dek[32];
    for (int i = 0; i < 32; i++) dek[i] = (unsigned char)(0x40 + i);
    pl = build_wrap(tx, token, "users/michael", 13, 1, dek, 32);
    if (corvus_exchange(c2s_wr, s2c_rd, 10, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
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
    if (corvus_exchange(c2s_wr, s2c_rd, 4, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
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
    // michael's session does not own users/susan; corvus must refuse
    // BEFORE any crypto. The envelope is irrelevant here (the gate fires
    // first) — we reuse michael's envelope as a non-empty placeholder.
    pl = build_unwrap(tx, token, "users/susan", 11, 1, envelope, 1217);
    if (corvus_exchange(c2s_wr, s2c_rd, 4, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
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
    if (corvus_exchange(c2s_wr, s2c_rd, 4, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
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
    // key_id is bound into the envelope AEAD AD; unwrapping the
    // key_id=1 envelope under key_id=999 must fail the AEGIS-256 tag.
    pl = build_unwrap(tx, token, "users/michael", 13, 999, envelope, 1217);
    if (corvus_exchange(c2s_wr, s2c_rd, 4, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
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
    // A structurally-malformed envelope is a client format error →
    // BadFormat, distinct from a tag failure (InternalError).
    unsigned char junk[10];
    for (int i = 0; i < 10; i++) junk[i] = 0xff;
    pl = build_unwrap(tx, token, "users/michael", 13, 1, junk, 10);
    if (corvus_exchange(c2s_wr, s2c_rd, 4, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
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
    if (corvus_exchange(c2s_wr, s2c_rd, 3, token, 33, rx, sizeof(rx), &st, &rlen) != 0) {
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

    // Drop our write side → corvus reads EOF on fd 0 → exits clean.
    t_close(c2s_wr);

    int corvus_status = -1;
    long corvus_reaped = t_wait_pid(&corvus_status);
    if (corvus_reaped != corvus_pid || corvus_status != 0) {
        t_putstr("joey: /sbin/corvus exited non-zero or wrong pid\n");
        return 1;
    }
    t_close(s2c_rd);
    t_putstr("joey: /sbin/corvus reaped status=0; corvus-d hybrid-PKE round-trip verified\n");

    return 0;
}
