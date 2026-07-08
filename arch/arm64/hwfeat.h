// ARM64 hardware feature detection (P1-H).
//
// Reads ID_AA64ISAR0_EL1, ID_AA64ISAR1_EL1, ID_AA64PFR1_EL1 to
// populate a struct that records which hardening-relevant features
// the running CPU supports. Accessed at boot for the banner; future
// callers (Phase 2 atomics, virtio drivers) consult the same struct.
//
// We don't trap-on-missing-feature for the hardening flags: PAC /
// BTI / MTE / LSE are all "set up unconditionally; hardware ignores
// the SCTLR / instruction-emission on cores that don't implement
// them". The struct is a *report*, not a *gate* — it tells boot_main
// what the banner should claim.
//
// Per ARCHITECTURE.md §24.3.

#ifndef THYLACINE_ARCH_ARM64_HWFEAT_H
#define THYLACINE_ARCH_ARM64_HWFEAT_H

#include <thylacine/types.h>

struct hw_features {
    // ID_AA64ISAR0_EL1
    bool atomic;     // FEAT_LSE — LSE atomic ops (LDADD, CAS, ...)
    bool crc32;      // FEAT_CRC32

    // ID_AA64ISAR1_EL1
    bool pac_apa;    // FEAT_PAuth via APIAKey (QARMA / IMPDEF)
    bool pac_api;    // FEAT_PAuth via APIBKey (QARMA / IMPDEF)
    bool pac_gpa;    // FEAT_PAuth via PACGA (QARMA)
    bool pac_gpi;    // FEAT_PAuth via PACGA (IMPDEF)

    // ID_AA64PFR1_EL1
    bool bti;        // FEAT_BTI
    u8   mte;        // FEAT_MTE: 0 none / 1 instructions / 2 + tags / 3 + async

    // Linux-compatible AT_HWCAP word for the EL0 exec auxv (the arm64
    // uapi hwcap bit numbers — FP/ASIMD/AES/PMULL/SHA1/SHA2/CRC32/
    // ATOMICS/SHA3/ASIMDDP/SHA512). Derived from ID_AA64ISAR0_EL1 +
    // ID_AA64PFR0_EL1 in hw_features_detect; exec_fill_auxv publishes
    // it verbatim so ported feature-detection (libsodium's
    // getauxval(AT_HWCAP) armcrypto gate, the Go runtime's
    // internal/cpu hwcap init) sees the real CPU. A CPU without a
    // feature reports a clear bit and consumers fall back to their
    // portable paths — fail-safe on crypto-less cores (RPi4's A72).
    u64  linux_hwcap;
};

// Singleton populated by hw_features_detect at boot. Read-only after
// init.
extern struct hw_features g_hw_features;

// Read ID registers and populate g_hw_features. Idempotent in the
// trivial sense (reads the same registers each call). Call once,
// early in boot_main.
void hw_features_detect(void);

// Compose a comma-separated string listing detected hardening
// features for the boot banner. Writes into `buf` (cap `cap`).
// Returns the number of bytes written (excluding NUL).
//
// Format example: "PAC,BTI,MTE2,LSE,CRC32"
unsigned hw_features_describe(char *buf, unsigned cap);

#endif // THYLACINE_ARCH_ARM64_HWFEAT_H
