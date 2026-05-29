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

// mem_contains — is `needle` (length nlen) a substring of `hay` (length
// hlen)? A tiny no-libc helper for content-checking relayed child output.
static int mem_contains(const unsigned char *hay, size_t hlen,
                        const char *needle, size_t nlen) {
    if (nlen == 0) return 1;
    if (nlen > hlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen && hay[i + j] == (unsigned char)needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
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

// build_user_create_ext — extended USER_CREATE (A-1b). The base payload plus the
// append-only supp_gids tail: supp_gid_count(1) + supp_gids[count] u32 LE.
static size_t build_user_create_ext(unsigned char *pl,
                                    const char *user, size_t user_len,
                                    const char *pass, size_t pass_len,
                                    const unsigned int *supp, size_t supp_n) {
    size_t o = build_user_create(pl, user, user_len, pass, pass_len);
    pl[o++] = (unsigned char)supp_n;
    for (size_t i = 0; i < supp_n; i++) {
        for (int b = 0; b < 4; b++) pl[o++] = (unsigned char)(supp[i] >> (8 * b));
    }
    return o;
}

// build_resolve_id — RESOLVE_ID payload (verb 11): principal_id u32 LE.
static size_t build_resolve_id(unsigned char *pl, unsigned int id) {
    for (int b = 0; b < 4; b++) pl[b] = (unsigned char)(id >> (8 * b));
    return 4;
}

// build_name1 — single name_len(1) + name payload, shared by RESOLVE_NAME
// (verb 12) and GROUP_CREATE (verb 13).
static size_t build_name1(unsigned char *pl, const char *name, size_t name_len) {
    size_t o = 0;
    pl[o++] = (unsigned char)name_len;
    for (size_t i = 0; i < name_len; i++) pl[o++] = (unsigned char)name[i];
    return o;
}

// rd_u32_le — little-endian u32 read from a response at rx[off].
static unsigned int rd_u32_le(const unsigned char *rx, size_t off) {
    return (unsigned int)rx[off] | ((unsigned int)rx[off + 1] << 8)
         | ((unsigned int)rx[off + 2] << 16) | ((unsigned int)rx[off + 3] << 24);
}

// P6-pouch-stratumd-boot 16c-retire: do_stratumd_stub_bringup (the
// userspace stratumd-stub bringup demo) retired. Real stratumd-via-9P
// is now joey's bring-up path; the stub was the audited interim that
// proved the 9P attach + mount + walk_open + read lifecycle against a
// dev9p simulacrum. Kernel-internal dev9p + stub tests remain in
// kernel/test/ for unit-level coverage of the Dev vtable machinery.

// =============================================================================
// pouch hello smoke (P6-pouch-hello-smoke). The first POSIX C programs
// Thylacine runs — /pouch-hello, /pouch-hello-stdio and
// /pouch-hello-printf, built against the pouch libc (musl's portable
// upper half + the Thylacine-native syscall seam). They are the first
// *runtime* exercise of the auxv startup frame, musl's static CRT,
// SYS_SET_TID_ADDRESS, the write(2) seam, the exit seam, the 0xFFFF
// unimplemented-syscall sentinel, the buffered-stdio backend, and the
// compiler runtime (libclang_rt.builtins.a — binary128 soft-float).
// =============================================================================

// pouch_smoke_one — spawn one pouch test binary with its stdout (fd 1)
// wired to a pipe, verify it exited 0, then relay its output to the
// boot-log UART and check it printed `expect`. Returns 0 / -1.
//
// A pouch binary is a POSIX C program: it writes via SYS_WRITE to fd 1,
// which a plain t_spawn'd child does not have. joey makes a pipe and
// installs its write-end as the child's fd 0 + fd 1 via t_spawn_with_fds
// (a POSIX fd *is* a Thylacine handle index — POUCH-DESIGN.md §6.1). fd 0
// is unused — the hello binaries never read stdin.
//
// Ordering matters: joey REAPS the child (t_wait_pid) BEFORE draining the
// pipe. A Thylacine zombie holds its handle table until proc_free, which
// runs at reap — so the child's write-end stays open (no EOF) until joey
// reaps it. A read-until-EOF before the reap would deadlock. The child's
// output is far under the 4 KiB pipe ring, so it never blocks on write
// and reaches exit on its own; joey reaps, then drains the buffered
// bytes. (Same lesson as do_stratumd_stub_bringup.)
// pouch_smoke_core — pouch_smoke_one's body, parameterized by optional
// cap_mask and optional perm_flags. If both are 0, uses t_spawn_with_fds
// (no extra caps, no perm stamps). If cap_mask != 0 and perm_flags == 0,
// uses t_spawn_full to also hand the child cap_mask (subset of joey's
// caps). If perm_flags != 0, uses t_spawn_with_perms to ALSO stamp the
// permission bits (T_SPAWN_PERM_MAY_POST_SERVICE for the /pouch-hello-
// sockets bind() call, which dispatches to SYS_post_service). All other
// semantics (pipe wiring, reap-before-drain, content check) are
// identical.
// `expect_fault != 0`: invert the exit-status check. The child is
// expected to exit non-zero (e.g., terminated by an EL0 fault routed
// through proc_fault_terminate at kernel/proc.c -- the snare:* family).
// Used only by /pouch-hello-fault.
static int pouch_smoke_core(const char *name, size_t name_len,
                            const char *expect, size_t expect_len,
                            unsigned long cap_mask,
                            unsigned long perm_flags,
                            int expect_fault) {
    long rd = -1, wr = -1;
    if (t_pipe(&rd, &wr) < 0) {
        t_putstr("joey: pouch-smoke t_pipe FAILED\n");
        return -1;
    }
    unsigned int fds[2] = { (unsigned int)wr, (unsigned int)wr };
    long pid;
    if (perm_flags != 0) {
        pid = t_spawn_with_perms(name, name_len, fds, 2, cap_mask, perm_flags);
    } else if (cap_mask != 0) {
        pid = t_spawn_full(name, name_len, fds, 2, cap_mask);
    } else {
        pid = t_spawn_with_fds(name, name_len, fds, 2);
    }
    if (pid <= 0) {
        t_putstr("joey: pouch-smoke spawn FAILED\n");
        (void)t_close(rd);
        (void)t_close(wr);
        return -1;
    }
    // Drop joey's writer ref. The child's two refs remain — released only
    // when the child is reaped below.
    if (t_close(wr) != 0) {
        t_putstr("joey: pouch-smoke t_close(wr) FAILED\n");
        (void)t_close(rd);
        return -1;
    }
    // Reap the child first. t_wait_pid blocks until it zombies; proc_free
    // then drains its handle table, closing the pipe write-end.
    int status = -1;
    long reaped = t_wait_pid(&status);
    if (reaped != pid) {
        t_putstr("joey: pouch-smoke t_wait_pid wrong pid\n");
        (void)t_close(rd);
        return -1;
    }
    // The write-end is now fully closed: drain the buffered output to the
    // boot-log UART, accumulating into `acc` for the content check.
    // 2048 B headroom — pouch-hello-sockets prints ~850 B of test progress
    // lines and the marker "<bin>: exit 0" must land inside the window.
    // Earlier 512 B sized for the leaner pre-sub-chunk-12 pouch binaries;
    // bumped here so the marker is never truncated out (the failure mode
    // looks like "expected marker absent" even when the child exited
    // cleanly).
    unsigned char acc[2048];
    size_t acc_len = 0;
    for (;;) {
        unsigned char buf[256];
        long n = t_read(rd, buf, sizeof(buf));
        if (n < 0) {
            t_putstr("joey: pouch-smoke t_read FAILED\n");
            (void)t_close(rd);
            return -1;
        }
        if (n == 0) break;  // EOF — write side closed at reap
        (void)t_puts((const char *)buf, (size_t)n);
        for (long i = 0; i < n && acc_len < sizeof(acc); i++)
            acc[acc_len++] = buf[i];
    }
    if (t_close(rd) != 0) {
        t_putstr("joey: pouch-smoke t_close(rd) FAILED\n");
        return -1;
    }
    if (expect_fault) {
        // /pouch-hello-fault path: child MUST exit non-zero (terminated
        // by proc_fault_terminate via exits(NOTE_NAME_SNARE_*) at v1.0,
        // collapsed to exit_status = 1 by sys_exits_handler).
        if (status == 0) {
            t_putstr("joey: pouch-smoke expected fault but child exited 0\n");
            return -1;
        }
    } else if (status != 0) {
        t_putstr("joey: pouch-smoke child exited non-zero\n");
        return -1;
    }
    if (!mem_contains(acc, acc_len, expect, expect_len)) {
        t_putstr("joey: pouch-smoke expected marker absent from child output\n");
        return -1;
    }
    return 0;
}

// pouch_smoke_one — default-perms variant. Spawns via t_spawn_with_fds;
// child inherits joey's default cap set (none of the gated caps unless a
// caps variant is used). Pre-existing API; the pouch hellos use this.
static int pouch_smoke_one(const char *name, size_t name_len,
                           const char *expect, size_t expect_len) {
    return pouch_smoke_core(name, name_len, expect, expect_len, 0, 0, 0);
}

// pouch_smoke_one_expect_fault — variant for the durable P6 hardening
// #3a regression. Spawns the child + drains its stdout + expects the
// marker AND expects exit_status != 0. The child terminates because
// arch/arm64/exception.c::exception_sync_lower_el routed the EL0
// fault through proc_fault_terminate(NOTE_NAME_SNARE_*, addr) ->
// exits(name). Pre-#3a (scripture e45a571) the same fault extincted
// the kernel; running this binary would prevent boot from reaching
// `Thylacine boot OK`.
static int pouch_smoke_one_expect_fault(const char *name, size_t name_len,
                                        const char *expect, size_t expect_len) {
    return pouch_smoke_core(name, name_len, expect, expect_len, 0, 0, 1);
}

// pouch_smoke_one_caps — capability-granting variant. Spawns via
// t_spawn_full so the child gets (joey_caps & cap_mask). Used by the
// CAP-gated probes (CAP_CSPRNG_READ for /pouch-hello-getrandom).
static int pouch_smoke_one_caps(const char *name, size_t name_len,
                                const char *expect, size_t expect_len,
                                unsigned long cap_mask) {
    return pouch_smoke_core(name, name_len, expect, expect_len, cap_mask, 0, 0);
}

// pouch_smoke_one_perms — permission-granting variant. Spawns via
// t_spawn_with_perms so the child gets (joey_caps & cap_mask) AND has
// `perm_flags` stamped (PROC_FLAG_MAY_POST_SERVICE for the AF_UNIX
// bind() in /pouch-hello-sockets). The bit is NOT a cap (rfork would
// propagate caps); kernel-stamped at spawn.
static int pouch_smoke_one_perms(const char *name, size_t name_len,
                                 const char *expect, size_t expect_len,
                                 unsigned long cap_mask,
                                 unsigned long perm_flags) {
    return pouch_smoke_core(name, name_len, expect, expect_len,
                            cap_mask, perm_flags, 0);
}

// argv_marker — one substring expected to appear in the child's stdout.
// Used by pouch_smoke_one_argv's content-check loop. R1 F2 fix:
// per-argv-position markers prove every argv[i] pointer was placed
// correctly, not just the last one.
struct argv_marker {
    const char *str;
    size_t      len;
};

// pouch_smoke_one_argv — argv-bearing variant. Spawns via t_spawn_full_argv
// with the caller-constructed argv buffer; same pipe + reap-before-drain
// + content-check pattern as pouch_smoke_core, but routed through the new
// struct-based SYS_SPAWN_FULL_ARGV (P6-pouch-stratumd-boot 16b-alpha) AND
// content-checks every marker in `markers` (not just one).
//
// argv_data + argv_data_len + argc encode the argv exactly as the kernel
// will read it: a flat buffer of `argc` NUL-terminated strings totaling
// argv_data_len bytes, with the final byte == '\0'.
//
// Inherited fds: this variant pre-installs (wr, wr) as fd 0 + fd 1 so the
// child's stdout flows to joey via the pipe (same shape as
// pouch_smoke_core). cap_mask + perm_flags are passed through to the
// kernel's spawn body; both default to 0 in this initial 16b-alpha probe.
//
// R1 F2 fix: markers is an array of `markers_count` substring checks;
// every marker must appear in the child's captured output. A single-
// marker check (the prior shape) only proved the LAST argv pointer was
// placed correctly; per-position markers prove every intermediate
// pointer too.
static int pouch_smoke_one_argv(const char *name, size_t name_len,
                                const char *argv_data, unsigned int argv_data_len,
                                unsigned int argc,
                                const struct argv_marker *markers,
                                size_t markers_count,
                                unsigned long cap_mask,
                                unsigned long perm_flags) {
    long rd = -1, wr = -1;
    if (t_pipe(&rd, &wr) < 0) {
        t_putstr("joey: pouch-smoke-argv t_pipe FAILED\n");
        return -1;
    }
    unsigned int fds[2] = { (unsigned int)wr, (unsigned int)wr };
    struct t_sys_spawn_args req = {
        .name_va       = (unsigned long)name,
        .argv_data_va  = (unsigned long)argv_data,
        .fd_list_va    = (unsigned long)fds,
        .name_len      = (unsigned int)name_len,
        .argv_data_len = argv_data_len,
        .argc          = argc,
        .fd_count      = 2,
        .perm_flags    = (unsigned int)perm_flags,
        ._pad_envp     = 0,
        .cap_mask      = cap_mask,
    };
    long pid = t_spawn_full_argv(&req);
    if (pid <= 0) {
        t_putstr("joey: pouch-smoke-argv spawn FAILED\n");
        (void)t_close(rd);
        (void)t_close(wr);
        return -1;
    }
    // Drop joey's writer ref. Child's two refs remain — released at reap.
    if (t_close(wr) != 0) {
        t_putstr("joey: pouch-smoke-argv t_close(wr) FAILED\n");
        (void)t_close(rd);
        return -1;
    }
    int status = -1;
    long reaped = t_wait_pid(&status);
    if (reaped != pid) {
        t_putstr("joey: pouch-smoke-argv t_wait_pid wrong pid\n");
        (void)t_close(rd);
        return -1;
    }
    // Same buffer size as pouch_smoke_core.
    unsigned char acc[2048];
    size_t acc_len = 0;
    for (;;) {
        unsigned char buf[256];
        long n = t_read(rd, buf, sizeof(buf));
        if (n < 0) {
            t_putstr("joey: pouch-smoke-argv t_read FAILED\n");
            (void)t_close(rd);
            return -1;
        }
        if (n == 0) break;
        (void)t_puts((const char *)buf, (size_t)n);
        for (long i = 0; i < n && acc_len < sizeof(acc); i++)
            acc[acc_len++] = buf[i];
    }
    if (t_close(rd) != 0) {
        t_putstr("joey: pouch-smoke-argv t_close(rd) FAILED\n");
        return -1;
    }
    if (status != 0) {
        t_putstr("joey: pouch-smoke-argv child exited non-zero\n");
        return -1;
    }
    for (size_t i = 0; i < markers_count; i++) {
        if (!mem_contains(acc, acc_len, markers[i].str, markers[i].len)) {
            t_putstr("joey: pouch-smoke-argv missing expected marker: ");
            t_putstr(markers[i].str);
            t_putstr("\n");
            return -1;
        }
    }
    return 0;
}

// do_pouch_hello_smoke — run the pouch POSIX C binaries and verify
// each prints + exits 0. /pouch-hello exercises the raw write(2) seam and
// the 0xFFFF unimplemented-syscall sentinel on both musl syscall paths;
// /pouch-hello-stdio exercises musl's buffered stdio (the patched
// __stdio_write backend); /pouch-hello-printf exercises printf(3), and so
// the compiler runtime (libclang_rt.builtins.a — binary128 soft-float).
// Returns 0 on success, -1 on any failure.
static int do_pouch_hello_smoke(void) {
    static const char ph_name[]   = "pouch-hello";
    static const char ph_expect[] = "pouch-hello: exit 0";
    if (pouch_smoke_one(ph_name, sizeof(ph_name) - 1,
                        ph_expect, sizeof(ph_expect) - 1) != 0)
        return -1;
    t_putstr("joey: pouch-hello smoke ok (write(2) seam + 0xFFFF sentinel both paths)\n");

    static const char ps_name[]   = "pouch-hello-stdio";
    static const char ps_expect[] = "buffer drains at exit";
    if (pouch_smoke_one(ps_name, sizeof(ps_name) - 1,
                        ps_expect, sizeof(ps_expect) - 1) != 0)
        return -1;
    t_putstr("joey: pouch-hello-stdio smoke ok (buffered stdio + patched __stdio_write)\n");

    static const char pp_name[]   = "pouch-hello-printf";
    static const char pp_expect[] = "pouch-hello-printf: exit 0";
    if (pouch_smoke_one(pp_name, sizeof(pp_name) - 1,
                        pp_expect, sizeof(pp_expect) - 1) != 0)
        return -1;
    t_putstr("joey: pouch-hello-printf smoke ok (printf + compiler-rt binary128 soft-float)\n");

    // P6-pouch-mem (7b): the heap-exercising hello — exercises mallocng
    // end-to-end over the 0003-pouch-mman boundary-line patch (which
    // routes __mmap / __munmap onto SYS_BURROW_ATTACH / SYS_BURROW_DETACH).
    // Touches small-slot, calloc, realloc-grow (small + large), and the
    // > MMAP_THRESHOLD individually-mmapped path; verifies byte-level
    // round-trip on every region.
    static const char pm_name[]   = "pouch-hello-malloc";
    static const char pm_expect[] = "pouch-hello-malloc: exit 0";
    if (pouch_smoke_one(pm_name, sizeof(pm_name) - 1,
                        pm_expect, sizeof(pm_expect) - 1) != 0)
        return -1;
    t_putstr("joey: pouch-hello-malloc smoke ok (mallocng over SYS_BURROW_ATTACH / DETACH)\n");

    // P6-pouch-threads (9b): the multi-thread proving binary. Drives
    // pthread_create + pthread_mutex_lock/unlock + pthread_join end-to-end
    // through the patched src/thread/ layer (0004-pouch-pthread) — every
    // boundary-line site exercised in one boot: SYS_THREAD_SPAWN (the
    // clone-call replacement), SYS_THREAD_EXIT (the per-thread exit +
    // clear_child_tid handoff), SYS_TORPOR_WAIT/WAKE (the mutex contention
    // path through __wait/__wake/__timedwait). The race-detector is the
    // counter: any lost increment trips the "COUNT MISMATCH" return.
    // Closes POUCH-DESIGN.md §13's multithreaded-test exit criterion.
    static const char pt_name[]   = "pouch-hello-threads";
    static const char pt_expect[] = "pouch-hello-threads: exit 0";
    if (pouch_smoke_one(pt_name, sizeof(pt_name) - 1,
                        pt_expect, sizeof(pt_expect) - 1) != 0)
        return -1;
    t_putstr("joey: pouch-hello-threads smoke ok (pthread + mutex over SYS_THREAD_* / SYS_TORPOR_*)\n");

    // P6-pouch-poll sub-chunk 10: the polling pouch hello. Exercises the
    // boundary-line patched src/select/{poll,ppoll,select,pselect}.c.
    // SYS_PIPE → devpipe; poll on the read end with timeout + with byte;
    // select() over the same pipe; the zero-fds zero-tv POSIX edge case.
    // Closes POUCH-DESIGN.md §6.3 / §14 sub-chunk 10's exit criterion.
    static const char ppl_name[]   = "pouch-hello-poll";
    static const char ppl_expect[] = "pouch-hello-poll: exit 0";
    if (pouch_smoke_one(ppl_name, sizeof(ppl_name) - 1,
                        ppl_expect, sizeof(ppl_expect) - 1) != 0)
        return -1;
    t_putstr("joey: pouch-hello-poll smoke ok (poll + select over SYS_POLL via devpipe)\n");

    // P6-pouch-devnodes (sub-chunk 11): the getrandom proving binary.
    // Spawned with CAP_CSPRNG_READ so musl's getrandom(2) reaches the
    // SYS_GETRANDOM kernel handler (gated on the cap). This is the path
    // libsodium will consume in sub-chunk 14; proving it here ahead of
    // the libsodium build removes a downstream surprise.
    static const char pg_name[]   = "pouch-hello-getrandom";
    static const char pg_expect[] = "pouch-hello-getrandom: exit 0";
    if (pouch_smoke_one_caps(pg_name, sizeof(pg_name) - 1,
                             pg_expect, sizeof(pg_expect) - 1,
                             T_CAP_CSPRNG_READ) != 0)
        return -1;
    t_putstr("joey: pouch-hello-getrandom smoke ok (musl getrandom(2) over SYS_GETRANDOM with CAP_CSPRNG_READ)\n");

    // P6-pouch-sockets (sub-chunk 12): the AF_UNIX SOCK_STREAM round
    // trip. Spawned with PROC_FLAG_MAY_POST_SERVICE so the server thread's
    // bind() can dispatch to SYS_post_service. Exercises every pouch
    // socket call (socket/bind/listen/accept/connect/getsockopt/close)
    // + read/write through the tagged-fd dispatch shim. Two pthreads in
    // a single Proc — server + client — round-trip "PING\n"/"PONG\n" +
    // verify SO_PEERCRED on both sides. Closes POUCH-DESIGN.md §6.2 /
    // §14 sub-chunk 12's exit criterion.
    static const char po_name[]   = "pouch-hello-sockets";
    static const char po_expect[] = "pouch-hello-sockets: exit 0";
    if (pouch_smoke_one_perms(po_name, sizeof(po_name) - 1,
                              po_expect, sizeof(po_expect) - 1,
                              0,
                              T_SPAWN_PERM_MAY_POST_SERVICE) != 0)
        return -1;
    t_putstr("joey: pouch-hello-sockets smoke ok (AF_UNIX SOCK_STREAM over /srv with SO_PEERCRED)\n");

    // P6-pouch-signals (sub-chunk 13b): the POSIX signal proving binary.
    // Exercises every site of the 0007-pouch-signals boundary-line patch:
    //   - sigaction(SIGINT, &h)       sigaction.c -> __pouch_sigtab + mask adjust
    //   - raise(SIGINT)               raise.c -> SYS_postnote(pid=0=self,"interrupt",9)
    //   - bootstrap __pouch_note_handler dispatches via __pouch_note_name_eq
    //   - SYS_NOTED(NCONT) restores the saved user context, raise() returns 0
    //   - sigaction(SIGINT, SIG_IGN)  bootstrap fast-paths to NCONT without
    //                                  invoking the user handler
    //   - sigaction(SIGUSR1, ...)     -> EINVAL (unsupported signum)
    // Closes POUCH-DESIGN.md §6.4 / §14 sub-chunk 13b's exit criterion.
    static const char psig_name[]   = "pouch-hello-signals";
    static const char psig_expect[] = "pouch-hello-signals: exit 0";
    if (pouch_smoke_one(psig_name, sizeof(psig_name) - 1,
                        psig_expect, sizeof(psig_expect) - 1) != 0)
        return -1;
    t_putstr("joey: pouch-hello-signals smoke ok (sigaction + raise via SYS_NOTIFY/POSTNOTE/NOTED)\n");

    // P6-pouch-libsodium (sub-chunk 14): the libsodium cross-build proving
    // binary. Cross-compiled against pouch's sysroot (which now ships
    // libsodium.a) and run inside Thylacine. Five primitives exercised:
    // sodium_init, SHA-256 KAT, BLAKE2b round-trip, xchacha20-poly1305
    // AEAD round-trip, ed25519 sign + verify + reject-tampered. The CSPRNG
    // path reaches the kernel through getentropy(3) -> getrandom(2) ->
    // SYS_GETRANDOM, which is gated on CAP_CSPRNG_READ — so this binary
    // spawns with the same cap as /pouch-hello-getrandom. Closes
    // POUCH-DESIGN.md §13's libsodium exit criterion.
    static const char psod_name[]   = "pouch-hello-sodium";
    static const char psod_expect[] = "pouch-hello-sodium: exit 0";
    if (pouch_smoke_one_caps(psod_name, sizeof(psod_name) - 1,
                             psod_expect, sizeof(psod_expect) - 1,
                             T_CAP_CSPRNG_READ) != 0)
        return -1;
    t_putstr("joey: pouch-hello-sodium smoke ok (libsodium KATs + AEAD + ed25519)\n");

    // /pouch-hello-argv — proves the new SYS_SPAWN_FULL_ARGV kernel
    // surface (P6-pouch-stratumd-boot sub-chunk 16b-alpha). Construct
    // argv = ["pouch-hello-argv", "alpha", "beta", "gamma"] as a flat
    // NUL-separated buffer; the kernel lays out the System V startup
    // frame (Shape B); pouch's musl _start exposes argv to main; the
    // binary prints each back. joey content-checks the round-trip via
    // per-position markers (R1 F2 fix: prior single-marker check only
    // proved the LAST argv pointer; per-position markers prove every
    // intermediate pointer too) PLUS the NULL-terminator marker (proves
    // the argv[argc]=NULL POSIX guarantee in the binary's argv probe).
    static const char pargv_name[]    = "pouch-hello-argv";
    static const char pargv_argv[]    = "pouch-hello-argv\0alpha\0beta\0gamma";
    static const char pargv_m0[]      = "pouch-hello-argv: argc=4";
    static const char pargv_m1[]      = "pouch-hello-argv: argv[0]=pouch-hello-argv";
    static const char pargv_m2[]      = "pouch-hello-argv: argv[1]=alpha";
    static const char pargv_m3[]      = "pouch-hello-argv: argv[2]=beta";
    static const char pargv_m4[]      = "pouch-hello-argv: argv[3]=gamma";
    static const char pargv_m5[]      = "pouch-hello-argv: argv[argc] NULL terminator ok";
    static const struct argv_marker pargv_markers[] = {
        { pargv_m0, sizeof(pargv_m0) - 1 },
        { pargv_m1, sizeof(pargv_m1) - 1 },
        { pargv_m2, sizeof(pargv_m2) - 1 },
        { pargv_m3, sizeof(pargv_m3) - 1 },
        { pargv_m4, sizeof(pargv_m4) - 1 },
        { pargv_m5, sizeof(pargv_m5) - 1 },
    };
    if (pouch_smoke_one_argv(pargv_name, sizeof(pargv_name) - 1,
                             pargv_argv, sizeof(pargv_argv),
                             /*argc=*/4u,
                             pargv_markers,
                             sizeof(pargv_markers) / sizeof(pargv_markers[0]),
                             /*cap_mask=*/0ul, /*perm_flags=*/0ul) != 0)
        return -1;
    t_putstr("joey: pouch-hello-argv smoke ok (argv pass-through via SYS_SPAWN_FULL_ARGV)\n");

    // /pouch-hello-fault — durable runtime regression for P6 hardening #3a
    // (scripture e45a571 -- docs/ERRORS.md "snare:* family"). The binary
    // deliberately NULL-derefs; the kernel MUST route the EL0 fault
    // through arch/arm64/exception.c::exception_sync_lower_el ->
    // proc_fault_terminate(NOTE_NAME_SNARE_SEGV, 0) -> exits("snare:segv")
    // -- NOT extinct. Joey reaps with exit_status = 1 (v1.0 collapse).
    // The marker "pouch-hello-fault: about to fault" proves the binary
    // actually ran the deref (not just failed to start). Pre-#3a (tip
    // 26e3156 and earlier) this binary would extinct the kernel and
    // boot would never reach `Thylacine boot OK`.
    static const char pflt_name[]   = "pouch-hello-fault";
    static const char pflt_expect[] = "pouch-hello-fault: about to fault";
    if (pouch_smoke_one_expect_fault(pflt_name, sizeof(pflt_name) - 1,
                                     pflt_expect, sizeof(pflt_expect) - 1) != 0)
        return -1;
    t_putstr("joey: pouch-hello-fault smoke ok (EL0 NULL deref terminated cleanly via snare:segv; kernel did NOT extinct)\n");
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

// A-1.7 capability-scoped service storage: idempotent mkdir returning a
// NON-OPENED (O_PATH) walkable handle (FS-delta) -- the valid base for the
// next level AND a valid SYS_CHROOT target. `parent_fd` must itself be a
// non-opened handle (FROM_ROOT, or a prior mkdir_or_open result): 9P
// forbids Twalk from an opened fid. Create the dir (DMDIR) if absent [the
// create returns an OPENED handle, closed at once], then walk_open it with
// T_OPATH. If it already exists (a prior boot), the create fails and the
// T_OPATH walk_open still yields the handle. Returns the dir fd or -1.
static long mkdir_or_open(long parent_fd, const char *name, unsigned long name_len) {
    long cf = t_walk_create(parent_fd, name, name_len, T_OREAD,
                            T_WALK_CREATE_DMDIR | 0755u);
    if (cf >= 0) (void)t_close(cf);
    return t_walk_open(parent_fd, name, name_len, T_OPATH);
}

// A-1.7: corvus bringup -- spawn /sbin/corvus handing it `storage_dup_fd`
// (a R|W-no-TRANSFER storage-root capability) at fd 0, then drive the
// verb-protocol E2E over /srv/corvus. Moved out of main + called
// POST-PIVOT so corvus lands on the persistent Stratum root. Returns 0 on
// success, 1 on any failure (the block's inline `return 1;`s are kept).
static int do_corvus_bringup(long storage_dup_fd) {
    char buf[24];
    // === /sbin/corvus spawn ===
    //
    // No pipes — corvus posts /srv/corvus and joey reaches it via
    // t_srv_connect. joey grants corvus T_SPAWN_PERM_MAY_POST_SERVICE
    // so corvus can call SYS_POST_SERVICE("corvus") at startup. The
    // perm bit is stamped on the child by the kernel atomically inside
    // the spawn thunk (BEFORE exec_setup; P5-corvus-srv-impl-b3a).
    const char corvus_name[] = "corvus";
    unsigned int corvus_fds[1] = { (unsigned int)storage_dup_fd };
    // P5-hostowner-b-b: joey grants corvus T_CAP_GRANT_HOSTOWNER so
    // corvus may write /cap/grant on ADMIN_ELEVATE (the kernel cap
    // device gates the grant write on this fork-grantable bit). joey
    // holds it via CAP_ALL; corvus inherits it through the spawn mask.
    long corvus_pid = t_spawn_with_perms(
        corvus_name, sizeof(corvus_name) - 1,
        corvus_fds, 1,
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
    // A-1b idempotent: on a FRESH pool michael is the bootstrap user (table
    // empty -> USER_CREATE free) and corvus returns OK + {principal_id=1000,
    // primary_gid=1000}. On a PERSISTENT pool (PRESERVE=1, boot N+1) michael was
    // loaded from disk and joey is not yet elevated, so the admin gate returns
    // PermissionDenied -- accepted here; AUTH below proves the RELOADED wrap,
    // and RESOLVE_NAME proves the persisted id. Either path proceeds to AUTH.
    pl = build_user_create(tx, "michael", 7, pass_michael, sizeof(pass_michael) - 1);
    if (corvus_exchange(conn_fd, 5, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: USER_CREATE michael transport FAILED\n");
        return 1;
    }
    if (st == 0) {
        if (rlen != 8) {
            t_putstr("joey: USER_CREATE michael OK but missing {id,gid} payload\n");
            return 1;
        }
        if (rd_u32_le(rx, 3) != 1000u || rd_u32_le(rx, 7) != 1000u) {
            t_putstr("joey: USER_CREATE michael unexpected id/gid (want 1000/1000)\n");
            return 1;
        }
        t_putstr("joey: USER_CREATE michael ok (bootstrap; id=1000 gid=1000)\n");
    } else if (st == 2) {
        t_putstr("joey: USER_CREATE michael already provisioned (persistent pool)\n");
    } else {
        t_putstr("joey: USER_CREATE michael unexpected status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }

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

    // === GROUP_CREATE wheel BEFORE elevation -> PermissionDenied (A-1b gate) ===
    // joey is not yet hostowner; the live-caps gate must refuse regardless of
    // whether "wheel" is a valid/new name. Idempotent across reboots (always 2).
    pl = build_name1(tx, "wheel", 5);
    if (corvus_exchange(conn_fd, 13, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: GROUP_CREATE(pre-elevate) transport FAILED\n");
        return 1;
    }
    if (st != 2) {
        t_putstr("joey: GROUP_CREATE(pre-elevate) expected PermissionDenied(2), got status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: GROUP_CREATE wheel refused pre-elevate PermissionDenied (gate verified)\n");

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
    // susan, WITH supplementary groups [100, 200] -- exercises the A-1b
    // USER_CREATE supp_gids extension; RESOLVE_ID 1001 below round-trips them.
    {
        unsigned int susan_supp[2] = { 100u, 200u };
        pl = build_user_create_ext(tx, "susan", 5, pass_susan,
                                   sizeof(pass_susan) - 1, susan_supp, 2);
    }
    if (corvus_exchange(conn_fd, 5, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: USER_CREATE susan transport FAILED\n");
        return 1;
    }
    if (st == 0) {
        if (rlen != 8) {
            t_putstr("joey: USER_CREATE susan OK but missing {id,gid} payload\n");
            return 1;
        }
        if (rd_u32_le(rx, 3) != 1001u) {
            t_putstr("joey: USER_CREATE susan unexpected id (want 1001)\n");
            return 1;
        }
        t_putstr("joey: USER_CREATE susan ok (id=1001; supp_gids [100,200]; gated)\n");
    } else if (st == 2) {
        t_putstr("joey: USER_CREATE susan already provisioned (persistent pool)\n");
    } else {
        t_putstr("joey: USER_CREATE susan unexpected status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }

    // === A-1b identity verbs: GROUP_CREATE (post-elevate) + RESOLVE round-trips ===
    // GROUP_CREATE wheel: now joey holds CAP_HOSTOWNER. Fresh pool -> OK + a gid
    // distinct from every UPG (shared counter); persistent pool -> BadFormat
    // (already loaded). Either way idempotent.
    pl = build_name1(tx, "wheel", 5);
    if (corvus_exchange(conn_fd, 13, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: GROUP_CREATE(post-elevate) transport FAILED\n");
        return 1;
    }
    if (st == 0) {
        if (rlen != 4) {
            t_putstr("joey: GROUP_CREATE wheel OK but missing gid\n");
            return 1;
        }
        unsigned int gid = rd_u32_le(rx, 3);
        if (gid < 1000u || gid == 1000u || gid == 1001u) {
            t_putstr("joey: GROUP_CREATE wheel gid collides with a UPG\n");
            return 1;
        }
        t_putstr("joey: GROUP_CREATE wheel ok (auto gid distinct from UPGs)\n");
    } else if (st == 5) {
        t_putstr("joey: GROUP_CREATE wheel already exists (persistent pool)\n");
    } else {
        t_putstr("joey: GROUP_CREATE wheel unexpected status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }

    // RESOLVE_NAME michael -> {1000, 1000} (deterministic on FRESH and PERSISTENT).
    pl = build_name1(tx, "michael", 7);
    if (corvus_exchange(conn_fd, 12, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: RESOLVE_NAME michael transport FAILED\n");
        return 1;
    }
    if (st != 0 || rlen != 8 || rd_u32_le(rx, 3) != 1000u || rd_u32_le(rx, 7) != 1000u) {
        t_putstr("joey: RESOLVE_NAME michael wrong status/id/gid (want OK 1000/1000)\n");
        return 1;
    }
    t_putstr("joey: RESOLVE_NAME michael -> {1000,1000} ok\n");

    // RESOLVE_NAME susan -> {1001, 1001}.
    pl = build_name1(tx, "susan", 5);
    if (corvus_exchange(conn_fd, 12, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: RESOLVE_NAME susan transport FAILED\n");
        return 1;
    }
    if (st != 0 || rlen != 8 || rd_u32_le(rx, 3) != 1001u) {
        t_putstr("joey: RESOLVE_NAME susan wrong status/id (want OK 1001)\n");
        return 1;
    }
    t_putstr("joey: RESOLVE_NAME susan -> 1001 ok\n");

    // RESOLVE_ID 1001 -> primary_gid 1001 + supp [100,200] + name "susan".
    // Reply: primary_gid(4) + supp_count(1) + supp[count]*4 + name_len(1) + name.
    pl = build_resolve_id(tx, 1001u);
    if (corvus_exchange(conn_fd, 11, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: RESOLVE_ID 1001 transport FAILED\n");
        return 1;
    }
    if (st != 0 || rlen != 4 + 1 + 8 + 1 + 5) {
        t_putstr("joey: RESOLVE_ID 1001 bad status/len status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    if (rd_u32_le(rx, 3) != 1001u || rx[7] != 2 ||
        rd_u32_le(rx, 8) != 100u || rd_u32_le(rx, 12) != 200u ||
        rx[16] != 5 || rx[17] != 's' || rx[18] != 'u' || rx[19] != 's' ||
        rx[20] != 'a' || rx[21] != 'n') {
        t_putstr("joey: RESOLVE_ID 1001 payload mismatch (want gid 1001, [100,200], susan)\n");
        return 1;
    }
    t_putstr("joey: RESOLVE_ID 1001 -> susan {gid 1001, supp [100,200]} ok\n");

    // RESOLVE_NAME ghost -> NotFound.
    pl = build_name1(tx, "ghost", 5);
    if (corvus_exchange(conn_fd, 12, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: RESOLVE_NAME ghost transport FAILED\n");
        return 1;
    }
    if (st != 3) {
        t_putstr("joey: RESOLVE_NAME ghost expected NotFound(3), got status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: RESOLVE_NAME ghost -> NotFound (expected)\n");

    // RESOLVE_ID 9999 -> NotFound.
    pl = build_resolve_id(tx, 9999u);
    if (corvus_exchange(conn_fd, 11, tx, pl, rx, sizeof(rx), &st, &rlen) != 0) {
        t_putstr("joey: RESOLVE_ID 9999 transport FAILED\n");
        return 1;
    }
    if (st != 3) {
        t_putstr("joey: RESOLVE_ID 9999 expected NotFound(3), got status=");
        t_putstr(itoa_dec(st, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    t_putstr("joey: RESOLVE_ID 9999 -> NotFound (expected)\n");

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

    return 0;
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

    // === /alloc-smoke orchestration (Phase 7 U-2b) ===
    // First native Rust binary that uses the alloc crate (Box / Vec /
    // String / small-alloc loop) backed by libthyla-rs::alloc::ThylaAlloc
    // via SYS_BURROW_ATTACH. Validates the libthyla-rs U-2b uplift
    // sub-chunk end-to-end: t::alloc + t::handle (via Drop) + the
    // burrow_attach syscall wrapper. On success the binary prints
    // "alloc-smoke: ... OK" + exits 0; any failed check prints a
    // tagged FAIL line + exits 1.
    const char alloc_smoke_name[] = "alloc-smoke";
    long as_pid = t_spawn(alloc_smoke_name, sizeof(alloc_smoke_name) - 1);
    if (as_pid <= 0) {
        t_putstr("joey: t_spawn(\"alloc-smoke\") FAILED\n");
        return 1;
    }
    int as_status = -1;
    long as_reaped = t_wait_pid(&as_status);
    if (as_reaped != as_pid || as_status != 0) {
        t_putstr("joey: /alloc-smoke orchestration FAILED\n");
        return 1;
    }
    t_putstr("joey: /alloc-smoke reaped status=0; libthyla-rs::alloc verified\n");

    // === /u-test orchestration (Phase 7 U-2-test) ===
    // Cumulative integration smoke for the libthyla-rs uplift, closes
    // the U-2 arc. Where alloc-smoke isolates each U-2X module's
    // surface in its own block, /u-test runs composed cross-module
    // flows -- the patterns a Utopia builtin will reach for. Six
    // flows: alloc+fs+io, process+pipe, notes+poll+time,
    // thread+torpor+time, ninep codec round-trip, hardware+cap.
    // Output: one "u-test: <flow> OK" per flow + "u-test: all OK".
    const char u_test_name[] = "u-test";
    long ut_pid = t_spawn(u_test_name, sizeof(u_test_name) - 1);
    if (ut_pid <= 0) {
        t_putstr("joey: t_spawn(\"u-test\") FAILED\n");
        return 1;
    }
    int ut_status = -1;
    long ut_reaped = t_wait_pid(&ut_status);
    if (ut_reaped != ut_pid || ut_status != 0) {
        t_putstr("joey: /u-test orchestration FAILED\n");
        return 1;
    }
    t_putstr("joey: /u-test reaped status=0; U-2 arc integration verified\n");

    // === /ut orchestration (Phase 7 U-3 -- Utopia shell skeleton) ===
    // `ut` is the Utopia shell binary; at U-3 its scope is the Pale
    // Fire version banner via libutopia + clean exit. The U-4..U-Z
    // arc fills in the line editor, parser, evaluator, builtins,
    // job control, coreutils, and PTY support. Booting /ut here
    // proves the new usr/utopia/ workspace builds, the binary links
    // against libutopia + libthyla-rs, and the Pale Fire ANSI
    // escapes emit to the boot UART without dirtying the log
    // (terminals that don't grok 24-bit colour degrade gracefully
    // per UTOPIA-VISUAL.md section 4.4).
    const char ut_name[] = "ut";
    long ut_shell_pid = t_spawn(ut_name, sizeof(ut_name) - 1);
    if (ut_shell_pid <= 0) {
        t_putstr("joey: t_spawn(\"ut\") FAILED\n");
        return 1;
    }
    int ut_shell_status = -1;
    long ut_shell_reaped = t_wait_pid(&ut_shell_status);
    if (ut_shell_reaped != ut_shell_pid || ut_shell_status != 0) {
        t_putstr("joey: /ut orchestration FAILED\n");
        return 1;
    }
    t_putstr("joey: /ut reaped status=0; Utopia shell skeleton verified\n");

    // === pouch hello smoke (P6-pouch-hello-smoke) ===
    // The first POSIX C programs Thylacine runs, built against the pouch
    // libc. Placed here, beside the /hello orchestration, so the two
    // spawn-and-verify milestones sit together on the boot path.
    if (do_pouch_hello_smoke() != 0) return 1;

    // === torpor SVC-dispatch smoke (P6-pouch-wait-addr, sub-chunk 8) ===
    // The kernel-side `torpor` wait-on-address primitive (the futex-
    // equivalent over which pouch's pthread layer will run at sub-chunk
    // 9). Joey runs single-threaded — the no-lost-wakeup race + the
    // wake-finds-waiter chain are covered by the kernel test suite's
    // torpor.* tests — so here we exercise only that the SVC numbers
    // (39 / 40) are wired and the fast paths return what they should.
    //
    // wait: pass an unmapped user VA (joey's heap doesn't include
    //   0x10000000) → -EFAULT. Validates the SVC entry + arg dispatch
    //   + uaccess fault routing.
    // wake: an empty bucket at the same address → 0. Validates the
    //   WAKE SVC dispatch + the empty-bucket bypass.
    {
        unsigned int *probe_addr = (unsigned int *)(unsigned long)0x10000000ul;
        long wait_rc = t_torpor_wait(probe_addr, 0u, -1);
        if (wait_rc != (long)T_TORPOR_ERR_EFAULT) {
            t_putstr("joey: torpor SVC-dispatch FAILED — wait did not return EFAULT\n");
            return 1;
        }
        long wake_rc = t_torpor_wake(probe_addr, 100u);
        if (wake_rc != 0) {
            t_putstr("joey: torpor SVC-dispatch FAILED — wake on empty bucket did not return 0\n");
            return 1;
        }
        t_putstr("joey: torpor SVC-dispatch ok (wait→EFAULT, wake→0 on empty bucket)\n");
    }

    // === thread-spawn smoke (P6-pouch-threads sub-chunk 9a) ===
    // SVC-dispatch fast paths for SYS_THREAD_SPAWN + SYS_THREAD_EXIT.
    // joey runs single-threaded — the end-to-end spawn-and-join chain
    // is exercised by /thread-probe below; here we only verify the
    // kernel rejects bad args.
    //
    // entry=0: -EINVAL (-22 — kernel rejects null entry)
    // entry misaligned: -EINVAL (F2 audit close — prevents EC_PC_ALIGN ELE)
    // sp_va misaligned: -EINVAL
    // entry above USER_VA_TOP: -EINVAL
    {
        long rc;
        // entry = 0 → -EINVAL
        rc = t_thread_spawn((void (*)(void *))0, (void *)0x80000000ul,
                            (void *)0, (void *)0);
        if (rc != -22) {
            t_putstr("joey: thread-spawn SVC-dispatch FAILED — null entry not rejected\n");
            return 1;
        }
        // entry not 4-byte aligned → -EINVAL (F2 regression: without
        // this gate, the eret would trigger EC_PC_ALIGN at EL1 → ELE).
        rc = t_thread_spawn((void (*)(void *))0x100001ul,
                            (void *)0x80000000ul,
                            (void *)0, (void *)0);
        if (rc != -22) {
            t_putstr("joey: thread-spawn SVC-dispatch FAILED — misaligned entry not rejected (F2)\n");
            return 1;
        }
        // sp_va not 16-aligned → -EINVAL
        rc = t_thread_spawn((void (*)(void *))0x100000ul,
                            (void *)0x80000008ul,    // 8-aligned but not 16
                            (void *)0, (void *)0);
        if (rc != -22) {
            t_putstr("joey: thread-spawn SVC-dispatch FAILED — misaligned sp not rejected\n");
            return 1;
        }
        // entry above USER_VA_TOP (1<<47) → -EINVAL
        rc = t_thread_spawn((void (*)(void *))0x800000000000ul,
                            (void *)0x80000000ul,
                            (void *)0, (void *)0);
        if (rc != -22) {
            t_putstr("joey: thread-spawn SVC-dispatch FAILED — entry above USER_VA_TOP not rejected\n");
            return 1;
        }
        t_putstr("joey: thread-spawn SVC-dispatch ok (null/misaligned entry / misaligned sp / OOB entry rejected)\n");
    }

    // === /thread-probe end-to-end ===
    // The thread-spawn + join chain proven at EL0. /thread-probe
    // (usr/thread-probe/thread-probe.c) does:
    //   - SYS_THREAD_SPAWN a worker that writes a shared counter
    //   - kernel exit-time clear+wake the worker's tidptr
    //   - main joins via t_torpor_wait, verifies the counter
    // Output goes to the UART via t_putstr; no pipe orchestration.
    {
        const char tp_name[] = "thread-probe";
        long tp_pid = t_spawn(tp_name, sizeof(tp_name) - 1);
        if (tp_pid <= 0) {
            t_putstr("joey: t_spawn(\"thread-probe\") FAILED\n");
            return 1;
        }
        int tp_status = -1;
        long tp_reaped = t_wait_pid(&tp_status);
        if (tp_reaped != tp_pid || tp_status != 0) {
            t_putstr("joey: /thread-probe orchestration FAILED\n");
            return 1;
        }
        t_putstr("joey: /thread-probe reaped status=0; SYS_THREAD_SPAWN/EXIT verified\n");
    }

    // === /sbin/corvus spawn + E2E ===
    // Moved to do_corvus_bringup() (defined above main) and called
    // POST-PIVOT below, so corvus lands on the persistent Stratum root
    // and receives its storage capability at fd 0 (A-1.7).

    // P6-pouch-stratumd-boot 16b-gamma sanity: walk /system.key from the
    // ramfs root via t_walk_open + t_fstat from joey itself. This isolates
    // whether the kernel-side SYS_FSTAT + SYS_WALK_OPEN(FROM_ROOT) work
    // BEFORE attempting stratumd's pouch-musl-mediated open+fstat. If
    // joey's direct call fails here, the kernel surface is broken; if it
    // succeeds but stratumd still fails, the pouch musl arm is the issue.
    {
        static const char sk_name[] = "system.key";
        long sk_fd = t_walk_open(T_WALK_OPEN_FROM_ROOT, sk_name,
                                  sizeof(sk_name) - 1, T_OREAD);
        if (sk_fd < 0) {
            t_putstr("joey: probe /system.key walk_open FAILED\n");
            return 1;
        }
        struct t_stat sk_st;
        long sk_rc = t_fstat(sk_fd, &sk_st);
        if (sk_rc != 0) {
            t_putstr("joey: probe /system.key fstat FAILED rc=");
            t_putstr(itoa_dec(sk_rc, buf, sizeof(buf)));
            t_putstr("\n");
            (void)t_close(sk_fd);
            return 1;
        }
        t_putstr("joey: probe /system.key fstat OK size=");
        t_putstr(itoa_dec((long)sk_st.size, buf, sizeof(buf)));
        t_putstr(" mode=0o");
        // print octal mode manually (itoa_dec is decimal)
        {
            unsigned int m = sk_st.mode;
            char obuf[12];
            int oi = (int)sizeof(obuf) - 1;
            obuf[oi--] = '\0';
            if (m == 0) obuf[oi--] = '0';
            while (m && oi >= 0) {
                obuf[oi--] = (char)('0' + (m & 7));
                m >>= 3;
            }
            t_putstr(&obuf[oi + 1]);
        }
        t_putstr("\n");
        // Also exercise t_lseek SEEK_END to verify the size path.
        long sk_end = t_lseek(sk_fd, 0, T_SEEK_END);
        if (sk_end != (long)sk_st.size) {
            t_putstr("joey: probe /system.key lseek SEEK_END mismatch (got ");
            t_putstr(itoa_dec(sk_end, buf, sizeof(buf)));
            t_putstr(")\n");
            (void)t_close(sk_fd);
            return 1;
        }
        long sk_set = t_lseek(sk_fd, 0, T_SEEK_SET);
        if (sk_set != 0) {
            t_putstr("joey: probe /system.key lseek SEEK_SET FAILED rc=");
            t_putstr(itoa_dec(sk_set, buf, sizeof(buf)));
            t_putstr("\n");
            (void)t_close(sk_fd);
            return 1;
        }
        (void)t_close(sk_fd);
        t_putstr("joey: probe /system.key lseek SEEK_END/SEEK_SET OK\n");
    }

    // === stratumd boot pool mount (P6-pouch-stratumd-boot sub-chunk 16b-gamma) ===
    //
    // The real end-to-end integration: spawn stratumd with argv pointing at
    // the pre-generated pool fixture, CAP_HW_CREATE granted from joey, and
    // SPAWN_PERM_MAY_POST_SERVICE for bind() onto /srv/stratum-fs. stratumd
    // claims the virtio-mmio bank via bdev_thylacine.c (Stratum-side arm on
    // the thylacine-pouch-arm branch), finds the second virtio-blk-device
    // (the pool backing; QEMU virt slot 31 — HIGH-to-LOW scan picks the
    // pool first), reads pool.img's superblock via the bdev I/O ops,
    // mounts the FS, accepts the system.key wrap-keyfile from the literal
    // ramfs file at /system.key (16b-gamma scope reduction: flat-cpio
    // root placement; FHS-shaped /etc/stratum/ is a v1.x lift atop the
    // devramfs subdir walk), and binds /srv/stratum-fs.
    //
    // 16b-gamma closes this: kernel SYS_FSTAT + SYS_LSEEK exposed at the
    // pouch musl ABI (patches 0010-pouch-fstat-lseek); stm_keyfile_load
    // open/fstat/peek/lseek/read sequence succeeds end-to-end.
    //
    // Joey verifies the bind by attempting t_srv_connect on the service
    // name. A bounded-retry loop tolerates the race between t_spawn_full_argv
    // returning and stratumd's libsodium init + bdev_thylacine init +
    // pthread spawn + mount + bind reaching the kernel's /srv registry.
    //
    // Earlier 16a probe (t_spawn("stratumd") + reap-on-usage-exit) is
    // SUBSUMED by this one: the new probe spawning succeeded confirms the
    // binary loads + libc init ran + argv parsing executed (the 16a
    // contract); successful t_srv_connect after spawn confirms mount + bind
    // (the additional 16b-gamma contract). No need for both.
    //
    // stratumd is NOT reaped here. Joey continues to do_stratumd_stub_-
    // bringup() afterwards (the stub demo uses pipes, not /srv, so no
    // conflict). At joey exit, the kernel will reap stratumd as part of
    // parent-exit child cleanup. 16c retires the stub demo and pivots
    // joey to use stratumd's real FS.
    {
        // argv = { "stratumd", "/dev/virtio-blk", "--listen",
        //          "/srv/stratum-fs", "--keyfile", "/system.key" }.
        // The flat buffer concatenates with explicit NUL terminators;
        // the trailing C-implicit NUL is excluded via the sizeof - 1.
        // bdev_thylacine.c ignores the path argument (slot determined by
        // virtio-mmio scan), so "/dev/virtio-blk" is purely informational.
        static const char sd_name[] = "stratumd";
        static const char sd_argv_data[] =
            "stratumd\0"
            "/dev/virtio-blk\0"
            "--listen\0"
            "/srv/stratum-fs\0"
            "--keyfile\0"
            "/system.key\0";
        // 6 strings: "stratumd" (9) + "/dev/virtio-blk" (16) +
        // "--listen" (9) + "/srv/stratum-fs" (16) + "--keyfile" (10) +
        // "/system.key" (12) = 72 bytes.
        struct t_sys_spawn_args sd_req = {
            .name_va       = (unsigned long)sd_name,
            .argv_data_va  = (unsigned long)sd_argv_data,
            .fd_list_va    = 0,
            .name_len      = sizeof(sd_name) - 1,
            .argv_data_len = sizeof(sd_argv_data) - 1,
            .argc          = 6,
            .fd_count      = 0,
            .perm_flags    = (unsigned int)T_SPAWN_PERM_MAY_POST_SERVICE,
            ._pad_envp     = 0,
            // F1 fix validation (P6 hardening #2): grant CAP_CSPRNG_READ
            // so stratumd's libsodium sodium_init reaches getrandom
            // successfully and progresses into stm_fs_mount. Pre-F1-fix,
            // this exposed the AEGIS-256 / mallocng heap-corruption bug
            // (kernel extinction via FAULT_UNHANDLED_USER on the stale-
            // PTE-routed access). With F1's mmu_uninstall_user_range +
            // 0012's mallocng-assert -> _Exit(127), the boot should now
            // either reach `joey: stratumd-boot /srv/stratum-fs bound
            // after retry N` (F1 was the root cause) or reach
            // `child exit_status=127` (F1 wasn't, mallocng still asserts
            // -- but cleanly, NOT as kernel extinction).
            .cap_mask      = T_CAP_HW_CREATE | T_CAP_CSPRNG_READ,
        };

        // Install a pipe whose write-end becomes stratumd's fd 1 + fd 2
        // (both stdout and stderr). Joey reads from the rd-end to capture
        // any output stratumd produces during startup / mount. This is
        // the debugging substitute for SYS_PUTS direct (which doesn't seem
        // to surface from pouch-built procs running an init_array
        // constructor in this kernel).
        long sd_rd = -1, sd_wr = -1;
        if (t_pipe(&sd_rd, &sd_wr) < 0) {
            t_putstr("joey: stratumd-boot t_pipe FAILED\n");
            return 1;
        }
        // fd 0 (stdin), fd 1 (stdout), fd 2 (stderr) all point at the
        // pipe's write-end so stratumd's libc stderr writes are captured.
        unsigned int sd_fds[3] = { (unsigned int)sd_wr, (unsigned int)sd_wr,
                                    (unsigned int)sd_wr };
        sd_req.fd_list_va = (unsigned long)sd_fds;
        sd_req.fd_count = 3;

        long sd_pid = t_spawn_full_argv(&sd_req);
        if (sd_pid <= 0) {
            t_putstr("joey: stratumd-boot t_spawn_full_argv FAILED\n");
            (void)t_close(sd_rd);
            (void)t_close(sd_wr);
            return 1;
        }
        // Drop joey's wr ref. Child's two refs remain. When stratumd
        // exits, the last wr ref drops; subsequent read on rd returns 0
        // (EOF) instead of blocking.
        if (t_close(sd_wr) != 0) {
            t_putstr("joey: stratumd-boot t_close(wr) FAILED\n");
            (void)t_close(sd_rd);
            return 1;
        }

        // Retry t_srv_connect with a bounded busy-wait between attempts,
        // so stratumd has time to libsodium-init + bdev_thylacine MMIO
        // claim + pool mount + pthread spawn + listen + bind.
        //
        // 16b-gamma-syscalls (this checkpoint) lands the SYS_FSTAT +
        // SYS_LSEEK kernel surfaces + the pouch open() -> openat()
        // redirect + the devramfs walk reuse-nc + the partial-walk
        // reject. stratumd reaches stm_fs_mount and successfully:
        //   - opens /system.key
        //   - fstats it (size 3656)
        //   - read-peeks the magic header
        //   - keyfile_load returns OK
        //   - stm_bdev_open(THYLACINE) claims MMIO + DMA + IRQ + does
        //     VirtIO init + reports capacity = 64 MiB
        //   - stm_sb_mount_scan fails STM_ENOENT — bdev_thylacine's
        //     read I/O isn't returning the pool's on-disk uberblocks
        //     to Stratum.
        //
        // The bdev-I/O block is a separate v1.x sub-chunk (16b-gamma-
        // mount-close). This probe stays NON-FATAL until that lands.
        static const char srv_name[] = "stratum-fs";
        long sd_srv_fd = -1;
        // 16c: between retries joey sleeps on torpor with a 1 ms timeout
        // (a never-woken local-stack u32 + expected-value 0). This
        // (a) yields the vCPU back to the scheduler so stratumd's worker
        // threads make progress instead of fighting joey's busy-spin under
        // QEMU TCG emulation; (b) gives a real, kernel-timed pacing that
        // is consistent across hosts. Pre-fix joey ran a 1 M-nop volatile
        // loop per retry which clocked ~30 ms wall under TCG (so 170
        // retries -> ~5.1 s wall, while real work would be sub-second).
        // 3000 retries * 1 ms = ~3 s outer bound; typical bind observed
        // well under retry 500 with the yielding pattern.
        // R1 F12 close: this is a sleep-only sentinel for torpor_wait's
        // wait-on-address contract; not a signaling word. The pacer name
        // documents intent so a future grepper doesn't confuse it with
        // an actual sync primitive. Never read by t_torpor_wake; the
        // wait always times out after 1 ms and the retry loops.
        unsigned int joey_retry_pacer_dummy = 0;
        for (int i = 0; i < 3000; i++) {
            sd_srv_fd = t_srv_connect(srv_name, sizeof(srv_name) - 1, 0, 0);
            if (sd_srv_fd >= 0) {
                t_putstr("joey: stratumd-boot /srv/stratum-fs bound after retry ");
                t_putstr(itoa_dec(i, buf, sizeof(buf)));
                t_putstr(" (pool mounted via bdev_thylacine)\n");
                break;
            }
            // Drain stratumd's stderr pipe each retry (non-blocking
            // poll). Two purposes: (1) surface stratumd's diagnostics
            // (assertion msgs, libsodium init traces, bdev errors) in
            // the boot log; (2) keep the pipe buffer from filling --
            // a full pipe buffer would block stratumd's next stderr
            // write, which would in turn keep stratumd from progressing
            // toward bind. The poll timeout of 0 makes this strictly
            // non-blocking; an empty pipe returns immediately.
            for (;;) {
                struct pollfd pfd = { .fd = (int)sd_rd, .events = POLLIN, .revents = 0 };
                long pr = t_poll(&pfd, 1, 0);
                if (pr <= 0 || !(pfd.revents & POLLIN)) break;
                unsigned char rd_buf[256];
                long n = t_read(sd_rd, rd_buf, sizeof(rd_buf));
                if (n <= 0) break;
                (void)t_puts((const char *)rd_buf, (size_t)n);
            }
            (void)t_torpor_wait(&joey_retry_pacer_dummy, 0, 1000); // sleep 1 ms
        }

        if (sd_srv_fd < 0) {
            t_putstr("joey: stratumd-boot /srv/stratum-fs not bound after retries (FATAL at 16c)\n");
            // Bounded-drain stratumd's stderr so any final diagnostic
            // msg lands in the boot log. No t_wait_pid here: stratumd
            // is a long-running daemon (16c+) and never zombifies, so
            // wait_pid would hang forever. The 16c v1.x lift is a
            // kernel `wait_pid_for(pid)` so kproc.wait_pid skips the
            // stratumd pid; until then, an unreaped stratumd at
            // joey-userspace exit reparents to kproc and the
            // kproc.wait_pid sees stratumd first, extincting on
            // "wrong pid" -- but here we're exiting non-zero anyway,
            // which the kernel handles via the joey-exit-non-zero
            // path (NOT wait_pid).
            for (int drain_i = 0; drain_i < 200; drain_i++) {
                struct pollfd pfd = { .fd = (int)sd_rd, .events = POLLIN, .revents = 0 };
                long pr = t_poll(&pfd, 1, 10);
                if (pr <= 0) break;
                if (!(pfd.revents & POLLIN)) break;
                unsigned char rd_buf[256];
                long n = t_read(sd_rd, rd_buf, sizeof(rd_buf));
                if (n <= 0) break;
                (void)t_puts((const char *)rd_buf, (size_t)n);
            }
            (void)t_close(sd_rd);
            return 1;
        } else {
            // Connection works. Drive 9P attach + pivot bringup
            // (P6-pouch-stratumd-boot 16c).
            //
            // 1. SYS_ATTACH_9P_SRV — wrap byte-mode SrvConn in kernel
            //    9P client; mints a KOBJ_SPOOR for the FS root.
            // 2. Pre-pivot probe — walk + read the /thylacine-version
            //    sentinel through the attach handle, verifying the
            //    end-to-end 9P plumb (Twalk + Tlopen + Tread).
            // 3. SYS_PIVOT_ROOT — atomic root_spoor swap; old devramfs
            //    root unrefs; new disk-backed root installs.
            // 4. Post-pivot probe — walk + read the sentinel via
            //    T_WALK_OPEN_FROM_ROOT, confirming the pivot took.
            long sd_attach_fd = t_attach_9p_srv(sd_srv_fd, "", 0, 0);
            if (sd_attach_fd < 0) {
                t_putstr("joey: stratumd-boot t_attach_9p_srv FAILED\n");
                return 1;
            }
            t_putstr("joey: stratumd-boot t_attach_9p_srv OK; attach_fd=");
            t_putstr(itoa_dec(sd_attach_fd, buf, sizeof(buf)));
            t_putstr("\n");
            // The kernel 9P client now owns the SrvConn; userspace can
            // release its client handle. (The adapter takes its own
            // srvconn_ref; SrvConn lives until both refs drop.)
            if (t_close(sd_srv_fd) != 0) {
                t_putstr("joey: stratumd-boot t_close(sd_srv_fd) post-attach FAILED\n");
                return 1;
            }

            static const char sentinel_name[] = "thylacine-version";
            const size_t sentinel_len = sizeof(sentinel_name) - 1;

            long pre_fd = t_walk_open(sd_attach_fd, sentinel_name,
                                       sentinel_len, T_OREAD);
            if (pre_fd < 0) {
                t_putstr("joey: stratumd-boot pre-pivot t_walk_open(/thylacine-version) FAILED rc=");
                t_putstr(itoa_dec(pre_fd, buf, sizeof(buf)));
                t_putstr("\n");
                return 1;
            }
            unsigned char pre_buf[128];
            long pre_n = t_read(pre_fd, pre_buf, sizeof(pre_buf));
            if (pre_n <= 0) {
                t_putstr("joey: stratumd-boot pre-pivot t_read FAILED rc=");
                t_putstr(itoa_dec(pre_n, buf, sizeof(buf)));
                t_putstr("\n");
                (void)t_close(pre_fd);
                return 1;
            }
            (void)t_close(pre_fd);
            t_putstr("joey: stratumd-boot pre-pivot /thylacine-version OK (");
            t_putstr(itoa_dec(pre_n, buf, sizeof(buf)));
            t_putstr(" bytes)\n");

            if (t_pivot_root(sd_attach_fd) != 0) {
                t_putstr("joey: stratumd-boot t_pivot_root FAILED\n");
                return 1;
            }
            if (t_close(sd_attach_fd) != 0) {
                t_putstr("joey: stratumd-boot t_close(sd_attach_fd) FAILED\n");
                return 1;
            }

            long post_fd = t_walk_open(T_WALK_OPEN_FROM_ROOT, sentinel_name,
                                        sentinel_len, T_OREAD);
            if (post_fd < 0) {
                t_putstr("joey: stratumd-boot post-pivot t_walk_open(FROM_ROOT, /thylacine-version) FAILED rc=");
                t_putstr(itoa_dec(post_fd, buf, sizeof(buf)));
                t_putstr("\n");
                return 1;
            }
            unsigned char post_buf[128];
            long post_n = t_read(post_fd, post_buf, sizeof(post_buf));
            if (post_n <= 0) {
                t_putstr("joey: stratumd-boot post-pivot t_read FAILED\n");
                (void)t_close(post_fd);
                return 1;
            }
            (void)t_close(post_fd);
            t_putstr("joey: stratumd-boot post-pivot /thylacine-version OK (");
            t_putstr(itoa_dec(post_n, buf, sizeof(buf)));
            t_putstr(" bytes); root now disk-backed Stratum FS\n");

            // === FS-mutation foundation E2E (FS-alpha + FS-beta) ===
            // Exercise SYS_WALK_CREATE + SYS_WRITE + SYS_FSYNC + SYS_READDIR
            // end-to-end against the real disk-backed Stratum FS (through the
            // syscall handlers + dev9p_create/_fsync/_readdir + stratumd).
            {
                const char probe_name[] = "fs-mut-probe.txt";
                const char probe_data[] = "FS-MUT-OK\n";
                // Idempotent: the Stratum pool is PERSISTENT across reboots,
                // so open-or-create. First boot on a fresh pool creates +
                // writes + fsyncs; a reboot finds the persisted file and just
                // verifies it reads back. A non-idempotent create would
                // EEXIST-fail (boot-fatally) on every reboot -- and this way
                // the probe doubles as a cross-reboot persistence regression
                // test (the durable bytes survive a power cycle).
                long rf = t_walk_open(T_WALK_OPEN_FROM_ROOT, probe_name,
                                      sizeof(probe_name) - 1, T_OREAD);
                if (rf < 0) {
                    // Not present (fresh pool) -- create + write + fsync.
                    long cf = t_walk_create(T_WALK_OPEN_FROM_ROOT, probe_name,
                                            sizeof(probe_name) - 1, T_OWRITE, 0644u);
                    if (cf < 0) {
                        t_putstr("joey: fs-mut create FAILED rc=");
                        t_putstr(itoa_dec(cf, buf, sizeof(buf)));
                        t_putstr("\n");
                        return 1;
                    }
                    if (write_all(cf, (const unsigned char *)probe_data,
                                  sizeof(probe_data) - 1) != 0) {
                        t_putstr("joey: fs-mut write FAILED\n");
                        return 1;
                    }
                    if (t_fsync(cf, 0) != 0) {
                        t_putstr("joey: fs-mut fsync FAILED\n");
                        return 1;
                    }
                    (void)t_close(cf);
                    rf = t_walk_open(T_WALK_OPEN_FROM_ROOT, probe_name,
                                     sizeof(probe_name) - 1, T_OREAD);
                    if (rf < 0) {
                        t_putstr("joey: fs-mut reopen FAILED rc=");
                        t_putstr(itoa_dec(rf, buf, sizeof(buf)));
                        t_putstr("\n");
                        return 1;
                    }
                }
                unsigned char rback[32];
                long rn = t_read(rf, rback, sizeof(rback));
                (void)t_close(rf);
                if (rn != (long)(sizeof(probe_data) - 1)) {
                    t_putstr("joey: fs-mut read-back wrong len\n");
                    return 1;
                }
                for (long i = 0; i < rn; i++) {
                    if (rback[i] != (unsigned char)probe_data[i]) {
                        t_putstr("joey: fs-mut read-back content mismatch\n");
                        return 1;
                    }
                }
                t_putstr("joey: fs-mut create/persist + read-back OK\n");

                // mkdir-via-DMDIR + readdir, also idempotent (open-or-create
                // the dir; Treaddir cookie-advance: a non-empty first run must
                // reach EOD on the next call).
                const char probe_dir[] = "fs-mut-dir";
                long cd = t_walk_open(T_WALK_OPEN_FROM_ROOT, probe_dir,
                                      sizeof(probe_dir) - 1, T_OREAD);
                if (cd < 0) {
                    cd = t_walk_create(T_WALK_OPEN_FROM_ROOT, probe_dir,
                                       sizeof(probe_dir) - 1, T_OREAD,
                                       T_WALK_CREATE_DMDIR | 0755u);
                    if (cd < 0) {
                        t_putstr("joey: fs-mut mkdir FAILED rc=");
                        t_putstr(itoa_dec(cd, buf, sizeof(buf)));
                        t_putstr("\n");
                        return 1;
                    }
                }
                unsigned char dbuf[256];
                long d1 = t_readdir(cd, dbuf, sizeof(dbuf));
                if (d1 < 0) {
                    t_putstr("joey: fs-mut readdir FAILED rc=");
                    t_putstr(itoa_dec(d1, buf, sizeof(buf)));
                    t_putstr("\n");
                    (void)t_close(cd);
                    return 1;
                }
                if (d1 > 0) {
                    // Cookie advanced -> the next readdir must report EOD.
                    long d2 = t_readdir(cd, dbuf, sizeof(dbuf));
                    if (d2 != 0) {
                        t_putstr("joey: fs-mut readdir EOD expected 0, got rc=");
                        t_putstr(itoa_dec(d2, buf, sizeof(buf)));
                        t_putstr("\n");
                        (void)t_close(cd);
                        return 1;
                    }
                }
                (void)t_close(cd);
                t_putstr("joey: fs-mut mkdir+readdir OK (d1=");
                t_putstr(itoa_dec(d1, buf, sizeof(buf)));
                t_putstr(" bytes)\n");
            }

            // === FS-gamma E2E (SYS_RENAME + SYS_UNLINK) ===
            // rename (atomic replace) + unlink + rmdir against the real Stratum
            // FS. Each step cleans up first AND after, so the sequence is fully
            // idempotent across the persistent pool (no artifact survives the
            // boot) -- the cleanup-first unlink also clears anything a crashed
            // prior boot left behind.
            {
                const char gsrc[] = "fs-gamma-src.txt";
                const char gdst[] = "fs-gamma-dst.txt";
                const char src_data[] = "RENAMED-SRC\n";
                const char dst_data[] = "OLD-DST-DATA\n";

                // Cleanup-first: drop any stale src/dst from a crashed boot.
                (void)t_unlink(T_WALK_OPEN_FROM_ROOT, gsrc, sizeof(gsrc) - 1, 0u);
                (void)t_unlink(T_WALK_OPEN_FROM_ROOT, gdst, sizeof(gdst) - 1, 0u);

                // Create src (the bytes we expect to survive the rename).
                long sf = t_walk_create(T_WALK_OPEN_FROM_ROOT, gsrc,
                                        sizeof(gsrc) - 1, T_OWRITE, 0644u);
                if (sf < 0 ||
                    write_all(sf, (const unsigned char *)src_data,
                              sizeof(src_data) - 1) != 0 ||
                    t_fsync(sf, 0) != 0) {
                    t_putstr("joey: fs-gamma src create/write/fsync FAILED\n");
                    return 1;
                }
                (void)t_close(sf);

                // Create dst with DIFFERENT bytes -- rename must atomically
                // REPLACE it (the property corvus's identity.db swap relies on).
                long df = t_walk_create(T_WALK_OPEN_FROM_ROOT, gdst,
                                        sizeof(gdst) - 1, T_OWRITE, 0644u);
                if (df < 0 ||
                    write_all(df, (const unsigned char *)dst_data,
                              sizeof(dst_data) - 1) != 0 ||
                    t_fsync(df, 0) != 0) {
                    t_putstr("joey: fs-gamma dst pre-create FAILED\n");
                    return 1;
                }
                (void)t_close(df);

                // Atomic rename src -> dst (replaces the existing dst).
                if (t_rename(T_WALK_OPEN_FROM_ROOT, gsrc, sizeof(gsrc) - 1,
                             T_WALK_OPEN_FROM_ROOT, gdst, sizeof(gdst) - 1) != 0) {
                    t_putstr("joey: fs-gamma rename FAILED\n");
                    return 1;
                }

                // src must be gone; dst must now hold SRC's bytes.
                long chk = t_walk_open(T_WALK_OPEN_FROM_ROOT, gsrc,
                                       sizeof(gsrc) - 1, T_OREAD);
                if (chk >= 0) {
                    (void)t_close(chk);
                    t_putstr("joey: fs-gamma rename left src behind\n");
                    return 1;
                }
                long rdf = t_walk_open(T_WALK_OPEN_FROM_ROOT, gdst,
                                       sizeof(gdst) - 1, T_OREAD);
                if (rdf < 0) {
                    t_putstr("joey: fs-gamma renamed dst missing\n");
                    return 1;
                }
                unsigned char gback[32];
                long gn = t_read(rdf, gback, sizeof(gback));
                (void)t_close(rdf);
                if (gn != (long)(sizeof(src_data) - 1)) {
                    t_putstr("joey: fs-gamma replace wrong len\n");
                    return 1;
                }
                for (long i = 0; i < gn; i++) {
                    if (gback[i] != (unsigned char)src_data[i]) {
                        t_putstr("joey: fs-gamma atomic-replace content mismatch\n");
                        return 1;
                    }
                }

                // unlink the renamed dst; it must then be gone.
                if (t_unlink(T_WALK_OPEN_FROM_ROOT, gdst, sizeof(gdst) - 1, 0u) != 0) {
                    t_putstr("joey: fs-gamma unlink FAILED\n");
                    return 1;
                }
                long gone = t_walk_open(T_WALK_OPEN_FROM_ROOT, gdst,
                                        sizeof(gdst) - 1, T_OREAD);
                if (gone >= 0) {
                    (void)t_close(gone);
                    t_putstr("joey: fs-gamma unlink did not remove dst\n");
                    return 1;
                }
                t_putstr("joey: fs-gamma rename(atomic-replace) + unlink OK\n");

                // rmdir an empty directory via SYS_UNLINK_REMOVEDIR.
                const char grd[] = "fs-gamma-rmdir";
                (void)t_unlink(T_WALK_OPEN_FROM_ROOT, grd, sizeof(grd) - 1,
                               T_UNLINK_REMOVEDIR);                 // stale cleanup
                long gd = t_walk_create(T_WALK_OPEN_FROM_ROOT, grd, sizeof(grd) - 1,
                                        T_OREAD, T_WALK_CREATE_DMDIR | 0755u);
                if (gd < 0) {
                    t_putstr("joey: fs-gamma mkdir FAILED\n");
                    return 1;
                }
                (void)t_close(gd);
                if (t_unlink(T_WALK_OPEN_FROM_ROOT, grd, sizeof(grd) - 1,
                             T_UNLINK_REMOVEDIR) != 0) {
                    t_putstr("joey: fs-gamma rmdir FAILED\n");
                    return 1;
                }
                long rgone = t_walk_open(T_WALK_OPEN_FROM_ROOT, grd,
                                         sizeof(grd) - 1, T_OREAD);
                if (rgone >= 0) {
                    (void)t_close(rgone);
                    t_putstr("joey: fs-gamma rmdir did not remove dir\n");
                    return 1;
                }
                t_putstr("joey: fs-gamma rmdir(REMOVEDIR) OK\n");
            }

            (void)t_close(sd_rd);
        }
    }

    // P6-pouch-stratumd-boot 16c-retire: the userspace stub bringup demo
    // (do_stratumd_stub_bringup -> /stratumd-stub) is retired -- the
    // real-stratumd path above (t_attach_9p_srv + pre/post-pivot probes)
    // covers the same lifecycle (9P attach + mount-equivalent + walk_open +
    // read) AGAINST a real on-disk FS instead of the in-RAM dev9p-stub
    // simulacrum. The kernel-internal dev9p + stub tests in kernel/test/
    // remain (they exercise the Dev vtable machinery in isolation).

    // === A-1.7: hand corvus its storage capability, then bring it up ===
    // Post-pivot: on the persistent Stratum root. Build corvus's storage
    // dir + hand it as a R|W-no-TRANSFER capability at fd 0; corvus chroots
    // to it so its filesystem world IS this dir (I-23).
    {
        long var_fd = mkdir_or_open(T_WALK_OPEN_FROM_ROOT, "var", 3);
        long lib_fd = (var_fd >= 0) ? mkdir_or_open(var_fd, "lib", 3) : -1;
        long cdir_fd = (lib_fd >= 0) ? mkdir_or_open(lib_fd, "corvus", 6) : -1;
        if (var_fd >= 0) (void)t_close(var_fd);
        if (lib_fd >= 0) (void)t_close(lib_fd);
        if (cdir_fd < 0) {
            t_putstr("joey: A-1.7 mkdir /var/lib/corvus FAILED\n");
            return 1;
        }
        long corvus_storage = t_dup(cdir_fd, T_RIGHT_READ | T_RIGHT_WRITE);
        (void)t_close(cdir_fd);
        if (corvus_storage < 0) {
            t_putstr("joey: A-1.7 t_dup(storage R|W) FAILED\n");
            return 1;
        }
        if (do_corvus_bringup(corvus_storage) != 0) {
            return 1;
        }
        (void)t_close(corvus_storage);
    }

    return 0;
}
