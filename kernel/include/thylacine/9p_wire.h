// 9P2000.L wire codec (P5-wire bring-up subset).
//
// Per ARCH §10.2 (9P dialect) + `specs/9p_client.tla` (the spec this codec
// will eventually service) + `stratum/v2/docs/reference/20-9p.md` (the
// canonical server's wire-format reference). 9P2000.L is the Linux-extended
// 9P dialect that v9fs speaks; the diod project's protocol description
// (https://github.com/chaos/diod/blob/master/protocol.md) defines the
// baseline. Stratum's `<stratum/9p.h>` is the cross-project ABI reference
// for opcodes and limits; this header mirrors those numerically.
//
// SCOPE OF P5-WIRE + P5-WIRE-IO + P5-WIRE-META + P5-WIRE-MUTATION (cumulative
// through this chunk):
//
//   - Common header (size + type + tag); pack/peek/unpack.
//   - Primitive marshalers/unmarshalers: u8, u16, u32, u64, str, qid.
//   - Handshake + navigation + clunk message family:
//       Tversion / Rversion (100 / 101)
//       Tattach  / Rattach  (104 / 105)
//       Twalk    / Rwalk    (110 / 111)
//       Tclunk   / Rclunk   (120 / 121)
//   - IO family (P5-wire-io extension):
//       Tlopen   / Rlopen   (12 / 13)
//       Tlcreate / Rlcreate (14 / 15)
//       Tread    / Rread    (116 / 117)
//       Twrite   / Rwrite   (118 / 119)
//   - Metadata family (P5-wire-meta extension):
//       Tgetattr / Rgetattr (24 / 25)
//       Tsetattr / Rsetattr (26 / 27)
//       Treaddir / Rreaddir (40 / 41)
//       Tstatfs  / Rstatfs  (8 / 9)
//       Tfsync   / Rfsync   (50 / 51)
//   - Mutation family (P5-wire-mutation extension):
//       Tsymlink   / Rsymlink   (16 / 17)
//       Tmknod     / Rmknod     (18 / 19)
//       Trename    / Rrename    (20 / 21)
//       Treadlink  / Rreadlink  (22 / 23)
//       Tlink      / Rlink      (70 / 71)
//       Tmkdir     / Rmkdir     (72 / 73)
//       Trenameat  / Rrenameat  (74 / 75)
//       Tunlinkat  / Runlinkat  (76 / 77)
//   - Rlerror parse (7) — every R-message can be replaced by Rlerror.
//
// OUT OF P5-WIRE-MUTATION (deferred to follow-up chunks):
//
//   - Lock family: Tlock / Tgetlock.
//   - Xattr family: Txattrwalk / Txattrcreate.
//   - Stratum extensions: Tsync / Treflink / Tbind / Tunbind.
//
// Wire conventions:
//
//   - All multi-byte integers are LITTLE-ENDIAN.
//   - Strings are 16-bit-length-prefixed, NOT NUL-terminated. The codec
//     hands out POINTERS INTO the source buffer; the caller is responsible
//     for copying out OR keeping the buffer alive.
//   - The common header is 7 bytes: [size: u32][type: u8][tag: u16].
//     `size` is the TOTAL message length INCLUDING the size field itself.
//
// Error convention:
//
//   - All pack/unpack/build/parse functions return either a non-negative
//     byte count (or 0 for success on parse) or a NEGATIVE value on error.
//   - There is no errno-style separate channel; errors are coarse-grained
//     ("buffer too small / message malformed / unexpected type").
//   - The caller short-circuits on the first negative return; subsequent
//     calls on the same buffer are no-ops.
//
// Audit posture: spec-bearing per CLAUDE.md §"Audit-triggering changes".
// Per `specs/9p_client.tla`, this codec is the wire-level realization of
// SendIO / SendWalk / SendClunk / ReceiveOp. Bugs in framing or
// length-bound checks can break I-10 (tag uniqueness if a malformed Rmsg's
// tag is mis-decoded) and I-11 (fid stability if a Twalk's newfid is
// corrupted). The unit-test suite at `kernel/test/test_9p_wire.c` covers
// round-trip + every malformed-input shape.

#ifndef THYLACINE_9P_WIRE_H
#define THYLACINE_9P_WIRE_H

#include <thylacine/types.h>

// =============================================================================
// 9P2000.L message types (P5-wire subset; deferred messages enumerated as
// constants for forward reference but not encoded/decoded here).
// =============================================================================

// Subset implemented at P5-wire:
#define P9_TVERSION    100u
#define P9_RVERSION    101u
#define P9_TATTACH     104u
#define P9_RATTACH     105u
#define P9_TWALK       110u
#define P9_RWALK       111u
#define P9_TCLUNK      120u
#define P9_RCLUNK      121u
#define P9_RLERROR     7u

