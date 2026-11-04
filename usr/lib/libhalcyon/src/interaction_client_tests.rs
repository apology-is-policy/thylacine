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
    /// Refused on the write that finishes the record, as the real server refuses.
    refuse: Vec<(Operation, i32)>,
    /// The request ran, and this errno comes on its reply's read.
    refuse_reply: Vec<(Operation, i32)>,
    /// This errno comes on a write after the first part of this operation's record.
    refuse_partway: Option<(Operation, i32)>,
    /// Added to the generation a commit answers with.
    skew: u64,
    /// Never answer this operation: its read stays in flight.
    hold: Option<Operation>,
    /// Never finish the write of this operation's record, where the real server parks.
    hold_write: Option<Operation>,
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
            refuse_reply: Vec::new(),
            refuse_partway: None,
            skew: 0,
            hold: None,
            hold_write: None,
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
        if let Some(i) = self.refuse_reply.iter().position(|(o, _)| *o == op) {
            self.wires[lane].refusal = Some(self.refuse_reply.remove(i).1);
        }
        let bytes = match request {
            Request::Hello => Response::Hello { session: 77 }.encode(id),
            Request::Bind {
                session,
                context,
                epoch,
            } => {
                assert_eq!((session, context, epoch), (77, CONTEXT, EPOCH));
                Response::Bound { controller: 5 }.encode(id)
            }
            Request::Mode {
                scope,
                sequence,
                mode,
                ..
            } => {
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
                Response::Read {
                    offset,
                    data: &self.clipboard[start..end],
                }
                .encode(id)
            }
            Request::Begin { .. } => {
                self.write_slots.clear();
                self.transfer += 1;
                Response::Begun {
                    transfer: self.transfer,
                }
                .encode(id)
            }
            Request::Write { offset, data, .. } => {
                assert_eq!(
                    offset as usize,
                    self.write_slots.len(),
                    "contiguous offsets"
                );
                self.write_slots.extend_from_slice(data);
                Response::Written {
                    count: data.len() as u32,
                }
                .encode(id)
            }
            Request::Commit { expected, .. } => {
                assert_eq!(expected, self.generation);
                self.clipboard = core::mem::take(&mut self.write_slots);
                self.generation += 1 + self.skew;
                Response::Committed {
                    generation: self.generation,
                }
                .encode(id)
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
            Some(Step::Write {
                lane,
                offset,
                bytes,
            }) => {
                let i = index(lane);
                assert_eq!(
                    offset as usize,
                    s.wires[i].record.len(),
                    "contiguous from zero"
                );
                Action::Write(lane, bytes.to_vec())
            }
            Some(Step::Read {
                lane,
                offset,
                count,
            }) => Action::Read(lane, offset as usize, count as usize),
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
                if s.wires[i].record.len() >= HEADER_BYTES {
                    let op = wire::Header::decode(&s.wires[i].record[..HEADER_BYTES])
                        .unwrap()
                        .operation;
                    if s.refuse_partway.is_some_and(|(o, _)| o == op) {
                        let errno = s.refuse_partway.take().unwrap().1;
                        s.wires[i].record.clear();
                        c.failed(lane, errno, now);
                        continue;
                    }
                }
                s.wires[i].record.extend_from_slice(&bytes[..n]);
                let complete = wire::frame(&s.wires[i].record).is_ok();
                if complete {
                    let op = wire::frame(&s.wires[i].record).unwrap().0.operation;
                    if s.hold_write == Some(op) {
                        s.log.push(op);
                        s.wires[i].record.clear();
                        continue;
                    }
                    if let Some(at) = s.refuse.iter().position(|(o, _)| *o == op) {
                        let errno = s.refuse.remove(at).1;
                        s.log.push(op);
                        s.wires[i].record.clear();
                        c.failed(lane, errno, now);
                        continue;
                    }
                }
                c.wrote(lane, n, now);
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
        vec![
            Operation::Hello,
            Operation::BindController,
            Operation::ReportMode
        ]
    );
    assert_eq!(s.modes, vec![(1, Mode::Normal)]);
    assert_eq!(
        c.next_deadline(),
        None,
        "bound and idle owes nothing, so the host may sleep"
    );
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
    assert_eq!(
        s.operations(Operation::ReportMode),
        1,
        "one in flight, nothing queued behind it"
    );
    // The answer lands: only the latest follows.
    s.hold = None;
    s.wires[0].refusal = None;
    let id = c.lanes[0].as_ref().unwrap().id;
    s.wires[0].reply = Response::Mode.encode(id).unwrap();
    drive(&mut c, &mut s, 0);
    let Some(exchange) = c.lanes[0].as_ref() else {
        panic!("the read was still owed")
    };
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
    assert_eq!(
        s.operations(Operation::ReportMode),
        2,
        "readonly is part of the report"
    );
}

