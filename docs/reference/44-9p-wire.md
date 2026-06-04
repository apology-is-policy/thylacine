# 44 — 9P2000.L wire codec

## Purpose

`kernel/9p_wire.c` + `kernel/include/thylacine/9p_wire.h` implement the pure-byte-level marshal/unmarshal layer for the 9P2000.L dialect Thylacine speaks. It is the **lowest** layer of the kernel 9P client stack (per `specs/9p_client.tla` + ARCH §14.2): P5-session, P5-transport, and P5-attach compose on top.

The codec has **no kernel state** — no allocation, no SLUB, no syscall dispatch, no I/O. Every function reads/writes a caller-supplied byte buffer and returns a byte count (or a negative error). This makes it trivially testable, trivially safe across kernel/userspace contexts (a future Phase 6+ POSIX shim could use the same codec verbatim), and trivially audited (the entire surface is ~370 LOC; every error path is local).

Audit class per CLAUDE.md §"Audit-triggering changes": the `kernel/9p_*` surface joins the trigger list once P5-session lands; this codec is the foundation that surface composes on. Bugs in framing or length-bound checks can break I-10 (tag uniqueness — if a malformed Rmsg's tag is mis-decoded) and I-11 (fid stability — if a Twalk's `newfid` is corrupted).

## Public API

