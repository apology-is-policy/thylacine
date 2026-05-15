// /sbin/corvus — Thylacine key-agent daemon.
//
// Sub-chunks (per CORVUS-DESIGN.md §10):
//   P5-corvus-bringup-a (7487054): startup hardening + readiness banner.
//   P5-corvus-bringup-b (9cf92c6): /srv/corvus/ Spoor server skeleton +
//                                  binary frame wire codec + AUTH +
//                                  SESSION_CLOSE verbs.
//   P5-corvus-bringup-c (24ddb91): Argon2id passphrase verification +
//                                  AEGIS-256 AEAD wrap/unwrap + the
//                                  state file format (magic CRVS) +
//                                  USER_CREATE. Keypair a placeholder.
//   P5-corvus-bringup-d (this chunk): the real ML-KEM-768 + X25519
//                                  hybrid keypair (replaces the -c
//                                  64-byte placeholder); the hybrid-PKE
//                                  DEK envelope; the WRAP (verb_id=10)
//                                  and UNWRAP (verb_id=4) verbs + the
//                                  in-memory dataset-ownership table
//                                  that enforces invariant C-7.
//
// At this sub-chunk corvus is a single-peer userspace daemon that:
//
//   1. Runs the hardening sequence at startup (mlockall + set_dumpable(0)
//      + set_traceable(0) + a CSPRNG-seeded probe).
//
//   2. Initializes the static-buffer heap (24 MiB), the user-state vec,
//      and the dataset-ownership table, then serves binary frames on
//      fd 0 (rx) / fd 1 (tx) — the pipe pair joey installs via
//      SYS_SPAWN_FULL.
//
//   3. Dispatches verb_id ∈ { AUTH, SESSION_CLOSE, UNWRAP, USER_CREATE,
//      WRAP } per CORVUS-DESIGN.md §6.4. Every other verb_id is refused
//      with status=BadFormat.
//
//   4. USER_CREATE (verb_id=5): generates a fresh ML-KEM-768 + X25519
//      hybrid keypair, AEGIS-256-wraps it under an Argon2id-derived KEK
//      into a CorvusUserState (magic CRVS), and registers the user's
//      dataset (users/<name>) in the ownership table.
//
//   5. AUTH (verb_id=1): re-derives the KEK via Argon2id, AEGIS-256-
//      unwraps the hybrid keypair, and RETAINS it in the mlock'd session
//      slab (corvus-c discarded it). The retained keypair is what WRAP
//      and UNWRAP consume.
//
//   6. WRAP (verb_id=10): wraps a 32-byte DEK under the session user's
//      hybrid PUBLIC key into a DEK envelope. C-7-gated.
//
//   7. UNWRAP (verb_id=4): C-7-gated. Refuses (PermissionDenied) a
//      (session, dataset) pair where session.user != owner_of(dataset)
//      BEFORE any crypto; on an owned dataset, unwraps the DEK envelope
//      with the session user's hybrid SECRET key and returns the DEK.
//
//   8. SESSION_CLOSE (verb_id=3): token-bound session clear (wipes the
//      keypair slab + token).
//
//   9. Exits 0 on rx EOF; non-zero on transport error.
//
// State storage at this sub-chunk: in-memory only. CorvusUserState
// records + the dataset-ownership table do not survive a corvus
// restart; FS persistence (loading /var/lib/corvus/) lands when that
// tree is mounted (P5+).
//
// Spec correspondence (specs/corvus.tla; P5-corvus-spec at c00de63):
//
//   AuthSuccess(p, u)     — handle_auth() insert path.
//   SessionClose(p)       — handle_session_close() clear path.
//   Unwrap(p, d)          — handle_unwrap(): the C-7 gate
//                           (session.bound_user == owner_of(dataset)) is
//                           UnwrapOwnerOnly; the cross-user refusal is
//                           the BuggyUnwrapCrossUser negative.
//   SessionUserImmutable  — Session::{user,keypair} set once at
//                           session_install(); cleared whole-record at
//                           session_clear(); no in-place setter.
//   WRAP / USER_CREATE    — outside the spec's session state-machine
//                           scope. WRAP shares Unwrap's C-7 gate;
//                           USER_CREATE is the precondition that makes
//                           a user's AuthSuccess reachable.
//   AdminElevate / AdminVerb — NOT YET (deferred).

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
// State file layout (in-memory at this sub-chunk; FS persistence at P5+):
//
//   [0..4)       magic 'CRVS'        (u32 LE = 0x53565243)
//   [4..8)       version             u32 LE = 1
//   [8..12)      t_cost              u32 LE
//   [12..20)     m_cost_kib          u64 LE
//   [20..24)     parallelism         u32 LE
//   [24..40)     argon2id_salt       16 B
//   [40..72)     aead_nonce          32 B  (AEGIS-256 nonce width)
//   [72..ct_end) ciphertext          KEYPAIR_LEN bytes (= hybrid keypair wrapped)
//   [ct_end..)   AEAD_tag            32 B  (AEGIS-256 tag width)
//
// The CRVS version stays 1 across P5-corvus-bringup-d: the layout is
// unchanged — only KEYPAIR_LEN grew (64-byte placeholder → 3648-byte
// real hybrid keypair), and the ciphertext length is implied by the
// file size, not a stored field. No CRVS file is persisted yet, so the
// keypair-size change breaks no on-disk format.

const CORVUS_MAGIC: u32 = 0x53565243; // 'CRVS' LE
const CORVUS_STATE_VERSION: u32 = 1;
const ARGON2_SALT_LEN: usize = 16;
const AEGIS256_KEY_LEN: usize = 32;
const AEGIS256_NONCE_LEN: usize = 32;
const AEGIS256_TAG_LEN: usize = 32;

