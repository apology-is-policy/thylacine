//! The client against a scripted server speaking real HIN1 records.
use super::*;
use alloc::{vec, vec::Vec};

const SEC: u64 = 1_000 * MS;
const ENOENT: i32 = -2;
const EBUSY: i32 = -16;
const EACCES: i32 = -1;
const EAGAIN: i32 = -11;
const ECANCELED: i32 = -125;

#[derive(Default)]
struct Wire {
    record: Vec<u8>,
    reply: Vec<u8>,
    refusal: Option<i32>,
}

struct Server {
    clipboard: Vec<u8>,
    generation: u64,
    wires: [Wire; 2],
    /// Requests seen, in order.
    log: Vec<Operation>,
    modes: Vec<(u64, Mode)>,
    refuse: Vec<(Operation, i32)>,
    /// Never answer this operation: its read stays in flight.
    hold: Option<Operation>,
    /// Cut the connection at this operation's answer.
    cut: Option<Operation>,
    max_io: usize,
    /// Connections made and closed.
    connects: u32,
    closes: u32,
    refuse_connect: bool,
    write_slots: Vec<u8>,
    transfer: u64,
}

impl Server {
    fn new() -> Self {
        Self {
            clipboard: Vec::new(),
            generation: 0,
            wires: Default::default(),
            log: Vec::new(),
            modes: Vec::new(),
            refuse: Vec::new(),
            hold: None,
            cut: None,
            max_io: usize::MAX,
            connects: 0,
            closes: 0,
            refuse_connect: false,
            write_slots: Vec::new(),
            transfer: 100,
        }
    }
    fn operations(&self, op: Operation) -> usize {
        self.log.iter().filter(|o| **o == op).count()
    }
    fn answer(&mut self, lane: usize) {
        let record = core::mem::take(&mut self.wires[lane].record);
        let (id, request) = Request::decode(&record).expect("the client sent a valid record");
        let op = wire::frame(&record).unwrap().0.operation;
        self.log.push(op);
        if let Some(i) = self.refuse.iter().position(|(o, _)| *o == op) {
            self.wires[lane].refusal = Some(self.refuse.remove(i).1);
            return;
        }
        let bytes = match request {
            Request::Hello => Response::Hello { session: 77 }.encode(id),
            Request::Bind { session, context, epoch } => {
                assert_eq!((session, context, epoch), (77, CONTEXT, EPOCH));
                Response::Bound { controller: 5 }.encode(id)
            }
            Request::Mode { scope, sequence, mode, .. } => {
                assert_eq!(scope.controller, 5);
                self.modes.push((sequence, mode));
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
                Response::Read { offset, data: &self.clipboard[start..end] }.encode(id)
            }
            Request::Begin { .. } => {
                self.write_slots.clear();
                self.transfer += 1;
                Response::Begun { transfer: self.transfer }.encode(id)
            }
            Request::Write { offset, data, .. } => {
                assert_eq!(offset as usize, self.write_slots.len(), "contiguous offsets");
                self.write_slots.extend_from_slice(data);
                Response::Written { count: data.len() as u32 }.encode(id)
            }
            Request::Commit { expected, .. } => {
                assert_eq!(expected, self.generation);
                self.clipboard = core::mem::take(&mut self.write_slots);
                self.generation += 1;
                Response::Committed { generation: self.generation }.encode(id)
            }
            Request::Cancel { .. } => Response::Cancelled.encode(id),
            Request::Unbind { .. } => Response::Unbound.encode(id),
        };
        self.wires[lane].reply = bytes.unwrap();
    }
}

enum Action {
    Connect,
    Close,
    Write(Lane, Vec<u8>),
    Read(Lane, usize, usize),
}

