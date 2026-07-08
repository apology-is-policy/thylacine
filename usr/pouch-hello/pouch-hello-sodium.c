// /pouch-hello-sodium — exercising the libsodium cross-build
// (Phase 6 sub-chunk 14 — pouch-libsodium).
//
// POUCH-DESIGN.md §13's libsodium exit criterion: "libsodium cross-
// compiles against pouch and its self-test passes." This proving binary
// is the Thylacine-side self-test — it cross-compiles against pouch's
// sysroot (which now ships libsodium.a) and runs the primitives Stratum
// will actually consume: chacha20-poly1305-IETF AEAD round-trip,
// SHA-256 KAT, BLAKE2b round-trip, ed25519 sign + verify + reject-tamper.
//
// CSPRNG: libsodium's default randombytes_sysrandom implementation calls
// musl's getentropy(3) (HAVE_GETENTROPY defined), which falls through to
// getrandom(2), which reaches the kernel SYS_GETRANDOM handler (gated
// on CAP_CSPRNG_READ). joey spawns this binary with the cap explicitly
// via pouch_smoke_one_caps. Same path /pouch-hello-getrandom proved at
// sub-chunk 11.
//
// Five probes; exit 0 on full success.

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

#include <sodium.h>

// SHA-256("abc") — FIPS 180-4 worked example.
static const unsigned char kat_sha256_abc[32] = {
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
};

static int hex_print(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    return 0;
}

