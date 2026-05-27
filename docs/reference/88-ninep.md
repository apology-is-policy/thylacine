# 88 — t::ninep (libthyla-rs 9P2000.L codec)

**Status**: Server-side codec landed at U-2h-ninep (commit `*(pending)*`). Client-side counterparts (T-builders + R-parsers) deferred; see *Deferred surfaces* below.

## Purpose

`libthyla_rs::ninep` is the 9P2000.L wire-format codec for native Thylacine programs that **serve** 9P (publish a `/srv/<name>` endpoint and dispatch T-messages from clients). The module provides three slices:

1. **Primitive pack/unpack** — `pack_u8`/`pack_u16`/`pack_u32`/`pack_u64`/`pack_str`/`pack_qid` and the unpack counterparts (`unpack_u8`..`unpack_str`). All little-endian; `unpack_str` returns a zero-copy slice into the caller's buffer.
2. **Header** — `Header` struct + `peek_header` (extracts size + mtype + tag from `buf[0..7]` without consuming the body).
3. **T-message parsers and R-message builders** — `parse_t{version,attach,walk,lopen,read,write,clunk}` consume inbound frames; `build_r{version,attach,walk,lopen,read,write,clunk,lerror}` produce outbound frames with back-patched size headers.

The module is `no_std` + `no_alloc`; every function operates on caller-supplied byte buffers. There is no I/O, no session state, no fid table, and no tag allocation — those live in the server (corvus today; future Plan 9-shaped `/srv` publishers tomorrow).

## Public API

### Constants

```rust
// 9P2000.L message types (the subset corvus serves at v1.0;
// extend as new operations land).
pub const P9_TVERSION: u8 = 100;  pub const P9_RVERSION: u8 = 101;
pub const P9_TAUTH:    u8 = 102;  pub const P9_RAUTH:    u8 = 103;
pub const P9_TATTACH:  u8 = 104;  pub const P9_RATTACH:  u8 = 105;
pub const P9_TWALK:    u8 = 110;  pub const P9_RWALK:    u8 = 111;
pub const P9_TLOPEN:   u8 = 12;   pub const P9_RLOPEN:   u8 = 13;
pub const P9_TREAD:    u8 = 116;  pub const P9_RREAD:    u8 = 117;
pub const P9_TWRITE:   u8 = 118;  pub const P9_RWRITE:   u8 = 119;
pub const P9_TCLUNK:   u8 = 120;  pub const P9_RCLUNK:   u8 = 121;
pub const P9_RLERROR:  u8 = 7;

// QID type bits — corvus uses QTDIR + QTFILE; the kernel adds
// QTSYMLINK / QTAUTH / QTAPPEND / QTEXCL / QTMOUNT / QTTMP in
// `kernel/include/thylacine/9p_wire.h`.
pub const P9_QTDIR:  u8 = 0x80;
pub const P9_QTFILE: u8 = 0x00;

// Sentinels.
pub const P9_NOFID: u32 = 0xFFFF_FFFF;
pub const P9_NOTAG: u16 = 0xFFFF;

// Widths.
pub const P9_HDR_LEN: usize = 7;
pub const P9_QID_LEN: usize = 13;

// Caps (matches kernel + Linux v9fs).
pub const P9_MAX_WALK: usize = 16;
pub const P9_NAME_MAX: usize = 255;

// Errnos used in Rlerror — POSIX-aligned values (compatible with
// musl client-side errno translation).
pub const E_PERM:      u32 = 1;
pub const E_NOENT:     u32 = 2;
pub const E_BADF:      u32 = 9;
pub const E_NOMEM:     u32 = 12;
pub const E_FAULT:     u32 = 14;
pub const E_EXIST:     u32 = 17;
pub const E_NOTDIR:    u32 = 20;
pub const E_ISDIR:     u32 = 21;
pub const E_INVAL:     u32 = 22;
pub const E_OPNOTSUPP: u32 = 95;
pub const E_PROTO:     u32 = 71;
pub const E_NOSYS:     u32 = 38;
```

### Types

