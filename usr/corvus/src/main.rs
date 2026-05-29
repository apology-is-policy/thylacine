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
//                                  USER_CREATE.
//   P5-corvus-bringup-d (cb0f849): the real ML-KEM-768 + X25519
//                                  hybrid keypair; the hybrid-PKE
//                                  DEK envelope; WRAP + UNWRAP verbs.
//   P5-corvus-srv-impl-b1 (4bf689c): Q11 wire 3 → 4 byte request
//                                  header (protocol_version byte).
//   P5-corvus-srv-impl-b3b (this chunk): corvus is now a real 9P2000.L
//                                  server reached via the kernel-owned
//                                  /srv/corvus transport (CORVUS-DESIGN
//                                  §6). The fd 0/1 pipe-pair harness
//                                  retires; corvus posts its service via
//                                  SYS_POST_SERVICE("corvus"), accepts
//                                  each client connection via
//                                  SYS_SRV_ACCEPT, and serves a tiny
//                                  9P2000.L namespace (root QTDIR
//                                  containing a single ctl QTFILE).
//                                  Verb frames travel as Twrite payloads
//                                  on /ctl; responses travel as Rread
//                                  payloads. The existing five verb
//                                  handlers — AUTH / SESSION_CLOSE /
//                                  UNWRAP / USER_CREATE / WRAP — are
//                                  reused unchanged at the dispatch
//                                  level; only the I/O transport
//                                  changes.
//
// At this sub-chunk corvus:
//
//   1. Runs the hardening sequence at startup (mlockall + set_dumpable(0)
//      + set_traceable(0) + a CSPRNG-seeded probe).
//
//   2. Initializes the static-buffer heap (24 MiB), the user-state vec,
//      and the dataset-ownership table.
//
//   3. Posts /srv/corvus via SYS_POST_SERVICE("corvus"). Requires the
//      joey-stamped PROC_FLAG_MAY_POST_SERVICE (granted by
//      SYS_SPAWN_WITH_PERMS in joey; see P5-corvus-srv-impl-b3a).
//
//   4. Main loop: SYS_POLL([listener, conns...]); on listener-ready
//      accept a connection, on conn-ready service one 9P Tmsg.
//
//   5. 9P2000.L server (per-connection arena): Tversion / Tauth(reject)
//      / Tattach / Twalk / Tlopen / Tread / Twrite / Tclunk. Tunknown →
//      Rlerror.
//
//   6. ctl-fid Twrite payloads carry the Q11-corrected 4-byte verb
//      header followed by the verb body. corvus accumulates Twrite
//      payloads until a complete verb frame is in hand; dispatches the
//      verb handler; stages the 3-byte response frame for the next
//      Tread(s) to drain.
//
//   7. Verbs (verb_id ∈ { AUTH, SESSION_CLOSE, UNWRAP, USER_CREATE,
//      WRAP }) behave identically to P5-corvus-bringup-d; only the I/O
//      transport changed. Other verb_ids → BadFormat.
//
// State storage: in-memory only at v1.0. CorvusUserState + dataset-
// ownership records do not survive a corvus restart; FS persistence
// (loading /var/lib/corvus/) lands once that tree is mounted.
//
// Multi-session at v1.0: corvus accepts multiple connections in
// parallel BUT keeps a single global SESSION. v1.0's kernel cap
// SRV_CONN_PER_PROC_MAX=1 plus joey being the single console-attached
// Proc means there's exactly one active connection at any time, so
// per-conn-session is equivalent to global session here. Multi-session
// support (a Session per Conn keyed by peer stripes) lifts when the
// multi-peer surface lands.
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
//   ConnAccept(p)         — accept_one() — new Conn record born at
//                           t_srv_accept return.
//   ConnTeardown(p)       — close_conn() — Conn record cleared on
//                           POLLHUP / wire fault / Tversion reset.
//   ConnOpIdentityIsKernelTruth — every Conn record carries
//                                 a SYS_SRV_PEER snapshot at accept time.

#![no_std]
#![no_main]

extern crate alloc;

// 9P2000.L server-side codec. Lifted from corvus's private `p9` module
// into `libthyla_rs::ninep` at U-2h-ninep; aliased back to `p9` so the
// existing dispatcher references survive byte-identical.
use libthyla_rs::ninep as p9;

use libthyla_rs::{
    t_cap_grant, t_chroot, t_close, t_explicit_bzero, t_fsync, t_getrandom, t_mlockall,
    t_poll, t_post_service, t_putstr, t_read, t_set_dumpable, t_set_traceable,
    t_srv_accept, t_srv_peer, t_walk_create, t_walk_open, t_write, TPollFd, TSrvPeerInfo,
    T_CAP_HOSTOWNER, T_OREAD, T_OWRITE, T_POLLHUP, T_POLLIN, T_WALK_OPEN_FROM_ROOT,
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

const CORVUS_MAGIC: u32 = 0x53565243; // 'CRVS' LE
const CORVUS_STATE_VERSION: u32 = 1;
const ARGON2_SALT_LEN: usize = 16;
const AEGIS256_KEY_LEN: usize = 32;
const AEGIS256_NONCE_LEN: usize = 32;
const AEGIS256_TAG_LEN: usize = 32;

// Hybrid keypair layout (P5-corvus-bringup-d): the AEGIS-wrapped
// plaintext is the concatenation of the X25519 and ML-KEM-768 key
// halves. See KP_* offsets below.
const X25519_KEY_LEN: usize = 32;
const MLKEM_EK_LEN: usize = 1184;
const MLKEM_DK_LEN: usize = 2400;
const MLKEM_CT_LEN: usize = 1088;
const KP_X25519_SK_OFF: usize = 0;
const KP_X25519_PK_OFF: usize = X25519_KEY_LEN;
const KP_MLKEM_EK_OFF: usize = X25519_KEY_LEN * 2;
const KP_MLKEM_DK_OFF: usize = X25519_KEY_LEN * 2 + MLKEM_EK_LEN;
const KEYPAIR_LEN: usize = X25519_KEY_LEN * 2 + MLKEM_EK_LEN + MLKEM_DK_LEN; // 3648

const HEADER_LEN: usize = 4 + 4 + 4 + 8 + 4 + ARGON2_SALT_LEN + AEGIS256_NONCE_LEN; // = 72
const TOTAL_LEN: usize = HEADER_LEN + KEYPAIR_LEN + AEGIS256_TAG_LEN; // = 3752

const _: () = assert!(HEADER_LEN == 72, "state file header layout drift");
const _: () = assert!(KEYPAIR_LEN == 3648, "hybrid keypair layout drift");
const _: () = assert!(KP_MLKEM_DK_OFF == 1248, "keypair sub-offset drift");
const _: () = assert!(AEGIS256_KEY_LEN == 32, "AEGIS-256 key width is 32");
const _: () = assert!(AEGIS256_NONCE_LEN == 32, "AEGIS-256 nonce width is 32");
const _: () = assert!(AEGIS256_TAG_LEN == 32, "AEGIS-256 tag width is 32");

// Argon2id v1.0 preset (CORVUS-DESIGN §4.3). m_cost=16 MiB is the
// bringup cap (one quarter of the design default 64 MiB) — bounded by
// the static 24 MiB heap. State-file m_cost_kib is per-record, so a
// future bump only resizes the heap.
const ARGON2_T_COST: u32 = 2;
const ARGON2_M_COST_KIB: u32 = 16 * 1024;
const ARGON2_PARALLELISM: u32 = 1;

// AD for the keypair-wrap AEAD: "thylacine-corvus-v1" || user_name ||
// backend_id. Backend ID at v1.0 is `passphrase` = 0.
const AD_PREFIX: &[u8] = b"thylacine-corvus-v1";
const BACKEND_ID_PASSPHRASE: u8 = 0;

#[derive(Clone)]
struct CorvusUserState {
    user: Vec<u8>,
    t_cost: u32,
    m_cost_kib: u32,
    parallelism: u32,
    salt: [u8; ARGON2_SALT_LEN],
    nonce: [u8; AEGIS256_NONCE_LEN],
    ciphertext: [u8; KEYPAIR_LEN],
    tag: [u8; AEGIS256_TAG_LEN],
}

impl CorvusUserState {
    fn build_ad(&self, ad_out: &mut Vec<u8>) {
        ad_out.clear();
        ad_out.extend_from_slice(AD_PREFIX);
        ad_out.extend_from_slice(&self.user);
        ad_out.push(BACKEND_ID_PASSPHRASE);
    }

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

const DEK_LEN: usize = 32;
const ENVELOPE_VERSION: u8 = 1;
const ENV_MLKEM_CT_OFF: usize = 1;
const ENV_EPH_PK_OFF: usize = ENV_MLKEM_CT_OFF + MLKEM_CT_LEN; // 1089
const ENV_NONCE_OFF: usize = ENV_EPH_PK_OFF + X25519_KEY_LEN; // 1121
const ENV_DEK_CT_OFF: usize = ENV_NONCE_OFF + AEGIS256_NONCE_LEN; // 1153
const ENV_DEK_TAG_OFF: usize = ENV_DEK_CT_OFF + DEK_LEN; // 1185
const ENVELOPE_LEN: usize = ENV_DEK_TAG_OFF + AEGIS256_TAG_LEN; // 1217

const _: () = assert!(ENVELOPE_LEN == 1217, "DEK envelope layout drift");

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
        self.fill_bytes(dest);
        Ok(())
    }
}

