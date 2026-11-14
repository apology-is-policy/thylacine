// /virtio-serial -- the line to the Mac (docs/MAC-RUNNER.md 4.4).
//
// Warden-bound (`virtio-pci:3`, persistent). probe claims the virtio console device over the
// modern-PCI transport, negotiates MULTIPORT, and sets up the control queues and the queues of
// the first PORTS_MAX ports. serve learns the ports' names over the control queue, gives the
// port named org.thylacine.mac its buffers, posts /srv/mac in 9P mode, and relays: each guest
// connection to /srv/mac is one channel of the framing mac1 (usr/lib/macline), by the rules in
// src/relay.rs. It parses no 9P.
//
// Two threads. The IRQ thread blocks on the device's interrupt (a KOBJ_IRQ cannot be polled)
// and rings a pipe; the relay thread polls that pipe with the listener and every guest
// connection, so all of the line's state belongs to one thread and needs no lock.
//
// The device is not trusted: every used-ring id and length is checked before it names a
// buffer, and a bad one is dropped and counted.
//
// Diagnostics go to the console (t_putstr); stdout carries exactly the warden's READY line.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

use alloc::boxed::Box;
use alloc::vec::Vec;

use libdriver::driver::{alloc_dma, run, Driver, DriverVa};
use libdriver::resource::BoundResources;
use libdriver::Error;
use libthyla_rs::hardware::{
    mmio_read16, mmio_read32, mmio_read8, mmio_write16, mmio_write32, mmio_write64, mmio_write8, Dma,
    PciDev, PciIrq, PciIrqMode, PciRegion,
};
use libthyla_rs::io::Write;
use libthyla_rs::time::monotonic_ns;
use libthyla_rs::{
    t_burrow_attach, t_close, t_open, t_pipe, t_poll, t_read, t_set_nonblock, t_srv_accept, t_walk_create,
    t_write, thread, virtio_rmb, TPollFd, T_OPATH, T_OREAD, T_POLLERR, T_POLLHUP, T_POLLIN, T_POLLOUT,
    T_WALK_OPEN_FROM_ROOT,
};

use virtio_serial::console::{self, Act, Console, Control};
use virtio_serial::relay::{Key, Relay, State, MAX_CONNS};

const VIRTIO_DEVICE_ID_CONSOLE: u32 = 3;

/// The BAR window: clear of libdriver's DriverVa bump region (0x0100_0000 + the pool).
const BAR_WINDOW_VA: u64 = 0x0200_0000;

const STATUS_ACKNOWLEDGE: u8 = 1;
const STATUS_DRIVER: u8 = 2;
const STATUS_DRIVER_OK: u8 = 4;
const STATUS_FEATURES_OK: u8 = 8;
const STATUS_FAILED: u8 = 128;
const VIRTIO_F_VERSION_1_BIT_HI: u32 = 1 << 0;
const VIRTIO_CONSOLE_F_MULTIPORT: u32 = 1 << 1;

const CCFG_DEVICE_FEATURE_SELECT: u64 = 0x00;
const CCFG_DEVICE_FEATURE: u64 = 0x04;
const CCFG_DRIVER_FEATURE_SELECT: u64 = 0x08;
const CCFG_DRIVER_FEATURE: u64 = 0x0C;
const CCFG_NUM_QUEUES: u64 = 0x12;
const CCFG_DEVICE_STATUS: u64 = 0x14;
const CCFG_QUEUE_SELECT: u64 = 0x16;
const CCFG_QUEUE_SIZE: u64 = 0x18;
const CCFG_QUEUE_ENABLE: u64 = 0x1C;
const CCFG_QUEUE_NOTIFY_OFF: u64 = 0x1E;
const CCFG_QUEUE_DESC: u64 = 0x20;
const CCFG_QUEUE_DRIVER: u64 = 0x28;
const CCFG_QUEUE_DEVICE: u64 = 0x30;
const CCFG_MIN_LEN: u32 = 0x38;

/// virtio_console_config: cols(2) rows(2) max_nr_ports(4) [emerg_wr(4)].
const CFG_MAX_NR_PORTS: u64 = 4;
const CFG_MIN_LEN: u32 = 8;

const VIRTQ_DESC_F_WRITE: u16 = 2;

