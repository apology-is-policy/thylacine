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
//
// MM-1 adds SYS_ACCOUNT's query on the caller's own account: its header checks and its
// copies can only be crossed from here. Other Procs are charged to the same account
// while this runs, so a figure is asserted only where they cannot move it past the test.
// A child account's are this Proc's alone, so those figures are exact. The spawn tail runs
// this program again as `account-child`, which checks from inside what its birth gave it.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::ptr::{read_volatile, write_volatile};
use libthyla_rs::account::{
    AccountChildRec, AccountLimitsRec, AccountQuery, ACCOUNT_CHILD, ACCOUNT_CLASSES,
    ACCOUNT_LIMIT_NONE, ACCOUNT_NORMAL, ACCOUNT_QUERY, ACCOUNT_SELF, ACCOUNT_SET_LIMITS,
    ACCOUNT_VERSION, ALLOCATE_FUNDED, CLASS_ALLOC, CLASS_DEVICE, CLASS_META, CLASS_RETAIN,
    VOUCHER_CREATE,
};
use libthyla_rs::process::Command;
use libthyla_rs::{
    t_account, t_account_child, t_account_query, t_account_query_of, t_account_set_limits,
    t_burrow_attach, t_burrow_detach, t_burrow_protect, t_burrow_reserve, t_close, t_execve,
    t_getpid, t_handle_flags_get, t_handle_flags_set, t_open, t_pipe, t_putstr, t_read,
    t_spawn_full_argv, t_wait_pid_for, TSpawnArgs, TSpawnExtAccount, T_BURROW_PROT_READ,
    T_BURROW_PROT_WRITE, T_OREAD, T_SPAWN_EXT_ACCOUNT, T_WALK_OPEN_FROM_ROOT,
};

const PAGE: u64 = 4096;
const MIB: u64 = 1 << 20;
const GIB: u64 = 1 << 30;
const R: u64 = T_BURROW_PROT_READ;
const RW: u64 = T_BURROW_PROT_READ | T_BURROW_PROT_WRITE;
const PATTERN: u64 = 0xCAFE_F00D_0000_0000;

// The registry's values, docs/ERRORS.md.
const EBADF: i64 = 9;
const EACCES: i64 = 13;
const EFAULT: i64 = 14;
const EINVAL: i64 = 22;
const ENOSPC: i64 = 28;
const EOPNOTSUPP: i64 = 95;

const RECORD: u64 = core::mem::size_of::<AccountQuery>() as u64;
const SELF: i64 = ACCOUNT_SELF as i64;
const QUERY: u64 = ACCOUNT_QUERY as u64;
// No answer the kernel gives has this in `generation`: it shows a refusal wrote nothing.
const UNTOUCHED: u64 = 0x5EED_5EED_5EED_5EED;

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

fn fail_rc(leg: &str, got: i64, want: i64) -> i64 {
    t_putstr("capacity-probe: FAIL ");
    t_putstr(leg);
    t_putstr(" got=-");
    put_num(got.unsigned_abs());
    t_putstr(" want=-");
    put_num(want.unsigned_abs());
    t_putstr("\n");
    1
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

// As much of a file as `buf` holds, read fresh.
fn read_path(path: &[u8], buf: &mut [u8]) -> Option<usize> {
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OREAD) };
    if fd < 0 {
        return None;
    }
    let mut total = 0usize;
    while total < buf.len() {
        let n = unsafe { t_read(fd, buf[total..].as_mut_ptr(), buf.len() - total) };
        if n <= 0 {
            break;
        }
        total += n as usize;
    }
    let _ = unsafe { t_close(fd) };
    Some(total)
}

// A file of this Proc's /proc/<pid> directory (devproc names the pid, Plan 9's
// shape; there is no `self`). `name` starts with its slash.
fn read_own(name: &[u8], buf: &mut [u8]) -> Option<usize> {
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
    for &c in name {
        path[n] = c;
        n += 1;
    }
    read_path(&path[..n], buf)
}

