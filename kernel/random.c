// /dev/random — CSPRNG Dev backed by ARM64 RNDR (P4-B).
//
// Per ARCHITECTURE.md §9.4 + ROADMAP §6.1. ROADMAP scripture says
// "ARM RNDR + chacha20 stir"; v1.0 P4-B lands the RNDR-only baseline
// (sufficient for ROADMAP §6.2 "cat /dev/random produces non-zero
// bytes"). The chacha20 stir is held to a future hardening sub-chunk
// — adds defense-in-depth without changing the API contract.
//
// RNDR is FEAT_RNG (ARM v8.5+, mandatory v9.0+). ID_AA64ISAR0_EL1
// bits[63:60] >= 1 indicates support. Detected at devrandom_init();
// if absent, all reads return -1 and the boot banner notes it.
//
// Per FEAT_RNG: `mrs xN, RNDR` reads a 64-bit random value and sets
// PSTATE.NZCV.C to 1 on success, 0 if the entropy source can't deliver
// (caller should retry; usually succeeds within a few attempts).

#include <thylacine/dev.h>
#include <thylacine/random.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

#include "../arch/arm64/uart.h"

// Detected at init; boot banner reflects status.
static bool g_rndr_available;

// ID_AA64ISAR0_EL1.RNDR field is bits[63:60]. Value >= 1 means RNDR /
// RNDRRS implemented.
static bool rndr_supported(void) {
    u64 isar0;
    __asm__ __volatile__("mrs %0, ID_AA64ISAR0_EL1" : "=r" (isar0));
    return ((isar0 >> 60) & 0xF) >= 1;
}

// Read a 64-bit random word via RNDR. Returns true on success, false
// after RNDR_RETRY_MAX exhausted attempts (CPU's entropy source
// transiently unavailable). The retry budget per FEAT_RNG: most reads
// succeed on the first attempt; a small handful retry.
//
// Encoding: RNDR is sysreg op0=3 op1=3 CRn=2 CRm=4 op2=0 →
//   "mrs xN, S3_3_C2_C4_0" (avoids needing a clang version that
//   accepts the symbolic "RNDR" name; same encoding either way).
//
// Per ARM ARM (FEAT_RNG): RNDR / RNDRRS set NZCV based on the result:
//   - success: NZCV = 0b0000 (Z=0; result valid)
//   - failure: NZCV = 0b0100 (Z=1; retry)
// We use `cset Wd, ne` immediately after the RNDR mrs to capture the
// Z=0 case as `ok=1`. The "cc" clobber tells the compiler that the
// flags-register state has been overwritten so any preceding flag-
// dependent code is conservatively re-evaluated.
#define RNDR_RETRY_MAX 10

static bool rndr64(u64 *out) {
    if (!g_rndr_available) return false;

    for (int i = 0; i < RNDR_RETRY_MAX; i++) {
        u64 val;
        u32 ok;
        __asm__ __volatile__(
            "mrs %0, S3_3_C2_C4_0\n"   // RNDR — sets NZ from result
            "cset %w1, ne\n"             // ok = 1 if Z==0 (success)
            : "=r"(val), "=r"(ok)
            :
            : "cc", "memory"
        );
        if (ok) {
            *out = val;
            return true;
        }
    }
    return false;
}

static void devrandom_reset(void)    { /* no-op */ }

static void devrandom_init(void) {
    g_rndr_available = rndr_supported();
    uart_puts("  random: ");
    uart_puts(g_rndr_available ? "RNDR available (FEAT_RNG)" :
                                 "RNDR ABSENT — /dev/random reads fail");
    uart_puts("\n");
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

// Read up to n bytes of entropy. Each RNDR yields 8 bytes; partial
// tail handled. Returns:
//   n on full success
//   < n if RNDR retried out mid-stream (caller gets short read; can
//        retry to get more)
//   -1 if RNDR is unavailable AND no bytes produced
//   0  if n == 0 (legal no-op)
//
// P5-corvus-syscalls: public surface for SYS_GETRANDOM. The 9P
// devrandom_read below is now a thin wrapper. Per CORVUS-DESIGN
// §4.1.1 + C-15.
long kern_random_bytes(void *buf, long n) {
    if (!buf) return -1;
    if (n < 0) return -1;
    if (n == 0) return 0;

    if (!g_rndr_available) return -1;

    u8 *out = (u8 *)buf;
    long produced = 0;
    while (produced < n) {
        u64 chunk;
        if (!rndr64(&chunk)) {
            // partial fill; SYS_GETRANDOM treats partial as failure
            // (returns -1 to the user) since the syscall surface prefers
            // atomic results; devrandom_read passes the partial through.
            return produced;
        }
        long need = n - produced;
        long copy = need >= 8 ? 8 : need;
        for (long i = 0; i < copy; i++) {
            out[produced + i] = (u8)(chunk >> (i * 8));
        }
        produced += copy;
    }
    return produced;
}

bool kern_random_seeded(void) {
    return g_rndr_available;
}

static long devrandom_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c; (void)off;
    return kern_random_bytes(buf, n);
}

static struct Block *devrandom_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

// Writes are silently consumed at v1.0 P4-B. Future hardening will mix
// caller-supplied entropy into the chacha20 stir state; the API stays
// unchanged when that lands.
static long devrandom_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)off;
    if (n < 0) return -1;
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
