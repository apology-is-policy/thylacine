// CPIO newc archive iterator (P4-E).
//
// Per ROADMAP §6.1 + the Linux initramfs convention. The newc format
// is the canonical cpio variant for Linux-style initramfs:
//   - 6-byte ASCII magic "070701"
//   - 13 × 8-byte ASCII hex header fields (110 bytes total)
//   - filename (NUL-terminated, c_namesize bytes incl. NUL)
//   - pad to 4-byte boundary after the header+name run
//   - file data (c_filesize bytes)
//   - pad to 4-byte boundary after the data run
//   - last entry has filename "TRAILER!!!" + filesize 0; this marks
//     end-of-archive.
//
// At v1.0 P4-E this iterator is read-only — devramfs uses it to
// populate a static file table at boot. No write side. Per ARCH §15
// the cpio data lives in the initrd-loaded memory range; the iterator
// reads from it via direct-map KVA.

#ifndef THYLACINE_CPIO_H
#define THYLACINE_CPIO_H

#include <thylacine/types.h>

// Newc header magic. Tested against the first 6 bytes of every entry.
#define CPIO_NEWC_MAGIC "070701"

// Per-entry callback view. Pointers into the input blob; valid only
// for the lifetime of the cpio_iter call (or as long as the input
// blob lives, in practice).
struct cpio_entry {
    const char *name;        // NUL-terminated; lives in the input blob
    const u8   *data;        // file content; lives in the input blob
    size_t      size;        // file content size in bytes
    u32         mode;        // c_mode field (e.g., 0100644 for regular)
};

// Iterate every non-trailer entry in the cpio newc archive at
// [blob, blob+blob_size). Calls cb(entry, arg) per entry; if cb
// returns non-zero, iteration stops + returns that value. After
// the trailer entry is reached, iteration stops + returns 0.
//
// Returns:
//   0   — iteration completed normally (every entry consumed,
//         trailer reached, OR cb returned 0 for every entry).
//   <0  — parse error (magic mismatch, hex parse failed, blob
//         truncated mid-entry, or filename overruns blob).
//   >0  — first non-zero callback return; iteration stopped here.
//
// The iterator does NOT recurse subdirectories or interpret modes —
// it reports every entry in archive order.
int cpio_newc_iter(const u8 *blob, size_t blob_size,
                   int (*cb)(const struct cpio_entry *, void *), void *arg);

// Convenience: count entries in the archive (excluding the trailer).
// Returns -1 on parse error.
int cpio_newc_count(const u8 *blob, size_t blob_size);

// Quick presence test — does the blob start with the newc magic?
bool cpio_newc_is_valid(const u8 *blob, size_t blob_size);

#endif  // THYLACINE_CPIO_H
