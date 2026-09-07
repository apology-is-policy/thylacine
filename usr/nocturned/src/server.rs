// The /srv/nocturne 9P tree (docs/NOCTURNE.md section 6.4). N-2a-1 grows the
// N-1 heritage floor into the graph core's first half: multiple VOICES mixed to
// the one sink.
//
//   / { ctl, info, audio, nodes/ }
//   nodes/new                 open -> mints a voice owned by this connection;
//                             read the same fid -> the new voice's id (decimal)
//   nodes/<id>/ { audio, ctl, info }
//
// `audio` at the root is voice 0 -- a persistent default voice, so `bind
// /dev/nocturne/audio /dev/audio` and the Plan 9 audio(3) shape still work.
// Every voice carries a bounded S16LE-stereo FIFO; a write that fills it PARKS
// (its Rwrite deferred until the mixer drains room -- Plan 9's blocking write).
// The device pump pulls one period at a time via `next_period`, which MIXES all
// voices in float32 with per-voice gain, clamps to S16, and hands the sink one
// period. A voice created through nodes/new dies with the connection that made
// it (the tapestry surface-lifetime idiom); voice 0 is never removed.
//
// The internal graph is byte-copy at N-2a-1 -- the designed fallback below the
// Weft hybrid threshold (section 6.5). The per-node Weft ring (SYS_WEFT_SHARE ->
// Tweft on nodes/<id>/data), ports, links and descants are N-2b / N-4.
//
// Framing + dispatch + the parked-write / Tflush machinery mirror
// usr/ptyfs/src/server.rs and are preserved verbatim from the N-1 audit.

use core::sync::atomic::{AtomicU32, Ordering};
use core::time::Duration;

use alloc::collections::VecDeque;
use alloc::vec::Vec;

use libthyla_rs::ninep as p9;
use libthyla_rs::sync::Mutex;
use libthyla_rs::torpor;
use libthyla_rs::weft as weftlib;
use libthyla_rs::{
    t_burrow_attach, t_burrow_detach, t_close, t_open, t_srv_peer, t_walk_create,
    t_weft_share, t_weft_unshare, TSrvPeerInfo, T_CAP_AUDIO_GRAPH, T_CAP_HOSTOWNER,
    T_OPATH, T_OREAD, T_PRINCIPAL_SYSTEM, T_SRV_PEER_FLAG_CONSOLE_OWNER,
    T_WALK_OPEN_FROM_ROOT,
};

use crate::snd::{Stats, BUFFER_BYTES, PERIODS, PERIOD_BYTES, RATE_HZ};

pub const MAX_CONNS: usize = 32;
const MAX_FIDS: usize = 32;
const MAX_PENDING_WRITES: usize = 8;
const SRV_MSIZE: u32 = 32768;
const SRV_MSIZE_USIZE: usize = SRV_MSIZE as usize;
/// ~340 ms of S16LE stereo at 48 kHz, per voice; the write-side backlog beyond
/// the four periods the device holds.
const FIFO_CAP: usize = 64 * 1024;
/// The mixer bound: voice 0 (persistent) + up to 15 client voices. Each voice's
/// FIFO_CAP is charged only as it fills, so the ceiling is a DoS bound, not a
/// reservation.
const MAX_VOICES: usize = 16;
/// Bytes per stereo S16 frame.
const FRAME: usize = 4;
/// The sink tap's mirror ceiling (N-3c-1): a bounded drop-oldest ring of the
/// final mixed output that the single authorized reader drains. Bounded so a
/// stalled reader bleeds the OLDEST audio rather than growing without limit (the
/// DoS floor -- the tap is realtime, not a durable buffer); a reader keeping up
/// (it drains faster than one period per PARKED_RETRY_MS) sees it near-empty.
const TAP_MIRROR_MAX: usize = 8 * PERIOD_BYTES;

const P9_VERSION_9P2000_L: &[u8] = b"9P2000.L";
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const DIR_MODE: u32 = S_IFDIR | 0o555;
const P9_GETATTR_SIZE: u64 = 0x200;

// Root paths.
const P_ROOT: u64 = 0;
const P_CTL: u64 = 1;
const P_INFO: u64 = 2;
const P_AUDIO: u64 = 3;
const P_NODES: u64 = 4;
const P_NODES_NEW: u64 = 5;
const P_VOLUME: u64 = 6;
const P_TAP: u64 = 7; // /srv/nocturne-ctl/tap: the gated ear on the sink mix (N-3c-1)
const P_SOURCE: u64 = 8; // /srv/nocturne-ctl/source: the gated device capture read (N-3c-2)

// Voice paths: VBIT | (id << 4) | leaf. Leaf 0 = the voice dir; 1/2/3 = the
// audio/ctl/info files. VBIT (bit 40) is above the 6 fixed root paths AND above
// the full u32 id shifted into bits 4..35, so a large id can neither set VBIT
// (aliasing a different path) nor be truncated by vid() -- the static assert
// below pins it.
const VBIT: u64 = 1 << 40;
const VLEAF_DIR: u64 = 0;
const VLEAF_AUDIO: u64 = 1;
const VLEAF_CTL: u64 = 2;
const VLEAF_INFO: u64 = 3;
const VLEAF_DATA: u64 = 4;

// The per-voice Weft ring (N-2b). One ANON Burrow per ring voice, shared to the
// owning client. RING_ENTRIES period slots (K >= 3); RING_SIZE is page-aligned
// and fits the weftlib header (RING_HDR + READY_HDR + DESC[K]) plus K period
// payloads (PERIOD_BYTES each). N-2b-1 allocates + shares + maps the ring; the
// producer/consumer period protocol over it is N-2b-2.
const RING_ENTRIES: u32 = 8;
const RING_SIZE: u64 = 20480; // 5 pages: 320 B hdr + >= 8 * PERIOD_BYTES payload

// The ring must fit K = RING_ENTRIES period slots of PERIOD_BYTES after the
// weftlib header (RING_HDR 64 + READY_HDR 128 + DESC[K]*16, 16-aligned). The
// consumer copies into payload_off + i*PERIOD_BYTES for i < K, so K*PERIOD_BYTES
// must fit the payload region -- pin it at compile time so a future const change
// (a bigger PERIOD_BYTES, a smaller RING_SIZE) cannot silently push the copy off
// the ring (the compile-time-invariants pattern; slot_consume trusts this).
const _: () = assert!(
    192 + (RING_ENTRIES as usize) * 16 + (RING_ENTRIES as usize) * PERIOD_BYTES <= RING_SIZE as usize
);

fn vpath(id: u32, leaf: u64) -> u64 {
    VBIT | ((id as u64) << 4) | leaf
}
fn is_voice(path: u64) -> bool {
    path & VBIT != 0
}
fn vid(path: u64) -> u32 {
    ((path >> 4) & 0xFFFF_FFFF) as u32
}
fn vleaf(path: u64) -> u64 {
    path & 0xF
}

// VBIT must sit above the full u32 id shifted into bits 4..35, so no voice id
// can set VBIT (which would alias a fixed path or a different voice) and vid()
// recovers every id bit.
const _: () = assert!(VBIT > ((u32::MAX as u64) << 4));

// The PLAYBACK tree's root children (name, path, mode) -- what joey mounts at
// /dev/nocturne. `volume` is READ-ONLY here (0o444): a shared mount carries the
// mounter's SYSTEM identity, so it must never be a write-authority path (the F1
// bypass). Writing volume goes to the CONTROL post (ROOT_CHILDREN_CTL), where
// the peer is the writer. The mount's read-only volume still serves `cat`.
const ROOT_CHILDREN: [(&[u8], u64, u32); 5] = [
    (b"ctl", P_CTL, S_IFREG | 0o644),
    (b"info", P_INFO, S_IFREG | 0o444),
    (b"volume", P_VOLUME, S_IFREG | 0o444),
    (b"audio", P_AUDIO, S_IFREG | 0o666),
    (b"nodes", P_NODES, S_IFDIR | 0o555),
];
// The CONTROL post's root children (/srv/nocturne-ctl, N-3a-3): the
// sink-authority surface, reached by a controller over its OWN connection
// (open=connect, never mounted), so the volume write's peer IS the writer and
// the 6.8 gate reads the real caller. `default`/`sinks/`/`sources/`/the sink
// `tap` join it as they land. Voices/playback are NOT here -- that is the mount.
const ROOT_CHILDREN_CTL: [(&[u8], u64, u32); 3] = [
    (b"volume", P_VOLUME, S_IFREG | 0o666),
    (b"tap", P_TAP, S_IFREG | 0o444),
    (b"source", P_SOURCE, S_IFREG | 0o444),
];

// The root-children table for a connection: the sink-authority set on the
// control post, the playback set otherwise.
fn root_children(control: bool) -> &'static [(&'static [u8], u64, u32)] {
    if control {
        &ROOT_CHILDREN_CTL
    } else {
        &ROOT_CHILDREN
    }
}
// nodes/ directory children (only the static `new`; voices are listed dynamically).
const NODES_STATIC: [(&[u8], u64, u32); 1] = [(b"new", P_NODES_NEW, S_IFREG | 0o666)];
// A voice directory's children (name, leaf, mode).
const VOICE_CHILDREN: [(&[u8], u64, u32); 4] = [
    (b"audio", VLEAF_AUDIO, S_IFREG | 0o666),
    (b"ctl", VLEAF_CTL, S_IFREG | 0o644),
    (b"info", VLEAF_INFO, S_IFREG | 0o444),
    // The zero-copy ring map fid (N-2b). Mode 0o666 like `audio`: the file mode
    // must PERMIT the open (the kernel's dev9p rwx enforcement gates open on the
    // mode before any handler runs, and boot/user clients are not uid 0), so the
    // real authority is server-side at Tweft (h_weft's owner gate) + the kernel's
    // consume-once share claim (SPSC producer-uniqueness). Not byte-read/written.
    (b"data", VLEAF_DATA, S_IFREG | 0o666),
];

