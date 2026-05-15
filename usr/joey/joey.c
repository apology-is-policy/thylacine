// /joey — first userspace process; the long-running init.
//
// At P5-spawn-wait, /joey demonstrates orchestration: spawn /hello,
// wait for it, report its status. The supervisor-loop extension (with
// stratumd-stub etc.) lands in subsequent chunks; the syscall surface
// (t_spawn + t_wait_pid) is the prerequisite that this chunk validates
// end-to-end in the production boot path.

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

int main(void) {
    t_putstr("joey: hello from /joey (real userspace binary, loaded from ramfs)\n");

    const char hello_name[] = "hello";
    long pid = t_spawn(hello_name, sizeof(hello_name) - 1);
    if (pid <= 0) {
        t_putstr("joey: t_spawn(\"hello\") FAILED\n");
        return 1;
    }

    char buf[24];
    t_putstr("joey: spawned /hello pid=");
    t_putstr(itoa_dec(pid, buf, sizeof(buf)));
    t_putstr("\n");

    int status = -1;
    long reaped = t_wait_pid(&status);
    if (reaped != pid) {
        t_putstr("joey: t_wait_pid returned wrong pid=");
        t_putstr(itoa_dec(reaped, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    if (status != 0) {
        t_putstr("joey: /hello exited non-zero status=");
        t_putstr(itoa_dec(status, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }

    t_putstr("joey: /hello reaped status=0; orchestration verified\n");

    // P5-corvus-bringup-b: production-shape orchestration of /sbin/corvus.
    //
    // 1. Create a pipe pair for joey -> corvus (c2s) and corvus -> joey
    //    (s2c). Two pipes = 4 fds total in joey's table:
    //       c2s_rd: corvus's stdin (joey gives to corvus as slot 0)
    //       c2s_wr: joey's write side
    //       s2c_rd: joey's read side
    //       s2c_wr: corvus's stdout (joey gives to corvus as slot 1)
    //
    // 2. t_spawn_full passes [c2s_rd, s2c_wr] as the child fd list +
    //    T_CAP_LOCK_PAGES | T_CAP_CSPRNG_READ as the cap mask. The
    //    kernel-side SYS_SPAWN_FULL handler bumps spoor refcounts for
    //    each fd, installs them in the child's slots 0 and 1, AND's
    //    cap_mask with joey's caps, exec's /sbin/corvus.
    //
    // 3. joey closes its copies of the child-side fds (c2s_rd, s2c_wr) —
    //    only the kernel-bumped refs in corvus's table keep those Spoor
    //    sides alive. joey retains c2s_wr (its write to corvus) and
    //    s2c_rd (its read from corvus).
    //
    // 4. Drive AUTH + SESSION_CLOSE round-trips over the wire.
    //
    // 5. Close c2s_wr → drops corvus's rx side to ref=0 → pipe EOF →
    //    corvus's server_loop returns 0 + exits clean.
    //
    // 6. t_wait_pid corvus; assert status=0.

    long c2s_rd, c2s_wr, s2c_rd, s2c_wr;
    if (t_pipe(&c2s_rd, &c2s_wr) != 0) {
        t_putstr("joey: t_pipe(c2s) FAILED\n");
        return 1;
    }
    if (t_pipe(&s2c_rd, &s2c_wr) != 0) {
        t_putstr("joey: t_pipe(s2c) FAILED\n");
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
    // Drop joey's copies of the child-side fds. corvus's slots 0/1 keep
    // the underlying Spoors alive via the kernel's ref bump in
    // SYS_SPAWN_FULL.
    t_close(c2s_rd);
    t_close(s2c_wr);
    t_putstr("joey: spawned /sbin/corvus pid=");
    t_putstr(itoa_dec(corvus_pid, buf, sizeof(buf)));
    t_putstr("\n");

    // Round-trip helpers, inline. read_exact / write_all loop until N
    // bytes are transferred or EOF/error; the kernel's SYS_READ /
    // SYS_WRITE may return short.
    unsigned char rx[3 + 33];   // worst case: status + len + 33-byte token
    unsigned char tx[3 + 1 + 32 + 2 + 256 + 1];  // worst case: USER_CREATE frame

    // === USER_CREATE === (P5-corvus-bringup-c)
    // Build USER_CREATE frame: verb_id=5 + payload_len + (user_len + user +
    // pass_len + pass + backend). Backend=0 (passphrase).
    //
    // This forces corvus through: Argon2id(passphrase, fresh salt) →
    // KEK, generate placeholder keypair, AEGIS-256-wrap, store in
    // in-memory vec. The wall-clock cost is dominated by Argon2id
    // (~few hundred ms for the v1.0 interactive preset).
    const char ucr_user[] = "michael";
    const char ucr_pass[] = "correct-horse-battery-staple-v1";
    unsigned int ucr_user_len = (unsigned int)(sizeof(ucr_user) - 1);
    unsigned int ucr_pass_len = (unsigned int)(sizeof(ucr_pass) - 1);
    unsigned int ucr_payload_len = 1 + ucr_user_len + 2 + ucr_pass_len + 1;
    if (ucr_payload_len > sizeof(tx) - 3) {
        t_putstr("joey: USER_CREATE frame too large for tx buffer\n");
        return 1;
    }
    size_t off = 0;
    tx[off++] = 5;                                       // verb_id = USER_CREATE
    tx[off++] = (unsigned char)(ucr_payload_len & 0xff);
    tx[off++] = (unsigned char)(ucr_payload_len >> 8);
    tx[off++] = (unsigned char)ucr_user_len;
    for (size_t i = 0; i < ucr_user_len; i++) tx[off++] = (unsigned char)ucr_user[i];
    tx[off++] = (unsigned char)(ucr_pass_len & 0xff);
    tx[off++] = (unsigned char)(ucr_pass_len >> 8);
    for (size_t i = 0; i < ucr_pass_len; i++) tx[off++] = (unsigned char)ucr_pass[i];
    tx[off++] = 0;                                       // backend = 0 (passphrase)

    size_t sent = 0;
    while (sent < off) {
        long n = t_write(c2s_wr, &tx[sent], off - sent);
        if (n <= 0) { t_putstr("joey: t_write(USER_CREATE) FAILED\n"); return 1; }
        sent += (size_t)n;
    }

    size_t got = 0;
    while (got < 3) {
        long n = t_read(s2c_rd, &rx[got], 3 - got);
        if (n <= 0) { t_putstr("joey: t_read(USER_CREATE header) FAILED/EOF\n"); return 1; }
        got += (size_t)n;
    }
    unsigned char ucr_status  = rx[0];
    unsigned int  ucr_rsp_len = (unsigned int)rx[1] | ((unsigned int)rx[2] << 8);
    if (ucr_rsp_len > sizeof(rx) - 3) {
        t_putstr("joey: USER_CREATE response payload too large\n");
        return 1;
    }
    while (got < (size_t)(3 + ucr_rsp_len)) {
        long n = t_read(s2c_rd, &rx[got], (size_t)(3 + ucr_rsp_len) - got);
        if (n <= 0) { t_putstr("joey: t_read(USER_CREATE payload) FAILED/EOF\n"); return 1; }
        got += (size_t)n;
    }
    if (ucr_status != 0 || ucr_rsp_len != 0) {
        t_putstr("joey: USER_CREATE returned non-OK status=");
        t_putstr(itoa_dec(ucr_status, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: USER_CREATE michael ok\n");

    // === AUTH (wrong passphrase) ===
    // Should return BadAuth (status=1) because the stored AEGIS-256 wrap
    // won't decrypt under a KEK derived from the wrong passphrase.
    // Exercises the AEAD tag-mismatch branch.
    const char bad_user[] = "michael";
    const char bad_pass[] = "wrong-passphrase";
    unsigned int bad_user_len = (unsigned int)(sizeof(bad_user) - 1);
    unsigned int bad_pass_len = (unsigned int)(sizeof(bad_pass) - 1);
    unsigned int bad_payload_len = 1 + bad_user_len + 2 + bad_pass_len;
    off = 0;
    tx[off++] = 1;                                       // verb_id = AUTH
    tx[off++] = (unsigned char)(bad_payload_len & 0xff);
    tx[off++] = (unsigned char)(bad_payload_len >> 8);
    tx[off++] = (unsigned char)bad_user_len;
    for (size_t i = 0; i < bad_user_len; i++) tx[off++] = (unsigned char)bad_user[i];
    tx[off++] = (unsigned char)(bad_pass_len & 0xff);
    tx[off++] = (unsigned char)(bad_pass_len >> 8);
    for (size_t i = 0; i < bad_pass_len; i++) tx[off++] = (unsigned char)bad_pass[i];

    sent = 0;
    while (sent < off) {
        long n = t_write(c2s_wr, &tx[sent], off - sent);
        if (n <= 0) { t_putstr("joey: t_write(AUTH-bad) FAILED\n"); return 1; }
        sent += (size_t)n;
    }
    got = 0;
    while (got < 3) {
        long n = t_read(s2c_rd, &rx[got], 3 - got);
        if (n <= 0) { t_putstr("joey: t_read(AUTH-bad header) FAILED/EOF\n"); return 1; }
        got += (size_t)n;
    }
    if (rx[0] != 1) {  // STATUS_BAD_AUTH = 1
        t_putstr("joey: AUTH(wrong pass) expected BadAuth(1), got status=");
        t_putstr(itoa_dec(rx[0], buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    unsigned int bad_rsp_len = (unsigned int)rx[1] | ((unsigned int)rx[2] << 8);
    while (got < (size_t)(3 + bad_rsp_len)) {
        long n = t_read(s2c_rd, &rx[got], (size_t)(3 + bad_rsp_len) - got);
        if (n <= 0) { t_putstr("joey: t_read(AUTH-bad drain) FAILED/EOF\n"); return 1; }
        got += (size_t)n;
    }
    t_putstr("joey: AUTH(wrong pass) returned BadAuth (expected)\n");

    // === AUTH === (correct passphrase)
    // Build AUTH frame: verb_id=1 + payload_len + (user_len + user + pass_len + pass).
    // Real crypto: Argon2id + AEGIS-256-unwrap. Reuses USER_CREATE's credentials.
    const char auth_user[] = "michael";
    const char auth_pass[] = "correct-horse-battery-staple-v1";
    unsigned int auth_user_len  = (unsigned int)(sizeof(auth_user)  - 1);
    unsigned int auth_pass_len  = (unsigned int)(sizeof(auth_pass)  - 1);
    unsigned int auth_payload_len = 1 + auth_user_len + 2 + auth_pass_len;
    if (auth_payload_len > sizeof(tx) - 3) {
        t_putstr("joey: AUTH frame too large for tx buffer\n");
        return 1;
    }
    off = 0;
    tx[off++] = 1;                                   // verb_id = AUTH
    tx[off++] = (unsigned char)(auth_payload_len & 0xff);
    tx[off++] = (unsigned char)(auth_payload_len >> 8);
    tx[off++] = (unsigned char)auth_user_len;
    for (size_t i = 0; i < auth_user_len; i++) tx[off++] = (unsigned char)auth_user[i];
    tx[off++] = (unsigned char)(auth_pass_len & 0xff);
    tx[off++] = (unsigned char)(auth_pass_len >> 8);
    for (size_t i = 0; i < auth_pass_len; i++) tx[off++] = (unsigned char)auth_pass[i];

    // write_all to c2s_wr
    sent = 0;
    while (sent < off) {
        long n = t_write(c2s_wr, &tx[sent], off - sent);
        if (n <= 0) { t_putstr("joey: t_write(AUTH) FAILED\n"); return 1; }
        sent += (size_t)n;
    }

    // read_exact 3-byte response header from s2c_rd
    got = 0;
    while (got < 3) {
        long n = t_read(s2c_rd, &rx[got], 3 - got);
        if (n <= 0) { t_putstr("joey: t_read(AUTH header) FAILED/EOF\n"); return 1; }
        got += (size_t)n;
    }
    unsigned char auth_status = rx[0];
    unsigned int  auth_rsp_len = (unsigned int)rx[1] | ((unsigned int)rx[2] << 8);
    if (auth_rsp_len > sizeof(rx) - 3) {
        t_putstr("joey: AUTH response payload too large\n");
        return 1;
    }
    // read_exact payload
    while (got < (size_t)(3 + auth_rsp_len)) {
        long n = t_read(s2c_rd, &rx[got], (size_t)(3 + auth_rsp_len) - got);
        if (n <= 0) { t_putstr("joey: t_read(AUTH payload) FAILED/EOF\n"); return 1; }
        got += (size_t)n;
    }
    if (auth_status != 0) {
        t_putstr("joey: AUTH returned non-OK status=");
        t_putstr(itoa_dec(auth_status, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    if (auth_rsp_len != 33) {
        t_putstr("joey: AUTH OK with unexpected payload_len=");
        t_putstr(itoa_dec((long)auth_rsp_len, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    if (rx[3] != 's') {
        t_putstr("joey: AUTH token doesn't start with 's'\n");
        return 1;
    }
    t_putstr("joey: AUTH ok (token=");
    // Print first 8 token chars (a glimpse, not the whole secret).
    for (int i = 0; i < 8; i++) {
        char c[2] = { (char)rx[3 + i], 0 };
        t_putstr(c);
    }
    t_putstr("...)\n");

    // Save the token for the SESSION_CLOSE follow-up.
    unsigned char saved_token[33];
    for (int i = 0; i < 33; i++) saved_token[i] = rx[3 + i];

    // === SESSION_CLOSE ===
    // Build SESSION_CLOSE frame: verb_id=3 + payload_len=33 + token.
    off = 0;
    tx[off++] = 3;             // verb_id = SESSION_CLOSE
    tx[off++] = 33;            // payload_len lo
    tx[off++] = 0;             // payload_len hi
    for (int i = 0; i < 33; i++) tx[off++] = saved_token[i];

    sent = 0;
    while (sent < off) {
        long n = t_write(c2s_wr, &tx[sent], off - sent);
        if (n <= 0) { t_putstr("joey: t_write(SESSION_CLOSE) FAILED\n"); return 1; }
        sent += (size_t)n;
    }

    got = 0;
    while (got < 3) {
        long n = t_read(s2c_rd, &rx[got], 3 - got);
        if (n <= 0) { t_putstr("joey: t_read(SESSION_CLOSE header) FAILED/EOF\n"); return 1; }
        got += (size_t)n;
    }
    unsigned char sc_status  = rx[0];
    unsigned int  sc_rsp_len = (unsigned int)rx[1] | ((unsigned int)rx[2] << 8);
    if (sc_rsp_len > sizeof(rx) - 3) {
        t_putstr("joey: SESSION_CLOSE response payload too large\n");
        return 1;
    }
    while (got < (size_t)(3 + sc_rsp_len)) {
        long n = t_read(s2c_rd, &rx[got], (size_t)(3 + sc_rsp_len) - got);
        if (n <= 0) { t_putstr("joey: t_read(SESSION_CLOSE payload) FAILED/EOF\n"); return 1; }
        got += (size_t)n;
    }
    if (sc_status != 0 || sc_rsp_len != 0) {
        t_putstr("joey: SESSION_CLOSE returned non-OK status=");
        t_putstr(itoa_dec(sc_status, buf, sizeof(buf)));
        t_putstr(" len=");
        t_putstr(itoa_dec((long)sc_rsp_len, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: SESSION_CLOSE ok\n");

    // Drop our write side → corvus reads EOF on fd 0 → server_loop
    // returns 0 → corvus exits clean.
    t_close(c2s_wr);
    // s2c_rd is still useful for any trailing diagnostics from corvus
    // post-shutdown, but the skeleton doesn't emit any past
    // "server_loop returned EOF". Close it after the wait_pid.

    int corvus_status = -1;
    long corvus_reaped = t_wait_pid(&corvus_status);
    if (corvus_reaped != corvus_pid) {
        t_putstr("joey: t_wait_pid(corvus) returned wrong pid=");
        t_putstr(itoa_dec(corvus_reaped, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    if (corvus_status != 0) {
        t_putstr("joey: /sbin/corvus exited non-zero status=");
        t_putstr(itoa_dec(corvus_status, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_close(s2c_rd);
    t_putstr("joey: /sbin/corvus reaped status=0; AUTH + SESSION_CLOSE round-trip verified\n");

    return 0;
}
