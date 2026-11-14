// ninep -- 9P2000.L wire-format codec for native Thylacine servers, reached
// as `libthyla_rs::ninep`. Its own crate so that its tests run on the host.
//
// At v1.0 the codec is server-side only: T-message parsers consume
// inbound frames; R-message builders produce outbound frames. The
// client-side counterparts (T-builders + R-parsers) are NOT here at
// v1.0 -- native programs reach 9P services via the kernel 9P client
// (mounted trees via SYS_OPEN / SYS_WALK_OPEN; a /srv 9P service via
// open=connect -> SYS_ATTACH_9P_SRV), not by speaking the protocol from
// userspace. The client side lifts when a real consumer surfaces (compare
// U-2g's deferral of std::thread::spawn(FnOnce) for the same reason).
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
// What this crate does NOT do:
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

#![no_std]

// =============================================================================
// 9P2000.L message types (the subset corvus serves; matches the kernel
// wire header). Extend as new operations land.
// =============================================================================

pub const P9_TVERSION: u8 = 100;
pub const P9_RVERSION: u8 = 101;
pub const P9_TAUTH: u8 = 102;
pub const P9_RAUTH: u8 = 103;
pub const P9_TATTACH: u8 = 104;
pub const P9_RATTACH: u8 = 105;
pub const P9_TWALK: u8 = 110;
pub const P9_RWALK: u8 = 111;
pub const P9_TLOPEN: u8 = 12;
pub const P9_RLOPEN: u8 = 13;
pub const P9_TREAD: u8 = 116;
pub const P9_RREAD: u8 = 117;
pub const P9_TWRITE: u8 = 118;
pub const P9_RWRITE: u8 = 119;
pub const P9_TCLUNK: u8 = 120;
pub const P9_RCLUNK: u8 = 121;
pub const P9_TGETATTR: u8 = 24;
pub const P9_RGETATTR: u8 = 25;
pub const P9_TREADDIR: u8 = 40;
pub const P9_RREADDIR: u8 = 41;
pub const P9_RLERROR: u8 = 7;
pub const P9_TFLUSH: u8 = 108;
pub const P9_RFLUSH: u8 = 109;
pub const P9_TSTATFS: u8 = 8;
pub const P9_RSTATFS: u8 = 9;
pub const P9_TLCREATE: u8 = 14;
pub const P9_RLCREATE: u8 = 15;
pub const P9_TSYMLINK: u8 = 16;
pub const P9_RSYMLINK: u8 = 17;
pub const P9_TMKNOD: u8 = 18;
pub const P9_RMKNOD: u8 = 19;
pub const P9_TRENAME: u8 = 20;
pub const P9_RRENAME: u8 = 21;
pub const P9_TREADLINK: u8 = 22;
pub const P9_RREADLINK: u8 = 23;
pub const P9_TSETATTR: u8 = 26;
pub const P9_RSETATTR: u8 = 27;
pub const P9_TFSYNC: u8 = 50;
pub const P9_RFSYNC: u8 = 51;
pub const P9_TLOCK: u8 = 52;
pub const P9_RLOCK: u8 = 53;
pub const P9_TGETLOCK: u8 = 54;
pub const P9_RGETLOCK: u8 = 55;
pub const P9_TLINK: u8 = 70;
pub const P9_RLINK: u8 = 71;
pub const P9_TMKDIR: u8 = 72;
pub const P9_RMKDIR: u8 = 73;
pub const P9_TRENAMEAT: u8 = 74;
pub const P9_RRENAMEAT: u8 = 75;
pub const P9_TUNLINKAT: u8 = 76;
pub const P9_RUNLINKAT: u8 = 77;

// QID type bits (the v1.0 surface -- corvus's namespace is one dir +
// one file, no symlinks/temp).
pub const P9_QTDIR: u8 = 0x80;
pub const P9_QTFILE: u8 = 0x00;
/// A symbolic link's qid type.
pub const P9_QTSYMLINK: u8 = 0x02;
/// Thylacine 9P extension (net-6b-2b; NET-DESIGN section 12.2): a "pollable" file
/// -- one whose server (netd's per-connection `ready`, ptyfs's `<n>ready`) answers
/// non-consuming readiness reads as [`ready_answer`] decides. The kernel dev9p.poll
/// reads ONLY a file whose qid.type carries this bit; an unmarked file is POSIX
/// always-ready. 0x01 is unused in 9P2000.L.
pub const P9_QTPOLL: u8 = 0x01;

// Linux dirent type byte (the `d_type` of a Treaddir entry). The kernel
// dev9p client passes this through verbatim to the userspace readdir
// consumer; only DIR vs REG matter for the /net tree.
pub const DT_DIR: u8 = 4;
pub const DT_REG: u8 = 8;
pub const DT_LNK: u8 = 10;

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
pub const E_PERM: u32 = 1;
pub const E_NOENT: u32 = 2;
pub const E_BADF: u32 = 9;
pub const E_AGAIN: u32 = 11; // EAGAIN/EWOULDBLOCK (a nonblocking read with no data, #52)
pub const E_NOMEM: u32 = 12;
pub const E_FAULT: u32 = 14;
pub const E_EXIST: u32 = 17;
pub const E_NODEV: u32 = 19; // ENODEV: the backing device is absent (no capture stream, N-3c-2)
pub const E_NOTDIR: u32 = 20;
pub const E_ISDIR: u32 = 21;
pub const E_INVAL: u32 = 22;
/// EBUSY (16): a resource another party holds -- a second session declaration while one is live.
pub const E_BUSY: u32 = 16;
pub const E_OPNOTSUPP: u32 = 95;
pub const E_PROTO: u32 = 71;
pub const E_NOSYS: u32 = 38;
pub const E_TIMEDOUT: u32 = 110; // POSIX ETIMEDOUT (a connect that got no SYN-ACK)
pub const E_CONNREFUSED: u32 = 111; // POSIX ECONNREFUSED (a connect RST/reset)
/// ESTALE: the file a fid named has died. A client reads this one by value: the kernel reads a
/// server's `E_BADF` as EIO, which a client cannot tell from a lost link (docs/ERRORS.md).
pub const E_STALE: u32 = 116;
// What a host filesystem answers (a shared folder). Each crosses the kernel by value
// (docs/ERRORS.md: a server errno needs no registry name).
pub const E_IO: u32 = 5;
pub const E_ACCES: u32 = 13;
pub const E_XDEV: u32 = 18;
pub const E_FBIG: u32 = 27;
pub const E_NOSPC: u32 = 28;
pub const E_ROFS: u32 = 30;
pub const E_MLINK: u32 = 31;
pub const E_NAMETOOLONG: u32 = 36;
pub const E_NOTEMPTY: u32 = 39;
pub const E_LOOP: u32 = 40;
pub const E_OVERFLOW: u32 = 75;
pub const E_DQUOT: u32 = 122;

// =============================================================================
// Qid -- in-memory shape of the 13-byte wire qid.
// =============================================================================

/// In-memory shape of a 9P qid. Pack via [`pack_qid`]; the wire layout
/// is `[kind: u8][version: u32][path: u64]` = 13 bytes.
#[derive(Copy, Clone, Default, Debug)]
pub struct Qid {
    pub kind: u8, // P9_QT*
    pub version: u32,
    pub path: u64,
}

// =============================================================================
// Primitive unpackers -- zero-copy slice into the caller's buffer.
// =============================================================================

pub fn unpack_u8(buf: &[u8], off: usize) -> Result<(u8, usize), ()> {
    if buf.len() < off + 1 {
        return Err(());
    }
    Ok((buf[off], off + 1))
}

pub fn unpack_u16(buf: &[u8], off: usize) -> Result<(u16, usize), ()> {
    if buf.len() < off + 2 {
        return Err(());
    }
    let v = (buf[off] as u16) | ((buf[off + 1] as u16) << 8);
    Ok((v, off + 2))
}

pub fn unpack_u32(buf: &[u8], off: usize) -> Result<(u32, usize), ()> {
    if buf.len() < off + 4 {
        return Err(());
    }
    let v = (buf[off] as u32)
        | ((buf[off + 1] as u32) << 8)
        | ((buf[off + 2] as u32) << 16)
        | ((buf[off + 3] as u32) << 24);
    Ok((v, off + 4))
}

pub fn unpack_u64(buf: &[u8], off: usize) -> Result<(u64, usize), ()> {
    if buf.len() < off + 8 {
        return Err(());
    }
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
    if buf.len() < end {
        return Err(());
    }
    Ok((&buf[p..end], end))
}

// =============================================================================
// Primitive packers -- write at `out[off..]`, return new offset.
// =============================================================================

pub fn pack_u8(out: &mut [u8], off: usize, v: u8) -> Result<usize, ()> {
    if out.len() < off + 1 {
        return Err(());
    }
    out[off] = v;
    Ok(off + 1)
}

pub fn pack_u16(out: &mut [u8], off: usize, v: u16) -> Result<usize, ()> {
    if out.len() < off + 2 {
        return Err(());
    }
    out[off] = (v & 0xff) as u8;
    out[off + 1] = ((v >> 8) & 0xff) as u8;
    Ok(off + 2)
}

pub fn pack_u32(out: &mut [u8], off: usize, v: u32) -> Result<usize, ()> {
    if out.len() < off + 4 {
        return Err(());
    }
    out[off] = (v & 0xff) as u8;
    out[off + 1] = ((v >> 8) & 0xff) as u8;
    out[off + 2] = ((v >> 16) & 0xff) as u8;
    out[off + 3] = ((v >> 24) & 0xff) as u8;
    Ok(off + 4)
}

