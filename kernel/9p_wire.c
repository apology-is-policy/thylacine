// 9P2000.L wire codec — bring-up subset (P5-wire).
//
// Per `kernel/include/thylacine/9p_wire.h`. Pure byte-level codec; no
// kernel state, no syscalls invoked, no SLUB allocation. Suitable for
// both kernel and userspace consumers (the kernel test framework calls
// these directly; a future userspace 9P client could too).
//
// Wire-format crib (per diod protocol.md + stratum/v2/docs/reference/20-9p.md):
//
//   Header (7 bytes, every message):
//     [size: u32 LE][type: u8][tag: u16 LE]
//   `size` is the TOTAL message length INCLUDING this 7-byte header.
//
//   9P-string (variable-length):
//     [slen: u16 LE][bytes: u8 * slen]
//   NOT NUL-terminated.
//
//   QID (13 bytes):
//     [type: u8][version: u32 LE][path: u64 LE]
//
// Per-message body shapes are documented at each build_* / parse_*
// definition below.
//
// All integer values are little-endian on the wire. AArch64 (Thylacine's
// only target) is little-endian, so the byte-shift encoding compiles to
// a single store on most paths.

#include <thylacine/9p_wire.h>
#include <thylacine/types.h>

// =============================================================================
// Compile-time invariants — pin the wire-format constants against drift.
// =============================================================================

_Static_assert(P9_HDR_LEN == 7,                 "9P header must be 7 bytes");
_Static_assert(P9_QID_LEN == 13,                "9P qid must be 13 bytes");
_Static_assert(P9_NOFID == 0xFFFFFFFFu,         "NOFID must be all-ones u32");
_Static_assert(P9_NOTAG == 0xFFFFu,             "NOTAG must be all-ones u16");
_Static_assert(P9_TVERSION == 100,              "Tversion type drift");
_Static_assert(P9_RVERSION == P9_TVERSION + 1,  "R-message must immediately follow T-message");
_Static_assert(P9_TATTACH  == 104,              "Tattach type drift");
_Static_assert(P9_RATTACH  == P9_TATTACH + 1,   "Rattach type drift");
_Static_assert(P9_TWALK    == 110,              "Twalk type drift");
_Static_assert(P9_RWALK    == P9_TWALK + 1,     "Rwalk type drift");
_Static_assert(P9_TCLUNK   == 120,              "Tclunk type drift");
_Static_assert(P9_RCLUNK   == P9_TCLUNK + 1,    "Rclunk type drift");
_Static_assert(P9_RLERROR  == 7,                "Rlerror type drift");
// IO family (P5-wire-io):
_Static_assert(P9_TLOPEN   == 12,               "Tlopen type drift");
_Static_assert(P9_RLOPEN   == P9_TLOPEN + 1,    "Rlopen type drift");
_Static_assert(P9_TLCREATE == 14,               "Tlcreate type drift");
_Static_assert(P9_RLCREATE == P9_TLCREATE + 1,  "Rlcreate type drift");
_Static_assert(P9_TREAD    == 116,              "Tread type drift");
_Static_assert(P9_RREAD    == P9_TREAD + 1,     "Rread type drift");
_Static_assert(P9_TWRITE   == 118,              "Twrite type drift");
_Static_assert(P9_RWRITE   == P9_TWRITE + 1,    "Rwrite type drift");

// Sanity check on struct p9_qid: the in-memory shape is callee-defined
// (we always serialize field-by-field per p9_pack_qid), so its sizeof()
// is allowed to exceed P9_QID_LEN due to alignment padding. This assert
// merely prevents the fields from shrinking accidentally.
_Static_assert(sizeof(struct p9_qid) >= P9_QID_LEN, "p9_qid fields too small");

// =============================================================================
// Primitive packers.
// =============================================================================

int p9_pack_u8(u8 *out, size_t cap, u8 v) {
    if (cap < 1) return -1;
    out[0] = v;
    return 1;
}

int p9_pack_u16(u8 *out, size_t cap, u16 v) {
    if (cap < 2) return -1;
    out[0] = (u8)(v       & 0xff);
    out[1] = (u8)((v >> 8) & 0xff);
    return 2;
}

