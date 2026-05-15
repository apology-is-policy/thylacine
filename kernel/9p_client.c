// 9P client high-level API — P5-client.
//
// Per `kernel/include/thylacine/9p_client.h`. Each public op composes:
//   session.send_*  →  transport.exchange  →  result extraction  →
//   error mapping.

#include <thylacine/9p_client.h>
#include <thylacine/9p_session.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_wire.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

_Static_assert(P9_CLIENT_MAGIC == 0x50394354u, "client magic drift");
_Static_assert(P9_CLIENT_OUT_BUF_MAX >= 256u,  "client out buf too small");

// R15-c F230: per-client lock acquired around every public op for the
// full build + transport-exchange + dispatch + bookkeeping window.
// Helpers keep the lock-release boilerplate out of every early-return
// path.
#define CLIENT_UNLOCK_RET(c, rc) do { spin_unlock(&(c)->lock); return (rc); } while (0)

// =============================================================================
// Internal: explicit struct copies. The kernel doesn't link libc, so
// struct assignments that the compiler would otherwise lower to memcpy
// fail at link time. Field-by-field copies sidestep the issue.
// =============================================================================

static void copy_qid(struct p9_qid *dst, const struct p9_qid *src) {
    dst->type    = src->type;
    dst->version = src->version;
    dst->path    = src->path;
}

static void copy_attr(struct p9_attr *dst, const struct p9_attr *src) {
    dst->valid       = src->valid;
    copy_qid(&dst->qid, &src->qid);
    dst->mode        = src->mode;
    dst->uid         = src->uid;
    dst->gid         = src->gid;
    dst->nlink       = src->nlink;
    dst->rdev        = src->rdev;
    dst->size        = src->size;
    dst->blksize     = src->blksize;
    dst->blocks      = src->blocks;
    dst->atime_sec   = src->atime_sec;
    dst->atime_nsec  = src->atime_nsec;
    dst->mtime_sec   = src->mtime_sec;
    dst->mtime_nsec  = src->mtime_nsec;
    dst->ctime_sec   = src->ctime_sec;
    dst->ctime_nsec  = src->ctime_nsec;
    dst->btime_sec   = src->btime_sec;
    dst->btime_nsec  = src->btime_nsec;
    dst->gen         = src->gen;
    dst->data_version = src->data_version;
}

static void copy_statfs(struct p9_statfs *dst, const struct p9_statfs *src) {
    dst->type    = src->type;
    dst->bsize   = src->bsize;
    dst->blocks  = src->blocks;
    dst->bfree   = src->bfree;
    dst->bavail  = src->bavail;
    dst->files   = src->files;
    dst->ffree   = src->ffree;
    dst->fsid    = src->fsid;
    dst->namelen = src->namelen;
}

// =============================================================================
// Internal: error mapping.
//
// `send_rc` and `recv_rc` carry layer-specific failure codes. The
// client surface maps them onto the errno convention documented in
// 9p_client.h.
// =============================================================================

static int map_error(int session_send_rc, int exchange_rc,
                      const struct p9_dispatch_result *r) {
    if (session_send_rc < 0) return -P9_E_IO;
    if (exchange_rc < 0) return -P9_E_IO;
    if (r->is_error) {
        // Rlerror surfaced an errno; map directly. The server-side
        // ecode is a Linux errno (positive); we negate to match the
        // client's signed-errno convention.
        return -(int)r->ecode;
    }
    return 0;
}

// =============================================================================
// Lifecycle.
// =============================================================================