// The `pages:` figure of /proc/<pid>/status: this address space's charged pages, data
// plus the pagemap's nodes.
fn pages() -> Option<u64> {
    let mut buf = [0u8; 1024];
    let total = read_own(b"/status", &mut buf)?;
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

fn fresh_record() -> AccountQuery {
    let mut q = AccountQuery::default();
    q.h.version = ACCOUNT_VERSION;
    q.h.size = RECORD as u32;
    q.generation = UNTOUCHED;
    q
}

// The call must answer `-want` and leave the record as it found it. Nonzero on a failure.
fn refused(leg: &str, handle: i64, op: u64, q: &mut AccountQuery, size: u64, want: i64) -> i64 {
    let rec = q as *mut AccountQuery as *mut u8;
    let got = unsafe { t_account(handle, op, rec, size) };
    if got != -want {
        return fail_rc(leg, got, want);
    }
    if q.generation != UNTOUCHED {
        return fail(leg);
    }
    0
}

// SYS_ACCOUNT's query, from EL0. Zero when every leg holds.
fn account_legs() -> i64 {
    // The positive control every refusal below is one variable away from.
    let mut q = AccountQuery::default();
    let rc = t_account_query(&mut q);
    if rc != 0 {
        return fail_rc("account-query", rc, 0);
    }
    if q.h.version != ACCOUNT_VERSION || q.h.size != RECORD as u32 || q.h.flags != 0 {
        return fail("account-header-echoed");
    }
    if q.h.incarnation == 0 || q.level != ACCOUNT_NORMAL {
        return fail("account-identity-and-level");
    }
    if q.c[CLASS_DEVICE as usize].limit != u64::MAX {
        return fail("account-device-limit");
    }
    ok("account-query");

    // The header and the shape, each wrong alone.
    macro_rules! bad_record {
        ($leg:expr, $set:expr) => {{
            let mut r = fresh_record();
            let set: fn(&mut AccountQuery) = $set;
            set(&mut r);
            let f = refused($leg, SELF, QUERY, &mut r, RECORD, EINVAL);
            if f != 0 {
                return f;
            }
        }};
    }
    bad_record!("account-version-0", |r| r.h.version = 0);
    bad_record!("account-version-2", |r| r.h.version = ACCOUNT_VERSION + 1);
    bad_record!("account-size", |r| r.h.size = RECORD as u32 - 8);
    bad_record!("account-flags", |r| r.h.flags = 1);
    bad_record!("account-pad", |r| r.h.pad = 1);
    let mut r = fresh_record();
    r.h.incarnation = q.h.incarnation.wrapping_add(1);
    let f = refused(
        "account-stale-incarnation",
        SELF,
        QUERY,
        &mut r,
        RECORD,
        EINVAL,
    );
    if f != 0 {
        return f;
    }
    let mut named = fresh_record();
    named.h.incarnation = q.h.incarnation;
    let rec = &mut named as *mut AccountQuery as *mut u8;
    let rc = unsafe { t_account(SELF, QUERY, rec, RECORD) };
    if rc != 0 || named.generation == UNTOUCHED {
        return fail_rc("account-own-incarnation", rc, 0);
    }
    let mut r = fresh_record();
    for (leg, handle, op, size, want) in [
        ("account-short", SELF, QUERY, RECORD - 1, EINVAL),
        ("account-long", SELF, QUERY, RECORD + 1, EINVAL),
        ("account-op-0", SELF, 0, RECORD, EINVAL),
        (
            "account-op-past",
            SELF,
            ALLOCATE_FUNDED as u64 + 1,
            RECORD,
            EINVAL,
        ),
        (
            "account-op-unbuilt",
            SELF,
            VOUCHER_CREATE as u64,
            RECORD,
            EOPNOTSUPP,
        ),
        ("account-handle", 3, QUERY, RECORD, EBADF),
    ] {
        let f = refused(leg, handle, op, &mut r, size, want);
        if f != 0 {
            return f;
        }
    }
    ok("account-refusals");

    // The record's memory: past the user range, unmapped, and readable but not writable,
    // which is the case that gets through the header and is stopped at the copy out.
    for (leg, va) in [
        ("account-kernel-va", 0xFFFF_0000_0000_0000u64),
        ("account-unmapped", 0x4000_0000u64),
    ] {
        let rc = unsafe { t_account(SELF, QUERY, va as *mut u8, RECORD) };
        if rc != -EFAULT {
            return fail_rc(leg, rc, EFAULT);
        }
    }
    let ro = unsafe { t_burrow_reserve(PAGE, RW, 0) };
    if ro <= 0 {
        return fail("account-ro-reserve");
    }
    let ro = ro as u64;
    unsafe { core::ptr::write(ro as *mut AccountQuery, fresh_record()) };
    if unsafe { t_burrow_protect(ro, PAGE, R, 0) } != 0 {
        return fail("account-ro-protect");
    }
    let rc = unsafe { t_account(SELF, QUERY, ro as *mut u8, RECORD) };
    if rc != -EFAULT {
        return fail_rc("account-ro-record", rc, EFAULT);
    }
    if unsafe { t_burrow_detach(ro, PAGE) } != 0 {
        return fail("account-ro-detach");
    }
    ok("account-record-memory");

    // Bytes: a figure in pages cannot reach 16 MiB here, whatever else the account is
    // charged. A thread is running, so META holds its pool pages.
    let touched = 16 * MIB;
    let region = unsafe { t_burrow_reserve(touched, RW, 0) };
    if region <= 0 {
        return fail("account-region");
    }
    let region = region as u64;
    let mut off = 0;
    while off < touched {
        unsafe { write_volatile((region + off) as *mut u64, PATTERN) };
        off += PAGE;
    }
    let rc = t_account_query(&mut q);
    if rc != 0 {
        return fail_rc("account-requery", rc, 0);
    }
    let alloc = q.c[CLASS_ALLOC as usize];
    if alloc.used < touched || !alloc.used.is_multiple_of(PAGE) {
        return fail_num("account-used-in-bytes", alloc.used, touched);
    }
    if q.c[CLASS_META as usize].used == 0 {
        return fail("account-meta-holds-a-thread");
    }
    if unsafe { t_burrow_detach(region, touched) } != 0 {
        return fail("account-region-detach");
    }
    ok("account-used-in-bytes");

    // /ctl/memory names the same pool, and shows no pairing broken since boot. Every class
    // of page is live here, held by the running services, which is what a kernel test
    // cannot arrange. A block landing between the two lines reads as a difference for that
    // one read, so only a difference that outlasts every read is a finding.
    let mut mem = [0u8; 512];
    let mut n = 0;
    let mut balanced = false;
    let (mut charged, mut ledger) = (None, None);
    for _ in 0..64 {
        n = match read_path(b"/ctl/memory", &mut mem) {
            Some(n) => n,
            None => return fail("account-ctl-memory"),
        };
        charged = field(&mem[..n], b"charged:");
        ledger = field(&mem[..n], b"ledger:");
        if charged.is_some() && charged == ledger {
            balanced = true;
            break;
        }
    }
    if !balanced {
        return fail_num(
            "account-ledger-is-the-pool",
            ledger.unwrap_or(0),
            charged.unwrap_or(0),
        );
    }
    let pool = match field(&mem[..n], b"pool:") {
        Some(p) => p * PAGE,
        None => return fail("account-pool-line"),
    };
    if q.c[CLASS_ALLOC as usize].limit != pool || q.c[CLASS_META as usize].limit != pool {
        return fail_num(
            "account-limit-is-the-pool",
            q.c[CLASS_ALLOC as usize].limit,
            pool,
        );
    }
    if q.headroom > pool {
        return fail_num("account-headroom-within-the-pool", q.headroom, pool);
    }
    match field(&mem[..n], b"unpaired:") {
        Some(0) => {}
        Some(u) => return fail_num("account-unpaired-line", u, 0),
        None => return fail("account-unpaired-line"),
    }
    ok("account-ctl-memory");

    // The text view reads the same account.
    let mut text = [0u8; 512];
    let n = match read_own(b"/account", &mut text) {
        Some(n) => n,
        None => return fail("account-proc-open"),
    };
    if field(&text[..n], b"incarnation:") != Some(q.h.incarnation) {
        return fail("account-proc-incarnation");
    }
    // alloc is the first class line, and "limit " with its space is not "limiting:".
    if field(&text[..n], b"limit ") != Some(pool) {
        return fail("account-proc-limit");
    }
    ok("account-proc-file");
    account_control_legs(pool)
}

#[repr(C)]
struct AccountSpawn {
    base: TSpawnArgs,
    tail: TSpawnExtAccount,
}

fn push_num(out: &mut alloc::vec::Vec<u8>, mut n: u64) {
    let mut digits = [0u8; 20];
    let mut i = digits.len();
    loop {
        i -= 1;
        digits[i] = b'0' + (n % 10) as u8;
        n /= 10;
        if n == 0 {
            break;
        }
    }
    out.extend_from_slice(&digits[i..]);
    out.push(0);
}

fn parse_num(s: &[u8]) -> Option<u64> {
    let mut n = 0u64;
    if s.is_empty() {
        return None;
    }
    for &c in s {
        if !c.is_ascii_digit() {
            return None;
        }
        n = n.checked_mul(10)?.checked_add((c - b'0') as u64)?;
    }
    Some(n)
}

// This program again, under `handle`, told the handle's number and what it should be born
// under. Returns the pid or a negated errno. Raw, since a refusal's errno is the point.
fn spawn_under(
    handle: u32,
    flags: u32,
    expected: u64,
    told_handle: u64,
    told_incarnation: u64,
) -> i64 {
    const NAME: &[u8] = b"capacity-probe";
    let mut argv = alloc::vec::Vec::new();
    argv.extend_from_slice(b"capacity-probe\0account-child\0");
    push_num(&mut argv, told_handle);
    push_num(&mut argv, told_incarnation);
    // SAFETY: every field of TSpawnArgs is an integer, and zero is each one's default.
    let mut rec: AccountSpawn = unsafe { core::mem::zeroed() };
    rec.base.name_va = NAME.as_ptr() as u64;
    rec.base.name_len = NAME.len() as u32;
    rec.base.argv_data_va = argv.as_ptr() as u64;
    rec.base.argv_data_len = argv.len() as u32;
    rec.base.argc = 4;
    rec.base.ext_flags = T_SPAWN_EXT_ACCOUNT;
    rec.tail = TSpawnExtAccount {
        account_h: handle,
        account_flags: flags,
        incarnation: expected,
    };
    // SAFETY: the pointer is the whole record's and argv and NAME outlive the call.
    unsafe { t_spawn_full_argv(&rec as *const AccountSpawn as *const TSpawnArgs) }
}

// A child holding `fd` as its descriptor 0, which does nothing and exits.
fn spawn_holding(fd: u32) -> i64 {
    const NAME: &[u8] = b"capacity-probe";
    const ARGV: &[u8] = b"capacity-probe\0account-noop\0";
    let fds = [fd];
    // SAFETY: every field of TSpawnArgs is an integer, and zero is each one's default.
    let mut rec: TSpawnArgs = unsafe { core::mem::zeroed() };
    rec.name_va = NAME.as_ptr() as u64;
    rec.name_len = NAME.len() as u32;
    rec.argv_data_va = ARGV.as_ptr() as u64;
    rec.argv_data_len = ARGV.len() as u32;
    rec.argc = 2;
    rec.fd_list_va = fds.as_ptr() as u64;
    rec.fd_count = 1;
    // SAFETY: the pointer is the record's and everything it names outlives the call.
    unsafe { t_spawn_full_argv(&rec as *const TSpawnArgs) }
}

fn spawn_mode(mode: &[u8]) -> i64 {
    const NAME: &[u8] = b"capacity-probe";
    let mut argv = alloc::vec::Vec::new();
    argv.extend_from_slice(b"capacity-probe\0");
    argv.extend_from_slice(mode);
    argv.push(0);
    // SAFETY: every field of TSpawnArgs is an integer, and zero is each one's default.
    let mut rec: TSpawnArgs = unsafe { core::mem::zeroed() };
    rec.name_va = NAME.as_ptr() as u64;
    rec.name_len = NAME.len() as u32;
    rec.argv_data_va = argv.as_ptr() as u64;
    rec.argv_data_len = argv.len() as u32;
    rec.argc = 2;
    // SAFETY: the pointer is the record's and argv and NAME outlive the call.
    unsafe { t_spawn_full_argv(&rec as *const TSpawnArgs) }
}

// A child account's control from EL0: what the kernel suite cannot reach is the record, the
// handle number and the spawn tail. Zero when every leg holds. `pool` is section 4's default.
fn account_control_legs(pool: u64) -> i64 {
    let mut own = AccountQuery::default();
    let rc = t_account_query(&mut own);
    if rc != 0 {
        return fail_rc("account-own", rc, 0);
    }
    let mut made = 0u64;
    let h = t_account_child(ACCOUNT_SELF, 0, &mut made);
    if h < 0 {
        return fail_rc("account-child", h, 0);
    }
    let hi = h as i32;
    if unsafe { t_handle_flags_get(h) } != 1 {
        return fail("account-child-cloexec");
    }
    if made <= own.h.incarnation {
        return fail_num("account-child-incarnation", made, own.h.incarnation);
    }
    let mut q = AccountQuery::default();
    let rc = t_account_query_of(hi, made, &mut q);
    if rc != 0 {
        return fail_rc("account-child-query", rc, 0);
    }
    if q.h.incarnation != made
        || q.c[CLASS_ALLOC as usize].used != 0
        || q.c[CLASS_META as usize].used != 0
    {
        return fail("account-child-starts-empty");
    }
    if q.c[CLASS_ALLOC as usize].limit != pool || q.c[CLASS_DEVICE as usize].limit != u64::MAX {
        return fail_num(
            "account-child-default-limit",
            q.c[CLASS_ALLOC as usize].limit,
            pool,
        );
    }
    ok("account-child");

    // A handle names one account: "whatever I am" is only the self selector's to say.
    for (leg, expected) in [
        ("account-handle-unnamed", 0),
        ("account-handle-stale", made + 1),
    ] {
        let rc = t_account_query_of(hi, expected, &mut q);
        if rc != -EINVAL {
            return fail_rc(leg, rc, EINVAL);
        }
    }
    let rc = t_account_query_of(ACCOUNT_SELF, made, &mut q);
    if rc != -EINVAL {
        return fail_rc("account-self-named-wrong", rc, EINVAL);
    }
    ok("account-expected-incarnation");

    let mut lim = [ACCOUNT_LIMIT_NONE; ACCOUNT_CLASSES];
    let rc = t_account_set_limits(ACCOUNT_SELF, 0, &lim);
    if rc != -EACCES {
        return fail_rc("account-set-own", rc, EACCES);
    }
    lim[CLASS_ALLOC as usize] = 64 * MIB;
    let rc = t_account_set_limits(hi, made, &lim);
    if rc != 0 {
        return fail_rc("account-set-child", rc, 0);
    }
    let rc = t_account_query_of(hi, made, &mut q);
    if rc != 0 || q.c[CLASS_ALLOC as usize].limit != 64 * MIB {
        return fail_num(
            "account-set-reads-back",
            q.c[CLASS_ALLOC as usize].limit,
            64 * MIB,
        );
    }
    // A good class beside one past the pool, which is the root's ceiling: neither lands.
    lim[CLASS_RETAIN as usize] = MIB;
    lim[CLASS_META as usize] = pool + PAGE;
    let rc = t_account_set_limits(hi, made, &lim);
    if rc != -EACCES {
        return fail_rc("account-set-past-the-pool", rc, EACCES);
    }
    let rc = t_account_query_of(hi, made, &mut q);
    if rc != 0 || q.c[CLASS_RETAIN as usize].limit != pool {
        return fail_num(
            "account-set-all-or-nothing",
            q.c[CLASS_RETAIN as usize].limit,
            pool,
        );
    }
    // The pool is the highest limit there is, and the machine has charged some of it, so only
    // an account reading its own figures can show all of it. One class left open still reads
    // the machine, and device storage is not such a class.
    lim = [ACCOUNT_LIMIT_NONE; ACCOUNT_CLASSES];
    lim[CLASS_ALLOC as usize] = pool;
    let rc = t_account_set_limits(hi, made, &lim);
    let rq = t_account_query_of(hi, made, &mut q);
    if rc != 0 || rq != 0 || q.headroom >= pool {
        return fail_num("account-open-class-reads-the-pool", q.headroom, pool);
    }
    lim[CLASS_RETAIN as usize] = pool;
    lim[CLASS_META as usize] = pool;
    let rc = t_account_set_limits(hi, made, &lim);
    let rq = t_account_query_of(hi, made, &mut q);
    if rc != 0 || rq != 0 || q.headroom != pool {
        return fail_num("account-limited-reads-itself", q.headroom, pool);
    }

    lim = [ACCOUNT_LIMIT_NONE; ACCOUNT_CLASSES];
    lim[CLASS_ALLOC as usize] = PAGE + 1;
    let rc = t_account_set_limits(hi, made, &lim);
    if rc != 0 {
        return fail_rc("account-set-unaligned", rc, 0);
    }
    let rc = t_account_query_of(hi, made, &mut q);
    if rc != 0 || q.c[CLASS_ALLOC as usize].limit != PAGE {
        return fail_num(
            "account-set-rounds-down",
            q.c[CLASS_ALLOC as usize].limit,
            PAGE,
        );
    }
    // Each operation's record, one size away.
    let mut r = AccountLimitsRec::default();
    r.h.version = ACCOUNT_VERSION;
    r.h.size = core::mem::size_of::<AccountLimitsRec>() as u32 - 8;
    r.h.incarnation = made;
    let rc = unsafe {
        t_account(
            h,
            ACCOUNT_SET_LIMITS as u64,
            &mut r as *mut AccountLimitsRec as *mut u8,
            core::mem::size_of::<AccountLimitsRec>() as u64 - 8,
        )
    };
    if rc != -EINVAL {
        return fail_rc("account-set-short", rc, EINVAL);
    }
    let mut r = AccountChildRec::default();
    r.h.version = ACCOUNT_VERSION;
    r.h.size = core::mem::size_of::<AccountChildRec>() as u32;
    r.h.flags = 1;
    let rc = unsafe {
        t_account(
            SELF,
            ACCOUNT_CHILD as u64,
            &mut r as *mut AccountChildRec as *mut u8,
            core::mem::size_of::<AccountChildRec>() as u64,
        )
    };
    if rc != -EINVAL {
        return fail_rc("account-child-flags", rc, EINVAL);
    }
    ok("account-limits");

    // Subdivision: a child under a child pays its parent, and hands the page back.
    let mut below = 0u64;
    let g = t_account_child(hi, made, &mut below);
    if g < 0 {
        return fail_rc("account-grandchild", g, 0);
    }
    let rc = t_account_query_of(hi, made, &mut q);
    if rc != 0 || q.c[CLASS_META as usize].used != PAGE {
        return fail_num(
            "account-child-pays-its-parent",
            q.c[CLASS_META as usize].used,
            PAGE,
        );
    }
    if unsafe { t_close(g) } != 0 {
        return fail("account-grandchild-close");
    }
    let rc = t_account_query_of(hi, made, &mut q);
    if rc != 0 || q.c[CLASS_META as usize].used != 0 {
        return fail_num("account-close-refunds", q.c[CLASS_META as usize].used, 0);
    }
    ok("account-subdivision");

    // Sixteen levels: the root's and fifteen below it, and this account is at some depth
    // of those. Nothing assumes which, only that the refusal comes and is the right one.
    let mut chain = alloc::vec::Vec::new();
    let mut parent = (hi, made);
    let stop = loop {
        let mut next = 0u64;
        let c = t_account_child(parent.0, parent.1, &mut next);
        if c < 0 {
            break c;
        }
        chain.push(c);
        parent = (c as i32, next);
        if chain.len() > 16 {
            return fail("account-depth-unbounded");
        }
    };
    if stop != -ENOSPC || chain.is_empty() {
        return fail_rc("account-depth", stop, ENOSPC);
    }
    while let Some(c) = chain.pop() {
        if unsafe { t_close(c) } != 0 {
            return fail("account-depth-close");
        }
    }
    ok("account-depth");

    // Any other kind of handle names no account, for any operation and for the spawn.
    let (pr, pw) = unsafe { t_pipe() };
    if pr < 0 || pw < 0 {
        return fail("account-pipe");
    }
    let rc = t_account_query_of(pr as i32, made, &mut q);
    if rc != -EBADF {
        return fail_rc("account-other-kind-query", rc, EBADF);
    }
    let mut unused = 0u64;
    let rc = t_account_child(pr as i32, made, &mut unused);
    if rc != -EBADF {
        return fail_rc("account-other-kind-child", rc, EBADF);
    }
    let rc = t_account_set_limits(pr as i32, made, &lim);
    if rc != -EBADF {
        return fail_rc("account-other-kind-limits", rc, EBADF);
    }
    let rc = spawn_under(pr as u32, 0, made, h as u64, made);
    if rc != -EBADF {
        return fail_rc("account-spawn-other-kind", rc, EBADF);
    }
    // Spawn's descriptor array takes the pipe and refuses the account.
    let pid = spawn_holding(pr as u32);
    if pid <= 0 {
        return fail_rc("account-fd-control", pid, 0);
    }
    let mut status = -1i32;
    let reaped = unsafe { t_wait_pid_for(pid as i32, 0, &mut status) };
    if reaped != pid || status != 0 {
        return fail_num("account-fd-control-exit", status as u32 as u64, 0);
    }
    let rc = spawn_holding(h as u32);
    if rc != -1 {
        return fail_rc("account-fd-refused", rc, 1);
    }
    let _ = unsafe { t_close(pr) };
    let _ = unsafe { t_close(pw) };
    let rc = spawn_under(1000, 0, made, h as u64, made);
    if rc != -EBADF {
        return fail_rc("account-spawn-no-handle", rc, EBADF);
    }
    let rc = spawn_under(h as u32, 1, made, h as u64, made);
    if rc != -1 {
        return fail_rc("account-spawn-flags", rc, 1);
    }
    for (leg, expected) in [
        ("account-spawn-unnamed", 0),
        ("account-spawn-stale", made + 1),
    ] {
        let rc = spawn_under(h as u32, 0, expected, h as u64, made);
        if rc != -EINVAL {
            return fail_rc(leg, rc, EINVAL);
        }
    }
    ok("account-refuses-other-kinds");

    // The control, one variable from those: the same spawn with a live handle.
    let pid = spawn_under(h as u32, 0, made, h as u64, made);
    if pid <= 0 {
        return fail_rc("account-spawn", pid, 0);
    }
    let mut status = -1i32;
    let reaped = unsafe { t_wait_pid_for(pid as i32, 0, &mut status) };
    if reaped != pid || status != 0 {
        return fail_num("account-spawn-child-saw", status as u32 as u64, 0);
    }
    let rc = t_account_query_of(hi, made, &mut q);
    if rc != 0 || q.c[CLASS_META as usize].used != 0 {
        return fail_num("account-spawn-refunds", q.c[CLASS_META as usize].used, 0);
    }
    ok("account-spawn-under");

    // Behind a cwd tail and an env tail, where only the sum of the lower tails finds it.
    let mut cmd = Command::new("/bin/capacity-probe");
    cmd.arg("account-child")
        .arg(alloc::format!("{}", h))
        .arg(alloc::format!("{}", made))
        .current_dir("/")
        .env_clear()
        .env("CAPACITY_PROBE_LEG", "tails")
        .account(h as u32, made);
    let mut child = match cmd.spawn() {
        Ok(child) => child,
        Err(e) => {
            t_putstr(&alloc::format!(
                "capacity-probe: FAIL account-tails-spawn: {e}\n"
            ));
            return 1;
        }
    };
    match child.wait() {
        Ok(status) if status.success() => {}
        _ => return fail("account-tails-child"),
    }
    ok("account-tails");

    // An exec closes the control. The child's status names what it saw.
    let pid = spawn_mode(b"account-exec");
    if pid <= 0 {
        return fail_rc("account-exec-spawn", pid, 0);
    }
    let mut status = -1i32;
    let reaped = unsafe { t_wait_pid_for(pid as i32, 0, &mut status) };
    if reaped != pid || status != 0 {
        return fail_num("account-exec-closes", status as u32 as u64, 0);
    }
    ok("account-exec-closes");

    // A record that cannot be written is the last thing the call finds, and leaves nothing
    // behind: the next child takes the number that one would have held, once closed.
    let ro = unsafe { t_burrow_reserve(PAGE, RW, 0) };
    if ro <= 0 {
        return fail("account-child-ro-reserve");
    }
    let ro = ro as u64;
    let mut seed = AccountChildRec::default();
    seed.h.version = ACCOUNT_VERSION;
    seed.h.size = core::mem::size_of::<AccountChildRec>() as u32;
    unsafe { core::ptr::write(ro as *mut AccountChildRec, seed) };
    if unsafe { t_burrow_protect(ro, PAGE, R, 0) } != 0 {
        return fail("account-child-ro-protect");
    }
    let free_before = {
        let probe = t_account_child(ACCOUNT_SELF, 0, &mut unused);
        if probe < 0 {
            return fail_rc("account-child-ro-probe", probe, 0);
        }
        let _ = unsafe { t_close(probe) };
        probe
    };
    let rc = unsafe {
        t_account(
            SELF,
            ACCOUNT_CHILD as u64,
            ro as *mut u8,
            core::mem::size_of::<AccountChildRec>() as u64,
        )
    };
    if rc != -EFAULT {
        return fail_rc("account-child-ro-record", rc, EFAULT);
    }
    let after = t_account_child(ACCOUNT_SELF, 0, &mut unused);
    if after != free_before {
        return fail_num(
            "account-child-ro-leaves-no-handle",
            after as u64,
            free_before as u64,
        );
    }
    let _ = unsafe { t_close(after) };
    if unsafe { t_burrow_detach(ro, PAGE) } != 0 {
        return fail("account-child-ro-detach");
    }
    ok("account-child-record-memory");

    if unsafe { t_close(h) } != 0 {
        return fail("account-close");
    }
    let rc = t_account_query_of(hi, made, &mut q);
    if rc != -EBADF {
        return fail_rc("account-closed", rc, EBADF);
    }
    ok("account-close");
    0
}

// `capacity-probe account-child <parent's handle number> <incarnation>`, spawned above. The
// exit status names what was wrong.
fn account_child_mode(told_handle: u64, told_incarnation: u64) -> i64 {
    let mut q = AccountQuery::default();
    if t_account_query(&mut q) != 0 {
        return 2;
    }
    if q.h.incarnation != told_incarnation {
        return 3;
    }
    if q.c[CLASS_META as usize].used == 0 {
        return 4; // its own thread is charged to it
    }
    // No handle crosses a Proc boundary, so the number names nothing here.
    let mut o = AccountQuery::default();
    if t_account_query_of(told_handle as i32, told_incarnation, &mut o) != -EBADF {
        return 5;
    }
    // Nobody adjusts their own account.
    if t_account_set_limits(ACCOUNT_SELF, 0, &[ACCOUNT_LIMIT_NONE; ACCOUNT_CLASSES]) != -EACCES {
        return 6;
    }
    0
}

// `capacity-probe account-exec`: a handle that works, its flag cleared, then an exec told its
// number. An exec closes control whatever the flag says.
fn account_exec_mode() -> i64 {
    let mut made = 0u64;
    let h = t_account_child(ACCOUNT_SELF, 0, &mut made);
    if h < 0 {
        return 8;
    }
    let mut q = AccountQuery::default();
    if t_account_query_of(h as i32, made, &mut q) != 0 {
        return 9;
    }
    if unsafe { t_handle_flags_set(h, 0) } != 0 || unsafe { t_handle_flags_get(h) } != 0 {
        return 13;
    }
    let mut argv = alloc::vec::Vec::new();
    argv.extend_from_slice(b"capacity-probe\0account-after-exec\0");
    push_num(&mut argv, h as u64);
    push_num(&mut argv, made);
    let _ = unsafe { t_execve(b"/bin/capacity-probe", &argv, 4) };
    10
}

// `capacity-probe account-after-exec <handle number> <incarnation>`: the number names nothing.
fn account_after_exec_mode(handle: u64, incarnation: u64) -> i64 {
    let mut q = AccountQuery::default();
    if t_account_query_of(handle as i32, incarnation, &mut q) != -EBADF {
        return 11;
    }
    0
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mut args = libthyla_rs::env::args();
    let mode = args.nth(1);
    if mode == Some(&b"account-noop"[..]) {
        return 0;
    }
    if mode == Some(&b"account-exec"[..]) {
        return account_exec_mode();
    }
    if mode == Some(&b"account-after-exec"[..]) {
        let handle = args.next().and_then(parse_num);
        let incarnation = args.next().and_then(parse_num);
        return match (handle, incarnation) {
            (Some(h), Some(i)) => account_after_exec_mode(h, i),
            _ => 12,
        };
    }
    if mode == Some(&b"account-child"[..]) {
        let handle = args.next().and_then(parse_num);
        let incarnation = args.next().and_then(parse_num);
        return match (handle, incarnation) {
            (Some(h), Some(i)) => account_child_mode(h, i),
            _ => 7,
        };
    }
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
    // 8. Last, because it leaves pages of stack the census above would have to explain.
    let failed = account_legs();
    if failed != 0 {
        return failed;
    }
    t_putstr("capacity-probe: ALL OK\n");
    0
}