int p9_pack_u32(u8 *out, size_t cap, u32 v) {
    if (cap < 4) return -1;
    out[0] = (u8)(v        & 0xff);
    out[1] = (u8)((v >> 8)  & 0xff);
    out[2] = (u8)((v >> 16) & 0xff);
    out[3] = (u8)((v >> 24) & 0xff);
    return 4;
}

int p9_pack_u64(u8 *out, size_t cap, u64 v) {
    if (cap < 8) return -1;
    for (int i = 0; i < 8; i++) {
        out[i] = (u8)((v >> (i * 8)) & 0xff);
    }
    return 8;
}

int p9_pack_qid(u8 *out, size_t cap, const struct p9_qid *q) {
    if (cap < P9_QID_LEN) return -1;
    if (!q) return -1;
    out[0] = q->type;
    // version (u32 LE) at bytes 1..5
    for (int i = 0; i < 4; i++) {
        out[1 + i] = (u8)((q->version >> (i * 8)) & 0xff);
    }
    // path (u64 LE) at bytes 5..13
    for (int i = 0; i < 8; i++) {
        out[5 + i] = (u8)((q->path >> (i * 8)) & 0xff);
    }
    return P9_QID_LEN;
}

int p9_pack_str(u8 *out, size_t cap, const u8 *s, size_t slen) {
    // u16 prefix bounds string length.
    if (slen > 0xFFFFu) return -1;
    if (cap < 2 + slen) return -1;
    if (slen > 0 && !s) return -1;
    out[0] = (u8)(slen       & 0xff);
    out[1] = (u8)((slen >> 8) & 0xff);
    for (size_t i = 0; i < slen; i++) {
        out[2 + i] = s[i];
    }
    return (int)(2 + slen);
}

// =============================================================================
// Primitive unpackers.
// =============================================================================

int p9_unpack_u8(const u8 *in, size_t remaining, u8 *out) {
    if (remaining < 1) return -1;
    if (!out) return -1;
    *out = in[0];
    return 1;
}

int p9_unpack_u16(const u8 *in, size_t remaining, u16 *out) {
    if (remaining < 2) return -1;
    if (!out) return -1;
    *out = (u16)in[0] | ((u16)in[1] << 8);
    return 2;
}

int p9_unpack_u32(const u8 *in, size_t remaining, u32 *out) {
    if (remaining < 4) return -1;
    if (!out) return -1;
    *out = (u32)in[0]
         | ((u32)in[1] << 8)
         | ((u32)in[2] << 16)
         | ((u32)in[3] << 24);
    return 4;
}

int p9_unpack_u64(const u8 *in, size_t remaining, u64 *out) {
    if (remaining < 8) return -1;
    if (!out) return -1;
    u64 v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (u64)in[i] << (i * 8);
    }
    *out = v;
    return 8;
}

int p9_unpack_qid(const u8 *in, size_t remaining, struct p9_qid *q) {
    if (remaining < P9_QID_LEN) return -1;
    if (!q) return -1;
    q->type = in[0];
    q->version = (u32)in[1]
               | ((u32)in[2] << 8)
               | ((u32)in[3] << 16)
               | ((u32)in[4] << 24);
    u64 path = 0;
    for (int i = 0; i < 8; i++) {
        path |= (u64)in[5 + i] << (i * 8);
    }
    q->path = path;
    return P9_QID_LEN;
}

int p9_unpack_str(const u8 *in, size_t remaining,
                  const u8 **out_ptr, u16 *out_len) {
    if (remaining < 2) return -1;
    if (!out_ptr || !out_len) return -1;
    u16 slen = (u16)in[0] | ((u16)in[1] << 8);
    if ((size_t)2 + (size_t)slen > remaining) return -1;
    *out_len = slen;
    *out_ptr = (slen > 0) ? (in + 2) : NULL;
    return (int)(2 + slen);
}

// =============================================================================
// Header peek.
// =============================================================================

int p9_peek_header(const u8 *in, size_t len,
                   u32 *size, u8 *type, u16 *tag) {
    if (len < P9_HDR_LEN) return -1;
    if (!size || !type || !tag) return -1;
    *size = (u32)in[0]
          | ((u32)in[1] << 8)
          | ((u32)in[2] << 16)
          | ((u32)in[3] << 24);
    *type = in[4];
    *tag  = (u16)in[5] | ((u16)in[6] << 8);
    return 0;
}

