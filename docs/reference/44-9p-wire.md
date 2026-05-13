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

// Tmsg builders (P5-wire subset).
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

// Rmsg parsers (P5-wire subset).
int p9_parse_rversion(const u8 *in, size_t len, u16 *tag, u32 *msize,
                      const u8 **version, u16 *version_len);
int p9_parse_rattach (const u8 *in, size_t len, u16 *tag, struct p9_qid *qid);
int p9_parse_rwalk   (const u8 *in, size_t len, u16 *tag,
                      u16 *nwqid, struct p9_qid *qids, size_t qid_cap);
int p9_parse_rclunk  (const u8 *in, size_t len, u16 *tag);
int p9_parse_rlerror (const u8 *in, size_t len, u16 *tag, u32 *ecode);
```

### Error convention

All functions return either:
- a **non-negative** count (bytes written/consumed, OR `0` for parsers indicating success), OR
- a **negative** value on error.

The caller short-circuits on the first negative return. Error categories are coarse (buffer-too-small / malformed / wrong-type); no errno-style separate channel.

### Strict-equality framing

All parsers enforce `header.size == frame_length` AND `body offset == frame_length` after the last field unpacks. Truncated frames are rejected; extra trailing bytes are rejected. Mirrors Stratum's `<libstratum-9p>` R111 P3 F-10 doctrine (`stratum/v2/docs/reference/23-9p_client.md` §"Five trust boundaries").

### R111 caller-cap-bound discipline

For server-supplied counts that get written into caller buffers, the bound is enforced BEFORE the write. In P5-wire's surface, this applies to:

- `p9_parse_rwalk`: the server-supplied `nwqid` is bounded against the caller's `qid_cap` BEFORE any qid is unpacked into the buffer. An out-of-spec `Rwalk(nwqid=99)` on a 2-qid caller buffer is rejected; the buffer is not touched.

The `p9_unpack_str` function returns a pointer INTO the input buffer (zero-copy), so caller-cap-bound is the caller's responsibility there: the caller checks `out_len <= ENFORCED_MAX` before consuming `out_ptr[0..out_len)`.

## Wire-format crib

Per `https://github.com/chaos/diod/blob/master/protocol.md` + `stratum/v2/docs/reference/20-9p.md`:

| Construct | Layout |
|---|---|
| Common header (every msg) | `[size: u32 LE][type: u8][tag: u16 LE]` (7 bytes) |
| 9P-string | `[slen: u16 LE][bytes: u8 * slen]` (NOT NUL-terminated) |
| QID | `[type: u8][version: u32 LE][path: u64 LE]` (13 bytes) |

Per-message bodies (for the P5-wire subset):

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

All integers little-endian (matches Thylacine's AArch64 host endianness; encoding is still explicit byte-shift to remain portable).

## Compile-time invariants

`_Static_assert` in `kernel/9p_wire.c`:

- `P9_HDR_LEN == 7` (header byte count).
- `P9_QID_LEN == 13` (qid byte count).
- `P9_NOFID == 0xFFFFFFFFu` (NOFID sentinel).
- `P9_NOTAG == 0xFFFFu` (NOTAG sentinel).
- `P9_TVERSION == 100`; `P9_RVERSION == P9_TVERSION + 1`; same for ATTACH/WALK/CLUNK pairs.
- `P9_RLERROR == 7`.
- `sizeof(struct p9_qid) >= P9_QID_LEN` (in-memory shape doesn't shrink below the wire shape).

## Implementation

| File | Purpose |
|---|---|
| `kernel/include/thylacine/9p_wire.h` | Public API + constants + `struct p9_qid` |
| `kernel/9p_wire.c` | Byte-level codec; static `write_header` + `validate_rmsg_header` helpers |
| `kernel/test/test_9p_wire.c` | 15 unit tests (round-trip + malformed-input rejection) |

The codec is purely procedural — no callbacks, no state, no allocation. Every function is freestanding.

## Tests

15 tests in `kernel/test/test_9p_wire.c`:

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
| Rlerror parse | **Landed (P5-wire)** |
| Tlopen / Rlopen | Phase 5+ (P5-wire-io) |
| Tread / Rread + Twrite / Rwrite | Phase 5+ (P5-wire-io) |
| Tlcreate / Rlcreate | Phase 5+ (P5-wire-io) |
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
