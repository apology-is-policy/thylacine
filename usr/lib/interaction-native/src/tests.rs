//! The adapter against loom-client's simulated kernel and a scripted server speaking
//! real HIN1 records. Every wait goes through one poll loop, as a host's does: pump
//! never sleeps, so the sim's clock moves only when the loop's own poll lets it.
use super::*;
use libhalcyon::interaction_body::{Request, Response};
use libhalcyon::interaction_client::Why;
use libhalcyon::interaction_wire::{self as wire, Failure, Operation, MAX_TEXT};
use loom_client::native::POLLIN;
use loom_client::sim::{Service, Sim, SimKernel};
use std::{cell::RefCell, collections::HashMap, rc::Rc};

const TEXT: &[u8] = b"the clipboard's text";
const MS: u64 = 1_000_000;

#[derive(Default)]
struct Wire {
    record: Vec<u8>,
    reply: Vec<u8>,
    refusal: Option<i64>,
}

#[derive(Default)]
struct Script {
    clipboard: Vec<u8>,
    generation: u64,
    wires: HashMap<usize, Wire>,
    log: Vec<Operation>,
    modes: Vec<u64>,
    connects: u32,
    transfer: u64,
    begun: Vec<u8>,
    /// Never answer this operation: its reply's read stays in flight.
    park: Option<Operation>,
    /// Answer every read one byte at a time.
    trickle: bool,
    /// Refuse this operation's reply with this errno.
    refuse: Option<(Operation, i64)>,
    /// Refuse the next write outright with this errno.
    refuse_write: Option<i64>,
    fail_reply: Option<(Operation, i64)>,
    /// Answer the next request with the wrong id.
    garble: bool,
    /// The connection has ended; only a new CONNECT makes another.
    sak: bool,
    sak_answered: bool,
}

#[derive(Clone, Default)]
struct Server(Rc<RefCell<Script>>);

impl Script {
    fn answer(&mut self, fid: usize) {
        let record = core::mem::take(&mut self.wires.get_mut(&fid).unwrap().record);
        let (id, request) = Request::decode(&record).expect("the client sent a valid record");
        let op = wire::frame(&record).unwrap().0.operation;
        self.log.push(op);
        if self.park == Some(op) {
            return;
        }
        if let Some((_, errno)) = self.refuse.filter(|(o, _)| *o == op) {
            self.refuse = None;
            let w = self.wires.get_mut(&fid).unwrap();
            w.reply.clear();
            w.refusal = Some(errno);
            return;
        }
        // The request runs, and this comes on its reply's read.
        if let Some((_, errno)) = self.fail_reply.filter(|(o, _)| *o == op) {
            self.fail_reply = None;
            self.wires.get_mut(&fid).unwrap().refusal = Some(errno);
        }
        let id = if core::mem::take(&mut self.garble) {
            id + 1
        } else {
            id
        };
        let reply = match request {
            Request::Hello => Response::Hello { session: 77 }.encode(id),
            Request::Bind { .. } => Response::Bound { controller: 5 }.encode(id),
            Request::Mode { sequence, .. } => {
                self.modes.push(sequence);
                Response::Mode.encode(id)
            }
            Request::Get { .. } => {
                self.transfer += 1;
                Response::Clipboard {
                    transfer: self.transfer,
                    generation: self.generation,
                    length: self.clipboard.len() as u32,
                }
                .encode(id)
            }
            Request::Read { offset, count, .. } => {
                let start = (offset as usize).min(self.clipboard.len());
                let end = (start + count as usize).min(self.clipboard.len());
                Response::Read {
                    offset,
                    data: &self.clipboard[start..end],
                }
                .encode(id)
            }
            Request::Begin { .. } => {
                self.begun.clear();
                self.transfer += 1;
                Response::Begun {
                    transfer: self.transfer,
                }
                .encode(id)
            }
            Request::Write { data, .. } => {
                self.begun.extend_from_slice(data);
                Response::Written {
                    count: data.len() as u32,
                }
                .encode(id)
            }
            Request::Commit { expected, .. } => {
                assert_eq!(
                    expected, self.generation,
                    "commit names the current generation"
                );
                self.clipboard = core::mem::take(&mut self.begun);
                self.generation += 1;
                Response::Committed {
                    generation: self.generation,
                }
                .encode(id)
            }
            Request::Cancel { .. } => Response::Cancelled.encode(id),
            Request::Unbind { .. } => Response::Unbound.encode(id),
        };
        self.wires.get_mut(&fid).unwrap().reply = reply.unwrap();
    }
}