/// Runs everything the client will start, answering what the script allows.
fn drive(c: &mut Client, s: &mut Server, now: u64) -> Vec<&'static str> {
    let mut seen = Vec::new();
    let mut guard = 0;
    loop {
        guard += 1;
        assert!(guard < 100_000, "the client never settled");
        let action = match c.next_step(now) {
            None => return seen,
            Some(Step::Connect) => Action::Connect,
            Some(Step::Close) => Action::Close,
            Some(Step::Write { lane, offset, bytes }) => {
                let i = index(lane);
                assert_eq!(offset as usize, s.wires[i].record.len(), "contiguous from zero");
                Action::Write(lane, bytes.to_vec())
            }
            Some(Step::Read { lane, offset, count }) => {
                Action::Read(lane, offset as usize, count as usize)
            }
        };
        match action {
            Action::Connect => {
                seen.push("connect");
                s.connects += 1;
                s.wires = Default::default();
                if s.refuse_connect {
                    c.connect_failed();
                } else {
                    c.connected();
                }
            }
            Action::Close => {
                seen.push("close");
                s.closes += 1;
                s.wires = Default::default();
            }
            Action::Write(lane, bytes) => {
                let i = index(lane);
                let n = bytes.len().min(s.max_io);
                s.wires[i].record.extend_from_slice(&bytes[..n]);
                c.wrote(lane, n, now);
                let complete = wire::frame(&s.wires[i].record).is_ok();
                if complete {
                    let op = wire::frame(&s.wires[i].record).unwrap().0.operation;
                    if s.hold == Some(op) {
                        s.log.push(op);
                        s.wires[i].record.clear();
                        s.wires[i].refusal = Some(i32::MAX);
                    } else {
                        s.answer(i);
                    }
                }
            }
            Action::Read(lane, offset, count) => {
                let i = index(lane);
                match s.wires[i].refusal {
                    // Held: the read stays in flight, so nothing is reported.
                    Some(i32::MAX) => {}
                    Some(errno) => {
                        s.wires[i].refusal = None;
                        c.failed(lane, errno, now);
                    }
                    None => {
                        let last = s.log.last().copied();
                        if last.is_some() && last == s.cut {
                            s.cut = None;
                            c.failed(lane, ECANCELED, now);
                        } else {
                            let reply = core::mem::take(&mut s.wires[i].reply);
                            let end = (offset + count.min(s.max_io)).min(reply.len());
                            let chunk = reply[offset..end].to_vec();
                            s.wires[i].reply = reply;
                            c.read(lane, &chunk, now);
                        }
                    }
                }
            }
        }
    }
}

fn bound(s: &mut Server, now: u64) -> Client {
    let mut c = Client::new();
    c.report_mode(Mode::Normal, false, "Nora", now).unwrap();
    drive(&mut c, s, now);
    assert!(c.is_bound());
    c
}

fn text(n: usize) -> Vec<u8> {
    (0..n).map(|i| b'a' + (i % 26) as u8).collect()
}

#[test]
fn nothing_connects_until_the_host_asks() {
    let mut c = Client::new();
    assert_eq!(c.next_deadline(), None);
    assert_eq!(c.next_step(0), None);
}

#[test]
fn a_mode_report_connects_then_hello_bind_and_report_in_order() {
    let mut s = Server::new();
    let c = bound(&mut s, 0);
    assert_eq!(
        s.log,
        vec![Operation::Hello, Operation::BindController, Operation::ReportMode]
    );
    assert_eq!(s.modes, vec![(1, Mode::Normal)]);
    assert_eq!(c.next_deadline(), None, "bound and idle owes nothing, so the host may sleep");
}

#[test]
fn a_short_read_and_a_short_write_change_nothing() {
    let mut s = Server::new();
    s.max_io = 7;
    let mut c = bound(&mut s, 0);
    s.clipboard = text(40_000);
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(c.take_event(), Some(Event::Pasted(text(40_000))));
}

#[test]
fn mode_reports_coalesce_while_one_is_in_flight() {
    let mut s = Server::new();
    s.hold = Some(Operation::ReportMode);
    let mut c = bound(&mut s, 0);
    for m in [Mode::Insert, Mode::Visual, Mode::Command] {
        c.report_mode(m, false, "Nora", 0).unwrap();
        drive(&mut c, &mut s, 0);
    }
    assert_eq!(s.operations(Operation::ReportMode), 1, "one in flight, nothing queued behind it");
    // The answer lands: only the latest follows.
    s.hold = None;
    s.wires[0].refusal = None;
    let id = c.lanes[0].as_ref().unwrap().id;
    s.wires[0].reply = Response::Mode.encode(id).unwrap();
    drive(&mut c, &mut s, 0);
    let Some(exchange) = c.lanes[0].as_ref() else { panic!("the read was still owed") };
    assert!(exchange.in_flight);
    let reply = s.wires[0].reply.clone();
    c.read(Lane::Control, &reply, 0);
    drive(&mut c, &mut s, 0);
    assert_eq!(s.modes, vec![(2, Mode::Command)]);
}

