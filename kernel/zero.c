// /dev/zero — kernel-internal source of zero bytes (P4-B).
//
// Per ARCHITECTURE.md §9.4 + ROADMAP §6.1. POSIX semantics: writes are
// silently consumed; reads fill the buffer with zeroes and return n.
// Single-file leaf Dev with dc='z'.

#include <thylacine/dev.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

static void devzero_reset(void)    { /* no-op */ }
static void devzero_init(void)     { /* no-op */ }
static void devzero_shutdown(void) { /* no-op */ }

static struct Spoor *devzero_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devzero, QTFILE);
}

static struct Walkqid *devzero_walk(struct Spoor *c, struct Spoor *nc,
                                    const char **name, int nname) {
    (void)c; (void)nc; (void)name; (void)nname;
    return NULL;
}

static int devzero_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devzero_open(struct Spoor *c, int omode) {
    return dev_simple_open(c, omode);
}

static void devzero_create(struct Spoor *c, const char *name, int omode, u32 perm) {
    (void)c; (void)name; (void)omode; (void)perm;
}

static void devzero_close(struct Spoor *c) {
    dev_simple_close(c);
}

static long devzero_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c; (void)off;
    if (!buf) return -1;
    if (n < 0) return -1;
    if (n == 0) return 0;

    u8 *out = (u8 *)buf;
    for (long i = 0; i < n; i++) out[i] = 0;
    return n;
}

static struct Block *devzero_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

static long devzero_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)off;
    if (n < 0) return -1;
    return n;               // silently consume
}

static long devzero_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

static void devzero_remove(struct Spoor *c) {
    (void)c;
}

static int devzero_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devzero_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devzero = {
    .dc       = 'z',
    .name     = "zero",

    .reset    = devzero_reset,
    .init     = devzero_init,
    .shutdown = devzero_shutdown,

    .attach   = devzero_attach,
    .walk     = devzero_walk,
    .stat     = devzero_stat,

    .open     = devzero_open,
    .create   = devzero_create,
    .close    = devzero_close,

    .read     = devzero_read,
    .bread    = devzero_bread,
    .write    = devzero_write,
    .bwrite   = devzero_bwrite,

    .remove   = devzero_remove,
    .wstat    = devzero_wstat,
    .power    = devzero_power,
};
