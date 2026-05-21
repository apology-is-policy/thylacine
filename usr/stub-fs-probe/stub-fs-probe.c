// /stub-fs-probe — raw 9P client driving Twalk + Tlopen + Tread
// against the stratumd-stub synthetic FS (P5-stratumd-stub-bringup-d).
//
// Mirror-image of /stratumd-stub: where the stub responds to Tmsgs,
// this probe drives Tmsgs. Both processes share the byte-pipe
// transport; the kernel test framework pre-installs the two
// transport fds in this probe's handle table BEFORE exec_setup:
//
//   fd 0 = c2s_wr — write end of the client→server pipe. Probe
//                    writes Tmsg frames here; stub reads them.
//   fd 1 = s2c_rd — read end of the server→client pipe. Probe
//                    reads Rmsg frames here; stub writes them.
//
// Why a raw-9P client rather than t_attach_9p + a userspace walk
// primitive? At this sub-chunk Thylacine has no userspace walk/open
// syscall that reaches into a dev9p-backed Spoor's underlying 9P
// fid table (only t_srv_connect drives walk-and-open, and that's
// /srv-specific). Driving raw bytes through pipes is the minimum-
// scope path to exercise the stub's Twalk / Tlopen / Tread handlers
// end-to-end without inventing a new syscall. Sub-chunk e (the
// userspace walk primitive) is when t_attach_9p + walk-through-mount
// becomes feasible.
//
// Sequence:
//   1. Tversion → Rversion(9P2000.L, msize=4096). Tag=NOFID(0xFFFF).
//   2. Tattach(fid=0, afid=NOFID, uname="", aname="/", n_uname=0) →
//      Rattach(qid for root, type=QTDIR, path=1). Tag=1.
//   3. Twalk(fid=0, newfid=1, nwname=1, ["hello"]) → Rwalk(nwqid=1,
//      qids=[hello]). Tag=2.
//   4. Tlopen(fid=1, flags=0) → Rlopen(qid, iounit). Tag=3.
//   5. Tread(fid=1, offset=0, count=64) → Rread(count, data). Tag=4.
//      Assert data matches HELLO_CONTENT.
//   6. Tread(fid=1, offset=25, count=64) → Rread(count=0). EOF
//      validation. Tag=5.
//   7. Tclunk(fid=1) → Rclunk. Tag=6.
//   8. Tclunk(fid=0) → Rclunk. Tag=7.
//   9. Close transport fds; exit 0.
//
// On any error: diagnostic + exit 1.

#include <thyla/syscall.h>

// 9P wire constants (mirror kernel/include/thylacine/9p_wire.h).
#define P9_HDR_LEN    7
#define P9_QID_LEN    13
#define P9_NOFID      0xFFFFFFFFu
#define P9_NOTAG      0xFFFFu
#define P9_QTDIR      (1 << 7)
#define P9_QTFILE     0x00

#define P9_TVERSION   100
#define P9_RVERSION   101
#define P9_TATTACH    104
#define P9_RATTACH    105
#define P9_RLERROR    7
#define P9_TWALK      110
#define P9_RWALK      111
#define P9_TLOPEN     12
#define P9_RLOPEN     13
#define P9_TREAD      116
#define P9_RREAD      117
#define P9_TCLUNK     120
#define P9_RCLUNK     121

#define TX_FD         0
#define RX_FD         1

#define BUF_MAX       512
#define ROOT_FID      0u
#define HELLO_FID     1u
#define MSIZE         4096u

// Expected synthetic-FS content (must match stratumd-stub.c).
static const unsigned char HELLO_CONTENT[] = "hello from stratumd-stub\n";
#define HELLO_CONTENT_LEN ((unsigned)(sizeof(HELLO_CONTENT) - 1))

// ---------- I/O helpers ----------

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

static int write_all(long fd, const unsigned char *buf, long n) {
    long off = 0;
    while (off < n) {
        long w = t_write(fd, buf + off, (size_t)(n - off));
        if (w <= 0) return -1;
        off += w;
    }
    return 0;
}