#[test]
fn an_unchanged_mode_is_not_sent_again() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    c.report_mode(Mode::Normal, false, "Nora", 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(s.operations(Operation::ReportMode), 1);
    c.report_mode(Mode::Normal, true, "Nora", 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(s.operations(Operation::ReportMode), 2, "readonly is part of the report");
}

#[test]
fn a_bad_label_is_refused_before_anything_is_sent() {
    let mut c = Client::new();
    assert_eq!(c.report_mode(Mode::Normal, false, "a\nb", 0), Err(Refusal::NotText));
    let long = "x".repeat(MAX_LABEL + 1);
    assert_eq!(c.report_mode(Mode::Normal, false, &long, 0), Err(Refusal::TooLarge));
    assert_eq!(c.next_step(0), None);
}

#[test]
fn bind_retries_gone_and_busy_on_its_deadline_without_sleeping() {
    let mut s = Server::new();
    s.refuse = vec![
        (Operation::BindController, ENOENT),
        (Operation::BindController, EBUSY),
    ];
    let mut c = Client::new();
    c.report_mode(Mode::Normal, false, "Nora", 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert!(!c.is_bound());
    assert_eq!(c.next_deadline(), Some(BIND_RETRY_NS), "the retry is a deadline, not a sleep");
    // Nothing happens before it, however often the host pumps.
    drive(&mut c, &mut s, BIND_RETRY_NS - 1);
    assert_eq!(s.operations(Operation::BindController), 1);
    drive(&mut c, &mut s, BIND_RETRY_NS);
    assert_eq!(s.operations(Operation::BindController), 2);
    drive(&mut c, &mut s, 2 * BIND_RETRY_NS);
    assert_eq!(s.operations(Operation::BindController), 3);
    assert!(c.is_bound());
}

#[test]
fn bind_gives_up_after_its_attempts_and_says_so_to_a_job() {
    let mut s = Server::new();
    s.refuse = vec![(Operation::BindController, ENOENT); BIND_ATTEMPTS as usize];
    let mut c = Client::new();
    c.paste(0).unwrap();
    let mut now = 0;
    for _ in 0..BIND_ATTEMPTS {
        drive(&mut c, &mut s, now);
        now += BIND_RETRY_NS;
    }
    assert_eq!(s.operations(Operation::BindController), BIND_ATTEMPTS as usize);
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::Refused(Failure::Gone))));
}

#[test]
fn a_denied_bind_is_not_retried_until_the_host_asks_again() {
    let mut s = Server::new();
    s.refuse = vec![(Operation::BindController, EACCES)];
    let mut c = Client::new();
    c.report_mode(Mode::Normal, false, "Nora", 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(s.closes, 1);
    assert_eq!(c.next_deadline(), None, "a background process does not hammer the server");
    c.report_mode(Mode::Insert, false, "Nora", SEC).unwrap();
    drive(&mut c, &mut s, SEC);
    assert_eq!(s.connects, 2);
    assert!(c.is_bound());
}

#[test]
fn a_handshake_past_its_bound_closes_and_backs_off() {
    let mut s = Server::new();
    s.hold = Some(Operation::Hello);
    let mut c = Client::new();
    c.report_mode(Mode::Normal, false, "Nora", 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(c.next_deadline(), Some(HANDSHAKE_NS));
    assert_eq!(drive(&mut c, &mut s, HANDSHAKE_NS - 1), Vec::<&str>::new());
    assert_eq!(drive(&mut c, &mut s, HANDSHAKE_NS), vec!["close"]);
    assert_eq!(c.next_deadline(), Some(HANDSHAKE_NS + RECONNECT_MIN_NS));
}

#[test]
fn reconnects_back_off_and_stop_until_the_host_asks_again() {
    let mut s = Server::new();
    s.refuse_connect = true;
    let mut c = Client::new();
    c.report_mode(Mode::Normal, false, "Nora", 0).unwrap();
    let mut now = 0;
    let mut gaps = Vec::new();
    loop {
        drive(&mut c, &mut s, now);
        let Some(at) = c.next_deadline() else { break };
        gaps.push(at - now);
        now = at;
        assert!(gaps.len() <= RECONNECT_ATTEMPTS as usize, "the retries must end");
    }
    assert_eq!(gaps.len(), RECONNECT_ATTEMPTS as usize);
    assert_eq!(gaps[0], RECONNECT_MIN_NS);
    assert_eq!(gaps[1], 2 * RECONNECT_MIN_NS);
    assert_eq!(*gaps.last().unwrap(), RECONNECT_MAX_NS);
    assert_eq!(s.connects, RECONNECT_ATTEMPTS + 1);
    // A new request starts a new round at once.
    s.refuse_connect = false;
    c.report_mode(Mode::Insert, false, "Nora", now).unwrap();
    drive(&mut c, &mut s, now);
    assert!(c.is_bound());
}

#[test]
fn a_lost_connection_rebinds_and_reports_the_mode_again() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    c.report_mode(Mode::Insert, false, "Nora", 0).unwrap();
    s.cut = Some(Operation::ReportMode);
    drive(&mut c, &mut s, 0);
    assert!(!c.is_bound());
    assert_eq!(s.closes, 1);
    let at = c.next_deadline().unwrap();
    drive(&mut c, &mut s, at);
    assert!(c.is_bound());
    assert_eq!(s.modes.len(), 3, "the report the cut lost goes again on the new connection");
    assert_eq!(s.modes.last().unwrap().1, Mode::Insert);
    assert_eq!(s.operations(Operation::Hello), 2);
}

#[test]
fn a_mode_refusal_is_told_and_keeps_the_connection() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.refuse = vec![(Operation::ReportMode, EAGAIN)];
    c.report_mode(Mode::Insert, false, "Nora", 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(c.take_event(), Some(Event::ModeRefused(Failure::Conflict)));
    assert!(c.is_bound());
    assert_eq!(s.closes, 0);
}

#[test]
fn a_copy_runs_get_release_begin_chunks_commit() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.log.clear();
    c.copy(text(40_000), 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(
        s.log,
        vec![
            Operation::GetClipboard,
            Operation::Cancel,
            Operation::BeginCopy,
            Operation::WriteCopy,
            Operation::WriteCopy,
            Operation::WriteCopy,
            Operation::CommitCopy,
        ]
    );
    assert_eq!(c.take_event(), Some(Event::Copied { generation: 1 }));
    assert_eq!(s.clipboard, text(40_000));
}

#[test]
fn an_empty_copy_commits_with_no_write() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    c.copy(Vec::new(), 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(s.operations(Operation::WriteCopy), 0);
    assert_eq!(c.take_event(), Some(Event::Copied { generation: 1 }));
}

#[test]
fn a_copy_that_is_not_text_never_reaches_the_server() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.log.clear();
    assert_eq!(c.copy(b"a\x1b[31m".to_vec(), 0), Err(Refusal::NotText));
    assert_eq!(c.copy(text(MAX_TEXT + 1), 0), Err(Refusal::TooLarge));
    drive(&mut c, &mut s, 0);
    assert!(s.log.is_empty());
}

