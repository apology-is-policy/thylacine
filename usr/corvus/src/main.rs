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

// The crypto core (CRVS wrap layout + Argon2id + AEGIS-256 + the hybrid keypair
// + the DEK envelope + the BIP-39 codec + the recovery/passphrase wraps) lives
// in the shared corvus-crypto lib (A-5c-b) so the host corvus-mint and corvus
// produce byte-identical wraps. corvus supplies the RNG (ThylaRng) + the heap.
use corvus_crypto::*;

// 9P2000.L server-side codec. Lifted from corvus's private `p9` module
// into `libthyla_rs::ninep` at U-2h-ninep; aliased back to `p9` so the
// existing dispatcher references survive byte-identical.
use libthyla_rs::ninep as p9;

use libthyla_rs::{
    t_cap_grant, t_cap_grant_clearance, t_cap_grant_imperium, t_chroot, t_close,
    t_console_episode, t_console_open, t_explicit_bzero, t_fsync, t_getrandom, t_mlockall,
    t_open, t_poll, t_putstr, t_read, t_rename, t_set_dumpable, t_set_traceable, t_srv_accept,
    t_srv_peer, t_unlink, t_walk_create, t_walk_open, t_write, TPollFd, TSrvPeerInfo,
    T_CAP_AUDIO_GRAPH, T_CAP_CHOWN, T_CAP_DAC_OVERRIDE, T_CAP_DEBUG, T_CAP_GRANT_FLAG_PROPAGATING,
    T_CAP_HOSTOWNER, T_CAP_JIT, T_CAP_KILL, T_CONSOLE_EPISODE_ARM, T_CONSOLE_EPISODE_END, T_OPATH,
    T_OREAD, T_OWRITE, T_POLLERR, T_POLLHUP, T_POLLIN, T_POLLNVAL, T_WALK_CREATE_DMDIR,
    T_WALK_OPEN_FROM_ROOT,
};
use libthyla_rs::notes::Notes;
use libthyla_rs::poll::AsFd;
use libthyla_rs::time::monotonic_ns;

use alloc::vec::Vec;

// IM-3: the trusted-path composer (the provincia cell grid + its serial
// rasterizer; TRUSTED-PATH.md 7, ratified fork F1).
mod provincia;

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

// A-1b: a user record carries BOTH identity (principal_id / primary_gid /
// supp_gids / backend / name) and wrap-state (argon2 params + salt + nonce +
// ciphertext + tag). The identity half persists in the central CRVS-v2
// identity.db; the wrap half persists in the per-user CRVS-v1 hybrid.corvus
// (the `to_bytes` blob). Loaded back from both at boot (CORVUS-DESIGN.md §16.2).
#[derive(Clone)]
struct CorvusUserState {
    user: Vec<u8>,
    principal_id: u32,
    primary_gid: u32,
    supp_gids: Vec<u32>,
    backend: u8,
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
        build_passphrase_ad(&self.user, ad_out);
    }

    // CRVS v1 per-user keypair wrap (the at-rest AEGIS-wrapped hybrid keypair).
    // Delegates to corvus_crypto's KeypairWrap serializer -- the single CRVS v1
    // packer shared with the recovery wrap (RecoveryWrap) and the A-5c-b
    // host-minted system-wrap, so the three on-disk forms cannot drift. The blob
    // is CIPHERTEXT, decryptable only with the passphrase-derived KEK.
    fn to_bytes(&self) -> [u8; TOTAL_LEN] {
        KeypairWrap {
            t_cost: self.t_cost,
            m_cost_kib: self.m_cost_kib,
            parallelism: self.parallelism,
            salt: self.salt,
            nonce: self.nonce,
            ciphertext: self.ciphertext,
            tag: self.tag,
        }
        .to_bytes()
    }

    // Parse a CRVS v1 hybrid.corvus blob into the wrap-state fields of a record
    // whose identity fields are already filled from identity.db. Fail-closed:
    // magic/version/length mismatch -> false (the user is then dropped from the
    // live table; CORVUS-DESIGN.md section 16.5 step 4).
    fn fill_wrap_from_bytes(&mut self, blob: &[u8]) -> bool {
        let kw = match KeypairWrap::from_bytes(blob) {
            Some(k) => k,
            None => return false,
        };
        self.t_cost = kw.t_cost;
        self.m_cost_kib = kw.m_cost_kib;
        self.parallelism = kw.parallelism;
        self.salt = kw.salt;
        self.nonce = kw.nonce;
        self.ciphertext = kw.ciphertext;
        self.tag = kw.tag;
        true
    }
}

// In-memory rate limit on the EXPENSIVE RECOVER path. A typo'd phrase fails the
// BIP-39 checksum first (cheap, before the KDF) and does NOT count here; only a
// checksum-VALID phrase that then fails the AEAD unwrap (a crafted/guessed
// attempt) is counted. After RECOVER_FAIL_MAX such failures for a subject,
// further RECOVER for that subject is refused until corvus restarts -- bounding
// the unauthenticated argon2 DoS surface without a wall clock. A legitimate
// holder's real phrase passes both checksum and unwrap on the first try, so this
// never locks them out. The full time-windowed, Stratum-persisted C-16 rate
// limit (covering AUTH too) is tracked separately.
const RECOVER_FAIL_MAX: u32 = 5;

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

// Boot-time self-test of the recovery codec + wrap layout (A-5c). FAST -- no
// argon2 (a fixed test KEK exercises the AEAD + AD + CRVS layout); the argon2-
// backed full RECOVER round-trip is the A-5c-c boot E2E. Validates: BIP-39
// round-trip; a deterministic wrong-checksum reject; an unknown-word reject; the
// recovery-wrap byte round-trip; AD domain-separation (a foreign subject's AD
// and the passphrase-wrap AD both fail to open it). Returns true on PASS.
unsafe fn recovery_selftest() -> bool {
    let mut ent = [0u8; RECOVERY_ENTROPY_BYTES];
    for i in 0..RECOVERY_ENTROPY_BYTES {
        ent[i] = i as u8;
    }

    // 1. BIP-39 round-trip.
    let phrase = bip39_encode(&ent);
    let nwords = phrase
        .split(|&c| c == b' ')
        .filter(|w| !w.is_empty())
        .count();
    if nwords != RECOVERY_WORD_COUNT {
        return false;
    }
    match bip39_decode(&phrase) {
        Some(d) if d == ent => {}
        _ => return false,
    }

    // 2. Deterministic checksum-reject: 24 valid words whose checksum byte is
    //    flipped -> decode must reject.
    let mut badbits = [0u8; RECOVERY_ENTROPY_BYTES + 1];
    badbits[..RECOVERY_ENTROPY_BYTES].copy_from_slice(&ent);
    badbits[RECOVERY_ENTROPY_BYTES] = sha256_checksum_byte(&ent) ^ 0xff;
    let badphrase = bip39_words_from_bits(&badbits);
    if bip39_decode(&badphrase).is_some() {
        return false;
    }

    // 3. Unknown-word reject (a non-wordlist token).
    let mut unk: Vec<u8> = Vec::new();
    for i in 0..RECOVERY_WORD_COUNT {
        if i > 0 {
            unk.push(b' ');
        }
        if i == RECOVERY_WORD_COUNT - 1 {
            unk.extend_from_slice(b"notaword");
        } else {
            unk.extend_from_slice(b"abandon");
        }
    }
    if bip39_decode(&unk).is_some() {
        return false;
    }

    // 4. Recovery-wrap byte round-trip + AD separation, with a FIXED test KEK.
    let test_key = [0x5au8; AEGIS256_KEY_LEN];
    let test_nonce = [0xa5u8; AEGIS256_NONCE_LEN];
    let subject = b"selftest";
    let mut keypair = [0u8; KEYPAIR_LEN];
    for i in 0..KEYPAIR_LEN {
        keypair[i] = (i & 0xff) as u8;
    }
    let mut ad: Vec<u8> = Vec::new();
    build_recovery_ad(subject, &mut ad);
    let mut ct = [0u8; KEYPAIR_LEN];
    let tag = aegis_wrap(&test_key, &test_nonce, &ad, &keypair, &mut ct);
    let rw = RecoveryWrap {
        t_cost: 1,
        m_cost_kib: 8,
        parallelism: 1,
        salt: [0u8; ARGON2_SALT_LEN],
        nonce: test_nonce,
        ciphertext: ct,
        tag,
    };
    let blob = rw.to_bytes();
    let rw2 = match RecoveryWrap::from_bytes(&blob) {
        Some(r) => r,
        None => return false,
    };
    match aegis_unwrap(&test_key, &rw2.nonce, &ad, &rw2.ciphertext, &rw2.tag) {
        Some(o) if o.as_slice() == &keypair[..] => {}
        _ => return false,
    }
    // Foreign-subject AD must fail.
    let mut ad_other: Vec<u8> = Vec::new();
    build_recovery_ad(b"other", &mut ad_other);
    if aegis_unwrap(&test_key, &rw2.nonce, &ad_other, &rw2.ciphertext, &rw2.tag).is_some() {
        return false;
    }
    // Passphrase-wrap AD (same subject, different prefix) must fail.
    let mut ad_pass: Vec<u8> = Vec::new();
    ad_pass.extend_from_slice(AD_PREFIX);
    ad_pass.extend_from_slice(subject);
    ad_pass.push(BACKEND_ID_PASSPHRASE);
    if aegis_unwrap(&test_key, &rw2.nonce, &ad_pass, &rw2.ciphertext, &rw2.tag).is_some() {
        return false;
    }
    wipe(&mut keypair);
    wipe(&mut ent);
    true
}

// =============================================================================
// User state vector + dataset ownership table — in-memory at v1.0.
// =============================================================================

// =============================================================================
// Identity model — id <-> name <-> groups (A-1b; CORVUS-DESIGN.md §16).
// =============================================================================
//
// corvus is the authoritative resolver. `next_auto_id` is ONE monotonic
// counter shared between principal_ids and gids (the Red Hat UPG scheme), so a
// user-private group's uid == gid is collision-free. Reserved values are never
// assigned (the counter starts at 1000 and only increments). An assigned id
// confers no ambient authority (I-22 / C-25).

const FIRST_AUTO_ID: u32 = 1000;
const PRINCIPAL_INVALID: u32 = 0;
const PRINCIPAL_SYSTEM: u32 = 0xFFFF_FFFE;
const PRINCIPAL_NONE: u32 = 0xFFFF_FFFF;
const PROC_SUPP_GIDS_MAX: usize = 15;
const MAX_GROUP_LEN: usize = 32;

// CRVS v2 identity.db — central, NON-secret id<->name<->group map.
const IDENTITY_DB_VERSION: u32 = 2;
const IDENTITY_DB_HEADER_LEN: usize = 20;
const _: () = assert!(IDENTITY_DB_HEADER_LEN == 20, "identity.db header layout drift");
const _: () = assert!(PROC_SUPP_GIDS_MAX == 15, "supp_gids cap drift (kernel PROC_SUPP_GIDS_MAX)");
// alloc_auto_id rejects everything >= PRINCIPAL_SYSTEM, which covers BOTH
// reserved sentinels; this pins the ordering so a single `>=` stays sufficient.
const _: () = assert!(PRINCIPAL_NONE > PRINCIPAL_SYSTEM, "reserved-id ordering drift");
const _: () = assert!(PRINCIPAL_SYSTEM > FIRST_AUTO_ID, "reserved-id ordering drift");

// The storage-capability fd handed by joey at spawn (fd 0). corvus chroots to
// it (rs_main, A-1.7), and it remains a valid R|W non-opened handle to corvus's
// root directory afterward (SYS_CHROOT borrows the fd; it is not consumed) --
// used as the directory fd for the dirent-durability fsync barrier (§16.6).
const STORAGE_ROOT_FD: i64 = 0;

// One monotonic id allocator shared between principal_ids and gids. Persisted
// in the identity.db header so a deleted id is never reused (§16.3).
static mut NEXT_AUTO_ID: u32 = FIRST_AUTO_ID;

unsafe fn alloc_auto_id() -> Option<u32> {
    let id = NEXT_AUTO_ID;
    // Refuse to mint a reserved value: the counter only climbs, so the only
    // way to reach one is exhausting the (1000 .. PRINCIPAL_SYSTEM) range.
    if id < FIRST_AUTO_ID || id >= PRINCIPAL_SYSTEM {
        return None;
    }
    NEXT_AUTO_ID = id + 1;
    Some(id)
}

struct GroupRecord {
    gid: u32,
    name: Vec<u8>,
}

static mut GROUPS: Option<Vec<GroupRecord>> = None;

unsafe fn groups_init() {
    core::ptr::write(core::ptr::addr_of_mut!(GROUPS), Some(Vec::new()));
}

unsafe fn group_name_exists(name: &[u8]) -> bool {
    match (*core::ptr::addr_of!(GROUPS)).as_ref() {
        Some(g) => g.iter().any(|r| r.name == name),
        None => false,
    }
}

// Append a group record. Caller guarantees the name is not a duplicate (the
// verb handlers check first). Returns false only if the table is uninitialized.
unsafe fn groups_push(gid: u32, name: &[u8]) -> bool {
    let table = match (*core::ptr::addr_of_mut!(GROUPS)).as_mut() {
        Some(t) => t,
        None => return false,
    };
    let mut n = Vec::new();
    n.extend_from_slice(name);
    table.push(GroupRecord { gid, name: n });
    true
}

unsafe fn groups_pop_last() {
    if let Some(t) = (*core::ptr::addr_of_mut!(GROUPS)).as_mut() {
        let _ = t.pop();
    }
}

unsafe fn groups_count() -> usize {
    match (*core::ptr::addr_of!(GROUPS)).as_ref() {
        Some(g) => g.len(),
        None => 0,
    }
}

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

// Replace the wrap fields (params + salt + nonce + ciphertext + tag) of an
// existing user in place, keeping identity fields. RECOVER calls this after the
// hybrid.corvus rename-swap so a subsequent AUTH (new passphrase) succeeds
// without a reboot. Returns false if the user is absent.
unsafe fn user_states_update_wrap(
    user: &[u8],
    t_cost: u32,
    m_cost_kib: u32,
    parallelism: u32,
    salt: &[u8; ARGON2_SALT_LEN],
    nonce: &[u8; AEGIS256_NONCE_LEN],
    ciphertext: &[u8; KEYPAIR_LEN],
    tag: &[u8; AEGIS256_TAG_LEN],
) -> bool {
    let states = match (*core::ptr::addr_of_mut!(USER_STATES)).as_mut() {
        Some(s) => s,
        None => return false,
    };
    for s in states.iter_mut() {
        if s.user == user {
            s.t_cost = t_cost;
            s.m_cost_kib = m_cost_kib;
            s.parallelism = parallelism;
            s.salt = *salt;
            s.nonce = *nonce;
            s.ciphertext = *ciphertext;
            s.tag = *tag;
            return true;
        }
    }
    false
}

unsafe fn user_states_find_by_id(principal_id: u32) -> Option<CorvusUserState> {
    let states = (*core::ptr::addr_of!(USER_STATES)).as_ref()?;
    for s in states {
        if s.principal_id == principal_id {
            return Some(s.clone());
        }
    }
    None
}