// Deferred — encoded/decoded in follow-up chunks (P5-wire-io / -meta /
// -mutation / -xattr / -lock / -stratum-ext). Listed here for forward
// reference + to keep the dispatcher's switch exhaustive when P5-session
// lands.
#define P9_TSTATFS     8u
#define P9_RSTATFS     9u
#define P9_TLOPEN      12u
#define P9_RLOPEN      13u
#define P9_TLCREATE    14u
#define P9_RLCREATE    15u
#define P9_TSYMLINK    16u
#define P9_RSYMLINK    17u
#define P9_TMKNOD      18u
#define P9_RMKNOD      19u
#define P9_TRENAME     20u
#define P9_RRENAME     21u
#define P9_TREADLINK   22u
#define P9_RREADLINK   23u
#define P9_TGETATTR    24u
#define P9_RGETATTR    25u
#define P9_TSETATTR    26u
#define P9_RSETATTR    27u
#define P9_TXATTRWALK  30u
#define P9_RXATTRWALK  31u
#define P9_TXATTRCREATE 32u
#define P9_RXATTRCREATE 33u
#define P9_TREADDIR    40u
#define P9_RREADDIR    41u
#define P9_TFSYNC      50u
#define P9_RFSYNC      51u
#define P9_TLOCK       52u
#define P9_RLOCK       53u
#define P9_TGETLOCK    54u
#define P9_RGETLOCK    55u
#define P9_TLINK       70u
#define P9_RLINK       71u
#define P9_TMKDIR      72u
#define P9_RMKDIR      73u
#define P9_TRENAMEAT   74u
#define P9_RRENAMEAT   75u
#define P9_TUNLINKAT   76u
#define P9_RUNLINKAT   77u
#define P9_TFLUSH      108u
#define P9_RFLUSH      109u
#define P9_TREAD       116u
#define P9_RREAD       117u
#define P9_TWRITE      118u
#define P9_RWRITE      119u
// Stratum extensions (per stratum/v2/include/stratum/9p.h):
#define P9_TBIND       124u
#define P9_RBIND       125u
#define P9_TUNBIND     126u
#define P9_RUNBIND     127u
#define P9_TSYNC       128u
#define P9_RSYNC       129u
#define P9_TREFLINK    130u
#define P9_RREFLINK    131u
#define P9_TFALLOCATE  132u
#define P9_RFALLOCATE  133u
// Thylacine extension (Weft-6; NET-THROUGHPUT.md section 6): the per-flow
// zero-copy dataplane ring setup. 134/135 sit just past the Stratum extension
// range (128/129 are Tsync/Rsync, 132/133 Tfallocate). Kernel-client-issued op
// (the #845 Tflush precedent).
#define P9_TWEFT       134u
#define P9_RWEFT       135u
// Thylacine extension (Weft-6b-2; NET-THROUGHPUT.md section 6.2): the data
// drive. After a flow's ring is mapped (Tweft), a large Twrite/Tread on the
// data fd issues Tweftio carrying the kernel-validated payload descriptor
// (offset + len within the flow's shared ring + a direction); netd reads/writes
// the ring IN PLACE + replies the count. Kernel-client-issued (the Tweft/Tflush
// precedent). 136/137 sit just past Tweft/Rweft (134/135).
#define P9_TWEFTIO     136u
#define P9_RWEFTIO     137u

// Tweftio direction -- which way the payload moves through the shared ring.
#define WEFT_DIR_WRITE 0u   // TX: netd reads ring[off..off+len] -> smoltcp send
#define WEFT_DIR_READ  1u   // RX: netd writes recv bytes -> ring[off..off+len]

// Thylacine POUNCE extension (docs/POUNCE-DESIGN.md): fused walk+getattr.
// One RPC walks up to P9_MAX_WALK components AND returns each walked
// component's full Rgetattr body (the walk-fused per-component X-search
// attrs); newfid == P9_NOFID is permitted as a walk-QUERY (walk + sample,
// bind nothing -- nothing to clunk; the 1-RPC stat). Kernel-client-issued
// against stratumd (the Tsync/Tflush precedent). NUMBERING: 140/141, NOT
// 138/139 -- the Stratum extension enum runs through Tfadvise 134/135 +
// Tpin 136/137 + Tunpin 138/139 (so Tweft/Tweftio above ALREADY collide
// latently with Stratum's 134-137 on a DISJOINT domain [Weft ops go
// kernel->netd only, never to stratumd] -- the registry reconciliation is
// #371); 140/141 is free in BOTH registries.
#define P9_TWALKGETATTR 140u
#define P9_RWALKGETATTR 141u

// One Rwalkgetattr element == one Rgetattr body (valid + qid + attrs).
#define P9_WGA_BODY_LEN 153u

// =============================================================================
// Wire-format constants.
// =============================================================================

// The common header on every 9P message: u32 size + u8 type + u16 tag.
#define P9_HDR_LEN     7u

// A qid is exactly 13 bytes on the wire: u8 type + u32 version + u64 path.
#define P9_QID_LEN     13u

// Sentinels (canonical per 9P2000 + 9P2000.L).
#define P9_NOFID       ((u32)0xFFFFFFFFu)
#define P9_NOTAG       ((u16)0xFFFFu)

