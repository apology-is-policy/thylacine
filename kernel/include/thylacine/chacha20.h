// ChaCha20 stream cipher core (Lazarus W3).
//
// The keystream primitive under the kernel CSPRNG (kernel/random.c). A
// pure, allocation-free, lock-free transform: 16 u32 words run through
// 20 rounds per 64-byte block. RFC 8439 / D. J. Bernstein's reference
// "chacha_private.h" layout (4 sigma constants + 8 key + 2 counter + 2
// nonce). The arc4random construction layered on top in random.c is the
// reseed/forward-secrecy state machine; this file is only the cipher.
//
// Naming: ChaCha20 is an industry-spec primitive, so the spec name wins
// over a thematic rename -- the same precedent as keeping "virtio" /
// "vring" / "9P" (the cross-implementation contract is the spec).

#ifndef THYLACINE_CHACHA20_H
#define THYLACINE_CHACHA20_H

#include <thylacine/types.h>

#define CHACHA_KEYSZ    32u   // 256-bit key
#define CHACHA_IVSZ      8u   // 64-bit nonce (the 64-bit block counter is separate)
#define CHACHA_BLOCKSZ  64u   // keystream block size

// ChaCha20 context: the 16-word state. Treat as opaque; mutated by
// chacha_keystream as the block counter advances.
struct chacha_ctx {
    u32 input[16];
};

// Install the 256-bit key (the four sigma constant words too). Leaves
// the counter/nonce words untouched -- call chacha_ivsetup next.
void chacha_keysetup(struct chacha_ctx *x, const u8 key[CHACHA_KEYSZ]);

// Install the 64-bit nonce and reset the 64-bit block counter to 0.
void chacha_ivsetup(struct chacha_ctx *x, const u8 iv[CHACHA_IVSZ]);

// Emit `bytes` of raw keystream into `out`, advancing the block counter
// (encrypting plaintext = XOR the caller's data against this stream).
// `out` must hold at least `bytes` bytes.
void chacha_keystream(struct chacha_ctx *x, u8 *out, u32 bytes);

#endif // THYLACINE_CHACHA20_H
