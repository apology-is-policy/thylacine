//! Shared crypto core for corvus + corvus-mint (CORVUS-DESIGN.md 4.3 / 5.6).
//
// One source of the CRVS wrap layout, Argon2id KDF, AEGIS-256 AEAD, the hybrid
// keypair (X25519 + ML-KEM-768), the hybrid-PKE DEK envelope, and the BIP-39
// recovery-phrase codec. Extracted from usr/corvus/src/main.rs (A-5c-b) so the
// host-target corvus-mint (which mints the system identity at build time) and
// the on-device corvus produce/consume byte-identical wraps -- no second
// implementation to drift. The RNG is a parameter: corvus passes a t_getrandom
// adapter; corvus-mint passes OsRng. The allocator is the linking binary's.
//
// `not(test)` is no_std so corvus links it; under `cargo test` the crate is std
// so the host test harness + assert macros work.
#![cfg_attr(not(test), no_std)]

extern crate alloc;
use alloc::vec::Vec;

mod bip39_wordlist;
pub use bip39_wordlist::BIP39_WORDLIST;

use aegis::aegis256::Aegis256;
use argon2::{Algorithm, Argon2, Params, Version};
use kem::{Decapsulate, Encapsulate};
use ml_kem::{Ciphertext, Encoded, EncodedSizeUser, KemCore, MlKem768};
use rand_core::{CryptoRng, RngCore};
use sha2::{Digest, Sha256};
use x25519_dalek::{EphemeralSecret, PublicKey, StaticSecret};

type MlKemEk = <MlKem768 as KemCore>::EncapsulationKey;
type MlKemDk = <MlKem768 as KemCore>::DecapsulationKey;

// =============================================================================
// CRVS wrap-state layout (the AEGIS-wrapped hybrid keypair) -- CORVUS-DESIGN 4.3.
// =============================================================================

pub const CORVUS_MAGIC: u32 = 0x53565243; // 'CRVS' LE
pub const CORVUS_STATE_VERSION: u32 = 1;
pub const ARGON2_SALT_LEN: usize = 16;
pub const AEGIS256_KEY_LEN: usize = 32;
pub const AEGIS256_NONCE_LEN: usize = 32;
pub const AEGIS256_TAG_LEN: usize = 32;

// Hybrid keypair layout: the AEGIS-wrapped plaintext is the concatenation of the
// X25519 and ML-KEM-768 key halves (see the KP_* offsets).
pub const X25519_KEY_LEN: usize = 32;
pub const MLKEM_EK_LEN: usize = 1184;
pub const MLKEM_DK_LEN: usize = 2400;
pub const MLKEM_CT_LEN: usize = 1088;
pub const KP_X25519_SK_OFF: usize = 0;
pub const KP_X25519_PK_OFF: usize = X25519_KEY_LEN;
pub const KP_MLKEM_EK_OFF: usize = X25519_KEY_LEN * 2;
pub const KP_MLKEM_DK_OFF: usize = X25519_KEY_LEN * 2 + MLKEM_EK_LEN;
pub const KEYPAIR_LEN: usize = X25519_KEY_LEN * 2 + MLKEM_EK_LEN + MLKEM_DK_LEN; // 3648

pub const HEADER_LEN: usize = 4 + 4 + 4 + 8 + 4 + ARGON2_SALT_LEN + AEGIS256_NONCE_LEN; // = 72
pub const TOTAL_LEN: usize = HEADER_LEN + KEYPAIR_LEN + AEGIS256_TAG_LEN; // = 3752

const _: () = assert!(HEADER_LEN == 72, "state file header layout drift");
const _: () = assert!(KEYPAIR_LEN == 3648, "hybrid keypair layout drift");
const _: () = assert!(KP_MLKEM_DK_OFF == 1248, "keypair sub-offset drift");
const _: () = assert!(AEGIS256_KEY_LEN == 32, "AEGIS-256 key width is 32");
const _: () = assert!(AEGIS256_NONCE_LEN == 32, "AEGIS-256 nonce width is 32");
const _: () = assert!(AEGIS256_TAG_LEN == 32, "AEGIS-256 tag width is 32");

