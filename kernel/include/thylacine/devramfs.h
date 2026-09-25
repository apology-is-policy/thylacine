// Public helpers for the in-kernel ramfs reader.
//
// devramfs (kernel/devramfs.c) parses the initrd cpio at boot and exposes
// files through the bestiary Dev vtable. For internal callers that need
// to slurp a file by name without going through the full Spoor /
// Walkqid / open / read path (e.g., test harnesses, boot-time program
// loaders), this header exposes a direct lookup.
//
// The returned pointer aliases storage owned by the initrd blob — which
// the kernel keeps mapped for the lifetime of the boot — so callers
// MUST NOT free it. The pointer is valid until kernel shutdown.

#ifndef THYLACINE_DEVRAMFS_H
#define THYLACINE_DEVRAMFS_H

#include <thylacine/types.h>

struct Qid;
struct t_stat;

// devramfs_lookup — find the regular file at archive path `name` (e.g.
// "joey", "lib/libc.so") in the parsed cpio. On success returns 0 and
// fills *out_data + *out_size with pointers into the initrd blob (caller
// MUST NOT modify or free). A directory is never found.
//
// Returns:
//   0   on success
//  -1   not found, or devramfs not initialized, or NULL args.
int devramfs_lookup(const char *name, const void **out_data, size_t *out_size);

// The entry table behind the Dev (ARCH 14.5; dec-2026-09-25-devramfs-
// directories): a static tree loaded from a cpio newc archive. The Dev serves
// one table, loaded from the initrd; the tests load crafted archives into
// their own. Only devramfs.c and the tests use the ramfs_table_* calls.
#define RAMFS_PARENT_ROOT (-1)

struct ramfs_entry {
    const char *name;     // the full archive path, NUL-terminated, in the blob
    const char *leaf;     // its last component: a suffix of name
    const u8   *data;     // file content, in the blob; a directory has none
    size_t      size;
    u32         mode;     // T_S_IFREG or T_S_IFDIR plus the archive's permission bits
    int         parent;   // the parent directory's index; RAMFS_PARENT_ROOT = the root
};

struct ramfs_table {
    struct ramfs_entry *e;
    int                 cap;
    int                 count;
    int                 skipped;     // entries the load could not place
    bool                truncated;   // the archive named more than cap placeable entries
};

// Load `blob` into `t` (whose e and cap the caller set). Returns
// cpio_newc_iter's verdict: < 0 = a malformed archive, and t is then empty.
int  ramfs_table_load(struct ramfs_table *t, const u8 *blob, size_t size);
bool ramfs_table_walk_one(const struct ramfs_table *t, u64 cur, const char *name,
                          struct Qid *out);
int  ramfs_table_stat(const struct ramfs_table *t, u64 qid_path, struct t_stat *out);
long ramfs_table_read(const struct ramfs_table *t, u64 qid_path, void *buf, long n,
                      s64 off);
long ramfs_table_readdir(const struct ramfs_table *t, u64 dir_path, void *buf, long n,
                         s64 off);
int  ramfs_table_find_file(const struct ramfs_table *t, const char *path);

#endif // THYLACINE_DEVRAMFS_H