```c
// Primitives — pack/unpack u8 / u16 / u32 / u64 / 9P-string / qid.
int p9_pack_u8 (u8 *out, size_t cap, u8  v);
int p9_pack_u16(u8 *out, size_t cap, u16 v);
int p9_pack_u32(u8 *out, size_t cap, u32 v);
int p9_pack_u64(u8 *out, size_t cap, u64 v);
int p9_pack_qid(u8 *out, size_t cap, const struct p9_qid *q);
int p9_pack_str(u8 *out, size_t cap, const u8 *s, size_t slen);

int p9_unpack_u8 (const u8 *in, size_t remaining, u8  *out);
int p9_unpack_u16(const u8 *in, size_t remaining, u16 *out);
int p9_unpack_u32(const u8 *in, size_t remaining, u32 *out);
int p9_unpack_u64(const u8 *in, size_t remaining, u64 *out);
int p9_unpack_qid(const u8 *in, size_t remaining, struct p9_qid *q);
int p9_unpack_str(const u8 *in, size_t remaining,
                  const u8 **out_ptr, u16 *out_len);

// Header — extract size + type + tag without consuming the body.
int p9_peek_header(const u8 *in, size_t len, u32 *size, u8 *type, u16 *tag);

// Tmsg builders.
int p9_build_tversion(u8 *out, size_t cap, u16 tag, u32 msize,
                      const u8 *version, size_t version_len);
int p9_build_tattach (u8 *out, size_t cap, u16 tag,
                      u32 fid, u32 afid,
                      const u8 *uname, size_t uname_len,
                      const u8 *aname, size_t aname_len,
                      u32 n_uname);
int p9_build_twalk   (u8 *out, size_t cap, u16 tag,
                      u32 fid, u32 newfid, u16 nwname,
                      const u8 *const *names, const size_t *name_lens);
int p9_build_tclunk  (u8 *out, size_t cap, u16 tag, u32 fid);
// IO family (P5-wire-io).
int p9_build_tlopen  (u8 *out, size_t cap, u16 tag, u32 fid, u32 flags);
int p9_build_tlcreate(u8 *out, size_t cap, u16 tag, u32 fid,
                      const u8 *name, size_t name_len,
                      u32 flags, u32 mode, u32 gid);
int p9_build_tread   (u8 *out, size_t cap, u16 tag,
                      u32 fid, u64 offset, u32 count);
int p9_build_twrite  (u8 *out, size_t cap, u16 tag,
                      u32 fid, u64 offset, u32 count, const u8 *data);
// Metadata family (P5-wire-meta).
int p9_build_tgetattr(u8 *out, size_t cap, u16 tag, u32 fid, u64 request_mask);
int p9_build_tsetattr(u8 *out, size_t cap, u16 tag, u32 fid,
                      const struct p9_setattr *attr);
int p9_build_treaddir(u8 *out, size_t cap, u16 tag,
                      u32 fid, u64 offset, u32 count);
int p9_build_tstatfs (u8 *out, size_t cap, u16 tag, u32 fid);
int p9_build_tfsync  (u8 *out, size_t cap, u16 tag, u32 fid, u32 datasync);
// Mutation family (P5-wire-mutation).
int p9_build_tsymlink (u8 *out, size_t cap, u16 tag, u32 fid,
                       const u8 *name, size_t name_len,
                       const u8 *symtgt, size_t symtgt_len, u32 gid);
int p9_build_tmknod   (u8 *out, size_t cap, u16 tag, u32 dfid,
                       const u8 *name, size_t name_len,
                       u32 mode, u32 major, u32 minor, u32 gid);
int p9_build_trename  (u8 *out, size_t cap, u16 tag, u32 fid, u32 dfid,
                       const u8 *name, size_t name_len);
int p9_build_treadlink(u8 *out, size_t cap, u16 tag, u32 fid);
int p9_build_tlink    (u8 *out, size_t cap, u16 tag, u32 dfid, u32 fid,
                       const u8 *name, size_t name_len);
int p9_build_tmkdir   (u8 *out, size_t cap, u16 tag, u32 dfid,
                       const u8 *name, size_t name_len,
                       u32 mode, u32 gid);
int p9_build_trenameat(u8 *out, size_t cap, u16 tag, u32 olddirfid,
                       const u8 *oldname, size_t oldname_len,
                       u32 newdirfid,
                       const u8 *newname, size_t newname_len);
int p9_build_tunlinkat(u8 *out, size_t cap, u16 tag, u32 dfid,
                       const u8 *name, size_t name_len, u32 flags);

// Rmsg parsers.
int p9_parse_rversion(const u8 *in, size_t len, u16 *tag, u32 *msize,
                      const u8 **version, u16 *version_len);
int p9_parse_rattach (const u8 *in, size_t len, u16 *tag, struct p9_qid *qid);
int p9_parse_rwalk   (const u8 *in, size_t len, u16 *tag,
                      u16 *nwqid, struct p9_qid *qids, size_t qid_cap);
int p9_parse_rclunk  (const u8 *in, size_t len, u16 *tag);
int p9_parse_rlerror (const u8 *in, size_t len, u16 *tag, u32 *ecode);
// IO family (P5-wire-io).
int p9_parse_rlopen  (const u8 *in, size_t len,
                      u16 *tag, struct p9_qid *qid, u32 *iounit);
int p9_parse_rlcreate(const u8 *in, size_t len,
                      u16 *tag, struct p9_qid *qid, u32 *iounit);
int p9_parse_rread   (const u8 *in, size_t len,
                      u16 *tag, u32 *count, const u8 **data_ptr, u32 data_cap);
int p9_parse_rwrite  (const u8 *in, size_t len, u16 *tag, u32 *count);
// Metadata family (P5-wire-meta).
int p9_parse_rgetattr(const u8 *in, size_t len,
                      u16 *tag, struct p9_attr *out_attr);
int p9_parse_rsetattr(const u8 *in, size_t len, u16 *tag);
int p9_parse_rreaddir(const u8 *in, size_t len,
                      u16 *tag, u32 *count, const u8 **data_ptr, u32 data_cap);
int p9_parse_rstatfs (const u8 *in, size_t len, u16 *tag, struct p9_statfs *out);
int p9_parse_rfsync  (const u8 *in, size_t len, u16 *tag);
// Dirent record unpack (consumed from Rreaddir's data stream).
int p9_unpack_dirent (const u8 *in, size_t remaining,
                      struct p9_qid *out_qid, u64 *out_offset, u8 *out_type,
                      const u8 **out_name_ptr, u16 *out_name_len);
// Mutation family (P5-wire-mutation).
int p9_parse_rsymlink (const u8 *in, size_t len, u16 *tag, struct p9_qid *qid);
int p9_parse_rmknod   (const u8 *in, size_t len, u16 *tag, struct p9_qid *qid);
int p9_parse_rrename  (const u8 *in, size_t len, u16 *tag);
int p9_parse_rreadlink(const u8 *in, size_t len, u16 *tag,
                       const u8 **target_ptr, u16 *target_len);
int p9_parse_rlink    (const u8 *in, size_t len, u16 *tag);
int p9_parse_rmkdir   (const u8 *in, size_t len, u16 *tag, struct p9_qid *qid);
int p9_parse_rrenameat(const u8 *in, size_t len, u16 *tag);
int p9_parse_runlinkat(const u8 *in, size_t len, u16 *tag);
```