/// Ports this driver sets queues up for. QEMU numbers a virtserialport from 1 (0 is kept for
/// a virtconsole) and the Mac app's QEMU has one; a queue pair for every port the device could
/// have (31 by default) would cost a page each for nothing.
const PORTS_MAX: u32 = 4;
const QUEUES_MAX: usize = 2 * PORTS_MAX as usize + 2;
const CTRL_RX: u16 = 2;
const CTRL_TX: u16 = 3;

/// Entries per queue; QEMU's queues hold 32 (control) and 128 (data).
const QSIZE: u16 = 16;
const QSIZE_USZ: usize = QSIZE as usize;
const PAGE: usize = 4096;
/// A queue in one page: descriptors at 0, the available ring at 256, the used ring at 512.
const AVAIL_OFF: u64 = 256;
const USED_OFF: u64 = 512;

/// A control receive buffer: the header and a port's name.
const CTRL_BUF: usize = 256;
/// A control transmit slot: one message.
const CTRL_SLOT: usize = 16;
/// The line's receive and transmit buffers.
const DATA_BUF: usize = 4096;

// The pool: one page per queue (queue q at page q), the control buffers, the line's buffers.
const CRX_OFF: usize = QUEUES_MAX * PAGE;
const CTX_OFF: usize = CRX_OFF + QSIZE_USZ * CTRL_BUF;
const PRX_OFF: usize = CTX_OFF + PAGE;
const PTX_OFF: usize = PRX_OFF + QSIZE_USZ * DATA_BUF;
const POOL: usize = PTX_OFF + QSIZE_USZ * DATA_BUF;

/// This end's maxframe: the largest frame the app may send. The receive buffers carry the
/// stream in any pieces; the parser holds at most one frame.
const MAXFRAME: u32 = 16384;
/// The largest frame this end makes: one transmit buffer.
const FRAME_MAX: u32 = DATA_BUF as u32;
/// Frames queued toward the app past which guest connections are not read.
const OUT_HIGH: usize = QSIZE_USZ * DATA_BUF;
/// Bytes read from one guest connection per pass, so one cannot crowd out the rest.
const READ_CHUNK: usize = 4096;

/// How long serve waits for the ports' names, and then for the app's HELLO, before READY.
const SETTLE_NS: u64 = 2_000_000_000;
/// The relay's poll bound: a doorbell the IRQ thread could not ring costs this much.
const BACKSTOP_MS: i32 = 1000;
const IRQ_STACK: u64 = 32 * 1024;
const E_AGAIN: i64 = 11;

const _: () = assert!(QSIZE_USZ * 16 <= AVAIL_OFF as usize);
const _: () = assert!(AVAIL_OFF as usize + 6 + 2 * QSIZE_USZ <= USED_OFF as usize);
const _: () = assert!(USED_OFF as usize + 6 + 8 * QSIZE_USZ <= PAGE);
const _: () = assert!(QSIZE_USZ * CTRL_SLOT <= PAGE);
const _: () = assert!(POOL <= 256 * 1024);
const _: () = assert!(2 + MAX_CONNS <= 64, "t_poll takes at most 64");

#[inline(always)]
unsafe fn r8(a: u64) -> u8 {
    mmio_read8(a)
}
#[inline(always)]
unsafe fn r16(a: u64) -> u16 {
    mmio_read16(a)
}
#[inline(always)]
unsafe fn r32(a: u64) -> u32 {
    mmio_read32(a)
}
#[inline(always)]
unsafe fn w8(a: u64, v: u8) {
    mmio_write8(a, v)
}
#[inline(always)]
unsafe fn w16(a: u64, v: u16) {
    mmio_write16(a, v)
}
#[inline(always)]
unsafe fn w32(a: u64, v: u32) {
    mmio_write32(a, v)
}
#[inline(always)]
unsafe fn w64(a: u64, v: u64) {
    mmio_write64(a, v)
}
#[inline(always)]
fn dsb_sy() {
    unsafe { core::arch::asm!("dsb sy", options(nostack, preserves_flags)) }
}

fn rx_queue(port: u32) -> u16 {
    if port == 0 {
        0
    } else {
        (2 * port + 2) as u16
    }
}

/// One split virtqueue in its page of the pool.
struct Vq {
    q: u16,
    va: u64,
    notify: u64,
    avail_idx: u16,
    used_idx: u16,
}

