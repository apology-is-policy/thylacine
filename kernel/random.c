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
#include <thylacine/page.h>
#include <thylacine/random.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>
#include <thylacine/virtio.h>

#include "../arch/arm64/uart.h"
#include "../mm/phys.h"

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

// Bounded poll budget for a virtio-rng completion (no IRQ path on the
// kernel virtio substrate). The device fills near-instantly under QEMU/
// HVF; the cap bounds a wedged device -- on timeout the CSPRNG keeps its
// prior state (never blocks, never serves stale-but-still-secure bytes).
#define RNG_VIRTIO_POLL_MAX (1u << 22)

static spin_lock_t   g_random_lock = SPIN_LOCK_INIT;
static struct chacha_ctx g_rng;            // BSS-zero until the first stir
static u8            g_rng_buf[RNG_BUFSZ];  // keystream pool; served from the tail
static size_t        g_rng_have;           // bytes still available in g_rng_buf
static u64           g_rng_count;          // bytes until the next strong stir
static bool          g_rng_seeded;         // a strong entropy source contributed
static bool          g_rndr_available;     // FEAT_RNG present (atomic: set at init, read cross-CPU)

// virtio-rng device access is serialized independently of the chacha
// state: the device bring-up/pull/teardown runs OUTSIDE g_random_lock
// (it allocs pages + spins on the used ring), so two concurrent reseeds
// can't both drive queue 0. Lock order: g_rng_dev_lock is never held
// while taking g_random_lock (the pull completes, then the absorb takes
// g_random_lock separately).
static spin_lock_t   g_rng_dev_lock = SPIN_LOCK_INIT;
static bool          g_rng_virtio_ok;      // a virtio-rng pull has ever succeeded

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
    if (!__atomic_load_n(&g_rndr_available, __ATOMIC_ACQUIRE)) return false;
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

// splitmix64 finalizer -- a public avalanche, used to DOMAIN-SEPARATE the
// DTB seeds from KASLR. KASLR derives the (publicly-disclosed) kernel
// offset from the SAME /chosen seeds via its own mix64 (MurmurHash3
// constants), so feeding the raw seed into the CSPRNG key would make the
// key correlate with the exposed offset. Distinct constants + a per-use
// domain tag (XORed in before mixing) decorrelate the two derivations.
static u64 rng_mix64(u64 x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}
#define RNG_DOMAIN_KASLR_SEED  0x9e3779b97f4a7c15ULL
#define RNG_DOMAIN_RNG_SEED    0xc2b2ae3d27d4eb4fULL

