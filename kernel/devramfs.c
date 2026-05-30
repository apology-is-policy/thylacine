// /ramfs — cpio-loaded in-memory filesystem (P4-E).
//
// Per ARCHITECTURE.md §9.4 + ROADMAP §6.1. Plan 9 idiom: a synthetic
// Dev whose namespace mirrors the contents of a cpio newc archive
// loaded by the bootloader.
//
// Boot flow:
//   1. QEMU loads the cpio archive at a physical address; advertises
//      the range via /chosen/linux,initrd-start + linux,initrd-end.
//   2. dtb_init parses the DTB (early in boot_main).
//   3. dev_init calls devramfs_init via the bestiary walk; we read
//      the initrd PA range, convert to direct-map KVA, parse the
//      cpio newc entries, and populate g_ramfs_files[].
//   4. Subsequent walks/reads dispatch by file index.
//
// At v1.0 P4-E:
//   - Flat file layout (no directories inside the archive).
//   - Fixed file table cap at 32 entries.
//   - Read-only — writes return -1.
//   - The cpio blob is NEVER freed at v1.0; Phase 5+ frees the initrd
//     once Stratum mounts the persistent FS (per ROADMAP §6.1
//     "freed once persistent FS is mounted").
//
// dc='m' (memfs / memory-fs); leaves 'r' for an alternative ramfs in a
// later sub-chunk if the layout grows.

#include <thylacine/cpio.h>
#include <thylacine/dev.h>
#include <thylacine/devramfs.h>
#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>

#include "../arch/arm64/uart.h"

// =============================================================================
// File table.
// =============================================================================

// Sized to hold the ramfs binaries Phase 6 ships (33+ entries by sub-chunk
// 14) with headroom for sub-chunks 15-16 (stratumd) and Phase 7 (Utopia
// userspace). 24 bytes per slot; the static array sits in BSS.
#define RAMFS_FILE_MAX 64

struct ramfs_file {
    const char *name;        // NUL-terminated; lives in the cpio blob
    const u8   *data;        // file content; lives in the cpio blob
    size_t      size;
    u32         mode;
};

static struct ramfs_file g_ramfs_files[RAMFS_FILE_MAX];
static int               g_ramfs_count;
static bool              g_ramfs_initialized;

// =============================================================================
// Qid encoding.
// =============================================================================
//
// path = 0          => root /ramfs (QTDIR)
// path = 1..N       => file at index (path - 1) (QTFILE)

#define RAMFS_QID_ROOT_PATH  0ULL

static inline u64 ramfs_qid_for_index(int idx) {
    return (u64)(idx + 1);
}

static inline int ramfs_index_for_qid(u64 path) {
    if (path == 0) return -1;
    return (int)(path - 1);
}

// =============================================================================
// Init: parse the cpio + populate the file table.
// =============================================================================

struct ramfs_init_state {
    bool overflow;
};

static int ramfs_init_cb(const struct cpio_entry *e, void *arg) {
    struct ramfs_init_state *s = (struct ramfs_init_state *)arg;
    if (g_ramfs_count >= RAMFS_FILE_MAX) {
        s->overflow = true;
        return 1;        // stop iteration
    }
    g_ramfs_files[g_ramfs_count].name = e->name;
    g_ramfs_files[g_ramfs_count].data = e->data;
    g_ramfs_files[g_ramfs_count].size = e->size;
    g_ramfs_files[g_ramfs_count].mode = e->mode;
    g_ramfs_count++;
    return 0;
}

static void devramfs_init_hook(void) {
    if (g_ramfs_initialized) return;       // idempotent
    g_ramfs_initialized = true;

    u64 start_pa, end_pa;
    if (!dtb_get_chosen_initrd(&start_pa, &end_pa)) {
        uart_puts("  ramfs: no initrd in DTB (/chosen/linux,initrd-* absent)\n");
        return;
    }

    size_t blob_size = (size_t)(end_pa - start_pa);
    const u8 *blob = (const u8 *)pa_to_kva((paddr_t)start_pa);

    if (!cpio_newc_is_valid(blob, blob_size)) {
        uart_puts("  ramfs: initrd present but not cpio newc (magic mismatch)\n");
        return;
    }

    struct ramfs_init_state st = { false };
    int rv = cpio_newc_iter(blob, blob_size, ramfs_init_cb, &st);
    if (rv < 0) {
        uart_puts("  ramfs: cpio parse error\n");
        g_ramfs_count = 0;
        return;
    }

    uart_puts("  ramfs: ");
    uart_putdec((u64)g_ramfs_count);
    uart_puts(" files loaded from initrd (");
    uart_putdec((u64)blob_size);
    uart_puts(" bytes");
    if (st.overflow) {
        uart_puts(", TRUNCATED — exceeded RAMFS_FILE_MAX");
    }
    uart_puts(")\n");
}

// =============================================================================
// File lookup.
// =============================================================================