int main(void) {
    // 1. sodium_init — the library's self-check. Returns 0 on first init,
    //    1 if already initialized, -1 on permanent failure (CSPRNG broken
    //    or other initialization fault).
    int rc = sodium_init();
    if (rc < 0) {
        printf("pouch-hello-sodium: FAIL sodium_init rc=%d\n", rc);
        fflush(stdout);
        return 1;
    }
    printf("pouch-hello-sodium: sodium_init -> %d (ok, version %s)\n",
           rc, sodium_version_string());
    fflush(stdout);

    // 2. SHA-256 KAT — confirms the soft-float compiler runtime + the
    //    portable upper half pulled the bit-twiddling math correctly.
    unsigned char sha[crypto_hash_sha256_BYTES];
    static const unsigned char abc[] = { 'a', 'b', 'c' };
    if (crypto_hash_sha256(sha, abc, sizeof abc) != 0) {
        printf("pouch-hello-sodium: FAIL crypto_hash_sha256\n");
        fflush(stdout);
        return 2;
    }
    if (memcmp(sha, kat_sha256_abc, sizeof sha) != 0) {
        printf("pouch-hello-sodium: FAIL sha256(\"abc\") = ");
        hex_print(sha, sizeof sha);
        printf(" (expected ba7816bf...)\n");
        fflush(stdout);
        return 3;
    }
    printf("pouch-hello-sodium: sha256(\"abc\") KAT ok\n");
    fflush(stdout);

    // 3. BLAKE2b round-trip — hash twice, verify equality. A weaker
    //    invariant than a KAT but exercises a different code path
    //    (BLAKE2b's compression vs SHA-256's). Two consecutive hashes of
    //    the same input must match bit-exactly.
    unsigned char b2a[crypto_generichash_BYTES];
    unsigned char b2b[crypto_generichash_BYTES];
    static const unsigned char b2_in[] = "Hello from pouch on Thylacine!";
    if (crypto_generichash(b2a, sizeof b2a, b2_in, sizeof b2_in - 1, NULL, 0) != 0 ||
        crypto_generichash(b2b, sizeof b2b, b2_in, sizeof b2_in - 1, NULL, 0) != 0) {
        printf("pouch-hello-sodium: FAIL crypto_generichash\n");
        fflush(stdout);
        return 4;
    }
    if (memcmp(b2a, b2b, sizeof b2a) != 0) {
        printf("pouch-hello-sodium: FAIL blake2b repeat-hash mismatch\n");
        fflush(stdout);
        return 5;
    }
    printf("pouch-hello-sodium: blake2b-256 round-trip ok\n");
    fflush(stdout);

    // 4. xchacha20-poly1305-IETF AEAD round-trip. Exercises the CSPRNG
    //    (randombytes_buf -> getentropy -> SYS_GETRANDOM), the AEAD
    //    encrypt path, and the AEAD decrypt path with constant-time
    //    Poly1305 tag verification. The decrypted plaintext must equal
    //    the original.
    unsigned char key[crypto_aead_xchacha20poly1305_ietf_KEYBYTES];
    unsigned char nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
    randombytes_buf(key, sizeof key);
    randombytes_buf(nonce, sizeof nonce);
    static const unsigned char pt[]  = "thylacinus cynocephalus";
    static const unsigned char ad[]  = "lazarus-species";
    unsigned char ct[sizeof pt - 1 + crypto_aead_xchacha20poly1305_ietf_ABYTES];
    unsigned long long ct_len = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            ct, &ct_len, pt, sizeof pt - 1,
            ad, sizeof ad - 1, NULL, nonce, key) != 0) {
        printf("pouch-hello-sodium: FAIL xchacha20poly1305 encrypt\n");
        fflush(stdout);
        return 6;
    }
    unsigned char rt[sizeof pt - 1];
    unsigned long long rt_len = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            rt, &rt_len, NULL, ct, ct_len,
            ad, sizeof ad - 1, nonce, key) != 0) {
        printf("pouch-hello-sodium: FAIL xchacha20poly1305 decrypt\n");
        fflush(stdout);
        return 7;
    }
    if (rt_len != sizeof pt - 1 || memcmp(rt, pt, sizeof pt - 1) != 0) {
        printf("pouch-hello-sodium: FAIL xchacha20poly1305 round-trip mismatch\n");
        fflush(stdout);
        return 8;
    }
    printf("pouch-hello-sodium: xchacha20-poly1305 round-trip ok\n");
    fflush(stdout);

    // 5. ed25519 sign + verify + reject-tampered-signature. Exercises the
    //    ref10 ed25519 implementation (curve25519 / GF(2^255 - 19) arith,
    //    the fe_25_5 field-element representation), HAVE_TI_MODE codegen
    //    for 128-bit intermediates, plus the SHA-512 hash that ed25519's
    //    Schnorr-style signing depends on.
    unsigned char pk[crypto_sign_PUBLICKEYBYTES];
    unsigned char sk[crypto_sign_SECRETKEYBYTES];
    if (crypto_sign_keypair(pk, sk) != 0) {
        printf("pouch-hello-sodium: FAIL ed25519 keypair\n");
        fflush(stdout);
        return 9;
    }
    static const unsigned char msg[] = "Stratum is the OS's filesystem.";
    unsigned char sig[crypto_sign_BYTES];
    unsigned long long sig_len = 0;
    if (crypto_sign_detached(sig, &sig_len, msg, sizeof msg - 1, sk) != 0) {
        printf("pouch-hello-sodium: FAIL ed25519 sign\n");
        fflush(stdout);
        return 10;
    }
    if (crypto_sign_verify_detached(sig, msg, sizeof msg - 1, pk) != 0) {
        printf("pouch-hello-sodium: FAIL ed25519 verify on valid sig\n");
        fflush(stdout);
        return 11;
    }
    // Tamper with the signature; verify MUST reject.
    sig[0] ^= 0x01;
    if (crypto_sign_verify_detached(sig, msg, sizeof msg - 1, pk) == 0) {
        printf("pouch-hello-sodium: FAIL ed25519 accepted tampered sig\n");
        fflush(stdout);
        return 12;
    }
    printf("pouch-hello-sodium: ed25519 sign + verify + reject-tampered ok\n");
    fflush(stdout);

    // 6. The AEAD-lever wiring proof: libsodium's armcrypto selection must
    // AGREE with the kernel's AT_HWCAP word (bit 3 = HWCAP_AES, the Linux
    // number). On an AES-capable CPU (QEMU-virt TCG -cpu max, HVF on
    // M-series) this proves the hardware AEGIS path is live end-to-end
    // (kernel ID-reg decode -> auxv -> musl getauxval -> the runtime
    // picker); on an AES-less CPU (RPi4's A72) it proves the fail-safe
    // fallback. A DISAGREEMENT either way is the regression: HWCAP-set
    // but soft-picked = the gate wiring broke (the 20-of-21-seconds
    // soft-decrypt regime returns silently); HWCAP-clear but
    // armcrypto-picked = a SIGILL time bomb.
    {
        unsigned long hw = getauxval(AT_HWCAP);
        int aes_hw = (hw & (1ul << 3)) != 0;
        int aes_sodium = sodium_runtime_has_armcrypto();
        if (aes_hw != aes_sodium) {
            printf("pouch-hello-sodium: FAIL armcrypto gate disagrees "
                   "(AT_HWCAP aes=%d, sodium armcrypto=%d)\n",
                   aes_hw, aes_sodium);
            fflush(stdout);
            return 13;
        }
        printf("pouch-hello-sodium: armcrypto gate ok (AT_HWCAP=0x%lx, "
               "aes=%d -> %s AEGIS)\n",
               hw, aes_hw, aes_hw ? "HARDWARE" : "soft");
        fflush(stdout);
    }

    printf("pouch-hello-sodium: exit 0\n");
    fflush(stdout);
    return 0;
}