impl rand_core::CryptoRng for ThylaRng {}

fn wipe(buf: &mut [u8]) {
    for b in buf.iter_mut() {
        unsafe { core::ptr::write_volatile(b as *mut u8, 0) };
    }
}

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

fn generate_hybrid_keypair() -> Option<[u8; KEYPAIR_LEN]> {
    let mut rng = ThylaRng;
    let x_sk = StaticSecret::random_from_rng(&mut rng);
    let x_pk = PublicKey::from(&x_sk);
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
    wipe(&mut dk_bytes[..]);
    wipe(&mut ek_bytes[..]);
    Some(kp)
}

fn dek_envelope_wrap(
    x25519_pk: &[u8; X25519_KEY_LEN],
    mlkem_ek_bytes: &[u8],
    dek: &[u8; DEK_LEN],
    ad_dataset: &[u8],
    key_id: u64,
) -> Option<[u8; ENVELOPE_LEN]> {
    let mut rng = ThylaRng;

    let enc = Encoded::<MlKemEk>::try_from(mlkem_ek_bytes).ok()?;
    let mlkem_ek = MlKemEk::from_bytes(&enc);
    let (mlkem_ct, mut ss_pq) = mlkem_ek.encapsulate(&mut rng).ok()?;
    if mlkem_ct.len() != MLKEM_CT_LEN || ss_pq.len() != AEGIS256_KEY_LEN {
        wipe(&mut ss_pq[..]);
        return None;
    }

    let eph = EphemeralSecret::random_from_rng(&mut rng);
    let eph_pk = PublicKey::from(&eph);
    let their_pk = PublicKey::from(*x25519_pk);
    let ss_cl = eph.diffie_hellman(&their_pk);

    let mut kek = sha256_kek(&[
        DEK_KDF_DOMAIN,
        &ss_pq[..],
        &ss_cl.as_bytes()[..],
        &eph_pk.as_bytes()[..],
        &mlkem_ct[..],
    ]);
    wipe(&mut ss_pq[..]);

    let mut nonce = [0u8; AEGIS256_NONCE_LEN];
    rng.fill_bytes(&mut nonce);
    let mut ad: Vec<u8> = Vec::new();
    ad.extend_from_slice(DEK_AD_PREFIX);
    ad.extend_from_slice(ad_dataset);
    ad.extend_from_slice(&key_id.to_le_bytes());
    let mut dek_ct = [0u8; DEK_LEN];
    let dek_tag = aegis_wrap(&kek, &nonce, &ad, dek, &mut dek_ct);
    wipe(&mut kek);

    let mut env = [0u8; ENVELOPE_LEN];
    env[0] = ENVELOPE_VERSION;
    env[ENV_MLKEM_CT_OFF..ENV_MLKEM_CT_OFF + MLKEM_CT_LEN].copy_from_slice(&mlkem_ct[..]);
    env[ENV_EPH_PK_OFF..ENV_EPH_PK_OFF + X25519_KEY_LEN].copy_from_slice(eph_pk.as_bytes());
    env[ENV_NONCE_OFF..ENV_NONCE_OFF + AEGIS256_NONCE_LEN].copy_from_slice(&nonce);
    env[ENV_DEK_CT_OFF..ENV_DEK_CT_OFF + DEK_LEN].copy_from_slice(&dek_ct);
    env[ENV_DEK_TAG_OFF..ENV_DEK_TAG_OFF + AEGIS256_TAG_LEN].copy_from_slice(&dek_tag);
    Some(env)
}

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

    let ct = Ciphertext::<MlKem768>::try_from(mlkem_ct_bytes).ok()?;
    let enc = Encoded::<MlKemDk>::try_from(mlkem_dk_bytes).ok()?;
    let mlkem_dk = MlKemDk::from_bytes(&enc);
    let mut ss_pq = mlkem_dk.decapsulate(&ct).ok()?;
    if ss_pq.len() != AEGIS256_KEY_LEN {
        wipe(&mut ss_pq[..]);
        return None;
    }

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
// User state vector + dataset ownership table — in-memory at v1.0.
// =============================================================================

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

struct DatasetOwner {
    dataset: Vec<u8>,
    owner: Vec<u8>,
}

static mut DATASET_OWNERS: Option<Vec<DatasetOwner>> = None;

unsafe fn dataset_owners_init() {
    core::ptr::write(core::ptr::addr_of_mut!(DATASET_OWNERS), Some(Vec::new()));
}

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
const VERB_ADMIN_ELEVATE: u8 = 7;
const VERB_WRAP: u8 = 10;

// System passphrase — verified by ADMIN_ELEVATE before the kernel `cap`
// grant is registered (CORVUS-DESIGN.md §5.5). v1.0 PLACEHOLDER: a
// hardcoded byte string shared with joey's boot sequence. Real
// installer-driven first-boot setup (argon2id(passphrase, salt) →
// system_KEK + AEAD-wrapped magic, persisted across reboots) lands when
// CRVS persistence does. The current scheme is functionally equivalent
// to "the system passphrase is `thylacine`" — sufficient to exercise
// the elevation mechanism end-to-end in the boot test.
const SYSTEM_PASSPHRASE: &[u8] = b"thylacine";

// Stratum↔corvus wire version. STRATUM-API-V1.md Q11 resolved to a
// 4-byte request header carrying this byte at offset 1; an unknown
// value is BadFormat-then-tear-down (the frame's shape may change
// across versions so a length-mismatched body would re-sync the stream
// nowhere safe).
const CORVUS_PROTOCOL_VERSION: u8 = 1;

const STATUS_OK: u8 = 0;
const STATUS_BAD_AUTH: u8 = 1;
const STATUS_PERMISSION_DENIED: u8 = 2;
const STATUS_NOT_FOUND: u8 = 3;
#[allow(dead_code)]
const STATUS_RATE_LIMITED: u8 = 4;
const STATUS_BAD_FORMAT: u8 = 5;
const STATUS_INTERNAL_ERROR: u8 = 6;

const TOKEN_LEN: usize = 33;
const TOKEN_ENTROPY_BYTES: usize = 16;

const MAX_USER_LEN: usize = 32;
const MAX_PASS_LEN: usize = 256;

const MAX_DATASET_LEN: usize = 64;
const MAX_WRAPPED_LEN: usize = 2048;

// UNWRAP carries the largest inbound payload:
//   token(33) + dataset_len(1) + dataset(≤64) + key_id(8)
//   + wrapped_len(2) + wrapped(≤2048) = 2156 bytes.
const MAX_PAYLOAD_LEN: usize = TOKEN_LEN + 1 + MAX_DATASET_LEN + 8 + 2 + MAX_WRAPPED_LEN;
const _: () = assert!(
    MAX_PAYLOAD_LEN >= 1 + MAX_USER_LEN + 2 + MAX_PASS_LEN + 1,
    "USER_CREATE payload must fit MAX_PAYLOAD_LEN"
);

// 4-byte request header (verb + protocol_version + len_lo + len_hi).
const REQ_HDR_LEN: usize = 4;

// 3-byte response header (status + len_lo + len_hi); responses carry no
// version byte (verb pin response shape — Q11 §6.4).
const RESP_HDR_LEN: usize = 3;

// Max staged response frame: 3-byte header + ENVELOPE_LEN. WRAP returns
// the DEK envelope which is the largest response payload.
const MAX_RESPONSE_FRAME: usize = RESP_HDR_LEN + ENVELOPE_LEN;

// =============================================================================
// Session table — single global slot at v1.0.
// =============================================================================
//
// Spec (specs/corvus.tla) models Sessions as SUBSET SessionRecord with at
// most one record per owner_proc. At v1.0 corvus has a single global
// SESSION slot — adequate because joey is the only console-attached
// Proc and the kernel cap SRV_CONN_PER_PROC_MAX=1 means at most one live
// connection per peer. Multi-session (per-Conn Session keyed by peer
// stripes) lifts when the multi-peer surface lands.
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
    keypair: [u8; KEYPAIR_LEN],
}

static mut SESSION: Session = Session {
    active: false,
    user_len: 0,
    user: [0; MAX_USER_LEN],
    token: [0; TOKEN_LEN],
    keypair: [0; KEYPAIR_LEN],
};

