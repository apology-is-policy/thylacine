//! The interaction client over a private Loom ring (ASYNC-SERVICE-LIFECYCLE, "AS-4
//! adoption design"). A UI host keeps its one poll: it adds `poll_fd`, bounds the
//! sleep by `next_deadline` and calls `pump`, which never sleeps. Dropping the host
//! closes the ring, which ends the whole private connection whatever the server is
//! doing. Only the thread that made it may use it, as with the ring itself.
#![cfg_attr(not(test), no_std)]
extern crate alloc;

use alloc::{boxed::Box, vec::Vec};
use core::{
    future::Future,
    pin::Pin,
    task::{Context, Poll, Waker},
};
use libhalcyon::interaction_client::{Client, Event, Lane, Refusal, Step};
use libhalcyon::interaction_wire::{Mode, MAX_RECORD};
use loom_client::native::{
    Connect, Fid, Kernel, PoolMember, Read, Result, Ring, Scope, Target, Write,
};

const ENTRIES: u32 = 8;
const INTERACTION: &[u8] = b"interaction";
const MAX_NAME: usize = 255;
const EIO: i32 = 5;
const EBUSY: i32 = 16;
/// A refusal of our own submission, in a form no server's refusal takes: the client
/// reads a refusal it knows as the server's answer, and this was not one.
const LOST: i32 = -EIO;
/// Passes through one pump, so that a server answering as fast as it is asked cannot
/// hold a host's input. Every pass leaves its work announced by the ring.
const PASSES: usize = 16;

/// Where HALCYON_INTERACTION says the service is. The locator is not authority.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Locator<'a> {
    pub service: &'a [u8],
    pub token: &'a [u8],
}

fn name_ok(name: &[u8]) -> bool {
    !name.is_empty()
        && name.len() <= MAX_NAME
        && name != b"."
        && name != b".."
        && !name.contains(&0)
}

/// `/srv/<service>/<token>/interaction`, and nothing else.
pub fn parse_locator(value: &[u8]) -> Option<Locator<'_>> {
    let rest = value.strip_prefix(b"/srv/")?;
    let mut parts = rest.split(|&b| b == b'/');
    let (service, token, leaf) = (parts.next()?, parts.next()?, parts.next()?);
    if parts.next().is_some() || leaf != INTERACTION || !name_ok(service) || !name_ok(token) {
        return None;
    }
    Some(Locator { service, token })
}

struct Connection<K: Kernel> {
    _scope: Scope<K>,
    fids: [Fid<K>; 2],
}

enum Op<K: Kernel> {
    Idle,
    /// A closed connection's aborted operation still holds this lane's buffer until
    /// its terminal, which is a poll event. If that never came, the client's deadline
    /// for this step would end the wait. A lane's first operation is always a write,
    /// so no read can find its buffer lent.
    Parked {
        offset: u64,
        bytes: Vec<u8>,
    },
    Write(Write<K>),
    Read(Read<K>),
}

const LANES: [Lane; 2] = [Lane::Control, Lane::Transfer];

fn index(lane: Lane) -> usize {
    match lane {
        Lane::Control => 0,
        Lane::Transfer => 1,
    }
}

fn range(lane: Lane, length: usize) -> PoolMember {
    PoolMember {
        buffer_index: index(lane) as u32,
        reserved: 0,
        offset: 0,
        length: length as u64,
    }
}

fn open_write<K: Kernel>(fid: &Fid<K>, lane: Lane, offset: u64, bytes: &[u8]) -> Result<Write<K>> {
    let n = bytes.len().min(MAX_RECORD);
    fid.write(range(lane, n), offset, &bytes[..n], None)
}

fn open_read<K: Kernel>(fid: &Fid<K>, lane: Lane, offset: u64, count: u32) -> Result<Read<K>> {
    fid.read(range(lane, (count as usize).min(MAX_RECORD)), offset, None)
}

async fn open_lane<K: Kernel>(scope: &Scope<K>, token: &[u8]) -> Result<Fid<K>> {
    let directory = scope.walk(token, None)?.await?;
    let fid = directory.walk(INTERACTION, None)?.await?;
    fid.open(2, None)?.await?;
    Ok(fid)
}

async fn establish<K: Kernel>(connect: Connect<K>, token: Vec<u8>) -> Result<Connection<K>> {
    let scope = connect.await?;
    let control = open_lane(&scope, &token).await?;
    let transfer = open_lane(&scope, &token).await?;
    Ok(Connection {
        _scope: scope,
        fids: [control, transfer],
    })
}

type Connecting<K> = Pin<Box<dyn Future<Output = Result<Connection<K>>>>>;

pub struct Host<K: Kernel + 'static> {
    client: Client,
    connecting: Option<Connecting<K>>,
    connection: Option<Connection<K>>,
    ops: [Op<K>; 2],
    token: Vec<u8>,
    target: Target<K>,
    ring: Ring<K>,
}

