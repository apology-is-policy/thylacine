// /bin/diorama-probe -- the VIVARIUM V-4a in-guest gate.
//
// Proves the whole chain end to end: connect to /srv/diorama, mount it, and read
// /self/exe back as THIS binary's own path. That single read exercises the
// kernel's V-4a-0 Proc.exe_path record, the V-4a-0b srv_peer_info.pid channel,
// the diorama's peer resolution, and the 9P server path -- none of which the
// diorama's own selftest can reach, because that runs before any client exists.
//
// WHY THIS PROBE MOUNTS THE DIORAMA ITSELF, rather than joey mounting it once:
// `self` is resolved from the 9P CONNECTION's peer, so it means "the Proc that
// opened this connection" -- the MOUNTER. Under a shared joey-owned mount every
// reader would see joey's identity (and joey, being blob-loaded, has no recorded
// exe at all, so the file would read empty). A Proc's territory is private, so a
// Proc that mounts the diorama itself gets itself as `self` by construction --
// which is also exactly how a vivarium will set up a container (V-7).
//
// The mount point /dio is created by joey on the pivoted root; mounting over it
// here affects only this Proc's territory.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::{
    t_close, t_exits, t_mount, t_open, t_putstr, t_read, T_MREPL, T_OREAD, T_WALK_OPEN_FROM_ROOT,
};

fn fail(msg: &str) -> ! {
    t_putstr("diorama-probe: FAIL ");
    t_putstr(msg);
    t_putstr("\n");
    unsafe { t_exits(1) }
}

fn contains(hay: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() || needle.len() > hay.len() {
        return needle.is_empty();
    }
    (0..=hay.len() - needle.len()).any(|i| &hay[i..i + needle.len()] == needle)
}

/// Print a decimal, so the probe can REPORT measured sizes rather than only
/// asserting on them -- the maps buffer bounds are sized from this number.
fn put_dec(mut v: u64) {
    let mut tmp = [0u8; 20];
    let mut i = 0;
    if v == 0 {
        tmp[i] = b'0';
        i += 1;
    }
    while v > 0 {
        tmp[i] = b'0' + (v % 10) as u8;
        v /= 10;
        i += 1;
    }
    let mut out = [0u8; 20];
    let n = i;
    while i > 0 {
        i -= 1;
        out[n - 1 - i] = tmp[i];
    }
    if let Ok(s) = core::str::from_utf8(&out[..n]) {
        t_putstr(s);
    }
}