pub fn pack_u64(out: &mut [u8], off: usize, v: u64) -> Result<usize, ()> {
    if out.len() < off + 8 {
        return Err(());
    }
    for i in 0..8 {
        out[off + i] = ((v >> (8 * i)) & 0xff) as u8;
    }
    Ok(off + 8)
}

pub fn pack_str(out: &mut [u8], off: usize, s: &[u8]) -> Result<usize, ()> {
    if s.len() > u16::MAX as usize {
        return Err(());
    }
    let off = pack_u16(out, off, s.len() as u16)?;
    if out.len() < off + s.len() {
        return Err(());
    }
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
    if buf.len() < P9_HDR_LEN {
        return Err(());
    }
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
    Ok(TattachArgs {
        fid,
        afid,
        uname,
        aname,
        n_uname,
    })
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
    if nwname as usize > P9_MAX_WALK {
        return Err(());
    }
    let mut names: [&[u8]; P9_MAX_WALK] = [&[][..]; P9_MAX_WALK];
    for i in 0..(nwname as usize) {
        let (name, np) = unpack_str(buf, p)?;
        if name.len() > P9_NAME_MAX {
            return Err(());
        }
        names[i] = name;
        p = np;
    }
    Ok(TwalkArgs {
        fid,
        newfid,
        nwname,
        names,
    })
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
    if buf.len() < end {
        return Err(());
    }
    Ok(TwriteArgs {
        fid,
        offset,
        count,
        data: &buf[p..end],
    })
}

#[derive(Copy, Clone)]
pub struct TclunkArgs {
    pub fid: u32,
}

pub fn parse_tclunk(buf: &[u8]) -> Result<TclunkArgs, ()> {
    let (fid, _) = unpack_u32(buf, P9_HDR_LEN)?;
    Ok(TclunkArgs { fid })
}

#[derive(Copy, Clone)]
pub struct TflushArgs {
    pub oldtag: u16,
}

/// Tflush body: `[oldtag: u16]` -- the tag of the in-flight request the client
/// abandons (sent by the kernel 9P client on a death-interrupt). A server must
/// stop processing `oldtag` and reply Rflush; per 9P the client reuses `oldtag`
/// only after the Rflush (I-10), so the Rflush is what frees the abandoned tag.
pub fn parse_tflush(buf: &[u8]) -> Result<TflushArgs, ()> {
    let (oldtag, _) = unpack_u16(buf, P9_HDR_LEN)?;
    Ok(TflushArgs { oldtag })
}

/// Treaddir body: `[fid: u32][offset: u64][count: u32]`. `offset` is the
/// opaque resume cookie from a prior Rreaddir entry (0 = from the start);
/// `count` bounds the dirent bytes the server may return.
#[derive(Copy, Clone)]
pub struct TreaddirArgs {
    pub fid: u32,
    pub offset: u64,
    pub count: u32,
}

pub fn parse_treaddir(buf: &[u8]) -> Result<TreaddirArgs, ()> {
    let (fid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (offset, p) = unpack_u64(buf, p)?;
    let (count, _) = unpack_u32(buf, p)?;
    Ok(TreaddirArgs { fid, offset, count })
}

/// The T-messages whose whole body is one fid: Tstatfs and Treadlink.
pub fn parse_tfid_only(buf: &[u8]) -> Result<u32, ()> {
    let (fid, _) = unpack_u32(buf, P9_HDR_LEN)?;
    Ok(fid)
}

// A name a creating or naming T-message carries: one path element, never empty.
fn unpack_name(buf: &[u8], off: usize) -> Result<(&[u8], usize), ()> {
    let (name, p) = unpack_str(buf, off)?;
    if name.is_empty() || name.len() > P9_NAME_MAX {
        return Err(());
    }
    Ok((name, p))
}

/// Tlcreate body: `[fid: u32][name: str][flags: u32][mode: u32][gid: u32]`. The fid names the
/// directory, and on success it names the new file, opened with `flags`.
#[derive(Copy, Clone)]
pub struct TlcreateArgs<'a> {
    pub fid: u32,
    pub name: &'a [u8],
    pub flags: u32,
    pub mode: u32,
    pub gid: u32,
}

pub fn parse_tlcreate(buf: &[u8]) -> Result<TlcreateArgs<'_>, ()> {
    let (fid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (name, p) = unpack_name(buf, p)?;
    let (flags, p) = unpack_u32(buf, p)?;
    let (mode, p) = unpack_u32(buf, p)?;
    let (gid, _) = unpack_u32(buf, p)?;
    Ok(TlcreateArgs {
        fid,
        name,
        flags,
        mode,
        gid,
    })
}

/// Tsymlink body: `[fid: u32][name: str][symtgt: str][gid: u32]`. The target is any bytes.
#[derive(Copy, Clone)]
pub struct TsymlinkArgs<'a> {
    pub fid: u32,
    pub name: &'a [u8],
    pub target: &'a [u8],
    pub gid: u32,
}

pub fn parse_tsymlink(buf: &[u8]) -> Result<TsymlinkArgs<'_>, ()> {
    let (fid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (name, p) = unpack_name(buf, p)?;
    let (target, p) = unpack_str(buf, p)?;
    let (gid, _) = unpack_u32(buf, p)?;
    Ok(TsymlinkArgs {
        fid,
        name,
        target,
        gid,
    })
}

/// Tmknod body: `[dfid: u32][name: str][mode: u32][major: u32][minor: u32][gid: u32]`.
#[derive(Copy, Clone)]
pub struct TmknodArgs<'a> {
    pub dfid: u32,
    pub name: &'a [u8],
    pub mode: u32,
    pub major: u32,
    pub minor: u32,
    pub gid: u32,
}

pub fn parse_tmknod(buf: &[u8]) -> Result<TmknodArgs<'_>, ()> {
    let (dfid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (name, p) = unpack_name(buf, p)?;
    let (mode, p) = unpack_u32(buf, p)?;
    let (major, p) = unpack_u32(buf, p)?;
    let (minor, p) = unpack_u32(buf, p)?;
    let (gid, _) = unpack_u32(buf, p)?;
    Ok(TmknodArgs {
        dfid,
        name,
        mode,
        major,
        minor,
        gid,
    })
}

/// Trename body: `[fid: u32][dfid: u32][name: str]`: the file `fid` names moves into the
/// directory `dfid` names, as `name`.
#[derive(Copy, Clone)]
pub struct TrenameArgs<'a> {
    pub fid: u32,
    pub dfid: u32,
    pub name: &'a [u8],
}

pub fn parse_trename(buf: &[u8]) -> Result<TrenameArgs<'_>, ()> {
    let (fid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (dfid, p) = unpack_u32(buf, p)?;
    let (name, _) = unpack_name(buf, p)?;
    Ok(TrenameArgs { fid, dfid, name })
}

// Tsetattr `valid` bits (Linux v9fs P9_ATTR_*; kernel/include/thylacine/9p_wire.h).
pub const P9_SETATTR_MODE: u32 = 1 << 0;
pub const P9_SETATTR_UID: u32 = 1 << 1;
pub const P9_SETATTR_GID: u32 = 1 << 2;
pub const P9_SETATTR_SIZE: u32 = 1 << 3;
pub const P9_SETATTR_ATIME: u32 = 1 << 4;
pub const P9_SETATTR_MTIME: u32 = 1 << 5;
pub const P9_SETATTR_CTIME: u32 = 1 << 6;
/// With ATIME: set atime to the message's value. Without: to the server's present time.
pub const P9_SETATTR_ATIME_SET: u32 = 1 << 7;
/// With MTIME: set mtime to the message's value. Without: to the server's present time.
pub const P9_SETATTR_MTIME_SET: u32 = 1 << 8;

/// Tsetattr body: `fid(4) valid(4) mode(4) uid(4) gid(4) size(8) atime{sec,nsec}(16)
/// mtime{sec,nsec}(16)`. Only the fields `valid` names are meant.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct TsetattrArgs {
    pub fid: u32,
    pub valid: u32,
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub size: u64,
    pub atime_sec: u64,
    pub atime_nsec: u64,
    pub mtime_sec: u64,
    pub mtime_nsec: u64,
}

