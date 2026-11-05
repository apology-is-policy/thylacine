//! The interaction client's protocol with no I/O (HALCYON-INTERACTION 7 and
//! ASYNC-SERVICE-LIFECYCLE, "AS-4 adoption design"). An adapter connects, moves
//! bytes and reports what happened; this decides what comes next, what each
//! deadline is and what the host is told. Nothing here can block.
use super::interaction_body::{Request, Response, Scope, IDLE_MS, LIFETIME_MS};
use super::interaction_wire::{
    self as wire, Failure, Header, Mode, Operation, HEADER_BYTES, MAX_CHUNK, MAX_LABEL, MAX_RECORD,
    MAX_TEXT,
};
use alloc::{collections::VecDeque, string::String, vec::Vec};

const MS: u64 = 1_000_000;
/// The server expires an unbound handshake at 2 s, so waiting longer is pointless.
const HANDSHAKE_NS: u64 = 2_000 * MS;
const IDLE_NS: u64 = IDLE_MS as u64 * MS;
const LIFETIME_NS: u64 = LIFETIME_MS as u64 * MS;
/// A foreground nomination may not be published yet when a new owner first binds.
const BIND_ATTEMPTS: u32 = 8;
const BIND_RETRY_NS: u64 = 20 * MS;
const RECONNECT_MIN_NS: u64 = 250 * MS;
const RECONNECT_MAX_NS: u64 = 8_000 * MS;
/// About half a minute of tries, which outlasts an ordinary trusted-seat episode.
const RECONNECT_ATTEMPTS: u32 = 8;
/// One terminal application has one text context; a host with more binds more clients.
const CONTEXT: u64 = 1;
const EPOCH: u64 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Lane {
    /// Hello, Bind and Mode.
    Control,
    /// One copy or paste, so a parked Get never holds up a mode report.
    Transfer,
}

/// What the adapter does next. A step's bytes are valid until the next call.
#[derive(Debug, PartialEq, Eq)]
pub enum Step<'a> {
    /// Connect, walk to the interaction file and open both lanes' fids.
    Connect,
    /// Write from the start of `bytes`; a short write is fine.
    Write {
        lane: Lane,
        offset: u64,
        bytes: &'a [u8],
    },
    /// Read up to `count`; a short read is fine.
    Read { lane: Lane, offset: u64, count: u32 },
    /// End the connection now: drop every fid, the scope and what is in flight.
    /// No completion for anything started before this may be reported afterwards.
    Close,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Why {
    Refused(Failure),
    /// The connection could not be made, or went away.
    Lost,
    TimedOut,
    /// The server's reply, or the clipboard's bytes, broke the protocol's rules.
    Malformed,
    /// The host gave up on it.
    Cancelled,
}

#[derive(Debug, PartialEq, Eq)]
pub enum Event {
    Copied {
        generation: u64,
    },
    /// The commit may or may not have landed. Never retried: the user may copy again.
    CopyUnknown,
    CopyFailed(Why),
    /// The whole clipboard, checked as text. Nothing is delivered before this.
    Pasted(Vec<u8>),
    PasteFailed(Why),
    ModeRefused(Failure),
}

/// Why the host's own call was turned down, before anything was sent.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    Busy,
    TooLarge,
    NotText,
}

#[derive(Clone, PartialEq, Eq)]
struct ModeReport {
    mode: Mode,
    readonly: bool,
    label: String,
}

struct Exchange {
    id: u64,
    operation: Operation,
    record: Vec<u8>,
    sent: usize,
    reply: Vec<u8>,
    length: Option<usize>,
    in_flight: bool,
    idle_at: u64,
    /// However steadily a server trickles, an exchange ends here.
    give_up: u64,
}

enum Link {
    Down {
        retry_at: Option<u64>,
    },
    Connecting {
        deadline: u64,
    },
    Hello {
        deadline: u64,
    },
    Bind {
        deadline: u64,
        session: u64,
        attempt: u32,
        not_before: u64,
    },
    Bound,
    Closing,
}

