// 9P2000.L wire codec unit tests (P5-wire).
//
// Round-trips each supported message family + exercises every
// rejection path (truncated, oversize, wrong type, malformed body).
//
// The codec under test is purely byte-level — no kernel state, no
// allocation, no I/O. These tests run in the test_*.c framework as
// stand-alone byte-buffer round-trips.

#include "test.h"

#include <thylacine/9p_wire.h>
#include <thylacine/types.h>

void test_9p_wire_primitives_round_trip(void);
void test_9p_wire_primitives_overflow(void);
void test_9p_wire_str_round_trip(void);
void test_9p_wire_qid_round_trip(void);
void test_9p_wire_header_peek(void);
void test_9p_wire_tversion_round_trip(void);
void test_9p_wire_tattach_round_trip(void);
void test_9p_wire_twalk_round_trip(void);
void test_9p_wire_twalk_zero_names_clone(void);
void test_9p_wire_tclunk_round_trip(void);
void test_9p_wire_rlerror_parse(void);
void test_9p_wire_rmsg_size_mismatch_rejected(void);
void test_9p_wire_rmsg_wrong_type_rejected(void);
void test_9p_wire_rwalk_count_cap_enforced(void);
void test_9p_wire_pack_str_overflow(void);

// 4 KiB scratch buffer; every message we build at v1.0 fits in this
// (Tattach with full-length uname+aname stays under 600 bytes;
// Twalk with 16 names of 255 bytes each tops out at ~4128 bytes — just
// over 4 KiB, so we use 8 KiB to be safe).
static u8 g_buf[8192];

// =============================================================================
// Primitive round-trips.
// =============================================================================

void test_9p_wire_primitives_round_trip(void) {
    int rc;
    u8  v8  = 0;
    u16 v16 = 0;
    u32 v32 = 0;
    u64 v64 = 0;

    rc = p9_pack_u8 (g_buf, sizeof(g_buf), 0xA5);                     TEST_EXPECT_EQ(rc, 1, "pack u8 ok");
    rc = p9_unpack_u8(g_buf, sizeof(g_buf), &v8);                     TEST_EXPECT_EQ(rc, 1, "unpack u8 ok");
    TEST_EXPECT_EQ((u64)v8, (u64)0xA5, "u8 round-trip");

    rc = p9_pack_u16(g_buf, sizeof(g_buf), 0xBEEF);                   TEST_EXPECT_EQ(rc, 2, "pack u16 ok");
    rc = p9_unpack_u16(g_buf, sizeof(g_buf), &v16);                   TEST_EXPECT_EQ(rc, 2, "unpack u16 ok");
    TEST_EXPECT_EQ((u64)v16, (u64)0xBEEF, "u16 round-trip");
    // LE encoding: low byte first.
    TEST_EXPECT_EQ((u64)g_buf[0], (u64)0xEF, "u16 LE low byte");
    TEST_EXPECT_EQ((u64)g_buf[1], (u64)0xBE, "u16 LE high byte");

    rc = p9_pack_u32(g_buf, sizeof(g_buf), 0xDEADBEEFu);              TEST_EXPECT_EQ(rc, 4, "pack u32 ok");
    rc = p9_unpack_u32(g_buf, sizeof(g_buf), &v32);                   TEST_EXPECT_EQ(rc, 4, "unpack u32 ok");
    TEST_EXPECT_EQ((u64)v32, (u64)0xDEADBEEFull, "u32 round-trip");

    rc = p9_pack_u64(g_buf, sizeof(g_buf), 0x1122334455667788ull);    TEST_EXPECT_EQ(rc, 8, "pack u64 ok");
    rc = p9_unpack_u64(g_buf, sizeof(g_buf), &v64);                   TEST_EXPECT_EQ(rc, 8, "unpack u64 ok");
    TEST_EXPECT_EQ(v64, (u64)0x1122334455667788ull, "u64 round-trip");
    // LE encoding spot-check: bottom byte = 0x88, top byte = 0x11.
    TEST_EXPECT_EQ((u64)g_buf[0], (u64)0x88, "u64 LE low byte");
    TEST_EXPECT_EQ((u64)g_buf[7], (u64)0x11, "u64 LE high byte");
}

