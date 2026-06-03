// t::ninep -- 9P2000.L wire-format codec for native Thylacine servers.
//
// At v1.0 the codec is server-side only: T-message parsers consume
// inbound frames; R-message builders produce outbound frames. The
// client-side counterparts (T-builders + R-parsers) are NOT here at
// v1.0 -- native programs reach 9P services via the kernel 9P client
// (mounted trees via SYS_WALK_OPEN; client SrvConn handshake via
// SYS_SRV_CONNECT), not by speaking the protocol from userspace. The
// client side lifts when a real consumer surfaces (compare U-2g's
// deferral of std::thread::spawn(FnOnce) for the same reason).
//
// Lifted from corvus's private `p9` module (P5-corvus-srv-impl-b3b at
// 4bf689c) as U-2h-ninep -- any future native 9P server (a successor
// to corvus, a Plan 9-shaped /srv publisher, a slate-style TUI daemon)
// reaches for the same codec.
//
// Wire conventions (canonical reference: `kernel/include/thylacine/9p_wire.h`;
// matches `stratum/v2/docs/reference/20-9p.md`):
//   - All multi-byte integers are LITTLE-ENDIAN.
//   - 9P-strings are length-prefixed (u16 LE), NOT NUL-terminated. The
//     unpack returns a zero-copy slice into the caller's buffer.
//   - Common header is 7 bytes: [size: u32][type: u8][tag: u16]. `size`
//     is the TOTAL message length INCLUDING the size field itself.
//   - A qid is 13 bytes on the wire: [type: u8][version: u32][path: u64].
//
// Error convention: every function returns a Result<usize, ()> -- Ok(n)
// is bytes consumed/produced; Err(()) is "buffer too small / message
// malformed / unexpected discriminant". The caller short-circuits on
// the first Err and replies with Rlerror. The error type is the unit
// because every failure mode is "wire violation"; downstream Rlerror
// mapping distinguishes EPROTO from EBADF / EISDIR / etc. via the
// caller's own logic. Promoting to t::err::Error is a v1.x cleanup
// when a non-corvus consumer surfaces that needs richer error context.
//
// What this module does NOT do:
//   - No fid table -- the server (corvus, future others) owns the
//     per-connection fid lifecycle (I-11).
//   - No tag allocation -- tags arrive in inbound Tmsgs and echo in
//     outbound Rmsgs; uniqueness across a session (I-10) is the
//     client's invariant, not the server's.
//   - No I/O -- pack/parse work on caller-supplied byte buffers; the
//     transport (SrvConn byte-mode pipe, mounted Spoor, future TCP)
//     is the caller's concern.
//   - No session state -- msize is negotiated once at Tversion and
//     stored by the caller; subsequent buffers are sized to msize.

// =============================================================================
// 9P2000.L message types (the subset corvus serves; matches the kernel
// wire header). Extend as new operations land.
// =============================================================================

pub const P9_TVERSION: u8 = 100;
pub const P9_RVERSION: u8 = 101;
pub const P9_TAUTH: u8    = 102;
pub const P9_RAUTH: u8    = 103;
pub const P9_TATTACH: u8  = 104;
pub const P9_RATTACH: u8  = 105;
pub const P9_TWALK: u8    = 110;
pub const P9_RWALK: u8    = 111;
pub const P9_TLOPEN: u8   = 12;
pub const P9_RLOPEN: u8   = 13;
pub const P9_TREAD: u8    = 116;
pub const P9_RREAD: u8    = 117;
pub const P9_TWRITE: u8   = 118;
pub const P9_RWRITE: u8   = 119;
pub const P9_TCLUNK: u8   = 120;
pub const P9_RCLUNK: u8   = 121;
pub const P9_TGETATTR: u8 = 24;
pub const P9_RGETATTR: u8 = 25;
pub const P9_RLERROR: u8  = 7;

// QID type bits (the v1.0 surface -- corvus's namespace is one dir +
// one file, no symlinks/temp).
pub const P9_QTDIR: u8  = 0x80;
pub const P9_QTFILE: u8 = 0x00;

// Sentinels.
pub const P9_NOFID: u32 = 0xFFFF_FFFF;
pub const P9_NOTAG: u16 = 0xFFFF;

// Header + qid widths (load-bearing -- the wire codec depends on them
// AND they're _Static_assert-pinned in the kernel header).
pub const P9_HDR_LEN: usize = 7;
pub const P9_QID_LEN: usize = 13;

