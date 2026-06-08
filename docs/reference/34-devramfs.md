# 34 — devramfs: cpio-loaded in-memory filesystem (P4-E)

The kernel's first **content-bearing** Dev. Loads a cpio newc archive at boot via QEMU's `-initrd` mechanism and exposes the files via the standard Dev vtable. dc='m'. Per ARCH §9.4 + ROADMAP §6.1.

---

## Purpose

ramfs is the bridge between the bootloader and the persistent FS. Phase 5+ Stratum integration replaces the role; until then, ramfs ships:

- **Sample text files** (`/ramfs/welcome`, `/ramfs/version`) — testable end-to-end smoke from cpio parse → walk → read.
- **Future:** `/joey` blob (the v1.0 first userspace binary; currently embedded in `kernel/joey.c::g_joey_elf_blob`), driver binaries (`drivers/virtio-blk` Rust ELF), shell + coreutils (Phase 5+).

The cpio data lives in physical memory between `/chosen/linux,initrd-start` and `/chosen/linux,initrd-end`; the kernel reads it through the direct map (`pa_to_kva`).

ROADMAP §6.1 says "freed once persistent FS is mounted" — at v1.0 the initrd memory lives forever. Phase 5+ Stratum landing introduces the freeing step.

---

## Boot flow

```
QEMU -initrd build/ramfs.cpio
   ↓
QEMU advertises [linux,initrd-start, linux,initrd-end) via DTB /chosen
   ↓
boot_main: dtb_init parses DTB
   ↓
phys_init: reserves the initrd PA range (4b. in mm/phys.c — bug fix
            in P4-E so buddy doesn't reuse those pages and clobber
            the cpio bytes)
   ↓
dev_init: dev_register(&devramfs); bestiary walk calls
            devramfs.init = devramfs_init_hook
   ↓
devramfs_init_hook:
   - dtb_get_chosen_initrd(&start_pa, &end_pa)
   - blob_kva = pa_to_kva(start_pa)
   - cpio_newc_iter(blob, blob_size, ramfs_init_cb, NULL)
   - ramfs_init_cb populates g_ramfs_files[g_ramfs_count++] with
       (name, data_kva, size, mode) per entry
   - Print "ramfs: N files loaded from initrd (B bytes)"
```

---

## Public API — `<thylacine/dev.h>` + `<thylacine/cpio.h>` + `<thylacine/dtb.h>`

```c
extern struct Dev devramfs;        // dc='m', name="ramfs"

// Diagnostics for tests.
int  devramfs_file_count(void);
bool devramfs_initialized(void);

// CPIO parser (kernel/cpio.{h,c}).
struct cpio_entry { const char *name; const u8 *data; size_t size; u32 mode; };
bool cpio_newc_is_valid(const u8 *blob, size_t blob_size);
int  cpio_newc_iter(const u8 *blob, size_t blob_size,
                    int (*cb)(const struct cpio_entry *, void *), void *arg);
int  cpio_newc_count(const u8 *blob, size_t blob_size);

// DTB initrd probe (lib/dtb.c).
bool dtb_get_chosen_initrd(u64 *start, u64 *end);
```

---

## Namespace + qid encoding

```
/ramfs                            path = 0                    QTDIR
/ramfs/<file_0>                   path = 1                    QTFILE
/ramfs/<file_1>                   path = 2                    QTFILE
...
/ramfs/<file_N>                   path = N+1                  QTFILE
```

File index = `path - 1`. RAMFS_FILE_MAX = 32 entries; cpio archives larger than 32 entries truncate at boot (banner notes "TRUNCATED").

---

## Walk semantics

Single-level layout (no subdirectories at v1.0):

| `cur_path` | `name` | Result |
|---|---|---|
| any | `".."` | go up (to root) |
| `0` (root) | matches a registered file's name | leaf qid (QTFILE) |
| anywhere else | any | miss |

Multi-step walks supported via the same `walkqid_alloc / spoor_clone / walk_one` loop. Reuses the directory-Dev pattern from devproc + devctl.

---

## Per-file content

Files stored as references into the cpio blob (no copy):

```c
struct ramfs_file {
    const char *name;        // NUL-terminated; lives in the cpio blob
    const u8   *data;        // file content; lives in the cpio blob
    size_t      size;
    u32         mode;
};
static struct ramfs_file g_ramfs_files[RAMFS_FILE_MAX];
```

