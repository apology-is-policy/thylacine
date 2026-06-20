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
void test_9p_wire_tflush_round_trip(void);
void test_9p_wire_tweft_round_trip(void);
void test_9p_wire_rlerror_parse(void);
void test_9p_wire_rmsg_size_mismatch_rejected(void);
void test_9p_wire_rmsg_wrong_type_rejected(void);
void test_9p_wire_rwalk_count_cap_enforced(void);
void test_9p_wire_pack_str_overflow(void);
void test_9p_wire_tlopen_round_trip(void);
void test_9p_wire_tlcreate_round_trip(void);
void test_9p_wire_tread_round_trip(void);
void test_9p_wire_twrite_round_trip(void);
void test_9p_wire_rread_data_cap_enforced(void);
void test_9p_wire_rread_size_mismatch_rejected(void);
void test_9p_wire_rlopen_vs_rlcreate_type_strict(void);
void test_9p_wire_tgetattr_round_trip(void);
void test_9p_wire_tsetattr_round_trip(void);
void test_9p_wire_treaddir_round_trip(void);
void test_9p_wire_dirent_unpack(void);
void test_9p_wire_tstatfs_round_trip(void);
void test_9p_wire_tfsync_round_trip(void);
void test_9p_wire_rreaddir_data_cap_enforced(void);
void test_9p_wire_tsymlink_round_trip(void);
void test_9p_wire_tmknod_round_trip(void);
void test_9p_wire_trename_round_trip(void);
void test_9p_wire_treadlink_round_trip(void);
void test_9p_wire_tlink_round_trip(void);
void test_9p_wire_tmkdir_round_trip(void);
void test_9p_wire_trenameat_round_trip(void);
void test_9p_wire_tunlinkat_round_trip(void);

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

// =============================================================================
// IO family (P5-wire-io): round-trips for Tlopen/Rlopen, Tlcreate/Rlcreate,
// Tread/Rread, Twrite/Rwrite.
// =============================================================================