// Collect cheap, non-blocking entropy into `out` (up to `cap` bytes).
//
// Sets *has_unobserved iff an UNOBSERVED strong source contributed --
// RNDR here; the caller treats a virtio-rng pull the same way. Only an
// unobserved source flips kern_random_seeded(): the DTB seeds are
// host-provided real entropy but are also consumed (and partially
// disclosed) by KASLR, and CNTPCT is weak -- so both are mixed as
// material that strengthens the key but does NOT gate readiness on its
// own. This closes the pre-virtio-reseed window in which a KASLR-
// correlated key could otherwise be served on an RNDR-less target.
static size_t rng_collect_cheap(u8 *out, size_t cap, bool *has_unobserved) {
    size_t off = 0;
    bool unobserved = false;

    // DTB boot entropy, domain-separated from KASLR (see rng_mix64).
    u64 ks = dtb_get_chosen_kaslr_seed();
    u64 rs = dtb_get_chosen_rng_seed();
    if (ks) off += put_u64(out, off, cap, rng_mix64(ks ^ RNG_DOMAIN_KASLR_SEED));
    if (rs) off += put_u64(out, off, cap, rng_mix64(rs ^ RNG_DOMAIN_RNG_SEED));

    // RNDR hardware CSPRNG (v8.5+): an unobserved strong source; gates
    // readiness when present.
    u64 r;
    if (rndr64(&r)) { unobserved = true; off += put_u64(out, off, cap, r); }

    // CNTPCT samples fill the remainder as supplementary material (never a
    // readiness-gating source on its own). Distinct samples carry some
    // sampling jitter on real hardware; no data-dependent spin -- that
    // added latency without adding cryptographic entropy on an emulator.
    while (off + sizeof(u64) <= cap) {
        off += put_u64(out, off, cap, read_cntpct());
    }
    // Byte-granular tail (cap not a multiple of 8).
    if (off < cap) {
        u64 t = read_cntpct();
        for (size_t i = 0; off < cap; i++) out[off++] = (u8)(t >> (8 * (i & 7)));
    }

    if (has_unobserved) *has_unobserved = unobserved;
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

// Collect cheap entropy + rekey + reset the reseed countdown. Flips
// g_rng_seeded only if an unobserved strong source (RNDR) contributed --
// the DTB/CNTPCT material strengthens the key but does not gate readiness
// (the virtio-rng reseed is the other path that flips it). Monotonic.
static void rng_stir_locked(void) {
    u8 seed[RNG_SEEDSZ];
    bool unobserved = false;
    size_t n = rng_collect_cheap(seed, sizeof(seed), &unobserved);
    rng_rekey_locked(seed, n);
    rng_secure_zero(seed, sizeof(seed));
    if (unobserved) __atomic_store_n(&g_rng_seeded, true, __ATOMIC_RELEASE);
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
// virtio-rng -- the strong/real entropy source.
// =============================================================================

// Bring up the kernel virtio-rng device, pull up to `want` bytes of host
// entropy into `out`, tear the device back down to dormant. Returns the
// number of bytes obtained (0 if no RNG device / negotiation /
// allocation / poll-timeout). Self-contained: serialized by
// g_rng_dev_lock; resets the device on every exit path so it composes
// with the test_virtio queue-0 exercises and with repeated reseeds.
//
// Process-context only: g_rng_dev_lock is a plain (non-IRQ-masking) lock
// held across alloc_pages/free_pages (which take the IRQ-masked buddy
// zone->lock), giving the order g_rng_dev_lock -> zone->lock. That is
// acyclic only while no IRQ handler can take g_rng_dev_lock -- so this
// must never be called from IRQ/deferred context. The two current
// callers (boot main.c + the SYS_GETRANDOM threshold path) are both
// process context. A future IRQ-context consumer must revisit this
// (switch to spin_lock_irqsave, but then the bounded poll cannot be held
// IRQ-masked -- it would need to drop the lock differently).
//
// I-5 note: the kernel now drives this one virtio-mmio slot (transiently
// -- reset-to-dormant between pulls). The slot shares a 4 KiB page with
// up to 7 sibling slots that userspace legitimately drives, so a
// page-granular kernel reservation is infeasible; the RNG slot inherits
// the v1.0 virtio-mmio trust posture (kproc-only CAP_HW_CREATE; the
// kobj_mmio overlap check; no userspace driver targets device-id RNG).
// See kernel/mmio_handle.c for the full rationale + the Phase-5+ seam.
static size_t random_virtio_pull(u8 *out, size_t want) {
    if (!out || want == 0) return 0;
    if (want > PAGE_SIZE) want = PAGE_SIZE;

    spin_lock(&g_rng_dev_lock);

    struct virtio_mmio_dev *dev = virtio_mmio_find_by_device_id(VIRTIO_DEVICE_ID_RNG);
    if (!dev) { spin_unlock(&g_rng_dev_lock); return 0; }

    // VIRTIO 1.2 §3.1.1 steps 1-6 (reset -> ACK -> DRIVER -> features ->
    // FEATURES_OK). virtio-rng negotiates no required features (mask 0).
    if (!virtio_negotiate_features(dev, 0)) {
        virtio_reset(dev);
        spin_unlock(&g_rng_dev_lock);
        return 0;
    }

    struct virtio_virtqueue *vq = virtio_virtqueue_create(dev, 0);
    if (!vq) {
        virtio_reset(dev);
        spin_unlock(&g_rng_dev_lock);
        return 0;
    }

    // §3.1.1 step 8: DRIVER_OK once the queue is armed -- the device
    // won't process buffers before this.
    virtio_add_status(dev, VIRTIO_STATUS_DRIVER_OK);

    struct page *pg = alloc_pages(0, KP_ZERO);
    if (!pg) {
        virtio_virtqueue_destroy(vq);
        virtio_reset(dev);
        spin_unlock(&g_rng_dev_lock);
        return 0;
    }
    paddr_t pa  = page_to_pa(pg);
    u8     *kva = (u8 *)pa_to_kva(pa);

    // One device-writable descriptor on the requestq (queue 0).
    vq->desc[0].addr  = (u64)pa;
    vq->desc[0].len   = (u32)want;
    vq->desc[0].flags = VRING_DESC_F_WRITE;
    vq->desc[0].next  = 0;

    // Publish into the available ring. avail->idx is the running entry
    // count; ring[idx % size] carries the head descriptor index.
    u16 head = vq->avail->idx;
    vq->avail->ring[head % vq->size] = 0;
    __asm__ __volatile__("dsb sy" ::: "memory");
    vq->avail->idx = (u16)(head + 1);
    __asm__ __volatile__("dsb sy" ::: "memory");

    virtio_vq_notify(vq);

    size_t got = 0;
    for (u64 spin = 0; spin < RNG_VIRTIO_POLL_MAX; spin++) {
        if (vq->used->idx != 0) {
            // Order the used-ring observation before the buffer read.
            __asm__ __volatile__("dsb sy" ::: "memory");
            u32 wrote = vq->used->ring[0].len;
            if (wrote > want) wrote = (u32)want;
            for (u32 i = 0; i < wrote; i++) out[i] = kva[i];
            got = wrote;
            break;
        }
        __asm__ __volatile__("yield" ::: "memory");
    }

    // Reset the device (full halt) BEFORE freeing any ring or buffer page.
    // virtio_virtqueue_destroy frees the ring pages, so reset must precede
    // it -- otherwise a slow device could post a late used-ring write into
    // freed memory between QUEUE_READY=0 and the reset.
    virtio_reset(dev);
    virtio_virtqueue_destroy(vq);
    rng_secure_zero(kva, want);
    free_pages(pg, 0);

    spin_unlock(&g_rng_dev_lock);

    // An all-zero pull is a coherency-miss / dead-device signal, not
    // entropy -- reject it so a non-coherent transport fails SAFE (the
    // CSPRNG keeps its prior seed) rather than silently mixing zero. This
    // is a guard, not a live path: virtio-rng is a QEMU-only, dma-coherent
    // device (the `dsb` ordering above suffices there); bare-metal entropy
    // is the BCM2711 HW RNG register read (no DMA) -- W4.
    if (got) {
        u8 acc = 0;
        for (size_t i = 0; i < got; i++) acc |= out[i];
        if (acc == 0) got = 0;
    }

    if (got) __atomic_store_n(&g_rng_virtio_ok, true, __ATOMIC_RELEASE);
    return got;
}

// Strong reseed: pull virtio-rng entropy (the real source) + a fresh
// cheap collection, then re-key. The device pull runs OUTSIDE
// g_random_lock; the absorb takes it. A successful virtio pull marks the
// pool seeded even on a target with no DTB seed and no RNDR. Returns the
// virtio byte count (0 if the device was unavailable).
static size_t random_reseed_strong(void) {
    u8 vbuf[RNG_SEEDSZ];
    size_t vn = random_virtio_pull(vbuf, sizeof(vbuf));

    u8 cbuf[RNG_SEEDSZ];
    bool cheap_unobserved = false;
    size_t cn = rng_collect_cheap(cbuf, sizeof(cbuf), &cheap_unobserved);

    spin_lock(&g_random_lock);
    if (vn) rng_rekey_locked(vbuf, vn);
    rng_rekey_locked(cbuf, cn);
    if (vn || cheap_unobserved) __atomic_store_n(&g_rng_seeded, true, __ATOMIC_RELEASE);
    g_rng_count = RNG_RESEED_BYTES;
    spin_unlock(&g_random_lock);

    rng_secure_zero(vbuf, sizeof(vbuf));
    rng_secure_zero(cbuf, sizeof(cbuf));
    return vn;
}

size_t random_seed_from_virtio(void) {
    return random_reseed_strong();
}

bool kern_random_virtio_contributed(void) {
    return __atomic_load_n(&g_rng_virtio_ok, __ATOMIC_ACQUIRE);
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

    // Strong-reseed cadence: once ~RNG_RESEED_BYTES have been served, pull
    // fresh virtio-rng entropy. The device I/O runs OUTSIDE g_random_lock
    // (it allocs + spins); decide under the lock, reseed without it.
    bool want_reseed;
    spin_lock(&g_random_lock);
    want_reseed = (g_rng_count <= (u64)n);
    spin_unlock(&g_random_lock);
    if (want_reseed) random_reseed_strong();

    spin_lock(&g_random_lock);
    // If the strong reseed was skipped or the device was unavailable and
    // we are still over the threshold, fall back to a cheap stir so the
    // pool never serves unboundedly without re-keying from fresh entropy.
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
    __atomic_store_n(&g_rndr_available, rndr_supported(), __ATOMIC_RELEASE);

    spin_lock(&g_random_lock);
    rng_stir_locked();
    bool seeded = __atomic_load_n(&g_rng_seeded, __ATOMIC_ACQUIRE);
    spin_unlock(&g_random_lock);

    // Only an unobserved strong source flips "seeded" -- RNDR here; the
    // virtio-rng reseed in main.c is the other path. The DTB seed + cntpct
    // prime the pool as material but do not gate readiness (the DTB seed is
    // also KASLR's, and partially disclosed). Reads fail closed until a
    // strong source contributes.
    uart_puts("  random: chacha20 CSPRNG ");
    if (seeded) uart_puts("seeded via RNDR (virtio-rng reseed to follow)\n");
    else        uart_puts("primed from DTB+cntpct; awaiting virtio-rng reseed\n");
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