unsafe fn session_active() -> bool {
    core::ptr::read(core::ptr::addr_of!(SESSION.active))
}

unsafe fn session_install(user: &[u8], token: &[u8; TOKEN_LEN], keypair: &[u8; KEYPAIR_LEN]) {
    let s = core::ptr::addr_of_mut!(SESSION);
    let user_ptr = core::ptr::addr_of_mut!((*s).user) as *mut u8;
    let token_ptr = core::ptr::addr_of_mut!((*s).token) as *mut u8;
    let kp_ptr = core::ptr::addr_of_mut!((*s).keypair) as *mut u8;
    for i in 0..MAX_USER_LEN {
        core::ptr::write(user_ptr.add(i), 0);
    }
    for i in 0..user.len() {
        core::ptr::write(user_ptr.add(i), user[i]);
    }
    for i in 0..TOKEN_LEN {
        core::ptr::write(token_ptr.add(i), token[i]);
    }
    core::ptr::copy_nonoverlapping(keypair.as_ptr(), kp_ptr, KEYPAIR_LEN);
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
    let mut diff: u8 = 0;
    for i in 0..TOKEN_LEN {
        let tok_byte = core::ptr::read(token_ptr.add(i));
        diff |= tok_byte ^ candidate[i];
    }
    diff == 0
}

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
    // Clear active FIRST so a concurrent reader can't observe a stale
    // token bound to a cleared session.
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
// Verb-frame staging — emit a 3-byte response frame (status, len_lo,
// len_hi, payload) into the conn's pending_response buffer. Drained by
// subsequent Treads on the ctl fid.
// =============================================================================

fn stage_response(response: &mut Vec<u8>, status: u8, payload: &[u8]) {
    response.clear();
    response.reserve(RESP_HDR_LEN + payload.len());
    response.push(status);
    response.push((payload.len() & 0xff) as u8);
    response.push(((payload.len() >> 8) & 0xff) as u8);
    response.extend_from_slice(payload);
}

// =============================================================================
// Verb handlers.
//
// Each handler parses the verb-specific payload and stages a response
// frame into the connection's `response` buffer. The transport (which
// 9P Twrites delivered this payload, which Treads will drain the
// response) is owned by the main loop; handlers know nothing about it.
// =============================================================================