// Walk caps (matches Stratum + Linux v9fs convention).
#define P9_MAX_WALK    16u            // per-Twalk wname-count cap
#define P9_NAME_MAX    255u           // per-name byte cap (matches Linux NAME_MAX)

// =============================================================================
// QID struct (in-memory shape; wire shape is the 13-byte little-endian
// encoding).
// =============================================================================

struct p9_qid {
    u8  type;       // QID type bits (P9_QT* below)
    u32 version;    // server-managed monotonic version
    u64 path;       // inode-like identifier
};

// QID type bits (mirrors Stratum's STM_9P_QT*).
#define P9_QTDIR        0x80u
#define P9_QTFILE       0x00u
#define P9_QTSYMLINK    0x02u
#define P9_QTAUTH       0x08u
#define P9_QTTMP        0x04u         // O_TMPFILE-shape
// Thylacine extension (net-6b-2b; NET-DESIGN section 12.2): a "pollable" file --
// one whose server (netd's per-connection `ready` file) serves a non-consuming
// readiness probe (a Tread whose offset is the poll event-mask, deferred until
// satisfiable). 0x01 is unused in 9P2000.L. dev9p.poll probes ONLY a file whose
// cached qid.type carries this bit; an unmarked file is POSIX always-ready (a
// regular file is never read by poll()). Additive; no server that omits it is
// affected. dev9p_poll fails SAFE (unmarked -> always-ready, never an unsound
// probe).
#define P9_QTPOLL       0x01u

// =============================================================================
// Primitive packers — write a value at `out`, return bytes written, or
// -1 if cap is insufficient. Each is a thin shift-and-byte-store.
// =============================================================================

int p9_pack_u8 (u8 *out, size_t cap, u8  v);
int p9_pack_u16(u8 *out, size_t cap, u16 v);
int p9_pack_u32(u8 *out, size_t cap, u32 v);
int p9_pack_u64(u8 *out, size_t cap, u64 v);
int p9_pack_qid(u8 *out, size_t cap, const struct p9_qid *q);

// 9P-strings: u16 LE length prefix + UTF-8 bytes (NOT NUL-terminated).
// `s` is the byte source; `slen` is its byte length. Returns 2 + slen on
// success, -1 on overflow OR slen > UINT16_MAX. Caller responsible for
// enforcing per-spec caps (e.g., P9_NAME_MAX = 255 for path components).
int p9_pack_str(u8 *out, size_t cap, const u8 *s, size_t slen);

// =============================================================================
// Primitive unpackers — read from `in`, advance, return bytes consumed,
// or -1 on underflow. Caller short-circuits on negative.
// =============================================================================

int p9_unpack_u8 (const u8 *in, size_t remaining, u8  *out);
int p9_unpack_u16(const u8 *in, size_t remaining, u16 *out);
int p9_unpack_u32(const u8 *in, size_t remaining, u32 *out);
int p9_unpack_u64(const u8 *in, size_t remaining, u64 *out);
int p9_unpack_qid(const u8 *in, size_t remaining, struct p9_qid *q);

// 9P-string unpack: sets *out_ptr to point INTO `in` (zero-copy);
// sets *out_len to the string's byte length. Returns 2 + *out_len on
// success, -1 on underflow / malformed. The caller MUST not free `in`
// before consuming *out_ptr.
int p9_unpack_str(const u8 *in, size_t remaining,
                  const u8 **out_ptr, u16 *out_len);

// =============================================================================
// Header peek: extract size + type + tag without consuming the body.
// Returns 0 on success, -1 on underflow (< 7 bytes available).
// =============================================================================

int p9_peek_header(const u8 *in, size_t len,
                   u32 *size, u8 *type, u16 *tag);

// =============================================================================
// High-level Tmsg builders. Each returns the total Tmsg length (including
// the 7-byte header), or -1 on overflow / arg violation. Caller passes a
// buffer of at least the expected Tmsg length; the function writes the
// complete framed Tmsg. `tag` may be P9_NOTAG only for Tversion (per spec).
// =============================================================================

// Tversion: handshake. version is the dialect string (typically
// "9P2000.L"). version_len is its byte length (excluding NUL).
int p9_build_tversion(u8 *out, size_t cap,
                      u16 tag, u32 msize,
                      const u8 *version, size_t version_len);

// Tattach: bind a fid to a server-side root for this connection. afid is
// the auth fid (P9_NOFID for no auth). uname/aname are 9P-strings.
// n_uname is the numeric Linux uid.
int p9_build_tattach(u8 *out, size_t cap, u16 tag,
                     u32 fid, u32 afid,
                     const u8 *uname, size_t uname_len,
                     const u8 *aname, size_t aname_len,
                     u32 n_uname);

// Twalk: walk from `fid` along `nwname` path components, binding the
// destination into `newfid`. `nwname == 0` clones `fid` into `newfid`.
// `names` is an array of `nwname` pointers; `name_lens` is the matching
// byte length for each name. Caller enforces P9_NAME_MAX per name and
// P9_MAX_WALK total names.
int p9_build_twalk(u8 *out, size_t cap, u16 tag,
                   u32 fid, u32 newfid,
                   u16 nwname,
                   const u8 *const *names, const size_t *name_lens);