Reads dispatch by qid index → memcpy from `f->data + offset` into the caller's buffer. No buffering; the cpio blob is the canonical storage.

---

## Implementation

| File | LOC | Scope |
|---|---|---|
| `tools/mkcpio.py` | ~80 | Host script: emit cpio newc archive from a directory. Used by tools/build.sh::build_ramfs. |
| `tools/build.sh` (build_ramfs) | ~25 | Generate ramfs source files (`welcome`, `version`) into `build/ramfs-src/`; invoke `mkcpio.py` to produce `build/ramfs.cpio`. Chained from `build_kernel` so every kernel build refreshes the cpio. |
| `tools/run-vm.sh` | ~10 | `-initrd $RAMFS_CPIO` flag (auto-skipped if the file is missing). |
| `kernel/include/thylacine/cpio.h` | ~60 | Iterator API. |
| `kernel/cpio.c` | ~140 | Newc parser: 110-byte ASCII-hex header → name + 4-byte align + data + 4-byte align. Trailer entry "TRAILER!!!" stops iteration. |
| `kernel/include/thylacine/dtb.h` (+1 fn) | +5 | `dtb_get_chosen_initrd(u64 *start, u64 *end)`. |
| `lib/dtb.c` (+1 fn) | +30 | Probe `/chosen/linux,initrd-start` + `linux,initrd-end`; supports 4- or 8-byte big-endian cells. |
| `mm/phys.c` (+initrd reservation) | +30 | Add initrd PA range to `phys_init`'s reservation list — closes the buddy-clobbers-cpio bug. |
| `kernel/devramfs.c` | ~270 | Dev (dc='m'); init reads DTB initrd + parses cpio + populates file table; walk + read + offset. |
| `kernel/test/test_cpio.c` | ~160 | 7 unit tests against synthetic in-memory cpio archives (constructed byte-by-byte). |
| `kernel/test/test_devramfs.c` | ~190 | 10 integration tests against the actual initrd-loaded files. |

### Newc parsing details

Header layout (110 bytes total):

```c
#define CPIO_NEWC_HDR_SIZE 110
#define OFF_MAGIC      0    // 6 bytes — "070701"
#define OFF_INO        6    // 8 ASCII hex
#define OFF_MODE       14
#define OFF_UID        22
#define OFF_GID        30
#define OFF_NLINK      38
#define OFF_MTIME      46
#define OFF_FILESIZE   54
#define OFF_DEVMAJOR   62
#define OFF_DEVMINOR   70
#define OFF_RDEVMAJOR  78
#define OFF_RDEVMINOR  86
#define OFF_NAMESIZE   94
#define OFF_CHECK      102
```

After the header: filename (NUL-terminated, `c_namesize` bytes incl. NUL); pad to 4-byte boundary computed FROM THE START of the header (`align4(name_off + namesize)`); file data; pad to 4-byte boundary again.

Trailer entry has filename `"TRAILER!!!"` and `c_filesize=0`. The iterator stops at the trailer and returns 0.

### Why the initrd reservation matters

Pre-P4-E `phys_init` reserved 4 regions: low firmware, kernel image, struct page array, DTB blob. The initrd was NOT reserved — buddy returned those pages on the free list. Subsequent allocations could reuse them, clobbering the cpio bytes that `g_ramfs_files[].name` and `.data` point at.

The bug surfaces under any condition that pushes buddy allocations far enough to reach the initrd PA range. UBSan's larger kernel (~9% bigger) + sanitizer instrumentation over hot paths exposed it deterministically: walk_to_welcome failed because the cpio name field had been overwritten between init and test runtime.

P4-E adds a fifth reservation in `phys_init`:

```c
paddr_t initrd_pa_start = 0, initrd_pa_end = 0;
bool    have_initrd = false;
{
    u64 s, e;
    if (dtb_get_chosen_initrd(&s, &e)) {
        initrd_pa_start = round_down((paddr_t)s, PAGE_SIZE);
        initrd_pa_end   = round_up((paddr_t)e, PAGE_SIZE);
        have_initrd = true;
    }
}
// ...add to res[] array; sort; check disjoint; pass to buddy_free_region loop.
```

The reservation is conditional: kernels booted without `-initrd` skip it cleanly.

