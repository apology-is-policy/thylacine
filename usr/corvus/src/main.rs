// /sbin/corvus — Thylacine key-agent daemon.
//
// Sub-chunks (per CORVUS-DESIGN.md §10):
//   P5-corvus-bringup-a (`7487054`):   startup hardening + readiness banner.
//   P5-corvus-bringup-b (`9cf92c6`):   /srv/corvus/ Spoor server skeleton +
//                                      binary frame wire codec + AUTH +
//                                      SESSION_CLOSE verbs (passphrase
//                                      NOT verified — placeholder).
//   P5-corvus-bringup-c (this chunk):  REAL crypto. Argon2id passphrase
//                                      verification + AEGIS-256 AEAD
//                                      wrap/unwrap + state file format
//                                      (magic CRVS) + USER_CREATE verb.
//                                      State persisted IN-MEMORY only
//                                      at this sub-chunk; FS persistence
//                                      lands at a later sub-chunk when
//                                      /var/lib/corvus/ is mounted (P5+).
//   P5-corvus-bringup-d onward:        UNWRAP + admin verbs + audit log +
//                                      ML-KEM-768 hybrid keypair half.
//
// At this sub-chunk corvus is a single-peer userspace daemon that:
//
//   1. Runs the hardening sequence at startup (unchanged from -a).
//
//   2. Initializes the static-buffer heap (24 MiB) and enters a server
//      loop reading binary frames from fd 0, writing responses on fd 1.
//      The pipe pair is installed by joey via SYS_SPAWN_FULL — corvus
//      inherits fd 0 = c2s_rd, fd 1 = s2c_wr.
//
//   3. Dispatches verb_id ∈ { AUTH, SESSION_CLOSE, USER_CREATE } per
//      CORVUS-DESIGN.md §6.4. Every other verb_id refused with
//      status=BadFormat (5).
//
//   4. USER_CREATE (verb_id=5): parses (user, passphrase, backend);
//      generates a fresh per-user salt (16 B) + AEAD nonce (32 B) via
//      t_getrandom; runs Argon2id(passphrase, salt, t=2, m=16 MiB, p=1)
//      → 32-byte KEK; generates a 64-byte placeholder keypair via
//      t_getrandom (ML-KEM-768+X25519 hybrid keypair generation lands
//      at -d); AEGIS-256-wraps the keypair under the KEK with AD =
//      "thylacine-corvus-v1" || user_name || backend_id; stores the
//      `CorvusUserState` record (magic CRVS, version, params, salt,
//      nonce, ciphertext, tag) in an in-memory Vec. Re-creating an
//      existing user is REFUSED (status=PermissionDenied).
//
//   5. AUTH (verb_id=1): parses (user, passphrase); looks up the user's
//      state; re-derives KEK via Argon2id(passphrase, stored_salt);
//      AEGIS-256-unwraps with stored nonce + AD; on tag mismatch
//      returns status=BadAuth; on success mints a 33-byte session
//      token ("s" + 32 hex chars of 128 bits CSPRNG entropy) + installs
//      single-slot session. The auth-failure path takes the same Argon2id
//      memory/time cost as the success path (no early-exit before KDF) —
//      passphrase oracle via timing is bounded by Argon2id's per-guess
//      cost (per CORVUS-DESIGN §4.5 timing-attack mitigation).
//
//   6. SESSION_CLOSE (verb_id=3): unchanged from -b — token-bound clear.
//
//   7. Exits 0 on rx EOF; non-zero on transport error.
//
// State storage at this sub-chunk: a static `BTreeMap<heapless::Vec<u8>,
// Box<CorvusUserState>>`-equivalent over the heap-backed allocator.
// Vec-of-state-records keyed by user name. Single-process; no
// concurrency. State is lost on corvus restart (audit F11 — joey
// supervises; user re-creates on first login after restart at this
// sub-chunk; FS persistence at -d makes this durable).
//
// Spec correspondence (specs/corvus.tla; P5-corvus-spec at c00de63):
//
//   AuthSuccess(p, u)     — handle_auth() insert path (now real-crypto-gated).
//   SessionClose(p)       — handle_session_close() clear path.
//   SessionUserImmutable  — Session::user has no public setter; set once
//                           at session_install() + cleared whole-record at
//                           session_clear().
//   USER_CREATE           — handle_user_create() (NEW; outside the spec's
//                           current state-machine scope, which models
//                           sessions only; USER_CREATE is the precondition
//                           for AuthSuccess to be reachable for a given
//                           user).
//   AdminElevate(p)       — NOT YET (deferred).
//   Unwrap(p, d)          — NOT YET.
//   AdminVerb(p)          — NOT YET.

#![no_std]
#![no_main]

extern crate alloc;

use libthyla_rs::{
    t_close, t_explicit_bzero, t_getrandom, t_mlockall, t_putstr, t_read, t_set_dumpable,
    t_set_traceable, t_write,
};

use alloc::vec::Vec;