// ---------- Wire encoders (LE) ----------

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
static void put_u64(unsigned char *p, unsigned long long v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)((v >> (i * 8)) & 0xff);
}
static unsigned short get_u16(const unsigned char *p) {
    return (unsigned short)((unsigned)p[0] | ((unsigned)p[1] << 8));
}
static unsigned int get_u32(const unsigned char *p) {
    return (unsigned)p[0]
         | ((unsigned)p[1] << 8)
         | ((unsigned)p[2] << 16)
         | ((unsigned)p[3] << 24);
}

// Send one Tmsg, receive one Rmsg, validate envelope. Writes the
// response into `resp` (cap BUF_MAX). Returns response size on success
// or -1 on transport / framing failure. The caller validates type +
// tag at the call site.
static long exchange(const unsigned char *req, long req_len,
                     unsigned char *resp) {
    if (write_all(TX_FD, req, req_len) != 0) return -1;
    // Read 4-byte size prefix.
    if (read_exact(RX_FD, resp, 4) != 4) return -1;
    unsigned int size = get_u32(resp);
    if (size < P9_HDR_LEN || size > BUF_MAX) return -1;
    if (read_exact(RX_FD, resp + 4, (long)(size - 4)) != (long)(size - 4)) return -1;
    return (long)size;
}

// ---------- Sequence ----------