// handle_auth — parse AUTH payload, run Argon2id + AEGIS-256-unwrap,
// retain the hybrid keypair in the session, mint a session token.
//
// Payload format:
//   [0]            user_len u8 (1..=MAX_USER_LEN)
//   [1..1+ul]      user
//   [1+ul..3+ul]   pass_len u16 LE (1..=MAX_PASS_LEN)
//   [3+ul..]       passphrase
unsafe fn handle_auth(payload: &[u8], response: &mut Vec<u8>) {
    if payload.len() < 3 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let user_len = payload[0] as usize;
    if user_len == 0 || user_len > MAX_USER_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    if payload.len() < 1 + user_len + 2 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let user = &payload[1..1 + user_len];
    let pass_len = (payload[1 + user_len] as usize) | ((payload[2 + user_len] as usize) << 8);
    if pass_len == 0 || pass_len > MAX_PASS_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    if payload.len() != 1 + user_len + 2 + pass_len {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let passphrase = &payload[1 + user_len + 2..1 + user_len + 2 + pass_len];

    // Spec's one-session-per-Proc precondition: refuse AUTH while a
    // session is bound (corvus.tla AuthSuccess `~(\E s : s.owner_proc =
    // p)`).
    if session_active() {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }

    let state = match user_states_find(user) {
        Some(s) => s,
        None => return stage_response(response, STATUS_BAD_AUTH, &[]),
    };

    let mut kek = match argon2id_kek(
        passphrase,
        &state.salt,
        state.t_cost,
        state.m_cost_kib,
        state.parallelism,
    ) {
        Some(k) => k,
        None => return stage_response(response, STATUS_INTERNAL_ERROR, &[]),
    };

    let mut ad: Vec<u8> = Vec::new();
    state.build_ad(&mut ad);
    let unwrap_result = aegis_unwrap(&kek, &state.nonce, &ad, &state.ciphertext, &state.tag);
    wipe(&mut kek);

    let mut keypair_vec = match unwrap_result {
        Some(k) => k,
        None => return stage_response(response, STATUS_BAD_AUTH, &[]),
    };
    if keypair_vec.len() != KEYPAIR_LEN {
        wipe(&mut keypair_vec);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    let mut keypair = [0u8; KEYPAIR_LEN];
    keypair.copy_from_slice(&keypair_vec);
    wipe(&mut keypair_vec);

    let mut entropy = [0u8; TOKEN_ENTROPY_BYTES];
    let rc = t_getrandom(entropy.as_mut_ptr(), TOKEN_ENTROPY_BYTES, 0);
    if rc != TOKEN_ENTROPY_BYTES as i64 {
        wipe(&mut keypair);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    let mut token = [0u8; TOKEN_LEN];
    token[0] = b's';
    for i in 0..TOKEN_ENTROPY_BYTES {
        token[1 + 2 * i] = nibble_to_hex(entropy[i] >> 4);
        token[1 + 2 * i + 1] = nibble_to_hex(entropy[i]);
    }
    let _ = t_explicit_bzero(entropy.as_mut_ptr(), TOKEN_ENTROPY_BYTES);

    session_install(user, &token, &keypair);
    wipe(&mut keypair);

    stage_response(response, STATUS_OK, &token);
    wipe(&mut token);
}

// handle_user_create — USER_CREATE verb (verb_id=5).
//
// Payload format:
//   [0]            user_len u8 (1..=MAX_USER_LEN)
//   [1..1+ul]      user
//   [1+ul..3+ul]   pass_len u16 LE (1..=MAX_PASS_LEN)
//   [3+ul..]       passphrase
//   [end-1]        backend u8 (v1.0: 0=passphrase only)
// peer_live_caps — fresh SYS_SRV_PEER read of the connection peer's
// current capability set. C-22 (CORVUS-DESIGN.md §6.3): the peer's caps
// are mutable (elevated by /cap/use mid-conversation), so admin-verb
// gating must re-query LIVE caps before each call, never relying on the
// at-accept snapshot in conn.peer.
//
// Returns 0 on any failure (dead peer / syscall error / etc.) — the
// fail-closed sentinel. A caller that requires CAP_HOSTOWNER bit-tests
// the returned mask; a 0 return necessarily fails the gate.
unsafe fn peer_live_caps(handle: i64) -> u64 {
    let mut info = TSrvPeerInfo::default();
    if t_srv_peer(handle, &mut info) != 0 {
        return 0;
    }
    if info.alive == 0 {
        return 0;
    }
    info.caps
}

// handle_user_create — USER_CREATE verb (verb_id=5). Admin-gated as of
// P5-hostowner-b-b: the caller's connection peer must hold CAP_HOSTOWNER
// (the corvus-d audit's deferred F2-gate-half now closes here), unless
// no users yet exist — the first USER_CREATE bootstraps the initial
// hostowner candidate (otherwise no user could authenticate to call
// ADMIN_ELEVATE, a chicken-and-egg). Live caps re-query per C-22.
unsafe fn handle_user_create(handle: i64, payload: &[u8], response: &mut Vec<u8>) {
    // P5-hostowner-b-b: admin gate. Bootstrap exception: the first
    // user creation is free (the hostowner candidate is the first user
    // — they AUTH, ADMIN_ELEVATE, then create subsequent users while
    // holding CAP_HOSTOWNER). After the first user exists, every
    // USER_CREATE requires the peer to hold CAP_HOSTOWNER via fresh
    // SYS_SRV_PEER. Single-threaded corvus → no TOCTOU on the count.
    if user_states_count() > 0 {
        let caps = peer_live_caps(handle);
        if (caps & T_CAP_HOSTOWNER) == 0 {
            return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
        }
    }

    if payload.len() < 4 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let user_len = payload[0] as usize;
    if user_len == 0 || user_len > MAX_USER_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    if payload.len() < 1 + user_len + 2 + 1 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let user_slice = &payload[1..1 + user_len];
    let pass_len = (payload[1 + user_len] as usize) | ((payload[2 + user_len] as usize) << 8);
    if pass_len == 0 || pass_len > MAX_PASS_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    if payload.len() != 1 + user_len + 2 + pass_len + 1 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let passphrase = &payload[1 + user_len + 2..1 + user_len + 2 + pass_len];
    let backend = payload[1 + user_len + 2 + pass_len];
    if backend != BACKEND_ID_PASSPHRASE {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }

    if user_states_find(user_slice).is_some() {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }
    if user_states_count() >= MAX_USERS {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }

    let mut salt = [0u8; ARGON2_SALT_LEN];
    if t_getrandom(salt.as_mut_ptr(), ARGON2_SALT_LEN, 0) != ARGON2_SALT_LEN as i64 {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    let mut nonce = [0u8; AEGIS256_NONCE_LEN];
    if t_getrandom(nonce.as_mut_ptr(), AEGIS256_NONCE_LEN, 0) != AEGIS256_NONCE_LEN as i64 {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }

    let mut keypair_plain = match generate_hybrid_keypair() {
        Some(kp) => kp,
        None => return stage_response(response, STATUS_INTERNAL_ERROR, &[]),
    };

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
            return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
        }
    };

    let mut ad: Vec<u8> = Vec::new();
    let mut user_vec: Vec<u8> = Vec::new();
    user_vec.extend_from_slice(user_slice);
    ad.extend_from_slice(AD_PREFIX);
    ad.extend_from_slice(&user_vec);
    ad.push(BACKEND_ID_PASSPHRASE);

    let mut ciphertext = [0u8; KEYPAIR_LEN];
    let tag = aegis_wrap(&kek, &nonce, &ad, &keypair_plain, &mut ciphertext);

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
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }

    let mut dataset = Vec::new();
    dataset.extend_from_slice(b"users/");
    dataset.extend_from_slice(user_slice);
    dataset_owner_register(&dataset, user_slice);

    stage_response(response, STATUS_OK, &[]);
}

// handle_admin_elevate — ADMIN_ELEVATE verb (verb_id=7). Verifies the
// peer is console-attached + the system passphrase is correct, then
// registers a pending CAP_HOSTOWNER grant against the peer's stripes
// via SYS_CAP_GRANT. The peer redeems via SYS_CAP_USE (joey-side).
//
// CORVUS-DESIGN.md §5.5. Two gates in two trust domains: corvus
// verifies the system passphrase (a check the kernel has no notion of);
// the kernel verifies PROC_FLAG_CONSOLE_ATTACHED at /use redemption.
// A compromised corvus could try to register grants for arbitrary
// stripes, but only a console-attached writer can actually redeem.
//
// Payload format (CORVUS-DESIGN.md §6.4):
//   [0..33)         token             33 B
//   [33..35)        sys_pass_len      u16 LE (1..=MAX_PASS_LEN)
//   [35..35+spl)    sys_passphrase    spl B
//
// Returns:
//   STATUS_OK                no payload
//   STATUS_BAD_FORMAT        malformed length / over-sized passphrase
//   STATUS_BAD_AUTH          unknown token OR wrong system passphrase
//   STATUS_PERMISSION_DENIED peer is not console-attached
//   STATUS_INTERNAL_ERROR    cap grant registration failed (e.g., table
//                            full; the kernel rejected the syscall)
unsafe fn handle_admin_elevate(peer: &TSrvPeerInfo, payload: &[u8],
                               response: &mut Vec<u8>) {
    if payload.len() < TOKEN_LEN + 2 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let token = &payload[0..TOKEN_LEN];
    let sys_pass_len = (payload[TOKEN_LEN] as usize)
        | ((payload[TOKEN_LEN + 1] as usize) << 8);
    if sys_pass_len == 0 || sys_pass_len > MAX_PASS_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    if payload.len() != TOKEN_LEN + 2 + sys_pass_len {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let sys_passphrase = &payload[TOKEN_LEN + 2..TOKEN_LEN + 2 + sys_pass_len];

    // Token validity — any logged-in session may attempt elevation; the
    // console + passphrase gates are the access controls.
    if !session_token_matches(token) {
        return stage_response(response, STATUS_BAD_AUTH, &[]);
    }

    // Console-attached check via the cached peer info — console
    // attachment is immutable (one-way kernel-stamped, never propagated
    // by rfork), so the at-accept snapshot is authoritative. The kernel
    // re-verifies at /cap/use redemption as the load-bearing gate.
    if peer.console == 0 {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }

    // System passphrase verification — v1.0 PLACEHOLDER (byte-compare
    // against SYSTEM_PASSPHRASE; argon2id + AEAD-wrap lands when CRVS
    // persistence does). The check still fails closed for any mismatch.
    if sys_passphrase.len() != SYSTEM_PASSPHRASE.len() {
        return stage_response(response, STATUS_BAD_AUTH, &[]);
    }
    let mut diff: u8 = 0;
    for i in 0..SYSTEM_PASSPHRASE.len() {
        diff |= sys_passphrase[i] ^ SYSTEM_PASSPHRASE[i];
    }
    if diff != 0 {
        return stage_response(response, STATUS_BAD_AUTH, &[]);
    }

    // Register the pending grant for the peer's stripes. The kernel's
    // cap_register_grant_for_writer gate-checks CAP_GRANT_HOSTOWNER (set
    // on corvus by joey's t_spawn_with_perms mask).
    if t_cap_grant(T_CAP_HOSTOWNER, peer.stripes) != 0 {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }

    stage_response(response, STATUS_OK, &[]);
}

// handle_wrap — WRAP verb (verb_id=10). Wraps a 32-byte DEK under the
// session user's hybrid PUBLIC key into a DEK envelope.
//
// Payload format:
//   [0..33)        token         33 B
//   [33]           dataset_len   u8 (1..=MAX_DATASET_LEN)
//   [34..34+dl)    dataset       dl B
//   [34+dl..42+dl) key_id        u64 LE
//   [42+dl..44+dl) dek_len       u16 LE  (must == DEK_LEN)
//   [44+dl..)      dek           dek_len B
unsafe fn handle_wrap(payload: &[u8], response: &mut Vec<u8>) {
    if payload.len() < TOKEN_LEN + 1 + 1 + 8 + 2 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let token = &payload[0..TOKEN_LEN];
    let dataset_len = payload[TOKEN_LEN] as usize;
    if dataset_len == 0 || dataset_len > MAX_DATASET_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let ds_off = TOKEN_LEN + 1;
    if payload.len() < ds_off + dataset_len + 8 + 2 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let dataset = &payload[ds_off..ds_off + dataset_len];
    let kid_off = ds_off + dataset_len;
    let mut key_id: u64 = 0;
    for i in 0..8 {
        key_id |= (payload[kid_off + i] as u64) << (8 * i);
    }
    let dek_len_off = kid_off + 8;
    let dek_len = (payload[dek_len_off] as usize) | ((payload[dek_len_off + 1] as usize) << 8);
    if dek_len != DEK_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let dek_off = dek_len_off + 2;
    if payload.len() != dek_off + dek_len {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let dek_slice = &payload[dek_off..dek_off + dek_len];

    if !session_token_matches(token) {
        return stage_response(response, STATUS_BAD_AUTH, &[]);
    }
    let session_user = match session_user_copy() {
        Some(u) => u,
        None => return stage_response(response, STATUS_BAD_AUTH, &[]),
    };
    let owner = match dataset_owner_find(dataset) {
        Some(o) => o,
        None => return stage_response(response, STATUS_NOT_FOUND, &[]),
    };
    if owner != session_user {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }

    let mut keypair = match session_keypair_copy() {
        Some(kp) => kp,
        None => return stage_response(response, STATUS_INTERNAL_ERROR, &[]),
    };
    let mut x_pk = [0u8; X25519_KEY_LEN];
    x_pk.copy_from_slice(&keypair[KP_X25519_PK_OFF..KP_X25519_PK_OFF + X25519_KEY_LEN]);
    let mut mlkem_ek = [0u8; MLKEM_EK_LEN];
    mlkem_ek.copy_from_slice(&keypair[KP_MLKEM_EK_OFF..KP_MLKEM_EK_OFF + MLKEM_EK_LEN]);
    let mut dek = [0u8; DEK_LEN];
    dek.copy_from_slice(dek_slice);

    let envelope = dek_envelope_wrap(&x_pk, &mlkem_ek, &dek, dataset, key_id);

    wipe(&mut dek);
    wipe(&mut keypair);

    match envelope {
        Some(env) => stage_response(response, STATUS_OK, &env),
        None => stage_response(response, STATUS_INTERNAL_ERROR, &[]),
    }
}

// handle_unwrap — UNWRAP verb (verb_id=4). Recovers a 32-byte DEK from
// a DEK envelope using the session user's hybrid SECRET key.
//
// Payload format:
//   [0..33)        token         33 B
//   [33]           dataset_len   u8
//   [34..34+dl)    dataset       dl B
//   [34+dl..42+dl) key_id        u64 LE
//   [42+dl..44+dl) wrapped_len   u16 LE
//   [44+dl..)      wrapped       wrapped_len B
unsafe fn handle_unwrap(payload: &[u8], response: &mut Vec<u8>) {
    if payload.len() < TOKEN_LEN + 1 + 1 + 8 + 2 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let token = &payload[0..TOKEN_LEN];
    let dataset_len = payload[TOKEN_LEN] as usize;
    if dataset_len == 0 || dataset_len > MAX_DATASET_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let ds_off = TOKEN_LEN + 1;
    if payload.len() < ds_off + dataset_len + 8 + 2 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let dataset = &payload[ds_off..ds_off + dataset_len];
    let kid_off = ds_off + dataset_len;
    let mut key_id: u64 = 0;
    for i in 0..8 {
        key_id |= (payload[kid_off + i] as u64) << (8 * i);
    }
    let wrapped_len_off = kid_off + 8;
    let wrapped_len =
        (payload[wrapped_len_off] as usize) | ((payload[wrapped_len_off + 1] as usize) << 8);
    if wrapped_len == 0 || wrapped_len > MAX_WRAPPED_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let wrapped_off = wrapped_len_off + 2;
    if payload.len() != wrapped_off + wrapped_len {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let wrapped = &payload[wrapped_off..wrapped_off + wrapped_len];
    if wrapped.len() != ENVELOPE_LEN || wrapped[0] != ENVELOPE_VERSION {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }

    if !session_token_matches(token) {
        return stage_response(response, STATUS_BAD_AUTH, &[]);
    }
    let session_user = match session_user_copy() {
        Some(u) => u,
        None => return stage_response(response, STATUS_BAD_AUTH, &[]),
    };
    let owner = match dataset_owner_find(dataset) {
        Some(o) => o,
        None => return stage_response(response, STATUS_NOT_FOUND, &[]),
    };
    if owner != session_user {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }

    let mut keypair = match session_keypair_copy() {
        Some(kp) => kp,
        None => return stage_response(response, STATUS_INTERNAL_ERROR, &[]),
    };
    let mut x_sk = [0u8; X25519_KEY_LEN];
    x_sk.copy_from_slice(&keypair[KP_X25519_SK_OFF..KP_X25519_SK_OFF + X25519_KEY_LEN]);
    let mut mlkem_dk = [0u8; MLKEM_DK_LEN];
    mlkem_dk.copy_from_slice(&keypair[KP_MLKEM_DK_OFF..KP_MLKEM_DK_OFF + MLKEM_DK_LEN]);

    let dek = dek_envelope_unwrap(&x_sk, &mlkem_dk, wrapped, dataset, key_id);

    wipe(&mut x_sk);
    wipe(&mut mlkem_dk);
    wipe(&mut keypair);

    match dek {
        Some(mut d) => {
            stage_response(response, STATUS_OK, &d);
            wipe(&mut d);
        }
        None => stage_response(response, STATUS_INTERNAL_ERROR, &[]),
    }
}

unsafe fn handle_session_close(payload: &[u8], response: &mut Vec<u8>) {
    if payload.len() != TOKEN_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    if !session_token_matches(payload) {
        return stage_response(response, STATUS_NOT_FOUND, &[]);
    }
    session_clear();
    stage_response(response, STATUS_OK, &[]);
}

// =============================================================================
// Per-connection state.
// =============================================================================
//
// Each accepted /srv/corvus connection has its own Conn record. The
// transport buffers (inbound 9P frame assembly + outbound 9P frame
// staging) and the per-conn fid table live on the Conn.
//
// Lifecycle:
//   - Born on t_srv_accept; peer identity captured immediately via
//     t_srv_peer (immutable across the conn's life).
//   - Lives across N 9P Tmsg-Rmsg exchanges. fid table tracks open
//     bindings; verb-frame accumulator + response staging track the
//     ctl-fid request-response state.
//   - Dies on POLLHUP (client disconnect), Twrite-of-bad-protocol-version
//     (Q11 tear-down), or any 9P framing error. close_conn() removes the
//     record + calls t_close; the kernel sets POLLHUP on the peer side
//     in response.

// The 9P fid table per connection. Two production fids at v1.0: root
// (fid 1 from Tattach) and ctl (fid 2 from Twalk). MAX = 8 gives
// headroom for clone-Twalk experiments without bothering with dynamic
// alloc.
const MAX_FIDS_PER_CONN: usize = 8;

#[derive(Copy, Clone)]
struct FidEntry {
    fid: u32,
    qid: p9::Qid,
    opened: bool,
}

// corvus's namespace at v1.0:
//   root: QTDIR, path = QID_ROOT_PATH
//   ctl:  QTFILE, path = QID_CTL_PATH
//
// Paths are stable inode-like identifiers (the 9P qid.path field). The
// client side uses them to detect file identity across Twalk paths.
const QID_ROOT_PATH: u64 = 1;
const QID_CTL_PATH: u64 = 2;

fn root_qid() -> p9::Qid { p9::Qid { kind: p9::P9_QTDIR, version: 0, path: QID_ROOT_PATH } }
fn ctl_qid() -> p9::Qid { p9::Qid { kind: p9::P9_QTFILE, version: 0, path: QID_CTL_PATH } }

// SERVER_MSIZE: negotiated 9P msize. Matches the kernel SrvConn ring
// (kernel/include/thylacine/srvconn.h SRVCONN_MSIZE = 4096). The Tmsg
// buffer + Rmsg buffer are both sized at this. Tversion negotiates
// min(client_msize, SERVER_MSIZE).
const SERVER_MSIZE: u32 = 4096;
const SERVER_MSIZE_USIZE: usize = SERVER_MSIZE as usize;

const P9_VERSION_9P2000_L: &[u8] = b"9P2000.L";

struct Conn {
    handle: i64,
    // Captured at t_srv_accept; carried by-value across the conn's life
    // for corvus.tla's ConnOpIdentityIsKernelTruth. Not yet read at the
    // dispatch layer (v1.0 has no admin verbs that gate on peer identity
    // — those land with ADMIN_ELEVATE / P5-hostowner-b).
    #[allow(dead_code)]
    peer: TSrvPeerInfo,
    // 9P protocol state. version_done + fid_table together implement
    // the conn state machine: Tversion → Tattach(fid 1) → Twalk(fid 2)
    // → Tlopen(fid 2) → Tread/Twrite on fid 2.
    version_done: bool,
    msize: u32,
    fids: [Option<FidEntry>; MAX_FIDS_PER_CONN],

    // I/O buffers.
    in_buf: Vec<u8>,         // accumulated incoming Tmsg bytes
    out_buf: Vec<u8>,        // assembled outgoing Rmsg (size-bound by msize)
    // ctl-fid request/response staging:
    pending_request: Vec<u8>, // Twrite-payload accumulator (decoded to a verb frame)
    pending_response: Vec<u8>,
    pending_response_off: usize,
    // F3 close (P5-corvus-srv-impl audit): when set, the conn is torn
    // down by the server loop after `pending_response` is fully drained
    // by the next Tread. The Q11-violating + oversize-payload paths in
    // `try_dispatch_verb` set this so the BadFormat reply actually
    // reaches the client BEFORE the EOF — matching the
    // STRATUM-API-V1.md Q11 contract that the stream cannot be safely
    // re-synced across a version mismatch.
    tear_down_after_drain: bool,
}

impl Conn {
    fn new(handle: i64, peer: TSrvPeerInfo) -> Self {
        Self {
            handle,
            peer,
            version_done: false,
            msize: SERVER_MSIZE,
            fids: [None; MAX_FIDS_PER_CONN],
            in_buf: Vec::with_capacity(SERVER_MSIZE_USIZE),
            out_buf: Vec::with_capacity(SERVER_MSIZE_USIZE),
            pending_request: Vec::with_capacity(REQ_HDR_LEN + MAX_PAYLOAD_LEN),
            pending_response: Vec::with_capacity(MAX_RESPONSE_FRAME),
            pending_response_off: 0,
            tear_down_after_drain: false,
        }
    }

    fn fid_find(&self, fid: u32) -> Option<usize> {
        for (i, slot) in self.fids.iter().enumerate() {
            if let Some(e) = slot {
                if e.fid == fid {
                    return Some(i);
                }
            }
        }
        None
    }

    fn fid_bind(&mut self, fid: u32, qid: p9::Qid, opened: bool) -> bool {
        // Replace if already bound.
        if let Some(idx) = self.fid_find(fid) {
            self.fids[idx] = Some(FidEntry { fid, qid, opened });
            return true;
        }
        for slot in self.fids.iter_mut() {
            if slot.is_none() {
                *slot = Some(FidEntry { fid, qid, opened });
                return true;
            }
        }
        false
    }

    fn fid_clunk(&mut self, fid: u32) -> bool {
        if let Some(idx) = self.fid_find(fid) {
            self.fids[idx] = None;
            true
        } else {
            false
        }
    }

    fn reset_state(&mut self) {
        // Tversion resets all conn state per 9P2000.L. clear fid table +
        // pending buffers; the next Tattach starts fresh.
        for slot in self.fids.iter_mut() {
            *slot = None;
        }
        self.pending_request.clear();
        self.pending_response.clear();
        self.pending_response_off = 0;
        self.version_done = false;
    }
}

// Drain bytes from `pending_response[pending_response_off..]` into the
// caller's buffer; advance pending_response_off; clear the staged
// response when fully drained.
fn drain_response(conn: &mut Conn, out: &mut [u8], cap: usize) -> usize {
    let avail = conn.pending_response.len().saturating_sub(conn.pending_response_off);
    let n = if cap < avail { cap } else { avail };
    if n == 0 {
        return 0;
    }
    out[..n].copy_from_slice(
        &conn.pending_response[conn.pending_response_off..conn.pending_response_off + n],
    );
    conn.pending_response_off += n;
    if conn.pending_response_off >= conn.pending_response.len() {
        conn.pending_response.clear();
        conn.pending_response_off = 0;
    }
    n
}

// Attempt to extract complete verb frames from pending_request and
// dispatch them; each dispatch stages a response (overwriting any
// already-pending response — joey's strict request-response means this
// never collides in practice, and a confused client gets the latest
// response).
unsafe fn try_dispatch_verb(conn: &mut Conn) {
    while conn.pending_request.len() >= REQ_HDR_LEN {
        let verb_id = conn.pending_request[0];
        let protocol_version = conn.pending_request[1];
        let payload_len = (conn.pending_request[2] as usize)
            | ((conn.pending_request[3] as usize) << 8);

        if protocol_version != CORVUS_PROTOCOL_VERSION {
            // Q11 close (P5-corvus-srv-impl audit F3): unknown wire
            // version → BadFormat then ACTUALLY tear down. The flag is
            // checked in `service_conn` after the next Tread drains
            // `pending_response` — so the client observes the
            // BadFormat reply BEFORE the EOF. Pre-audit this only
            // staged the reply and relied on the client to disconnect,
            // which the joey test happened to do but the wire contract
            // (STRATUM-API-V1.md Q11: "stream cannot be safely
            // re-synced across a version mismatch") explicitly forbids.
            stage_response(&mut conn.pending_response, STATUS_BAD_FORMAT, &[]);
            conn.pending_response_off = 0;
            conn.pending_request.clear();
            conn.tear_down_after_drain = true;
            return;
        }
        if payload_len > MAX_PAYLOAD_LEN {
            // Frame oversize — same fail-stop discipline as the Q11 path:
            // the body length can no longer be re-synced safely, so tear
            // down after delivering the BadFormat reply.
            stage_response(&mut conn.pending_response, STATUS_BAD_FORMAT, &[]);
            conn.pending_response_off = 0;
            conn.pending_request.clear();
            conn.tear_down_after_drain = true;
            return;
        }
        if conn.pending_request.len() < REQ_HDR_LEN + payload_len {
            // Incomplete frame; wait for more bytes.
            return;
        }

        // Have a full frame. Dispatch.
        // SAFETY: copy the payload out before touching pending_request
        // again (handlers don't mutate pending_request but we re-enter
        // the loop after, so be defensive).
        let payload = &conn.pending_request[REQ_HDR_LEN..REQ_HDR_LEN + payload_len];
        let mut payload_owned = Vec::with_capacity(payload.len());
        payload_owned.extend_from_slice(payload);

        // Reset response staging before dispatch.
        conn.pending_response.clear();
        conn.pending_response_off = 0;

        // Snapshot the bits the gated verbs need before re-borrowing
        // conn.pending_response. The handle is a primitive; the peer
        // snapshot is Copy (TSrvPeerInfo is repr-C with primitive
        // fields). C-22: peer_live_caps inside the gated handlers does
        // the fresh SYS_SRV_PEER query — these are just stable inputs
        // (immutable identity + the handle for re-query).
        let conn_handle = conn.handle;
        let conn_peer = conn.peer;

        match verb_id {
            VERB_AUTH => handle_auth(&payload_owned, &mut conn.pending_response),
            VERB_SESSION_CLOSE => handle_session_close(&payload_owned, &mut conn.pending_response),
            VERB_UNWRAP => handle_unwrap(&payload_owned, &mut conn.pending_response),
            VERB_USER_CREATE => handle_user_create(conn_handle, &payload_owned,
                                                   &mut conn.pending_response),
            VERB_ADMIN_ELEVATE => handle_admin_elevate(&conn_peer, &payload_owned,
                                                       &mut conn.pending_response),
            VERB_WRAP => handle_wrap(&payload_owned, &mut conn.pending_response),
            _ => stage_response(&mut conn.pending_response, STATUS_BAD_FORMAT, &[]),
        }

        // Wipe the payload buffer (verb payloads carry secrets).
        wipe(&mut payload_owned);

        // Drain the consumed frame from pending_request.
        let consumed = REQ_HDR_LEN + payload_len;
        conn.pending_request.drain(..consumed);

        // Process at most one frame per Twrite at v1.0 (strict request-
        // response; a queued second request before the first response
        // drains is a client protocol error — let the loop handle it).
        return;
    }
}

// =============================================================================
// 9P message dispatch.
//
// dispatch_one parses one Tmsg from `tmsg` (a slice of exactly one
// framed message), produces an Rmsg into `conn.out_buf`, and returns
// the Rmsg byte count. If the Tmsg is malformed in any way, an Rlerror
// is emitted in its place. dispatch_one never tears down the conn; the
// caller decides on tear-down based on the Rmsg type (Rlerror is a
// continuation, but a BadFormat response staged via try_dispatch_verb
// signals tear-down to the caller).
// =============================================================================

// Returns the byte length of the Rmsg written into conn.out_buf; 0
// indicates no Rmsg (Tmsg parse-error so bad we couldn't even extract
// the tag; conn should be torn down).
unsafe fn dispatch_one(conn: &mut Conn, tmsg: &[u8]) -> usize {
    let hdr = match p9::peek_header(tmsg) {
        Ok(h) => h,
        Err(_) => return 0,
    };
    let tag = hdr.tag;
    let cap = conn.out_buf.capacity();
    conn.out_buf.clear();
    conn.out_buf.resize(cap, 0);

    let result = match hdr.mtype {
        p9::P9_TVERSION => dispatch_tversion(conn, tmsg, tag),
        p9::P9_TAUTH => Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_OPNOTSUPP).unwrap_or(0)),
        p9::P9_TATTACH => dispatch_tattach(conn, tmsg, tag),
        p9::P9_TWALK => dispatch_twalk(conn, tmsg, tag),
        p9::P9_TLOPEN => dispatch_tlopen(conn, tmsg, tag),
        p9::P9_TREAD => dispatch_tread(conn, tmsg, tag),
        p9::P9_TWRITE => dispatch_twrite(conn, tmsg, tag),
        p9::P9_TCLUNK => dispatch_tclunk(conn, tmsg, tag),
        _ => Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_NOSYS).unwrap_or(0)),
    };
    match result {
        Ok(n) => {
            conn.out_buf.truncate(n);
            n
        }
        Err(_) => {
            // build_r* failed (out_buf too small? shouldn't happen). Emit
            // an Rlerror in a fresh attempt; if that also fails the conn
            // must die.
            conn.out_buf.clear();
            conn.out_buf.resize(cap, 0);
            let n = p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO).unwrap_or(0);
            conn.out_buf.truncate(n);
            n
        }
    }
}