enum Job {
    Paste(PasteStage),
    Copy {
        text: Vec<u8>,
        stage: CopyStage,
    },
    /// A failed job's open transfer. Its event has gone; only the slot is owed.
    Release {
        transfer: u64,
    },
}
enum PasteStage {
    Get,
    Reading {
        transfer: u64,
        length: usize,
        bytes: Vec<u8>,
    },
    /// All read. The bytes are the host's however the release goes.
    Releasing {
        transfer: u64,
        result: Result<Vec<u8>, Why>,
    },
}
enum CopyStage {
    /// The current generation, which Commit must name.
    Get,
    ReleaseRead {
        transfer: u64,
        generation: u64,
    },
    Begin {
        generation: u64,
    },
    Writing {
        transfer: u64,
        generation: u64,
        offset: usize,
        length: usize,
    },
    Commit {
        transfer: u64,
        generation: u64,
        handed: bool,
    },
}

pub struct Client {
    link: Link,
    scope: Option<Scope>,
    next_id: u64,
    lanes: [Option<Exchange>; 2],
    job: Option<Job>,
    job_expires: u64,
    wanted: Option<ModeReport>,
    reported: Option<ModeReport>,
    sequence: u64,
    retries: u32,
    backoff: u64,
    events: VecDeque<Event>,
    rerun_spent: bool,
    reruns: u32,
}

fn index(lane: Lane) -> usize {
    match lane {
        Lane::Control => 0,
        Lane::Transfer => 1,
    }
}

impl Default for Client {
    fn default() -> Self {
        Self::new()
    }
}

impl Client {
    pub fn new() -> Self {
        Self {
            link: Link::Down { retry_at: None },
            scope: None,
            next_id: 1,
            lanes: [None, None],
            job: None,
            job_expires: 0,
            wanted: None,
            reported: None,
            sequence: 0,
            retries: RECONNECT_ATTEMPTS,
            backoff: RECONNECT_MIN_NS,
            events: VecDeque::new(),
            rerun_spent: false,
            reruns: 0,
        }
    }

    pub fn is_bound(&self) -> bool {
        matches!(self.link, Link::Bound)
    }

    pub fn take_event(&mut self) -> Option<Event> {
        self.events.pop_front()
    }

    /// Jobs that have gone again on a fresh connection, ever. A job that recovers
    /// reports no failure, so this is the only sign its connection died.
    pub fn reruns(&self) -> u32 {
        self.reruns
    }

    /// The latest value is what reaches the server. Older ones are dropped.
    pub fn report_mode(
        &mut self,
        mode: Mode,
        readonly: bool,
        label: &str,
        now: u64,
    ) -> Result<(), Refusal> {
        if label.len() > MAX_LABEL {
            return Err(Refusal::TooLarge);
        }
        if label.chars().any(char::is_control) {
            return Err(Refusal::NotText);
        }
        self.wanted = Some(ModeReport {
            mode,
            readonly,
            label: String::from(label),
        });
        self.want_link(now);
        Ok(())
    }

    pub fn copy(&mut self, text: Vec<u8>, now: u64) -> Result<(), Refusal> {
        if self.job.is_some() {
            return Err(Refusal::Busy);
        }
        if text.len() > MAX_TEXT {
            return Err(Refusal::TooLarge);
        }
        if wire::clipboard_text(&text).is_err() {
            return Err(Refusal::NotText);
        }
        self.job = Some(Job::Copy {
            text,
            stage: CopyStage::Get,
        });
        self.rerun_spent = false;
        self.job_expires = now.saturating_add(LIFETIME_NS);
        self.want_link(now);
        Ok(())
    }

    pub fn paste(&mut self, now: u64) -> Result<(), Refusal> {
        if self.job.is_some() {
            return Err(Refusal::Busy);
        }
        self.job = Some(Job::Paste(PasteStage::Get));
        self.rerun_spent = false;
        self.job_expires = now.saturating_add(LIFETIME_NS);
        self.want_link(now);
        Ok(())
    }