// Argon2id v1.0 preset. m_cost=16 MiB is the bringup cap (one quarter of the
// design default 64 MiB) -- bounded by corvus's static 24 MiB heap. State-file
// m_cost_kib is per-record, so a future bump only resizes the heap.
pub const ARGON2_T_COST: u32 = 2;
pub const ARGON2_M_COST_KIB: u32 = 16 * 1024;
pub const ARGON2_PARALLELISM: u32 = 1;

// AD for the keypair-wrap AEAD: "thylacine-corvus-v1" || user_name || backend_id.
pub const AD_PREFIX: &[u8] = b"thylacine-corvus-v1";
pub const BACKEND_ID_PASSPHRASE: u8 = 0;

// =============================================================================
// DEK envelope -- the hybrid-PKE wrapped-DEK blob (P5-corvus-bringup-d).
// =============================================================================

pub const DEK_LEN: usize = 32;
pub const ENVELOPE_VERSION: u8 = 1;
pub const ENV_MLKEM_CT_OFF: usize = 1;
pub const ENV_EPH_PK_OFF: usize = ENV_MLKEM_CT_OFF + MLKEM_CT_LEN; // 1089
pub const ENV_NONCE_OFF: usize = ENV_EPH_PK_OFF + X25519_KEY_LEN; // 1121
pub const ENV_DEK_CT_OFF: usize = ENV_NONCE_OFF + AEGIS256_NONCE_LEN; // 1153
pub const ENV_DEK_TAG_OFF: usize = ENV_DEK_CT_OFF + DEK_LEN; // 1185
pub const ENVELOPE_LEN: usize = ENV_DEK_TAG_OFF + AEGIS256_TAG_LEN; // 1217

const _: () = assert!(ENVELOPE_LEN == 1217, "DEK envelope layout drift");

pub const DEK_KDF_DOMAIN: &[u8] = b"thylacine-corvus-dek-kdf-v1";
pub const DEK_AD_PREFIX: &[u8] = b"thylacine-corvus-dek-v1";

// =============================================================================
// Recovery keyslot -- A-5c (CORVUS-DESIGN.md 5.6 + IDENTITY-DESIGN 9.9).
// =============================================================================
//
// A recovery keyslot is a SECOND wrap of a subject's hybrid keypair under a
// recovery-phrase-derived KEK (the LUKS two-keyslot model). Same CRVS v1 layout
// (TOTAL_LEN); only the AD prefix and the KEK source differ. The phrase is a
// 24-word BIP-39 mnemonic over 256-bit CSPRNG entropy + an 8-bit SHA-256
// checksum (24*11 = 264 = 256+8). The KEK derives from the DECODED ENTROPY (the
// canonical 32-byte form), not the phrase text -- so whitespace/case never
// affect derivation. 256-bit entropy is the security floor; the KDF cost is
// defense-in-depth.
pub const RECOVERY_ENTROPY_BYTES: usize = 32; // 256-bit
pub const RECOVERY_WORD_COUNT: usize = 24;
pub const RECOVERY_CHECKSUM_BITS: usize = 8; // 256 / 32
pub const BIP39_WORD_BITS: usize = 11;
pub const BIP39_WORDLIST_LEN: usize = 2048; // 2^11
pub const BIP39_MAX_WORD_LEN: usize = 8; // longest canonical English word
// Worst-case encoded phrase bytes: 24 words (<=8) + 23 single-space joins.
pub const RECOVERY_PHRASE_MAX: usize = RECOVERY_WORD_COUNT * BIP39_MAX_WORD_LEN + (RECOVERY_WORD_COUNT - 1);

const _: () = assert!(RECOVERY_ENTROPY_BYTES * 8 == 256, "recovery entropy is 256-bit");
const _: () = assert!(
    RECOVERY_WORD_COUNT * BIP39_WORD_BITS == RECOVERY_ENTROPY_BYTES * 8 + RECOVERY_CHECKSUM_BITS,
    "BIP-39 word/bit accounting drift (24*11 == 256+8)"
);
// The wordlist must exactly fill the 11-bit index space, else an 11-bit index
// could read past the array (panic) or leave words unreachable.
const _: () = assert!(
    BIP39_WORDLIST.len() == BIP39_WORDLIST_LEN,
    "BIP-39 wordlist must be exactly 2^11 entries"
);

