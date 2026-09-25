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
//      cpio newc entries, and load them into g_ramfs.
//   4. Subsequent walks/reads dispatch by entry index.
//
// The archive is a static tree (ARCH 14.5; dec-2026-09-25-devramfs-
// directories): each entry keeps its parent and its last component, and
// walk, `..`, readdir and stat work per directory. The root holds the
// synthetic mount points, bin/ (every program and data file) and lib/
// (dec-2026-09-25-initrd-bin-directory). Read-only -- writes return -1.
// The blob is never freed: after the pivot joey binds the initrd's bin/
// onto /bin and its lib/ into the /lib union.
//
// dc='m' (memfs / memory-fs); leaves 'r' for an alternative ramfs in a
// later sub-chunk if the layout grows.

#include <thylacine/cpio.h>
#include <thylacine/dev.h>
#include <thylacine/devramfs.h>
#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/path.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>

#include "../arch/arm64/uart.h"

// =============================================================================
// Entry table.
// =============================================================================

// The cap has run out twice as the image grew: 64 silently TRUNCATED the load
// at the U-6e-pre coreutils adoption (dropping /welcome et al.), and the net /
// TLS / Weft arc left ~127 entries under a cap of 128. 256 then held ~235
// entries once the image gained lib/, so 1024 restores room. An overflow is
// counted, reported at boot, and fails devramfs.load_complete. 48 bytes per
// slot in BSS; indices stay far below RAMFS_QID_SYNTH_BASE.
#define RAMFS_ENTRY_MAX 1024

static struct ramfs_entry g_ramfs_entries[RAMFS_ENTRY_MAX];
static struct ramfs_table g_ramfs = { g_ramfs_entries, RAMFS_ENTRY_MAX, 0, 0, false };
static bool               g_ramfs_initialized;

// =============================================================================
// Qid encoding.
// =============================================================================
//
// path = 0                  => root /ramfs (QTDIR)
// path = 1..N               => the entry at index (path - 1): QTDIR for a
//                              directory, QTFILE for a file
// path >= RAMFS_QID_SYNTH_BASE => synthetic mount-point dir (QTDIR)

#define RAMFS_QID_ROOT_PATH  0ULL

// Synthetic mount-point directories (stalk-2, design D4 = Plan 9 M1). The boot
// root (devramfs, before joey pivots to the disk FS) must provide walkable
// directories to mount onto -- a Spoor-identity-keyed mount cannot graft onto a
// path that does not resolve. These dirs are EMPTY (a walk into them misses;
// only `..` -> root succeeds) and exist purely as mount points, at the root
// only. The qid.path range is far above any entry index (RAMFS_ENTRY_MAX), so
// it never collides with an entry's qid.
#define RAMFS_QID_SYNTH_BASE  0x1000000000000000ULL
#define RAMFS_QID_SYNTH_SRV   (RAMFS_QID_SYNTH_BASE + 1ULL)   // /srv
#define RAMFS_QID_SYNTH_PROC  (RAMFS_QID_SYNTH_BASE + 2ULL)   // /proc
#define RAMFS_QID_SYNTH_CTL   (RAMFS_QID_SYNTH_BASE + 3ULL)   // /ctl
#define RAMFS_QID_SYNTH_DEV   (RAMFS_QID_SYNTH_BASE + 4ULL)   // /dev (#57b)
#define RAMFS_QID_SYNTH_HW    (RAMFS_QID_SYNTH_BASE + 5ULL)   // /hw (Menagerie devhw)
#define RAMFS_QID_SYNTH_ENV   (RAMFS_QID_SYNTH_BASE + 6ULL)   // /env (G15, Go Stage 4a)

struct ramfs_synth_dir {
    const char *name;
    u64         qid_path;
};

