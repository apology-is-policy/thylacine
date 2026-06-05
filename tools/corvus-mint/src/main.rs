// corvus-mint -- A-5c-b host-target system-identity minter.
//
// Generates the admin hybrid keypair, wraps it under the build-time system
// passphrase (-> system-wrap) and under a fresh 24-word BIP-39 recovery phrase
// (-> system-recovery-wrap), self-verifies BOTH keyslots unwrap to the same
// keypair, writes the two CRVS-v1 blobs into <out-dir>, and prints the recovery
// phrase to stdout (the build logs it -- forensic, like the mkfs seed). Reuses
// corvus's exact crypto via corvus-crypto so the wraps are byte-identical to
// what the on-device corvus reads at boot.
//
// v1.0: the build-time system passphrase defaults to the known "thylacine"
// constant (overridable via CORVUS_SYSTEM_PASSPHRASE) so joey's ADMIN_ELEVATE
// boot E2E stays green and the build is reproducible; a v1.x installer supplies
// a real per-install secret. The WRAP is real Argon2id+AEGIS either way.

use corvus_crypto::{
    bip39_encode, generate_hybrid_keypair, make_recovery_wrap, unwrap_keypair_passphrase,
    unwrap_recovery, wipe, wrap_keypair_passphrase, RECOVERY_ENTROPY_BYTES, SYSTEM_WRAP_SUBJECT,
};
use rand::RngCore;
use std::io::Write;

fn write_blob(path: &str, bytes: &[u8]) {
    let mut f = std::fs::File::create(path).unwrap_or_else(|e| {
        eprintln!("corvus-mint: create {}: {}", path, e);
        std::process::exit(1);
    });
    f.write_all(bytes).unwrap_or_else(|e| {
        eprintln!("corvus-mint: write {}: {}", path, e);
        std::process::exit(1);
    });
    f.sync_all().ok();
}

fn main() {
    let out_dir = std::env::args().nth(1).unwrap_or_else(|| {
        eprintln!("usage: corvus-mint <out-dir>  (writes <out-dir>/{{system-wrap,system-recovery-wrap}})");
        std::process::exit(2);
    });
    let passphrase =
        std::env::var("CORVUS_SYSTEM_PASSPHRASE").unwrap_or_else(|_| "thylacine".to_string());

    let mut rng = rand::rngs::OsRng;

    let mut keypair = generate_hybrid_keypair(&mut rng).expect("corvus-mint: keygen failed");

    // system-wrap: the admin keypair under the system passphrase.
    let sw = wrap_keypair_passphrase(&mut rng, SYSTEM_WRAP_SUBJECT, passphrase.as_bytes(), &keypair)
        .expect("corvus-mint: system-wrap failed");

    // system-recovery-wrap: the SAME keypair under a fresh recovery phrase.
    let mut entropy = [0u8; RECOVERY_ENTROPY_BYTES];
    rng.fill_bytes(&mut entropy);
    let phrase = bip39_encode(&entropy);
    let srw = make_recovery_wrap(&mut rng, SYSTEM_WRAP_SUBJECT, &entropy, &keypair)
        .expect("corvus-mint: system-recovery-wrap failed");

    // Self-verify BOTH keyslots unwrap to the EXACT keypair the device will read
    // -- catches any wrap/unwrap mismatch before the bytes are baked into the pool.
    let mut v1 = unwrap_keypair_passphrase(SYSTEM_WRAP_SUBJECT, passphrase.as_bytes(), &sw)
        .expect("corvus-mint: system-wrap self-verify unwrap failed");
    assert!(v1 == keypair, "corvus-mint: system-wrap self-verify MISMATCH");
    let mut v2 = unwrap_recovery(SYSTEM_WRAP_SUBJECT, &entropy, &srw)
        .expect("corvus-mint: system-recovery-wrap self-verify unwrap failed");
    assert!(v2 == keypair, "corvus-mint: system-recovery-wrap self-verify MISMATCH");

    write_blob(&format!("{}/system-wrap", out_dir), &sw.to_bytes());
    write_blob(&format!("{}/system-recovery-wrap", out_dir), &srw.to_bytes());

    // The recovery phrase -- to stdout for the build log. At v1.0 the system
    // passphrase is the known constant, so this phrase is the only fresh secret
    // and it exists only at host-bake time.
    println!("{}", std::str::from_utf8(&phrase).expect("corvus-mint: phrase not utf8"));
    eprintln!(
        "corvus-mint: wrote system-wrap + system-recovery-wrap to {} (subject 'system', passphrase '{}')",
        out_dir, passphrase
    );

    wipe(&mut keypair);
    wipe(&mut entropy);
    wipe(&mut v1);
    wipe(&mut v2);
}