int p9_client_init(struct p9_client *c,
                    u32 root_fid, u32 msize,
                    struct p9_transport_ops transport_ops,
                    u8 *recv_buf, size_t recv_cap) {
    if (!c) return -P9_E_INVAL;
    if (!recv_buf) return -P9_E_INVAL;
    if (recv_cap < P9_HDR_LEN) return -P9_E_INVAL;
    int rc = p9_session_init(&c->session, root_fid, msize);
    if (rc < 0) return -P9_E_INVAL;
    rc = p9_transport_init(&c->transport, transport_ops, recv_buf, recv_cap);
    if (rc < 0) {
        p9_session_destroy(&c->session);
        return -P9_E_INVAL;
    }
    c->magic        = P9_CLIENT_MAGIC;
    spin_lock_init(&c->lock);
    // Fid allocator starts at root_fid + 1; dev9p (and other callers)
    // pull fresh fids monotonically via p9_client_alloc_fid.
    c->next_fid     = (root_fid < P9_NOFID - 1) ? (root_fid + 1) : 1;
    c->total_ops    = 0;
    c->total_errors = 0;
    return 0;
}

void p9_client_destroy(struct p9_client *c) {
    if (!c) return;
    if (c->magic != P9_CLIENT_MAGIC) return;
    c->magic = 0;
    p9_transport_destroy(&c->transport);
    p9_session_destroy(&c->session);
}

int p9_client_close(struct p9_client *c) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    int rc = p9_transport_close(&c->transport);
    int rc2 = p9_session_close(&c->session);
    spin_unlock(&c->lock);
    if (rc < 0) return -P9_E_IO;
    if (rc2 < 0) return -P9_E_IO;
    return 0;
}

// =============================================================================
// Handshake.
// =============================================================================

int p9_client_handshake(struct p9_client *c,
                         const u8 *uname, size_t uname_len,
                         const u8 *aname, size_t aname_len,
                         u32 n_uname) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);

    // Phase 1: Tversion → Rversion (drives INIT → VERSIONED).
    int len = p9_session_send_version(&c->session, c->out_buf,
                                       sizeof(c->out_buf), NULL, 0);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }

    // Phase 2: Tattach → Rattach (drives VERSIONED → OPEN).
    len = p9_session_send_attach(&c->session, c->out_buf,
                                  sizeof(c->out_buf),
                                  uname, uname_len, aname, aname_len,
                                  n_uname);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    rc = p9_transport_exchange(&c->transport, &c->session,
                                c->out_buf, (size_t)len, &r);
    e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

// =============================================================================
// Path operations.
// =============================================================================

int p9_client_walk(struct p9_client *c,
                    u32 src_fid, u32 new_fid,
                    u16 nwname,
                    const u8 *const *names, const size_t *name_lens,
                    u16 *out_nwqid, struct p9_qid *out_qids) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_walk(&c->session, c->out_buf,
                                    sizeof(c->out_buf),
                                    src_fid, new_fid,
                                    nwname, names, name_lens);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_nwqid) *out_nwqid = r.nwqid;
    if (out_qids) {
        for (u16 i = 0; i < r.nwqid && i < P9_MAX_WALK; i++) {
            copy_qid(&out_qids[i], &r.qids[i]);
        }
    }
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_walk_one(struct p9_client *c,
                        u32 src_fid, u32 new_fid,
                        const u8 *name, size_t name_len,
                        struct p9_qid *out_qid) {
    const u8 *names[1] = { name };
    const size_t name_lens[1] = { name_len };
    u16 nwqid;
    struct p9_qid qids[P9_MAX_WALK];
    int e = p9_client_walk(c, src_fid, new_fid, 1, names, name_lens, &nwqid, qids);
    if (e != 0) return e;
    if (nwqid != 1) return -P9_E_IO;       // partial walk; we asked for 1
    if (out_qid) copy_qid(out_qid, &qids[0]);
    return 0;
}