#[test]
fn a_bad_label_is_refused_before_anything_is_sent() {
    let mut c = Client::new();
    assert_eq!(
        c.report_mode(Mode::Normal, false, "a\nb", 0),
        Err(Refusal::NotText)
    );
    let long = "x".repeat(MAX_LABEL + 1);
    assert_eq!(
        c.report_mode(Mode::Normal, false, &long, 0),
        Err(Refusal::TooLarge)
    );
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
    assert_eq!(
        c.next_deadline(),
        Some(BIND_RETRY_NS),
        "the retry is a deadline, not a sleep"
    );
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
    assert_eq!(
        s.operations(Operation::BindController),
        BIND_ATTEMPTS as usize
    );
    assert_eq!(
        c.take_event(),
        Some(Event::PasteFailed(Why::Refused(Failure::Gone)))
    );
}

#[test]
fn a_denied_bind_is_asked_again_on_the_schedule_and_then_waits_for_the_host() {
    let mut s = Server::new();
    // A seat away for a trusted episode answers this, and it comes back on its own.
    s.refuse = vec![(Operation::BindController, EACCES)];
    let mut c = Client::new();
    c.report_mode(Mode::Normal, false, "Nora", 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(s.closes, 1);
    let retry = c.next_deadline().expect("a seat can come back");
    drive(&mut c, &mut s, retry);
    assert_eq!(s.connects, 2);
    assert!(c.is_bound());
    assert_eq!(s.modes.last().unwrap().1, Mode::Normal);

    // One that never comes back spends the schedule and no more.
    let mut s = Server::new();
    s.refuse = vec![(Operation::BindController, EACCES); 2 * RECONNECT_ATTEMPTS as usize];
    let mut c = Client::new();
    c.report_mode(Mode::Normal, false, "Nora", 0).unwrap();
    drive(&mut c, &mut s, 0);
    let mut spent = 0;
    while let Some(at) = c.next_deadline() {
        drive(&mut c, &mut s, at);
        spent += 1;
        assert!(spent <= RECONNECT_ATTEMPTS, "it kept asking");
    }
    assert_eq!(spent, RECONNECT_ATTEMPTS);
    assert_eq!(
        c.next_deadline(),
        None,
        "a background process does not hammer the server"
    );
    c.report_mode(Mode::Insert, false, "Nora", SEC).unwrap();
    assert!(c.next_deadline().is_some(), "the host asked again");
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
        assert!(
            gaps.len() <= RECONNECT_ATTEMPTS as usize,
            "the retries must end"
        );
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
    assert_eq!(
        s.modes.len(),
        3,
        "the report the cut lost goes again on the new connection"
    );
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
fn a_commit_parked_on_its_write_may_have_landed() {
    let mut s = Server::new();
    s.hold_write = Some(Operation::CommitCopy);
    let mut c = bound(&mut s, 0);
    c.copy(text(10), 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(s.log.last(), Some(&Operation::CommitCopy));
    let at = c.next_deadline().unwrap();
    assert_eq!(at, IDLE_NS);
    assert_eq!(drive(&mut c, &mut s, at - 1), Vec::<&str>::new());
    assert_eq!(c.take_event(), None);
    drive(&mut c, &mut s, at);
    assert_eq!(c.take_event(), Some(Event::CopyUnknown));
}

#[test]
fn a_get_parked_on_its_write_holds_up_no_report_and_ends_at_its_deadline() {
    let mut s = Server::new();
    s.hold_write = Some(Operation::GetClipboard);
    let mut c = bound(&mut s, 0);
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(s.log.last(), Some(&Operation::GetClipboard));
    c.report_mode(Mode::Insert, false, "Nora", 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(s.modes.last().unwrap().1, Mode::Insert);
    let at = c.next_deadline().unwrap();
    drive(&mut c, &mut s, at);
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::TimedOut)));
    assert_eq!(c.take_event(), None);
}

#[test]
fn cancel_closes_the_connection_a_get_is_parked_on_the_write_of() {
    let mut s = Server::new();
    s.hold_write = Some(Operation::GetClipboard);
    let mut c = bound(&mut s, 0);
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    c.cancel();
    assert_eq!(drive(&mut c, &mut s, 0), vec!["close"]);
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::Cancelled)));
    assert!(!c.is_bound());
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
    assert_eq!(c.reruns(), 0, "the Get was answered");
}