impl Vq {
    unsafe fn set_desc(&self, i: u16, pa: u64, len: u32, flags: u16) {
        let d = self.va + 16 * u64::from(i);
        w64(d, pa);
        w32(d + 8, len);
        w16(d + 12, flags);
        w16(d + 14, 0);
    }

    /// Offer descriptor `i` to the device; `kick` tells it.
    unsafe fn offer(&mut self, i: u16) {
        let slot = u64::from(self.avail_idx % QSIZE);
        w16(self.va + AVAIL_OFF + 4 + 2 * slot, i);
        dsb_sy();
        self.avail_idx = self.avail_idx.wrapping_add(1);
        w16(self.va + AVAIL_OFF + 2, self.avail_idx);
    }

    unsafe fn kick(&self) {
        dsb_sy();
        w16(self.notify, self.q);
    }

    /// The next used entry, as the device wrote it: (descriptor id, length).
    unsafe fn used(&mut self) -> Option<(u32, u32)> {
        let idx = r16(self.va + USED_OFF + 2);
        virtio_rmb();
        if idx == self.used_idx {
            return None;
        }
        let e = self.va + USED_OFF + 4 + 8 * u64::from(self.used_idx % QSIZE);
        let id = r32(e);
        let len = r32(e + 4);
        self.used_idx = self.used_idx.wrapping_add(1);
        Some((id, len))
    }
}

struct Device {
    _pci: PciDev,
    _pool: Dma,
    pool_va: u64,
    pool_pa: u64,
    common: u64,
    notify_base: u64,
    notify_len: u32,
    notify_mul: u64,
    isr_va: u64,
    irq_mode: PciIrqMode,
    ports: u32,
    ctrl_rx: Vq,
    ctrl_tx: Vq,
    /// Control transmit slots the device holds.
    ctrl_busy: [bool; QSIZE_USZ],
    /// The line's queues, once the port is known.
    port_rx: Option<Vq>,
    port_tx: Option<Vq>,
    /// A port was taken once: its rings' indexes live on in the device, so a port the
    /// device removes and adds again is not taken again until the next bind.
    taken: bool,
    /// The doorbell of every queue set up, indexed by queue.
    queue_notify: [u64; QUEUES_MAX],
    tx_busy: [bool; QSIZE_USZ],
    bad_used: u64,
}

