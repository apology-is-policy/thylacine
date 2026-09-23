// /capacity-probe -- B-1a' (capacity): the range detach, the lifted reservation
// cap and the accounting, driven from EL0 through the native syscalls (ARCH 6.5
// "Range detach" + "Capacity, and the I-32 default"). Spawned by joey at boot;
// prints one "capacity-probe: <leg> OK" line per leg and exits 0, or
// "capacity-probe: FAIL <leg> got=<n> want=<m>" and exits 1.
//
// The kernel suite proves these claims against the kernel entries. What only
// EL0 can show is the memory bar as a PROGRAM sees it: the census the kernel
// publishes (/proc/<pid>/status `pages:`) rises with what the program touches
// and falls back to exactly where it started when the program gives the memory
// back -- across a 4 GiB reservation the old cap refused, through a range
// detach that cuts across the pieces a protect left.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::ptr::{read_volatile, write_volatile};
use libthyla_rs::{
    t_burrow_attach, t_burrow_detach, t_burrow_protect, t_burrow_reserve, t_close, t_getpid,
    t_open, t_putstr, t_read, T_BURROW_PROT_READ, T_BURROW_PROT_WRITE, T_OREAD,
    T_WALK_OPEN_FROM_ROOT,
};

const PAGE: u64 = 4096;
const MIB: u64 = 1 << 20;
const GIB: u64 = 1 << 30;
const R: u64 = T_BURROW_PROT_READ;
const RW: u64 = T_BURROW_PROT_READ | T_BURROW_PROT_WRITE;
const PATTERN: u64 = 0xCAFE_F00D_0000_0000;

fn put_num(mut n: u64) {
    let mut buf = [0u8; 20];
    let mut i = buf.len();
    if n == 0 {
        i -= 1;
        buf[i] = b'0';
    }
    while n > 0 {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
    }
    // The digits are ASCII by construction.
    if let Ok(s) = core::str::from_utf8(&buf[i..]) {
        t_putstr(s);
    }
}

fn fail(leg: &str) -> i64 {
    t_putstr("capacity-probe: FAIL ");
    t_putstr(leg);
    t_putstr("\n");
    1
}

fn fail_num(leg: &str, got: u64, want: u64) -> i64 {
    t_putstr("capacity-probe: FAIL ");
    t_putstr(leg);
    t_putstr(" got=");
    put_num(got);
    t_putstr(" want=");
    put_num(want);
    t_putstr("\n");
    1
}

fn ok(leg: &str) {
    t_putstr("capacity-probe: ");
    t_putstr(leg);
    t_putstr(" OK\n");
}

// The decimal after `key` in `buf`, or None.
fn field(buf: &[u8], key: &[u8]) -> Option<u64> {
    let mut i = 0;
    while i + key.len() <= buf.len() {
        if &buf[i..i + key.len()] == key {
            let mut j = i + key.len();
            while j < buf.len() && buf[j] == b' ' {
                j += 1;
            }
            let mut v: u64 = 0;
            let mut got = false;
            while j < buf.len() && buf[j].is_ascii_digit() {
                v = v * 10 + (buf[j] - b'0') as u64;
                j += 1;
                got = true;
            }
            return if got { Some(v) } else { None };
        }
        i += 1;
    }
    None
}

// The `pages:` figure of /proc/<pid>/status (devproc names the pid, Plan 9's
// shape; there is no `self`): this address space's charged pages, data plus
// the pagemap's nodes, read fresh each time.
fn pages() -> Option<u64> {
    let pid = unsafe { t_getpid() };
    if pid <= 0 {
        return None;
    }
    let mut path = [0u8; 40];
    let mut n = 0usize;
    for &c in b"/proc/" {
        path[n] = c;
        n += 1;
    }
    let mut digits = [0u8; 20];
    let mut d = 0usize;
    let mut v = pid as u64;
    loop {
        digits[d] = b'0' + (v % 10) as u8;
        d += 1;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    while d > 0 {
        d -= 1;
        path[n] = digits[d];
        n += 1;
    }
    for &c in b"/status" {
        path[n] = c;
        n += 1;
    }
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), n, T_OREAD) };
    if fd < 0 {
        return None;
    }
    let mut buf = [0u8; 1024];
    let mut total = 0usize;
    while total < buf.len() {
        let n = unsafe { t_read(fd, buf[total..].as_mut_ptr(), buf.len() - total) };
        if n <= 0 {
            break;
        }
        total += n as usize;
    }
    let _ = unsafe { t_close(fd) };
    // The data view: `pages` carries the page tables too (charged and reclaimed
    // since the B-1a' close), and how many tables a touch grows depends on
    // where the reservation landed -- a figure a program cannot predict; and
    // the file pages of this program's own text and rodata (the Image cache's,
    // charged per leaf touched since the round-2 close), which move with what
    // the code path touched. The census a program reasons about is pages less
    // both.
    let pages = field(&buf[..total], b"pages:")?;
    let tables = field(&buf[..total], b"tables:")?;
    let file = field(&buf[..total], b"file:")?;
    Some(pages - tables - file)
}

