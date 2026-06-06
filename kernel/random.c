// /dev/random — kernel CSPRNG (Lazarus W3).
//
// A ChaCha20 forward-secure CSPRNG (the OpenBSD arc4random construction)
// replacing the RNDR-only v1.0 baseline. RNDR (FEAT_RNG) is no longer the
// sole entropy source: on RNDR-less targets (Apple cores under HVF, A72)
// the old path returned -1 and broke userspace crypto (corvus, libsodium).
// Now the SAME code path runs on every substrate -- the chacha state is
// keyed by a mixed seed (DTB boot entropy + CNTPCT jitter + RNDR-when-
// present), and W3b adds a kernel virtio-rng pull as the strong source.
//
// Construction (arc4random):
//   - A ChaCha20 context (key + 64-bit nonce + 64-bit counter).
//   - rng_rekey: generate a fresh RNG_BUFSZ keystream buffer; optionally
//     fold new entropy into its head; re-key from the first KEYSZ+IVSZ
//     bytes (fast key erasure -> the old key is unrecoverable, giving
//     backtracking resistance); zero those bytes (never handed out);
//     serve the remainder.
//   - rng_stir: collect fresh entropy + rekey. Run at boot, and every
//     RNG_RESEED_BYTES bytes generated (W3b: pulls virtio-rng).
//
// Per ARCHITECTURE.md §9.4 + ROADMAP §6.1 ("ARM RNDR + chacha20 stir") +
// PORTABILITY.md §6. Audit-bearing: CSPRNG quality + the seed sources +
// the reseed cadence (CLAUDE.md / ARCH §25.4 audit-trigger row).

#include <thylacine/chacha20.h>
#include <thylacine/dev.h>
#include <thylacine/dtb.h>
#include <thylacine/random.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

#include "../arch/arm64/uart.h"

// =============================================================================
// ChaCha20 forward-secure CSPRNG state.
// =============================================================================

#define RNG_KEYSZ        CHACHA_KEYSZ      // 32
#define RNG_IVSZ         CHACHA_IVSZ       // 8
#define RNG_SEEDSZ       (RNG_KEYSZ + RNG_IVSZ)   // 40 -- one rekey's worth
#define RNG_BUFSZ        (16u * CHACHA_BLOCKSZ)   // 1024 -- keystream buffer

// Strong-reseed cadence: pull fresh entropy after this many bytes are
// served. Matches arc4random's ~1 MiB stir interval -- defense-in-depth
// against state compromise; the per-rekey key erasure already provides
// backtracking resistance independent of this.
#define RNG_RESEED_BYTES (1u << 20)

static spin_lock_t   g_random_lock = SPIN_LOCK_INIT;
static struct chacha_ctx g_rng;            // BSS-zero until the first stir
static u8            g_rng_buf[RNG_BUFSZ];  // keystream pool; served from the tail
static size_t        g_rng_have;           // bytes still available in g_rng_buf
static u64           g_rng_count;          // bytes until the next strong stir
static bool          g_rng_seeded;         // a strong entropy source contributed
static bool          g_rndr_available;     // FEAT_RNG present (a stir source)

// =============================================================================
// Low-level entropy sources.
// =============================================================================

// Defeat dead-store elimination on secret scrubbing.
static void rng_secure_zero(void *p, size_t n) {
    volatile u8 *q = (volatile u8 *)p;
    while (n--) *q++ = 0;
}