```rust
#[derive(Copy, Clone, Default, Debug)]
pub struct Qid {
    pub kind: u8,    // P9_QT*
    pub version: u32,
    pub path: u64,
}

#[derive(Copy, Clone, Debug)]
pub struct Header {
    pub size: u32,   // total message length INCLUDING size field
    pub mtype: u8,
    pub tag: u16,
}

// Tmsg parsed-args structs (zero-copy references where applicable):
pub struct TversionArgs<'a> { pub msize: u32, pub version: &'a [u8] }
pub struct TattachArgs<'a>  { pub fid: u32, pub afid: u32,
                              pub uname: &'a [u8], pub aname: &'a [u8],
                              pub n_uname: u32 }
pub struct TwalkArgs<'a>    { pub fid: u32, pub newfid: u32, pub nwname: u16,
                              pub names: [&'a [u8]; P9_MAX_WALK] }
pub struct TlopenArgs       { pub fid: u32, pub flags: u32 }
pub struct TreadArgs        { pub fid: u32, pub offset: u64, pub count: u32 }
pub struct TwriteArgs<'a>   { pub fid: u32, pub offset: u64, pub count: u32,
                              pub data: &'a [u8] }
pub struct TclunkArgs       { pub fid: u32 }
```

### Functions

```rust
// Primitive packers (write at out[off..], return new offset).
pub fn pack_u8 (out: &mut [u8], off: usize, v: u8 ) -> Result<usize, ()>;
pub fn pack_u16(out: &mut [u8], off: usize, v: u16) -> Result<usize, ()>;
pub fn pack_u32(out: &mut [u8], off: usize, v: u32) -> Result<usize, ()>;
pub fn pack_u64(out: &mut [u8], off: usize, v: u64) -> Result<usize, ()>;
pub fn pack_str(out: &mut [u8], off: usize, s: &[u8]) -> Result<usize, ()>;
pub fn pack_qid(out: &mut [u8], off: usize, q: &Qid) -> Result<usize, ()>;

// Primitive unpackers (return parsed value + new offset).
pub fn unpack_u8 (buf: &[u8], off: usize) -> Result<(u8 , usize), ()>;
pub fn unpack_u16(buf: &[u8], off: usize) -> Result<(u16, usize), ()>;
pub fn unpack_u32(buf: &[u8], off: usize) -> Result<(u32, usize), ()>;
pub fn unpack_u64(buf: &[u8], off: usize) -> Result<(u64, usize), ()>;
pub fn unpack_str<'a>(buf: &'a [u8], off: usize) -> Result<(&'a [u8], usize), ()>;

// Header peek (reads buf[0..7]).
pub fn peek_header(buf: &[u8]) -> Result<Header, ()>;

// T-message parsers — caller passes the FULL framed Tmsg (header + body);
// parser starts at offset P9_HDR_LEN.
pub fn parse_tversion(buf: &[u8]) -> Result<TversionArgs<'_>, ()>;
pub fn parse_tattach (buf: &[u8]) -> Result<TattachArgs<'_>,  ()>;
pub fn parse_twalk   (buf: &[u8]) -> Result<TwalkArgs<'_>,    ()>;
pub fn parse_tlopen  (buf: &[u8]) -> Result<TlopenArgs,        ()>;
pub fn parse_tread   (buf: &[u8]) -> Result<TreadArgs,         ()>;
pub fn parse_twrite  (buf: &[u8]) -> Result<TwriteArgs<'_>,   ()>;
pub fn parse_tclunk  (buf: &[u8]) -> Result<TclunkArgs,        ()>;

// R-message builders — write a full framed Rmsg into `out`; return
// bytes written. The header size field is back-patched after the body.
pub fn build_rversion(out: &mut [u8], tag: u16, msize: u32, version: &[u8]) -> Result<usize, ()>;
pub fn build_rattach (out: &mut [u8], tag: u16, qid: &Qid)                    -> Result<usize, ()>;
pub fn build_rwalk   (out: &mut [u8], tag: u16, qids: &[Qid])                 -> Result<usize, ()>;
pub fn build_rlopen  (out: &mut [u8], tag: u16, qid: &Qid, iounit: u32)        -> Result<usize, ()>;
pub fn build_rread   (out: &mut [u8], tag: u16, data: &[u8])                   -> Result<usize, ()>;
pub fn build_rwrite  (out: &mut [u8], tag: u16, count: u32)                     -> Result<usize, ()>;
pub fn build_rclunk  (out: &mut [u8], tag: u16)                                  -> Result<usize, ()>;
pub fn build_rlerror (out: &mut [u8], tag: u16, ecode: u32)                     -> Result<usize, ()>;
```

## Implementation

`usr/lib/libthyla-rs/src/ninep.rs` (~430 LOC, originally `usr/corvus/src/p9.rs` at corvus commit `4bf689c` — lifted byte-identical at U-2h-ninep with the module header rewritten for the library context).

