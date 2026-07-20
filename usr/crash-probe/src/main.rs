// /crash-probe -- the warden's bounded-supervision proof (Menagerie build-arc
// 5e-2). A driver whose `probe` always fails: it exits `EXIT_PROBE` (72) every
// time the warden spawns it, exercising the supervisor's restart-on-crash loop
// (libdriver::supervise) end to end -- restart with back-off, up to the bound,
// then give up. A give-up is a SOFT per-device failure: the device is
// unavailable, but the boot proceeds (the warden does not exit non-zero).
//
// It binds the SYNTHETIC `restart-test` node the warden itself publishes
// (TAPESTRY.md section 18.12 F15) -- no real device backs it, so the demo never
// contends with a working driver. (It originally squatted virtio:16, the then-
// undriven GPU id; G-1 gave the GPU a real resident driver, gpud, and re-homed
// this demo here.) It crashes BEFORE any hardware claim -- its grant is empty
// (mmio=0 irq=0 dma=0) and it never calls SYS_MMIO/IRQ/DMA_CREATE -- so the
// supervision ladder is exercised with zero hardware footprint.
//
// This is a TEST vehicle -- the deliberate crash that proves the supervisor is
// real, the negative twin of menagerie-probe's positive bring-up. A real driver
// that fails to bring its device up returns the same EXIT_PROBE; the warden
// treats both identically.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libdriver::driver::{run, Driver};
use libdriver::resource::BoundResources;
use libdriver::Error;

/// Console-direct diagnostics (T_SYS_PUTS), visible in the boot log regardless of
/// fd wiring -- a boot probe loses fd-2 (eprintln) output.
macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

struct CrashDriver;

impl Driver for CrashDriver {
    fn probe(res: &BoundResources) -> Result<Self, Error> {
        // Fail BEFORE any handle mint -- no MMIO/IRQ/DMA is ever claimed, so the
        // conferred (page-rounded) allowance is never a live claim and cannot
        // conflict with a peer driver sharing the rounded page. `run` turns this
        // Err into EXIT_PROBE, which the warden's supervisor reads as a crash.
        say!(
            "crash-probe: grant compat={} mmio={} irq={} dma={:#x} -> failing probe (supervision demo)",
            res.compatible,
            res.mmio.len(),
            res.irq.len(),
            res.dma_max
        );
        Err(Error::Hardware)
    }

    fn serve(self, _res: &BoundResources) -> Result<(), Error> {
        // Unreachable: probe never returns Ok.
        Ok(())
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run::<CrashDriver>()
}
