// R12-uaccess: kernel-side user-VA accessor primitives (P4-R12-uaccess).
//
// Replaces the userspace `pretouch_rodata_pages()` discipline with
// proper kernel-mode fault recovery. See uaccess.S for design rationale.
//
// Public surface:
//   - uaccess_load_u8(va, out) — read one byte from a user VA.
//   - uaccess_fixup_lookup(pc) — walk the fixup table by faulting PC.
//
// Caller contract:
//   - VA must lie in [0, USER_VA_TOP). Callers should validate before
//     invoking the primitive; the asm primitive does not range-check.
//   - The output pointer must point at a kernel-writable byte.
//   - On return == -1, the caller treats the access as -EFAULT.
//
// At v1.0 only uaccess_load_u8 is provided; SYS_PUTS is the sole
// consumer. Larger primitives (load_u16/u32/u64, store_*, copy_*)
// extend the table on demand.

#ifndef THYLACINE_ARCH_ARM64_UACCESS_H
#define THYLACINE_ARCH_ARM64_UACCESS_H

#include <thylacine/types.h>

// Read a single byte from a user VA. Returns 0 on success (*out
// updated) or -1 on translation/permission fault. On -1 the fault
// was caught by the fixup table; the kernel does NOT extinct. Caller
// MUST treat -1 as an EFAULT-equivalent error code on the syscall
// surface (or whatever the caller's error convention is).
//
// The output is written only on success; callers must not rely on
// the byte's state after a -1 return.
extern s64 uaccess_load_u8(u64 user_va, u8 *out);

// Write a single byte to a user VA. Returns 0 on success or -1 on
// translation/permission fault. On -1 the fault was caught by the
// fixup table; the kernel does NOT extinct. Caller MUST treat -1 as
// an EFAULT-equivalent error code on the syscall surface.
//
// Symmetric to uaccess_load_u8; added at P5-fd-rw for SYS_READ's
// per-byte copy from kernel scratch into the user-VA buffer.
extern s64 uaccess_store_u8(u64 user_va, u8 value);

// Look up the fixup PC for a faulting instruction PC. Returns 0 if
// `fault_pc` is not in the table (the fault is not a uaccess fault).
// Otherwise returns the fixup PC to which the dispatcher must
// transfer control (by writing ctx->elr).
//
// Called only by the kernel-mode sync fault handler. Linear scan of
// the .uaccess_fixup table; the table size is small (one entry per
// kernel-mode uaccess primitive — single-digit at v1.0).
u64 uaccess_fixup_lookup(u64 fault_pc);

// User-half VA bound shared with arch/arm64/mmu.h (USER_VA_TOP). The
// dispatcher uses this to recognize "this kernel-mode fault is on a
// user VA" before consulting the fixup table.
#define UACCESS_USER_VA_TOP  (1ull << 47)

#endif // THYLACINE_ARCH_ARM64_UACCESS_H
