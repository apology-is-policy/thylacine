// PSCI (Power State Coordination Interface) — secondary CPU bring-up.
//
// Per ARCHITECTURE.md §20: SMP up to 8 cores at v1.0; secondaries are
// brought up by PSCI calls (HVC on QEMU virt; SMC on most bare metal).
//
// PSCI 0.2+ standard function IDs (Arm DEN 0022D). The on-CPU SMCCC
// machinery is in psci.c — psci_init() reads /psci/method from DTB
// and stores the conduit (HVC vs SMC); psci_cpu_on() issues the
// CPU_ON_64 call to wake a secondary at a given entry-point PA.
//
// At v1.0 this is enough to wake secondaries to a per-CPU init loop.
// CPU_OFF / SUSPEND / SYSTEM_RESET / SYSTEM_OFF are not used by the
// kernel itself; halcyon and a future post-v1.0 powerdown chunk
// would extend this.

#ifndef THYLACINE_PSCI_H
#define THYLACINE_PSCI_H

#include <thylacine/types.h>

// Standard PSCI function IDs (Arm DEN 0022D, PSCI 0.2+).
// SMC32 (32-bit calling convention; high bit 0x80000000): used by
// CPU_OFF and other no-arg functions. SMC64 (high bit 0xC0000000):
// used by CPU_ON_64 (entry point is a 64-bit PA).
#define PSCI_VERSION              0x84000000u
#define PSCI_CPU_OFF              0x84000002u
#define PSCI_CPU_ON_64            0xC4000003u
#define PSCI_AFFINITY_INFO_64     0xC4000004u
#define PSCI_SYSTEM_OFF           0x84000008u
#define PSCI_SYSTEM_RESET         0x84000009u

// Return codes (Arm DEN 0022D). Negative integers per the SMCCC.
#define PSCI_SUCCESS              0
#define PSCI_NOT_SUPPORTED        -1
#define PSCI_INVALID_PARAMETERS   -2
#define PSCI_DENIED               -3
#define PSCI_ALREADY_ON           -4
#define PSCI_ON_PENDING           -5
#define PSCI_INTERNAL_FAILURE     -6
#define PSCI_NOT_PRESENT          -7
#define PSCI_DISABLED             -8
#define PSCI_INVALID_ADDRESS      -9

// Initialize PSCI — reads /psci/method from DTB and caches the conduit
// (HVC or SMC). Returns true if PSCI is usable; false if /psci is
// missing, method is unknown, or DTB isn't ready.
//
// Must be called after dtb_init. Idempotent.
bool psci_init(void);

// True iff psci_init succeeded and a conduit was selected.
bool psci_is_ready(void);

// Issue PSCI_CPU_ON_64 to wake the secondary identified by `target`
// (the MPIDR aff bits per /cpus/cpu@N/reg). The secondary starts at
// `entry_point` (a physical address) with x0 = `context_id` and MMU
// off, EL1.
//
// Returns 0 (PSCI_SUCCESS) or a negative PSCI error code per the
// PSCI spec. Caller is responsible for arranging the entry-point
// trampoline + per-CPU boot stack BEFORE calling.
//
// Not safe to call concurrently for the SAME target — PSCI tracks per-
// CPU state and may return ON_PENDING / ALREADY_ON. Caller should
// serialize bring-up requests.
int psci_cpu_on(u64 target, u64 entry_point, u64 context_id);

#endif // THYLACINE_PSCI_H