#[test]
fn one_job_at_a_time() {
    let mut s = Server::new();
    s.hold = Some(Operation::GetClipboard);
    let mut c = bound(&mut s, 0);
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(c.paste(0), Err(Refusal::Busy));
    assert_eq!(c.copy(text(3), 0), Err(Refusal::Busy));
}

#[test]
fn a_commit_lost_after_it_was_sent_is_unknown_and_never_retried() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.cut = Some(Operation::CommitCopy);
    c.copy(text(10), 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(c.take_event(), Some(Event::CopyUnknown));
    assert_eq!(c.take_event(), None);
    // The reconnect exists only for the mode report: no second commit.
    let at = c.next_deadline().unwrap();
    drive(&mut c, &mut s, at);
    assert!(c.is_bound());
    assert_eq!(s.operations(Operation::CommitCopy), 1);
    assert_eq!(c.take_event(), None);
}

#[test]
fn a_commit_past_its_deadline_is_unknown_too() {
    let mut s = Server::new();
    s.hold = Some(Operation::CommitCopy);
    let mut c = bound(&mut s, 0);
    c.copy(text(10), 0).unwrap();
    drive(&mut c, &mut s, 0);
    let at = c.next_deadline().unwrap();
    assert_eq!(at, IDLE_NS);
    assert_eq!(drive(&mut c, &mut s, at - 1), Vec::<&str>::new());
    assert_eq!(c.take_event(), None);
    drive(&mut c, &mut s, at);
    assert_eq!(c.take_event(), Some(Event::CopyUnknown));
}

#[test]
fn a_write_lost_before_the_commit_is_a_plain_failure() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.cut = Some(Operation::WriteCopy);
    c.copy(text(10), 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(c.take_event(), Some(Event::CopyFailed(Why::Lost)));
    assert_eq!(s.operations(Operation::CommitCopy), 0);
}

#[test]
fn a_refused_commit_is_a_definite_failure_and_releases_its_transfer() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.refuse = vec![(Operation::CommitCopy, EAGAIN)];
    c.copy(text(10), 0).unwrap();
    s.log.clear();
    drive(&mut c, &mut s, 0);
    assert_eq!(c.take_event(), Some(Event::CopyFailed(Why::Refused(Failure::Conflict))));
    assert_eq!(s.log.last(), Some(&Operation::Cancel));
    assert!(c.is_bound());
    c.paste(0).unwrap();
}