// Hybrid keypair (P5-corvus-bringup-d). The AEGIS-wrapped plaintext is
// the concatenation of the X25519 and ML-KEM-768 key halves:
//
//   [0..32)      x25519 secret key      32 B
//   [32..64)     x25519 public key      32 B
//   [64..1248)   ML-KEM-768 encapsulation (public) key   1184 B
//   [1248..3648) ML-KEM-768 decapsulation (secret) key   2400 B
//
// Both public halves are stored so corvus can WRAP (needs the public
// keys) without re-deriving them; the layout is fixed-offset so the
// session slab can be sliced directly.
const X25519_KEY_LEN: usize = 32;
const MLKEM_EK_LEN: usize = 1184; // ML-KEM-768 encapsulation key
const MLKEM_DK_LEN: usize = 2400; // ML-KEM-768 decapsulation key
const MLKEM_CT_LEN: usize = 1088; // ML-KEM-768 ciphertext
const KP_X25519_SK_OFF: usize = 0;
const KP_X25519_PK_OFF: usize = X25519_KEY_LEN;
const KP_MLKEM_EK_OFF: usize = X25519_KEY_LEN * 2;
const KP_MLKEM_DK_OFF: usize = X25519_KEY_LEN * 2 + MLKEM_EK_LEN;
const KEYPAIR_LEN: usize = X25519_KEY_LEN * 2 + MLKEM_EK_LEN + MLKEM_DK_LEN; // 3648

const HEADER_LEN: usize = 4 + 4 + 4 + 8 + 4 + ARGON2_SALT_LEN + AEGIS256_NONCE_LEN; // = 72
const TOTAL_LEN: usize = HEADER_LEN + KEYPAIR_LEN + AEGIS256_TAG_LEN; // = 3752 at -d

const _: () = assert!(HEADER_LEN == 72, "state file header layout drift");
const _: () = assert!(KEYPAIR_LEN == 3648, "hybrid keypair layout drift");
const _: () = assert!(KP_MLKEM_DK_OFF == 1248, "keypair sub-offset drift");
const _: () = assert!(AEGIS256_KEY_LEN == 32, "AEGIS-256 key width is 32");
const _: () = assert!(AEGIS256_NONCE_LEN == 32, "AEGIS-256 nonce width is 32");
const _: () = assert!(AEGIS256_TAG_LEN == 32, "AEGIS-256 tag width is 32");

// Argon2id v1.0 preset. Design default (CORVUS-DESIGN §4.3): t_cost=2,
// m_cost=64 MiB, parallelism=1.
//
// Bringup uses m_cost=16 MiB (KiB units) — a quarter of the design
// default. The cap is the kernel allocator: corvus's heap is a static
// BSS buffer mapped by a single contiguous physical allocation
// (kernel/burrow.c::burrow_create_anon rounds page_count up to one
// buddy order; a 64 MiB heap needs an order-14 = 64 MiB contiguous
// block, which strains the buddy free-list after the kernel test
// suite has fragmented memory). 24 MiB heap → order-13 (32 MiB
// contiguous) is comfortably allocatable; 16 MiB m_cost fits with
// 8 MiB headroom for Vec bookkeeping.
//
// The state-file format carries m_cost_kib as a per-record stored
// field, so a future chunk can raise the default — new records use the
// higher cost, old records still verify at their stored cost, no
// format break.
const ARGON2_T_COST: u32 = 2;
const ARGON2_M_COST_KIB: u32 = 16 * 1024;
const ARGON2_PARALLELISM: u32 = 1;

// Additional Data for the keypair-wrap AEAD: "thylacine-corvus-v1" ||
// user_name || backend_id. Backend ID at v1.0 is `passphrase` = 0. Per
// CORVUS-DESIGN §4.3 binding rationale, AD prevents cross-user /
// cross-backend wrap substitution.
const AD_PREFIX: &[u8] = b"thylacine-corvus-v1";
const BACKEND_ID_PASSPHRASE: u8 = 0;

#[derive(Clone)]
struct CorvusUserState {
    // Stable user identity (the AUTH key).
    user: Vec<u8>,
    // Argon2id params. Per-user (a user with sensitive preset will have
    // different t_cost/m_cost_kib; bringup uses the interactive preset
    // for every user but the field is per-record by design).
    t_cost: u32,
    m_cost_kib: u32,
    parallelism: u32,
    // Fresh CSPRNG per state record. Re-randomized on every passphrase
    // rotation per CORVUS-DESIGN §4.3 / audit F7.
    salt: [u8; ARGON2_SALT_LEN],
    nonce: [u8; AEGIS256_NONCE_LEN],
    // AEGIS-256(KEK, nonce, AD).encrypt(hybrid keypair) → (ciphertext, tag).
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