// =============================================================================
// Internal: write the common header at out[0..6]. Caller back-patches
// `size` once the body length is known (we pre-write `size = body_len + 7`
// here as a convenience since the caller already knows it).
// =============================================================================

static int write_header(u8 *out, size_t cap, u32 size, u8 type, u16 tag) {
    if (cap < P9_HDR_LEN) return -1;
    out[0] = (u8)(size       & 0xff);
    out[1] = (u8)((size >> 8) & 0xff);
    out[2] = (u8)((size >> 16) & 0xff);
    out[3] = (u8)((size >> 24) & 0xff);
    out[4] = type;
    out[5] = (u8)(tag       & 0xff);
    out[6] = (u8)((tag >> 8) & 0xff);
    return P9_HDR_LEN;
}

// =============================================================================
// Internal: validate header for an R-message of `expected_type`, returning
// the in-buffer body offset (P9_HDR_LEN) on success.
//
// Rejects:
//   - len < P9_HDR_LEN (truncated header)
//   - header.size != len (frame mismatch — Thylacine's caller is expected
//     to deliver one complete framed message per parse call)
//   - header.size < P9_HDR_LEN (impossible header)
//   - header.type != expected_type
// =============================================================================

static int validate_rmsg_header(const u8 *in, size_t len,
                                 u8 expected_type, u16 *tag_out) {
    u32 size;
    u8  type;
    u16 tag;
    int rc = p9_peek_header(in, len, &size, &type, &tag);
    if (rc < 0) return -1;
    if (size != (u32)len) return -1;
    if (size < P9_HDR_LEN) return -1;
    if (type != expected_type) return -1;
    if (tag_out) *tag_out = tag;
    return (int)P9_HDR_LEN;
}

// =============================================================================
// Tversion / Rversion (handshake).
//
// Tversion body: [msize: u32][version: 9P-string]
// Rversion body: [msize: u32][version: 9P-string]
// =============================================================================

int p9_build_tversion(u8 *out, size_t cap,
                      u16 tag, u32 msize,
                      const u8 *version, size_t version_len) {
    if (version_len > 0 && !version) return -1;
    if (version_len > 0xFFFFu) return -1;
    // Body: msize (4) + version-string (2 + version_len) = 6 + version_len
    size_t body_len = 4 + 2 + version_len;
    size_t total = P9_HDR_LEN + body_len;
    if (total > 0xFFFFFFFFull) return -1;
    if (cap < total) return -1;
    int rc = write_header(out, cap, (u32)total, P9_TVERSION, tag);
    if (rc < 0) return -1;
    size_t off = P9_HDR_LEN;
    rc = p9_pack_u32(out + off, cap - off, msize);
    if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_str(out + off, cap - off, version, version_len);
    if (rc < 0) return -1; off += (size_t)rc;
    return (int)off;
}

int p9_parse_rversion(const u8 *in, size_t len,
                      u16 *tag, u32 *msize,
                      const u8 **version, u16 *version_len) {
    if (!tag || !msize || !version || !version_len) return -1;
    int hdr = validate_rmsg_header(in, len, P9_RVERSION, tag);
    if (hdr < 0) return -1;
    size_t off = (size_t)hdr;
    size_t rem = len - off;
    int rc = p9_unpack_u32(in + off, rem, msize);
    if (rc < 0) return -1; off += (size_t)rc; rem -= (size_t)rc;
    rc = p9_unpack_str(in + off, rem, version, version_len);
    if (rc < 0) return -1; off += (size_t)rc;
    if (off != len) return -1;       // strict body-length equality (R111 P3 F-10)
    return 0;
}

// =============================================================================
// Tattach / Rattach.
//
// Tattach body: [fid: u32][afid: u32][uname: str][aname: str][n_uname: u32]
// Rattach body: [qid: 13]
// =============================================================================

