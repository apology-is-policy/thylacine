//! Boot-trusted seat owner. Tapestry receives only a DMA allowance and the
//! normal broker connection; display/input hardware never leaves this process.
#![no_std]
#![no_main]
extern crate alloc;
use alloc::{vec::Vec, vec};
use libdriver::{driver::{run, Driver}, resource::BoundResources, Error};
use libthyla_rs::{io::Write, *};
use lictor::{backend::{gpu::Gpu, input::InputDev, intake, screen::Screen, device::Device,
    server::Conn, seat::{Seat, Input}}, endpoint as ep, gpu_api::{admit, Request, Stats}, wire::Wire};
#[global_allocator]
static ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;
const MAX_CONNS: usize = 16;
struct Lictor {
    // Preserve this order: the device reset precedes all private DMA release.
    device: Device,
    screen: Screen,
    inputs: Vec<Input>,
    seat: Seat,
}
impl Driver for Lictor {
    fn probe(resources: &BoundResources) -> Result<Self, Error> {
        let mut status = ep::Message::default();
        ep::call(ep::STATUS, &mut status).map_err(|_| Error::Hardware)?;
        let mut gpu = Gpu::probe(0x0080_0000, 0x0150_0000, 0x0220_0000, 0x0151_0000)?;
        let windows = [(0x00e0_0000, 0x0152_0000), (0x0160_0000, 0x0153_0000), (0x01c0_0000, 0x0154_0000)];
        // This QEMU backend qualifies one GPU and at most three input
        // functions. Never silently ignore a granted input device.
        let count = resources.pci_extra.len();
        if resources.pci.is_none() || count == 0 || count > windows.len() { return Err(Error::Hardware); }
        let mut inputs = Vec::new();
        let mut devices = Vec::new();
        for (index, &(bar, dma)) in windows[..count].iter().enumerate() {
            let device = InputDev::probe(index as u32, bar, dma)?;
            inputs.push(Input::new(&device));
            devices.push(device);
        }
        if inputs.iter().filter(|i| i.keyboard).count() != 1 { return Err(Error::Hardware); }
        let screen = Screen::new(&mut gpu, lictor::backend::screen::declared_scale())?;
        #[cfg_attr(not(feature = "test-mode"), allow(unused_mut))]
        let mut device = Device::new(gpu, 0)?;
        #[cfg(feature = "test-mode")]
        {
            let mut buf = [0u8; 1024];
            let line = lictor::backend::bootargs(&mut buf);
            device.test_declare(lictor::gpu_api::bootarg_declared(line, lictor::gpu_api::TEST_LEVERS_WORD));
            device.test_declare_chained(lictor::gpu_api::bootarg_declared(line, lictor::gpu_api::TEST_CHAINED_WORD));
            device.test_declare_restore(lictor::gpu_api::bootarg_declared(line, lictor::gpu_api::TEST_RESTORE_WORD));
        }
        // The input devices go to their own thread, so a GPU wait here never leaves a
        // ring undrained. Last: nothing after it can fail the probe.
        intake::start(devices)?;
        Ok(Self { device, screen, inputs, seat: Seat::default() })
    }
    fn serve(mut self, _resources: &BoundResources) -> Result<(), Error> {
        let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
        if srv < 0 { return Err(Error::Hardware); }
        let listener = unsafe { t_walk_create(srv, b"lictor".as_ptr(), 6, T_OREAD, 0) };
        unsafe { t_close(srv); }
        if listener < 0 { lictor::backend::diagnostic("lictor: listener post refused\n"); return Err(Error::Hardware); }
        lictor::backend::diagnostic("lictor: broker posted\n");
        // Narrowed hardware drivers are leaves. Warden starts the DMA-only
        // compositor after this readiness signal and stamps its kernel role.
        libthyla_rs::io::stdout().write_all(b"READY\n").map_err(|_| Error::Hardware)?;
        let mut conns: Vec<Conn> = Vec::new();
        #[cfg(feature = "test-mode")]
        let mut witnessed_park = 0;
        loop {
            self.seat.input(&mut self.inputs);
            #[cfg(feature = "test-mode")]
            if core::mem::take(&mut self.seat.test_blind) { self.device.test_blind_loop(); }
            self.device.reap();
            self.seat.step(&mut self.device, &mut self.screen, &mut self.inputs);
            // After the seat's look at the chord, so a pass's device time
            // never stands before it; outside any request, so a stall met
            // here is no request's.
            if self.seat.phase == 0 { self.device.pay_owed_frees(); }
            // Only while no trusted episode is up, beginning or owed its
            // restoration: entering one must find the screen whole.
            if self.seat.phase == 0 { self.screen.follow(&mut self.device.gpu); }
            for conn in &mut conns {
                conn.advance(&self.device);
                #[cfg(feature = "test-mode")]
                if self.seat.phase == 0 {
                    if let Some((_, req)) = conn.pending.as_ref() { self.device.test_latch_consider(req); }
                }
                let runnable = conn.pending.as_ref().is_some_and(|(_, req)| {
                    // A failed seat parks normal work exactly like a live
                    // episode: it recovers to NORMAL, and an error reply here
                    // would make the compositor give up on a display that is
                    // about to come back.
                    admit(req, self.seat.phase, self.device.retirement_hold(req), conn.parked_ms())
                });
                if !runnable {
                    #[cfg(feature = "test-mode")]
                    if conn.pending.is_some() && self.seat.phase == 1
                        && witnessed_park != self.seat.generation {
                        // This thread alone can ACK the hardware seat. Logging
                        // before that ACK cannot race entry to EXCLUSIVE.
                        lictor::backend::diagnostic("lictor: ordinary GPU RPC parked before cancellation ACK\n");
                        witnessed_park = self.seat.generation;
                    }
                    continue;
                }
                let (sequence, request) = conn.pending.take().unwrap();
                let stalls_begun = self.device.gpu.stalls();
                #[cfg(feature = "test-mode")]
                let shot = self.device.test_stall_ride(conn.fd);
                let result = match request {
                    Request::InputInfo { index } => {
                        let info = self.inputs.get(index as usize).map(|i| i.info);
                        let mut bytes = Vec::new(); info.put(&mut bytes); Ok(bytes)
                    }
                    Request::InputDrain { index } => {
                        let mut bytes = Vec::new();
                        if let Some(input) = self.inputs.get_mut(index as usize) {
                            // The seat and the events in one reply: a client
                            // that asks the seat and then drains can have a
                            // whole episode between its two requests.
                            let events = core::mem::take(&mut input.events);
                            let reply: lictor::gpu_api::Drained = (self.seat.generation, self.seat.phase, events);
                            reply.put(&mut bytes); Ok(bytes)
                        } else { Err(Error::BadField) }
                    }
                    Request::SeatState => {
                        let mut bytes = Vec::new(); (self.seat.generation, self.seat.phase).put(&mut bytes); Ok(bytes)
                    }
                    Request::SeatQuiesced { generation } => {
                        // Recheck designation at use, not just connection accept.
                        let mut peer = TSrvPeerInfo::default();
                        let mut designated = ep::Message::default();
                        let known = ep::call(ep::CLIENT, &mut designated).is_ok();
                        if !known || unsafe { t_srv_peer(conn.fd, &mut peer) } != 0
                            || peer.alive == 0 || peer.pid != designated.code || peer.stripes != designated.sequence {
                            Err(Error::Hardware)
                        } else {
                            let accepted = self.seat.acknowledge_cancellation(generation);
                            let mut bytes = Vec::new(); accepted.put(&mut bytes); Ok(bytes)
                        }
                    }
                    request if self.seat.phase == 0 => self.device.execute(conn.fd, request),
                    _ => Err(Error::Hardware),
                };
                #[cfg(feature = "test-mode")]
                if shot { self.device.test_stall_settle(); }
                conn.finish(sequence, Stats { stalls_begun, ..Stats::from(&self.device.gpu) }, result);
            }
            #[cfg(feature = "test-mode")]
            for conn in conns.iter().filter(|c| !c.alive()) { self.device.test_stall_forget(conn.fd); }
            conns.retain(Conn::alive);
            let mut poll = vec![TPollFd { fd: listener as i32, events: T_POLLIN, revents: 0 }];
            let base = poll.len();
            for conn in &conns { poll.push(TPollFd { fd: conn.fd as i32, events: conn.events(), revents: 0 }); }
            let result = unsafe { t_poll(poll.as_mut_ptr(), poll.len(), 10) };
            if result < 0 { lictor::backend::diagnostic(&alloc::format!("lictor: poll failed {}\n", result)); return Err(Error::Hardware); }
            for (i, conn) in conns.iter_mut().enumerate() { conn.io(poll[base + i].revents); }
            if poll[0].revents & T_POLLIN != 0 {
                let fd = unsafe { t_srv_accept(listener) };
                if fd >= 0 {
                    let mut peer = TSrvPeerInfo::default();
                    let mut designated = ep::Message::default();
                    let known = ep::call(ep::CLIENT, &mut designated).is_ok();
                    let nonblocking = unsafe { t_set_nonblock(fd, true) } == 0;
                    let admitted = nonblocking && unsafe { t_srv_peer(fd, &mut peer) } == 0 && peer.alive != 0
                        && known && peer.pid == designated.code && peer.stripes == designated.sequence
                        && self.device.bind_owner(peer.stripes)
                        && conns.len() < MAX_CONNS;
                    if admitted { lictor::backend::diagnostic("lictor: accepted compositor peer\n"); conns.push(Conn::new(fd)); } else {
                        lictor::backend::diagnostic(&alloc::format!("lictor: peer refused pid={} designated={} known={} alive={}\n", peer.pid, designated.code, known, peer.alive));
                        unsafe { t_close(fd); }
                    }
                }
            }
        }
    }
}
#[no_mangle]
pub extern "C" fn rs_main() -> i64 { run::<Lictor>() }