#[test]
fn a_paste_nothing_answered_goes_again_on_a_fresh_connection() {
    let mut s = Server::new();
    s.clipboard = text(10);
    let mut c = bound(&mut s, 0);
    s.cut = Some(Operation::GetClipboard);
    c.paste(0).unwrap();
    assert_eq!(drive(&mut c, &mut s, 0), vec!["close"]);
    assert_eq!(c.take_event(), None, "the host hears nothing of it");
    let at = c.next_deadline().unwrap();
    assert_eq!(drive(&mut c, &mut s, at), vec!["connect"]);
    assert_eq!(c.take_event(), Some(Event::Pasted(text(10))));
    assert_eq!(c.reruns(), 1);
    assert!(c.is_bound());
}

#[test]
fn a_copy_nothing_answered_goes_again_and_commits_once() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.cut = Some(Operation::GetClipboard);
    c.copy(text(10), 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(c.take_event(), None);
    let at = c.next_deadline().unwrap();
    drive(&mut c, &mut s, at);
    assert!(matches!(c.take_event(), Some(Event::Copied { .. })));
    assert_eq!(s.operations(Operation::CommitCopy), 1);
    assert_eq!(s.clipboard, text(10));
    assert_eq!(c.reruns(), 1);
}

#[test]
fn a_second_loss_is_reported_and_the_next_job_has_its_own_chance() {
    let mut s = Server::new();
    s.clipboard = text(10);
    let mut c = bound(&mut s, 0);
    s.cut = Some(Operation::GetClipboard);
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(c.take_event(), None);
    s.cut = Some(Operation::GetClipboard);
    let at = c.next_deadline().unwrap();
    drive(&mut c, &mut s, at);
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::Lost)));
    assert_eq!(c.reruns(), 1);

    let at = c.next_deadline().unwrap();
    drive(&mut c, &mut s, at);
    assert!(c.is_bound());
    s.cut = Some(Operation::GetClipboard);
    c.paste(at).unwrap();
    drive(&mut c, &mut s, at);
    assert_eq!(c.take_event(), None);
    let at = c.next_deadline().unwrap();
    drive(&mut c, &mut s, at);
    assert_eq!(c.take_event(), Some(Event::Pasted(text(10))));
    assert_eq!(c.reruns(), 2);

    s.cut = Some(Operation::GetClipboard);
    c.copy(text(4), at).unwrap();
    drive(&mut c, &mut s, at);
    assert_eq!(c.take_event(), None, "a copy has its own chance too");
    let at = c.next_deadline().unwrap();
    drive(&mut c, &mut s, at);
    assert!(matches!(c.take_event(), Some(Event::Copied { .. })));
    assert_eq!(c.reruns(), 3);
}

#[test]
fn only_a_lost_bound_connection_runs_a_job_again() {
    // Never formed.
    let mut s = Server::new();
    s.refuse_connect = true;
    let mut c = Client::new();
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::Lost)));
    assert_eq!(c.reruns(), 0);

    // Past its deadline: a server that does not answer gets no second request.
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.hold = Some(Operation::GetClipboard);
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    let at = c.next_deadline().unwrap();
    drive(&mut c, &mut s, at);
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::TimedOut)));
    assert_eq!(c.reruns(), 0);

    // Given up on by the host.
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.hold = Some(Operation::GetClipboard);
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    c.cancel();
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::Cancelled)));
    assert_eq!(c.reruns(), 0);
}