impl Service for Server {
    fn connected(&mut self, _scope: usize) {
        let mut s = self.0.borrow_mut();
        s.connects += 1;
        s.wires.clear();
    }
    fn write(&mut self, _scope: usize, fid: usize, offset: u64, bytes: &[u8]) -> Option<i64> {
        let mut s = self.0.borrow_mut();
        if let Some(errno) = s.refuse_write.take() {
            return Some(errno);
        }
        let w = s.wires.entry(fid).or_default();
        assert_eq!(offset as usize, w.record.len(), "contiguous from zero");
        w.record.extend_from_slice(bytes);
        if wire::frame(&w.record).is_ok() {
            s.answer(fid);
            // The server refuses on the write that finishes the record.
            let w = s.wires.get_mut(&fid).unwrap();
            if w.reply.is_empty() {
                if let Some(errno) = w.refusal.take() {
                    return Some(errno);
                }
            }
        }
        Some(bytes.len() as i64)
    }
    fn read(&mut self, _scope: usize, fid: usize, offset: u64, out: &mut [u8]) -> Option<i64> {
        let mut s = self.0.borrow_mut();
        let trickle = s.trickle;
        let w = s.wires.entry(fid).or_default();
        if let Some(errno) = w.refusal.take() {
            return Some(errno);
        }
        let at = offset as usize;
        if w.reply.is_empty() || at >= w.reply.len() {
            return None;
        }
        let mut n = (w.reply.len() - at).min(out.len());
        if trickle {
            n = 1;
        }
        out[..n].copy_from_slice(&w.reply[at..at + n]);
        if at + n == w.reply.len() {
            w.reply.clear();
        }
        Some(n as i64)
    }
}

/// paneplace.rs's limit on what a session negotiates.
const SRV_MSIZE: usize = 8192;

fn locator() -> Locator<'static> {
    Locator {
        service: b"clip",
        token: b"tok",
    }
}

fn fixture() -> (Sim, Server, Host<SimKernel>) {
    let sim = Sim::new();
    let server = Server::default();
    sim.s()
        .files
        .insert(b"tok/interaction".to_vec(), Vec::new());
    sim.s().service = Some(Box::new(server.clone()));
    sim.s().msize = Some(SRV_MSIZE);
    let host = Host::new(sim.kernel(), 5, locator()).expect("the host starts");
    (sim, server, host)
}

/// One turn of a host's loop: the single poll, bounded by what the client owes.
fn host_turn(sim: &Sim, host: &mut Host<SimKernel>) {
    let now = host.now();
    let fd = host.poll_fd().expect("the ring stays up");
    let timeout = match host.next_deadline() {
        Some(at) => at.saturating_sub(now).div_ceil(MS).min(i32::MAX as u64) as i32,
        None => -1,
    };
    let polls = sim.s().polls.len();
    use loom_client::native::Kernel;
    let events = sim.kernel().poll(fd, POLLIN, timeout);
    assert!(events >= 0, "poll: {events}");
    let now = host.now();
    host.pump(now);
    assert_eq!(
        sim.s().polls.len(),
        polls + 1,
        "only the host's own poll may sleep"
    );
}

fn until<T>(
    sim: &Sim,
    host: &mut Host<SimKernel>,
    mut done: impl FnMut(&mut Host<SimKernel>) -> Option<T>,
) -> T {
    for _ in 0..200_000 {
        if let Some(value) = done(host) {
            return value;
        }
        host_turn(sim, host);
    }
    panic!("the host never settled");
}

fn next_event(sim: &Sim, host: &mut Host<SimKernel>) -> Event {
    until(sim, host, |h| h.take_event())
}

fn copy(sim: &Sim, host: &mut Host<SimKernel>, text: &[u8]) -> Event {
    let now = host.now();
    host.copy(text.to_vec(), now).expect("the copy starts");
    next_event(sim, host)
}

fn paste(sim: &Sim, host: &mut Host<SimKernel>) -> Event {
    let now = host.now();
    host.paste(now).expect("the paste starts");
    next_event(sim, host)
}

