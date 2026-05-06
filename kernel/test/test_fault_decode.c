// P3-C: fault_info_decode unit tests.
//
// Pure decoder — takes (esr, far, elr) and produces a struct fault_info.
// No kernel state reads; trivially testable. These tests construct
// synthetic ESR values for representative fault classes and verify the
// decoder classifies them correctly.
//
// ESR_EL1 layout (per ARM ARM D17.2.40):
//   ESR[31:26] = EC (exception class).
//   ESR[24:0]  = ISS (instruction-specific syndrome).
//   ISS[5:0]   = DFSC/IFSC for data/instruction aborts.
//   ISS[9]     = WnR (data aborts only).

#include "test.h"

#include "../../arch/arm64/fault.h"

#include <thylacine/types.h>

void test_fault_decode_kernel_data_translation_l2(void);
void test_fault_decode_kernel_data_permission_write(void);
void test_fault_decode_user_data_translation(void);
void test_fault_decode_user_instruction_fetch(void);
void test_fault_decode_access_flag(void);

// Helper: construct an ESR_EL1 value from EC + ISS bits.
static inline u64 mk_esr(u32 ec, u32 iss) {
    return ((u64)ec << 26) | ((u64)iss & 0x1FFFFFFu);
}

// EC values from arch/arm64/fault.c (re-derived here so the test
// surface doesn't pull in private constants).
#define EC_INST_ABORT_LOWER 0x20u
#define EC_INST_ABORT_SAME  0x21u
#define EC_DATA_ABORT_LOWER 0x24u
#define EC_DATA_ABORT_SAME  0x25u

// ISS bit positions.
#define ISS_WNR_BIT   9

// Test 1: kernel-mode data abort, translation fault at level 2, read.
void test_fault_decode_kernel_data_translation_l2(void) {
    // ISS[5:0] = 0x06 (translation fault L2).
    // ISS[9]   = 0 (read).
    u64 esr = mk_esr(EC_DATA_ABORT_SAME, 0x06);
    struct fault_info fi;
    fault_info_decode(esr, 0xDEADBEEFul, 0xCAFEBABEul, &fi);

    TEST_EXPECT_EQ(fi.vaddr, 0xDEADBEEFul, "vaddr");
    TEST_EXPECT_EQ(fi.elr,   0xCAFEBABEul, "elr");
    TEST_EXPECT_EQ(fi.ec,    EC_DATA_ABORT_SAME, "ec");
    TEST_EXPECT_EQ(fi.fsc,   0x06u, "fsc");
    TEST_EXPECT_EQ(fi.fault_level, 2u, "fault_level == 2 (FSC[1:0] of 0x06)");

    TEST_ASSERT(!fi.from_user,       "data abort SAME → !from_user");
    TEST_ASSERT(!fi.is_instruction,  "data abort → !is_instruction");
    TEST_ASSERT(!fi.is_write,        "WnR=0 → read");
    TEST_ASSERT(fi.is_translation,   "FSC=0x06 → translation");
    TEST_ASSERT(!fi.is_permission,   "FSC=0x06 → !permission");
    TEST_ASSERT(!fi.is_access_flag,  "FSC=0x06 → !access_flag");
}

// Test 2: kernel-mode data abort, permission fault at level 3, write.
void test_fault_decode_kernel_data_permission_write(void) {
    // ISS[5:0] = 0x0F (permission fault L3).
    // ISS[9]   = 1 (write).
    u64 esr = mk_esr(EC_DATA_ABORT_SAME, (1u << ISS_WNR_BIT) | 0x0F);
    struct fault_info fi;
    fault_info_decode(esr, 0x40080000ul, 0xFFFF000040080000ul, &fi);

    TEST_EXPECT_EQ(fi.fsc, 0x0Fu, "fsc");
    TEST_EXPECT_EQ(fi.fault_level, 3u, "fault_level == 3 (FSC[1:0] of 0x0F)");
    TEST_ASSERT(!fi.from_user,       "current EL → !from_user");
    TEST_ASSERT(!fi.is_instruction,  "data abort → !is_instruction");
    TEST_ASSERT(fi.is_write,         "WnR=1 → write");
    TEST_ASSERT(!fi.is_translation,  "FSC=0x0F → !translation");
    TEST_ASSERT(fi.is_permission,    "FSC=0x0F → permission");
    TEST_ASSERT(!fi.is_access_flag,  "FSC=0x0F → !access_flag");
}

// Test 3: user-mode data abort (lower EL), translation fault L1.
void test_fault_decode_user_data_translation(void) {
    // ISS[5:0] = 0x05 (translation fault L1). WnR=0.
    u64 esr = mk_esr(EC_DATA_ABORT_LOWER, 0x05);
    struct fault_info fi;
    fault_info_decode(esr, 0x100000ul, 0x200000ul, &fi);

    TEST_ASSERT(fi.from_user,       "lower EL → from_user");
    TEST_ASSERT(!fi.is_instruction, "data abort → !is_instruction");
    TEST_ASSERT(!fi.is_write,       "WnR=0 → read");
    TEST_ASSERT(fi.is_translation,  "FSC=0x05 → translation");
    TEST_EXPECT_EQ(fi.fault_level, 1u, "fault_level == 1");
}

// Test 4: user-mode instruction abort (lower EL).
void test_fault_decode_user_instruction_fetch(void) {
    // Instruction aborts have no WnR; ISS[9] is RES0.
    u64 esr = mk_esr(EC_INST_ABORT_LOWER, 0x07);   // translation fault L3
    struct fault_info fi;
    fault_info_decode(esr, 0x300000ul, 0x300000ul, &fi);

    TEST_ASSERT(fi.from_user,        "lower EL → from_user");
    TEST_ASSERT(fi.is_instruction,   "instruction abort → is_instruction");
    TEST_ASSERT(!fi.is_write,        "instruction abort → never writes");
    TEST_ASSERT(fi.is_translation,   "FSC=0x07 → translation");
    TEST_ASSERT(!fi.is_permission,   "FSC=0x07 → !permission");
    TEST_EXPECT_EQ(fi.fault_level, 3u, "fault_level == 3");
}

// Test 5: access-flag fault classification.
void test_fault_decode_access_flag(void) {
    // ISS[5:0] = 0x0A (access flag L2).
    u64 esr = mk_esr(EC_DATA_ABORT_SAME, 0x0A);
    struct fault_info fi;
    fault_info_decode(esr, 0x400000ul, 0x400000ul, &fi);

    TEST_ASSERT(fi.is_access_flag,  "FSC=0x0A → access_flag");
    TEST_ASSERT(!fi.is_translation, "FSC=0x0A → !translation");
    TEST_ASSERT(!fi.is_permission,  "FSC=0x0A → !permission");
    TEST_EXPECT_EQ(fi.fault_level, 2u, "fault_level == 2");
}
