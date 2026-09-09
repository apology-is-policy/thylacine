// forage -- mount a remote 9P2000.L tree served over TCP.
//
// The thylacine ranged well beyond its den to feed; what it brought back went
// in the larder. This grafts a tree from outside the machine into the local
// namespace, and the pages it yields are cached by the Larder (the guest-side
// FS cache, I-38). The name is a PROPOSAL per CLAUDE.md "Thematic naming" --
// `mount` and `srv` are the Plan 9 words, `quarry` and `prowl` are taken.
//
//   forage [-a aname] host!port mountpoint
//   forage -a / 10.0.2.100!7820 /n/host
//
// WHY A USERSPACE SHIM AT ALL. The kernel's 9P client speaks 9P2000.L over a
// BYTE PIPE, not over TCP: `9p_spoor_transport` moves messages across a pair of
// Spoors, and there is no in-kernel socket transport (there is deliberately no
// TCP in the kernel at all -- netd owns the stack). SYS_ATTACH_9P's own doc
// names the way across:
//
//     "For half-duplex (Plan 9 pipes from SYS_PIPE), userspace creates two
//      pipe pairs and passes the matching write-end and read-end."
//
// So forage holds the SERVER ends of two pipes, hands the CLIENT ends to
// SYS_ATTACH_9P, and shuttles bytes between those and the TCP connection. The
// kernel believes it is talking to a local 9P server; the server believes it is
// talking to a local 9P client. `usr/viv` already drives this exact attach for
// the container diorama -- the pattern is live, not new.
//
// THE ORDERING THAT MATTERS. SYS_ATTACH_9P performs Tversion + Tattach
// SYNCHRONOUSLY inside the syscall, so it blocks until replies arrive. Nothing
// can reply unless the pumps are already running. Hence: spawn both pump
// threads FIRST, then attach from the main thread. Reversing those two
// deadlocks -- the kernel waits for an Rversion that only a thread that has not
// been spawned could deliver.
//
// FRAMED, NOT BLIND. A plain byte splice would be correct for this chunk (9P
// over TCP is just the stream). The pump parses the 9P `size[4]` prefix anyway,
// for two reasons: the npxf secure channel seals ONE 9P MESSAGE PER AEAD RECORD
// and so needs message boundaries, and framing lets a hostile or confused peer's
// absurd length claim be refused HERE rather than handed inward. The npxf layer
// is then a swap of `read_msg`/`write_msg`, not a rewrite of the pump.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicI64, AtomicU32, Ordering};

use libthyla_rs::env::{self, Args};
use libthyla_rs::net::{SocketAddrV4, TcpStream};
use libthyla_rs::thread;
use libthyla_rs::{
    t_attach_9p, t_burrow_attach, t_close, t_mount, t_pipe, t_putstr, T_MREPL,
};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

/// The largest 9P message forage will relay in either direction.
///
/// A 9P message is `size[4]` INCLUDING those four bytes, so this bounds the
/// whole frame. It is a shim-side ceiling, not the negotiated msize: forage
/// does not parse Tversion (it is a transport, not a client), so it cannot know
/// what the two ends agreed on. 1 MiB is far above any msize the kernel
/// negotiates and far below a length a hostile peer could use to make us
/// allocate the heap -- which is the point of having it at all.
const MSG_MAX: u32 = 1024 * 1024;

/// A 9P message shorter than its own header is malformed by construction.
const MSG_MIN: u32 = 4;

/// Per-pump stack. One page is ample -- a pump's frame is a buffer pointer and
/// a few counters; the message buffer itself is heap.
const PUMP_STACK: u64 = 64 * 1024;

// The pumps run as bare `extern "C" fn(u64)` entries, so their inputs arrive
// through statics rather than a closure. Written once before either spawn and
// only read afterwards.
static KERNEL_RD: AtomicI64 = AtomicI64::new(-1); // we read what the kernel wrote
static KERNEL_WR: AtomicI64 = AtomicI64::new(-1); // we write what the server said
static TCP_FD: AtomicI64 = AtomicI64::new(-1);
/// Set by whichever pump stops first, so the survivor's diagnosis names the
/// side that actually ended rather than the side that noticed.
static STOPPED: AtomicU32 = AtomicU32::new(0);

const STOP_NONE: u32 = 0;
const STOP_UP: u32 = 1; // kernel -> server direction ended
const STOP_DOWN: u32 = 2; // server -> kernel direction ended