fn dispatch_tversion(conn: &mut Conn, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
    let args = match p9::parse_tversion(tmsg) {
        Ok(a) => a,
        Err(_) => return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO)?),
    };
    let negotiated = if args.msize < SERVER_MSIZE { args.msize } else { SERVER_MSIZE };
    let version_out: &[u8] = if args.version == P9_VERSION_9P2000_L {
        P9_VERSION_9P2000_L
    } else {
        // Per spec: server returns "unknown" if dialect not supported.
        // corvus only speaks 9P2000.L; the client should immediately
        // fail rather than continue.
        b"unknown"
    };
    conn.reset_state();
    conn.msize = negotiated;
    if version_out == P9_VERSION_9P2000_L {
        conn.version_done = true;
    }
    p9::build_rversion(&mut conn.out_buf, tag, negotiated, version_out)
}

fn dispatch_tattach(conn: &mut Conn, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
    if !conn.version_done {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO)?);
    }
    let args = match p9::parse_tattach(tmsg) {
        Ok(a) => a,
        Err(_) => return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO)?),
    };
    if args.afid != p9::P9_NOFID {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_OPNOTSUPP)?);
    }
    if !conn.fid_bind(args.fid, root_qid(), false) {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_NOMEM)?);
    }
    p9::build_rattach(&mut conn.out_buf, tag, &root_qid())
}

