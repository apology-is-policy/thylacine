// /dev/full — kernel-internal "full disk" Dev (P6-pouch-devnodes).
//
// Per POUCH-DESIGN.md §6.6 + §14 sub-chunk 11. POSIX semantics (Linux
// compatible; the Linux man 4 full reference): writes always fail, reads
// fill the buffer with NUL bytes (just like /dev/zero). The intent is to
// give POSIX C programs a deterministic write-error path to exercise
// disk-full handling without provisioning a full filesystem.
//
// Single-file leaf Dev with dc='f', name="full" — third member of the
// trivial-Devs trio that pouch's minimal synthetic-FS namespace names.
//
// v1.0 errno caveat: devfull_write returns -1 (the generic dev-error
// convention; sys_write_for_proc collapses negative returns to flat -1).
// pouch's syscall_ret decodes that to errno=EIO, not errno=ENOSPC. A
// future chunk that widens sys_write_for_proc's error channel (passing
// dev-supplied -errno through unchanged) can flip this to ENOSPC without
// touching this file — kept here as a documented v1.0 limitation.

#include <thylacine/dev.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

static void devfull_reset(void)    { /* no-op */ }
static void devfull_init(void)     { /* no-op */ }
static void devfull_shutdown(void) { /* no-op */ }

static struct Spoor *devfull_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devfull, QTFILE);
}

static struct Walkqid *devfull_walk(struct Spoor *c, struct Spoor *nc,
                                    const char **name, int nname) {
    (void)c; (void)nc; (void)name; (void)nname;
    return NULL;            // leaf Dev — no walking
}

static int devfull_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devfull_open(struct Spoor *c, int omode) {
    return dev_simple_open(c, omode);
}

static void devfull_create(struct Spoor *c, const char *name, int omode, u32 perm) {
    (void)c; (void)name; (void)omode; (void)perm;
}

static void devfull_close(struct Spoor *c) {
    dev_simple_close(c);
}

// Read: NUL-fill the caller's buffer (same shape as devzero). The
// "full disk" abstraction is asymmetric — full on the write path, not
// the read path — so a reader observes a stream of zeros (Linux man 4
// full: "Reads from the /dev/full device will return \0 characters").
static long devfull_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c; (void)off;
    if (!buf) return -1;
    if (n < 0) return -1;
    if (n == 0) return 0;

    u8 *out = (u8 *)buf;
    for (long i = 0; i < n; i++) out[i] = 0;
    return n;
}

static struct Block *devfull_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

// Write: always fails. -1 surfaces as EIO to userspace via syscall_ret
// at v1.0 (see file-header caveat); the "full disk" semantic in pouch
// is still observable — the write returns < 0 and the program enters
// its disk-full branch.
static long devfull_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)n; (void)off;
    return -1;
}

static long devfull_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

static void devfull_remove(struct Spoor *c) {
    (void)c;
}

static int devfull_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devfull_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devfull = {
    .dc       = 'f',
    .name     = "full",

    .reset    = devfull_reset,
    .init     = devfull_init,
    .shutdown = devfull_shutdown,

    .attach   = devfull_attach,
    .walk     = devfull_walk,
    .stat     = devfull_stat,

    .open     = devfull_open,
    .create   = devfull_create,
    .close    = devfull_close,

    .read     = devfull_read,
    .bread    = devfull_bread,
    .write    = devfull_write,
    .bwrite   = devfull_bwrite,

    .remove   = devfull_remove,
    .wstat    = devfull_wstat,
    .power    = devfull_power,
};
