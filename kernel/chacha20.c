// ChaCha20 keystream core (Lazarus W3). See chacha20.h.
//
// Adapted from D. J. Bernstein's public-domain reference
// "chacha_private.h" (the same source OpenBSD's arc4random builds on).
// 20 rounds; 64-bit counter at words [12,13]; 64-bit nonce at [14,15].
// Little-endian load/store on every word (ARM64 is LE, but we go through
// explicit byte ops so the result is endianness-defined per the spec).

#include <thylacine/chacha20.h>

#define ROTL32(v, c)  (((v) << (c)) | ((v) >> (32 - (c))))

#define QUARTERROUND(a, b, c, d)        \
    a += b; d ^= a; d = ROTL32(d, 16);  \
    c += d; b ^= c; b = ROTL32(b, 12);  \
    a += b; d ^= a; d = ROTL32(d, 8);   \
    c += d; b ^= c; b = ROTL32(b, 7);

// "expand 32-byte k" -- the ChaCha sigma constants for a 256-bit key.
static const u8 sigma[16] = {
    'e', 'x', 'p', 'a', 'n', 'd', ' ', '3',
    '2', '-', 'b', 'y', 't', 'e', ' ', 'k',
};

static u32 load_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void store_le32(u8 *p, u32 v) {
    p[0] = (u8)(v);
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

void chacha_keysetup(struct chacha_ctx *x, const u8 *key) {
    x->input[0] = load_le32(sigma + 0);
    x->input[1] = load_le32(sigma + 4);
    x->input[2] = load_le32(sigma + 8);
    x->input[3] = load_le32(sigma + 12);
    for (int i = 0; i < 8; i++) {
        x->input[4 + i] = load_le32(key + i * 4);
    }
}

void chacha_ivsetup(struct chacha_ctx *x, const u8 *iv) {
    x->input[12] = 0;                  // counter low
    x->input[13] = 0;                  // counter high
    x->input[14] = load_le32(iv + 0);  // nonce low
    x->input[15] = load_le32(iv + 4);  // nonce high
}

void chacha_keystream(struct chacha_ctx *x, u8 *out, u32 bytes) {
    if (bytes == 0) return;

    u32 j[16];
    for (int i = 0; i < 16; i++) j[i] = x->input[i];

    for (;;) {
        u32 s[16];
        for (int i = 0; i < 16; i++) s[i] = j[i];

        // 20 rounds = 10 double-rounds (column + diagonal).
        for (int i = 0; i < 10; i++) {
            QUARTERROUND(s[0], s[4], s[8],  s[12]);
            QUARTERROUND(s[1], s[5], s[9],  s[13]);
            QUARTERROUND(s[2], s[6], s[10], s[14]);
            QUARTERROUND(s[3], s[7], s[11], s[15]);
            QUARTERROUND(s[0], s[5], s[10], s[15]);
            QUARTERROUND(s[1], s[6], s[11], s[12]);
            QUARTERROUND(s[2], s[7], s[8],  s[13]);
            QUARTERROUND(s[3], s[4], s[9],  s[14]);
        }

        for (int i = 0; i < 16; i++) s[i] += j[i];

        // Advance the 64-bit block counter (words 12 low, 13 high).
        j[12]++;
        if (j[12] == 0) j[13]++;

        if (bytes < CHACHA_BLOCKSZ) {
            // Partial tail: serialize one block, copy the needed prefix.
            u8 block[CHACHA_BLOCKSZ];
            for (int i = 0; i < 16; i++) store_le32(block + i * 4, s[i]);
            for (u32 i = 0; i < bytes; i++) out[i] = block[i];
            // Persist the advanced counter for the next call.
            x->input[12] = j[12];
            x->input[13] = j[13];
            return;
        }

        for (int i = 0; i < 16; i++) store_le32(out + i * 4, s[i]);
        out   += CHACHA_BLOCKSZ;
        bytes -= CHACHA_BLOCKSZ;

        if (bytes == 0) {
            x->input[12] = j[12];
            x->input[13] = j[13];
            return;
        }
    }
}