// Per-Twalk and per-name caps (matches kernel/include/thylacine/9p_wire.h
// + Linux v9fs convention). corvus uses these to reject malformed Twalks
// at parse time without dynamic allocation.
pub const P9_MAX_WALK: usize = 16;
pub const P9_NAME_MAX: usize = 255;

// Selected Linux errnos used in Rlerror responses. Match POSIX values
// (and Thylacine's T_E_* registry per docs/ERRORS.md -- the Rlerror
// numeric is what musl + glibc translate to errno on the client).
pub const E_PERM: u32     = 1;
pub const E_NOENT: u32    = 2;
pub const E_BADF: u32     = 9;
pub const E_NOMEM: u32    = 12;
pub const E_FAULT: u32    = 14;
pub const E_EXIST: u32    = 17;
pub const E_NOTDIR: u32   = 20;
pub const E_ISDIR: u32    = 21;
pub const E_INVAL: u32    = 22;
pub const E_OPNOTSUPP: u32 = 95;
pub const E_PROTO: u32    = 71;
pub const E_NOSYS: u32    = 38;

// =============================================================================
// Qid -- in-memory shape of the 13-byte wire qid.
// =============================================================================

/// In-memory shape of a 9P qid. Pack via [`pack_qid`]; the wire layout
/// is `[kind: u8][version: u32][path: u64]` = 13 bytes.
#[derive(Copy, Clone, Default, Debug)]
pub struct Qid {
    pub kind: u8,    // P9_QT*
    pub version: u32,
    pub path: u64,
}

// =============================================================================
// Primitive unpackers -- zero-copy slice into the caller's buffer.
// =============================================================================

pub fn unpack_u8(buf: &[u8], off: usize) -> Result<(u8, usize), ()> {
    if buf.len() < off + 1 { return Err(()); }
    Ok((buf[off], off + 1))
}

pub fn unpack_u16(buf: &[u8], off: usize) -> Result<(u16, usize), ()> {
    if buf.len() < off + 2 { return Err(()); }
    let v = (buf[off] as u16) | ((buf[off + 1] as u16) << 8);
    Ok((v, off + 2))
}

pub fn unpack_u32(buf: &[u8], off: usize) -> Result<(u32, usize), ()> {
    if buf.len() < off + 4 { return Err(()); }
    let v = (buf[off] as u32)
          | ((buf[off + 1] as u32) << 8)
          | ((buf[off + 2] as u32) << 16)
          | ((buf[off + 3] as u32) << 24);
    Ok((v, off + 4))
}

pub fn unpack_u64(buf: &[u8], off: usize) -> Result<(u64, usize), ()> {
    if buf.len() < off + 8 { return Err(()); }
    let mut v: u64 = 0;
    for i in 0..8 {
        v |= (buf[off + i] as u64) << (8 * i);
    }
    Ok((v, off + 8))
}

/// Zero-copy string slice into `buf`. Caller MUST NOT retain it past
/// the buffer's lifetime. Returns `(slice, new_offset)`.
pub fn unpack_str<'a>(buf: &'a [u8], off: usize) -> Result<(&'a [u8], usize), ()> {
    let (len, p) = unpack_u16(buf, off)?;
    let end = p + len as usize;
    if buf.len() < end { return Err(()); }
    Ok((&buf[p..end], end))
}

// =============================================================================
// Primitive packers -- write at `out[off..]`, return new offset.
// =============================================================================

pub fn pack_u8(out: &mut [u8], off: usize, v: u8) -> Result<usize, ()> {
    if out.len() < off + 1 { return Err(()); }
    out[off] = v;
    Ok(off + 1)
}

pub fn pack_u16(out: &mut [u8], off: usize, v: u16) -> Result<usize, ()> {
    if out.len() < off + 2 { return Err(()); }
    out[off]     = (v & 0xff) as u8;
    out[off + 1] = ((v >> 8) & 0xff) as u8;
    Ok(off + 2)
}

pub fn pack_u32(out: &mut [u8], off: usize, v: u32) -> Result<usize, ()> {
    if out.len() < off + 4 { return Err(()); }
    out[off]     = (v & 0xff) as u8;
    out[off + 1] = ((v >> 8) & 0xff) as u8;
    out[off + 2] = ((v >> 16) & 0xff) as u8;
    out[off + 3] = ((v >> 24) & 0xff) as u8;
    Ok(off + 4)
}

pub fn pack_u64(out: &mut [u8], off: usize, v: u64) -> Result<usize, ()> {
    if out.len() < off + 8 { return Err(()); }
    for i in 0..8 {
        out[off + i] = ((v >> (8 * i)) & 0xff) as u8;
    }
    Ok(off + 8)
}