// Tclunk: release the fid. The server-side fid table entry is cleared.
int p9_build_tclunk(u8 *out, size_t cap, u16 tag, u32 fid);

// =============================================================================
// High-level Rmsg parsers. Each validates the header (size matches body
// length AND type is as expected) and extracts the body fields. Returns 0
// on success, -1 on malformed (wrong type, truncated, oversize, internal
// length mismatch). The caller should peek the header first to dispatch on
// type (which may be P9_RLERROR even where an Rxx was expected).
// =============================================================================

// Rversion: server's negotiated msize + dialect version.
int p9_parse_rversion(const u8 *in, size_t len,
                      u16 *tag, u32 *msize,
                      const u8 **version, u16 *version_len);

// Rattach: server-supplied qid for the bound root.
int p9_parse_rattach(const u8 *in, size_t len,
                     u16 *tag, struct p9_qid *qid);

// Rwalk: array of qids for each walked component. Caller passes a qid
// buffer of capacity `qid_cap` (typically P9_MAX_WALK). nwqid is set to
// the actual count returned; *nwqid <= qid_cap is enforced by the parser.
// Per the protocol, *nwqid <= nwname from the matching Twalk; the parser
// doesn't see Twalk, so the caller-cap-bound (R111 doctrine from
// stratum/v2/docs/reference/23-9p_client.md §"Five trust boundaries")
// is the load-bearing check here.
int p9_parse_rwalk(const u8 *in, size_t len,
                   u16 *tag, u16 *nwqid,
                   struct p9_qid *qids, size_t qid_cap);

// Rclunk: no body. Just header validation.
int p9_parse_rclunk(const u8 *in, size_t len, u16 *tag);

// Tflush: abandon the request bearing `oldtag`. Body: [oldtag: u16].
// Rflush: server acknowledgement (header only); after it, `oldtag` is
// reclaimable by the client.
int p9_build_tflush(u8 *out, size_t cap, u16 tag, u16 oldtag);
int p9_parse_rflush(const u8 *in, size_t len, u16 *tag);

// Rlerror: u32 LE Linux errno. Used in lieu of any expected Rxx when the
// server cannot fulfill the request.
int p9_parse_rlerror(const u8 *in, size_t len, u16 *tag, u32 *ecode);

// =============================================================================
// IO family (P5-wire-io extension).
//
// Tlopen / Rlopen — open an existing fid (post-walk) with Linux open(2)
// semantics. After Rlopen the fid retains its binding but the server-side
// state transitions from "walked" to "opened-with-mode".
//
//   Tlopen body: [fid: u32][flags: u32]
//   Rlopen body: [qid: 13][iounit: u32]
//
// Tlcreate / Rlcreate — create a new file in the directory `fid` and bind
// `fid` to the new file (NOT to the parent). Per 9P2000.L: after Rlcreate,
// the same fid number refers to the new file's inode; the client's fid
// table doesn't shift because the fid is already bound to "the same fid
// number, different inode after this op." Caller responsible for
// understanding the binding change.
//
//   Tlcreate body: [fid: u32][name: str][flags: u32][mode: u32][gid: u32]
//   Rlcreate body: [qid: 13][iounit: u32]
//
// Tread / Rread — read up to `count` bytes from offset `offset` of fid's
// open data stream. Returns the actual count read (may be < requested at
// EOF). 9P2000.L explicit-offset model means concurrent Tread on the same
// fid with different offsets is legal at the wire layer.
//
//   Tread body: [fid: u32][offset: u64][count: u32]
//   Rread body: [count: u32][data: u8 * count]
//
// Twrite / Rwrite — write `count` bytes at offset `offset` of fid's open
// data stream. Returns the actual count accepted (may be < requested
// under back-pressure).
//
//   Twrite body: [fid: u32][offset: u64][count: u32][data: u8 * count]
//   Rwrite body: [count: u32]
// =============================================================================

// Tlopen: build a Tlopen Tmsg targeting `fid` with Linux O_* flags.
int p9_build_tlopen(u8 *out, size_t cap, u16 tag, u32 fid, u32 flags);

// Rlopen parse: extract qid + iounit. iounit is the server's recommended
// max single-Tread/Twrite count (0 means "no recommendation").
int p9_parse_rlopen(const u8 *in, size_t len,
                    u16 *tag, struct p9_qid *qid, u32 *iounit);

// Tlcreate: create file `name` in directory `fid` with `flags` + `mode`
// + `gid`. After the server processes this, fid binds to the new file.
int p9_build_tlcreate(u8 *out, size_t cap, u16 tag, u32 fid,
                      const u8 *name, size_t name_len,
                      u32 flags, u32 mode, u32 gid);

// Rlcreate parse: same shape as Rlopen.
int p9_parse_rlcreate(const u8 *in, size_t len,
                      u16 *tag, struct p9_qid *qid, u32 *iounit);