    // Serialize to the wire-format byte vec (for FS persistence at P5+;
    // included here so the layout has one authoritative serializer).
    // Output is exactly TOTAL_LEN bytes.
    #[allow(dead_code)] // FS persistence consumes this at P5+
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
// DEK envelope — the hybrid-PKE wrapped-DEK blob (P5-corvus-bringup-d).
// =============================================================================
//
// CORVUS-DESIGN.md §6.4 WRAP/UNWRAP carry a `wrapped` blob: a 32-byte
// dataset DEK encrypted to a user's hybrid keypair. The envelope is a
// KEM-DEM hybrid:
//
//   KEM: ML-KEM-768 encapsulation  +  X25519 ECDH (ephemeral → static).
//        The two shared secrets + the ciphertext transcript feed a
//        SHA-256 KEK combiner — the result stays secure if EITHER
//        primitive holds (post-quantum AND classical).
//   DEM: AEGIS-256 over the 32-byte DEK, AD-bound to the dataset name
//        so a wrapped DEK cannot be replayed against another dataset.
//
//   [0]            envelope_version  u8 = 1
//   [1..1089)      mlkem_ct          1088 B
//   [1089..1121)   x25519_eph_pk     32 B
//   [1121..1153)   aead_nonce        32 B
//   [1153..1185)   dek_ct            32 B
//   [1185..1217)   dek_tag           32 B
const DEK_LEN: usize = 32;
const ENVELOPE_VERSION: u8 = 1;
const ENV_MLKEM_CT_OFF: usize = 1;
const ENV_EPH_PK_OFF: usize = ENV_MLKEM_CT_OFF + MLKEM_CT_LEN; // 1089
const ENV_NONCE_OFF: usize = ENV_EPH_PK_OFF + X25519_KEY_LEN; // 1121
const ENV_DEK_CT_OFF: usize = ENV_NONCE_OFF + AEGIS256_NONCE_LEN; // 1153
const ENV_DEK_TAG_OFF: usize = ENV_DEK_CT_OFF + DEK_LEN; // 1185
const ENVELOPE_LEN: usize = ENV_DEK_TAG_OFF + AEGIS256_TAG_LEN; // 1217

const _: () = assert!(ENVELOPE_LEN == 1217, "DEK envelope layout drift");

// Domain separators. The KDF prefix binds the SHA-256 combiner to this
// construction + version; the AEAD AD prefix binds the DEM layer.
const DEK_KDF_DOMAIN: &[u8] = b"thylacine-corvus-dek-kdf-v1";
const DEK_AD_PREFIX: &[u8] = b"thylacine-corvus-dek-v1";

// =============================================================================
// Crypto: Argon2id KDF + AEGIS-256 AEAD + ML-KEM-768 + X25519 + SHA-256.
// =============================================================================

use aegis::aegis256::Aegis256;
use argon2::{Algorithm, Argon2, Params, Version};
use kem::{Decapsulate, Encapsulate};
use ml_kem::{Ciphertext, Encoded, EncodedSizeUser, KemCore, MlKem768};
use rand_core::RngCore;
use sha2::{Digest, Sha256};
use x25519_dalek::{EphemeralSecret, PublicKey, StaticSecret};

type MlKemEk = <MlKem768 as KemCore>::EncapsulationKey;
type MlKemDk = <MlKem768 as KemCore>::DecapsulationKey;

// rand_core adapter over the kernel CSPRNG. ml-kem + x25519-dalek both
// take an explicit RNG; corvus feeds them t_getrandom. corvus verified
// the CSPRNG seeded at startup, so a post-startup t_getrandom failure
// is an invariant violation — fatal (corvus cannot mint sound keys).
struct ThylaRng;

impl rand_core::RngCore for ThylaRng {
    fn next_u32(&mut self) -> u32 {
        let mut b = [0u8; 4];
        self.fill_bytes(&mut b);
        u32::from_le_bytes(b)
    }
    fn next_u64(&mut self) -> u64 {
        let mut b = [0u8; 8];
        self.fill_bytes(&mut b);
        u64::from_le_bytes(b)
    }
    fn fill_bytes(&mut self, dest: &mut [u8]) {
        // Loop: SYS_GETRANDOM clamps a request above the kernel's
        // per-call cap and returns the short (clamped) count, so a
        // single >cap request must not be treated as failure. Advance
        // by whatever was filled until the buffer is complete; only a
        // non-positive return is a genuine (fatal) CSPRNG failure.
        let mut off: usize = 0;
        while off < dest.len() {
            let rc = unsafe { t_getrandom(dest.as_mut_ptr().add(off), dest.len() - off, 0) };
            if rc <= 0 {
                t_putstr("corvus: t_getrandom failed mid-operation\n");
                unsafe { libthyla_rs::t_exits(1) }
            }
            off += rc as usize;
        }
    }
    fn try_fill_bytes(&mut self, dest: &mut [u8]) -> Result<(), rand_core::Error> {
        // fill_bytes is fatal-on-failure, so this only ever returns Ok.
        self.fill_bytes(dest);
        Ok(())
    }
}

impl rand_core::CryptoRng for ThylaRng {}

// Volatile-wipe a byte slice. The optimizer cannot elide volatile
// writes — used for secret transients the crates don't zeroize.
fn wipe(buf: &mut [u8]) {
    for b in buf.iter_mut() {
        unsafe { core::ptr::write_volatile(b as *mut u8, 0) };
    }
}

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

// SHA-256 KEK combiner for the DEK envelope. Hashes the domain prefix +
// both KEM shared secrets + the ciphertext transcript into a 32-byte
// AEGIS-256 key. Binding the transcript (eph_pk ‖ mlkem_ct) ties the
// KEK to the exact ciphertext, so a substituted ciphertext yields a
// different KEK and the AEAD tag check then fails.
fn sha256_kek(parts: &[&[u8]]) -> [u8; AEGIS256_KEY_LEN] {
    let mut h = Sha256::new();
    for p in parts {
        h.update(p);
    }
    let digest = h.finalize();
    let mut kek = [0u8; AEGIS256_KEY_LEN];
    kek.copy_from_slice(&digest);
    kek
}

// Generate a fresh ML-KEM-768 + X25519 hybrid keypair. Returns the
// 3648-byte plaintext layout (see KP_* offsets). None only on a crate
// size-contract mismatch — a build-time-impossible drift guard.
fn generate_hybrid_keypair() -> Option<[u8; KEYPAIR_LEN]> {
    let mut rng = ThylaRng;

    // X25519 — classical half. StaticSecret zeroizes on drop.
    let x_sk = StaticSecret::random_from_rng(&mut rng);
    let x_pk = PublicKey::from(&x_sk);

    // ML-KEM-768 — post-quantum half.
    let (mlkem_dk, mlkem_ek) = MlKem768::generate(&mut rng);
    let mut ek_bytes = mlkem_ek.as_bytes();
    let mut dk_bytes = mlkem_dk.as_bytes();
    if ek_bytes.len() != MLKEM_EK_LEN || dk_bytes.len() != MLKEM_DK_LEN {
        return None;
    }

    let mut kp = [0u8; KEYPAIR_LEN];
    kp[KP_X25519_SK_OFF..KP_X25519_SK_OFF + X25519_KEY_LEN].copy_from_slice(x_sk.as_bytes());
    kp[KP_X25519_PK_OFF..KP_X25519_PK_OFF + X25519_KEY_LEN].copy_from_slice(x_pk.as_bytes());
    kp[KP_MLKEM_EK_OFF..KP_MLKEM_EK_OFF + MLKEM_EK_LEN].copy_from_slice(&ek_bytes[..]);
    kp[KP_MLKEM_DK_OFF..KP_MLKEM_DK_OFF + MLKEM_DK_LEN].copy_from_slice(&dk_bytes[..]);

    // x_sk and the typed ml-kem keys (mlkem_dk / mlkem_ek) zeroize on
    // drop — the x25519-dalek AND ml-kem `zeroize` features are both
    // enabled. The SERIALIZED `dk_bytes` array (from `as_bytes()`) is a
    // plain hybrid_array::Array with no Drop wipe — scrub it explicitly.
    // (ek is public; scrubbing ek_bytes is hygiene, not a requirement.)
    wipe(&mut dk_bytes[..]);
    wipe(&mut ek_bytes[..]);
    Some(kp)
}

// WRAP: encrypt `dek` (32 B) to the hybrid PUBLIC key. Produces the
// 1217-byte DEK envelope. `ad_dataset` binds the DEM layer to the
// dataset name. None on a crate size-contract mismatch.
fn dek_envelope_wrap(
    x25519_pk: &[u8; X25519_KEY_LEN],
    mlkem_ek_bytes: &[u8],
    dek: &[u8; DEK_LEN],
    ad_dataset: &[u8],
    key_id: u64,
) -> Option<[u8; ENVELOPE_LEN]> {
    let mut rng = ThylaRng;

    // ML-KEM encapsulate to the recipient's encapsulation key.
    let enc = Encoded::<MlKemEk>::try_from(mlkem_ek_bytes).ok()?;
    let mlkem_ek = MlKemEk::from_bytes(&enc);
    let (mlkem_ct, mut ss_pq) = mlkem_ek.encapsulate(&mut rng).ok()?;
    if mlkem_ct.len() != MLKEM_CT_LEN || ss_pq.len() != AEGIS256_KEY_LEN {
        wipe(&mut ss_pq[..]);
        return None;
    }

    // X25519 ECDH: fresh ephemeral → recipient static.
    let eph = EphemeralSecret::random_from_rng(&mut rng);
    let eph_pk = PublicKey::from(&eph);
    let their_pk = PublicKey::from(*x25519_pk);
    let ss_cl = eph.diffie_hellman(&their_pk);

    // KEK = SHA-256(domain ‖ ss_pq ‖ ss_cl ‖ eph_pk ‖ mlkem_ct).
    let mut kek = sha256_kek(&[
        DEK_KDF_DOMAIN,
        &ss_pq[..],
        &ss_cl.as_bytes()[..],
        &eph_pk.as_bytes()[..],
        &mlkem_ct[..],
    ]);
    // ss_cl zeroizes on drop (zeroize feature); ss_pq does not.
    wipe(&mut ss_pq[..]);

    // DEM: AEGIS-256 over the DEK, AD = prefix ‖ dataset.
    let mut nonce = [0u8; AEGIS256_NONCE_LEN];
    rng.fill_bytes(&mut nonce);
    // AD binds the DEM layer to the dataset AND the key_id, so a wrapped
    // DEK cannot be replayed against a different dataset or a different
    // key generation (rotation safety).
    let mut ad: Vec<u8> = Vec::new();
    ad.extend_from_slice(DEK_AD_PREFIX);
    ad.extend_from_slice(ad_dataset);
    ad.extend_from_slice(&key_id.to_le_bytes());
    let mut dek_ct = [0u8; DEK_LEN];
    let dek_tag = aegis_wrap(&kek, &nonce, &ad, dek, &mut dek_ct);
    wipe(&mut kek);

    // Assemble the envelope.
    let mut env = [0u8; ENVELOPE_LEN];
    env[0] = ENVELOPE_VERSION;
    env[ENV_MLKEM_CT_OFF..ENV_MLKEM_CT_OFF + MLKEM_CT_LEN].copy_from_slice(&mlkem_ct[..]);
    env[ENV_EPH_PK_OFF..ENV_EPH_PK_OFF + X25519_KEY_LEN].copy_from_slice(eph_pk.as_bytes());
    env[ENV_NONCE_OFF..ENV_NONCE_OFF + AEGIS256_NONCE_LEN].copy_from_slice(&nonce);
    env[ENV_DEK_CT_OFF..ENV_DEK_CT_OFF + DEK_LEN].copy_from_slice(&dek_ct);
    env[ENV_DEK_TAG_OFF..ENV_DEK_TAG_OFF + AEGIS256_TAG_LEN].copy_from_slice(&dek_tag);
    Some(env)
}

// UNWRAP: recover the 32-byte DEK from a DEK envelope using the hybrid
// SECRET key. None on a malformed envelope OR an AEAD tag failure
// (wrong keypair / corrupted blob). `ad_dataset` must match the wrap.
fn dek_envelope_unwrap(
    x25519_sk: &[u8; X25519_KEY_LEN],
    mlkem_dk_bytes: &[u8],
    envelope: &[u8],
    ad_dataset: &[u8],
    key_id: u64,
) -> Option<[u8; DEK_LEN]> {
    if envelope.len() != ENVELOPE_LEN || envelope[0] != ENVELOPE_VERSION {
        return None;
    }
    let mlkem_ct_bytes = &envelope[ENV_MLKEM_CT_OFF..ENV_MLKEM_CT_OFF + MLKEM_CT_LEN];
    let mut eph_pk = [0u8; X25519_KEY_LEN];
    eph_pk.copy_from_slice(&envelope[ENV_EPH_PK_OFF..ENV_EPH_PK_OFF + X25519_KEY_LEN]);
    let mut nonce = [0u8; AEGIS256_NONCE_LEN];
    nonce.copy_from_slice(&envelope[ENV_NONCE_OFF..ENV_NONCE_OFF + AEGIS256_NONCE_LEN]);
    let dek_ct = &envelope[ENV_DEK_CT_OFF..ENV_DEK_CT_OFF + DEK_LEN];
    let mut dek_tag = [0u8; AEGIS256_TAG_LEN];
    dek_tag.copy_from_slice(&envelope[ENV_DEK_TAG_OFF..ENV_DEK_TAG_OFF + AEGIS256_TAG_LEN]);

    // ML-KEM decapsulate. FIPS 203 implicit rejection: a bad ciphertext
    // yields a deterministic-but-wrong shared secret, NOT an error — the
    // AEGIS-256 tag check below is the integrity gate.
    let ct = Ciphertext::<MlKem768>::try_from(mlkem_ct_bytes).ok()?;
    let enc = Encoded::<MlKemDk>::try_from(mlkem_dk_bytes).ok()?;
    let mlkem_dk = MlKemDk::from_bytes(&enc);
    let mut ss_pq = mlkem_dk.decapsulate(&ct).ok()?;
    if ss_pq.len() != AEGIS256_KEY_LEN {
        wipe(&mut ss_pq[..]);
        return None;
    }

    // X25519 ECDH: static secret → envelope ephemeral public.
    let sk = StaticSecret::from(*x25519_sk);
    let eph_pub = PublicKey::from(eph_pk);
    let ss_cl = sk.diffie_hellman(&eph_pub);

    let mut kek = sha256_kek(&[
        DEK_KDF_DOMAIN,
        &ss_pq[..],
        &ss_cl.as_bytes()[..],
        &eph_pk[..],
        mlkem_ct_bytes,
    ]);
    wipe(&mut ss_pq[..]);

    // AD must match the wrap exactly: prefix ‖ dataset ‖ key_id.
    let mut ad: Vec<u8> = Vec::new();
    ad.extend_from_slice(DEK_AD_PREFIX);
    ad.extend_from_slice(ad_dataset);
    ad.extend_from_slice(&key_id.to_le_bytes());
    let plain = aegis_unwrap(&kek, &nonce, &ad, dek_ct, &dek_tag);
    wipe(&mut kek);

    let mut plain = plain?;
    if plain.len() != DEK_LEN {
        wipe(&mut plain);
        return None;
    }
    let mut dek = [0u8; DEK_LEN];
    dek.copy_from_slice(&plain);
    wipe(&mut plain);
    Some(dek)
}

// =============================================================================
// User state vector — in-memory at this sub-chunk.
// =============================================================================
//
// `static mut Vec<CorvusUserState>` (single-threaded; corvus is one Proc
// at v1.0). All access goes through helper functions that take the
// addr_of_mut! pointer — same discipline as SESSION.
//
// MAX_USERS bounds the table so a hostile peer cannot OOM the daemon by
// spamming USER_CREATE (the heap is 24 MiB; each record is ~3.7 KiB).
// Real rate-limiting (C-16) + the CapHostowner gate are later sub-chunks.

const MAX_USERS: usize = 256;

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

unsafe fn user_states_count() -> usize {
    match (*core::ptr::addr_of!(USER_STATES)).as_ref() {
        Some(s) => s.len(),
        None => 0,
    }
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
// Dataset-ownership table — in-memory at -d (CORVUS-DESIGN §4.4 / §5.4).
// =============================================================================
//
// Maps a dataset name → its owning user. USER_CREATE registers
// `users/<name>` → `<name>`. WRAP and UNWRAP consult it for the C-7
// gate: corvus refuses a (session, dataset) pair where the session's
// bound user is not the dataset's owner. FS persistence (loading from
// /var/lib/corvus/datasets/) lands when that tree is mounted.

struct DatasetOwner {
    dataset: Vec<u8>,
    owner: Vec<u8>,
}

static mut DATASET_OWNERS: Option<Vec<DatasetOwner>> = None;

unsafe fn dataset_owners_init() {
    core::ptr::write(core::ptr::addr_of_mut!(DATASET_OWNERS), Some(Vec::new()));
}

// Look up a dataset's owner. Returns a copy of the owner name, or None
// if the dataset is unknown.
unsafe fn dataset_owner_find(dataset: &[u8]) -> Option<Vec<u8>> {
    let table = (*core::ptr::addr_of!(DATASET_OWNERS)).as_ref()?;
    for entry in table {
        if entry.dataset == dataset {
            let mut owner = Vec::new();
            owner.extend_from_slice(&entry.owner);
            return Some(owner);
        }
    }
    None
}

// Register a dataset→owner mapping. Returns false (no-op) if the dataset
// is already registered.
unsafe fn dataset_owner_register(dataset: &[u8], owner: &[u8]) -> bool {
    let table = match (*core::ptr::addr_of_mut!(DATASET_OWNERS)).as_mut() {
        Some(t) => t,
        None => return false,
    };
    for entry in table.iter() {
        if entry.dataset == dataset {
            return false;
        }
    }
    let mut d = Vec::new();
    d.extend_from_slice(dataset);
    let mut o = Vec::new();
    o.extend_from_slice(owner);
    table.push(DatasetOwner { dataset: d, owner: o });
    true
}

// =============================================================================
// Wire constants — CORVUS-DESIGN.md §6.4.
// =============================================================================

const VERB_AUTH: u8 = 1;
const VERB_SESSION_CLOSE: u8 = 3;
const VERB_UNWRAP: u8 = 4;
const VERB_USER_CREATE: u8 = 5;
const VERB_WRAP: u8 = 10;

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

// Dataset names: "users/michael" etc. 64 is generous headroom.
const MAX_DATASET_LEN: usize = 64;
// The wrapped-DEK blob. ENVELOPE_LEN (1217) is the v1.0 size; the cap
// has headroom for envelope-format evolution.
const MAX_WRAPPED_LEN: usize = 2048;

// UNWRAP carries the largest inbound payload:
//   token(33) + dataset_len(1) + dataset(≤64) + key_id(8)
//   + wrapped_len(2) + wrapped(≤2048).
const MAX_PAYLOAD_LEN: usize = TOKEN_LEN + 1 + MAX_DATASET_LEN + 8 + 2 + MAX_WRAPPED_LEN;
const _: () = assert!(
    MAX_PAYLOAD_LEN >= 1 + MAX_USER_LEN + 2 + MAX_PASS_LEN + 1,
    "USER_CREATE payload must fit MAX_PAYLOAD_LEN"
);

// WRAP returns the DEK envelope — the largest response payload.
const MAX_RESPONSE_FRAME: usize = 3 + ENVELOPE_LEN;

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
// The session carries the AEGIS-unwrapped hybrid keypair (P5-corvus-
// bringup-d): AUTH installs it, WRAP / UNWRAP read it, session_clear
// wipes it. The keypair slab is the load-bearing secret in corvus's
// mlock'd RAM.

#[repr(C)]
struct Session {
    active: bool,
    user_len: u8,
    user: [u8; MAX_USER_LEN],
    token: [u8; TOKEN_LEN],
    // The session user's hybrid keypair, AEGIS-unwrapped at AUTH and
    // held here for WRAP / UNWRAP. Wiped (volatile) at session_clear().
    keypair: [u8; KEYPAIR_LEN],
}

static mut SESSION: Session = Session {
    active: false,
    user_len: 0,
    user: [0; MAX_USER_LEN],
    token: [0; TOKEN_LEN],
    keypair: [0; KEYPAIR_LEN],
};

// Accessor wrappers go through raw pointers + element-by-element writes
// so we don't take a &mut reference to the static (Rust 1.77+'s
// static_mut_refs lint fires on `&mut SESSION` patterns).

unsafe fn session_active() -> bool {
    core::ptr::read(core::ptr::addr_of!(SESSION.active))
}

unsafe fn session_install(user: &[u8], token: &[u8; TOKEN_LEN], keypair: &[u8; KEYPAIR_LEN]) {
    let s = core::ptr::addr_of_mut!(SESSION);
    let user_ptr = core::ptr::addr_of_mut!((*s).user) as *mut u8;
    let token_ptr = core::ptr::addr_of_mut!((*s).token) as *mut u8;
    let kp_ptr = core::ptr::addr_of_mut!((*s).keypair) as *mut u8;
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
    // Install keypair
    core::ptr::copy_nonoverlapping(keypair.as_ptr(), kp_ptr, KEYPAIR_LEN);
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
    // Constant-time compare: never short-circuit on mismatch.
    let mut diff: u8 = 0;
    for i in 0..TOKEN_LEN {
        let tok_byte = core::ptr::read(token_ptr.add(i));
        diff |= tok_byte ^ candidate[i];
    }
    diff == 0
}

// Copy the session's bound user name. None if no active session.
unsafe fn session_user_copy() -> Option<Vec<u8>> {
    if !session_active() {
        return None;
    }
    let s = core::ptr::addr_of!(SESSION);
    let len = core::ptr::read(core::ptr::addr_of!((*s).user_len)) as usize;
    if len == 0 || len > MAX_USER_LEN {
        return None;
    }
    let user_ptr = core::ptr::addr_of!((*s).user) as *const u8;
    let mut out = Vec::with_capacity(len);
    for i in 0..len {
        out.push(core::ptr::read(user_ptr.add(i)));
    }
    Some(out)
}

// Copy the session's hybrid keypair into a caller-owned buffer. None if
// no active session. The caller MUST wipe the copy after use.
unsafe fn session_keypair_copy() -> Option<[u8; KEYPAIR_LEN]> {
    if !session_active() {
        return None;
    }
    let s = core::ptr::addr_of!(SESSION);
    let kp_ptr = core::ptr::addr_of!((*s).keypair) as *const u8;
    let mut out = [0u8; KEYPAIR_LEN];
    core::ptr::copy_nonoverlapping(kp_ptr, out.as_mut_ptr(), KEYPAIR_LEN);
    Some(out)
}

unsafe fn session_clear() {
    let s = core::ptr::addr_of_mut!(SESSION);
    // Clear active FIRST so a concurrent reader (none at v1.0; future-
    // proof) can't observe a stale token bound to a cleared session.
    core::ptr::write(core::ptr::addr_of_mut!((*s).active), false);
    let user_ptr = core::ptr::addr_of_mut!((*s).user) as *mut u8;
    let token_ptr = core::ptr::addr_of_mut!((*s).token) as *mut u8;
    let kp_ptr = core::ptr::addr_of_mut!((*s).keypair) as *mut u8;
    for i in 0..MAX_USER_LEN {
        core::ptr::write(user_ptr.add(i), 0);
    }
    for i in 0..TOKEN_LEN {
        core::ptr::write(token_ptr.add(i), 0);
    }
    // The keypair is the load-bearing secret — volatile-wipe it.
    for i in 0..KEYPAIR_LEN {
        core::ptr::write_volatile(kp_ptr.add(i), 0);
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
// retain the hybrid keypair in the session, mint a session token.
//
// Payload format:
//   [0]            user_len u8 (1..=MAX_USER_LEN)
//   [1..1+ul]      user
//   [1+ul..3+ul]   pass_len u16 LE (1..=MAX_PASS_LEN)
//   [3+ul..]       passphrase
//
// Timing-attack mitigation: the not-found path takes a different amount
// of time than the unwrap-fail path (no Argon2id run vs Argon2id run).
// At v1.0 we accept the username-existence leak: a user-enumeration
// attacker who can talk to corvus can already enumerate via the audit
// log + the user-create surface. The Argon2id cost still bounds
// per-guess attempts on the passphrase.
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

    // Wipe KEK regardless of outcome (CORVUS-DESIGN §5.1).
    wipe(&mut kek);

    let mut keypair_vec = match unwrap_result {
        Some(k) => k,
        None => return send_response(TX_FD, STATUS_BAD_AUTH, &[]),
    };
    if keypair_vec.len() != KEYPAIR_LEN {
        wipe(&mut keypair_vec);
        return send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]);
    }
    // Move the unwrapped keypair into a fixed buffer for session_install.
    let mut keypair = [0u8; KEYPAIR_LEN];
    keypair.copy_from_slice(&keypair_vec);
    wipe(&mut keypair_vec);

    // Generate 16 bytes of entropy via CSPRNG → 32-char hex → prepend 's'.
    let mut entropy = [0u8; TOKEN_ENTROPY_BYTES];
    let rc = t_getrandom(entropy.as_mut_ptr(), TOKEN_ENTROPY_BYTES, 0);
    if rc != TOKEN_ENTROPY_BYTES as i64 {
        wipe(&mut keypair);
        return send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]);
    }
    let mut token = [0u8; TOKEN_LEN];
    token[0] = b's';
    for i in 0..TOKEN_ENTROPY_BYTES {
        token[1 + 2 * i] = nibble_to_hex(entropy[i] >> 4);
        token[1 + 2 * i + 1] = nibble_to_hex(entropy[i]);
    }
    let _ = t_explicit_bzero(entropy.as_mut_ptr(), TOKEN_ENTROPY_BYTES);

    // Install the session with the retained keypair, then wipe the
    // local copy — the mlock'd session slab is now the authority.
    session_install(user, &token, &keypair);
    wipe(&mut keypair);

    // Per CORVUS-DESIGN.md §6.4: the AUTH success payload is the
    // session token (33 bytes). Wipe the local copy after sending —
    // the mlock'd session slab is the authority.
    let rc = send_response(TX_FD, STATUS_OK, &token);
    wipe(&mut token);
    rc
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
//   2. Generate the real ML-KEM-768 + X25519 hybrid keypair.
//   3. Argon2id(passphrase, salt, default-params) → KEK.
//   4. AEGIS-256-wrap(KEK, nonce, AD, keypair) → ciphertext + tag.
//   5. Insert CorvusUserState; register the user's dataset for C-7.
//      Refuse if the user already exists.
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

    // Bound the in-memory user table: a hostile peer must not be able to
    // OOM the daemon by spamming USER_CREATE. (CapHostowner-gating this
    // verb + rate-limiting C-16 land with ADMIN_ELEVATE / P5-hostowner.)
    if user_states_count() >= MAX_USERS {
        return send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]);
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

    // Generate the real ML-KEM-768 + X25519 hybrid keypair.
    let mut keypair_plain = match generate_hybrid_keypair() {
        Some(kp) => kp,
        None => return send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]),
    };

    // Argon2id KEK.
    let mut kek = match argon2id_kek(
        passphrase,
        &salt,
        ARGON2_T_COST,
        ARGON2_M_COST_KIB,
        ARGON2_PARALLELISM,
    ) {
        Some(k) => k,
        None => {
            wipe(&mut keypair_plain);
            return send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]);
        }
    };

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
    wipe(&mut kek);
    wipe(&mut keypair_plain);

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

    // Register the user's dataset for the C-7 ownership gate. The
    // dataset (users/<name>) is unique because the user is unique
    // (user_states_insert already gated that), so this always succeeds.
    let mut dataset = Vec::new();
    dataset.extend_from_slice(b"users/");
    dataset.extend_from_slice(user_slice);
    dataset_owner_register(&dataset, user_slice);

    send_response(TX_FD, STATUS_OK, &[])
}