int p9_client_clunk(struct p9_client *c, u32 fid) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_clunk(&c->session, c->out_buf,
                                     sizeof(c->out_buf), fid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

// =============================================================================
// IO operations.
// =============================================================================

int p9_client_lopen(struct p9_client *c, u32 fid, u32 flags,
                     struct p9_qid *out_qid, u32 *out_iounit) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_lopen(&c->session, c->out_buf,
                                     sizeof(c->out_buf), fid, flags);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_qid) copy_qid(out_qid, &r.open_qid);
    if (out_iounit) *out_iounit = r.open_iounit;
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_lcreate(struct p9_client *c, u32 fid,
                       const u8 *name, size_t name_len,
                       u32 flags, u32 mode, u32 gid,
                       struct p9_qid *out_qid, u32 *out_iounit) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_lcreate(&c->session, c->out_buf,
                                       sizeof(c->out_buf), fid,
                                       name, name_len, flags, mode, gid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_qid) copy_qid(out_qid, &r.open_qid);
    if (out_iounit) *out_iounit = r.open_iounit;
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_read(struct p9_client *c, u32 fid, u64 offset,
                    u32 count, u8 *out_data, u32 *out_count) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    if (count > 0 && !out_data) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_read(&c->session, c->out_buf,
                                    sizeof(c->out_buf),
                                    fid, offset, count);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    // Copy out the data (Rread's data_ptr aliases the transport's
    // recv_buf; we copy so the caller doesn't have to track lifetime).
    if (r.read_count > count) CLIENT_UNLOCK_RET(c, -P9_E_IO);     // defense
    for (u32 i = 0; i < r.read_count; i++) {
        out_data[i] = r.read_data[i];
    }
    if (out_count) *out_count = r.read_count;
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_write(struct p9_client *c, u32 fid, u64 offset,
                     u32 count, const u8 *data, u32 *out_accepted) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    if (count > 0 && !data) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_write(&c->session, c->out_buf,
                                     sizeof(c->out_buf),
                                     fid, offset, count, data);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_accepted) *out_accepted = r.write_count;
    spin_unlock(&c->lock);
    return 0;
}

// =============================================================================
// Metadata operations.
// =============================================================================

