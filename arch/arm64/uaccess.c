// R12-uaccess: kernel-mode user-VA fault-fixup table walker.
//
// See uaccess.S for design rationale and uaccess.h for the public
// surface. This file implements the table lookup that the kernel
// sync fault handler consults on every translation/permission fault
// whose FAR lies in the user-VA half.

#include "uaccess.h"

#include "mmu.h"            // USER_VA_TOP (cross-check)

#include <thylacine/types.h>

// Compile-time sanity: the shared USER_VA_TOP constant (used by both
// the VMA-layer reject in burrow.c and the uaccess fault dispatcher)
// must match. Drift here would mean a kernel-mode fault on a VA the
// VMA layer would have rejected slips past the uaccess check.
_Static_assert(UACCESS_USER_VA_TOP == USER_VA_TOP,
    "UACCESS_USER_VA_TOP must equal mmu.h USER_VA_TOP");

u64 uaccess_fixup_lookup(u64 fault_pc) {
    // Linear scan; at v1.0 the table has one entry (uaccess_load_u8).
    // The s32 offsets are sign-extended to s64 before adding to the
    // base address so a "fixup behind the slot" (negative offset)
    // would still resolve correctly.
    const struct uaccess_fixup_entry *e = _uaccess_fixup_start;
    while (e < _uaccess_fixup_end) {
        u64 op_pc = (u64)(uintptr_t)&e->op_rel + (u64)(s64)e->op_rel;
        if (op_pc == fault_pc) {
            return (u64)(uintptr_t)&e->fixup_rel + (u64)(s64)e->fixup_rel;
        }
        e++;
    }
    return 0;
}