pub fn pack_str(out: &mut [u8], off: usize, s: &[u8]) -> Result<usize, ()> {
    if s.len() > u16::MAX as usize { return Err(()); }
    let off = pack_u16(out, off, s.len() as u16)?;
    if out.len() < off + s.len() { return Err(()); }
    out[off..off + s.len()].copy_from_slice(s);
    Ok(off + s.len())
}

pub fn pack_qid(out: &mut [u8], off: usize, q: &Qid) -> Result<usize, ()> {
    let off = pack_u8(out, off, q.kind)?;
    let off = pack_u32(out, off, q.version)?;
    pack_u64(out, off, q.path)
}

// =============================================================================
// Header peek -- extract size + type + tag without consuming the body.
// =============================================================================

/// Parsed common header. `size` is the TOTAL message length INCLUDING
/// the size field itself.
#[derive(Copy, Clone, Debug)]
pub struct Header {
    pub size: u32,
    pub mtype: u8,
    pub tag: u16,
}

/// Read the 7-byte common header from `buf[0..7]`.
pub fn peek_header(buf: &[u8]) -> Result<Header, ()> {
    if buf.len() < P9_HDR_LEN { return Err(()); }
    let (size, p) = unpack_u32(buf, 0)?;
    let (mtype, p) = unpack_u8(buf, p)?;
    let (tag, _) = unpack_u16(buf, p)?;
    Ok(Header { size, mtype, tag })
}

// =============================================================================
// Tmsg parsers -- server-side input. Each takes the FULL framed Tmsg
// (header + body); returns the parsed fields.
//
// Convention: the size + type are already validated by the dispatcher
// (peek_header); the parser starts at offset P9_HDR_LEN.
// =============================================================================

#[derive(Copy, Clone)]
pub struct TversionArgs<'a> {
    pub msize: u32,
    pub version: &'a [u8],
}

pub fn parse_tversion(buf: &[u8]) -> Result<TversionArgs<'_>, ()> {
    let (msize, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (version, _) = unpack_str(buf, p)?;
    Ok(TversionArgs { msize, version })
}

#[derive(Copy, Clone)]
pub struct TattachArgs<'a> {
    pub fid: u32,
    pub afid: u32,
    pub uname: &'a [u8],
    pub aname: &'a [u8],
    pub n_uname: u32,
}

pub fn parse_tattach(buf: &[u8]) -> Result<TattachArgs<'_>, ()> {
    let (fid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (afid, p) = unpack_u32(buf, p)?;
    let (uname, p) = unpack_str(buf, p)?;
    let (aname, p) = unpack_str(buf, p)?;
    let (n_uname, _) = unpack_u32(buf, p)?;
    Ok(TattachArgs { fid, afid, uname, aname, n_uname })
}

/// Twalk -- variable-length wname array. Names are zero-copy slices
/// into the body; the parser rejects nwname > P9_MAX_WALK and per-name
/// length > P9_NAME_MAX, so the caller never sees a malformed frame.
pub struct TwalkArgs<'a> {
    pub fid: u32,
    pub newfid: u32,
    pub nwname: u16,
    pub names: [&'a [u8]; P9_MAX_WALK],
}

pub fn parse_twalk(buf: &[u8]) -> Result<TwalkArgs<'_>, ()> {
    let (fid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (newfid, p) = unpack_u32(buf, p)?;
    let (nwname, mut p) = unpack_u16(buf, p)?;
    if nwname as usize > P9_MAX_WALK { return Err(()); }
    let mut names: [&[u8]; P9_MAX_WALK] = [&[][..]; P9_MAX_WALK];
    for i in 0..(nwname as usize) {
        let (name, np) = unpack_str(buf, p)?;
        if name.len() > P9_NAME_MAX { return Err(()); }
        names[i] = name;
        p = np;
    }
    Ok(TwalkArgs { fid, newfid, nwname, names })
}

#[derive(Copy, Clone)]
pub struct TlopenArgs {
    pub fid: u32,
    pub flags: u32,
}

