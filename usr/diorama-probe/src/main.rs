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
    t_close, t_exits, t_mount, t_open, t_putstr, t_read, t_walk_create, t_write, T_MREPL, T_OPATH,
    T_OREAD, T_OWRITE, T_WALK_OPEN_FROM_ROOT,
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

/// Write a decimal into `out`, returning its length. Used to build the
/// `/dio/<pid>/...` paths and to look our own pid up in a readdir stream.
fn put_dec_into(out: &mut [u8], mut v: u64) -> usize {
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
    let n = i;
    if n > out.len() {
        return 0;
    }
    while i > 0 {
        i -= 1;
        out[n - 1 - i] = tmp[i];
    }
    n
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

    // 6b2. environ (V-4b-6): the environment as a NUL-separated block. The probe
    //      SETS the variable itself first, so the assertion is exact and does not
    //      depend on what the boot chain happened to leave in our inherited /env.
    //      That makes this the whole chain in one read: a /env write reaches the
    //      kernel Env, /proc/<pid>/environ renders it (through the owner gate --
    //      the diorama is SYSTEM and so are we), and the diorama passes it
    //      through. None of it is reachable from the diorama's own selftest,
    //      which has no environment of its own to render.
    {
        let edir = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/env".as_ptr(), 4, T_OPATH) };
        if edir < 0 {
            fail("open /env");
        }
        let v = unsafe { t_walk_create(edir, b"DIOENVTEST".as_ptr(), 10, T_OWRITE, 0o644) };
        if v < 0 {
            fail("create /env/DIOENVTEST");
        }
        if unsafe { t_write(v, b"v4b6-ok".as_ptr(), 7) } != 7 {
            fail("write /env/DIOENVTEST");
        }
        let _ = unsafe { t_close(v) };
        let _ = unsafe { t_close(edir) };

        let mut envbuf = [0u8; 4096];
        let en = match read_all(b"/dio/self/environ", &mut envbuf) {
            Some(n) => n,
            None => fail("open /dio/self/environ"),
        };
        let env = &envbuf[..en];
        // The record, terminator included -- so this pins the ENCODING, not just
        // that the name appears somewhere.
        if !contains(env, b"DIOENVTEST=v4b6-ok\0") {
            fail("environ has no DIOENVTEST=v4b6-ok record");
        }
        // Linux's block ends on a record boundary; a consumer splitting on NUL
        // must not find a headless tail.
        if en == 0 || env[en - 1] != 0 {
            fail("environ does not end on a record terminator");
        }
        t_putstr("diorama-probe: environ bytes=");
        put_dec(en as u64);
        t_putstr("\n");
    }

    // 6c. per-pid (V-4b-3): the same files under our OWN numeric dir. This is
    //     the leg the selftest cannot reach -- it proves the numeric walk, the
    //     native existence check behind it, and that a per-pid render reaches
    //     the same Proc /self does. Reading OUR pid keeps the assertion exact
    //     without depending on what else is running.
    let me = libthyla_rs::identity::pid();
    let mut dbuf = [0u8; 64];
    let dn = {
        let mut i = 0;
        for &c in b"/dio/" {
            dbuf[i] = c;
            i += 1;
        }
        i += put_dec_into(&mut dbuf[i..], me as u64);
        i
    };
    let mut xbuf = [0u8; 128];
    {
        let mut p = [0u8; 96];
        p[..dn].copy_from_slice(&dbuf[..dn]);
        let mut pn = dn;
        for &c in b"/exe" {
            p[pn] = c;
            pn += 1;
        }
        let xn = match read_all(&p[..pn], &mut xbuf) {
            Some(n) => n,
            None => fail("open /dio/<pid>/exe"),
        };
        if xn != want.len() || &xbuf[..xn] != want {
            fail("per-pid exe mismatch");
        }
    }
    // V-4b-6: environ is NOT served under /<pid>, even our own. The diorama is
    // SYSTEM, so the kernel would let it read any SYSTEM Proc's environ and it
    // would then hand those bytes to a client of any principal -- the section 6.2
    // deputy-as-authority leak. /self is the only sound target. This is the LIVE
    // proof of the omission; the diorama's selftest pins the walk table.
    {
        let mut p = [0u8; 96];
        p[..dn].copy_from_slice(&dbuf[..dn]);
        let mut pn = dn;
        for &c in b"/environ" {
            p[pn] = c;
            pn += 1;
        }
        let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, p.as_ptr(), pn, T_OREAD) };
        if fd >= 0 {
            let _ = unsafe { t_close(fd) };
            fail("/dio/<pid>/environ resolved -- the cross-principal leak is back");
        }
    }
    // status via the per-pid path takes the NATIVE id parse (the /self path
    // uses the kernel-stamped peer instead), so this is the only place that
    // branch runs. Uid must be present and must agree with our own.
    {
        let mut p = [0u8; 96];
        p[..dn].copy_from_slice(&dbuf[..dn]);
        let mut pn = dn;
        for &c in b"/status" {
            p[pn] = c;
            pn += 1;
        }
        let mut sb = [0u8; 512];
        let sn = match read_all(&p[..pn], &mut sb) {
            Some(n) => n,
            None => fail("open /dio/<pid>/status"),
        };
        if !contains(&sb[..sn], b"Uid:\t") {
            fail("per-pid status has no Uid (the native id parse failed)");
        }
        // Match the whole "Uid:\t<uid>\t" prefix, not the bare number: the pid
        // and the uid can coincide, and a bare substring search would then pass
        // on the Pid line while the Uid line said something else entirely.
        let mut uid_pat = [0u8; 32];
        let mut up = 0usize;
        for &c in b"Uid:\t" {
            uid_pat[up] = c;
            up += 1;
        }
        up += put_dec_into(&mut uid_pat[up..], libthyla_rs::identity::uid() as u64);
        uid_pat[up] = b'\t';
        up += 1;
        if !contains(&sb[..sn], &uid_pat[..up]) {
            fail("per-pid status Uid disagrees with getuid");
        }
    }
    // A pid that cannot exist must be ENOENT, not a directory of empty files --
    // that is how a Linux consumer detects that a process is gone.
    {
        let gone = b"/dio/4294967295/exe";
        let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, gone.as_ptr(), gone.len(), T_OREAD) };
        if fd >= 0 {
            let _ = unsafe { t_close(fd) };
            fail("a nonexistent pid RESOLVED");
        }
    }
    // The root enumerates the live pids, so a `ps` that readdirs /proc sees
    // them. Our own pid must be among them.
    {
        let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/dio".as_ptr(), 4, T_OREAD) };
        if fd < 0 {
            fail("open /dio for readdir");
        }
        let mut want_pid = [0u8; 24];
        let wp = put_dec_into(&mut want_pid, me as u64);
        let mut found = false;
        let mut rounds = 0;
        loop {
            let mut db = [0u8; 2048];
            let n = unsafe { libthyla_rs::t_readdir(fd, db.as_mut_ptr(), db.len()) };
            if n <= 0 || rounds > 16 {
                break;
            }
            rounds += 1;
            // 9P2000.L dirent: qid(13) offset(8) type(1) namelen(2) name.
            let mut off = 0usize;
            let end = n as usize;
            while off + 24 <= end {
                let nl = db[off + 22] as usize | ((db[off + 23] as usize) << 8);
                let ns = off + 24;
                if ns + nl > end {
                    break;
                }
                if &db[ns..ns + nl] == &want_pid[..wp] {
                    found = true;
                }
                off = ns + nl;
            }
            if found {
                break;
            }
        }
        let _ = unsafe { t_close(fd) };
        if !found {
            fail("root readdir did not list our own pid");
        }
    }

    // 6d. sys/kernel (V-4b-3): the phenotype's self-description.
    {
        let mut ob = [0u8; 64];
        let on = match read_all(b"/dio/sys/kernel/ostype", &mut ob) {
            Some(n) => n,
            None => fail("open /dio/sys/kernel/ostype"),
        };
        if &ob[..on] != b"Linux\n" {
            fail("ostype is not Linux");
        }
        let mut rb = [0u8; 64];
        let rn = match read_all(b"/dio/sys/kernel/osrelease", &mut rb) {
            Some(n) => n,
            None => fail("open /dio/sys/kernel/osrelease"),
        };
        // A Linux consumer parses this as <major>.<minor>...; a major below 4
        // is where glibc starts refusing to run at all.
        if rn < 6 || !rb[0].is_ascii_digit() || rb[1] != b'.' || rb[0] - b'0' < 4 {
            fail("osrelease would not satisfy a glibc kernel check");
        }
        let mut hb = [0u8; 64];
        let hn = match read_all(b"/dio/sys/kernel/hostname", &mut hb) {
            Some(n) => n,
            None => fail("open /dio/sys/kernel/hostname"),
        };
        // Matches what native `uname -n` reports -- one answer, not two.
        if &hb[..hn] != b"(none)\n" {
            fail("hostname disagrees with uname");
        }
    }

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

    t_putstr("diorama-probe: PASS (/self/exe=/bin/diorama-probe; cmdline+status+cwd+maps+environ+meminfo+uptime OK; per-pid+enum+sys/kernel OK; write refused)\n");
    unsafe { t_exits(0) }
}
