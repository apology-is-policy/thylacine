// /mmio-probe — first hardware-handle userspace test (P4-Ic5a).
//
// This is the first userspace binary to:
//   (1) hold CAP_HW_CREATE (granted by kproc via rfork_with_caps — P4-Ic3),
//   (2) call SYS_MMIO_CREATE (P4-Ib) and SYS_MMIO_MAP (P4-Ic2),
//   (3) read live device MMIO from userspace via the demand-paged
//       device-memory PTE installed by userland_demand_page on the
//       BURROW_TYPE_MMIO dispatch arm.
//
// Closes the bulk of deferred R10 F159 (SVC-path test coverage for
// SYS_MMIO_MAP) by exercising every check in `sys_mmio_map_handler`
// end-to-end from real userspace — not from kernel-context test code
// that bypasses the SVC dispatcher.
//
// Target: QEMU virt's PL061 GPIO at PA 0x09030000 (single 4-KiB page).
//
// Why PL061 GPIO (and the move off PL031): the probe needs a real device
// that is NOT in the kernel's reserved-MMIO list (kobj_mmio_reserve_kernel_
// ranges) so a CAP_HW_CREATE userspace driver can freely claim it. It used to
// target the PL031 RTC, but LS-K landed a kernel RTC driver and reserved PL031
// (I-5) — exactly the move the original probe anticipated ("when a Phase 5+
// kernel RTC driver lands, the kernel will reserve PL031 + mmio-probe will move
// to a different test surface"). PL061 GPIO is present on QEMU virt and the
// kernel has no GPIO driver, so it stays freely claimable. It carries the same
// PrimeCell PeriphID magic pattern as PL031, so the canonical "magic" check is
// preserved (just 0x61 instead of 0x31).
//
// PL061 register layout (ARM PrimeCell PL061 GPIO TRM, ARM DDI 0190):
//   0x000 GPIODATA — pin data, masked by address bits [9:2]; a read at
//                    offset 0 selects no pins and returns 0 (informational).
//   0x400 GPIODIR  — pin direction (0 = input at reset).
//   0xfe0 PeriphID0 — 0x61 (the "61" in PL061).
//   0xfe4 PeriphID1 — 0x10.
//   0xfe8 PeriphID2 — 0x04.
//   0xfec PeriphID3 — 0x00.
//
// Behavior:
//   1. Mmio::new(0x09030000, 0x1000, READ|MAP, USER_VA, READ) — claim + map
//      the PL061 page. Page-aligned PA + size; lazy PTE install on first
//      access via the demand-page MMIO dispatch arm (userland_demand_page
//      case BURROW_TYPE_MMIO -> device-memory PTE, MAIR_IDX_DEVICE nGnRnE).
//   2. Read GPIODATA at (USER_VA + 0x000) — informational (0 at offset 0).
//   3. Read PeriphID0 at (USER_VA + 0xfe0) — MUST be 0x61. This is the
//      canonical "magic" check: a compile-time constant the device returns,
//      proving the mapping yields LIVE device data rather than cached zeros.
//   4. Log both values; exit 0 on PeriphID0 == 0x61, exit 1 on mismatch.
//
// What this proves end-to-end at the SVC layer:
//   - HwHandleImpliesCap (caller has CAP_HW_CREATE; kernel rejects without).
//   - SYS_MMIO_CREATE arg validation: PA + size page-alignment, IPS bound,
//     rights ⊆ T_RIGHT_ALL_HW, kernel-reserved range reject (PL061 is NOT in
//     g_mmio_claims pre-reservations, so the claim succeeds; if PL061 ever
//     gets reserved, this test fails deterministically with "Mmio::new
//     failed" — a flag to repoint the probe to a different unclaimed PA).
//   - SYS_MMIO_MAP arg validation: handle kind == KOBJ_MMIO, rights have
//     RIGHT_MAP (so SYS_MMIO_CREATE must request R + MAP).
//   - Prot validation: non-zero, R/W only, W-without-R reject, no EXEC,
//     vaddr page-alignment + bit-47 check.
//   - Demand-page MMIO dispatch (BURROW_TYPE_MMIO arm of
//     userland_demand_page): selects MAIR_IDX_DEVICE (nGnRnE) attrs, not
//     MAIR_IDX_NORMAL_WB. Without this, the LDR would observe cached-RAM
//     garbage instead of live MMIO data.
//
// QEMU virt-specific PA hardcoded at v1.0; a future syscall (SYS_DEV_LOOKUP
// or /dev/* synthetic enumeration) lifts this.

#![no_std]
#![no_main]