#[test]
fn a_paste_delivers_only_when_whole_and_released() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.clipboard = text(40_000);
    s.hold = Some(Operation::Cancel);
    s.log.clear();
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(s.operations(Operation::ReadClipboard), 3, "16 KiB chunks");
    assert_eq!(c.take_event(), None, "the release is still in flight");
    // A lost release costs nothing: every byte is already here.
    c.cancel();
    assert_eq!(c.take_event(), Some(Event::Pasted(text(40_000))));
}

#[test]
fn an_empty_clipboard_pastes_empty() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(s.operations(Operation::ReadClipboard), 0);
    assert_eq!(c.take_event(), Some(Event::Pasted(Vec::new())));
}

#[test]
fn a_clipboard_that_is_not_text_is_never_delivered() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.clipboard = b"a\x1b[31m".to_vec();
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(s.operations(Operation::Cancel), 1, "the snapshot is still released");
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::Malformed)));
}

#[test]
fn a_refused_read_fails_the_paste_and_releases_the_snapshot() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.clipboard = text(10);
    s.refuse = vec![(Operation::ReadClipboard, -22)];
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::Refused(Failure::Invalid))));
    assert_eq!(s.log.last(), Some(&Operation::Cancel));
    assert!(c.paste(0).is_ok(), "the release finished, so the next job may start");
}

#[test]
fn a_parked_get_does_not_hold_up_a_mode_report() {
    let mut s = Server::new();
    s.hold = Some(Operation::GetClipboard);
    let mut c = bound(&mut s, 0);
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    c.report_mode(Mode::Insert, false, "Nora", 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(s.modes.last().unwrap().1, Mode::Insert);
}

#[test]
fn cancel_closes_the_connection_a_parked_get_is_on() {
    let mut s = Server::new();
    s.hold = Some(Operation::GetClipboard);
    let mut c = bound(&mut s, 0);
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    c.cancel();
    assert_eq!(drive(&mut c, &mut s, 0), vec!["close"]);
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::Cancelled)));
    assert_eq!(c.take_event(), None);
    assert!(!c.is_bound());
}

#[test]
fn a_transfer_ends_at_its_lifetime_even_with_progress() {
    let mut s = Server::new();
    s.hold = Some(Operation::ReadClipboard);
    let mut c = bound(&mut s, 0);
    s.clipboard = text(10);
    c.paste(SEC).unwrap();
    drive(&mut c, &mut s, SEC);
    // Keep the exchange fresh, as a slow server answering in time would.
    c.lanes[1].as_mut().unwrap().idle_at = u64::MAX;
    assert_eq!(c.next_deadline(), Some(SEC + LIFETIME_NS));
    drive(&mut c, &mut s, SEC + LIFETIME_NS);
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::TimedOut)));
}

#[test]
fn a_reply_with_the_wrong_identity_ends_the_connection() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.hold = Some(Operation::GetClipboard);
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    let id = c.lanes[1].as_ref().unwrap().id;
    let wrong = Response::Clipboard { transfer: 1, generation: 0, length: 0 }.encode(id + 1).unwrap();
    c.read(Lane::Transfer, &wrong, 0);
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::Malformed)));
    assert_eq!(drive(&mut c, &mut s, 0)[0], "close");
}

#[test]
fn a_read_past_the_reply_or_an_empty_one_ends_the_connection() {
    for bytes in [vec![], vec![0u8; MAX_RECORD + 1]] {
        let mut s = Server::new();
        s.hold = Some(Operation::GetClipboard);
        let mut c = bound(&mut s, 0);
        c.paste(0).unwrap();
        drive(&mut c, &mut s, 0);
        c.read(Lane::Transfer, &bytes, 0);
        assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::Malformed)), "{}", bytes.len());
    }
}

#[test]
fn a_completion_after_the_close_is_ignored() {
    let mut s = Server::new();
    s.hold = Some(Operation::GetClipboard);
    let mut c = bound(&mut s, 0);
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    c.cancel();
    drive(&mut c, &mut s, 0);
    let _ = c.take_event();
    c.wrote(Lane::Transfer, 4, 0);
    c.read(Lane::Transfer, b"late", 0);
    c.failed(Lane::Transfer, ECANCELED, 0);
    assert_eq!(c.take_event(), None);
    assert!(c.lanes[1].is_none());
}