fn dispatch_twalk(conn: &mut Conn, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
    let args = match p9::parse_twalk(tmsg) {
        Ok(a) => a,
        Err(_) => return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO)?),
    };
    let src_idx = match conn.fid_find(args.fid) {
        Some(i) => i,
        None => return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_BADF)?),
    };
    let src_qid = conn.fids[src_idx].as_ref().unwrap().qid;
    // If src is opened, walking from it is illegal (9P2000.L spec).
    if conn.fids[src_idx].as_ref().unwrap().opened {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO)?);
    }
    // newfid binding rules: if newfid == fid, replace src binding with
    // destination on success. If newfid != fid, newfid must not be in
    // use; per spec violations are EINVAL.
    if args.newfid != args.fid && conn.fid_find(args.newfid).is_some() {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_INVAL)?);
    }

    if args.nwname == 0 {
        // Clone: bind newfid to src's qid (or no-op if newfid == fid).
        if !conn.fid_bind(args.newfid, src_qid, false) {
            return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_NOMEM)?);
        }
        return p9::build_rwalk(&mut conn.out_buf, tag, &[]);
    }

    // v1.0 namespace: root → ctl. Only walks rooted at the root qid
    // are recognized; any other walk fails on component 0 (Rlerror
    // ENOENT — no qids walked at all).
    if src_qid.path != QID_ROOT_PATH {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_NOENT)?);
    }
    // Single-component walks only at v1.0 (corvus's namespace has no
    // hierarchy below root). Multi-component requests fail on
    // component 1.
    if args.nwname != 1 {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_NOENT)?);
    }
    let name = args.names[0];
    if name != b"ctl" {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_NOENT)?);
    }
    if !conn.fid_bind(args.newfid, ctl_qid(), false) {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_NOMEM)?);
    }
    let qids = [ctl_qid()];
    p9::build_rwalk(&mut conn.out_buf, tag, &qids)
}

