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
#define P9_QTFILE     0x00

#define P9_TVERSION   100
#define P9_RVERSION   101
#define P9_TATTACH    104
#define P9_RATTACH    105
#define P9_RLERROR    7
#define P9_TCLUNK     120
#define P9_RCLUNK     121
#define P9_TWALK      110
#define P9_RWALK      111
#define P9_TLOPEN     12
#define P9_RLOPEN     13
#define P9_TREAD      116
#define P9_RREAD      117

#define READ_FD       0
#define WRITE_FD      1

#define BUF_MAX       512

// Synthetic FS (P5-stratumd-stub-bringup-d): one root dir + one file.
// Path numbers double as qid path values for stable identity.
#define PATH_ROOT     1u
#define PATH_HELLO    2u
static const unsigned char HELLO_CONTENT[] = "hello from stratumd-stub\n";
#define HELLO_CONTENT_LEN ((unsigned)(sizeof(HELLO_CONTENT) - 1))

// Per-session fid table. The stub serves one client per process
// lifetime, so a tiny fixed-capacity table is sufficient. Plenty of
// headroom for Tattach + Twalk-derived fids; bound prevents runaway.
#define MAX_FIDS      16
struct fid_entry {
    unsigned int   fid_id;
    unsigned char  path;        // PATH_ROOT or PATH_HELLO; 0 = slot free
    unsigned char  opened;      // 1 after Tlopen, 0 before
};
static struct fid_entry g_fids[MAX_FIDS];

static int fid_find(unsigned int fid_id) {
    for (int i = 0; i < MAX_FIDS; i++) {
        if (g_fids[i].path != 0 && g_fids[i].fid_id == fid_id) return i;
    }
    return -1;
}
static int fid_alloc(unsigned int fid_id, unsigned char path) {
    if (fid_find(fid_id) >= 0) return -1;  // already bound
    for (int i = 0; i < MAX_FIDS; i++) {
        if (g_fids[i].path == 0) {
            g_fids[i].fid_id = fid_id;
            g_fids[i].path   = path;
            g_fids[i].opened = 0;
            return i;
        }
    }
    return -1;
}
static void fid_free(int slot) {
    if (slot >= 0 && slot < MAX_FIDS) {
        g_fids[slot].fid_id = 0;
        g_fids[slot].path   = 0;
        g_fids[slot].opened = 0;
    }
}

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
static unsigned short get_u16(const unsigned char *p) {
    return (unsigned short)((unsigned)p[0] | ((unsigned)p[1] << 8));
}
static unsigned int get_u32(const unsigned char *p) {
    return (unsigned)p[0]
         | ((unsigned)p[1] << 8)
         | ((unsigned)p[2] << 16)
         | ((unsigned)p[3] << 24);
}
static unsigned long long get_u64(const unsigned char *p) {
    unsigned long long lo = get_u32(p);
    unsigned long long hi = get_u32(p + 4);
    return lo | (hi << 32);
}

// Encode a qid (13 bytes: type + version(u32) + path(u64)) at `p`.
static void put_qid(unsigned char *p, unsigned char qtype, unsigned long long path) {
    p[0] = qtype;
    put_u32(p + 1, 0);  // version always 0 at v1.0
    for (int i = 0; i < 8; i++) p[5 + i] = (unsigned char)((path >> (i * 8)) & 0xff);
}

// Map a synthetic-FS path to its qid type. Used by Rwalk + Rlopen.
static unsigned char qtype_for_path(unsigned char path) {
    return (path == PATH_ROOT) ? P9_QTDIR : P9_QTFILE;
}