// =============================================================================
// Heap allocator — required for Argon2id's working-memory matrix.
// =============================================================================
//
// linked_list_allocator backed by a fixed 24 MiB static BSS buffer.
// Initialized once at rs_main entry (BEFORE the hardening sequence so
// any panic-handler allocation works). Argon2id with m_cost=16 MiB
// (the v1.0 interactive preset; design default is 64 MiB but we cap at
// 16 MiB during bringup to keep the BSS reservation modest — the
// state-file-stored m_cost_kib field carries the actual params used,
// so future tuning is a single constant change + BSS resize).
//
// The buffer is `#[repr(align(64))]` to satisfy alignment requirements
// for any allocation up to a typical cache line. Inside is just a
// `[u8; HEAP_SIZE]`. Element-by-element access via raw pointers when
// initializing the allocator (we never take &mut to this static; same
// discipline as SESSION).
//
// On Thylacine: BSS is zero-filled by the ELF loader (kernel/elf.c)
// when the binary's PT_LOAD memsz exceeds filesz. The BSS contributes
// to memsz only, so the ramfs ELF stays small — only the load-time
// mapping grows.

use core::mem::MaybeUninit;
use linked_list_allocator::LockedHeap;

const HEAP_SIZE: usize = 24 * 1024 * 1024;