fn dispatch_tlopen(conn: &mut Conn, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
    let args = match p9::parse_tlopen(tmsg) {
        Ok(a) => a,
        Err(_) => return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO)?),
    };
    let idx = match conn.fid_find(args.fid) {
        Some(i) => i,
        None => return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_BADF)?),
    };
    let entry = conn.fids[idx].as_ref().unwrap();
    if entry.opened {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO)?);
    }
    let qid = entry.qid;
    // Only ctl is openable at v1.0; the root QTDIR isn't (no readdir).
    if qid.kind != p9::P9_QTFILE || qid.path != QID_CTL_PATH {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_ISDIR)?);
    }
    // flags ignored at v1.0 — ctl is unconditionally RDWR.
    let _ = args.flags;
    conn.fids[idx] = Some(FidEntry { fid: args.fid, qid, opened: true });
    p9::build_rlopen(&mut conn.out_buf, tag, &qid, 0)
}

fn dispatch_tread(conn: &mut Conn, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
    let args = match p9::parse_tread(tmsg) {
        Ok(a) => a,
        Err(_) => return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO)?),
    };
    let idx = match conn.fid_find(args.fid) {
        Some(i) => i,
        None => return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_BADF)?),
    };
    let entry = conn.fids[idx].as_ref().unwrap();
    if !entry.opened {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO)?);
    }
    if entry.qid.path != QID_CTL_PATH {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_ISDIR)?);
    }
    // Drain from pending_response. The kernel-side offset (args.offset)
    // is ignored — ctl is message-oriented; corvus tracks its own
    // drain offset across the same staged response.
    let _ = args.offset;
    let cap_msize = (conn.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4);
    let count_cap = (args.count as usize).min(cap_msize);
    let mut tmp = [0u8; SERVER_MSIZE_USIZE];
    let n = drain_response(conn, &mut tmp[..count_cap], count_cap);
    p9::build_rread(&mut conn.out_buf, tag, &tmp[..n])
}

unsafe fn dispatch_twrite(conn: &mut Conn, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
    let args = match p9::parse_twrite(tmsg) {
        Ok(a) => a,
        Err(_) => return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO)?),
    };
    let idx = match conn.fid_find(args.fid) {
        Some(i) => i,
        None => return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_BADF)?),
    };
    let entry = conn.fids[idx].as_ref().unwrap();
    if !entry.opened {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO)?);
    }
    if entry.qid.path != QID_CTL_PATH {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_ISDIR)?);
    }
    // Accept all bytes; bound them by msize.
    let cap_msize = (conn.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4 + 4 + 8);
    if args.data.len() > cap_msize {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_INVAL)?);
    }
    // Append to verb-frame accumulator; bound the total at a sane cap.
    // A pathological client could otherwise grow the accumulator without
    // ever sending a header — cap at 2 * (REQ_HDR_LEN + MAX_PAYLOAD_LEN).
    // F9 close (P5-corvus-srv-impl audit): on overflow, reset the
    // accumulator (drop the garbage) so a subsequent well-formed
    // Twrite can recover; without the reset the conn was wedged on
    // every future Twrite.
    if conn.pending_request.len() + args.data.len()
        > 2 * (REQ_HDR_LEN + MAX_PAYLOAD_LEN)
    {
        conn.pending_request.clear();
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_INVAL)?);
    }
    conn.pending_request.extend_from_slice(args.data);
    let accepted = args.data.len() as u32;
    try_dispatch_verb(conn);
    p9::build_rwrite(&mut conn.out_buf, tag, accepted)
}

fn dispatch_tclunk(conn: &mut Conn, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
    let args = match p9::parse_tclunk(tmsg) {
        Ok(a) => a,
        Err(_) => return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO)?),
    };
    if !conn.fid_clunk(args.fid) {
        return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_BADF)?);
    }
    p9::build_rclunk(&mut conn.out_buf, tag)
}

// =============================================================================
// Per-conn service step — try to read one or more 9P Tmsgs and process.
//
// Returns Ok(true) if the conn is still healthy; Ok(false) if the conn
// should be torn down; Err(()) is unused (kept for future structured
// errors).
// =============================================================================

unsafe fn service_conn(conn: &mut Conn) -> Result<bool, ()> {
    // Read whatever's currently available into in_buf. A single t_read
    // returns up to 4 KiB at v1.0 (kernel SYS_RW_MAX).
    let cur_len = conn.in_buf.len();
    let need = SERVER_MSIZE_USIZE - cur_len;
    if need == 0 {
        // Buffer is full; one Tmsg must fit. Try to parse without
        // reading more — if parse-and-dispatch consumes bytes, in_buf
        // shrinks and the next iteration reads.
    } else {
        conn.in_buf.resize(cur_len + need, 0);
        let n = t_read(
            conn.handle,
            conn.in_buf.as_mut_ptr().add(cur_len),
            need,
        );
        if n < 0 {
            return Ok(false);
        }
        let n = n as usize;
        conn.in_buf.truncate(cur_len + n);
        if n == 0 {
            // EOF — conn closed by peer.
            return Ok(false);
        }
    }

    // Drain Tmsgs while complete frames are available.
    loop {
        if conn.in_buf.len() < p9::P9_HDR_LEN {
            return Ok(true);
        }
        let hdr = match p9::peek_header(&conn.in_buf) {
            Ok(h) => h,
            Err(_) => return Ok(true),
        };
        let size = hdr.size as usize;
        if size < p9::P9_HDR_LEN || size > SERVER_MSIZE_USIZE {
            // Framing violation — tear down.
            return Ok(false);
        }
        if conn.in_buf.len() < size {
            return Ok(true);
        }

        // Process one Tmsg.
        let tmsg_bytes: Vec<u8> = conn.in_buf[..size].to_vec();
        let rmsg_len = dispatch_one(conn, &tmsg_bytes);
        if rmsg_len == 0 {
            return Ok(false);
        }
        // Write the Rmsg.
        let rmsg_bytes: Vec<u8> = conn.out_buf[..rmsg_len].to_vec();
        let mut sent: usize = 0;
        while sent < rmsg_bytes.len() {
            let n = t_write(
                conn.handle,
                rmsg_bytes.as_ptr().add(sent),
                rmsg_bytes.len() - sent,
            );
            if n <= 0 {
                return Ok(false);
            }
            sent += n as usize;
        }

        // Remove the consumed Tmsg from in_buf.
        conn.in_buf.drain(..size);

        // F3 close (P5-corvus-srv-impl audit): the conn is in fail-stop
        // state — try_dispatch_verb staged a Q11 (or oversize-payload)
        // BadFormat reply and set `tear_down_after_drain`. Tear down
        // once the staged reply has been fully drained by Treads;
        // `pending_response` becomes empty after the last Tread on the
        // ctl fid. The reply is delivered to the client BEFORE the
        // tear-down EOF, matching STRATUM-API-V1.md Q11.
        if conn.tear_down_after_drain
            && conn.pending_response.is_empty()
        {
            return Ok(false);
        }
    }
}

// =============================================================================
// Server loop.
// =============================================================================

const MAX_CONNS: usize = 8;