/// Read a whole file into `out`; returns the byte count or None.
fn read_all(path: &[u8], out: &mut [u8]) -> Option<usize> {
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OREAD) };
    if fd < 0 {
        return None;
    }
    let mut total = 0usize;
    loop {
        if total >= out.len() {
            break;
        }
        let n = unsafe { t_read(fd, out.as_mut_ptr().add(total), out.len() - total) };
        if n <= 0 {
            break;
        }
        total += n as usize;
    }
    let _ = unsafe { t_close(fd) };
    Some(total)
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // 1. open=connect to the diorama (9P-mode -> a mountable dev9p root).
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/diorama".as_ptr(), 12, T_OREAD) };
    if root < 0 {
        fail("connect /srv/diorama");
    }
    // 2. Mount it in OUR territory. t_mount takes its own ref, so the connect fd
    //    closes after (ARCH 9.6.6).
    if unsafe { t_mount(b"/dio".as_ptr(), 4, root, T_MREPL) } != 0 {
        fail("mount /dio");
    }
    let _ = unsafe { t_close(root) };

    // 3. The gate: /self/exe must be OUR path, because we are the connection's
    //    peer. A wrong answer here means the peer resolution regressed; an empty
    //    one means the kernel stopped recording exe_path at exec.
    let mut buf = [0u8; 128];
    let n = match read_all(b"/dio/self/exe", &mut buf) {
        Some(n) => n,
        None => fail("open /dio/self/exe"),
    };
    let want = b"/bin/diorama-probe";
    if n != want.len() || &buf[..n] != want {
        t_putstr("diorama-probe: FAIL exe mismatch: got '");
        // The render is bare bytes (no NUL), so bound the print by n.
        for i in 0..n {
            let c = [buf[i]];
            t_putstr(unsafe { core::str::from_utf8_unchecked(&c) });
        }
        t_putstr("' want '/bin/diorama-probe'\n");
        unsafe { t_exits(1) }
    }

    // 4. cmdline is the same path, NUL-terminated (the Linux argv[0] shape).
    let mut cbuf = [0u8; 128];
    let cn = match read_all(b"/dio/self/cmdline", &mut cbuf) {
        Some(n) => n,
        None => fail("open /dio/self/cmdline"),
    };
    if cn != want.len() + 1 || &cbuf[..want.len()] != want || cbuf[cn - 1] != 0 {
        fail("cmdline shape");
    }

    // 5. status carries our own pid + a Linux-shaped Name line.
    let mut sbuf = [0u8; 512];
    let sn = match read_all(b"/dio/self/status", &mut sbuf) {
        Some(n) => n,
        None => fail("open /dio/self/status"),
    };
    if sn == 0 {
        fail("status empty");
    }
    // Name: must be the basename, not the whole path.
    if &sbuf[..6] != b"Name:\t" || &sbuf[6..6 + 13] != b"diorama-probe" {
        fail("status Name");
    }

    // 6. cwd (V-4b-1): never empty for a live peer -- an un-chdir'd Proc is "/".
    let mut wbuf = [0u8; 128];
    let wn = match read_all(b"/dio/self/cwd", &mut wbuf) {
        Some(n) => n,
        None => fail("open /dio/self/cwd"),
    };
    if wn == 0 || wbuf[0] != b'/' {
        fail("cwd shape");
    }

    // 6b. maps (V-4b-2): the address space in Linux's shape. Every exec'd Proc
    //     has a stack VMA and file-backed text (REVENANT demand-pages it), so
    //     both the role tag and the pathname column are exercised for real --
    //     neither is reachable from the diorama's own selftest, which has no
    //     live address space to render.
    let mut pbuf = [0u8; 4096];
    let pn = match read_all(b"/dio/self/maps", &mut pbuf) {
        Some(n) => n,
        None => fail("open /dio/self/maps"),
    };
    if pn == 0 {
        fail("maps empty");
    }
    // Linux's first column is a bare lowercase-hex range: no 0x, no header.
    if !pbuf[0].is_ascii_hexdigit() {
        fail("maps does not start with a hex address");
    }
    let maps = &pbuf[..pn];
    if !contains(maps, b"[stack]") {
        fail("maps has no [stack] mapping");
    }
    // The text segment is file-backed, so its row carries this binary's path.
    if !contains(maps, b"/bin/diorama-probe") {
        fail("maps has no file-backed row naming the executable");
    }
    // Every row must have Linux's column shape; check the first one fully.
    // "<hex>-<hex> rwxp <hex8> <maj>:<min> <inode>"
    let first_len = maps.iter().position(|&c| c == b'\n').unwrap_or(0);
    if first_len < 30 {
        fail("maps first row is too short to be well-formed");
    }
    // Report the size so the buffer bounds stay measured, not guessed.
    let rows = maps.iter().filter(|&&c| c == b'\n').count();
    t_putstr("diorama-probe: maps rows=");
    put_dec(rows as u64);
    t_putstr(" bytes=");
    put_dec(pn as u64);
    t_putstr("\n");

    // 7. meminfo + uptime render from the system-wide native sources.
    let mut mbuf = [0u8; 256];
    let mn = match read_all(b"/dio/meminfo", &mut mbuf) {
        Some(n) => n,
        None => fail("open /dio/meminfo"),
    };
    if mn == 0 || &mbuf[..9] != b"MemTotal:" {
        fail("meminfo shape");
    }
    let mut ubuf = [0u8; 64];
    let un = match read_all(b"/dio/uptime", &mut ubuf) {
        Some(n) => n,
        None => fail("open /dio/uptime"),
    };
    if un == 0 || !ubuf[0].is_ascii_digit() {
        fail("uptime shape");
    }

    // 8. READ-ONLY: opening any diorama file for write must be refused. This is
    //    the property that keeps the whole surface small, so it is worth a gate.
    const T_OWRITE: u32 = 1;
    let w = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/dio/self/exe".as_ptr(), 13, T_OWRITE) };
    if w >= 0 {
        let _ = unsafe { t_close(w) };
        fail("write-open was ALLOWED (read-only violated)");
    }

    t_putstr("diorama-probe: PASS (/self/exe=/bin/diorama-probe; cmdline+status+cwd+maps+meminfo+uptime OK; write refused)\n");
    unsafe { t_exits(0) }
}
