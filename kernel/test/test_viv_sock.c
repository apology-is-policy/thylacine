// The vivarium socket shells against a /net served over 9P (ARCH 8.8.3).
//
// signal(7)'s list names accept and connect for their WAITS alone: accept's
// wait is the held listen open, connect's is TCP's handshake (the held data
// open). Every other step of either call must ride a caught note out -- the
// dial verb because an abandoned Twrite may already have dialed, accept's
// post-dequeue steps because an EINTR there would hang up a connection the
// guest never saw. These tests drive the REAL shells through dev9p + stalk
// against a loopback /net whose responder runs in the calling thread, so it
// records the caller's note_interruptible at each step; and they drive connect's
// failure verdicts (a caught signal, a timeout, a refusal) through dev9p's own
// open errno channel, so the errno a guest sees is the one the wire carried.
// A connect a signal interrupted is finished by the socket's next use -- send,
// recv, read, write, or SO_ERROR once netd's status file says the handshake
// resolved -- since POSIX has that connection established asynchronously.

#include "test.h"

#include <thylacine/9p_client.h>
#include <thylacine/9p_transport_loopback.h>
#include <thylacine/9p_wire.h>
#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/dev9p.h>
#include <thylacine/errno.h>
#include <thylacine/handle.h>
#include <thylacine/pipe.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/stalk.h>
#include <thylacine/syscall.h>
#include <thylacine/territory.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/vivarium.h>

#include "../../arch/arm64/exception.h"
#include "../../mm/slub.h"

extern s64 viv_sock_connect_for_test(struct Proc *p, u64 fd, const u8 ip4[4], u16 port);
extern s64 viv_accept_for_test(struct Proc *p, u64 fd);
extern s64 viv_socket_for_test(struct Proc *p, u64 domain, u64 type, u64 protocol);
extern s64 viv_sendto_empty_for_test(struct Proc *p, u64 fd);
extern s64 viv_recvfrom_empty_for_test(struct Proc *p, u64 fd);
extern s64 viv_recvmsg_for_test(struct Proc *p, u64 fd, u64 msg_va);
extern s32 viv_sock_pending_error_for_test(struct Proc *p, u64 fd);
extern bool viv_linux_dispatch_for_test(struct exception_context *ctx, struct Proc *p);
extern s64 viv_listen_for_test(struct Proc *p, u64 fd);
extern s64 viv_sock_dgram_send_for_test(struct Proc *p, u64 fd, const u8 ip4[4], u16 port);
extern void viv_sock_pin_hook_for_test(void (*hook)(struct Proc *, u64));
extern s64 viv_dup_for_test(struct Proc *p, u64 fd);
extern s64 viv_dup3_for_test(struct Proc *p, u64 oldfd, u64 newfd, u64 flags);

void test_vivsock_accept_wait_is_over(void);
void test_vivsock_connect_tcp_holds_the_dial(void);
void test_vivsock_connect_udp_never_waits(void);
void test_vivsock_connect_keeps_a_reused_number(void);
void test_vivsock_socket_unwind_publishes_nothing(void);
void test_vivsock_connect_eintr_then_retry(void);
void test_vivsock_connect_connecting_resumes(void);
void test_vivsock_connect_failure_verdicts(void);
void test_vivsock_send_recv_finish_connect(void);
void test_vivsock_read_write_finish_connect(void);
void test_vivsock_sendto_recvfrom_narrow_fd(void);
void test_vivsock_so_error_reports_the_dial(void);
void test_vivsock_positioned_io_finishes_connect(void);
void test_vivsock_so_error_needs_no_descriptor(void);
void test_vivsock_ctl_verbs_need_their_ctl(void);
void test_vivsock_ctl_verbs_meet_a_racing_peer(void);
void test_vivsock_ctl_identity_travels_with_the_fd(void);
void test_vivsock_connect_finished_by_a_peer(void);
void test_vivsock_connect_commits_a_realiased_row(void);

// ---------------------------------------------------------------------------
// The /net stub: net/{tcp,udp}/<n>/{ctl,data,listen,remote,status}. A qid
// path's low byte is the node's kind, so a test can read what an fd names off
// its Spoor.
// ---------------------------------------------------------------------------

enum { NS_DIR = 1, NS_CTL = 2, NS_DATA = 3, NS_LISTEN = 4, NS_FILE = 5, NS_STATUS = 6 };

struct ns_fid { u32 fid; u8 used; u8 kind; u32 n; u64 qpath; };
#define NS_FIDS 48u

static struct ns_fid g_ns_fid[NS_FIDS];

static struct {
    bool listen_intr, read_intr, data_intr, dial_intr;   // the caller's flag at each
    u32  listen_opens, data_opens, ctl_reads, ctl_writes, ctl_clunks;
    u32  clone_n;         // the conversation a clone walk lands on
    u32  data_fail;       // != 0: the NEXT data Tlopen answers Rlerror(this)
    u32  accept_n;        // the conversation a listen open hands back
    const char *status;   // a status file's content; NULL: there is no status file
    u32  status_reads;
    char verb[48];
    u32  verb_len;
    // != NULL: the NEXT data Tlopen stands in for a peer thread that closes
    // swap_fd and opens swap_in, landing on the same number while connect
    // sleeps in that open; swap_got is the number it landed on.
    struct Proc  *swap_p;
    struct Spoor *swap_in;
    s64           swap_fd, swap_got;
    // != NULL: the NEXT data Tlopen stands in for a peer thread finishing the
    // same connect first -- it swaps finish_fd from ctl onto finish_dsp (a
    // data fid it opened beforehand) with connect's commit. The commit's
    // detached ctl reference is left for the test to clunk.
    struct Proc  *finish_p;
    struct Spoor *finish_dsp, *finish_detached;
    s64           finish_fd;
    bool          finish_ok;
    // != NULL: the NEXT data Tlopen stands in for a peer thread that re-makes
    // realias_fd's row while connect sleeps: dup(realias_fd) -> realias_got,
    // then dup3(realias_got, realias_fd), through the real dup and dup3 arms.
    struct Proc  *realias_p;
    s64           realias_fd, realias_got;
    bool          realias_done;
    u32  file_writes;     // Twrites to a non-/net-verb file (a `remote`)
    u32  data_clunks;
} g_ns;

static void ns_put32(u8 *b, u32 v) {
    b[0] = (u8)v; b[1] = (u8)(v >> 8); b[2] = (u8)(v >> 16); b[3] = (u8)(v >> 24);
}
static void ns_put64(u8 *b, u64 v) { for (int i = 0; i < 8; i++) b[i] = (u8)(v >> (8 * i)); }
static u32 ns_get32(const u8 *b) {
    return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}
static u16 ns_get16(const u8 *b) { return (u16)((u16)b[0] | ((u16)b[1] << 8)); }

static struct ns_fid *ns_find(u32 fid) {
    for (u32 i = 0; i < NS_FIDS; i++)
        if (g_ns_fid[i].used && g_ns_fid[i].fid == fid) return &g_ns_fid[i];
    return NULL;
}

static void ns_bind(u32 fid, u8 kind, u32 n, u64 qpath) {
    struct ns_fid *f = ns_find(fid);
    for (u32 i = 0; !f && i < NS_FIDS; i++)
        if (!g_ns_fid[i].used) f = &g_ns_fid[i];
    if (!f) return;
    f->fid = fid; f->used = 1; f->kind = kind; f->n = n; f->qpath = qpath;
}

static bool ns_name_is(const u8 *nm, u16 len, const char *lit) {
    u16 i = 0;
    for (; i < len; i++) if (lit[i] == '\0' || (u8)lit[i] != nm[i]) return false;
    return lit[i] == '\0';
}

// One walk step from a directory. False for a name the stub does not serve.
static bool ns_step(u8 *kind, u32 *n, u64 *qpath, const u8 *nm, u16 len) {
    if (*kind != NS_DIR || len == 0) return false;
    bool digits = true;
    u32  v      = 0;
    for (u16 i = 0; i < len; i++) {
        if (nm[i] < '0' || nm[i] > '9') { digits = false; break; }
        v = v * 10u + (u32)(nm[i] - '0');
    }
    u8 k;
    if (digits)                                       { k = NS_DIR; *n = v; }
    else if (ns_name_is(nm, len, "net") ||
             ns_name_is(nm, len, "tcp") ||
             ns_name_is(nm, len, "udp"))              k = NS_DIR;
    else if (ns_name_is(nm, len, "ctl"))              k = NS_CTL;
    else if (ns_name_is(nm, len, "clone"))            { k = NS_CTL; *n = g_ns.clone_n; }
    else if (ns_name_is(nm, len, "data"))             k = NS_DATA;
    else if (ns_name_is(nm, len, "listen"))           k = NS_LISTEN;
    else if (ns_name_is(nm, len, "remote"))           k = NS_FILE;
    else if (ns_name_is(nm, len, "status") && g_ns.status) k = NS_STATUS;
    else                                              return false;
    u64 h = *qpath >> 8;
    for (u16 i = 0; i < len; i++) h = h * 131u + nm[i];
    *kind  = k;
    *qpath = ((h & 0x00ffffffffffffffull) << 8) | k;
    return true;
}

static int ns_hdr(u8 *resp, size_t cap, size_t total, u8 type, u16 tag) {
    if (cap < total) return -1;
    for (size_t i = 0; i < total; i++) resp[i] = 0;
    ns_put32(resp, (u32)total);
    resp[4] = type;
    resp[5] = (u8)tag; resp[6] = (u8)(tag >> 8);
    return (int)total;
}

static int ns_lerror(u8 *resp, size_t cap, u16 tag, u32 ecode) {
    if (ns_hdr(resp, cap, P9_HDR_LEN + 4, P9_RLERROR, tag) < 0) return -1;
    ns_put32(resp + P9_HDR_LEN, ecode);
    return (int)(P9_HDR_LEN + 4);
}

static void ns_qid(u8 *b, u8 kind, u64 qpath) {
    b[0] = (kind == NS_DIR) ? P9_QTDIR : P9_QTFILE;
    ns_put64(b + 5, qpath);
}