impl<K: Kernel + 'static> Host<K> {
    /// Without the private ring this answers -EOPNOTSUPP. That is not an error state:
    /// the host carries on with its local registers and tries no other path.
    /// `registry_fd` is from open("/srv", O_PATH), and is read only here.
    pub fn new(kernel: K, registry_fd: i32, locator: Locator<'_>) -> Result<Self> {
        let ring = Ring::setup(kernel, ENTRIES, &[MAX_RECORD, MAX_RECORD])?;
        let target = ring.target(registry_fd, locator.service)?;
        Ok(Self {
            client: Client::new(),
            connecting: None,
            connection: None,
            ops: [Op::Idle, Op::Idle],
            token: locator.token.to_vec(),
            target,
            ring,
        })
    }

    /// The monotonic nanoseconds every other time here is in.
    pub fn now(&self) -> u64 {
        self.ring.now()
    }

    /// For the host's poll set, POLLIN. None once the ring is closed.
    pub fn poll_fd(&self) -> Option<i32> {
        let fd = self.ring.poll_fd();
        (fd >= 0).then_some(fd)
    }

    /// The latest the host's poll may sleep until, if anything is owed.
    pub fn next_deadline(&self) -> Option<u64> {
        self.client.next_deadline()
    }

    pub fn is_bound(&self) -> bool {
        self.client.is_bound()
    }

    pub fn take_event(&mut self) -> Option<Event> {
        self.client.take_event()
    }

    pub fn report_mode(
        &mut self,
        mode: Mode,
        readonly: bool,
        label: &str,
        now: u64,
    ) -> core::result::Result<(), Refusal> {
        let result = self.client.report_mode(mode, readonly, label, now);
        self.pump(now);
        result
    }

    pub fn copy(&mut self, text: Vec<u8>, now: u64) -> core::result::Result<(), Refusal> {
        let result = self.client.copy(text, now);
        self.pump(now);
        result
    }

    pub fn paste(&mut self, now: u64) -> core::result::Result<(), Refusal> {
        let result = self.client.paste(now);
        self.pump(now);
        result
    }

    pub fn cancel(&mut self, now: u64) {
        self.client.cancel();
        self.pump(now);
    }

    /// Everything that can move without sleeping. Call it when `poll_fd` is ready or
    /// `next_deadline` has passed. Whatever it leaves is announced by one of those.
    pub fn pump(&mut self, now: u64) {
        for _ in 0..PASSES {
            // A ring's fault reaches each operation as that operation's failure.
            let mut moved = self.ring.turn(Some(now)).is_ok_and(|routed| routed > 0);
            moved |= self.poll_connecting();
            moved |= self.poll_op(0, now);
            moved |= self.poll_op(1, now);
            moved |= self.start_steps(now);
            if !moved {
                return;
            }
        }
    }

    fn poll_connecting(&mut self) -> bool {
        let Some(future) = self.connecting.as_mut() else {
            return false;
        };
        let mut cx = Context::from_waker(Waker::noop());
        let Poll::Ready(result) = future.as_mut().poll(&mut cx) else {
            return false;
        };
        self.connecting = None;
        match result {
            Ok(connection) => {
                self.connection = Some(connection);
                self.client.connected();
            }
            Err(_) => self.client.connect_failed(),
        }
        true
    }

    fn poll_op(&mut self, i: usize, now: u64) -> bool {
        let lane = LANES[i];
        let mut cx = Context::from_waker(Waker::noop());
        match &mut self.ops[i] {
            Op::Idle => false,
            Op::Parked { offset, bytes } => {
                let Some(connection) = self.connection.as_ref() else {
                    return false;
                };
                match open_write(&connection.fids[i], lane, *offset, bytes) {
                    Ok(write) => self.ops[i] = Op::Write(write),
                    Err(errno) if errno == -EBUSY => return false,
                    Err(_) => {
                        self.ops[i] = Op::Idle;
                        self.client.failed(lane, LOST, now);
                    }
                }
                true
            }
            Op::Write(write) => {
                let Poll::Ready(result) = Pin::new(write).poll(&mut cx) else {
                    return false;
                };
                self.ops[i] = Op::Idle;
                match result {
                    Ok(count) => self.client.wrote(lane, count, now),
                    Err(errno) => self.client.failed(lane, errno, now),
                }
                true
            }
            Op::Read(read) => {
                let Poll::Ready(result) = Pin::new(read).poll(&mut cx) else {
                    return false;
                };
                self.ops[i] = Op::Idle;
                match result {
                    Ok(done) => self.client.read(lane, done.bytes(), now),
                    Err(errno) => self.client.failed(lane, errno, now),
                }
                true
            }
        }
    }

    fn start_steps(&mut self, now: u64) -> bool {
        let mut started = false;
        while let Some(step) = self.client.next_step(now) {
            started = true;
            let fids = self.connection.as_ref().map(|c| &c.fids);
            let refused = match step {
                Step::Connect => match self.target.connect(None) {
                    Ok(connect) => {
                        let token = self.token.clone();
                        self.connecting = Some(Box::pin(establish(connect, token)));
                        None
                    }
                    Err(_) => {
                        self.client.connect_failed();
                        None
                    }
                },
                Step::Close => {
                    // What is in flight is cancelled with its scope, and no completion
                    // of it can reach the client now.
                    self.connecting = None;
                    self.ops = [Op::Idle, Op::Idle];
                    self.connection = None;
                    None
                }
                Step::Write {
                    lane,
                    offset,
                    bytes,
                } => {
                    let i = index(lane);
                    match fids.map(|f| open_write(&f[i], lane, offset, bytes)) {
                        Some(Ok(write)) => {
                            self.ops[i] = Op::Write(write);
                            None
                        }
                        Some(Err(errno)) if errno == -EBUSY => {
                            let bytes = bytes.to_vec();
                            self.ops[i] = Op::Parked { offset, bytes };
                            None
                        }
                        _ => Some(lane),
                    }
                }
                Step::Read {
                    lane,
                    offset,
                    count,
                } => {
                    let i = index(lane);
                    match fids.map(|f| open_read(&f[i], lane, offset, count)) {
                        Some(Ok(read)) => {
                            self.ops[i] = Op::Read(read);
                            None
                        }
                        _ => Some(lane),
                    }
                }
            };
            if let Some(lane) = refused {
                self.client.failed(lane, LOST, now);
            }
        }
        started
    }
}

#[cfg(any(test, feature = "qualification"))]
pub mod qualification;

#[cfg(test)]
mod tests;