#[test]
fn a_locator_has_exactly_one_shape() {
    assert_eq!(parse_locator(b"/srv/clip/tok/interaction"), Some(locator()));
    for bad in [
        &b""[..],
        b"/srv/clip/tok",
        b"srv/clip/tok/interaction",
        b"/srv//tok/interaction",
        b"/srv/clip//interaction",
        b"/srv/clip/tok/interaction/",
        b"/srv/clip/tok/other",
        b"/srv/../tok/interaction",
        b"/srv/clip/./interaction",
        b"/srv/cl\0ip/tok/interaction",
        b"/mnt/clip/tok/interaction",
    ] {
        assert_eq!(parse_locator(bad), None, "{bad:?}");
    }
    let long = [b'x'; 256];
    let mut path = b"/srv/clip/".to_vec();
    path.extend_from_slice(&long);
    path.extend_from_slice(b"/interaction");
    assert_eq!(parse_locator(&path), None);
}

#[test]
fn no_private_ring_is_reported_to_the_host_and_nothing_else_is_tried() {
    let sim = Sim::new();
    sim.s().setup_errno = Some(-95);
    assert_eq!(Host::new(sim.kernel(), 5, locator()).err(), Some(-95));
    assert_eq!(sim.regions(), 0, "a refused setup leaves nothing attached");
}

#[test]
fn a_service_the_registry_does_not_hold_is_refused_at_the_start() {
    let sim = Sim::new();
    let missing = Locator {
        service: b"gone",
        token: b"tok",
    };
    assert_eq!(Host::new(sim.kernel(), 5, missing).err(), Some(-2));
}

#[test]
fn a_connect_that_fails_ends_the_job_at_once_and_the_next_one_connects() {
    let (sim, server, mut host) = fixture();
    // The name resolved when the host started, and the service has gone since.
    sim.s().services.clear();
    let start = host.now();
    assert_eq!(copy(&sim, &mut host, TEXT), Event::CopyFailed(Why::Lost));
    assert_eq!(host.now(), start, "it did not wait for the handshake's deadline");
    assert_eq!(server.0.borrow().connects, 0);
    sim.s().services.push(b"clip".to_vec());
    assert_eq!(copy(&sim, &mut host, TEXT), Event::Copied { generation: 1 });
    assert_eq!(server.0.borrow().connects, 1);
}

#[test]
fn a_connect_a_closed_ring_refuses_ends_the_job_at_once() {
    let (sim, _server, mut host) = fixture();
    sim.close_under();
    let now = host.now();
    host.copy(TEXT.to_vec(), now).unwrap();
    assert_eq!(host.take_event(), Some(Event::CopyFailed(Why::Lost)));
}

#[test]
fn a_copy_and_a_paste_round_trip_through_one_poll_loop() {
    let (sim, server, mut host) = fixture();
    assert_eq!(copy(&sim, &mut host, TEXT), Event::Copied { generation: 1 });
    assert_eq!(server.0.borrow().clipboard, TEXT);
    assert_eq!(paste(&sim, &mut host), Event::Pasted(TEXT.to_vec()));
    assert_eq!(server.0.borrow().connects, 1);
    assert!(host.is_bound());
}

#[test]
fn the_whole_clipboard_crosses_in_chunks_and_records() {
    let (sim, server, mut host) = fixture();
    let text = vec![b'x'; MAX_TEXT];
    assert_eq!(
        copy(&sim, &mut host, &text),
        Event::Copied { generation: 1 }
    );
    assert_eq!(server.0.borrow().clipboard.len(), MAX_TEXT);
    assert_eq!(paste(&sim, &mut host), Event::Pasted(text));
}

#[test]
fn each_lane_walks_the_route_and_the_directories_are_clunked() {
    let (sim, server, mut host) = fixture();
    assert_eq!(copy(&sim, &mut host, TEXT), Event::Copied { generation: 1 });
    // The control and transfer lanes keep separate fids, so their records never mix.
    assert_eq!(server.0.borrow().wires.len(), 2);
    assert_eq!(sim.s().clunks, 2);
}