// AD for the recovery-keyslot AEAD: "thylacine-corvus-recovery-v1" || subject ||
// 0x00. Domain-separated from the passphrase-wrap AD (AD_PREFIX) by the prefix,
// so a recovery wrap can never be unwrapped as a passphrase wrap or vice versa
// even if a file is swapped.
pub const RECOVERY_AD_PREFIX: &[u8] = b"thylacine-corvus-recovery-v1";
pub const RECOVERY_AD_DOMAIN_BYTE: u8 = 0;

// Recovery KDF preset. m_cost stays the interactive 16 MiB (heap-bounded -- the
// libsodium "sensitive" 1 GiB m_cost is a v1.x heap-resize seam); the time cost
// is raised (recovery is rare, so a 4x work factor is affordable). Stored
// per-wrap in the CRVS header, so a future bump is a single-constant change.
pub const RECOVERY_ARGON2_T_COST: u32 = 8;
pub const RECOVERY_ARGON2_M_COST_KIB: u32 = ARGON2_M_COST_KIB;
pub const RECOVERY_ARGON2_PARALLELISM: u32 = 1;

// =============================================================================
// Primitives: Argon2id KDF + AEGIS-256 AEAD + SHA-256.
// =============================================================================

pub fn wipe(buf: &mut [u8]) {
    for b in buf.iter_mut() {
        unsafe { core::ptr::write_volatile(b as *mut u8, 0) };
    }
}