### Wire conventions

Mirror of `kernel/include/thylacine/9p_wire.h`. Canonical reference: `stratum/v2/docs/reference/20-9p.md`.

- All multi-byte integers are **little-endian**.
- 9P-strings are length-prefixed `u16`; NOT NUL-terminated. Strings are zero-copy slices into the caller's buffer.
- Common header is **7 bytes**: `[size: u32][type: u8][tag: u16]`. `size` is the total message length INCLUDING the size field itself.
- A qid is **13 bytes** on the wire: `[type: u8][version: u32][path: u64]`.

### Error convention

Every function returns `Result<usize, ()>` — `Ok(n)` is bytes consumed/produced; `Err(())` is "buffer too small / message malformed / unexpected discriminant". The caller (server dispatcher) short-circuits on the first `Err` and replies with `Rlerror(EPROTO)`. The error type is the unit because every failure mode reduces to "wire violation"; downstream `Rlerror` mapping distinguishes `EPROTO` from `EBADF`/`EISDIR`/etc. via the caller's own dispatch logic.

A future promotion to `t::err::Error` is a v1.x cleanup item — gated on a non-corvus consumer that needs richer error context.

### Back-patched header size

`build_r*` functions write the common header with a placeholder size of `0`, then write the body, then call the private `patch_header_size` to back-patch the total length at `out[0..4]` before returning. This avoids needing to know the body length up-front (the body may include variable-length strings or qid arrays).

### Twalk parsing

The variable-length wname array is bounded twice: the parser rejects `nwname > P9_MAX_WALK` (16, per the kernel wire header) and rejects any individual name longer than `P9_NAME_MAX` (255). The names are returned as a fixed-size `[&[u8]; P9_MAX_WALK]` array with the first `nwname` slots populated; subsequent slots are empty slices. No dynamic allocation.

## Data structures

```rust
// Qid: 13 bytes on the wire; in-memory shape is the same fields.
//   bytes 0:        kind (u8)
//   bytes 1..5:     version (u32 LE)
//   bytes 5..13:    path (u64 LE)
//
// Header: 7 bytes on the wire.
//   bytes 0..4:     size (u32 LE) — TOTAL message length
//   bytes 4:        mtype (u8)
//   bytes 5..7:     tag (u16 LE)
```

No `_Static_assert` / `const _ assertion` on size — the in-memory `Qid` and `Header` structs are Rust types, not `#[repr(C)]` mirrors. The wire layout is pinned by the pack/unpack code that reads them.

## Spec cross-reference

No formal `specs/*.tla` module for ninep. Per the **spec-to-code FULLY suspended** broadening (CLAUDE.md, 2026-05-23 direction), the codec's invariants are validated by prose reasoning + the audit + the runtime test suite. The relevant invariants:

- **Wire format conformance**: every `pack_X` followed by the corresponding `unpack_X` is the identity (over byte sequences). Validated by alloc-smoke's primitive round-trip.
- **No buffer over-read**: every unpack short-circuits to `Err(())` if the buffer is shorter than the requested width. Validated by alloc-smoke's short-buffer rejects.
- **No buffer over-write**: every pack short-circuits to `Err(())` if `out[off..]` is shorter than the value being packed. Validated by the pack-into-too-small-buffer reject path (not explicitly tested in alloc-smoke; the code path is `if out.len() < off + N { return Err(()); }`).
- **Twalk nwname bound**: `parse_twalk` rejects `nwname > P9_MAX_WALK`. Validated by inspection — the check is at line 250 in `ninep.rs`.
- **9P session invariants** (I-10 tag uniqueness, I-11 fid stability per-session) are **NOT** codec invariants — they're server-state invariants enforced by the dispatcher above the codec. corvus's `main.rs` is the canonical example.

## Tests

`usr/alloc-smoke/src/main.rs` ninep section (~135 LOC, added U-2h-ninep):

1. Primitive pack/unpack round-trip: every integer width + str + qid.
2. `build_rversion` + `peek_header` cycle.
3. Hand-built Tversion frame + `parse_tversion` field check.
4. Hand-built Twalk frame with 3 names ("a", "bb", "ccc") + `parse_twalk` field + names check.
5. `build_rwalk` with 3 qids + `peek_header` + read-back of `nwqid`.
6. `build_rlerror` + read-back of ecode.
7. Short-buffer reject: `peek_header(&[u8; 3])` → `Err(())`.
8. Short-buffer reject: `parse_twalk` on a buffer with a truncated nwname field → `Err(())`.

