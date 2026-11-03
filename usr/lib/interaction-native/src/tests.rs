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
    /// Answer the next request with the wrong id.
    garble: bool,
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
            self.wires.get_mut(&fid).unwrap().refusal = Some(errno);
            return;
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
fn a_service_the_registry_does_not_hold_is_a_lost_connection() {
    let sim = Sim::new();
    let missing = Locator {
        service: b"gone",
        token: b"tok",
    };
    let mut host = Host::new(sim.kernel(), 5, missing).expect("names are checked at connect");
    assert_eq!(copy(&sim, &mut host, TEXT), Event::CopyFailed(Why::Lost));
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
    // Nothing was in flight to tell the client, so it hears at its next use.
    host.copy(TEXT.to_vec(), now).unwrap();
    assert_eq!(host.take_event(), Some(Event::CopyFailed(Why::Lost)));
}

#[test]
fn a_ring_that_fails_loses_the_connection_rather_than_refusing_the_request() {
    let (sim, _server, mut host) = fixture();
    assert_eq!(copy(&sim, &mut host, TEXT), Event::Copied { generation: 1 });
    // -EINVAL from ENTER would read as the server's own Invalid if it reached the client.
    sim.s().enter_errno = Some(-22);
    let now = host.now();
    host.paste(now).unwrap();
    assert_eq!(host.take_event(), Some(Event::PasteFailed(Why::Lost)));
    assert_eq!(host.poll_fd(), None, "a failure closes the descriptor");
}

#[test]
fn a_refused_read_is_the_servers_answer_and_arrives_at_once() {
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
