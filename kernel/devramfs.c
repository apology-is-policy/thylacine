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
// 14) plus the Phase 7 Utopia userspace -- the U-6e-pre coreutils adoption
// pushed the live cpio to ~69 entries (it had been ~51), so the prior cap of
// 64 silently TRUNCATED the load (dropping /welcome et al.). 128 restores
// comfortable headroom for the deferred coreutils + future tools. 24 bytes
// per slot; the static array sits in BSS (~3 KiB), well below the synth-dir
// qid base (RAMFS_QID_SYNTH_BASE) so file indices never collide.
#define RAMFS_FILE_MAX 128

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
// path = 0                  => root /ramfs (QTDIR)
// path = 1..N               => file at index (path - 1) (QTFILE)
// path >= RAMFS_QID_SYNTH_BASE => synthetic mount-point dir (QTDIR)

#define RAMFS_QID_ROOT_PATH  0ULL

// Synthetic mount-point directories (stalk-2, design D4 = Plan 9 M1). The boot
// root (devramfs, before joey pivots to the disk FS) must provide walkable
// directories to mount onto -- a Spoor-identity-keyed mount cannot graft onto a
// path that does not resolve. These dirs are EMPTY (a walk into them misses;
// only `..` -> root succeeds) and exist purely as mount points. The qid.path
// range is well above any plausible file index (RAMFS_FILE_MAX = 128), so it
// never collides with a real file's qid.
#define RAMFS_QID_SYNTH_BASE  0x1000000000000000ULL
#define RAMFS_QID_SYNTH_SRV   (RAMFS_QID_SYNTH_BASE + 1ULL)   // /srv
#define RAMFS_QID_SYNTH_PROC  (RAMFS_QID_SYNTH_BASE + 2ULL)   // /proc
#define RAMFS_QID_SYNTH_CTL   (RAMFS_QID_SYNTH_BASE + 3ULL)   // /ctl

struct ramfs_synth_dir {
    const char *name;
    u64         qid_path;
};

static const struct ramfs_synth_dir g_ramfs_synth_dirs[] = {
    { "srv",  RAMFS_QID_SYNTH_SRV  },
    { "proc", RAMFS_QID_SYNTH_PROC },
    { "ctl",  RAMFS_QID_SYNTH_CTL  },
};

static inline bool ramfs_qid_is_synth(u64 path) {
    return path >= RAMFS_QID_SYNTH_BASE;
}

// Single-component name equality (both NUL-terminated). The walk caller has
// already split + NUL-terminated each component.
static bool ramfs_streq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == '\0' && b[i] == '\0';
}

static inline u64 ramfs_qid_for_index(int idx) {
    return (u64)(idx + 1);
}