### Error convention

All functions return either:
- a **non-negative** count (bytes written/consumed, OR `0` for parsers indicating success), OR
- a **negative** value on error.

The caller short-circuits on the first negative return. Error categories are coarse (buffer-too-small / malformed / wrong-type); no errno-style separate channel.

### Strict-equality framing

All parsers enforce `header.size == frame_length` AND `body offset == frame_length` after the last field unpacks. Truncated frames are rejected; extra trailing bytes are rejected. Mirrors Stratum's `<libstratum-9p>` R111 P3 F-10 doctrine (`stratum/v2/docs/reference/23-9p_client.md` §"Five trust boundaries").

### R111 caller-cap-bound discipline

For server-supplied counts that get written into caller buffers, the bound is enforced BEFORE the write. In the codec's surface, this applies to:

- `p9_parse_rwalk`: the server-supplied `nwqid` is bounded against the caller's `qid_cap` BEFORE any qid is unpacked into the buffer. An out-of-spec `Rwalk(nwqid=99)` on a 2-qid caller buffer is rejected; the buffer is not touched.
- `p9_parse_rread`: the server-supplied `count` is bounded against the caller's `data_cap` BEFORE the data pointer is exposed. An out-of-spec `Rread(count=4096)` against a 512-byte caller cap is rejected; `*data_ptr` stays `NULL` and the caller never observes the oversize buffer.
- `p9_parse_rreaddir`: identical discipline to Rread — server-supplied dirent-stream count bounded against caller's `data_cap` BEFORE the data pointer is exposed.

The `p9_unpack_str` function returns a pointer INTO the input buffer (zero-copy), so caller-cap-bound is the caller's responsibility there: the caller checks `out_len <= ENFORCED_MAX` before consuming `out_ptr[0..out_len)`. The same zero-copy discipline applies to `p9_parse_rread`'s `*data_ptr` output AND `p9_parse_rreaddir`'s `*data_ptr` — the caller must not free the input buffer while consuming either. `p9_unpack_dirent` likewise hands out pointers into the dirent stream for the `name` field.

## Wire-format crib

Per `https://github.com/chaos/diod/blob/master/protocol.md` + `stratum/v2/docs/reference/20-9p.md`:

| Construct | Layout |
|---|---|
| Common header (every msg) | `[size: u32 LE][type: u8][tag: u16 LE]` (7 bytes) |
| 9P-string | `[slen: u16 LE][bytes: u8 * slen]` (NOT NUL-terminated) |
| QID | `[type: u8][version: u32 LE][path: u64 LE]` (13 bytes) |

Per-message bodies (the cumulative codec subset through P5-wire-io):