#[test]
fn pump_never_sleeps_on_a_server_that_answers_nothing() {
    let (sim, _server, mut host) = fixture();
    sim.s().hold = true;
    let now = host.now();
    host.copy(TEXT.to_vec(), now).unwrap();
    for _ in 0..50 {
        host.pump(now);
    }
    assert!(sim.s().polls.is_empty(), "pump slept");
    assert_eq!(host.take_event(), None);
    assert!(
        host.next_deadline().is_some(),
        "the host must be told when to come back"
    );
}

#[test]
fn a_mode_report_passes_a_parked_get() {
    let (sim, server, mut host) = fixture();
    server.0.borrow_mut().park = Some(Operation::GetClipboard);
    let now = host.now();
    host.paste(now).unwrap();
    until(&sim, &mut host, |_| {
        server
            .0
            .borrow()
            .log
            .contains(&Operation::GetClipboard)
            .then_some(())
    });
    let now = host.now();
    host.report_mode(Mode::Insert, false, "nora", now).unwrap();
    until(&sim, &mut host, |_| {
        (!server.0.borrow().modes.is_empty()).then_some(())
    });
    assert_eq!(host.take_event(), None, "the paste is still parked");
}

#[test]
fn a_server_that_stops_at_any_operation_costs_one_event_and_recovers() {
    let mut finished = 0;
    let mut unknown = 0;
    let mut stopped = 0;
    for n in 0..40 {
        let (sim, server, mut host) = fixture();
        sim.s().hold_after = Some(n);
        let start = host.now();
        let event = copy(&sim, &mut host, TEXT);
        let cost = host.now() - start;
        assert!(
            cost <= 121_000 * MS,
            "stopping at {n} took {cost} ns to report"
        );
        match event {
            Event::Copied { .. } => finished += 1,
            Event::CopyUnknown => unknown += 1,
            Event::CopyFailed(Why::TimedOut | Why::Lost) => stopped += 1,
            other => panic!("stopping at {n}: {other:?}"),
        }
        assert_eq!(
            host.take_event(),
            None,
            "stopping at {n}: exactly one event"
        );
        // The server comes back. The next copy needs nothing the old connection held.
        {
            let mut s = sim.s();
            s.hold = false;
            s.hold_after = None;
        }
        let generation = server.0.borrow().generation;
        assert_eq!(
            copy(&sim, &mut host, b"again"),
            Event::Copied {
                generation: generation + 1
            },
            "after stopping at {n}"
        );
        assert_eq!(server.0.borrow().clipboard, b"again");
    }
    assert!(stopped > 10, "stopped {stopped}");
    assert!(
        unknown >= 1,
        "no stop landed between a commit and its reply"
    );
    assert!(finished >= 1, "the sweep never reached the end of a copy");
}

#[test]
fn the_buffer_an_abort_still_holds_parks_the_next_request_and_is_no_refusal() {
    let (sim, server, mut host) = fixture();
    server.0.borrow_mut().park = Some(Operation::GetClipboard);
    let now = host.now();
    host.paste(now).unwrap();
    until(&sim, &mut host, |_| {
        server
            .0
            .borrow()
            .log
            .contains(&Operation::GetClipboard)
            .then_some(())
    });
    // Closing aborts the scope. The kernel keeps the terminal of the parked read back,
    // so that read's buffer stays lent into the next connection.
    sim.s().hold_terminals = true;
    let now = host.now();
    host.cancel(now);
    assert_eq!(host.take_event(), Some(Event::PasteFailed(Why::Cancelled)));
    server.0.borrow_mut().park = None;
    let gets = server
        .0
        .borrow()
        .log
        .iter()
        .filter(|o| **o == Operation::GetClipboard)
        .count();
    let now = host.now();
    host.paste(now).unwrap();
    until(&sim, &mut host, |h| h.is_bound().then_some(()));
    // The clock stands still here: a poll with nothing due would sleep to the deadline.
    let now = host.now();
    for _ in 0..3 {
        host.pump(now);
        assert_eq!(
            host.take_event(),
            None,
            "the lent buffer must not look like an answer"
        );
    }
    let log = server
        .0
        .borrow()
        .log
        .iter()
        .filter(|o| **o == Operation::GetClipboard)
        .count();
    assert_eq!(log, gets, "the Get waited for its buffer");
    sim.release_terminals();
    assert_eq!(next_event(&sim, &mut host), Event::Pasted(Vec::new()));
}