unsafe fn user_states_pop_last() {
    if let Some(s) = (*core::ptr::addr_of_mut!(USER_STATES)).as_mut() {
        let _ = s.pop();
    }
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
// RECOVER rate limit — in-memory, per-subject (A-5c). See RECOVER_FAIL_MAX.
// =============================================================================
//
// Counts checksum-VALID-but-unwrap-FAILED RECOVER attempts per subject (a typo
// fails the cheap checksum first and is NOT counted; only a crafted/guessed
// phrase that reaches -- and fails -- the AEAD unwrap is). A legitimate holder's
// real phrase succeeds on the first try, so this never locks them out. Cleared
// on success. The full time-windowed, Stratum-persisted C-16 (covering AUTH) is
// tracked separately.

struct RecoverFail {
    subject: Vec<u8>,
    count: u32,
}

static mut RECOVER_FAILS: Option<Vec<RecoverFail>> = None;

unsafe fn recover_fails_init() {
    core::ptr::write(core::ptr::addr_of_mut!(RECOVER_FAILS), Some(Vec::new()));
}

unsafe fn recover_fail_count(subject: &[u8]) -> u32 {
    match (*core::ptr::addr_of!(RECOVER_FAILS)).as_ref() {
        Some(v) => v
            .iter()
            .find(|r| r.subject == subject)
            .map(|r| r.count)
            .unwrap_or(0),
        None => 0,
    }
}

unsafe fn recover_fail_inc(subject: &[u8]) {
    let v = match (*core::ptr::addr_of_mut!(RECOVER_FAILS)).as_mut() {
        Some(v) => v,
        None => return,
    };
    for r in v.iter_mut() {
        if r.subject == subject {
            r.count = r.count.saturating_add(1);
            return;
        }
    }
    // Bound the table. The subject universe is up to MAX_USERS users PLUS the fixed
    // SYSTEM_WRAP_SUBJECT (handle_recover_system charges it), so the cap is MAX_USERS+1
    // (RW-6 R2-F5/R3-F2) -- a flat MAX_USERS could drop the system subject under table
    // exhaustion and silently un-rate-limit RECOVER(system).
    if v.len() >= MAX_USERS + 1 {
        return;
    }
    let mut s = Vec::new();
    s.extend_from_slice(subject);
    v.push(RecoverFail { subject: s, count: 1 });
}

unsafe fn recover_fail_reset(subject: &[u8]) {
    if let Some(v) = (*core::ptr::addr_of_mut!(RECOVER_FAILS)).as_mut() {
        for r in v.iter_mut() {
            if r.subject == subject {
                r.count = 0;
                return;
            }
        }
    }
}

// =============================================================================
// On-disk persistence — A-1b (CORVUS-DESIGN.md §16.4 / §16.5 / §16.6).
// =============================================================================
//
// corvus's root IS /var/lib/corvus post-chroot (A-1.7), so every path here
// resolves WITHIN the handed storage capability. Two artifacts: the central
// non-secret CRVS-v2 identity.db, and the per-user CRVS-v1 hybrid.corvus wrap
// (encrypted keypair). The DB is rewritten atomically via the A-1.6 rename-swap
// substrate; the wrap is write-once at USER_CREATE.

const IDB: &[u8] = b"identity.db";
const IDB_TMP: &[u8] = b"identity.db.tmp";
const USERS_DIR: &[u8] = b"users";
const HYBRID: &[u8] = b"hybrid.corvus";
const HYBRID_TMP: &[u8] = b"hybrid.corvus.tmp";
// A-5c recovery keyslot, alongside hybrid.corvus under users/<name>/.
const RECOVERY_FILE: &[u8] = b"recovery.corvus";
const RECOVERY_TMP: &[u8] = b"recovery.corvus.tmp";
// A-5c-b system identity, host-baked into corvus's root (NOT under users/): the
// admin keypair wrapped under the system passphrase (system-wrap) and under a
// recovery phrase (system-recovery-wrap). Read at boot; rolled by RECOVER(system).
const SYSTEM_WRAP_FILE: &[u8] = b"system-wrap";
const SYSTEM_WRAP_TMP: &[u8] = b"system-wrap.tmp";
const SYSTEM_RECOVERY_FILE: &[u8] = b"system-recovery-wrap";
const SYSTEM_RECOVERY_TMP: &[u8] = b"system-recovery-wrap.tmp";

const FS_IO_CHUNK: usize = 2048; // bounded per-call read/write (<= SYS_RW_MAX)
const IDENTITY_DB_MAX: usize = 256 * 1024; // read cap; ~36 KiB worst case at 256 users

// Write the whole buffer to an opened fd, chunked at SYS_RW_MAX. A short or
// error return fails the whole write (partial writes are not retried past the
// reported count, but a 0/-1 return is fatal).
unsafe fn write_all_fd(fd: i64, buf: &[u8]) -> bool {
    let mut off = 0usize;
    while off < buf.len() {
        let chunk = core::cmp::min(buf.len() - off, FS_IO_CHUNK);
        let w = t_write(fd, buf[off..].as_ptr(), chunk);
        if w <= 0 {
            return false;
        }
        off += w as usize;
        if (w as usize) > chunk {
            return false; // impossible over-report -> fail closed
        }
    }
    true
}

// Read an opened fd to EOF into `out`, failing closed past `max` bytes.
unsafe fn read_to_end_fd(fd: i64, out: &mut Vec<u8>, max: usize) -> bool {
    let mut buf = [0u8; FS_IO_CHUNK];
    loop {
        let r = t_read(fd, buf.as_mut_ptr(), FS_IO_CHUNK);
        if r < 0 {
            return false;
        }
        if r == 0 {
            return true; // EOF
        }
        let n = r as usize;
        if n > FS_IO_CHUNK || out.len() + n > max {
            return false; // over-report / oversize -> fail closed
        }
        out.extend_from_slice(&buf[..n]);
    }
}

// Idempotent mkdir returning a NON-OPENED (O_PATH) walkable handle (FS-delta);
// mirrors joey's mkdir_or_open. `parent` must itself be non-opened (FROM_ROOT
// or a prior result -- 9P forbids Twalk from an opened fid). Returns the dir fd
// (>= 0) or -1.
unsafe fn mkdir_opath(parent: i64, name: &[u8]) -> i64 {
    let cf = t_walk_create(parent, name.as_ptr(), name.len(), T_OREAD,
                           T_WALK_CREATE_DMDIR | 0o755);
    if cf >= 0 {
        let _ = t_close(cf);
    }
    t_walk_open(parent, name.as_ptr(), name.len(), T_OPATH)
}

// Serialize the full in-memory DB to the CRVS v2 byte format (§16.4). Returns
// an empty Vec only if a table is uninitialized (a caller-side bug); an empty
// DB serializes to the 20-byte header.
unsafe fn identity_db_serialize() -> Vec<u8> {
    let states = match (*core::ptr::addr_of!(USER_STATES)).as_ref() {
        Some(s) => s,
        None => return Vec::new(),
    };
    let groups = match (*core::ptr::addr_of!(GROUPS)).as_ref() {
        Some(g) => g,
        None => return Vec::new(),
    };
    let mut out = Vec::new();
    out.extend_from_slice(&CORVUS_MAGIC.to_le_bytes());
    out.extend_from_slice(&IDENTITY_DB_VERSION.to_le_bytes());
    out.extend_from_slice(&NEXT_AUTO_ID.to_le_bytes());
    out.extend_from_slice(&(states.len() as u32).to_le_bytes());
    out.extend_from_slice(&(groups.len() as u32).to_le_bytes());
    for s in states {
        out.extend_from_slice(&s.principal_id.to_le_bytes());
        out.extend_from_slice(&s.primary_gid.to_le_bytes());
        out.push(s.backend);
        out.push(s.supp_gids.len() as u8);
        out.push(s.user.len() as u8);
        for g in &s.supp_gids {
            out.extend_from_slice(&g.to_le_bytes());
        }
        out.extend_from_slice(&s.user);
    }
    for g in groups {
        out.extend_from_slice(&g.gid.to_le_bytes());
        out.push(g.name.len() as u8);
        out.extend_from_slice(&g.name);
    }
    out
}

// Parse a CRVS v2 identity.db into NEXT_AUTO_ID + USER_STATES (identity fields
// only; wrap fields zeroed, filled later from hybrid.corvus) + GROUPS. Every
// length field is bounds-checked against the remaining buffer (no over-read);
// any malformed/truncated record fails the WHOLE load closed (§16.4). Trailing
// bytes past the last record also fail closed (a clean file is consumed
// exactly). Returns true on a fully-parsed file.
unsafe fn identity_db_parse(blob: &[u8]) -> bool {
    if blob.len() < IDENTITY_DB_HEADER_LEN {
        return false;
    }
    let magic = u32::from_le_bytes([blob[0], blob[1], blob[2], blob[3]]);
    let version = u32::from_le_bytes([blob[4], blob[5], blob[6], blob[7]]);
    if magic != CORVUS_MAGIC || version != IDENTITY_DB_VERSION {
        return false;
    }
    let next_id = u32::from_le_bytes([blob[8], blob[9], blob[10], blob[11]]);
    let user_count = u32::from_le_bytes([blob[12], blob[13], blob[14], blob[15]]) as usize;
    let group_count = u32::from_le_bytes([blob[16], blob[17], blob[18], blob[19]]) as usize;
    if next_id < FIRST_AUTO_ID || next_id >= PRINCIPAL_SYSTEM {
        return false;
    }
    if user_count > MAX_USERS || group_count > 2 * MAX_USERS {
        return false;
    }

    let mut users: Vec<CorvusUserState> = Vec::new();
    let mut groups: Vec<GroupRecord> = Vec::new();
    let mut off = IDENTITY_DB_HEADER_LEN;

    for _ in 0..user_count {
        if off + 11 > blob.len() {
            return false;
        }
        let principal_id =
            u32::from_le_bytes([blob[off], blob[off + 1], blob[off + 2], blob[off + 3]]);
        let primary_gid =
            u32::from_le_bytes([blob[off + 4], blob[off + 5], blob[off + 6], blob[off + 7]]);
        let backend = blob[off + 8];
        let supp_n = blob[off + 9] as usize;
        let name_n = blob[off + 10] as usize;
        off += 11;
        if supp_n > PROC_SUPP_GIDS_MAX {
            return false;
        }
        if name_n == 0 || name_n > MAX_USER_LEN {
            return false;
        }
        // The writer emits only BACKEND_ID_PASSPHRASE; reject any other backend so the
        // parser's accepted domain equals the writer's emitted domain (RW-6 R3-F5).
        // Inert today (build_ad always uses the passphrase AD), but a future
        // backend-dispatching AUTH must never select an unwrap path from an
        // unvalidated on-disk byte.
        if backend != BACKEND_ID_PASSPHRASE {
            return false;
        }
        if off + 4 * supp_n + name_n > blob.len() {
            return false;
        }
        // Reserved/illegal identity values fail closed. Every assigned id is
        // strictly below next_id (the counter is past all live ids).
        if principal_id == PRINCIPAL_INVALID || principal_id >= next_id {
            return false;
        }
        // primary_gid is a corvus-assigned id (the UPG == principal_id), so it
        // is always < next_id; reject INVALID and any id never assigned (which
        // covers the reserved sentinels, since next_id <= PRINCIPAL_SYSTEM).
        if primary_gid == PRINCIPAL_INVALID || primary_gid >= next_id {
            return false;
        }
        let mut supp = Vec::with_capacity(supp_n);
        for _ in 0..supp_n {
            let g = u32::from_le_bytes([blob[off], blob[off + 1], blob[off + 2], blob[off + 3]]);
            // Reject reserved sentinels (INVALID/SYSTEM/NONE). supp_gids are NOT
            // bounded to corvus-assigned ids -- they may be external group
            // references -- so they are not range-checked against next_id.
            if g == PRINCIPAL_INVALID || g >= PRINCIPAL_SYSTEM {
                return false;
            }
            supp.push(g);
            off += 4;
        }
        let mut name = Vec::with_capacity(name_n);
        name.extend_from_slice(&blob[off..off + name_n]);
        off += name_n;
        users.push(CorvusUserState {
            user: name,
            principal_id,
            primary_gid,
            supp_gids: supp,
            backend,
            t_cost: 0,
            m_cost_kib: 0,
            parallelism: 0,
            salt: [0; ARGON2_SALT_LEN],
            nonce: [0; AEGIS256_NONCE_LEN],
            ciphertext: [0; KEYPAIR_LEN],
            tag: [0; AEGIS256_TAG_LEN],
        });
    }
    for _ in 0..group_count {
        if off + 5 > blob.len() {
            return false;
        }
        let gid = u32::from_le_bytes([blob[off], blob[off + 1], blob[off + 2], blob[off + 3]]);
        let name_n = blob[off + 4] as usize;
        off += 5;
        if name_n == 0 || name_n > MAX_GROUP_LEN {
            return false;
        }
        if off + name_n > blob.len() {
            return false;
        }
        if gid == PRINCIPAL_INVALID || gid >= PRINCIPAL_SYSTEM {
            return false;
        }
        let mut name = Vec::with_capacity(name_n);
        name.extend_from_slice(&blob[off..off + name_n]);
        off += name_n;
        groups.push(GroupRecord { gid, name });
    }
    if off != blob.len() {
        return false; // trailing garbage -> fail closed
    }

    core::ptr::write(core::ptr::addr_of_mut!(USER_STATES), Some(users));
    core::ptr::write(core::ptr::addr_of_mut!(GROUPS), Some(groups));
    NEXT_AUTO_ID = next_id;
    true
}

// Open users/<name>/hybrid.corvus and fill `rec`'s wrap fields. `users_fd` is
// the non-opened users/ dir handle. Returns false (drop the user, fail-closed)
// on any failure -- a user with no usable secret is not authoritative for login.
unsafe fn load_keypair_wrap(users_fd: i64, name: &[u8], rec: &mut CorvusUserState) -> bool {
    let udir = t_walk_open(users_fd, name.as_ptr(), name.len(), T_OPATH);
    if udir < 0 {
        return false;
    }
    let wf = t_walk_open(udir, HYBRID.as_ptr(), HYBRID.len(), T_OREAD);
    let _ = t_close(udir);
    if wf < 0 {
        return false;
    }
    let mut blob: Vec<u8> = Vec::new();
    let ok = read_to_end_fd(wf, &mut blob, TOTAL_LEN + 64);
    let _ = t_close(wf);
    if !ok {
        return false;
    }
    rec.fill_wrap_from_bytes(&blob)
}

// Write the per-user keypair wrap (write-once, fsync'd) to
// users/<name>/hybrid.corvus, creating the per-user dir if absent. A stale
// orphan from a prior crashed create is unlinked first. Returns false on any
// failure (caller aborts the create BEFORE committing identity.db; §16.6 step 1).
unsafe fn persist_keypair_wrap(users_fd: i64, name: &[u8], rec: &CorvusUserState) -> bool {
    let udir = mkdir_opath(users_fd, name);
    if udir < 0 {
        return false;
    }
    let _ = t_unlink(udir, HYBRID.as_ptr(), HYBRID.len(), 0);
    let wf = t_walk_create(udir, HYBRID.as_ptr(), HYBRID.len(), T_OWRITE, 0o600);
    if wf < 0 {
        let _ = t_close(udir);
        return false;
    }
    let blob = rec.to_bytes();
    let wrote = write_all_fd(wf, &blob);
    let file_synced = t_fsync(wf, 0) == 0;
    let _ = t_close(wf);
    // Dirent-durability barriers (CORVUS-DESIGN.md §16.6). On the CURRENT
    // Stratum, Tfsync (h_fsync) is a WHOLE-POOL commit (stm_fs_commit), so the
    // file fsync above already commits the hybrid.corvus + users/<name> dirents
    // -- these per-dir fsyncs are forward-portable insurance that becomes
    // load-bearing the moment Stratum makes Tfsync per-fid (the POSIX FS
    // contract, where a file fsync does NOT make the name->inode link durable).
    // Harmless idempotent re-commits today; kept so the path is correct on any
    // 9P server. (The load-bearing cross-reboot fixes were the bdev partial-tail
    // RMW + the read-offset alignment, Stratum-side; see DEBUGGING-PLAYBOOK.md.)
    let wrap_dir_synced = t_fsync(udir, 0) == 0;
    let _ = t_close(udir);
    let users_dir_synced = t_fsync(users_fd, 0) == 0;
    wrote && file_synced && wrap_dir_synced && users_dir_synced
}

// Write the per-user recovery keyslot (A-5c; write-once at USER_CREATE, fsync'd)
// to users/<name>/recovery.corvus, creating the per-user dir if absent. A stale
// orphan from a prior crashed create is unlinked first. Returns false on any
// failure (caller aborts the create BEFORE committing identity.db). Mirrors
// persist_keypair_wrap.
unsafe fn persist_recovery_wrap(users_fd: i64, name: &[u8], rw: &RecoveryWrap) -> bool {
    let udir = mkdir_opath(users_fd, name);
    if udir < 0 {
        return false;
    }
    let _ = t_unlink(udir, RECOVERY_FILE.as_ptr(), RECOVERY_FILE.len(), 0);
    let wf = t_walk_create(udir, RECOVERY_FILE.as_ptr(), RECOVERY_FILE.len(), T_OWRITE, 0o600);
    if wf < 0 {
        let _ = t_close(udir);
        return false;
    }
    let blob = rw.to_bytes();
    let wrote = write_all_fd(wf, &blob);
    let file_synced = t_fsync(wf, 0) == 0;
    let _ = t_close(wf);
    let wrap_dir_synced = t_fsync(udir, 0) == 0;
    let _ = t_close(udir);
    let users_dir_synced = t_fsync(users_fd, 0) == 0;
    wrote && file_synced && wrap_dir_synced && users_dir_synced
}

// Open users/<name>/recovery.corvus and parse the recovery keyslot. None if
// absent (a user predating A-5c enrollment) or corrupt. Read-only -- the wrap is
// consulted only during RECOVER, never at boot.
unsafe fn load_recovery_wrap(users_fd: i64, name: &[u8]) -> Option<RecoveryWrap> {
    let udir = t_walk_open(users_fd, name.as_ptr(), name.len(), T_OPATH);
    if udir < 0 {
        return None;
    }
    let rf = t_walk_open(udir, RECOVERY_FILE.as_ptr(), RECOVERY_FILE.len(), T_OREAD);
    let _ = t_close(udir);
    if rf < 0 {
        return None;
    }
    let mut blob: Vec<u8> = Vec::new();
    let ok = read_to_end_fd(rf, &mut blob, TOTAL_LEN + 64);
    let _ = t_close(rf);
    if !ok {
        return None;
    }
    RecoveryWrap::from_bytes(&blob)
}

// Atomic rename-swap of a per-user wrap file (RECOVER re-wrap of an EXISTING
// hybrid.corvus / recovery.corvus): write tmp -> fsync(tmp) -> rename(tmp ->
// real) -> fsync(dir). Atomicity matters here (unlike the write-once create
// paths) because the old file must never be lost to a partial write -- both
// keyslots wrap the same keypair, but a torn file would be unreadable. `udir`
// is the caller-owned per-user O_PATH dir handle (not closed here).
unsafe fn persist_wrap_swap(udir: i64, real: &[u8], tmp: &[u8], blob: &[u8]) -> bool {
    let _ = t_unlink(udir, tmp.as_ptr(), tmp.len(), 0);
    let tf = t_walk_create(udir, tmp.as_ptr(), tmp.len(), T_OWRITE, 0o600);
    if tf < 0 {
        return false;
    }
    let wrote = write_all_fd(tf, blob);
    let synced = t_fsync(tf, 0) == 0;
    let _ = t_close(tf);
    if !wrote || !synced {
        let _ = t_unlink(udir, tmp.as_ptr(), tmp.len(), 0);
        return false;
    }
    if t_rename(udir, tmp.as_ptr(), tmp.len(), udir, real.as_ptr(), real.len()) != 0 {
        let _ = t_unlink(udir, tmp.as_ptr(), tmp.len(), 0);
        return false;
    }
    let _ = t_fsync(udir, 0);
    true
}

// Atomic rewrite-swap of identity.db (§16.6 step 2): write tmp -> fsync(tmp) ->
// rename(tmp -> real) -> fsync(dir). The rename is the atomic commit point; the
// directory fsync (on the storage-cap fd, the dir handle) is the dirent-
// durability barrier. Returns false on any failure (caller rolls back its
// in-memory mutation). C-26 crash-safety: a crash before the rename leaves the
// intact old DB + a partial tmp (unlinked at next boot); a crash at/after the
// rename leaves a complete DB (rename is atomic).
unsafe fn identity_persist() -> bool {
    let blob = identity_db_serialize();
    if blob.len() < IDENTITY_DB_HEADER_LEN {
        return false; // uninitialized tables -- never on the live path
    }
    let _ = t_unlink(T_WALK_OPEN_FROM_ROOT, IDB_TMP.as_ptr(), IDB_TMP.len(), 0);
    let tf = t_walk_create(T_WALK_OPEN_FROM_ROOT, IDB_TMP.as_ptr(), IDB_TMP.len(), T_OWRITE, 0o600);
    if tf < 0 {
        return false;
    }
    let wrote = write_all_fd(tf, &blob);
    let synced = t_fsync(tf, 0) == 0;
    let _ = t_close(tf);
    if !wrote || !synced {
        let _ = t_unlink(T_WALK_OPEN_FROM_ROOT, IDB_TMP.as_ptr(), IDB_TMP.len(), 0);
        return false;
    }
    if t_rename(T_WALK_OPEN_FROM_ROOT, IDB_TMP.as_ptr(), IDB_TMP.len(),
                T_WALK_OPEN_FROM_ROOT, IDB.as_ptr(), IDB.len()) != 0 {
        let _ = t_unlink(T_WALK_OPEN_FROM_ROOT, IDB_TMP.as_ptr(), IDB_TMP.len(), 0);
        return false;
    }
    // Dirent-durability barrier (best-effort): the rename already committed
    // atomically; this hardens against power loss. fd 0 is the still-open R|W
    // non-opened handle to corvus's root dir (SYS_CHROOT borrowed, not consumed).
    let _ = t_fsync(STORAGE_ROOT_FD, 0);
    true
}

// Boot-time load (§16.5). Returns false on a FATAL condition (present-but-
// corrupt DB, FS error) so corvus refuses to start rather than silently
// re-bootstrapping (an attacker who corrupts identity.db must NOT get a free
// first-user bootstrap). An ABSENT identity.db is a fresh install -> empty DB.
unsafe fn identity_load() -> bool {
    // 1. Ensure users/ exists (corvus root IS /var/lib/corvus post-chroot).
    let users_fd = mkdir_opath(T_WALK_OPEN_FROM_ROOT, USERS_DIR);
    if users_fd < 0 {
        return false;
    }

    // 2. Clean a stale tmp left by a crash between create-tmp and rename.
    let _ = t_unlink(T_WALK_OPEN_FROM_ROOT, IDB_TMP.as_ptr(), IDB_TMP.len(), 0);

    // 3. Open identity.db. Absent -> fresh install (empty DB; defaults stand).
    let dbf = t_walk_open(T_WALK_OPEN_FROM_ROOT, IDB.as_ptr(), IDB.len(), T_OREAD);
    if dbf < 0 {
        let _ = t_close(users_fd);
        return true;
    }
    let mut blob: Vec<u8> = Vec::new();
    let read_ok = read_to_end_fd(dbf, &mut blob, IDENTITY_DB_MAX);
    let _ = t_close(dbf);
    if !read_ok || !identity_db_parse(&blob) {
        let _ = t_close(users_fd);
        return false; // present-but-unreadable/corrupt -> fail closed
    }

    // 4. Fill each user's wrap from users/<name>/hybrid.corvus; drop a user
    //    whose wrap is missing/corrupt (no usable secret). Re-register the
    //    dataset-owner mapping so WRAP/UNWRAP work post-reboot.
    let taken = (*core::ptr::addr_of_mut!(USER_STATES)).take();
    let states = match taken {
        Some(s) => s,
        None => {
            let _ = t_close(users_fd);
            return false;
        }
    };
    let mut keep: Vec<CorvusUserState> = Vec::new();
    for mut rec in states.into_iter() {
        let uname = rec.user.clone();
        if load_keypair_wrap(users_fd, &uname, &mut rec) {
            let mut ds = Vec::new();
            ds.extend_from_slice(b"users/");
            ds.extend_from_slice(&uname);
            dataset_owner_register(&ds, &uname);
            keep.push(rec);
        } else {
            t_putstr("corvus: identity load dropped a user (missing/corrupt wrap)\n");
        }
    }
    let kept = keep.len();
    core::ptr::write(core::ptr::addr_of_mut!(USER_STATES), Some(keep));
    let _ = t_close(users_fd);

    t_putstr("corvus: identity.db loaded (");
    let mut nbuf = [0u8; 12];
    t_putstr(usize_dec(kept, &mut nbuf));
    t_putstr(" users)\n");
    true
}

// =============================================================================
// System identity — A-5c-b (CORVUS-DESIGN.md §5.5.1 + §5.6).
// =============================================================================
//
// The admin hybrid keypair is host-baked into corvus's root (tools/build.sh runs
// corvus-mint at build time) as two CRVS-v1 wraps of the SAME keypair: system-wrap
// (under the system passphrase, verified by ADMIN_ELEVATE) and system-recovery-wrap
// (under a recovery phrase, consumed by RECOVER(system)). Both are loaded once at
// boot and held as parsed wraps in these statics. They hold CIPHERTEXT only -- the
// plaintext keypair is decrypted transiently inside ADMIN_ELEVATE / RECOVER(system)
// and wiped, never persisted in RAM. There is no USER_STATES record for the system
// subject; the system identity lives entirely in these wraps + the two root files.
static mut SYSTEM_KEYPAIR_WRAP: Option<KeypairWrap> = None;
static mut SYSTEM_RECOVERY_WRAP: Option<RecoveryWrap> = None;

// Read a CRVS-v1 wrap blob from a root-relative file (corvus is chroot'd, so
// these resolve within the storage cap). None on absent / FS error / oversized.
unsafe fn read_root_wrap_blob(name: &[u8]) -> Option<Vec<u8>> {
    let fd = t_walk_open(T_WALK_OPEN_FROM_ROOT, name.as_ptr(), name.len(), T_OREAD);
    if fd < 0 {
        return None;
    }
    let mut blob: Vec<u8> = Vec::new();
    let ok = read_to_end_fd(fd, &mut blob, TOTAL_LEN + 64);
    let _ = t_close(fd);
    if !ok {
        return None;
    }
    Some(blob)
}

// Boot-time load of the host-baked system identity. FATAL (returns false) if
// EITHER wrap is absent or unparseable -- corvus refuses to start without an
// authoritative system identity rather than fall back to a byte-compare. The
// wraps are parse-validated only (magic / version / length); the CRYPTOGRAPHIC
// check (does the passphrase open system-wrap?) is deferred to ADMIN_ELEVATE,
// because corvus does NOT know the system passphrase -- only the wrap.
unsafe fn system_identity_load() -> bool {
    let kw_blob = match read_root_wrap_blob(SYSTEM_WRAP_FILE) {
        Some(b) => b,
        None => return false,
    };
    let kw = match KeypairWrap::from_bytes(&kw_blob) {
        Some(k) => k,
        None => return false,
    };
    let rw_blob = match read_root_wrap_blob(SYSTEM_RECOVERY_FILE) {
        Some(b) => b,
        None => return false,
    };
    let rw = match RecoveryWrap::from_bytes(&rw_blob) {
        Some(r) => r,
        None => return false,
    };
    core::ptr::write(core::ptr::addr_of_mut!(SYSTEM_KEYPAIR_WRAP), Some(kw));
    core::ptr::write(core::ptr::addr_of_mut!(SYSTEM_RECOVERY_WRAP), Some(rw));
    true
}

// =============================================================================
// Clearance levels + the legate (A-4a-3; CORVUS-DESIGN.md §5.7 + IDENTITY-DESIGN
// §9.8). corvus owns the clearance POLICY (the level table + per-user
// eligibility); the kernel owns the cap-stamp + scope (the `cap` device clearance
// grant -> proc_become_legate). No local crypto -- corvus's CAP_GRANT_CLEARANCE
// is the trust root, and it verifies a level's `auth_required` before ever
// registering the grant.
// =============================================================================

// auth_required scales with stakes (scripture §3.1). v1.0 enforces RE_AUTH in
// band (a valid session token IS the re-auth proof). DISTINCT_SECRET is the
// kernel SAK trusted path (IM-3, IMPERIUM-DESIGN.md 11.5): unlocked ONLY by
// IMPERIUM_REQUEST + a serial BREAK + the per-(user, level) key typed on the
// frozen console -- CLEARANCE_ACTIVATE(_SELF) on such a level stays REFUSED.
// SYSTEM_KEY / HOSTOWNER_COSIGN remain unbuilt and refused (a documented gap).
const AUTH_REQ_RE_AUTH: u8 = 0;
const AUTH_REQ_DISTINCT_SECRET: u8 = 1;
const AUTH_REQ_SYSTEM_KEY: u8 = 2;
const AUTH_REQ_HOSTOWNER_COSIGN: u8 = 3;

const MAX_LEVEL_LEN: usize = 32;

// A clearance level -- a built-in policy object at v1.0 (the coarse set scripture
// names; no LEVEL_CREATE verb exists, so runtime authoring + per-level-file
// persistence are a v1.x seam). A level's caps MUST be a subset of the kernel's
// CAP_GRANTABLE_CLEARANCE ({DAC_OVERRIDE, CHOWN, KILL, DEBUG, JIT, AUDIO_GRAPH}) -- the kernel grant
// rejects anything else -- and only RE_AUTH levels are activatable at v1.0.
struct ClearanceLevel {
    name: &'static [u8],
    caps: u64,
    auth_required: u8,
    time_bound_ns: u64, // 0 = no time bound (scope ends only on legate root exit)
    // IM-3: the scope the redeemer creates is PROPAGATING -- the caps FLOW to
    // its rfork descendants, which die with it (IMPERIUM-DESIGN.md 11.4; the
    // kernel bounds a propagating grant to CAP_GRANTABLE_IMPERIUM).
    propagating: bool,
}

// The redeemer may ask for SHORTER than the level's bound, never longer; an
// unbounded level honors the request as-is (0 = none). Shared by every path
// that turns a level + a request into a grant (verbs 15 / 18 / 19).
fn level_valid_for(lvl: &ClearanceLevel, valid_until_req: u64) -> u64 {
    if valid_until_req == 0 {
        lvl.time_bound_ns
    } else if lvl.time_bound_ns != 0 {
        core::cmp::min(valid_until_req, lvl.time_bound_ns)
    } else {
        valid_until_req
    }
}

// The v1.0 built-in level set. fs-admin (the DAC-override + chown split out of
// CAP_HOSTOWNER -- the only level with a live consumer today, via perm_check) +
// supervisor (CAP_KILL; its /proc-ctl consumer lands in A-4b -- the cap is inert
// until then, but the clearance mechanism is proven with a second cap). Both
// RE_AUTH. hw-dev / user-admin / clearance-admin (scripture's other coarse names)
// are NOT v1.0 levels: their caps are not in CAP_GRANTABLE_CLEARANCE
// (HW_CREATE is fork-grantable; user/clearance-admin ride CAP_HOSTOWNER), so they
// are v1.x once a finer cap or dev-mode (§3.1 F-7) lands.
static CLEARANCE_LEVELS: &[ClearanceLevel] = &[
    ClearanceLevel {
        name: b"fs-admin",
        caps: T_CAP_DAC_OVERRIDE | T_CAP_CHOWN,
        auth_required: AUTH_REQ_RE_AUTH,
        time_bound_ns: 0,
        propagating: false,
    },
    ClearanceLevel {
        name: b"supervisor",
        caps: T_CAP_KILL,
        auth_required: AUTH_REQ_RE_AUTH,
        time_bound_ns: 0,
        propagating: false,
    },
    // jit (CL-7k / I-42): the authority to create a dual-mapped code region --
    // the only path by which userspace-emitted bytes become executable. This is
    // the level the comment above anticipated: CAP_JIT, unlike CAP_HW_CREATE,
    // IS in CAP_GRANTABLE_CLEARANCE (it is elevation-only, which I-42 requires
    // so the capability is never inherited), so it is grantable exactly here.
    //
    // Its consumer is a runtime that compiles at run time -- Mesa's llvmpipe
    // shader JIT at CL-7, later a wasm or JS engine. Routing it through a
    // clearance rather than a spawn-time grant is what keeps "who may emit
    // code" an explicit, bounded, auditable act rather than an ambient one.
    ClearanceLevel {
        name: b"jit",
        caps: T_CAP_JIT,
        auth_required: AUTH_REQ_RE_AUTH,
        time_bound_ns: 0,
        propagating: false,
    },
    // audio-graph (Nocturne N-3a / I-46; docs/NOCTURNE.md 6.8): the whole-sink
    // authority. Its holder may operate on the SYSTEM-owned sink beyond its own
    // voices -- set the sink volume/default, insert a descant at the sink input
    // (a system EQ), and tap the sink loopback. The consumer is nocturned (the
    // sink owner), which checks the bit via SYS_SRV_PEER. Unlike jit this is NOT
    // user-default-eligible: it is whole-sink, cross-owner authority, not a
    // self-contained one, so 6.8 -- "a system-level program ... needs a grant" --
    // means eligibility is admin-granted (VERB_CLEARANCE_GRANT), never seeded.
    ClearanceLevel {
        name: b"audio-graph",
        caps: T_CAP_AUDIO_GRAPH,
        auth_required: AUTH_REQ_RE_AUTH,
        time_bound_ns: 0,
        propagating: false,
    },
    // imperium (IM-3; IMPERIUM-DESIGN.md 11.5): the power-user tier. The same
    // three elevation-only caps fs-admin + supervisor carry, but unlocked ONLY
    // on the trusted path -- a serial BREAK opens the kernel episode and the
    // per-user imperium key is typed on the frozen console, never a session
    // token -- bounded to four hours, and PROPAGATING: the sub-shell's children
    // inherit the caps and die with the scope (I-25 strengthened; a forgotten
    // imperium shell is not a forgotten root shell). Eligibility is
    // admin-granted WITH the key (CLEARANCE_GRANT's key tail); the request's
    // cap-set is the self-restriction subset (`imperium chown dac`).
    ClearanceLevel {
        name: b"imperium",
        caps: T_CAP_DAC_OVERRIDE | T_CAP_CHOWN | T_CAP_KILL,
        auth_required: AUTH_REQ_DISTINCT_SECRET,
        time_bound_ns: 4 * 3600 * 1_000_000_000,
        propagating: true,
    },
];

fn level_by_name(name: &[u8]) -> Option<&'static ClearanceLevel> {
    CLEARANCE_LEVELS.iter().find(|l| l.name == name)
}

