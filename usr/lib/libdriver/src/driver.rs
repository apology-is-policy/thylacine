// The driver runtime -- the only libthyla-rs layer (feature `driver`).
//
// A driver built on the framework is `impl Driver for MyDriver { probe + serve }`
// plus `fn main() { libdriver::run::<MyDriver>() }`. The scaffold:
//   1. `bind`   -- decode the warden's spawn descriptor from argv[1] into a
//                  `BoundResources` (the conferred, allowance-bounded grant).
//   2. `probe`  -- the driver mints its handles from the grant (the helpers
//                  below) + brings the device up.
//   3. `serve`  -- the driver publishes its file + services requests until the
//                  warden group-terminates it (DeviceRemoved) or a fatal error.
//
// The conferred allowance (step-5a `Command::allowance`) is the kernel-enforced
// bound: every `Mmio::new`/`Irq::new`/`Dma::new` the helpers issue is gated
// against it (I-34). The descriptor merely *informs* the driver which resources
// to create handles for; it confers no authority -- a driver that fabricated a
// PA outside its allowance would be rejected by the kernel gate, not the codec.
//
// `to_allowance` is the warden's mirror: it turns a `BoundResources` into the
// `TAllowanceDesc` for `Command::allowance`, so the authority the kernel enforces
// and the resources the driver maps come from one `BoundResources` value.

use libthyla_rs::env;
use libthyla_rs::handle::Rights;
use libthyla_rs::hardware::{Dma, Irq, Mmio};
use libthyla_rs::{eprintln, t_exit_group, TAllowanceDesc, T_PROT_READ, T_PROT_WRITE};

use crate::resource::BoundResources;
use crate::Error;

/// A driver written against the framework. `run::<D>()` drives a `D` through its
/// lifecycle.
pub trait Driver: Sized {
    /// Bring the device up. Mint MMIO/IRQ/DMA handles from `res` (use `map_mmio`
    /// / `claim_irq` / `alloc_dma`), program the device, and return the running
    /// state. A returned `Err` exits the Proc with `EXIT_PROBE` (the supervisor
    /// reads it for restart policy).
    fn probe(res: &BoundResources) -> Result<Self, Error>;

    /// Publish the device's file(s) into the namespace and service requests until
    /// the device is removed (the warden group-terminates this Proc) or a fatal
    /// error. A clean stop returns `Ok(())` (exit `EXIT_OK`); a fatal error exits
    /// with `EXIT_SERVE`.
    fn serve(self, res: &BoundResources) -> Result<(), Error>;
}

/// Clean serve stop / driver exit.
pub const EXIT_OK: i64 = 0;
/// The spawn descriptor was missing or unparseable (a warden bug -- the supervisor
/// should not blindly restart).
pub const EXIT_BIND: i64 = 71;
/// Device init (`probe`) failed.
pub const EXIT_PROBE: i64 = 72;
/// The serve loop exited with an error.
pub const EXIT_SERVE: i64 = 73;

/// The base of a driver's private device-VA bump region (16 MiB). Clears the
/// fixed VAs the existing transports hardcode (netdev's MMIO/DMA at 0x50_0000..
/// 0x63_0000; the PCI BAR window at 0x80_0000..0xE0_0000), so a driver built on
/// `DriverVa` never collides with them. Per-Proc address spaces mean this only
/// has to be free within one driver Proc; the kernel rejects a collision
/// (fail-closed), it is never UB.
pub const DRIVER_VA_BASE: u64 = 0x0100_0000;

const PAGE: u64 = 0x1000;

/// A page-granular bump allocator over a driver's private device-VA region. A
/// driver makes one in `probe` and draws MMIO/DMA map targets from it.
pub struct DriverVa(u64);

impl DriverVa {
    /// Start a fresh region at `DRIVER_VA_BASE`.
    pub const fn new() -> Self {
        DriverVa(DRIVER_VA_BASE)
    }

    /// Reserve `len` bytes (rounded up to a page) and return the start VA.
    /// Saturating, so a pathological `len` (a malformed descriptor window size)
    /// can never wrap/panic the bump pointer -- a resulting out-of-range VA fails
    /// cleanly at `Mmio::new`/`Dma::new` (the kernel rejects it), never UB.
    pub fn take(&mut self, len: u64) -> u64 {
        let va = self.0;
        let pages = len.div_ceil(PAGE).max(1);
        self.0 = self.0.saturating_add(pages.saturating_mul(PAGE));
        va
    }
}

impl Default for DriverVa {
    fn default() -> Self {
        Self::new()
    }
}

/// Decode the bound-resource descriptor the warden passed as argv[1].
pub fn bind() -> Result<BoundResources, Error> {
    let args = env::args();
    let desc = args.get_str(1).ok_or(Error::NoDescriptor)?;
    BoundResources::parse_descriptor(desc)
}