impl Device {
    fn open(res: &BoundResources, va: &mut DriverVa) -> Result<(Device, PciIrq), Error> {
        let pci = unsafe { PciDev::claim(VIRTIO_DEVICE_ID_CONSOLE, BAR_WINDOW_VA) }.map_err(|e| {
            say!("virtio-serial: virtio console claim failed: {:?}", e);
            Error::Hardware
        })?;
        let (common, common_len) = pci.region(PciRegion::Common).ok_or(Error::Hardware)?;
        let (notify_base, notify_len) = pci.region(PciRegion::Notify).ok_or(Error::Hardware)?;
        let (isr_va, isr_len) = pci.region(PciRegion::Isr).ok_or(Error::Hardware)?;
        let (dev_va, dev_len) = pci.region(PciRegion::Device).ok_or(Error::Hardware)?;
        if common_len < CCFG_MIN_LEN || isr_len < 1 || dev_len < CFG_MIN_LEN {
            say!("virtio-serial: a register region is too small ({} {} {})", common_len, isr_len, dev_len);
            return Err(Error::Hardware);
        }
        // Which ports get queues: the device's count (its config is valid before the
        // handshake; QEMU fills it whether or not MULTIPORT is taken) and its queue count.
        let max_nr_ports = unsafe { r32(dev_va + CFG_MAX_NR_PORTS) };
        let num_queues = u32::from(unsafe { r16(common + CCFG_NUM_QUEUES) });
        let ports = max_nr_ports.min(PORTS_MAX).min(num_queues.saturating_sub(2) / 2);
        if ports < 2 {
            say!(
                "virtio-serial: the device has {} port(s) and {} queues: no named port can be on it",
                max_nr_ports, num_queues
            );
            return Err(Error::Hardware);
        }
        let mut queues: Vec<u16> = Vec::new();
        for p in 0..ports {
            queues.push(rx_queue(p));
            queues.push(rx_queue(p) + 1);
        }
        queues.push(CTRL_RX);
        queues.push(CTRL_TX);
        queues.sort_unstable();
        let irq = PciIrq::for_virtio(&pci, &queues).map_err(|_| {
            say!("virtio-serial: PCI interrupt endpoint creation failed");
            Error::Hardware
        })?;
        let pool = alloc_dma(res, POOL, va)?;
        let pool_va = pool.base_va() as u64;
        let pool_pa = pool.paddr();
        let mut off = 0usize;
        while off < POOL {
            unsafe { w8(pool_va + off as u64, 0) };
            off += PAGE;
        }
        unsafe {
            w8(common + CCFG_DEVICE_STATUS, STATUS_ACKNOWLEDGE);
            w8(common + CCFG_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
            w32(common + CCFG_DEVICE_FEATURE_SELECT, 0);
            let lo = r32(common + CCFG_DEVICE_FEATURE);
            w32(common + CCFG_DEVICE_FEATURE_SELECT, 1);
            let hi = r32(common + CCFG_DEVICE_FEATURE);
            if hi & VIRTIO_F_VERSION_1_BIT_HI == 0 || lo & VIRTIO_CONSOLE_F_MULTIPORT == 0 {
                say!("virtio-serial: the device lacks VERSION_1 or MULTIPORT (lo=0x{:08x} hi=0x{:08x})", lo, hi);
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }
            w32(common + CCFG_DRIVER_FEATURE_SELECT, 0);
            w32(common + CCFG_DRIVER_FEATURE, VIRTIO_CONSOLE_F_MULTIPORT);
            w32(common + CCFG_DRIVER_FEATURE_SELECT, 1);
            w32(common + CCFG_DRIVER_FEATURE, VIRTIO_F_VERSION_1_BIT_HI);
            w8(common + CCFG_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
            if r8(common + CCFG_DEVICE_STATUS) & STATUS_FEATURES_OK == 0 {
                say!("virtio-serial: FEATURES_OK refused");
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }
        }
        let mut dev = Device {
            _pci: pci,
            _pool: pool,
            pool_va,
            pool_pa,
            common,
            notify_base,
            notify_len,
            notify_mul: 0,
            isr_va,
            irq_mode: irq.mode(),
            ports,
            ctrl_rx: Vq { q: CTRL_RX, va: 0, notify: 0, avail_idx: 0, used_idx: 0 },
            ctrl_tx: Vq { q: CTRL_TX, va: 0, notify: 0, avail_idx: 0, used_idx: 0 },
            ctrl_busy: [false; QSIZE_USZ],
            port_rx: None,
            port_tx: None,
            taken: false,
            queue_notify: [0; QUEUES_MAX],
            tx_busy: [false; QSIZE_USZ],
            bad_used: 0,
        };
        dev.notify_mul = u64::from(dev._pci.notify_off_multiplier());
        for &q in &queues {
            let notify = dev.setup_queue(q).ok_or_else(|| {
                say!("virtio-serial: queue {} setup failed", q);
                unsafe { w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED) };
                Error::Hardware
            })?;
            dev.queue_notify[q as usize] = notify;
        }
        dev.ctrl_rx = dev.vq(CTRL_RX);
        dev.ctrl_tx = dev.vq(CTRL_TX);
        unsafe {
            w8(
                common + CCFG_DEVICE_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK,
            );
            // Every control receive buffer offered: the device drops a control message it has
            // no buffer for.
            for i in 0..QSIZE {
                let pa = pool_pa + (CRX_OFF + usize::from(i) * CTRL_BUF) as u64;
                dev.ctrl_rx.set_desc(i, pa, CTRL_BUF as u32, VIRTQ_DESC_F_WRITE);
                dev.ctrl_rx.offer(i);
            }
            dev.ctrl_rx.kick();
        }
        say!(
            "virtio-serial: virtio console up: {} of {} port(s) have queues, irq mode={:?}",
            ports, max_nr_ports, irq.mode()
        );
        Ok((dev, irq))
    }

    /// Queue `q` at page `q` of the pool; its doorbell, inside the notify region.
    fn setup_queue(&self, q: u16) -> Option<u64> {
        let page = self.pool_pa + (usize::from(q) * PAGE) as u64;
        let off = unsafe {
            w16(self.common + CCFG_QUEUE_SELECT, q);
            if r16(self.common + CCFG_QUEUE_SIZE) < QSIZE {
                return None;
            }
            w16(self.common + CCFG_QUEUE_SIZE, QSIZE);
            w64(self.common + CCFG_QUEUE_DESC, page);
            w64(self.common + CCFG_QUEUE_DRIVER, page + AVAIL_OFF);
            w64(self.common + CCFG_QUEUE_DEVICE, page + USED_OFF);
            let off = r16(self.common + CCFG_QUEUE_NOTIFY_OFF);
            w16(self.common + CCFG_QUEUE_ENABLE, 1);
            off
        };
        let o = u64::from(off) * self.notify_mul;
        if o + 2 > u64::from(self.notify_len) {
            return None;
        }
        Some(self.notify_base + o)
    }

    fn vq(&self, q: u16) -> Vq {
        Vq { q, va: self.pool_va + (usize::from(q) * PAGE) as u64, notify: self.queue_notify[q as usize], avail_idx: 0, used_idx: 0 }
    }

    fn irq_ack(&self) -> IrqAck {
        IrqAck { va: self.isr_va, intx: self.irq_mode == PciIrqMode::Intx }
    }

    fn send_control(&mut self, c: Control) {
        // Reap first: a slot is reused only once the device gave it back.
        self.reap_ctrl_tx();
        let Some(i) = self.ctrl_busy.iter().position(|b| !b) else {
            say!("virtio-serial: no control slot free; event {} for port {} not sent", c.event, c.id);
            return;
        };
        let off = CTX_OFF + i * CTRL_SLOT;
        let bytes = console::encode(c);
        unsafe {
            core::ptr::copy_nonoverlapping(bytes.as_ptr(), (self.pool_va + off as u64) as *mut u8, bytes.len());
            self.ctrl_tx.set_desc(i as u16, self.pool_pa + off as u64, bytes.len() as u32, 0);
            self.ctrl_tx.offer(i as u16);
            self.ctrl_tx.kick();
        }
        self.ctrl_busy[i] = true;
    }

    fn reap_ctrl_tx(&mut self) {
        for _ in 0..QSIZE {
            let Some((id, _)) = (unsafe { self.ctrl_tx.used() }) else { return };
            match self.ctrl_busy.get_mut(id as usize) {
                Some(b) if *b => *b = false,
                _ => self.bad_used += 1,
            }
        }
    }

    /// The next control message from the device, its buffer offered again after it is read.
    fn next_control(&mut self) -> Option<(Control, Vec<u8>)> {
        loop {
            let (id, len) = unsafe { self.ctrl_rx.used() }?;
            if id >= u32::from(QSIZE) || len as usize > CTRL_BUF {
                self.bad_used += 1;
                continue;
            }
            let off = CRX_OFF + id as usize * CTRL_BUF;
            let buf = unsafe { core::slice::from_raw_parts((self.pool_va + off as u64) as *const u8, len as usize) };
            let got = console::decode(buf).map(|(c, rest)| (c, rest.to_vec()));
            unsafe {
                self.ctrl_rx.offer(id as u16);
                self.ctrl_rx.kick();
            }
            match got {
                Some(m) => return Some(m),
                None => self.bad_used += 1,
            }
        }
    }

    /// The line's port is `port`: its queues, and every receive buffer offered. False when a
    /// port was taken before.
    fn take_port(&mut self, port: u32) -> bool {
        if self.taken {
            return false;
        }
        self.taken = true;
        let mut rx = self.vq(rx_queue(port));
        let tx = self.vq(rx_queue(port) + 1);
        unsafe {
            for i in 0..QSIZE {
                let pa = self.pool_pa + (PRX_OFF + usize::from(i) * DATA_BUF) as u64;
                rx.set_desc(i, pa, DATA_BUF as u32, VIRTQ_DESC_F_WRITE);
                rx.offer(i);
            }
            rx.kick();
        }
        self.port_rx = Some(rx);
        self.port_tx = Some(tx);
        self.tx_busy = [false; QSIZE_USZ];
        true
    }

    /// The port was removed. Its queues stay as the device left them.
    fn drop_port(&mut self) {
        self.port_rx = None;
        self.port_tx = None;
    }

    /// Bytes received on the line's port, each buffer offered again once copied. `discard`
    /// drops them (what came before PORT_OPEN changed belongs to the connection before).
    fn take_rx(&mut self, relay: &mut Relay, discard: bool) -> Option<macline::Malformed> {
        let mut why = None;
        let Some(rx) = self.port_rx.as_mut() else { return None };
        let mut offered = false;
        for _ in 0..QSIZE {
            let Some((id, len)) = (unsafe { rx.used() }) else { break };
            if id >= u32::from(QSIZE) || len as usize > DATA_BUF {
                self.bad_used += 1;
                continue;
            }
            let off = PRX_OFF + id as usize * DATA_BUF;
            if !discard && why.is_none() {
                let buf = unsafe { core::slice::from_raw_parts((self.pool_va + off as u64) as *const u8, len as usize) };
                why = relay.rx(buf);
            }
            unsafe { rx.offer(id as u16) };
            offered = true;
        }
        if offered {
            unsafe { rx.kick() };
        }
        why
    }

    /// Frames toward the app, whole frames packed into each free transmit buffer.
    fn push_tx(&mut self, relay: &mut Relay) {
        let Some(tx) = self.port_tx.as_mut() else { return };
        for _ in 0..QSIZE {
            let Some((id, _)) = (unsafe { tx.used() }) else { break };
            match self.tx_busy.get_mut(id as usize) {
                Some(b) if *b => *b = false,
                _ => self.bad_used += 1,
            }
        }
        let mut kicked = false;
        while relay.out_bytes() > 0 {
            let Some(i) = self.tx_busy.iter().position(|b| !b) else { break };
            let base = self.pool_va + (PTX_OFF + i * DATA_BUF) as u64;
            let mut used = 0usize;
            while let Some(f) = relay.next_frame(DATA_BUF - used) {
                unsafe { core::ptr::copy_nonoverlapping(f.as_ptr(), (base + used as u64) as *mut u8, f.len()) };
                used += f.len();
            }
            if used == 0 {
                break;
            }
            let pa = self.pool_pa + (PTX_OFF + i * DATA_BUF) as u64;
            unsafe {
                tx.set_desc(i as u16, pa, used as u32, 0);
                tx.offer(i as u16);
            }
            self.tx_busy[i] = true;
            kicked = true;
        }
        if kicked {
            unsafe { tx.kick() };
        }
    }
}

// The IRQ thread holds a PCI reference too; reset the device before the pool can go.
impl Drop for Device {
    fn drop(&mut self) {
        unsafe {
            w8(self.common + CCFG_DEVICE_STATUS, 0);
        }
        dsb_sy();
    }
}

struct IrqAck {
    va: u64,
    intx: bool,
}

impl IrqAck {
    fn acknowledge(&self) {
        if self.intx {
            let _ = unsafe { r8(self.va) };
        }
        dsb_sy();
    }
}

struct IrqCtx {
    irq: PciIrq,
    ack: IrqAck,
    /// The pipe's write end, nonblocking: a full pipe is a doorbell already rung.
    bell: i64,
}

extern "C" fn irq_entry(arg: u64) -> ! {
    // SAFETY: the IrqCtx leaked in serve(); only this thread touches it.
    let ctx: &IrqCtx = unsafe { &*(arg as *const IrqCtx) };
    ctx.ack.acknowledge();
    if ctx.irq.arm().is_err() {
        say!("virtio-serial: PCI interrupt arm failed; the relay keeps its backstop");
        thread::exit_self();
    }
    loop {
        match ctx.irq.wait() {
            Ok(Some(event)) => {
                ctx.ack.acknowledge();
                let _ = unsafe { t_write(ctx.bell, [1u8].as_ptr(), 1) };
                match ctx.irq.complete(event) {
                    Ok(()) | Err(libthyla_rs::err::Error::WouldBlock) => {}
                    Err(_) => {
                        say!("virtio-serial: PCI interrupt completion failed; the relay keeps its backstop");
                        thread::exit_self();
                    }
                }
            }
            Ok(None) => {}
            Err(_) => {
                say!("virtio-serial: PCI interrupt wait ended; the relay keeps its backstop");
                thread::exit_self();
            }
        }
    }
}

struct VirtioSerial {
    dev: Device,
    irq: PciIrq,
}

impl Driver for VirtioSerial {
    fn probe(res: &BoundResources) -> Result<Self, Error> {
        let mut va = DriverVa::new();
        let (dev, irq) = Device::open(res, &mut va)?;
        Ok(VirtioSerial { dev, irq })
    }