// caps -> the versioned TLV the wire carries (scripture: "structured TLV, NOT a
// bare u64", so v1.x resource-scoping is additive). v1.0 emits ONE entry: the
// cap bitmask. Layout: version u8 (=1) + {tag u8 (=BITMASK), len u16 LE (=8),
// value u64 LE}. A v1.x decoder reads entries it understands + skips unknown
// tags -- additive by construction.
const CAPS_TLV_VERSION: u8 = 1;
const CAPS_TLV_TAG_BITMASK: u8 = 1;

fn caps_tlv_encode(caps: u64, out: &mut Vec<u8>) {
    out.push(CAPS_TLV_VERSION);
    out.push(CAPS_TLV_TAG_BITMASK);
    out.extend_from_slice(&8u16.to_le_bytes());
    out.extend_from_slice(&caps.to_le_bytes());
}

// -----------------------------------------------------------------------------
// Per-user eligibility -- the persisted "who may activate which level" table. A
// grant creates a record; a revoke deletes it (per-user, no shared-secret
// rotation; scripture §3.1). Persisted in /var/lib/corvus/clearance.db
// (CRVS-format, atomic rename-swap, mirroring identity.db §16.6). For a RE_AUTH
// level there is no secret unlock material, so eligibility is a plain record;
// the secret-bearing CRVS wrap (for DISTINCT_SECRET / SYSTEM_KEY levels) is the
// A-4c / v1.x extension.
// -----------------------------------------------------------------------------

const SUBJECT_KIND_USER: u8 = 0;
const SUBJECT_KIND_GROUP: u8 = 1;

const CLEARANCE_DB: &[u8] = b"clearance.db";
const CLEARANCE_DB_TMP: &[u8] = b"clearance.db.tmp";
// IM-3: version 2 adds the KEY record kind (the per-(user, level) capability-
// key verifier, IMPERIUM-DESIGN.md 11.5 / ratified F4). The writer emits v2;
// the reader accepts v1 (eligibility records only) and v2. A v1 reader fails
// closed on a v2 file, which is the posture a corrupt-or-unknown db already
// has: it must never silently drop records.
const CLEARANCE_DB_VERSION_V1: u32 = 1;
const CLEARANCE_DB_VERSION: u32 = 2;
const CLEARANCE_DB_HEADER_LEN: usize = 12;
// Pin the header layout (RW-6 R3-F4), mirroring identity.db's IDENTITY_DB_HEADER_LEN
// assert: magic(4) + version(4) + count(4). A future field add on one side without
// the other would drift the parse offset silently; catch it at build time.
const _: () = assert!(CLEARANCE_DB_HEADER_LEN == 12, "clearance.db header layout drift");
// Same bound class as GROUPS. Since #163 one slot per user is auto-consumed by
// the default jit seed, so a 256-user deployment starts at half capacity and
// the admin-grantable remainder is ~256, not 512 (audit F4). Both push sites
// gate on `< MAX_ELIGIBILITY` and clearance_db_parse rejects only `>`, so a
// full table is a loud refusal, never a next-boot parse brick.
const MAX_ELIGIBILITY: usize = 2 * MAX_USERS;
const CLEARANCE_DB_MAX: usize = 256 * 1024;

#[derive(Clone)]
struct EligibilityRecord {
    subject_kind: u8,
    subject: Vec<u8>,
    level: Vec<u8>,
}

static mut ELIGIBILITY: Option<Vec<EligibilityRecord>> = None;

// #163: the level every human user is eligible for by default. CAP_JIT's only
// power is emitting code into the caller's OWN process under the I-42 W^X
// dual-map (BURROW_TYPE_CODE is refused by burrow_share_into, and the
// construction handle is dropped at creation, so no handle exists to transfer
// -- the authority cannot reach a peer Proc), so eligibility is user-default at
// v1.0; activation stays an explicit RE_AUTH-gated act and the table stays
// authoritative for every other level. Seeded at USER_CREATE; backfilled at
// clearance_load for users minted before this existed.
//
// THE COST, stated (audit F1): this is not fully policy-neutral. Verb 15
// (CLEARANCE_ACTIVATE) grants to the CALLER's stripes on presentation of a
// session token -- its own header concedes a stolen token is worth something --
// so widening eligibility from one user to all users multiplies the population
// of tokens that yield CAP_JIT by the number of users. What bounds the damage
// is I-42's self-process containment above, NOT the eligibility table. Verb 18
// (SELF) has no such gap: identity is the kernel stamp, never a quoted secret.
// Retiring verb 15 (task #140) removes the amplified surface outright.
const SEED_LEVEL_JIT: &[u8] = b"jit";

// IM-3: the capability-key VERIFIER records -- one per (user, DISTINCT_SECRET
// level), minted at CLEARANCE_GRANT from the key the hostowner supplies and
// consumed only by the SAK episode. clearance.db record kind 2. A verifier
// (argon2id + AEGIS over a random token; the tag is the check), never a DEK:
// a stolen clearance.db is an offline guessing target and nothing more. Every
// key record pairs with an eligibility record (REVOKE removes both), so the
// bound is MAX_ELIGIBILITY by construction; a stray key with no eligibility is
// INERT (the request path requires both) and is tolerated on load.
const RECORD_KIND_KEY: u8 = 2;
const MAX_CAPKEYS: usize = MAX_ELIGIBILITY;

struct CapKeyRecord {
    subject: Vec<u8>,
    level: Vec<u8>,
    wrap: CapKeyWrap,
}

static mut CAPKEYS: Option<Vec<CapKeyRecord>> = None;

unsafe fn clearance_init() {
    core::ptr::write(core::ptr::addr_of_mut!(ELIGIBILITY), Some(Vec::new()));
    core::ptr::write(core::ptr::addr_of_mut!(CAPKEYS), Some(Vec::new()));
}

unsafe fn capkey_count() -> usize {
    match (*core::ptr::addr_of!(CAPKEYS)).as_ref() {
        Some(k) => k.len(),
        None => 0,
    }
}

unsafe fn capkey_find(subject: &[u8], level: &[u8]) -> Option<CapKeyWrap> {
    let table = (*core::ptr::addr_of!(CAPKEYS)).as_ref()?;
    table
        .iter()
        .find(|r| r.subject == subject && r.level == level)
        .map(|r| r.wrap.clone())
}

// Insert or replace. Ok(the previous wrap, if any) so a persist failure can
// put it back; Err on an uninitialized or full table.
unsafe fn capkey_set(subject: &[u8], level: &[u8], wrap: CapKeyWrap) -> Result<Option<CapKeyWrap>, ()> {
    let table = match (*core::ptr::addr_of_mut!(CAPKEYS)).as_mut() {
        Some(t) => t,
        None => return Err(()),
    };
    for r in table.iter_mut() {
        if r.subject == subject && r.level == level {
            let prev = core::mem::replace(&mut r.wrap, wrap);
            return Ok(Some(prev));
        }
    }
    if table.len() >= MAX_CAPKEYS {
        return Err(());
    }
    let mut s = Vec::new();
    s.extend_from_slice(subject);
    let mut l = Vec::new();
    l.extend_from_slice(level);
    table.push(CapKeyRecord { subject: s, level: l, wrap });
    Ok(None)
}

// Remove, returning the wrap so a persist failure can restore it.
unsafe fn capkey_take(subject: &[u8], level: &[u8]) -> Option<CapKeyWrap> {
    let table = (*core::ptr::addr_of_mut!(CAPKEYS)).as_mut()?;
    let idx = table
        .iter()
        .position(|r| r.subject == subject && r.level == level)?;
    Some(table.remove(idx).wrap)
}

// The rollback twin of capkey_set / capkey_take: None -> remove, Some -> put
// back. Best-effort (a full table cannot refuse a restore of a record it just
// held; an uninitialized table never reaches here).
unsafe fn capkey_restore(subject: &[u8], level: &[u8], prev: Option<CapKeyWrap>) {
    match prev {
        Some(w) => {
            let _ = capkey_set(subject, level, w);
        }
        None => {
            let _ = capkey_take(subject, level);
        }
    }
}

unsafe fn eligibility_count() -> usize {
    match (*core::ptr::addr_of!(ELIGIBILITY)).as_ref() {
        Some(e) => e.len(),
        None => 0,
    }
}

unsafe fn eligibility_has(kind: u8, subject: &[u8], level: &[u8]) -> bool {
    match (*core::ptr::addr_of!(ELIGIBILITY)).as_ref() {
        Some(e) => e
            .iter()
            .any(|r| r.subject_kind == kind && r.subject == subject && r.level == level),
        None => false,
    }
}

// Append an eligibility record (caller checked non-duplicate + bound). false on
// uninitialized table.
unsafe fn eligibility_push(kind: u8, subject: &[u8], level: &[u8]) -> bool {
    let table = match (*core::ptr::addr_of_mut!(ELIGIBILITY)).as_mut() {
        Some(t) => t,
        None => return false,
    };
    let mut s = Vec::new();
    s.extend_from_slice(subject);
    let mut l = Vec::new();
    l.extend_from_slice(level);
    table.push(EligibilityRecord { subject_kind: kind, subject: s, level: l });
    true
}

unsafe fn eligibility_pop_last() {
    if let Some(t) = (*core::ptr::addr_of_mut!(ELIGIBILITY)).as_mut() {
        let _ = t.pop();
    }
}

// Remove the matching record; returns true if one was removed.
unsafe fn eligibility_remove(kind: u8, subject: &[u8], level: &[u8]) -> bool {
    let table = match (*core::ptr::addr_of_mut!(ELIGIBILITY)).as_mut() {
        Some(t) => t,
        None => return false,
    };
    let before = table.len();
    table.retain(|r| !(r.subject_kind == kind && r.subject == subject && r.level == level));
    table.len() != before
}

unsafe fn group_gid_by_name(name: &[u8]) -> Option<u32> {
    match (*core::ptr::addr_of!(GROUPS)).as_ref() {
        Some(g) => g.iter().find(|r| r.name == name).map(|r| r.gid),
        None => None,
    }
}

// Is `user` eligible for `level`? Direct user eligibility OR group eligibility
// (a group record whose gid is one of the user's groups -- primary_gid or a
// supp_gid; like group membership, §5.7).
unsafe fn user_eligible_for(user: &[u8], level: &[u8]) -> bool {
    let table = match (*core::ptr::addr_of!(ELIGIBILITY)).as_ref() {
        Some(e) => e,
        None => return false,
    };
    let urec = user_states_find(user);
    for r in table {
        if r.level != level {
            continue;
        }
        match r.subject_kind {
            SUBJECT_KIND_USER => {
                if r.subject == user {
                    return true;
                }
            }
            SUBJECT_KIND_GROUP => {
                if let Some(ref u) = urec {
                    if let Some(gid) = group_gid_by_name(&r.subject) {
                        if u.primary_gid == gid || u.supp_gids.iter().any(|g| *g == gid) {
                            return true;
                        }
                    }
                }
            }
            _ => {}
        }
    }
    false
}

// Serialize (v2): the eligibility records, then the key records. `count` in
// the header is the TOTAL record count.
unsafe fn clearance_db_serialize() -> Vec<u8> {
    let table = match (*core::ptr::addr_of!(ELIGIBILITY)).as_ref() {
        Some(t) => t,
        None => return Vec::new(),
    };
    let keys = match (*core::ptr::addr_of!(CAPKEYS)).as_ref() {
        Some(k) => k,
        None => return Vec::new(),
    };
    let mut out = Vec::new();
    out.extend_from_slice(&CORVUS_MAGIC.to_le_bytes());
    out.extend_from_slice(&CLEARANCE_DB_VERSION.to_le_bytes());
    out.extend_from_slice(&((table.len() + keys.len()) as u32).to_le_bytes());
    for r in table {
        out.push(r.subject_kind);
        out.push(r.subject.len() as u8);
        out.push(r.level.len() as u8);
        out.extend_from_slice(&r.subject);
        out.extend_from_slice(&r.level);
    }
    for k in keys {
        out.push(RECORD_KIND_KEY);
        out.push(k.subject.len() as u8);
        out.push(k.level.len() as u8);
        out.extend_from_slice(&k.subject);
        out.extend_from_slice(&k.level);
        out.extend_from_slice(&(CAPKEY_WRAP_LEN as u16).to_le_bytes());
        out.extend_from_slice(&k.wrap.to_bytes());
    }
    out
}

// Parse clearance.db into ELIGIBILITY + CAPKEYS. Every length is bounds-checked
// against the remaining buffer; any malformed/truncated record (or trailing
// bytes) fails the WHOLE load closed (the identity_db_parse discipline). A v1
// file carries eligibility records only; a key record (kind 2) is admitted
// only under v2, and its wrap must parse (magic / version / the cost envelope)
// or the load fails -- a tampered verifier is never carried live.
unsafe fn clearance_db_parse(blob: &[u8]) -> bool {
    if blob.len() < CLEARANCE_DB_HEADER_LEN {
        return false;
    }
    let magic = u32::from_le_bytes([blob[0], blob[1], blob[2], blob[3]]);
    let version = u32::from_le_bytes([blob[4], blob[5], blob[6], blob[7]]);
    if magic != CORVUS_MAGIC
        || (version != CLEARANCE_DB_VERSION_V1 && version != CLEARANCE_DB_VERSION)
    {
        return false;
    }
    let count = u32::from_le_bytes([blob[8], blob[9], blob[10], blob[11]]) as usize;
    if count > MAX_ELIGIBILITY + MAX_CAPKEYS {
        return false;
    }
    let mut recs: Vec<EligibilityRecord> = Vec::new();
    let mut keys: Vec<CapKeyRecord> = Vec::new();
    let mut off = CLEARANCE_DB_HEADER_LEN;
    for _ in 0..count {
        if off + 3 > blob.len() {
            return false;
        }
        let kind = blob[off];
        let sl = blob[off + 1] as usize;
        let ll = blob[off + 2] as usize;
        off += 3;
        let is_key = kind == RECORD_KIND_KEY && version == CLEARANCE_DB_VERSION;
        if !is_key && kind != SUBJECT_KIND_USER && kind != SUBJECT_KIND_GROUP {
            return false;
        }
        if sl == 0 || sl > MAX_USER_LEN || ll == 0 || ll > MAX_LEVEL_LEN {
            return false;
        }
        if off + sl + ll > blob.len() {
            return false;
        }
        let mut subject = Vec::with_capacity(sl);
        subject.extend_from_slice(&blob[off..off + sl]);
        off += sl;
        let mut level = Vec::with_capacity(ll);
        level.extend_from_slice(&blob[off..off + ll]);
        off += ll;
        if is_key {
            if off + 2 > blob.len() {
                return false;
            }
            let wl = (blob[off] as usize) | ((blob[off + 1] as usize) << 8);
            off += 2;
            if wl != CAPKEY_WRAP_LEN || off + wl > blob.len() {
                return false;
            }
            let wrap = match CapKeyWrap::from_bytes(&blob[off..off + wl]) {
                Some(w) => w,
                None => return false,
            };
            off += wl;
            if keys.len() >= MAX_CAPKEYS {
                return false;
            }
            keys.push(CapKeyRecord { subject, level, wrap });
        } else {
            if recs.len() >= MAX_ELIGIBILITY {
                return false;
            }
            recs.push(EligibilityRecord { subject_kind: kind, subject, level });
        }
    }
    if off != blob.len() {
        return false; // trailing garbage -> fail closed
    }
    core::ptr::write(core::ptr::addr_of_mut!(ELIGIBILITY), Some(recs));
    core::ptr::write(core::ptr::addr_of_mut!(CAPKEYS), Some(keys));
    true
}

// Atomic rewrite-swap of clearance.db (mirrors identity_persist §16.6: write tmp
// -> fsync -> rename -> fsync dir). Returns false on any failure; caller rolls
// back its in-memory mutation.
unsafe fn clearance_persist() -> bool {
    let blob = clearance_db_serialize();
    if blob.len() < CLEARANCE_DB_HEADER_LEN {
        return false;
    }
    let _ = t_unlink(T_WALK_OPEN_FROM_ROOT, CLEARANCE_DB_TMP.as_ptr(), CLEARANCE_DB_TMP.len(), 0);
    let tf = t_walk_create(T_WALK_OPEN_FROM_ROOT, CLEARANCE_DB_TMP.as_ptr(),
                           CLEARANCE_DB_TMP.len(), T_OWRITE, 0o600);
    if tf < 0 {
        return false;
    }
    let wrote = write_all_fd(tf, &blob);
    let synced = t_fsync(tf, 0) == 0;
    let _ = t_close(tf);
    if !wrote || !synced {
        let _ = t_unlink(T_WALK_OPEN_FROM_ROOT, CLEARANCE_DB_TMP.as_ptr(), CLEARANCE_DB_TMP.len(), 0);
        return false;
    }
    if t_rename(T_WALK_OPEN_FROM_ROOT, CLEARANCE_DB_TMP.as_ptr(), CLEARANCE_DB_TMP.len(),
                T_WALK_OPEN_FROM_ROOT, CLEARANCE_DB.as_ptr(), CLEARANCE_DB.len()) != 0 {
        let _ = t_unlink(T_WALK_OPEN_FROM_ROOT, CLEARANCE_DB_TMP.as_ptr(), CLEARANCE_DB_TMP.len(), 0);
        return false;
    }
    let _ = t_fsync(STORAGE_ROOT_FD, 0);
    true
}