/// Read exactly `buf.len()` bytes, or report short. A single `read` on either a
/// pipe or a TCP stream may return less than asked for, and a 9P frame that is
/// split across two reads is the ordinary case, not the exceptional one.
fn read_exact(fd: i64, buf: &mut [u8]) -> bool {
    let mut off = 0usize;
    while off < buf.len() {
        let n = unsafe { libthyla_rs::t_read(fd, buf[off..].as_mut_ptr(), buf.len() - off) };
        if n <= 0 {
            return false; // EOF or error: the caller treats both as "channel over"
        }
        off += n as usize;
    }
    true
}

fn write_exact(fd: i64, buf: &[u8]) -> bool {
    let mut off = 0usize;
    while off < buf.len() {
        let n = unsafe { libthyla_rs::t_write(fd, buf[off..].as_ptr(), buf.len() - off) };
        if n <= 0 {
            return false;
        }
        off += n as usize;
    }
    true
}

/// Decode the 9P `size[4]` little-endian prefix and validate it.
///
/// Returns the TOTAL frame length (header included), or None if the peer
/// claimed a size that cannot be a 9P message. Refusing here is what keeps a
/// bad length from becoming a giant allocation or an inward-handed lie.
fn frame_len(hdr: &[u8; 4]) -> Option<u32> {
    let size = u32::from_le_bytes(*hdr);
    if !(MSG_MIN..=MSG_MAX).contains(&size) {
        return None;
    }
    Some(size)
}

/// Relay one framed 9P message from `src` to `dst`. Returns false at end of
/// channel or on a malformed frame.
fn relay_one(src: i64, dst: i64, buf: &mut Vec<u8>) -> bool {
    let mut hdr = [0u8; 4];
    if !read_exact(src, &mut hdr) {
        return false;
    }
    let size = match frame_len(&hdr) {
        Some(s) => s,
        None => {
            say!(
                "forage: refusing a frame claiming {} bytes (bound {})",
                u32::from_le_bytes(hdr),
                MSG_MAX
            );
            return false;
        }
    };
    // The header is part of the message, so the body is size - 4.
    let body = (size - 4) as usize;
    if buf.len() < body {
        buf.resize(body, 0);
    }
    if body > 0 && !read_exact(src, &mut buf[..body]) {
        return false;
    }
    // One write per half would let a peer observe a torn frame; the two halves
    // go out back to back and a failure of either ends the channel.
    write_exact(dst, &hdr) && (body == 0 || write_exact(dst, &buf[..body]))
}

/// kernel -> server. Reads T-messages the kernel wrote into its pipe and
/// forwards them to the TCP connection.
extern "C" fn pump_up(_arg: u64) {
    let src = KERNEL_RD.load(Ordering::Acquire);
    let dst = TCP_FD.load(Ordering::Acquire);
    let mut buf: Vec<u8> = Vec::new();
    while relay_one(src, dst, &mut buf) {}
    let _ = STOPPED.compare_exchange(STOP_NONE, STOP_UP, Ordering::AcqRel, Ordering::Acquire);
    // Closing our write end is what turns "the server hung up" into an EOF the
    // kernel's session can see, rather than a session parked forever on a reply
    // that will never come.
    let _ = unsafe { t_close(KERNEL_WR.load(Ordering::Acquire)) };
    unsafe { libthyla_rs::t_thread_exit() };
}

/// server -> kernel. Reads R-messages off the TCP connection and forwards them
/// into the kernel's pipe.
extern "C" fn pump_down(_arg: u64) {
    let src = TCP_FD.load(Ordering::Acquire);
    let dst = KERNEL_WR.load(Ordering::Acquire);
    let mut buf: Vec<u8> = Vec::new();
    while relay_one(src, dst, &mut buf) {}
    let _ = STOPPED.compare_exchange(STOP_NONE, STOP_DOWN, Ordering::AcqRel, Ordering::Acquire);
    let _ = unsafe { t_close(dst) };
    unsafe { libthyla_rs::t_thread_exit() };
}

fn spawn_pump(entry: extern "C" fn(u64)) -> Result<(), &'static str> {
    let stack = unsafe { t_burrow_attach(PUMP_STACK) };
    if stack < 0 {
        return Err("pump stack");
    }
    // AAPCS64 wants the stack TOP 16-aligned; the burrow base is page-aligned
    // and the size is a page multiple, so base + size is too.
    let sp = (stack as u64) + PUMP_STACK;
    unsafe { thread::spawn_raw(entry as *const () as u64, sp, 0, 0) }
        .map(|_| ())
        .map_err(|_| "pump spawn")
}