static const struct ramfs_synth_dir g_ramfs_synth_dirs[] = {
    { "srv",  RAMFS_QID_SYNTH_SRV  },
    { "proc", RAMFS_QID_SYNTH_PROC },
    { "ctl",  RAMFS_QID_SYNTH_CTL  },
    { "dev",  RAMFS_QID_SYNTH_DEV  },
    { "hw",   RAMFS_QID_SYNTH_HW   },
    { "env",  RAMFS_QID_SYNTH_ENV  },
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

#define RAMFS_SYNTH_COUNT (sizeof(g_ramfs_synth_dirs) / sizeof(g_ramfs_synth_dirs[0]))

static inline u64 ramfs_qid_for_index(int idx) {
    return (u64)(idx + 1);
}

static inline bool ramfs_is_dir(const struct ramfs_entry *e) {
    return (e->mode & T_S_IFMT) == T_S_IFDIR;
}

// The entry a qid names; NULL for the root, a synthetic dir, or a path past
// the table.
static const struct ramfs_entry *ramfs_entry_for_qid(const struct ramfs_table *t,
                                                     u64 path) {
    if (path == RAMFS_QID_ROOT_PATH || ramfs_qid_is_synth(path)) return NULL;
    if (path - 1 >= (u64)t->count) return NULL;
    return &t->e[path - 1];
}

// The child of directory `dir` (RAMFS_PARENT_ROOT = the root) whose last
// component is name[0..len), or -1. `name` need not be NUL-terminated.
static int ramfs_find_child(const struct ramfs_table *t, int dir,
                            const char *name, size_t len) {
    for (int i = 0; i < t->count; i++) {
        const struct ramfs_entry *e = &t->e[i];
        if (e->parent != dir) continue;
        size_t j = 0;
        while (j < len && e->leaf[j] == name[j]) j++;
        if (j == len && e->leaf[len] == '\0') return i;
    }
    return -1;
}

static bool ramfs_is_synth_name(const char *name, size_t len) {
    for (size_t k = 0; k < RAMFS_SYNTH_COUNT; k++) {
        const char *s = g_ramfs_synth_dirs[k].name;
        size_t j = 0;
        while (j < len && s[j] == name[j]) j++;
        if (j == len && s[len] == '\0') return true;
    }
    return false;
}

// A component a walk can name: non-empty, not `.` or `..`, and within the
// walk's per-component bound.
static bool ramfs_component_ok(const char *s, size_t len) {
    if (len == 0 || len > SYS_WALK_OPEN_NAME_MAX) return false;
    if (s[0] == '.' && (len == 1 || (len == 2 && s[1] == '.'))) return false;
    return true;
}

// =============================================================================
// Load: place each cpio entry in the tree.
// =============================================================================

// The parent -- the path up to the last '/' -- must already be a directory
// entry: an archive names every directory before its contents, as mkcpio's
// pre-order walk does and as Linux's initramfs, which creates in archive
// order, requires. An entry that cannot be placed is skipped and counted.
static int ramfs_load_cb(const struct cpio_entry *ce, void *arg) {
    struct ramfs_table *t = (struct ramfs_table *)arg;
    u32 type = ce->mode & T_S_IFMT;
    if (type != T_S_IFREG && type != T_S_IFDIR) {
        t->skipped++;
        return 0;
    }

    int         parent = RAMFS_PARENT_ROOT;
    const char *comp   = ce->name;
    const char *p      = ce->name;
    for (;; p++) {
        if (*p != '/' && *p != '\0') continue;
        size_t len = (size_t)(p - comp);
        if (!ramfs_component_ok(comp, len)) {
            t->skipped++;
            return 0;
        }
        if (*p == '\0') break;
        int d = ramfs_find_child(t, parent, comp, len);
        if (d < 0 || !ramfs_is_dir(&t->e[d])) {
            t->skipped++;
            return 0;
        }
        parent = d;
        comp   = p + 1;
    }
    size_t leaf_len = (size_t)(p - comp);
    if ((parent == RAMFS_PARENT_ROOT && ramfs_is_synth_name(comp, leaf_len)) ||
        ramfs_find_child(t, parent, comp, leaf_len) >= 0) {
        t->skipped++;
        return 0;
    }
    if (t->count >= t->cap) {
        t->truncated = true;
        return 1;        // stop iteration
    }
    struct ramfs_entry *e = &t->e[t->count++];
    e->name   = ce->name;
    e->leaf   = comp;
    e->data   = ce->data;
    e->size   = (type == T_S_IFDIR) ? 0 : ce->size;
    // setuid / setgid / sticky mean nothing here; mkcpio drops them too.
    e->mode   = type | (ce->mode & 0777u);
    e->parent = parent;
    return 0;
}

int ramfs_table_load(struct ramfs_table *t, const u8 *blob, size_t size) {
    t->count     = 0;
    t->skipped   = 0;
    t->truncated = false;
    int rv = cpio_newc_iter(blob, size, ramfs_load_cb, t);
    if (rv < 0) t->count = 0;
    return rv;
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

    if (ramfs_table_load(&g_ramfs, blob, blob_size) < 0) {
        uart_puts("  ramfs: cpio parse error\n");
        return;
    }

    int ndirs = 0;
    for (int i = 0; i < g_ramfs.count; i++)
        if (ramfs_is_dir(&g_ramfs.e[i])) ndirs++;
    uart_puts("  ramfs: ");
    uart_putdec((u64)(g_ramfs.count - ndirs));
    uart_puts(" files, ");
    uart_putdec((u64)ndirs);
    uart_puts(" dirs loaded from initrd (");
    uart_putdec((u64)blob_size);
    uart_puts(" bytes");
    if (g_ramfs.skipped > 0) {
        uart_puts(", ");
        uart_putdec((u64)g_ramfs.skipped);
        uart_puts(" SKIPPED -- not placeable");
    }
    if (g_ramfs.truncated) {
        uart_puts(", TRUNCATED -- exceeded RAMFS_ENTRY_MAX");
    }
    uart_puts(")\n");
}

// =============================================================================
// Lookup by archive path.
// =============================================================================

int ramfs_table_find_file(const struct ramfs_table *t, const char *path) {
    for (int i = 0; i < t->count; i++) {
        const struct ramfs_entry *e = &t->e[i];
        if (!ramfs_is_dir(e) && ramfs_streq(e->name, path)) return i;
    }
    return -1;
}

// Public API. Returns 0 on success and populates *out_data / *out_size
// with pointers into the initrd blob (kernel-owned for boot lifetime;
// caller must not modify or free). Returns -1 if devramfs hasn't been
// initialized, no regular file has that archive path, or out_data /
// out_size / name is NULL.
int devramfs_lookup(const char *name, const void **out_data, size_t *out_size) {
    if (!name || !out_data || !out_size)        return -1;
    if (!g_ramfs_initialized)                   return -1;
    int idx = ramfs_table_find_file(&g_ramfs, name);
    if (idx < 0)                                return -1;
    *out_data = (const void *)g_ramfs.e[idx].data;
    *out_size = g_ramfs.e[idx].size;
    return 0;
}

// =============================================================================
// Walk.
// =============================================================================

static void ramfs_qid_set(struct Qid *q, u64 path, u8 type) {
    q->path = path;
    q->vers = 0;
    q->type = type;
    q->pad[0] = q->pad[1] = q->pad[2] = 0;
}

// One step from `cur`. A step starts at a directory, as a 9P walk does: from a
// file every name misses, `..` included. `..` climbs to the parent; the root's
// `..` is the root, and so is an (empty) mount point's.
bool ramfs_table_walk_one(const struct ramfs_table *t, u64 cur, const char *name,
                          struct Qid *out) {
    ramfs_qid_set(out, 0, 0);
    bool dotdot = name[0] == '.' && name[1] == '.' && name[2] == '\0';

    int dir;
    if (cur == RAMFS_QID_ROOT_PATH) {
        dir = RAMFS_PARENT_ROOT;
    } else if (ramfs_qid_is_synth(cur)) {
        if (!dotdot) return false;
        ramfs_qid_set(out, RAMFS_QID_ROOT_PATH, QTDIR);
        return true;
    } else {
        const struct ramfs_entry *d = ramfs_entry_for_qid(t, cur);
        if (!d || !ramfs_is_dir(d)) return false;
        dir = (int)(cur - 1);
    }

    if (dotdot) {
        int up = (dir == RAMFS_PARENT_ROOT) ? RAMFS_PARENT_ROOT : t->e[dir].parent;
        ramfs_qid_set(out, (up == RAMFS_PARENT_ROOT) ? RAMFS_QID_ROOT_PATH
                                                     : ramfs_qid_for_index(up),
                      QTDIR);
        return true;
    }

    // The mount points shadow nothing: the load refuses a root entry of the
    // same name.
    if (dir == RAMFS_PARENT_ROOT) {
        for (size_t k = 0; k < RAMFS_SYNTH_COUNT; k++) {
            if (ramfs_streq(name, g_ramfs_synth_dirs[k].name)) {
                ramfs_qid_set(out, g_ramfs_synth_dirs[k].qid_path, QTDIR);
                return true;
            }
        }
    }
    size_t len = 0;
    while (name[len]) len++;
    int c = ramfs_find_child(t, dir, name, len);
    if (c < 0) return false;
    ramfs_qid_set(out, ramfs_qid_for_index(c), ramfs_is_dir(&t->e[c]) ? QTDIR : QTFILE);
    return true;
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
    struct Spoor *c = dev_simple_attach(&devramfs, QTDIR);
    // #66: devramfs is the root filesystem -- its attach root is "/" (the boot
    // root_spoor; also the /bin bind SOURCE, where stalk_cross_mounts then
    // transplants "/bin" onto the crossed clone, so this "/" is seen only when
    // devramfs IS the namespace root). Seeded at birth, before publication ->
    // immutable (I-33 set-before-publish; no lock). NULL (OOM) -> "unknown".
    if (c) c->path = path_make_root();
    return c;
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
        if (!ramfs_table_walk_one(&g_ramfs, cur->qid.path, name[i], &next)) break;
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

// ramfs_table_stat — the qid-keyed t_stat fill shared by devramfs_stat_native
// (SYS_FSTAT / the stalk X-search) and devramfs_walk_attrs (the POUNCE
// per-component records — one source of truth so the pounce cannot diverge
// from the per-component loop).
//
// Returns 0 on success, -1 if `qid_path` does not name a known node.
int ramfs_table_stat(const struct ramfs_table *t, u64 qid_path, struct t_stat *out) {
    // Zero everything first so any field we don't set is a defined zero.
    // The caller can rely on every t_stat reaching it byte-for-byte equal
    // to what the kernel wrote, with no stack-garbage leakage.
    for (size_t i = 0; i < sizeof(*out); i++) ((u8 *)out)[i] = 0;

    if (qid_path == RAMFS_QID_ROOT_PATH) {
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

    if (ramfs_qid_is_synth(qid_path)) {
        // A synthetic mount-point dir (stalk-2 D4): system-owned, world-r/x
        // (0555), same as the root. The X bit is load-bearing -- stalk's
        // per-component X-search must pass for a principal to traverse onto the
        // mount point and cross. Empty (no entries); a directory.
        out->mode      = T_S_IFDIR | 0555u;
        out->nlink     = 1;
        out->qid_path  = qid_path;
        out->qid_vers  = 0;
        out->qid_type  = QTDIR;
        out->blksize   = 4096;
        out->uid       = PRINCIPAL_SYSTEM;
        out->gid       = GID_SYSTEM;
        return 0;
    }

    const struct ramfs_entry *e = ramfs_entry_for_qid(t, qid_path);
    if (!e) return -1;

    // The load kept only T_S_IFREG / T_S_IFDIR with the permission bits, so
    // the mode passes through as a POSIX mode (pouch fstat translates).
    bool dir = ramfs_is_dir(e);
    out->size      = dir ? 0 : (u64)e->size;
    out->mode      = e->mode;
    out->nlink     = 1;
    out->qid_path  = qid_path;
    out->qid_vers  = 0;
    out->qid_type  = dir ? QTDIR : QTFILE;
    out->blksize   = 4096;
    // POSIX blocks: count of 512-byte units. ceil(size / 512).
    out->blocks    = dir ? 0 : (u64)((e->size + 511u) / 512u);
    out->uid       = PRINCIPAL_SYSTEM;
    out->gid       = GID_SYSTEM;
    return 0;
}

// devramfs_stat_native — the SYS_FSTAT vtable surface (P6-pouch-stratumd-boot
// 16b-gamma): the caller (syscall handler) provides a kernel-scratch t_stat;
// the handler then copies it out to user-VA byte-by-byte via uaccess_store_u8.
static int devramfs_stat_native(struct Spoor *c, struct t_stat *out) {
    if (!c || !out) return -1;
    return ramfs_table_stat(&g_ramfs, c->qid.path, out);
}

// The POUNCE walk-fused getattr (docs/POUNCE-DESIGN.md §4) — native: the
// attrs live in the in-kernel ramfs table, so this is walk + stat per
// component with zero I/O. Exists so stalk keeps ONE fast-path shape across
// both perm-enforced Devs (the design's stated reason), and because the
// per-component walk cursor here is a local u64 — the strict walk_attrs
// contract (nc untouched unless the walk is FULL) falls out naturally.
static struct Walkqid *devramfs_walk_attrs(struct Spoor *c, struct Spoor *nc,
                                           const char **names,
                                           const size_t *name_lens,
                                           int nname, struct t_stat *sts) {
    if (!c || nname <= 0 || nname > DEV_WALK_ATTRS_MAX) return NULL;
    if (!names || !name_lens || !sts) return NULL;

    struct Walkqid *wq = walkqid_alloc(nname);
    if (!wq) return NULL;

    u64 cur = c->qid.path;
    int n = 0;
    for (int i = 0; i < nname; i++) {
        // walk_one takes a NUL-terminated name; names[] are (ptr, len) slices
        // into the resolver's path buffer. Per-component bounded copy (the
        // resolver enforces len <= SYS_WALK_OPEN_NAME_MAX before calling).
        char nb[SYS_WALK_OPEN_NAME_MAX + 1];
        size_t l = name_lens[i];
        if (l == 0 || l > SYS_WALK_OPEN_NAME_MAX) break;
        for (size_t k = 0; k < l; k++) nb[k] = names[i][k];
        nb[l] = '\0';

        struct Qid next;
        if (!ramfs_table_walk_one(&g_ramfs, cur, nb, &next)) break;
        if (ramfs_table_stat(&g_ramfs, next.path, &sts[n]) != 0) break;
        cur = next.path;
        wq->qid[n++] = next;
    }

    wq->nqid = n;
    if (nc && n == nname) {
        nc->qid  = wq->qid[n - 1];
        wq->spoor = nc;
    } else {
        // Partial walk or the query form: nothing transitioned (contract:
        // nc untouched — devramfs Spoors are qid-only, so "untouched" means
        // the qid is not advanced).
        wq->spoor = NULL;
    }
    return wq;
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

long ramfs_table_read(const struct ramfs_table *t, u64 qid_path, void *buf, long n,
                      s64 off) {
    if (!buf) return -1;
    if (n < 0) return -1;
    if (n == 0) return 0;
    if (off < 0) return -1;

    // A directory's bytes are not readable -- the root, a mount point and an
    // archive directory alike; readdir lists them (same as devproc / devctl).
    const struct ramfs_entry *f = ramfs_entry_for_qid(t, qid_path);
    if (!f || ramfs_is_dir(f)) return -1;
    if ((size_t)off >= f->size) return 0;        // EOF

    size_t avail = f->size - (size_t)off;
    size_t copy = avail > (size_t)n ? (size_t)n : avail;

    u8 *out = (u8 *)buf;
    for (size_t i = 0; i < copy; i++) out[i] = f->data[(size_t)off + i];
    return (long)copy;
}

static long devramfs_read(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c) return -1;
    return ramfs_table_read(&g_ramfs, c->qid.path, buf, n, off);
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

// ramfs_table_readdir -- enumerate the directory `dir_path` names: an archive
// directory lists its children; the root lists its children plus the six
// synthetic mount-point dirs, which are themselves empty. Emits the Thylacine
// 9P2000.L dirent wire format the SYS_READDIR handler parses
// (kernel/syscall.c sys_readdir_handler):
//
//   qid(13) + offset(8 LE) + type(1) + name_len(2 LE) + name(name_len)
//
// The `offset` field is a RESUME COOKIE, not a byte position: a 1-based ordinal
// over one global order, [every entry | synth-dirs], of which a directory
// emits only its own children. `off` (the cookie of the last entry the
// previous call handed out, or 0 to start) selects where to resume, so
// successive calls walk forward with no duplication and no skip -- mirroring
// dev9p's server-cookie semantics. The handler stores the last emitted entry's
// cookie back into c->offset for the next call. Whole entries only: emit until
// the next would not fit `n`, leaving the rest for the next call. Returns the
// byte count (>= 0; 0 == end-of-directory), -1 on a non-directory.
//
// `n` is the handler's user buf_len, already bounded to (0, SYS_RW_STACK] and to
// the kernel scratch size, so writing up to `n` bytes into `buf` is in-bounds.
// If the FIRST entry of a run does not fit `n`, returns -1 (not 0): 0 means
// end-of-directory per the ABI, so reporting it would silently truncate the
// listing -- instead we signal "buffer too small" the way Linux getdents does
// with EINVAL, and the caller must enlarge its buffer. libthyla_rs::fs::ReadDir
// stages 4 KiB, far above 24 + any name, so it never trips this.
long ramfs_table_readdir(const struct ramfs_table *t, u64 dir_path, void *buf, long n,
                         s64 off) {
    if (!buf) return -1;
    if (n <= 0) return 0;

    int dir;
    if (dir_path == RAMFS_QID_ROOT_PATH) {
        dir = RAMFS_PARENT_ROOT;
    } else if (ramfs_qid_is_synth(dir_path)) {
        return 0;                                  // an empty mount point: EOD
    } else {
        const struct ramfs_entry *d = ramfs_entry_for_qid(t, dir_path);
        if (!d || !ramfs_is_dir(d)) return -1;     // a file does not enumerate
        dir = (int)(dir_path - 1);
    }

    u8 *out = (u8 *)buf;
    long cap = n;
    long pos = 0;

    u64 nentries = (u64)t->count;
    u64 nsynth   = (dir == RAMFS_PARENT_ROOT) ? (u64)RAMFS_SYNTH_COUNT : 0;
    u64 total    = nentries + nsynth;

    u64 start = (off < 0) ? 0 : (u64)off;     // 0 = from the beginning
    for (u64 ord = start + 1; ord <= total; ord++) {
        const char *name;
        u8  qtype;
        u64 qpath;
        if (ord <= nentries) {
            const struct ramfs_entry *e = &t->e[ord - 1];
            if (e->parent != dir) continue;
            name  = e->leaf;
            qtype = ramfs_is_dir(e) ? QTDIR : QTFILE;
            qpath = ord;                           // == ramfs_qid_for_index(ord - 1)
        } else {
            const struct ramfs_synth_dir *sd = &g_ramfs_synth_dirs[ord - 1 - nentries];
            name  = sd->name;
            qtype = QTDIR;
            qpath = sd->qid_path;
        }

        u64 nlen = 0;
        while (name[nlen] != '\0') nlen++;
        if (nlen > 0xffffu) return -1;        // name_len is a u16 field (cold: the load bounds a component)
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
    return pos;        // 0 iff no child at or past the cookie (EOD) or the first entry did not fit
}

static long devramfs_readdir(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c) return -1;
    return ramfs_table_readdir(&g_ramfs, c->qid.path, buf, n, off);
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
    // #217 F1: serves real file content -- may back executable pages.
    .may_back_exec = true,
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
    .walk_attrs = devramfs_walk_attrs,   // POUNCE: native (RAM-table) fused walk+attrs
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
int  devramfs_entry_count(void)             { return g_ramfs.count; }
int  devramfs_synth_dir_count(void)         { return (int)RAMFS_SYNTH_COUNT; }
bool devramfs_initialized(void)             { return g_ramfs_initialized; }
int  devramfs_skipped_count(void)           { return g_ramfs.skipped; }
bool devramfs_truncated(void)               { return g_ramfs.truncated; }

int devramfs_root_child_count(void) {
    int n = 0;
    for (int i = 0; i < g_ramfs.count; i++)
        if (g_ramfs.e[i].parent == RAMFS_PARENT_ROOT) n++;
    return n;
}