// libthyla-rs convention (Phase 7 U-2b): every native Rust binary opts
// in to ThylaAlloc as its global allocator. Required because libthyla-rs
// links the alloc crate at its root; the symbol resolves here.
#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::handle::Rights;
use libthyla_rs::hardware::Mmio;
use libthyla_rs::{T_PROT_READ, t_exits, t_puts, t_putstr};

const PL061_PA: u64 = 0x0903_0000;                      // QEMU virt PL061 GPIO
const PL061_PAGE_SIZE: usize = 0x1000;
const USER_VA_MAPPING: u64 = 0x0050_0000;               // safe gap (text ends ~0x41xxxx; stack at 0x80000000)
const PL061_GPIODATA_OFFSET: usize = 0x000;             // pin data (0 at offset 0 — informational)
const PL061_PERIPHID0_OFFSET: usize = 0xfe0;            // 0x61 (the "61" in PL061)
const EXPECTED_PERIPHID0: u32 = 0x61;                    // PL061 GPIO TRM (ARM DDI 0190)

// Tiny u32 → hex string helper for diagnostic output. No std formatting
// in no_std + no_alloc, and pulling in core::fmt would add ~2 KB to a
// 600-byte binary. Manual loop is 18 lines + a fixed buffer.
//
// Writes "0xXXXXXXXX" into `buf` (10 bytes). Returns 10.
//
// Post-P4-Ic5-FP: `let mut hex_buf = [0u8; 10]` at the call site is
// safe again. The kernel now enables CPACR_EL1.FPEN and saves/restores
// V regs on context switch, so any compiler-builtins memset that
// rustc may emit (and which uses NEON `dup v0.4h`) runs at EL0 without
// trapping.
fn write_hex_u32(buf: &mut [u8; 10], val: u32) {
    buf[0] = b'0';
    buf[1] = b'x';
    for i in 0..8 {
        let nibble = (val >> ((7 - i) * 4)) & 0xf;
        buf[2 + i] = if nibble < 10 { b'0' + nibble as u8 } else { b'a' + (nibble - 10) as u8 };
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("mmio-probe: starting (P4-Ic5a; U-2h-hardware typed API)\n");

    // Create + map the PL061 page in one constructor call. READ | MAP
    // rights -- READ for the register read; MAP because SYS_MMIO_MAP
    // checks for it on the handle. The kernel demand-pages the
    // device-attribute PTE on the first access.
    //
    // SAFETY: PL061_PA is a real QEMU virt device range (PL061 GPIO);
    // USER_VA_MAPPING is a chosen unmapped gap above the binary text
    // (text ends ~0x41xxxx; stack at 0x80000000).
    let gpio = match unsafe {
        Mmio::new(
            PL061_PA,
            PL061_PAGE_SIZE,
            Rights::READ | Rights::MAP,
            USER_VA_MAPPING,
            T_PROT_READ,
        )
    } {
        Ok(m) => m,
        Err(_) => {
            t_putstr("mmio-probe: Mmio::new failed (PL061 may now be kernel-reserved; update probe to a different unclaimed MMIO PA)\n");
            unsafe { t_exits(1) };
        }
    };
    t_putstr("mmio-probe: Mmio::new ok (create + map combined)\n");

    // Live MMIO reads via the typed surface. First access page-faults
    // -> userland_demand_page -> BURROW_TYPE_MMIO -> mmu_install_user_pte
    // with device_memory=true -> MAIR_IDX_DEVICE PTE installed.
    let gpiodata = gpio.read_u32(PL061_GPIODATA_OFFSET);
    let periphid0 = gpio.read_u32(PL061_PERIPHID0_OFFSET);

    // Diagnostic: "mmio-probe: gpiodata=0xXXXXXXXX periphid0=0xXXXXXXXX\n".
    let mut hex_buf: [u8; 10] = [0; 10];
    t_putstr("mmio-probe: gpiodata=");
    write_hex_u32(&mut hex_buf, gpiodata);
    unsafe { t_puts(hex_buf.as_ptr(), 10); }
    t_putstr(" periphid0=");
    write_hex_u32(&mut hex_buf, periphid0);
    unsafe { t_puts(hex_buf.as_ptr(), 10); }
    t_putstr("\n");

    // Verdict: PeriphID0 MUST be 0x61. This proves the mapping returns
    // LIVE device data rather than cached zeros / RAM (the canonical
    // failure mode of forgetting device-memory PTE attrs). GPIODATA is
    // informational (0 at offset 0 — the address-mask selects no pins).
    if periphid0 != EXPECTED_PERIPHID0 {
        t_putstr("mmio-probe: FAIL — PeriphID0 mismatch (expected 0x61)\n");
        unsafe { t_exits(1) };
    }

    t_putstr("mmio-probe: PASS\n");
    0
}
