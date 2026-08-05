// /bin/viv-probe -- the V-7 in-container gate (docs/VIVARIUM.md section 7.2).
//
// Runs as the ENTRYPOINT of a viv-assembled container (the host-baked
// /vivarium/probe bundle) and proves the container properties from the
// inside -- the vantage no kernel unit test or host-side probe can reach:
//
//   1. the bundle rootfs is `/` (its marker reads back; host paths do NOT
//      resolve -- the ABSENT set is checked first, per the pinned rule that a
//      badly-broken system must not be able to satisfy the assertion);
//   2. /proc + /sys serve from the per-container diorama (Linux shapes);
//   3. host pids outside the container tree neither enumerate nor resolve
//      (the enumeration is EXACTLY {self}: the runner, its diorama, joey,
//      login -- all invisible);
//   4. host /srv names are unreachable;
//   5. /net is absent (this bundle does not grant it);
//   6. the container principal is the INVOKER's (compared against the
//      VIV_EXPECT_UID the bundle manifest carries -- the manifest is the
//      gate's fixture, so the expectation travels with the bundle, not
//      hardcoded here). This is the checkable half of the I-32 posture: the
//      floor binds by principal, and viv minted no new/exempt identity. (In
//      the boot gate the invoker is joey = PRINCIPAL_SYSTEM, which is
//      I-32-EXEMPT, so an over-limit probe would be vacuous -- the honest leg
//      is the principal identity itself.)
//   7. the /dev leaf binds are LIVE files, not just resolvable names
//      (/dev/zero reads zeros, /dev/null eats writes and reads EOF) -- the
//      file-over-file bind mechanism proven end to end.
//
// Exit 0 + "viv-probe: PASS" only if every leg holds; the first failure
// prints its leg and exits 1 (viv propagates the status; joey's boot leg is
// fatal on nonzero).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::{
    t_close, t_exits, t_getpid, t_getuid, t_open, t_putstr, t_read, t_write, T_OPATH, T_OREAD,
    T_OWRITE, T_WALK_OPEN_FROM_ROOT,
};

fn fail(leg: &str) -> ! {
    t_putstr("viv-probe: FAIL: ");
    t_putstr(leg);
    t_putstr("\n");
    unsafe { t_exits(1) }
}

fn open_ro(path: &str) -> i64 {
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OREAD) }
}

fn resolves(path: &str) -> bool {
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OPATH) };
    if fd >= 0 {
        let _ = unsafe { t_close(fd) };
        return true;
    }
    false
}

/// Read up to `buf.len()` bytes of `path`. None on open/read failure.
fn read_some(path: &str, buf: &mut [u8]) -> Option<usize> {
    let fd = open_ro(path);
    if fd < 0 {
        return None;
    }
    let mut total = 0usize;
    while total < buf.len() {
        let n = unsafe { t_read(fd, buf[total..].as_mut_ptr(), buf.len() - total) };
        if n < 0 {
            let _ = unsafe { t_close(fd) };
            return None;
        }
        if n == 0 {
            break;
        }
        total += n as usize;
    }
    let _ = unsafe { t_close(fd) };
    Some(total)
}

