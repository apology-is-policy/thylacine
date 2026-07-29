// /jit-prover (CL-7k-2) -- the end-to-end I-42 prover (docs/JIT-ON-WX-DESIGN.md;
// LLVM-DESIGN.md section 8). Spawned by joey after the corvus bringup has granted
// "michael" eligibility for the `jit` clearance level.
//
// The kernel unit tests prove the mechanism: two aliases, W^X-clean at the real
// PTEs, over one physical region. What they structurally cannot prove is the
// thing the whole invariant exists for -- that a CPU at EL0 actually FETCHES AND
// EXECUTES bytes this process just wrote. That needs a real EL0 Proc, so it
// happens here.
//
// The proof is deliberately ONE process across a capability boundary:
//
//   1. connect /srv/corvus + AUTH as michael -> a session token.
//   2. WITHOUT the capability: SYS_JIT_CREATE must be REFUSED. (Same binary,
//      same everything -- so when step 5 succeeds, the capability is the only
//      thing that changed. Two different processes would leave "maybe it was
//      something else about that process" open.)
//   3. CLEARANCE_LIST -> assert `jit` eligibility.
//   4. CLEARANCE_ACTIVATE(jit) -> corvus registers the kernel grant against OUR
//      stripes; redeem it via the cap device -> we now hold CAP_JIT.
//   5. create a code region, emit a real AArch64 function through the WRITER
//      alias, publish it with SYS_ICACHE_SYNC, and CALL it through the EXEC
//      alias. A wrong answer, a fault, or a hang here means the dual map, the
//      RX PTE, or the cache maintenance is broken.
//   6. destroy the region; assert a second destroy fails cleanly.
//
// Step 5 is the load-bearing one. It is also the only place in the tree where
// the icache sync is genuinely load-bearing rather than merely correct: skip it
// and the CPU may fetch the pre-write contents of those physical pages, which
// for a fresh region is zero -- UDF #0 -- and would fault rather than return.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::vec::Vec;
use libthyla_rs::cap::{self, Caps};
use libthyla_rs::jit::{a64, CodeRegion, JitError};
use libthyla_rs::{
    t_close, t_open, t_putstr, t_read, t_write, T_CAP_JIT, T_OREAD, T_ORDWR,
    T_WALK_OPEN_FROM_ROOT,
};

// Shared test passphrase for "michael" -- MUST match joey's pass_michael.
const PASS_MICHAEL: &[u8] = b"correct-horse-battery-staple-v1";

const VERB_AUTH: u8 = 1;
const VERB_CLEARANCE_LIST: u8 = 14;
const VERB_CLEARANCE_ACTIVATE: u8 = 15;
const CORVUS_PROTOCOL_VERSION: u8 = 1;
const TOKEN_LEN: usize = 33;
const LEVEL_JIT: &[u8] = b"jit";

// Render `v` as decimal into `buf` and return it as a &str. t_putstr is the
// ONLY console path available: joey spawns this prover with t_spawn (no fds),
// so fd 1 is not open and t_write(1, ..) writes nowhere.
fn dec(v: i64, buf: &mut [u8; 24]) -> &str {
    let neg = v < 0;
    let mut m = if neg { (-(v as i128)) as u128 } else { v as u128 };
    let mut tmp = [0u8; 24];
    let mut n = 0usize;
    if m == 0 {
        tmp[n] = b'0';
        n += 1;
    }
    while m > 0 {
        tmp[n] = b'0' + (m % 10) as u8;
        n += 1;
        m /= 10;
    }
    let mut o = 0usize;
    if neg {
        buf[o] = b'-';
        o += 1;
    }
    while n > 0 {
        n -= 1;
        buf[o] = tmp[n];
        o += 1;
    }
    // Digits + an optional '-' only: valid ASCII by construction.
    unsafe { core::str::from_utf8_unchecked(&buf[..o]) }
}

fn fail(msg: &str) -> ! {
    t_putstr(msg);
    unsafe { libthyla_rs::t_exits(1) }
}

unsafe fn write_all(fd: i64, buf: &[u8]) -> bool {
    let mut off = 0usize;
    while off < buf.len() {
        let w = t_write(fd, buf.as_ptr().add(off), buf.len() - off);
        if w <= 0 {
            return false;
        }
        off += w as usize;
    }
    true
}

unsafe fn read_exact(fd: i64, buf: &mut [u8]) -> bool {
    let mut off = 0usize;
    while off < buf.len() {
        let r = t_read(fd, buf.as_mut_ptr().add(off), buf.len() - off);
        if r <= 0 {
            return false;
        }
        off += r as usize;
    }
    true
}

unsafe fn exchange(fd: i64, verb: u8, payload: &[u8]) -> Option<(u8, Vec<u8>)> {
    let hdr = [
        verb,
        CORVUS_PROTOCOL_VERSION,
        (payload.len() & 0xff) as u8,
        ((payload.len() >> 8) & 0xff) as u8,
    ];
    if !write_all(fd, &hdr) {
        return None;
    }
    if !payload.is_empty() && !write_all(fd, payload) {
        return None;
    }
    let mut rh = [0u8; 3];
    if !read_exact(fd, &mut rh) {
        return None;
    }
    let rlen = (rh[1] as usize) | ((rh[2] as usize) << 8);
    let mut resp = Vec::new();
    resp.resize(rlen, 0);
    if rlen > 0 && !read_exact(fd, &mut resp) {
        return None;
    }
    Some((rh[0], resp))
}