// Tread: read `count` bytes at `offset` from `fid`.
int p9_build_tread(u8 *out, size_t cap, u16 tag,
                   u32 fid, u64 offset, u32 count);

// Rread parse: returns the actual count + a zero-copy pointer to data
// bytes INTO the input buffer. Caller MUST not free the input buffer
// while consuming *data_ptr.
//
// R111 doctrine: `data_cap` is the caller's expected upper bound on count
// (typically negotiated_msize - 11 for a single-message read). If the
// server's reported count exceeds data_cap, the parser refuses BEFORE
// touching the data buffer.
int p9_parse_rread(const u8 *in, size_t len,
                   u16 *tag, u32 *count,
                   const u8 **data_ptr, u32 data_cap);

// Twrite: write `count` bytes from `data` at `offset` to `fid`. The data
// bytes are copied into the output buffer.
int p9_build_twrite(u8 *out, size_t cap, u16 tag,
                    u32 fid, u64 offset, u32 count,
                    const u8 *data);

// Rwrite parse: returns the actual byte count accepted.
int p9_parse_rwrite(const u8 *in, size_t len, u16 *tag, u32 *count);

// =============================================================================
// Metadata family (P5-wire-meta extension).
//
// Tgetattr / Rgetattr — Linux statx-shaped attribute query. Caller supplies a
// `request_mask` u64 of which fields it wants; server's Rgetattr returns a
// `valid` u64 of which fields it filled. Mirrors the Linux statx mask.
//
//   Tgetattr body: [fid: u32][request_mask: u64]
//   Rgetattr body: [valid: u64][qid: 13][mode: u32][uid: u32][gid: u32]
//                  [nlink: u64][rdev: u64][size: u64][blksize: u64][blocks: u64]
//                  [atime_sec: u64][atime_nsec: u64]
//                  [mtime_sec: u64][mtime_nsec: u64]
//                  [ctime_sec: u64][ctime_nsec: u64]
//                  [btime_sec: u64][btime_nsec: u64]
//                  [gen: u64][data_version: u64]
//
// Tsetattr / Rsetattr — set Linux attributes (chmod / chown / truncate /
// futimens). Caller supplies a `valid` u32 mask of which fields are present.
//
//   Tsetattr body: [fid: u32][valid: u32][mode: u32][uid: u32][gid: u32]
//                  [size: u64][atime_sec: u64][atime_nsec: u64]
//                  [mtime_sec: u64][mtime_nsec: u64]
//   Rsetattr body: (empty body; 7-byte msg)
//
// Treaddir / Rreaddir — directory enumeration. Same on-wire shape as Tread /
// Rread (offset + count), but the returned data is a packed sequence of
// dirent records: [qid: 13][offset: u64][type: u8][name: str].
//
//   Treaddir body: [fid: u32][offset: u64][count: u32]
//   Rreaddir body: [count: u32][data: u8 * count]  (consumer parses dirents)
//
// Tstatfs / Rstatfs — filesystem statistics. Linux statfs(2)-shaped.
//
//   Tstatfs body: [fid: u32]
//   Rstatfs body: [type: u32][bsize: u32][blocks: u64][bfree: u64]
//                 [bavail: u64][files: u64][ffree: u64][fsid: u64][namelen: u32]
//
// Tfsync / Rfsync — barrier: block until prior writes on `fid` are durable.
// `datasync` (u32) is 0 for "sync everything" or 1 for "data only" (Linux
// fdatasync semantics).
//
//   Tfsync body: [fid: u32][datasync: u32]
//   Rfsync body: (empty body; 7-byte msg)
// =============================================================================

// Linux statx-like attribute record. Used by both Tgetattr's response and
// Tsetattr's payload (the latter via `struct p9_setattr` below).
struct p9_attr {
    u64 valid;          // Rgetattr: which fields the server filled
    struct p9_qid qid;
    u32 mode;
    u32 uid;
    u32 gid;
    u64 nlink;
    u64 rdev;
    u64 size;
    u64 blksize;
    u64 blocks;
    u64 atime_sec;
    u64 atime_nsec;
    u64 mtime_sec;
    u64 mtime_nsec;
    u64 ctime_sec;
    u64 ctime_nsec;
    u64 btime_sec;
    u64 btime_nsec;
    u64 gen;
    u64 data_version;
};

// Tsetattr payload (smaller than p9_attr; only the fields the protocol
// allows to set). The `valid` mask says which fields are present in the
// message; the others are ignored by the server.
struct p9_setattr {
    u32 valid;          // bit mask: see P9_SETATTR_* below
    u32 mode;
    u32 uid;
    u32 gid;
    u64 size;
    u64 atime_sec;
    u64 atime_nsec;
    u64 mtime_sec;
    u64 mtime_nsec;
};

// Tsetattr valid-mask bits (Linux v9fs convention).
#define P9_SETATTR_MODE       (1u << 0)
#define P9_SETATTR_UID        (1u << 1)
#define P9_SETATTR_GID        (1u << 2)
#define P9_SETATTR_SIZE       (1u << 3)
#define P9_SETATTR_ATIME      (1u << 4)
#define P9_SETATTR_MTIME      (1u << 5)
#define P9_SETATTR_CTIME      (1u << 6)
#define P9_SETATTR_ATIME_SET  (1u << 7)
#define P9_SETATTR_MTIME_SET  (1u << 8)

