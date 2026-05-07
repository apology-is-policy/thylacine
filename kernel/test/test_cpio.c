// CPIO newc parser unit tests (P4-E).
//
// Tests run against synthetic in-memory cpio archives built byte-by-byte
// in the test code so the parser is exercised independent of any
// initrd presence.

#include "test.h"

#include <thylacine/cpio.h>
#include <thylacine/types.h>

void test_cpio_is_valid_recognizes_magic(void);
void test_cpio_iter_empty_archive(void);
void test_cpio_iter_single_entry(void);
void test_cpio_iter_two_entries(void);
void test_cpio_iter_rejects_truncated(void);
void test_cpio_iter_rejects_bad_magic(void);
void test_cpio_count_matches(void);

// =============================================================================
// Helpers — build a cpio newc entry into a buffer at a given offset.
// =============================================================================

static size_t emit_hex8(u8 *dst, u32 v) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) dst[i] = (u8)hex[(v >> ((7 - i) * 4)) & 0xF];
    return 8;
}

static size_t emit_hdr(u8 *dst, u32 mode, u32 namesize, u32 filesize) {
    static const char magic[] = "070701";
    for (int i = 0; i < 6; i++) dst[i] = (u8)magic[i];
    size_t off = 6;

    // 13 hex fields. Order matches CPIO_NEWC_HDR fields:
    // ino, mode, uid, gid, nlink, mtime, filesize, devmajor,
    // devminor, rdevmajor, rdevminor, namesize, check.
    off += emit_hex8(dst + off, 0);          // ino
    off += emit_hex8(dst + off, mode);       // mode
    off += emit_hex8(dst + off, 0);          // uid
    off += emit_hex8(dst + off, 0);          // gid
    off += emit_hex8(dst + off, 1);          // nlink
    off += emit_hex8(dst + off, 0);          // mtime
    off += emit_hex8(dst + off, filesize);   // filesize
    off += emit_hex8(dst + off, 0);          // devmajor
    off += emit_hex8(dst + off, 0);          // devminor
    off += emit_hex8(dst + off, 0);          // rdevmajor
    off += emit_hex8(dst + off, 0);          // rdevminor
    off += emit_hex8(dst + off, namesize);   // namesize
    off += emit_hex8(dst + off, 0);          // check
    return off;        // 110
}

// Append a name + 4-byte align padding starting at offset `off` in dst.
// `name` is a NUL-terminated literal; `namesize` is its full byte count
// including the trailing NUL.
static size_t emit_name(u8 *dst, size_t hdr_off, const char *name, size_t namesize) {
    size_t off = hdr_off + 110;
    for (size_t i = 0; i < namesize; i++) dst[off + i] = (u8)name[i];
    off += namesize;
    while ((off - hdr_off) & 3) dst[off++] = 0;
    return off;
}

static size_t emit_data(u8 *dst, size_t after_name_off, size_t hdr_off,
                        const u8 *data, size_t filesize) {
    (void)hdr_off;
    size_t off = after_name_off;
    for (size_t i = 0; i < filesize; i++) dst[off + i] = data[i];
    off += filesize;
    while (off & 3) dst[off++] = 0;
    return off;
}

// Emit a complete entry (header + name + data + alignment) starting at
// `start`. Returns the new write offset.
static size_t emit_entry(u8 *dst, size_t start,
                         const char *name, const u8 *data, size_t filesize,
                         u32 mode) {
    size_t namesize = 0;
    while (name[namesize]) namesize++;
    namesize++;        // include NUL

    emit_hdr(dst + start, mode, (u32)namesize, (u32)filesize);
    size_t after_name = emit_name(dst, start, name, namesize);
    size_t after_data = emit_data(dst, after_name, start, data, filesize);
    return after_data;
}

static size_t emit_trailer(u8 *dst, size_t start) {
    return emit_entry(dst, start, "TRAILER!!!", NULL, 0, 0);
}

// =============================================================================
// Iter callback that records names + data into a small fixture array.
// =============================================================================

#define MAX_FIXTURE_ENTRIES 4
struct fixture_state {
    int n;
    const char *names[MAX_FIXTURE_ENTRIES];
    const u8   *datas[MAX_FIXTURE_ENTRIES];
    size_t      sizes[MAX_FIXTURE_ENTRIES];
};

static int fixture_cb(const struct cpio_entry *e, void *arg) {
    struct fixture_state *s = (struct fixture_state *)arg;
    if (s->n >= MAX_FIXTURE_ENTRIES) return 1;
    s->names[s->n] = e->name;
    s->datas[s->n] = e->data;
    s->sizes[s->n] = e->size;
    s->n++;
    return 0;
}

// =============================================================================
// Tests.
// =============================================================================