#[test]
fn a_server_answering_as_fast_as_it_is_asked_cannot_hold_a_pump() {
    let (sim, server, mut host) = fixture();
    assert_eq!(copy(&sim, &mut host, TEXT), Event::Copied { generation: 1 });
    sim.s().step_on_enter = true;
    server.0.borrow_mut().trickle = true;
    let now = host.now();
    host.paste(now).unwrap();
    assert_eq!(
        host.take_event(),
        None,
        "one pump finished a whole trickled paste"
    );
    assert_eq!(next_event(&sim, &mut host), Event::Pasted(TEXT.to_vec()));
}

#[test]
fn dropping_the_host_ends_the_connection_whatever_the_server_is_doing() {
    let (sim, server, mut host) = fixture();
    server.0.borrow_mut().park = Some(Operation::GetClipboard);
    let now = host.now();
    host.paste(now).unwrap();
    until(&sim, &mut host, |_| {
        server
            .0
            .borrow()
            .log
            .contains(&Operation::GetClipboard)
            .then_some(())
    });
    assert!(
        sim.pending_ops() > 0,
        "the read of the parked Get is in flight"
    );
    drop(host);
    assert_eq!(sim.pending_ops(), 0);
    assert_eq!(sim.regions(), 0);
    assert_eq!(sim.s().closed_fds, vec![7]);
}

#[test]
fn a_ring_closed_under_the_host_is_a_lost_connection_and_no_spin() {
    let (sim, _server, mut host) = fixture();
    assert_eq!(copy(&sim, &mut host, TEXT), Event::Copied { generation: 1 });
    sim.close_under();
    let now = host.now();
    host.pump(now);
    assert_eq!(host.poll_fd(), None);
    // Nothing was in flight to tell the client, so it hears at its next use. The job
    // goes again, and a closed ring has no fresh connection to give it.
    host.copy(TEXT.to_vec(), now).unwrap();
    assert_eq!(host.take_event(), None);
    host.pump(host.next_deadline().expect("the second try is owed"));
    assert_eq!(host.take_event(), Some(Event::CopyFailed(Why::Lost)));
    assert_eq!(host.reruns(), 1);
}

#[test]
fn a_ring_that_fails_loses_the_connection_rather_than_refusing_the_request() {
    let (sim, _server, mut host) = fixture();
    assert_eq!(copy(&sim, &mut host, TEXT), Event::Copied { generation: 1 });
    // -EINVAL from ENTER would read as the server's own Invalid if it reached the client.
    sim.s().enter_errno = Some(-22);
    let now = host.now();
    host.paste(now).unwrap();
    assert_eq!(host.take_event(), None);
    host.pump(host.next_deadline().expect("the second try is owed"));
    assert_eq!(host.take_event(), Some(Event::PasteFailed(Why::Lost)));
    assert_eq!(host.poll_fd(), None, "a failure closes the descriptor");
}

#[test]
fn a_refused_commit_is_the_servers_answer_and_arrives_at_once() {
    let (sim, server, mut host) = fixture();
    assert_eq!(copy(&sim, &mut host, TEXT), Event::Copied { generation: 1 });
    server.0.borrow_mut().refuse = Some((Operation::CommitCopy, -11));
    let start = host.now();
    assert_eq!(
        copy(&sim, &mut host, TEXT),
        Event::CopyFailed(Why::Refused(Failure::Conflict))
    );
    assert_eq!(host.now(), start, "the answer did not wait for a deadline");
    assert!(host.is_bound(), "a refusal is not a lost connection");
}

#[test]
fn an_errno_on_a_commits_reply_is_no_answer_and_leaves_it_unknown() {
    let (sim, server, mut host) = fixture();
    assert_eq!(copy(&sim, &mut host, TEXT), Event::Copied { generation: 1 });
    server.0.borrow_mut().fail_reply = Some((Operation::CommitCopy, -2));
    let start = host.now();
    assert_eq!(copy(&sim, &mut host, b"two"), Event::CopyUnknown);
    assert_eq!(host.now(), start, "the errno did not wait for a deadline");
    assert_eq!(server.0.borrow().generation, 2, "the commit did land");
    assert_eq!(paste(&sim, &mut host), Event::Pasted(b"two".to_vec()));
}