// Rstatfs payload.
struct p9_statfs {
    u32 type;
    u32 bsize;
    u64 blocks;
    u64 bfree;
    u64 bavail;
    u64 files;
    u64 ffree;
    u64 fsid;
    u32 namelen;
};

// Tgetattr request-mask convention (Linux v9fs STATX_*); the server may
// return fewer fields than requested (its `valid` reports which it filled).
// Individual field bits (a consumer that enforces against mode/uid/gid must
// confirm the server actually filled them -- A-2d / A-2a F2 fail-closed).
#define P9_GETATTR_MODE       0x00000001ull
#define P9_GETATTR_NLINK      0x00000002ull
#define P9_GETATTR_UID        0x00000004ull
#define P9_GETATTR_GID        0x00000008ull
#define P9_GETATTR_BASIC      0x000007FFull
#define P9_GETATTR_ALL        0x00003FFFull

// Tgetattr: request attributes for `fid`. `request_mask` is a hint; the
// server's response carries the authoritative `valid` mask.
int p9_build_tgetattr(u8 *out, size_t cap, u16 tag,
                      u32 fid, u64 request_mask);

// Rgetattr parse: extract the full attribute record. `out_attr->valid`
// tells the caller which fields are actually meaningful.
int p9_parse_rgetattr(const u8 *in, size_t len,
                      u16 *tag, struct p9_attr *out_attr);

// One Rgetattr BODY (valid + qid + attrs; P9_WGA_BODY_LEN bytes) parsed
// from `in` -- the shared element parser for Rgetattr and Rwalkgetattr
// (the POUNCE per-component attrs; the two layouts are byte-identical
// by design). Returns the consumed byte count (== P9_WGA_BODY_LEN) or
// -1 on a short buffer.
int p9_parse_getattr_body(const u8 *in, size_t rem, struct p9_attr *out_attr);

// Twalkgetattr (POUNCE, 140): Twalk's shape + request_mask. newfid may
// be P9_NOFID (walk-query: server binds nothing).
int p9_build_twalkgetattr(u8 *out, size_t cap, u16 tag,
                          u32 fid, u32 newfid, u64 request_mask,
                          u16 nwname,
                          const u8 *const *names, const size_t *name_lens);

// Rwalkgetattr parse (141): validates the frame STRICTLY (nwqid bounded
// by qid_cap/P9_MAX_WALK; body length == nwqid * P9_WGA_BODY_LEN), fills
// qids[i] from each element's embedded qid, and returns via *body_out a
// pointer to the FIRST element (aliases `in`; the caller extracts each
// element's full attrs with p9_parse_getattr_body while the reply frame
// stays alive). body_out may be NULL when only qids are wanted.
int p9_parse_rwalkgetattr(const u8 *in, size_t len,
                          u16 *tag, u16 *nwqid_out,
                          struct p9_qid *qids, size_t qid_cap,
                          const u8 **body_out);

// Tsetattr: set attributes for `fid`. The `attr->valid` mask says which
// fields are being set; only those should be honored by the server.
int p9_build_tsetattr(u8 *out, size_t cap, u16 tag,
                      u32 fid, const struct p9_setattr *attr);

// Rsetattr parse: header-only validation (no body).
int p9_parse_rsetattr(const u8 *in, size_t len, u16 *tag);

// Treaddir: read up to `count` bytes of dirent data from `fid` at `offset`.
// Same on-wire shape as Tread.
int p9_build_treaddir(u8 *out, size_t cap, u16 tag,
                      u32 fid, u64 offset, u32 count);

// Rreaddir parse: zero-copy data pointer into the input buffer. The caller
// parses dirent records via p9_unpack_dirent (below). Same R111
// caller-cap-bound discipline as Rread.
int p9_parse_rreaddir(const u8 *in, size_t len,
                      u16 *tag, u32 *count,
                      const u8 **data_ptr, u32 data_cap);

// Unpack one 9P2000.L dirent record from the dirent stream.
//   record: [qid: 13][offset: u64][type: u8][name: str]
// `out_name_ptr` points INTO `in` (zero-copy); caller copies if needed.
// Returns bytes consumed (>0) on success, -1 on underflow.
int p9_unpack_dirent(const u8 *in, size_t remaining,
                     struct p9_qid *out_qid, u64 *out_offset,
                     u8 *out_type,
                     const u8 **out_name_ptr, u16 *out_name_len);

// Tstatfs: filesystem statistics for `fid`.
int p9_build_tstatfs(u8 *out, size_t cap, u16 tag, u32 fid);

// Rstatfs parse.
int p9_parse_rstatfs(const u8 *in, size_t len,
                     u16 *tag, struct p9_statfs *out);