static inline int ramfs_index_for_qid(u64 path) {
    if (path == 0) return -1;
    if (ramfs_qid_is_synth(path)) return -1;   // synth dir, not a file index
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
        // Synthetic mount-point dirs (stalk-2 D4) shadow same-named files (none
        // ship by those names): /srv, /proc resolve to empty walkable dirs.
        for (size_t k = 0; k < sizeof(g_ramfs_synth_dirs) /
                                sizeof(g_ramfs_synth_dirs[0]); k++) {
            if (ramfs_streq(name, g_ramfs_synth_dirs[k].name)) {
                out_qid->path = g_ramfs_synth_dirs[k].qid_path;
                out_qid->type = QTDIR;
                return true;
            }
        }
        int idx = ramfs_find_by_name(name);
        if (idx < 0) return false;
        out_qid->path = ramfs_qid_for_index(idx);
        out_qid->type = QTFILE;
        return true;
    }
    // Walking out of a synthetic mount-point dir misses (they are empty; `..`
    // back to root was handled above). They exist only as mount points.
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

    if (ramfs_qid_is_synth(c->qid.path)) {
        // A synthetic mount-point dir (stalk-2 D4): system-owned, world-r/x
        // (0555), same as the root. The X bit is load-bearing -- stalk's
        // per-component X-search must pass for a principal to traverse onto the
        // mount point and cross. Empty (no entries); a directory.
        out->mode      = T_S_IFDIR | 0555u;
        out->nlink     = 1;
        out->qid_path  = c->qid.path;
        out->qid_vers  = 0;
        out->qid_type  = QTDIR;
        out->blksize   = 4096;
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

// devramfs_readdir -- enumerate the directory backed by `c`. The boot ramfs is
// a FLAT namespace: the root lists every cpio file plus the synthetic mount-
// point dirs (srv, proc); every file is a leaf, and the synthetic dirs are
// empty mount points. Emits the Thylacine 9P2000.L dirent wire format the
// SYS_READDIR handler parses (kernel/syscall.c sys_readdir_handler):
//
//   qid(13) + offset(8 LE) + type(1) + name_len(2 LE) + name(name_len)
//
// The `offset` field is a RESUME COOKIE, not a byte position: a 1-based ordinal
// over the concatenated [files | synth-dirs] list. `off` (the cookie of the last
// entry the previous call handed out, or 0 to start) selects where to resume, so
// successive calls walk forward with no duplication and no skip -- mirroring
// dev9p's server-cookie semantics. The handler stores the last emitted entry's
// cookie back into c->offset for the next call. Whole entries only: emit until
// the next would not fit `n`, leaving the rest for the next call. Returns the
// byte count (>= 0; 0 == end-of-directory), -1 on a non-directory Spoor.
//
// `n` is the handler's user buf_len, already bounded to (0, SYS_RW_MAX] and to
// the kernel scratch size, so writing up to `n` bytes into `buf` is in-bounds.
// If the FIRST entry of a run does not fit `n`, returns -1 (not 0): 0 means
// end-of-directory per the ABI, so reporting it would silently truncate the
// listing -- instead we signal "buffer too small" the way Linux getdents does
// with EINVAL, and the caller must enlarge its buffer. libthyla_rs::fs::ReadDir
// stages 4 KiB, far above 24 + any name, so it never trips this.
static long devramfs_readdir(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf) return -1;
    if (n <= 0) return 0;

    // Only directories enumerate. A regular-file Spoor is not a directory; a
    // synthetic mount-point dir (srv/proc) is a directory but empty (EOD).
    bool is_root  = (c->qid.path == RAMFS_QID_ROOT_PATH);
    bool is_synth = ramfs_qid_is_synth(c->qid.path);
    if (!is_root && !is_synth) return -1;
    if (is_synth) return 0;

    u8 *out = (u8 *)buf;
    long cap = n;
    long pos = 0;

    int nfiles = g_ramfs_count;
    int nsynth = (int)(sizeof(g_ramfs_synth_dirs) / sizeof(g_ramfs_synth_dirs[0]));
    u64 total  = (u64)nfiles + (u64)nsynth;

    u64 start = (off < 0) ? 0 : (u64)off;     // 0 = from the beginning
    for (u64 ord = start + 1; ord <= total; ord++) {
        const char *name;
        u8  qtype;
        u64 qpath;
        if (ord <= (u64)nfiles) {
            name  = g_ramfs_files[ord - 1].name;
            qtype = QTFILE;
            qpath = ramfs_qid_for_index((int)(ord - 1));
        } else {
            const struct ramfs_synth_dir *sd = &g_ramfs_synth_dirs[ord - 1 - (u64)nfiles];
            name  = sd->name;
            qtype = QTDIR;
            qpath = sd->qid_path;
        }

        u64 nlen = 0;
        while (name[nlen] != '\0') nlen++;
        if (nlen > 0xffffu) return -1;        // name_len is a u16 field (cold: cpio names are short)
        long entry = 24 + (long)nlen;
        if (pos + entry > cap) {
            // The first entry of this run does not fit the caller's buffer:
            // signal "too small" (-1) rather than 0, which the handler would
            // pass up as a (silently truncating) EOD. A later entry not fitting
            // just resumes on the next call.
            if (pos == 0) return -1;
            break;
        }

        // qid(13): type(1) + version(4 LE, 0) + path(8 LE).
        out[pos + 0] = qtype;
        out[pos + 1] = 0; out[pos + 2] = 0; out[pos + 3] = 0; out[pos + 4] = 0;
        for (int b = 0; b < 8; b++) out[pos + 5 + b]  = (u8)(qpath >> (8 * b));
        // offset(8 LE): the resume cookie = this entry's ordinal.
        for (int b = 0; b < 8; b++) out[pos + 13 + b] = (u8)(ord >> (8 * b));
        // type(1): the d_type byte (we mirror the qid type bits).
        out[pos + 21] = qtype;
        // name_len(2 LE).
        out[pos + 22] = (u8)(nlen & 0xffu);
        out[pos + 23] = (u8)((nlen >> 8) & 0xffu);
        // name.
        for (u64 i = 0; i < nlen; i++) out[pos + 24 + (long)i] = (u8)name[i];
        pos += entry;
    }
    return pos;        // 0 iff start >= total (EOD) or the first entry did not fit
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
    // A-2d: system-owned, world-readable boot FS -> rwx enforcement is live
    // (the boot chain owns everything it touches; a non-system principal gets
    // other-r/x but not write). See <thylacine/dev.h>::perm_enforced.
    .perm_enforced = true,

    .reset    = devramfs_reset,
    .init     = devramfs_init,
    .shutdown = devramfs_shutdown,

    .attach   = devramfs_attach,
    .walk     = devramfs_walk,
    .stat     = devramfs_stat,
    .stat_native = devramfs_stat_native,
    .seekable = true,   // file content: read/write honor the byte offset (RW-4 R2-F2)

    .open     = devramfs_open,
    .create   = devramfs_create,
    .close    = devramfs_close,

    .read     = devramfs_read,
    .bread    = devramfs_bread,
    .write    = devramfs_write,
    .bwrite   = devramfs_bwrite,
    .fsync    = devramfs_fsync,
    // U-6e-b-1: the boot ramfs enumerates so read_dir / ls / glob work on the
    // pre-pivot FS the user stands in (the synthetic root + srv/proc dirs). The
    // disk-backed Stratum FS (dev9p) is the post-pivot readdir target.
    .readdir  = devramfs_readdir,

    .remove   = devramfs_remove,
    .wstat    = devramfs_wstat,
    .power    = devramfs_power,
};

// Diagnostic accessors for tests.
int  devramfs_file_count(void)              { return g_ramfs_count; }
bool devramfs_initialized(void)             { return g_ramfs_initialized; }