pub fn parse_tlopen(buf: &[u8]) -> Result<TlopenArgs, ()> {
    let (fid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (flags, _) = unpack_u32(buf, p)?;
    Ok(TlopenArgs { fid, flags })
}

#[derive(Copy, Clone)]
pub struct TreadArgs {
    pub fid: u32,
    pub offset: u64,
    pub count: u32,
}

pub fn parse_tread(buf: &[u8]) -> Result<TreadArgs, ()> {
    let (fid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (offset, p) = unpack_u64(buf, p)?;
    let (count, _) = unpack_u32(buf, p)?;
    Ok(TreadArgs { fid, offset, count })
}

#[derive(Copy, Clone)]
pub struct TwriteArgs<'a> {
    pub fid: u32,
    pub offset: u64,
    pub count: u32,
    pub data: &'a [u8],
}

pub fn parse_twrite(buf: &[u8]) -> Result<TwriteArgs<'_>, ()> {
    let (fid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (offset, p) = unpack_u64(buf, p)?;
    let (count, p) = unpack_u32(buf, p)?;
    let end = p + count as usize;
    if buf.len() < end { return Err(()); }
    Ok(TwriteArgs { fid, offset, count, data: &buf[p..end] })
}

#[derive(Copy, Clone)]
pub struct TclunkArgs {
    pub fid: u32,
}

pub fn parse_tclunk(buf: &[u8]) -> Result<TclunkArgs, ()> {
    let (fid, _) = unpack_u32(buf, P9_HDR_LEN)?;
    Ok(TclunkArgs { fid })
}

// =============================================================================
// Rmsg builders -- server-side output. Each writes a full framed Rmsg
// into `out`; returns the byte count written. The `size` field in the
// header is back-patched after the body length is known.
//
// Caller passes a buffer sized to hold the largest Rmsg the server
// emits. For corvus that's bounded by msize (negotiated at Tversion).
// =============================================================================

// Write the common header with a placeholder size of 0, returning the
// offset past the header. After the body is complete, call
// patch_header_size(out, total_len) to back-patch.
fn build_header(out: &mut [u8], mtype: u8, tag: u16) -> Result<usize, ()> {
    let p = pack_u32(out, 0, 0)?; // size placeholder
    let p = pack_u8(out, p, mtype)?;
    pack_u16(out, p, tag)
}

fn patch_header_size(out: &mut [u8], total_len: usize) -> Result<(), ()> {
    if out.len() < 4 || total_len > u32::MAX as usize { return Err(()); }
    let v = total_len as u32;
    out[0] = (v & 0xff) as u8;
    out[1] = ((v >> 8) & 0xff) as u8;
    out[2] = ((v >> 16) & 0xff) as u8;
    out[3] = ((v >> 24) & 0xff) as u8;
    Ok(())
}

pub fn build_rversion(out: &mut [u8], tag: u16, msize: u32, version: &[u8]) -> Result<usize, ()> {
    let p = build_header(out, P9_RVERSION, tag)?;
    let p = pack_u32(out, p, msize)?;
    let p = pack_str(out, p, version)?;
    patch_header_size(out, p)?;
    Ok(p)
}

pub fn build_rattach(out: &mut [u8], tag: u16, qid: &Qid) -> Result<usize, ()> {
    let p = build_header(out, P9_RATTACH, tag)?;
    let p = pack_qid(out, p, qid)?;
    patch_header_size(out, p)?;
    Ok(p)
}

/// Write `nwqid` qids from `qids[0..nwqid]`. Per the spec, `nwqid` must
/// equal the count of components actually walked; if all `nwname`
/// components walked successfully, `nwqid == nwname` (and the last qid
/// is the destination). Partial walk on a not-found path returns the
/// prefix that succeeded; corvus at v1.0 either walks completely or
/// fails the whole Twalk with Rlerror(ENOENT).
pub fn build_rwalk(out: &mut [u8], tag: u16, qids: &[Qid]) -> Result<usize, ()> {
    if qids.len() > P9_MAX_WALK { return Err(()); }
    let p = build_header(out, P9_RWALK, tag)?;
    let mut p = pack_u16(out, p, qids.len() as u16)?;
    for q in qids {
        p = pack_qid(out, p, q)?;
    }
    patch_header_size(out, p)?;
    Ok(p)
}

pub fn build_rlopen(out: &mut [u8], tag: u16, qid: &Qid, iounit: u32) -> Result<usize, ()> {
    let p = build_header(out, P9_RLOPEN, tag)?;
    let p = pack_qid(out, p, qid)?;
    let p = pack_u32(out, p, iounit)?;
    patch_header_size(out, p)?;
    Ok(p)
}

/// Write `[count: u32][data: u8 * count]`. `data` is COPIED into the
/// output buffer. Caller bounds `data.len()` by negotiated `msize -
/// P9_HDR_LEN - 4` (the Rread header + count overhead).
pub fn build_rread(out: &mut [u8], tag: u16, data: &[u8]) -> Result<usize, ()> {
    if data.len() > u32::MAX as usize { return Err(()); }
    let p = build_header(out, P9_RREAD, tag)?;
    let p = pack_u32(out, p, data.len() as u32)?;
    if out.len() < p + data.len() { return Err(()); }
    out[p..p + data.len()].copy_from_slice(data);
    let total = p + data.len();
    patch_header_size(out, total)?;
    Ok(total)
}

pub fn build_rwrite(out: &mut [u8], tag: u16, count: u32) -> Result<usize, ()> {
    let p = build_header(out, P9_RWRITE, tag)?;
    let p = pack_u32(out, p, count)?;
    patch_header_size(out, p)?;
    Ok(p)
}

pub fn build_rclunk(out: &mut [u8], tag: u16) -> Result<usize, ()> {
    let p = build_header(out, P9_RCLUNK, tag)?;
    patch_header_size(out, p)?;
    Ok(p)
}

pub fn build_rlerror(out: &mut [u8], tag: u16, ecode: u32) -> Result<usize, ()> {
    let p = build_header(out, P9_RLERROR, tag)?;
    let p = pack_u32(out, p, ecode)?;
    patch_header_size(out, p)?;
    Ok(p)
}

// Tgetattr request-mask bits (Linux v9fs STATX_*). The kernel's dev9p_stat_native
// reads mode/uid/gid only when their `valid` bit is set; a 9P-mode service (the
// stalk-3b-β open=connect target) MUST fill the security trio so the kernel's
// per-component X-search (the A-3 dev9p enforcement) does not fail-closed.
pub const P9_GETATTR_MODE: u64 = 0x1;
pub const P9_GETATTR_NLINK: u64 = 0x2;
pub const P9_GETATTR_UID: u64 = 0x4;
pub const P9_GETATTR_GID: u64 = 0x8;

/// parse_tgetattr -- extract the fid from a Tgetattr. Body: `[fid: u32]
/// [request_mask: u64]`; the request_mask is a hint a minimal server may ignore.
pub fn parse_tgetattr(buf: &[u8]) -> Result<u32, ()> {
    let (fid, _) = unpack_u32(buf, P9_HDR_LEN)?;
    Ok(fid)
}

/// build_rgetattr -- the full 9P2000.L Rgetattr (153-byte body; the kernel's
/// `p9_parse_rgetattr` enforces exact body length). `valid` is the mask of
/// meaningfully-filled fields; qid/mode/uid/gid/nlink/size are the node's. The
/// remaining statx fields (rdev, blksize, blocks, times, gen, data_version) are
/// zeroed -- a 0 blksize defaults to 4096 in dev9p_stat_native.
///
/// Body: valid(8) qid(13) mode(4) uid(4) gid(4) nlink(8) rdev(8) size(8)
///   blksize(8) blocks(8) atime{s,ns}(16) mtime{s,ns}(16) ctime{s,ns}(16)
///   btime{s,ns}(16) gen(8) data_version(8).
#[allow(clippy::too_many_arguments)]
pub fn build_rgetattr(out: &mut [u8], tag: u16, valid: u64, qid: &Qid,
                      mode: u32, uid: u32, gid: u32, nlink: u64,
                      size: u64) -> Result<usize, ()> {
    let p = build_header(out, P9_RGETATTR, tag)?;
    let p = pack_u64(out, p, valid)?;
    let p = pack_qid(out, p, qid)?;
    let p = pack_u32(out, p, mode)?;
    let p = pack_u32(out, p, uid)?;
    let p = pack_u32(out, p, gid)?;
    let p = pack_u64(out, p, nlink)?;
    let p = pack_u64(out, p, 0)?;        // rdev
    let p = pack_u64(out, p, size)?;     // size
    let p = pack_u64(out, p, 0)?;        // blksize (0 -> dev9p default 4096)
    let p = pack_u64(out, p, 0)?;        // blocks
    let p = pack_u64(out, p, 0)?;        // atime_sec
    let p = pack_u64(out, p, 0)?;        // atime_nsec
    let p = pack_u64(out, p, 0)?;        // mtime_sec
    let p = pack_u64(out, p, 0)?;        // mtime_nsec
    let p = pack_u64(out, p, 0)?;        // ctime_sec
    let p = pack_u64(out, p, 0)?;        // ctime_nsec
    let p = pack_u64(out, p, 0)?;        // btime_sec
    let p = pack_u64(out, p, 0)?;        // btime_nsec
    let p = pack_u64(out, p, 0)?;        // gen
    let p = pack_u64(out, p, 0)?;        // data_version
    patch_header_size(out, p)?;
    Ok(p)
}