int p9_build_tattach(u8 *out, size_t cap, u16 tag,
                     u32 fid, u32 afid,
                     const u8 *uname, size_t uname_len,
                     const u8 *aname, size_t aname_len,
                     u32 n_uname) {
    if (uname_len > 0 && !uname) return -1;
    if (aname_len > 0 && !aname) return -1;
    if (uname_len > 0xFFFFu) return -1;
    if (aname_len > 0xFFFFu) return -1;
    // Body: fid(4) + afid(4) + uname(2 + un) + aname(2 + an) + n_uname(4)
    size_t body_len = 4 + 4 + 2 + uname_len + 2 + aname_len + 4;
    size_t total = P9_HDR_LEN + body_len;
    if (total > 0xFFFFFFFFull) return -1;
    if (cap < total) return -1;
    int rc = write_header(out, cap, (u32)total, P9_TATTACH, tag);
    if (rc < 0) return -1;
    size_t off = P9_HDR_LEN;
    rc = p9_pack_u32(out + off, cap - off, fid);     if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_u32(out + off, cap - off, afid);    if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_str(out + off, cap - off, uname, uname_len);
                                                     if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_str(out + off, cap - off, aname, aname_len);
                                                     if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_u32(out + off, cap - off, n_uname); if (rc < 0) return -1; off += (size_t)rc;
    return (int)off;
}

int p9_parse_rattach(const u8 *in, size_t len,
                     u16 *tag, struct p9_qid *qid) {
    if (!tag || !qid) return -1;
    int hdr = validate_rmsg_header(in, len, P9_RATTACH, tag);
    if (hdr < 0) return -1;
    size_t off = (size_t)hdr;
    size_t rem = len - off;
    int rc = p9_unpack_qid(in + off, rem, qid);
    if (rc < 0) return -1; off += (size_t)rc;
    if (off != len) return -1;
    return 0;
}

// =============================================================================
// Twalk / Rwalk.
//
// Twalk body: [fid: u32][newfid: u32][nwname: u16][wname[nwname]: str * nwname]
// Rwalk body: [nwqid: u16][wqid[nwqid]: qid * nwqid]
// =============================================================================

int p9_build_twalk(u8 *out, size_t cap, u16 tag,
                   u32 fid, u32 newfid,
                   u16 nwname,
                   const u8 *const *names, const size_t *name_lens) {
    if (nwname > P9_MAX_WALK) return -1;
    if (nwname > 0 && (!names || !name_lens)) return -1;
    // Compute body length: 4 + 4 + 2 + sum(2 + name_lens[i])
    size_t body_len = 4 + 4 + 2;
    for (u16 i = 0; i < nwname; i++) {
        if (name_lens[i] > 0xFFFFu) return -1;
        if (name_lens[i] > 0 && !names[i]) return -1;
        body_len += 2 + name_lens[i];
    }
    size_t total = P9_HDR_LEN + body_len;
    if (total > 0xFFFFFFFFull) return -1;
    if (cap < total) return -1;
    int rc = write_header(out, cap, (u32)total, P9_TWALK, tag);
    if (rc < 0) return -1;
    size_t off = P9_HDR_LEN;
    rc = p9_pack_u32(out + off, cap - off, fid);    if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_u32(out + off, cap - off, newfid); if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_u16(out + off, cap - off, nwname); if (rc < 0) return -1; off += (size_t)rc;
    for (u16 i = 0; i < nwname; i++) {
        rc = p9_pack_str(out + off, cap - off, names[i], name_lens[i]);
        if (rc < 0) return -1;
        off += (size_t)rc;
    }
    return (int)off;
}

int p9_parse_rwalk(const u8 *in, size_t len,
                   u16 *tag, u16 *nwqid_out,
                   struct p9_qid *qids, size_t qid_cap) {
    if (!tag || !nwqid_out) return -1;
    int hdr = validate_rmsg_header(in, len, P9_RWALK, tag);
    if (hdr < 0) return -1;
    size_t off = (size_t)hdr;
    size_t rem = len - off;
    u16 nwqid = 0;
    int rc = p9_unpack_u16(in + off, rem, &nwqid);
    if (rc < 0) return -1; off += (size_t)rc; rem -= (size_t)rc;
    // R111 doctrine (Stratum's lib audit): bound server-supplied count
    // against caller-supplied cap BEFORE writing into the caller buffer.
    if (nwqid > qid_cap) return -1;
    if (nwqid > P9_MAX_WALK) return -1;
    if (qids == NULL && nwqid > 0) return -1;
    for (u16 i = 0; i < nwqid; i++) {
        rc = p9_unpack_qid(in + off, rem, &qids[i]);
        if (rc < 0) return -1;
        off += (size_t)rc; rem -= (size_t)rc;
    }
    if (off != len) return -1;
    *nwqid_out = nwqid;
    return 0;
}