pub fn parse_tsetattr(buf: &[u8]) -> Result<TsetattrArgs, ()> {
    let (fid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (valid, p) = unpack_u32(buf, p)?;
    let (mode, p) = unpack_u32(buf, p)?;
    let (uid, p) = unpack_u32(buf, p)?;
    let (gid, p) = unpack_u32(buf, p)?;
    let (size, p) = unpack_u64(buf, p)?;
    let (atime_sec, p) = unpack_u64(buf, p)?;
    let (atime_nsec, p) = unpack_u64(buf, p)?;
    let (mtime_sec, p) = unpack_u64(buf, p)?;
    let (mtime_nsec, _) = unpack_u64(buf, p)?;
    Ok(TsetattrArgs {
        fid,
        valid,
        mode,
        uid,
        gid,
        size,
        atime_sec,
        atime_nsec,
        mtime_sec,
        mtime_nsec,
    })
}

/// Tfsync body: `[fid: u32][datasync: u32]`; a nonzero datasync asks for the data alone.
#[derive(Copy, Clone)]
pub struct TfsyncArgs {
    pub fid: u32,
    pub datasync: u32,
}

pub fn parse_tfsync(buf: &[u8]) -> Result<TfsyncArgs, ()> {
    let (fid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (datasync, _) = unpack_u32(buf, p)?;
    Ok(TfsyncArgs { fid, datasync })
}

// Tlock / Tgetlock types and flags, and Rlock's status (kernel/include/thylacine/9p_wire.h).
pub const P9_LOCK_TYPE_RDLCK: u8 = 0;
pub const P9_LOCK_TYPE_WRLCK: u8 = 1;
pub const P9_LOCK_TYPE_UNLCK: u8 = 2;
pub const P9_LOCK_FLAGS_BLOCK: u32 = 1;
pub const P9_LOCK_SUCCESS: u8 = 0;
pub const P9_LOCK_BLOCKED: u8 = 1;
pub const P9_LOCK_ERROR: u8 = 2;
pub const P9_LOCK_GRACE: u8 = 3;

/// Tlock body: `[fid: u32][type: u8][flags: u32][start: u64][length: u64][proc_id: u32]
/// [client_id: str]`.
#[derive(Copy, Clone)]
pub struct TlockArgs<'a> {
    pub fid: u32,
    pub kind: u8,
    pub flags: u32,
    pub start: u64,
    pub length: u64,
    pub proc_id: u32,
    pub client_id: &'a [u8],
}

pub fn parse_tlock(buf: &[u8]) -> Result<TlockArgs<'_>, ()> {
    let (fid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (kind, p) = unpack_u8(buf, p)?;
    let (flags, p) = unpack_u32(buf, p)?;
    let (start, p) = unpack_u64(buf, p)?;
    let (length, p) = unpack_u64(buf, p)?;
    let (proc_id, p) = unpack_u32(buf, p)?;
    let (client_id, _) = unpack_str(buf, p)?;
    Ok(TlockArgs {
        fid,
        kind,
        flags,
        start,
        length,
        proc_id,
        client_id,
    })
}

/// Tgetlock body: `[fid: u32][type: u8][start: u64][length: u64][proc_id: u32][client_id: str]`.
#[derive(Copy, Clone)]
pub struct TgetlockArgs<'a> {
    pub fid: u32,
    pub kind: u8,
    pub start: u64,
    pub length: u64,
    pub proc_id: u32,
    pub client_id: &'a [u8],
}

pub fn parse_tgetlock(buf: &[u8]) -> Result<TgetlockArgs<'_>, ()> {
    let (fid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (kind, p) = unpack_u8(buf, p)?;
    let (start, p) = unpack_u64(buf, p)?;
    let (length, p) = unpack_u64(buf, p)?;
    let (proc_id, p) = unpack_u32(buf, p)?;
    let (client_id, _) = unpack_str(buf, p)?;
    Ok(TgetlockArgs {
        fid,
        kind,
        start,
        length,
        proc_id,
        client_id,
    })
}

/// Tlink body: `[dfid: u32][fid: u32][name: str]`: a hard link to `fid`'s file, in `dfid`.
#[derive(Copy, Clone)]
pub struct TlinkArgs<'a> {
    pub dfid: u32,
    pub fid: u32,
    pub name: &'a [u8],
}

pub fn parse_tlink(buf: &[u8]) -> Result<TlinkArgs<'_>, ()> {
    let (dfid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (fid, p) = unpack_u32(buf, p)?;
    let (name, _) = unpack_name(buf, p)?;
    Ok(TlinkArgs { dfid, fid, name })
}

/// Tmkdir body: `[dfid: u32][name: str][mode: u32][gid: u32]`.
#[derive(Copy, Clone)]
pub struct TmkdirArgs<'a> {
    pub dfid: u32,
    pub name: &'a [u8],
    pub mode: u32,
    pub gid: u32,
}

pub fn parse_tmkdir(buf: &[u8]) -> Result<TmkdirArgs<'_>, ()> {
    let (dfid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (name, p) = unpack_name(buf, p)?;
    let (mode, p) = unpack_u32(buf, p)?;
    let (gid, _) = unpack_u32(buf, p)?;
    Ok(TmkdirArgs {
        dfid,
        name,
        mode,
        gid,
    })
}

/// Trenameat body: `[olddirfid: u32][oldname: str][newdirfid: u32][newname: str]`.
#[derive(Copy, Clone)]
pub struct TrenameatArgs<'a> {
    pub olddirfid: u32,
    pub oldname: &'a [u8],
    pub newdirfid: u32,
    pub newname: &'a [u8],
}

pub fn parse_trenameat(buf: &[u8]) -> Result<TrenameatArgs<'_>, ()> {
    let (olddirfid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (oldname, p) = unpack_name(buf, p)?;
    let (newdirfid, p) = unpack_u32(buf, p)?;
    let (newname, _) = unpack_name(buf, p)?;
    Ok(TrenameatArgs {
        olddirfid,
        oldname,
        newdirfid,
        newname,
    })
}

/// Tunlinkat's flag that asks for a directory's removal (Linux AT_REMOVEDIR).
pub const P9_UNLINK_AT_REMOVEDIR: u32 = 0x200;

/// Tunlinkat body: `[dfid: u32][name: str][flags: u32]`.
#[derive(Copy, Clone)]
pub struct TunlinkatArgs<'a> {
    pub dfid: u32,
    pub name: &'a [u8],
    pub flags: u32,
}