#[test]
fn a_record_too_big_for_one_write_is_written_in_parts_and_refused_on_its_last() {
    let (sim, server, mut host) = fixture();
    let big = alloc::vec![b'x'; 20_000];
    assert_eq!(copy(&sim, &mut host, &big), Event::Copied { generation: 1 });
    assert_eq!(paste(&sim, &mut host), Event::Pasted(big.clone()));
    server.0.borrow_mut().refuse = Some((Operation::WriteCopy, -11));
    assert_eq!(
        copy(&sim, &mut host, &big),
        Event::CopyFailed(Why::Refused(Failure::Conflict)),
        "a part-written record's refusal"
    );
    assert_eq!(server.0.borrow().generation, 1);
    // The record had gone in parts, so the fid is not to be trusted again.
    assert_eq!(paste(&sim, &mut host), Event::Pasted(big));
    assert_eq!(server.0.borrow().connects, 2);
}

#[test]
fn a_refused_write_is_the_servers_answer_and_arrives_at_once() {
    let (sim, server, mut host) = fixture();
    assert_eq!(copy(&sim, &mut host, TEXT), Event::Copied { generation: 1 });
    server.0.borrow_mut().refuse_write = Some(-1);
    let start = host.now();
    assert_eq!(
        paste(&sim, &mut host),
        Event::PasteFailed(Why::Refused(Failure::Denied))
    );
    assert_eq!(host.now(), start, "the answer did not wait for a deadline");
}

#[test]
fn a_close_with_nothing_in_flight_still_ends_the_connection() {
    let (sim, server, mut host) = fixture();
    assert_eq!(copy(&sim, &mut host, TEXT), Event::Copied { generation: 1 });
    assert!(sim.s().aborts.is_empty());
    server.0.borrow_mut().garble = true;
    assert_eq!(paste(&sim, &mut host), Event::PasteFailed(Why::Malformed));
    assert_eq!(sim.s().aborts.len(), 1, "the old scope was not aborted");
    assert_eq!(paste(&sim, &mut host), Event::Pasted(TEXT.to_vec()));
    assert_eq!(server.0.borrow().connects, 2);
}

mod sak {
    //! The qualification run against a simulated SAK, which ends the connection the
    //! way the server's close does: everything on it now fails, and a new CONNECT is a
    //! new connection. In a guest the key press is what releases the hold.
    use super::*;
    use crate::qualification::{check_value, run, Moment, Progress, Report, Stepping};
    use loom_client::native::Kernel;

    const OLD: &[u8] = b"what was already on the clipboard";
    /// Two chunks on the wire and two records back, as the guest's payload is.
    const NEW: &[u8] = &[b'x'; 20_000];
    /// What the kernel answers on a connection the server ended: what was in flight
    /// fails with -EIO, and the scope then refuses what comes next.
    const EIO: i64 = -5;
    const ECANCELED: i64 = -125;

    #[derive(Clone, Copy, Default)]
    struct Faults {
        /// The server keeps the connection through the SAK.
        retain: bool,
        /// The SAK leaves other bytes on the clipboard.
        stale: bool,
        /// The server answers an operation inside the ENTER that submits it, so the
        /// hold finds its completion unrouted rather than in flight.
        answer_on_enter: bool,
        /// The ring itself closes at the hold, which a SAK never does.
        close_ring: bool,
        /// The server refuses this, with this errno, whether or not a hold comes.
        refuse: Option<(Operation, i64)>,
    }

    fn drive(at: Option<(u32, Moment)>, faults: Faults) -> crate::qualification::Result<Report> {
        drive_seeing(at, faults, |_| {})
    }

