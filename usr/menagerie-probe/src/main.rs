// /menagerie-probe -- the warden's bind-loop proof (Menagerie build-arc 5c), and
// the first real `impl libdriver::Driver`. The warden discovers the QEMU-virt
// pl061 GPIO node in /hw, intersects it with this driver's manifest, and spawns
// us with the conferred (narrowed) hardware allowance + the grant descriptor.
//
// We prove the loop end to end with one positive and one negative:
//   - POSITIVE: map the granted pl061 MMIO window. The warden's grant came from
//     the node's own `reg`, the kernel allowance gate (I-34) admits exactly that
//     window, and pl061 is undriven/unreserved -- so the map succeeds. This is
//     the descriptor round-trip (we received the exact window the warden granted)
//     plus the allowance permitting the grant.
//   - NEGATIVE: try to create an MMIO handle for a PA *outside* the grant. Our
//     allowance is narrowed to pl061, so the kernel gate rejects it -- proving the
//     conferred allowance is active, not broad. A success here would mean the
//     allowance was not enforced (the bug we are guarding against).
//
// We hold no long-lived device (a GPIO controller we never program); `serve`
// returns immediately so the warden can reap us and read the lifecycle status.
// A real driver (5d: netdev) serves until DeviceRemoved.
//
// PASS = exit 0 (libdriver::EXIT_OK); a probe failure exits EXIT_PROBE (the
// warden reports it).

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libdriver::driver::{map_mmio, run, Driver, DriverVa};
use libdriver::resource::BoundResources;
use libdriver::Error;
use libthyla_rs::handle::Rights;
use libthyla_rs::hardware::Mmio;
use libthyla_rs::{T_PROT_READ, T_PROT_WRITE};

/// Console-direct diagnostics (T_SYS_PUTS), visible in the boot log regardless of
/// fd wiring -- a boot probe loses fd-2 (eprintln) output.
macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

/// A PA the warden never grants this driver -- well outside pl061's window and
/// not a real device region, so the narrowed allowance is the *only* thing that
/// can reject it.
const OUT_OF_GRANT_PA: u64 = 0xDEAD_0000;

struct ProbeDriver;

impl Driver for ProbeDriver {
    fn probe(res: &BoundResources) -> Result<Self, Error> {
        say!(
            "menagerie-probe: grant compat={} serve={} mmio={} irq={} dma={:#x}",
            res.compatible,
            res.serves,
            res.mmio.len(),
            res.irq.len(),
            res.dma_max
        );

        // POSITIVE: map the granted window (in-grant + unreserved -> admitted).
        let (pa, size) = *res.mmio.first().ok_or(Error::NoSuchResource)?;
        let mut va = DriverVa::new();
        let mmio = map_mmio(res, 0, &mut va)?;
        say!(
            "menagerie-probe: mapped granted MMIO {:#x}/{:#x} OK ({} bytes)",
            pa,
            size,
            mmio.len()
        );

        // NEGATIVE: an out-of-grant create must be rejected by the I-34 gate.
        let bad_va = va.take(0x1000);
        let denied = unsafe {
            Mmio::new(
                OUT_OF_GRANT_PA,
                0x1000,
                Rights::READ | Rights::WRITE | Rights::MAP,
                bad_va,
                T_PROT_READ | T_PROT_WRITE,
            )
        };
        if denied.is_ok() {
            say!(
                "menagerie-probe: FAIL -- out-of-grant MMIO {:#x} create SUCCEEDED \
                 (narrowed allowance not enforced)",
                OUT_OF_GRANT_PA
            );
            return Err(Error::Hardware);
        }
        say!("menagerie-probe: out-of-grant MMIO create rejected OK (allowance enforced)");

        // The map handle drops here; nothing to serve.
        drop(mmio);
        Ok(ProbeDriver)
    }

    fn serve(self, _res: &BoundResources) -> Result<(), Error> {
        say!("menagerie-probe: PASS (grant verified + allowance enforced)");
        Ok(())
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run::<ProbeDriver>()
}