Runs at boot via joey-spawned `/alloc-smoke`. Pass shows `alloc-smoke: NineP codec OK` in the log.

## Error paths

Every codec function returns `Err(())` on:
- Buffer underrun (read past `buf.len()`).
- Buffer overrun (write past `out.len()`).
- Per-message size overflow (a value that would not fit in the u32 size field — practically unreachable at v1.0 msize ≤ 8 MiB).
- Twalk nwname > P9_MAX_WALK.
- Twalk per-name length > P9_NAME_MAX.
- pack_str: input string longer than `u16::MAX`.

The server dispatcher in `usr/corvus/src/main.rs` handles `Err(())` by emitting `Rlerror(P9_E_PROTO)` and continuing the connection (or tearing it down at the discretion of the dispatcher; corvus tears down on persistent protocol violations).

## Performance characteristics

The codec is a thin layer of byte shuffling. No allocations; no syscalls; bounded by buffer-copy speed. Negligible overhead in the corvus benchmark; not a measurable component of the 9P round-trip latency budget (the budget is dominated by SrvConn ring synchronization).

## Status

- **Server-side codec**: LANDED at U-2h-ninep. corvus's previous private `p9` module is gone (file `usr/corvus/src/p9.rs` deleted); corvus consumes `libthyla_rs::ninep` via `use libthyla_rs::ninep as p9;` — every existing `p9::*` call site continues to resolve.
- **Client-side codec** (T-builders + R-parsers): NOT present at v1.0. Native programs reach 9P services via the kernel client (mounted trees + `SYS_SRV_CONNECT`); userspace does not speak the protocol directly. Adding the client side is mechanical mirror of the existing code (~400 LOC); held for when a real consumer surfaces — the same deferral pattern as `std::thread::spawn(FnOnce)` in U-2g.
- **Extended message types** beyond corvus's subset: deferred. The kernel wire header (`kernel/include/thylacine/9p_wire.h`) declares the full 9P2000.L surface (Tlcreate, Tsymlink, Tmknod, Trename, Treadlink, Tgetattr, Tsetattr, Tlock, Tlink, Tmkdir, Trenameat, Tunlinkat, Tstatfs, Txattrwalk, Txattrcreate, Treaddir, Tfsync); the codec extends row-by-row as new servers need them.

## Naming rationale

`ninep` is the spell-it-out form of "9p". The kernel uses `p9_*` C identifiers (Linux v9fs convention); Rust's identifier rules forbid digit-leading names, so we spell out. The thematic register is mild here — Plan 9 is the lineage, not a marsupial; we don't rename `9P` to a thylacine word because that would obscure the wire-protocol terminology that the kernel + Linux v9fs + Stratum all share.

## Known caveats / footguns

- **Tmsg parsers don't validate the size field against the type-specific expected length.** A caller that passes `&buf[..size]` (the size from `peek_header`) gets correct bounds; a caller that passes `&buf[..buf.len()]` may parse a Tmsg + trailing garbage. The kernel parser does this same trust-the-dispatcher pattern (`kernel/9p_client.c`).
- **`unpack_str` returns `&[u8]`, not `&str`.** The wire format permits non-UTF-8 names. Callers that want UTF-8 must `core::str::from_utf8(slice)` themselves.
- **Back-patched size is u32**, capping single-message size at ~4 GiB. The kernel imposes a tighter bound via msize (8 MiB at v1.0); the codec doesn't enforce msize — it's the server's job.
- **`P9_NOTAG` is the only sentinel value reserved at the wire layer.** Tag `0` is a valid tag; tag `0xFFFF` is reserved for Tversion / Rversion exchange.
- **No client-side primitives**: a server that needs to make outgoing 9P calls (e.g., a stacked dispatcher) cannot build T-messages with this codec. That's the deferred surface (see Status).

## References

- Kernel header: `kernel/include/thylacine/9p_wire.h`
- Kernel 9P client implementation (the canonical wire-format reference): `kernel/9p_client.c`
- 9P2000.L spec (as adopted by Stratum): `stratum/v2/docs/reference/20-9p.md`
- corvus 9P server (the canonical codec consumer): `usr/corvus/src/main.rs` (dispatcher + per-connection fid table + the verb handlers)
- Plan 9 9P documentation: https://9p.io/sys/man/5/INDEX.html
- Linux v9fs documentation: https://www.kernel.org/doc/Documentation/filesystems/9p.txt
