// ARM64 hardware feature detection — reads ID_AA64* registers.
//
// The ARM Architecture Reference Manual (DDI 0487, current rev L)
// defines per-feature 4-bit fields in the ID_AA64* registers:
// 0b0000 = "feature not present", non-zero = "feature present at the
// indicated revision". We only care about presence (zero / non-zero)
// for the hardening-relevant features.
//
// ID_AA64ISAR0_EL1 layout (relevant fields):
//   bits 23:20  Atomic   — FEAT_LSE
//   bits 19:16  CRC32    — FEAT_CRC32
//
// ID_AA64ISAR1_EL1 layout:
//   bits  3:0   DPB      — Data Persistence Barrier (FEAT_DPB / DPB2)
//   bits  7:4   APA      — FEAT_PAuth (QARMA, APIA key)
//   bits 11:8   API      — FEAT_PAuth (IMPDEF, APIA key)
//   bits 27:24  GPA      — FEAT_PAuth (PACGA, QARMA)
//   bits 31:28  GPI      — FEAT_PAuth (PACGA, IMPDEF)
//
// ID_AA64PFR1_EL1 layout:
//   bits  3:0   BT       — FEAT_BTI (0 = none, 1 = present)
//   bits 11:8   MTE      — FEAT_MTE: 0 none, 1 inst-only, 2 +tags, 3 +async
//
// Per ARM ARM D17.2.x. Linux's arch/arm64/include/asm/cpufeature.h is
// the reference for the bit layouts.

#include "hwfeat.h"
#include "hwdebug.h"

#include <thylacine/types.h>

struct hw_features g_hw_features;

static inline u64 read_id_aa64isar0_el1(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, id_aa64isar0_el1" : "=r"(v));
    return v;
}
static inline u64 read_id_aa64isar1_el1(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, id_aa64isar1_el1" : "=r"(v));
    return v;
}
static inline u64 read_id_aa64pfr1_el1(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, id_aa64pfr1_el1" : "=r"(v));
    return v;
}
static inline u64 read_id_aa64pfr0_el1(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, id_aa64pfr0_el1" : "=r"(v));
    return v;
}

#define FIELD_GET(val, shift, mask) (((val) >> (shift)) & (mask))

// Linux arm64 uapi hwcap bit numbers (arch/arm64/include/uapi/asm/hwcap.h).
// Only the bits Thylacine derives are listed; the published word is
// append-only (a consumer must treat unknown-clear as feature-absent).
#define THWCAP_FP       (1ull << 0)
#define THWCAP_ASIMD    (1ull << 1)
#define THWCAP_AES      (1ull << 3)
#define THWCAP_PMULL    (1ull << 4)
#define THWCAP_SHA1     (1ull << 5)
#define THWCAP_SHA2     (1ull << 6)
#define THWCAP_CRC32    (1ull << 7)
#define THWCAP_ATOMICS  (1ull << 8)
#define THWCAP_SHA3     (1ull << 17)
#define THWCAP_ASIMDDP  (1ull << 20)
#define THWCAP_SHA512   (1ull << 21)