// CLEARANCE_LIST reply: count u8, then per level {name_len u8, name,
// auth_required u8, time_bound u64 LE, caps_tlv_len u16 LE, caps_tlv}.
fn list_has_level(list: &[u8], want: &[u8]) -> bool {
    if list.is_empty() {
        return false;
    }
    let count = list[0] as usize;
    let mut off = 1usize;
    for _ in 0..count {
        if off >= list.len() {
            return false;
        }
        let nl = list[off] as usize;
        off += 1;
        if off + nl > list.len() {
            return false;
        }
        let name = &list[off..off + nl];
        off += nl;
        if off + 1 + 8 + 2 > list.len() {
            return false;
        }
        off += 1 + 8;
        let tlv_len = (list[off] as usize) | ((list[off + 1] as usize) << 8);
        off += 2;
        if off + tlv_len > list.len() {
            return false;
        }
        off += tlv_len;
        if name == want {
            return true;
        }
    }
    false
}

// The function we JIT: `fn(a: u64, b: u64) -> u64 { a + b + 7 }`.
//
// Chosen so a wrong answer cannot be produced by accident. `add x0, x0, x1`
// alone would return the right value if the region held a leftover `add`; the
// `+ 7` means the result depends on an instruction that must be OURS. And using
// both arguments means an entry landing one instruction late (a mis-computed
// exec offset) yields a visibly wrong number rather than a plausible one.
fn emit_add7(region: &mut CodeRegion) -> bool {
    // bti c              -- landing pad: this is reached via a function pointer,
    //                       and on BTI-enforcing silicon an indirect call to a
    //                       non-landing-pad instruction traps. A NOP elsewhere.
    // add  x0, x0, x1
    // mov  x2, #7
    // add  x0, x0, x2
    // ret
    let prog = [
        a64::BTI_C,
        a64::add(0, 0, 1),
        a64::movz(2, 7),
        a64::add(0, 0, 2),
        a64::RET,
    ];
    for (i, insn) in prog.iter().enumerate() {
        if !region.write_insn(i * 4, *insn) {
            return false;
        }
    }
    true
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("jit-prover: starting (CL-7k I-42 E2E)\n");

    // --- Step 2 (before any capability): creation must be REFUSED. -----------
    // Runs FIRST, on purpose. If it ran after the redeem it would prove nothing;
    // running it here, in the same process that succeeds later, makes CAP_JIT
    // the only variable between the two outcomes.
    match CodeRegion::new(4096) {
        Err(JitError::NotPermitted) => {
            t_putstr("jit-prover: ungated create REFUSED (no CAP_JIT) -- correct\n");
        }
        Err(e) => {
            // Any other error means the gate did not fire; the create failed for
            // an unrelated reason and the denial is unproven.
            t_putstr("jit-prover: FAIL ungated create failed for the WRONG reason: ");
            t_putstr(match e {
                JitError::BadLength => "BadLength\n",
                JitError::OutOfMemory => "OutOfMemory\n",
                _ => "Other\n",
            });
            unsafe { libthyla_rs::t_exits(1) }
        }
        Ok(_) => fail("jit-prover: FAIL ungated create SUCCEEDED -- CAP_JIT gate is open\n"),
    }

    // --- Steps 1/3/4: acquire CAP_JIT through the real clearance path. -------
    let mut conn: i64 = -1;
    for _ in 0..64 {
        let root =
            unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/corvus".as_ptr(), 11, T_OREAD) };
        if root >= 0 {
            let ctl = unsafe { t_open(root, b"ctl".as_ptr(), 3, T_ORDWR) };
            let _ = unsafe { t_close(root) };
            if ctl >= 0 {
                conn = ctl;
                break;
            }
        }
    }
    if conn < 0 {
        fail("jit-prover: FAIL connect /srv/corvus\n");
    }

    let mut auth_pl: Vec<u8> = Vec::new();
    auth_pl.push(7);
    auth_pl.extend_from_slice(b"michael");
    auth_pl.push((PASS_MICHAEL.len() & 0xff) as u8);
    auth_pl.push(((PASS_MICHAEL.len() >> 8) & 0xff) as u8);
    auth_pl.extend_from_slice(PASS_MICHAEL);
    let (st, resp) = match unsafe { exchange(conn, VERB_AUTH, &auth_pl) } {
        Some(x) => x,
        None => fail("jit-prover: FAIL AUTH transport\n"),
    };
    if st != 0 || resp.len() != TOKEN_LEN {
        fail("jit-prover: FAIL AUTH (bad status/token)\n");
    }
    let mut token = [0u8; TOKEN_LEN];
    token.copy_from_slice(&resp);

    let (st, list) = match unsafe { exchange(conn, VERB_CLEARANCE_LIST, &token) } {
        Some(x) => x,
        None => fail("jit-prover: FAIL CLEARANCE_LIST transport\n"),
    };
    if st != 0 {
        fail("jit-prover: FAIL CLEARANCE_LIST (status)\n");
    }
    if !list_has_level(&list, LEVEL_JIT) {
        fail("jit-prover: FAIL not eligible for jit\n");
    }

    let mut act_pl: Vec<u8> = Vec::new();
    act_pl.extend_from_slice(&token);
    act_pl.push(LEVEL_JIT.len() as u8);
    act_pl.extend_from_slice(LEVEL_JIT);
    act_pl.extend_from_slice(&0u64.to_le_bytes()); // self_restrict
    act_pl.extend_from_slice(&0u64.to_le_bytes()); // valid_until_req
    let (st, ar) = match unsafe { exchange(conn, VERB_CLEARANCE_ACTIVATE, &act_pl) } {
        Some(x) => x,
        None => fail("jit-prover: FAIL CLEARANCE_ACTIVATE transport\n"),
    };
    if st != 0 || ar.len() != 12 {
        fail("jit-prover: FAIL CLEARANCE_ACTIVATE (status/len)\n");
    }
    let granted = u64::from_le_bytes([ar[4], ar[5], ar[6], ar[7], ar[8], ar[9], ar[10], ar[11]]);
    if granted != T_CAP_JIT {
        fail("jit-prover: FAIL granted caps != CAP_JIT\n");
    }
    if cap::use_grant(Caps::from_bits(granted)).is_err() {
        fail("jit-prover: FAIL cap use_grant (redeem)\n");
    }
    t_putstr("jit-prover: CAP_JIT acquired via the jit clearance\n");

    // --- Step 5: emit, publish, EXECUTE. -------------------------------------
    let mut region = match CodeRegion::new(4096) {
        Ok(r) => r,
        Err(e) => {
            t_putstr("jit-prover: FAIL create after redeem: ");
            match e {
                JitError::NotPermitted => { t_putstr("NotPermitted (CAP_JIT missing)\n"); }
                JitError::BadLength => { t_putstr("BadLength\n"); }
                JitError::OutOfMemory => { t_putstr("OutOfMemory\n"); }
                JitError::Other(rc) => {
                    t_putstr("Other rc=");
                    t_putstr(dec(rc, &mut [0u8; 24]));
                    t_putstr("\n");
                }
            }
            unsafe { libthyla_rs::t_exits(1) }
        }
    };
    let writer = region.writer_ptr() as u64;
    let exec = region.exec_ptr() as u64;
    if writer == exec {
        fail("jit-prover: FAIL writer and exec alias are the same VA\n");
    }

    if !emit_add7(&mut region) {
        fail("jit-prover: FAIL emit (write out of range)\n");
    }
    if !region.publish() {
        fail("jit-prover: FAIL SYS_ICACHE_SYNC\n");
    }

    // The moment of truth: branch into the region through the EXECUTABLE alias.
    // If the dual map were a lie, the RX PTE wrong, or the cache maintenance
    // absent, this either faults or returns garbage.
    let f: extern "C" fn(u64, u64) -> u64 = unsafe { region.entry(0) };
    let got = f(35, 100);
    if got != 142 {
        t_putstr("jit-prover: FAIL JITed call returned the wrong value\n");
        unsafe { libthyla_rs::t_exits(1) }
    }
    t_putstr("jit-prover: JITed fn(35,100) = 142 -- emitted, published, EXECUTED\n");

    // Re-emit a DIFFERENT function into the same region and call it again. This
    // is what a real JIT does when it recompiles, and it is the case where a
    // missing icache invalidate shows up as "the old function keeps running"
    // rather than as a fault -- the failure mode that is easy to miss because
    // nothing crashes.
    let prog2 = [a64::BTI_C, a64::movz(0, 99), a64::RET];
    for (i, insn) in prog2.iter().enumerate() {
        if !region.write_insn(i * 4, *insn) {
            fail("jit-prover: FAIL re-emit\n");
        }
    }
    if !region.publish_range(0, prog2.len() * 4) {
        fail("jit-prover: FAIL publish_range\n");
    }
    let g: extern "C" fn(u64, u64) -> u64 = unsafe { region.entry(0) };
    let got2 = g(35, 100);
    if got2 != 99 {
        t_putstr("jit-prover: FAIL re-emitted fn returned stale/wrong value\n");
        unsafe { libthyla_rs::t_exits(1) }
    }
    t_putstr("jit-prover: re-emitted fn returns 99 -- icache invalidate is live\n");

    // --- Step 6: teardown. ---------------------------------------------------
    // Drop tears down BOTH aliases and frees the pages; a second destroy of the
    // same writer VA must then fail cleanly rather than dismantle whatever
    // occupies that address next.
    drop(region);
    let second = unsafe { libthyla_rs::t_jit_destroy(writer) };
    if second == 0 {
        fail("jit-prover: FAIL second destroy SUCCEEDED (double teardown)\n");
    }

    t_putstr("jit-prover: PASS (CL-7k I-42 end-to-end)\n");
    0
}