// Field is referenced only via raw pointers (addr_of_mut!) — silences
// dead_code complaint about the field never being read by value.
#[repr(align(64))]
struct AlignedHeap(#[allow(dead_code)] [MaybeUninit<u8>; HEAP_SIZE]);

static mut HEAP_BUF: AlignedHeap = AlignedHeap([MaybeUninit::uninit(); HEAP_SIZE]);

#[global_allocator]
static ALLOCATOR: LockedHeap = LockedHeap::empty();

unsafe fn heap_init() {
    let buf_ptr = core::ptr::addr_of_mut!(HEAP_BUF) as *mut u8;
    ALLOCATOR.lock().init(buf_ptr, HEAP_SIZE);
}

// =============================================================================
// State file format — CORVUS-DESIGN.md §4.3.
// =============================================================================
//
// State file layout (in-memory at this sub-chunk; FS persistence at -d):
//
//   [0..4)       magic 'CRVS'        (u32 LE = 0x53565243)
//   [4..8)       version             u32 LE = 1
//   [8..12)      t_cost              u32 LE
//   [12..20)     m_cost_kib          u64 LE
//   [20..24)     parallelism         u32 LE
//   [24..40)     argon2id_salt       16 B
//   [40..72)     aead_nonce          32 B  (AEGIS-256 nonce width)
//   [72..ct_end) ciphertext          KEYPAIR_LEN bytes (= hybrid_pk||hybrid_sk wrapped)
//   [ct_end..)   AEAD_tag            32 B  (AEGIS-256 tag width)
//
// At v1.0 first bringup the "hybrid keypair" is a 64-byte CSPRNG
// placeholder. Phase 5+ extends to ML-KEM-768 + X25519 hybrid (~1.2
// KiB) via the design's keypair-bytes contract. The KEYPAIR_LEN
// constant is the only thing that changes; the file layout's
// variable-length section absorbs the resize.

const CORVUS_MAGIC: u32 = 0x53565243; // 'CRVS' LE
const CORVUS_STATE_VERSION: u32 = 1;
const ARGON2_SALT_LEN: usize = 16;
const AEGIS256_KEY_LEN: usize = 32;
const AEGIS256_NONCE_LEN: usize = 32;
const AEGIS256_TAG_LEN: usize = 32;
const KEYPAIR_LEN: usize = 64; // CSPRNG placeholder at -c; ML-KEM+X25519 hybrid at -d.

const HEADER_LEN: usize = 4 + 4 + 4 + 8 + 4 + ARGON2_SALT_LEN + AEGIS256_NONCE_LEN; // = 72
const TOTAL_LEN: usize = HEADER_LEN + KEYPAIR_LEN + AEGIS256_TAG_LEN; // = 168 at -c

const _: () = assert!(HEADER_LEN == 72, "state file header layout drift");
const _: () = assert!(AEGIS256_KEY_LEN == 32, "AEGIS-256 key width is 32");
const _: () = assert!(AEGIS256_NONCE_LEN == 32, "AEGIS-256 nonce width is 32");
const _: () = assert!(AEGIS256_TAG_LEN == 32, "AEGIS-256 tag width is 32");

// Argon2id v1.0 preset. Design default (CORVUS-DESIGN §4.3): t_cost=2,
// m_cost=64 MiB, parallelism=1.
//
// Bringup-c uses m_cost=16 MiB (KiB units) — a quarter of the design
// default. The cap is the kernel allocator: corvus's heap is a static
// BSS buffer mapped by a single contiguous physical allocation
// (kernel/burrow.c::burrow_create_anon rounds page_count up to one
// buddy order; a 64 MiB heap needs an order-14 = 64 MiB contiguous
// block, which strains the buddy free-list after the kernel test
// suite has fragmented memory). 24 MiB heap → order-13 (32 MiB
// contiguous) is comfortably allocatable; 16 MiB m_cost fits with
// 8 MiB headroom for Vec bookkeeping.
//
// 16 MiB m_cost still imposes a meaningful per-guess Argon2id cost
// (the §4.5 timing-attack / brute-force mitigation). The state-file
// format carries `m_cost_kib` as a per-record stored field, so a
// future chunk can raise the default — new records use the higher
// cost, old records still verify at their stored cost, no format
// break. The full 64 MiB lands when the kernel grows a multi-extent
// BURROW or corvus gets a dedicated boot-time heap reservation.
const ARGON2_T_COST: u32 = 2;
const ARGON2_M_COST_KIB: u32 = 16 * 1024;
const ARGON2_PARALLELISM: u32 = 1;

// Additional Data for AEAD: "thylacine-corvus-v1" || user_name || backend_id.
// Backend ID at v1.0 is just `passphrase` = 0. Per CORVUS-DESIGN §4.3 binding
// rationale, AD prevents cross-user / cross-backend wrap substitution.
const AD_PREFIX: &[u8] = b"thylacine-corvus-v1";
const BACKEND_ID_PASSPHRASE: u8 = 0;

#[derive(Clone)]
struct CorvusUserState {
    // Stable user identity (the AUTH key).
    user: Vec<u8>,
    // Argon2id params. Per-user (a user with sensitive preset will have
    // different t_cost/m_cost_kib; bringup-c uses the interactive preset
    // for every user but the field is per-record by design).
    t_cost: u32,
    m_cost_kib: u32,
    parallelism: u32,
    // Fresh CSPRNG per state record. Re-randomized on every passphrase
    // rotation per CORVUS-DESIGN §4.3 / audit F7.
    salt: [u8; ARGON2_SALT_LEN],
    nonce: [u8; AEGIS256_NONCE_LEN],
    // AEGIS-256(KEK, nonce, AD).encrypt(keypair_plain) → (ciphertext, tag).
    ciphertext: [u8; KEYPAIR_LEN],
    tag: [u8; AEGIS256_TAG_LEN],
}

impl CorvusUserState {
    // Build the AEAD AD for this user record. Format-bound to the design
    // contract; any change here is an on-wire format break.
    fn build_ad(&self, ad_out: &mut Vec<u8>) {
        ad_out.clear();
        ad_out.extend_from_slice(AD_PREFIX);
        ad_out.extend_from_slice(&self.user);
        ad_out.push(BACKEND_ID_PASSPHRASE);
    }

    // Serialize to the wire-format byte vec (for FS persistence at -d;
    // included at -c for the round-trip test). Output is exactly
    // TOTAL_LEN bytes.
    #[allow(dead_code)] // FS persistence consumes this at -d
    fn to_bytes(&self) -> [u8; TOTAL_LEN] {
        let mut out = [0u8; TOTAL_LEN];
        out[0..4].copy_from_slice(&CORVUS_MAGIC.to_le_bytes());
        out[4..8].copy_from_slice(&CORVUS_STATE_VERSION.to_le_bytes());
        out[8..12].copy_from_slice(&self.t_cost.to_le_bytes());
        out[12..20].copy_from_slice(&(self.m_cost_kib as u64).to_le_bytes());
        out[20..24].copy_from_slice(&self.parallelism.to_le_bytes());
        out[24..40].copy_from_slice(&self.salt);
        out[40..72].copy_from_slice(&self.nonce);
        out[72..72 + KEYPAIR_LEN].copy_from_slice(&self.ciphertext);
        out[72 + KEYPAIR_LEN..72 + KEYPAIR_LEN + AEGIS256_TAG_LEN].copy_from_slice(&self.tag);
        out
    }
}

// =============================================================================
// Crypto: Argon2id KDF + AEGIS-256 AEAD.
// =============================================================================

use aegis::aegis256::Aegis256;
use argon2::{Algorithm, Argon2, Params, Version};

// Argon2id KEK derivation. Always runs at full cost (no early-exit on
// validation failure). Returns the 32-byte KEK on success, None on
// Argon2id init failure (only fires if params are out of spec; we
// hardcode known-good params here, so the None branch is dead-code
// guard).
fn argon2id_kek(
    passphrase: &[u8],
    salt: &[u8],
    t_cost: u32,
    m_cost_kib: u32,
    parallelism: u32,
) -> Option<[u8; AEGIS256_KEY_LEN]> {
    let params = Params::new(m_cost_kib, t_cost, parallelism, Some(AEGIS256_KEY_LEN)).ok()?;
    let argon = Argon2::new(Algorithm::Argon2id, Version::V0x13, params);
    let mut kek = [0u8; AEGIS256_KEY_LEN];
    argon.hash_password_into(passphrase, salt, &mut kek).ok()?;
    Some(kek)
}

// AEGIS-256 wrap. Encrypts `plaintext` into `ciphertext_out` (must be
// the same length). Returns the 32-byte tag. The `aegis` crate's
// Vec-returning encrypt() is feature="std" gated; we use the in-place
// variant which has no alloc requirement at the API level.
//
// Note: AEGIS-256 signature is `Aegis256::new(key, nonce)` — key first.
fn aegis_wrap(
    key: &[u8; AEGIS256_KEY_LEN],
    nonce: &[u8; AEGIS256_NONCE_LEN],
    ad: &[u8],
    plaintext: &[u8],
    ciphertext_out: &mut [u8],
) -> [u8; AEGIS256_TAG_LEN] {
    debug_assert_eq!(plaintext.len(), ciphertext_out.len());
    ciphertext_out.copy_from_slice(plaintext);
    let cipher = Aegis256::<AEGIS256_TAG_LEN>::new(key, nonce);
    cipher.encrypt_in_place(ciphertext_out, ad)
}

// AEGIS-256 unwrap. Returns Some(plaintext_vec) on tag-valid, None on
// tag-invalid. The crate's `decrypt_in_place` does a constant-time tag
// compare internally; we copy ciphertext into a new Vec so the caller
// gets ownership of plaintext to drop/wipe explicitly.
fn aegis_unwrap(
    key: &[u8; AEGIS256_KEY_LEN],
    nonce: &[u8; AEGIS256_NONCE_LEN],
    ad: &[u8],
    ciphertext: &[u8],
    tag: &[u8; AEGIS256_TAG_LEN],
) -> Option<Vec<u8>> {
    let mut buf: Vec<u8> = Vec::with_capacity(ciphertext.len());
    buf.extend_from_slice(ciphertext);
    let cipher = Aegis256::<AEGIS256_TAG_LEN>::new(key, nonce);
    cipher.decrypt_in_place(&mut buf, tag, ad).ok()?;
    Some(buf)
}

// =============================================================================
// User state vector — in-memory at -c.
// =============================================================================
//
// `static mut Vec<CorvusUserState>` (single-threaded; corvus is one Proc
// at v1.0). All access goes through helper functions that take the
// addr_of_mut! pointer — same discipline as SESSION.

static mut USER_STATES: Option<Vec<CorvusUserState>> = None;

unsafe fn user_states_init() {
    core::ptr::write(core::ptr::addr_of_mut!(USER_STATES), Some(Vec::new()));
}

unsafe fn user_states_find(user: &[u8]) -> Option<CorvusUserState> {
    let states = (*core::ptr::addr_of!(USER_STATES)).as_ref()?;
    for s in states {
        if s.user == user {
            return Some(s.clone());
        }
    }
    None
}

unsafe fn user_states_insert(record: CorvusUserState) -> bool {
    let states_ref = (*core::ptr::addr_of_mut!(USER_STATES)).as_mut();
    let states = match states_ref {
        Some(s) => s,
        None => return false,
    };
    for s in states.iter() {
        if s.user == record.user {
            return false;
        }
    }
    states.push(record);
    true
}

// =============================================================================
// Wire constants — CORVUS-DESIGN.md §6.4.
// =============================================================================

const VERB_AUTH: u8 = 1;
const VERB_SESSION_CLOSE: u8 = 3;
const VERB_USER_CREATE: u8 = 5;

const STATUS_OK: u8 = 0;
const STATUS_BAD_AUTH: u8 = 1;
const STATUS_PERMISSION_DENIED: u8 = 2;
const STATUS_NOT_FOUND: u8 = 3;
#[allow(dead_code)]
const STATUS_RATE_LIMITED: u8 = 4;
const STATUS_BAD_FORMAT: u8 = 5;
const STATUS_INTERNAL_ERROR: u8 = 6;

// 33 bytes = 's' + 32 hex chars (16 bytes of CSPRNG entropy hex-encoded).
const TOKEN_LEN: usize = 33;
const TOKEN_ENTROPY_BYTES: usize = 16;

const MAX_USER_LEN: usize = 32;
const MAX_PASS_LEN: usize = 256;

// Worst case: USER_CREATE = 1 + MAX_USER_LEN + 2 + MAX_PASS_LEN + 1 = 292.
// AUTH = 291. SESSION_CLOSE = TOKEN_LEN = 33. Pick the largest.
const MAX_PAYLOAD_LEN: usize = 1 + MAX_USER_LEN + 2 + MAX_PASS_LEN + 1;
// Response: largest payload is the session token (33). Frame size = 3 + 33.
const MAX_RESPONSE_FRAME: usize = 3 + TOKEN_LEN;

const RX_FD: i64 = 0;
const TX_FD: i64 = 1;

// =============================================================================
// Session table — single-slot at this skeleton.
// =============================================================================
//
// Spec (specs/corvus.tla) models Sessions as SUBSET SessionRecord with at
// most one record per owner_proc. This skeleton has one peer, so a
// single static slot suffices. Multi-slot expansion lands when corvus
// serves multiple peer Procs (per-user stratumd processes) over distinct
// Spoor pairs.
//
// Spec's identity model is (creation_proc, bound_user). At this skeleton
// we don't yet have peer Proc identity exposed on the pipe (the
// kernel-stamped /srv/corvus/peer/ surface lands at -c or later), so the
// stored creation_proc is logically "the only peer" — implicit. We
// store bound_user (the username string from AUTH) and the session
// token as the identifying state.

#[repr(C)]
struct Session {
    active: bool,
    user_len: u8,
    user: [u8; MAX_USER_LEN],
    token: [u8; TOKEN_LEN],
}

static mut SESSION: Session = Session {
    active: false,
    user_len: 0,
    user: [0; MAX_USER_LEN],
    token: [0; TOKEN_LEN],
};

// Accessor wrappers go through raw pointers + element-by-element writes
// so we don't take a &mut reference to the static (Rust 1.77+'s
// static_mut_refs lint fires on `&mut SESSION` patterns). This keeps the
// code lint-clean and Miri-honest in case the kernel ever multiplexes
// corvus's server loop in the future.

unsafe fn session_active() -> bool {
    core::ptr::read(core::ptr::addr_of!(SESSION.active))
}

unsafe fn session_install(user: &[u8], token: &[u8; TOKEN_LEN]) {
    let s = core::ptr::addr_of_mut!(SESSION);
    let user_ptr = core::ptr::addr_of_mut!((*s).user) as *mut u8;
    let token_ptr = core::ptr::addr_of_mut!((*s).token) as *mut u8;
    // Wipe + install user
    for i in 0..MAX_USER_LEN {
        core::ptr::write(user_ptr.add(i), 0);
    }
    for i in 0..user.len() {
        core::ptr::write(user_ptr.add(i), user[i]);
    }
    // Install token
    for i in 0..TOKEN_LEN {
        core::ptr::write(token_ptr.add(i), token[i]);
    }
    // user_len + active LAST so partial reads can never see a half-set state
    core::ptr::write(core::ptr::addr_of_mut!((*s).user_len), user.len() as u8);
    core::ptr::write(core::ptr::addr_of_mut!((*s).active), true);
}

unsafe fn session_token_matches(candidate: &[u8]) -> bool {
    if candidate.len() != TOKEN_LEN {
        return false;
    }
    if !session_active() {
        return false;
    }
    let token_ptr = core::ptr::addr_of!(SESSION.token) as *const u8;
    // Constant-time compare: never short-circuit on mismatch. (At this
    // skeleton the token is fresh CSPRNG entropy; not strictly required,
    // but the discipline carries forward to future secret-equality
    // checks in -c onward.)
    let mut diff: u8 = 0;
    for i in 0..TOKEN_LEN {
        let tok_byte = core::ptr::read(token_ptr.add(i));
        diff |= tok_byte ^ candidate[i];
    }
    diff == 0
}

unsafe fn session_clear() {
    let s = core::ptr::addr_of_mut!(SESSION);
    // Clear active FIRST so a concurrent reader (none at v1.0; future-
    // proof) can't observe a stale token bound to a cleared session.
    core::ptr::write(core::ptr::addr_of_mut!((*s).active), false);
    let user_ptr = core::ptr::addr_of_mut!((*s).user) as *mut u8;
    let token_ptr = core::ptr::addr_of_mut!((*s).token) as *mut u8;
    for i in 0..MAX_USER_LEN {
        core::ptr::write(user_ptr.add(i), 0);
    }
    for i in 0..TOKEN_LEN {
        core::ptr::write(token_ptr.add(i), 0);
    }
    core::ptr::write(core::ptr::addr_of_mut!((*s).user_len), 0);
}

// =============================================================================
// Hex encoding.
// =============================================================================

fn nibble_to_hex(n: u8) -> u8 {
    let n = n & 0x0f;
    if n < 10 {
        b'0' + n
    } else {
        b'a' + (n - 10)
    }
}

// =============================================================================
// Frame I/O.
// =============================================================================

// read_exact — loop t_read until `buf.len()` bytes received OR EOF/error.
// Returns the count read on success (== buf.len()), 0 on clean EOF at
// frame boundary, negative on error or short read across EOF.
unsafe fn read_exact(fd: i64, buf: &mut [u8]) -> i64 {
    let mut got: usize = 0;
    while got < buf.len() {
        let n = t_read(fd, buf.as_mut_ptr().add(got), buf.len() - got);
        if n == 0 {
            // EOF. If we've read nothing, signal clean EOF; else short
            // read at frame boundary — protocol violation.
            return if got == 0 { 0 } else { -1 };
        }
        if n < 0 {
            return -1;
        }
        got += n as usize;
    }
    got as i64
}

// write_all — loop t_write until all bytes drained.
unsafe fn write_all(fd: i64, buf: &[u8]) -> i64 {
    let mut sent: usize = 0;
    while sent < buf.len() {
        let n = t_write(fd, buf.as_ptr().add(sent), buf.len() - sent);
        if n <= 0 {
            return -1;
        }
        sent += n as usize;
    }
    sent as i64
}

// send_response — encode + write a response frame. Returns 0 on
// success, -1 on transport error.
unsafe fn send_response(fd: i64, status: u8, payload: &[u8]) -> i64 {
    if payload.len() > 0xFFFF {
        return -1;
    }
    let mut frame = [0u8; MAX_RESPONSE_FRAME];
    if 3 + payload.len() > frame.len() {
        return -1;
    }
    frame[0] = status;
    let len_lo = (payload.len() & 0xFF) as u8;
    let len_hi = ((payload.len() >> 8) & 0xFF) as u8;
    frame[1] = len_lo;
    frame[2] = len_hi;
    for i in 0..payload.len() {
        frame[3 + i] = payload[i];
    }
    if write_all(fd, &frame[..3 + payload.len()]) < 0 {
        return -1;
    }
    0
}

// =============================================================================
// Verb handlers.
// =============================================================================

// handle_auth — parse AUTH payload, run Argon2id + AEGIS-256-unwrap,
// mint session token on success.
//
// Payload format:
//   [0]            user_len u8 (1..=MAX_USER_LEN)
//   [1..1+ul]      user
//   [1+ul..3+ul]   pass_len u16 LE (1..=MAX_PASS_LEN)
//   [3+ul..]       passphrase
//
// Wire crypto path (per CORVUS-DESIGN §5.1):
//   1. Parse + frame-validate.
//   2. Look up user's CorvusUserState (must exist from prior USER_CREATE).
//   3. Argon2id(passphrase, stored_salt, stored_params) → 32-byte KEK.
//   4. AEGIS-256-unwrap with stored nonce + AD; tag-mismatch ⇒ BadAuth.
//   5. (-c stub) Discard the unwrapped keypair (DEK unwrap lands at -d).
//   6. Mint session token, install session, return OK + token.
//
// Timing-attack mitigation: the not-found path takes a DIFFERENT amount
// of time than the unwrap-fail path (no Argon2id run vs Argon2id run).
// At v1.0 we accept the username-existence leak: a user-enumeration
// attacker who can talk to corvus can already enumerate via the audit
// log + the user-create surface. The Argon2id cost still bounds
// per-guess attempts on the passphrase. Constant-time across
// existing-vs-nonexisting would require running a dummy Argon2id on
// every not-found path, which doubles the rate-limiter's effectiveness
// only marginally — design judgment is to keep the simple form. Audit
// F-future may revisit.
unsafe fn handle_auth(payload: &[u8]) -> i64 {
    if payload.len() < 3 {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let user_len = payload[0] as usize;
    if user_len == 0 || user_len > MAX_USER_LEN {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    if payload.len() < 1 + user_len + 2 {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let user = &payload[1..1 + user_len];
    let pass_len = (payload[1 + user_len] as usize) | ((payload[2 + user_len] as usize) << 8);
    if pass_len == 0 || pass_len > MAX_PASS_LEN {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    if payload.len() != 1 + user_len + 2 + pass_len {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let passphrase = &payload[1 + user_len + 2..1 + user_len + 2 + pass_len];

    // Spec's one-session-per-Proc precondition (AuthSuccess's
    // `~(\E s : s.owner_proc = p)`). At this skeleton "the peer" is
    // implicit; an active session blocks AUTH.
    if session_active() {
        return send_response(TX_FD, STATUS_PERMISSION_DENIED, &[]);
    }

    // Look up user.
    let state = match user_states_find(user) {
        Some(s) => s,
        None => return send_response(TX_FD, STATUS_BAD_AUTH, &[]),
    };

    // Argon2id(passphrase, stored_salt, stored_params) → KEK.
    let mut kek = match argon2id_kek(
        passphrase,
        &state.salt,
        state.t_cost,
        state.m_cost_kib,
        state.parallelism,
    ) {
        Some(k) => k,
        None => return send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]),
    };

    // AEGIS-256-unwrap. Build AD from the stored record.
    let mut ad: Vec<u8> = Vec::new();
    state.build_ad(&mut ad);
    let unwrap_result = aegis_unwrap(&kek, &state.nonce, &ad, &state.ciphertext, &state.tag);

    // Wipe KEK regardless of outcome (CORVUS-DESIGN §5.1: "explicit_bzero
    // passphrase + KEK"). We rely on Rust's drop semantics for `ad` since
    // it doesn't carry secret material; KEK is the load-bearing wipe.
    for b in kek.iter_mut() {
        core::ptr::write_volatile(b as *mut u8, 0);
    }

    let mut keypair = match unwrap_result {
        Some(k) => k,
        None => return send_response(TX_FD, STATUS_BAD_AUTH, &[]),
    };

    // -c stub: we've proven the passphrase by AEAD-unwrapping the
    // keypair. The keypair itself is consumed at -d (UNWRAP verb's
    // DEK-decrypt path); at -c just wipe it after the auth succeeds.
    for b in keypair.iter_mut() {
        core::ptr::write_volatile(b as *mut u8, 0);
    }

    // Generate 16 bytes of entropy via CSPRNG → 32-char hex → prepend 's'.
    let mut entropy = [0u8; TOKEN_ENTROPY_BYTES];
    let rc = t_getrandom(entropy.as_mut_ptr(), TOKEN_ENTROPY_BYTES, 0);
    if rc != TOKEN_ENTROPY_BYTES as i64 {
        return send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]);
    }
    let mut token = [0u8; TOKEN_LEN];
    token[0] = b's';
    for i in 0..TOKEN_ENTROPY_BYTES {
        token[1 + 2 * i] = nibble_to_hex(entropy[i] >> 4);
        token[1 + 2 * i + 1] = nibble_to_hex(entropy[i]);
    }
    // Wipe the raw entropy buffer — the hex form lives in `token` and
    // the session table; the raw bytes have no further use.
    let _ = t_explicit_bzero(entropy.as_mut_ptr(), TOKEN_ENTROPY_BYTES);

    session_install(user, &token);

    // Per CORVUS-DESIGN.md §6.4: the AUTH success payload is the
    // session token (33 bytes).
    send_response(TX_FD, STATUS_OK, &token)
}

// handle_user_create — USER_CREATE verb (verb_id=5).
//
// Payload format (per CORVUS-DESIGN §6.4):
//   [0]            user_len u8 (1..=MAX_USER_LEN)
//   [1..1+ul]      user
//   [1+ul..3+ul]   pass_len u16 LE (1..=MAX_PASS_LEN)
//   [3+ul..]       passphrase
//   [end-1]        backend u8 (v1.0: 0=passphrase only)
//
// Crypto path:
//   1. Generate fresh salt (16 B) + nonce (32 B) via t_getrandom.
//   2. Argon2id(passphrase, salt, default-params) → KEK.
//   3. Generate placeholder 64-byte keypair via t_getrandom (ML-KEM-768
//      + X25519 hybrid lands at -d).
//   4. AEGIS-256-wrap(KEK, nonce, AD, keypair) → ciphertext + tag.
//   5. Construct CorvusUserState; insert into in-memory vec. Refuse if
//      user already exists.
//
// Spec note: USER_CREATE is outside the spec's session state-machine
// scope (specs/corvus.tla models sessions; user-state is a precondition
// for AuthSuccess being reachable). A future spec extension models the
// user-create → user-state ledger; bug shapes are "user-create
// elevates rights" (none here — corvus's caps don't change) and
// "user-create overwrites existing state" (refused via the user_states_insert
// existence check).
unsafe fn handle_user_create(payload: &[u8]) -> i64 {
    if payload.len() < 4 {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let user_len = payload[0] as usize;
    if user_len == 0 || user_len > MAX_USER_LEN {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    if payload.len() < 1 + user_len + 2 + 1 {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let user_slice = &payload[1..1 + user_len];
    let pass_len = (payload[1 + user_len] as usize) | ((payload[2 + user_len] as usize) << 8);
    if pass_len == 0 || pass_len > MAX_PASS_LEN {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    if payload.len() != 1 + user_len + 2 + pass_len + 1 {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let passphrase = &payload[1 + user_len + 2..1 + user_len + 2 + pass_len];
    let backend = payload[1 + user_len + 2 + pass_len];
    if backend != BACKEND_ID_PASSPHRASE {
        // v1.0 only supports passphrase backend; fido2/smartcard/escrow at v1.x.
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }

    // Pre-check existence (the insert path is the authoritative gate but
    // erroring early saves an Argon2id run on a doomed request).
    if user_states_find(user_slice).is_some() {
        return send_response(TX_FD, STATUS_PERMISSION_DENIED, &[]);
    }

    // Fresh salt + nonce.
    let mut salt = [0u8; ARGON2_SALT_LEN];
    if t_getrandom(salt.as_mut_ptr(), ARGON2_SALT_LEN, 0) != ARGON2_SALT_LEN as i64 {
        return send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]);
    }
    let mut nonce = [0u8; AEGIS256_NONCE_LEN];
    if t_getrandom(nonce.as_mut_ptr(), AEGIS256_NONCE_LEN, 0) != AEGIS256_NONCE_LEN as i64 {
        return send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]);
    }

    // Argon2id KEK.
    let mut kek = match argon2id_kek(
        passphrase,
        &salt,
        ARGON2_T_COST,
        ARGON2_M_COST_KIB,
        ARGON2_PARALLELISM,
    ) {
        Some(k) => k,
        None => return send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]),
    };

    // Generate the placeholder keypair (64 bytes CSPRNG at -c; ML-KEM
    // + X25519 hybrid at -d).
    let mut keypair_plain = [0u8; KEYPAIR_LEN];
    if t_getrandom(keypair_plain.as_mut_ptr(), KEYPAIR_LEN, 0) != KEYPAIR_LEN as i64 {
        return send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]);
    }

    // Build AD.
    let mut ad: Vec<u8> = Vec::new();
    let mut user_vec: Vec<u8> = Vec::new();
    user_vec.extend_from_slice(user_slice);
    ad.extend_from_slice(AD_PREFIX);
    ad.extend_from_slice(&user_vec);
    ad.push(BACKEND_ID_PASSPHRASE);

    // AEGIS-256-wrap.
    let mut ciphertext = [0u8; KEYPAIR_LEN];
    let tag = aegis_wrap(&kek, &nonce, &ad, &keypair_plain, &mut ciphertext);

    // Wipe sensitive transients.
    for b in kek.iter_mut() {
        core::ptr::write_volatile(b as *mut u8, 0);
    }
    for b in keypair_plain.iter_mut() {
        core::ptr::write_volatile(b as *mut u8, 0);
    }

    let record = CorvusUserState {
        user: user_vec,
        t_cost: ARGON2_T_COST,
        m_cost_kib: ARGON2_M_COST_KIB,
        parallelism: ARGON2_PARALLELISM,
        salt,
        nonce,
        ciphertext,
        tag,
    };

    if !user_states_insert(record) {
        return send_response(TX_FD, STATUS_PERMISSION_DENIED, &[]);
    }

    send_response(TX_FD, STATUS_OK, &[])
}

