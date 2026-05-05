// PSCI conduit implementation (P2-Ca).
//
// Reads /psci/method from DTB at psci_init time + caches the conduit
// (HVC or SMC). psci_cpu_on dispatches the SMC64 calling-convention
// CPU_ON_64 call via the appropriate instruction.
//
// SMCCC argument convention: function ID in x0, args in x1-x3, return
// in x0 (we only need the first return register for PSCI status).
// HVC immediate is #0; SMC immediate is #0. The conduit is fixed at
// boot time — we don't change it at runtime.

#include "psci.h"

#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/types.h>

static dtb_psci_method_t g_conduit = DTB_PSCI_NONE;
static bool              g_initialized = false;

bool psci_init(void) {
    if (g_initialized) return g_conduit != DTB_PSCI_NONE;

    g_conduit = dtb_psci_method();
    g_initialized = true;
    return g_conduit != DTB_PSCI_NONE;
}

bool psci_is_ready(void) {
    return g_initialized && g_conduit != DTB_PSCI_NONE;
}

// Single SMCCC call. Function ID in x0; args in x1, x2, x3; return
// in x0. Conduit chosen at compile-time-of-this-call via a branch on
// g_conduit — we can't use constant-conduit dispatch because both
// HVC and SMC paths must be present in the binary (different boards
// have different conduits, and the build is conduit-agnostic).
//
// Memory clobber + register clobbers per SMCCC: the caller's view of
// memory may have been mutated by the secure-world / hypervisor
// implementation (e.g., PSCI may change the secondary CPU's stack-
// page mapping during CPU_ON), so we conservatively invalidate the
// compiler's memory model. The "cc" clobber covers the flag changes
// the call may produce.
static s64 smccc_call(u64 func_id, u64 a1, u64 a2, u64 a3) {
    register u64 x0 __asm__("x0") = func_id;
    register u64 x1 __asm__("x1") = a1;
    register u64 x2 __asm__("x2") = a2;
    register u64 x3 __asm__("x3") = a3;

    if (g_conduit == DTB_PSCI_HVC) {
        __asm__ __volatile__(
            "hvc #0\n"
            : "+r"(x0)
            : "r"(x1), "r"(x2), "r"(x3)
            : "memory", "cc"
        );
    } else if (g_conduit == DTB_PSCI_SMC) {
        __asm__ __volatile__(
            "smc #0\n"
            : "+r"(x0)
            : "r"(x1), "r"(x2), "r"(x3)
            : "memory", "cc"
        );
    } else {
        // Conduit not initialized or NONE — return NOT_SUPPORTED so
        // callers see the failure without a useless trap.
        return PSCI_NOT_SUPPORTED;
    }

    return (s64)x0;
}

int psci_cpu_on(u64 target, u64 entry_point, u64 context_id) {
    if (!g_initialized) extinction("psci_cpu_on before psci_init");
    s64 ret = smccc_call(PSCI_CPU_ON_64, target, entry_point, context_id);
    return (int)ret;
}