| Msg | Body |
|---|---|
| Tversion (100) | `[msize: u32][version: str]` |
| Rversion (101) | `[msize: u32][version: str]` |
| Tattach (104) | `[fid: u32][afid: u32][uname: str][aname: str][n_uname: u32]` |
| Rattach (105) | `[qid: 13]` |
| Twalk (110) | `[fid: u32][newfid: u32][nwname: u16][wname[nwname]: str * nwname]` |
| Rwalk (111) | `[nwqid: u16][wqid[nwqid]: qid * nwqid]` |
| Tclunk (120) | `[fid: u32]` |
| Rclunk (121) | (empty body; 7-byte msg) |
| Rlerror (7) | `[ecode: u32]` (Linux errno) |
| Tlopen (12) | `[fid: u32][flags: u32]` |
| Rlopen (13) | `[qid: 13][iounit: u32]` |
| Tlcreate (14) | `[fid: u32][name: str][flags: u32][mode: u32][gid: u32]` |
| Rlcreate (15) | `[qid: 13][iounit: u32]` |
| Tread (116) | `[fid: u32][offset: u64][count: u32]` |
| Rread (117) | `[count: u32][data: u8 * count]` |
| Twrite (118) | `[fid: u32][offset: u64][count: u32][data: u8 * count]` |
| Rwrite (119) | `[count: u32]` |
| Tgetattr (24) | `[fid: u32][request_mask: u64]` |
| Rgetattr (25) | `[valid: u64][qid: 13][mode: u32][uid: u32][gid: u32][nlink: u64][rdev: u64][size: u64][blksize: u64][blocks: u64][atime_sec: u64][atime_nsec: u64][mtime_sec: u64][mtime_nsec: u64][ctime_sec: u64][ctime_nsec: u64][btime_sec: u64][btime_nsec: u64][gen: u64][data_version: u64]` (153 bytes body) |
| Tsetattr (26) | `[fid: u32][valid: u32][mode: u32][uid: u32][gid: u32][size: u64][atime_sec: u64][atime_nsec: u64][mtime_sec: u64][mtime_nsec: u64]` (60 bytes body) |
| Rsetattr (27) | (empty body; 7-byte msg) |
| Treaddir (40) | `[fid: u32][offset: u64][count: u32]` |
| Rreaddir (41) | `[count: u32][data: u8 * count]` (dirent stream) |
| Tstatfs (8) | `[fid: u32]` |
| Rstatfs (9) | `[type: u32][bsize: u32][blocks: u64][bfree: u64][bavail: u64][files: u64][ffree: u64][fsid: u64][namelen: u32]` (60 bytes body) |
| Tfsync (50) | `[fid: u32][datasync: u32]` |
| Rfsync (51) | (empty body; 7-byte msg) |
| Dirent record (within Rreaddir's data) | `[qid: 13][offset: u64][type: u8][name: str]` |
| Tsymlink (16) | `[fid: u32][name: str][symtgt: str][gid: u32]` |
| Rsymlink (17) | `[qid: 13]` |
| Tmknod (18) | `[dfid: u32][name: str][mode: u32][major: u32][minor: u32][gid: u32]` |
| Rmknod (19) | `[qid: 13]` |
| Trename (20) | `[fid: u32][dfid: u32][name: str]` |
| Rrename (21) | (empty body; 7-byte msg) |
| Treadlink (22) | `[fid: u32]` |
| Rreadlink (23) | `[target: str]` |
| Tlink (70) | `[dfid: u32][fid: u32][name: str]` |
| Rlink (71) | (empty body; 7-byte msg) |
| Tmkdir (72) | `[dfid: u32][name: str][mode: u32][gid: u32]` |
| Rmkdir (73) | `[qid: 13]` |
| Trenameat (74) | `[olddirfid: u32][oldname: str][newdirfid: u32][newname: str]` |
| Rrenameat (75) | (empty body; 7-byte msg) |
| Tunlinkat (76) | `[dfid: u32][name: str][flags: u32]` |
| Runlinkat (77) | (empty body; 7-byte msg) |

All integers little-endian (matches Thylacine's AArch64 host endianness; encoding is still explicit byte-shift to remain portable).

## Compile-time invariants

`_Static_assert` in `kernel/9p_wire.c`:

- `P9_HDR_LEN == 7` (header byte count).
- `P9_QID_LEN == 13` (qid byte count).
- `P9_NOFID == 0xFFFFFFFFu` (NOFID sentinel).
- `P9_NOTAG == 0xFFFFu` (NOTAG sentinel).
- `P9_TVERSION == 100`; `P9_RVERSION == P9_TVERSION + 1`; same for ATTACH/WALK/CLUNK pairs.
- `P9_TLOPEN == 12`, `P9_TLCREATE == 14`, `P9_TREAD == 116`, `P9_TWRITE == 118`; each `RX = TX + 1` (IO family, P5-wire-io).
- `P9_TGETATTR == 24`, `P9_TSETATTR == 26`, `P9_TREADDIR == 40`, `P9_TSTATFS == 8`, `P9_TFSYNC == 50`; each `RX = TX + 1` (metadata family, P5-wire-meta).
- `P9_TSYMLINK == 16`, `P9_TMKNOD == 18`, `P9_TRENAME == 20`, `P9_TREADLINK == 22`, `P9_TLINK == 70`, `P9_TMKDIR == 72`, `P9_TRENAMEAT == 74`, `P9_TUNLINKAT == 76`; each `RX = TX + 1` (mutation family, P5-wire-mutation).
- `P9_RLERROR == 7`.
- `sizeof(struct p9_qid) >= P9_QID_LEN` (in-memory shape doesn't shrink below the wire shape).

## Implementation

| File | Purpose |
|---|---|
| `kernel/include/thylacine/9p_wire.h` | Public API + constants + `struct p9_qid` |
| `kernel/9p_wire.c` | Byte-level codec; static `write_header` + `validate_rmsg_header` helpers |
| `kernel/test/test_9p_wire.c` | 37 unit tests (round-trip + malformed-input rejection; covers handshake/walk/clunk + IO + metadata + mutation families) |

The codec is purely procedural — no callbacks, no state, no allocation. Every function is freestanding.

## Tests

37 tests in `kernel/test/test_9p_wire.c`:

| Test | Covers |
|---|---|
| `9p_wire.primitives_round_trip` | u8/u16/u32/u64 pack→unpack + LE byte order |
| `9p_wire.primitives_overflow` | Pack underflow + unpack underflow rejection |
| `9p_wire.str_round_trip` | 9P-string pack→unpack incl. empty string |
| `9p_wire.pack_str_overflow` | Short-cap rejection at prefix + body boundaries |
| `9p_wire.qid_round_trip` | QID pack→unpack + field-bit preservation |
| `9p_wire.header_peek` | Header decode + truncated-header rejection |
| `9p_wire.tversion_round_trip` | Build Tversion → flip type → parse Rversion |
| `9p_wire.tattach_round_trip` | Build Tattach + synthesize Rattach + parse |
| `9p_wire.twalk_round_trip` | 2-component walk + synthesize Rwalk + parse |
| `9p_wire.twalk_zero_names_clone` | Twalk(nwname=0) fid-clone shape |
| `9p_wire.tclunk_round_trip` | Build Tclunk + Rclunk parse |
| `9p_wire.rlerror_parse` | Rlerror decode with ENOENT (ecode=2) |
| `9p_wire.rmsg_size_mismatch_rejected` | `header.size != frame_length` rejected |
| `9p_wire.rmsg_wrong_type_rejected` | Parser refuses wrong opcode |
| `9p_wire.rwalk_count_cap_enforced` | R111 caller-cap-bound on `nwqid > qid_cap` |
| `9p_wire.tlopen_round_trip` | Build Tlopen + synthesize Rlopen + parse (qid + iounit) |
| `9p_wire.tlcreate_round_trip` | Build Tlcreate (with name) + synthesize Rlcreate + parse |
| `9p_wire.tread_round_trip` | Build Tread + synthesize Rread + parse (zero-copy data) |
| `9p_wire.twrite_round_trip` | Build Twrite (with payload) + synthesize Rwrite + parse count |
| `9p_wire.rread_data_cap_enforced` | R111 caller-cap-bound on Rread `count > data_cap` |
| `9p_wire.rread_size_mismatch_rejected` | Strict-equality: header.size != 11 + count rejected |
| `9p_wire.rlopen_vs_rlcreate_type_strict` | Identical body shapes but parser type-strict |
| `9p_wire.tgetattr_round_trip` | Build Tgetattr + synthesize 153-byte Rgetattr + parse statx-shaped record |
| `9p_wire.tsetattr_round_trip` | Build Tsetattr (60-byte body) + synthesize header-only Rsetattr |
| `9p_wire.treaddir_round_trip` | Build Treaddir + synthesize Rreaddir with `.` + `..` dirent stream + parse |
| `9p_wire.dirent_unpack` | Hand-construct one dirent + p9_unpack_dirent zero-copy name |
| `9p_wire.tstatfs_round_trip` | Build Tstatfs + synthesize 60-byte Rstatfs + parse statfs record |
| `9p_wire.tfsync_round_trip` | Build Tfsync (with datasync) + synthesize header-only Rfsync |
| `9p_wire.rreaddir_data_cap_enforced` | R111 caller-cap-bound on Rreaddir `count > data_cap` |
| `9p_wire.tsymlink_round_trip` | Build Tsymlink + synthesize Rsymlink (qid-only) + parse |
| `9p_wire.tmknod_round_trip` | Build Tmknod (mode/major/minor) + synthesize Rmknod (qid-only) + parse |
| `9p_wire.trename_round_trip` | Build Trename + synthesize header-only Rrename |
| `9p_wire.treadlink_round_trip` | Build Treadlink + synthesize Rreadlink (target string) + zero-copy parse |
| `9p_wire.tlink_round_trip` | Build Tlink + synthesize header-only Rlink |
| `9p_wire.tmkdir_round_trip` | Build Tmkdir + synthesize Rmkdir (qid-only) + parse |
| `9p_wire.trenameat_round_trip` | Build Trenameat (old/new dir + names) + synthesize header-only Rrenameat |
| `9p_wire.tunlinkat_round_trip` | Build Tunlinkat + synthesize header-only Runlinkat + flag encoding check |

## Error paths

Every public function returns negative on:

- Caller passed `NULL` for a required out-param.
- Buffer too small (`cap < required_bytes` for pack; `remaining < required_bytes` for unpack).
- Builder body length > 0xFFFFFFFFu (4-GiB cap — fits 9P's `size: u32`).
- Parser: `header.size != frame_length` (mismatched framing).
- Parser: `header.type != expected_type` (wrong opcode).
- Parser: extra trailing bytes after the last body field unpacks (strict equality).
- `p9_pack_str` / `p9_unpack_str`: `slen > 0xFFFFu` (9P-string length cap).
- `p9_parse_rwalk`: `nwqid > qid_cap` (caller-cap-bound) OR `nwqid > P9_MAX_WALK`.

## Performance characteristics

- Per-byte cost is constant (a few stores/shifts per primitive).
- No allocation, no syscalls, no locking — the codec is reentrant.
- A typical Tversion-Rversion handshake is ~25 bytes round-trip; a typical Tattach-Rattach is ~50 bytes; a 16-component Twalk-Rwalk is ~280 bytes Tmsg + ~220 bytes Rmsg.

## Status

| Component | State |
|---|---|
| Header peek + pack/unpack primitives | **Landed (P5-wire)** in `kernel/9p_wire.c` |
| Tversion / Rversion | **Landed (P5-wire)** |
| Tattach / Rattach | **Landed (P5-wire)** |
| Twalk / Rwalk | **Landed (P5-wire)** |
| Tclunk / Rclunk | **Landed (P5-wire)** |
| Tflush (build) / Rflush (parse) | **Landed (#845)** -- `p9_build_tflush` (body `[oldtag:u16]`) + `p9_parse_rflush` (header-only) |
| Rlerror parse | **Landed (P5-wire)** |
| Tlopen / Rlopen | **Landed (P5-wire-io)** |
| Tread / Rread + Twrite / Rwrite | **Landed (P5-wire-io)** |
| Tlcreate / Rlcreate | **Landed (P5-wire-io)** |
| Tgetattr / Rgetattr + Tsetattr / Rsetattr | Phase 5+ (P5-wire-meta) |
| Treaddir / Rreaddir | Phase 5+ (P5-wire-meta) |
| Tstatfs / Rstatfs | Phase 5+ (P5-wire-meta) |
| Tfsync / Rfsync | Phase 5+ (P5-wire-meta) |
| Mutation family (Tunlinkat / Trename / Trenameat / Tsymlink / Tmknod / Tlink / Treadlink / Tmkdir) | Phase 5+ (P5-wire-mutation) |
| Tlock / Rlock + Tgetlock / Rgetlock | Phase 5+ (P5-wire-lock) |
| Txattrwalk / Txattrcreate + family | Phase 5+ (P5-wire-xattr) |
| Stratum extensions (Tsync / Treflink / Tbind / Tunbind / Tfallocate / Tfadvise) | Phase 5+ (P5-wire-stratum-ext) |

## Known caveats / footguns

1. **9P-string out_ptr aliases the input buffer**. `p9_unpack_str` returns `*out_ptr` pointing INTO `in`. Caller MUST NOT free or mutate the input buffer until the string is consumed (copied out or processed). For kernel callers this is typically fine — the parser is called from a fixed message-receive buffer that outlives the parse.
2. **No support for messages larger than 4 GiB** (`size: u32` ceiling). Real-world workloads never approach this; the 9P spec itself bounds it at the same value.
3. **No msize negotiation enforced at the codec layer**. The codec writes/reads whatever the caller passes. msize enforcement (rejecting Tversion/Rversion with `msize > MAX_NEGOTIATED`) is the caller's responsibility — typically the P5-session layer.
4. **Strict body-length equality is intentional**. A message with trailing bytes past the last advertised field is rejected. This defends against future server bugs that emit hidden extra payload (potential covert channel / shape-change masking). Per Stratum's R111 P3 F-10 doctrine.
5. **`p9_build_*` overwrites the entire output buffer through the message length**. Caller is responsible for not aliasing the buffer with any unrelated data the builder might clobber.

## Naming rationale

`p9_*` prefix matches the diod project + Linux v9fs convention (`p9_*` types and helpers). Distinct from `P9_*` constants (uppercase) used for opcodes/sentinels. The full canonical spelling `9p2000.l` is used in the wire-format `version` string and in docs; the C identifier prefix is `p9_` per established convention.

## Spec cross-reference

The codec is "below" `specs/9p_client.tla` in the stack — the spec describes session-level state (tags, fids, outstanding-table) that the codec serves but doesn't model. The spec's `SendIO` / `SendWalk` / `SendClunk` / `ReceiveOp` actions are implemented by P5-session, which composes this codec. See `specs/SPEC-TO-CODE.md` for the action ↔ symbol mapping.

| Spec action | Composes via codec call |
|---|---|
| `OpenSession` | `p9_build_tversion` + `p9_parse_rversion` + `p9_build_tattach` + `p9_parse_rattach` |
| `SendIO` | (P5-wire-io extension; not yet landed) |
| `SendWalk` | `p9_build_twalk` + on Rmsg `p9_parse_rwalk` |
| `SendClunk` | `p9_build_tclunk` + on Rmsg `p9_parse_rclunk` |
| `ReceiveOp` (dispatch on type) | `p9_peek_header` + per-type `p9_parse_*` |

Any Rmsg can replace its expected type with `P9_RLERROR`; the session layer peeks the header type to dispatch.