#[test]
fn a_refused_commit_is_a_definite_failure_and_releases_its_transfer() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.refuse = vec![(Operation::CommitCopy, EAGAIN)];
    c.copy(text(10), 0).unwrap();
    s.log.clear();
    drive(&mut c, &mut s, 0);
    assert_eq!(
        c.take_event(),
        Some(Event::CopyFailed(Why::Refused(Failure::Conflict)))
    );
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
    assert_eq!(
        s.operations(Operation::Cancel),
        1,
        "the snapshot is still released"
    );
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::Malformed)));
}

#[test]
fn a_refused_read_fails_the_paste_and_releases_the_snapshot() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.clipboard = text(10);
    s.refuse = vec![(Operation::ReadClipboard, EAGAIN)];
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(
        c.take_event(),
        Some(Event::PasteFailed(Why::Refused(Failure::Conflict)))
    );
    assert_eq!(s.log.last(), Some(&Operation::Cancel));
    assert!(
        c.paste(0).is_ok(),
        "the release finished, so the next job may start"
    );
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
    let wrong = Response::Clipboard {
        transfer: 1,
        generation: 0,
        length: 0,
    }
    .encode(id + 1)
    .unwrap();
    c.read(Lane::Transfer, &wrong, 0);
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::Malformed)));
    assert_eq!(drive(&mut c, &mut s, 0)[0], "close");
}

/// The header alone decides, so a server cannot make the client read a record it has
/// already said is not the reply.
#[test]
fn a_header_that_is_not_the_reply_ends_the_connection_without_reading_on() {
    let body = [b'x'; 100];
    for case in 0..3 {
        let mut s = Server::new();
        let mut c = bound(&mut s, 0);
        s.hold = Some(Operation::GetClipboard);
        c.paste(0).unwrap();
        drive(&mut c, &mut s, 0);
        let id = c.lanes[1].as_ref().unwrap().id;
        let scope = c.scope.unwrap();
        let record = match case {
            0 => Response::Clipboard {
                transfer: 1,
                generation: 0,
                length: 0,
            }
            .encode(id + 1)
            .unwrap(),
            1 => Response::Read {
                offset: 0,
                data: &body,
            }
            .encode(id)
            .unwrap(),
            _ => Request::Get { scope }.encode(id).unwrap(),
        };
        c.read(Lane::Transfer, &record[..HEADER_BYTES], 0);
        assert_eq!(
            c.take_event(),
            Some(Event::PasteFailed(Why::Malformed)),
            "case {case}"
        );
    }
}

#[test]
fn a_read_longer_than_the_record_it_announces_ends_the_connection() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.hold = Some(Operation::GetClipboard);
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    let id = c.lanes[1].as_ref().unwrap().id;
    let mut record = Response::Clipboard {
        transfer: 1,
        generation: 0,
        length: 0,
    }
    .encode(id)
    .unwrap();
    record.extend_from_slice(b"more than it said");
    c.read(Lane::Transfer, &record, 0);
    assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::Malformed)));
}

#[test]
fn a_second_copy_commits_against_the_generation_it_read() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    for generation in 1..=3 {
        c.copy(text(10), 0).unwrap();
        drive(&mut c, &mut s, 0);
        assert_eq!(c.take_event(), Some(Event::Copied { generation }));
    }
}

#[test]
fn a_commit_the_server_says_is_gone_is_a_definite_failure() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.refuse = vec![(Operation::CommitCopy, ENOENT)];
    c.copy(text(10), 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(
        c.take_event(),
        Some(Event::CopyFailed(Why::Refused(Failure::Gone)))
    );
    assert_eq!(c.take_event(), None);
}