static int ramfs_find_by_name(const char *name) {
    for (int i = 0; i < g_ramfs_count; i++) {
        const char *f = g_ramfs_files[i].name;
        int j = 0;
        while (f[j] && name[j] && f[j] == name[j]) j++;
        if (f[j] == '\0' && name[j] == '\0') return i;
    }
    return -1;
}

// Public API. Returns 0 on success and populates *out_data / *out_size
// with pointers into the initrd blob (kernel-owned for boot lifetime;
// caller must not modify or free). Returns -1 if devramfs hasn't been
// initialized, the file isn't in the table, or out_data / out_size /
// name is NULL.
int devramfs_lookup(const char *name, const void **out_data, size_t *out_size) {
    if (!name || !out_data || !out_size)        return -1;
    if (!g_ramfs_initialized)                   return -1;
    int idx = ramfs_find_by_name(name);
    if (idx < 0)                                return -1;
    *out_data = (const void *)g_ramfs_files[idx].data;
    *out_size = g_ramfs_files[idx].size;
    return 0;
}

// =============================================================================
// Walk.
// =============================================================================

static bool walk_one(u64 cur_path, const char *name, struct Qid *out_qid) {
    out_qid->path = 0;
    out_qid->vers = 0;
    out_qid->type = 0;
    out_qid->pad[0] = out_qid->pad[1] = out_qid->pad[2] = 0;

    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        out_qid->path = RAMFS_QID_ROOT_PATH;
        out_qid->type = QTDIR;
        return true;
    }

    if (cur_path == RAMFS_QID_ROOT_PATH) {
        int idx = ramfs_find_by_name(name);
        if (idx < 0) return false;
        out_qid->path = ramfs_qid_for_index(idx);
        out_qid->type = QTFILE;
        return true;
    }
    return false;
}

// =============================================================================
// Vtable.
// =============================================================================

static void devramfs_reset(void)    { /* no-op */ }
static void devramfs_shutdown(void) { /* no-op */ }

// dev_init walks bestiary calling each ->init(); use that hook to
// parse the cpio. Intentional ordering: the bestiary walk is
// deterministic so devramfs_init_hook runs after spoor_init + dev
// registrations.
static void devramfs_init(void) {
    devramfs_init_hook();
}

static struct Spoor *devramfs_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devramfs, QTDIR);
}

static struct Walkqid *devramfs_walk(struct Spoor *c, struct Spoor *nc,
                                      const char **name, int nname) {
    if (!c) return NULL;
    if (nname < 0) return NULL;

    struct Walkqid *wq = walkqid_alloc(nname);
    if (!wq) return NULL;

    // SYS_WALK_OPEN handler contract (kernel/syscall.c::sys_walk_open_handler):
    // the caller pre-allocates nc (via spoor_clone(c)) and expects walk()
    // to RETURN nc (the wq->spoor == nc check at line ~1275). When nc is
    // non-NULL we mutate it in place. When nc is NULL (legacy callers:
    // kernel-internal tests in kernel/test/test_devramfs.c), we clone c
    // ourselves to preserve the original devramfs_walk shape.
    //
    // P6-pouch-stratumd-boot 16b-gamma: this dual mode is what lets the
    // userspace SYS_WALK_OPEN(FROM_ROOT, ...) path walk devramfs at all.
    // Pre-16b-gamma the self-cloning shape made sys_walk_open_handler
    // reject every devramfs FROM_ROOT walk (wq->spoor != nc).
    struct Spoor *cur;
    if (nc) {
        cur = nc;
        cur->qid = c->qid;
    } else {
        cur = spoor_clone(c);
        if (!cur) {
            walkqid_free(wq);
            return NULL;
        }
    }

    int n = 0;
    for (int i = 0; i < nname; i++) {
        struct Qid next;
        if (!walk_one(cur->qid.path, name[i], &next)) break;
        cur->qid = next;
        wq->qid[n++] = next;
    }

    wq->spoor = cur;
    wq->nqid  = n;
    return wq;
}

static int devramfs_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