// Tfsync: barrier for `fid`. `datasync` per Linux fdatasync(2): 0 = sync
// data + metadata, 1 = sync data only.
int p9_build_tfsync(u8 *out, size_t cap, u16 tag, u32 fid, u32 datasync);

// Rfsync parse: header-only (no body).
int p9_parse_rfsync(const u8 *in, size_t len, u16 *tag);

// =============================================================================
// Mutation family (P5-wire-mutation extension).
//
// Directory-mutation surface: rename / unlink / link / symlink / mknod /
// mkdir / readlink. Together with the metadata + IO families, these
// complete the codec's coverage of the standard 9P2000.L filesystem
// surface. Stratum extensions (Tsync / Treflink / Tbind / Tunbind /
// Tfallocate / Tfadvise) + lock + xattr remain deferred.
//
// Three response shapes:
//
//   - Empty body (Runlinkat, Rrename, Rrenameat, Rlink) — header-only.
//   - Single qid (Rsymlink, Rmknod, Rmkdir) — server returns the qid
//     of the newly-created entry.
//   - String (Rreadlink) — server returns the symlink target.
//
// All mutation ops at the wire layer accept concurrent ops on the same
// fid EXCEPT Trename — it mutates the named binding of `fid` server-
// side, so the canonical client discipline is to refuse other in-flight
// ops on the same fid (mirrors the setattr discipline from P5-wire-meta).
//
// Tunlinkat AT_REMOVEDIR flag (Linux unlinkat(2) convention):
#define P9_UNLINK_AT_REMOVEDIR   0x200u

// Tsymlink: create symlink `name` in directory `fid`, pointing at
// `symtgt`. `gid` is the creator's primary group (Linux v9fs convention).
//   Tsymlink body: [fid: u32][name: str][symtgt: str][gid: u32]
//   Rsymlink body: [qid: 13]
int p9_build_tsymlink(u8 *out, size_t cap, u16 tag, u32 fid,
                      const u8 *name, size_t name_len,
                      const u8 *symtgt, size_t symtgt_len,
                      u32 gid);
int p9_parse_rsymlink(const u8 *in, size_t len,
                      u16 *tag, struct p9_qid *qid);

// Tmknod: create a device node (or fifo/socket) `name` in directory
// `dfid`. `mode` is Linux mknod(2) mode bits; `major` / `minor` are
// device numbers (ignored for fifo/socket); `gid` is the creator's gid.
//   Tmknod body: [dfid: u32][name: str][mode: u32][major: u32][minor: u32][gid: u32]
//   Rmknod body: [qid: 13]
int p9_build_tmknod(u8 *out, size_t cap, u16 tag, u32 dfid,
                    const u8 *name, size_t name_len,
                    u32 mode, u32 major, u32 minor, u32 gid);
int p9_parse_rmknod(const u8 *in, size_t len,
                    u16 *tag, struct p9_qid *qid);

// Trename: rename the file referenced by `fid` to `name` in directory
// `dfid`. After Rrename the server-side fid still refers to the same
// inode but at a different path.
//   Trename body: [fid: u32][dfid: u32][name: str]
//   Rrename body: (empty body; 7-byte msg)
int p9_build_trename(u8 *out, size_t cap, u16 tag,
                     u32 fid, u32 dfid,
                     const u8 *name, size_t name_len);
int p9_parse_rrename(const u8 *in, size_t len, u16 *tag);

// Treadlink: read the target of the symlink referenced by `fid`.
//   Treadlink body: [fid: u32]
//   Rreadlink body: [target: str]
// `target_ptr` / `target_len` are zero-copy pointers INTO `in`. The
// caller must not free the input buffer while consuming them.
int p9_build_treadlink(u8 *out, size_t cap, u16 tag, u32 fid);
int p9_parse_rreadlink(const u8 *in, size_t len,
                       u16 *tag,
                       const u8 **target_ptr, u16 *target_len);

// Tlink: create a hard link to the file referenced by `fid`, with name
// `name`, in directory `dfid`.
//   Tlink body: [dfid: u32][fid: u32][name: str]
//   Rlink body: (empty body; 7-byte msg)
int p9_build_tlink(u8 *out, size_t cap, u16 tag,
                   u32 dfid, u32 fid,
                   const u8 *name, size_t name_len);
int p9_parse_rlink(const u8 *in, size_t len, u16 *tag);

// Tmkdir: create directory `name` in directory `dfid` with mode bits
// `mode` (Linux v9fs convention). `gid` is the creator's gid.
//   Tmkdir body: [dfid: u32][name: str][mode: u32][gid: u32]
//   Rmkdir body: [qid: 13]
int p9_build_tmkdir(u8 *out, size_t cap, u16 tag, u32 dfid,
                    const u8 *name, size_t name_len,
                    u32 mode, u32 gid);
int p9_parse_rmkdir(const u8 *in, size_t len,
                    u16 *tag, struct p9_qid *qid);