void test_9p_wire_tlopen_round_trip(void) {
    int total = p9_build_tlopen(g_buf, sizeof(g_buf), 0x0010, 42, 0x8000u /* O_LARGEFILE */);
    // hdr(7) + fid(4) + flags(4) = 15
    TEST_EXPECT_EQ(total, 15, "Tlopen total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TLOPEN, "Tlopen type");

    // Synthesize Rlopen: header (size=24, type=P9_RLOPEN, tag=0x0010) +
    // qid(13) + iounit(4) = 17 bytes body.
    size_t r_total = P9_HDR_LEN + P9_QID_LEN + 4;            // 24
    g_buf[0] = (u8)(r_total & 0xff);
    g_buf[1] = (u8)((r_total >> 8) & 0xff);
    g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RLOPEN;
    g_buf[5] = 0x10; g_buf[6] = 0;
    struct p9_qid q = { .type = P9_QTFILE, .version = 7, .path = 0xDEADBEEFull };
    int rc = p9_pack_qid(g_buf + P9_HDR_LEN, sizeof(g_buf) - P9_HDR_LEN, &q);
    TEST_EXPECT_EQ(rc, (int)P9_QID_LEN, "synth Rlopen qid");
    rc = p9_pack_u32(g_buf + P9_HDR_LEN + P9_QID_LEN,
                     sizeof(g_buf) - P9_HDR_LEN - P9_QID_LEN, 4096u);
    TEST_EXPECT_EQ(rc, 4, "synth Rlopen iounit");

    u16 tag_out = 0;
    struct p9_qid q_out = { 0 };
    u32 iounit_out = 0;
    rc = p9_parse_rlopen(g_buf, r_total, &tag_out, &q_out, &iounit_out);
    TEST_EXPECT_EQ(rc, 0,                            "Rlopen parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0010,        "Rlopen tag");
    TEST_EXPECT_EQ((u64)q_out.type, (u64)P9_QTFILE,  "Rlopen qid.type");
    TEST_EXPECT_EQ((u64)q_out.version, (u64)7,       "Rlopen qid.version");
    TEST_EXPECT_EQ(q_out.path, (u64)0xDEADBEEFull,   "Rlopen qid.path");
    TEST_EXPECT_EQ((u64)iounit_out, (u64)4096,       "Rlopen iounit");
}

void test_9p_wire_tlcreate_round_trip(void) {
    const u8 name[] = {'h', 'e', 'l', 'l', 'o'};
    int total = p9_build_tlcreate(g_buf, sizeof(g_buf), 0x0011,
                                  /*fid*/      99,
                                  name, sizeof(name),
                                  /*flags*/    0x42u,         // O_CREAT|O_WRONLY
                                  /*mode*/     0644u,
                                  /*gid*/      1000u);
    // hdr(7) + fid(4) + name(2 + 5) + flags(4) + mode(4) + gid(4) = 30
    TEST_EXPECT_EQ(total, 30, "Tlcreate total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TLCREATE, "Tlcreate type");

    // Synthesize Rlcreate (same body shape as Rlopen).
    size_t r_total = P9_HDR_LEN + P9_QID_LEN + 4;            // 24
    g_buf[0] = (u8)(r_total & 0xff);
    g_buf[1] = (u8)((r_total >> 8) & 0xff);
    g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RLCREATE;
    g_buf[5] = 0x11; g_buf[6] = 0;
    struct p9_qid q = { .type = P9_QTFILE, .version = 0, .path = 12345ull };
    int rc = p9_pack_qid(g_buf + P9_HDR_LEN, sizeof(g_buf) - P9_HDR_LEN, &q);
    TEST_EXPECT_EQ(rc, (int)P9_QID_LEN, "synth Rlcreate qid");
    rc = p9_pack_u32(g_buf + P9_HDR_LEN + P9_QID_LEN,
                     sizeof(g_buf) - P9_HDR_LEN - P9_QID_LEN, 8192u);
    TEST_EXPECT_EQ(rc, 4, "synth Rlcreate iounit");

    u16 tag_out = 0;
    struct p9_qid q_out = { 0 };
    u32 iounit_out = 0;
    rc = p9_parse_rlcreate(g_buf, r_total, &tag_out, &q_out, &iounit_out);
    TEST_EXPECT_EQ(rc, 0,                            "Rlcreate parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0011,        "Rlcreate tag");
    TEST_EXPECT_EQ(q_out.path, (u64)12345ull,        "Rlcreate qid.path");
    TEST_EXPECT_EQ((u64)iounit_out, (u64)8192,       "Rlcreate iounit");
}

void test_9p_wire_tread_round_trip(void) {
    int total = p9_build_tread(g_buf, sizeof(g_buf), 0x0012, 0xABCDu,
                                /*offset*/ 0x100000000ull,
                                /*count*/  4096u);
    // hdr(7) + fid(4) + offset(8) + count(4) = 23
    TEST_EXPECT_EQ(total, 23, "Tread total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TREAD, "Tread type");

    // Synthesize Rread with a 4-byte data payload.
    const u8 payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    size_t r_total = P9_HDR_LEN + 4 + sizeof(payload);       // 7+4+4 = 15
    g_buf[0] = (u8)(r_total & 0xff);
    g_buf[1] = (u8)((r_total >> 8) & 0xff);
    g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RREAD;
    g_buf[5] = 0x12; g_buf[6] = 0;
    int rc = p9_pack_u32(g_buf + P9_HDR_LEN, sizeof(g_buf) - P9_HDR_LEN,
                          (u32)sizeof(payload));
    TEST_EXPECT_EQ(rc, 4, "synth Rread count");
    for (size_t i = 0; i < sizeof(payload); i++) {
        g_buf[P9_HDR_LEN + 4 + i] = payload[i];
    }

    u16 tag_out = 0;
    u32 count_out = 0;
    const u8 *data_ptr = NULL;
    rc = p9_parse_rread(g_buf, r_total, &tag_out, &count_out, &data_ptr, 8192u);
    TEST_EXPECT_EQ(rc, 0,                                "Rread parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0012,            "Rread tag");
    TEST_EXPECT_EQ((u64)count_out, (u64)sizeof(payload), "Rread count");
    TEST_ASSERT(data_ptr != NULL,                        "Rread data_ptr non-null");
    for (size_t i = 0; i < sizeof(payload); i++) {
        TEST_ASSERT(data_ptr[i] == payload[i],           "Rread data byte mismatch");
    }
}

void test_9p_wire_twrite_round_trip(void) {
    const u8 payload[] = {'h', 'e', 'l', 'l', 'o'};
    int total = p9_build_twrite(g_buf, sizeof(g_buf), 0x0013, 7,
                                 /*offset*/ 0,
                                 /*count*/  (u32)sizeof(payload),
                                 payload);
    // hdr(7) + fid(4) + offset(8) + count(4) + data(5) = 28
    TEST_EXPECT_EQ(total, 28, "Twrite total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TWRITE, "Twrite type");
    // Verify the payload landed at the expected offset.
    for (size_t i = 0; i < sizeof(payload); i++) {
        TEST_ASSERT(g_buf[P9_HDR_LEN + 4 + 8 + 4 + i] == payload[i],
                    "Twrite payload byte mismatch");
    }

    // Synthesize Rwrite (count-only body).
    size_t r_total = P9_HDR_LEN + 4;                          // 11
    g_buf[0] = (u8)(r_total & 0xff);
    g_buf[1] = (u8)((r_total >> 8) & 0xff);
    g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RWRITE;
    g_buf[5] = 0x13; g_buf[6] = 0;
    int rc = p9_pack_u32(g_buf + P9_HDR_LEN, sizeof(g_buf) - P9_HDR_LEN,
                          (u32)sizeof(payload));
    TEST_EXPECT_EQ(rc, 4, "synth Rwrite count");

    u16 tag_out = 0;
    u32 count_out = 0;
    rc = p9_parse_rwrite(g_buf, r_total, &tag_out, &count_out);
    TEST_EXPECT_EQ(rc, 0,                                "Rwrite parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0013,            "Rwrite tag");
    TEST_EXPECT_EQ((u64)count_out, (u64)sizeof(payload), "Rwrite count");
}

// =============================================================================
// R111 doctrine on Rread: server-claimed count exceeding caller-supplied
// data_cap must be refused before any data byte is exposed.
// =============================================================================

void test_9p_wire_rread_data_cap_enforced(void) {
    // Synthesize Rread claiming count=1024 in a frame that's only 11+1024
    // bytes. Pass data_cap=512. Parser must refuse.
    size_t r_total = P9_HDR_LEN + 4 + 1024;
    g_buf[0] = (u8)(r_total & 0xff);
    g_buf[1] = (u8)((r_total >> 8) & 0xff);
    g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RREAD;
    g_buf[5] = 0; g_buf[6] = 0;
    // count = 1024 (LE)
    g_buf[P9_HDR_LEN]     = 0x00;
    g_buf[P9_HDR_LEN + 1] = 0x04;
    g_buf[P9_HDR_LEN + 2] = 0x00;
    g_buf[P9_HDR_LEN + 3] = 0x00;
    // Data bytes don't matter — the cap check fires first.

    u16 tag_out = 0;
    u32 count_out = 0;
    const u8 *data_ptr = NULL;
    int rc = p9_parse_rread(g_buf, r_total, &tag_out, &count_out, &data_ptr, 512u);
    TEST_EXPECT_EQ(rc, -1, "Rread count > data_cap rejected (R111 caller-cap-bound)");
}

// =============================================================================
// Strict-equality on Rread: header.size != P9_HDR_LEN + 4 + count must be
// rejected. We craft an Rread claiming count=4 but in a frame of size
// 11+8 (extra trailing bytes).
// =============================================================================

void test_9p_wire_rread_size_mismatch_rejected(void) {
    size_t r_total = P9_HDR_LEN + 4 + 8;
    g_buf[0] = (u8)(r_total & 0xff);
    g_buf[1] = (u8)((r_total >> 8) & 0xff);
    g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RREAD;
    g_buf[5] = 0; g_buf[6] = 0;
    g_buf[P9_HDR_LEN]     = 0x04;       // count = 4
    g_buf[P9_HDR_LEN + 1] = 0x00;
    g_buf[P9_HDR_LEN + 2] = 0x00;
    g_buf[P9_HDR_LEN + 3] = 0x00;

    u16 tag_out = 0;
    u32 count_out = 0;
    const u8 *data_ptr = NULL;
    int rc = p9_parse_rread(g_buf, r_total, &tag_out, &count_out, &data_ptr, 8192u);
    TEST_EXPECT_EQ(rc, -1, "Rread header.size > 11 + count rejected");
}

// =============================================================================
// Rlopen vs Rlcreate type strict: feeding an Rlopen frame to parse_rlcreate
// must fail (and vice versa). Body shapes are identical but the types are
// strictly distinct.
// =============================================================================

void test_9p_wire_rlopen_vs_rlcreate_type_strict(void) {
    // Build a valid Rlopen frame.
    size_t r_total = P9_HDR_LEN + P9_QID_LEN + 4;
    g_buf[0] = (u8)(r_total & 0xff);
    g_buf[1] = (u8)((r_total >> 8) & 0xff);
    g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RLOPEN;
    g_buf[5] = 1; g_buf[6] = 0;
    struct p9_qid q = { .type = P9_QTFILE, .version = 0, .path = 0 };
    (void)p9_pack_qid(g_buf + P9_HDR_LEN, sizeof(g_buf) - P9_HDR_LEN, &q);
    (void)p9_pack_u32(g_buf + P9_HDR_LEN + P9_QID_LEN,
                       sizeof(g_buf) - P9_HDR_LEN - P9_QID_LEN, 0);

    u16 tag_out = 0;
    struct p9_qid q_out = { 0 };
    u32 iounit_out = 0;
    int rc = p9_parse_rlcreate(g_buf, r_total, &tag_out, &q_out, &iounit_out);
    TEST_EXPECT_EQ(rc, -1, "parse_rlcreate rejects Rlopen frame");
    // And vice versa: build an Rlcreate frame, feed to parse_rlopen.
    g_buf[4] = P9_RLCREATE;
    rc = p9_parse_rlopen(g_buf, r_total, &tag_out, &q_out, &iounit_out);
    TEST_EXPECT_EQ(rc, -1, "parse_rlopen rejects Rlcreate frame");
}

// =============================================================================
// Metadata family (P5-wire-meta): Tgetattr / Tsetattr / Treaddir / Tstatfs
// / Tfsync round-trips + dirent unpack + R111 caller-cap-bound.
// =============================================================================

void test_9p_wire_tgetattr_round_trip(void) {
    int total = p9_build_tgetattr(g_buf, sizeof(g_buf), 0x0020,
                                   /*fid*/ 7,
                                   /*request_mask*/ P9_GETATTR_BASIC);
    // hdr(7) + fid(4) + request_mask(8) = 19
    TEST_EXPECT_EQ(total, 19, "Tgetattr total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TGETATTR, "Tgetattr type");

    // Synthesize an Rgetattr with all-zero-but-distinguishable values.
    // Body: valid(8) + qid(13) + mode(4) + uid(4) + gid(4)
    //       + 5*u64(nlink/rdev/size/blksize/blocks) = 40
    //       + 4*(sec+nsec) = 8 u64 = 64
    //       + gen(8) + data_version(8)
    //   = 8 + 13 + 12 + 40 + 64 + 16 = 153 bytes.
    //   Equivalently: 8 (valid) + 13 (qid) + 12 (3 u32) + 15*8 (15 u64).
    size_t body_len = 8 + P9_QID_LEN + 4 * 3 + 8 * 15;
    size_t r_total = P9_HDR_LEN + body_len;
    g_buf[0] = (u8)(r_total & 0xff);
    g_buf[1] = (u8)((r_total >> 8) & 0xff);
    g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RGETATTR;
    g_buf[5] = 0x20; g_buf[6] = 0;
    size_t off = P9_HDR_LEN;
    int rc;
    rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off, P9_GETATTR_BASIC);
    TEST_EXPECT_EQ(rc, 8, "synth Rgetattr valid"); off += (size_t)rc;
    struct p9_qid q = { .type = P9_QTFILE, .version = 9, .path = 42 };
    rc = p9_pack_qid(g_buf + off, sizeof(g_buf) - off, &q);
    TEST_EXPECT_EQ(rc, (int)P9_QID_LEN, "synth Rgetattr qid"); off += (size_t)rc;
    // mode/uid/gid:
    rc = p9_pack_u32(g_buf + off, sizeof(g_buf) - off, 0644u);  TEST_EXPECT_EQ(rc, 4, "synth mode");  off += (size_t)rc;
    rc = p9_pack_u32(g_buf + off, sizeof(g_buf) - off, 1000u);  TEST_EXPECT_EQ(rc, 4, "synth uid");   off += (size_t)rc;
    rc = p9_pack_u32(g_buf + off, sizeof(g_buf) - off, 100u);   TEST_EXPECT_EQ(rc, 4, "synth gid");   off += (size_t)rc;
    // nlink/rdev/size/blksize/blocks:
    rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off, 1ull);       off += (size_t)rc;
    rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off, 0ull);       off += (size_t)rc;
    rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off, 12345ull);   off += (size_t)rc;
    rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off, 4096ull);    off += (size_t)rc;
    rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off, 24ull);      off += (size_t)rc;
    // 4 × (sec + nsec) — atime/mtime/ctime/btime:
    for (int i = 0; i < 8; i++) {
        rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off,
                          (u64)(0x100 + i));
        off += (size_t)rc;
    }
    // gen + data_version:
    rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off, 7ull);   off += (size_t)rc;
    rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off, 99ull);  off += (size_t)rc;
    TEST_EXPECT_EQ((u64)off, (u64)r_total, "synth Rgetattr body length");

    u16 tag_out = 0;
    struct p9_attr a = { 0 };
    rc = p9_parse_rgetattr(g_buf, r_total, &tag_out, &a);
    TEST_EXPECT_EQ(rc, 0,                          "Rgetattr parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0020,      "Rgetattr tag");
    TEST_EXPECT_EQ(a.valid, (u64)P9_GETATTR_BASIC, "Rgetattr valid");
    TEST_EXPECT_EQ(a.qid.path, (u64)42,            "Rgetattr qid.path");
    TEST_EXPECT_EQ((u64)a.mode, (u64)0644,         "Rgetattr mode");
    TEST_EXPECT_EQ((u64)a.uid, (u64)1000,          "Rgetattr uid");
    TEST_EXPECT_EQ(a.size, (u64)12345,             "Rgetattr size");
    TEST_EXPECT_EQ(a.blocks, (u64)24,              "Rgetattr blocks");
    TEST_EXPECT_EQ(a.atime_sec, (u64)0x100,        "Rgetattr atime_sec");
    TEST_EXPECT_EQ(a.btime_nsec, (u64)0x107,       "Rgetattr btime_nsec");
    TEST_EXPECT_EQ(a.gen, (u64)7,                  "Rgetattr gen");
    TEST_EXPECT_EQ(a.data_version, (u64)99,        "Rgetattr data_version");
}

void test_9p_wire_tsetattr_round_trip(void) {
    struct p9_setattr sa = {
        .valid       = P9_SETATTR_MODE | P9_SETATTR_SIZE,
        .mode        = 0755u,
        .uid         = 0,
        .gid         = 0,
        .size        = 4096ull,
        .atime_sec   = 0,
        .atime_nsec  = 0,
        .mtime_sec   = 0,
        .mtime_nsec  = 0,
    };
    int total = p9_build_tsetattr(g_buf, sizeof(g_buf), 0x0021,
                                   /*fid*/ 11, &sa);
    // hdr(7) + fid(4) + valid(4) + mode(4) + uid(4) + gid(4)
    //   + size(8) + 2 * 16 = 67
    TEST_EXPECT_EQ(total, 67, "Tsetattr total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TSETATTR, "Tsetattr type");

    // Synthesize Rsetattr (header-only, body empty).
    g_buf[0] = 7; g_buf[1] = 0; g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RSETATTR;
    g_buf[5] = 0x21; g_buf[6] = 0;
    u16 tag_out = 0;
    int rc = p9_parse_rsetattr(g_buf, 7, &tag_out);
    TEST_EXPECT_EQ(rc, 0,                     "Rsetattr parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0021, "Rsetattr tag");
}

void test_9p_wire_treaddir_round_trip(void) {
    int total = p9_build_treaddir(g_buf, sizeof(g_buf), 0x0022,
                                   /*fid*/ 5, /*offset*/ 0, /*count*/ 4096);
    // hdr(7) + fid(4) + offset(8) + count(4) = 23
    TEST_EXPECT_EQ(total, 23, "Treaddir total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TREADDIR, "Treaddir type");

    // Synthesize Rreaddir with a small dirent stream: "." + ".."
    // Dirent body: qid(13) + offset(8) + type(1) + name-str(2+len)
    // "." : 24 + 1 = 25 bytes; ".." : 24 + 2 = 26 bytes. Total: 51 bytes.
    u8 dirent_data[51];
    size_t dpos = 0;
    int rc;
    // Entry 1: "."
    struct p9_qid q0 = { .type = P9_QTDIR, .version = 0, .path = 100 };
    rc = p9_pack_qid(dirent_data + dpos, sizeof(dirent_data) - dpos, &q0); dpos += (size_t)rc;
    rc = p9_pack_u64(dirent_data + dpos, sizeof(dirent_data) - dpos, 1);   dpos += (size_t)rc;
    rc = p9_pack_u8 (dirent_data + dpos, sizeof(dirent_data) - dpos, 4);   dpos += (size_t)rc;  // DT_DIR
    rc = p9_pack_str(dirent_data + dpos, sizeof(dirent_data) - dpos,
                      (const u8 *)".", 1);
    dpos += (size_t)rc;
    // Entry 2: ".."
    struct p9_qid q1 = { .type = P9_QTDIR, .version = 0, .path = 101 };
    rc = p9_pack_qid(dirent_data + dpos, sizeof(dirent_data) - dpos, &q1); dpos += (size_t)rc;
    rc = p9_pack_u64(dirent_data + dpos, sizeof(dirent_data) - dpos, 2);   dpos += (size_t)rc;
    rc = p9_pack_u8 (dirent_data + dpos, sizeof(dirent_data) - dpos, 4);   dpos += (size_t)rc;
    rc = p9_pack_str(dirent_data + dpos, sizeof(dirent_data) - dpos,
                      (const u8 *)"..", 2);
    dpos += (size_t)rc;
    TEST_EXPECT_EQ((u64)dpos, (u64)51, "dirent stream size");

    size_t r_total = P9_HDR_LEN + 4 + dpos;
    g_buf[0] = (u8)(r_total & 0xff);
    g_buf[1] = (u8)((r_total >> 8) & 0xff);
    g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RREADDIR;
    g_buf[5] = 0x22; g_buf[6] = 0;
    rc = p9_pack_u32(g_buf + P9_HDR_LEN, sizeof(g_buf) - P9_HDR_LEN, (u32)dpos);
    TEST_EXPECT_EQ(rc, 4, "synth Rreaddir count");
    for (size_t i = 0; i < dpos; i++) g_buf[P9_HDR_LEN + 4 + i] = dirent_data[i];

    u16 tag_out = 0;
    u32 count_out = 0;
    const u8 *data_ptr = NULL;
    rc = p9_parse_rreaddir(g_buf, r_total, &tag_out, &count_out, &data_ptr, 8192u);
    TEST_EXPECT_EQ(rc, 0,                              "Rreaddir parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0022,          "Rreaddir tag");
    TEST_EXPECT_EQ((u64)count_out, (u64)dpos,          "Rreaddir count");
    TEST_ASSERT(data_ptr != NULL,                      "Rreaddir data non-null");
    // Spot-check first byte is qid.type (P9_QTDIR = 0x80).
    TEST_EXPECT_EQ((u64)data_ptr[0], (u64)P9_QTDIR,    "Rreaddir first byte = qid.type");
}

void test_9p_wire_dirent_unpack(void) {
    // Hand-construct one dirent (qid + offset + type + name).
    u8 buf[64];
    size_t pos = 0;
    int rc;
    struct p9_qid q = { .type = P9_QTFILE, .version = 0, .path = 500 };
    rc = p9_pack_qid(buf + pos, sizeof(buf) - pos, &q); pos += (size_t)rc;
    rc = p9_pack_u64(buf + pos, sizeof(buf) - pos, 12345ull); pos += (size_t)rc;
    rc = p9_pack_u8 (buf + pos, sizeof(buf) - pos, 8);  pos += (size_t)rc;  // DT_REG
    rc = p9_pack_str(buf + pos, sizeof(buf) - pos, (const u8 *)"hello.txt", 9);
    pos += (size_t)rc;
    // Expected size: 13 + 8 + 1 + 2 + 9 = 33

    struct p9_qid q_out = { 0 };
    u64 off_out = 0;
    u8 type_out = 0;
    const u8 *name_ptr = NULL;
    u16 name_len = 0;
    rc = p9_unpack_dirent(buf, pos, &q_out, &off_out, &type_out,
                           &name_ptr, &name_len);
    TEST_EXPECT_EQ(rc, 33,                           "dirent total len");
    TEST_EXPECT_EQ(q_out.path, (u64)500,             "dirent qid.path");
    TEST_EXPECT_EQ(off_out, (u64)12345,              "dirent offset");
    TEST_EXPECT_EQ((u64)type_out, (u64)8,            "dirent type = DT_REG");
    TEST_EXPECT_EQ((u64)name_len, (u64)9,            "dirent name len");
    TEST_ASSERT(name_ptr != NULL,                    "dirent name non-null");
    for (size_t i = 0; i < 9; i++) {
        TEST_ASSERT(name_ptr[i] == (u8)"hello.txt"[i], "dirent name byte");
    }
}

void test_9p_wire_tstatfs_round_trip(void) {
    int total = p9_build_tstatfs(g_buf, sizeof(g_buf), 0x0023, /*fid*/ 0);
    // hdr(7) + fid(4) = 11
    TEST_EXPECT_EQ(total, 11, "Tstatfs total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TSTATFS, "Tstatfs type");

    // Synthesize Rstatfs: type(4) + bsize(4) + 6×u64(48) + namelen(4) = 60 B
    size_t body_len = 4 + 4 + 6 * 8 + 4;
    size_t r_total = P9_HDR_LEN + body_len;
    g_buf[0] = (u8)(r_total & 0xff);
    g_buf[1] = (u8)((r_total >> 8) & 0xff);
    g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RSTATFS;
    g_buf[5] = 0x23; g_buf[6] = 0;
    size_t off = P9_HDR_LEN;
    int rc;
    rc = p9_pack_u32(g_buf + off, sizeof(g_buf) - off, 0x53544D31u); off += (size_t)rc;  // "STM1"
    rc = p9_pack_u32(g_buf + off, sizeof(g_buf) - off, 4096u);       off += (size_t)rc;
    rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off, 1024ull);     off += (size_t)rc;
    rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off, 512ull);      off += (size_t)rc;
    rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off, 256ull);      off += (size_t)rc;
    rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off, 64ull);       off += (size_t)rc;
    rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off, 16ull);       off += (size_t)rc;
    rc = p9_pack_u64(g_buf + off, sizeof(g_buf) - off, 0xCAFEBABEull); off += (size_t)rc;
    rc = p9_pack_u32(g_buf + off, sizeof(g_buf) - off, 255u);        off += (size_t)rc;

    u16 tag_out = 0;
    struct p9_statfs sf = { 0 };
    rc = p9_parse_rstatfs(g_buf, r_total, &tag_out, &sf);
    TEST_EXPECT_EQ(rc, 0,                              "Rstatfs parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0023,          "Rstatfs tag");
    TEST_EXPECT_EQ((u64)sf.type, (u64)0x53544D31u,     "Rstatfs type");
    TEST_EXPECT_EQ((u64)sf.bsize, (u64)4096,           "Rstatfs bsize");
    TEST_EXPECT_EQ(sf.blocks, (u64)1024,               "Rstatfs blocks");
    TEST_EXPECT_EQ(sf.bavail, (u64)256,                "Rstatfs bavail");
    TEST_EXPECT_EQ(sf.fsid, (u64)0xCAFEBABEull,        "Rstatfs fsid");
    TEST_EXPECT_EQ((u64)sf.namelen, (u64)255,          "Rstatfs namelen");
}

void test_9p_wire_tfsync_round_trip(void) {
    int total = p9_build_tfsync(g_buf, sizeof(g_buf), 0x0024,
                                 /*fid*/ 1, /*datasync*/ 1);
    // hdr(7) + fid(4) + datasync(4) = 15
    TEST_EXPECT_EQ(total, 15, "Tfsync total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TFSYNC, "Tfsync type");

    // Synthesize Rfsync (header-only).
    g_buf[0] = 7; g_buf[1] = 0; g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RFSYNC;
    g_buf[5] = 0x24; g_buf[6] = 0;
    u16 tag_out = 0;
    int rc = p9_parse_rfsync(g_buf, 7, &tag_out);
    TEST_EXPECT_EQ(rc, 0,                     "Rfsync parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0024, "Rfsync tag");
}

void test_9p_wire_rreaddir_data_cap_enforced(void) {
    // Same shape attack as Rread: server claims count > caller's data_cap.
    size_t r_total = P9_HDR_LEN + 4 + 1024;
    g_buf[0] = (u8)(r_total & 0xff);
    g_buf[1] = (u8)((r_total >> 8) & 0xff);
    g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RREADDIR;
    g_buf[5] = 0; g_buf[6] = 0;
    g_buf[P9_HDR_LEN]     = 0x00;
    g_buf[P9_HDR_LEN + 1] = 0x04;
    g_buf[P9_HDR_LEN + 2] = 0x00;
    g_buf[P9_HDR_LEN + 3] = 0x00;

    u16 tag_out = 0;
    u32 count_out = 0;
    const u8 *data_ptr = NULL;
    int rc = p9_parse_rreaddir(g_buf, r_total, &tag_out, &count_out, &data_ptr, 512u);
    TEST_EXPECT_EQ(rc, -1, "Rreaddir count > data_cap rejected (R111)");
}

// =============================================================================
// Mutation family (P5-wire-mutation): round-trips for each of the 8 ops.
// =============================================================================

// Helper: synthesize a header-only response (Rrename / Rrenameat / Rlink /
// Runlinkat). Writes a 7-byte frame.
static void synth_empty_r(u8 *buf, u8 type, u16 tag) {
    buf[0] = 7; buf[1] = 0; buf[2] = 0; buf[3] = 0;
    buf[4] = type;
    buf[5] = (u8)(tag & 0xff);
    buf[6] = (u8)((tag >> 8) & 0xff);
}

// Helper: synthesize a qid-only response (Rsymlink / Rmknod / Rmkdir).
static void synth_qid_r(u8 *buf, u8 type, u16 tag,
                         u8 qtype, u32 qver, u64 qpath) {
    size_t total = P9_HDR_LEN + P9_QID_LEN;
    buf[0] = (u8)(total & 0xff);
    buf[1] = 0; buf[2] = 0; buf[3] = 0;
    buf[4] = type;
    buf[5] = (u8)(tag & 0xff);
    buf[6] = (u8)((tag >> 8) & 0xff);
    buf[7] = qtype;
    for (int i = 0; i < 4; i++) buf[8 + i] = (u8)((qver >> (i * 8)) & 0xff);
    for (int i = 0; i < 8; i++) buf[12 + i] = (u8)((qpath >> (i * 8)) & 0xff);
}

void test_9p_wire_tsymlink_round_trip(void) {
    const u8 name[]   = {'l', 'i', 'n', 'k'};
    const u8 symtgt[] = {'.', '.', '/', 't', 'g', 't'};
    int total = p9_build_tsymlink(g_buf, sizeof(g_buf), 0x0030,
                                   /*fid*/ 5,
                                   name, sizeof(name),
                                   symtgt, sizeof(symtgt),
                                   /*gid*/ 1000);
    // hdr(7) + fid(4) + name(2+4) + symtgt(2+6) + gid(4) = 29
    TEST_EXPECT_EQ(total, 29, "Tsymlink total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TSYMLINK, "Tsymlink type");

    synth_qid_r(g_buf, P9_RSYMLINK, 0x0030, P9_QTSYMLINK, 0, 444);
    u16 tag_out = 0;
    struct p9_qid q = { 0 };
    int rc = p9_parse_rsymlink(g_buf, P9_HDR_LEN + P9_QID_LEN, &tag_out, &q);
    TEST_EXPECT_EQ(rc, 0,                            "Rsymlink parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0030,        "Rsymlink tag");
    TEST_EXPECT_EQ((u64)q.type, (u64)P9_QTSYMLINK,   "Rsymlink qid.type");
    TEST_EXPECT_EQ(q.path, (u64)444,                 "Rsymlink qid.path");
}

void test_9p_wire_tmknod_round_trip(void) {
    const u8 name[] = {'n', 'u', 'l', 'l'};
    int total = p9_build_tmknod(g_buf, sizeof(g_buf), 0x0031,
                                 /*dfid*/ 5,
                                 name, sizeof(name),
                                 /*mode*/  020666u,    // S_IFCHR | 0666
                                 /*major*/ 1, /*minor*/ 3,
                                 /*gid*/   0);
    // hdr(7) + dfid(4) + name(2+4) + mode(4) + major(4) + minor(4) + gid(4) = 33
    TEST_EXPECT_EQ(total, 33, "Tmknod total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TMKNOD, "Tmknod type");

    synth_qid_r(g_buf, P9_RMKNOD, 0x0031, P9_QTFILE, 0, 555);
    u16 tag_out = 0;
    struct p9_qid q = { 0 };
    int rc = p9_parse_rmknod(g_buf, P9_HDR_LEN + P9_QID_LEN, &tag_out, &q);
    TEST_EXPECT_EQ(rc, 0,                          "Rmknod parse ok");
    TEST_EXPECT_EQ(q.path, (u64)555,               "Rmknod qid.path");
}

void test_9p_wire_trename_round_trip(void) {
    const u8 name[] = {'n', 'e', 'w'};
    int total = p9_build_trename(g_buf, sizeof(g_buf), 0x0032,
                                  /*fid*/ 7, /*dfid*/ 5,
                                  name, sizeof(name));
    // hdr(7) + fid(4) + dfid(4) + name(2+3) = 20
    TEST_EXPECT_EQ(total, 20, "Trename total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TRENAME, "Trename type");

    synth_empty_r(g_buf, P9_RRENAME, 0x0032);
    u16 tag_out = 0;
    int rc = p9_parse_rrename(g_buf, 7, &tag_out);
    TEST_EXPECT_EQ(rc, 0,                          "Rrename parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0032,      "Rrename tag");
}

void test_9p_wire_treadlink_round_trip(void) {
    int total = p9_build_treadlink(g_buf, sizeof(g_buf), 0x0033, /*fid*/ 9);
    // hdr(7) + fid(4) = 11
    TEST_EXPECT_EQ(total, 11, "Treadlink total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TREADLINK, "Treadlink type");

    // Synthesize Rreadlink: header + str("/etc/hostname") = 7 + 2 + 13 = 22.
    const u8 tgt[] = {'/', 'e', 't', 'c', '/', 'h', 'o', 's', 't', 'n', 'a', 'm', 'e'};
    size_t r_total = P9_HDR_LEN + 2 + sizeof(tgt);
    g_buf[0] = (u8)(r_total & 0xff);
    g_buf[1] = 0; g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RREADLINK;
    g_buf[5] = 0x33; g_buf[6] = 0;
    int rc = p9_pack_str(g_buf + P9_HDR_LEN, sizeof(g_buf) - P9_HDR_LEN, tgt, sizeof(tgt));
    TEST_EXPECT_EQ(rc, 2 + (int)sizeof(tgt), "synth Rreadlink target");

    u16 tag_out = 0;
    const u8 *target_ptr = NULL;
    u16 target_len = 0;
    rc = p9_parse_rreadlink(g_buf, r_total, &tag_out, &target_ptr, &target_len);
    TEST_EXPECT_EQ(rc, 0,                                    "Rreadlink parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0033,                "Rreadlink tag");
    TEST_EXPECT_EQ((u64)target_len, (u64)sizeof(tgt),        "Rreadlink target len");
    TEST_ASSERT(target_ptr != NULL,                          "Rreadlink target non-null");
    for (size_t i = 0; i < sizeof(tgt); i++) {
        TEST_ASSERT(target_ptr[i] == tgt[i], "Rreadlink target byte mismatch");
    }
}

void test_9p_wire_tlink_round_trip(void) {
    const u8 name[] = {'a', 'l', 'i', 'a', 's'};
    int total = p9_build_tlink(g_buf, sizeof(g_buf), 0x0034,
                                /*dfid*/ 5, /*fid*/ 11,
                                name, sizeof(name));
    // hdr(7) + dfid(4) + fid(4) + name(2+5) = 22
    TEST_EXPECT_EQ(total, 22, "Tlink total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TLINK, "Tlink type");

    synth_empty_r(g_buf, P9_RLINK, 0x0034);
    u16 tag_out = 0;
    int rc = p9_parse_rlink(g_buf, 7, &tag_out);
    TEST_EXPECT_EQ(rc, 0,                          "Rlink parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0034,      "Rlink tag");
}

void test_9p_wire_tmkdir_round_trip(void) {
    const u8 name[] = {'s', 'u', 'b'};
    int total = p9_build_tmkdir(g_buf, sizeof(g_buf), 0x0035,
                                 /*dfid*/ 5,
                                 name, sizeof(name),
                                 /*mode*/ 0755, /*gid*/ 0);
    // hdr(7) + dfid(4) + name(2+3) + mode(4) + gid(4) = 24
    TEST_EXPECT_EQ(total, 24, "Tmkdir total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TMKDIR, "Tmkdir type");

    synth_qid_r(g_buf, P9_RMKDIR, 0x0035, P9_QTDIR, 0, 666);
    u16 tag_out = 0;
    struct p9_qid q = { 0 };
    int rc = p9_parse_rmkdir(g_buf, P9_HDR_LEN + P9_QID_LEN, &tag_out, &q);
    TEST_EXPECT_EQ(rc, 0,                          "Rmkdir parse ok");
    TEST_EXPECT_EQ((u64)q.type, (u64)P9_QTDIR,     "Rmkdir qid.type");
    TEST_EXPECT_EQ(q.path, (u64)666,               "Rmkdir qid.path");
}

void test_9p_wire_trenameat_round_trip(void) {
    const u8 oldn[] = {'o', 'l', 'd'};
    const u8 newn[] = {'n', 'e', 'w'};
    int total = p9_build_trenameat(g_buf, sizeof(g_buf), 0x0036,
                                    /*olddirfid*/ 5,
                                    oldn, sizeof(oldn),
                                    /*newdirfid*/ 6,
                                    newn, sizeof(newn));
    // hdr(7) + olddirfid(4) + oldname(2+3) + newdirfid(4) + newname(2+3) = 25
    TEST_EXPECT_EQ(total, 25, "Trenameat total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TRENAMEAT, "Trenameat type");

    synth_empty_r(g_buf, P9_RRENAMEAT, 0x0036);
    u16 tag_out = 0;
    int rc = p9_parse_rrenameat(g_buf, 7, &tag_out);
    TEST_EXPECT_EQ(rc, 0,                          "Rrenameat parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0036,      "Rrenameat tag");
}

void test_9p_wire_tunlinkat_round_trip(void) {
    const u8 name[] = {'r', 'm'};
    int total = p9_build_tunlinkat(g_buf, sizeof(g_buf), 0x0037,
                                    /*dfid*/ 5,
                                    name, sizeof(name),
                                    /*flags*/ 0);
    // hdr(7) + dfid(4) + name(2+2) + flags(4) = 19
    TEST_EXPECT_EQ(total, 19, "Tunlinkat total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TUNLINKAT, "Tunlinkat type");

    synth_empty_r(g_buf, P9_RUNLINKAT, 0x0037);
    u16 tag_out = 0;
    int rc = p9_parse_runlinkat(g_buf, 7, &tag_out);
    TEST_EXPECT_EQ(rc, 0,                          "Runlinkat parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0037,      "Runlinkat tag");

    // Verify P9_UNLINK_AT_REMOVEDIR flag encodes correctly: build with
    // the flag and inspect the body's flags field offset (7+4+2+2=15).
    total = p9_build_tunlinkat(g_buf, sizeof(g_buf), 0x0038,
                                5, name, sizeof(name),
                                P9_UNLINK_AT_REMOVEDIR);
    TEST_EXPECT_EQ((u64)g_buf[15], (u64)0x00, "flags byte 0");
    TEST_EXPECT_EQ((u64)g_buf[16], (u64)0x02, "flags byte 1 = 0x200 high half");
}

// Tflush build (body = [oldtag:u16]) + Rflush parse (header only). #845.
void test_9p_wire_tflush_round_trip(void) {
    // hdr(7) + oldtag(2) = 9. tag=7, oldtag=3.
    int total = p9_build_tflush(g_buf, sizeof(g_buf), 0x0007, 0x0003);
    TEST_EXPECT_EQ(total, 9,                       "Tflush total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TFLUSH,  "Tflush type");
    TEST_EXPECT_EQ((u64)g_buf[5], (u64)0x07,       "Tflush tag LE low");
    TEST_EXPECT_EQ((u64)g_buf[7], (u64)0x03,       "Tflush oldtag LE low");
    TEST_EXPECT_EQ((u64)g_buf[8], (u64)0x00,       "Tflush oldtag LE high");

    // Synthesize Rflush: header-only (size=7, type=P9_RFLUSH, tag=7).
    g_buf[0] = 7; g_buf[1] = 0; g_buf[2] = 0; g_buf[3] = 0;
    g_buf[4] = P9_RFLUSH;
    g_buf[5] = 0x07; g_buf[6] = 0;
    u16 tag_out = 0;
    int rc = p9_parse_rflush(g_buf, 7, &tag_out);
    TEST_EXPECT_EQ(rc, 0,                        "Rflush parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0007,    "Rflush tag round-trip");

    // An Rflush carrying a body is rejected (header-only invariant).
    g_buf[0] = 8;                  // claim size 8, pass len 8 (1 trailing byte)
    rc = p9_parse_rflush(g_buf, 8, &tag_out);
    TEST_EXPECT_EQ(rc, -1,                       "Rflush with body rejected");

    // Wrong type rejected.
    g_buf[0] = 7; g_buf[4] = P9_RCLUNK;
    rc = p9_parse_rflush(g_buf, 7, &tag_out);
    TEST_EXPECT_EQ(rc, -1,                       "Rflush wrong-type rejected");
}

// Tweft build (body = [fid:u32]) + Rweft (body = [share_id:u64][ring_size:u32]
// [ring_entries:u32]) round-trip + the parse-side validators. Weft-6.
void test_9p_wire_tweft_round_trip(void) {
    // Tweft: hdr(7) + fid(4) = 11. tag=0x0010, fid=0x12345678.
    int total = p9_build_tweft(g_buf, sizeof(g_buf), 0x0010, 0x12345678u);
    TEST_EXPECT_EQ(total, 11,                       "Tweft total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_TWEFT,    "Tweft type");
    TEST_EXPECT_EQ((u64)g_buf[5], (u64)0x10,        "Tweft tag LE low");
    TEST_EXPECT_EQ((u64)g_buf[7], (u64)0x78,        "Tweft fid LE byte0");
    TEST_EXPECT_EQ((u64)g_buf[10], (u64)0x12,       "Tweft fid LE byte3");

    u16 tag_out = 0; u32 fid_out = 0;
    int rc = p9_parse_tweft(g_buf, (size_t)total, &tag_out, &fid_out);
    TEST_EXPECT_EQ(rc, 0,                           "Tweft parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0010,       "Tweft tag round-trip");
    TEST_EXPECT_EQ((u64)fid_out, (u64)0x12345678u,  "Tweft fid round-trip");

    // Rweft: hdr(7) + share_id(8) + ring_size(4) + ring_entries(4) = 23.
    struct p9_weft_geom g = {
        .share_id = 0x1122334455667788ull,
        .ring_size = 0x00010000u,           // 64 KiB
        .ring_entries = 256u,
    };
    total = p9_build_rweft(g_buf, sizeof(g_buf), 0x0010, &g);
    TEST_EXPECT_EQ(total, 23,                        "Rweft total len");
    TEST_EXPECT_EQ((u64)g_buf[4], (u64)P9_RWEFT,     "Rweft type");

    struct p9_weft_geom go;
    go.share_id = 0; go.ring_size = 0; go.ring_entries = 0;
    tag_out = 0;
    rc = p9_parse_rweft(g_buf, (size_t)total, &tag_out, &go);
    TEST_EXPECT_EQ(rc, 0,                              "Rweft parse ok");
    TEST_EXPECT_EQ((u64)tag_out, (u64)0x0010,          "Rweft tag round-trip");
    TEST_EXPECT_EQ((u64)go.share_id, (u64)0x1122334455667788ull, "Rweft share_id round-trip");
    TEST_EXPECT_EQ((u64)go.ring_size, (u64)0x00010000u, "Rweft ring_size round-trip");
    TEST_EXPECT_EQ((u64)go.ring_entries, (u64)256u,    "Rweft ring_entries round-trip");

    // Truncated Rweft body rejected (header claims 23, deliver 22).
    rc = p9_parse_rweft(g_buf, 22, &tag_out, &go);
    TEST_EXPECT_EQ(rc, -1,                           "Rweft truncated rejected");

    // size/len mismatch rejected (header claims 23, deliver 24).
    rc = p9_parse_rweft(g_buf, 24, &tag_out, &go);
    TEST_EXPECT_EQ(rc, -1,                           "Rweft oversize rejected");

    // Wrong type rejected.
    g_buf[4] = P9_RCLUNK;
    rc = p9_parse_rweft(g_buf, 23, &tag_out, &go);
    TEST_EXPECT_EQ(rc, -1,                           "Rweft wrong-type rejected");

    // NULL-arg guards.
    TEST_EXPECT_EQ(p9_build_rweft(g_buf, sizeof(g_buf), 0, (struct p9_weft_geom *)0), -1,
                   "Rweft build NULL geom");
    TEST_EXPECT_EQ(p9_parse_rweft(g_buf, 23, (u16 *)0, &go), -1,
                   "Rweft parse NULL tag");
}