---

## Spec cross-reference

P4-E is impl-only — no new TLA+ module. The cpio parser is config-parsing (per CLAUDE.md "skip the spec; just write + test"). The Dev pattern is the same as devproc + devctl.

Future Phase 5+ Stratum integration adds an invariant on initrd lifetime ("freed once Stratum mounts persistent FS"); the spec for that lifecycle lives in Stratum's repo.

---

## Tests

### CPIO parser (`test_cpio.c`, 7 tests)

- `cpio.is_valid_recognizes_magic` — magic check accepts/rejects.
- `cpio.iter_empty_archive` — trailer-only archive: 0 entries, clean exit.
- `cpio.iter_single_entry` — one entry: name + size + first-byte data.
- `cpio.iter_two_entries` — two entries reported in order.
- `cpio.iter_rejects_truncated` — archive truncated mid-entry: negative error.
- `cpio.iter_rejects_bad_magic` — corrupted magic: negative error.
- `cpio.count_matches` — count helper matches three-entry archive.

Tests build synthetic cpio newc archives byte-by-byte using inline `emit_hdr` / `emit_name` / `emit_data` helpers — no host file I/O, no QEMU plumbing required.

### devramfs (`test_devramfs.c`, 15 tests)

- `devramfs.bestiary_smoke` — registration + lookup by dc/name.
- `devramfs.initialized_with_files` — devramfs_initialized + count >= 2.
- `devramfs.attach_returns_dir` — root attach: QTDIR, path=0.
- `devramfs.walk_to_welcome` — walk("welcome") yields QTFILE.
- `devramfs.walk_unknown_misses` — walk("no-such-file"): nqid=0.
- `devramfs.read_welcome` — content contains "Welcome" + "Thylacine".
- `devramfs.read_version` — content contains "Thylacine" + "0.1-dev".
- `devramfs.read_partial_offset` — partial slice + EOF semantics.
- `devramfs.read_dir_returns_neg1` — plain `read()` on a directory returns -1 (use `readdir`).
- `devramfs.write_rejected` — write returns -1 (read-only).
- `devramfs.stat_native_system_owned` — A-2a: every entry SYSTEM-owned.
- `devramfs.readdir_enumerates_root` (U-6e-b-1) — root lists welcome/version + the synthetic srv/proc dirs; the qid type byte marks files QTFILE and srv/proc QTDIR.
- `devramfs.readdir_file_returns_neg1` (U-6e-b-1) — readdir on a regular-file Spoor returns -1.
- `devramfs.readdir_buffer_too_small_errs` (U-6e-b-1) — a buffer too small for the first entry returns -1 (not 0/EOD).
- `devramfs.readdir_synth_dir_empty` (U-6e-b-1) — readdir on the bare srv synthetic dir returns 0 (empty).
- `devramfs.readdir_paginates_no_dup_no_skip` (U-6e-b-1) — a small buffer paginates the whole root via the resume cookie with strictly-increasing cookies and exactly `files + 2` entries (no dup, no skip).

Tests skip gracefully when `devramfs_file_count() < N` — environments without `-initrd` (e.g., a future bare-board boot) keep the bestiary tests valid even when the file-content tests can't run.

---

## Status

| Component | State |
|---|---|
| `tools/mkcpio.py` | Landed (P4-E) |
| `tools/build.sh::build_ramfs` | Landed (P4-E) |
| `tools/run-vm.sh` `-initrd` flag | Landed (P4-E) |
| `kernel/cpio.{h,c}` | Landed (P4-E) |
| `dtb_get_chosen_initrd` | Landed (P4-E) |
| `phys_init` initrd reservation | Landed (P4-E) — closes the buddy-clobbers-cpio bug |
| `kernel/devramfs.c` + devramfs Dev (dc='m') | Landed (P4-E) |
| In-kernel tests | 22 (7 cpio unit + 15 devramfs integration) |
| `devramfs_readdir` (dc='m' `.readdir` slot) | Landed (U-6e-b-1) — flat-root enumeration over `SYS_READDIR` |
| Bestiary count | 8 (devnone + cons + null + zero + random + proc + ctl + ramfs) |
| Boot banner: `ramfs: N files loaded from initrd (B bytes)` | Landed (P4-E) |
| /joey blob loaded from ramfs | Held — joey.c currently builds the blob inline; future chunk swaps to ramfs lookup |
| Driver binaries in ramfs | Held to P4-I+ (Rust userspace drivers) |
| Subdirectory walk in ramfs | Held to a Phase 4+ chunk if needed |
| Initrd freeing on Stratum mount | Held to Phase 5+ Stratum integration |