// Boot-time load (after identity_load, so user/group resolution works). Absent
// clearance.db -> fresh install (empty eligibility). Present-but-corrupt -> fail
// closed (an attacker who corrupts clearance.db must not erase eligibility
// records by making the file unreadable -- corvus refuses to start).
unsafe fn clearance_load() -> bool {
    let _ = t_unlink(T_WALK_OPEN_FROM_ROOT, CLEARANCE_DB_TMP.as_ptr(), CLEARANCE_DB_TMP.len(), 0);
    let dbf = t_walk_open(T_WALK_OPEN_FROM_ROOT, CLEARANCE_DB.as_ptr(), CLEARANCE_DB.len(), T_OREAD);
    if dbf < 0 {
        // Absent -> fresh install: the table is already the empty Vec from
        // clearance_init. The backfill MUST still run here (audit F3): an
        // absent clearance.db is EXACTLY the state a failed first seed leaves
        // (every clearance_persist failure arm unlinks the tmp), so returning
        // early would make handle_user_create's "heals next boot" contract
        // false on the one path that cites it -- and would also strand a pool
        // restored from an identity.db with no clearance.db.
        return clearance_backfill_jit();
    }
    let mut blob: Vec<u8> = Vec::new();
    let read_ok = read_to_end_fd(dbf, &mut blob, CLEARANCE_DB_MAX);
    let _ = t_close(dbf);
    if !read_ok || !clearance_db_parse(&blob) {
        return false;
    }
    t_putstr("corvus: clearance.db loaded (");
    let mut nbuf = [0u8; 12];
    t_putstr(usize_dec(eligibility_count(), &mut nbuf));
    t_putstr(" eligibility records, ");
    t_putstr(usize_dec(capkey_count(), &mut nbuf));
    t_putstr(" key records)\n");
    clearance_backfill_jit()
}

// #163: give every user in USER_STATES the default jit eligibility it lacks.
// Runs on BOTH clearance_load arms (present and absent db). Additive and
// presence-checked, so it is idempotent across boots; persisted once iff
// anything was added. Always returns true -- a backfill failure degrades
// (fail-closed: no eligibility) and retries next boot, it never refuses the
// boot the way a corrupt clearance.db does.
unsafe fn clearance_backfill_jit() -> bool {
    // Clone the names first: this ends the USER_STATES shared borrow before
    // the loop mutates ELIGIBILITY (disjoint statics, no aliasing).
    let names: Vec<Vec<u8>> = match (*core::ptr::addr_of!(USER_STATES)).as_ref() {
        Some(users) => users.iter().map(|u| u.user.clone()).collect(),
        None => Vec::new(),
    };
    let mut nbuf = [0u8; 12];
    let mut seeded = 0usize;
    let mut at_bound = 0usize;
    for name in &names {
        if eligibility_has(SUBJECT_KIND_USER, name, SEED_LEVEL_JIT) {
            continue;
        }
        if eligibility_count() >= MAX_ELIGIBILITY {
            at_bound += 1;   // audit F4: never skip silently
            continue;
        }
        if eligibility_push(SUBJECT_KIND_USER, name, SEED_LEVEL_JIT) {
            seeded += 1;
        }
    }
    if at_bound > 0 {
        t_putstr("corvus: WARN eligibility table FULL -- jit seed skipped for ");
        t_putstr(usize_dec(at_bound, &mut nbuf));
        t_putstr(" user(s)\n");
    }
    if seeded > 0 {
        if clearance_persist() {
            t_putstr("corvus: jit eligibility backfilled for ");
            t_putstr(usize_dec(seeded, &mut nbuf));
            t_putstr(" user(s)\n");
        } else {
            t_putstr("corvus: WARN jit backfill not persisted (in-memory this boot; retried next boot)\n");
        }
    }
    true
}

// Monotonic legate-session-id counter. Nonzero (0 = the kernel not-a-legate
// sentinel), fits u32 (the kernel grant requires session_id != 0 && <= u32).
// Ephemeral -- not persisted; it is audit attribution WITHIN a boot (§3.1).
static mut NEXT_LEGATE_SESSION: u32 = 1;

unsafe fn next_legate_session() -> u32 {
    let s = NEXT_LEGATE_SESSION;
    // A u32 of activations in one boot is unreachable; saturate rather than wrap
    // to 0 (which the kernel would reject as not-a-legate).
    if NEXT_LEGATE_SESSION < u32::MAX {
        NEXT_LEGATE_SESSION += 1;
    }
    s
}

// =============================================================================
// Wire constants — CORVUS-DESIGN.md §6.4.
// =============================================================================

const VERB_AUTH: u8 = 1;
const VERB_SESSION_CLOSE: u8 = 3;
const VERB_UNWRAP: u8 = 4;
const VERB_USER_CREATE: u8 = 5;
const VERB_ADMIN_ELEVATE: u8 = 7;
// A-5c recovery: reset a subject's passphrase via the paper recovery phrase.
// subject_kind discriminates user (1; A-5c-a) vs system/hostowner-c (0; A-5c-b).
const VERB_RECOVER: u8 = 8;
const VERB_WRAP: u8 = 10;
// A-1b identity verbs (CORVUS-DESIGN.md §16.7). RESOLVE_* are ungated
// (getpwuid/getpwnam equivalents -- an id<->name map is not secret).
// GROUP_CREATE is CAP_HOSTOWNER-gated (groupadd is privileged).
const VERB_RESOLVE_ID: u8 = 11;
const VERB_RESOLVE_NAME: u8 = 12;
const VERB_GROUP_CREATE: u8 = 13;
// A-4a clearance/legate verbs (CORVUS-DESIGN.md §6.4 + IDENTITY-DESIGN §9.8).
// CLEARANCE_LIST / CLEARANCE_ACTIVATE are user-facing (gated by a valid session
// token); CLEARANCE_GRANT / CLEARANCE_REVOKE are CAP_HOSTOWNER-gated eligibility
// admin (the hostowner decides who may become which legate -- like GROUP_CREATE).
const VERB_CLEARANCE_LIST: u8 = 14;
const VERB_CLEARANCE_ACTIVATE: u8 = 15;
const VERB_CLEARANCE_GRANT: u8 = 16;
const VERB_CLEARANCE_REVOKE: u8 = 17;
// #139: the SELF form of CLEARANCE_ACTIVATE -- same policy, identity from the
// connection's kernel-stamped principal instead of a bearer token. A DISTINCT
// verb rather than a magic-value branch inside 15: the two forms differ in
// their authorization gate, and a second authorization path should be nameable
// on the wire and separately auditable, not discriminated by payload length
// (the /cap/grant 16-vs-32 shape, which works but hides what it is doing).
const VERB_CLEARANCE_ACTIVATE_SELF: u8 = 18;
// IM-3 (IMPERIUM-DESIGN.md 11.5): the lex curiata. The verb-18 payload shape
// for a DISTINCT_SECRET level; the reply is DEFERRED until the SAK episode
// concludes (the requester's read parks), or immediate on a gate refusal.
const VERB_IMPERIUM_REQUEST: u8 = 19;
// IM-4 (IMPERIUM-DESIGN.md 11.6): the SELF form of CLEARANCE_LIST -- identity
// from the connection's kernel-stamped principal (SYS_SRV_PEER, the verb-18
// shape), no bearer token. Read-only disclosure of the caller's OWN eligibility
// ladder ("what could I become"); it backs `imperium --list`'s eligibility half.
// The reply is byte-identical to verb 14; the two share `emit_eligible_levels`.
const VERB_CLEARANCE_LIST_SELF: u8 = 20;

// The system passphrase is no longer a corvus-side constant (A-5c-b): ADMIN_ELEVATE
// verifies a supplied passphrase by unwrapping the host-baked system-wrap (the admin
// keypair under that passphrase). corvus holds only the wrap, never the passphrase --
// so there is nothing to byte-compare against. See SYSTEM_KEYPAIR_WRAP + system_identity_load.

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
const STATUS_RATE_LIMITED: u8 = 4;
const STATUS_BAD_FORMAT: u8 = 5;
const STATUS_INTERNAL_ERROR: u8 = 6;
// IM-3 (wire-additive): the deferred reply's own outcomes. TIMEOUT = no SAK
// within the request window, or no key within the prompt bound; BUSY = a
// request is already pending (one slot system-wide).
const STATUS_TIMEOUT: u8 = 7;
const STATUS_BUSY: u8 = 8;

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
// USER_CREATE (extended, A-1b): user_len(1) + user + pass_len(2) + pass +
// backend(1) + supp_gid_count(1) + supp_gids(4 * PROC_SUPP_GIDS_MAX).
const _: () = assert!(
    MAX_PAYLOAD_LEN >= 1 + MAX_USER_LEN + 2 + MAX_PASS_LEN + 1 + 1 + 4 * PROC_SUPP_GIDS_MAX,
    "extended USER_CREATE payload must fit MAX_PAYLOAD_LEN"
);
// RECOVER(user): subject_kind(1) + user_len(1) + user + phrase_len(2) + phrase
// + new_pass_len(2) + new_passphrase.
const _: () = assert!(
    MAX_PAYLOAD_LEN >= 1 + 1 + MAX_USER_LEN + 2 + RECOVERY_PHRASE_MAX + 2 + MAX_PASS_LEN,
    "RECOVER(user) payload must fit MAX_PAYLOAD_LEN"
);
// USER_CREATE OK (id + gid + phrase_len + phrase) and RECOVER OK (phrase_len +
// phrase) must fit the staged response frame.
const _: () = assert!(
    MAX_RESPONSE_FRAME >= RESP_HDR_LEN + 8 + 2 + RECOVERY_PHRASE_MAX,
    "phrase-bearing OK responses must fit MAX_RESPONSE_FRAME"
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
    // A-5b: conn_id of the connection that ran the successful AUTH. The
    // global AUTH session is cleared on THIS connection's close (close_conn)
    // or an explicit SESSION_CLOSE -- never on a non-owning bearer-token
    // connection's close, so a 2nd Proc presenting the token for an UNWRAP
    // (the section 6.3 forward; e.g. the A-5b storage coordinator pulling a
    // home DEK) cannot wipe a live login session by disconnecting. 0 = none.
    owner_conn_id: u64,
}

static mut SESSION: Session = Session {
    active: false,
    user_len: 0,
    user: [0; MAX_USER_LEN],
    token: [0; TOKEN_LEN],
    keypair: [0; KEYPAIR_LEN],
    owner_conn_id: 0,
};

unsafe fn session_active() -> bool {
    core::ptr::read(core::ptr::addr_of!(SESSION.active))
}

unsafe fn session_install(user: &[u8], token: &[u8; TOKEN_LEN], keypair: &[u8; KEYPAIR_LEN],
                          owner_conn_id: u64) {
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
    core::ptr::write(core::ptr::addr_of_mut!((*s).owner_conn_id), owner_conn_id);
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
    core::ptr::write(core::ptr::addr_of_mut!((*s).owner_conn_id), 0);
}