void test_cpio_is_valid_recognizes_magic(void) {
    static u8 buf[256];
    for (int i = 0; i < 256; i++) buf[i] = 0;
    size_t off = emit_trailer(buf, 0);
    TEST_ASSERT(cpio_newc_is_valid(buf, off), "trailer-only archive is valid");

    // Garbled magic.
    buf[0] = 'X';
    TEST_ASSERT(!cpio_newc_is_valid(buf, off), "wrong magic rejected");

    // Tiny buffer.
    TEST_ASSERT(!cpio_newc_is_valid(buf, 5), "5-byte buffer rejected");
    TEST_ASSERT(!cpio_newc_is_valid(NULL, 0), "NULL blob rejected");
}

void test_cpio_iter_empty_archive(void) {
    static u8 buf[256];
    for (int i = 0; i < 256; i++) buf[i] = 0;
    size_t off = emit_trailer(buf, 0);

    struct fixture_state st = { 0 };
    int rv = cpio_newc_iter(buf, off, fixture_cb, &st);
    TEST_EXPECT_EQ(rv, 0, "trailer-only archive iterates cleanly");
    TEST_EXPECT_EQ(st.n, 0, "no entries reported");
}

void test_cpio_iter_single_entry(void) {
    static u8 buf[512];
    for (int i = 0; i < 512; i++) buf[i] = 0;
    static const u8 payload[] = "the quick brown fox\n";
    size_t off = emit_entry(buf, 0, "fox", payload,
                            sizeof(payload) - 1, 0100644);
    off = emit_trailer(buf, off);

    struct fixture_state st = { 0 };
    int rv = cpio_newc_iter(buf, off, fixture_cb, &st);
    TEST_EXPECT_EQ(rv, 0, "iter clean");
    TEST_EXPECT_EQ(st.n, 1, "one entry");
    TEST_ASSERT(st.names[0][0] == 'f' && st.names[0][1] == 'o' &&
                st.names[0][2] == 'x' && st.names[0][3] == 0,
                "name 'fox'");
    TEST_EXPECT_EQ(st.sizes[0], (size_t)(sizeof(payload) - 1),
                   "size matches payload");
    TEST_ASSERT(st.datas[0][0] == 't' && st.datas[0][1] == 'h' &&
                st.datas[0][2] == 'e',
                "data starts with 'the'");
}

void test_cpio_iter_two_entries(void) {
    static u8 buf[1024];
    for (int i = 0; i < 1024; i++) buf[i] = 0;
    static const u8 a[] = "alpha";
    static const u8 b[] = "beta!";
    size_t off = emit_entry(buf, 0, "a", a, 5, 0100644);
    off = emit_entry(buf, off, "bb", b, 5, 0100644);
    off = emit_trailer(buf, off);

    struct fixture_state st = { 0 };
    int rv = cpio_newc_iter(buf, off, fixture_cb, &st);
    TEST_EXPECT_EQ(rv, 0, "iter clean");
    TEST_EXPECT_EQ(st.n, 2, "two entries");
    TEST_ASSERT(st.names[0][0] == 'a' && st.names[0][1] == 0,  "first name 'a'");
    TEST_ASSERT(st.names[1][0] == 'b' && st.names[1][1] == 'b' &&
                st.names[1][2] == 0, "second name 'bb'");
}

void test_cpio_iter_rejects_truncated(void) {
    static u8 buf[256];
    for (int i = 0; i < 256; i++) buf[i] = 0;
    static const u8 a[] = "alpha";
    size_t off = emit_entry(buf, 0, "a", a, 5, 0100644);
    // No trailer; just truncate halfway through the entry.

    struct fixture_state st = { 0 };
    int rv = cpio_newc_iter(buf, off / 2, fixture_cb, &st);
    TEST_ASSERT(rv < 0, "truncated archive returns negative error");
}

void test_cpio_iter_rejects_bad_magic(void) {
    static u8 buf[256];
    for (int i = 0; i < 256; i++) buf[i] = 0;
    size_t off = emit_trailer(buf, 0);
    buf[0] = 'X';        // corrupt magic

    struct fixture_state st = { 0 };
    int rv = cpio_newc_iter(buf, off, fixture_cb, &st);
    TEST_ASSERT(rv < 0, "bad magic returns negative error");
}

void test_cpio_count_matches(void) {
    static u8 buf[1024];
    for (int i = 0; i < 1024; i++) buf[i] = 0;
    size_t off = 0;
    static const u8 d[] = "data";
    off = emit_entry(buf, off, "a", d, 4, 0100644);
    off = emit_entry(buf, off, "b", d, 4, 0100644);
    off = emit_entry(buf, off, "c", d, 4, 0100644);
    off = emit_trailer(buf, off);

    int n = cpio_newc_count(buf, off);
    TEST_EXPECT_EQ(n, 3, "count matches three entries");
}
