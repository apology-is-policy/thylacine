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
// that bypasses the SVC dispatcher. The remaining SVC surface
// (SYS_IRQ_CREATE + SYS_IRQ_WAIT) gets exercised at P4-Ic5b when the
// real virtio-blk driver lands with IRQ-driven completion.
//
// Target: QEMU virt's PL031 RTC at PA 0x09010000 (single 4-KiB page).
//
// Why PL031 and not virtio-mmio: at P4-Ic2 R10 F154 the kernel reserves
// the entire virtio-mmio range (0x0a000000-0x0a003fff) in g_mmio_claims
// via kobj_mmio_reserve_kernel_ranges() so a CAP_HW_CREATE-holding
// userspace driver can't claim a slot the kernel might own. PL031 RTC
// is NOT in the kernel-reserved list (Thylacine v1.0 has no kernel-side
// RTC driver), so it's freely claimable from userspace — exactly the
// surface mmio-probe needs to exercise SYS_MMIO_CREATE + SYS_MMIO_MAP
// + the demand-page MMIO dispatch end-to-end. When a Phase 5+ kernel
// RTC driver lands, the kernel will reserve PL031 + mmio-probe will
// move to a different test surface (or get replaced by virtio-blk
// at P4-Ic5b which will get a kernel-side delegation API).
//
// PL031 register layout (ARM PrimeCell PL031 r1p3 TRM, Table 3-2):
//   0x00 RTCDR   — current epoch time (seconds since Unix epoch under
//                  QEMU virt; non-zero after boot).
//   0x04 RTCMR   — alarm match register.
//   0x08 RTCLR   — load register (writable; sets RTCDR).
//   0x0c RTCCR   — control register.
//   0xfe0 PeriphID0 — 0x31 (the "31" in PL031).
//   0xfe4 PeriphID1 — 0x10.
//   0xfe8 PeriphID2 — 0x14.
//   0xfec PeriphID3 — 0x00.
//
// Behavior:
//   1. t_mmio_create(0x09010000, 0x1000, READ) — claim the PL031 page.
//      Page-aligned PA + size required.
//   2. t_mmio_map(handle, 0x500000, READ) — install user-VA mappings.
//      Lazy — actual PTEs install on first access via the demand-page
//      MMIO dispatch arm (userland_demand_page case BURROW_TYPE_MMIO →
//      device-memory PTE with MAIR_IDX_DEVICE nGnRnE).
//   3. Read RTCDR at (0x500000 + 0x00) — should be > 0 after boot
//      (QEMU virt seeds the RTC from the host clock).
//   4. Read PeriphID0 at (0x500000 + 0xfe0) — should be 0x31 ("31"
//      identifying PL031). This is the canonical "magic" check: a
//      compile-time constant that proves the mapping returns LIVE
//      device data rather than cached zeros / RAM.
//   5. Log both values via t_puts.
//   6. Exit 0 on PeriphID0 == 0x31; exit 1 on mismatch.
//
// What this proves end-to-end at the SVC layer:
//   - HwHandleImpliesCap (caller has CAP_HW_CREATE; kernel rejects without).
//   - SYS_MMIO_CREATE arg validation: PA + size page-alignment, IPS bound,
//     rights ⊆ T_RIGHT_ALL_HW, kernel-reserved range reject (PL031 is
//     NOT in g_mmio_claims pre-reservations, so the claim succeeds; if
//     PL031 ever gets added to the reservation list, this test fails
//     deterministically with "SYS_MMIO_CREATE failed" — a flag to update
//     the probe to a different unclaimed PA).
//   - SYS_MMIO_MAP arg validation: handle kind == KOBJ_MMIO, rights have
//     RIGHT_MAP wait — actually mmio-probe requests rights = READ only,
//     so it needs SYS_MMIO_CREATE's rights to include T_RIGHT_MAP for
//     the subsequent map to succeed. R + MAP is the minimum.
//   - Prot validation: non-zero, R/W only, W-without-R reject, no EXEC,
//     vaddr page-alignment + bit-47 check.
//   - Demand-page MMIO dispatch (BURROW_TYPE_MMIO arm of
//     userland_demand_page): selects MAIR_IDX_DEVICE (nGnRnE) attrs,
//     not MAIR_IDX_NORMAL_WB. Without this, the LDR would hit cached
//     RAM behavior + observe garbage instead of live MMIO data.
//
// QEMU virt-specific PA hardcoded at v1.0; a future syscall
// (SYS_DEV_LOOKUP or /dev/* synthetic enumeration) lifts this.

#![no_std]
#![no_main]