// A-5b: the conn_id that owns the live AUTH session (0 = none). The owner is
// the connection that ran AUTH; only its close clears the session.
unsafe fn session_owner_conn_id() -> u64 {
    core::ptr::read(core::ptr::addr_of!(SESSION.owner_conn_id))
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

// Format a usize as decimal into `buf`; returns the populated &str. Boot
// diagnostics only (no heap).
fn usize_dec(mut n: usize, buf: &mut [u8; 12]) -> &str {
    if n == 0 {
        buf[0] = b'0';
        return unsafe { core::str::from_utf8_unchecked(&buf[..1]) };
    }
    let mut tmp = [0u8; 12];
    let mut i = 0;
    while n > 0 && i < tmp.len() {
        tmp[i] = b'0' + (n % 10) as u8;
        n /= 10;
        i += 1;
    }
    for j in 0..i {
        buf[j] = tmp[i - 1 - j];
    }
    unsafe { core::str::from_utf8_unchecked(&buf[..i]) }
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
unsafe fn handle_auth(owner_conn_id: u64, payload: &[u8], response: &mut Vec<u8>) {
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

    // Refuse AUTH while a session is bound.
    //
    // Read this as an IMPLEMENTATION narrowing, not a spec requirement -- the
    // distinction was lost here and #139 was the bill. corvus.tla AuthSuccess
    // says `~(\E s \in sessions : s.owner_proc = p)`: no session owned by THIS
    // Proc. The model permits concurrent sessions from different Procs. What
    // runs below is the single global SESSION slot (see the struct's note), so
    // it refuses AUTH from ANY Proc while ANY session exists -- strictly
    // stricter than the model, and the elided `s.owner_proc = p` is exactly
    // where the strictness hides.
    //
    // The narrowing was sound for the world it was written in (joey the only
    // console-attached Proc, one connection per peer). Login and user programs
    // arrived later and the bound stayed; a correct bound whose reason had
    // expired. The consequence was not a refused login but a structural one: no
    // post-login program could ever obtain a session token, so the token-gated
    // CLEARANCE_ACTIVATE (verb 15) became unreachable for every user-launched
    // program -- CAP_JIT included, i.e. all of GL.
    //
    // Verb 18 (CLEARANCE_ACTIVATE_SELF) routes around it without touching this
    // gate, because widening it is the larger change: per-connection sessions
    // re-open the A-5b cross-session-wipe question that owner_conn_id closed.
    // That lift stays available (it is what the model already describes) and is
    // the right move if a second verb ever needs the same escape.
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

    session_install(user, &token, &keypair, owner_conn_id);
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

// Like peer_live_caps but returns the full LIVE peer snapshot (RW-6 R2-F2). The
// privileged console-gated verbs (ADMIN_ELEVATE / RECOVER(system)) must gate on the
// live console bit + stripes, NOT the at-accept cache: console attachment is revocable
// on a live Proc (kernel proc_revoke_console_attached), so a cached snapshot can be
// stale in either direction. None on a dead peer or syscall error -- fail closed.
unsafe fn peer_live_info(handle: i64) -> Option<TSrvPeerInfo> {
    let mut info = TSrvPeerInfo::default();
    if t_srv_peer(handle, &mut info) != 0 {
        return None;
    }
    if info.alive == 0 {
        return None;
    }
    Some(info)
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
    // A-5b (#828 B-F1): the username becomes BOTH the per-user dataset name AND
    // the login proxy's `--datasets-allowed ds:<user>` glob pattern -- the sole
    // load-bearing attach gate for the encrypted home. Constrain it to a safe
    // charset at this mint point (the authoritative identity chokepoint) so no
    // derived pattern can ever carry a glob metacharacter ('*'/'**') or a path
    // separator ('/') that would widen the gate beyond a single literal name.
    for &b in user_slice {
        if !(b.is_ascii_alphanumeric() || b == b'.' || b == b'_' || b == b'-') {
            return stage_response(response, STATUS_BAD_FORMAT, &[]);
        }
    }
    // A-5c-c (audit F1): reserve the system AD subject. The system identity wraps
    // (system-wrap / system-recovery-wrap) key their AEAD AD on the fixed subject
    // SYSTEM_WRAP_SUBJECT (b"system"); a user literally named "system" would mint
    // users/system/{hybrid,recovery}.corvus with byte-identical AD, defeating the
    // AD-domain binding the file-swap defense rests on (C-27). The KEK + the file
    // path already differ, so this is defense-in-depth -- but rejecting the name
    // at the mint chokepoint keeps the AD-uniqueness property the design claims.
    if user_slice == SYSTEM_WRAP_SUBJECT {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let pass_len = (payload[1 + user_len] as usize) | ((payload[2 + user_len] as usize) << 8);
    if pass_len == 0 || pass_len > MAX_PASS_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    // Through the backend byte. Then the A-1b append-only extension:
    //   [base]   supp_gid_count u8 (absent => 0)
    //   [base+1] supp_gids[count] u32 LE
    let backend_off = 1 + user_len + 2 + pass_len;
    if payload.len() < backend_off + 1 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let passphrase = &payload[1 + user_len + 2..backend_off];
    let backend = payload[backend_off];
    if backend != BACKEND_ID_PASSPHRASE {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let ext_off = backend_off + 1;
    let mut supp_gids: Vec<u32> = Vec::new();
    if payload.len() != ext_off {
        // Extension present: exactly supp_gid_count + count*4 trailing bytes.
        if payload.len() < ext_off + 1 {
            return stage_response(response, STATUS_BAD_FORMAT, &[]);
        }
        let supp_n = payload[ext_off] as usize;
        if supp_n > PROC_SUPP_GIDS_MAX {
            return stage_response(response, STATUS_BAD_FORMAT, &[]);
        }
        if payload.len() != ext_off + 1 + 4 * supp_n {
            return stage_response(response, STATUS_BAD_FORMAT, &[]);
        }
        let mut o = ext_off + 1;
        for _ in 0..supp_n {
            let g = u32::from_le_bytes([payload[o], payload[o + 1], payload[o + 2], payload[o + 3]]);
            // A reserved sentinel is never a valid group membership (I-22: a
            // reserved id confers no authority and must not enter the table).
            if g == PRINCIPAL_INVALID || g >= PRINCIPAL_SYSTEM {
                return stage_response(response, STATUS_BAD_FORMAT, &[]);
            }
            supp_gids.push(g);
            o += 4;
        }
    }

    if user_states_find(user_slice).is_some() {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }
    if user_states_count() >= MAX_USERS {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    // Cap the live group count before the UPG push below (RW-6 R3-F1, the a1b-F1
    // sibling). groups_count() counts standalone groups AND per-user UPGs; a1b-F1
    // capped handle_group_create but this UPG side-writer was unguarded, so creating
    // 2*MAX_USERS standalone groups then one user persisted group_count==513, which
    // identity_db_parse (cap 2*MAX_USERS==512) rejects -> next-boot FATAL boot-brick.
    // 256 UPG + up-to-256 standalone == 512 exactly, so a legitimate 256-user
    // deployment never false-fails.
    if groups_count() >= 2 * MAX_USERS {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    // A UPG group named after the user must not already exist (it would be an
    // orphan group with no owner). Fail closed.
    if group_name_exists(user_slice) {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }

    let mut user_vec: Vec<u8> = Vec::new();
    user_vec.extend_from_slice(user_slice);

    let mut keypair_plain = match generate_hybrid_keypair(&mut ThylaRng) {
        Some(kp) => kp,
        None => return stage_response(response, STATUS_INTERNAL_ERROR, &[]),
    };

    // Wrap the keypair under the passphrase (hybrid.corvus) AND mint the A-5c
    // recovery keyslot (recovery.corvus) over the SAME keypair -- mandatory
    // enrollment. The 24-word phrase is returned once in the OK response.
    let kw = match wrap_keypair_passphrase(&mut ThylaRng, user_slice, passphrase, &keypair_plain) {
        Some(k) => k,
        None => {
            wipe(&mut keypair_plain);
            return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
        }
    };
    let mut recovery_entropy = [0u8; RECOVERY_ENTROPY_BYTES];
    if t_getrandom(recovery_entropy.as_mut_ptr(), RECOVERY_ENTROPY_BYTES, 0)
        != RECOVERY_ENTROPY_BYTES as i64
    {
        wipe(&mut keypair_plain);
        wipe(&mut recovery_entropy);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    let recovery_rw = match make_recovery_wrap(&mut ThylaRng, user_slice, &recovery_entropy, &keypair_plain) {
        Some(r) => r,
        None => {
            wipe(&mut keypair_plain);
            wipe(&mut recovery_entropy);
            return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
        }
    };
    wipe(&mut keypair_plain);

    // UPG: principal_id == primary_gid == next_auto_id++ (§16.3). A burned id
    // (counter bumped but a later step fails) is never reused -- monotonic.
    let principal_id = match alloc_auto_id() {
        Some(id) => id,
        None => {
            wipe(&mut recovery_entropy);
            return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
        }
    };
    let primary_gid = principal_id;

    let record = CorvusUserState {
        user: user_vec.clone(),
        principal_id,
        primary_gid,
        supp_gids,
        backend: BACKEND_ID_PASSPHRASE,
        t_cost: ARGON2_T_COST,
        m_cost_kib: ARGON2_M_COST_KIB,
        parallelism: ARGON2_PARALLELISM,
        salt: kw.salt,
        nonce: kw.nonce,
        ciphertext: kw.ciphertext,
        tag: kw.tag,
    };

    // Persist (§16.6): BOTH wraps (write-once, fsync'd) BEFORE the identity.db
    // commit, so a crash leaves orphan wrap(s) (harmless), never an identity
    // record pointing at a missing wrap. Open users/ once.
    let users_fd = mkdir_opath(T_WALK_OPEN_FROM_ROOT, USERS_DIR);
    if users_fd < 0 {
        wipe(&mut recovery_entropy);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    let wrap_ok = persist_keypair_wrap(users_fd, &user_vec, &record)
        && persist_recovery_wrap(users_fd, &user_vec, &recovery_rw);
    let _ = t_close(users_fd);
    if !wrap_ok {
        wipe(&mut recovery_entropy);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }

    // In-memory commit (append) + UPG group, THEN identity.db rewrite-swap (the
    // atomic commit point). On swap failure, roll back the appends -- the new
    // user is then neither in identity.db nor in the live table.
    if !user_states_insert(record) {
        wipe(&mut recovery_entropy);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    if !groups_push(principal_id, &user_vec) {
        user_states_pop_last();
        wipe(&mut recovery_entropy);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    if !identity_persist() {
        groups_pop_last();
        user_states_pop_last();
        wipe(&mut recovery_entropy);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }

    // Committed. Seed the user-default jit eligibility (#163). Best-effort by
    // design: the user is already durably created, and clearance_backfill_jit
    // (run on BOTH clearance_load arms) heals any miss on the next boot -- so a
    // failure here degrades, never rolls back the create.
    //
    // The no-rollback-on-persist-failure is licensed SPECIFICALLY by that
    // backfill re-creating THIS level (audit F5) and does not generalize: every
    // other mutation here rolls back (handle_clearance_grant pops, revoke
    // re-pushes), because a memory-grants/disk-lacks record with no healer
    // would survive until the next mutation and then vanish at reboot.
    if eligibility_has(SUBJECT_KIND_USER, &user_vec, SEED_LEVEL_JIT) {
        // already seeded (re-created name reusing a live record) -- nothing to do
    } else if eligibility_count() >= MAX_ELIGIBILITY {
        // audit F4: a full table must not silently mint jit-less users.
        t_putstr("corvus: WARN eligibility table FULL -- new user has no jit seed\n");
    } else if eligibility_push(SUBJECT_KIND_USER, &user_vec, SEED_LEVEL_JIT)
        && !clearance_persist()
    {
        t_putstr("corvus: WARN jit seed not persisted (backfill heals next boot)\n");
    }

    // Register the dataset-owner mapping (users/<name> -> <name>) so
    // WRAP/UNWRAP gate correctly; done after the commit so a rolled-back create
    // leaves no stale owner.
    let mut dataset = Vec::new();
    dataset.extend_from_slice(b"users/");
    dataset.extend_from_slice(&user_vec);
    dataset_owner_register(&dataset, &user_vec);

    // OK reply: principal_id u32 + primary_gid u32 + phrase_len u16 + phrase
    // (A-5c mandatory enrollment; the caller displays the phrase once).
    let mut phrase = bip39_encode(&recovery_entropy);
    wipe(&mut recovery_entropy);
    let mut ok: Vec<u8> = Vec::with_capacity(8 + 2 + phrase.len());
    ok.extend_from_slice(&principal_id.to_le_bytes());
    ok.extend_from_slice(&primary_gid.to_le_bytes());
    ok.push((phrase.len() & 0xff) as u8);
    ok.push(((phrase.len() >> 8) & 0xff) as u8);
    ok.extend_from_slice(&phrase);
    stage_response(response, STATUS_OK, &ok);
    wipe(&mut phrase);
    wipe(&mut ok);
}

// handle_recover — RECOVER verb (verb_id=8; A-5c). subject_kind=1 (user): reset
// a user's passphrase from their paper recovery phrase, with NO session token
// and NO capability -- the user lost the passphrase, so phrase-knowledge + the
// rate limit are the entire gate. On success the keypair is re-wrapped under the
// new passphrase AND a FRESH recovery phrase is rolled (the used one retired);
// the fresh phrase is returned. The keypair value is unchanged, so every DEK
// envelope stays valid (no Stratum/kernel surface). subject_kind=0 (system /
// hostowner-c) is A-5c-b and is rejected here.
//
// Payload (subject_kind=1):
//   [0]              subject_kind u8 (1)
//   [1]              user_len u8 (1..=MAX_USER_LEN)
//   [2..2+ul]        user
//   [2+ul..4+ul]     phrase_len u16 LE (1..=RECOVERY_PHRASE_MAX)
//   [4+ul..]         phrase (phrase_len)
//   [..2]            new_pass_len u16 LE (1..=MAX_PASS_LEN)
//   [..]             new_passphrase (new_pass_len)
//
// OK reply: phrase_len u16 LE + fresh_phrase.
unsafe fn handle_recover(
    handle: i64,
    payload: &[u8],
    response: &mut Vec<u8>,
) {
    if payload.is_empty() {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    if payload[0] == 0 {
        // subject_kind=0 (system / hostowner-c): recover the system identity.
        return handle_recover_system(handle, payload, response);
    }
    if payload[0] != 1 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    if payload.len() < 2 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let user_len = payload[1] as usize;
    if user_len == 0 || user_len > MAX_USER_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let user_off = 2;
    if payload.len() < user_off + user_len + 2 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let user = &payload[user_off..user_off + user_len];
    let phrase_len_off = user_off + user_len;
    let phrase_len =
        (payload[phrase_len_off] as usize) | ((payload[phrase_len_off + 1] as usize) << 8);
    if phrase_len == 0 || phrase_len > RECOVERY_PHRASE_MAX {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let phrase_off = phrase_len_off + 2;
    if payload.len() < phrase_off + phrase_len + 2 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let phrase = &payload[phrase_off..phrase_off + phrase_len];
    let np_len_off = phrase_off + phrase_len;
    let new_pass_len = (payload[np_len_off] as usize) | ((payload[np_len_off + 1] as usize) << 8);
    if new_pass_len == 0 || new_pass_len > MAX_PASS_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let np_off = np_len_off + 2;
    if payload.len() != np_off + new_pass_len {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let new_passphrase = &payload[np_off..np_off + new_pass_len];

    // The subject must exist. A nonexistent user fails cheap (no KDF, no rate-
    // limit charge), so a name-probe cannot DoS or distinguish on cost.
    let state = match user_states_find(user) {
        Some(s) => s,
        None => return stage_response(response, STATUS_BAD_AUTH, &[]),
    };

    // Rate-limit the EXPENSIVE path BEFORE any KDF.
    if recover_fail_count(user) >= RECOVER_FAIL_MAX {
        return stage_response(response, STATUS_RATE_LIMITED, &[]);
    }

    let users_fd = mkdir_opath(T_WALK_OPEN_FROM_ROOT, USERS_DIR);
    if users_fd < 0 {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }

    // Load the recovery keyslot. Absent -> the user predates A-5c enrollment (or
    // a corrupt wrap) -> BadAuth (cheap; no charge).
    let rw = match load_recovery_wrap(users_fd, user) {
        Some(r) => r,
        None => {
            let _ = t_close(users_fd);
            return stage_response(response, STATUS_BAD_AUTH, &[]);
        }
    };

    // Decode + checksum-verify the phrase. A typo aborts BEFORE the KDF and is
    // NOT charged to the rate limit.
    let mut entropy = match bip39_decode(phrase) {
        Some(e) => e,
        None => {
            let _ = t_close(users_fd);
            return stage_response(response, STATUS_BAD_AUTH, &[]);
        }
    };

    // Derive the recovery KEK + unwrap (the expensive, charged step).
    let unwrap = unwrap_recovery(user, &entropy, &rw);
    wipe(&mut entropy);
    let mut keypair = match unwrap {
        Some(kp) => kp,
        None => {
            // Checksum-valid but wrong phrase: a crafted/guessed attempt.
            recover_fail_inc(user);
            let _ = t_close(users_fd);
            return stage_response(response, STATUS_BAD_AUTH, &[]);
        }
    };

    // SUCCESS. Build BOTH new wraps in memory before touching disk, so the only
    // failure points are the two rename-swaps.
    let kw = match wrap_keypair_passphrase(&mut ThylaRng, user, new_passphrase, &keypair) {
        Some(k) => k,
        None => {
            wipe(&mut keypair);
            let _ = t_close(users_fd);
            return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
        }
    };
    let mut new_rec = state;
    new_rec.t_cost = ARGON2_T_COST;
    new_rec.m_cost_kib = ARGON2_M_COST_KIB;
    new_rec.parallelism = ARGON2_PARALLELISM;
    new_rec.salt = kw.salt;
    new_rec.nonce = kw.nonce;
    new_rec.ciphertext = kw.ciphertext;
    new_rec.tag = kw.tag;
    let new_hybrid_blob = new_rec.to_bytes();

    let mut new_entropy = [0u8; RECOVERY_ENTROPY_BYTES];
    if t_getrandom(new_entropy.as_mut_ptr(), RECOVERY_ENTROPY_BYTES, 0)
        != RECOVERY_ENTROPY_BYTES as i64
    {
        wipe(&mut keypair);
        wipe(&mut new_entropy);
        let _ = t_close(users_fd);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    let new_rw = match make_recovery_wrap(&mut ThylaRng, user, &new_entropy, &keypair) {
        Some(r) => r,
        None => {
            wipe(&mut keypair);
            wipe(&mut new_entropy);
            let _ = t_close(users_fd);
            return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
        }
    };
    wipe(&mut keypair);
    let new_recovery_blob = new_rw.to_bytes();

    let udir = mkdir_opath(users_fd, user);
    if udir < 0 {
        wipe(&mut new_entropy);
        let _ = t_close(users_fd);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    // Commit order: hybrid.corvus (the passphrase reset, the user's primary
    // goal) FIRST, then recovery.corvus (the rolled phrase). Because BOTH
    // keyslots wrap the SAME keypair, no crash can strand the user: whichever
    // file survives independently recovers the keypair. A crash after the hybrid
    // swap but before the recovery swap leaves the new passphrase live AND the
    // OLD recovery phrase still valid (recovery.corvus untouched) -- the user can
    // log in and re-run recovery. We return a fresh phrase only once it is on
    // disk.
    if !persist_wrap_swap(udir, HYBRID, HYBRID_TMP, &new_hybrid_blob) {
        wipe(&mut new_entropy);
        let _ = t_close(udir);
        let _ = t_close(users_fd);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    // The passphrase wrap is committed; reflect it in the live table NOW so a
    // subsequent AUTH (new passphrase) works without a reboot.
    user_states_update_wrap(
        user,
        ARGON2_T_COST,
        ARGON2_M_COST_KIB,
        ARGON2_PARALLELISM,
        &kw.salt,
        &kw.nonce,
        &kw.ciphertext,
        &kw.tag,
    );
    if !persist_wrap_swap(udir, RECOVERY_FILE, RECOVERY_TMP, &new_recovery_blob) {
        // Passphrase reset succeeded; the phrase roll did not. The OLD phrase is
        // still valid (recovery.corvus untouched) -> no data loss, but we did not
        // produce a durable fresh phrase, so report error rather than hand back a
        // phrase that is not on disk.
        wipe(&mut new_entropy);
        let _ = t_close(udir);
        let _ = t_close(users_fd);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    let _ = t_fsync(users_fd, 0);
    let _ = t_close(udir);
    let _ = t_close(users_fd);

    recover_fail_reset(user);

    // OK reply: phrase_len u16 LE + fresh_phrase.
    let mut phrase_out = bip39_encode(&new_entropy);
    wipe(&mut new_entropy);
    let mut out: Vec<u8> = Vec::with_capacity(2 + phrase_out.len());
    out.push((phrase_out.len() & 0xff) as u8);
    out.push(((phrase_out.len() >> 8) & 0xff) as u8);
    out.extend_from_slice(&phrase_out);
    stage_response(response, STATUS_OK, &out);
    wipe(&mut phrase_out);
    wipe(&mut out);
}

// handle_recover_system — RECOVER(subject_kind=0; A-5c-b / hostowner-c): reset the
// SYSTEM passphrase (the host-baked admin keypair's passphrase wrap) from the
// system recovery phrase. The system identity has NO USER_STATES record -- it
// lives entirely in the SYSTEM_* statics + the two root files (system-wrap +
// system-recovery-wrap), so this path is wholly distinct from handle_recover's
// user path. Gate: console-attached + the recovery phrase + the rate limit (NO
// session token, NO capability). The console requirement is the EXTRA gate over
// RECOVER(user) and mirrors ADMIN_ELEVATE -- the system identity is the most
// privileged secret, so it must never be recoverable from a non-console peer (a
// token-only or non-console path would be the bug). On success the keypair is
// re-wrapped under the new passphrase (rolling system-wrap) AND a fresh recovery
// phrase is rolled (rolling system-recovery-wrap); the keypair value is unchanged,
// so every system-wrapped envelope stays valid (no Stratum/kernel surface).
//
// Payload (subject_kind=0):
//   [0]            subject_kind u8 (0)
//   [1..3]         phrase_len u16 LE (1..=RECOVERY_PHRASE_MAX)
//   [3..3+pl]      phrase
//   [3+pl..5+pl]   new_pass_len u16 LE (1..=MAX_PASS_LEN)
//   [5+pl..]       new_passphrase
//
// OK reply: phrase_len u16 LE + fresh_phrase.
unsafe fn handle_recover_system(handle: i64, payload: &[u8], response: &mut Vec<u8>) {
    // Console gate FIRST (cheap), on the LIVE peer snapshot (RW-6 R2-F2). The system
    // identity must never be recoverable from a non-console peer (the trusted-path
    // requirement, same as ADMIN_ELEVATE), and console attachment is revocable on a
    // live Proc (kernel proc_revoke_console_attached) -- so the at-accept cache could
    // pass a peer that has since lost console. This is the ONLY console gate on this
    // path (unlike ADMIN_ELEVATE, the kernel has no /cap/use backstop here), so it must
    // be a fresh SYS_SRV_PEER query. Fail closed on a dead peer / syscall error.
    let info = match peer_live_info(handle) {
        Some(i) => i,
        None => return stage_response(response, STATUS_PERMISSION_DENIED, &[]),
    };
    if info.console == 0 {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }
    // Parse: subject_kind(1) + phrase_len(2) + phrase + new_pass_len(2) + pass.
    if payload.len() < 3 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let phrase_len = (payload[1] as usize) | ((payload[2] as usize) << 8);
    if phrase_len == 0 || phrase_len > RECOVERY_PHRASE_MAX {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let phrase_off = 3;
    if payload.len() < phrase_off + phrase_len + 2 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let phrase = &payload[phrase_off..phrase_off + phrase_len];
    let np_len_off = phrase_off + phrase_len;
    let new_pass_len = (payload[np_len_off] as usize) | ((payload[np_len_off + 1] as usize) << 8);
    if new_pass_len == 0 || new_pass_len > MAX_PASS_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let np_off = np_len_off + 2;
    if payload.len() != np_off + new_pass_len {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let new_passphrase = &payload[np_off..np_off + new_pass_len];

    let subject = SYSTEM_WRAP_SUBJECT;

    // Rate-limit the EXPENSIVE path BEFORE any KDF (same accounting as RECOVER(user):
    // a typo fails the cheap checksum and is not charged; only a checksum-valid but
    // unwrap-failed attempt is). The subject is the fixed system subject.
    if recover_fail_count(subject) >= RECOVER_FAIL_MAX {
        return stage_response(response, STATUS_RATE_LIMITED, &[]);
    }

    // The system recovery wrap must be present (system_identity_load is FATAL on
    // absent, so this is defensive -- fail closed if somehow cleared).
    if (*core::ptr::addr_of!(SYSTEM_RECOVERY_WRAP)).is_none() {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }

    // Decode + checksum the phrase. A typo aborts BEFORE the KDF, NOT charged.
    let mut entropy = match bip39_decode(phrase) {
        Some(e) => e,
        None => return stage_response(response, STATUS_BAD_AUTH, &[]),
    };

    // Derive the recovery KEK + unwrap (the expensive, charged step). The borrow
    // of the static is scoped so it ends before the rolling writes below.
    let unwrap = {
        let rw = (*core::ptr::addr_of!(SYSTEM_RECOVERY_WRAP)).as_ref().unwrap();
        unwrap_recovery(subject, &entropy, rw)
    };
    wipe(&mut entropy);
    let mut keypair = match unwrap {
        Some(kp) => kp,
        None => {
            recover_fail_inc(subject);
            return stage_response(response, STATUS_BAD_AUTH, &[]);
        }
    };

    // Build BOTH new wraps in memory before touching disk, so the only failure
    // points are the two rename-swaps.
    let new_kw = match wrap_keypair_passphrase(&mut ThylaRng, subject, new_passphrase, &keypair) {
        Some(k) => k,
        None => {
            wipe(&mut keypair);
            return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
        }
    };
    let mut new_entropy = [0u8; RECOVERY_ENTROPY_BYTES];
    if t_getrandom(new_entropy.as_mut_ptr(), RECOVERY_ENTROPY_BYTES, 0)
        != RECOVERY_ENTROPY_BYTES as i64
    {
        wipe(&mut keypair);
        wipe(&mut new_entropy);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    let new_rw = match make_recovery_wrap(&mut ThylaRng, subject, &new_entropy, &keypair) {
        Some(r) => r,
        None => {
            wipe(&mut keypair);
            wipe(&mut new_entropy);
            return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
        }
    };
    wipe(&mut keypair);

    let new_hybrid_blob = new_kw.to_bytes();
    let new_recovery_blob = new_rw.to_bytes();

    // Commit system-wrap (the passphrase reset) FIRST, then system-recovery-wrap
    // (the phrase roll). Same crash-safety argument as the user path: both wraps
    // hold the SAME keypair, so a crash between leaves the new passphrase live AND
    // the OLD phrase still valid. The system wraps live at the chroot root; fd 0
    // (STORAGE_ROOT_FD) is the root dir handle, on which create/rename/fsync are
    // all valid (the same handle identity_persist fsyncs).
    if !persist_wrap_swap(STORAGE_ROOT_FD, SYSTEM_WRAP_FILE, SYSTEM_WRAP_TMP, &new_hybrid_blob) {
        wipe(&mut new_entropy);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    // Reflect the new passphrase wrap in the live static NOW so a subsequent
    // ADMIN_ELEVATE (new passphrase) works without a reboot. KeypairWrap is a
    // non-Drop POD (fixed arrays only), so overwriting via ptr::write leaks nothing.
    core::ptr::write(core::ptr::addr_of_mut!(SYSTEM_KEYPAIR_WRAP), Some(new_kw));

    if !persist_wrap_swap(STORAGE_ROOT_FD, SYSTEM_RECOVERY_FILE, SYSTEM_RECOVERY_TMP,
                          &new_recovery_blob) {
        // The passphrase reset committed; the phrase roll did not. The OLD phrase
        // is still valid (system-recovery-wrap untouched) -> no lockout, but we did
        // not produce a durable fresh phrase, so report error rather than hand back
        // a phrase that is not on disk.
        wipe(&mut new_entropy);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    // Reflect the rolled recovery wrap in the live static too.
    core::ptr::write(core::ptr::addr_of_mut!(SYSTEM_RECOVERY_WRAP), Some(new_rw));

    let _ = t_fsync(STORAGE_ROOT_FD, 0);

    recover_fail_reset(subject);

    // OK reply: phrase_len u16 LE + fresh_phrase.
    let mut phrase_out = bip39_encode(&new_entropy);
    wipe(&mut new_entropy);
    let mut out: Vec<u8> = Vec::with_capacity(2 + phrase_out.len());
    out.push((phrase_out.len() & 0xff) as u8);
    out.push(((phrase_out.len() >> 8) & 0xff) as u8);
    out.extend_from_slice(&phrase_out);
    stage_response(response, STATUS_OK, &out);
    wipe(&mut phrase_out);
    wipe(&mut out);
}

// handle_resolve_id — RESOLVE_ID verb (verb_id=11; UNGATED). The getpwuid
// equivalent: id -> primary_gid + supp_gids + name. A uid<->name map is not
// secret (CORVUS-DESIGN.md §16.7).
//
// Request:  principal_id u32 LE
// OK reply: primary_gid u32 LE + supp_gid_count u8 + supp_gids[count] u32 LE
//           + name_len u8 + name
unsafe fn handle_resolve_id(payload: &[u8], response: &mut Vec<u8>) {
    if payload.len() != 4 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let principal_id = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
    let rec = match user_states_find_by_id(principal_id) {
        Some(r) => r,
        None => return stage_response(response, STATUS_NOT_FOUND, &[]),
    };
    let mut out: Vec<u8> = Vec::new();
    out.extend_from_slice(&rec.primary_gid.to_le_bytes());
    out.push(rec.supp_gids.len() as u8);
    for g in &rec.supp_gids {
        out.extend_from_slice(&g.to_le_bytes());
    }
    out.push(rec.user.len() as u8);
    out.extend_from_slice(&rec.user);
    stage_response(response, STATUS_OK, &out);
}

// handle_resolve_name — RESOLVE_NAME verb (verb_id=12; UNGATED). The getpwnam
// equivalent: name -> {principal_id, primary_gid}. Login needs name->id
// pre-AUTH (§16.7).
//
// Request:  name_len u8 + name
// OK reply: principal_id u32 LE + primary_gid u32 LE
unsafe fn handle_resolve_name(payload: &[u8], response: &mut Vec<u8>) {
    if payload.is_empty() {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let name_len = payload[0] as usize;
    if name_len == 0 || name_len > MAX_USER_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    if payload.len() != 1 + name_len {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let name = &payload[1..1 + name_len];
    let rec = match user_states_find(name) {
        Some(r) => r,
        None => return stage_response(response, STATUS_NOT_FOUND, &[]),
    };
    let mut out = [0u8; 8];
    out[0..4].copy_from_slice(&rec.principal_id.to_le_bytes());
    out[4..8].copy_from_slice(&rec.primary_gid.to_le_bytes());
    stage_response(response, STATUS_OK, &out);
}

// handle_group_create — GROUP_CREATE verb (verb_id=13; CAP_HOSTOWNER-gated via
// a live peer re-query, like the admin verbs -- groupadd is privileged).
// Auto-assigns a gid from the shared counter, so a standalone group's gid is
// distinct from every UPG (§16.7).
//
// Request:  name_len u8 + name
// OK reply: gid u32 LE
unsafe fn handle_group_create(handle: i64, payload: &[u8], response: &mut Vec<u8>) {
    // Gate FIRST -- a non-hostowner peer learns nothing about the group table
    // (PermissionDenied regardless of whether the name is valid/duplicate).
    let caps = peer_live_caps(handle);
    if (caps & T_CAP_HOSTOWNER) == 0 {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }

    if payload.is_empty() {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let name_len = payload[0] as usize;
    if name_len == 0 || name_len > MAX_GROUP_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    if payload.len() != 1 + name_len {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let name = &payload[1..1 + name_len];
    if group_name_exists(name) {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    // Cap live groups at the same bound identity_db_parse enforces
    // (2 * MAX_USERS) so a GROUP_CREATE can never serialize a DB the next boot
    // refuses to load. Symmetric to USER_CREATE's user_states_count() guard.
    if groups_count() >= 2 * MAX_USERS {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }

    let gid = match alloc_auto_id() {
        Some(id) => id,
        None => return stage_response(response, STATUS_INTERNAL_ERROR, &[]),
    };
    if !groups_push(gid, name) {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    if !identity_persist() {
        groups_pop_last();
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    stage_response(response, STATUS_OK, &gid.to_le_bytes());
}

// handle_clearance_list — CLEARANCE_LIST verb (verb_id=14; user-facing, gated by
// a valid session token). Returns the built-in levels the session's user is
// eligible for, each with its caps (the versioned TLV), auth_required, and
// time_bound (CORVUS-DESIGN §6.4 + §5.7).
//
// Request:  token (33)
// OK reply: count u8, then per level:
//   name_len u8 + name + auth_required u8 + time_bound u64 LE
//   + caps_tlv_len u16 LE + caps_tlv
unsafe fn handle_clearance_list(payload: &[u8], response: &mut Vec<u8>) {
    if payload.len() != TOKEN_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    if !session_token_matches(payload) {
        return stage_response(response, STATUS_BAD_AUTH, &[]);
    }
    let user = match session_user_copy() {
        Some(u) => u,
        None => return stage_response(response, STATUS_BAD_AUTH, &[]),
    };
    emit_eligible_levels(&user, response);
}

// The eligible-levels reply body, shared by CLEARANCE_LIST (verb 14, session-
// token identity) and CLEARANCE_LIST_SELF (verb 20, connection-principal
// identity). ONE encoder so the two wire forms cannot drift: count u8, then per
// eligible level -- name_len u8 + name + auth_required u8 + time_bound u64 LE +
// caps_tlv_len u16 LE + caps_tlv (the versioned TLV, additive). The list is
// bounded by CLEARANCE_LEVELS (a small static array), so the reply is bounded.
unsafe fn emit_eligible_levels(user: &[u8], response: &mut Vec<u8>) {
    let mut out: Vec<u8> = Vec::new();
    out.push(0); // count placeholder (CLEARANCE_LEVELS <= 255)
    let mut count: u8 = 0;
    for lvl in CLEARANCE_LEVELS {
        if !user_eligible_for(user, lvl.name) {
            continue;
        }
        out.push(lvl.name.len() as u8);
        out.extend_from_slice(lvl.name);
        out.push(lvl.auth_required);
        out.extend_from_slice(&lvl.time_bound_ns.to_le_bytes());
        let mut tlv: Vec<u8> = Vec::new();
        caps_tlv_encode(lvl.caps, &mut tlv);
        out.extend_from_slice(&(tlv.len() as u16).to_le_bytes());
        out.extend_from_slice(&tlv);
        count = count.saturating_add(1);
    }
    out[0] = count;
    stage_response(response, STATUS_OK, &out);
}

// handle_clearance_list_self -- CLEARANCE_LIST_SELF (verb 20; IM-4). The SELF
// form of CLEARANCE_LIST: identity is the connection's kernel-stamped principal
// (the KObj_Srv peer is non-transferable, so it is pinned to the Proc that
// opened the connection), NOT a bearer token -- the verb-18 shape. This is a
// read-only disclosure of the caller's OWN eligibility ladder, so no re-auth is
// required (listing what you may become is not an elevation, unlike verb 18's
// activation, which needs a live session): a live corvus user reads its own
// ladder; PRINCIPAL_SYSTEM and any principal without a corvus user record get
// PermissionDenied (an id that was never minted by USER_CREATE has no
// eligibility -- the same gate that keeps the boot chain out). The reply is
// byte-identical to verb 14 via `emit_eligible_levels`.
//
// Request:  (no payload -- the identity IS the connection)
// OK reply: identical to CLEARANCE_LIST (count + per-level TLV)
unsafe fn handle_clearance_list_self(handle: i64, response: &mut Vec<u8>) {
    let peer = match peer_live_info(handle) {
        Some(p) => p,
        None => return stage_response(response, STATUS_INTERNAL_ERROR, &[]),
    };
    if peer.principal_id == PRINCIPAL_INVALID {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }
    let urec = match user_states_find_by_id(peer.principal_id) {
        Some(u) => u,
        None => return stage_response(response, STATUS_PERMISSION_DENIED, &[]),
    };
    emit_eligible_levels(&urec.user, response);
}

// The activation tail shared by both CLEARANCE_ACTIVATE forms (verbs 15 + 18):
// `level_len u8 + level + self_restrict u64 LE + valid_until_req u64 LE`. Verb
// 15 passes the payload PAST its 33-byte token; verb 18 passes the whole
// payload. One parser so the two wire forms cannot drift in their bounds.
fn parse_activate_tail(p: &[u8]) -> Option<(&[u8], u64, u64)> {
    if p.is_empty() {
        return None;
    }
    let level_len = p[0] as usize;
    if level_len == 0 || level_len > MAX_LEVEL_LEN {
        return None;
    }
    if p.len() != 1 + level_len + 8 + 8 {
        return None;
    }
    let level = &p[1..1 + level_len];
    let sr_off = 1 + level_len;
    let mut self_restrict: u64 = 0;
    for i in 0..8 {
        self_restrict |= (p[sr_off + i] as u64) << (8 * i);
    }
    let vu_off = sr_off + 8;
    let mut valid_until_req: u64 = 0;
    for i in 0..8 {
        valid_until_req |= (p[vu_off + i] as u64) << (8 * i);
    }
    Some((level, self_restrict, valid_until_req))
}

// The POLICY half of clearance activation, shared verbatim by verbs 15 and 18:
// eligibility -> auth_required -> self_restrict -> the kernel grant. The two
// verbs differ ONLY in how they establish `user`; everything downstream of the
// identity is identical, so it lives here rather than in two bodies that would
// have to be kept in step. Keeping the fork at the identity and nowhere else is
// deliberate: a reader auditing "who may activate" reads the two short handlers,
// not a diff of two long ones.
unsafe fn clearance_activate_grant(handle: i64, user: &[u8], level: &[u8], self_restrict: u64,
                                   valid_until_req: u64, response: &mut Vec<u8>) {
    let lvl = match level_by_name(level) {
        Some(l) => l,
        None => return stage_response(response, STATUS_NOT_FOUND, &[]),
    };
    if !user_eligible_for(user, level) {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }
    // auth_required: v1.0 enforces RE_AUTH in-band. Each verb supplies its own
    // proof before calling here -- verb 15 a live session token, verb 18 a live
    // session for the caller's OWN kernel-stamped principal -- so by this point
    // "the user authenticated" is established either way. A DISTINCT_SECRET
    // level is unlocked ONLY on the kernel SAK trusted path (IMPERIUM_REQUEST,
    // verb 19, IM-3) -- a session is not that secret, so both in-band forms
    // refuse it; SYSTEM_KEY / HOSTOWNER_COSIGN remain unbuilt and refused.
    match lvl.auth_required {
        AUTH_REQ_RE_AUTH => {}
        AUTH_REQ_DISTINCT_SECRET | AUTH_REQ_SYSTEM_KEY | AUTH_REQ_HOSTOWNER_COSIGN => {
            return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
        }
        _ => return stage_response(response, STATUS_PERMISSION_DENIED, &[]),
    }

    // self_restrict narrows the level's caps (STS-style, I-2). 0 = no restriction
    // (take the full level set). Restricting to nothing is meaningless -> reject.
    let effective_caps = if self_restrict == 0 {
        lvl.caps
    } else {
        lvl.caps & self_restrict
    };
    if effective_caps == 0 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }

    let valid_for_ns = level_valid_for(lvl, valid_until_req);

    // The peer's stripes -- the kernel's per-Proc identity tag + the grant
    // target. Read LIVE (C-22); a dead peer / zero stripes fails closed.
    let mut info = TSrvPeerInfo::default();
    if t_srv_peer(handle, &mut info) != 0 || info.alive == 0 || info.stripes == 0 {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }

    let session_id = next_legate_session();

    // Register the kernel clearance grant (gated kernel-side on corvus holding
    // CAP_GRANT_CLEARANCE). The peer redeems for its own stripes via t_cap_use.
    let rc =
        t_cap_grant_clearance(effective_caps, info.stripes, valid_for_ns, session_id as u64);
    if rc != 0 {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }

    let mut out = [0u8; 12];
    out[0..4].copy_from_slice(&session_id.to_le_bytes());
    out[4..12].copy_from_slice(&effective_caps.to_le_bytes());
    stage_response(response, STATUS_OK, &out);
}

// handle_clearance_activate — CLEARANCE_ACTIVATE verb (verb_id=15; the legate
// path, BEARER form). Identity comes from the presented session token: the
// caller proves it holds a live session and is authorized AS THAT SESSION'S
// USER, whatever the caller's own principal is.
//
// That decoupling is the form's whole purpose and its whole cost. It is what
// the BOOT provers need -- jit-prover / legate-prover / osmesa-prove run as
// PRINCIPAL_SYSTEM (joey spawns them with a plain t_spawn, no identity stamp)
// and AUTH as michael to obtain the token, so their principal is not, and
// cannot be, the user being authorized. It is also why a stolen token is worth
// something here: the grant lands on the CALLER's stripes, not the session
// owner's. Verb 18 is the form without that gap; prefer it wherever the caller
// genuinely runs as the user.
//
// Request:  token (33) + level_len u8 + level + self_restrict u64 LE
//           + valid_until_req u64 LE (0 = level default)
// OK reply: legate_session_id u32 LE + granted_caps u64 LE
unsafe fn handle_clearance_activate(handle: i64, payload: &[u8], response: &mut Vec<u8>) {
    if payload.len() < TOKEN_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let token = &payload[0..TOKEN_LEN];
    let (level, self_restrict, valid_until_req) = match parse_activate_tail(&payload[TOKEN_LEN..]) {
        Some(t) => t,
        None => return stage_response(response, STATUS_BAD_FORMAT, &[]),
    };

    if !session_token_matches(token) {
        return stage_response(response, STATUS_BAD_AUTH, &[]);
    }
    let user = match session_user_copy() {
        Some(u) => u,
        None => return stage_response(response, STATUS_BAD_AUTH, &[]),
    };
    clearance_activate_grant(handle, &user, level, self_restrict, valid_until_req, response);
}

// handle_clearance_activate_self — CLEARANCE_ACTIVATE_SELF verb (verb_id=18;
// the legate path, SELF form). Identity is the connection's KERNEL-STAMPED
// principal (SYS_SRV_PEER) -- the caller quotes nothing and is authorized as
// who it demonstrably is.
//
// This is the flow section 5.7 has always described ("[user shell]
// CLEARANCE_ACTIVATE ... [corvus] resolve the session's principal
// (SYS_SRV_PEER)"): the implementation resolved the principal from the token
// and used SYS_SRV_PEER only for the grant target, which left the documented
// actor -- an ordinary program a logged-in user runs -- with no way through at
// all. It could not AUTH (handle_auth refuses while any session is bound, see
// the note there) and so could never obtain a token to present. #139: every
// user-launched GL program was structurally unable to acquire CAP_JIT.
//
// The Plan 9 factotum shape: the agent holds the secret; the client is
// identified by WHO IT IS, not by what it can quote. Two properties follow that
// verb 15 cannot offer:
//
//   - There is no bearer secret to steal, delegate, or leak. (This is what
//     makes option (c) -- passing login's token down to children -- the wrong
//     answer: it would make a bearer credential ambient, which is precisely
//     what the capability model exists to prevent.)
//   - The identity axis and the grant target are the SAME Proc. Verb 15 grants
//     to whoever presents the token, whatever their principal; here the Proc
//     being authorized is the Proc being stamped. Strictly tighter.
//
// RE_AUTH is satisfied WITHOUT a token by requiring a live authenticated
// session for that same principal: corvus knows this user authenticated, and
// the kernel vouches that this Proc is that user. Both facts, no secret. With
// nobody logged in it fails closed -- an unattended daemon running as michael
// cannot elevate, which is the conservative reading of "re-auth required" and a
// one-line relaxation if a real workload ever needs it.
//
// Request:  level_len u8 + level + self_restrict u64 LE + valid_until_req u64 LE
// OK reply: legate_session_id u32 LE + granted_caps u64 LE (identical to 15)
unsafe fn handle_clearance_activate_self(handle: i64, payload: &[u8], response: &mut Vec<u8>) {
    let (level, self_restrict, valid_until_req) = match parse_activate_tail(payload) {
        Some(t) => t,
        None => return stage_response(response, STATUS_BAD_FORMAT, &[]),
    };

    // Identity: the kernel's stamp on THIS connection. The connection Spoor is
    // KObj_Srv (non-transferable), so the peer is pinned to the Proc that
    // opened it and cannot change under us. Read LIVE -- a dead peer fails
    // closed (C-22).
    let peer = match peer_live_info(handle) {
        Some(p) => p,
        None => return stage_response(response, STATUS_INTERNAL_ERROR, &[]),
    };
    if peer.principal_id == PRINCIPAL_INVALID {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }
    // A principal with no corvus user record has no eligibility and cannot be
    // one. This is also what keeps PRINCIPAL_SYSTEM out: the boot chain's
    // principal was never minted by USER_CREATE, so it resolves to nothing.
    let urec = match user_states_find_by_id(peer.principal_id) {
        Some(u) => u,
        None => return stage_response(response, STATUS_PERMISSION_DENIED, &[]),
    };

    // The RE_AUTH proof: a live session belonging to this SAME principal.
    // Compared by id, not by name -- ids come from the monotonic allocator with
    // burned entries, so they cannot be recycled onto a different human the way
    // a re-created name could.
    let sess_user = match session_user_copy() {
        Some(u) => u,
        None => return stage_response(response, STATUS_BAD_AUTH, &[]),
    };
    let sess_principal = match user_states_find(&sess_user) {
        Some(s) => s.principal_id,
        None => return stage_response(response, STATUS_BAD_AUTH, &[]),
    };
    if sess_principal != peer.principal_id {
        return stage_response(response, STATUS_BAD_AUTH, &[]);
    }

    clearance_activate_grant(handle, &urec.user, level, self_restrict, valid_until_req, response);
}

// Parse the CLEARANCE_GRANT / CLEARANCE_REVOKE payload (identical head). On
// success returns (subject_kind, subject, level, tail): `tail` is whatever
// follows the level -- empty for REVOKE and for a RE_AUTH-level GRANT; the key
// (`key_len u16 LE + key`, parse_key_tail) for a DISTINCT_SECRET-level GRANT
// (IM-3). The token is present in the wire (reserved for v1.x per-admin audit
// attribution); the authority gate is the peer's live CAP_HOSTOWNER, like
// GROUP_CREATE.
//
// Layout: token (33) + subject_kind u8 + subject_len u8 + subject
//         + level_len u8 + level [+ tail]
fn parse_clearance_admin(payload: &[u8]) -> Option<(u8, &[u8], &[u8], &[u8])> {
    if payload.len() < TOKEN_LEN + 1 + 1 + 1 + 1 {
        return None;
    }
    let kind = payload[TOKEN_LEN];
    if kind != SUBJECT_KIND_USER && kind != SUBJECT_KIND_GROUP {
        return None;
    }
    let sl = payload[TOKEN_LEN + 1] as usize;
    if sl == 0 || sl > MAX_USER_LEN {
        return None;
    }
    let subj_off = TOKEN_LEN + 2;
    if payload.len() < subj_off + sl + 1 {
        return None;
    }
    let subject = &payload[subj_off..subj_off + sl];
    let ll_off = subj_off + sl;
    let ll = payload[ll_off] as usize;
    if ll == 0 || ll > MAX_LEVEL_LEN {
        return None;
    }
    let lvl_off = ll_off + 1;
    if payload.len() < lvl_off + ll {
        return None;
    }
    let level = &payload[lvl_off..lvl_off + ll];
    Some((kind, subject, level, &payload[lvl_off + ll..]))
}

// The GRANT key tail: exactly `key_len u16 LE + key`, 1..=MAX_PASS_LEN bytes.
fn parse_key_tail(tail: &[u8]) -> Option<&[u8]> {
    if tail.len() < 2 {
        return None;
    }
    let kl = (tail[0] as usize) | ((tail[1] as usize) << 8);
    if kl == 0 || kl > MAX_PASS_LEN || tail.len() != 2 + kl {
        return None;
    }
    Some(&tail[2..])
}

// handle_clearance_grant — CLEARANCE_GRANT verb (verb_id=16; CAP_HOSTOWNER-gated
// eligibility admin, like GROUP_CREATE). Records that `subject` (a user or
// group) may activate `level`. Idempotent (re-granting an existing eligibility
// returns OK).
//
// IM-3: a DISTINCT_SECRET level (imperium) is granted WITH the subject's
// initial per-(user, level) key (ratified F4): the hostowner supplies it in
// the tail, corvus stores only a verifier (make_capkey_wrap), and the key is
// grantable to a USER subject alone -- a group cannot hold one key. A RE_AUTH
// level carries no tail (BadFormat if one is present). Idempotent on retry:
// when the eligibility AND a verifier exist and the supplied key still
// verifies, nothing is rewritten (the boot ladder re-grants every boot); a
// DIFFERENT key is the hostowner's reset of that subject's key -- the v1.0
// rotation path (self-rotation is v1.x).
//
// Request:  token (33) + subject_kind u8 + subject_len u8 + subject
//           + level_len u8 + level [+ key_len u16 LE + key]
// OK reply: no payload
unsafe fn handle_clearance_grant(handle: i64, payload: &[u8], response: &mut Vec<u8>) {
    // Gate FIRST -- a non-hostowner peer learns nothing about the eligibility table.
    let caps = peer_live_caps(handle);
    if (caps & T_CAP_HOSTOWNER) == 0 {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }
    let (kind, subject, level, tail) = match parse_clearance_admin(payload) {
        Some(t) => t,
        None => return stage_response(response, STATUS_BAD_FORMAT, &[]),
    };
    // The level must be a known built-in; the subject must exist.
    let lvl = match level_by_name(level) {
        Some(l) => l,
        None => return stage_response(response, STATUS_NOT_FOUND, &[]),
    };
    let subject_exists = match kind {
        SUBJECT_KIND_USER => user_states_find(subject).is_some(),
        SUBJECT_KIND_GROUP => group_name_exists(subject),
        _ => false,
    };
    if !subject_exists {
        return stage_response(response, STATUS_NOT_FOUND, &[]);
    }
    // The tail is shaped by the level: a key for DISTINCT_SECRET (user subject
    // only), nothing otherwise. Decided before any mutation.
    let key: Option<&[u8]> = if lvl.auth_required == AUTH_REQ_DISTINCT_SECRET {
        if kind != SUBJECT_KIND_USER {
            return stage_response(response, STATUS_BAD_FORMAT, &[]);
        }
        match parse_key_tail(tail) {
            Some(k) => Some(k),
            None => return stage_response(response, STATUS_BAD_FORMAT, &[]),
        }
    } else {
        if !tail.is_empty() {
            return stage_response(response, STATUS_BAD_FORMAT, &[]);
        }
        None
    };
    let has_elig = eligibility_has(kind, subject, level);
    let key = match key {
        None => {
            // Idempotent: a re-grant of an existing eligibility is a no-op success.
            if has_elig {
                return stage_response(response, STATUS_OK, &[]);
            }
            if eligibility_count() >= MAX_ELIGIBILITY {
                return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
            }
            if !eligibility_push(kind, subject, level) {
                return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
            }
            if !clearance_persist() {
                eligibility_pop_last();
                return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
            }
            return stage_response(response, STATUS_OK, &[]);
        }
        Some(k) => k,
    };
    // Idempotent on retry: the same key against the standing verifier changes
    // nothing on disk (one argon2 pass, no rewrite).
    if has_elig {
        if let Some(w) = capkey_find(subject, level) {
            if verify_capkey(subject, level, key, &w) {
                return stage_response(response, STATUS_OK, &[]);
            }
        }
    }
    if !has_elig && eligibility_count() >= MAX_ELIGIBILITY {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    let wrap = match make_capkey_wrap(&mut ThylaRng, subject, level, key) {
        Some(w) => w,
        None => return stage_response(response, STATUS_INTERNAL_ERROR, &[]),
    };
    // Mutate memory (the verifier, then the eligibility if absent), persist,
    // and roll BOTH back on a persist failure so memory matches disk.
    let prev = match capkey_set(subject, level, wrap) {
        Ok(p) => p,
        Err(_) => return stage_response(response, STATUS_INTERNAL_ERROR, &[]),
    };
    let pushed = !has_elig && eligibility_push(kind, subject, level);
    if !has_elig && !pushed {
        capkey_restore(subject, level, prev);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    if !clearance_persist() {
        if pushed {
            eligibility_pop_last();
        }
        capkey_restore(subject, level, prev);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    stage_response(response, STATUS_OK, &[]);
}

// handle_clearance_revoke — CLEARANCE_REVOKE verb (verb_id=17; CAP_HOSTOWNER-
// gated). Deletes the eligibility record; an active legate keeps its already-
// stamped caps until scope exit -- revoke blocks FUTURE activation (§3.1).
// NotFound if no such eligibility. IM-3: the subject's key verifier for the
// level goes with the eligibility (it has no meaning without it), both rolled
// back together on a persist failure.
//
// Request:  same shape as CLEARANCE_GRANT (no tail)
// OK reply: no payload
unsafe fn handle_clearance_revoke(handle: i64, payload: &[u8], response: &mut Vec<u8>) {
    let caps = peer_live_caps(handle);
    if (caps & T_CAP_HOSTOWNER) == 0 {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }
    let (kind, subject, level, tail) = match parse_clearance_admin(payload) {
        Some(t) => t,
        None => return stage_response(response, STATUS_BAD_FORMAT, &[]),
    };
    if !tail.is_empty() {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    if !eligibility_has(kind, subject, level) {
        return stage_response(response, STATUS_NOT_FOUND, &[]);
    }
    if !eligibility_remove(kind, subject, level) {
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    let prev_key = capkey_take(subject, level);
    if !clearance_persist() {
        // Best-effort rollback so memory matches disk on persist failure.
        let _ = eligibility_push(kind, subject, level);
        capkey_restore(subject, level, prev_key);
        return stage_response(response, STATUS_INTERNAL_ERROR, &[]);
    }
    stage_response(response, STATUS_OK, &[]);
}

// =============================================================================
// Imperium -- the lex curiata (IM-3; IMPERIUM-DESIGN.md 11.5). The pending
// request slot, the per-(user, level) rate limit, IMPERIUM_REQUEST (verb 19;
// the DEFERRED reply) and the episode consumer that runs on the `sak` note.
// =============================================================================
//
// The flow: an untrusted requester (usr/imperium, or the boot probe) sends
// IMPERIUM_REQUEST over its own /srv/corvus connection; corvus records ONE
// pending request system-wide and stages NO reply -- the requester's next Tread
// is PARKED (dispatch_tread) until the episode concludes. The operator presses
// the SAK (a serial BREAK only the kernel recognizes; no program can press it);
// the kernel opens the episode -- the console is corvus's alone, frozen for
// everyone else -- and posts the `sak` note. corvus then, INLINE in its single
// thread: opens the console, shows the PROVINCIA (exactly the cap-set the grant
// would carry, read on the unforgeable channel BEFORE any authentication),
// reads the requester's imperium key raw and unechoed, verifies it against the
// stored verifier, re-derives the requester LIVE, and registers the kernel
// grant (SYS_CAP_GRANT_IMPERIUM, PROPAGATING when the level says so); then it
// stages the reply, answers the parked read, ENDs the episode, closes the
// console handle and wipes every buffer. A SAK with nothing pending shows a
// "nothing pending" panel and ENDs on any key.
//
// Bounds: one slot; 60 s from the request to a SAK else TIMEOUT; 60 s for the
// key prompt else TIMEOUT; wrong keys counted per (user, level) (the RECOVER
// discipline) -- past IMPERIUM_FAIL_MAX the key is not even asked for. The
// episode blocks corvus's other clients for its duration (their 9P messages
// wait in the kernel rings): a documented residue, simpler to audit than a
// state machine interleaved with the poll loop.

#[derive(Clone)]
struct ImperiumPending {
    conn_id: u64,
    stripes: u64,
    principal_id: u32,
    pid: u32,
    user: Vec<u8>,
    level: &'static [u8],
    caps: u64,
    valid_for_ns: u64,
    propagating: bool,
    deadline_ns: u64,
}

static mut IMPERIUM_PENDING: Option<ImperiumPending> = None;

const IMPERIUM_REQUEST_TIMEOUT_NS: u64 = 60 * 1_000_000_000;
const IMPERIUM_PROMPT_TIMEOUT_MS: u64 = 60_000;
const IMPERIUM_PROMPT_SLICE_MS: u64 = 1000;
// Wrong keys per (user, level) before the level locks until restart. A decline
// (empty key / Ctrl-C) or a prompt timeout is not a wrong key and is not
// counted; the legitimate holder's real key passes first time.
const IMPERIUM_FAIL_MAX: u32 = 5;

struct ImperiumFail {
    subject: Vec<u8>,
    level: Vec<u8>,
    count: u32,
}

static mut IMPERIUM_FAILS: Option<Vec<ImperiumFail>> = None;

unsafe fn imperium_init() {
    core::ptr::write(core::ptr::addr_of_mut!(IMPERIUM_FAILS), Some(Vec::new()));
    core::ptr::write(core::ptr::addr_of_mut!(IMPERIUM_PENDING), None);
}

unsafe fn imperium_fail_count(subject: &[u8], level: &[u8]) -> u32 {
    match (*core::ptr::addr_of!(IMPERIUM_FAILS)).as_ref() {
        Some(v) => v
            .iter()
            .find(|r| r.subject == subject && r.level == level)
            .map(|r| r.count)
            .unwrap_or(0),
        None => 0,
    }
}

unsafe fn imperium_fail_inc(subject: &[u8], level: &[u8]) {
    let v = match (*core::ptr::addr_of_mut!(IMPERIUM_FAILS)).as_mut() {
        Some(v) => v,
        None => return,
    };
    for r in v.iter_mut() {
        if r.subject == subject && r.level == level {
            r.count = r.count.saturating_add(1);
            return;
        }
    }
    // Only an eligible + enrolled (user, level) pair reaches a wrong-key
    // verdict, and there are at most MAX_ELIGIBILITY of those, so the table
    // covers its whole subject universe and can never silently un-rate-limit
    // a subject it has no room for (the RW-6 R2-F5 class).
    if v.len() >= MAX_ELIGIBILITY {
        return;
    }
    let mut s = Vec::new();
    s.extend_from_slice(subject);
    let mut l = Vec::new();
    l.extend_from_slice(level);
    v.push(ImperiumFail { subject: s, level: l, count: 1 });
}

unsafe fn imperium_fail_reset(subject: &[u8], level: &[u8]) {
    if let Some(v) = (*core::ptr::addr_of_mut!(IMPERIUM_FAILS)).as_mut() {
        for r in v.iter_mut() {
            if r.subject == subject && r.level == level {
                r.count = 0;
                return;
            }
        }
    }
}

unsafe fn imperium_pending_is_some() -> bool {
    (*core::ptr::addr_of!(IMPERIUM_PENDING)).is_some()
}

unsafe fn imperium_pending_snapshot() -> Option<ImperiumPending> {
    (*core::ptr::addr_of!(IMPERIUM_PENDING)).as_ref().cloned()
}

unsafe fn imperium_pending_clear() {
    // Assignment through the deref drops the old record (its Vec); ptr::write
    // would leak it.
    *core::ptr::addr_of_mut!(IMPERIUM_PENDING) = None;
}

// The requester walked away (its Tread flushed, its connection closed or
// reset): drop its request so the slot frees and a SAK finds nothing pending.
unsafe fn imperium_pending_cancel_if_conn(conn_id: u64) {
    let owned = match (*core::ptr::addr_of!(IMPERIUM_PENDING)).as_ref() {
        Some(p) => p.conn_id == conn_id,
        None => false,
    };
    if owned {
        t_putstr("corvus: imperium: request abandoned (requester gone)\n");
        imperium_pending_clear();
    }
}

// Milliseconds until the pending request expires: None = nothing pending (the
// poll blocks), Some(0) = overdue, else at least 1 (never a 0 that spins).
unsafe fn imperium_pending_remaining_ms() -> Option<i32> {
    let p = (*core::ptr::addr_of!(IMPERIUM_PENDING)).as_ref()?;
    let now = monotonic_ns();
    if now >= p.deadline_ns {
        return Some(0);
    }
    let ms = (p.deadline_ns - now) / 1_000_000;
    Some(core::cmp::min(ms, (i32::MAX as u64) - 1) as i32 + 1)
}

fn putbytes(b: &[u8]) {
    t_putstr(core::str::from_utf8(b).unwrap_or("?"));
}

fn put_hex64(v: u64) {
    let mut buf = [0u8; 16];
    for i in 0..16 {
        buf[i] = nibble_to_hex((v >> (60 - 4 * i)) as u8);
    }
    t_putstr(core::str::from_utf8(&buf).unwrap_or("?"));
}

// A request nobody conferred within its window -> TIMEOUT to the requester.
unsafe fn imperium_expire(conns: &mut Vec<Conn>) {
    let (conn_id, user) = match (*core::ptr::addr_of!(IMPERIUM_PENDING)).as_ref() {
        Some(p) if monotonic_ns() >= p.deadline_ns => (p.conn_id, p.user.clone()),
        _ => return,
    };
    t_putstr("corvus: imperium: request from ");
    putbytes(&user);
    t_putstr(" TIMED OUT (no SAK within 60 s)\n");
    let _ = conn_stage_deferred(conns, conn_id, STATUS_TIMEOUT, &[]);
    imperium_pending_clear();
}

// handle_imperium_request — IMPERIUM_REQUEST verb (verb_id=19; IM-3). The
// SELF shape of verb 18 (identity = the connection's kernel-stamped principal,
// no token) for a DISTINCT_SECRET level -- and the reply is DEFERRED: nothing
// is staged on the accepted path; the requester's next read parks until the
// SAK episode concludes with OK / BadAuth (denied) / RateLimited / Timeout.
// The gates refuse at once with the usual statuses, or BUSY while another
// request is pending. No live login session is required: the SAK plus the
// distinct key ARE the authentication, and a session would be a weaker second
// factor bolted onto a stronger one.
//
// Request:  level_len u8 + level + self_restrict u64 LE + valid_until_req u64 LE
// OK reply: legate_session_id u32 LE + granted_caps u64 LE (the verb 15/18 shape)
unsafe fn handle_imperium_request(
    handle: i64,
    conn_id: u64,
    payload: &[u8],
    response: &mut Vec<u8>,
    defer: &mut bool,
) {
    let (level, self_restrict, valid_until_req) = match parse_activate_tail(payload) {
        Some(t) => t,
        None => return stage_response(response, STATUS_BAD_FORMAT, &[]),
    };
    let peer = match peer_live_info(handle) {
        Some(p) => p,
        None => return stage_response(response, STATUS_INTERNAL_ERROR, &[]),
    };
    if peer.principal_id == PRINCIPAL_INVALID || peer.stripes == 0 {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }
    // The boot chain's PRINCIPAL_SYSTEM was never minted by USER_CREATE and
    // resolves to nothing -- the same wall verb 18 has.
    let urec = match user_states_find_by_id(peer.principal_id) {
        Some(u) => u,
        None => return stage_response(response, STATUS_PERMISSION_DENIED, &[]),
    };
    let lvl = match level_by_name(level) {
        Some(l) => l,
        None => return stage_response(response, STATUS_NOT_FOUND, &[]),
    };
    // A RE_AUTH level has its own in-band path (verbs 15 / 18); the lex curiata
    // is for the levels that need the trusted path.
    if lvl.auth_required != AUTH_REQ_DISTINCT_SECRET {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }
    if !user_eligible_for(&urec.user, lvl.name) {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }
    // Eligible but not enrolled (no key granted): nothing could ever verify.
    if capkey_find(&urec.user, lvl.name).is_none() {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }
    let effective = if self_restrict == 0 {
        lvl.caps
    } else {
        lvl.caps & self_restrict
    };
    if effective == 0 {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    let valid_for_ns = level_valid_for(lvl, valid_until_req);
    if imperium_pending_is_some() {
        return stage_response(response, STATUS_BUSY, &[]);
    }
    let now = monotonic_ns();
    *core::ptr::addr_of_mut!(IMPERIUM_PENDING) = Some(ImperiumPending {
        conn_id,
        stripes: peer.stripes,
        principal_id: peer.principal_id,
        pid: peer.pid,
        user: urec.user.clone(),
        level: lvl.name,
        caps: effective,
        valid_for_ns,
        propagating: lvl.propagating,
        deadline_ns: now.saturating_add(IMPERIUM_REQUEST_TIMEOUT_NS),
    });
    *defer = true;
    let mut nbuf = [0u8; 12];
    t_putstr("corvus: imperium: request from ");
    putbytes(&urec.user);
    t_putstr(" pid ");
    t_putstr(usize_dec(peer.pid as usize, &mut nbuf));
    t_putstr(" level ");
    putbytes(lvl.name);
    t_putstr(" caps 0x");
    put_hex64(effective);
    t_putstr(" -- awaiting the SAK\n");
}

enum ImperiumOutcome {
    Conferred { session: u32 },
    Denied,
    RateLimited,
    Timeout,
    Internal,
}

enum NextByte {
    Byte(u8),
    Timeout,
    Error,
}

enum KeyRead {
    Key(Vec<u8>),
    Declined,
    Timeout,
    Overflow,
    Error,
}

unsafe fn console_write_all(fd: i64, buf: &[u8]) -> bool {
    let mut off = 0usize;
    while off < buf.len() {
        let chunk = core::cmp::min(buf.len() - off, FS_IO_CHUNK);
        let w = t_write(fd, buf[off..].as_ptr(), chunk);
        if w <= 0 || (w as usize) > chunk {
            return false;
        }
        off += w as usize;
    }
    true
}

// One bounded wait for a console byte. The bound is the clock; `idle_slices`
// counts only the polls that EXPIRED (never the ones that returned a byte), so
// a broken clock still bounds the prompt at ~timeout_ms of idle time while a
// keystroke flood cannot exhaust it.
unsafe fn console_next_byte(fd: i64, start_ns: u64, timeout_ms: u64, idle_slices: &mut u64) -> NextByte {
    loop {
        let now = monotonic_ns();
        let elapsed_ms = if start_ns != 0 && now >= start_ns {
            (now - start_ns) / 1_000_000
        } else {
            0
        };
        if elapsed_ms >= timeout_ms || *idle_slices * IMPERIUM_PROMPT_SLICE_MS >= timeout_ms {
            return NextByte::Timeout;
        }
        let wait = core::cmp::min(IMPERIUM_PROMPT_SLICE_MS, timeout_ms - elapsed_ms) as i32;
        let mut pfd = TPollFd { fd: fd as i32, events: T_POLLIN, revents: 0 };
        let rc = t_poll(&mut pfd as *mut TPollFd, 1, wait);
        if rc < 0 {
            return NextByte::Error;
        }
        if rc == 0 {
            *idle_slices += 1;
            continue;
        }
        if pfd.revents & (T_POLLERR | T_POLLHUP | T_POLLNVAL) != 0 {
            return NextByte::Error;
        }
        if pfd.revents & T_POLLIN == 0 {
            *idle_slices += 1;
            continue;
        }
        let mut b = [0u8; 1];
        let n = t_read(fd, b.as_mut_ptr(), 1);
        if n < 0 {
            return NextByte::Error;
        }
        if n == 0 {
            continue;
        }
        return NextByte::Byte(b[0]);
    }
}

// The key prompt: raw and unechoed (BEGIN forced RAW + ECHO off). DEL/BS edit,
// Ctrl-U kills the line, CR/LF submits, Ctrl-C declines. Every path that does
// not hand the key back wipes it first.
unsafe fn console_read_key(fd: i64, timeout_ms: u64) -> KeyRead {
    let mut key: Vec<u8> = Vec::with_capacity(MAX_PASS_LEN);
    let start = monotonic_ns();
    let mut idle: u64 = 0;
    loop {
        let b = match console_next_byte(fd, start, timeout_ms, &mut idle) {
            NextByte::Byte(b) => b,
            NextByte::Timeout => {
                wipe(&mut key);
                return KeyRead::Timeout;
            }
            NextByte::Error => {
                wipe(&mut key);
                return KeyRead::Error;
            }
        };
        match b {
            0x03 => {
                wipe(&mut key);
                return KeyRead::Declined;
            }
            0x0d | 0x0a => return KeyRead::Key(key),
            0x7f | 0x08 => {
                if let Some(last) = key.len().checked_sub(1) {
                    key[last] = 0;
                    key.truncate(last);
                }
            }
            0x15 => {
                wipe(&mut key);
                key.clear();
            }
            c => {
                if key.len() >= MAX_PASS_LEN {
                    wipe(&mut key);
                    return KeyRead::Overflow;
                }
                key.push(c);
            }
        }
    }
}

// Dismiss an informational panel: any byte, or the bound.
unsafe fn console_wait_any_key(fd: i64, timeout_ms: u64) {
    let start = monotonic_ns();
    let mut idle: u64 = 0;
    let _ = console_next_byte(fd, start, timeout_ms, &mut idle);
}

fn caps_names(caps: u64, out: &mut Vec<&'static [u8]>) {
    const TABLE: &[(u64, &[u8])] = &[
        (T_CAP_DAC_OVERRIDE, b"CAP_DAC_OVERRIDE"),
        (T_CAP_CHOWN, b"CAP_CHOWN"),
        (T_CAP_KILL, b"CAP_KILL"),
        (T_CAP_DEBUG, b"CAP_DEBUG"),
        (T_CAP_JIT, b"CAP_JIT"),
        (T_CAP_AUDIO_GRAPH, b"CAP_AUDIO_GRAPH"),
    ];
    let mut rest = caps;
    for &(bit, name) in TABLE {
        if caps & bit != 0 {
            out.push(name);
            rest &= !bit;
        }
    }
    if rest != 0 {
        out.push(b"CAP_?");
    }
}

// Stage a reply into the requester's connection and answer its parked read.
// false if the connection is gone (the reply has nowhere to go).
unsafe fn conn_stage_deferred(conns: &mut Vec<Conn>, conn_id: u64, status: u8, payload: &[u8]) -> bool {
    for c in conns.iter_mut() {
        if c.conn_id == conn_id {
            wipe(&mut c.pending_response);
            stage_response(&mut c.pending_response, status, payload);
            c.pending_response_off = 0;
            c.awaiting_deferred = false;
            conn_reply_parked(c);
            return true;
        }
    }
    false
}

// Answer a parked Tread (R1) from the staged response. A write failure is
// left to the connection's own POLLHUP: the reply is lost with it.
unsafe fn conn_reply_parked(conn: &mut Conn) {
    let pr = match conn.parked_read.take() {
        Some(p) => p,
        None => return,
    };
    let cap_msize = (conn.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4);
    let count_cap = (pr.count as usize).min(cap_msize);
    let mut tmp = [0u8; SERVER_MSIZE_USIZE];
    let n = drain_response(conn, &mut tmp[..count_cap], count_cap);
    let cap = conn.out_buf.capacity();
    conn.out_buf.clear();
    conn.out_buf.resize(cap, 0);
    let len = p9::build_rread(&mut conn.out_buf, pr.tag, &tmp[..n]).unwrap_or(0);
    wipe(&mut tmp[..n]);
    if len == 0 {
        return;
    }
    let mut sent = 0usize;
    while sent < len {
        let w = t_write(conn.handle, conn.out_buf.as_ptr().add(sent), len - sent);
        if w <= 0 {
            break;
        }
        sent += w as usize;
    }
}

unsafe fn episode_end() {
    if t_console_episode(T_CONSOLE_EPISODE_END) != 0 {
        t_putstr("corvus: imperium: EPISODE_END refused (no open episode)\n");
    }
}

unsafe fn verdict(fd: i64, out: &mut Vec<u8>, line: &[u8]) {
    out.clear();
    provincia::compose_verdict(line, out);
    let _ = console_write_all(fd, out);
}

// The `sak` note arrived: the kernel opened the episode (the console is ours
// alone until END). Everything trusted happens here, then END + close + wipe
// on every path.
unsafe fn episode_consume(conns: &mut Vec<Conn>) {
    let fd = t_console_open();
    if fd < 0 {
        t_putstr("corvus: imperium: episode: console open FAILED -- ending\n");
        episode_end();
        return;
    }
    let mut out: Vec<u8> = Vec::new();
    match imperium_pending_snapshot() {
        None => {
            provincia::compose_nothing_pending(&mut out);
            let _ = console_write_all(fd, &out);
            console_wait_any_key(fd, IMPERIUM_PROMPT_TIMEOUT_MS);
            out.clear();
            provincia::compose_verdict(b"trusted path: done", &mut out);
            let _ = console_write_all(fd, &out);
            t_putstr("corvus: imperium: SAK with nothing pending\n");
        }
        Some(p) => {
            let outcome = episode_confer(fd, &p, conns, &mut out);
            let mut ok = [0u8; 12];
            let (status, plen) = match outcome {
                ImperiumOutcome::Conferred { session } => {
                    ok[0..4].copy_from_slice(&session.to_le_bytes());
                    ok[4..12].copy_from_slice(&p.caps.to_le_bytes());
                    (STATUS_OK, 12usize)
                }
                ImperiumOutcome::Denied => (STATUS_BAD_AUTH, 0usize),
                ImperiumOutcome::RateLimited => (STATUS_RATE_LIMITED, 0usize),
                ImperiumOutcome::Timeout => (STATUS_TIMEOUT, 0usize),
                ImperiumOutcome::Internal => (STATUS_INTERNAL_ERROR, 0usize),
            };
            if !conn_stage_deferred(conns, p.conn_id, status, &ok[..plen]) {
                t_putstr("corvus: imperium: requester connection gone -- reply dropped\n");
            }
            imperium_pending_clear();
        }
    }
    wipe(&mut out);
    episode_end();
    let _ = t_close(fd);
}

// The conferral: rate limit -> provincia -> key -> verify -> the requester
// LIVE -> the kernel grant. The operator's verdict line goes out on the
// trusted channel; the requester's status is the return.
unsafe fn episode_confer(
    fd: i64,
    p: &ImperiumPending,
    conns: &mut Vec<Conn>,
    out: &mut Vec<u8>,
) -> ImperiumOutcome {
    // 1. The rate limit, BEFORE the provincia and any KDF: a locked subject is
    //    told so and asked nothing.
    if imperium_fail_count(&p.user, p.level) >= IMPERIUM_FAIL_MAX {
        out.clear();
        provincia::compose_locked(&p.user, p.level, out);
        let _ = console_write_all(fd, out);
        console_wait_any_key(fd, IMPERIUM_PROMPT_TIMEOUT_MS);
        t_putstr("corvus: imperium: RATE-LIMITED ");
        putbytes(&p.user);
        t_putstr("\n");
        return ImperiumOutcome::RateLimited;
    }
    // 2. The provincia: the whole grant, on the unforgeable channel, first.
    let mut names: Vec<&'static [u8]> = Vec::new();
    caps_names(p.caps, &mut names);
    out.clear();
    provincia::compose_provincia(
        &provincia::Provincia {
            user: &p.user,
            pid: p.pid,
            level: p.level,
            caps_names: &names,
            axe: p.caps & T_CAP_KILL != 0,
            term_ns: p.valid_for_ns,
            propagating: p.propagating,
        },
        out,
    );
    provincia::compose_key_prompt(out);
    if !console_write_all(fd, out) {
        t_putstr("corvus: imperium: console write FAILED\n");
        return ImperiumOutcome::Internal;
    }
    // 3. The key.
    let mut key = match console_read_key(fd, IMPERIUM_PROMPT_TIMEOUT_MS) {
        KeyRead::Key(k) => k,
        KeyRead::Declined => {
            verdict(fd, out, b"imperium DECLINED");
            t_putstr("corvus: imperium: DECLINED at the prompt\n");
            return ImperiumOutcome::Denied;
        }
        KeyRead::Timeout => {
            verdict(fd, out, b"imperium TIMEOUT: no key within 60 s");
            t_putstr("corvus: imperium: prompt TIMED OUT\n");
            return ImperiumOutcome::Timeout;
        }
        KeyRead::Overflow => {
            verdict(fd, out, b"imperium DENIED: key too long");
            t_putstr("corvus: imperium: DENIED (key too long)\n");
            return ImperiumOutcome::Denied;
        }
        KeyRead::Error => {
            verdict(fd, out, b"imperium ERROR: console read failed");
            t_putstr("corvus: imperium: console read FAILED\n");
            return ImperiumOutcome::Internal;
        }
    };
    if key.is_empty() {
        verdict(fd, out, b"imperium DECLINED");
        t_putstr("corvus: imperium: DECLINED at the prompt (empty key)\n");
        return ImperiumOutcome::Denied;
    }
    // 4. Verify against the stored verifier (argon2id + the AEAD tag).
    let wrap = match capkey_find(&p.user, p.level) {
        Some(w) => w,
        None => {
            wipe(&mut key);
            verdict(fd, out, b"imperium DENIED: not enrolled");
            return ImperiumOutcome::Denied;
        }
    };
    let good = verify_capkey(&p.user, p.level, &key, &wrap);
    wipe(&mut key);
    if !good {
        imperium_fail_inc(&p.user, p.level);
        verdict(fd, out, b"imperium DENIED: wrong key");
        t_putstr("corvus: imperium: DENIED (wrong key) for ");
        putbytes(&p.user);
        t_putstr("\n");
        return ImperiumOutcome::Denied;
    }
    imperium_fail_reset(&p.user, p.level);
    // 5. The requester, LIVE (C-22): the Proc that asked must still be the
    //    Proc that receives -- alive, the same stripes and principal, still
    //    eligible. Read fresh; the request-time snapshot authorizes nothing.
    let handle = match conns.iter().find(|c| c.conn_id == p.conn_id).map(|c| c.handle) {
        Some(h) => h,
        None => {
            verdict(fd, out, b"imperium DENIED: requester gone");
            return ImperiumOutcome::Denied;
        }
    };
    let peer = match peer_live_info(handle) {
        Some(x) => x,
        None => {
            verdict(fd, out, b"imperium DENIED: requester died");
            return ImperiumOutcome::Denied;
        }
    };
    if peer.stripes == 0 || peer.stripes != p.stripes || peer.principal_id != p.principal_id {
        verdict(fd, out, b"imperium DENIED: requester changed");
        return ImperiumOutcome::Denied;
    }
    if !user_eligible_for(&p.user, p.level) {
        verdict(fd, out, b"imperium DENIED: eligibility revoked");
        return ImperiumOutcome::Denied;
    }
    // 6. The grant. The kernel bounds a PROPAGATING mask to
    //    CAP_GRANTABLE_IMPERIUM and refuses its redeem into an existing scope
    //    (propagating never nests -- abdicate first).
    let session = next_legate_session();
    let flags = if p.propagating { T_CAP_GRANT_FLAG_PROPAGATING } else { 0 };
    if t_cap_grant_imperium(p.caps, peer.stripes, p.valid_for_ns, session as u64, flags) != 0 {
        verdict(fd, out, b"imperium DENIED: kernel grant refused");
        t_putstr("corvus: imperium: kernel grant REFUSED for ");
        putbytes(&p.user);
        t_putstr("\n");
        return ImperiumOutcome::Internal;
    }
    verdict(fd, out, b"imperium CONFERRED -- redeem in the requester");
    let mut nbuf = [0u8; 12];
    t_putstr("corvus: imperium: CONFERRED to ");
    putbytes(&p.user);
    t_putstr(" session ");
    t_putstr(usize_dec(session as usize, &mut nbuf));
    t_putstr(" caps 0x");
    put_hex64(p.caps);
    t_putstr("\n");
    ImperiumOutcome::Conferred { session }
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
unsafe fn handle_admin_elevate(handle: i64, payload: &[u8],
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

    // Console-attached check on the LIVE peer snapshot (RW-6 R2-F2). Console
    // attachment is REVOCABLE on a live Proc (kernel proc_revoke_console_attached),
    // so the at-accept cache can be stale -- re-query per C-22, matching every sibling
    // verb (peer_live_caps). The kernel re-verifies PROC_FLAG_CONSOLE_ATTACHED at
    // /cap/use redemption as the load-bearing gate; this corvus-side check is
    // defense-in-depth, now live. Fail closed on a dead peer / syscall error.
    let info = match peer_live_info(handle) {
        Some(i) => i,
        None => return stage_response(response, STATUS_PERMISSION_DENIED, &[]),
    };
    if info.console == 0 {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
    }

    // System passphrase verification (A-5c-b): unwrap the host-baked system-wrap
    // (the admin hybrid keypair under the system passphrase) with the SUPPLIED
    // passphrase. A correct passphrase opens the AEAD (yields the keypair, which
    // ADMIN_ELEVATE does not need -- so wipe it); any mismatch fails the tag ->
    // BadAuth. Fail closed: an absent wrap (system_identity_load is FATAL, so this
    // is defensive) or a KDF failure is BadAuth, never a bypass. The argon2id work
    // here is the deliberate brute-force cost on the system passphrase.
    let sys_kw = match (*core::ptr::addr_of!(SYSTEM_KEYPAIR_WRAP)).as_ref() {
        Some(k) => k,
        None => return stage_response(response, STATUS_BAD_AUTH, &[]),
    };
    match unwrap_keypair_passphrase(SYSTEM_WRAP_SUBJECT, sys_passphrase, sys_kw) {
        Some(mut kp) => wipe(&mut kp),
        None => return stage_response(response, STATUS_BAD_AUTH, &[]),
    }

    // Register the pending grant for the peer's stripes. The kernel's
    // cap_register_grant_for_writer gate-checks CAP_GRANT_HOSTOWNER (set
    // on corvus by joey's t_spawn_with_perms mask).
    if t_cap_grant(T_CAP_HOSTOWNER, info.stripes) != 0 {
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

    let envelope = dek_envelope_wrap(&mut ThylaRng, &x_pk, &mlkem_ek, &dek, dataset, key_id);

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

unsafe fn handle_session_close(conn_id: u64, payload: &[u8], response: &mut Vec<u8>) {
    if payload.len() != TOKEN_LEN {
        return stage_response(response, STATUS_BAD_FORMAT, &[]);
    }
    if !session_token_matches(payload) {
        return stage_response(response, STATUS_NOT_FOUND, &[]);
    }
    // Only the session-OWNING connection may close it via the verb. A
    // non-owning bearer-token connection -- the storage coordinator pulling a
    // per-user DEK over the S6.3 token-forward -- holds a valid token but must
    // not wipe a live login session (that would break A-4 legate elevation,
    // which re-presents the same token). The connection-close path
    // (close_conn) already gates on ownership; this gates the explicit verb,
    // completing the lift. session_owner_conn_id() is 0 only when no session
    // is active and conn_id is always >= 1 (the allocator skips the 0
    // sentinel), so a non-owner can never alias the no-owner state.
    if conn_id != session_owner_conn_id() {
        return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
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

// POSIX st_mode type bits, reported in Rgetattr (stalk-3b-β; see dispatch_tgetattr).
const S_IFDIR: u32 = 0o40000;
const S_IFREG: u32 = 0o100000;

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
    // A-5b: a process-unique monotonic id assigned at accept. The AUTH
    // session records its owning conn's id; only the owner's close clears
    // the session (close_conn), so a non-owning bearer-token connection
    // cannot wipe a live login session by disconnecting.
    conn_id: u64,
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
    // IM-3 (the deferred reply): `awaiting_deferred` is set by a verb whose
    // reply comes later (IMPERIUM_REQUEST). While it is set, a Tread on ctl
    // that finds nothing staged is PARKED here instead of being answered with
    // a 0-byte Rread (which the kernel client hands to userspace as EOF), and
    // conn_reply_parked answers it once the reply is staged. Cleared by the
    // reply, a Tflush of the parked tag, a Tversion reset, and close.
    awaiting_deferred: bool,
    parked_read: Option<ParkedRead>,
}

#[derive(Copy, Clone)]
struct ParkedRead {
    tag: u16,
    count: u32,
}

impl Conn {
    fn new(handle: i64, peer: TSrvPeerInfo, conn_id: u64) -> Self {
        Self {
            handle,
            conn_id,
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
            awaiting_deferred: false,
            parked_read: None,
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
        // Scrub before clear (RW-6 R2-F3/SA-5): pending_request may hold a passphrase
        // frame, pending_response a token/DEK; clear() alone leaves them in capacity.
        wipe(&mut self.pending_request);
        self.pending_request.clear();
        wipe(&mut self.pending_response);
        self.pending_response.clear();
        self.pending_response_off = 0;
        self.version_done = false;
        // IM-3: a reset abandons a parked read and the request it waited on.
        self.awaiting_deferred = false;
        self.parked_read = None;
        unsafe { imperium_pending_cancel_if_conn(self.conn_id) };
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
        // A fully-drained response may have carried a secret (the AUTH token, the
        // UNWRAP DEK); scrub the backing bytes before clear() (RW-6 R2-F3/SA-5).
        // clear() drops len to 0 but leaves the secret in the Vec's capacity.
        wipe(&mut conn.pending_response);
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

        // Reset response staging before dispatch; scrub any prior (possibly
        // partially-drained) secret response first (RW-6 R2-F3/SA-5).
        wipe(&mut conn.pending_response);
        conn.pending_response.clear();
        conn.pending_response_off = 0;

        // Snapshot the conn handle + id before re-borrowing conn.pending_response.
        // C-22: every gated handler does its OWN fresh SYS_SRV_PEER query from the
        // handle (peer_live_caps / peer_live_info) -- the dispatch passes NO peer
        // snapshot, so a stale at-accept identity can never reach a gate (RW-6 R2-F2;
        // the at-accept conn.peer stays for corvus.tla's ConnOpIdentityIsKernelTruth).
        let conn_handle = conn.handle;
        let conn_id = conn.conn_id;

        match verb_id {
            VERB_AUTH => handle_auth(conn_id, &payload_owned, &mut conn.pending_response),
            VERB_SESSION_CLOSE => handle_session_close(conn_id, &payload_owned,
                                                       &mut conn.pending_response),
            VERB_UNWRAP => handle_unwrap(&payload_owned, &mut conn.pending_response),
            VERB_USER_CREATE => handle_user_create(conn_handle, &payload_owned,
                                                   &mut conn.pending_response),
            VERB_ADMIN_ELEVATE => handle_admin_elevate(conn_handle, &payload_owned,
                                                       &mut conn.pending_response),
            VERB_RECOVER => handle_recover(conn_handle, &payload_owned,
                                           &mut conn.pending_response),
            VERB_WRAP => handle_wrap(&payload_owned, &mut conn.pending_response),
            VERB_RESOLVE_ID => handle_resolve_id(&payload_owned, &mut conn.pending_response),
            VERB_RESOLVE_NAME => handle_resolve_name(&payload_owned, &mut conn.pending_response),
            VERB_GROUP_CREATE => handle_group_create(conn_handle, &payload_owned,
                                                     &mut conn.pending_response),
            VERB_CLEARANCE_LIST => handle_clearance_list(&payload_owned,
                                                         &mut conn.pending_response),
            VERB_CLEARANCE_LIST_SELF => handle_clearance_list_self(conn_handle,
                                                         &mut conn.pending_response),
            VERB_CLEARANCE_ACTIVATE_SELF => handle_clearance_activate_self(conn_handle,
                                                &payload_owned, &mut conn.pending_response),
            VERB_CLEARANCE_ACTIVATE => handle_clearance_activate(conn_handle, &payload_owned,
                                                                 &mut conn.pending_response),
            VERB_CLEARANCE_GRANT => handle_clearance_grant(conn_handle, &payload_owned,
                                                           &mut conn.pending_response),
            VERB_CLEARANCE_REVOKE => handle_clearance_revoke(conn_handle, &payload_owned,
                                                             &mut conn.pending_response),
            // IM-3: on the accepted path this stages NOTHING and sets
            // awaiting_deferred -- the next Tread parks (dispatch_tread) until
            // the episode stages the reply.
            VERB_IMPERIUM_REQUEST => handle_imperium_request(conn_handle, conn_id, &payload_owned,
                                                             &mut conn.pending_response,
                                                             &mut conn.awaiting_deferred),
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

// Returns None when the conn must be torn down (a Tmsg parse-error so bad we
// couldn't even extract the tag, or an Rmsg that could not be built), Some(0)
// when the Tmsg was consumed with NO reply yet (a parked Tread -- the IM-3
// deferred reply), else the byte length of the Rmsg written into conn.out_buf.
unsafe fn dispatch_one(conn: &mut Conn, tmsg: &[u8]) -> Option<usize> {
    let hdr = match p9::peek_header(tmsg) {
        Ok(h) => h,
        Err(_) => return None,
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
        p9::P9_TGETATTR => dispatch_tgetattr(conn, tmsg, tag),
        p9::P9_TFLUSH => dispatch_tflush(conn, tmsg, tag),
        _ => Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_NOSYS).unwrap_or(0)),
    };
    match result {
        Ok(n) => {
            conn.out_buf.truncate(n);
            Some(n)
        }
        Err(_) => {
            // build_r* failed (out_buf too small? shouldn't happen). Emit
            // an Rlerror in a fresh attempt; if that also fails the conn
            // must die.
            conn.out_buf.clear();
            conn.out_buf.resize(cap, 0);
            let n = p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO).unwrap_or(0);
            conn.out_buf.truncate(n);
            if n == 0 {
                None
            } else {
                Some(n)
            }
        }
    }
}

// dispatch_tgetattr -- stalk-3b-β: the namespace-resident /srv routes service
// opens through the kernel's stalk resolver + dev9p, whose A-3 per-component
// X-search calls Tgetattr on each traversed node. corvus reports the connecting
// client (conn.peer, the kernel-stamped SO_PEERCRED identity) as the OWNER with
// permissive perms (root drwxr-xr-x, ctl -rw------- owned by the client), so the
// owner-first perm_check passes. corvus's real authorization is conn.peer + the
// per-verb console/passphrase checks; the reported FS rwx only lets the client
// REACH the ctl channel (which it already may, having connected). The valid mask
// MUST include mode/uid/gid -- dev9p_stat_native fail-closes on an unset bit.
fn dispatch_tgetattr(conn: &mut Conn, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
    let fid = match p9::parse_tgetattr(tmsg) {
        Ok(f) => f,
        Err(_) => return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO)?),
    };
    let qid = match conn.fid_find(fid) {
        Some(idx) => conn.fids[idx].unwrap().qid,
        None => return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_BADF)?),
    };
    let (mode, nlink) = if qid.kind == p9::P9_QTDIR {
        (S_IFDIR | 0o755u32, 1u64) // drwxr-xr-x
    } else {
        (S_IFREG | 0o600u32, 1u64) // -rw------- (owner = the connecting client)
    };
    let valid =
        p9::P9_GETATTR_MODE | p9::P9_GETATTR_UID | p9::P9_GETATTR_GID | p9::P9_GETATTR_NLINK;
    p9::build_rgetattr(
        &mut conn.out_buf, tag, valid, &qid, mode,
        conn.peer.principal_id, conn.peer.primary_gid, nlink, 0,
    )
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
    // IM-3: a DEFERRED reply -- hold the Tread until the episode stages it
    // (conn_reply_parked answers it). A 0-byte Rread is not a park: the
    // kernel client hands it to userspace as EOF.
    if conn.awaiting_deferred && conn.pending_response.is_empty() {
        conn.parked_read = Some(ParkedRead { tag, count: args.count });
        return Ok(0);
    }
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

// Tflush: the client abandoned `oldtag`. The only request corvus ever holds
// is a parked Tread (IM-3), so a flush of that tag drops the park AND the
// pending imperium request it was waiting on -- the requester walked away, and
// a later SAK must find nothing pending. Rflush regardless: per 9P the client
// reuses oldtag only after it (I-10), so the Rflush is what frees the tag.
unsafe fn dispatch_tflush(conn: &mut Conn, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
    let args = match p9::parse_tflush(tmsg) {
        Ok(a) => a,
        Err(_) => return Ok(p9::build_rlerror(&mut conn.out_buf, tag, p9::E_PROTO)?),
    };
    if let Some(pr) = conn.parked_read {
        if pr.tag == args.oldtag {
            conn.parked_read = None;
            conn.awaiting_deferred = false;
            imperium_pending_cancel_if_conn(conn.conn_id);
        }
    }
    p9::build_rflush(&mut conn.out_buf, tag)
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
        let rmsg_len = match dispatch_one(conn, &tmsg_bytes) {
            None => return Ok(false),
            Some(n) => n,
        };
        // Write the Rmsg -- none for a parked Tread (IM-3), whose reply goes
        // out later from conn_reply_parked.
        if rmsg_len > 0 {
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
        }

        // Scrub then remove the consumed Tmsg: a Twrite frame carries the verb payload
        // (an AUTH/RECOVER passphrase) and drain leaves the consumed prefix in the Vec's
        // capacity otherwise (RW-6 R2-F3/SA-5). payload_owned is wiped at dispatch; this
        // closes the raw inbound copy.
        wipe(&mut conn.in_buf[..size]);
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

// The poll set is [listener, the notes fd] + per-conn.
const POLL_FIXED: usize = 2;

unsafe fn srv_server_loop(listener: i64, notes: &Notes) -> i64 {
    let mut conns: Vec<Conn> = Vec::with_capacity(MAX_CONNS);
    // A-5b: monotonic per-accept conn id (0 = the "no owner" sentinel).
    let mut next_conn_id: u64 = 1;

    // Bounded poll-fd buffer: [listener, notes] + per-conn (max MAX_CONNS).
    let mut pollfds: [TPollFd; POLL_FIXED + MAX_CONNS] =
        [TPollFd { fd: 0, events: 0, revents: 0 }; POLL_FIXED + MAX_CONNS];
    let notes_fd = notes.as_raw_fd();

    loop {
        // Build pollfd list.
        pollfds[0] = TPollFd { fd: listener as i32, events: T_POLLIN, revents: 0 };
        pollfds[1] = TPollFd { fd: notes_fd, events: T_POLLIN, revents: 0 };
        let nfds = POLL_FIXED + conns.len();
        for (i, c) in conns.iter().enumerate() {
            pollfds[POLL_FIXED + i] = TPollFd { fd: c.handle as i32, events: T_POLLIN, revents: 0 };
        }

        // IM-3: a pending imperium request bounds the wait at its expiry.
        let timeout_ms = imperium_pending_remaining_ms().unwrap_or(-1);
        let rc = t_poll(pollfds.as_mut_ptr(), nfds, timeout_ms);
        if rc < 0 {
            t_putstr("corvus: t_poll failed\n");
            return -1;
        }

        // IM-3: the `sak` note is handled AFTER the connections below, so a
        // request whose Twrite and the operator's BREAK land in the same poll
        // is recorded before the episode looks for it -- the other order would
        // show "nothing pending" and strand the request until its timeout.
        let sak_ready = pollfds[1].revents & T_POLLIN != 0;

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
                        conns.push(Conn::new(h, peer, next_conn_id));
                        // Skip the 0 sentinel on the 2^64 wrap so a recycled
                        // id can never alias the "no owner" value and falsely
                        // pass the SESSION_CLOSE / close_conn ownership gate.
                        next_conn_id = next_conn_id.wrapping_add(1);
                        if next_conn_id == 0 {
                            next_conn_id = 1;
                        }
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
        // invalidate indices. Only service the conns that were actually
        // POLLED this iteration (nfds - POLL_FIXED), NOT a conn just accepted
        // above -- its pollfds[] slot was never written by t_poll and holds a
        // STALE revents from a previously-closed conn at that index, which
        // would spuriously close the fresh conn before its Tversion is read.
        // The just-accepted conn is serviced on the next iteration
        // (stalk-3b-β: the dev9p close sends Tclunk x2 before the EOF, so a
        // peer's POLLHUP now lands in a separate poll from a reconnect's
        // listener-POLLIN, exposing this).
        let mut i = nfds - POLL_FIXED;
        while i > 0 {
            i -= 1;
            let pf = pollfds[POLL_FIXED + i];
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

        // IM-3: the `sak` note -> the trusted episode, run INLINE (the
        // connections wait; the prompt is bounded). Any other note on corvus's
        // queue is drained and ignored -- corvus is never a Ctrl-C target and
        // spawns nothing.
        if sak_ready {
            if let Ok(note) = notes.read() {
                if note.name == "sak" {
                    episode_consume(&mut conns);
                } else {
                    t_putstr("corvus: note ignored: ");
                    t_putstr(&note.name);
                    t_putstr("\n");
                }
            }
        }
        // IM-3: a request nobody conferred within its window.
        imperium_expire(&mut conns);
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
    // A-5b: clear the global AUTH session only when its OWNING connection
    // (the one that ran AUTH) closes. A non-owning bearer-token connection
    // -- e.g. the A-5b storage coordinator that presented login's token for
    // an UNWRAP -- disconnecting must NOT wipe a live login session (that
    // would break mid-session A-4 legate elevation, which re-presents the
    // same token). The SESSION_CLOSE verb is likewise owner-gated
    // (handle_session_close); only process shutdown clears unconditionally.
    let owner = session_owner_conn_id();
    if owner != 0 && conn.conn_id == owner {
        session_clear();
    }
    // IM-3: a requester that left takes its pending request with it (a later
    // SAK must find nothing pending); its parked read dies with the conn.
    imperium_pending_cancel_if_conn(conn.conn_id);
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

    // stalk-3c (create=post, STALK-DESIGN.md §5.3 / D2): post /srv/corvus by
    // CREATE on the namespace-resident /srv. This is the ONE intentional use of
    // the inherited namespace and it MUST precede the storage-cap chroot below:
    // the chroot displaces the namespace root, after which /srv is unreachable.
    // The KObj_Srv listener handle is a capability -- it survives the chroot;
    // corvus accepts on it in srv_server_loop. perm=0 posts 9P-mode (corvus
    // serves /ctl as 9P; no DMSRVBYTE). Requires PROC_FLAG_MAY_POST_SERVICE
    // (joey stamps it via SYS_SPAWN_WITH_PERMS). Raw syscalls -- no heap yet.
    let listener = unsafe {
        let srv = t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH);
        if srv < 0 { step_fail(6, srv); }
        let l = t_walk_create(srv, b"corvus".as_ptr(), 6, T_OREAD, 0);
        let _ = t_close(srv);
        l
    };
    if listener < 0 { step_fail(6, listener); }

    // A-1.7 (F2): confine to the handed storage-root capability (fd 0) --
    // immediately after the post above (the sole pre-chroot namespace use) so
    // there is no further ambient-FS window. corvus inherits joey's broad
    // Stratum root via territory_clone; this chroot displaces it
    // (territory_chroot spoor_clunks the old root, spoor_refs the cap), making
    // corvus's filesystem world the capability (I-23). joey ALWAYS hands the
    // capability now, so a missing/invalid fd 0 is a fatal boot error, not a
    // fallback. chroot is a raw syscall -- no heap needed yet.
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
        groups_init();
        clearance_init();
        recover_fails_init();
        imperium_init();
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

    // A-5c: self-test the recovery codec + wrap layout before serving. Fast (no
    // argon2); a codec bug here would silently brick recovery, so corvus refuses
    // to start on failure.
    unsafe {
        if recovery_selftest() {
            t_putstr("corvus: recovery self-test OK (bip39 + keyslot)\n");
        } else {
            t_putstr("corvus: FATAL recovery self-test FAILED\n");
            return 1;
        }
    }

    // A-5c-b: load the host-baked system identity (system-wrap + system-recovery-wrap)
    // from the chrooted storage cap. Fail-closed -- absent/corrupt means a broken bake
    // or tampering, and corvus cannot authorize ADMIN_ELEVATE / RECOVER(system) without
    // it (no byte-compare fallback survives). The wraps are always host-baked into the
    // pool, so absence is a real fault, not a fresh-install case.
    unsafe {
        if system_identity_load() {
            t_putstr("corvus: system identity loaded (system-wrap + recovery)\n");
        } else {
            t_putstr("corvus: FATAL system identity load failed (missing/corrupt system-wrap)\n");
            return 1;
        }
    }

    // A-1b: load the identity DB + per-user keypair wraps from within the
    // chrooted storage cap. Fail-closed -- a present-but-corrupt identity.db
    // aborts boot rather than silently re-bootstrapping a free first user.
    unsafe {
        if !identity_load() {
            t_putstr("corvus: FATAL identity.db load failed (corrupt/unreadable)\n");
            return 1;
        }
    }

    // A-4a-3: load the clearance eligibility DB AFTER identity (group/user
    // resolution must be live). Fail-closed -- a present-but-corrupt clearance.db
    // aborts boot rather than silently dropping eligibility records.
    unsafe {
        if !clearance_load() {
            t_putstr("corvus: FATAL clearance.db load failed (corrupt/unreadable)\n");
            return 1;
        }
    }

    // IM-3: the trusted-episode consumer. The notes fd first (the `sak` note
    // lands on the Proc's queue either way, but reading it needs the fd; a
    // syscall, so the chroot above is no obstacle), then ARM. A refused ARM
    // means corvus is not the trusted login authority (a spawn outside the
    // boot chain): the lex curiata is unavailable, every other duty serves.
    let notes = match Notes::open_self() {
        Ok(n) => n,
        Err(_) => {
            t_putstr("corvus: FATAL notes fd open failed\n");
            return 1;
        }
    };
    unsafe {
        if t_console_episode(T_CONSOLE_EPISODE_ARM) == 0 {
            t_putstr("corvus: trusted-episode consumer ARMED (imperium)\n");
        } else {
            t_putstr("corvus: WARN episode ARM refused (not the trusted authority) -- imperium unavailable\n");
        }
    }

    let rc = unsafe { srv_server_loop(listener, &notes) };
    if rc < 0 {
        t_putstr("corvus: srv_server_loop FAILED\n");
        return 1;
    }

    unsafe { session_clear() };
    let _ = unsafe { t_close(listener) };
    t_putstr("corvus: srv_server_loop returned cleanly; shutting down\n");
    0
}
