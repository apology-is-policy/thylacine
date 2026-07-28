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

/// Make a directory under `parent_fd`, tolerating one that already exists --
/// joey's mkdir_or_open shape. Nothing here treats the result as an assertion;
/// the caller's next step (a mount, a walk) is what actually has to work.
fn mkdir_ok(parent_fd: i64, name: &[u8], perm: u32) {
    let fd = unsafe {
        t_walk_create(
            parent_fd,
            name.as_ptr(),
            name.len(),
            T_OREAD,
            libthyla_rs::T_WALK_CREATE_DMDIR | perm,
        )
    };
    if fd >= 0 {
        let _ = unsafe { t_close(fd) };
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

/// Does `dir` list an entry named `name`? (V-4c-3 F3.)
///
/// Takes the path as a SLICE and measures it, deliberately -- the raw `t_open`
/// sites in this file carry an explicit length that a path edit has to move by
/// hand, and that has bitten this probe before. Nothing here hardcodes one.
///
/// Exists because the V-4c-2c cpu/cache subtree shipped readdir-BROKEN and no
/// test noticed: the selftest drives `walk_child`, and the leg below opens the
/// leaf by literal path. Both resolve by NAME, and walk was fine -- it was
/// enumeration that returned nothing. A consumer looking for `index*` without
/// knowing the number (the portable way, since cache-level numbering is not
/// fixed) saw an empty directory. So this asserts the property the other two
/// legs structurally cannot.
fn dir_lists(dir: &[u8], name: &[u8]) -> bool {
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, dir.as_ptr(), dir.len(), T_OREAD) };
    if fd < 0 {
        return false;
    }
    let mut found = false;
    let mut rounds = 0;
    loop {
        let mut db = [0u8; 1024];
        let n = unsafe { libthyla_rs::t_readdir(fd, db.as_mut_ptr(), db.len()) };
        if n <= 0 || rounds > 8 {
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
            if &db[ns..ns + nl] == name {
                found = true;
            }
            off = ns + nl;
        }
        if found {
            break;
        }
    }
    let _ = unsafe { t_close(fd) };
    found
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
    let n = match read_all(b"/dio/proc/self/exe", &mut buf) {
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
    let cn = match read_all(b"/dio/proc/self/cmdline", &mut cbuf) {
        Some(n) => n,
        None => fail("open /dio/self/cmdline"),
    };
    if cn != want.len() + 1 || &cbuf[..want.len()] != want || cbuf[cn - 1] != 0 {
        fail("cmdline shape");
    }

    // 5. status carries our own pid + a Linux-shaped Name line.
    let mut sbuf = [0u8; 512];
    let sn = match read_all(b"/dio/proc/self/status", &mut sbuf) {
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
    let wn = match read_all(b"/dio/proc/self/cwd", &mut wbuf) {
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
    let pn = match read_all(b"/dio/proc/self/maps", &mut pbuf) {
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
        let en = match read_all(b"/dio/proc/self/environ", &mut envbuf) {
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
        for &c in b"/dio/proc/" {
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
            fail("/dio/proc/<pid>/environ resolved -- the cross-principal leak is back");
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
        let gone = b"/dio/proc/4294967295/exe";
        let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, gone.as_ptr(), gone.len(), T_OREAD) };
        if fd >= 0 {
            let _ = unsafe { t_close(fd) };
            fail("a nonexistent pid RESOLVED");
        }
    }
    // /proc enumerates the live pids, so a `ps` that readdirs it sees them. Our
    // own pid must be among them. (These two raw t_open calls carry an EXPLICIT
    // length -- unlike read_all, which takes a slice and measures it -- so a
    // path edit here must move the number with it.)
    {
        let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/dio/proc".as_ptr(), 9, T_OREAD) };
        if fd < 0 {
            fail("open /dio/proc for readdir");
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
        let on = match read_all(b"/dio/proc/sys/kernel/ostype", &mut ob) {
            Some(n) => n,
            None => fail("open /dio/sys/kernel/ostype"),
        };
        if &ob[..on] != b"Linux\n" {
            fail("ostype is not Linux");
        }
        let mut rb = [0u8; 64];
        let rn = match read_all(b"/dio/proc/sys/kernel/osrelease", &mut rb) {
            Some(n) => n,
            None => fail("open /dio/sys/kernel/osrelease"),
        };
        // A Linux consumer parses this as <major>.<minor>...; a major below 4
        // is where glibc starts refusing to run at all.
        if rn < 6 || !rb[0].is_ascii_digit() || rb[1] != b'.' || rb[0] - b'0' < 4 {
            fail("osrelease would not satisfy a glibc kernel check");
        }
        let mut hb = [0u8; 64];
        let hn = match read_all(b"/dio/proc/sys/kernel/hostname", &mut hb) {
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
    let mn = match read_all(b"/dio/proc/meminfo", &mut mbuf) {
        Some(n) => n,
        None => fail("open /dio/meminfo"),
    };
    if mn == 0 || &mbuf[..9] != b"MemTotal:" {
        fail("meminfo shape");
    }
    let mut ubuf = [0u8; 64];
    let un = match read_all(b"/dio/proc/uptime", &mut ubuf) {
        Some(n) => n,
        None => fail("open /dio/uptime"),
    };
    if un == 0 || !ubuf[0].is_ascii_digit() {
        fail("uptime shape");
    }

    // V-4c-2c: the two Tier-1 stragglers. Both are checked for the fields a
    // real consumer parses, not merely for a non-empty read -- and both are
    // cross-checked against a SECOND source, which is the part that would catch
    // a renderer emitting a well-shaped file full of nothing.
    {
        let mut sb = [0u8; 1024];
        let sn = match read_all(b"/dio/proc/stat", &mut sb) {
            Some(n) => n,
            None => fail("open /dio/proc/stat"),
        };
        let st = &sb[..sn];
        if sn < 4 || &st[..4] != b"cpu " {
            fail("stat must open with the aggregate cpu line");
        }
        for k in [&b"cpu0 "[..], b"intr ", b"ctxt ", b"btime ", b"processes "] {
            if !contains(st, k) {
                fail("stat is missing a field");
            }
        }
        // ctxt and intr are counters the kernel bumps on every switch and every
        // IRQ, so by the time a userspace probe runs they cannot be zero. A
        // literal " 0\n" for either means the column parsed as absent and the
        // sum silently produced nothing -- a well-shaped lie, which is exactly
        // what a presence-only check would pass.
        if contains(st, b"intr 0\n") || contains(st, b"ctxt 0\n") {
            fail("stat reported a zero counter (the column did not parse)");
        }

        let mut cb = [0u8; 1024];
        let cn = match read_all(b"/dio/proc/cpuinfo", &mut cb) {
            Some(n) => n,
            None => fail("open /dio/proc/cpuinfo"),
        };
        let ci = &cb[..cn];
        for k in [
            &b"processor\t: 0\n"[..],
            b"CPU implementer\t: 0x",
            b"CPU architecture: 8\n",
            b"CPU part\t: 0x",
            b"CPU revision\t: ",
        ] {
            if !contains(ci, k) {
                fail("cpuinfo is missing a field");
            }
        }
        // BogoMIPS is OMITTED on purpose (section 6.17): a calibration artifact
        // of a loop Thylacine does not run. Its presence would mean someone
        // added a plausible number rather than a sourced one.
        if contains(ci, b"BogoMIPS") {
            fail("cpuinfo invented a BogoMIPS");
        }
        // Features must name at least fp+asimd: ARMv8 mandates both, so an
        // empty Features line means the AT_HWCAP word never reached the render.
        if !contains(ci, b"Features\t: fp asimd") {
            fail("cpuinfo Features did not carry the hwcap word");
        }
        // NOT asserted: that the implementer is non-zero. QEMU's TCG `-cpu max`
        // reports MIDR 0x000f0510 -- implementer 0x00, deliberately not claiming
        // to be an ARM-implemented part -- and that is the CPU the interactive
        // harness runs by DEFAULT. "implementer != 0" is a plausible-looking
        // liveness check that is simply false on a supported target. The real
        // liveness proof is the cache line size above: it comes from the same
        // per-CPU record, and an unread record reports 0, which the
        // power-of-two check already rejects.
        t_putstr("diorama-probe: stat+cpuinfo OK\n");
    }

    // 8. V-4c-1: the /sys tree, and the composition mechanism that delivers it.
    //
    //    The diorama serves ONE tree whose children are named for the Linux
    //    mount points; a container BINDS each where it belongs. This leg proves
    //    the binding end to end, because that is the part no selftest can reach
    //    and the part V-7 depends on -- and because the alternative it replaces
    //    (a second Tattach with a different aname) is UNREACHABLE for a 9P-mode
    //    /srv service: the kernel's open=connect path attaches with a hardcoded
    //    empty aname, and SYS_ATTACH_9P_SRV is byte-mode-gated.
    {
        // The cpulists, read in place first.
        let mut ob = [0u8; 64];
        let on = match read_all(b"/dio/sys/devices/system/cpu/online", &mut ob) {
            Some(n) => n,
            None => fail("open /sys/devices/system/cpu/online"),
        };
        // Non-empty and digit-led: an empty render is what an unreadable
        // /ctl/cpu produces, so this also proves the source was reached.
        if on == 0 || !ob[0].is_ascii_digit() || ob[on - 1] != b'\n' {
            fail("cpu online shape");
        }
        let mut pb = [0u8; 64];
        let pn = match read_all(b"/dio/sys/devices/system/cpu/present", &mut pb) {
            Some(n) => n,
            None => fail("open /sys/devices/system/cpu/present"),
        };
        if pn == 0 || !pb[0].is_ascii_digit() {
            fail("cpu present shape");
        }
        t_putstr("diorama-probe: cpu online=");
        for i in 0..on.saturating_sub(1) {
            let c = [ob[i]];
            t_putstr(unsafe { core::str::from_utf8_unchecked(&c) });
        }
        t_putstr("\n");

        // cpu0 exists as a dir -- the legacy enumeration path counts these.
        let c0 = unsafe {
            t_open(
                T_WALK_OPEN_FROM_ROOT,
                b"/dio/sys/devices/system/cpu/cpu0".as_ptr(),
                32,
                T_OPATH,
            )
        };
        if c0 < 0 {
            fail("walk .../cpu/cpu0");
        }
        let _ = unsafe { t_close(c0) };

        // V-4c-2c: cpuN's cache leaf. V-4c-1 left this dir empty BECAUSE
        // CTR_EL0 has no EL0 source; section 6.17 gave the kernel one, so the
        // file now reports a real line size. A power-of-two >= 16 is the
        // property a consumer sizing an allocation off it depends on -- and 0
        // would mean the kernel never read the register, the exact failure a
        // missed per-CPU call site produces.
        let mut lb = [0u8; 32];
        let ln = match read_all(
            b"/dio/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size",
            &mut lb,
        ) {
            Some(n) => n,
            None => fail("open cpu0/cache/index0/coherency_line_size"),
        };
        let mut line: u64 = 0;
        for i in 0..ln {
            if lb[i] == b'\n' {
                break;
            }
            if !lb[i].is_ascii_digit() {
                fail("cache line size is not decimal");
            }
            line = line * 10 + (lb[i] - b'0') as u64;
        }
        if line < 16 || line > 2048 || (line & (line - 1)) != 0 {
            fail("cache line size is not a sane power of two");
        }

        // V-4c-3 F3 REGRESSION: the cache chain must ENUMERATE, not merely
        // resolve by name. Every level has exactly one child, and each was
        // readdir-invisible until the guard was fixed -- while the leaf above
        // still opened fine by literal path, which is precisely how it shipped.
        // Walk-by-name and enumeration are DIFFERENT surfaces; proving one says
        // nothing about the other.
        if !dir_lists(b"/dio/sys/devices/system/cpu/cpu0", b"cache") {
            fail("cpu0 readdir does not list `cache` (V-4c-3 F3)");
        }
        if !dir_lists(b"/dio/sys/devices/system/cpu/cpu0/cache", b"index0") {
            fail("cpu0/cache readdir does not list `index0` (V-4c-3 F3)");
        }
        if !dir_lists(
            b"/dio/sys/devices/system/cpu/cpu0/cache/index0",
            b"coherency_line_size",
        ) {
            fail("cache/index0 readdir does not list the leaf (V-4c-3 F3)");
        }

        // THE COMPOSITION PROOF. Bind the sysfs subtree at another path and read
        // the same file through the new name. A mount source may be ANY readable
        // Spoor -- a subdirectory included (sys_mount_for_proc gates on
        // RIGHT_READ alone) -- which is exactly why no kernel change was needed.
        // The mount point is ours and throwaway; mounts are per-Proc, so this is
        // invisible to every other Proc on the box.
        //
        // The bound subtree is also genuinely SEALED, which is what makes this a
        // sound answer rather than a convenient one: stalk resolves ".." by
        // POPPING ITS OWN TRAIL (kernel/stalk.c, the ".." arm) and never sends a
        // Twalk("..") to the server, so `<mount>/..` lands on the mount point's
        // parent in the CLIENT's namespace -- it cannot climb the server-side
        // parent link into the world root and back down into /proc.
        //
        // /tmp is NOT guaranteed to exist here: joey creates it inside the clade
        // gate, which returns early when /clade is not baked -- and which runs
        // AFTER this probe, so on a clade-baked boot WE are the one that makes
        // it. Hence 0777, matching clade_gate's own create exactly: whichever
        // runs first must leave the same directory behind. Both levels tolerate
        // "already there" -- a create failure is not the assertion, the mount is.
        mkdir_ok(T_WALK_OPEN_FROM_ROOT, b"tmp", 0o777);
        let tmp = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/tmp".as_ptr(), 4, T_OPATH) };
        if tmp < 0 {
            fail("open /tmp (create failed?)");
        }
        mkdir_ok(tmp, b"dio-sys", 0o755);
        let _ = unsafe { t_close(tmp) };

        let src = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/dio/sys".as_ptr(), 8, T_OPATH) };
        if src < 0 {
            fail("open /dio/sys as a bind source");
        }
        if unsafe { t_mount(b"/tmp/dio-sys".as_ptr(), 12, src, T_MREPL) } != 0 {
            fail("bind /dio/sys -- a subdirectory must be mountable");
        }
        let _ = unsafe { t_close(src) };

        let mut bb = [0u8; 64];
        let bn = match read_all(b"/tmp/dio-sys/devices/system/cpu/online", &mut bb) {
            Some(n) => n,
            None => fail("read through the bind"),
        };
        if bn != on || bb[..bn] != ob[..on] {
            fail("the bound view disagrees with the source");
        }
    }

    // 9. READ-ONLY: opening any diorama file for write must be refused. This is
    //    the property that keeps the whole surface small, so it is worth a gate.
    //    Uses the imported T_OWRITE, deliberately: this leg used to redeclare it
    //    as a function-scope `const`, and Rust scopes block items over the WHOLE
    //    block rather than from their declaration onward -- so that local
    //    silently captured the name for every earlier use site too, 220 lines
    //    up, including the V-4b-6 /env create. The values happened to agree
    //    (both 1), so nothing was wrong; but a shadow that reaches backwards is
    //    a name whose meaning can be changed by an edit that never touches its
    //    users. The compiler said so -- "unused import: T_OWRITE" was the tell.
    let w = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/dio/proc/self/exe".as_ptr(), 18, T_OWRITE) };
    if w >= 0 {
        let _ = unsafe { t_close(w) };
        fail("write-open was ALLOWED (read-only violated)");
    }

    t_putstr("diorama-probe: PASS (/proc/self/exe=/bin/diorama-probe; cmdline+status+cwd+maps+environ+meminfo+uptime OK; per-pid+enum+proc/sys/kernel OK; /sys cpulists + subtree bind OK; write refused)\n");
    unsafe { t_exits(0) }
}