// devramfs_stat_native — populate struct t_stat for the file backed by `c`.
// P6-pouch-stratumd-boot sub-chunk 16b-gamma. The Thylacine-native SYS_FSTAT
// surface: caller (syscall handler) provides a kernel-scratch t_stat; we
// fill it from the in-kernel ramfs table; the syscall handler then copies
// it out to user-VA byte-by-byte via uaccess_store_u8.
//
// Returns 0 on success, -1 if `c` does not name a real file (root dir or
// out-of-range qid).
static int devramfs_stat_native(struct Spoor *c, struct t_stat *out) {
    if (!c || !out) return -1;

    // Zero everything first so any field we don't set is a defined zero.
    // The caller can rely on every t_stat reaching it byte-for-byte equal
    // to what the kernel wrote, with no stack-garbage leakage.
    for (size_t i = 0; i < sizeof(*out); i++) ((u8 *)out)[i] = 0;

    if (c->qid.path == RAMFS_QID_ROOT_PATH) {
        // The synthetic root directory. Reasonable for "what is fd N?"
        // when N is the root spoor; size 0 + S_IFDIR + qid_type=QTDIR.
        out->mode      = T_S_IFDIR | 0555u;
        out->nlink     = 1;
        out->qid_path  = RAMFS_QID_ROOT_PATH;
        out->qid_vers  = 0;
        out->qid_type  = QTDIR;
        out->blksize   = 4096;
        // The boot FS is system-owned; world-readable (A-2a §9.5). No per-file
        // owner table in the read-only cpio ramfs -- every entry is SYSTEM.
        out->uid       = PRINCIPAL_SYSTEM;
        out->gid       = GID_SYSTEM;
        return 0;
    }

    int idx = ramfs_index_for_qid(c->qid.path);
    if (idx < 0 || idx >= g_ramfs_count) return -1;

    const struct ramfs_file *f = &g_ramfs_files[idx];
    out->size      = (u64)f->size;
    // The cpio mode field is a 16-bit POSIX file mode (e.g., 0100644 =
    // S_IFREG | 0644). Pass through directly; the userspace consumer
    // (pouch fstat) translates to musl's bits if it cares about the
    // type bits at this granularity. Force T_S_IFREG if the cpio mode
    // looks malformed (no type bits set) — defensive against a cpio
    // emitter that forgot the type field; v1.0 mkcpio.py always emits
    // 0100644 so the fallback is structurally cold.
    out->mode      = f->mode ? f->mode : (T_S_IFREG | 0644u);
    out->nlink     = 1;
    out->qid_path  = c->qid.path;
    out->qid_vers  = 0;
    out->qid_type  = QTFILE;
    out->blksize   = 4096;
    // POSIX blocks: count of 512-byte units. ceil(size / 512).
    out->blocks    = (u64)((f->size + 511u) / 512u);
    out->uid       = PRINCIPAL_SYSTEM;
    out->gid       = GID_SYSTEM;
    return 0;
}

static struct Spoor *devramfs_open(struct Spoor *c, int omode) {
    return dev_simple_open(c, omode);
}

static struct Spoor *devramfs_create(struct Spoor *c, const char *name,
                                       int omode, u32 perm, u32 gid) {
    (void)c; (void)name; (void)omode; (void)perm; (void)gid;
    // ramfs is read-only at v1.0 — SYS_WALK_CREATE on a ramfs dir returns -1.
    return NULL;
}

static void devramfs_close(struct Spoor *c) {
    dev_simple_close(c);
}

static long devramfs_read(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf) return -1;
    if (n < 0) return -1;
    if (n == 0) return 0;
    if (off < 0) return -1;

    // Reads on root return -1 (readdir held — same reasoning as
    // devproc / devctl).
    if (c->qid.path == RAMFS_QID_ROOT_PATH) return -1;

    int idx = ramfs_index_for_qid(c->qid.path);
    if (idx < 0 || idx >= g_ramfs_count) return -1;

    const struct ramfs_file *f = &g_ramfs_files[idx];
    if ((size_t)off >= f->size) return 0;        // EOF

    size_t avail = f->size - (size_t)off;
    size_t copy = avail > (size_t)n ? (size_t)n : avail;

    u8 *out = (u8 *)buf;
    for (size_t i = 0; i < copy; i++) out[i] = f->data[(size_t)off + i];
    return (long)copy;
}

static struct Block *devramfs_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

static long devramfs_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)n; (void)off;
    return -1;        // read-only
}

static long devramfs_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

// ramfs is in-memory (durable-to-itself); fsync is a no-op success so generic
// write-then-fsync code works against a ramfs file. (FS-mutation foundation.)
static int devramfs_fsync(struct Spoor *c, u32 datasync) {
    (void)c; (void)datasync;
    return 0;
}

static void devramfs_remove(struct Spoor *c) {
    (void)c;
}

static int devramfs_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devramfs_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devramfs = {
    .dc       = 'm',
    .name     = "ramfs",

    .reset    = devramfs_reset,
    .init     = devramfs_init,
    .shutdown = devramfs_shutdown,

    .attach   = devramfs_attach,
    .walk     = devramfs_walk,
    .stat     = devramfs_stat,
    .stat_native = devramfs_stat_native,

    .open     = devramfs_open,
    .create   = devramfs_create,
    .close    = devramfs_close,

    .read     = devramfs_read,
    .bread    = devramfs_bread,
    .write    = devramfs_write,
    .bwrite   = devramfs_bwrite,
    .fsync    = devramfs_fsync,
    // .readdir left NULL at v1.0 -- ramfs enumeration is a deferred nicety;
    // the load-bearing readdir is dev9p (the disk-backed Stratum FS).

    .remove   = devramfs_remove,
    .wstat    = devramfs_wstat,
    .power    = devramfs_power,
};

// Diagnostic accessors for tests.
int  devramfs_file_count(void)              { return g_ramfs_count; }
bool devramfs_initialized(void)             { return g_ramfs_initialized; }