fn mode_of(path: u64) -> u32 {
    if is_voice(path) {
        match vleaf(path) {
            VLEAF_DIR => DIR_MODE,
            leaf => VOICE_CHILDREN
                .iter()
                .find(|(_, l, _)| *l == leaf)
                .map(|(_, _, m)| *m)
                .unwrap_or(S_IFREG | 0o444),
        }
    } else {
        // The tap + source live only on the control post (ROOT_CHILDREN_CTL), so
        // ROOT_CHILDREN below does not carry them; report their read-only mode here.
        if path == P_TAP || path == P_SOURCE {
            return S_IFREG | 0o444;
        }
        for (_, p, m) in ROOT_CHILDREN {
            if p == path {
                return m;
            }
        }
        for (_, p, m) in NODES_STATIC {
            if p == path {
                return m;
            }
        }
        DIR_MODE
    }
}

fn is_dir(path: u64) -> bool {
    path == P_ROOT || path == P_NODES || (is_voice(path) && vleaf(path) == VLEAF_DIR)
}

/// A voice's zero-copy Weft ring (N-2b): an ANON Burrow nocturned allocates and
/// shares to the owning client (`SYS_WEFT_SHARE` -> the client's `SYS_WEFT_MAP`
/// on the node's `data` fid). nocturned is the CONSUMER (reads finished periods
/// on the device clock); the client is the PRODUCER. The share is bounded by the
/// voice (I-37): dropping the voice releases nocturned's side here; the kernel's
/// #847 dual count keeps the pages alive until the client's mapping is gone too,
/// so there is no in-flight-page UAF and no stale access.
struct RingVoice {
    ring_va: u64,
    ring_size: u64,
    share_id: u64,
    geom: weftlib::RingGeom,
}

impl Drop for RingVoice {
    fn drop(&mut self) {
        // `t_weft_unshare` disarms an UN-CLAIMED share (a client that never
        // mapped); if the client already claimed it the kernel consumed the id
        // and this is a clean no-op. `t_burrow_detach` drops nocturned's own
        // mapping. The client's mapping (if any) is reclaimed by the kernel
        // (vma_drain / the #847 dual count), so releasing our side is safe in
        // any interleaving.
        unsafe {
            let _ = t_weft_unshare(self.share_id);
            let _ = t_burrow_detach(self.ring_va, self.ring_size);
        }
    }
}

impl RingVoice {
    /// Consume ONE period from the ring into `scratch` (>= PERIOD_BYTES); the
    /// consumer half of the fixed-slot SPSC (N-2b-2). Returns the valid S16 byte
    /// count (0 = ring empty or a dropped/invalid slot -- both silence this
    /// period). The client is the producer; nocturned reads on the device clock
    /// (one period per next_period call), computing the slot offset itself and
    /// validating only the client-written len (I-30).
    fn consume_period(&self, scratch: &mut [u8]) -> usize {
        unsafe {
            weftlib::slot_consume(
                self.ring_va as *mut u8,
                &self.geom,
                PERIOD_BYTES as u32,
                FRAME as u32,
                scratch,
            )
        }
    }

    /// Published-but-undrained periods in this ring. The device start + idle-stop
    /// read this so a ring-only voice (empty byte FIFO) still starts and holds
    /// the stream.
    fn pending(&self) -> u32 {
        unsafe { weftlib::slot_pending(self.ring_va as *const u8) }
    }
}

/// One mixer input: an independent S16LE-stereo stream with its own bounded
/// FIFO and gain. Voice 0 is the persistent default (the root `audio` file);
/// every other voice is owned by the connection that minted it.
struct Voice {
    id: u32,
    fifo: VecDeque<u8>,
    /// Linear gain (1.0 = unity); set via `ctl gain <percent>`.
    gain: f32,
    /// The connection handle that minted this voice, or -1 for the persistent
    /// voice 0. Every voice a connection owns is dropped when it closes.
    owner: i64,
    bytes_in: u64,
    flushes: u64,
    /// The zero-copy ring, lazily allocated at the first Tweft (N-2b). None =
    /// a byte-copy-only voice (the FIFO path). Voice 0 never has a ring.
    ring: Option<RingVoice>,
}

impl Voice {
    fn new(id: u32, owner: i64) -> Voice {
        Voice {
            id,
            fifo: VecDeque::new(),
            gain: 1.0,
            owner,
            bytes_in: 0,
            flushes: 0,
            ring: None,
        }
    }
}

/// The mixer graph: the voices, the id allocator, and the device stats the
/// cycle thread publishes for `info`. Reached only through [`Shared`]'s lock,
/// so each field has a single writer at a time across the two threads (D-1c).
pub struct Graph {
    voices: Vec<Voice>,
    next_id: u32,
    pub stats: Stats,
    pub started: bool,
    /// The sink gain stage (N-3a): Plan 9 volume(3) controls, 0..=100 per
    /// channel (100 = unity). Effective per-channel linear gain on the final
    /// mix = (sink_audio/100) * (sink_mix/100) -- `audio` the PCM out level,
    /// `mix` the master, the mixfs master*control shape.
    sink_audio: [u32; 2],
    sink_mix: [u32; 2],
    /// The sink tap (N-3c-1): a bounded drop-oldest mirror of the FINAL mixed
    /// output (post-gain, post-clamp -- an ear on what actually plays), filled by
    /// `next_period` ONLY while `tap_open`, drained by the one authorized reader.
    /// `tap_open` is the single-reader guard (a second open gets EBUSY); it is
    /// cleared on the holder's clunk/teardown, which also drops the mirror.
    tap_mirror: VecDeque<u8>,
    tap_open: bool,
    /// The device-capture source (N-3c-2): the RX twin of the tap. A bounded
    /// drop-oldest mirror filled by the cycle's `pump_rx` while `source_open`,
    /// drained by the one authorized reader. `source_open` is BOTH the single-reader
    /// guard AND the cycle's on-demand capture trigger -- the cycle STARTs the RX
    /// stream while it is set, STOPs it when cleared. `capture_available` is fixed at
    /// driver open (whether the device offered a D_INPUT stream): false => no
    /// `source` to open.
    source_mirror: VecDeque<u8>,
    source_open: bool,
    /// Whether the device offers a capture stream. Seeded at driver open and
    /// re-published by the cycle each iteration (like `started`), so it tracks a
    /// mid-run capture wedge (stop_capture's re-PREPARE failing) rather than going
    /// stale -- a post-wedge `source` open then gets a clean ENODEV, not a hang.
    pub capture_available: bool,
}

/// The daemon's cross-thread state (N-2c). The graph lives behind a try-lockable
/// mutex the CYCLE thread takes non-blockingly (a miss = "graph edit in
/// progress, replay last period") and the CONTROL thread takes blockingly for
/// brief edits, never across a 9P reply.
pub struct Shared {
    pub graph: Mutex<Graph>,
    // The cycle thread parks on this word when the stream is STOPPED (there is
    // no device IRQ to wake it then); the control thread bumps it + wakes when a
    // byte write makes a voice playable, so a stopped stream starts promptly
    // instead of waiting the backstop. A ring producer in ANOTHER Proc cannot
    // poke through this same-Proc word -- that cross-Proc wake stays N-2b-2b; a
    // bounded backstop in the park covers it.
    wake: AtomicU32,
}

impl Shared {
    pub fn new(capture_available: bool) -> Shared {
        Shared {
            graph: Mutex::new(Graph::new(capture_available)),
            wake: AtomicU32::new(0),
        }
    }

    /// Wake a cycle thread parked on a stopped stream. Cheap no-op when the
    /// cycle is running (it waits on the device IRQ, not this word).
    pub fn poke_cycle(&self) {
        self.wake.fetch_add(1, Ordering::Release);
        let _ = torpor::wake_one(&self.wake);
    }

    /// The cycle thread's stopped-state park: sleep until a poke or `timeout`.
    /// Register-then-observe on `wake` closes the poke-vs-park race (no lost
    /// start).
    pub fn cycle_park(&self, timeout: Duration) {
        let seen = self.wake.load(Ordering::Acquire);
        let _ = torpor::wait(&self.wake, seen, Some(timeout));
    }
}

impl Graph {
    pub fn new(capture_available: bool) -> Graph {
        let mut voices = Vec::with_capacity(MAX_VOICES);
        voices.push(Voice::new(0, -1)); // the persistent default voice
        Graph {
            voices,
            next_id: 1,
            stats: Stats::default(),
            started: false,
            sink_audio: [100, 100],
            sink_mix: [100, 100],
            tap_mirror: VecDeque::new(),
            tap_open: false,
            source_mirror: VecDeque::new(),
            source_open: false,
            capture_available,
        }
    }

    /// True iff an authorized reader currently holds `source` (the cycle uses this
    /// to drive the on-demand RX start/stop).
    pub fn source_open(&self) -> bool {
        self.source_open
    }