---

## Known caveats / footguns

### `RAMFS_FILE_MAX = 128`

A cpio archive with > 128 entries truncates at boot. Banner notes "TRUNCATED" and the `perm.devramfs_enforced_real_metadata` test (it walks `/welcome`) catches a silent drop. The live Phase 7 corpus is ~72 entries; comfortable headroom. Bump or switch to a dynamic table if it grows.

### Initrd memory lives forever at v1.0

ROADMAP §6.1 wording is "freed once persistent FS is mounted" — Phase 5+ Stratum landing adds the freeing. v1.0 the cpio bytes occupy initrd PA permanently. With the small archive (< 1 KiB) this is negligible.

### Directory enumeration (`readdir`, U-6e-b-1)

`devramfs_readdir` enumerates the flat root: every cpio file (QTFILE) plus the synthetic `srv`/`proc` mount-point dirs (QTDIR). It emits the Thylacine 9P2000.L dirent wire format the `SYS_READDIR` handler parses — `qid(13) + offset(8 LE) + type(1) + name_len(2 LE) + name` — with the `offset` field a 1-based ordinal RESUME COOKIE (the handler round-trips it via `c->offset`, so successive calls walk forward with no dup/skip). It emits whole entries only, stopping before one that won't fit; a regular-file Spoor returns -1 (not a directory); a synthetic dir is empty (0/EOD). If the FIRST entry of a run does not fit the caller's buffer it returns -1 (the getdents/EINVAL convention) rather than 0 — 0 would silently truncate the listing as a spurious EOD. The data it reads (`g_ramfs_files`, the synth-dir table) is immutable after init, so no lock is needed.

A plain `read()` on a directory qid still returns -1 (as on devproc + devctl) — directory bytes are obtained via `readdir`, not `read`.

The userspace consumer is `libthyla_rs::fs::read_dir` / `ReadDir` / `DirEntry` (it stages a 4 KiB buffer, far above any single entry, so it never trips the too-small case). Enumerating a *mounted* `/srv` (devsrv) or `/proc` (devproc) is a separate Dev-readdir feature (task #932) — those Devs do not yet implement `.readdir`.

### Writes are rejected (read-only fs)

Modifying a file in ramfs would only affect the in-memory copy; the persistent layer doesn't exist yet. Phase 5+ adds writable backing via Stratum.

### `pa_to_kva` for the initrd

Direct map covers the full physical memory at v1.0 (set up at P3-Bb). The initrd PA falls within this range; `pa_to_kva` returns a stable KVA. Phase 5+ may revisit if the direct map shrinks (e.g., to enforce a smaller working set per ARCH §6.4).

### CPIO parser does NOT validate `c_check`

Newc format reserves `c_check` as 8 ASCII hex digits; it's used by the **crc** variant (cpio newc-crc, magic "070702") to validate file content. v1.0 P4-E parses **only** newc ("070701") — checksum field is read but not checked. Future hardening might add crc support for tamper detection.

### Subdirectory paths in cpio are flattened

mkcpio.py iterates the source directory non-recursively; nested files are skipped. devramfs's parser would accept nested paths (e.g., `dir/file`) as a flat name — they'd appear as a single file named "dir/file" rather than a walked-into subdir. v1.0 P4-E doesn't exercise this; future chunks adding subdirectory layout extend the parser to split on `/` + build a tree.

---

## References

- `docs/ARCHITECTURE.md` §9.4 — /ramfs in the canonical /dev/ layout.
- `docs/ROADMAP.md` §6.1 — Phase 4 deliverables (dev/ramfs).
- `docs/reference/30-dev-spoor.md` — Dev vtable + Spoor lifecycle.
- `docs/reference/32-devproc.md` + `33-devctl.md` — directory-Dev pattern (predecessors).
- `mm/phys.c::phys_init` — reservation array (initrd is the 5th entry).
- `lib/dtb.c::dtb_get_chosen_initrd` — DTB probe.
- cpio(5) Linux man page — newc format spec.