void test_9p_wire_primitives_overflow(void) {
    int rc;
    u8 small[1];
    rc = p9_pack_u16(small, 1, 0);  TEST_EXPECT_EQ(rc, -1, "pack u16 underflow caught");
    rc = p9_pack_u32(small, 1, 0);  TEST_EXPECT_EQ(rc, -1, "pack u32 underflow caught");
    rc = p9_pack_u64(small, 1, 0);  TEST_EXPECT_EQ(rc, -1, "pack u64 underflow caught");

    u8  v8 = 0;
    u16 v16 = 0;
    rc = p9_unpack_u8 (small, 0, &v8);   TEST_EXPECT_EQ(rc, -1, "unpack u8 underflow caught");
    rc = p9_unpack_u16(small, 1, &v16);  TEST_EXPECT_EQ(rc, -1, "unpack u16 underflow caught");
}

// =============================================================================
// 9P-string round-trip.
// =============================================================================

void test_9p_wire_str_round_trip(void) {
    int rc;
    const u8 src[] = {'9', 'P', '2', '0', '0', '0', '.', 'L'};
    const size_t src_len = sizeof(src);

    rc = p9_pack_str(g_buf, sizeof(g_buf), src, src_len);
    TEST_EXPECT_EQ(rc, (int)(2 + src_len), "pack 9P-string ok");
    // Length prefix bytes:
    TEST_EXPECT_EQ((u64)g_buf[0], (u64)src_len, "9P-string len byte 0");
    TEST_EXPECT_EQ((u64)g_buf[1], (u64)0,       "9P-string len byte 1 (high)");

    const u8 *out = NULL;
    u16 out_len = 0;
    rc = p9_unpack_str(g_buf, sizeof(g_buf), &out, &out_len);
    TEST_EXPECT_EQ(rc, (int)(2 + src_len), "unpack 9P-string ok");
    TEST_EXPECT_EQ((u64)out_len, (u64)src_len, "9P-string length round-trip");
    // Spot-check byte-equal:
    for (size_t i = 0; i < src_len; i++) {
        TEST_ASSERT(out[i] == src[i], "9P-string byte mismatch");
    }

    // Empty string: pack 2 bytes (length prefix only); unpack returns
    // out_ptr = NULL (since slen == 0 maps to no body bytes).
    rc = p9_pack_str(g_buf, sizeof(g_buf), NULL, 0);
    TEST_EXPECT_EQ(rc, 2, "pack empty 9P-string");
    rc = p9_unpack_str(g_buf, sizeof(g_buf), &out, &out_len);
    TEST_EXPECT_EQ(rc, 2, "unpack empty 9P-string");
    TEST_EXPECT_EQ((u64)out_len, (u64)0, "empty 9P-string len");
    TEST_ASSERT(out == NULL, "empty 9P-string out_ptr == NULL");
}

void test_9p_wire_pack_str_overflow(void) {
    int rc;
    // Cap < 2 (length prefix doesn't fit).
    rc = p9_pack_str(g_buf, 1, (const u8 *)"x", 1);
    TEST_EXPECT_EQ(rc, -1, "pack str cap=1 rejected");
    // Cap < 2 + slen (body doesn't fit).
    rc = p9_pack_str(g_buf, 2, (const u8 *)"xx", 2);
    TEST_EXPECT_EQ(rc, -1, "pack str cap=2 body=2 rejected");
}

// =============================================================================
// QID round-trip.
// =============================================================================

void test_9p_wire_qid_round_trip(void) {
    struct p9_qid q_in  = { .type = P9_QTDIR, .version = 0xCAFEBABEu, .path = 0x123456789ABCDEF0ull };
    struct p9_qid q_out = { 0 };

    int rc = p9_pack_qid(g_buf, sizeof(g_buf), &q_in);
    TEST_EXPECT_EQ(rc, (int)P9_QID_LEN, "pack qid ok");
    rc = p9_unpack_qid(g_buf, sizeof(g_buf), &q_out);
    TEST_EXPECT_EQ(rc, (int)P9_QID_LEN, "unpack qid ok");
    TEST_EXPECT_EQ((u64)q_out.type, (u64)P9_QTDIR,    "qid.type round-trip");
    TEST_EXPECT_EQ((u64)q_out.version, (u64)0xCAFEBABEull, "qid.version round-trip");
    TEST_EXPECT_EQ(q_out.path, (u64)0x123456789ABCDEF0ull, "qid.path round-trip");
}

// =============================================================================
// Header peek.
// =============================================================================

