// /stratumd-stub — userspace 9P responder; minimal model of
// stratumd's wire side for P5-stratumd-stub-bringup.
//
// What this proves: a USERSPACE process can be the 9P server end of
// a `SYS_ATTACH_9P`-attached Spoor pair, identical to how real
// stratumd will serve. The kernel test framework (kernel/test/
// test_stratumd_stub.c) wires this stub + the /attach-probe client
// together via two pipe pairs, runs them as concurrent Procs, and
// reaps both. Until this chunk, every Phase 5 userspace 9P test
// used a kernel-thread responder (test_attach_probe.c's kthread);
// this is the first userspace-server test, which is the actual
// production shape.
//
// Calling convention: the kernel test framework pre-installs two
// KOBJ_SPOOR handles in this Proc's handle table BEFORE exec_setup:
//
//   fd 0 = c2s_rd — read end of client→server pipe; stub reads
//                    Tmsgs framed at the wire layer here.
//   fd 1 = s2c_wr — write end of server→client pipe; stub writes
//                    Rmsgs framed at the wire layer here.
//
// Loop:
//   1. read 4-byte size; if 0 bytes returned, EOF → exit 0.
//   2. read (size - 4) bytes; if short, exit 1.
//   3. dispatch on byte at offset 4 (P9 type):
//        TVERSION (100) → RVERSION (101) with msize=4096 + "9P2000.L"
//        TATTACH  (104) → RATTACH (105) with root qid (QTDIR, ver=0, path=1)
//        TCLUNK   (120) → RCLUNK (121) (empty body)
//        anything else → RLERROR (7) errno=EIO (5)
//   4. write the response back; loop.
//
// The /attach-probe client at v1.0 sends Tversion + Tattach only;
// Tclunk on root_fid is rejected at the session layer (kernel/
// 9p_session.c::p9_session_send_clunk) and never reaches the wire.
// This stub handles Tclunk anyway for shape completeness and to
// remain forward-compatible with future clients that DO clunk
// non-root fids.

#include <thyla/syscall.h>

// 9P wire constants (mirror kernel/include/thylacine/9p_wire.h).
#define P9_HDR_LEN    7
#define P9_QID_LEN    13
#define P9_QTDIR      (1 << 7)

#define P9_TVERSION   100
#define P9_RVERSION   101
#define P9_TATTACH    104
#define P9_RATTACH    105
#define P9_RLERROR    7
#define P9_TCLUNK     120
#define P9_RCLUNK     121

#define READ_FD       0
#define WRITE_FD      1

#define BUF_MAX       256

// Read exactly `n` bytes from `fd`. Returns:
//   n  on full read
//   0  on EOF before any byte
//  -1  on partial read after EOF, or read error
static long read_exact(long fd, unsigned char *buf, long n) {
    long off = 0;
    while (off < n) {
        long r = t_read(fd, buf + off, (size_t)(n - off));
        if (r < 0) return -1;
        if (r == 0) return (off == 0) ? 0 : -1;
        off += r;
    }
    return n;
}

// Write all `n` bytes. Returns 0 on success, -1 on any short or
// failed write.
static int write_all(long fd, const unsigned char *buf, long n) {
    long off = 0;
    while (off < n) {
        long w = t_write(fd, buf + off, (size_t)(n - off));
        if (w <= 0) return -1;
        off += w;
    }
    return 0;
}

// Little-endian byte helpers — libt has no libc, so write them inline.
static void put_u16(unsigned char *p, unsigned short v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}
static void put_u32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

// Build a response for one request. Returns response size in bytes,
// or -1 on internal buffer-cap violation.
static long build_response(const unsigned char *req, long req_len,
                           unsigned char *resp, long resp_cap) {
    if (req_len < P9_HDR_LEN) return -1;
    unsigned char type = req[4];
    unsigned short tag = (unsigned short)((unsigned)req[5] | ((unsigned)req[6] << 8));

    if (type == P9_TVERSION) {
        // Rversion body: msize (u32) + version-len (u16) + "9P2000.L" (8).
        static const char ver[] = "9P2000.L";
        long total = P9_HDR_LEN + 4 + 2 + 8;
        if (total > resp_cap) return -1;
        put_u32(resp, (unsigned)total);
        resp[4] = P9_RVERSION;
        // Tversion uses NOFID tag (0xffff); mirror it.
        put_u16(resp + 5, 0xffff);
        put_u32(resp + 7, 4096);  // msize
        put_u16(resp + 11, 8);    // version-len
        for (int i = 0; i < 8; i++) resp[13 + i] = (unsigned char)ver[i];
        return total;
    }

    if (type == P9_TATTACH) {
        // Rattach body: qid (13 bytes; type + version + path).
        long total = P9_HDR_LEN + P9_QID_LEN;
        if (total > resp_cap) return -1;
        put_u32(resp, (unsigned)total);
        resp[4] = P9_RATTACH;
        put_u16(resp + 5, tag);
        // Root qid: type=QTDIR, version=0, path=1.
        resp[7] = P9_QTDIR;
        put_u32(resp + 8, 0);   // version
        // path = 1 (8-byte LE).
        resp[12] = 1;
        for (int i = 13; i < 20; i++) resp[i] = 0;
        return total;
    }

    if (type == P9_TCLUNK) {
        // Rclunk: empty body.
        long total = P9_HDR_LEN;
        if (total > resp_cap) return -1;
        put_u32(resp, (unsigned)total);
        resp[4] = P9_RCLUNK;
        put_u16(resp + 5, tag);
        return total;
    }

    // Unknown / unsupported → Rlerror with errno=5 (EIO).
    long total = P9_HDR_LEN + 4;
    if (total > resp_cap) return -1;
    put_u32(resp, (unsigned)total);
    resp[4] = P9_RLERROR;
    put_u16(resp + 5, tag);
    put_u32(resp + 7, 5);  // errno = EIO
    return total;
}

int main(void) {
    unsigned char req[BUF_MAX];
    unsigned char resp[BUF_MAX];

    t_putstr("stratumd-stub: serving on fd 0 (rx) + fd 1 (tx)\n");

    for (;;) {
        // Read 4-byte size prefix. EOF here = clean shutdown.
        long got = read_exact(READ_FD, req, 4);
        if (got == 0) {
            t_putstr("stratumd-stub: EOF on rx; exit 0\n");
            return 0;
        }
        if (got < 0) {
            t_putstr("stratumd-stub: short read on header; exit 1\n");
            return 1;
        }

        unsigned int size = (unsigned int)req[0]
                          | ((unsigned int)req[1] << 8)
                          | ((unsigned int)req[2] << 16)
                          | ((unsigned int)req[3] << 24);
        if (size < P9_HDR_LEN || size > BUF_MAX) {
            t_putstr("stratumd-stub: bad frame size; exit 1\n");
            return 1;
        }

        long body_len = (long)size - 4;
        if (read_exact(READ_FD, req + 4, body_len) != body_len) {
            t_putstr("stratumd-stub: short read on body; exit 1\n");
            return 1;
        }

        long rlen = build_response(req, (long)size, resp, BUF_MAX);
        if (rlen <= 0) {
            t_putstr("stratumd-stub: build_response failed; exit 1\n");
            return 1;
        }

        if (write_all(WRITE_FD, resp, rlen) != 0) {
            // Write failure typically means the client closed its rx ring;
            // treat as graceful shutdown rather than error.
            t_putstr("stratumd-stub: write_all failed (client gone?); exit 0\n");
            return 0;
        }
    }
}