// =============================================================================
// Tclunk / Rclunk.
//
// Tclunk body: [fid: u32]
// Rclunk body: (empty)
// =============================================================================

int p9_build_tclunk(u8 *out, size_t cap, u16 tag, u32 fid) {
    size_t total = P9_HDR_LEN + 4;
    if (cap < total) return -1;
    int rc = write_header(out, cap, (u32)total, P9_TCLUNK, tag);
    if (rc < 0) return -1;
    rc = p9_pack_u32(out + P9_HDR_LEN, cap - P9_HDR_LEN, fid);
    if (rc < 0) return -1;
    return (int)total;
}

int p9_parse_rclunk(const u8 *in, size_t len, u16 *tag) {
    if (!tag) return -1;
    int hdr = validate_rmsg_header(in, len, P9_RCLUNK, tag);
    if (hdr < 0) return -1;
    // Rclunk has no body.
    if ((size_t)hdr != len) return -1;
    return 0;
}

// =============================================================================
// Rlerror — server-side error response, replaces any expected Rxx.
//
// Rlerror body: [ecode: u32]   (Linux errno value)
// =============================================================================

int p9_parse_rlerror(const u8 *in, size_t len, u16 *tag, u32 *ecode) {
    if (!tag || !ecode) return -1;
    int hdr = validate_rmsg_header(in, len, P9_RLERROR, tag);
    if (hdr < 0) return -1;
    size_t off = (size_t)hdr;
    size_t rem = len - off;
    int rc = p9_unpack_u32(in + off, rem, ecode);
    if (rc < 0) return -1; off += (size_t)rc;
    if (off != len) return -1;
    return 0;
}

// =============================================================================
// IO family — Tlopen / Rlopen, Tlcreate / Rlcreate, Tread / Rread,
// Twrite / Rwrite. Wire shapes documented at the declarations in
// `kernel/include/thylacine/9p_wire.h`.
//
// Rlopen and Rlcreate share an identical body shape (qid + iounit); a
// single shared parser would conflate the two semantic operations, so
// each has its own thin wrapper that validates the expected R-type.
// =============================================================================

int p9_build_tlopen(u8 *out, size_t cap, u16 tag, u32 fid, u32 flags) {
    size_t total = P9_HDR_LEN + 4 + 4;     // fid(4) + flags(4)
    if (cap < total) return -1;
    int rc = write_header(out, cap, (u32)total, P9_TLOPEN, tag);
    if (rc < 0) return -1;
    size_t off = P9_HDR_LEN;
    rc = p9_pack_u32(out + off, cap - off, fid);    if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_u32(out + off, cap - off, flags);  if (rc < 0) return -1; off += (size_t)rc;
    return (int)off;
}

// Shared body shape for Rlopen + Rlcreate: qid (13) + iounit (4).
static int parse_open_create_body(const u8 *in, size_t len, u8 expected_type,
                                   u16 *tag, struct p9_qid *qid, u32 *iounit) {
    if (!tag || !qid || !iounit) return -1;
    int hdr = validate_rmsg_header(in, len, expected_type, tag);
    if (hdr < 0) return -1;
    size_t off = (size_t)hdr;
    size_t rem = len - off;
    int rc = p9_unpack_qid(in + off, rem, qid);
    if (rc < 0) return -1; off += (size_t)rc; rem -= (size_t)rc;
    rc = p9_unpack_u32(in + off, rem, iounit);
    if (rc < 0) return -1; off += (size_t)rc;
    if (off != len) return -1;
    return 0;
}

int p9_parse_rlopen(const u8 *in, size_t len,
                    u16 *tag, struct p9_qid *qid, u32 *iounit) {
    return parse_open_create_body(in, len, P9_RLOPEN, tag, qid, iounit);
}

