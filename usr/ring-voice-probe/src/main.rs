// /ring-voice-probe -- the N-2b-1 substrate witness (docs/NOCTURNE.md section 6.5).
//
// N-2a proved a voice plays via the byte-copy `audio` write. N-2b-1 proves the
// zero-copy ring SUBSTRATE: mint a voice, open its `data` leaf THROUGH THE MOUNT
// (SYS_WEFT_MAP needs a dev9p fd -- a direct /srv/nocturne srvconn will not do,
// docs/NOCTURNE.md section 6.5 + the Weft transport caveat), SYS_WEFT_MAP it,
// and validate the shared ring geometry (WEFT_MAGIC, K slots, a payload region
// big enough for K periods). The period producer/consumer protocol over the
// ring is N-2b-2; this witnesses the map + the authority gate only.
//
// Controls (a check that cannot fail proves nothing, #245): (a) a SECOND map of
// the same fd is idempotent (same VA -- the kernel's priv->weft fast-path);
// (b) mapping VOICE 0's `data` is REFUSED -- voice 0 is the world-shared byte
// sink and has no private SPSC ring, so a positive here would mean the id==0
// gate is dead.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::io::Write;
use libthyla_rs::weft;
use libthyla_rs::{
    t_close, t_open, t_putstr, t_read, t_weft_map, T_OREAD, T_WALK_OPEN_FROM_ROOT,
};

// Must match nocturned/src/server.rs (RING_ENTRIES) and snd.rs (PERIOD_BYTES).
const RING_ENTRIES: u32 = 8;
const PERIOD_BYTES: u32 = 2048;

fn say(s: &str) {
    let _ = t_putstr(s);
    let mut out = libthyla_rs::io::stdout();
    let _ = out.write_all(s.as_bytes());
}

fn fail(why: &str) -> i64 {
    let mut s = String::from("RING-VOICE-PROBE FAIL: ");
    s.push_str(why);
    s.push('\n');
    say(&s);
    1
}

/// Mint a voice via /dev/nocturne/nodes/new; the read returns its decimal id.
fn mint_voice() -> Option<u32> {
    let p = b"/dev/nocturne/nodes/new";
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, p.as_ptr(), p.len(), T_OREAD) };
    if fd < 0 {
        return None;
    }
    let mut buf = [0u8; 16];
    let n = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
    let _ = unsafe { t_close(fd) };
    if n <= 0 {
        return None;
    }
    core::str::from_utf8(&buf[..n as usize]).ok()?.trim().parse::<u32>().ok()
}

/// Open a voice's `data` map fid THROUGH THE MOUNT (the dev9p path SYS_WEFT_MAP
/// requires). The open mode is a formality -- the ring is mapped RW by the
/// kernel regardless; the fid is only the Tweft anchor.
fn open_data(id: u32) -> i64 {
    let path = alloc::format!("/dev/nocturne/nodes/{}/data", id);
    let b = path.as_bytes();
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b.as_ptr(), b.len(), T_OREAD) }
}


#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let id = match mint_voice() {
        Some(id) => id,
        None => return fail("mint voice (/dev/nocturne/nodes/new)"),
    };
    let data_fd = open_data(id);
    if data_fd < 0 {
        return fail("open nodes/<id>/data through the mount");
    }

    // Map the ring: SYS_WEFT_MAP -> kernel Tweft(data fid) -> nocturned's h_weft
    // allocates + shares the ANON ring -> the kernel maps it into this Proc.
    let ring_va = unsafe { t_weft_map(data_fd as u64, 0) };
    if ring_va < 0 {
        let _ = unsafe { t_close(data_fd) };
        return fail("SYS_WEFT_MAP the voice ring");
    }
    let ring_va = ring_va as u64;

    let geom = match unsafe { weft::read_ring_geom(ring_va as *const u8) } {
        Some(g) => g,
        None => {
            let _ = unsafe { t_close(data_fd) };
            return fail("read_ring_geom (bad WEFT_MAGIC / geometry)");
        }
    };
    if geom.ring_entries != RING_ENTRIES {
        return fail("ring_entries mismatch");
    }
    if geom.payload_size < RING_ENTRIES * PERIOD_BYTES {
        return fail("payload region too small for K periods");
    }

    // Control (a): a second map of the same fd is idempotent (same VA).
    let ring_va2 = unsafe { t_weft_map(data_fd as u64, 0) };
    if ring_va2 < 0 || ring_va2 as u64 != ring_va {
        let _ = unsafe { t_close(data_fd) };
        return fail("second map of the same fd is not idempotent");
    }

    // Control (b): voice 0's data map must be REFUSED (no world-shared ring).
    let d0 = open_data(0);
    if d0 >= 0 {
        let r0 = unsafe { t_weft_map(d0 as u64, 0) };
        let _ = unsafe { t_close(d0) };
        if r0 >= 0 {
            let _ = unsafe { t_close(data_fd) };
            return fail("voice 0 data map was NOT refused (the id==0 gate is dead)");
        }
    }

    let _ = unsafe { t_close(data_fd) };

    // Like /nocturne-probe, this mount-minted voice (and its ring) outlives the
    // probe -- owned by joey's shared mount connection, not the probe (the F5
    // limitation). A client wanting per-exit lifetime needs a direct
    // /srv/nocturne connection, which cannot SYS_WEFT_MAP (that needs a dev9p
    // fd); reconciling the two is N-2c/N-3. Benign for a boot smoke.
    let s = alloc::format!(
        "ring-voice-probe: mapped ring va={:#x} K={} size={} payload={} (voice {})\n",
        ring_va, geom.ring_entries, geom.ring_size, geom.payload_size, id,
    );
    say(&s);
    say("RING-VOICE-PROBE PASS (weft ring mapped + geometry valid + 2 controls)\n");
    0
}