    /// Try to claim the single-reader device-capture source (N-3c-2). True on
    /// success (the caller holds it AND the cycle should start capturing); false if
    /// a reader already does (-> EBUSY). Empties the mirror so a new reader never
    /// inherits a prior one's audio.
    fn source_try_open(&mut self) -> bool {
        if self.source_open {
            return false;
        }
        self.source_open = true;
        self.source_mirror.clear();
        true
    }

    /// Release the source and drop its mirror (the holder clunked or vanished);
    /// clearing `source_open` also tells the cycle to STOP capturing.
    fn source_release(&mut self) {
        self.source_open = false;
        self.source_mirror.clear();
    }

    /// Drain up to `max` bytes from the source mirror (FIFO). Empty => the reader
    /// parks until `pump_rx` fills it.
    fn source_take(&mut self, max: usize) -> Vec<u8> {
        let n = self.source_mirror.len().min(max);
        self.source_mirror.drain(..n).collect()
    }

    /// Append a captured period to the source mirror (called by the cycle's
    /// `pump_rx` sink, under the graph lock). Bounded drop-oldest: the DoS floor, so
    /// a slow reader cannot make the mirror grow without bound.
    pub fn source_push(&mut self, bytes: &[u8]) {
        if !self.source_open {
            return;
        }
        self.source_mirror.extend(bytes.iter().copied());
        if self.source_mirror.len() > TAP_MIRROR_MAX {
            let drop = self.source_mirror.len() - TAP_MIRROR_MAX;
            self.source_mirror.drain(..drop);
        }
    }

    /// Try to claim the single-reader sink tap (N-3c-1). True on success (the
    /// caller now holds it); false if a reader already does (-> EBUSY). The
    /// mirror starts empty so a new reader never inherits a prior one's audio.
    fn tap_try_open(&mut self) -> bool {
        if self.tap_open {
            return false;
        }
        self.tap_open = true;
        self.tap_mirror.clear();
        true
    }

    /// Release the tap and drop its mirror (the holder clunked or vanished).
    fn tap_release(&mut self) {
        self.tap_open = false;
        self.tap_mirror.clear();
    }

    /// Drain up to `max` bytes from the tap mirror (FIFO). An empty vec means the
    /// mirror is empty -- the caller parks until `next_period` fills it.
    fn tap_take(&mut self, max: usize) -> Vec<u8> {
        let n = self.tap_mirror.len().min(max);
        self.tap_mirror.drain(..n).collect()
    }

    fn voice_pos(&self, id: u32) -> Option<usize> {
        self.voices.iter().position(|v| v.id == id)
    }

    /// The connection handle that minted `id`, or None if no such voice. Voice 0
    /// returns -1 (the persistent, world-shared default, exempt from the owner
    /// gate).
    fn voice_owner(&self, id: u32) -> Option<i64> {
        self.voice_pos(id).map(|i| self.voices[i].owner)
    }

    /// Lazily allocate + share voice `id`'s zero-copy ring; idempotent (returns
    /// the stored geometry). None on OOM / registry-full / a missing voice, so
    /// the caller falls back to the byte-copy `audio` write. Voice 0 has no ring
    /// (the world-shared byte sink); callers gate that before here.
    fn weft_ensure(&mut self, id: u32) -> Option<(u64, u64, u32)> {
        let i = self.voice_pos(id)?;
        if let Some(r) = &self.voices[i].ring {
            return Some((r.share_id, r.ring_size, r.geom.ring_entries)); // idempotent
        }
        // ANON, RW, demand-zero -- satisfies SYS_WEFT_SHARE's ANON + RW-only +
        // whole-ring check; page-aligned so the later detach matches.
        let ring_va = unsafe { t_burrow_attach(RING_SIZE) };
        if ring_va < 0 {
            return None;
        }
        let ring_va = ring_va as u64;
        let geom = match unsafe { weftlib::init_ring(ring_va as *mut u8, RING_SIZE, RING_ENTRIES) } {
            Some(g) => g,
            None => {
                unsafe {
                    let _ = t_burrow_detach(ring_va, RING_SIZE);
                }
                return None;
            }
        };
        // The kernel takes the I-30 registration pin (the ring lives across the
        // correlation window even if nocturned detaches its own mapping) and
        // mints the kernel-scoped, consume-once share_id.
        let share_id = unsafe { t_weft_share(ring_va, RING_SIZE) };
        if share_id <= 0 {
            unsafe {
                let _ = t_burrow_detach(ring_va, RING_SIZE);
            }
            return None;
        }
        self.voices[i].ring = Some(RingVoice {
            ring_va,
            ring_size: RING_SIZE,
            share_id: share_id as u64,
            geom,
        });
        Some((share_id as u64, RING_SIZE, RING_ENTRIES))
    }

    /// Total buffered bytes across every voice (the device idle-stop reads this).
    pub fn fifo_len(&self) -> usize {
        self.voices.iter().map(|v| v.fifo.len()).sum()
    }

    /// True if any voice has data to play -- byte FIFO bytes OR published ring
    /// periods (N-2b-2). The device start condition and the idle-stop both read
    /// this so a ring-only voice (empty FIFO) still starts and holds the stream.
    /// A ring producer writing to a STOPPED stream generates no poll event, so
    /// the serve loop's bounded idle poll is what notices it (the wake poke is
    /// N-2b-2b).
    pub fn has_playable(&self) -> bool {
        if self.fifo_len() > 0 {
            return true;
        }
        self.voices
            .iter()
            .any(|v| v.ring.as_ref().map_or(false, |r| r.pending() > 0))
    }

    /// Clear every voice's FIFO. Used when the device stream fails to start:
    /// the backlog cannot play, so drop it rather than wedge the idle-stop.
    pub fn drop_fifo(&mut self) {
        for v in self.voices.iter_mut() {
            v.fifo.clear();
        }
    }

    /// Mint a voice owned by `conn`; returns its id, or None at the cap.
    fn mint_voice(&mut self, conn: i64) -> Option<u32> {
        if self.voices.len() >= MAX_VOICES {
            return None;
        }
        let id = self.next_id;
        self.next_id = self.next_id.wrapping_add(1);
        // wrapping_add is defensive only: MAX_VOICES caps live voices at 16, so
        // 2^32 mints (id reuse) is unreachable; guard against a live collision
        // regardless, so a wrapped id can never alias a living voice.
        if id == 0 || self.voice_pos(id).is_some() {
            return None;
        }
        self.voices.push(Voice::new(id, conn));
        Some(id)
    }

    /// Drop every voice a closing connection owned (never voice 0).
    pub fn drop_conn_voices(&mut self, conn: i64) {
        self.voices.retain(|v| v.id == 0 || v.owner != conn);
    }

    fn drop_fifo_voice(&mut self, id: u32) {
        if let Some(i) = self.voice_pos(id) {
            self.voices[i].fifo.clear();
            self.voices[i].flushes = self.voices[i].flushes.saturating_add(1);
        }
    }

    /// Mix one period from every voice into `buf` (S16LE stereo). Each voice
    /// contributes whole frames in float32 scaled by its gain; the sum is
    /// clamped to the S16 range. An empty voice contributes silence. Returns
    /// true if ANY voice supplied real data (false = pure silence, which the
    /// idle-stop counts).
    ///
    /// I-14 posture at the graph layer: the accumulator is f32 so N unity
    /// voices cannot integer-overflow the sink sample; the clamp is the only
    /// place a hot mix is bounded, exactly once.
    pub fn next_period(&mut self, buf: &mut [u8]) -> bool {
        // buf is always PERIOD_BYTES; bound nsamp to the fixed scratch anyway so
        // an over-long buf can never slice-panic mix[..nsamp].
        let nsamp = (buf.len() / 2).min(PERIOD_BYTES / 2); // i16 samples (2 per frame)
        let mut mix = [0f32; PERIOD_BYTES / 2];
        let mix = &mut mix[..nsamp];
        let mut any = false;
        // One scratch period reused across ring voices; the zero-copy ring
        // (N-2b-2) is drained through it, one period per voice per call.
        let mut ring_scratch = [0u8; PERIOD_BYTES];
        for v in self.voices.iter_mut() {
            let g = v.gain;
            // A ring voice reads the zero-copy Weft ring; a byte voice its FIFO.
            // The two are mutually exclusive per voice (a ring client does not
            // also byte-write audio); voice 0 never has a ring.
            if let Some(r) = v.ring.as_ref() {
                let n = r.consume_period(&mut ring_scratch);
                // Whole frames only, bounded to the mix width.
                let have = n.min(buf.len());
                let have = have - (have % FRAME);
                if have == 0 {
                    continue;
                }
                any = true;
                let samples = have / 2;
                for (k, m) in mix.iter_mut().take(samples).enumerate() {
                    let lo = ring_scratch[2 * k] as u16;
                    let hi = ring_scratch[2 * k + 1] as u16;
                    let s = (lo | (hi << 8)) as i16;
                    *m += s as f32 * g;
                }
                continue;
            }
            // Whole frames only: a torn frame would shift the channel phase.
            let have = v.fifo.len().min(buf.len());
            let have = have - (have % FRAME);
            if have == 0 {
                continue;
            }
            any = true;
            let samples = have / 2;
            for m in mix.iter_mut().take(samples) {
                let lo = v.fifo.pop_front().unwrap_or(0) as u16;
                let hi = v.fifo.pop_front().unwrap_or(0) as u16;
                let s = (lo | (hi << 8)) as i16;
                *m += s as f32 * g;
            }
        }
        // The sink gain stage (N-3a): scale each channel by its effective Plan 9
        // volume(3) gain before the single I-14 clamp. 0..=100 -> 0.0..=1.0, so
        // this only ever attenuates -- it can never push the mix past the clamp.
        let g_l = (self.sink_audio[0] as f32 / 100.0) * (self.sink_mix[0] as f32 / 100.0);
        let g_r = (self.sink_audio[1] as f32 / 100.0) * (self.sink_mix[1] as f32 / 100.0);
        for (i, m) in mix.iter().enumerate() {
            let s = *m * if i & 1 == 0 { g_l } else { g_r };
            let clamped = if s > 32767.0 {
                32767i16
            } else if s < -32768.0 {
                -32768i16
            } else {
                s as i16
            };
            let b = (clamped as u16).to_le_bytes();
            buf[2 * i] = b[0];
            buf[2 * i + 1] = b[1];
        }
        // N-3c-1: mirror the final mixed output to the sink tap while an
        // authorized reader holds it (post-gain, post-clamp -- what actually
        // plays, so a muted sink taps silence). Bounded drop-oldest: append,
        // then trim the oldest so a stalled reader cannot grow the mirror.
        if self.tap_open {
            let out = &buf[..nsamp * 2];
            self.tap_mirror.extend(out.iter().copied());
            if self.tap_mirror.len() > TAP_MIRROR_MAX {
                let drop = self.tap_mirror.len() - TAP_MIRROR_MAX;
                self.tap_mirror.drain(..drop);
            }
        }
        // If buf held an odd trailing byte (never, PERIOD_BYTES is even), leave
        // it zeroed by the caller's cleared buffer.
        let _ = nsamp;
        any
    }