/// The framework entry. A driver's `main` is just `libdriver::run::<MyDriver>()`.
/// Never returns -- exits the Proc with a lifecycle status code.
pub fn run<D: Driver>() -> ! {
    let res = match bind() {
        Ok(r) => r,
        Err(e) => {
            eprintln!("libdriver: bind failed: {:?}", e);
            // SAFETY: terminate the whole driver Proc (it has no peer threads
            // yet at bind time, but exit_group is the correct whole-Proc stop).
            unsafe { t_exit_group(EXIT_BIND) }
        }
    };
    let dev = match D::probe(&res) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("libdriver: probe failed: {:?}", e);
            unsafe { t_exit_group(EXIT_PROBE) }
        }
    };
    match dev.serve(&res) {
        Ok(()) => unsafe { t_exit_group(EXIT_OK) },
        Err(e) => {
            eprintln!("libdriver: serve failed: {:?}", e);
            unsafe { t_exit_group(EXIT_SERVE) }
        }
    }
}

/// Build the kernel allowance descriptor for `Command::allowance` from a grant.
/// The WARDEN's mirror of the codec: the authority the kernel enforces and the
/// resources the driver maps both come from this one `BoundResources`. (A driver
/// never calls this -- it has no broad allowance to narrow.)
pub fn to_allowance(res: &BoundResources) -> TAllowanceDesc {
    let mut d = TAllowanceDesc::empty();
    for (base, size) in &res.mmio {
        // MMIO is mapped page-granular, so the allowance window MUST be page-
        // granular -- the kernel I-34 gate checks the driver's *page*-sized
        // `SYS_MMIO_CREATE` against the allowance, and a sub-page device register
        // (a virtio-mmio slot is 0x200) is only reachable by mapping its whole
        // page. `page_round` rounds the window out so the page map is covered. For
        // a virtio-mmio net slot this grants the shared page, which also spans an
        // adjacent blk slot -- the documented #140 / net-2 co-residency over-grant;
        // sub-page separation is the net-2 seam. (The descriptor keeps the exact
        // sub-page window, so the driver still learns its precise slot address.)
        // capacities mirror MAX_MMIO/MAX_IRQ; resolve/codec already bound them.
        let (lo, len) = crate::resource::page_round(*base, *size);
        let _ = d.push_mmio(lo, len);
    }
    for intid in &res.irq {
        let _ = d.push_irq(*intid);
    }
    d.set_dma_max(res.dma_max);
    d
}

/// Map MMIO window `idx` from the grant into the driver's VA region, R+W (device
/// memory; no EXEC per I-12). The PA/size came from the warden's conferred
/// allowance, so the kernel gate admits the claim.
pub fn map_mmio(res: &BoundResources, idx: usize, va: &mut DriverVa) -> Result<Mmio, Error> {
    let (pa, size) = *res.mmio.get(idx).ok_or(Error::NoSuchResource)?;
    let vaddr = va.take(size);
    // SAFETY: `pa..pa+size` is a real device window the warden conferred (the
    // kernel allowance gate + claim table validate it); `vaddr` is the driver's
    // private bump region, unmapped.
    unsafe {
        Mmio::new(
            pa,
            size as usize,
            Rights::READ | Rights::WRITE | Rights::MAP,
            vaddr,
            T_PROT_READ | T_PROT_WRITE,
        )
    }
    .map_err(|_| Error::Hardware)
}

/// Claim wired IRQ `idx` from the grant (for `Irq::wait`). The INTID came from
/// the warden's conferred allowance.
pub fn claim_irq(res: &BoundResources, idx: usize) -> Result<Irq, Error> {
    let intid = *res.irq.get(idx).ok_or(Error::NoSuchResource)?;
    Irq::new(intid, Rights::SIGNAL).map_err(|_| Error::Hardware)
}

/// Allocate a `size`-byte DMA buffer (R+W) mapped into the driver's VA region.
/// `size` must be within the grant's `dma_max` (the kernel allowance gate also
/// enforces it).
pub fn alloc_dma(res: &BoundResources, size: usize, va: &mut DriverVa) -> Result<Dma, Error> {
    if res.dma_max == 0 || size as u64 > res.dma_max {
        return Err(Error::NoSuchResource);
    }
    let vaddr = va.take(size as u64);
    // SAFETY: `vaddr` is the driver's private bump region, unmapped; the kernel
    // allocates + pins the backing pages and rejects a VA collision.
    unsafe {
        Dma::new(
            size,
            Rights::READ | Rights::WRITE | Rights::MAP,
            vaddr,
            T_PROT_READ | T_PROT_WRITE,
        )
    }
    .map_err(|_| Error::Hardware)
}

/// A reference null driver -- the compile-proof that the `Driver` + `run` scaffold
/// type-checks end to end (the device build instantiates `run::<NopDriver>` via
/// `nop_entry`). A driver that binds, takes no hardware, and serves nothing; a
/// usable placeholder for a node with no real driver.
pub mod example {
    use super::{run, Driver};
    use crate::resource::BoundResources;
    use crate::Error;

    /// A driver that holds no hardware and exits cleanly.
    pub struct NopDriver;

    impl Driver for NopDriver {
        fn probe(_res: &BoundResources) -> Result<Self, Error> {
            Ok(NopDriver)
        }
        fn serve(self, _res: &BoundResources) -> Result<(), Error> {
            Ok(())
        }
    }

    /// A complete driver `main` for `NopDriver` -- instantiating `run::<NopDriver>`
    /// forces the whole scaffold to monomorphize + type-check in the device build.
    pub fn nop_entry() -> ! {
        run::<NopDriver>()
    }
}