// Trenameat: rename `oldname` in directory `olddirfid` to `newname` in
// directory `newdirfid`. Pure path-based; no fid is rebound.
//   Trenameat body: [olddirfid: u32][oldname: str][newdirfid: u32][newname: str]
//   Rrenameat body: (empty body; 7-byte msg)
int p9_build_trenameat(u8 *out, size_t cap, u16 tag,
                       u32 olddirfid,
                       const u8 *oldname, size_t oldname_len,
                       u32 newdirfid,
                       const u8 *newname, size_t newname_len);
int p9_parse_rrenameat(const u8 *in, size_t len, u16 *tag);

// Tunlinkat: unlink `name` from directory `dfid`. `flags` may include
// P9_UNLINK_AT_REMOVEDIR for rmdir-equivalent.
//   Tunlinkat body: [dfid: u32][name: str][flags: u32]
//   Runlinkat body: (empty body; 7-byte msg)
int p9_build_tunlinkat(u8 *out, size_t cap, u16 tag, u32 dfid,
                       const u8 *name, size_t name_len, u32 flags);
int p9_parse_runlinkat(const u8 *in, size_t len, u16 *tag);

// =============================================================================
// Weft extension (Weft-6; NET-THROUGHPUT.md section 6).
//
// The per-flow zero-copy dataplane ring setup -- a kernel-client-issued
// request/reply op (the #845 Tflush precedent). On the FIRST zero-copy use of a
// /net flow, the kernel dev9p client issues Tweft(fid) to netd; netd resolves
// the fid to its connection, allocates the per-flow shared ring,
// SYS_WEFT_SHARE-registers it (minting a kernel-scoped share_id), and replies
// Rweft carrying that share_id + the ring geometry. The kernel then joins the
// share_id to the pinned ring Burrow and maps it into the guest.
//
//   Tweft body: [fid: u32]
//   Rweft body: [share_id: u64][ring_size: u32][ring_entries: u32]   (16 bytes)
//
// The share_id is kernel-minted (by SYS_WEFT_SHARE) and netd-echoed here; the
// kernel honors it only when it arrives on the kernel's own Tweft to that netd
// (the RDMA-rkey shape -- never handed to the guest, never forgeable).
// =============================================================================

// Rweft payload: the kernel-minted registration token + the ring geometry the
// kernel needs to compute its private weft_ring_view (it does NOT trust the
// shared header's geometry mirror -- the Weft-3 snapshot discipline).
struct p9_weft_geom {
    u64 share_id;       // kernel-scoped registration token (netd->kernel join key)
    u32 ring_size;      // total shared ring size in bytes
    u32 ring_entries;   // descriptor-ring entry count
};

// Tweft: request a zero-copy ring for the flow bound to `fid`.
int p9_build_tweft(u8 *out, size_t cap, u16 tag, u32 fid);

// Tweft parse (server / test side): extract the target fid.
int p9_parse_tweft(const u8 *in, size_t len, u16 *tag, u32 *fid);

// Rweft: the server's ring grant -- share_id + geometry.
int p9_build_rweft(u8 *out, size_t cap, u16 tag,
                   const struct p9_weft_geom *geom);

// Rweft parse (client side): extract share_id + geometry.
int p9_parse_rweft(const u8 *in, size_t len,
                   u16 *tag, struct p9_weft_geom *out);

// =============================================================================
// Weft data drive (Weft-6b-2; NET-THROUGHPUT.md section 6.2) -- Tweftio/Rweftio.
//
// After Tweft maps the flow's shared ring, a large Twrite/Tread on the data fd
// drives the payload through the ring instead of the 9P body. The kernel dev9p
// path validates the descriptor (the syscall buffer's offset + len within the
// flow's payload region, against its private weft_ring_view -- the I-30
// validator-once) and issues Tweftio; netd reads/writes the ring IN PLACE at
// that offset and replies Rweftio(count).
//
//   Tweftio body: [fid: u32][off: u32][len: u32][dir: u32]   (16 bytes)
//   Rweftio body: [count: u32]                               (4 bytes)
//
// `off`/`len` are PAYLOAD-region-relative (the same domain as weft_desc.addr),
// already bounds-validated by the kernel; netd reads them as a trusted region
// within the ring it allocated. `dir` is WEFT_DIR_WRITE / WEFT_DIR_READ.
// =============================================================================

// Tweftio: drive `len` payload bytes at ring offset `off` for the flow bound to
// `fid`, in direction `dir` (WEFT_DIR_WRITE / WEFT_DIR_READ).
int p9_build_tweftio(u8 *out, size_t cap, u16 tag,
                     u32 fid, u32 off, u32 len, u32 dir);

// Tweftio parse (server / test side): extract the descriptor + direction.
int p9_parse_tweftio(const u8 *in, size_t len, u16 *tag,
                     u32 *fid, u32 *off, u32 *xfer_len, u32 *dir);

// Rweftio: the count of payload bytes the consumer actually moved.
int p9_build_rweftio(u8 *out, size_t cap, u16 tag, u32 count);

// Rweftio parse (client side): extract the moved-byte count.
int p9_parse_rweftio(const u8 *in, size_t len, u16 *tag, u32 *count);

#endif  // THYLACINE_9P_WIRE_H