    /// Give up on the job in progress. Only closing the connection is sure to end
    /// a request the server has stopped answering, so that is what this does.
    pub fn cancel(&mut self) {
        if self.job.is_some() {
            self.lose_link(Why::Cancelled);
        }
    }

    fn want_link(&mut self, now: u64) {
        self.retries = RECONNECT_ATTEMPTS;
        self.backoff = RECONNECT_MIN_NS;
        if let Link::Down { retry_at } = &mut self.link {
            *retry_at = Some(now);
        }
    }

    fn wants_link(&self) -> bool {
        self.job.is_some() || (self.wanted.is_some() && self.wanted != self.reported)
    }

    /// The earliest time something here is owed, for the host's poll timeout.
    pub fn next_deadline(&self) -> Option<u64> {
        let mut soonest = match self.link {
            Link::Down { retry_at } => retry_at,
            Link::Connecting { deadline } | Link::Hello { deadline } => Some(deadline),
            Link::Bind {
                deadline,
                not_before,
                ..
            } => Some(if self.lanes[0].is_none() {
                deadline.min(not_before)
            } else {
                deadline
            }),
            Link::Bound => None,
            Link::Closing => Some(0),
        };
        let mut earlier = |t: u64| soonest = Some(soonest.map_or(t, |s: u64| s.min(t)));
        for exchange in self.lanes.iter().flatten() {
            earlier(exchange.idle_at);
        }
        if self.job.is_some() {
            earlier(self.job_expires);
        }
        soonest
    }