fn contains(hay: &[u8], needle: &[u8]) -> bool {
    hay.len() >= needle.len() && hay.windows(needle.len()).any(|w| w == needle)
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // --- leg 1a: the host is ABSENT (checked FIRST: every one of these
    //     resolves in the invoker's namespace, so a viv that failed to chroot
    //     fails here rather than passing on the marker by accident).
    for host in ["/bin/joey", "/bin/viv", "/vivarium", "/ctl", "/dio", "/goroot", "/srv"] {
        if resolves(host) {
            fail(host);
        }
    }
    // /srv by NAME too: the strongest form is that no /srv path prefix exists
    // at all (above), so the posted names cannot be reached either.
    if resolves("/srv/viv-dio") || resolves("/srv/diorama") || resolves("/srv/corvus") {
        fail("a host /srv name resolved");
    }

    // --- leg 1b: the bundle rootfs is `/`.
    let mut mbuf = [0u8; 128];
    let n = read_some("/etc/viv-marker", &mut mbuf).unwrap_or_else(|| fail("read /etc/viv-marker"));
    if !contains(&mbuf[..n], b"thylacine-vivarium-probe-bundle") {
        fail("marker content");
    }

    // --- leg 2: /proc + /sys are the diorama's Linux shapes.
    let mut buf = [0u8; 4096];
    let n = read_some("/proc/self/status", &mut buf).unwrap_or_else(|| fail("read /proc/self/status"));
    if !contains(&buf[..n], b"Name:") || !contains(&buf[..n], b"Pid:") {
        fail("/proc/self/status shape");
    }
    let n = read_some("/proc/meminfo", &mut buf).unwrap_or_else(|| fail("read /proc/meminfo"));
    if !contains(&buf[..n], b"MemTotal:") {
        fail("/proc/meminfo shape");
    }
    let n = read_some("/sys/devices/system/cpu/online", &mut buf)
        .unwrap_or_else(|| fail("read /sys cpu online"));
    if n == 0 {
        fail("/sys cpu online empty");
    }

    // --- leg 3: the pid view is exactly {self}.
    let my_pid = unsafe { t_getpid() } as u64;
    let self_dir = format!("/proc/{}", my_pid);
    if !resolves(&self_dir) {
        fail("own pid does not resolve");
    }
    if resolves("/proc/1") {
        fail("host pid 1 resolves");
    }
    // Enumerate /proc and collect the numeric entries. The listing must be
    // exactly {my_pid}: the runner, its diorama, and every host Proc are
    // filtered out by the container-tree membership.
    let mut listed: Vec<u64> = Vec::new();
    match libthyla_rs::fs::read_dir("/proc") {
        Ok(rd) => {
            for ent in rd.flatten() {
                let name = ent.file_name();
                if !name.is_empty() && name.bytes().all(|c| c.is_ascii_digit()) {
                    if let Ok(v) = name.parse::<u64>() {
                        listed.push(v);
                    }
                }
            }
        }
        Err(_) => fail("readdir /proc"),
    }
    if listed.len() != 1 || listed[0] != my_pid {
        let msg = format!("pid enumeration not exactly self ({} entries)", listed.len());
        t_putstr("viv-probe: FAIL: ");
        t_putstr(&msg);
        t_putstr("\n");
        unsafe { t_exits(1) }
    }

    // --- leg 4 rode leg 1a (/srv has no path into this namespace at all).

    // --- leg 5: /net absent (this bundle grants nothing). The anchor dir
    //     exists in the rootfs; the NETWORK must not be behind it.
    if resolves("/net/tcp") || resolves("/net/tcp/clone") || resolves("/net/cs") {
        fail("/net reachable without a grant");
    }

    // --- leg 6: the container principal is the invoker's.
    match libthyla_rs::env::var("VIV_EXPECT_UID") {
        Some(want) => {
            let want: u64 = match want.trim().parse() {
                Ok(v) => v,
                Err(_) => fail("VIV_EXPECT_UID unparsable"),
            };
            let got = unsafe { t_getuid() } as u64;
            if got != want {
                fail("principal is not the invoker's");
            }
        }
        None => fail("VIV_EXPECT_UID unset (env bind or manifest env broken)"),
    }
    // The manifest env is EXACT: nothing inherited beyond it. The probe
    // manifest carries exactly one variable, so a second one is a leak.
    let mut env_names: Vec<String> = Vec::new();
    if let Ok(rd) = libthyla_rs::fs::read_dir("/env") {
        for ent in rd.flatten() {
            env_names.push(String::from(ent.file_name()));
        }
    }
    if env_names.len() != 1 || env_names[0] != "VIV_EXPECT_UID" {
        fail("environment is not exactly the manifest's");
    }

    // --- leg 7: the /dev leaf binds are live.
    let mut z = [0xAAu8; 16];
    let n = read_some("/dev/zero", &mut z).unwrap_or_else(|| fail("read /dev/zero"));
    if n != 16 || z.iter().any(|&b| b != 0) {
        fail("/dev/zero content");
    }
    let nullw = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, "/dev/null".as_ptr(), 9, T_OWRITE) };
    if nullw < 0 {
        fail("open /dev/null for write");
    }
    if unsafe { t_write(nullw, b"sink".as_ptr(), 4) } != 4 {
        fail("/dev/null write");
    }
    let _ = unsafe { t_close(nullw) };
    let mut nb = [0u8; 4];
    if read_some("/dev/null", &mut nb) != Some(0) {
        fail("/dev/null read not EOF");
    }

    t_putstr("viv-probe: PASS\n");
    0
}