    fn drive_seeing(
        at: Option<(u32, Moment)>,
        faults: Faults,
        at_hold: impl Fn(&Script) + 'static,
    ) -> crate::qualification::Result<Report> {
        let sim = Sim::new();
        let server = Server::default();
        server.0.borrow_mut().clipboard = OLD.to_vec();
        {
            let mut s = sim.s();
            s.files.insert(b"tok/interaction".to_vec(), Vec::new());
            s.service = Some(Box::new(Dying(server.clone())));
            s.step_on_enter = faults.answer_on_enter;
            s.msize = Some(SRV_MSIZE);
        }
        server.0.borrow_mut().refuse = faults.refuse;
        let on_hold = server.clone();
        let ring = Sim(sim.0.clone());
        let (kernel, progress): (_, Rc<Progress>) = Stepping::new(sim.kernel(), at, move || {
            let mut s = on_hold.0.borrow_mut();
            at_hold(&s);
            s.sak = !faults.retain;
            if faults.stale {
                s.clipboard = b"neither of them".to_vec();
            }
            if faults.close_ring {
                ring.close_under();
            }
        });
        let host = Host::new(kernel, 5, locator()).expect("the host starts");
        let poller = sim.kernel();
        run(
            host,
            progress,
            move |fd, timeout| {
                let events = poller.poll(fd, POLLIN, timeout);
                assert!(events >= 0, "poll: {events}");
            },
            OLD,
            NEW.to_vec(),
        )
    }

    /// Script's server with an end to its connection.
    struct Dying(Server);

    impl Service for Dying {
        fn connected(&mut self, scope: usize) {
            let mut s = self.0 .0.borrow_mut();
            s.sak = false;
            s.sak_answered = false;
            drop(s);
            self.0.connected(scope);
        }
        fn write(&mut self, scope: usize, fid: usize, offset: u64, bytes: &[u8]) -> Option<i64> {
            if let Some(errno) = self.dead() {
                return Some(errno);
            }
            self.0.write(scope, fid, offset, bytes)
        }
        fn read(&mut self, scope: usize, fid: usize, offset: u64, out: &mut [u8]) -> Option<i64> {
            if let Some(errno) = self.dead() {
                return Some(errno);
            }
            self.0.read(scope, fid, offset, out)
        }
    }

    impl Dying {
        fn dead(&self) -> Option<i64> {
            let mut s = self.0 .0.borrow_mut();
            s.sak.then(|| {
                if core::mem::replace(&mut s.sak_answered, true) {
                    ECANCELED
                } else {
                    EIO
                }
            })
        }
    }

    fn total() -> u32 {
        let report = drive(None, Faults::default()).expect("the plain round trip");
        assert!(!report.held);
        assert_eq!(report.value, NEW);
        assert!(matches!(report.copy, Event::Copied { .. }));
        report.submitted
    }

    const MOMENTS: [Moment; 2] = [Moment::Before, Moment::After];

    #[test]
    fn a_sak_at_every_operation_keeps_the_value_and_recovers() {
        let total = total();
        assert!(total >= 30, "the run's phases: {total}");
        for answer_on_enter in [false, true] {
            let faults = Faults {
                answer_on_enter,
                ..Faults::default()
            };
            for moment in MOMENTS {
                let mut bound = 0;
                for n in 1..=total {
                    let report = drive(Some((n, moment)), faults)
                        .unwrap_or_else(|e| panic!("{moment:?} {n}: {e}"));
                    assert!(report.held, "{moment:?} {n}: never held");
                    if report.bound_at_hold {
                        assert!(report.down_after, "{moment:?} {n}: never went down");
                        bound += 1;
                    }
                }
                // Not vacuous: at the least, each of the job's eight requests, write and
                // reply, was held on a bound connection.
                assert!(bound >= 16, "{moment:?}: bound at {bound} holds of {total}");
                let beyond = drive(Some((total + 1, moment)), faults).expect("past the end");
                assert!(!beyond.held, "{moment:?}: a hold past the last submission");
                assert_eq!(beyond.submitted, total);
            }
        }
    }

    #[test]
    fn a_number_names_one_phase() {
        let total = total();
        let phases = Rc::new(RefCell::new(Vec::new()));
        for n in 1..=total {
            let seen = phases.clone();
            let faults = Faults {
                answer_on_enter: true,
                ..Faults::default()
            };
            drive_seeing(Some((n, Moment::After)), faults, move |s: &Script| {
                seen.borrow_mut().push(s.log.last().copied());
            })
            .unwrap_or_else(|e| panic!("{n}: {e}"));
        }
        let phases = phases.borrow();
        // Connect and both lanes' walks and opens, which a server's log does not see, then
        // each request's write and reply. A clunk falls in Hello's. A server logs a request
        // at the last write of its record, so the first chunk's first two writes fall under
        // Begin here. The scenario's header quotes this table.
        let mut want = vec![None; 8];
        for (op, submissions) in [
            (Operation::Hello, 3),
            (Operation::BindController, 2),
            (Operation::ReportMode, 2),
            (Operation::GetClipboard, 2),
            (Operation::Cancel, 2),
            (Operation::BeginCopy, 4),
            (Operation::WriteCopy, 4),
            (Operation::CommitCopy, 2),
            (Operation::GetClipboard, 2),
            (Operation::ReadClipboard, 6),
            (Operation::Cancel, 2),
        ] {
            want.extend(std::iter::repeat_n(Some(op), submissions));
        }
        assert_eq!(*phases, want, "{total}");
    }