int main(void) {
    unsigned char req[BUF_MAX];
    unsigned char resp[BUF_MAX];

    t_putstr("stub-fs-probe: serving on fd 0 (tx) + fd 1 (rx)\n");

    // === Tversion ===
    // Body: [msize u32][version str]. version = "9P2000.L" (8 bytes).
    {
        static const char ver[] = "9P2000.L";
        long total = P9_HDR_LEN + 4 + 2 + 8;
        put_u32(req, (unsigned)total);
        req[4] = P9_TVERSION;
        put_u16(req + 5, P9_NOTAG);
        put_u32(req + 7, MSIZE);
        put_u16(req + 11, 8);
        for (int i = 0; i < 8; i++) req[13 + i] = (unsigned char)ver[i];

        long n = exchange(req, total, resp);
        if (n < 0 || resp[4] != P9_RVERSION) {
            t_putstr("stub-fs-probe: Tversion FAIL\n");
            return 1;
        }
    }

    // === Tattach(fid=0, afid=NOFID, uname="", aname="/", n_uname=0) ===
    // Body: [fid u32][afid u32][uname str][aname str][n_uname u32].
    {
        long off = P9_HDR_LEN;
        // Reserve header; fill after computing body size.
        put_u32(req + off, ROOT_FID); off += 4;
        put_u32(req + off, P9_NOFID); off += 4;
        put_u16(req + off, 0); off += 2;        // uname = empty
        put_u16(req + off, 1); off += 2;        // aname-len
        req[off++] = '/';                       // aname = "/"
        put_u32(req + off, 0); off += 4;        // n_uname

        put_u32(req, (unsigned)off);
        req[4] = P9_TATTACH;
        put_u16(req + 5, 1);                    // tag

        long n = exchange(req, off, resp);
        if (n < 0 || resp[4] != P9_RATTACH) {
            t_putstr("stub-fs-probe: Tattach FAIL\n");
            return 1;
        }
    }

    // === Twalk(fid=0, newfid=1, nwname=1, ["hello"]) ===
    // Body: [fid u32][newfid u32][nwname u16][wname[] str×nwname].
    {
        static const char hello[] = "hello";
        long off = P9_HDR_LEN;
        put_u32(req + off, ROOT_FID);   off += 4;
        put_u32(req + off, HELLO_FID);  off += 4;
        put_u16(req + off, 1);          off += 2;
        put_u16(req + off, 5);          off += 2;
        for (int i = 0; i < 5; i++) req[off++] = (unsigned char)hello[i];

        put_u32(req, (unsigned)off);
        req[4] = P9_TWALK;
        put_u16(req + 5, 2);                    // tag

        long n = exchange(req, off, resp);
        if (n < 0 || resp[4] != P9_RWALK) {
            t_putstr("stub-fs-probe: Twalk FAIL\n");
            return 1;
        }
        // Expect nwqid=1.
        unsigned short nwqid = get_u16(resp + P9_HDR_LEN);
        if (nwqid != 1) {
            t_putstr("stub-fs-probe: Rwalk nwqid != 1\n");
            return 1;
        }
        // First qid type should be QTFILE.
        if (resp[P9_HDR_LEN + 2] != P9_QTFILE) {
            t_putstr("stub-fs-probe: Rwalk hello qtype != QTFILE\n");
            return 1;
        }
    }

    // === Tlopen(fid=1, flags=0) ===
    // Body: [fid u32][flags u32].
    {
        long total = P9_HDR_LEN + 4 + 4;
        put_u32(req, (unsigned)total);
        req[4] = P9_TLOPEN;
        put_u16(req + 5, 3);
        put_u32(req + P9_HDR_LEN, HELLO_FID);
        put_u32(req + P9_HDR_LEN + 4, 0);       // flags

        long n = exchange(req, total, resp);
        if (n < 0 || resp[4] != P9_RLOPEN) {
            t_putstr("stub-fs-probe: Tlopen FAIL\n");
            return 1;
        }
    }

    // === Tread(fid=1, offset=0, count=64) ===
    // Body: [fid u32][offset u64][count u32]. Expect Rread with the
    // full HELLO_CONTENT_LEN bytes.
    {
        long total = P9_HDR_LEN + 4 + 8 + 4;
        put_u32(req, (unsigned)total);
        req[4] = P9_TREAD;
        put_u16(req + 5, 4);
        put_u32(req + P9_HDR_LEN, HELLO_FID);
        put_u64(req + P9_HDR_LEN + 4, 0);       // offset
        put_u32(req + P9_HDR_LEN + 12, 64);     // count

        long n = exchange(req, total, resp);
        if (n < 0 || resp[4] != P9_RREAD) {
            t_putstr("stub-fs-probe: Tread@0 FAIL\n");
            return 1;
        }
        unsigned int got = get_u32(resp + P9_HDR_LEN);
        if (got != HELLO_CONTENT_LEN) {
            t_putstr("stub-fs-probe: Tread@0 unexpected count\n");
            return 1;
        }
        for (unsigned int i = 0; i < got; i++) {
            if (resp[P9_HDR_LEN + 4 + i] != HELLO_CONTENT[i]) {
                t_putstr("stub-fs-probe: Tread@0 content mismatch\n");
                return 1;
            }
        }
    }

    // === Tread(fid=1, offset=HELLO_CONTENT_LEN, count=64) ===
    // Expect Rread with count=0 (EOF; not an error).
    {
        long total = P9_HDR_LEN + 4 + 8 + 4;
        put_u32(req, (unsigned)total);
        req[4] = P9_TREAD;
        put_u16(req + 5, 5);
        put_u32(req + P9_HDR_LEN, HELLO_FID);
        put_u64(req + P9_HDR_LEN + 4, HELLO_CONTENT_LEN);
        put_u32(req + P9_HDR_LEN + 12, 64);

        long n = exchange(req, total, resp);
        if (n < 0 || resp[4] != P9_RREAD) {
            t_putstr("stub-fs-probe: Tread@EOF FAIL\n");
            return 1;
        }
        unsigned int got = get_u32(resp + P9_HDR_LEN);
        if (got != 0) {
            t_putstr("stub-fs-probe: Tread@EOF expected count=0\n");
            return 1;
        }
    }

    // === Tclunk(fid=1) + Tclunk(fid=0) ===
    for (int round = 0; round < 2; round++) {
        long total = P9_HDR_LEN + 4;
        put_u32(req, (unsigned)total);
        req[4] = P9_TCLUNK;
        put_u16(req + 5, (unsigned short)(6 + round));
        put_u32(req + P9_HDR_LEN, (round == 0) ? HELLO_FID : ROOT_FID);

        long n = exchange(req, total, resp);
        if (n < 0 || resp[4] != P9_RCLUNK) {
            t_putstr("stub-fs-probe: Tclunk FAIL\n");
            return 1;
        }
    }

    t_putstr("stub-fs-probe: PASS\n");
    return 0;
}
