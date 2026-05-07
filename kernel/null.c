// /dev/null — kernel-internal bit bucket Dev (P4-B).
//
// Per ARCHITECTURE.md §9.4 + ROADMAP §6.1. POSIX semantics: writes are
// silently consumed; reads return 0 (EOF). Single-file leaf Dev with
// dc='0'.

#include <thylacine/dev.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

static void devnull_reset(void)    { /* no-op */ }
static void devnull_init(void)     { /* no-op */ }
static void devnull_shutdown(void) { /* no-op */ }

static struct Spoor *devnull_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devnull, QTFILE);
}

static struct Walkqid *devnull_walk(struct Spoor *c, struct Spoor *nc,
                                    const char **name, int nname) {
    (void)c; (void)nc; (void)name; (void)nname;
    return NULL;            // leaf Dev — no walking
}

static int devnull_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;              // 9P stat encoding lands when the syscall surface needs it
}

static struct Spoor *devnull_open(struct Spoor *c, int omode) {
    return dev_simple_open(c, omode);
}

static void devnull_create(struct Spoor *c, const char *name, int omode, u32 perm) {
    (void)c; (void)name; (void)omode; (void)perm;
    // create on a leaf is silently ignored at v1.0
}

static void devnull_close(struct Spoor *c) {
    dev_simple_close(c);
}

static long devnull_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)n; (void)off;
    return 0;               // EOF on every read
}

static struct Block *devnull_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

static long devnull_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)off;
    if (n < 0) return -1;
    return n;               // silently consume
}

static long devnull_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

static void devnull_remove(struct Spoor *c) {
    (void)c;
}

static int devnull_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devnull_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devnull = {
    .dc       = '0',
    .name     = "null",

    .reset    = devnull_reset,
    .init     = devnull_init,
    .shutdown = devnull_shutdown,

    .attach   = devnull_attach,
    .walk     = devnull_walk,
    .stat     = devnull_stat,

    .open     = devnull_open,
    .create   = devnull_create,
    .close    = devnull_close,

    .read     = devnull_read,
    .bread    = devnull_bread,
    .write    = devnull_write,
    .bwrite   = devnull_bwrite,

    .remove   = devnull_remove,
    .wstat    = devnull_wstat,
    .power    = devnull_power,
};