void hw_features_detect(void) {
    u64 isar0 = read_id_aa64isar0_el1();
    u64 isar1 = read_id_aa64isar1_el1();
    u64 pfr1  = read_id_aa64pfr1_el1();
    u64 pfr0  = read_id_aa64pfr0_el1();

    // Atomic (23:20): 0b0010 = FEAT_LSE, 0b0011 = FEAT_LSE128; 0b0001 is
    // RESERVED, so the gate is >= 2 (Linux's cpufeature threshold) -- a
    // future 0b0001 assignment would denote something LESS than full LSE,
    // and this bool feeds both the W1.5 alternatives patcher and the EL0
    // AT_HWCAP ATOMICS bit (audit F4).
    g_hw_features.atomic   = FIELD_GET(isar0, 20, 0xF) >= 2;
    g_hw_features.crc32    = FIELD_GET(isar0, 16, 0xF) != 0;

    g_hw_features.pac_apa  = FIELD_GET(isar1,  4, 0xF) != 0;
    g_hw_features.pac_api  = FIELD_GET(isar1,  8, 0xF) != 0;
    g_hw_features.pac_gpa  = FIELD_GET(isar1, 24, 0xF) != 0;
    g_hw_features.pac_gpi  = FIELD_GET(isar1, 28, 0xF) != 0;

    g_hw_features.bti      = FIELD_GET(pfr1,  0, 0xF) != 0;
    g_hw_features.mte      = (u8)FIELD_GET(pfr1,  8, 0xF);

    // The Linux-compatible AT_HWCAP word (hwfeat.h). Field semantics per
    // ARM ARM D17.2.x:
    //   ISAR0.AES  (7:4):   1 = AESE/AESD/AESMC/AESIMC, 2 = + PMULL
    //   ISAR0.SHA1 (11:8):  1 = SHA1 instructions
    //   ISAR0.SHA2 (15:12): 1 = SHA256, 2 = + SHA512
    //   ISAR0.SHA3 (35:32): 1 = SHA3 instructions
    //   ISAR0.DP   (47:44): 1 = ASIMD dot product
    //   PFR0.FP (19:16) / PFR0.AdvSIMD (23:20): 0b0000 = implemented,
    //     0b1111 = NOT implemented — inverted-sentinel fields, unlike
    //     the zero-means-absent ISAR0 fields above.
    u64 hc = 0;
    if (FIELD_GET(pfr0, 16, 0xF) != 0xF) hc |= THWCAP_FP;
    if (FIELD_GET(pfr0, 20, 0xF) != 0xF) hc |= THWCAP_ASIMD;
    u64 aes = FIELD_GET(isar0, 4, 0xF);
    if (aes >= 1) hc |= THWCAP_AES;
    if (aes >= 2) hc |= THWCAP_PMULL;
    if (FIELD_GET(isar0,  8, 0xF) != 0) hc |= THWCAP_SHA1;
    u64 sha2 = FIELD_GET(isar0, 12, 0xF);
    if (sha2 >= 1) hc |= THWCAP_SHA2;
    if (sha2 >= 2) hc |= THWCAP_SHA512;
    if (FIELD_GET(isar0, 32, 0xF) != 0) hc |= THWCAP_SHA3;
    if (FIELD_GET(isar0, 44, 0xF) != 0) hc |= THWCAP_ASIMDDP;
    if (g_hw_features.crc32)  hc |= THWCAP_CRC32;
    if (g_hw_features.atomic) hc |= THWCAP_ATOMICS;
    g_hw_features.linux_hwcap = hc;

    // Go IDE Stage 8a-2: the hardware breakpoint / watchpoint counts (DFR0).
    hwdebug_enumerate();
}

// Append a literal string to a buffer; returns chars written.
// Reserves the last byte for a caller-written NUL — `pos + 1 < cap`
// (P1-H audit F19). Without this, a full-buffer truncation would
// clobber the last character with NUL in the caller's safety branch.
static unsigned append_str(char *buf, unsigned cap, unsigned pos,
                           const char *s) {
    while (*s && pos + 1 < cap) {
        buf[pos++] = *s++;
    }
    return pos;
}

unsigned hw_features_describe(char *buf, unsigned cap) {
    if (cap == 0) return 0;
    unsigned pos = 0;
    bool first = true;

    // Track whether to emit a leading comma.
    #define EMIT(name) do { \
        if (!first) pos = append_str(buf, cap, pos, ","); \
        pos = append_str(buf, cap, pos, (name)); \
        first = false; \
    } while (0)

    if (g_hw_features.pac_apa || g_hw_features.pac_api) EMIT("PAC");
    if (g_hw_features.bti)                              EMIT("BTI");
    if (g_hw_features.mte > 0) {
        if (g_hw_features.mte == 1)      EMIT("MTE1");
        else if (g_hw_features.mte == 2) EMIT("MTE2");
        else                              EMIT("MTE3");
    }
    if (g_hw_features.atomic) EMIT("LSE");
    if (g_hw_features.crc32)  EMIT("CRC32");

    if (first) {
        // No features detected.
        pos = append_str(buf, cap, pos, "(none)");
    }

    if (pos < cap) buf[pos] = 0;
    else if (cap > 0) buf[cap - 1] = 0;
    #undef EMIT
    return pos;
}