    /// Append `data` to voice `id`; returns the count accepted (0 if the voice
    /// is gone or its FIFO is full).
    fn push(&mut self, id: u32, data: &[u8]) -> usize {
        let i = match self.voice_pos(id) {
            Some(i) => i,
            None => return 0,
        };
        let v = &mut self.voices[i];
        let room = FIFO_CAP.saturating_sub(v.fifo.len());
        let n = data.len().min(room);
        v.fifo.extend(data[..n].iter().copied());
        v.bytes_in = v.bytes_in.saturating_add(n as u64);
        n
    }

    fn set_gain(&mut self, id: u32, percent: u32) -> bool {
        if let Some(i) = self.voice_pos(id) {
            // Plan 9 volume(3) grammar: 0..100 is the ordinary range; allow up
            // to 1000% for headroom, clamped so a hostile value cannot blow the
            // mix past the f32 clamp's usefulness.
            let p = percent.min(1000);
            self.voices[i].gain = p as f32 / 100.0;
            return true;
        }
        false
    }

    fn render_info(&self, out: &mut Vec<u8>) {
        let s = &self.stats;
        let buffered = self.fifo_len() as u64 + u64::from(s.last_latency_bytes);
        let text = alloc::format!(
            "device virtio-snd stream 0 playback\nformat s16c2r{}\nvoices {}\nbufsize {}\nbuffered {}\nperiod-bytes {}\nbuffer-bytes {}\nperiods {}\nstarted {}\nperiods-played {}\nsilence-periods {}\ntx-errors {}\nbad-used {}\nlatency-bytes {}\ncapture {}\ncapturing {}\nperiods-captured {}\nrx-errors {}\n",
            RATE_HZ,
            self.voices.len(),
            PERIOD_BYTES,
            buffered,
            PERIOD_BYTES,
            BUFFER_BYTES,
            PERIODS,
            u8::from(self.started),
            s.periods_played,
            s.silence_periods,
            s.tx_errors,
            s.bad_used,
            s.last_latency_bytes,
            u8::from(self.capture_available),
            u8::from(self.source_open),
            s.periods_captured,
            s.rx_errors,
        );
        out.extend_from_slice(text.as_bytes());
    }

    fn render_voice_info(&self, id: u32, out: &mut Vec<u8>) {
        if let Some(i) = self.voice_pos(id) {
            let v = &self.voices[i];
            let text = alloc::format!(
                "voice {}\ngain {}\nbuffered {}\nbytes-in {}\nflushes {}\nowner {}\n",
                v.id,
                (v.gain * 100.0) as u32,
                v.fifo.len(),
                v.bytes_in,
                v.flushes,
                v.owner,
            );
            out.extend_from_slice(text.as_bytes());
        }
    }

    fn render_ctl(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(b"nocturne n-3a: mixed voices; write s16le stereo 48000 Hz to a voice's audio; root ctl: flush; per-voice ctl: gain <percent>, flush, remove; root volume: Plan 9 volume(3) grammar (audio/mix, 0..100)\n");
    }

    /// Render the sink gain in Plan 9 volume(3) grammar (0..100 per channel).
    fn render_volume(&self, out: &mut Vec<u8>) {
        let text = alloc::format!(
            "audio {} {}\nmix {} {}\n",
            self.sink_audio[0], self.sink_audio[1], self.sink_mix[0], self.sink_mix[1],
        );
        out.extend_from_slice(text.as_bytes());
    }

    /// Apply a Plan 9 volume(3) write to the sink gain. Each line is
    /// `<control> <v>` (both channels) or `<control> <l> <r>`; controls are
    /// `audio` and `mix`, values clamped to 0..=100. `dev <name>` (sink select)
    /// is N-3b (single sink today). Returns false (=> EINVAL) on an unknown
    /// control or a non-numeric value; an all-blank write is likewise EINVAL.
    fn apply_volume(&mut self, data: &[u8]) -> bool {
        // Two-pass (N-3a-3 F3): validate EVERY line into staged copies first, so
        // a malformed line late in a multi-line write leaves the sink gain
        // UNCHANGED (never a partial-apply-then-EINVAL). Commit only once every
        // line has parsed.
        let mut new_audio = self.sink_audio;
        let mut new_mix = self.sink_mix;
        let mut any = false;
        for line in data.split(|&b| b == b'\n') {
            let line = trim_line(line);
            if line.is_empty() {
                continue;
            }
            let mut it = line.split(|&b| b == b' ').filter(|t| !t.is_empty());
            let ctl = match it.next() {
                Some(c) => c,
                None => continue,
            };
            let v0 = match it.next().and_then(parse_u32) {
                Some(v) => v.min(100),
                None => return false,
            };
            let v1 = match it.next() {
                Some(t) => match parse_u32(t) {
                    Some(v) => v.min(100),
                    None => return false,
                },
                None => v0,
            };
            match ctl {
                b"audio" => new_audio = [v0, v1],
                b"mix" => new_mix = [v0, v1],
                _ => return false,
            }
            any = true;
        }
        if any {
            self.sink_audio = new_audio;
            self.sink_mix = new_mix;
        }
        any
    }
}

#[derive(Copy, Clone)]
struct Fid {
    fid: u32,
    path: u64,
    opened: bool,
    /// The voice minted when this fid opened nodes/new; -1 if this is not a
    /// freshly-minted new fid. A read of such a fid returns the id decimal.
    minted: i64,
}

enum Disp {
    Reply(usize),
    Deferred,
    Fatal,
}

/// A Twrite to a voice's audio that found the FIFO full: the bytes not yet
/// accepted, the target voice, and the tag whose Rwrite is owed once they are.
struct PendingWrite {
    tag: u16,
    fid: u32,
    voice: u32,
    data: Vec<u8>,
    done: usize,
}

/// A Tread on the sink tap (N-3c-1) that found the mirror empty: parked until
/// `next_period` fills it (retried within PARKED_RETRY_MS), or failed closed
/// (EPERM) if the peer loses authority first. `count` is already clamped to
/// msize. At most one per connection (the tap is single-reader).
struct PendingTapRead {
    tag: u16,
    count: u32,
}

pub struct Conn {
    handle: i64,
    /// N-3a-3: true iff this connection arrived on the sink-authority control
    /// post (/srv/nocturne-ctl). It serves the ROOT_CHILDREN_CTL tree and is the
    /// ONLY tree whose `volume` accepts writes -- the peer here is the writer.
    control: bool,
    version_done: bool,
    msize: u32,
    fids: [Option<Fid>; MAX_FIDS],
    in_buf: Vec<u8>,
    out_buf: Vec<u8>,
    defer: bool,
    pending: Vec<PendingWrite>,
    /// N-3c-1: the fid on THIS connection holding the single-reader sink tap
    /// (control post only), or None. Cleared on that fid's clunk or on teardown,
    /// which also releases the graph's tap guard + drops the mirror.
    tap_fid: Option<u32>,
    /// A parked Tread on the tap awaiting a mixed period (or a fail-closed EPERM).
    pending_tap_read: Option<PendingTapRead>,
    /// N-3c-2: the fid holding the single-reader device-capture `source` (control
    /// post only), or None. Cleared on that fid's clunk/teardown, which releases the
    /// graph's source guard (STOPPING the on-demand capture) + drops the mirror.
    source_fid: Option<u32>,
    /// A parked Tread on `source` awaiting a captured period (or a fail-closed EPERM).
    pending_source_read: Option<PendingTapRead>,
}

pub fn post_srv_nocturne() -> Result<i64, ()> {
    post_srv(b"nocturne")
}

/// The sink-authority control post (/srv/nocturne-ctl, N-3a-3). NOT mounted:
/// controllers connect per-conn (open=connect) so the volume write's peer is
/// the writer, and the 6.8 gate judges the real caller. The `-ctl` companion
/// name follows /srv/stratum-ctl.
pub fn post_srv_nocturne_ctl() -> Result<i64, ()> {
    post_srv(b"nocturne-ctl")
}

