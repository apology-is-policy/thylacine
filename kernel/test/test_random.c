// Kernel CSPRNG + ChaCha20 core tests (Lazarus W3).
//
// The ChaCha20 block test pins the cipher against the canonical
// zero-key / zero-nonce / zero-counter keystream vector (RFC 8439 /
// DJB), so the primitive is proven correct independent of any RNG
// state-machine behavior. The kern_random tests exercise the
// forward-secure CSPRNG layered on top (kernel/random.c): non-zero
// output, fresh bytes per read, and the rekey-on-buffer-drain path for
// reads larger than the internal keystream buffer.

#include "test.h"

#include <thylacine/types.h>
#include <thylacine/chacha20.h>
#include <thylacine/random.h>

void test_chacha20_block_vector(void);
void test_chacha20_keystream_continuity(void);
void test_kern_random_two_reads_differ(void);
void test_kern_random_large_read_nonzero(void);
void test_kern_random_virtio_reseed(void);

static bool bytes_eq(const u8 *a, const u8 *b, u32 n) {
    for (u32 i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

// ChaCha20 keystream for an all-zero 256-bit key, all-zero 64-bit nonce,
// block counter 0 -- the canonical first-block test vector.
static const u8 chacha_zero_vector[64] = {
    0x76, 0xb8, 0xe0, 0xad, 0xa0, 0xf1, 0x3d, 0x90,
    0x40, 0x5d, 0x6a, 0xe5, 0x53, 0x86, 0xbd, 0x28,
    0xbd, 0xd2, 0x19, 0xb8, 0xa0, 0x8d, 0xed, 0x1a,
    0xa8, 0x36, 0xef, 0xcc, 0x8b, 0x77, 0x0d, 0xc7,
    0xda, 0x41, 0x59, 0x7c, 0x51, 0x57, 0x48, 0x8d,
    0x77, 0x24, 0xe0, 0x3f, 0xb8, 0xd8, 0x4a, 0x37,
    0x6a, 0x43, 0xb8, 0xf4, 0x15, 0x18, 0xa1, 0x1c,
    0xc3, 0x87, 0xb6, 0x69, 0xb2, 0xee, 0x65, 0x86,
};

void test_chacha20_block_vector(void) {
    u8 zero_key[CHACHA_KEYSZ] = {0};
    u8 zero_iv[CHACHA_IVSZ]   = {0};
    struct chacha_ctx x;
    chacha_keysetup(&x, zero_key);
    chacha_ivsetup(&x, zero_iv);

    u8 out[64];
    chacha_keystream(&x, out, sizeof(out));

    TEST_ASSERT(bytes_eq(out, chacha_zero_vector, sizeof(out)),
                "ChaCha20 zero-key/nonce/counter keystream matches the canonical vector");
}

void test_chacha20_keystream_continuity(void) {
    u8 zero_key[CHACHA_KEYSZ] = {0};
    u8 zero_iv[CHACHA_IVSZ]   = {0};
    struct chacha_ctx x;
    chacha_keysetup(&x, zero_key);
    chacha_ivsetup(&x, zero_iv);

    // 128 bytes = two blocks from one context; the counter advances.
    u8 out[128];
    chacha_keystream(&x, out, sizeof(out));

    TEST_ASSERT(bytes_eq(out, chacha_zero_vector, 64),
                "first 64 bytes of a 128-byte keystream match block 0");
    TEST_ASSERT(!bytes_eq(out, out + 64, 64),
                "block 1 differs from block 0 (counter advanced)");
}

void test_kern_random_two_reads_differ(void) {
    u8 a[32] = {0};
    u8 b[32] = {0};
    TEST_EXPECT_EQ(kern_random_bytes(a, (long)sizeof(a)), (long)sizeof(a),
                   "first 32-byte read fills");
    TEST_EXPECT_EQ(kern_random_bytes(b, (long)sizeof(b)), (long)sizeof(b),
                   "second 32-byte read fills");
    TEST_ASSERT(!bytes_eq(a, b, sizeof(a)),
                "two consecutive CSPRNG reads differ (stream advances)");
}

// A read larger than the internal keystream buffer (RNG_BUFSZ = 1024)
// exercises the rekey-on-drain refill path across buffer boundaries.
static u8 g_big_read[2048];

void test_kern_random_large_read_nonzero(void) {
    long n = (long)sizeof(g_big_read);
    TEST_EXPECT_EQ(kern_random_bytes(g_big_read, n), n,
                   "2048-byte read fills (crosses internal buffer boundary)");
    bool any_nonzero = false;
    for (long i = 0; i < n; i++) if (g_big_read[i] != 0) { any_nonzero = true; break; }
    TEST_ASSERT(any_nonzero, "large CSPRNG read is not all-zero");
}

// The kernel virtio-rng driver: a full bring-up -> pull -> teardown on
// QEMU's attached virtio-rng device. Boot already did one pull (main.c
// after virtio_init); this re-exercises the device cycle the threshold
// reseed depends on, and confirms the pool still serves afterward.
void test_kern_random_virtio_reseed(void) {
    size_t got = random_seed_from_virtio();
    TEST_ASSERT(got > 0, "virtio-rng pull returns entropy");
    TEST_ASSERT(kern_random_virtio_contributed(),
                "a virtio-rng pull has contributed to the pool");

    u8 buf[32] = {0};
    TEST_EXPECT_EQ(kern_random_bytes(buf, (long)sizeof(buf)), (long)sizeof(buf),
                   "CSPRNG serves after a virtio reseed");
}