#[test]
fn an_errno_on_a_commits_reply_leaves_the_commit_unknown() {
    // The route can go between the write and the read, and the ring can run out of
    // memory for the read. Either way the write's success said the commit ran.
    for errno in [ENOENT, -12, EAGAIN, EBUSY, -22] {
        let mut s = Server::new();
        let mut c = bound(&mut s, 0);
        s.refuse_reply = vec![(Operation::CommitCopy, errno)];
        c.copy(text(10), 0).unwrap();
        drive(&mut c, &mut s, 0);
        assert_eq!(c.take_event(), Some(Event::CopyUnknown), "{errno}");
        assert_eq!(s.closes, 1, "{errno}");
        assert_eq!(s.generation, 1, "{errno}: the commit did land");
    }
}

#[test]
fn an_errno_on_any_other_reply_ends_the_connection_too() {
    for op in [
        Operation::GetClipboard,
        Operation::ReadClipboard,
        Operation::Cancel,
    ] {
        let mut s = Server::new();
        s.clipboard = text(10);
        let mut c = bound(&mut s, 0);
        s.refuse_reply = vec![(op, EAGAIN)];
        c.paste(0).unwrap();
        drive(&mut c, &mut s, 0);
        match op {
            // The bytes were whole, so they are the host's however the release went.
            Operation::Cancel => assert!(matches!(c.take_event(), Some(Event::Pasted(_)))),
            // Nothing was answered yet, so the job goes again.
            Operation::GetClipboard => {
                assert_eq!(c.take_event(), None);
                let at = c.next_deadline().unwrap();
                drive(&mut c, &mut s, at);
                assert_eq!(c.take_event(), Some(Event::Pasted(text(10))));
            }
            _ => assert_eq!(c.take_event(), Some(Event::PasteFailed(Why::Lost)), "{op:?}"),
        }
        assert_eq!(s.closes, 1, "{op:?}");
    }
}

#[test]
fn a_refusal_that_spoils_the_fid_ends_the_connection_and_the_next_job_works() {
    for errno in [-22, -7, -12, -95] {
        let mut s = Server::new();
        let mut c = bound(&mut s, 0);
        s.refuse = vec![(Operation::GetClipboard, errno)];
        c.paste(0).unwrap();
        drive(&mut c, &mut s, 0);
        assert!(
            matches!(c.take_event(), Some(Event::PasteFailed(Why::Refused(_)))),
            "{errno}"
        );
        assert_eq!(s.closes, 1, "{errno}");
        c.paste(SEC).unwrap();
        drive(&mut c, &mut s, SEC);
        assert!(matches!(c.take_event(), Some(Event::Pasted(_))), "{errno}");
    }
}

#[test]
fn a_badhandle_on_a_job_ends_the_connection_and_the_next_job_works() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.refuse = vec![(Operation::GetClipboard, -116)];
    c.paste(0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(
        c.take_event(),
        Some(Event::PasteFailed(Why::Refused(Failure::BadHandle)))
    );
    assert_eq!(s.closes, 1);
    c.paste(SEC).unwrap();
    drive(&mut c, &mut s, SEC);
    assert!(matches!(c.take_event(), Some(Event::Pasted(_))));
}

#[test]
fn the_table_reads_every_failure_by_its_own_value() {
    for f in [
        Failure::Denied,
        Failure::Gone,
        Failure::TooLarge,
        Failure::Conflict,
        Failure::NoMemory,
        Failure::Busy,
        Failure::Invalid,
        Failure::Unsupported,
        Failure::Timeout,
        Failure::BadHandle,
    ] {
        assert_eq!(failure(-(f as u32 as i32)), Some(f));
    }
}

#[test]
fn a_server_ebadf_is_no_answer_because_the_kernel_reads_it_as_eio() {
    assert_eq!(failure(-116), Some(Failure::BadHandle));
    assert_eq!(failure(-9), None);
    assert_eq!(failure(-5), None);
}