    /// Bind's reply: before it the client cannot yet see the connection stay up.
    const LAST_BEFORE_BOUND: u32 = 13;

    #[test]
    fn a_connection_that_survives_the_sak_is_caught() {
        let total = total();
        let mut caught = 0;
        for moment in MOMENTS {
            for n in 1..=total {
                let faults = Faults {
                    retain: true,
                    ..Faults::default()
                };
                match drive(Some((n, moment)), faults) {
                    Err("the connection survived the SAK") => caught += 1,
                    Err(other) => panic!("{moment:?} {n}: {other}"),
                    Ok(_) => assert!(n <= LAST_BEFORE_BOUND, "{moment:?} {n}: it passed"),
                }
            }
        }
        assert!(caught >= 32, "caught at {caught} holds");
    }

    #[test]
    fn stale_bytes_after_a_sak_are_caught() {
        let total = total();
        let mut caught = 0;
        for moment in MOMENTS {
            for n in 1..=total {
                let faults = Faults {
                    stale: true,
                    ..Faults::default()
                };
                let result = drive(Some((n, moment)), faults);
                // The job's last request is read back only after recovery.
                assert!(n + 3 <= total || result.is_err(), "{moment:?} {n}");
                caught += result.is_err() as u32;
            }
        }
        assert!(caught >= 32, "caught at {caught} holds");
    }

    #[test]
    fn a_ring_that_closes_is_no_sak_and_is_caught() {
        for moment in MOMENTS {
            let faults = Faults {
                close_ring: true,
                ..Faults::default()
            };
            assert_eq!(
                drive(Some((5, moment)), faults).err(),
                Some("the ring closed"),
                "{moment:?}"
            );
        }
    }

    #[test]
    fn a_failure_with_no_sak_to_blame_is_not_forgiven() {
        let mode = Faults {
            refuse: Some((Operation::ReportMode, -11)),
            ..Faults::default()
        };
        assert_eq!(
            drive(None, mode).err(),
            Some("a mode report refused with no SAK to blame")
        );
        let read = Faults {
            refuse: Some((Operation::ReadClipboard, -2)),
            ..Faults::default()
        };
        assert_eq!(
            drive(None, read).err(),
            Some("a paste failed with no SAK to blame")
        );
        for (op, errno) in [
            (Operation::CommitCopy, -11),
            (Operation::CommitCopy, -2),
            (Operation::WriteCopy, -22),
            (Operation::BeginCopy, -16),
        ] {
            let copy = Faults {
                refuse: Some((op, errno)),
                ..Faults::default()
            };
            assert_eq!(
                drive(None, copy).err(),
                Some("a copy failed with no SAK to blame"),
                "{op:?} {errno}"
            );
        }
    }

    #[test]
    fn each_outcome_promises_its_own_values() {
        let failed = Event::CopyFailed(Why::Lost);
        let done = Event::Copied { generation: 2 };
        let unknown = Event::CopyUnknown;
        for (event, value, ok) in [
            (&done, NEW, true),
            (&done, OLD, false),
            (&failed, OLD, true),
            (&failed, NEW, false),
            (&unknown, OLD, true),
            (&unknown, NEW, true),
            (&unknown, b"neither".as_slice(), false),
            (&done, b"neither".as_slice(), false),
            (&failed, b"neither".as_slice(), false),
        ] {
            assert_eq!(
                check_value(OLD, NEW, event, value).is_ok(),
                ok,
                "{event:?} {value:?}"
            );
        }
        assert!(check_value(OLD, NEW, &Event::Pasted(NEW.to_vec()), NEW).is_err());
    }
}