const USAGE: &str = "\
usage: forage [-a aname] host!port mountpoint
  Mount a remote 9P2000.L tree served over TCP.
  -a aname   the tree to attach (default \"/\")
  --help     show this help

Example:
  forage 10.0.2.100!7820 /n/host
";

struct Parsed {
    addr: String,
    mountpoint: String,
    aname: String,
}

fn parse_args(args: Args) -> Result<Parsed, &'static str> {
    let mut aname = String::from("/");
    let mut positional: Vec<String> = Vec::new();
    let mut i = 1usize; // argv[0] is the program name
    while let Some(a) = args.get(i) {
        if a == b"-a" {
            i += 1;
            match args.get_str(i) {
                Some(v) => aname = String::from(v),
                None => return Err("-a wants a tree name"),
            }
        } else if a == b"-h" || a == b"--help" {
            let _ = t_putstr(USAGE);
            return Err("");
        } else if a.first() == Some(&b'-') && a.len() > 1 {
            return Err("unknown option");
        } else {
            match args.get_str(i) {
                Some(v) => positional.push(String::from(v)),
                None => return Err("argument is not UTF-8"),
            }
        }
        i += 1;
    }
    if positional.len() != 2 {
        let _ = t_putstr(USAGE);
        return Err("");
    }
    Ok(Parsed {
        addr: positional[0].clone(),
        mountpoint: positional[1].clone(),
        aname,
    })
}

fn run(argv: Args) -> Result<(), &'static str> {
    let args = parse_args(argv)?;

    let addr = SocketAddrV4::parse(&args.addr).map_err(|_| "address (want host!port)")?;
    let stream = TcpStream::connect(addr).map_err(|_| "connect")?;
    // The pumps outlive this scope and address the connection by fd, so the
    // TcpStream must NOT drop (its Drop closes ctl + data, tearing the
    // connection down under them). Leaked deliberately: forage lives for the
    // mount's lifetime, so the connection's lifetime IS the process's.
    let tcp_fd = stream.as_raw_fd() as i64;
    core::mem::forget(stream);

    // Two half-duplex pipes: c2s carries T-messages, s2c carries R-messages.
    let (c2s_rd, c2s_wr) = unsafe { t_pipe() };
    let (s2c_rd, s2c_wr) = unsafe { t_pipe() };
    if c2s_rd < 0 || c2s_wr < 0 || s2c_rd < 0 || s2c_wr < 0 {
        return Err("pipe pair");
    }

    KERNEL_RD.store(c2s_rd, Ordering::Release);
    KERNEL_WR.store(s2c_wr, Ordering::Release);
    TCP_FD.store(tcp_fd, Ordering::Release);

    // BOTH PUMPS BEFORE THE ATTACH. SYS_ATTACH_9P drives Tversion + Tattach
    // synchronously; with no pump running, the kernel would wait for an
    // Rversion nothing can deliver and the mount would hang rather than fail.
    spawn_pump(pump_up)?;
    spawn_pump(pump_down)?;

    // The attach takes its own refs on both transport Spoors, so our copies of
    // the CLIENT ends close immediately after -- the session keeps the channel,
    // not these fds. (The SERVER ends stay open: they are the pumps'.)
    let root = unsafe {
        t_attach_9p(
            c2s_wr,
            s2c_rd,
            args.aname.as_ptr(),
            args.aname.len(),
            0,
        )
    };
    let _ = unsafe { t_close(c2s_wr) };
    let _ = unsafe { t_close(s2c_rd) };
    if root < 0 {
        // The pumps have the diagnosis: a server that hung up mid-handshake
        // stops one of them, and which one says whether we failed to send or
        // failed to hear back.
        return Err(match STOPPED.load(Ordering::Acquire) {
            STOP_UP => "attach (the connection closed while sending)",
            STOP_DOWN => "attach (the server closed without replying)",
            _ => "attach (9P handshake refused)",
        });
    }

    let rc = unsafe {
        t_mount(
            args.mountpoint.as_ptr(),
            args.mountpoint.len(),
            root,
            T_MREPL,
        )
    };
    let _ = unsafe { t_close(root) };
    if rc < 0 {
        return Err("mount");
    }

    say!(
        "forage: {} mounted at {} (aname {})",
        args.addr,
        args.mountpoint,
        args.aname
    );

    // The mount lives exactly as long as this Proc does: the pumps ARE the
    // transport, so exiting here would tear the session down under whoever is
    // walking the tree. Park until a pump reports the channel ended.
    loop {
        if STOPPED.load(Ordering::Acquire) != STOP_NONE {
            say!("forage: {} closed the connection -- the mount is dead", args.addr);
            return Ok(());
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(200));
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    match run(env::args()) {
        Ok(()) => 0,
        // The empty reason is --help / usage: already printed, and a second
        // line naming it as a failure would be wrong.
        Err("") => 2,
        Err(why) => {
            say!("forage: {}", why);
            1
        }
    }
}