static int ns_responder(void *ctx, const u8 *req, size_t req_len,
                        u8 *resp, size_t resp_cap) {
    (void)ctx;
    u32 size; u8 type; u16 tag;
    if (req_len < P9_HDR_LEN || p9_peek_header(req, req_len, &size, &type, &tag) < 0)
        return -1;
    bool intr = current_thread()->note_interruptible;

    if (type == P9_TVERSION) {
        int t = ns_hdr(resp, resp_cap, P9_HDR_LEN + 4 + 2 + 8, P9_RVERSION, 0xffffu);
        if (t < 0) return -1;
        ns_put32(resp + 7, 8192);
        resp[11] = 8;
        const char *v = "9P2000.L";
        for (int i = 0; i < 8; i++) resp[13 + i] = (u8)v[i];
        return t;
    }
    if (type == P9_TATTACH) {
        if (req_len < P9_HDR_LEN + 4) return -1;
        ns_bind(ns_get32(req + 7), NS_DIR, 0, (1ull << 8) | NS_DIR);
        int t = ns_hdr(resp, resp_cap, P9_HDR_LEN + P9_QID_LEN, P9_RATTACH, tag);
        if (t < 0) return -1;
        ns_qid(resp + 7, NS_DIR, (1ull << 8) | NS_DIR);
        return t;
    }
    if (type == P9_TWALK) {
        if (req_len < P9_HDR_LEN + 10) return -1;
        struct ns_fid *from = ns_find(ns_get32(req + 7));
        if (!from) return ns_lerror(resp, resp_cap, tag, 9 /* EBADF */);
        u32 newfid = ns_get32(req + 11);
        u16 nwname = ns_get16(req + 15);
        u8  kind = from->kind; u32 n = from->n; u64 qpath = from->qpath;
        u8  qids[16 * P9_QID_LEN];
        u16 nq = 0;
        size_t off = P9_HDR_LEN + 10;
        for (u16 i = 0; i < nwname && i < 16u; i++) {
            if (off + 2 > req_len) return -1;
            u16 len = ns_get16(req + off);
            if (off + 2 + len > req_len) return -1;
            if (!ns_step(&kind, &n, &qpath, req + off + 2, len)) break;
            for (u32 j = 0; j < P9_QID_LEN; j++) qids[nq * P9_QID_LEN + j] = 0;
            ns_qid(&qids[nq * P9_QID_LEN], kind, qpath);
            nq++;
            off += 2 + len;
        }
        if (nwname > 0 && nq == 0) return ns_lerror(resp, resp_cap, tag, 2 /* ENOENT */);
        if (nq == nwname) ns_bind(newfid, kind, n, qpath);   // only a full walk binds
        int t = ns_hdr(resp, resp_cap, P9_HDR_LEN + 2 + (size_t)nq * P9_QID_LEN,
                       P9_RWALK, tag);
        if (t < 0) return -1;
        resp[7] = (u8)nq; resp[8] = (u8)(nq >> 8);
        for (u32 j = 0; j < (u32)nq * P9_QID_LEN; j++) resp[9 + j] = qids[j];
        return t;
    }
    if (type == P9_TWALKGETATTR)   // unsupported: dev9p latches it and walks per component
        return ns_lerror(resp, resp_cap, tag, 38 /* ENOSYS */);
    if (type == P9_TGETATTR) {
        struct ns_fid *f = (req_len >= P9_HDR_LEN + 4) ? ns_find(ns_get32(req + 7)) : NULL;
        if (!f) return ns_lerror(resp, resp_cap, tag, 9);
        int t = ns_hdr(resp, resp_cap, P9_HDR_LEN + 153, P9_RGETATTR, tag);
        if (t < 0) return -1;
        size_t o = P9_HDR_LEN;
        ns_put64(resp + o, 0x7ffu);                    o += 8;    // valid = BASIC
        ns_qid(resp + o, f->kind, f->qpath);           o += P9_QID_LEN;
        ns_put32(resp + o, (f->kind == NS_DIR) ? 040755u : 0100666u); o += 4;
        o += 4 + 4;                                                // uid, gid = 0
        ns_put64(resp + o, 1);                         o += 8;    // nlink
        o += 8 + 8;                                                // rdev, size
        ns_put64(resp + o, 4096);                                  // blksize
        return t;
    }
    if (type == P9_TLOPEN) {
        struct ns_fid *f = (req_len >= P9_HDR_LEN + 8) ? ns_find(ns_get32(req + 7)) : NULL;
        if (!f) return ns_lerror(resp, resp_cap, tag, 9);
        if (f->kind == NS_LISTEN) {
            // netd holds this open until a call arrives, then the fid IS the
            // accepted conversation's ctl.
            g_ns.listen_opens++;
            g_ns.listen_intr = intr;
            u64 qp = ((u64)(0x4000u + g_ns.accept_n) << 8) | NS_CTL;
            f->kind = NS_CTL; f->n = g_ns.accept_n; f->qpath = qp;
        } else if (f->kind == NS_DATA) {
            g_ns.data_opens++;
            g_ns.data_intr = intr;
            if (g_ns.swap_in) {
                (void)handle_close(g_ns.swap_p, (hidx_t)g_ns.swap_fd);
                g_ns.swap_got = handle_alloc(g_ns.swap_p, KOBJ_SPOOR, RIGHT_READ,
                                             g_ns.swap_in);
                g_ns.swap_in  = NULL;
            }
            if (g_ns.finish_dsp) {
                // No RPC from inside the reply: the data fid was opened ahead,
                // and neither the swap nor the snapshot's put drops a last
                // reference to ctl (the row and connect's own pin hold it).
                struct viv_sock e;
                struct Handle   h;
                struct Proc    *fp = g_ns.finish_p;
                if (viv_socktab_get(fp->socktab, (s32)g_ns.finish_fd, &e) &&
                    handle_get(fp, (hidx_t)g_ns.finish_fd, &h) == 0) {
                    struct viv_socktab_connected c = {
                        .tab = fp->socktab, .fd = (s32)g_ns.finish_fd,
                        .expect_epoch = e.epoch, .ctl = (const struct Spoor *)h.obj,
                        .addr = e.remote_addr, .port = e.remote_port,
                        .ok = false, .detached = NULL,
                    };
                    // The key is held across the replace: handle_get's reference.
                    if (handle_replace_hooked(fp, (hidx_t)g_ns.finish_fd, h.obj, KOBJ_SPOOR,
                                              RIGHT_READ | RIGHT_WRITE, g_ns.finish_dsp,
                                              viv_socktab_connected_locked, &c) == 0) {
                        g_ns.finish_dsp      = NULL;   // the slot's now
                        g_ns.finish_ok       = c.ok;
                        g_ns.finish_detached = c.detached;
                    }
                    handle_put(&h);
                }
            }
            if (g_ns.realias_p) {
                // Neither arm sends an RPC: each reference they drop on ctl
                // leaves one held by a row, a slot or connect's own pin.
                struct Proc *rp = g_ns.realias_p;
                g_ns.realias_p   = NULL;
                g_ns.realias_got = viv_dup_for_test(rp, (u64)g_ns.realias_fd);
                g_ns.realias_done = g_ns.realias_got >= 0 &&
                    viv_dup3_for_test(rp, (u64)g_ns.realias_got, (u64)g_ns.realias_fd, 0) ==
                        g_ns.realias_fd;
            }
            if (g_ns.data_fail) {
                u32 ec = g_ns.data_fail;
                g_ns.data_fail = 0;
                return ns_lerror(resp, resp_cap, tag, ec);
            }
        }
        int t = ns_hdr(resp, resp_cap, P9_HDR_LEN + P9_QID_LEN + 4, P9_RLOPEN, tag);
        if (t < 0) return -1;
        ns_qid(resp + 7, f->kind, f->qpath);
        ns_put32(resp + 7 + P9_QID_LEN, 4096);
        return t;
    }
    if (type == P9_TREAD) {
        struct ns_fid *f = (req_len >= P9_HDR_LEN + 16) ? ns_find(ns_get32(req + 7)) : NULL;
        if (!f) return ns_lerror(resp, resp_cap, tag, 9);
        u64  offset = (u64)ns_get32(req + 11) | ((u64)ns_get32(req + 15) << 32);
        char dec[16] = { 0 };
        u32  dn = 0;
        if (f->kind == NS_STATUS) {
            g_ns.status_reads++;
            for (const char *c = g_ns.status; offset == 0 && *c && dn < sizeof(dec); c++)
                dec[dn++] = *c;
        }
        if (f->kind == NS_CTL) {
            g_ns.ctl_reads++;
            g_ns.read_intr = intr;
            if (offset == 0) {
                char rev[11];
                u32  rn = 0, v = f->n;
                do { rev[rn++] = (char)('0' + v % 10u); v /= 10u; } while (v && rn < 10u);
                while (rn) dec[dn++] = rev[--rn];
            }
        }
        int t = ns_hdr(resp, resp_cap, P9_HDR_LEN + 4 + dn, P9_RREAD, tag);
        if (t < 0) return -1;
        ns_put32(resp + 7, dn);
        for (u32 i = 0; i < dn; i++) resp[11 + i] = (u8)dec[i];
        return t;
    }
    if (type == P9_TWRITE) {
        struct ns_fid *f = (req_len >= P9_HDR_LEN + 16) ? ns_find(ns_get32(req + 7)) : NULL;
        if (!f) return ns_lerror(resp, resp_cap, tag, 9);
        u32 count = ns_get32(req + 19);
        if (f->kind == NS_FILE) g_ns.file_writes++;
        if (f->kind == NS_CTL) {
            g_ns.ctl_writes++;
            g_ns.dial_intr = intr;
            g_ns.verb_len = 0;
            for (u32 i = 0; i < count && i < sizeof(g_ns.verb) - 1u &&
                            (size_t)23 + i < req_len; i++)
                g_ns.verb[g_ns.verb_len++] = (char)req[23 + i];
            g_ns.verb[g_ns.verb_len] = '\0';
        }
        int t = ns_hdr(resp, resp_cap, P9_HDR_LEN + 4, P9_RWRITE, tag);
        if (t < 0) return -1;
        ns_put32(resp + 7, count);
        return t;
    }
    if (type == P9_TCLUNK) {
        struct ns_fid *f = (req_len >= P9_HDR_LEN + 4) ? ns_find(ns_get32(req + 7)) : NULL;
        if (f && f->kind == NS_CTL) g_ns.ctl_clunks++;
        if (f && f->kind == NS_DATA) g_ns.data_clunks++;
        if (f) f->used = 0;
        return ns_hdr(resp, resp_cap, P9_HDR_LEN, P9_RCLUNK, tag);
    }
    return ns_lerror(resp, resp_cap, tag, 38);
}

// ---------------------------------------------------------------------------
// The fixture: a Linux-phenotype Proc whose root is the stub, with an empty
// socket table. The territory owns the root; ns_free tears the Proc down before
// the client, so every clunk reaches a live session.
// ---------------------------------------------------------------------------

static struct p9_client   g_ns_client;
static struct p9_loopback g_ns_lb;
static u8                 g_ns_recv[8192];
static u8                 g_ns_resp[8192];

static struct Proc *ns_proc(void) {
    for (u32 i = 0; i < NS_FIDS; i++) g_ns_fid[i].used = 0;
    for (size_t i = 0; i < sizeof(g_ns); i++) ((u8 *)&g_ns)[i] = 0;
    g_ns.accept_n = 8;
    if (p9_loopback_init(&g_ns_lb, g_ns_resp, sizeof(g_ns_resp), ns_responder, NULL) != 0)
        return NULL;
    if (p9_client_init(&g_ns_client, 0, 8192, p9_loopback_ops_for(&g_ns_lb),
                       g_ns_recv, sizeof(g_ns_recv)) != 0) {
        p9_loopback_destroy(&g_ns_lb);
        return NULL;
    }
    const u8 uname[] = { 'r', 'o', 'o', 't' };
    const u8 aname[] = { '/' };
    struct Spoor *root = NULL;
    struct Proc  *p    = NULL;
    if (p9_client_handshake(&g_ns_client, uname, sizeof(uname), aname, sizeof(aname), 0) != 0)
        goto fail;
    root = dev9p_attach_client(&g_ns_client, 0);
    if (!root) goto fail;
    p = proc_alloc();
    if (!p) goto fail;
    p->phenotype    = PHENO_LINUX;
    p->principal_id = 0x1234u;
    p->primary_gid  = 0x5678u;
    p->caps         = CAP_DAC_OVERRIDE;   // the stub's modes are not under test
    p->territory    = territory_alloc();
    p->socktab      = (struct viv_socktab *)kzalloc(sizeof(struct viv_socktab), 0);
    if (!p->territory || !p->socktab) goto fail;
    for (u32 i = 0; i < VIV_SOCK_MAX; i++) p->socktab->s[i].fd = -1;
    p->territory->root_spoor = root;      // the territory owns this ref
    return p;
fail:
    if (root) spoor_clunk(root);
    if (p) { p->state = PROC_STATE_ZOMBIE; proc_free(p); }
    p9_client_destroy(&g_ns_client);
    p9_loopback_destroy(&g_ns_lb);
    return NULL;
}

static void ns_free(struct Proc *p) {
    current_thread()->note_interruptible = false;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
    p9_client_destroy(&g_ns_client);
    p9_loopback_destroy(&g_ns_lb);
}