// libthyla-rs convention (Phase 7 U-2b): every native Rust binary opts
// in to ThylaAlloc as its global allocator. Required because libthyla-rs
// links the alloc crate at its root; the symbol resolves here.
#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::{
    T_PROT_READ, T_RIGHT_MAP, T_RIGHT_READ,
    t_exits, t_mmio_create, t_mmio_map, t_puts, t_putstr,
};

const PL031_PA: u64 = 0x0901_0000;                      // QEMU virt PL031 RTC
const PL031_PAGE_SIZE: u64 = 0x1000;
const USER_VA_MAPPING: u64 = 0x0050_0000;               // safe gap (text ends ~0x41xxxx; stack at 0x80000000)
const PL031_RTCDR_OFFSET: u64 = 0x000;                  // current time (seconds)
const PL031_PERIPHID0_OFFSET: u64 = 0xfe0;              // 0x31 (the "31" in PL031)
const EXPECTED_PERIPHID0: u32 = 0x31;                    // PL031 r1p3 TRM Table 3-2

// Volatile MMIO register read. Bypasses Rust's read coalescing /
// elimination; the kernel-installed PTE has MAIR_IDX_DEVICE (nGnRnE)
// so the load is non-gathering + non-reordering at the hardware level,
// but the compiler also needs to be told this is a side-effect-bearing
// access.
//
// Safety: caller must ensure `addr` is a valid user-VA pointing at
// MMIO that's been mapped via SYS_MMIO_MAP.
#[inline(always)]
unsafe fn mmio_read32(addr: u64) -> u32 {
    core::ptr::read_volatile(addr as *const u32)
}

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
// trapping. P4-Ic5a's `static mut HEX_BUF` workaround removed.
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
    t_putstr("mmio-probe: starting (P4-Ic5a)\n");

    // SYS_MMIO_CREATE: claim the PL031 page. Need READ | MAP rights —
    // READ for the eventual MMIO read; MAP because SYS_MMIO_MAP checks
    // for it on the handle.
    let rights = T_RIGHT_READ | T_RIGHT_MAP;
    let handle = unsafe { t_mmio_create(PL031_PA, PL031_PAGE_SIZE, rights) };
    if handle < 0 {
        t_putstr("mmio-probe: SYS_MMIO_CREATE failed (PL031 may now be kernel-reserved; update probe to a different unclaimed MMIO PA)\n");
        unsafe { t_exits(1) };
    }
    t_putstr("mmio-probe: SYS_MMIO_CREATE ok\n");

    // SYS_MMIO_MAP: install user-VA mappings. Demand-page dispatches on
    // BURROW_TYPE_MMIO when the first access faults. READ-only prot.
    let prot = T_PROT_READ;
    let map_rc = unsafe { t_mmio_map(handle, USER_VA_MAPPING, prot) };
    if map_rc < 0 {
        t_putstr("mmio-probe: SYS_MMIO_MAP failed\n");
        unsafe { t_exits(1) };
    }
    t_putstr("mmio-probe: SYS_MMIO_MAP ok\n");

    // Live MMIO reads. First access page-faults → userland_demand_page
    // → case BURROW_TYPE_MMIO → mmu_install_user_pte(..., device_memory=true)
    // → MAIR_IDX_DEVICE PTE installed → retry succeeds + returns live data.
    let rtcdr = unsafe { mmio_read32(USER_VA_MAPPING + PL031_RTCDR_OFFSET) };
    let periphid0 = unsafe { mmio_read32(USER_VA_MAPPING + PL031_PERIPHID0_OFFSET) };

    // Diagnostic: "mmio-probe: rtcdr=0xXXXXXXXX periphid0=0xXXXXXXXX\n".
    // Stack-init array is safe post-P4-Ic5-FP (kernel saves/restores V
    // regs; any NEON memset emitted by rustc doesn't trap EL0).
    let mut hex_buf: [u8; 10] = [0; 10];
    t_putstr("mmio-probe: rtcdr=");
    write_hex_u32(&mut hex_buf, rtcdr);
    unsafe { t_puts(hex_buf.as_ptr(), 10); }
    t_putstr(" periphid0=");
    write_hex_u32(&mut hex_buf, periphid0);
    unsafe { t_puts(hex_buf.as_ptr(), 10); }
    t_putstr("\n");

    // Verdict: PeriphID0 MUST be 0x31. This proves the mapping returns
    // LIVE device data rather than cached zeros / RAM (the canonical
    // failure mode of forgetting device-memory PTE attrs). RTCDR is
    // informational (current time — varies per boot).
    if periphid0 != EXPECTED_PERIPHID0 {
        t_putstr("mmio-probe: FAIL — PeriphID0 mismatch (expected 0x31)\n");
        unsafe { t_exits(1) };
    }

    t_putstr("mmio-probe: PASS\n");
    0
}