fn post_srv(name: &[u8]) -> Result<i64, ()> {
    let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
    if srv < 0 {
        return Err(());
    }
    let listener = unsafe { t_walk_create(srv, name.as_ptr(), name.len(), T_OREAD, 0) };
    let _ = unsafe { t_close(srv) };
    if listener < 0 {
        return Err(());
    }
    Ok(listener)
}

impl Conn {
    pub fn new(handle: i64, control: bool) -> Conn {
        Conn {
            handle,
            control,
            version_done: false,
            msize: SRV_MSIZE,
            fids: [None; MAX_FIDS],
            in_buf: Vec::new(),
            out_buf: Vec::new(),
            defer: false,
            pending: Vec::new(),
            tap_fid: None,
            pending_tap_read: None,
            source_fid: None,
            pending_source_read: None,
        }
    }

    pub fn handle(&self) -> i64 {
        self.handle
    }

    pub fn teardown(&mut self, sh: &Shared) {
        for slot in self.fids.iter_mut() {
            *slot = None;
        }
        self.pending.clear();
        self.pending_tap_read = None;
        self.pending_source_read = None;
        // Every voice this connection minted dies with it; and if this conn held
        // the single-reader sink tap (N-3c-1) or device-capture source (N-3c-2),
        // release it so a vanished reader never wedges the guard -- releasing the
        // source also clears source_open, so the cycle STOPS the RX stream. One
        // lock spans all.
        let held_tap = self.tap_fid.take().is_some();
        let held_source = self.source_fid.take().is_some();
        let mut g = sh.graph.lock();
        g.drop_conn_voices(self.handle);
        if held_tap {
            g.tap_release();
        }
        if held_source {
            g.source_release();
        }
    }

    /// True iff this connection has parked I/O awaiting the cycle thread: a
    /// parked write (FIFO room) or a parked tap read (a mixed period). The
    /// control loop shortens its poll timeout while any conn does, so a parked
    /// op completes within about a period of the cycle producing.
    pub fn has_pending(&self) -> bool {
        !self.pending.is_empty()
            || self.pending_tap_read.is_some()
            || self.pending_source_read.is_some()
    }

    /// Retry the parked writes in order; a fully-accepted one gets its Rwrite.
    /// False if the connection's reply write failed (close it).
    pub fn poll_writes(&mut self, sh: &Shared) -> bool {
        while !self.pending.is_empty() {
            let (tag, total, finished, poke) = {
                let pw = &mut self.pending[0];
                // Lock only for the push; the Rwrite send below runs unlocked.
                let (accepted, gone, was_stopped) = {
                    let mut g = sh.graph.lock();
                    let n = g.push(pw.voice, &pw.data[pw.done..]);
                    // A voice that vanished under a parked write (its conn is us,
                    // so this cannot happen for our own voice; defensive) drains
                    // as accepted so the reply is not stuck forever.
                    (n, g.voice_pos(pw.voice).is_none(), !g.started)
                };
                pw.done += accepted;
                (
                    pw.tag,
                    pw.data.len(),
                    gone || pw.done >= pw.data.len(),
                    accepted > 0 && was_stopped,
                )
            };
            // New room made a stopped stream playable: wake the cycle thread.
            // (While writes are parked the stream is normally running, so this is
            // a rare edge; the poke is a cheap no-op when the cycle is running.)
            if poke {
                sh.poke_cycle();
            }
            if !finished {
                return true; // still parked; keep order
            }
            self.pending.remove(0);
            self.out_buf.clear();
            self.out_buf.resize(SRV_MSIZE_USIZE, 0);
            match p9::build_rwrite(&mut self.out_buf, tag, total as u32) {
                Ok(len) => {
                    if !self.send_all(len) {
                        return false;
                    }
                }
                Err(()) => return false,
            }
        }
        // N-3c-1: retry a parked tap read. The cycle thread fills the mirror at
        // period rate; re-check authority FRESH here so a mid-recording
        // revocation (caps dropped, or the console owner changed) fails closed.
        if let Some(ptr) = self.pending_tap_read.take() {
            if !self.sink_authorized() {
                self.out_buf.clear();
                self.out_buf.resize(SRV_MSIZE_USIZE, 0);
                match p9::build_rlerror(&mut self.out_buf, ptr.tag, p9::E_PERM) {
                    Ok(len) => {
                        if !self.send_all(len) {
                            return false;
                        }
                    }
                    Err(()) => return false,
                }
            } else {
                let bytes = sh.graph.lock().tap_take(ptr.count as usize);
                if bytes.is_empty() {
                    self.pending_tap_read = Some(ptr); // still empty -- keep parked
                } else {
                    self.out_buf.clear();
                    self.out_buf.resize(SRV_MSIZE_USIZE, 0);
                    match p9::build_rread(&mut self.out_buf, ptr.tag, &bytes) {
                        Ok(len) => {
                            if !self.send_all(len) {
                                return false;
                            }
                        }
                        Err(()) => return false,
                    }
                }
            }
        }
        // N-3c-2: retry a parked device-capture `source` read (the RX twin of the
        // tap retry above). The cycle's pump_rx fills the mirror at capture-period
        // rate; re-check authority FRESH so a mid-recording revocation fails closed.
        if let Some(ptr) = self.pending_source_read.take() {
            if !self.sink_authorized() {
                self.out_buf.clear();
                self.out_buf.resize(SRV_MSIZE_USIZE, 0);
                match p9::build_rlerror(&mut self.out_buf, ptr.tag, p9::E_PERM) {
                    Ok(len) => {
                        if !self.send_all(len) {
                            return false;
                        }
                    }
                    Err(()) => return false,
                }
            } else {
                let bytes = sh.graph.lock().source_take(ptr.count as usize);
                if bytes.is_empty() {
                    self.pending_source_read = Some(ptr); // still empty -- keep parked
                } else {
                    self.out_buf.clear();
                    self.out_buf.resize(SRV_MSIZE_USIZE, 0);
                    match p9::build_rread(&mut self.out_buf, ptr.tag, &bytes) {
                        Ok(len) => {
                            if !self.send_all(len) {
                                return false;
                            }
                        }
                        Err(()) => return false,
                    }
                }
            }
        }
        true
    }

    fn fid_find(&self, fid: u32) -> Option<usize> {
        self.fids.iter().position(|f| matches!(f, Some(e) if e.fid == fid))
    }

    fn fid_set(&mut self, fid: u32, path: u64) -> bool {
        if let Some(i) = self.fid_find(fid) {
            self.fids[i] = Some(Fid { fid, path, opened: false, minted: -1 });
            return true;
        }
        if let Some(i) = self.fids.iter().position(|f| f.is_none()) {
            self.fids[i] = Some(Fid { fid, path, opened: false, minted: -1 });
            return true;
        }
        false
    }

    /// Read available bytes and dispatch every complete frame (the ptyfs shape).
    pub fn service(&mut self, sh: &Shared) -> bool {
        let cur = self.in_buf.len();
        if cur >= SRV_MSIZE_USIZE {
            return false;
        }
        let want = SRV_MSIZE_USIZE - cur;
        self.in_buf.resize(cur + want, 0);
        let n = unsafe { libthyla_rs::t_read(self.handle, self.in_buf.as_mut_ptr().add(cur), want) };
        if n <= 0 {
            self.in_buf.truncate(cur);
            return false;
        }
        self.in_buf.truncate(cur + n as usize);

        loop {
            if self.in_buf.len() < p9::P9_HDR_LEN {
                return true;
            }
            let hdr = match p9::peek_header(&self.in_buf) {
                Ok(h) => h,
                Err(_) => return false,
            };
            let size = hdr.size as usize;
            if !(p9::P9_HDR_LEN..=SRV_MSIZE_USIZE).contains(&size) {
                return false;
            }
            if self.in_buf.len() < size {
                return true;
            }
            let frame: Vec<u8> = self.in_buf[..size].to_vec();
            match self.dispatch(sh, &frame, hdr) {
                Disp::Fatal => return false,
                Disp::Deferred => {}
                Disp::Reply(rlen) => {
                    if !self.send_all(rlen) {
                        return false;
                    }
                }
            }
            self.in_buf.drain(..size);
        }
    }

    fn dispatch(&mut self, sh: &Shared, tmsg: &[u8], hdr: p9::Header) -> Disp {
        let tag = hdr.tag;
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        let r = match hdr.mtype {
            p9::P9_TVERSION => self.h_version(tmsg, tag),
            p9::P9_TATTACH => self.h_attach(tmsg, tag),
            p9::P9_TWALK => self.h_walk(sh, tmsg, tag),
            p9::P9_TLOPEN => self.h_lopen(sh, tmsg, tag),
            p9::P9_TREAD => self.h_read(sh, tmsg, tag),
            p9::P9_TWRITE => self.h_write(sh, tmsg, tag),
            p9::P9_TREADDIR => self.h_readdir(sh, tmsg, tag),
            p9::P9_TGETATTR => self.h_getattr(tmsg, tag),
            p9::P9_TCLUNK => self.h_clunk(sh, tmsg, tag),
            p9::P9_TFLUSH => self.h_flush(tmsg, tag),
            p9::P9_TWEFT => self.h_weft(sh, tmsg, tag),
            _ => self.err(tag, p9::E_NOSYS),
        };
        if self.defer {
            self.defer = false;
            return Disp::Deferred;
        }
        let len = r.unwrap_or_else(|_| {
            self.out_buf.clear();
            self.out_buf.resize(SRV_MSIZE_USIZE, 0);
            p9::build_rlerror(&mut self.out_buf, tag, p9::E_PROTO).unwrap_or(0)
        });
        if len == 0 {
            Disp::Fatal
        } else {
            Disp::Reply(len)
        }
    }