    fn serve(self, _res: &BoundResources) -> Result<(), Error> {
        let VirtioSerial { dev, irq } = self;
        let (bell_r, bell_w) = unsafe { t_pipe() };
        if bell_r < 0 || bell_w < 0 {
            say!("virtio-serial: doorbell pipe failed");
            return Err(Error::Hardware);
        }
        unsafe {
            t_set_nonblock(bell_r, true);
            t_set_nonblock(bell_w, true);
        }
        let ctx: &'static IrqCtx = Box::leak(Box::new(IrqCtx { irq, ack: dev.irq_ack(), bell: bell_w }));
        let stack = unsafe { t_burrow_attach(IRQ_STACK) };
        if stack < 0 {
            say!("virtio-serial: irq-thread stack attach failed");
            return Err(Error::Hardware);
        }
        let spawned = unsafe {
            thread::spawn_raw(irq_entry as *const () as u64, stack as u64 + IRQ_STACK, ctx as *const IrqCtx as u64, 0)
        };
        if spawned.is_err() {
            say!("virtio-serial: irq-thread spawn failed");
            return Err(Error::Hardware);
        }
        let mut line = LineDriver {
            con: Console::new(dev.ports),
            dev,
            relay: Relay::new(MAXFRAME, FRAME_MAX),
            listener: -1,
            conns: Vec::new(),
            bell: bell_r,
        };
        let ready = Console::ready();
        line.dev.send_control(ready);
        line.run()
    }
}

struct LineDriver {
    dev: Device,
    con: Console,
    relay: Relay,
    /// The /srv/mac listener, or -1 while the port is not there.
    listener: i64,
    conns: Vec<Key>,
    bell: i64,
}

impl LineDriver {
    fn run(&mut self) -> ! {
        let start = monotonic_ns();
        let mut ready = false;
        let mut pollfds: Vec<TPollFd> = Vec::with_capacity(2 + MAX_CONNS);
        loop {
            self.service_device();
            if !ready {
                let waited = monotonic_ns().saturating_sub(start);
                let named = self.con.settled() || waited >= SETTLE_NS;
                let hello = !self.con.host_open() || self.relay.state() == State::Up || waited >= 2 * SETTLE_NS;
                if named && hello {
                    ready = true;
                    match self.con.target() {
                        Some(p) => say!(
                            "virtio-serial: /srv/mac on port {}; the Mac app is {}",
                            p,
                            if self.relay.state() == State::Up { "on the line" } else { "not on the line" }
                        ),
                        None => say!("virtio-serial: no port named org.thylacine.mac; nothing is posted"),
                    }
                    let mut out = libthyla_rs::io::stdout();
                    let _ = out.write_all(b"READY\n");
                }
            }
            pollfds.clear();
            pollfds.push(TPollFd { fd: self.bell as i32, events: T_POLLIN, revents: 0 });
            if self.listener >= 0 {
                pollfds.push(TPollFd { fd: self.listener as i32, events: T_POLLIN, revents: 0 });
            }
            let reading = self.relay.out_bytes() < OUT_HIGH;
            for &k in &self.conns {
                let mut ev = 0;
                if reading {
                    ev |= T_POLLIN;
                }
                if !self.relay.pending(k).is_empty() {
                    ev |= T_POLLOUT;
                }
                pollfds.push(TPollFd { fd: k as i32, events: ev, revents: 0 });
            }
            let timeout = if ready { BACKSTOP_MS } else { 20 };
            let n = unsafe { t_poll(pollfds.as_mut_ptr(), pollfds.len(), timeout) };
            if n < 0 {
                continue;
            }
            if pollfds[0].revents & T_POLLIN != 0 {
                let mut drain = [0u8; 64];
                while unsafe { t_read(self.bell, drain.as_mut_ptr(), drain.len()) } > 0 {}
            }
            let mut at = 1;
            if self.listener >= 0 {
                if pollfds[1].revents & T_POLLIN != 0 {
                    self.accept();
                }
                at = 2;
            }
            for i in at..pollfds.len() {
                let (k, rev) = (pollfds[i].fd as Key, pollfds[i].revents);
                if rev & (T_POLLHUP | T_POLLERR) != 0 {
                    self.relay.guest_hangup(k);
                    continue;
                }
                if rev & T_POLLOUT != 0 {
                    self.write_guest(k);
                }
                if rev & T_POLLIN != 0 {
                    self.read_guest(k);
                }
            }
            self.hang_up();
        }
    }