// /net/<proto>/<n>/<file> opened ORDWR through the Proc's own root, or NULL
// with *err set.
static struct Spoor *ns_open(struct Proc *p, enum viv_net_proto proto, u32 n,
                             const char *file, int *err) {
    char path[40];
    u32  len = 0;
    for (const char *c = (proto == VIV_NET_UDP) ? "net/udp/" : "net/tcp/"; *c; c++)
        path[len++] = *c;
    char rev[11];
    u32  rn = 0;
    do { rev[rn++] = (char)('0' + n % 10u); n /= 10u; } while (n && rn < 10u);
    while (rn) path[len++] = rev[--rn];
    path[len++] = '/';
    while (*file && len < sizeof(path)) path[len++] = *file++;
    *err = 0;
    return stalk_err(p, p->territory->root_spoor, path, len, STALK_OPEN,
                     2u /* ORDWR */, err);
}

// A socket as socket() leaves one: an fd on <proto>/<n>/ctl with its row, which
// names that ctl as its identity (a CONNECTED row has none: its fd is data).
static s64 ns_socket(struct Proc *p, enum viv_net_proto proto, u32 n, enum viv_sock_state st) {
    int err = 0;
    struct Spoor *s = ns_open(p, proto, n, "ctl", &err);
    if (!s) return -(s64)(err > 0 ? err : (int)T_E_IO);
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, s);
    if (fd < 0) { spoor_clunk(s); return -(s64)T_E_MFILE; }
    if (!viv_socktab_claim(p->socktab, (s32)fd, proto, n, st,
                           (st == VIV_SOCK_CONNECTED) ? NULL : s))
        return -(s64)T_E_MFILE;
    return (s64)fd;
}

static bool ns_row(struct Proc *p, s64 fd, struct viv_sock *out) {
    return fd >= 0 && viv_socktab_get(p->socktab, (s32)fd, out);
}

// What the fd names: the low byte of its Spoor's qid path.
static u8 ns_fd_kind(struct Proc *p, s64 fd) {
    struct Handle h;
    if (fd < 0 || handle_get(p, (hidx_t)fd, &h) < 0) return 0;
    u8 k = (h.kind == KOBJ_SPOOR) ? (u8)(((struct Spoor *)h.obj)->qid.path & 0xffu) : 0;
    handle_put(&h);
    return k;
}

static bool ns_verb_is(const char *lit) {
    u32 i = 0;
    for (; lit[i] != '\0'; i++) if (i >= g_ns.verb_len || g_ns.verb[i] != lit[i]) return false;
    return i == g_ns.verb_len;
}

static const u8 g_ns_peer[4]  = { 10, 0, 0, 2 };
static const u8 g_ns_other[4] = { 10, 0, 0, 9 };

// ---------------------------------------------------------------------------
// accept: the listen open is the wait; everything after it is a dequeued
// connection's setup and rides a note out.
// ---------------------------------------------------------------------------
void test_vivsock_accept_wait_is_over(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    s64 lfd = ns_socket(p, VIV_NET_TCP, 7, VIV_SOCK_LISTENING);
    current_thread()->note_interruptible = true;   // accept is on signal(7)'s list
    s64  afd   = (lfd >= 0) ? viv_accept_for_test(p, (u64)lfd) : -1;
    bool after = current_thread()->note_interruptible;
    current_thread()->note_interruptible = false;
    struct viv_sock ea;
    bool arow  = ns_row(p, afd, &ea);
    u8   akind = ns_fd_kind(p, afd);
    u32  lopens = g_ns.listen_opens, reads = g_ns.ctl_reads, dopens = g_ns.data_opens;
    bool lintr  = g_ns.listen_intr,  rintr = g_ns.read_intr,  dintr  = g_ns.data_intr;
    ns_free(p);

    TEST_ASSERT(lfd >= 0, "the listening socket's ctl opened");
    TEST_ASSERT(afd >= 0, "accept returned the accepted connection's fd");
    TEST_EXPECT_EQ(lopens, 1u, "one listen open");
    TEST_ASSERT(lintr, "the listen open -- accept's wait -- ran note-interruptible");
    TEST_EXPECT_EQ(reads, 1u, "the accepted conversation's number was read off its ctl");
    TEST_ASSERT(!rintr, "that read came after the wait: a caught note rides it out");
    TEST_EXPECT_EQ(dopens, 1u, "one data open");
    TEST_ASSERT(!dintr,
        "the accepted data open rides a note out -- an EINTR there would hang up a "
        "connection netd has already handed over");
    TEST_ASSERT(!after, "accept leaves the flag clear");
    TEST_ASSERT(arow && ea.state == VIV_SOCK_CONNECTED && ea.n == 8,
                "the new fd is a CONNECTED row on the accepted conversation");
    TEST_EXPECT_EQ(akind, (u8)NS_DATA, "and names that conversation's data file");
}

// ---------------------------------------------------------------------------
// connect: the dial rides a note out; TCP's handshake is the wait.
// ---------------------------------------------------------------------------
void test_vivsock_connect_tcp_holds_the_dial(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    s64 fd = ns_socket(p, VIV_NET_TCP, 7, VIV_SOCK_FRESH);
    current_thread()->note_interruptible = true;   // connect is on signal(7)'s list
    s64  rc    = (fd >= 0) ? viv_sock_connect_for_test(p, (u64)fd, g_ns_peer, 80) : -1;
    bool after = current_thread()->note_interruptible;
    current_thread()->note_interruptible = false;
    struct viv_sock e;
    bool row   = ns_row(p, fd, &e);
    u8   kind  = ns_fd_kind(p, fd);
    u32  writes = g_ns.ctl_writes, dopens = g_ns.data_opens;
    bool dial_intr = g_ns.dial_intr, data_intr = g_ns.data_intr;
    bool verb  = ns_verb_is("connect 10.0.0.2!80");
    ns_free(p);

    TEST_ASSERT(fd >= 0, "the socket's ctl opened");
    TEST_EXPECT_EQ(rc, 0, "connect succeeded");
    TEST_EXPECT_EQ(writes, 1u, "one dial verb");
    TEST_ASSERT(verb, "the dial names the peer");
    TEST_ASSERT(!dial_intr,
        "the dial verb rides a note out -- a Twrite abandoned for a note may already "
        "have dialed");
    TEST_EXPECT_EQ(dopens, 1u, "one data open");
    TEST_ASSERT(data_intr, "the handshake wait (the held data open) is note-interruptible");
    TEST_ASSERT(!after, "connect leaves the flag clear");
    TEST_ASSERT(row && e.state == VIV_SOCK_CONNECTED && e.remote_addr == 0x0A000002u &&
                e.remote_port == 80, "CONNECTED to the peer");
    TEST_EXPECT_EQ(kind, (u8)NS_DATA, "the fd was swapped onto data");
}

static bool ns_fd_nonblock(struct Proc *p, s64 fd) {
    struct Handle h;
    if (fd < 0 || handle_get(p, (hidx_t)fd, &h) < 0) return false;
    bool nb = h.kind == KOBJ_SPOOR && (spoor_flag_get((struct Spoor *)h.obj) & CNONBLOCK);
    handle_put(&h);
    return nb;
}

// A peer thread that closes the socket while connect sleeps in the handshake,
// and opens its own file at the same number, keeps that file: the swap is keyed
// on the ctl Spoor the call started from, not on whatever the number names by
// the time the handshake ends.
void test_vivsock_connect_keeps_a_reused_number(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");

    // Control: an unraced nonblocking socket connects, its number names data,
    // and data carries ctl's O_NONBLOCK across the swap.
    s64 cfd = ns_socket(p, VIV_NET_TCP, 7, VIV_SOCK_FRESH);
    struct Handle ch;
    bool cgot = cfd >= 0 && handle_get(p, (hidx_t)cfd, &ch) == 0;
    if (cgot) {
        spoor_flag_set((struct Spoor *)ch.obj, CNONBLOCK);
        handle_put(&ch);
    }
    s64  crc   = cgot ? viv_sock_connect_for_test(p, (u64)cfd, g_ns_peer, 80) : -1;
    u8   ckind = ns_fd_kind(p, cfd);
    bool cnb   = ns_fd_nonblock(p, cfd);

    // The raced socket. The test's own reference on its ctl keeps the peer's
    // close from being ctl's last, so a connect that holds none still cannot
    // send a Tclunk from inside the stub's reply.
    s64 fd = ns_socket(p, VIV_NET_TCP, 9, VIV_SOCK_FRESH);
    struct Handle sh;
    bool sgot = fd >= 0 && handle_get(p, (hidx_t)fd, &sh) == 0;
    if (sgot) spoor_flag_set((struct Spoor *)sh.obj, CNONBLOCK);
    struct Spoor *prd = NULL, *pwr = NULL;
    bool piped = pipe_create(&prd, &pwr) == 0;
    if (piped && sgot) {
        g_ns.swap_p   = p;
        g_ns.swap_fd  = fd;
        g_ns.swap_got = -1;
        g_ns.swap_in  = prd;
    }
    s64 rc = (piped && sgot) ? viv_sock_connect_for_test(p, (u64)fd, g_ns_peer, 80) : -1;
    s64 landed = g_ns.swap_got;
    struct Handle ah;
    bool aget       = fd >= 0 && handle_get(p, (hidx_t)fd, &ah) == 0;
    bool names_pipe = aget && ah.kind == KOBJ_SPOOR && ah.obj == prd;
    if (aget) handle_put(&ah);
    if (sgot) handle_put(&sh);
    // The pipe end is the table's once it landed; otherwise still the test's.
    if (piped && (g_ns.swap_in || landed < 0)) spoor_clunk(prd);
    g_ns.swap_in = NULL;
    if (pwr) spoor_clunk(pwr);
    ns_free(p);

    TEST_ASSERT(cfd >= 0 && crc == 0, "control: an unraced connect succeeds");
    TEST_EXPECT_EQ(ckind, (u8)NS_DATA, "control: its number names data");
    TEST_ASSERT(cnb, "control: data carries ctl's O_NONBLOCK across the swap");
    TEST_ASSERT(fd >= 0 && sgot && piped, "fixture: the raced socket and the peer's pipe");
    TEST_EXPECT_EQ(landed, fd, "the peer's open landed on the socket's own number");
    TEST_EXPECT_EQ(rc, -(s64)T_E_IO,
        "connect refuses to swap a number that no longer names its socket");
    TEST_ASSERT(names_pipe,
        "the number still names the peer's pipe -- not swapped out and released");
}

static int ns_reserved_count(struct Proc *p) {
    int n = 0;
    for (hidx_t i = 0; i < PROC_HANDLE_MAX; i++)
        if (handle_is_reserved(p, i)) n++;
    return n;
}