    /// The next thing for the adapter to start, or None when it should wait for a
    /// completion or for next_deadline. Call until None, after every event.
    pub fn next_step(&mut self, now: u64) -> Option<Step<'_>> {
        self.check_deadlines(now);
        match self.link {
            Link::Closing => {
                self.link = Link::Down {
                    retry_at: self.retry_time(now),
                };
                return Some(Step::Close);
            }
            Link::Down { retry_at: Some(at) } if now >= at => {
                self.link = Link::Connecting {
                    deadline: now.saturating_add(HANDSHAKE_NS),
                };
                return Some(Step::Connect);
            }
            Link::Down { .. } | Link::Connecting { .. } => return None,
            _ => {}
        }
        self.start_control(now);
        self.start_transfer(now);
        let lane = [Lane::Control, Lane::Transfer]
            .into_iter()
            .find(|&l| self.lanes[index(l)].as_ref().is_some_and(|e| !e.in_flight))?;
        let exchange = self.lanes[index(lane)].as_mut()?;
        exchange.in_flight = true;
        if exchange.sent < exchange.record.len() {
            if let Some(Job::Copy {
                stage: CopyStage::Commit { handed, .. },
                ..
            }) = &mut self.job
            {
                if lane == Lane::Transfer {
                    *handed = true;
                }
            }
            let offset = exchange.sent as u64;
            return Some(Step::Write {
                lane,
                offset,
                bytes: &exchange.record[exchange.sent..],
            });
        }
        let got = exchange.reply.len();
        let count = exchange.length.unwrap_or(MAX_RECORD) - got;
        Some(Step::Read {
            lane,
            offset: got as u64,
            count: count as u32,
        })
    }

    fn check_deadlines(&mut self, now: u64) {
        let handshake = match self.link {
            Link::Connecting { deadline } | Link::Hello { deadline } => Some(deadline),
            Link::Bind { deadline, .. } => Some(deadline),
            _ => None,
        };
        let idle = self.lanes.iter().flatten().any(|e| now >= e.idle_at);
        if handshake.is_some_and(|d| now >= d)
            || idle
            || (self.job.is_some() && now >= self.job_expires)
        {
            self.lose_link(Why::TimedOut);
        }
    }

    fn retry_time(&mut self, now: u64) -> Option<u64> {
        if !self.wants_link() || self.retries == 0 {
            return None;
        }
        self.retries -= 1;
        let at = now.saturating_add(self.backoff);
        self.backoff = (self.backoff * 2).min(RECONNECT_MAX_NS);
        Some(at)
    }

    fn exchange(&mut self, lane: Lane, request: Request<'_>, now: u64) {
        let id = self.next_id;
        self.next_id += 1;
        let operation = match request {
            Request::Hello => Operation::Hello,
            Request::Bind { .. } => Operation::BindController,
            Request::Mode { .. } => Operation::ReportMode,
            Request::Get { .. } => Operation::GetClipboard,
            Request::Read { .. } => Operation::ReadClipboard,
            Request::Begin { .. } => Operation::BeginCopy,
            Request::Write { .. } => Operation::WriteCopy,
            Request::Commit { .. } => Operation::CommitCopy,
            Request::Cancel { .. } => Operation::Cancel,
            Request::Unbind { .. } => Operation::UnbindController,
        };
        // Every value reaching here was checked on its way in or came from the
        // server's checked reply, so a refusal is a defect here, not an input.
        let Ok(record) = request.encode(id) else {
            self.lose_link(Why::Malformed);
            return;
        };
        self.lanes[index(lane)] = Some(Exchange {
            id,
            operation,
            record,
            sent: 0,
            reply: Vec::new(),
            length: None,
            in_flight: false,
            idle_at: now.saturating_add(IDLE_NS),
            give_up: now.saturating_add(LIFETIME_NS),
        });
    }

    fn start_control(&mut self, now: u64) {
        if self.lanes[0].is_some() {
            return;
        }
        match self.link {
            Link::Hello { .. } => self.exchange(Lane::Control, Request::Hello, now),
            Link::Bind {
                session,
                not_before,
                ..
            } if now >= not_before => self.exchange(
                Lane::Control,
                Request::Bind {
                    session,
                    context: CONTEXT,
                    epoch: EPOCH,
                },
                now,
            ),
            Link::Bound if self.wanted != self.reported => {
                let (Some(scope), Some(report)) = (self.scope, self.wanted.clone()) else {
                    return;
                };
                self.sequence += 1;
                let request = Request::Mode {
                    scope,
                    sequence: self.sequence,
                    mode: report.mode,
                    readonly: report.readonly,
                    label: &report.label,
                };
                self.exchange(Lane::Control, request, now);
                self.reported = Some(report);
            }
            _ => {}
        }
    }

    fn start_transfer(&mut self, now: u64) {
        if self.lanes[1].is_some() || !self.is_bound() {
            return;
        }
        let Some(scope) = self.scope else { return };
        let mut job = self.job.take();
        match &mut job {
            Some(Job::Paste(stage)) => match stage {
                PasteStage::Get => self.exchange(Lane::Transfer, Request::Get { scope }, now),
                PasteStage::Reading {
                    transfer,
                    length,
                    bytes,
                } => {
                    if bytes.len() < *length {
                        let request = Request::Read {
                            transfer: *transfer,
                            offset: bytes.len() as u32,
                            count: (*length - bytes.len()).min(MAX_CHUNK) as u32,
                        };
                        self.exchange(Lane::Transfer, request, now);
                    }
                }
                PasteStage::Releasing { transfer, .. } => self.exchange(
                    Lane::Transfer,
                    Request::Cancel {
                        transfer: *transfer,
                    },
                    now,
                ),
            },
            Some(Job::Copy { text, stage }) => match stage {
                CopyStage::Get => self.exchange(Lane::Transfer, Request::Get { scope }, now),
                CopyStage::ReleaseRead { transfer, .. } => self.exchange(
                    Lane::Transfer,
                    Request::Cancel {
                        transfer: *transfer,
                    },
                    now,
                ),
                CopyStage::Begin { .. } => {
                    let request = Request::Begin {
                        scope,
                        length: text.len() as u32,
                    };
                    self.exchange(Lane::Transfer, request, now);
                }
                CopyStage::Writing {
                    transfer,
                    offset,
                    length,
                    ..
                } => {
                    let end = (*offset + MAX_CHUNK).min(*length);
                    let request = Request::Write {
                        transfer: *transfer,
                        offset: *offset as u32,
                        data: &text[*offset..end],
                    };
                    self.exchange(Lane::Transfer, request, now);
                }
                CopyStage::Commit {
                    transfer,
                    generation,
                    ..
                } => {
                    let request = Request::Commit {
                        scope,
                        transfer: *transfer,
                        expected: *generation,
                    };
                    self.exchange(Lane::Transfer, request, now);
                }
            },
            Some(Job::Release { transfer }) => self.exchange(
                Lane::Transfer,
                Request::Cancel {
                    transfer: *transfer,
                },
                now,
            ),
            None => {}
        }
        if self.job.is_none() {
            self.job = job;
        }
    }

    /// The connection ends here. What was in flight goes with it. The job, if any,
    /// runs once more when nothing had answered it (vote 60), and otherwise learns its
    /// fate now rather than at a reply that will not come.
    fn lose_link(&mut self, why: Why) {
        let was_bound = matches!(self.link, Link::Bound);
        self.lanes = [None, None];
        self.scope = None;
        self.reported = None;
        // A SAK closes an idle connection and nothing says so until its next use. A Get
        // changes nothing, so a job still waiting on its first answer goes again, once.
        // A connection that was never bound served nobody, and the schedule decides those.
        let unanswered = matches!(
            self.job,
            Some(Job::Paste(PasteStage::Get))
                | Some(Job::Copy {
                    stage: CopyStage::Get,
                    ..
                })
        );
        if matches!(why, Why::Lost) && was_bound && unanswered && !self.rerun_spent {
            self.rerun_spent = true;
            self.reruns += 1;
            self.link = Link::Closing;
            return;
        }
        match self.job.take() {
            Some(Job::Copy {
                stage: CopyStage::Commit { handed: true, .. },
                ..
            }) => self.events.push_back(Event::CopyUnknown),
            Some(Job::Copy { .. }) => self.events.push_back(Event::CopyFailed(why)),
            Some(Job::Paste(PasteStage::Releasing { result, .. })) => self.deliver_paste(result),
            Some(Job::Paste(_)) => self.events.push_back(Event::PasteFailed(why)),
            Some(Job::Release { .. }) | None => {}
        }
        // A connection that never formed has nothing for the adapter to close, but
        // closing nothing is harmless, and one path keeps the retry rule in one place.
        self.link = Link::Closing;
    }

    fn deliver_paste(&mut self, result: Result<Vec<u8>, Why>) {
        self.events.push_back(match result {
            Ok(bytes) => Event::Pasted(bytes),
            Err(why) => Event::PasteFailed(why),
        });
    }

    /// Fail the job with `why`, keeping the connection. An open transfer holds one of
    /// the session's slots until it expires, so it is released first.
    fn fail_job(&mut self, why: Why, transfer: Option<u64>) {
        let event = match self.job.take() {
            Some(Job::Copy { .. }) => Event::CopyFailed(why),
            Some(Job::Paste(_)) => Event::PasteFailed(why),
            Some(Job::Release { .. }) | None => return,
        };
        self.events.push_back(event);
        self.job = transfer.map(|transfer| Job::Release { transfer });
    }

    pub fn connected(&mut self) {
        if let Link::Connecting { deadline } = self.link {
            self.link = Link::Hello { deadline };
        }
    }

    pub fn connect_failed(&mut self) {
        if matches!(self.link, Link::Connecting { .. }) {
            self.lose_link(Why::Lost);
        }
    }

    fn live(&mut self, lane: Lane) -> Option<&mut Exchange> {
        self.lanes[index(lane)].as_mut().filter(|e| e.in_flight)
    }

    pub fn wrote(&mut self, lane: Lane, count: usize, now: u64) {
        let Some(exchange) = self.live(lane) else {
            return;
        };
        exchange.in_flight = false;
        if count == 0 || count > exchange.record.len() - exchange.sent {
            self.lose_link(Why::Malformed);
            return;
        }
        exchange.sent += count;
        exchange.idle_at = now.saturating_add(IDLE_NS).min(exchange.give_up);
    }

    pub fn read(&mut self, lane: Lane, bytes: &[u8], now: u64) {
        let Some(exchange) = self.live(lane) else {
            return;
        };
        exchange.in_flight = false;
        let room = exchange.length.unwrap_or(MAX_RECORD) - exchange.reply.len();
        if bytes.is_empty() || bytes.len() > room {
            self.lose_link(Why::Malformed);
            return;
        }
        exchange.reply.extend_from_slice(bytes);
        exchange.idle_at = now.saturating_add(IDLE_NS).min(exchange.give_up);
        if exchange.length.is_none() && exchange.reply.len() >= HEADER_BYTES {
            match Header::decode(&exchange.reply[..HEADER_BYTES]) {
                Ok(h)
                    if h.response
                        && h.request_id == exchange.id
                        && h.operation == exchange.operation
                        && exchange.reply.len() <= h.length =>
                {
                    exchange.length = Some(h.length);
                }
                _ => {
                    self.lose_link(Why::Malformed);
                    return;
                }
            }
        }
        if exchange.length == Some(exchange.reply.len()) {
            self.answered(lane, now);
        }
    }

    /// A refusal the server chose is an answer, and it comes on the write that finishes
    /// a record: success there means the request ran. So an errno on a reply's read is
    /// no answer whatever its value, and on a commit's reply it leaves the commit
    /// unknown. Anything else means the transport failed, and with it the connection.
    pub fn failed(&mut self, lane: Lane, errno: i32, now: u64) {
        let Some(exchange) = self.live(lane) else {
            return;
        };
        let written = exchange.sent == exchange.record.len();
        let partial = exchange.sent > 0;
        match failure(errno) {
            Some(f) if !written => self.refused(lane, f, partial, now),
            _ => self.lose_link(Why::Lost),
        }
    }

    fn refused(&mut self, lane: Lane, f: Failure, partial: bool, now: u64) {
        self.lanes[index(lane)] = None;
        // A record the server could not take poisons its fid until the fid closes, and
        // part of a record leaves the next request at the wrong offset. These values are
        // also what a whole request can be refused with, and nothing here tells the two
        // apart. Keeping the connection would have every later request refused for ever.
        let spoiled = partial
            || matches!(
                f,
                Failure::Invalid | Failure::TooLarge | Failure::NoMemory | Failure::Unsupported
            );
        if lane == Lane::Control {
            match self.link {
                Link::Bind {
                    deadline,
                    session,
                    attempt,
                    ..
                } if !partial
                    && matches!(f, Failure::Gone | Failure::Busy)
                    && attempt + 1 < BIND_ATTEMPTS =>
                {
                    self.link = Link::Bind {
                        deadline,
                        session,
                        attempt: attempt + 1,
                        not_before: now.saturating_add(BIND_RETRY_NS),
                    };
                }
                Link::Bind { .. } | Link::Hello { .. } => {
                    // A nomination still to come, an owner still leaving and a seat
                    // away for a trusted episode can each change the answer without the
                    // host asking again, and the schedule bounds what is spent on them.
                    if !matches!(f, Failure::Gone | Failure::Busy | Failure::Denied) {
                        self.retries = 0;
                    }
                    self.lose_link(Why::Refused(f));
                }
                _ if spoiled => {
                    // Not sent again until the host reports anew, or a server that
                    // always refuses it would be connected to for ever.
                    self.wanted = None;
                    self.events.push_back(Event::ModeRefused(f));
                    self.lose_link(Why::Lost);
                }
                _ if matches!(f, Failure::Gone | Failure::BadHandle) => self.lose_link(Why::Lost),
                _ => self.events.push_back(Event::ModeRefused(f)),
            }
            return;
        }
        // On the write, so a commit refused here did not land.
        if let Some(Job::Copy {
            stage: CopyStage::Commit { handed, .. },
            ..
        }) = &mut self.job
        {
            *handed = false;
        }
        if spoiled {
            self.lose_link(Why::Refused(f));
            return;
        }
        // A release that fails has nothing left to protect.
        let transfer = match self.job.as_ref() {
            Some(Job::Paste(PasteStage::Releasing { .. })) => {
                if let Some(Job::Paste(PasteStage::Releasing { result, .. })) = self.job.take() {
                    self.deliver_paste(result);
                }
                return;
            }
            Some(Job::Copy {
                stage: CopyStage::ReleaseRead { generation, .. },
                ..
            }) => {
                let generation = *generation;
                if let Some(Job::Copy { stage, .. }) = &mut self.job {
                    *stage = CopyStage::Begin { generation };
                }
                return;
            }
            Some(Job::Release { .. }) => {
                self.job = None;
                return;
            }
            Some(Job::Paste(PasteStage::Reading { transfer, .. })) => Some(*transfer),
            Some(Job::Copy {
                stage: CopyStage::Writing { transfer, .. },
                ..
            })
            | Some(Job::Copy {
                stage: CopyStage::Commit { transfer, .. },
                ..
            }) => Some(*transfer),
            _ => None,
        };
        // A fid the server no longer knows answers every later request the same way.
        if matches!(f, Failure::Gone | Failure::BadHandle) {
            self.lose_link(Why::Refused(f));
            return;
        }
        self.fail_job(Why::Refused(f), transfer);
    }

    fn answered(&mut self, lane: Lane, now: u64) {
        let Some(exchange) = self.lanes[index(lane)].take() else {
            return;
        };
        let Ok(response) = Response::decode(&exchange.reply, exchange.operation, exchange.id)
        else {
            self.lose_link(Why::Malformed);
            return;
        };
        match lane {
            Lane::Control => self.control_answer(response, now),
            Lane::Transfer => self.transfer_answer(response),
        }
    }

    fn control_answer(&mut self, response: Response<'_>, now: u64) {
        match (&self.link, response) {
            (Link::Hello { deadline }, Response::Hello { session }) => {
                self.link = Link::Bind {
                    deadline: *deadline,
                    session,
                    attempt: 0,
                    not_before: now,
                };
            }
            (Link::Bind { session, .. }, Response::Bound { controller }) => {
                self.scope = Some(Scope {
                    session: *session,
                    controller,
                    context: CONTEXT,
                    epoch: EPOCH,
                });
                // The schedule stays spent: a server that binds and then fails the report
                // it was bound for would otherwise be connected to for ever. Only the host
                // asking again restores it.
                self.link = Link::Bound;
            }
            (Link::Bound, Response::Mode) => {}
            _ => self.lose_link(Why::Malformed),
        }
    }

    fn transfer_answer(&mut self, response: Response<'_>) {
        match (self.job.take(), response) {
            (
                Some(Job::Paste(PasteStage::Get)),
                Response::Clipboard {
                    transfer, length, ..
                },
            ) => {
                // length was bounded by the decoder, so this cannot be a hostile size.
                let mut bytes = Vec::new();
                if bytes.try_reserve_exact(length as usize).is_err() {
                    self.job = Some(Job::Paste(PasteStage::Get));
                    self.fail_job(Why::Refused(Failure::NoMemory), Some(transfer));
                    return;
                }
                let length = length as usize;
                self.job = Some(Job::Paste(if length == 0 {
                    PasteStage::Releasing {
                        transfer,
                        result: Ok(bytes),
                    }
                } else {
                    PasteStage::Reading {
                        transfer,
                        length,
                        bytes,
                    }
                }));
            }
            (
                Some(Job::Paste(PasteStage::Reading {
                    transfer,
                    length,
                    mut bytes,
                })),
                Response::Read { offset, data },
            ) => {
                if offset as usize != bytes.len()
                    || data.is_empty()
                    || data.len() > length - bytes.len()
                {
                    self.job = Some(Job::Paste(PasteStage::Get));
                    self.fail_job(Why::Malformed, Some(transfer));
                    return;
                }
                bytes.extend_from_slice(data);
                self.job = Some(Job::Paste(if bytes.len() < length {
                    PasteStage::Reading {
                        transfer,
                        length,
                        bytes,
                    }
                } else {
                    let result = match wire::clipboard_text(&bytes) {
                        Ok(_) => Ok(bytes),
                        Err(_) => Err(Why::Malformed),
                    };
                    PasteStage::Releasing { transfer, result }
                }));
            }
            (Some(Job::Paste(PasteStage::Releasing { result, .. })), Response::Cancelled) => {
                self.deliver_paste(result);
            }
            (
                Some(Job::Copy {
                    text,
                    stage: CopyStage::Get,
                }),
                Response::Clipboard {
                    transfer,
                    generation,
                    ..
                },
            ) => {
                self.job = Some(Job::Copy {
                    text,
                    stage: CopyStage::ReleaseRead {
                        transfer,
                        generation,
                    },
                });
            }
            (
                Some(Job::Copy {
                    text,
                    stage: CopyStage::ReleaseRead { generation, .. },
                }),
                Response::Cancelled,
            ) => {
                self.job = Some(Job::Copy {
                    text,
                    stage: CopyStage::Begin { generation },
                });
            }
            (
                Some(Job::Copy {
                    text,
                    stage: CopyStage::Begin { generation },
                }),
                Response::Begun { transfer },
            ) => {
                let length = text.len();
                let stage = if length == 0 {
                    CopyStage::Commit {
                        transfer,
                        generation,
                        handed: false,
                    }
                } else {
                    CopyStage::Writing {
                        transfer,
                        generation,
                        offset: 0,
                        length,
                    }
                };
                self.job = Some(Job::Copy { text, stage });
            }
            (
                Some(Job::Copy {
                    text,
                    stage:
                        CopyStage::Writing {
                            transfer,
                            generation,
                            offset,
                            length,
                        },
                }),
                Response::Written { count },
            ) => {
                let asked = (length - offset).min(MAX_CHUNK);
                let count = count as usize;
                if count == 0 || count > asked {
                    self.job = Some(Job::Copy {
                        text,
                        stage: CopyStage::Get,
                    });
                    self.fail_job(Why::Malformed, Some(transfer));
                    return;
                }
                let offset = offset + count;
                let stage = if offset < length {
                    CopyStage::Writing {
                        transfer,
                        generation,
                        offset,
                        length,
                    }
                } else {
                    CopyStage::Commit {
                        transfer,
                        generation,
                        handed: false,
                    }
                };
                self.job = Some(Job::Copy { text, stage });
            }
            (
                Some(Job::Copy {
                    text,
                    stage:
                        CopyStage::Commit {
                            transfer,
                            generation: expected,
                            handed,
                        },
                }),
                Response::Committed { generation },
            ) => {
                if expected.checked_add(1) != Some(generation) {
                    self.job = Some(Job::Copy {
                        text,
                        stage: CopyStage::Commit {
                            transfer,
                            generation: expected,
                            handed,
                        },
                    });
                    self.lose_link(Why::Malformed);
                    return;
                }
                self.events.push_back(Event::Copied { generation });
            }
            (Some(Job::Release { .. }), Response::Cancelled) => {}
            (job, _) => {
                self.job = job;
                self.lose_link(Why::Malformed);
            }
        }
    }
}

fn failure(errno: i32) -> Option<Failure> {
    Some(match errno.unsigned_abs() {
        1 => Failure::Denied,
        2 => Failure::Gone,
        7 => Failure::TooLarge,
        11 => Failure::Conflict,
        12 => Failure::NoMemory,
        16 => Failure::Busy,
        22 => Failure::Invalid,
        95 => Failure::Unsupported,
        110 => Failure::Timeout,
        116 => Failure::BadHandle,
        _ => return None,
    })
}

#[cfg(test)]
#[path = "interaction_client_tests.rs"]
mod tests;