void test_9p_wire_header_peek(void) {
    // Manually craft a header: size=0x12 (18), type=P9_RVERSION, tag=0xABCD.
    g_buf[0] = 0x12; g_buf[1] = 0; g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RVERSION;
    g_buf[5] = 0xCD; g_buf[6] = 0xAB;

    u32 size = 0;
    u8  type = 0;
    u16 tag  = 0;
    int rc = p9_peek_header(g_buf, sizeof(g_buf), &size, &type, &tag);
    TEST_EXPECT_EQ(rc, 0, "peek header ok");
    TEST_EXPECT_EQ((u64)size, (u64)0x12,        "peek size");
    TEST_EXPECT_EQ((u64)type, (u64)P9_RVERSION, "peek type");
    TEST_EXPECT_EQ((u64)tag,  (u64)0xABCD,      "peek tag");

    rc = p9_peek_header(g_buf, 6, &size, &type, &tag);
    TEST_EXPECT_EQ(rc, -1, "peek header truncated rejected");
}

// =============================================================================
// Tversion round-trip: build Tversion, then "parse" by manually flipping the
// type to P9_RVERSION (the wire shape is identical) and re-parsing.
// =============================================================================

void test_9p_wire_tversion_round_trip(void) {
    const u8 ver[] = {'9', 'P', '2', '0', '0', '0', '.', 'L'};
    const size_t ver_len = sizeof(ver);

    int total = p9_build_tversion(g_buf, sizeof(g_buf), P9_NOTAG, 8192, ver, ver_len);
    // Expected: header(7) + msize(4) + str(2 + ver_len) = 13 + ver_len.
    TEST_EXPECT_EQ(total, (int)(13 + ver_len), "Tversion total len");

    // Header field check.
    u32 sz; u8 ty; u16 tg;
    int rc = p9_peek_header(g_buf, (size_t)total, &sz, &ty, &tg);
    TEST_EXPECT_EQ(rc, 0,                          "Tversion header peek ok");
    TEST_EXPECT_EQ((u64)sz, (u64)total,            "Tversion size = total");
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TVERSION,      "Tversion type");
    TEST_EXPECT_EQ((u64)tg, (u64)P9_NOTAG,         "Tversion tag = NOTAG");

    // Flip type to RVERSION (Rversion has identical body shape) and parse.
    g_buf[4] = P9_RVERSION;
    u16 tag_out = 0;
    u32 msize_out = 0;
    const u8 *ver_ptr = NULL;
    u16 ver_len_out = 0;
    rc = p9_parse_rversion(g_buf, (size_t)total, &tag_out, &msize_out, &ver_ptr, &ver_len_out);
    TEST_EXPECT_EQ(rc, 0,                              "Rversion parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)P9_NOTAG,        "Rversion tag round-trip");
    TEST_EXPECT_EQ((u64)msize_out, (u64)8192,          "Rversion msize round-trip");
    TEST_EXPECT_EQ((u64)ver_len_out, (u64)ver_len,     "Rversion version length");
    for (size_t i = 0; i < ver_len; i++) {
        TEST_ASSERT(ver_ptr[i] == ver[i], "Rversion version byte mismatch");
    }
}

// =============================================================================
// Tattach + Rattach round-trip.
// =============================================================================