    fn send_all(&mut self, rlen: usize) -> bool {
        let mut sent = 0usize;
        while sent < rlen {
            let w = unsafe { libthyla_rs::t_write(self.handle, self.out_buf.as_ptr().add(sent), rlen - sent) };
            if w <= 0 {
                return false;
            }
            sent += w as usize;
        }
        true
    }

    fn err(&mut self, tag: u16, code: u32) -> Result<usize, ()> {
        p9::build_rlerror(&mut self.out_buf, tag, code)
    }

    /// I-46 / NOCTURNE.md 6.8: may this connection's peer exercise the
    /// SYSTEM-owned sink authority -- the volume WRITE and the tap READ (N-3c-1)?
    /// The two-axis rule (the console-owner SESSION -- the person at the keyboard
    /// -- OR the `audio-graph` clearance), plus the SYSTEM TCB and the
    /// CAP_HOSTOWNER admin axis. Read FRESH per operation via SYS_SRV_PEER (caps
    /// mutate), fail-closed on a dead/unknown peer.
    ///
    /// The console axis is SRV_PEER_FLAG_CONSOLE_OWNER (the peer's session owns
    /// the console), NOT the `console` field (console-ATTACHMENT, which I-27
    /// makes corvus-only -- the N-3a-2 F2). It is trustworthy ONLY on the
    /// per-connection control post, where the peer is the caller; a shared mount
    /// presents the mounter (SYSTEM), which this predicate would ADMIT -- so the
    /// `!self.control` guard at each authority site is what keeps sink authority
    /// off the mount (the F1 lesson), never this predicate alone.
    fn sink_authorized(&self) -> bool {
        let mut info = TSrvPeerInfo::default();
        if unsafe { t_srv_peer(self.handle, &mut info) } != 0 || info.alive != 1 {
            return false;
        }
        info.principal_id == T_PRINCIPAL_SYSTEM
            || (info.caps & T_CAP_HOSTOWNER) != 0
            || (info.caps & T_CAP_AUDIO_GRAPH) != 0
            || (info.flags & T_SRV_PEER_FLAG_CONSOLE_OWNER) != 0
    }

    fn qid_of(path: u64) -> p9::Qid {
        p9::Qid {
            kind: if is_dir(path) { p9::P9_QTDIR } else { p9::P9_QTFILE },
            version: 0,
            path,
        }
    }