    /// The device's news: control messages first (so bytes delivered before PORT_OPEN changed
    /// are dropped with the connection they belong to), then the line's bytes, then frames out.
    fn service_device(&mut self) {
        while let Some((c, rest)) = self.dev.next_control() {
            for act in self.con.event(c, &rest) {
                match act {
                    Act::Send(m) => self.dev.send_control(m),
                    Act::Target(p) => {
                        if self.dev.take_port(p) {
                            self.post();
                        } else {
                            say!("virtio-serial: the port came back as port {}; it is taken again only when the driver binds again", p);
                        }
                    }
                    Act::HostOpen => {
                        // Whatever came before this connection's first frame is not its.
                        self.dev.take_rx(&mut self.relay, true);
                        self.relay.host_open();
                        say!("virtio-serial: the Mac app is on the line");
                    }
                    Act::HostClosed => {
                        self.relay.host_closed();
                        self.dev.take_rx(&mut self.relay, true);
                        say!("virtio-serial: the Mac app went away; every channel is closed");
                    }
                    Act::TargetGone => {
                        self.relay.host_closed();
                        self.dev.drop_port();
                        self.unpost();
                        say!("virtio-serial: the port was removed; /srv/mac is withdrawn");
                    }
                }
            }
        }
        if let Some(why) = self.dev.take_rx(&mut self.relay, false) {
            say!("virtio-serial: the line is malformed ({:?}); RESET sent", why);
        }
        self.dev.push_tx(&mut self.relay);
        self.hang_up();
    }