// socket() reserves its number before the open: a refusal past the open and the
// read of N (no row left in the socket table) installs nothing, returns the
// number, and clunks the clone's ctl, which frees the conversation at netd. The
// control, one variable away: with a row free the same call installs once and
// keeps its ctl.
void test_vivsock_socket_unwind_publishes_nothing(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    g_ns.clone_n = 5;

    u64 i0   = handle_table_installs(p);
    u32 c0   = g_ns.ctl_clunks;
    s64 ok   = viv_socket_for_test(p, 2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
    u64 i_ok = handle_table_installs(p) - i0;
    u32 c_ok = g_ns.ctl_clunks - c0;
    struct viv_sock e;
    bool row  = ns_row(p, ok, &e);
    u8   kind = ns_fd_kind(p, ok);

    u32 filled = 0;   // rows on numbers no handle names: the table only counts
    while (filled < VIV_SOCK_MAX + 1u &&
           viv_socktab_claim(p->socktab, (s32)(PROC_HANDLE_MAX - 1 - (int)filled),
                             VIV_NET_TCP, 60u + filled, VIV_SOCK_FRESH, NULL))
        filled++;
    u64 i1     = handle_table_installs(p);
    u32 c1     = g_ns.ctl_clunks;
    u32 r1     = g_ns.ctl_reads;
    s64 full   = viv_socket_for_test(p, 2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
    u64 i_full = handle_table_installs(p) - i1;
    u32 c_full = g_ns.ctl_clunks - c1;
    u32 r_full = g_ns.ctl_reads - r1;
    int res    = ns_reserved_count(p);
    ns_free(p);

    TEST_ASSERT(ok >= 0, "control: socket() returns a descriptor");
    TEST_EXPECT_EQ(i_ok, (u64)1, "control: one install");
    TEST_EXPECT_EQ(c_ok, 0u, "control: the clone's ctl stays open");
    TEST_ASSERT(row && e.state == VIV_SOCK_FRESH && e.n == 5,
                "control: a FRESH row on the conversation the ctl named");
    TEST_EXPECT_EQ(kind, (u8)NS_CTL, "control: the descriptor names the ctl");
    TEST_ASSERT(filled > 0 && filled < VIV_SOCK_MAX + 1u, "fixture: the socket table filled");
    TEST_EXPECT_EQ(full, -(s64)T_E_MFILE, "socket: a full socket table is EMFILE");
    TEST_EXPECT_EQ(r_full, 1u,
        "socket: N was read first, so the refusal came past the open and the reservation");
    TEST_EXPECT_EQ(i_full, (u64)0, "socket: the refusal installed nothing");
    TEST_EXPECT_EQ(res, 0, "socket: and returned its number");
    TEST_EXPECT_EQ(c_full, 1u, "socket: and clunked the clone's ctl, freeing the conversation");
}

// A UDP connect never waits (Linux's is a table update), so none of it is
// interruptible -- the control that the TCP handshake's flag is the protocol's
// doing, not the call's.
void test_vivsock_connect_udp_never_waits(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    s64 fd = ns_socket(p, VIV_NET_UDP, 9, VIV_SOCK_FRESH);
    current_thread()->note_interruptible = true;
    s64 rc = (fd >= 0) ? viv_sock_connect_for_test(p, (u64)fd, g_ns_peer, 53) : -1;
    current_thread()->note_interruptible = false;
    struct viv_sock e;
    bool row = ns_row(p, fd, &e);
    u32  dopens = g_ns.data_opens;
    bool dial_intr = g_ns.dial_intr, data_intr = g_ns.data_intr;
    ns_free(p);

    TEST_ASSERT(fd >= 0, "the socket's ctl opened");
    TEST_EXPECT_EQ(rc, 0, "connect succeeded");
    TEST_ASSERT(!dial_intr, "the dial rides a note out");
    TEST_EXPECT_EQ(dopens, 1u, "one data open");
    TEST_ASSERT(!data_intr, "a UDP data open is no wait, and rides a note out too");
    TEST_ASSERT(row && e.state == VIV_SOCK_CONNECTED && e.remote_port == 53, "CONNECTED");
}

// A caught signal unwinds the handshake: connect reports EINTR -- not a refusal
// -- and the row is CONNECTING, so the retry (with an address Linux ignores in
// SS_CONNECTING) waits on the dial already made instead of dialing again.
void test_vivsock_connect_eintr_then_retry(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    s64 fd = ns_socket(p, VIV_NET_TCP, 11, VIV_SOCK_FRESH);
    g_ns.data_fail = 4;   // EINTR, as dev9p reports an open unwound for a note
    current_thread()->note_interruptible = true;
    s64 rc1 = (fd >= 0) ? viv_sock_connect_for_test(p, (u64)fd, g_ns_peer, 80) : -1;
    current_thread()->note_interruptible = false;
    struct viv_sock e1;
    bool row1   = ns_row(p, fd, &e1);
    u8   kind1  = ns_fd_kind(p, fd);
    u32  writes1 = g_ns.ctl_writes;

    current_thread()->note_interruptible = true;
    s64 rc2 = (fd >= 0) ? viv_sock_connect_for_test(p, (u64)fd, g_ns_other, 99) : -1;
    current_thread()->note_interruptible = false;
    struct viv_sock e2;
    bool row2   = ns_row(p, fd, &e2);
    u8   kind2  = ns_fd_kind(p, fd);
    u32  writes2 = g_ns.ctl_writes, dopens = g_ns.data_opens;
    ns_free(p);

    TEST_ASSERT(fd >= 0, "the socket's ctl opened");
    TEST_EXPECT_EQ(rc1, -(s64)T_E_INTR, "a caught signal is EINTR, not ECONNREFUSED");
    TEST_ASSERT(row1 && e1.state == VIV_SOCK_CONNECTING && e1.remote_addr == 0x0A000002u &&
                e1.remote_port == 80, "the row is CONNECTING, carrying the dialed peer");
    TEST_EXPECT_EQ(kind1, (u8)NS_CTL, "the fd still names ctl");
    TEST_EXPECT_EQ(writes1, 1u, "one dial");
    TEST_EXPECT_EQ(rc2, 0, "the retry completes the connection");
    TEST_EXPECT_EQ(writes2, 1u, "the retry did NOT dial again");
    TEST_EXPECT_EQ(dopens, 2u, "the retry waited on the data open again");
    TEST_ASSERT(row2 && e2.state == VIV_SOCK_CONNECTED && e2.remote_addr == 0x0A000002u &&
                e2.remote_port == 80, "CONNECTED to the peer first dialed, not the retry's");
    TEST_EXPECT_EQ(kind2, (u8)NS_DATA, "the fd was swapped onto data");
}

// The retry rule on its own, from a CONNECTING row the test builds -- a control
// that does not lean on the EINTR path above.
void test_vivsock_connect_connecting_resumes(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    s64 fd = ns_socket(p, VIV_NET_TCP, 12, VIV_SOCK_FRESH);
    struct viv_sock e0;
    bool begun = ns_row(p, fd, &e0) &&
                 viv_socktab_begin_connect(p->socktab, (s32)fd, e0.epoch, 0x0A000002u, 80);
    current_thread()->note_interruptible = true;
    s64 rc = begun ? viv_sock_connect_for_test(p, (u64)fd, g_ns_other, 99) : -1;
    current_thread()->note_interruptible = false;
    struct viv_sock e;
    bool row = ns_row(p, fd, &e);
    u32  writes = g_ns.ctl_writes, dopens = g_ns.data_opens;
    bool data_intr = g_ns.data_intr;
    ns_free(p);

    TEST_ASSERT(begun, "the row was CONNECTING before the call");
    TEST_EXPECT_EQ(rc, 0, "connect completed");
    TEST_EXPECT_EQ(writes, 0u, "a CONNECTING row is not dialed again");
    TEST_EXPECT_EQ(dopens, 1u, "it waits on the data open");
    TEST_ASSERT(data_intr, "and that wait is note-interruptible");
    TEST_ASSERT(row && e.state == VIV_SOCK_CONNECTED && e.remote_addr == 0x0A000002u &&
                e.remote_port == 80, "CONNECTED to the dialed peer");
}

// netd's two failure verdicts reach the guest as themselves: a timed-out dial
// is ETIMEDOUT, a refused one ECONNREFUSED. Either leaves the row FRESH, so a
// retry dials anew (Linux resets a failed connect to unconnected).
void test_vivsock_connect_failure_verdicts(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    s64 tfd = ns_socket(p, VIV_NET_TCP, 13, VIV_SOCK_FRESH);
    s64 rfd = ns_socket(p, VIV_NET_TCP, 14, VIV_SOCK_FRESH);
    g_ns.data_fail = 110;   // ETIMEDOUT: netd's connect deadline
    current_thread()->note_interruptible = true;
    s64 rc_t = (tfd >= 0) ? viv_sock_connect_for_test(p, (u64)tfd, g_ns_peer, 80) : -1;
    current_thread()->note_interruptible = false;
    struct viv_sock et;
    bool trow = ns_row(p, tfd, &et);

    g_ns.data_fail = 111;   // ECONNREFUSED: the peer reset the handshake
    current_thread()->note_interruptible = true;
    s64 rc_r = (rfd >= 0) ? viv_sock_connect_for_test(p, (u64)rfd, g_ns_peer, 80) : -1;
    current_thread()->note_interruptible = false;
    struct viv_sock er;
    bool rrow = ns_row(p, rfd, &er);
    u32  writes_before = g_ns.ctl_writes;

    current_thread()->note_interruptible = true;
    s64 rc_again = (rfd >= 0) ? viv_sock_connect_for_test(p, (u64)rfd, g_ns_peer, 80) : -1;
    current_thread()->note_interruptible = false;
    struct viv_sock ea;
    bool arow = ns_row(p, rfd, &ea);
    u32  writes_after = g_ns.ctl_writes;
    ns_free(p);

    TEST_ASSERT(tfd >= 0 && rfd >= 0, "both sockets' ctl opened");
    TEST_EXPECT_EQ(rc_t, -(s64)T_E_TIMEDOUT, "a timed-out dial is ETIMEDOUT, as netd says");
    TEST_ASSERT(trow && et.state == VIV_SOCK_FRESH && et.remote_port == 0,
                "the timed-out row is FRESH with its peer forgotten");
    TEST_EXPECT_EQ(rc_r, -(s64)T_E_CONNREFUSED, "a refused dial stays ECONNREFUSED");
    TEST_ASSERT(rrow && er.state == VIV_SOCK_FRESH && er.remote_port == 0,
                "the refused row is FRESH with its peer forgotten");
    TEST_EXPECT_EQ(rc_again, 0, "a refused socket can connect again");
    TEST_EXPECT_EQ(writes_after - writes_before, 1u, "the retry dialed anew");
    TEST_ASSERT(arow && ea.state == VIV_SOCK_CONNECTED, "and is CONNECTED");
}

// ---------------------------------------------------------------------------
// A connect a signal interrupted is finished by the socket's next use.
// ---------------------------------------------------------------------------

// A CONNECTING row as an interrupted connect leaves one: the dial made, the fd
// still on ctl. Built directly, so these tests do not lean on the EINTR path.
static bool ns_connecting(struct Proc *p, s64 fd) {
    struct viv_sock e;
    return ns_row(p, fd, &e) &&
           viv_socktab_begin_connect(p->socktab, (s32)fd, e.epoch, 0x0A000002u, 80);
}

static void ns_frame(struct exception_context *c, u64 linux_nr, s64 fd) {
    for (size_t i = 0; i < sizeof(*c); i++) ((u8 *)c)[i] = 0;
    c->regs[8] = linux_nr;
    c->regs[0] = (u64)fd;
}

// send and recv wait for the handshake, as Linux's do, before they move data.
// The wait is theirs, so it is note-interruptible, and the flag is theirs again
// for the transfer. A FRESH socket is the control: nothing to finish, so send
// declines as it always has, without touching the connection.
void test_vivsock_send_recv_finish_connect(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    s64  sfd    = ns_socket(p, VIV_NET_TCP, 15, VIV_SOCK_FRESH);
    s64  rfd    = ns_socket(p, VIV_NET_TCP, 16, VIV_SOCK_FRESH);
    s64  ffd    = ns_socket(p, VIV_NET_TCP, 17, VIV_SOCK_FRESH);
    s64  mfd    = ns_socket(p, VIV_NET_TCP, 27, VIV_SOCK_FRESH);
    bool begun  = ns_connecting(p, sfd) && ns_connecting(p, rfd) && ns_connecting(p, mfd);

    current_thread()->note_interruptible = true;   // send and recv are on signal(7)'s list
    s64  src    = begun ? viv_sendto_empty_for_test(p, (u64)sfd) : -1;
    bool safter = current_thread()->note_interruptible;
    bool sintr  = g_ns.data_intr;
    u32  sopens = g_ns.data_opens;
    struct viv_sock es;
    bool srow   = ns_row(p, sfd, &es);
    u8   skind  = ns_fd_kind(p, sfd);

    g_ns.data_fail = 4;                            // a caught signal in recv's wait
    s64  rrc1   = begun ? viv_recvfrom_empty_for_test(p, (u64)rfd) : -1;
    struct viv_sock er1;
    bool rrow1  = ns_row(p, rfd, &er1);
    u8   rkind1 = ns_fd_kind(p, rfd);
    s64  rrc2   = begun ? viv_recvfrom_empty_for_test(p, (u64)rfd) : -1;
    struct viv_sock er2;
    bool rrow2  = ns_row(p, rfd, &er2);
    u8   rkind2 = ns_fd_kind(p, rfd);
    u32  ropens = g_ns.data_opens - sopens;

    // A kernel address is never a user msghdr, so recvmsg ends in EFAULT -- but
    // only once the connect is finished and the socket is one it serves.
    s64  mrc    = begun ? viv_recvmsg_for_test(p, (u64)mfd, (u64)&g_ns) : -1;
    struct viv_sock em;
    bool mrow   = ns_row(p, mfd, &em);

    u32  mark   = g_ns.data_opens;
    s64  frc    = (ffd >= 0) ? viv_sendto_empty_for_test(p, (u64)ffd) : -1;
    u32  fopens = g_ns.data_opens - mark;
    u32  writes = g_ns.ctl_writes;
    current_thread()->note_interruptible = false;
    ns_free(p);

    TEST_ASSERT(begun && ffd >= 0, "four sockets, three of them CONNECTING");
    TEST_ASSERT(src != -(s64)T_E_NOSYS, "send on a CONNECTING socket is served, not declined");
    TEST_EXPECT_EQ(sopens, 1u, "send waited on the handshake: one data open");
    TEST_ASSERT(sintr, "that wait is send's, so it is note-interruptible");
    TEST_ASSERT(safter, "and the flag is send's again for the transfer");
    TEST_ASSERT(srow && es.state == VIV_SOCK_CONNECTED && es.remote_addr == 0x0A000002u &&
                es.remote_port == 80, "the socket is CONNECTED to the dialed peer");
    TEST_EXPECT_EQ(skind, (u8)NS_DATA, "and its fd names data");
    TEST_EXPECT_EQ(rrc1, -(s64)T_E_INTR, "a signal in recv's wait is EINTR");
    TEST_ASSERT(rrow1 && er1.state == VIV_SOCK_CONNECTING, "and leaves the socket CONNECTING");
    TEST_EXPECT_EQ(rkind1, (u8)NS_CTL, "on ctl");
    TEST_ASSERT(rrc2 != -(s64)T_E_NOSYS, "the next recv is served");
    TEST_ASSERT(rrow2 && er2.state == VIV_SOCK_CONNECTED, "and finishes the connect");
    TEST_EXPECT_EQ(rkind2, (u8)NS_DATA, "onto data");
    TEST_EXPECT_EQ(ropens, 2u, "each recv waited once");
    TEST_EXPECT_EQ(mrc, -(s64)T_E_FAULT, "recvmsg finished the connect, then read its msghdr");
    TEST_ASSERT(mrow && em.state == VIV_SOCK_CONNECTED, "leaving the socket CONNECTED");
    TEST_EXPECT_EQ(writes, 0u, "nothing dialed again: every wait was on the dial already made");
    TEST_EXPECT_EQ(frc, -(s64)T_E_NOSYS, "control: send on a FRESH socket declines as before");
    TEST_EXPECT_EQ(fopens, 0u, "without a data open");
}

// 13:05Z (width audit r1 F1): sendto/recvfrom narrow their fd to 32 bits at the
// dispatch (Linux declares `int fd`), and the narrowing wants a RUNTIME witness,
// not just a reading one. A connected send()/recv() (no address) delegates to
// the native write/read handler BY NUMBER, and that handler's sys_hidx_arg
// refuses any descriptor above 0x7fffffff -- so a word with junk above bit 31
// that is NOT narrowed falls out as EBADF, while the narrowed low 32 bits name
// the real socket. (viv_sock_row re-narrows internally via its own (u32) cast,
// so the discriminating consumer is the handler, not the row lookup -- which is
// why the mutant that drops the `(u64)(u32)` at the dispatch is caught here and
// nowhere upstream.) A zero-length send/recv needs no user buffer and answers 0
// on a live fd, EBADF on a wild one.
void test_vivsock_sendto_recvfrom_narrow_fd(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    s64 sfd = ns_socket(p, VIV_NET_TCP, 41, VIV_SOCK_CONNECTED);
    s64 rfd = ns_socket(p, VIV_NET_TCP, 42, VIV_SOCK_CONNECTED);
    TEST_ASSERT(sfd >= 0 && rfd >= 0, "fixture: two CONNECTED sockets");

    const u64 junk = (u64)1 << 32;   // above the int fd Linux reads

    // A connected send()/recv() delegates to the native write/read handler,
    // which re-derives the Proc from current_thread() (see viv_sock_sendto's
    // note) -- so the fixture Proc must be current for the fd to resolve, and
    // the discriminating refusal (sys_hidx_arg rejects > 0x7fffffff) lives in
    // that handler. Saved and restored around the dispatches (the test_pts.c
    // pattern); a zero-length op touches no user memory, so no TTBR0 swap.
    struct Thread *th = current_thread();
    struct Proc   *saved = th->proc;
    th->proc = p;

    // sendto: control (clean fd) then witness (junk high word). x1..x5 stay 0,
    // so this is a connected send() of length 0 -- no address, no buffer.
    struct exception_context sc;
    ns_frame(&sc, VIV_LINUX_SENDTO, sfd);
    (void)viv_linux_dispatch_for_test(&sc, p);
    s64 s_clean = (s64)sc.regs[0];

    struct exception_context sj;
    ns_frame(&sj, VIV_LINUX_SENDTO, sfd);
    sj.regs[0] = junk | (u64)sfd;
    (void)viv_linux_dispatch_for_test(&sj, p);
    s64 s_junk = (s64)sj.regs[0];

    // recvfrom: control then witness, the same shape (NULL src-addr, flags 0).
    struct exception_context rc;
    ns_frame(&rc, VIV_LINUX_RECVFROM, rfd);
    (void)viv_linux_dispatch_for_test(&rc, p);
    s64 r_clean = (s64)rc.regs[0];

    struct exception_context rj;
    ns_frame(&rj, VIV_LINUX_RECVFROM, rfd);
    rj.regs[0] = junk | (u64)rfd;
    (void)viv_linux_dispatch_for_test(&rj, p);
    s64 r_junk = (s64)rj.regs[0];

    th->proc = saved;

    TEST_EXPECT_EQ(s_clean, 0, "sendto: a zero-length connected send on a live fd is 0");
    TEST_EXPECT_EQ(s_junk, s_clean, "sendto narrows the fd: junk above bit 31 reaches the same socket");
    TEST_EXPECT_NE(s_junk, -(s64)T_E_BADF, "sendto: the junk fd is not refused as a wild descriptor");
    TEST_EXPECT_EQ(r_clean, 0, "recvfrom: a zero-length connected recv on a live fd is 0");
    TEST_EXPECT_EQ(r_junk, r_clean, "recvfrom narrows the fd: junk above bit 31 reaches the same socket");
    TEST_EXPECT_NE(r_junk, -(s64)T_E_BADF, "recvfrom: the junk fd is not refused as a wild descriptor");
}

// read and write reach the file an fd names, and a CONNECTING socket's names
// ctl: the dispatcher's entry hook finishes the connect first. Driven through
// the dispatcher itself, since its classification is what makes the wait
// interruptible (a socket is a slow fd).
void test_vivsock_read_write_finish_connect(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    s64  wfd   = ns_socket(p, VIV_NET_TCP, 18, VIV_SOCK_FRESH);
    s64  rfd   = ns_socket(p, VIV_NET_TCP, 19, VIV_SOCK_FRESH);
    s64  ffd   = ns_socket(p, VIV_NET_TCP, 20, VIV_SOCK_FRESH);
    bool begun = ns_connecting(p, wfd) && ns_connecting(p, rfd);

    struct exception_context w;
    ns_frame(&w, VIV_LINUX_WRITE, wfd);
    bool wnative = begun && viv_linux_dispatch_for_test(&w, p);
    bool wintr   = g_ns.data_intr;
    u32  wopens  = g_ns.data_opens;
    struct viv_sock ew;
    bool wrow    = ns_row(p, wfd, &ew);
    u8   wkind   = ns_fd_kind(p, wfd);

    g_ns.data_fail = 4;                            // a caught signal in read's wait
    struct exception_context r;
    ns_frame(&r, VIV_LINUX_READ, rfd);
    bool rnative = begun && viv_linux_dispatch_for_test(&r, p);
    struct viv_sock er;
    bool rrow    = ns_row(p, rfd, &er);
    u8   rkind   = ns_fd_kind(p, rfd);

    struct exception_context f;
    ns_frame(&f, VIV_LINUX_WRITE, ffd);
    u32  before  = g_ns.data_opens;
    bool fnative = (ffd >= 0) && viv_linux_dispatch_for_test(&f, p);
    u32  fopens  = g_ns.data_opens - before;
    struct viv_sock ef;
    bool frow    = ns_row(p, ffd, &ef);
    current_thread()->note_interruptible = false;  // syscall_dispatch's job on the way out
    ns_free(p);

    TEST_ASSERT(begun && ffd >= 0, "three sockets, two of them CONNECTING");
    TEST_ASSERT(wnative, "write goes on to the native handler");
    TEST_EXPECT_EQ(w.regs[8], (u64)SYS_WRITE, "renumbered to SYS_WRITE");
    TEST_EXPECT_EQ(wopens, 1u, "after waiting on the handshake: one data open");
    TEST_ASSERT(wintr, "a socket is a slow fd, so write's wait is note-interruptible");
    TEST_ASSERT(wrow && ew.state == VIV_SOCK_CONNECTED, "the socket is CONNECTED");
    TEST_EXPECT_EQ(wkind, (u8)NS_DATA, "so write reaches data, not ctl");
    TEST_ASSERT(!rnative, "a signal in read's wait ends the call before the native read");
    TEST_EXPECT_EQ(r.regs[0], (u64)(-(s64)T_E_INTR), "with EINTR");
    TEST_ASSERT(rrow && er.state == VIV_SOCK_CONNECTING, "and the socket stays CONNECTING");
    TEST_EXPECT_EQ(rkind, (u8)NS_CTL, "on ctl");
    TEST_ASSERT(fnative, "control: write on a FRESH socket goes straight to the native handler");
    TEST_EXPECT_EQ(fopens, 0u, "with nothing to finish");
    TEST_ASSERT(frow && ef.state == VIV_SOCK_FRESH, "and the socket stays FRESH");
}

// SO_ERROR on a CONNECTING socket reports the interrupted connect's outcome
// and never waits: 0 while netd's status file says the handshake is in flight,
// and once it has resolved the connect is finished and a failed dial is the
// error. A missing status file and a FRESH socket both answer 0 without
// touching the connection.
void test_vivsock_so_error_reports_the_dial(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    s64  afd   = ns_socket(p, VIV_NET_TCP, 21, VIV_SOCK_FRESH);   // in flight
    s64  bfd   = ns_socket(p, VIV_NET_TCP, 22, VIV_SOCK_FRESH);   // refused
    s64  cfd   = ns_socket(p, VIV_NET_TCP, 23, VIV_SOCK_FRESH);   // timed out
    s64  dfd   = ns_socket(p, VIV_NET_TCP, 24, VIV_SOCK_FRESH);   // established
    s64  efd   = ns_socket(p, VIV_NET_TCP, 25, VIV_SOCK_FRESH);   // no status file
    s64  ffd   = ns_socket(p, VIV_NET_TCP, 26, VIV_SOCK_FRESH);   // never connected
    bool begun = ns_connecting(p, afd) && ns_connecting(p, bfd) && ns_connecting(p, cfd) &&
                 ns_connecting(p, dfd) && ns_connecting(p, efd);

    g_ns.status = "Syn-Sent";
    s32  a       = begun ? viv_sock_pending_error_for_test(p, (u64)afd) : -1;
    u32  a_opens = g_ns.data_opens, a_reads = g_ns.status_reads;
    struct viv_sock ea;
    bool arow    = ns_row(p, afd, &ea);

    g_ns.status    = "Closed";
    g_ns.data_fail = 111;                          // the peer reset the handshake
    s32  b       = begun ? viv_sock_pending_error_for_test(p, (u64)bfd) : -1;
    struct viv_sock eb;
    bool brow    = ns_row(p, bfd, &eb);

    g_ns.data_fail = 110;                          // netd's connect deadline
    s32  c       = begun ? viv_sock_pending_error_for_test(p, (u64)cfd) : -1;
    struct viv_sock ec;
    bool crow    = ns_row(p, cfd, &ec);

    g_ns.status  = "Established";
    s32  d       = begun ? viv_sock_pending_error_for_test(p, (u64)dfd) : -1;
    struct viv_sock ed;
    bool drow    = ns_row(p, dfd, &ed);
    u8   dkind   = ns_fd_kind(p, dfd);

    g_ns.status  = NULL;
    u32  e_base  = g_ns.data_opens;
    s32  e       = begun ? viv_sock_pending_error_for_test(p, (u64)efd) : -1;
    u32  e_opens = g_ns.data_opens - e_base;
    struct viv_sock ee;
    bool erow    = ns_row(p, efd, &ee);

    g_ns.status  = "Closed";
    u32  f_base  = g_ns.status_reads + g_ns.data_opens;
    s32  f       = (ffd >= 0) ? viv_sock_pending_error_for_test(p, (u64)ffd) : -1;
    u32  f_asks  = g_ns.status_reads + g_ns.data_opens - f_base;
    u32  writes  = g_ns.ctl_writes;
    ns_free(p);

    TEST_ASSERT(begun && ffd >= 0, "five CONNECTING sockets and a FRESH one");
    TEST_EXPECT_EQ(a, 0, "a handshake in flight has no error yet");
    TEST_EXPECT_EQ(a_reads, 1u, "which netd's status file said");
    TEST_EXPECT_EQ(a_opens, 0u, "so nothing waited on the connection");
    TEST_ASSERT(arow && ea.state == VIV_SOCK_CONNECTING, "and it is still CONNECTING");
    TEST_EXPECT_EQ(b, (s32)T_E_CONNREFUSED, "a refused dial is ECONNREFUSED");
    TEST_ASSERT(brow && eb.state == VIV_SOCK_FRESH, "and its socket is reset to FRESH");
    TEST_EXPECT_EQ(c, (s32)T_E_TIMEDOUT, "a timed-out dial is ETIMEDOUT");
    TEST_ASSERT(crow && ec.state == VIV_SOCK_FRESH, "and its socket is reset to FRESH");
    TEST_EXPECT_EQ(d, 0, "an established connection has no error");
    TEST_ASSERT(drow && ed.state == VIV_SOCK_CONNECTED, "and SO_ERROR finished it");
    TEST_EXPECT_EQ(dkind, (u8)NS_DATA, "onto data");
    TEST_EXPECT_EQ(e, 0, "an unreadable status counts as in flight");
    TEST_EXPECT_EQ(e_opens, 0u, "so SO_ERROR still never waits");
    TEST_ASSERT(erow && ee.state == VIV_SOCK_CONNECTING, "and the socket stays CONNECTING");
    TEST_EXPECT_EQ(f, 0, "a socket never connected has no pending error");
    TEST_EXPECT_EQ(f_asks, 0u, "and SO_ERROR asks netd nothing about it");
    TEST_EXPECT_EQ(writes, 0u, "nothing dialed");
}

// pread64 and pwrite64 reach the file an fd names as read and write do, so a
// CONNECTING socket's finish its connect before they reach it -- never ctl.
// (Linux answers ESPIPE on any socket, which the errno registry cannot say yet.)
void test_vivsock_positioned_io_finishes_connect(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    s64  wfd   = ns_socket(p, VIV_NET_TCP, 28, VIV_SOCK_FRESH);
    s64  rfd   = ns_socket(p, VIV_NET_TCP, 29, VIV_SOCK_FRESH);
    bool begun = ns_connecting(p, wfd) && ns_connecting(p, rfd);

    struct exception_context w;
    ns_frame(&w, VIV_LINUX_PWRITE64, wfd);
    bool wnative = begun && viv_linux_dispatch_for_test(&w, p);
    u32  wopens  = g_ns.data_opens;
    struct viv_sock ew;
    bool wrow    = ns_row(p, wfd, &ew);
    u8   wkind   = ns_fd_kind(p, wfd);

    struct exception_context r;
    ns_frame(&r, VIV_LINUX_PREAD64, rfd);
    bool rnative = begun && viv_linux_dispatch_for_test(&r, p);
    u32  ropens  = g_ns.data_opens - wopens;
    struct viv_sock er;
    bool rrow    = ns_row(p, rfd, &er);
    u8   rkind   = ns_fd_kind(p, rfd);
    u32  touched = g_ns.ctl_reads + g_ns.ctl_writes;
    current_thread()->note_interruptible = false;  // syscall_dispatch's job on the way out
    ns_free(p);

    TEST_ASSERT(begun, "two CONNECTING sockets");
    TEST_ASSERT(wnative, "pwrite64 goes on to the native handler");
    TEST_EXPECT_EQ(w.regs[8], (u64)SYS_PWRITE, "renumbered to SYS_PWRITE");
    TEST_EXPECT_EQ(wopens, 1u, "pwrite64 finished the connect first: one data open");
    TEST_ASSERT(wrow && ew.state == VIV_SOCK_CONNECTED, "the pwrite64 socket is CONNECTED");
    TEST_EXPECT_EQ(wkind, (u8)NS_DATA, "so pwrite64 reaches data, not ctl");
    TEST_ASSERT(rnative, "pread64 goes on to the native handler");
    TEST_EXPECT_EQ(r.regs[8], (u64)SYS_PREAD, "renumbered to SYS_PREAD");
    TEST_EXPECT_EQ(ropens, 1u, "pread64 finished the connect first: one data open");
    TEST_ASSERT(rrow && er.state == VIV_SOCK_CONNECTED, "the pread64 socket is CONNECTED");
    TEST_EXPECT_EQ(rkind, (u8)NS_DATA, "so pread64 reaches data, not ctl");
    TEST_EXPECT_EQ(touched, 0u, "nothing read or wrote ctl");
}

// Every free descriptor slot takes another reference to fd's file. The count.
static u32 ns_fill_fds(struct Proc *p, s64 fd) {
    struct Handle h;
    if (fd < 0 || handle_get(p, (hidx_t)fd, &h) < 0) return 0;
    struct Spoor *s = (struct Spoor *)h.obj;
    u32 n = 0;
    for (;;) {
        spoor_ref(s);
        if (handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, s) < 0) { spoor_clunk(s); break; }
        n++;
    }
    handle_put(&h);
    return n;
}

// SO_ERROR reads netd's status file without taking a descriptor of the guest's:
// a guest whose table is full still gets netd's answer rather than a guess,
// and no peer thread can see a transient fd.
void test_vivsock_so_error_needs_no_descriptor(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    s64  afd    = ns_socket(p, VIV_NET_TCP, 30, VIV_SOCK_FRESH);
    bool begun  = ns_connecting(p, afd);
    u32  filled = begun ? ns_fill_fds(p, afd) : 0;

    g_ns.status = "Syn-Sent";
    u32  reads0 = g_ns.status_reads, opens0 = g_ns.data_opens;
    s32  a      = begun ? viv_sock_pending_error_for_test(p, (u64)afd) : -1;
    u32  reads  = g_ns.status_reads - reads0;
    u32  opens  = g_ns.data_opens - opens0;
    struct viv_sock ea;
    bool arow   = ns_row(p, afd, &ea);
    ns_free(p);

    TEST_ASSERT(begun, "a CONNECTING socket");
    TEST_ASSERT(filled + 16u >= (u32)PROC_HANDLE_MAX,
                "control: the descriptor table was filled to its ceiling");
    TEST_EXPECT_EQ(reads, 1u, "SO_ERROR read netd's status with no descriptor free");
    TEST_EXPECT_EQ(a, 0, "and the handshake it reports is in flight: no error yet");
    TEST_EXPECT_EQ(opens, 0u, "so nothing waited on the connection");
    TEST_ASSERT(arow && ea.state == VIV_SOCK_CONNECTING, "and the socket is still CONNECTING");
}

// ---------------------------------------------------------------------------
// The ctl-verb arms write only to the socket's own ctl.
// ---------------------------------------------------------------------------

// The number's row, bound to `port` as bind() leaves it.
static bool ns_bind_row(struct Proc *p, s64 fd, u16 port) {
    struct viv_sock e;
    return ns_row(p, fd, &e) && viv_socktab_set_bound(p->socktab, (s32)fd, e.epoch, 0, port);
}

// dup3(file, fd) held at its window: the slot already holds `file` (a reference
// of the slot's own) while the row, which dup3 drops only after the install,
// still names the socket.
static bool ns_dup3_window(struct Proc *p, s64 fd, struct Spoor *file) {
    struct Handle h;
    if (fd < 0 || !file || handle_get(p, (hidx_t)fd, &h) < 0) return false;
    spoor_ref(file);
    // The key is held across the replace: handle_get's reference.
    bool held = handle_replace(p, (hidx_t)fd, h.obj, KOBJ_SPOOR,
                               RIGHT_READ | RIGHT_WRITE, file) == 0;
    if (!held) spoor_clunk(file);
    handle_put(&h);
    return held;
}

static bool ns_slot_is(struct Proc *p, s64 fd, const struct Spoor *sp) {
    struct Handle h;
    if (fd < 0 || handle_get(p, (hidx_t)fd, &h) < 0) return false;
    bool is = (h.obj == sp);
    handle_put(&h);
    return is;
}

// The ctl identity `fd`'s row holds (NULL for none, or no row). White box.
static const struct Spoor *ns_row_ctl(const struct viv_socktab *tab, s64 fd) {
    for (u32 i = 0; tab && i < VIV_SOCK_MAX; i++)
        if (tab->s[i].state != VIV_SOCK_FREE && tab->s[i].fd == (s32)fd) return tab->ctl[i];
    return NULL;
}

static int ns_ref(const struct Spoor *s) {
    return s ? __atomic_load_n(&s->ref, __ATOMIC_ACQUIRE) : -1;
}

// A peer thread's dup3(file, sockfd) installs the file BEFORE it drops the
// socket's row, so for a moment the number's row says "socket" while its slot
// holds the file. An arm that read the row in that moment pins the file; each
// ctl-verb arm -- connect's dial, listen's announce, the datagram sendto's
// re-dial -- must refuse it (ENOTSOCK: the number is a file now) before
// writing a byte, and connect must not swap the file out for a connection. The
// control, one variable away: the same arms on unswapped sockets write their
// verbs to ctl -- and a socket()-made row holds its ctl with a reference of its
// own, which a connect gives up once the fd is data.
void test_vivsock_ctl_verbs_need_their_ctl(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    int ferr = 0;
    struct Spoor *file = ns_open(p, VIV_NET_TCP, 0, "remote", &ferr);   // the peer's file
    u32  fw0  = g_ns.file_writes;
    long fput = file ? file->dev->write(file, "x", 1, 0) : -1;
    u32  fwc  = g_ns.file_writes - fw0;

    s64  ccfd   = ns_socket(p, VIV_NET_TCP, 31, VIV_SOCK_FRESH);
    const struct Spoor *ccid = ns_row_ctl(p->socktab, ccfd);
    int  ccref  = ns_ref(ccid);
    u32  m0 = g_ns.ctl_writes, k0 = g_ns.ctl_clunks;
    s64  ccrc   = (ccfd >= 0) ? viv_sock_connect_for_test(p, (u64)ccfd, g_ns_peer, 80) : -1;
    u32  ccw = g_ns.ctl_writes - m0, cck = g_ns.ctl_clunks - k0;
    bool ccverb = ns_verb_is("connect 10.0.0.2!80");
    bool ccgone = (ccfd >= 0) && ns_row_ctl(p->socktab, ccfd) == NULL;

    s64  clfd   = ns_socket(p, VIV_NET_TCP, 32, VIV_SOCK_FRESH);
    bool clb    = ns_bind_row(p, clfd, 8080);
    u32  m1     = g_ns.ctl_writes;
    s64  clrc   = clb ? viv_listen_for_test(p, (u64)clfd) : -1;
    u32  clw    = g_ns.ctl_writes - m1;
    bool clverb = ns_verb_is("announce *!8080");

    s64  cdfd   = ns_socket(p, VIV_NET_UDP, 33, VIV_SOCK_FRESH);
    u32  m2 = g_ns.ctl_writes, o2 = g_ns.data_opens;
    s64  cdrc   = (cdfd >= 0) ? viv_sock_dgram_send_for_test(p, (u64)cdfd, g_ns_peer, 53) : -1;
    u32  cdw = g_ns.ctl_writes - m2, cdo = g_ns.data_opens - o2;
    bool cdverb = ns_verb_is("connect 10.0.0.2!53");

    s64  sc  = ns_socket(p, VIV_NET_TCP, 34, VIV_SOCK_FRESH);
    s64  sl  = ns_socket(p, VIV_NET_TCP, 35, VIV_SOCK_FRESH);
    s64  sd  = ns_socket(p, VIV_NET_UDP, 36, VIV_SOCK_FRESH);
    bool slb = ns_bind_row(p, sl, 8081);
    bool held = file && slb && ns_dup3_window(p, sc, file) &&
                ns_dup3_window(p, sl, file) && ns_dup3_window(p, sd, file);
    u32  w0 = g_ns.ctl_writes, f0 = g_ns.file_writes, o0 = g_ns.data_opens;
    s64  src = held ? viv_sock_connect_for_test(p, (u64)sc, g_ns_peer, 80) : -1;
    s64  srl = held ? viv_listen_for_test(p, (u64)sl) : -1;
    s64  srd = held ? viv_sock_dgram_send_for_test(p, (u64)sd, g_ns_peer, 53) : -1;
    u32  sw = g_ns.ctl_writes - w0, sf = g_ns.file_writes - f0, so = g_ns.data_opens - o0;
    bool sckeep = ns_slot_is(p, sc, file);
    struct viv_sock el, ed;
    bool slrow = ns_row(p, sl, &el), sdrow = ns_row(p, sd, &ed);
    if (file) spoor_clunk(file);
    ns_free(p);

    TEST_ASSERT(file != NULL, "fixture: the peer's file, a writable /net file that is no ctl");
    TEST_EXPECT_EQ(fput, 1, "control: a byte written to the file lands");
    TEST_EXPECT_EQ(fwc, 1u, "control: and the stub counts it, so the zero below is a count");
    TEST_ASSERT(ccid != NULL, "a socket()-made row names its ctl");
    TEST_EXPECT_EQ(ccref, 2, "with a reference of its own beside the fd's");
    TEST_EXPECT_EQ(ccrc, 0, "control: connect on an unraced socket succeeds");
    TEST_EXPECT_EQ(ccw, 1u, "control: its dial is written to ctl");
    TEST_ASSERT(ccverb, "control: naming the peer");
    TEST_ASSERT(ccgone, "a CONNECTED row gives its ctl identity up");
    TEST_EXPECT_EQ(cck, 1u, "so ctl's fid is released once data holds the connection");
    TEST_EXPECT_EQ(clrc, 0, "control: listen on an unraced socket succeeds");
    TEST_EXPECT_EQ(clw, 1u, "control: its announce is written to ctl");
    TEST_ASSERT(clverb, "control: for the bound port");
    TEST_EXPECT_EQ(cdrc, 0, "control: a datagram sendto on an unraced socket succeeds");
    TEST_EXPECT_EQ(cdw, 1u, "control: its re-dial is written to ctl");
    TEST_ASSERT(cdverb, "control: naming the destination");
    TEST_EXPECT_EQ(cdo, 1u, "control: and the datagram rides a data fid");

    TEST_ASSERT(held, "fixture: three numbers held at dup3's window");
    TEST_EXPECT_EQ(src, -(s64)T_E_NOTSOCK, "connect refuses a number whose slot is not its ctl");
    TEST_EXPECT_EQ(srl, -(s64)T_E_NOTSOCK, "listen refuses it");
    TEST_EXPECT_EQ(srd, -(s64)T_E_NOTSOCK, "the datagram sendto refuses it");
    TEST_EXPECT_EQ(sf, 0u, "not one verb reached the file");
    TEST_EXPECT_EQ(sw, 0u, "nor any ctl");
    TEST_EXPECT_EQ(so, 0u, "and no data file was opened");
    TEST_ASSERT(sckeep, "connect left the file at the number, not swapped out for a connection");
    TEST_ASSERT(slrow && el.state == VIV_SOCK_FRESH, "listen transitioned nothing");
    TEST_ASSERT(sdrow && ed.remote_port == 0, "the sendto recorded no destination");
}

// What a peer thread does in the moment between an arm's row read and its pin.
enum { PEER_IDLE, PEER_CONNECTS, PEER_CLOSES };
static struct {
    struct Proc  *p;          // only this fixture's calls meet the peer
    int           act;        // fires at the next pin, then goes idle
    struct Spoor *dsp;        // PEER_CONNECTS: the data fid it opened ahead
    struct Spoor *detached;   // its commit's ctl reference, for the test to clunk
    bool          done;
} g_peer;

static void ns_peer_at_pin(struct Proc *p, u64 fd) {
    if (p != g_peer.p || g_peer.act == PEER_IDLE) return;
    int act = g_peer.act;
    g_peer.act = PEER_IDLE;
    if (act == PEER_CLOSES) {
        // close() as the dispatcher runs it: the row first, then the number.
        viv_socktab_drop_closing(p->socktab, p, (s32)fd);
        g_peer.done = handle_close(p, (hidx_t)fd) == 0;
        return;
    }
    // connect() completing on the peer thread: the fd swapped onto data, the
    // row committed CONNECTED in the swap's own hold.
    struct viv_sock e;
    struct Handle   h;
    if (!g_peer.dsp || !viv_socktab_get(p->socktab, (s32)fd, &e) ||
        handle_get(p, (hidx_t)fd, &h) < 0)
        return;
    struct viv_socktab_connected c = {
        .tab = p->socktab, .fd = (s32)fd, .expect_epoch = e.epoch,
        .ctl = (const struct Spoor *)h.obj, .addr = 0x0A000002u, .port = 80,
        .ok = false, .detached = NULL,
    };
    // The key is held across the replace: handle_get's reference.
    if (handle_replace_hooked(p, (hidx_t)fd, h.obj, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE,
                              g_peer.dsp, viv_socktab_connected_locked, &c) == 0) {
        g_peer.dsp      = NULL;           // the slot's now
        g_peer.detached = c.detached;
        g_peer.done     = c.ok;
    }
    handle_put(&h);
}

// Arm the peer for the next pin; a connecting peer opens its data fid now.
static bool ns_peer_arm(struct Proc *p, enum viv_net_proto proto, u32 n, int act) {
    g_peer.p    = p;
    g_peer.done = false;
    g_peer.dsp  = NULL;
    if (act == PEER_CONNECTS) {
        int err = 0;
        g_peer.dsp = ns_open(p, proto, n, "data", &err);
        if (!g_peer.dsp) return false;
    }
    g_peer.act = act;
    return true;
}

static void ns_peer_settle(void) {
    if (g_peer.dsp)      { spoor_clunk(g_peer.dsp); g_peer.dsp = NULL; }
    if (g_peer.detached) { spoor_clunk(g_peer.detached); g_peer.detached = NULL; }
    g_peer.act = PEER_IDLE;
}

// A peer thread can act between a ctl-verb arm's row read and its pin. If it
// CONNECTS the socket (the fd onto data and the row CONNECTED, one hold), the
// arm answers as the connected socket does -- the errno a call on the settled
// socket gets, derived here from the decides that make it -- and a CONNECTING
// socket's finish is simply over. If it CLOSES the number, the arm answers
// EBADF. No verb is written in any of these.
void test_vivsock_ctl_verbs_meet_a_racing_peer(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    viv_sock_pin_hook_for_test(ns_peer_at_pin);
    u32  w0 = g_ns.ctl_writes;

    s64  cfd   = ns_socket(p, VIV_NET_TCP, 40, VIV_SOCK_FRESH);
    bool ca    = cfd >= 0 && ns_peer_arm(p, VIV_NET_TCP, 40, PEER_CONNECTS);
    s64  crc   = ca ? viv_sock_connect_for_test(p, (u64)cfd, g_ns_peer, 80) : -1;
    bool cdone = g_peer.done;
    ns_peer_settle();
    s64  csettled = (cfd >= 0) ? viv_sock_connect_for_test(p, (u64)cfd, g_ns_peer, 80) : -1;

    s64  lfd   = ns_socket(p, VIV_NET_TCP, 41, VIV_SOCK_FRESH);
    bool la    = ns_bind_row(p, lfd, 8084) && ns_peer_arm(p, VIV_NET_TCP, 41, PEER_CONNECTS);
    s64  lrc   = la ? viv_listen_for_test(p, (u64)lfd) : -1;
    bool ldone = g_peer.done;
    ns_peer_settle();
    s32  lwant = 0;
    (void)vivarium_listen_decide(VIV_NET_TCP, VIV_SOCK_CONNECTED, 8084, &lwant);

    s64  dfd   = ns_socket(p, VIV_NET_UDP, 42, VIV_SOCK_FRESH);
    bool da    = dfd >= 0 && ns_peer_arm(p, VIV_NET_UDP, 42, PEER_CONNECTS);
    u32  o0    = g_ns.data_opens;
    s64  drc   = da ? viv_sock_dgram_send_for_test(p, (u64)dfd, g_ns_peer, 53) : -1;
    u32  dopens = g_ns.data_opens - o0;
    bool ddone = g_peer.done;
    ns_peer_settle();
    s32  dwant = 0;
    (void)vivarium_sendto_decide(VIV_NET_UDP, (u8)VIV_SOCK_CONNECTED, 0, 1, 16, &dwant);

    s64  ffd   = ns_socket(p, VIV_NET_TCP, 43, VIV_SOCK_FRESH);
    bool fa    = ns_connecting(p, ffd) && ns_peer_arm(p, VIV_NET_TCP, 43, PEER_CONNECTS);
    const struct Spoor *fdata = g_peer.dsp;
    u32  o1    = g_ns.data_opens;
    s64  frc   = fa ? viv_sock_connect_for_test(p, (u64)ffd, g_ns_peer, 80) : -1;
    u32  fopens = g_ns.data_opens - o1;
    bool fdone = g_peer.done;
    bool fnames = ns_slot_is(p, ffd, fdata);
    ns_peer_settle();

    s64  xc  = ns_socket(p, VIV_NET_TCP, 44, VIV_SOCK_FRESH);
    s64  xl  = ns_socket(p, VIV_NET_TCP, 45, VIV_SOCK_FRESH);
    s64  xd  = ns_socket(p, VIV_NET_UDP, 46, VIV_SOCK_FRESH);
    bool xlb = ns_bind_row(p, xl, 8085);
    s64  xcrc = (xc >= 0 && ns_peer_arm(p, VIV_NET_TCP, 44, PEER_CLOSES))
              ? viv_sock_connect_for_test(p, (u64)xc, g_ns_peer, 80) : -1;
    bool xcdone = g_peer.done;
    s64  xlrc = (xlb && ns_peer_arm(p, VIV_NET_TCP, 45, PEER_CLOSES))
              ? viv_listen_for_test(p, (u64)xl) : -1;
    bool xldone = g_peer.done;
    s64  xdrc = (xd >= 0 && ns_peer_arm(p, VIV_NET_UDP, 46, PEER_CLOSES))
              ? viv_sock_dgram_send_for_test(p, (u64)xd, g_ns_peer, 53) : -1;
    bool xddone = g_peer.done;
    u32  writes = g_ns.ctl_writes - w0;
    ns_peer_settle();
    viv_sock_pin_hook_for_test(NULL);
    g_peer.p = NULL;
    ns_free(p);

    TEST_ASSERT(ca && cdone, "fixture: a peer connected the first socket at the pin");
    TEST_EXPECT_EQ(crc, -(s64)T_E_ISCONN, "connect meets a socket a peer connected: EISCONN");
    TEST_EXPECT_EQ(crc, csettled, "the answer a connect on the settled socket gets");
    TEST_ASSERT(la && ldone, "fixture: a peer connected the bound socket at the pin");
    TEST_EXPECT_NE(lwant, 0, "the decide refuses listen on a CONNECTED socket");
    TEST_EXPECT_EQ(lrc, -(s64)lwant, "and listen answers with the decide's errno");
    TEST_ASSERT(da && ddone, "fixture: a peer connected the datagram socket at the pin");
    TEST_EXPECT_NE(dwant, 0, "the decide refuses a destination on a CONNECTED socket");
    TEST_EXPECT_EQ(drc, -(s64)dwant, "and the datagram sendto answers with its errno");
    TEST_EXPECT_EQ(dopens, 0u, "without opening a data file");
    TEST_ASSERT(fa && fdone, "fixture: a peer completed a CONNECTING socket's connect at the pin");
    TEST_EXPECT_EQ(frc, 0, "the finish is over: the connection is made");
    TEST_EXPECT_EQ(fopens, 0u, "with no wait of its own");
    TEST_ASSERT(fnames, "and the number names the peer's connection");
    TEST_ASSERT(xcdone && xldone && xddone, "fixture: a peer closed each number at the pin");
    TEST_EXPECT_EQ(xcrc, -(s64)T_E_BADF, "connect on a number closed at the pin: EBADF");
    TEST_EXPECT_EQ(xlrc, -(s64)T_E_BADF, "listen: EBADF");
    TEST_EXPECT_EQ(xdrc, -(s64)T_E_BADF, "the datagram sendto: EBADF");
    TEST_EXPECT_EQ(writes, 0u, "and no verb was written in any of them");
}

// dup and fork hand the new number the SAME ctl Spoor, so the row they make
// carries the ctl identity -- each with a reference of its own -- and the
// arms on the new number pass the check the source's do.
void test_vivsock_ctl_identity_travels_with_the_fd(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");

    s64  afd  = ns_socket(p, VIV_NET_TCP, 47, VIV_SOCK_FRESH);
    bool ab   = ns_bind_row(p, afd, 8086);
    const struct Spoor *aid = ns_row_ctl(p->socktab, afd);
    int  ar0  = ns_ref(aid);
    s64  dfd  = ab ? viv_dup_for_test(p, (u64)afd) : -1;
    const struct Spoor *did = ns_row_ctl(p->socktab, dfd);
    int  ar1  = ns_ref(aid);
    u32  m0   = g_ns.ctl_writes;
    s64  drc  = (dfd >= 0) ? viv_listen_for_test(p, (u64)dfd) : -1;
    u32  dw   = g_ns.ctl_writes - m0;
    bool dverb = ns_verb_is("announce *!8086");

    s64  ffd  = ns_socket(p, VIV_NET_TCP, 48, VIV_SOCK_FRESH);
    bool fb   = ns_bind_row(p, ffd, 8087);
    const struct Spoor *pid = ns_row_ctl(p->socktab, ffd);
    int  fr0  = ns_ref(pid);
    struct Proc *child = proc_alloc();
    bool forked = false;
    if (child) {
        child->phenotype = PHENO_LINUX;
        struct viv_socktab_fork sf = { .parent = p, .dst = NULL };
        if (viv_socktab_fork_prepare(&sf, p) == 0) {
            handle_table_copy_into_hooked(child, p, viv_socktab_fork_snapshot, &sf);
            viv_socktab_fork_finish(child, &sf);
            forked = __atomic_load_n(&child->socktab, __ATOMIC_ACQUIRE) != NULL;
        }
    }
    const struct Spoor *cid = forked ? ns_row_ctl(child->socktab, ffd) : NULL;
    int  fr1  = ns_ref(pid);
    u32  m1   = g_ns.ctl_writes;
    s64  crc  = (forked && fb) ? viv_listen_for_test(child, (u64)ffd) : -1;
    u32  cw   = g_ns.ctl_writes - m1;
    bool cverb = ns_verb_is("announce *!8087");
    if (child) { child->state = PROC_STATE_ZOMBIE; proc_free(child); }
    int  fr2  = ns_ref(pid);
    ns_free(p);

    TEST_ASSERT(ab && aid != NULL && dfd >= 0, "fixture: a bound socket and its dup");
    TEST_ASSERT(did == aid, "the dup's row names the source's ctl");
    TEST_EXPECT_EQ(ar1, ar0 + 2, "with a reference of its own beside the dup's fd");
    TEST_EXPECT_EQ(drc, 0, "listen through the dup passes the check");
    TEST_EXPECT_EQ(dw, 1u, "and writes its announce to that ctl");
    TEST_ASSERT(dverb, "for the bound port");
    TEST_ASSERT(fb && pid != NULL && forked, "fixture: a bound socket and a forked child");
    TEST_ASSERT(cid == pid, "the child's row names the parent's ctl, which its copied fd holds");
    TEST_EXPECT_EQ(fr1, fr0 + 2, "with a reference of the child's own beside its fd's");
    TEST_EXPECT_EQ(crc, 0, "the child's listen on the inherited socket passes the check");
    TEST_EXPECT_EQ(cw, 1u, "and announces");
    TEST_ASSERT(cverb, "on the bound port");
    TEST_EXPECT_EQ(fr2, fr0, "and the child's exit gives both references back");
}

// Two threads can finish one interrupted connect -- a read on the CONNECTING fd
// while connect() is retried, say. The first to swap the fd onto data wins; the
// other's swap is refused, and since the row it started from is CONNECTED now,
// the connection is made and the number names it: success, with its surplus
// data fid released -- not the EIO a refused swap otherwise means.
void test_vivsock_connect_finished_by_a_peer(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    s64  fd    = ns_socket(p, VIV_NET_TCP, 49, VIV_SOCK_FRESH);
    bool begun = ns_connecting(p, fd);
    int  derr  = 0;
    struct Spoor *peer = begun ? ns_open(p, VIV_NET_TCP, 49, "data", &derr) : NULL;
    if (peer) {
        g_ns.finish_p   = p;
        g_ns.finish_fd  = fd;
        g_ns.finish_dsp = peer;
    }
    u32  o0 = g_ns.data_opens, w0 = g_ns.ctl_writes, c0 = g_ns.data_clunks;
    s64  rc = peer ? viv_sock_connect_for_test(p, (u64)fd, g_ns_peer, 80) : -1;
    u32  opens = g_ns.data_opens - o0, writes = g_ns.ctl_writes - w0;
    u32  dclunks = g_ns.data_clunks - c0;
    bool swapped = peer && g_ns.finish_dsp == NULL && g_ns.finish_ok;
    bool names_peer = ns_slot_is(p, fd, peer);
    struct viv_sock e;
    bool row = ns_row(p, fd, &e);
    if (g_ns.finish_dsp)      { spoor_clunk(g_ns.finish_dsp); g_ns.finish_dsp = NULL; }
    if (g_ns.finish_detached) { spoor_clunk(g_ns.finish_detached); g_ns.finish_detached = NULL; }
    ns_free(p);

    TEST_ASSERT(begun && peer != NULL, "fixture: a CONNECTING socket and the peer's data fid");
    TEST_ASSERT(swapped, "the peer finished the connect first, inside the call's wait");
    TEST_EXPECT_EQ(rc, 0, "the call's refused swap is success: the connection is made");
    TEST_ASSERT(names_peer, "and the number names the peer's data fid");
    TEST_ASSERT(row && e.state == VIV_SOCK_CONNECTED, "the socket is CONNECTED");
    TEST_EXPECT_EQ(opens, 1u, "the call waited once");
    TEST_EXPECT_EQ(dclunks, 1u, "and released the data fid it no longer needs");
    TEST_EXPECT_EQ(writes, 0u, "dialing nothing: the dial was the interrupted connect's");
}

// A peer thread can re-make the socket's row while connect() waits for the
// handshake: dup the socket, then dup3 the dup back onto the number. The number
// still holds the socket's own ctl, so connect's swap goes through, and the
// re-made row -- a fresh epoch over the same ctl identity -- is the same socket:
// the commit must land on it. Left CONNECTING over a data fd, the number would
// answer ENOTSOCK to every later use. The control: the dup, whose slot still
// holds ctl, keeps its row and its ctl identity.
void test_vivsock_connect_commits_a_realiased_row(void) {
    struct Proc *p = ns_proc();
    TEST_ASSERT(p != NULL, "fixture: a /net stub over dev9p");
    s64  fd = ns_socket(p, VIV_NET_TCP, 50, VIV_SOCK_FRESH);
    const struct Spoor *id = ns_row_ctl(p->socktab, fd);
    struct viv_sock e0;
    bool r0 = ns_row(p, fd, &e0);
    if (r0) {
        g_ns.realias_p  = p;
        g_ns.realias_fd = fd;
    }
    s64  rc   = r0 ? viv_sock_connect_for_test(p, (u64)fd, g_ns_peer, 80) : -1;
    s64  dup  = g_ns.realias_got;
    bool made = g_ns.realias_done;
    struct viv_sock e, ed;
    bool row  = ns_row(p, fd, &e);
    bool drow = ns_row(p, dup, &ed);
    const struct Spoor *rid = ns_row_ctl(p->socktab, fd);
    const struct Spoor *did = ns_row_ctl(p->socktab, dup);
    u8   kind  = ns_fd_kind(p, fd);
    s64  again = row ? viv_sock_connect_for_test(p, (u64)fd, g_ns_peer, 80) : -1;
    ns_free(p);

    TEST_ASSERT(r0 && id != NULL, "fixture: a FRESH socket naming its ctl");
    TEST_ASSERT(made && dup >= 0, "fixture: a peer re-made the row while connect waited");
    TEST_ASSERT(row && e.epoch != e0.epoch, "the number's row is the re-made one");
    TEST_EXPECT_EQ(rc, 0, "connect succeeds");
    TEST_EXPECT_EQ(kind, (u8)NS_DATA, "and the number holds the connection");
    TEST_ASSERT(e.state == VIV_SOCK_CONNECTED && e.remote_port == 80,
                "the commit landed on the re-made row");
    TEST_ASSERT(rid == NULL, "which gave its ctl identity up");
    TEST_EXPECT_EQ(again, -(s64)T_E_ISCONN, "so the number answers as the connected socket");
    TEST_ASSERT(drow && did == id, "control: the dup keeps the ctl its slot still holds");
}