void test_9p_wire_tattach_round_trip(void) {
    const u8 uname[] = {'r', 'o', 'o', 't'};
    const u8 aname[] = {'/'};

    int total = p9_build_tattach(g_buf, sizeof(g_buf), 0x0001,
                                 /*fid*/    0x1234ABCDu,
                                 /*afid*/   P9_NOFID,
                                 uname, sizeof(uname),
                                 aname, sizeof(aname),
                                 /*n_uname*/ 0);
    // Expected: hdr(7) + fid(4) + afid(4) + uname(2+4) + aname(2+1) + n_uname(4) = 28
    TEST_EXPECT_EQ(total, 28, "Tattach total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TATTACH, "Tattach header.type");

    // Synthesize an Rattach: replace type with P9_RATTACH, replace body
    // with a single qid (13 bytes), update size.
    g_buf[0] = (u8)(7 + 13);
    g_buf[1] = 0; g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RATTACH;
    // tag is g_buf[5..7] = 0x0001 already.
    struct p9_qid q_in = { .type = P9_QTDIR, .version = 1, .path = 42 };
    int rc = p9_pack_qid(g_buf + P9_HDR_LEN, sizeof(g_buf) - P9_HDR_LEN, &q_in);
    TEST_EXPECT_EQ(rc, (int)P9_QID_LEN, "synthesize Rattach qid");

    u16 tag_out = 0;
    struct p9_qid q_out = { 0 };
    rc = p9_parse_rattach(g_buf, P9_HDR_LEN + P9_QID_LEN, &tag_out, &q_out);
    TEST_EXPECT_EQ(rc, 0,                       "Rattach parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0001,   "Rattach tag round-trip");
    TEST_EXPECT_EQ((u64)q_out.type, (u64)P9_QTDIR,  "Rattach qid.type");
    TEST_EXPECT_EQ((u64)q_out.version, (u64)1,      "Rattach qid.version");
    TEST_EXPECT_EQ(q_out.path, (u64)42,             "Rattach qid.path");
}

// =============================================================================
// Twalk + Rwalk round-trip with 2 path components.
// =============================================================================

void test_9p_wire_twalk_round_trip(void) {
    const u8 n0[] = {'e', 't', 'c'};
    const u8 n1[] = {'h', 'o', 's', 't', 'n', 'a', 'm', 'e'};
    const u8 *names[2]      = { n0, n1 };
    const size_t name_lens[2] = { sizeof(n0), sizeof(n1) };

    int total = p9_build_twalk(g_buf, sizeof(g_buf), 0x0002,
                               /*fid*/    1,
                               /*newfid*/ 2,
                               /*nwname*/ 2,
                               names, name_lens);
    // hdr(7) + fid(4) + newfid(4) + nwname(2) + str(2+3) + str(2+8) = 32
    TEST_EXPECT_EQ(total, 32, "Twalk total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TWALK, "Twalk header.type");

    // Synthesize Rwalk with 2 qids.
    struct p9_qid q0 = { .type = P9_QTDIR,  .version = 1, .path = 100 };
    struct p9_qid q1 = { .type = P9_QTFILE, .version = 2, .path = 200 };

    size_t body_total = 2 + 2 * P9_QID_LEN;             // nwqid(2) + 2 qids
    size_t r_total    = P9_HDR_LEN + body_total;         // 35
    g_buf[0] = (u8)(r_total & 0xff);
    g_buf[1] = (u8)((r_total >> 8) & 0xff);
    g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RWALK;
    // tag g_buf[5..7] = 0x0002 already set.
    int rc = p9_pack_u16(g_buf + P9_HDR_LEN, sizeof(g_buf) - P9_HDR_LEN, 2);
    TEST_EXPECT_EQ(rc, 2, "synth Rwalk nwqid");
    rc = p9_pack_qid(g_buf + P9_HDR_LEN + 2, sizeof(g_buf) - P9_HDR_LEN - 2, &q0);
    TEST_EXPECT_EQ(rc, (int)P9_QID_LEN, "synth Rwalk qid0");
    rc = p9_pack_qid(g_buf + P9_HDR_LEN + 2 + P9_QID_LEN,
                     sizeof(g_buf) - P9_HDR_LEN - 2 - P9_QID_LEN, &q1);
    TEST_EXPECT_EQ(rc, (int)P9_QID_LEN, "synth Rwalk qid1");

    u16 tag_out = 0;
    u16 nwqid_out = 0;
    struct p9_qid qids_out[P9_MAX_WALK] = { 0 };
    rc = p9_parse_rwalk(g_buf, r_total, &tag_out, &nwqid_out, qids_out, P9_MAX_WALK);
    TEST_EXPECT_EQ(rc, 0,                            "Rwalk parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0002,        "Rwalk tag round-trip");
    TEST_EXPECT_EQ((u64)nwqid_out, (u64)2,           "Rwalk nwqid round-trip");
    TEST_EXPECT_EQ((u64)qids_out[0].path, (u64)100,  "Rwalk qid0.path");
    TEST_EXPECT_EQ((u64)qids_out[1].path, (u64)200,  "Rwalk qid1.path");
}

// Walk with nwname=0 (fid clone), no names array passed.
void test_9p_wire_twalk_zero_names_clone(void) {
    int total = p9_build_twalk(g_buf, sizeof(g_buf), 0x0003, 7, 8, 0, NULL, NULL);
    // hdr(7) + fid(4) + newfid(4) + nwname(2) = 17
    TEST_EXPECT_EQ(total, 17, "Twalk clone total len");
}

// =============================================================================
// Tclunk + Rclunk round-trip.
// =============================================================================

void test_9p_wire_tclunk_round_trip(void) {
    int total = p9_build_tclunk(g_buf, sizeof(g_buf), 0x0004, 42);
    // hdr(7) + fid(4) = 11
    TEST_EXPECT_EQ(total, 11, "Tclunk total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TCLUNK, "Tclunk type");

    // Synthesize Rclunk: header-only (size=7, type=P9_RCLUNK).
    g_buf[0] = 7; g_buf[1] = 0; g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RCLUNK;
    g_buf[5] = 0x04; g_buf[6] = 0;

    u16 tag_out = 0;
    int rc = p9_parse_rclunk(g_buf, 7, &tag_out);
    TEST_EXPECT_EQ(rc, 0,                        "Rclunk parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0004,    "Rclunk tag round-trip");
}

// =============================================================================
// Rlerror parse.
// =============================================================================

void test_9p_wire_rlerror_parse(void) {
    // Hand-construct Rlerror: size=11, type=7, tag=0x0005, ecode=2 (ENOENT).
    g_buf[0]  = 11; g_buf[1]  = 0; g_buf[2]  = 0; g_buf[3]  = 0;
    g_buf[4]  = P9_RLERROR;
    g_buf[5]  = 0x05; g_buf[6]  = 0;
    g_buf[7]  = 2;   g_buf[8]  = 0; g_buf[9]  = 0; g_buf[10] = 0;

    u16 tag_out  = 0;
    u32 ecode    = 0;
    int rc = p9_parse_rlerror(g_buf, 11, &tag_out, &ecode);
    TEST_EXPECT_EQ(rc, 0,                       "Rlerror parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0005,   "Rlerror tag");
    TEST_EXPECT_EQ((u64)ecode, (u64)2,          "Rlerror ecode = ENOENT");
}

// =============================================================================
// Rejection: header.size != actual frame length.
// =============================================================================

void test_9p_wire_rmsg_size_mismatch_rejected(void) {
    // Build a real Rclunk (size=7) but pass len=8 to the parser.
    g_buf[0] = 7; g_buf[1] = 0; g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RCLUNK;
    g_buf[5] = 0; g_buf[6] = 0;

    u16 tag_out = 0;
    int rc = p9_parse_rclunk(g_buf, 8, &tag_out);
    TEST_EXPECT_EQ(rc, -1, "Rclunk size != len rejected");

    // And len=6 (truncated header).
    rc = p9_parse_rclunk(g_buf, 6, &tag_out);
    TEST_EXPECT_EQ(rc, -1, "Rclunk truncated header rejected");
}

// =============================================================================
// Rejection: header.type doesn't match the parser's expectation.
// =============================================================================

void test_9p_wire_rmsg_wrong_type_rejected(void) {
    // Build a Tclunk; call parse_rclunk (expects R, not T).
    int total = p9_build_tclunk(g_buf, sizeof(g_buf), 1, 0);
    TEST_EXPECT_EQ(total, 11, "build Tclunk for wrong-type test");
    u16 tag_out = 0;
    int rc = p9_parse_rclunk(g_buf, (size_t)total, &tag_out);
    TEST_EXPECT_EQ(rc, -1, "parse_rclunk rejects Tclunk type");
}

// =============================================================================
// R111-doctrine: Rwalk's server-supplied nwqid bounded against caller's cap.
// A malformed Rwalk that claims nwqid > qid_cap must be rejected BEFORE
// any qid is written into the caller's buffer.
// =============================================================================

void test_9p_wire_rwalk_count_cap_enforced(void) {
    // Synthesize an Rwalk with nwqid=5 but pass qid_cap=2.
    size_t r_total = P9_HDR_LEN + 2 + 5 * P9_QID_LEN;       // 7 + 2 + 65 = 74
    g_buf[0] = (u8)(r_total & 0xff);
    g_buf[1] = (u8)((r_total >> 8) & 0xff);
    g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RWALK;
    g_buf[5] = 0; g_buf[6] = 0;
    g_buf[P9_HDR_LEN]     = 5;     // nwqid low byte
    g_buf[P9_HDR_LEN + 1] = 0;
    // Body bytes (5 qids worth) don't need to be valid — the bound check
    // fires before any qid is unpacked.

    u16 tag_out = 0;
    u16 nwqid_out = 0;
    struct p9_qid qids[2] = { 0 };
    int rc = p9_parse_rwalk(g_buf, r_total, &tag_out, &nwqid_out, qids, 2);
    TEST_EXPECT_EQ(rc, -1, "Rwalk nwqid > qid_cap rejected (R111 caller-cap-bound)");
}