int p9_build_tlcreate(u8 *out, size_t cap, u16 tag, u32 fid,
                      const u8 *name, size_t name_len,
                      u32 flags, u32 mode, u32 gid) {
    if (name_len > 0 && !name) return -1;
    if (name_len > P9_NAME_MAX) return -1;
    // Body: fid(4) + name(2 + name_len) + flags(4) + mode(4) + gid(4)
    size_t body_len = 4 + 2 + name_len + 4 + 4 + 4;
    size_t total = P9_HDR_LEN + body_len;
    if (total > 0xFFFFFFFFull) return -1;
    if (cap < total) return -1;
    int rc = write_header(out, cap, (u32)total, P9_TLCREATE, tag);
    if (rc < 0) return -1;
    size_t off = P9_HDR_LEN;
    rc = p9_pack_u32(out + off, cap - off, fid);    if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_str(out + off, cap - off, name, name_len);
                                                    if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_u32(out + off, cap - off, flags);  if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_u32(out + off, cap - off, mode);   if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_u32(out + off, cap - off, gid);    if (rc < 0) return -1; off += (size_t)rc;
    return (int)off;
}

int p9_parse_rlcreate(const u8 *in, size_t len,
                      u16 *tag, struct p9_qid *qid, u32 *iounit) {
    return parse_open_create_body(in, len, P9_RLCREATE, tag, qid, iounit);
}

int p9_build_tread(u8 *out, size_t cap, u16 tag,
                   u32 fid, u64 offset, u32 count) {
    size_t total = P9_HDR_LEN + 4 + 8 + 4;     // fid + offset + count
    if (cap < total) return -1;
    int rc = write_header(out, cap, (u32)total, P9_TREAD, tag);
    if (rc < 0) return -1;
    size_t off = P9_HDR_LEN;
    rc = p9_pack_u32(out + off, cap - off, fid);    if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_u64(out + off, cap - off, offset); if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_u32(out + off, cap - off, count);  if (rc < 0) return -1; off += (size_t)rc;
    return (int)off;
}

int p9_parse_rread(const u8 *in, size_t len,
                   u16 *tag, u32 *count,
                   const u8 **data_ptr, u32 data_cap) {
    if (!tag || !count || !data_ptr) return -1;
    int hdr = validate_rmsg_header(in, len, P9_RREAD, tag);
    if (hdr < 0) return -1;
    size_t off = (size_t)hdr;
    size_t rem = len - off;
    u32 c = 0;
    int rc = p9_unpack_u32(in + off, rem, &c);
    if (rc < 0) return -1; off += (size_t)rc; rem -= (size_t)rc;
    // R111 doctrine: bound server-supplied count against caller cap BEFORE
    // exposing data pointer. The strict-equality check below also catches
    // gross size mismatches, but bounding first defends the caller against
    // buffers smaller than what the server claims to be returning.
    if (c > data_cap) return -1;
    if ((size_t)c != rem) return -1;
    *count    = c;
    *data_ptr = (c > 0) ? (in + off) : NULL;
    return 0;
}

int p9_build_twrite(u8 *out, size_t cap, u16 tag,
                    u32 fid, u64 offset, u32 count,
                    const u8 *data) {
    if (count > 0 && !data) return -1;
    // Body: fid(4) + offset(8) + count(4) + data(count)
    size_t body_len = 4 + 8 + 4 + (size_t)count;
    size_t total = P9_HDR_LEN + body_len;
    if (total > 0xFFFFFFFFull) return -1;
    if (cap < total) return -1;
    int rc = write_header(out, cap, (u32)total, P9_TWRITE, tag);
    if (rc < 0) return -1;
    size_t off = P9_HDR_LEN;
    rc = p9_pack_u32(out + off, cap - off, fid);    if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_u64(out + off, cap - off, offset); if (rc < 0) return -1; off += (size_t)rc;
    rc = p9_pack_u32(out + off, cap - off, count);  if (rc < 0) return -1; off += (size_t)rc;
    for (u32 i = 0; i < count; i++) out[off + i] = data[i];
    off += (size_t)count;
    return (int)off;
}

int p9_parse_rwrite(const u8 *in, size_t len, u16 *tag, u32 *count) {
    if (!tag || !count) return -1;
    int hdr = validate_rmsg_header(in, len, P9_RWRITE, tag);
    if (hdr < 0) return -1;
    size_t off = (size_t)hdr;
    size_t rem = len - off;
    int rc = p9_unpack_u32(in + off, rem, count);
    if (rc < 0) return -1; off += (size_t)rc;
    if (off != len) return -1;
    return 0;
}