#[test]
fn a_write_refused_part_way_through_a_record_ends_the_connection() {
    // The next request would start at offset 0 against a half-assembled record.
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.max_io = 8169;
    s.refuse_partway = Some((Operation::WriteCopy, EAGAIN));
    c.copy(text(16 * 1024), 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(
        c.take_event(),
        Some(Event::CopyFailed(Why::Refused(Failure::Conflict)))
    );
    assert_eq!(s.closes, 1);
    assert_eq!(s.generation, 0);
    c.copy(text(10), SEC).unwrap();
    drive(&mut c, &mut s, SEC);
    assert_eq!(c.take_event(), Some(Event::Copied { generation: 1 }));
}

/// Hello and Bind answered, the report never: being bound is not having come back.
#[test]
fn a_server_that_binds_and_then_fails_the_report_spends_the_schedule_and_no_more() {
    let losses = 2 * RECONNECT_ATTEMPTS as usize;
    for (refused, on_the_reply) in [
        (vec![(Operation::ReportMode, ENOENT); losses], Vec::new()),
        (Vec::new(), vec![(Operation::ReportMode, ENOENT); losses]),
    ] {
        let mut s = Server::new();
        s.refuse = refused;
        s.refuse_reply = on_the_reply;
        let mut c = Client::new();
        c.report_mode(Mode::Normal, false, "Nora", 0).unwrap();
        drive(&mut c, &mut s, 0);
        assert!(s.operations(Operation::ReportMode) == 1, "it was asked");
        let mut spent = 0;
        while let Some(at) = c.next_deadline() {
            drive(&mut c, &mut s, at);
            spent += 1;
            assert!(spent <= RECONNECT_ATTEMPTS, "it kept connecting");
        }
        assert_eq!(spent, RECONNECT_ATTEMPTS);
        assert_eq!(s.connects, RECONNECT_ATTEMPTS + 1);
        c.report_mode(Mode::Insert, false, "Nora", SEC).unwrap();
        assert!(c.next_deadline().is_some(), "the host asked again");
    }
}

#[test]
fn a_mode_refused_so_that_it_spoils_the_fid_is_not_sent_for_ever() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.refuse = vec![(Operation::ReportMode, -22)];
    let before = s.connects;
    c.report_mode(Mode::Insert, false, "Nora", 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(c.take_event(), Some(Event::ModeRefused(Failure::Invalid)));
    assert_eq!(s.closes, 1);
    assert_eq!(c.next_deadline(), None, "nothing is owed until the host asks");
    assert_eq!(s.connects, before);
    c.report_mode(Mode::Normal, false, "Nora", SEC).unwrap();
    drive(&mut c, &mut s, SEC);
    assert!(c.is_bound());
    assert_eq!(s.modes.last().unwrap().1, Mode::Normal);
}

#[test]
fn a_commit_answered_with_the_wrong_generation_is_unknown() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    s.skew = 1;
    c.copy(text(10), 0).unwrap();
    drive(&mut c, &mut s, 0);
    assert_eq!(c.take_event(), Some(Event::CopyUnknown));
    assert_eq!(s.closes, 1);
}

#[test]
fn a_reply_trickled_for_ever_still_ends() {
    let mut s = Server::new();
    let mut c = bound(&mut s, 0);
    c.report_mode(Mode::Insert, false, "Nora", 0).unwrap();
    let Some(Step::Write { lane, bytes, .. }) = c.next_step(0) else {
        panic!("the report's write");
    };
    let n = bytes.len();
    let id = wire::frame(bytes).unwrap().0.request_id;
    c.wrote(lane, n, 0);
    let Some(Step::Read { .. }) = c.next_step(0) else {
        panic!("the report's read");
    };
    let header = Header {
        operation: Operation::ReportMode,
        response: true,
        request_id: id,
        length: MAX_RECORD,
    }
    .encode()
    .unwrap();
    c.read(lane, &header, 0);
    let mut now = 0;
    let step = IDLE_NS / 2;
    while now < LIFETIME_NS {
        assert!(c.is_bound(), "lost at {now}");
        let Some(Step::Read { .. }) = c.next_step(now) else {
            panic!("the next read");
        };
        now += step;
        c.read(lane, &[0], now);
    }
    assert!(c.next_deadline().is_some_and(|at| at <= LIFETIME_NS));
    assert!(matches!(c.next_step(LIFETIME_NS), Some(Step::Close)));
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
        assert_eq!(
            c.take_event(),
            Some(Event::PasteFailed(Why::Malformed)),
            "{}",
            bytes.len()
        );
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