unsafe fn srv_server_loop(listener: i64) -> i64 {
    let mut conns: Vec<Conn> = Vec::with_capacity(MAX_CONNS);

    // Bounded poll-fd buffer: [listener] + per-conn (max MAX_CONNS).
    let mut pollfds: [TPollFd; 1 + MAX_CONNS] =
        [TPollFd { fd: 0, events: 0, revents: 0 }; 1 + MAX_CONNS];

    loop {
        // Build pollfd list.
        pollfds[0] = TPollFd { fd: listener as i32, events: T_POLLIN, revents: 0 };
        let nfds = 1 + conns.len();
        for (i, c) in conns.iter().enumerate() {
            pollfds[1 + i] = TPollFd { fd: c.handle as i32, events: T_POLLIN, revents: 0 };
        }

        let rc = t_poll(pollfds.as_mut_ptr(), nfds, -1);
        if rc < 0 {
            t_putstr("corvus: t_poll failed\n");
            return -1;
        }

        // Accept new connections.
        if pollfds[0].revents & T_POLLIN != 0 {
            if conns.len() < MAX_CONNS {
                let h = t_srv_accept(listener);
                if h >= 0 {
                    let mut peer = TSrvPeerInfo::default();
                    // F4 close (P5-corvus-srv-impl audit): fail-closed
                    // on a non-zero `t_srv_peer` rc. The pre-audit code
                    // discarded the return and left `peer` at zeros —
                    // future admin verbs (P5-hostowner-b) that read
                    // `caps`/`stripes` for gating would have aliased a
                    // failed-read with a fail-closed value rather than
                    // explicitly catching it. Better to refuse the
                    // accept now than to admit an unknown-identity
                    // conn into the loop. The handle is closed so the
                    // kernel-side SrvConn tears down promptly.
                    if t_srv_peer(h, &mut peer) != 0 {
                        let _ = t_close(h);
                        // Continue — the kernel's tear-down on
                        // `t_close` will surface to a subsequent
                        // listener-side state; corvus's loop will
                        // re-poll.
                    } else {
                        conns.push(Conn::new(h, peer));
                    }
                }
                // else: listener torn down; loop will pick it up via
                // POLLHUP on the listener (T_POLLHUP isn't requested
                // here — at v1.0 corvus only ever sees POLLIN on the
                // listener).
            } else {
                // Backlog full; defer accept (the listener's accept
                // backlog absorbs up to SRV_ACCEPT_BACKLOG = 16 per
                // kernel/devsrv.c). Continue serving extant conns; the
                // backlog will drain as conns close.
            }
        }
        if pollfds[0].revents & T_POLLHUP != 0 {
            // Listener torn down (admin/proc exit / unposted). Drain
            // existing conns and exit.
            t_putstr("corvus: listener POLLHUP\n");
            break;
        }

        // Service ready conns. Iterate from the end so removal doesn't
        // invalidate indices.
        let mut i = conns.len();
        while i > 0 {
            i -= 1;
            let pf = pollfds[1 + i];
            let mut should_close = false;
            if pf.revents & T_POLLIN != 0 {
                match service_conn(&mut conns[i]) {
                    Ok(true) => {}
                    Ok(false) => should_close = true,
                    Err(_) => should_close = true,
                }
            }
            if pf.revents & T_POLLHUP != 0 {
                should_close = true;
            }
            if should_close {
                close_conn(&mut conns, i);
            }
        }
    }

    // Drain extant conns on shutdown.
    while !conns.is_empty() {
        let last = conns.len() - 1;
        close_conn(&mut conns, last);
    }
    0
}

unsafe fn close_conn(conns: &mut Vec<Conn>, idx: usize) {
    let conn = &mut conns[idx];
    // Wipe per-conn buffers (request/response carry secrets — verb
    // payloads + token + DEK envelopes).
    wipe(&mut conn.in_buf);
    wipe(&mut conn.out_buf);
    wipe(&mut conn.pending_request);
    wipe(&mut conn.pending_response);
    // Clear the global session — at v1.0 only one client at a time,
    // so a closed conn implies the session (if any) is orphaned.
    session_clear();
    let _ = t_close(conn.handle);
    conns.remove(idx);
}

// =============================================================================
// Startup hardening (unchanged from -d).
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

// A-1.7 capability-scoped service storage (ARCH §3.6; NOVEL §3.10; I-23).
// Runs AFTER corvus has chrooted to its handed storage capability, so
// `FROM_ROOT` now resolves to the storage subtree. Proves read/write
// WITHIN the capability and that a path which exists ABOVE it (the
// Stratum-root sentinel `/thylacine-version`) is unreachable -- i.e. the
// chroot actually confines. Idempotent across reboots: on a persistent
// pool the probe file survives, so a failed create (EEXIST) falls through
// to read-verify the persisted content (which also proves persistence).
unsafe fn corvus_cap_smoke() -> bool {
    let name = b"cap-smoke";
    let payload = b"corvus-cap-v1";

    let cf = t_walk_create(T_WALK_OPEN_FROM_ROOT, name.as_ptr(), name.len(), T_OWRITE, 0o600);
    if cf >= 0 {
        let w = t_write(cf, payload.as_ptr(), payload.len());
        let _ = t_fsync(cf, 0);
        let _ = t_close(cf);
        if w != payload.len() as i64 {
            return false;
        }
    }
    // else: the probe file already exists from a prior boot -- the
    // capability persisted; fall through to read-verify it.

    let rf = t_walk_open(T_WALK_OPEN_FROM_ROOT, name.as_ptr(), name.len(), T_OREAD);
    if rf < 0 {
        return false;
    }
    let mut buf = [0u8; 16];
    let r = t_read(rf, buf.as_mut_ptr(), buf.len());
    let _ = t_close(rf);
    if r != payload.len() as i64 || buf[..payload.len()] != payload[..] {
        return false;
    }

    // Confinement: the Stratum-root sentinel exists ABOVE the capability
    // and MUST be unreachable post-chroot. If it opens, corvus is not
    // confined -- fail the smoke.
    let sentinel = b"thylacine-version";
    let esc = t_walk_open(T_WALK_OPEN_FROM_ROOT, sentinel.as_ptr(), sentinel.len(), T_OREAD);
    if esc >= 0 {
        let _ = t_close(esc);
        return false;
    }
    true
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("corvus: starting (P5-corvus-srv-impl-b3b)\n");

    // A-1.7 (F2): confine to the handed storage-root capability (fd 0) as the
    // FIRST action -- before anything else -- so there is no ambient-FS window.
    // corvus inherits joey's broad Stratum root via territory_clone; this chroot
    // displaces it (territory_chroot spoor_clunks the old root, spoor_refs the
    // cap), making corvus's filesystem world the capability (I-23). joey ALWAYS
    // hands the capability now, so a missing/invalid fd 0 is a fatal boot error,
    // not a fallback. chroot is a raw syscall -- no heap needed yet.
    unsafe {
        if t_chroot(0) != 0 {
            t_putstr("corvus: FATAL no storage capability at fd 0\n");
            libthyla_rs::t_exits(1);
        }
    }

    unsafe {
        heap_init();
        user_states_init();
        dataset_owners_init();
    }

    let rc = unsafe { t_mlockall(0) };
    if rc != 0 { step_fail(1, rc); }
    let rc = unsafe { t_set_dumpable(0) };
    if rc != 0 { step_fail(2, rc); }
    let rc = unsafe { t_set_traceable(0) };
    if rc != 0 { step_fail(3, rc); }
    let mut probe: [u8; PROBE_LEN] = [0; PROBE_LEN];
    let rc = unsafe { t_getrandom(probe.as_mut_ptr(), PROBE_LEN, 0) };
    if rc != PROBE_LEN as i64 { step_fail(4, rc); }
    let rc = unsafe { t_explicit_bzero(probe.as_mut_ptr(), PROBE_LEN) };
    if rc != 0 { step_fail(5, rc); }

    // Post /srv/corvus. Requires PROC_FLAG_MAY_POST_SERVICE (joey grants
    // it via SYS_SPAWN_WITH_PERMS at spawn time per P5-corvus-srv-impl-
    // b3a).
    let listener = unsafe { t_post_service(b"corvus".as_ptr(), 6) };
    if listener < 0 { step_fail(6, listener); }

    t_putstr("corvus: ready (hardening applied; serving /srv/corvus)\n");

    // A-1.7: corvus is already chroot'd to its storage capability (the first
    // action in rs_main, F2). Prove confinement: create/read WITHIN the
    // capability + assert a path ABOVE it is unreachable post-chroot.
    unsafe {
        if corvus_cap_smoke() {
            t_putstr("corvus: storage capability OK (confined; /thylacine-version unreachable)\n");
        } else {
            t_putstr("corvus: storage capability smoke FAILED\n");
            return 1;
        }
    }

    let rc = unsafe { srv_server_loop(listener) };
    if rc < 0 {
        t_putstr("corvus: srv_server_loop FAILED\n");
        return 1;
    }

    unsafe { session_clear() };
    let _ = unsafe { t_close(listener) };
    t_putstr("corvus: srv_server_loop returned cleanly; shutting down\n");
    0
}
