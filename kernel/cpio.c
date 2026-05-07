// CPIO newc parser — read-only iterator (P4-E).
//
// Per <thylacine/cpio.h>. Parses the cpio newc format Linux uses for
// initramfs. Header is 110 bytes of ASCII (6-byte magic + 13 8-byte
// hex fields); filename + data follow with 4-byte alignment.

#include <thylacine/cpio.h>
#include <thylacine/types.h>

// =============================================================================
// Header layout (per cpio(5) newc).
// =============================================================================

#define CPIO_NEWC_HDR_SIZE  110

// Per-field byte offsets within a 110-byte newc header. All fields
// are 8 ASCII hex digits.
#define OFF_MAGIC       0       // 6 bytes — "070701"
#define OFF_INO         6
#define OFF_MODE        14
#define OFF_UID         22
#define OFF_GID         30
#define OFF_NLINK       38
#define OFF_MTIME       46
#define OFF_FILESIZE    54
#define OFF_DEVMAJOR    62
#define OFF_DEVMINOR    70
#define OFF_RDEVMAJOR   78
#define OFF_RDEVMINOR   86
#define OFF_NAMESIZE    94
#define OFF_CHECK       102

// =============================================================================
// Parsing primitives.
// =============================================================================

// Parse 8 ASCII hex digits at p[0..8). Sets *out + returns true on
// clean parse. Returns false on any non-hex byte.
static bool parse_hex8(const u8 *p, u32 *out) {
    u32 v = 0;
    for (int i = 0; i < 8; i++) {
        u8 c = p[i];
        u32 d;
        if (c >= '0' && c <= '9')      d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
        else                            return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

static bool magic_eq(const u8 *p) {
    static const char m[] = CPIO_NEWC_MAGIC;     // "070701"
    for (int i = 0; i < 6; i++) {
        if (p[i] != (u8)m[i]) return false;
    }
    return true;
}

// Round x up to a 4-byte boundary.
static size_t align4(size_t x) {
    return (x + 3) & ~(size_t)3;
}

// Compare a NUL-terminated name in the blob to a literal s. Returns
// true iff equal (including the NUL).
static bool name_is(const char *name, const char *s) {
    int i = 0;
    while (s[i]) {
        if (name[i] != s[i]) return false;
        i++;
    }
    return name[i] == '\0';
}

bool cpio_newc_is_valid(const u8 *blob, size_t blob_size) {
    if (!blob || blob_size < CPIO_NEWC_HDR_SIZE) return false;
    return magic_eq(blob);
}

// =============================================================================
// Iteration.
// =============================================================================

int cpio_newc_iter(const u8 *blob, size_t blob_size,
                   int (*cb)(const struct cpio_entry *, void *), void *arg) {
    if (!blob || !cb) return -1;

    size_t off = 0;
    while (off + CPIO_NEWC_HDR_SIZE <= blob_size) {
        const u8 *hdr = blob + off;

        if (!magic_eq(hdr)) return -2;        // magic mismatch

        u32 mode, filesize, namesize;
        if (!parse_hex8(hdr + OFF_MODE,     &mode))     return -3;
        if (!parse_hex8(hdr + OFF_FILESIZE, &filesize)) return -3;
        if (!parse_hex8(hdr + OFF_NAMESIZE, &namesize)) return -3;

        // Name immediately follows the header.
        size_t name_off = off + CPIO_NEWC_HDR_SIZE;
        if (name_off + namesize > blob_size) return -4;     // truncated name
        if (namesize == 0)                   return -5;     // illegal: no NUL

        const char *name = (const char *)(blob + name_off);
        if (name[namesize - 1] != '\0') return -6;          // not NUL-terminated

        // Data starts after a 4-byte aligned (header+name) run, where
        // alignment is computed FROM THE START of the header.
        size_t data_off = align4(name_off + namesize);
        if (data_off + filesize > blob_size) return -7;     // truncated data

        // The trailer entry has name "TRAILER!!!" + filesize 0 — it
        // marks end-of-archive and is not reported to the callback.
        if (name_is(name, "TRAILER!!!")) return 0;

        struct cpio_entry e = {
            .name = name,
            .data = blob + data_off,
            .size = filesize,
            .mode = mode,
        };
        int rv = cb(&e, arg);
        if (rv != 0) return rv;

        // Advance to next entry. Data is also 4-byte aligned.
        off = align4(data_off + filesize);
    }

    // Ran off the end without finding a trailer — malformed.
    return -8;
}

// =============================================================================
// cpio_newc_count: convenience using cpio_newc_iter.
// =============================================================================

static int count_cb(const struct cpio_entry *e, void *arg) {
    (void)e;
    int *n = (int *)arg;
    (*n)++;
    return 0;
}

int cpio_newc_count(const u8 *blob, size_t blob_size) {
    int n = 0;
    int rv = cpio_newc_iter(blob, blob_size, count_cb, &n);
    if (rv < 0) return -1;
    return n;
}