int p9_client_getattr(struct p9_client *c, u32 fid,
                       u64 request_mask, struct p9_attr *out_attr) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_getattr(&c->session, c->out_buf,
                                       sizeof(c->out_buf),
                                       fid, request_mask);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_attr) copy_attr(out_attr, &r.attr);
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_setattr(struct p9_client *c, u32 fid,
                       const struct p9_setattr *attr) {
    if (!c || !attr) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_setattr(&c->session, c->out_buf,
                                       sizeof(c->out_buf), fid, attr);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_readdir(struct p9_client *c, u32 fid, u64 offset,
                       u32 count, u8 *out_data, u32 *out_count) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    if (count > 0 && !out_data) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_readdir(&c->session, c->out_buf,
                                       sizeof(c->out_buf),
                                       fid, offset, count);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (r.readdir_count > count) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    for (u32 i = 0; i < r.readdir_count; i++) {
        out_data[i] = r.readdir_data[i];
    }
    if (out_count) *out_count = r.readdir_count;
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_statfs(struct p9_client *c, u32 fid,
                      struct p9_statfs *out_statfs) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_statfs(&c->session, c->out_buf,
                                      sizeof(c->out_buf), fid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_statfs) copy_statfs(out_statfs, &r.statfs);
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_fsync(struct p9_client *c, u32 fid, u32 datasync) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_fsync(&c->session, c->out_buf,
                                     sizeof(c->out_buf), fid, datasync);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

// =============================================================================
// Mutation operations.
// =============================================================================

int p9_client_symlink(struct p9_client *c, u32 fid,
                       const u8 *name, size_t name_len,
                       const u8 *symtgt, size_t symtgt_len,
                       u32 gid, struct p9_qid *out_qid) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_symlink(&c->session, c->out_buf,
                                       sizeof(c->out_buf),
                                       fid, name, name_len,
                                       symtgt, symtgt_len, gid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_qid) copy_qid(out_qid, &r.created_qid);
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_mknod(struct p9_client *c, u32 dfid,
                     const u8 *name, size_t name_len,
                     u32 mode, u32 major, u32 minor, u32 gid,
                     struct p9_qid *out_qid) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_mknod(&c->session, c->out_buf,
                                     sizeof(c->out_buf),
                                     dfid, name, name_len,
                                     mode, major, minor, gid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_qid) copy_qid(out_qid, &r.created_qid);
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_rename(struct p9_client *c, u32 fid, u32 dfid,
                      const u8 *name, size_t name_len) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_rename(&c->session, c->out_buf,
                                      sizeof(c->out_buf),
                                      fid, dfid, name, name_len);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_readlink(struct p9_client *c, u32 fid,
                        u8 *out_target, u16 *out_target_len) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    if (!out_target || !out_target_len) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_readlink(&c->session, c->out_buf,
                                        sizeof(c->out_buf), fid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    // Caller's `out_target` is assumed to be sized for P9_NAME_MAX
    // (255 bytes) by convention; we cap the copy at *out_target_len
    // on entry to admit smaller caller buffers.
    u16 cap = *out_target_len;
    u16 to_copy = (r.readlink_target_len <= cap) ? r.readlink_target_len : cap;
    for (u16 i = 0; i < to_copy; i++) out_target[i] = r.readlink_target[i];
    *out_target_len = r.readlink_target_len;
    if (r.readlink_target_len > cap) CLIENT_UNLOCK_RET(c, -P9_E_INVAL);
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_link(struct p9_client *c, u32 dfid, u32 fid,
                    const u8 *name, size_t name_len) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_link(&c->session, c->out_buf,
                                    sizeof(c->out_buf),
                                    dfid, fid, name, name_len);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_mkdir(struct p9_client *c, u32 dfid,
                     const u8 *name, size_t name_len,
                     u32 mode, u32 gid, struct p9_qid *out_qid) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_mkdir(&c->session, c->out_buf,
                                     sizeof(c->out_buf),
                                     dfid, name, name_len, mode, gid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_qid) copy_qid(out_qid, &r.created_qid);
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_renameat(struct p9_client *c, u32 olddirfid,
                        const u8 *oldname, size_t oldname_len,
                        u32 newdirfid,
                        const u8 *newname, size_t newname_len) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_renameat(&c->session, c->out_buf,
                                        sizeof(c->out_buf),
                                        olddirfid, oldname, oldname_len,
                                        newdirfid, newname, newname_len);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_unlinkat(struct p9_client *c, u32 dfid,
                        const u8 *name, size_t name_len, u32 flags) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_unlinkat(&c->session, c->out_buf,
                                        sizeof(c->out_buf),
                                        dfid, name, name_len, flags);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int rc = p9_transport_exchange(&c->transport, &c->session,
                                    c->out_buf, (size_t)len, &r);
    int e = map_error(len, rc, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

// =============================================================================
// Query helpers.
// =============================================================================

u32 p9_client_alloc_fid(struct p9_client *c) {
    if (!c) return P9_NOFID;
    if (c->magic != P9_CLIENT_MAGIC) return P9_NOFID;
    spin_lock(&c->lock);
    if (c->next_fid == P9_NOFID) {
        spin_unlock(&c->lock);
        return P9_NOFID;       // exhausted
    }
    u32 fid = c->next_fid;
    c->next_fid++;
    spin_unlock(&c->lock);
    return fid;
}

bool p9_client_is_open(const struct p9_client *c) {
    if (!c) return false;
    if (c->magic != P9_CLIENT_MAGIC) return false;
    // const-cast for the lock: spin_lock requires non-const; the read
    // through the lock is logically const at the caller's level.
    spin_lock(&((struct p9_client *)c)->lock);
    bool open = p9_session_is_open(&c->session);
    spin_unlock(&((struct p9_client *)c)->lock);
    return open;
}

size_t p9_client_inflight(const struct p9_client *c) {
    if (!c) return 0;
    if (c->magic != P9_CLIENT_MAGIC) return 0;
    spin_lock(&((struct p9_client *)c)->lock);
    size_t n = p9_session_inflight(&c->session);
    spin_unlock(&((struct p9_client *)c)->lock);
    return n;
}