// Build an Rlerror frame for `tag` with `ecode` and return its size. The
// caller already validated `resp_cap >= P9_HDR_LEN + 4`.
static long build_rlerror(unsigned char *resp, long resp_cap,
                          unsigned short tag, unsigned int ecode) {
    long total = P9_HDR_LEN + 4;
    if (total > resp_cap) return -1;
    put_u32(resp, (unsigned)total);
    resp[4] = P9_RLERROR;
    put_u16(resp + 5, tag);
    put_u32(resp + 7, ecode);
    return total;
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
        // Tattach body: [fid u32][afid u32][uname str][aname str][n_uname u32].
        // Bind `fid` to the synthetic root before responding.
        if (req_len < P9_HDR_LEN + 4) return build_rlerror(resp, resp_cap, tag, 5);
        unsigned int fid = get_u32(req + 7);
        if (fid_alloc(fid, PATH_ROOT) < 0) {
            // Slot exhausted or duplicate binding. EBADF.
            return build_rlerror(resp, resp_cap, tag, 9);
        }
        long total = P9_HDR_LEN + P9_QID_LEN;
        if (total > resp_cap) return -1;
        put_u32(resp, (unsigned)total);
        resp[4] = P9_RATTACH;
        put_u16(resp + 5, tag);
        put_qid(resp + P9_HDR_LEN, P9_QTDIR, PATH_ROOT);
        return total;
    }

    if (type == P9_TWALK) {
        // Twalk body: [fid u32][newfid u32][nwname u16][wname[]:str×nwname].
        // Synthetic FS only supports two walks:
        //   nwname=0:               clone source → newfid (same path).
        //   nwname=1, name="hello", from root: newfid binds to /hello.
        // Anything else → Rlerror ENOENT (2).
        if (req_len < P9_HDR_LEN + 4 + 4 + 2)
            return build_rlerror(resp, resp_cap, tag, 5);
        unsigned int fid    = get_u32(req + 7);
        unsigned int newfid = get_u32(req + 11);
        unsigned short nwname = get_u16(req + 15);

        int src = fid_find(fid);
        if (src < 0) return build_rlerror(resp, resp_cap, tag, 9);  // EBADF

        unsigned char target_path = 0;
        if (nwname == 0) {
            target_path = g_fids[src].path;
        } else if (nwname == 1) {
            // Decode the single name.
            long off = P9_HDR_LEN + 4 + 4 + 2;
            if (off + 2 > req_len) return build_rlerror(resp, resp_cap, tag, 5);
            unsigned short nlen = get_u16(req + off);
            off += 2;
            if (off + nlen > req_len) return build_rlerror(resp, resp_cap, tag, 5);
            if (g_fids[src].path == PATH_ROOT
                && nlen == 5
                && req[off + 0] == 'h' && req[off + 1] == 'e' && req[off + 2] == 'l'
                && req[off + 3] == 'l' && req[off + 4] == 'o') {
                target_path = PATH_HELLO;
            } else {
                return build_rlerror(resp, resp_cap, tag, 2);  // ENOENT
            }
        } else {
            // Multi-component walks are not supported at v1.0.
            return build_rlerror(resp, resp_cap, tag, 2);
        }

        // newfid must not already be bound (9P invariant) — except newfid==fid
        // (allowed by 9P2000.L; the source fid is replaced in place).
        if (newfid != fid && fid_find(newfid) >= 0)
            return build_rlerror(resp, resp_cap, tag, 9);
        if (newfid == fid) {
            // Same-fid walk replaces the binding atomically per 9P semantics.
            g_fids[src].path   = target_path;
            g_fids[src].opened = 0;
        } else {
            if (fid_alloc(newfid, target_path) < 0)
                return build_rlerror(resp, resp_cap, tag, 9);  // table full
        }

        // Rwalk body: [nwqid u16][wqid[]:qid×nwqid].
        unsigned short nwqid = (nwname == 0) ? 0 : 1;
        long total = P9_HDR_LEN + 2 + (long)nwqid * P9_QID_LEN;
        if (total > resp_cap) return -1;
        put_u32(resp, (unsigned)total);
        resp[4] = P9_RWALK;
        put_u16(resp + 5, tag);
        put_u16(resp + P9_HDR_LEN, nwqid);
        if (nwqid == 1) {
            put_qid(resp + P9_HDR_LEN + 2, qtype_for_path(target_path), target_path);
        }
        return total;
    }

    if (type == P9_TLOPEN) {
        // Tlopen body: [fid u32][flags u32]. Mark fid open + return its qid.
        // Flags ignored at v1.0 (read-only synthetic FS; any open succeeds).
        if (req_len < P9_HDR_LEN + 4 + 4)
            return build_rlerror(resp, resp_cap, tag, 5);
        unsigned int fid = get_u32(req + 7);
        int slot = fid_find(fid);
        if (slot < 0) return build_rlerror(resp, resp_cap, tag, 9);  // EBADF
        g_fids[slot].opened = 1;
        // Rlopen body: [qid 13][iounit u32]. iounit=0 = no recommendation.
        long total = P9_HDR_LEN + P9_QID_LEN + 4;
        if (total > resp_cap) return -1;
        put_u32(resp, (unsigned)total);
        resp[4] = P9_RLOPEN;
        put_u16(resp + 5, tag);
        put_qid(resp + P9_HDR_LEN, qtype_for_path(g_fids[slot].path), g_fids[slot].path);
        put_u32(resp + P9_HDR_LEN + P9_QID_LEN, 0);
        return total;
    }

    if (type == P9_TREAD) {
        // Tread body: [fid u32][offset u64][count u32].
        // Only /hello has content; root reads would route through Treaddir
        // (not implemented at this sub-chunk).
        if (req_len < P9_HDR_LEN + 4 + 8 + 4)
            return build_rlerror(resp, resp_cap, tag, 5);
        unsigned int fid = get_u32(req + 7);
        unsigned long long offset = get_u64(req + 11);
        unsigned int req_count = get_u32(req + 19);
        int slot = fid_find(fid);
        if (slot < 0) return build_rlerror(resp, resp_cap, tag, 9);  // EBADF
        if (!g_fids[slot].opened) return build_rlerror(resp, resp_cap, tag, 9);
        if (g_fids[slot].path != PATH_HELLO) {
            // Reading the directory not supported at this sub-chunk.
            return build_rlerror(resp, resp_cap, tag, 21);  // EISDIR
        }
        // Compute the slice. EOF returns count=0 (NOT an error).
        unsigned int avail = 0;
        if (offset < HELLO_CONTENT_LEN) {
            avail = HELLO_CONTENT_LEN - (unsigned int)offset;
        }
        unsigned int n = (req_count < avail) ? req_count : avail;
        long total = P9_HDR_LEN + 4 + (long)n;
        if (total > resp_cap) return -1;
        put_u32(resp, (unsigned)total);
        resp[4] = P9_RREAD;
        put_u16(resp + 5, tag);
        put_u32(resp + P9_HDR_LEN, n);
        for (unsigned int i = 0; i < n; i++) {
            resp[P9_HDR_LEN + 4 + i] = HELLO_CONTENT[(unsigned int)offset + i];
        }
        return total;
    }

    if (type == P9_TCLUNK) {
        // Tclunk body: [fid u32]. Free the slot (idempotent for unbound fids).
        if (req_len >= P9_HDR_LEN + 4) {
            unsigned int fid = get_u32(req + 7);
            int slot = fid_find(fid);
            if (slot >= 0) fid_free(slot);
        }
        long total = P9_HDR_LEN;
        if (total > resp_cap) return -1;
        put_u32(resp, (unsigned)total);
        resp[4] = P9_RCLUNK;
        put_u16(resp + 5, tag);
        return total;
    }

    // Unknown / unsupported → Rlerror with errno=5 (EIO).
    return build_rlerror(resp, resp_cap, tag, 5);
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