pub fn argon2id_kek(
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

pub fn aegis_wrap(
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

pub fn aegis_unwrap(
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

pub fn sha256_kek(parts: &[&[u8]]) -> [u8; AEGIS256_KEY_LEN] {
    let mut h = Sha256::new();
    for p in parts {
        h.update(p);
    }
    let digest = h.finalize();
    let mut kek = [0u8; AEGIS256_KEY_LEN];
    kek.copy_from_slice(&digest);
    kek
}

// =============================================================================
// Hybrid keypair generation + the DEK envelope.
// =============================================================================

pub fn generate_hybrid_keypair<R: RngCore + CryptoRng>(rng: &mut R) -> Option<[u8; KEYPAIR_LEN]> {
    let x_sk = StaticSecret::random_from_rng(&mut *rng);
    let x_pk = PublicKey::from(&x_sk);
    let (mlkem_dk, mlkem_ek) = MlKem768::generate(&mut *rng);
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

pub fn dek_envelope_wrap<R: RngCore + CryptoRng>(
    rng: &mut R,
    x25519_pk: &[u8; X25519_KEY_LEN],
    mlkem_ek_bytes: &[u8],
    dek: &[u8; DEK_LEN],
    ad_dataset: &[u8],
    key_id: u64,
) -> Option<[u8; ENVELOPE_LEN]> {
    let enc = Encoded::<MlKemEk>::try_from(mlkem_ek_bytes).ok()?;
    let mlkem_ek = MlKemEk::from_bytes(&enc);
    let (mlkem_ct, mut ss_pq) = mlkem_ek.encapsulate(&mut *rng).ok()?;
    if mlkem_ct.len() != MLKEM_CT_LEN || ss_pq.len() != AEGIS256_KEY_LEN {
        wipe(&mut ss_pq[..]);
        return None;
    }

    let eph = EphemeralSecret::random_from_rng(&mut *rng);
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

pub fn dek_envelope_unwrap(
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
// BIP-39 phrase codec.
// =============================================================================

// Look up a single word (case-insensitive, ASCII) in the canonical wordlist;
// returns its 0..2048 index. The list is lexicographically sorted, so binary
// search over the lowercased word resolves it. An unknown or over-long word
// (no canonical word exceeds BIP39_MAX_WORD_LEN) returns None.
pub fn bip39_word_index(word: &[u8]) -> Option<u16> {
    if word.is_empty() || word.len() > BIP39_MAX_WORD_LEN {
        return None;
    }
    let mut lower = [0u8; BIP39_MAX_WORD_LEN];
    for (i, &b) in word.iter().enumerate() {
        lower[i] = b.to_ascii_lowercase();
    }
    let key = core::str::from_utf8(&lower[..word.len()]).ok()?;
    match BIP39_WORDLIST.binary_search(&key) {
        Ok(idx) => Some(idx as u16),
        Err(_) => None,
    }
}

// First 8 bits of SHA-256(entropy) -- the BIP-39 checksum for 256-bit entropy.
pub fn sha256_checksum_byte(entropy: &[u8]) -> u8 {
    let mut h = Sha256::new();
    h.update(entropy);
    h.finalize()[0]
}

// Map a 264-bit buffer (entropy || checksum) to 24 space-joined words, MSB-first
// 11 bits per word. Shared by bip39_encode + the self-test (which feeds a
// deliberately-wrong checksum to exercise the reject path deterministically).
pub fn bip39_words_from_bits(bits: &[u8; RECOVERY_ENTROPY_BYTES + 1]) -> Vec<u8> {
    let mut out: Vec<u8> = Vec::with_capacity(RECOVERY_PHRASE_MAX);
    for w in 0..RECOVERY_WORD_COUNT {
        let mut idx: u16 = 0;
        for b in 0..BIP39_WORD_BITS {
            let p = w * BIP39_WORD_BITS + b;
            let bit = (bits[p / 8] >> (7 - (p % 8))) & 1;
            idx = (idx << 1) | bit as u16;
        }
        if w > 0 {
            out.push(b' ');
        }
        out.extend_from_slice(BIP39_WORDLIST[idx as usize].as_bytes());
    }
    out
}

// Encode 256-bit entropy as a 24-word space-joined BIP-39 phrase.
pub fn bip39_encode(entropy: &[u8; RECOVERY_ENTROPY_BYTES]) -> Vec<u8> {
    let mut bits = [0u8; RECOVERY_ENTROPY_BYTES + 1];
    bits[..RECOVERY_ENTROPY_BYTES].copy_from_slice(entropy);
    bits[RECOVERY_ENTROPY_BYTES] = sha256_checksum_byte(entropy);
    let words = bip39_words_from_bits(&bits);
    wipe(&mut bits); // the buffer holds the entropy bytes
    words
}

// Decode a 24-word phrase to 256-bit entropy. None on: not exactly 24 words, an
// unknown word, or a checksum mismatch (a typo). Splitting is on ASCII
// whitespace (runs collapsed), so leading/trailing/multiple/tab/newline
// separators are all accepted.
pub fn bip39_decode(phrase: &[u8]) -> Option<[u8; RECOVERY_ENTROPY_BYTES]> {
    let mut bits = [0u8; RECOVERY_ENTROPY_BYTES + 1];
    let mut bitpos: usize = 0;
    let mut nwords: usize = 0;
    for word in phrase.split(|&c| c == b' ' || c == b'\t' || c == b'\n' || c == b'\r') {
        if word.is_empty() {
            continue;
        }
        nwords += 1;
        if nwords > RECOVERY_WORD_COUNT {
            wipe(&mut bits);
            return None;
        }
        let idx = match bip39_word_index(word) {
            Some(i) => i,
            None => {
                wipe(&mut bits);
                return None;
            }
        };
        for b in 0..BIP39_WORD_BITS {
            let bit = ((idx >> (BIP39_WORD_BITS - 1 - b)) & 1) as u8;
            if bit != 0 {
                bits[bitpos / 8] |= 1 << (7 - (bitpos % 8));
            }
            bitpos += 1;
        }
    }
    if nwords != RECOVERY_WORD_COUNT {
        wipe(&mut bits);
        return None;
    }
    let mut entropy = [0u8; RECOVERY_ENTROPY_BYTES];
    entropy.copy_from_slice(&bits[..RECOVERY_ENTROPY_BYTES]);
    let cs_ok = sha256_checksum_byte(&entropy) == bits[RECOVERY_ENTROPY_BYTES];
    wipe(&mut bits);
    if !cs_ok {
        wipe(&mut entropy);
        return None;
    }
    Some(entropy)
}

// =============================================================================
// The recovery wrap + the passphrase wrap.
// =============================================================================

// AD for the recovery-keyslot AEAD: prefix || subject || domain-byte. The prefix
// domain-separates it from the passphrase-wrap AD.
pub fn build_recovery_ad(subject: &[u8], out: &mut Vec<u8>) {
    out.clear();
    out.extend_from_slice(RECOVERY_AD_PREFIX);
    out.extend_from_slice(subject);
    out.push(RECOVERY_AD_DOMAIN_BYTE);
}

// The recovery wrap on disk -- the same CRVS v1 layout (TOTAL_LEN) as
// hybrid.corvus, distinct only by AD + KEK source.
pub struct RecoveryWrap {
    pub t_cost: u32,
    pub m_cost_kib: u32,
    pub parallelism: u32,
    pub salt: [u8; ARGON2_SALT_LEN],
    pub nonce: [u8; AEGIS256_NONCE_LEN],
    pub ciphertext: [u8; KEYPAIR_LEN],
    pub tag: [u8; AEGIS256_TAG_LEN],
}

impl RecoveryWrap {
    pub fn to_bytes(&self) -> [u8; TOTAL_LEN] {
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

    pub fn from_bytes(blob: &[u8]) -> Option<RecoveryWrap> {
        if blob.len() != TOTAL_LEN {
            return None;
        }
        let magic = u32::from_le_bytes([blob[0], blob[1], blob[2], blob[3]]);
        let version = u32::from_le_bytes([blob[4], blob[5], blob[6], blob[7]]);
        if magic != CORVUS_MAGIC || version != CORVUS_STATE_VERSION {
            return None;
        }
        let t_cost = u32::from_le_bytes([blob[8], blob[9], blob[10], blob[11]]);
        let m64 = u64::from_le_bytes([
            blob[12], blob[13], blob[14], blob[15], blob[16], blob[17], blob[18], blob[19],
        ]);
        if m64 > u32::MAX as u64 {
            return None;
        }
        let parallelism = u32::from_le_bytes([blob[20], blob[21], blob[22], blob[23]]);
        let mut salt = [0u8; ARGON2_SALT_LEN];
        let mut nonce = [0u8; AEGIS256_NONCE_LEN];
        let mut ciphertext = [0u8; KEYPAIR_LEN];
        let mut tag = [0u8; AEGIS256_TAG_LEN];
        salt.copy_from_slice(&blob[24..40]);
        nonce.copy_from_slice(&blob[40..72]);
        ciphertext.copy_from_slice(&blob[72..72 + KEYPAIR_LEN]);
        tag.copy_from_slice(&blob[72 + KEYPAIR_LEN..72 + KEYPAIR_LEN + AEGIS256_TAG_LEN]);
        Some(RecoveryWrap {
            t_cost,
            m_cost_kib: m64 as u32,
            parallelism,
            salt,
            nonce,
            ciphertext,
            tag,
        })
    }
}

// Wrap a keypair under a FRESH recovery keyslot for `subject`, keyed by the
// decoded 256-bit `entropy`. RNG-supplied salt + nonce, derive the recovery KEK,
// AEAD-seal. None on RNG/KDF failure.
pub fn make_recovery_wrap<R: RngCore + CryptoRng>(
    rng: &mut R,
    subject: &[u8],
    entropy: &[u8; RECOVERY_ENTROPY_BYTES],
    keypair: &[u8; KEYPAIR_LEN],
) -> Option<RecoveryWrap> {
    let mut salt = [0u8; ARGON2_SALT_LEN];
    rng.try_fill_bytes(&mut salt).ok()?;
    let mut nonce = [0u8; AEGIS256_NONCE_LEN];
    rng.try_fill_bytes(&mut nonce).ok()?;
    let mut kek = argon2id_kek(
        entropy,
        &salt,
        RECOVERY_ARGON2_T_COST,
        RECOVERY_ARGON2_M_COST_KIB,
        RECOVERY_ARGON2_PARALLELISM,
    )?;
    let mut ad: Vec<u8> = Vec::new();
    build_recovery_ad(subject, &mut ad);
    let mut ciphertext = [0u8; KEYPAIR_LEN];
    let tag = aegis_wrap(&kek, &nonce, &ad, keypair, &mut ciphertext);
    wipe(&mut kek);
    Some(RecoveryWrap {
        t_cost: RECOVERY_ARGON2_T_COST,
        m_cost_kib: RECOVERY_ARGON2_M_COST_KIB,
        parallelism: RECOVERY_ARGON2_PARALLELISM,
        salt,
        nonce,
        ciphertext,
        tag,
    })
}

// Unwrap a recovery keyslot with `entropy`. None on KDF failure or AEAD tag
// mismatch (wrong phrase). The recovered keypair value EQUALS the passphrase
// wrap's plaintext (same keypair, two keyslots) -- the invariant the DEK
// envelopes depend on.
pub fn unwrap_recovery(
    subject: &[u8],
    entropy: &[u8; RECOVERY_ENTROPY_BYTES],
    rw: &RecoveryWrap,
) -> Option<[u8; KEYPAIR_LEN]> {
    let mut kek = argon2id_kek(entropy, &rw.salt, rw.t_cost, rw.m_cost_kib, rw.parallelism)?;
    let mut ad: Vec<u8> = Vec::new();
    build_recovery_ad(subject, &mut ad);
    let res = aegis_unwrap(&kek, &rw.nonce, &ad, &rw.ciphertext, &rw.tag);
    wipe(&mut kek);
    let mut plain = res?;
    if plain.len() != KEYPAIR_LEN {
        wipe(&mut plain);
        return None;
    }
    let mut kp = [0u8; KEYPAIR_LEN];
    kp.copy_from_slice(&plain);
    wipe(&mut plain);
    Some(kp)
}

// Shared "wrap the keypair under a passphrase" path -- USER_CREATE's initial
// wrap AND RECOVER's re-wrap under a new passphrase. One code path keeps the AD
// + nonce discipline identical at both sites. None on RNG/KDF failure.
pub struct KeypairWrap {
    pub salt: [u8; ARGON2_SALT_LEN],
    pub nonce: [u8; AEGIS256_NONCE_LEN],
    pub ciphertext: [u8; KEYPAIR_LEN],
    pub tag: [u8; AEGIS256_TAG_LEN],
}

pub fn wrap_keypair_passphrase<R: RngCore + CryptoRng>(
    rng: &mut R,
    subject: &[u8],
    passphrase: &[u8],
    keypair: &[u8; KEYPAIR_LEN],
) -> Option<KeypairWrap> {
    let mut salt = [0u8; ARGON2_SALT_LEN];
    rng.try_fill_bytes(&mut salt).ok()?;
    let mut nonce = [0u8; AEGIS256_NONCE_LEN];
    rng.try_fill_bytes(&mut nonce).ok()?;
    let mut kek = argon2id_kek(
        passphrase,
        &salt,
        ARGON2_T_COST,
        ARGON2_M_COST_KIB,
        ARGON2_PARALLELISM,
    )?;
    let mut ad: Vec<u8> = Vec::new();
    ad.extend_from_slice(AD_PREFIX);
    ad.extend_from_slice(subject);
    ad.push(BACKEND_ID_PASSPHRASE);
    let mut ciphertext = [0u8; KEYPAIR_LEN];
    let tag = aegis_wrap(&kek, &nonce, &ad, keypair, &mut ciphertext);
    wipe(&mut kek);
    Some(KeypairWrap {
        salt,
        nonce,
        ciphertext,
        tag,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    // Deterministic test RNG -- a counter stream. CryptoRng is a marker the wrap
    // functions require; for tests the bytes need only be distinct, not secure.
    struct CounterRng(u64);
    impl RngCore for CounterRng {
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
            for b in dest.iter_mut() {
                self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
                *b = (self.0 >> 33) as u8;
            }
        }
        fn try_fill_bytes(&mut self, dest: &mut [u8]) -> Result<(), rand_core::Error> {
            self.fill_bytes(dest);
            Ok(())
        }
    }
    impl CryptoRng for CounterRng {}

    #[test]
    fn bip39_round_trip() {
        let mut ent = [0u8; RECOVERY_ENTROPY_BYTES];
        for (i, b) in ent.iter_mut().enumerate() {
            *b = (i as u8).wrapping_mul(7).wrapping_add(3);
        }
        let phrase = bip39_encode(&ent);
        let nwords = phrase.split(|&c| c == b' ').filter(|w| !w.is_empty()).count();
        assert_eq!(nwords, RECOVERY_WORD_COUNT);
        assert_eq!(bip39_decode(&phrase), Some(ent));
    }

    #[test]
    fn bip39_rejects_bad_checksum() {
        let ent = [0x5au8; RECOVERY_ENTROPY_BYTES];
        let mut bits = [0u8; RECOVERY_ENTROPY_BYTES + 1];
        bits[..RECOVERY_ENTROPY_BYTES].copy_from_slice(&ent);
        bits[RECOVERY_ENTROPY_BYTES] = sha256_checksum_byte(&ent) ^ 0xff;
        let bad = bip39_words_from_bits(&bits);
        assert!(bip39_decode(&bad).is_none());
    }

    #[test]
    fn bip39_rejects_unknown_word() {
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
        assert!(bip39_decode(&unk).is_none());
    }

    #[test]
    fn bip39_decode_is_whitespace_canonical() {
        let mut ent = [0u8; RECOVERY_ENTROPY_BYTES];
        for (i, b) in ent.iter_mut().enumerate() {
            *b = i as u8;
        }
        let phrase = bip39_encode(&ent);
        // Re-join with mixed/extra whitespace + uppercase -- decode must agree.
        let s = core::str::from_utf8(&phrase).unwrap();
        let mut messy = Vec::new();
        messy.extend_from_slice(b"  ");
        for (i, w) in s.split(' ').enumerate() {
            if i > 0 {
                messy.extend_from_slice(b"\t \n");
            }
            messy.extend_from_slice(w.to_uppercase().as_bytes());
        }
        messy.extend_from_slice(b"  ");
        assert_eq!(bip39_decode(&messy), Some(ent));
    }

    #[test]
    fn recovery_wrap_byte_round_trip() {
        let rw = RecoveryWrap {
            t_cost: 8,
            m_cost_kib: 16 * 1024,
            parallelism: 1,
            salt: [0x11u8; ARGON2_SALT_LEN],
            nonce: [0x22u8; AEGIS256_NONCE_LEN],
            ciphertext: [0x33u8; KEYPAIR_LEN],
            tag: [0x44u8; AEGIS256_TAG_LEN],
        };
        let blob = rw.to_bytes();
        assert_eq!(blob.len(), TOTAL_LEN);
        let rw2 = RecoveryWrap::from_bytes(&blob).expect("parse");
        assert_eq!(rw2.t_cost, rw.t_cost);
        assert_eq!(rw2.m_cost_kib, rw.m_cost_kib);
        assert_eq!(rw2.parallelism, rw.parallelism);
        assert_eq!(rw2.salt, rw.salt);
        assert_eq!(rw2.nonce, rw.nonce);
        assert_eq!(&rw2.ciphertext[..], &rw.ciphertext[..]);
        assert_eq!(rw2.tag, rw.tag);
        // Magic + version pinned at the documented offsets.
        assert_eq!(&blob[0..4], &CORVUS_MAGIC.to_le_bytes());
        assert_eq!(&blob[4..8], &CORVUS_STATE_VERSION.to_le_bytes());
    }

    #[test]
    fn recovery_wrap_rejects_bad_magic() {
        let rw = RecoveryWrap {
            t_cost: 1,
            m_cost_kib: 8,
            parallelism: 1,
            salt: [0u8; ARGON2_SALT_LEN],
            nonce: [0u8; AEGIS256_NONCE_LEN],
            ciphertext: [0u8; KEYPAIR_LEN],
            tag: [0u8; AEGIS256_TAG_LEN],
        };
        let mut blob = rw.to_bytes();
        blob[0] ^= 0xff;
        assert!(RecoveryWrap::from_bytes(&blob).is_none());
        assert!(RecoveryWrap::from_bytes(&blob[..TOTAL_LEN - 1]).is_none());
    }

    #[test]
    fn aead_ad_domain_separation() {
        // A recovery wrap cannot be opened under a foreign subject's AD nor under
        // the passphrase-wrap AD -- the property the file-swap defense rests on.
        let key = [0x5au8; AEGIS256_KEY_LEN];
        let nonce = [0xa5u8; AEGIS256_NONCE_LEN];
        let subject = b"alice";
        let mut keypair = [0u8; KEYPAIR_LEN];
        for (i, b) in keypair.iter_mut().enumerate() {
            *b = (i & 0xff) as u8;
        }
        let mut ad: Vec<u8> = Vec::new();
        build_recovery_ad(subject, &mut ad);
        let mut ct = [0u8; KEYPAIR_LEN];
        let tag = aegis_wrap(&key, &nonce, &ad, &keypair, &mut ct);

        // Correct AD opens.
        let ok = aegis_unwrap(&key, &nonce, &ad, &ct, &tag).expect("open");
        assert_eq!(&ok[..], &keypair[..]);
        // Foreign subject fails.
        let mut ad_other: Vec<u8> = Vec::new();
        build_recovery_ad(b"bob", &mut ad_other);
        assert!(aegis_unwrap(&key, &nonce, &ad_other, &ct, &tag).is_none());
        // Passphrase-wrap AD (same subject, different prefix) fails.
        let mut ad_pass: Vec<u8> = Vec::new();
        ad_pass.extend_from_slice(AD_PREFIX);
        ad_pass.extend_from_slice(subject);
        ad_pass.push(BACKEND_ID_PASSPHRASE);
        assert!(aegis_unwrap(&key, &nonce, &ad_pass, &ct, &tag).is_none());
    }

    #[test]
    fn passphrase_wrap_unwrap_round_trip() {
        // Full Argon2id+AEGIS path: wrap a keypair, then reconstruct the KEK the
        // same way and AEAD-open it. Proves wrap_keypair_passphrase produces an
        // openable wrap (the AUTH path's inverse).
        let mut rng = CounterRng(0x1234_5678_9abc_def0);
        let subject = b"carol";
        let pass = b"correct horse battery staple";
        let mut keypair = [0u8; KEYPAIR_LEN];
        for (i, b) in keypair.iter_mut().enumerate() {
            *b = (i.wrapping_mul(31) & 0xff) as u8;
        }
        let kw = wrap_keypair_passphrase(&mut rng, subject, pass, &keypair).expect("wrap");
        let kek = argon2id_kek(pass, &kw.salt, ARGON2_T_COST, ARGON2_M_COST_KIB, ARGON2_PARALLELISM)
            .expect("kek");
        let mut ad: Vec<u8> = Vec::new();
        ad.extend_from_slice(AD_PREFIX);
        ad.extend_from_slice(subject);
        ad.push(BACKEND_ID_PASSPHRASE);
        let plain = aegis_unwrap(&kek, &kw.nonce, &ad, &kw.ciphertext, &kw.tag).expect("open");
        assert_eq!(&plain[..], &keypair[..]);
        // Wrong passphrase fails the tag.
        let kek_bad = argon2id_kek(b"wrong", &kw.salt, ARGON2_T_COST, ARGON2_M_COST_KIB, ARGON2_PARALLELISM)
            .expect("kek");
        assert!(aegis_unwrap(&kek_bad, &kw.nonce, &ad, &kw.ciphertext, &kw.tag).is_none());
    }

    #[test]
    fn recovery_keyslot_full_round_trip() {
        // Mint a recovery wrap over a keypair + entropy, then unwrap with the same
        // entropy -> the SAME keypair (the no-DEK-rewrite invariant). Wrong
        // entropy fails.
        let mut rng = CounterRng(0xdead_beef_cafe_0001);
        let subject = b"dave";
        let mut entropy = [0u8; RECOVERY_ENTROPY_BYTES];
        rng.fill_bytes(&mut entropy);
        let mut keypair = [0u8; KEYPAIR_LEN];
        for (i, b) in keypair.iter_mut().enumerate() {
            *b = (i.wrapping_mul(17).wrapping_add(5) & 0xff) as u8;
        }
        let rw = make_recovery_wrap(&mut rng, subject, &entropy, &keypair).expect("mint");
        let got = unwrap_recovery(subject, &entropy, &rw).expect("unwrap");
        assert_eq!(&got[..], &keypair[..]);
        let mut bad_entropy = entropy;
        bad_entropy[0] ^= 1;
        assert!(unwrap_recovery(subject, &bad_entropy, &rw).is_none());
    }

    #[test]
    fn keypair_generation_layout() {
        let mut rng = CounterRng(0x0102_0304_0506_0708);
        let kp = generate_hybrid_keypair(&mut rng).expect("keygen");
        assert_eq!(kp.len(), KEYPAIR_LEN);
        // The X25519 public half must derive from the secret half.
        let mut sk = [0u8; X25519_KEY_LEN];
        sk.copy_from_slice(&kp[KP_X25519_SK_OFF..KP_X25519_SK_OFF + X25519_KEY_LEN]);
        let pk = PublicKey::from(&StaticSecret::from(sk));
        assert_eq!(pk.as_bytes(), &kp[KP_X25519_PK_OFF..KP_X25519_PK_OFF + X25519_KEY_LEN]);
    }
}