// Physical counter -- a timing/jitter source. Read at EL1 on every
// substrate (TCG / HVF / bare metal); under HVF CNTPCT is the un-offset
// physical count, which is exactly what we want for entropy (absolute
// value irrelevant; the low bits carry sampling jitter). No ISB before
// the read -- we want the natural variance, not a serialized snapshot.
static u64 read_cntpct(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

// ID_AA64ISAR0_EL1.RNDR is bits[63:60]; >= 1 means RNDR/RNDRRS present.
static bool rndr_supported(void) {
    u64 isar0;
    __asm__ __volatile__("mrs %0, ID_AA64ISAR0_EL1" : "=r"(isar0));
    return ((isar0 >> 60) & 0xF) >= 1;
}

// One 64-bit RNDR word. Returns false (retry budget exhausted) when the
// CPU entropy source is transiently dry. RNDR sets PSTATE.NZCV: Z=0 on
// success. Used only as a stir source -- never the sole entropy.
#define RNDR_RETRY_MAX 10
static bool rndr64(u64 *out) {
    if (!g_rndr_available) return false;
    for (int i = 0; i < RNDR_RETRY_MAX; i++) {
        u64 val;
        u32 ok;
        __asm__ __volatile__(
            "mrs %0, S3_3_C2_C4_0\n"   // RNDR -- sets NZ from the result
            "cset %w1, ne\n"             // ok = 1 iff Z == 0 (success)
            : "=r"(val), "=r"(ok)
            :
            : "cc", "memory");
        if (ok) { *out = val; return true; }
    }
    return false;
}

static size_t put_u64(u8 *out, size_t off, size_t cap, u64 v) {
    size_t i = 0;
    while (i < sizeof(u64) && off + i < cap) {
        out[off + i] = (u8)(v >> (8 * i));
        i++;
    }
    return i;
}

// Collect cheap, non-blocking entropy into `out` (up to `cap` bytes).
// Sets *strong iff a high-quality independent source contributed (DTB
// boot seed present, or RNDR) -- this gates kern_random_seeded(). CNTPCT
// jitter fills any remainder as defense-in-depth, never as a strong
// source on its own. W3b extends the strong set with a virtio-rng pull.
static size_t rng_collect_cheap(u8 *out, size_t cap, bool *strong) {
    size_t off = 0;
    bool got_strong = false;

    // DTB boot entropy: /chosen/kaslr-seed (UEFI/bare-metal) +
    // /chosen/rng-seed (QEMU direct boot; host-provided). Either being
    // non-zero is a real, independent entropy source.
    u64 ks = dtb_get_chosen_kaslr_seed();
    u64 rs = dtb_get_chosen_rng_seed();
    if (ks) { got_strong = true; off += put_u64(out, off, cap, ks); }
    if (rs) { got_strong = true; off += put_u64(out, off, cap, rs); }

    // RNDR hardware CSPRNG (v8.5+): stirs when present.
    u64 r;
    if (rndr64(&r)) { got_strong = true; off += put_u64(out, off, cap, r); }

    // CNTPCT jitter fills the remainder. A short variable-latency spin
    // between samples (bounded by the sampled low bits) widens the
    // timing variance the low bits capture.
    while (off + sizeof(u64) <= cap) {
        u64 t = read_cntpct();
        off += put_u64(out, off, cap, t);
        for (volatile int i = 0; i < (int)(t & 0x1F); i++) {
            __asm__ __volatile__("" ::: "memory");
        }
    }
    // Byte-granular tail (cap not a multiple of 8).
    if (off < cap) {
        u64 t = read_cntpct();
        for (size_t i = 0; off < cap; i++) out[off++] = (u8)(t >> (8 * (i & 7)));
    }

    if (strong) *strong = got_strong;
    return off;
}

// =============================================================================
// CSPRNG core -- all under g_random_lock.
// =============================================================================

// Refill the keystream buffer + re-key (fast key erasure). Optionally
// fold `dat` (fresh entropy) into the rekey material first.
static void rng_rekey_locked(const u8 *dat, size_t datlen) {
    chacha_keystream(&g_rng, g_rng_buf, RNG_BUFSZ);

    if (dat) {
        size_t m = datlen < RNG_SEEDSZ ? datlen : RNG_SEEDSZ;
        for (size_t i = 0; i < m; i++) g_rng_buf[i] ^= dat[i];
    }

    chacha_keysetup(&g_rng, g_rng_buf);
    chacha_ivsetup(&g_rng, g_rng_buf + RNG_KEYSZ);

    // The first KEYSZ+IVSZ bytes became the new key/nonce -- they are
    // never served. Erasing them is the forward-secrecy step.
    rng_secure_zero(g_rng_buf, RNG_SEEDSZ);
    g_rng_have = RNG_BUFSZ - RNG_SEEDSZ;
}

// Collect fresh entropy + rekey + reset the reseed countdown. Sets
// g_rng_seeded once a strong source has ever contributed (monotonic).
static void rng_stir_locked(void) {
    u8 seed[RNG_SEEDSZ];
    bool strong = false;
    size_t n = rng_collect_cheap(seed, sizeof(seed), &strong);
    rng_rekey_locked(seed, n);
    rng_secure_zero(seed, sizeof(seed));
    if (strong) __atomic_store_n(&g_rng_seeded, true, __ATOMIC_RELEASE);
    g_rng_count = RNG_RESEED_BYTES;
}

// Serve `n` bytes from the keystream buffer, rekeying to refill on drain.
// Each served byte is zeroed in place so it is never re-handed-out.
static void rng_buf_consume_locked(u8 *out, size_t n) {
    while (n > 0) {
        if (g_rng_have == 0) rng_rekey_locked(NULL, 0);
        size_t take = n < g_rng_have ? n : g_rng_have;
        u8 *src = g_rng_buf + (RNG_BUFSZ - g_rng_have);
        for (size_t i = 0; i < take; i++) { out[i] = src[i]; src[i] = 0; }
        out += take;
        n   -= take;
        g_rng_have -= take;
    }
}

// =============================================================================
// Public API (random.h) -- contracts unchanged from the RNDR-only baseline.
// =============================================================================

long kern_random_bytes(void *buf, long n) {
    if (!buf)  return -1;
    if (n < 0) return -1;
    if (n == 0) return 0;

    // Fail closed when no strong source has ever seeded the pool. Matches
    // the old "RNDR absent -> -1" contract; SYS_GETRANDOM gates on this.
    if (!__atomic_load_n(&g_rng_seeded, __ATOMIC_ACQUIRE)) return -1;

    spin_lock(&g_random_lock);
    if (g_rng_count <= (u64)n) {
        rng_stir_locked();
    }
    g_rng_count -= (g_rng_count > (u64)n) ? (u64)n : g_rng_count;
    rng_buf_consume_locked((u8 *)buf, (size_t)n);
    spin_unlock(&g_random_lock);
    return n;
}

bool kern_random_seeded(void) {
    return __atomic_load_n(&g_rng_seeded, __ATOMIC_ACQUIRE);
}

// =============================================================================
// Dev bring-up + the Plan 9 /dev/random surface (thin wrapper over the API).
// =============================================================================

static void devrandom_reset(void) { /* no-op */ }

static void devrandom_init(void) {
    g_rndr_available = rndr_supported();

    spin_lock(&g_random_lock);
    rng_stir_locked();
    bool seeded = __atomic_load_n(&g_rng_seeded, __ATOMIC_ACQUIRE);
    spin_unlock(&g_random_lock);

    uart_puts("  random: chacha20 CSPRNG ");
    uart_puts(seeded ? "seeded (dtb-seed + cntpct" : "UNSEEDED (no strong source");
    if (seeded && g_rndr_available) uart_puts(" + rndr");
    uart_puts(seeded ? "); virtio-rng reseed pending\n"
                     : "); /dev/random reads fail until reseeded\n");
}

static void devrandom_shutdown(void) { /* no-op */ }

static struct Spoor *devrandom_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devrandom, QTFILE);
}