// handle_wrap — WRAP verb (verb_id=10). Wraps a 32-byte DEK under the
// session user's hybrid PUBLIC key into a DEK envelope.
//
// Payload format:
//   [0..33)        token         33 B
//   [33]           dataset_len   u8 (1..=MAX_DATASET_LEN)
//   [34..34+dl)    dataset       dl B
//   [34+dl..42+dl) key_id        u64 LE  (carried; v1.x multi-key — ignored)
//   [42+dl..44+dl) dek_len       u16 LE  (must == DEK_LEN)
//   [44+dl..)      dek           dek_len B
//
// C-7 gate: the session user must own `dataset` (else PermissionDenied /
// NotFound) — WRAP shares UNWRAP's ownership discipline so a session
// cannot mint envelopes tagged for datasets it does not own.
unsafe fn handle_wrap(payload: &[u8]) -> i64 {
    // token + dataset_len + (>=1 dataset byte) + key_id + dek_len.
    if payload.len() < TOKEN_LEN + 1 + 1 + 8 + 2 {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let token = &payload[0..TOKEN_LEN];
    let dataset_len = payload[TOKEN_LEN] as usize;
    if dataset_len == 0 || dataset_len > MAX_DATASET_LEN {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let ds_off = TOKEN_LEN + 1;
    if payload.len() < ds_off + dataset_len + 8 + 2 {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let dataset = &payload[ds_off..ds_off + dataset_len];
    let kid_off = ds_off + dataset_len;
    // key_id (u64 LE) — bound into the DEK envelope AD (rotation safety).
    let mut key_id: u64 = 0;
    for i in 0..8 {
        key_id |= (payload[kid_off + i] as u64) << (8 * i);
    }
    let dek_len_off = kid_off + 8;
    let dek_len = (payload[dek_len_off] as usize) | ((payload[dek_len_off + 1] as usize) << 8);
    if dek_len != DEK_LEN {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let dek_off = dek_len_off + 2;
    if payload.len() != dek_off + dek_len {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let dek_slice = &payload[dek_off..dek_off + dek_len];

    // Session-token gate.
    if !session_token_matches(token) {
        return send_response(TX_FD, STATUS_BAD_AUTH, &[]);
    }
    // C-7 ownership gate.
    let session_user = match session_user_copy() {
        Some(u) => u,
        None => return send_response(TX_FD, STATUS_BAD_AUTH, &[]),
    };
    let owner = match dataset_owner_find(dataset) {
        Some(o) => o,
        None => return send_response(TX_FD, STATUS_NOT_FOUND, &[]),
    };
    if owner != session_user {
        return send_response(TX_FD, STATUS_PERMISSION_DENIED, &[]);
    }

    // Wrap the DEK under the session user's hybrid public key.
    let mut keypair = match session_keypair_copy() {
        Some(kp) => kp,
        None => return send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]),
    };
    let mut x_pk = [0u8; X25519_KEY_LEN];
    x_pk.copy_from_slice(&keypair[KP_X25519_PK_OFF..KP_X25519_PK_OFF + X25519_KEY_LEN]);
    let mut mlkem_ek = [0u8; MLKEM_EK_LEN];
    mlkem_ek.copy_from_slice(&keypair[KP_MLKEM_EK_OFF..KP_MLKEM_EK_OFF + MLKEM_EK_LEN]);
    let mut dek = [0u8; DEK_LEN];
    dek.copy_from_slice(dek_slice);

    let envelope = dek_envelope_wrap(&x_pk, &mlkem_ek, &dek, dataset, key_id);

    // Wipe secret-bearing locals (x_pk / mlkem_ek are public — no wipe).
    wipe(&mut dek);
    wipe(&mut keypair);

    match envelope {
        Some(env) => send_response(TX_FD, STATUS_OK, &env),
        None => send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]),
    }
}

// handle_unwrap — UNWRAP verb (verb_id=4). Recovers a 32-byte DEK from a
// DEK envelope using the session user's hybrid SECRET key.
//
// Payload format (STRATUM-API-V1.md §5.2):
//   [0..33)        token         33 B
//   [33]           dataset_len   u8
//   [34..34+dl)    dataset       dl B
//   [34+dl..42+dl) key_id        u64 LE  (carried; v1.x multi-key)
//   [42+dl..44+dl) wrapped_len   u16 LE
//   [44+dl..)      wrapped       wrapped_len B
//
// C-7 gate (specs/corvus.tla UnwrapOwnerOnly): the session user must
// own `dataset`. The gate fires BEFORE any crypto — a cross-user UNWRAP
// is refused (PermissionDenied) without touching the keypair.
unsafe fn handle_unwrap(payload: &[u8]) -> i64 {
    if payload.len() < TOKEN_LEN + 1 + 1 + 8 + 2 {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let token = &payload[0..TOKEN_LEN];
    let dataset_len = payload[TOKEN_LEN] as usize;
    if dataset_len == 0 || dataset_len > MAX_DATASET_LEN {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let ds_off = TOKEN_LEN + 1;
    if payload.len() < ds_off + dataset_len + 8 + 2 {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let dataset = &payload[ds_off..ds_off + dataset_len];
    let kid_off = ds_off + dataset_len;
    // key_id (u64 LE) — bound into the DEK envelope AD (rotation safety).
    let mut key_id: u64 = 0;
    for i in 0..8 {
        key_id |= (payload[kid_off + i] as u64) << (8 * i);
    }
    let wrapped_len_off = kid_off + 8;
    let wrapped_len =
        (payload[wrapped_len_off] as usize) | ((payload[wrapped_len_off + 1] as usize) << 8);
    if wrapped_len == 0 || wrapped_len > MAX_WRAPPED_LEN {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let wrapped_off = wrapped_len_off + 2;
    if payload.len() != wrapped_off + wrapped_len {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let wrapped = &payload[wrapped_off..wrapped_off + wrapped_len];
    // A structurally-malformed envelope (wrong length / version) is a
    // client format error, not a corvus fault — report BadFormat. The
    // AEAD-tag-failure path (dek_envelope_unwrap → None) below is what
    // yields InternalError.
    if wrapped.len() != ENVELOPE_LEN || wrapped[0] != ENVELOPE_VERSION {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }

    // Session-token gate.
    if !session_token_matches(token) {
        return send_response(TX_FD, STATUS_BAD_AUTH, &[]);
    }
    // C-7 ownership gate — refuse cross-user BEFORE any crypto.
    let session_user = match session_user_copy() {
        Some(u) => u,
        None => return send_response(TX_FD, STATUS_BAD_AUTH, &[]),
    };
    let owner = match dataset_owner_find(dataset) {
        Some(o) => o,
        None => return send_response(TX_FD, STATUS_NOT_FOUND, &[]),
    };
    if owner != session_user {
        return send_response(TX_FD, STATUS_PERMISSION_DENIED, &[]);
    }

    // Owned dataset — unwrap the DEK envelope with the hybrid secret key.
    let mut keypair = match session_keypair_copy() {
        Some(kp) => kp,
        None => return send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]),
    };
    let mut x_sk = [0u8; X25519_KEY_LEN];
    x_sk.copy_from_slice(&keypair[KP_X25519_SK_OFF..KP_X25519_SK_OFF + X25519_KEY_LEN]);
    let mut mlkem_dk = [0u8; MLKEM_DK_LEN];
    mlkem_dk.copy_from_slice(&keypair[KP_MLKEM_DK_OFF..KP_MLKEM_DK_OFF + MLKEM_DK_LEN]);

    let dek = dek_envelope_unwrap(&x_sk, &mlkem_dk, wrapped, dataset, key_id);

    // Wipe secret-bearing locals.
    wipe(&mut x_sk);
    wipe(&mut mlkem_dk);
    wipe(&mut keypair);

    match dek {
        Some(mut d) => {
            let rc = send_response(TX_FD, STATUS_OK, &d);
            wipe(&mut d);
            rc
        }
        // A well-formed frame whose envelope is malformed or fails the
        // AEAD tag: the DEK could not be recovered. Not a wire-format
        // fault (the frame parsed) — report InternalError per
        // STRATUM-API-V1.md §5.5.
        None => send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]),
    }
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
            // payload).
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
            VERB_UNWRAP => handle_unwrap(&payload[..payload_len]),
            VERB_USER_CREATE => handle_user_create(&payload[..payload_len]),
            VERB_WRAP => handle_wrap(&payload[..payload_len]),
            _ => send_response(TX_FD, STATUS_BAD_FORMAT, &[]),
        };
        if rc < 0 {
            return -1;
        }
        // Wipe payload before next iteration (defence-in-depth: verbs
        // carry secrets — passphrases, DEKs, wrapped blobs). A bzero
        // failure means the buffer may still hold a secret — fail closed.
        if t_explicit_bzero(payload.as_mut_ptr(), payload_len) != 0 {
            return -1;
        }
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
    t_putstr("corvus: starting (P5-corvus-bringup-d)\n");

    // Heap + state vecs FIRST. Every allocation downstream (argon2 +
    // ml-kem working memory, AD buffers, the user-state + dataset
    // tables) depends on the allocator being live.
    unsafe {
        heap_init();
        user_states_init();
        dataset_owners_init();
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

    // Wipe any residual session state before exiting.
    unsafe { session_clear() };

    // Close our pipe fds explicitly. The kernel will release them on
    // exit anyway, but the explicit close exercises the cleanup path.
    let _ = unsafe { t_close(RX_FD) };
    let _ = unsafe { t_close(TX_FD) };

    t_putstr("corvus: server_loop returned EOF; shutting down clean\n");
    0
}