pub fn parse_tunlinkat(buf: &[u8]) -> Result<TunlinkatArgs<'_>, ()> {
    let (dfid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (name, p) = unpack_name(buf, p)?;
    let (flags, _) = unpack_u32(buf, p)?;
    Ok(TunlinkatArgs { dfid, name, flags })
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
    if out.len() < 4 || total_len > u32::MAX as usize {
        return Err(());
    }
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
    if qids.len() > P9_MAX_WALK {
        return Err(());
    }
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
    if data.len() > u32::MAX as usize {
        return Err(());
    }
    let p = build_header(out, P9_RREAD, tag)?;
    let p = pack_u32(out, p, data.len() as u32)?;
    if out.len() < p + data.len() {
        return Err(());
    }
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

/// Rflush has no body: it acknowledges that the Tflush `oldtag` is abandoned, so
/// the client may reuse it. Header-only, like Rclunk.
pub fn build_rflush(out: &mut [u8], tag: u16) -> Result<usize, ()> {
    let p = build_header(out, P9_RFLUSH, tag)?;
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

/// build_rgetattr -- an Rgetattr for a node that keeps no times: the node's
/// qid/mode/uid/gid/nlink/size, and every other field zero (a 0 blksize
/// defaults to 4096 in dev9p_stat_native). `valid` is the mask of the
/// meaningfully-filled fields. [`build_rgetattr_attr`] carries every field.
#[allow(clippy::too_many_arguments)]
pub fn build_rgetattr(
    out: &mut [u8],
    tag: u16,
    valid: u64,
    qid: &Qid,
    mode: u32,
    uid: u32,
    gid: u32,
    nlink: u64,
    size: u64,
) -> Result<usize, ()> {
    let attr = Attr {
        valid,
        qid: *qid,
        mode,
        uid,
        gid,
        nlink,
        size,
        ..Attr::default()
    };
    build_rgetattr_attr(out, tag, &attr)
}

// Tgetattr request-mask / Rgetattr valid bits past the security trio (statx's).
pub const P9_GETATTR_RDEV: u64 = 0x10;
pub const P9_GETATTR_ATIME: u64 = 0x20;
pub const P9_GETATTR_MTIME: u64 = 0x40;
pub const P9_GETATTR_CTIME: u64 = 0x80;
pub const P9_GETATTR_INO: u64 = 0x100;
pub const P9_GETATTR_SIZE: u64 = 0x200;
pub const P9_GETATTR_BLOCKS: u64 = 0x400;
/// The fields a POSIX stat has: MODE through BLOCKS.
pub const P9_GETATTR_BASIC: u64 = 0x7FF;
pub const P9_GETATTR_BTIME: u64 = 0x800;
pub const P9_GETATTR_GEN: u64 = 0x1000;
pub const P9_GETATTR_DATA_VERSION: u64 = 0x2000;

/// Every field of an Rgetattr. A time is `(seconds, nanoseconds)`.
#[derive(Copy, Clone, Default, Debug)]
pub struct Attr {
    pub valid: u64,
    pub qid: Qid,
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub nlink: u64,
    pub rdev: u64,
    pub size: u64,
    pub blksize: u64,
    pub blocks: u64,
    pub atime: (u64, u64),
    pub mtime: (u64, u64),
    pub ctime: (u64, u64),
    pub btime: (u64, u64),
    pub gen: u64,
    pub data_version: u64,
}

/// The Rgetattr body's length: the kernel's `p9_parse_rgetattr` takes exactly this many bytes.
pub const P9_RGETATTR_BODY_LEN: usize = 8 + P9_QID_LEN + 12 + 15 * 8;

/// build_rgetattr_attr -- the full 9P2000.L Rgetattr.
///
/// Body: valid(8) qid(13) mode(4) uid(4) gid(4) nlink(8) rdev(8) size(8)
///   blksize(8) blocks(8) atime{s,ns}(16) mtime{s,ns}(16) ctime{s,ns}(16)
///   btime{s,ns}(16) gen(8) data_version(8).
pub fn build_rgetattr_attr(out: &mut [u8], tag: u16, a: &Attr) -> Result<usize, ()> {
    let p = build_header(out, P9_RGETATTR, tag)?;
    let p = pack_u64(out, p, a.valid)?;
    let p = pack_qid(out, p, &a.qid)?;
    let p = pack_u32(out, p, a.mode)?;
    let p = pack_u32(out, p, a.uid)?;
    let mut p = pack_u32(out, p, a.gid)?;
    for v in [
        a.nlink,
        a.rdev,
        a.size,
        a.blksize,
        a.blocks,
        a.atime.0,
        a.atime.1,
        a.mtime.0,
        a.mtime.1,
        a.ctime.0,
        a.ctime.1,
        a.btime.0,
        a.btime.1,
        a.gen,
        a.data_version,
    ] {
        p = pack_u64(out, p, v)?;
    }
    patch_header_size(out, p)?;
    Ok(p)
}

// The replies of the creating and naming operations. Three shapes: empty (Rrename,
// Rrenameat, Rlink, Runlinkat, Rsetattr, Rfsync), one qid (Rsymlink, Rmknod, Rmkdir),
// and a qid with an iounit (Rlcreate, as Rlopen).

fn build_empty(out: &mut [u8], mtype: u8, tag: u16) -> Result<usize, ()> {
    let p = build_header(out, mtype, tag)?;
    patch_header_size(out, p)?;
    Ok(p)
}

fn build_qid_only(out: &mut [u8], mtype: u8, tag: u16, qid: &Qid) -> Result<usize, ()> {
    let p = build_header(out, mtype, tag)?;
    let p = pack_qid(out, p, qid)?;
    patch_header_size(out, p)?;
    Ok(p)
}

pub fn build_rrename(out: &mut [u8], tag: u16) -> Result<usize, ()> {
    build_empty(out, P9_RRENAME, tag)
}

pub fn build_rrenameat(out: &mut [u8], tag: u16) -> Result<usize, ()> {
    build_empty(out, P9_RRENAMEAT, tag)
}

pub fn build_rlink(out: &mut [u8], tag: u16) -> Result<usize, ()> {
    build_empty(out, P9_RLINK, tag)
}

pub fn build_runlinkat(out: &mut [u8], tag: u16) -> Result<usize, ()> {
    build_empty(out, P9_RUNLINKAT, tag)
}

pub fn build_rsetattr(out: &mut [u8], tag: u16) -> Result<usize, ()> {
    build_empty(out, P9_RSETATTR, tag)
}

pub fn build_rfsync(out: &mut [u8], tag: u16) -> Result<usize, ()> {
    build_empty(out, P9_RFSYNC, tag)
}

pub fn build_rsymlink(out: &mut [u8], tag: u16, qid: &Qid) -> Result<usize, ()> {
    build_qid_only(out, P9_RSYMLINK, tag, qid)
}

pub fn build_rmknod(out: &mut [u8], tag: u16, qid: &Qid) -> Result<usize, ()> {
    build_qid_only(out, P9_RMKNOD, tag, qid)
}

pub fn build_rmkdir(out: &mut [u8], tag: u16, qid: &Qid) -> Result<usize, ()> {
    build_qid_only(out, P9_RMKDIR, tag, qid)
}

pub fn build_rlcreate(out: &mut [u8], tag: u16, qid: &Qid, iounit: u32) -> Result<usize, ()> {
    let p = build_header(out, P9_RLCREATE, tag)?;
    let p = pack_qid(out, p, qid)?;
    let p = pack_u32(out, p, iounit)?;
    patch_header_size(out, p)?;
    Ok(p)
}

/// Rreadlink body: `[target: str]`.
pub fn build_rreadlink(out: &mut [u8], tag: u16, target: &[u8]) -> Result<usize, ()> {
    let p = build_header(out, P9_RREADLINK, tag)?;
    let p = pack_str(out, p, target)?;
    patch_header_size(out, p)?;
    Ok(p)
}

/// An Rstatfs's fields (Linux statfs).
#[derive(Copy, Clone, Default, Debug)]
pub struct Statfs {
    pub kind: u32,
    pub bsize: u32,
    pub blocks: u64,
    pub bfree: u64,
    pub bavail: u64,
    pub files: u64,
    pub ffree: u64,
    pub fsid: u64,
    pub namelen: u32,
}

/// Rstatfs body: `type(4) bsize(4) blocks(8) bfree(8) bavail(8) files(8) ffree(8) fsid(8)
/// namelen(4)`, 60 bytes.
pub fn build_rstatfs(out: &mut [u8], tag: u16, st: &Statfs) -> Result<usize, ()> {
    let p = build_header(out, P9_RSTATFS, tag)?;
    let p = pack_u32(out, p, st.kind)?;
    let p = pack_u32(out, p, st.bsize)?;
    let p = pack_u64(out, p, st.blocks)?;
    let p = pack_u64(out, p, st.bfree)?;
    let p = pack_u64(out, p, st.bavail)?;
    let p = pack_u64(out, p, st.files)?;
    let p = pack_u64(out, p, st.ffree)?;
    let p = pack_u64(out, p, st.fsid)?;
    let p = pack_u32(out, p, st.namelen)?;
    patch_header_size(out, p)?;
    Ok(p)
}

/// Rlock body: `[status: u8]`.
pub fn build_rlock(out: &mut [u8], tag: u16, status: u8) -> Result<usize, ()> {
    let p = build_header(out, P9_RLOCK, tag)?;
    let p = pack_u8(out, p, status)?;
    patch_header_size(out, p)?;
    Ok(p)
}

/// Rgetlock body: `[type: u8][start: u64][length: u64][proc_id: u32][client_id: str]`.
/// `kind` P9_LOCK_TYPE_UNLCK says no lock is in the way.
pub fn build_rgetlock(
    out: &mut [u8],
    tag: u16,
    kind: u8,
    start: u64,
    length: u64,
    proc_id: u32,
    client_id: &[u8],
) -> Result<usize, ()> {
    let p = build_header(out, P9_RGETLOCK, tag)?;
    let p = pack_u8(out, p, kind)?;
    let p = pack_u64(out, p, start)?;
    let p = pack_u64(out, p, length)?;
    let p = pack_u32(out, p, proc_id)?;
    let p = pack_str(out, p, client_id)?;
    patch_header_size(out, p)?;
    Ok(p)
}

// =============================================================================
// Tweft / Rweft (Weft-6 Thylacine extension) -- the per-flow zero-copy ring
// setup op. A kernel-client-issued op (the #845 Tflush precedent): the kernel's
// dev9p client issues Tweft(fid F) the first time a /net flow goes zero-copy;
// netd's 9P server allocates + registers a per-flow ring and answers Rweft with
// the kernel-scoped share_id + geometry. The share_id is a netd->kernel join key
// (it rides the kernel's OWN Tweft round-trip), never handed to the guest -- the
// RDMA-rkey shape (NET-THROUGHPUT.md section 4.6 / 6). Mirrors the kernel wire
// codec (kernel/9p_wire.c::p9_{build,parse}_{tweft,rweft}) byte for byte.
//
// Tweft body: [fid: u32].  Rweft body: [share_id: u64][ring_size: u32][ring_entries: u32].
// =============================================================================

// 142/143: renumbered from 134/135 at #371 (the old pair latently collided
// with Stratum Tfadvise/Tpin; registry = thylacine docs/9P-EXTENSIONS.md).
pub const P9_TWEFT: u8 = 142;
pub const P9_RWEFT: u8 = 143;

/// The ring grant an Rweft carries: the kernel-scoped registration token + the
/// geometry the kernel needs to compute its private weft_ring_view (it does NOT
/// trust the shared header's mirror -- the Weft-3 snapshot discipline).
#[derive(Copy, Clone)]
pub struct WeftGeom {
    pub share_id: u64,
    pub ring_size: u32,
    pub ring_entries: u32,
}

/// parse_tweft (server side) -- extract the target fid from a Tweft.
pub fn parse_tweft(buf: &[u8]) -> Result<u32, ()> {
    let (fid, _) = unpack_u32(buf, P9_HDR_LEN)?;
    Ok(fid)
}

/// build_rweft (server side) -- the ring grant: share_id + geometry.
pub fn build_rweft(
    out: &mut [u8],
    tag: u16,
    share_id: u64,
    ring_size: u32,
    ring_entries: u32,
) -> Result<usize, ()> {
    let p = build_header(out, P9_RWEFT, tag)?;
    let p = pack_u64(out, p, share_id)?;
    let p = pack_u32(out, p, ring_size)?;
    let p = pack_u32(out, p, ring_entries)?;
    patch_header_size(out, p)?;
    Ok(p)
}

/// build_tweft (client side) -- request a zero-copy ring for the flow on `fid`.
/// The kernel dev9p client is the only v1.0 issuer; provided for symmetry + tests.
pub fn build_tweft(out: &mut [u8], tag: u16, fid: u32) -> Result<usize, ()> {
    let p = build_header(out, P9_TWEFT, tag)?;
    let p = pack_u32(out, p, fid)?;
    patch_header_size(out, p)?;
    Ok(p)
}

/// parse_rweft (client side) -- extract share_id + geometry from an Rweft.
pub fn parse_rweft(buf: &[u8]) -> Result<WeftGeom, ()> {
    let (share_id, p) = unpack_u64(buf, P9_HDR_LEN)?;
    let (ring_size, p) = unpack_u32(buf, p)?;
    let (ring_entries, _) = unpack_u32(buf, p)?;
    Ok(WeftGeom {
        share_id,
        ring_size,
        ring_entries,
    })
}

// =============================================================================
// Tweftio / Rweftio (Weft-6b-2 Thylacine extension) -- the data drive. After a
// flow's ring is mapped (Tweft), a large Twrite/Tread issues Tweftio carrying
// the kernel-validated payload descriptor (offset + len within the flow's shared
// ring + a direction); netd reads/writes the ring IN PLACE + replies the count.
// Mirrors kernel/9p_wire.c::p9_{build,parse}_{tweftio,rweftio} byte for byte.
//
// Tweftio body: [fid: u32][off: u32][len: u32][dir: u32]   (16 bytes).
// Rweftio body: [count: u32]                               (4 bytes).
// =============================================================================

// 144/145: renumbered from 136/137 at #371 (see P9_TWEFT above).
pub const P9_TWEFTIO: u8 = 144;
pub const P9_RWEFTIO: u8 = 145;

/// Tweftio direction -- which way the payload moves through the shared ring.
pub const WEFT_DIR_WRITE: u32 = 0; // TX: netd reads ring[off..off+len] -> send
pub const WEFT_DIR_READ: u32 = 1; // RX: netd writes recv bytes -> ring[off..off+len]

/// The validated payload descriptor a Tweftio carries: the flow fid + the
/// payload-region-relative window (off, len) + the direction.
#[derive(Copy, Clone)]
pub struct WeftIoReq {
    pub fid: u32,
    pub off: u32,
    pub len: u32,
    pub dir: u32,
}

/// parse_tweftio (server side) -- extract the descriptor + direction.
pub fn parse_tweftio(buf: &[u8]) -> Result<WeftIoReq, ()> {
    let (fid, p) = unpack_u32(buf, P9_HDR_LEN)?;
    let (off, p) = unpack_u32(buf, p)?;
    let (len, p) = unpack_u32(buf, p)?;
    let (dir, _) = unpack_u32(buf, p)?;
    Ok(WeftIoReq { fid, off, len, dir })
}

/// build_rweftio (server side) -- the count of payload bytes the consumer moved.
pub fn build_rweftio(out: &mut [u8], tag: u16, count: u32) -> Result<usize, ()> {
    let p = build_header(out, P9_RWEFTIO, tag)?;
    let p = pack_u32(out, p, count)?;
    patch_header_size(out, p)?;
    Ok(p)
}

/// build_tweftio (client side) -- the kernel dev9p client is the only v1.0
/// issuer; provided for symmetry + tests.
pub fn build_tweftio(
    out: &mut [u8],
    tag: u16,
    fid: u32,
    off: u32,
    len: u32,
    dir: u32,
) -> Result<usize, ()> {
    let p = build_header(out, P9_TWEFTIO, tag)?;
    let p = pack_u32(out, p, fid)?;
    let p = pack_u32(out, p, off)?;
    let p = pack_u32(out, p, len)?;
    let p = pack_u32(out, p, dir)?;
    patch_header_size(out, p)?;
    Ok(p)
}

/// parse_rweftio (client side) -- extract the moved-byte count from an Rweftio.
pub fn parse_rweftio(buf: &[u8]) -> Result<u32, ()> {
    let (count, _) = unpack_u32(buf, P9_HDR_LEN)?;
    Ok(count)
}

// =============================================================================
// Treaddir / Rreaddir (9P2000.L directory enumeration).
//
// A Rreaddir body is `[count: u32][data: count]` where `data` is a packed
// sequence of dirent records, each:
//   [qid: 13][next_offset: u64][type: u8][name: str]
// `next_offset` is the opaque cookie the client echoes in the FOLLOWING
// Treaddir to resume AFTER this entry (so it must be strictly increasing and
// never 0 -- the kernel readdir-cookie convention). The server assembles the
// dirent stream with `pack_dirent` (bounding the total by the Treaddir count
// and msize), then frames it with `build_rreaddir`.
// =============================================================================

/// Byte length of one packed dirent with a `name_len`-byte name. Lets a server
/// check that the next entry fits the remaining budget before packing it.
pub fn dirent_len(name_len: usize) -> usize {
    P9_QID_LEN + 8 + 1 + 2 + name_len
}

/// Pack one dirent record at `out[off..]`. `next_offset` is the resume cookie
/// for the entry AFTER this one. Returns the new offset.
pub fn pack_dirent(
    out: &mut [u8],
    off: usize,
    qid: &Qid,
    next_offset: u64,
    dtype: u8,
    name: &[u8],
) -> Result<usize, ()> {
    let off = pack_qid(out, off, qid)?;
    let off = pack_u64(out, off, next_offset)?;
    let off = pack_u8(out, off, dtype)?;
    pack_str(out, off, name)
}

/// Frame an assembled dirent stream as a Rreaddir. `data` is the packed dirent
/// records (built with `pack_dirent`); it is COPIED into `out`. The caller
/// bounds `data.len()` by the Treaddir count and `msize - P9_HDR_LEN - 4`.
pub fn build_rreaddir(out: &mut [u8], tag: u16, data: &[u8]) -> Result<usize, ()> {
    if data.len() > u32::MAX as usize {
        return Err(());
    }
    let p = build_header(out, P9_RREADDIR, tag)?;
    let p = pack_u32(out, p, data.len() as u32)?;
    if out.len() < p + data.len() {
        return Err(());
    }
    out[p..p + data.len()].copy_from_slice(data);
    let total = p + data.len();
    patch_header_size(out, total)?;
    Ok(total)
}

// =============================================================================
// Readiness files (NET-DESIGN section 12.2). A Tread of a P9_QTPOLL file carries
// poll events in its offset; the reply is the satisfied revents as a u32 LE.
// =============================================================================

/// The poll events a readiness Tread asks about: the offset's low 16 bits.
pub const P9_POLL_MASK: u64 = 0xFFFF;
/// Set in a readiness Tread's offset: a SNAPSHOT, answered at once with the
/// current revents, 0 included, and never held. Clear: the ARM, held until the
/// file is ready for the mask.
pub const P9_POLL_SNAPSHOT: u64 = 1 << 16;

/// What a readiness file does with one Tread.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReadyAnswer {
    /// Reply now: the first `len` bytes of `revents` as a u32 LE.
    Reply { revents: u16, len: usize },
    /// Hold the Tread; once the file is ready for `mask`, reply `len` bytes.
    Defer { mask: u16, len: usize },
    /// Rlerror with this code: the offset carries a bit the protocol does not
    /// define, and a future bit must never be read as part of a mask.
    Refuse(u32),
}

/// Decide a readiness Tread. `level` returns the file's revents for a mask as
/// they are now. It is asked once, on arrival, for a snapshot and an arm alike,
/// so an arm sent after the file became ready is answered at once: the kernel's
/// snapshot may predate the rise, and an arm that waited for a later one would
/// park its poller on readiness that already happened. A refused offset is not
/// evaluated at all.
pub fn ready_answer(
    offset: u64,
    count: u32,
    msize: u32,
    level: impl FnOnce(u16) -> u16,
) -> ReadyAnswer {
    if offset & !(P9_POLL_MASK | P9_POLL_SNAPSHOT) != 0 {
        return ReadyAnswer::Refuse(E_INVAL);
    }
    let mask = (offset & P9_POLL_MASK) as u16;
    let len = (msize as usize)
        .saturating_sub(P9_HDR_LEN + 4)
        .min(count as usize)
        .min(4);
    let revents = level(mask);
    if revents != 0 || offset & P9_POLL_SNAPSHOT != 0 {
        ReadyAnswer::Reply { revents, len }
    } else {
        ReadyAnswer::Defer { mask, len }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // A framed message: the common header, `body`, the size back-patched.
    fn frame(mtype: u8, tag: u16, body: &[u8], out: &mut [u8]) -> usize {
        let p = build_header(out, mtype, tag).unwrap();
        out[p..p + body.len()].copy_from_slice(body);
        let n = p + body.len();
        patch_header_size(out, n).unwrap();
        n
    }

    // A Twalk from fid 1 to newfid 2 declaring `nwname` names, each `name`.
    fn twalk(nwname: u16, name: &[u8], out: &mut [u8]) -> usize {
        let mut body = [0u8; 1024];
        let p = pack_u32(&mut body, 0, 1).unwrap();
        let p = pack_u32(&mut body, p, 2).unwrap();
        let mut p = pack_u16(&mut body, p, nwname).unwrap();
        for _ in 0..nwname {
            p = pack_str(&mut body, p, name).unwrap();
        }
        frame(P9_TWALK, 1, &body[..p], out)
    }

    // A body laid out field by field, in the order kernel/9p_wire.c's builder writes it.
    struct Body {
        b: [u8; 512],
        n: usize,
    }

    impl Body {
        fn new() -> Body {
            Body { b: [0; 512], n: 0 }
        }
        fn u8(mut self, v: u8) -> Body {
            self.n = pack_u8(&mut self.b, self.n, v).unwrap();
            self
        }
        fn u32(mut self, v: u32) -> Body {
            self.n = pack_u32(&mut self.b, self.n, v).unwrap();
            self
        }
        fn u64(mut self, v: u64) -> Body {
            self.n = pack_u64(&mut self.b, self.n, v).unwrap();
            self
        }
        fn s(mut self, v: &[u8]) -> Body {
            self.n = pack_str(&mut self.b, self.n, v).unwrap();
            self
        }
        fn framed(&self, mtype: u8, out: &mut [u8]) -> usize {
            frame(mtype, 9, &self.b[..self.n], out)
        }
    }

    #[test]
    fn the_naming_tmessages_parse_in_the_kernels_field_order() {
        let mut m = [0u8; 600];
        let n = Body::new()
            .u32(3)
            .s(b"new")
            .u32(0o1101)
            .u32(0o644)
            .u32(1000)
            .framed(P9_TLCREATE, &mut m);
        let a = parse_tlcreate(&m[..n]).unwrap();
        assert_eq!(
            (a.fid, a.name, a.flags, a.mode, a.gid),
            (3, &b"new"[..], 0o1101, 0o644, 1000)
        );

        let n = Body::new()
            .u32(4)
            .s(b"ln")
            .s(b"../t")
            .u32(7)
            .framed(P9_TSYMLINK, &mut m);
        let a = parse_tsymlink(&m[..n]).unwrap();
        assert_eq!(
            (a.fid, a.name, a.target, a.gid),
            (4, &b"ln"[..], &b"../t"[..], 7)
        );

        let n = Body::new()
            .u32(5)
            .s(b"fifo")
            .u32(0o10644)
            .u32(1)
            .u32(2)
            .u32(8)
            .framed(P9_TMKNOD, &mut m);
        let a = parse_tmknod(&m[..n]).unwrap();
        assert_eq!(
            (a.dfid, a.name, a.mode, a.major, a.minor, a.gid),
            (5, &b"fifo"[..], 0o10644, 1, 2, 8)
        );

        let n = Body::new()
            .u32(6)
            .u32(7)
            .s(b"to")
            .framed(P9_TRENAME, &mut m);
        let a = parse_trename(&m[..n]).unwrap();
        assert_eq!((a.fid, a.dfid, a.name), (6, 7, &b"to"[..]));

        let n = Body::new()
            .u32(8)
            .u32(9)
            .s(b"hard")
            .framed(P9_TLINK, &mut m);
        let a = parse_tlink(&m[..n]).unwrap();
        assert_eq!((a.dfid, a.fid, a.name), (8, 9, &b"hard"[..]));

        let n = Body::new()
            .u32(10)
            .s(b"d")
            .u32(0o755)
            .u32(11)
            .framed(P9_TMKDIR, &mut m);
        let a = parse_tmkdir(&m[..n]).unwrap();
        assert_eq!((a.dfid, a.name, a.mode, a.gid), (10, &b"d"[..], 0o755, 11));

        let n = Body::new()
            .u32(12)
            .s(b"a")
            .u32(13)
            .s(b"b")
            .framed(P9_TRENAMEAT, &mut m);
        let a = parse_trenameat(&m[..n]).unwrap();
        assert_eq!(
            (a.olddirfid, a.oldname, a.newdirfid, a.newname),
            (12, &b"a"[..], 13, &b"b"[..])
        );

        let n = Body::new()
            .u32(14)
            .s(b"gone")
            .u32(P9_UNLINK_AT_REMOVEDIR)
            .framed(P9_TUNLINKAT, &mut m);
        let a = parse_tunlinkat(&m[..n]).unwrap();
        assert_eq!((a.dfid, a.name, a.flags), (14, &b"gone"[..], 0x200));

        let n = Body::new().u32(15).framed(P9_TREADLINK, &mut m);
        assert_eq!(parse_tfid_only(&m[..n]), Ok(15));
        let n = Body::new().u32(16).u32(1).framed(P9_TFSYNC, &mut m);
        let a = parse_tfsync(&m[..n]).unwrap();
        assert_eq!((a.fid, a.datasync), (16, 1));
    }

    #[test]
    fn a_name_that_is_empty_or_past_name_max_is_refused() {
        let mut m = [0u8; 600];
        let n = Body::new()
            .u32(1)
            .s(b"")
            .u32(0o755)
            .u32(0)
            .framed(P9_TMKDIR, &mut m);
        assert!(parse_tmkdir(&m[..n]).is_err());
        let long = [b'x'; P9_NAME_MAX + 1];
        let n = Body::new()
            .u32(1)
            .s(&long)
            .u32(0)
            .framed(P9_TUNLINKAT, &mut m);
        assert!(parse_tunlinkat(&m[..n]).is_err());
        let fits = [b'x'; P9_NAME_MAX];
        let n = Body::new()
            .u32(1)
            .s(&fits)
            .u32(0)
            .framed(P9_TUNLINKAT, &mut m);
        assert_eq!(parse_tunlinkat(&m[..n]).unwrap().name.len(), P9_NAME_MAX);
        // A symlink's target is a path, not a name: it may be long, and it may be empty.
        let n = Body::new()
            .u32(1)
            .s(b"l")
            .s(b"")
            .u32(0)
            .framed(P9_TSYMLINK, &mut m);
        assert_eq!(parse_tsymlink(&m[..n]).unwrap().target, b"");
    }

    #[test]
    fn a_truncated_naming_tmessage_is_refused() {
        let mut m = [0u8; 600];
        let n = Body::new()
            .u32(12)
            .s(b"a")
            .u32(13)
            .s(b"b")
            .framed(P9_TRENAMEAT, &mut m);
        for cut in P9_HDR_LEN..n {
            assert!(parse_trenameat(&m[..cut]).is_err(), "cut at {cut}");
        }
        let n = Body::new()
            .u32(3)
            .s(b"new")
            .u32(0)
            .u32(0o644)
            .u32(0)
            .framed(P9_TLCREATE, &mut m);
        for cut in P9_HDR_LEN..n {
            assert!(parse_tlcreate(&m[..cut]).is_err(), "cut at {cut}");
        }
    }

    #[test]
    fn tsetattr_is_sixty_bytes_of_body_in_the_kernels_order() {
        let mut m = [0u8; 600];
        let n = Body::new()
            .u32(2)
            .u32(P9_SETATTR_SIZE | P9_SETATTR_MTIME | P9_SETATTR_MTIME_SET)
            .u32(0o600)
            .u32(1000)
            .u32(1001)
            .u64(4096)
            .u64(11)
            .u64(12)
            .u64(13)
            .u64(14)
            .framed(P9_TSETATTR, &mut m);
        assert_eq!(n, P9_HDR_LEN + 60);
        let a = parse_tsetattr(&m[..n]).unwrap();
        assert_eq!(
            a,
            TsetattrArgs {
                fid: 2,
                valid: 0x128,
                mode: 0o600,
                uid: 1000,
                gid: 1001,
                size: 4096,
                atime_sec: 11,
                atime_nsec: 12,
                mtime_sec: 13,
                mtime_nsec: 14,
            }
        );
        assert!(parse_tsetattr(&m[..n - 1]).is_err());
    }

    #[test]
    fn the_lock_tmessages_parse_in_the_kernels_order() {
        let mut m = [0u8; 600];
        let n = Body::new()
            .u32(1)
            .u8(P9_LOCK_TYPE_WRLCK)
            .u32(P9_LOCK_FLAGS_BLOCK)
            .u64(10)
            .u64(20)
            .u32(77)
            .s(b"thylacine")
            .framed(P9_TLOCK, &mut m);
        let a = parse_tlock(&m[..n]).unwrap();
        assert_eq!(
            (
                a.fid,
                a.kind,
                a.flags,
                a.start,
                a.length,
                a.proc_id,
                a.client_id
            ),
            (1, 1, 1, 10, 20, 77, &b"thylacine"[..])
        );
        let n = Body::new()
            .u32(2)
            .u8(P9_LOCK_TYPE_RDLCK)
            .u64(5)
            .u64(6)
            .u32(8)
            .s(b"c")
            .framed(P9_TGETLOCK, &mut m);
        let a = parse_tgetlock(&m[..n]).unwrap();
        assert_eq!(
            (a.fid, a.kind, a.start, a.length, a.proc_id, a.client_id),
            (2, 0, 5, 6, 8, &b"c"[..])
        );
    }

    // The body of a framed R-message: what the kernel's strict parser measures.
    fn body_of(out: &[u8], n: usize, mtype: u8, tag: u16) -> &[u8] {
        let h = peek_header(&out[..n]).unwrap();
        assert_eq!((h.size as usize, h.mtype, h.tag), (n, mtype, tag));
        &out[P9_HDR_LEN..n]
    }

    #[test]
    fn the_replies_have_the_lengths_the_kernels_strict_parsers_take() {
        let mut o = [0u8; 300];
        let q = Qid {
            kind: P9_QTSYMLINK,
            version: 3,
            path: 0x55,
        };
        let empty: [(fn(&mut [u8], u16) -> Result<usize, ()>, u8); 6] = [
            (build_rrename, P9_RRENAME),
            (build_rrenameat, P9_RRENAMEAT),
            (build_rlink, P9_RLINK),
            (build_runlinkat, P9_RUNLINKAT),
            (build_rsetattr, P9_RSETATTR),
            (build_rfsync, P9_RFSYNC),
        ];
        for (f, t) in empty {
            let n = f(&mut o, 4).unwrap();
            assert_eq!(body_of(&o, n, t, 4).len(), 0);
        }
        let qid_only: [(fn(&mut [u8], u16, &Qid) -> Result<usize, ()>, u8); 3] = [
            (build_rsymlink, P9_RSYMLINK),
            (build_rmknod, P9_RMKNOD),
            (build_rmkdir, P9_RMKDIR),
        ];
        for (f, t) in qid_only {
            let n = f(&mut o, 5, &q).unwrap();
            let b = body_of(&o, n, t, 5);
            assert_eq!(b, [P9_QTSYMLINK, 3, 0, 0, 0, 0x55, 0, 0, 0, 0, 0, 0, 0]);
        }
        let n = build_rlcreate(&mut o, 6, &q, 8192).unwrap();
        assert_eq!(body_of(&o, n, P9_RLCREATE, 6).len(), P9_QID_LEN + 4);
        assert_eq!(unpack_u32(&o, P9_HDR_LEN + P9_QID_LEN), Ok((8192, n)));

        let n = build_rreadlink(&mut o, 7, b"../target").unwrap();
        assert_eq!(
            unpack_str(body_of(&o, n, P9_RREADLINK, 7), 0),
            Ok((&b"../target"[..], 11))
        );

        let st = Statfs {
            kind: 0x01021997,
            bsize: 4096,
            blocks: 1,
            bfree: 2,
            bavail: 3,
            files: 4,
            ffree: 5,
            fsid: 6,
            namelen: 255,
        };
        let n = build_rstatfs(&mut o, 8, &st).unwrap();
        let b = body_of(&o, n, P9_RSTATFS, 8);
        assert_eq!(b.len(), 60);
        assert_eq!(unpack_u32(b, 0), Ok((0x01021997, 4)));
        assert_eq!(unpack_u64(b, 40), Ok((5, 48)), "ffree");
        assert_eq!(unpack_u64(b, 48), Ok((6, 56)), "fsid");
        assert_eq!(unpack_u32(b, 56), Ok((255, 60)));

        let n = build_rlock(&mut o, 9, P9_LOCK_SUCCESS).unwrap();
        assert_eq!(body_of(&o, n, P9_RLOCK, 9), [0]);
        let n = build_rgetlock(&mut o, 10, P9_LOCK_TYPE_UNLCK, 1, 2, 3, b"id").unwrap();
        let b = body_of(&o, n, P9_RGETLOCK, 10);
        assert_eq!(b.len(), 1 + 8 + 8 + 4 + 2 + 2);
        assert_eq!(b[0], P9_LOCK_TYPE_UNLCK);
        assert_eq!(unpack_str(b, 21), Ok((&b"id"[..], 25)));
    }

    #[test]
    fn a_reply_that_does_not_fit_is_refused() {
        let q = Qid::default();
        let mut o = [0u8; P9_HDR_LEN + P9_QID_LEN + 3];
        assert!(build_rlcreate(&mut o, 1, &q, 0).is_err());
        let mut o = [0u8; P9_HDR_LEN + 59];
        assert!(build_rstatfs(&mut o, 1, &Statfs::default()).is_err());
        let mut o = [0u8; P9_HDR_LEN + 2 + 3];
        assert!(build_rreadlink(&mut o, 1, b"four").is_err());
        assert_eq!(build_rreadlink(&mut o, 1, b"abc"), Ok(P9_HDR_LEN + 5));
    }

    #[test]
    fn rgetattr_attr_carries_every_field_in_the_kernels_order() {
        let mut o = [0u8; 300];
        let a = Attr {
            valid: P9_GETATTR_BASIC | P9_GETATTR_BTIME,
            qid: Qid {
                kind: P9_QTFILE,
                version: 1,
                path: 2,
            },
            mode: 0o100600,
            uid: 1000,
            gid: 1000,
            nlink: 1,
            rdev: 3,
            size: 4,
            blksize: 5,
            blocks: 6,
            atime: (7, 8),
            mtime: (9, 10),
            ctime: (11, 12),
            btime: (13, 14),
            gen: 15,
            data_version: 16,
        };
        let n = build_rgetattr_attr(&mut o, 2, &a).unwrap();
        let b = body_of(&o, n, P9_RGETATTR, 2);
        assert_eq!(b.len(), P9_RGETATTR_BODY_LEN);
        assert_eq!(P9_RGETATTR_BODY_LEN, 153);
        assert_eq!(unpack_u64(b, 0), Ok((0xFFF, 8)));
        assert_eq!(unpack_u32(b, 8 + P9_QID_LEN), Ok((0o100600, 25)));
        // The fifteen u64s after the security trio: nlink first, data_version last.
        let mut off = 8 + P9_QID_LEN + 12;
        for want in 1..=16u64 {
            if want == 2 {
                continue; // the qid's path, not a u64 of the tail
            }
            let (v, p) = unpack_u64(b, off).unwrap();
            assert_eq!(v, want, "the u64 at {off}");
            off = p;
        }
        assert_eq!(off, b.len());
    }

    #[test]
    fn the_plain_rgetattr_is_the_full_one_with_its_other_fields_zero() {
        let q = Qid {
            kind: P9_QTDIR,
            version: 0,
            path: 9,
        };
        let mut plain = [0u8; 300];
        let n = build_rgetattr(&mut plain, 3, 0xF, &q, 0o40700, 1000, 1000, 2, 0).unwrap();
        let mut full = [0u8; 300];
        let a = Attr {
            valid: 0xF,
            qid: q,
            mode: 0o40700,
            uid: 1000,
            gid: 1000,
            nlink: 2,
            ..Attr::default()
        };
        assert_eq!(build_rgetattr_attr(&mut full, 3, &a), Ok(n));
        assert_eq!(plain[..n], full[..n]);
        assert_eq!(n, P9_HDR_LEN + P9_RGETATTR_BODY_LEN);
    }

    // Every message number this crate names is the kernel's: read against the wire enum the
    // kernel's client sends (kernel/include/thylacine/9p_wire.h), so neither can move alone.
    // Tauth/Rauth, which the kernel never sends, are 9P2000's.
    #[test]
    fn the_message_numbers_are_the_kernels() {
        let h = include_str!("../../../../kernel/include/thylacine/9p_wire.h");
        let kernel = |name: &str| -> Option<u8> {
            h.lines().find_map(|l| {
                let mut w = l.split_whitespace();
                if w.next() != Some("#define") || w.next() != Some(name) {
                    return None;
                }
                w.next()?
                    .trim_matches(|c| c == '(' || c == ')')
                    .trim_end_matches('u')
                    .parse()
                    .ok()
            })
        };
        let ours: &[(&str, u8)] = &[
            ("P9_TVERSION", P9_TVERSION),
            ("P9_RVERSION", P9_RVERSION),
            ("P9_TATTACH", P9_TATTACH),
            ("P9_RATTACH", P9_RATTACH),
            ("P9_TWALK", P9_TWALK),
            ("P9_RWALK", P9_RWALK),
            ("P9_TLOPEN", P9_TLOPEN),
            ("P9_RLOPEN", P9_RLOPEN),
            ("P9_TREAD", P9_TREAD),
            ("P9_RREAD", P9_RREAD),
            ("P9_TWRITE", P9_TWRITE),
            ("P9_RWRITE", P9_RWRITE),
            ("P9_TCLUNK", P9_TCLUNK),
            ("P9_RCLUNK", P9_RCLUNK),
            ("P9_TGETATTR", P9_TGETATTR),
            ("P9_RGETATTR", P9_RGETATTR),
            ("P9_TREADDIR", P9_TREADDIR),
            ("P9_RREADDIR", P9_RREADDIR),
            ("P9_RLERROR", P9_RLERROR),
            ("P9_TFLUSH", P9_TFLUSH),
            ("P9_RFLUSH", P9_RFLUSH),
            ("P9_TSTATFS", P9_TSTATFS),
            ("P9_RSTATFS", P9_RSTATFS),
            ("P9_TLCREATE", P9_TLCREATE),
            ("P9_RLCREATE", P9_RLCREATE),
            ("P9_TSYMLINK", P9_TSYMLINK),
            ("P9_RSYMLINK", P9_RSYMLINK),
            ("P9_TMKNOD", P9_TMKNOD),
            ("P9_RMKNOD", P9_RMKNOD),
            ("P9_TRENAME", P9_TRENAME),
            ("P9_RRENAME", P9_RRENAME),
            ("P9_TREADLINK", P9_TREADLINK),
            ("P9_RREADLINK", P9_RREADLINK),
            ("P9_TSETATTR", P9_TSETATTR),
            ("P9_RSETATTR", P9_RSETATTR),
            ("P9_TFSYNC", P9_TFSYNC),
            ("P9_RFSYNC", P9_RFSYNC),
            ("P9_TLOCK", P9_TLOCK),
            ("P9_RLOCK", P9_RLOCK),
            ("P9_TGETLOCK", P9_TGETLOCK),
            ("P9_RGETLOCK", P9_RGETLOCK),
            ("P9_TLINK", P9_TLINK),
            ("P9_RLINK", P9_RLINK),
            ("P9_TMKDIR", P9_TMKDIR),
            ("P9_RMKDIR", P9_RMKDIR),
            ("P9_TRENAMEAT", P9_TRENAMEAT),
            ("P9_RRENAMEAT", P9_RRENAMEAT),
            ("P9_TUNLINKAT", P9_TUNLINKAT),
            ("P9_RUNLINKAT", P9_RUNLINKAT),
            ("P9_TWEFT", P9_TWEFT),
            ("P9_RWEFT", P9_RWEFT),
            ("P9_TWEFTIO", P9_TWEFTIO),
            ("P9_RWEFTIO", P9_RWEFTIO),
        ];
        assert!(
            ours.len() >= 50,
            "the list is every P9_T*/P9_R* the crate names"
        );
        for (name, v) in ours {
            assert_eq!(kernel(name), Some(*v), "{name}");
        }
        assert_eq!((P9_TAUTH, P9_RAUTH), (102, 103));
    }

    #[test]
    fn integers_round_trip_little_endian_at_an_offset() {
        let mut b = [0u8; 16];
        assert_eq!(pack_u16(&mut b, 1, 0xBEEF), Ok(3));
        assert_eq!(b[1..3], [0xEF, 0xBE]);
        assert_eq!(unpack_u16(&b, 1), Ok((0xBEEF, 3)));
        assert_eq!(pack_u32(&mut b, 3, 0x0102_0304), Ok(7));
        assert_eq!(unpack_u32(&b, 3), Ok((0x0102_0304, 7)));
        assert_eq!(pack_u64(&mut b, 7, 0x1122_3344_5566_7788), Ok(15));
        assert_eq!(b[7], 0x88);
        assert_eq!(unpack_u64(&b, 7), Ok((0x1122_3344_5566_7788, 15)));
        assert_eq!(pack_u8(&mut b, 15, 0xA5), Ok(16));
        assert_eq!(unpack_u8(&b, 15), Ok((0xA5, 16)));
    }

    #[test]
    fn a_short_buffer_is_refused_never_overrun() {
        let b = [0u8; 7];
        assert_eq!(unpack_u8(&b, 7), Err(()));
        assert_eq!(unpack_u16(&b, 6), Err(()));
        assert_eq!(unpack_u32(&b, 4), Err(()));
        assert_eq!(unpack_u64(&b, 0), Err(()));
        assert!(peek_header(&b[..P9_HDR_LEN - 1]).is_err());
        let mut o = [0u8; 7];
        assert_eq!(pack_u8(&mut o, 7, u8::MAX), Err(()));
        assert_eq!(pack_u16(&mut o, 6, u16::MAX), Err(()));
        assert_eq!(pack_u32(&mut o, 4, u32::MAX), Err(()));
        assert_eq!(pack_u64(&mut o, 0, u64::MAX), Err(()));
        assert_eq!(o, [0u8; 7], "a refused pack writes nothing");
    }

    #[test]
    fn a_string_is_length_prefixed_and_bounded_by_its_buffer() {
        let mut o = [0u8; 8];
        assert_eq!(pack_str(&mut o, 0, b"net"), Ok(5));
        assert_eq!(o[..5], [3, 0, b'n', b'e', b't']);
        assert_eq!(unpack_str(&o, 0), Ok((&b"net"[..], 5)));
        // The prefix claims five bytes and three follow.
        assert_eq!(unpack_str(&[5, 0, b'a', b'b', b'c'], 0), Err(()));
        assert_eq!(pack_str(&mut o, 4, b"toolong"), Err(()));
    }

    #[test]
    fn a_qid_is_thirteen_bytes_kind_version_path() {
        let q = Qid {
            kind: P9_QTDIR,
            version: 0x0102_0304,
            path: 0x0506_0708_090A_0B0C,
        };
        let mut o = [0u8; P9_QID_LEN];
        assert_eq!(pack_qid(&mut o, 0, &q), Ok(P9_QID_LEN));
        assert_eq!(o, [0x80, 4, 3, 2, 1, 0x0C, 0x0B, 0x0A, 9, 8, 7, 6, 5]);
        assert_eq!(pack_qid(&mut o[..P9_QID_LEN - 1], 0, &q), Err(()));
    }

    #[test]
    fn a_reply_back_patches_its_total_size() {
        let mut o = [0u8; 32];
        let n = build_rread(&mut o, 0x1234, &[9, 8, 7]).unwrap();
        assert_eq!(n, P9_HDR_LEN + 4 + 3);
        let h = peek_header(&o).unwrap();
        assert_eq!((h.size as usize, h.mtype, h.tag), (n, P9_RREAD, 0x1234));
        assert_eq!(unpack_u32(&o, P9_HDR_LEN), Ok((3, P9_HDR_LEN + 4)));
        assert_eq!(o[P9_HDR_LEN + 4..n], [9, 8, 7]);

        let n = build_rlerror(&mut o, 7, E_INVAL).unwrap();
        let h = peek_header(&o).unwrap();
        assert_eq!(
            (n, h.size as usize, h.mtype, h.tag),
            (P9_HDR_LEN + 4, n, P9_RLERROR, 7)
        );
        assert_eq!(unpack_u32(&o, P9_HDR_LEN), Ok((E_INVAL, n)));
    }

    #[test]
    fn a_reply_too_big_for_its_buffer_is_refused() {
        let mut o = [0u8; P9_HDR_LEN + 4 + 2];
        assert_eq!(build_rread(&mut o, 1, &[1, 2, 3]), Err(()));
        assert_eq!(build_rread(&mut o, 1, &[1, 2]), Ok(o.len()));
        assert_eq!(build_rlerror(&mut o[..P9_HDR_LEN + 3], 1, E_INVAL), Err(()));
    }

    #[test]
    fn tread_carries_the_whole_offset() {
        let mut body = [0u8; 16];
        let p = pack_u32(&mut body, 0, 7).unwrap();
        let p = pack_u64(&mut body, p, 0x8000_0000_0001_0004).unwrap();
        pack_u32(&mut body, p, 4).unwrap();
        let mut f = [0u8; 32];
        let n = frame(P9_TREAD, 3, &body, &mut f);
        let a = parse_tread(&f[..n]).unwrap();
        assert_eq!((a.fid, a.offset, a.count), (7, 0x8000_0000_0001_0004, 4));
        assert!(parse_tread(&f[..n - 1]).is_err());
    }

    #[test]
    fn twalk_bounds_the_name_count_and_each_name() {
        let mut f = [0u8; 1100];
        let n = twalk(2, b"tcp", &mut f);
        let a = parse_twalk(&f[..n]).unwrap();
        assert_eq!((a.fid, a.newfid, a.nwname), (1, 2, 2));
        assert_eq!((a.names[0], a.names[1]), (&b"tcp"[..], &b"tcp"[..]));

        let n = twalk(P9_MAX_WALK as u16, b"a", &mut f);
        assert!(parse_twalk(&f[..n]).is_ok());
        let n = twalk(P9_MAX_WALK as u16 + 1, b"a", &mut f);
        assert!(parse_twalk(&f[..n]).is_err());

        let n = twalk(1, &[b'x'; P9_NAME_MAX], &mut f);
        assert!(parse_twalk(&f[..n]).is_ok());
        let n = twalk(1, &[b'x'; P9_NAME_MAX + 1], &mut f);
        assert!(parse_twalk(&f[..n]).is_err());
    }

    const POLLIN: u16 = 0x001;
    const POLLOUT: u16 = 0x004;
    const MSIZE: u32 = 8192;

    #[test]
    fn a_snapshot_is_answered_at_once_even_when_nothing_is_ready() {
        let mut asked = None;
        let a = ready_answer(P9_POLL_SNAPSHOT | POLLIN as u64, 4, MSIZE, |m| {
            asked = Some(m);
            0
        });
        assert_eq!(a, ReadyAnswer::Reply { revents: 0, len: 4 });
        assert_eq!(asked, Some(POLLIN));
    }

    #[test]
    fn a_snapshot_reports_what_is_ready() {
        let a = ready_answer(
            P9_POLL_SNAPSHOT | (POLLIN | POLLOUT) as u64,
            4,
            MSIZE,
            |m| m & POLLOUT,
        );
        assert_eq!(
            a,
            ReadyAnswer::Reply {
                revents: POLLOUT,
                len: 4
            }
        );
    }

    #[test]
    fn an_arm_is_held_while_nothing_is_ready() {
        let a = ready_answer(POLLIN as u64, 4, MSIZE, |_| 0);
        assert_eq!(
            a,
            ReadyAnswer::Defer {
                mask: POLLIN,
                len: 4
            }
        );
    }

    #[test]
    fn an_arm_sent_after_the_rise_is_answered_at_once() {
        let a = ready_answer(POLLIN as u64, 4, MSIZE, |m| m & POLLIN);
        assert_eq!(
            a,
            ReadyAnswer::Reply {
                revents: POLLIN,
                len: 4
            }
        );
    }

    #[test]
    fn any_other_bit_above_the_mask_is_refused_unevaluated() {
        let offsets = [
            1 << 17,
            P9_POLL_SNAPSHOT | 1 << 17 | POLLOUT as u64,
            1 << 32 | POLLIN as u64,
            1 << 63,
            u64::MAX,
        ];
        for off in offsets {
            let mut evaluated = false;
            let a = ready_answer(off, 4, MSIZE, |m| {
                evaluated = true;
                m
            });
            assert_eq!(a, ReadyAnswer::Refuse(E_INVAL), "offset {off:#x}");
            assert!(!evaluated, "offset {off:#x}");
        }
        // One variable away: the snapshot bit alone is part of the protocol.
        let a = ready_answer(P9_POLL_SNAPSHOT | POLLOUT as u64, 4, MSIZE, |m| m);
        assert_eq!(
            a,
            ReadyAnswer::Reply {
                revents: POLLOUT,
                len: 4
            }
        );
    }

    #[test]
    fn the_mask_is_exactly_the_low_sixteen_bits() {
        let mut asked = None;
        ready_answer(P9_POLL_SNAPSHOT | P9_POLL_MASK, 4, MSIZE, |m| {
            asked = Some(m);
            0
        });
        assert_eq!(asked, Some(0xFFFF));
        let a = ready_answer(0, 4, MSIZE, |m| {
            asked = Some(m);
            0
        });
        assert_eq!(
            (asked, a),
            (Some(0), ReadyAnswer::Defer { mask: 0, len: 4 })
        );
    }

    #[test]
    fn the_reply_is_cut_to_the_tread_count_and_the_msize() {
        let snap =
            |count, msize| ready_answer(P9_POLL_SNAPSHOT | POLLOUT as u64, count, msize, |m| m);
        assert_eq!(
            snap(4, MSIZE),
            ReadyAnswer::Reply {
                revents: POLLOUT,
                len: 4
            }
        );
        assert_eq!(
            snap(64, MSIZE),
            ReadyAnswer::Reply {
                revents: POLLOUT,
                len: 4
            }
        );
        assert_eq!(
            snap(1, MSIZE),
            ReadyAnswer::Reply {
                revents: POLLOUT,
                len: 1
            }
        );
        assert_eq!(
            snap(0, MSIZE),
            ReadyAnswer::Reply {
                revents: POLLOUT,
                len: 0
            }
        );
        let tight = (P9_HDR_LEN + 4 + 2) as u32;
        assert_eq!(
            snap(4, tight),
            ReadyAnswer::Reply {
                revents: POLLOUT,
                len: 2
            }
        );
        assert_eq!(
            snap(4, 0),
            ReadyAnswer::Reply {
                revents: POLLOUT,
                len: 0
            }
        );
        // A held arm keeps the cut it arrived with.
        let a = ready_answer(POLLIN as u64, 2, MSIZE, |_| 0);
        assert_eq!(
            a,
            ReadyAnswer::Defer {
                mask: POLLIN,
                len: 2
            }
        );
    }
}