static struct Walkqid *devrandom_walk(struct Spoor *c, struct Spoor *nc,
                                      const char **name, int nname) {
    (void)c; (void)nc; (void)name; (void)nname;
    return NULL;
}

static int devrandom_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devrandom_open(struct Spoor *c, int omode) {
    return dev_simple_open(c, omode);
}

static struct Spoor *devrandom_create(struct Spoor *c, const char *name, int omode, u32 perm, u32 gid) {
    (void)c; (void)name; (void)omode; (void)perm; (void)gid;
    return NULL;
}

static void devrandom_close(struct Spoor *c) {
    dev_simple_close(c);
}

static long devrandom_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c; (void)off;
    return kern_random_bytes(buf, n);
}

static struct Block *devrandom_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

// Writes mix caller-supplied entropy into the CSPRNG (Plan 9 /dev/random
// write semantics: add to the pool). Never reduces entropy; folds the
// whole buffer in rekey-sized chunks. `buf` is a kernel-side scratch
// (sys_write_handler bounces the user VA via uaccess_load_u8).
static long devrandom_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)off;
    if (n < 0) return -1;
    if (n > 0 && buf) {
        const u8 *p = (const u8 *)buf;
        size_t left = (size_t)n;
        spin_lock(&g_random_lock);
        while (left > 0) {
            size_t chunk = left < RNG_SEEDSZ ? left : RNG_SEEDSZ;
            rng_rekey_locked(p, chunk);
            p += chunk;
            left -= chunk;
        }
        spin_unlock(&g_random_lock);
    }
    return n;
}

static long devrandom_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

static void devrandom_remove(struct Spoor *c) {
    (void)c;
}

static int devrandom_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devrandom_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devrandom = {
    .dc       = 'r',
    .name     = "random",

    .reset    = devrandom_reset,
    .init     = devrandom_init,
    .shutdown = devrandom_shutdown,

    .attach   = devrandom_attach,
    .walk     = devrandom_walk,
    .stat     = devrandom_stat,

    .open     = devrandom_open,
    .create   = devrandom_create,
    .close    = devrandom_close,

    .read     = devrandom_read,
    .bread    = devrandom_bread,
    .write    = devrandom_write,
    .bwrite   = devrandom_bwrite,

    .remove   = devrandom_remove,
    .wstat    = devrandom_wstat,
    .power    = devrandom_power,
};