    fn post(&mut self) {
        if self.listener >= 0 {
            return;
        }
        let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
        if srv < 0 {
            say!("virtio-serial: /srv did not open");
            return;
        }
        let l = unsafe { t_walk_create(srv, b"mac".as_ptr(), 3, T_OREAD, 0) };
        let _ = unsafe { t_close(srv) };
        if l < 0 {
            say!("virtio-serial: /srv/mac post failed ({})", l);
            return;
        }
        self.listener = l;
    }

    fn unpost(&mut self) {
        if self.listener >= 0 {
            let _ = unsafe { t_close(self.listener) };
            self.listener = -1;
        }
    }

    fn accept(&mut self) {
        let h = unsafe { t_srv_accept(self.listener) };
        if h < 0 {
            return;
        }
        // While the line is not up the opener's handshake fails at once (EIO), not at the
        // kernel's 5 s deadline.
        if self.conns.len() >= MAX_CONNS || !self.relay.accept(h) {
            let _ = unsafe { t_close(h) };
            return;
        }
        unsafe { t_set_nonblock(h, true) };
        self.conns.push(h);
        self.dev.push_tx(&mut self.relay);
    }

    fn read_guest(&mut self, k: Key) {
        let mut buf = [0u8; READ_CHUNK];
        let n = unsafe { t_read(k, buf.as_mut_ptr(), buf.len()) };
        if n > 0 {
            self.relay.guest_data(k, &buf[..n as usize]);
            self.dev.push_tx(&mut self.relay);
        } else if n != -E_AGAIN {
            // 0 is EOF on a nonblocking connection.
            self.relay.guest_hangup(k);
        }
    }

    fn write_guest(&mut self, k: Key) {
        let n = {
            let p = self.relay.pending(k);
            if p.is_empty() {
                return;
            }
            unsafe { t_write(k, p.as_ptr(), p.len()) }
        };
        if n > 0 {
            self.relay.wrote(k, n as usize);
        } else if n != -E_AGAIN {
            self.relay.guest_hangup(k);
        }
    }

    /// Closes every connection the relay let go, and pushes the CLOSE frames it made.
    fn hang_up(&mut self) {
        let gone = self.relay.take_hangups();
        if gone.is_empty() {
            return;
        }
        for k in gone {
            if let Some(i) = self.conns.iter().position(|&c| c == k) {
                self.conns.swap_remove(i);
                let _ = unsafe { t_close(k) };
            }
        }
        self.dev.push_tx(&mut self.relay);
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run::<VirtioSerial>()
}