    fn h_version(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tversion(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let negotiated = a.msize.min(SRV_MSIZE);
        for slot in self.fids.iter_mut() {
            *slot = None;
        }
        self.pending.clear();
        self.msize = negotiated;
        let ver: &[u8] = if a.version == P9_VERSION_9P2000_L {
            self.version_done = true;
            P9_VERSION_9P2000_L
        } else {
            self.version_done = false;
            b"unknown"
        };
        p9::build_rversion(&mut self.out_buf, tag, negotiated, ver)
    }

    fn h_attach(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        if !self.version_done {
            return self.err(tag, p9::E_PROTO);
        }
        let a = match p9::parse_tattach(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        if a.afid != p9::P9_NOFID {
            return self.err(tag, p9::E_OPNOTSUPP);
        }
        if a.fid == p9::P9_NOFID || self.fid_find(a.fid).is_some() {
            return self.err(tag, p9::E_INVAL);
        }
        if !self.fid_set(a.fid, P_ROOT) {
            return self.err(tag, p9::E_NOMEM);
        }
        p9::build_rattach(&mut self.out_buf, tag, &Conn::qid_of(P_ROOT))
    }

    /// Resolve one path component from `cur`. Returns the child path, or None.
    /// `control` selects which root the connection sees (the sink-authority
    /// tree on the control post, the playback tree otherwise).
    fn walk_child(g: &Graph, cur: u64, name: &[u8], control: bool) -> Option<u64> {
        if name == b".." || name == b"." {
            // ".." off a voice leaf/dir climbs to nodes/, off nodes/ to root.
            return Some(match cur {
                P_ROOT => P_ROOT,
                P_NODES => P_ROOT,
                _ if is_voice(cur) && vleaf(cur) == VLEAF_DIR => P_NODES,
                _ if is_voice(cur) => vpath(vid(cur), VLEAF_DIR),
                _ => P_ROOT,
            });
        }
        match cur {
            P_ROOT => root_children(control)
                .iter()
                .find(|(nm, _, _)| *nm == name)
                .map(|(_, p, _)| *p),
            P_NODES => {
                if let Some((_, p, _)) = NODES_STATIC.iter().find(|(nm, _, _)| *nm == name) {
                    return Some(*p);
                }
                // A decimal voice id that names a live voice.
                let id = parse_u32(name)?;
                if g.voice_pos(id).is_some() {
                    Some(vpath(id, VLEAF_DIR))
                } else {
                    None
                }
            }
            _ if is_voice(cur) && vleaf(cur) == VLEAF_DIR => {
                let id = vid(cur);
                VOICE_CHILDREN
                    .iter()
                    .find(|(nm, _, _)| *nm == name)
                    .map(|(_, leaf, _)| vpath(id, *leaf))
            }
            _ => None,
        }
    }

    fn h_walk(&mut self, sh: &Shared, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_twalk(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if f.opened {
            return self.err(tag, p9::E_PROTO);
        }
        if a.newfid != a.fid && self.fid_find(a.newfid).is_some() {
            return self.err(tag, p9::E_INVAL);
        }
        let mut cur = f.path;
        let control = self.control;
        let mut qids: [p9::Qid; p9::P9_MAX_WALK] = [p9::Qid::default(); p9::P9_MAX_WALK];
        let mut n = 0usize;
        {
            // One lock for the whole (bounded) walk; released before the reply.
            let g = sh.graph.lock();
            for k in 0..(a.nwname as usize).min(p9::P9_MAX_WALK) {
                match Conn::walk_child(&g, cur, a.names[k], control) {
                    Some(p) => {
                        cur = p;
                        qids[n] = Conn::qid_of(p);
                        n += 1;
                    }
                    None => break,
                }
            }
        }
        if a.nwname > 0 && n == 0 {
            return self.err(tag, p9::E_NOENT);
        }
        if n == a.nwname as usize && !self.fid_set(a.newfid, cur) {
            return self.err(tag, p9::E_NOMEM);
        }
        p9::build_rwalk(&mut self.out_buf, tag, &qids[..n])
    }

    fn h_lopen(&mut self, sh: &Shared, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tlopen(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if f.opened {
            return self.err(tag, p9::E_PROTO);
        }
        // Opening nodes/new MINTS a voice owned by this connection; the fid
        // remembers the id so a read returns it (the tapestry surface/new idiom).
        let minted = if f.path == P_NODES_NEW {
            match sh.graph.lock().mint_voice(self.handle) {
                Some(id) => id as i64,
                None => return self.err(tag, p9::E_NOMEM),
            }
        } else {
            -1
        };
        // N-3c-1: opening the sink tap requires (a) the CONTROL post -- the tap
        // never rides the shared mount whose peer is the mounter=SYSTEM (the F1
        // lesson; sink_authorized would ADMIT that peer), (b) authority AT OPEN so
        // an unauthorized caller cannot seize the single-reader slot and wedge it,
        // and (c) the single-reader guard. Recorded on the fid so clunk/teardown
        // release it.
        if f.path == P_TAP {
            if !self.control || !self.sink_authorized() {
                return self.err(tag, p9::E_PERM);
            }
            if !sh.graph.lock().tap_try_open() {
                return self.err(tag, p9::E_BUSY);
            }
            self.tap_fid = Some(f.fid);
        }
        // N-3c-2: opening the device-capture `source` -- the RX twin of the tap.
        // Authority FIRST (an unauthorized caller gets EPERM without learning
        // whether a capture device even exists), THEN capture-availability (a clean
        // ENODEV for an authorized caller on a box with no D_INPUT stream), THEN the
        // single-reader claim. Opening it turns capture ON: source_open drives the
        // cycle to START the RX stream, so poke the (possibly parked) cycle.
        if f.path == P_SOURCE {
            if !self.control || !self.sink_authorized() {
                return self.err(tag, p9::E_PERM);
            }
            {
                let mut g = sh.graph.lock();
                if !g.capture_available {
                    return self.err(tag, p9::E_NODEV);
                }
                if !g.source_try_open() {
                    return self.err(tag, p9::E_BUSY);
                }
            }
            self.source_fid = Some(f.fid);
            sh.poke_cycle();
        }
        self.fids[i] = Some(Fid {
            fid: f.fid,
            path: f.path,
            opened: true,
            minted,
        });
        p9::build_rlopen(&mut self.out_buf, tag, &Conn::qid_of(f.path), 0)
    }

    fn read_text(&mut self, tag: u16, off: u64, count: u32, text: &[u8]) -> Result<usize, ()> {
        let off = off as usize;
        if off >= text.len() {
            return p9::build_rread(&mut self.out_buf, tag, &[]);
        }
        let cap = (self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4);
        let k = (text.len() - off).min(count as usize).min(cap);
        p9::build_rread(&mut self.out_buf, tag, &text[off..off + k])
    }

    fn h_read(&mut self, sh: &Shared, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tread(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened {
            return self.err(tag, p9::E_PROTO);
        }
        // A freshly-minted nodes/new fid: its read is the new voice's id.
        if f.path == P_NODES_NEW && f.minted >= 0 {
            let text = alloc::format!("{}\n", f.minted);
            return self.read_text(tag, a.offset, a.count, text.as_bytes());
        }
        if is_dir(f.path) {
            return self.err(tag, p9::E_ISDIR);
        }
        // N-3c-1: the mount `/dev/nocturne/audio` READ is refused. Recording is
        // the gated /srv/nocturne-ctl/tap, never this shared-mount file -- a
        // shared mount cannot carry the per-reader identity a tap needs (the F1
        // lesson). Playback (write) is unchanged. (Was the audio(3) output-only
        // EOF; now an explicit refusal so the recording boundary is discoverable.)
        if f.path == P_AUDIO {
            return self.err(tag, p9::E_PERM);
        }
        // A per-voice `audio` read stays the audio(3) output-only EOF: it is the
        // caller's OWN write-only voice, so it leaks nothing.
        if is_voice(f.path) && vleaf(f.path) == VLEAF_AUDIO {
            return p9::build_rread(&mut self.out_buf, tag, &[]);
        }
        // N-3c-1: the sink tap -- an ear on the mixed output. Only on the control
        // post (the F1 guard: a mount peer is SYSTEM, which sink_authorized would
        // admit) and gated FRESH per read (authority mutates mid-recording),
        // fail-closed. Serve buffered bytes; park when the mirror is empty (the
        // cycle fills it at period rate; a stopped/idle sink yields nothing until
        // playback resumes).
        if f.path == P_TAP {
            if !self.control || !self.sink_authorized() {
                return self.err(tag, p9::E_PERM);
            }
            let cap = (self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4);
            let want = (a.count as usize).min(cap);
            // A zero-count read gets a zero-count reply -- never park (a parked
            // want=0 would never be satisfiable, wedging the single-reader slot).
            if want == 0 {
                return p9::build_rread(&mut self.out_buf, tag, &[]);
            }
            // F1 (round-7): at most ONE outstanding tap read per connection.
            // `pending_tap_read` is a single slot, so a second concurrent read (a
            // pipelined distinct tag) would either CLOBBER the parked one (a lost
            // reply) or -- if the cycle filled the mirror between poll passes --
            // drain it AHEAD of the parked first read (a reordered stream). The
            // tap is single-reader realtime: one read at a time, refuse the rest.
            if self.pending_tap_read.is_some() {
                return self.err(tag, p9::E_BUSY);
            }
            let bytes = sh.graph.lock().tap_take(want);
            if bytes.is_empty() {
                // Park: poll_writes replies once the cycle fills the mirror.
                self.pending_tap_read = Some(PendingTapRead { tag, count: want as u32 });
                self.defer = true;
                return Ok(0); // ignored: dispatch returns Disp::Deferred
            }
            return p9::build_rread(&mut self.out_buf, tag, &bytes);
        }
        // N-3c-2: the device-capture `source` -- the RX twin of the tap read. Same
        // guards verbatim: control post only (a mount peer is SYSTEM, which
        // sink_authorized would admit), gated FRESH per read (fail-closed on a
        // mid-recording authority loss), want==0 => an immediate empty reply (never
        // park a want=0, which would wedge the single-reader slot), at most one
        // outstanding read (a pipelined second would clobber/reorder), and park on
        // an empty mirror (the cycle's pump_rx fills it at capture-period rate).
        if f.path == P_SOURCE {
            if !self.control || !self.sink_authorized() {
                return self.err(tag, p9::E_PERM);
            }
            let cap = (self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4);
            let want = (a.count as usize).min(cap);
            if want == 0 {
                return p9::build_rread(&mut self.out_buf, tag, &[]);
            }
            if self.pending_source_read.is_some() {
                return self.err(tag, p9::E_BUSY);
            }
            let bytes = sh.graph.lock().source_take(want);
            if bytes.is_empty() {
                self.pending_source_read = Some(PendingTapRead { tag, count: want as u32 });
                self.defer = true;
                return Ok(0); // ignored: dispatch returns Disp::Deferred
            }
            return p9::build_rread(&mut self.out_buf, tag, &bytes);
        }
        // The `data` leaf is a Weft map fid (driven by SYS_WEFT_MAP -> Tweft),
        // not a byte file; a Tread on it is a protocol error.
        if is_voice(f.path) && vleaf(f.path) == VLEAF_DATA {
            return self.err(tag, p9::E_INVAL);
        }
        let mut text: Vec<u8> = Vec::new();
        {
            let g = sh.graph.lock();
            if is_voice(f.path) {
                match vleaf(f.path) {
                    VLEAF_INFO => g.render_voice_info(vid(f.path), &mut text),
                    VLEAF_CTL => g.render_ctl(&mut text),
                    _ => {}
                }
            } else if f.path == P_INFO {
                g.render_info(&mut text);
            } else if f.path == P_VOLUME {
                g.render_volume(&mut text);
            } else {
                g.render_ctl(&mut text);
            }
        }
        self.read_text(tag, a.offset, a.count, &text)
    }

    /// A ctl verb line (`flush`, `gain <n>`, `remove`) applied to `voice`.
    /// Returns Ok(true) if the verb is known + accepted, Ok(false) if unknown.
    fn apply_ctl(g: &mut Graph, voice: u32, data: &[u8]) -> bool {
        // First token.
        let vend = data
            .iter()
            .position(|&b| b == b' ' || b == b'\n')
            .unwrap_or(data.len());
        let verb = &data[..vend];
        if verb == b"flush" {
            g.drop_fifo_voice(voice);
            true
        } else if verb == b"remove" {
            // Never remove voice 0; the connection's teardown reaps the rest,
            // but an explicit remove is allowed for a client that is done.
            if voice != 0 {
                g.voices.retain(|v| v.id != voice);
            }
            true
        } else if verb == b"gain" {
            // gain <percent>
            let rest = &data[vend..];
            let start = rest.iter().position(|&b| b != b' ').unwrap_or(rest.len());
            match parse_u32(trim_line(&rest[start..])) {
                Some(p) => g.set_gain(voice, p),
                None => false,
            }
        } else {
            false
        }
    }

    fn h_write(&mut self, sh: &Shared, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_twrite(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened {
            return self.err(tag, p9::E_PROTO);
        }

        // Which voice, if any, does this fid write audio into?
        let audio_voice: Option<u32> = if f.path == P_AUDIO {
            Some(0)
        } else if is_voice(f.path) && vleaf(f.path) == VLEAF_AUDIO {
            Some(vid(f.path))
        } else {
            None
        };

        // ctl (root or per-voice).
        let ctl_voice: Option<u32> = if f.path == P_CTL {
            Some(0) // root ctl acts on voice 0 (the default)
        } else if is_voice(f.path) && vleaf(f.path) == VLEAF_CTL {
            Some(vid(f.path))
        } else {
            None
        };

        if let Some(voice) = ctl_voice {
            // I-46(a): a per-voice ctl acts only for the connection that minted
            // the voice (voice 0 is the world-shared default, exempt). The
            // server is the authority -- an unauthorized ctl is refused
            // regardless of the advisory mode bits. One lock spans the owner
            // check + verb; released before the reply.
            let ok = {
                let mut g = sh.graph.lock();
                if voice != 0 {
                    match g.voice_owner(voice) {
                        Some(o) if o == self.handle => {}
                        Some(_) => return self.err(tag, p9::E_PERM),
                        None => return self.err(tag, p9::E_BADF),
                    }
                }
                // Root ctl historically accepts only `flush`; a per-voice ctl
                // adds gain/remove. Route both through apply_ctl (root ctl's
                // `remove` no-ops on voice 0 by the guard above).
                Conn::apply_ctl(&mut g, voice, a.data)
            };
            return if ok {
                p9::build_rwrite(&mut self.out_buf, tag, a.data.len() as u32)
            } else {
                self.err(tag, p9::E_INVAL)
            };
        }

        if let Some(voice) = audio_voice {
            if a.data.is_empty() {
                return p9::build_rwrite(&mut self.out_buf, tag, 0);
            }
            // Order matters: a write behind a parked one must queue behind it,
            // so push directly only when nothing is parked ahead.
            let was_empty = self.pending.is_empty();
            // One lock spans the existence + owner check + (when unparked) the
            // push; the push count and the stopped-state come back so the reply,
            // parking, and poke all run unlocked.
            let (accepted, was_stopped) = {
                let mut g = sh.graph.lock();
                if g.voice_pos(voice).is_none() {
                    return self.err(tag, p9::E_BADF);
                }
                // I-46(a): only the minting connection may write a voice (voice 0
                // is the world-shared default). The server enforces it directly,
                // so a cross-Proc write to a private voice is refused even though
                // the audio file's mode admits the open.
                if voice != 0 && g.voice_owner(voice) != Some(self.handle) {
                    return self.err(tag, p9::E_PERM);
                }
                let n = if was_empty { g.push(voice, a.data) } else { 0 };
                (n, !g.started)
            };
            // New room made a stopped stream playable: wake the cycle thread.
            if accepted > 0 && was_stopped {
                sh.poke_cycle();
            }
            if was_empty && accepted == a.data.len() {
                return p9::build_rwrite(&mut self.out_buf, tag, accepted as u32);
            }
            if self.pending.len() >= MAX_PENDING_WRITES {
                return self.err(tag, p9::E_NOMEM);
            }
            self.pending.push(PendingWrite {
                tag,
                fid: a.fid,
                voice,
                data: a.data.to_vec(),
                done: if was_empty { accepted } else { 0 },
            });
            self.defer = true;
            return Ok(0); // ignored: dispatch returns Disp::Deferred
        }

        if f.path == P_VOLUME {
            // N-3a-3 (NOCTURNE.md 6.8): the sink volume is writable ONLY on the
            // control post. The playback tree's volume is read-only info; the
            // MOUNT path is closed at open (mode 0o444), and a DIRECT playback
            // connection is closed here -- so no write can ride the mounter's
            // (SYSTEM) identity (the F1 root fix).
            if !self.control {
                return self.err(tag, p9::E_PERM);
            }
            // The two-axis gate is read FRESH per write (caps mutate -- a
            // clearance redeemed or expired after connect -- so an accept-time
            // snapshot would be stale) and fails closed on a dead peer. Here the
            // peer IS the writer (a per-connection control conn), so the gate
            // reads the real caller's identity/caps/console-owner session.
            if !self.sink_authorized() {
                return self.err(tag, p9::E_PERM);
            }
            let ok = {
                let mut g = sh.graph.lock();
                g.apply_volume(a.data)
            };
            return if ok {
                p9::build_rwrite(&mut self.out_buf, tag, a.data.len() as u32)
            } else {
                self.err(tag, p9::E_INVAL)
            };
        }

        self.err(tag, p9::E_PERM)
    }

    fn h_readdir(&mut self, sh: &Shared, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_treaddir(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened {
            return self.err(tag, p9::E_PROTO);
        }
        let budget = (a.count as usize).min((self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4));
        let mut data: Vec<u8> = Vec::new();
        let mut ord: u64 = 0;

        // Assemble the entry list for this directory: (name-bytes, path, dt).
        let mut push_entry = |name: &[u8], path: u64, ord: &mut u64| -> bool {
            *ord += 1;
            if *ord <= a.offset {
                return true;
            }
            if data.len() + p9::dirent_len(name.len()) > budget {
                return false;
            }
            let dt = if is_dir(path) { p9::DT_DIR } else { p9::DT_REG };
            let mut scratch = [0u8; 64 + p9::P9_QID_LEN + 8 + 1 + 2];
            match p9::pack_dirent(&mut scratch, 0, &Conn::qid_of(path), *ord, dt, name) {
                Ok(used) => {
                    data.extend_from_slice(&scratch[..used]);
                    true
                }
                Err(()) => false,
            }
        };

        match f.path {
            P_ROOT => {
                for &(name, path, _) in root_children(self.control) {
                    if !push_entry(name, path, &mut ord) {
                        break;
                    }
                }
            }
            P_NODES => {
                for (name, path, _) in NODES_STATIC {
                    if !push_entry(name, path, &mut ord) {
                        break;
                    }
                }
                // The live voices, by decimal id (voice 0 included -- it is
                // reachable as both /audio and /nodes/0). Snapshot the ids under
                // the lock, then build entries unlocked.
                let voice_ids: Vec<u32> = {
                    let g = sh.graph.lock();
                    g.voices.iter().map(|v| v.id).collect()
                };
                let mut buf16 = [0u8; 16];
                for id in voice_ids {
                    let name = fmt_u32(id, &mut buf16);
                    if !push_entry(name, vpath(id, VLEAF_DIR), &mut ord) {
                        break;
                    }
                }
            }
            _ if is_voice(f.path) && vleaf(f.path) == VLEAF_DIR => {
                for (name, leaf, _) in VOICE_CHILDREN {
                    if !push_entry(name, vpath(vid(f.path), leaf), &mut ord) {
                        break;
                    }
                }
            }
            _ => return self.err(tag, p9::E_NOTDIR),
        }
        p9::build_rreaddir(&mut self.out_buf, tag, &data)
    }

    fn h_getattr(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let fid = match p9::parse_tgetattr(tmsg) {
            Ok(f) => f,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        // volume on the control post is writable (0o666); on the mounted
        // playback tree it is read-only info (0o444, so the kernel dev9p gate
        // refuses a write-open through the mount). mode_of returns the playback
        // (0o444) mode; override for the control connection.
        let base_mode = mode_of(f.path);
        let mode = if f.path == P_VOLUME && self.control {
            S_IFREG | 0o666
        } else {
            base_mode
        };
        let nlink = if is_dir(f.path) { 2u64 } else { 1u64 };
        // The security trio must be filled: dev9p's per-component X-search reads
        // it, and an unfilled trio fails closed (the /dev/pts lesson).
        let valid = p9::P9_GETATTR_MODE
            | p9::P9_GETATTR_NLINK
            | p9::P9_GETATTR_UID
            | p9::P9_GETATTR_GID
            | P9_GETATTR_SIZE;
        p9::build_rgetattr(&mut self.out_buf, tag, valid, &Conn::qid_of(f.path), mode, 0, 0, nlink, 0)
    }

    fn h_clunk(&mut self, sh: &Shared, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tclunk(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        match self.fid_find(a.fid) {
            Some(i) => {
                self.fids[i] = None;
                self.pending.retain(|pw| pw.fid != a.fid);
                // N-3c-1: clunking the tap fid releases the single-reader guard +
                // the mirror, and abandons any parked read still on that fid.
                if self.tap_fid == Some(a.fid) {
                    self.tap_fid = None;
                    self.pending_tap_read = None;
                    sh.graph.lock().tap_release();
                }
                // N-3c-2: clunking the source fid releases the single-reader guard +
                // the mirror AND clears source_open, so the cycle STOPS the RX stream.
                if self.source_fid == Some(a.fid) {
                    self.source_fid = None;
                    self.pending_source_read = None;
                    sh.graph.lock().source_release();
                }
                p9::build_rclunk(&mut self.out_buf, tag)
            }
            None => self.err(tag, p9::E_BADF),
        }
    }

    /// Tflush(oldtag): cancel a parked write with that tag (its bytes already
    /// accepted stay queued -- Plan 9 semantics: what was buffered plays).
    fn h_flush(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tflush(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        self.pending.retain(|pw| pw.tag != a.oldtag);
        // N-3c-1: Tflush of a parked tap read cancels it (no Rread owed -- the
        // flushed request just gets the Rflush; the tap fid stays open).
        if self.pending_tap_read.as_ref().is_some_and(|p| p.tag == a.oldtag) {
            self.pending_tap_read = None;
        }
        // N-3c-2: same for a parked device-capture `source` read.
        if self.pending_source_read.as_ref().is_some_and(|p| p.tag == a.oldtag) {
            self.pending_source_read = None;
        }
        p9::build_rflush(&mut self.out_buf, tag)
    }

    /// Tweft(fid) on a voice's `data` leaf: allocate + share that voice's
    /// zero-copy ring, reply Rweft(share_id, size, entries). The kernel issues
    /// this only from a client's SYS_WEFT_MAP and maps the ring into that
    /// client. Authority (I-37/I-46a): only the connection that minted the voice
    /// (the F1 owner gate); voice 0 (the world-shared byte sink) has no ring.
    /// The kernel's consume-once share claim makes the mapper unique -- the SPSC
    /// producer -- even though mounted clients share one dev9p connection.
    fn h_weft(&mut self, sh: &Shared, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let fid = match p9::parse_tweft(tmsg) {
            Ok(f) => f,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened || !(is_voice(f.path) && vleaf(f.path) == VLEAF_DATA) {
            return self.err(tag, p9::E_INVAL);
        }
        let id = vid(f.path);
        if id == 0 {
            return self.err(tag, p9::E_INVAL);
        }
        // One lock spans the owner check + the (idempotent) ring alloc/share;
        // released before the reply. weft_ensure runs its burrow/share syscalls
        // under the lock -- bounded, once per ring voice, and it must be atomic
        // with the owner check against a concurrent teardown anyway.
        let res = {
            let mut g = sh.graph.lock();
            match g.voice_owner(id) {
                Some(o) if o == self.handle => {}
                Some(_) => return self.err(tag, p9::E_PERM),
                None => return self.err(tag, p9::E_BADF),
            }
            g.weft_ensure(id)
        };
        match res {
            Some((share_id, ring_size, ring_entries)) => {
                p9::build_rweft(&mut self.out_buf, tag, share_id, ring_size as u32, ring_entries)
            }
            None => self.err(tag, p9::E_NOMEM),
        }
    }
}

/// Parse an unsigned decimal from a byte slice (no leading/trailing space).
/// Returns None on empty or a non-digit.
fn parse_u32(b: &[u8]) -> Option<u32> {
    if b.is_empty() {
        return None;
    }
    let mut n: u32 = 0;
    for &c in b {
        if !c.is_ascii_digit() {
            return None;
        }
        n = n.checked_mul(10)?.checked_add((c - b'0') as u32)?;
    }
    Some(n)
}

/// Trim a trailing newline / spaces from a line for token parsing.
fn trim_line(b: &[u8]) -> &[u8] {
    let mut end = b.len();
    while end > 0 && (b[end - 1] == b'\n' || b[end - 1] == b' ' || b[end - 1] == b'\r') {
        end -= 1;
    }
    &b[..end]
}

/// Format a u32 into `buf`, returning the used slice (decimal, no NUL).
fn fmt_u32(mut v: u32, buf: &mut [u8; 16]) -> &[u8] {
    if v == 0 {
        buf[0] = b'0';
        return &buf[..1];
    }
    let mut tmp = [0u8; 10];
    let mut n = 0;
    while v > 0 {
        tmp[n] = b'0' + (v % 10) as u8;
        v /= 10;
        n += 1;
    }
    for i in 0..n {
        buf[i] = tmp[n - 1 - i];
    }
    &buf[..n]
}
