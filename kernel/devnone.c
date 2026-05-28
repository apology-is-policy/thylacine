// devnone — the no-op stub Dev (P4-A).
//
// Per ARCHITECTURE.md §9.2 + handoff 024. devnone is the sentinel Dev
// that anchors Spoors before they're attached to a real Dev, and serves
// as an audit guard: any Spoor in production code with dev == &devnone
// is a bug (you forgot to attach to a real Dev).
//
// All ops are safe no-ops or graceful failures. None extinct — devnone
// must be safe to invoke from any path that might encounter an
// unconfigured Spoor (e.g., test scaffolding, error paths, future
// driver-supervision recovery).
//
// The dc='-' character is unused in the Plan 9 device-character space
// (cons='c', null='⊘', proc='p', etc.); it's the conventional
// "this slot is the no-op stub" sigil.

#include <thylacine/dev.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

// All ops are unused-arg unless they need access to specific fields;
// suppress "unused parameter" warnings via void-cast in each body.

static void devnone_reset(void)    { /* no-op */ }
static void devnone_init(void)     { /* no-op */ }
static void devnone_shutdown(void) { /* no-op */ }

static struct Spoor *devnone_attach(const char *spec) {
    (void)spec;
    return NULL;            // no namespace to attach to
}

static struct Walkqid *devnone_walk(struct Spoor *c, struct Spoor *nc,
                                    const char **name, int nname) {
    (void)c; (void)nc; (void)name; (void)nname;
    return NULL;            // no path to walk
}

static int devnone_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;              // nothing to describe
}

static struct Spoor *devnone_open(struct Spoor *c, int omode) {
    (void)c; (void)omode;
    return NULL;            // can't open
}

static struct Spoor *devnone_create(struct Spoor *c, const char *name, int omode, u32 perm, u32 gid) {
    (void)c; (void)name; (void)omode; (void)perm; (void)gid;
    // create on devnone returns NULL — the SYS_WALK_CREATE handler maps
    // that to -1 (the error path Plan 9 would express as "create not supported").
    return NULL;
}

static void devnone_close(struct Spoor *c) {
    (void)c;
    // no-op. spoor_clunk calls this unconditionally; devnone has no
    // per-Spoor resources to release.
}

static long devnone_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)n; (void)off;
    return -1;
}

static struct Block *devnone_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

static long devnone_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)n; (void)off;
    return -1;
}

static long devnone_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

static void devnone_remove(struct Spoor *c) {
    (void)c;
    // no-op
}

static int devnone_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devnone_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devnone = {
    .dc       = '-',
    .name     = "none",

    .reset    = devnone_reset,
    .init     = devnone_init,
    .shutdown = devnone_shutdown,

    .attach   = devnone_attach,
    .walk     = devnone_walk,
    .stat     = devnone_stat,

    .open     = devnone_open,
    .create   = devnone_create,
    .close    = devnone_close,

    .read     = devnone_read,
    .bread    = devnone_bread,
    .write    = devnone_write,
    .bwrite   = devnone_bwrite,

    .remove   = devnone_remove,
    .wstat    = devnone_wstat,
    .power    = devnone_power,
};