// `pages()` must read `want`, else the leg fails with both figures printed.
macro_rules! census {
    ($leg:expr, $want:expr) => {
        match pages() {
            Some(got) if got == $want => {}
            Some(got) => return fail_num($leg, got, $want),
            None => return fail($leg),
        }
    };
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    unsafe {
        // The census is read once before the baseline is taken, so the pages
        // this probe's own stack needs for a read are resident already and
        // every later figure is a delta of the reservations alone.
        if pages().is_none() {
            return fail("census-readable");
        }
        let base = match pages() {
            Some(b) => b,
            None => return fail("census-readable"),
        };

        // 1. A 4 GiB reservation is admitted (the old cap was 256 MiB), costs
        //    nothing untouched, and the census rises by what is touched: one
        //    page per 512 MiB plus the pagemap's 13 nodes (a 2^20-slot map is
        //    three levels deep; pairs of touches share a level-1 node).
        let va = t_burrow_reserve(4 * GIB, RW, 0);
        if va <= 0 {
            return fail("reserve-4gib");
        }
        let va = va as u64;
        census!("reserve-costs-nothing", base);
        for k in 0..8u64 {
            let p = (va + k * 512 * MIB) as *mut u64;
            write_volatile(p, PATTERN | k);
            if read_volatile(p) != PATTERN | k {
                return fail("touch-read-back");
            }
        }
        census!("touch-census-rises", base + 8 + 13);
        ok("reserve-4gib-touch-census");

        // 2. The middle GiB to R: the bytes stay readable through the R piece.
        if t_burrow_protect(va + 1536 * MIB, GIB, R, 0) != 0 {
            return fail("protect-middle-r");
        }
        if read_volatile((va + 2 * GIB) as *const u64) != PATTERN | 4 {
            return fail("read-through-r-piece");
        }
        census!("protect-releases-nothing", base + 8 + 13);
        ok("protect-middle-r");

        // 3. A 2 GiB range across the pieces -- the first piece's tail, the R
        //    piece whole, the last piece's head: exactly the four pages inside
        //    it come back, with the six nodes that emptied; the survivors keep
        //    their bytes.
        if t_burrow_detach(va + GIB, 2 * GIB) != 0 {
            return fail("detach-across-pieces");
        }
        census!("detach-returns-exactly-the-range", base + 4 + 7);
        if read_volatile((va + 512 * MIB) as *const u64) != PATTERN | 1 {
            return fail("survivor-head-bytes");
        }
        if read_volatile((va + 3 * GIB) as *const u64) != PATTERN | 6 {
            return fail("survivor-tail-bytes");
        }
        ok("range-detach-across-pieces");

        // 4. The rest, in one range over the hole and both survivors: the
        //    census is back where it started.
        if t_burrow_detach(va, 4 * GIB) != 0 {
            return fail("detach-rest");
        }
        census!("census-back-to-start", base);
        // A range that maps nothing answers 0 (the Linux form), not an error.
        if t_burrow_detach(va, 4 * GIB) != 0 {
            return fail("empty-range-is-0");
        }
        ok("footprint-shrinks-to-start");

        // 5. A 512 MiB lazy reservation detaches (the old detach refused any
        //    length over 256 MiB, so such a region could never be given back).
        let r = t_burrow_reserve(512 * MIB, RW, 0);
        if r <= 0 {
            return fail("reserve-512mib");
        }
        let r = r as u64;
        write_volatile(r as *mut u64, PATTERN);
        write_volatile((r + 512 * MIB - PAGE) as *mut u64, PATTERN);
        census!("512mib-touched", base + 2 + 3);
        if t_burrow_detach(r, 512 * MIB) != 0 {
            return fail("detach-512mib");
        }
        census!("512mib-back", base);
        ok("lazy-over-256mib-detaches");

        // 6. An eager region cut into pieces stays charged until its last piece
        //    goes: a middle cut is served, refunds nothing, keeps the outer
        //    pages' bytes; the two remaining pieces give the block back.
        let e = t_burrow_attach(4 * PAGE);
        if e <= 0 {
            return fail("attach-eager");
        }
        let e = e as u64;
        for k in 0..4u64 {
            write_volatile((e + k * PAGE) as *mut u64, PATTERN | k);
        }
        census!("eager-charged", base + 4);
        if t_burrow_detach(e + PAGE, 2 * PAGE) != 0 {
            return fail("eager-middle-cut");
        }
        census!("eager-trim-refunds-nothing", base + 4);
        if read_volatile(e as *const u64) != PATTERN
            || read_volatile((e + 3 * PAGE) as *const u64) != PATTERN | 3
        {
            return fail("eager-outer-bytes");
        }
        if t_burrow_detach(e, PAGE) != 0 {
            return fail("eager-head-piece");
        }
        census!("eager-still-charged-by-last-piece", base + 4);
        if t_burrow_detach(e + 3 * PAGE, PAGE) != 0 {
            return fail("eager-last-piece");
        }
        census!("eager-back", base);
        ok("eager-pages-go-with-the-last-piece");

        // 7. The refusals: malformed shapes, and a range below the window that
        //    is not a hardware map, each -1 and each changing nothing.
        if t_burrow_detach(va + 1, PAGE) != -1 {
            return fail("unaligned");
        }
        if t_burrow_detach(va, 0) != -1 {
            return fail("len-0");
        }
        if t_burrow_detach(0x4000_0000, PAGE) != -1 {
            return fail("below-window");
        }
        census!("refusals-change-nothing", base);
        ok("refusals");
    }
    t_putstr("capacity-probe: ALL OK\n");
    0
}