// handle_session_close — verify token + clear session.
unsafe fn handle_session_close(payload: &[u8]) -> i64 {
    if payload.len() != TOKEN_LEN {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    if !session_token_matches(payload) {
        return send_response(TX_FD, STATUS_NOT_FOUND, &[]);
    }
    session_clear();
    send_response(TX_FD, STATUS_OK, &[])
}

// =============================================================================
// Server loop.
// =============================================================================

unsafe fn server_loop() -> i64 {
    let mut header = [0u8; 3];
    let mut payload = [0u8; MAX_PAYLOAD_LEN];
    loop {
        let n = read_exact(RX_FD, &mut header);
        if n == 0 {
            // Clean EOF — peer closed write side.
            return 0;
        }
        if n < 0 {
            return -1;
        }
        let verb_id = header[0];
        let payload_len = (header[1] as usize) | ((header[2] as usize) << 8);
        if payload_len > MAX_PAYLOAD_LEN {
            // Frame too large; emit BAD_FORMAT response, terminate (the
            // protocol's framed; we can't safely drain a too-large
            // payload without knowing it's bytes count, which we already
            // refused).
            let _ = send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
            return -1;
        }
        if payload_len > 0 {
            let n = read_exact(RX_FD, &mut payload[..payload_len]);
            if n != payload_len as i64 {
                return -1;
            }
        }
        let rc = match verb_id {
            VERB_AUTH => handle_auth(&payload[..payload_len]),
            VERB_SESSION_CLOSE => handle_session_close(&payload[..payload_len]),
            VERB_USER_CREATE => handle_user_create(&payload[..payload_len]),
            _ => send_response(TX_FD, STATUS_BAD_FORMAT, &[]),
        };
        if rc < 0 {
            return -1;
        }
        // Wipe payload before next iteration (defence-in-depth: future
        // verbs may carry secrets like passphrases or wrapped DEKs).
        let _ = t_explicit_bzero(payload.as_mut_ptr(), payload_len);
    }
}

// =============================================================================
// Startup hardening (unchanged from -a).
// =============================================================================

const PROBE_LEN: usize = 32;

#[cold]
#[inline(never)]
fn step_fail(step: u8, rc: i64) -> ! {
    t_putstr("corvus: STEP=");
    let digit = b'0' + step;
    let buf = [digit, 0];
    let _ = t_putstr(unsafe { core::str::from_utf8_unchecked(&buf[..1]) });
    t_putstr(" FAIL rc=");
    let nibble = (rc as u8) & 0x0f;
    let hex_char = if nibble < 10 {
        b'0' + nibble
    } else {
        b'a' + (nibble - 10)
    };
    let hex_buf = [hex_char, 0];
    let _ = t_putstr(unsafe { core::str::from_utf8_unchecked(&hex_buf[..1]) });
    t_putstr("\n");
    unsafe { libthyla_rs::t_exits(1) }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("corvus: starting (P5-corvus-bringup-c)\n");

    // Heap + state-vec FIRST. Every allocation downstream (argon2 working
    // memory, AD buffers, the user-state Vec) depends on the allocator
    // being live. heap_init is idempotent in practice (called once per
    // Proc) but defensive against any future restart path.
    unsafe {
        heap_init();
        user_states_init();
    }

    let rc = unsafe { t_mlockall(0) };
    if rc != 0 {
        step_fail(1, rc);
    }
    let rc = unsafe { t_set_dumpable(0) };
    if rc != 0 {
        step_fail(2, rc);
    }
    let rc = unsafe { t_set_traceable(0) };
    if rc != 0 {
        step_fail(3, rc);
    }
    let mut probe: [u8; PROBE_LEN] = [0; PROBE_LEN];
    let rc = unsafe { t_getrandom(probe.as_mut_ptr(), PROBE_LEN, 0) };
    if rc != PROBE_LEN as i64 {
        step_fail(4, rc);
    }
    let rc = unsafe { t_explicit_bzero(probe.as_mut_ptr(), PROBE_LEN) };
    if rc != 0 {
        step_fail(5, rc);
    }

    t_putstr("corvus: ready (hardening applied; serving /srv/corvus/ over fd 0/1)\n");

    // Enter server loop. Exits 0 on clean EOF; non-zero on any wire
    // error. The boot test framework reaps corvus's exit via joey's
    // wait_pid and surfaces non-zero.
    let rc = unsafe { server_loop() };
    if rc < 0 {
        t_putstr("corvus: server_loop FAILED\n");
        return 1;
    }

    // Wipe any residual session state before exiting. The
    // explicit_bzero discipline carries forward to every shutdown path.
    unsafe { session_clear() };

    // Close our pipe fds explicitly. The kernel will release them on
    // exit anyway, but the explicit close exercises the cleanup path.
    let _ = unsafe { t_close(RX_FD) };
    let _ = unsafe { t_close(TX_FD) };

    t_putstr("corvus: server_loop returned EOF; shutting down clean\n");
    0
}
