//! Terminal authority lives in the compositor's ordered event loop.
use super::{Comp, Conn};
use alloc::vec::Vec;
use libhalcyon::interaction_control::{Op, Reply, Request};
use libthyla_rs::err::Error;
use libthyla_rs::ninep as p9;
use libthyla_rs::pty_observer::{BindingId, Watch};
use libthyla_rs::{TPollFd, TPtyInteractionState, T_POLLERR, T_POLLHUP, T_POLLIN, T_POLLNVAL};

pub(super) struct Binding {
    conn: u64,
    leaf: u32,
    surface: usize,
    generation: u32,
    id: BindingId,
    watch: Watch,
    state: TPtyInteractionState,
    context: Option<Request>,
    last_controller: u64,
}
impl Drop for Binding {
    fn drop(&mut self) {
        let _ = self.id.unbind();
    }
}
fn errno(e: Error) -> u32 {
    e.as_errno() as u32
}
fn state(id: BindingId) -> Result<TPtyInteractionState, u32> {
    let s = id.state().map_err(errno)?;
    if s.version != 1 || s.flags & 1 == 0 || s.binding_id != id.locator() {
        Err(p9::E_NOENT)
    } else {
        Ok(s)
    }
}
impl Comp {
    fn interaction_surface(&self, conn: u64, leaf: u32) -> Option<(usize, u32)> {
        if conn == 0 || !self.session_declared(conn) {
            return None;
        }
        let slot = self.layout.slot_of_id(leaf)?;
        let n = match self.layout.get(slot)?.kind {
            super::pane::Kind::Leaf { surface: Some(n) } => n,
            _ => return None,
        };
        let surface = self.surf(n)?;
        if surface.owner_conn != conn {
            return None;
        }
        Some((n, surface.gen))
    }
    pub fn interaction_suspend(&mut self) {
        let changed = self
            .interactions
            .iter()
            .flatten()
            .any(|b| b.context.is_some());
        for b in self.interactions.iter_mut().flatten() {
            b.context = None;
        }
        if changed {
            for f in self.ordered.iter_mut().flatten() {
                let _ = f.journal.push(Body::Reset);
            }
        }
    }
    pub fn interaction_sweep(&mut self) {
        for i in 0..self.interactions.len() {
            let stale = self.interactions[i].as_ref().is_some_and(|b| {
                self.interaction_surface(b.conn, b.leaf) != Some((b.surface, b.generation))
            });
            if stale {
                self.interaction_retire(i);
            }
        }
    }
    pub fn interaction_poll(&self, fds: &mut Vec<TPollFd>) {
        for b in self.interactions.iter().flatten() {
            fds.push(TPollFd {
                fd: b.watch.as_raw_fd(),
                events: T_POLLIN,
                revents: 0,
            });
        }
    }
    pub fn interaction_ready(&mut self, fds: &[TPollFd]) {
        for i in 0..self.interactions.len() {
            let Some(b) = self.interactions[i].as_mut() else {
                continue;
            };
            let Some(p) = fds.iter().find(|p| p.fd == b.watch.as_raw_fd()) else {
                continue;
            };
            if p.revents == 0 {
                continue;
            }
            if p.revents & (T_POLLHUP | T_POLLERR | T_POLLNVAL) != 0 {
                self.interaction_retire(i);
                continue;
            }
            match b.watch.read() {
                Ok(Some(s))
                    if s.version == 1 && s.binding_id == b.id.locator() && s.flags & 1 != 0 =>
                {
                    if s.foreground_epoch != b.state.foreground_epoch
                        || s.flags & 2 == 0
                        || b.context.is_some_and(|c| c.subject != s.subject_stripes)
                    {
                        b.context = None;
                    }
                    let changed = b.state.foreground_epoch != s.foreground_epoch
                        || b.state.subject_stripes != s.subject_stripes
                        || b.state.flags != s.flags;
                    b.state = s;
                    if changed {
                        self.interaction_notify_terminal(i);
                    }
                }
                Err(Error::WouldBlock) => {}
                _ => self.interaction_retire(i),
            }
        }
    }
    pub(super) fn interaction_request(&mut self, conn: u64, r: Request) -> Result<Reply, u32> {
        self.interaction_sweep();
        let (seat, phase) = self.gpu.seat_state().map_err(|_| p9::E_PERM)?;
        // The seat can change after the loop's initial sample. Never accept
        // a previous episode's context under a fresh generation.
        if self.interaction_seat != Some(seat) {
            self.interaction_suspend();
            self.interaction_seat = Some(seat);
        }
        if phase != 0 || self.layout.epoch == u64::MAX {
            self.interaction_suspend();
            return Err(p9::E_PERM);
        }
        let (surface, generation) = self.interaction_surface(conn, r.leaf).ok_or(p9::E_PERM)?;
        let index = self
            .interactions
            .iter()
            .position(|x| x.as_ref().is_some_and(|b| b.leaf == r.leaf));
        if r.op == Op::Bind {
            if index.is_some()
                || self
                    .interactions
                    .iter()
                    .flatten()
                    .any(|b| b.id.locator() == r.binding)
            {
                return Err(p9::E_BUSY);
            }
            let id = BindingId::from_locator(r.binding).map_err(errno)?;
            let s = state(id)?;
            if s.binder_pid != r.binder_pid {
                return Err(p9::E_PERM);
            }
            let slot = self
                .interactions
                .iter()
                .position(Option::is_none)
                .ok_or(p9::E_NOMEM)?;
            let watch = id.watch().map_err(errno)?;
            self.interactions[slot] = Some(Binding {
                conn,
                leaf: r.leaf,
                surface,
                generation,
                id,
                watch,
                state: s,
                context: None,
                last_controller: 0,
            });
            self.interaction_notify_terminal(slot);
            return Ok(Reply {
                op: r.op,
                request: r.request,
                focus: self.layout.epoch,
                seat,
                foreground: s.foreground_epoch,
            });
        }
        let i = index.ok_or(p9::E_NOENT)?;
        let b = self.interactions[i].as_mut().unwrap();
        if b.conn != conn || b.id.locator() != r.binding {
            return Err(p9::E_PERM);
        }
        if r.op == Op::Unbind {
            self.interaction_retire(i);
            return Ok(Reply {
                op: r.op,
                request: r.request,
                focus: self.layout.epoch,
                seat,
                foreground: 0,
            });
        }
        let s = state(b.id)?;
        let changed = b.state.foreground_epoch != s.foreground_epoch
            || b.state.subject_stripes != s.subject_stripes
            || b.state.flags != s.flags;
        if s.foreground_epoch != b.state.foreground_epoch
            || s.flags & 2 == 0
            || b.context.is_some_and(|c| c.subject != s.subject_stripes)
        {
            b.context = None;
        }
        b.state = s;
        if changed {
            self.interaction_notify_terminal(i);
        }
        let b = self.interactions[i].as_mut().unwrap();
        if r.op == Op::Publish {
            if r.controller < b.last_controller
                || (r.controller == b.last_controller
                    && !b
                        .context
                        .is_some_and(|c| c.subject == r.subject && c.epoch < r.epoch))
            {
                return Err(p9::E_INVAL);
            }
            b.context = None;
            b.id.acknowledge(r.foreground, r.subject).map_err(errno)?;
            b.context = Some(r);
            b.last_controller = r.controller;
            // ACK changes nomination even when the foreground epoch is unchanged.
            b.state = state(b.id)?;
            self.interaction_notify_terminal(i);
        } else {
            let c = b.context.ok_or(p9::E_NOENT)?;
            if (c.controller, c.context, c.epoch, c.foreground, c.subject)
                != (r.controller, r.context, r.epoch, r.foreground, r.subject)
            {
                return Err(p9::E_PERM);
            }
            if self.layout.focused_surface() != Some(surface) {
                return Err(p9::E_PERM);
            }
            b.id.check(r.foreground, r.subject).map_err(errno)?;
        }
        Ok(Reply {
            op: r.op,
            request: r.request,
            focus: self.layout.epoch,
            seat,
            foreground: r.foreground,
        })
    }
}

/// One reusable ctl transaction. Only a strictly newer request can replace it;
/// exact retries return the stored decision, including failures, without ACKing
/// or checking a second time. The broker must never reuse a consumed receipt.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct Transaction {
    pub request: Request,
    pub result: Result<[u8; libhalcyon::interaction_control::REPLY_BYTES], u32>,
}
impl Conn {
    pub(super) fn interaction_control(
        &mut self,
        comp: &mut Comp,
        i: usize,
        data: &[u8],
    ) -> Result<(), u32> {
        let r = Request::decode(data).ok_or(p9::E_INVAL)?;
        if let Some(previous) = self.fids[i].as_ref().unwrap().interaction {
            if r == previous.request {
                return previous.result.map(|_| ());
            }
            if r.request <= previous.request.request {
                return Err(p9::E_INVAL);
            }
        }
        let result = comp.interaction_request(self.conn_id, r).map(Reply::encode);
        self.fids[i].as_mut().unwrap().interaction = Some(Transaction { request: r, result });
        result.map(|_| ())
    }
}

use libhalcyon::interaction_events::{Body, Journal, Record};
pub(super) struct Feed {
    conn: u64,
    journal: Journal,
}
impl Comp {
    pub(super) fn ordered_install(&mut self, conn: u64) -> Result<(), u32> {
        if !self.session_declared(conn) || self.ordered.iter().flatten().any(|f| f.conn == conn) {
            return Err(p9::E_PERM);
        }
        let slot = self
            .ordered
            .iter()
            .position(Option::is_none)
            .ok_or(p9::E_NOMEM)?;
        let next = self.next_ordered.checked_add(1).ok_or(p9::E_NOMEM)?;
        let mut journal = Journal::new(self.next_ordered).ok_or(p9::E_INVAL)?;
        for b in self
            .interactions
            .iter()
            .flatten()
            .filter(|b| b.conn == conn)
        {
            journal.push(terminal_event(b)).map_err(|_| p9::E_NOMEM)?;
        }
        self.next_ordered = next;
        self.ordered[slot] = Some(Feed { conn, journal });
        Ok(())
    }
    fn ordered_emit(&mut self, conn: u64, event: Body) -> Result<(), u32> {
        let Some(f) = self.ordered.iter_mut().flatten().find(|f| f.conn == conn) else {
            return Ok(());
        };
        if f.journal.push(event).is_err() {
            for b in self
                .interactions
                .iter_mut()
                .flatten()
                .filter(|b| b.conn == conn)
            {
                b.context = None;
            }
            return Err(super::E_IO);
        }
        Ok(())
    }
    pub(super) fn ordered_remove(&mut self, conn: u64) {
        for f in &mut self.ordered {
            if f.as_ref().is_some_and(|f| f.conn == conn) {
                *f = None;
            }
        }
        for b in self
            .interactions
            .iter_mut()
            .flatten()
            .filter(|b| b.conn == conn)
        {
            b.context = None;
        }
    }
    pub(super) fn ordered_pop(&mut self, conn: u64) -> Result<Option<Record>, u32> {
        self.ordered
            .iter_mut()
            .flatten()
            .find(|f| f.conn == conn)
            .ok_or(p9::E_NOENT)?
            .journal
            .pop()
            .map_err(|_| super::E_IO)
    }
    pub(super) fn interaction_focus_lost(&mut self, surface: usize) {
        for i in 0..self.interactions.len() {
            if let Some(b) = &self.interactions[i] {
                if b.surface == surface {
                    let (conn, event) = (
                        b.conn,
                        Body::FocusLost {
                            leaf: b.leaf,
                            binding: b.id.locator(),
                            epoch: self.layout.epoch,
                        },
                    );
                    let _ = self.ordered_emit(conn, event);
                }
            }
        }
    }
    fn interaction_retire(&mut self, i: usize) {
        if let Some(b) = self.interactions[i].take() {
            let _ = self.ordered_emit(
                b.conn,
                Body::Retired {
                    leaf: b.leaf,
                    binding: b.id.locator(),
                },
            );
        }
    }
    fn interaction_notify_terminal(&mut self, i: usize) {
        if let Some(b) = &self.interactions[i] {
            let (conn, event) = (b.conn, terminal_event(b));
            let _ = self.ordered_emit(conn, event);
        }
    }
}
fn terminal_event(b: &Binding) -> Body {
    Body::Terminal {
        leaf: b.leaf,
        binding: b.id.locator(),
        foreground: b.state.foreground_epoch,
        subject: if b.state.flags & 2 != 0 {
            b.state.subject_stripes
        } else {
            0
        },
    }
}
impl Conn {
    pub(super) fn ordered_control(
        &mut self,
        comp: &mut Comp,
        i: usize,
        data: &[u8],
    ) -> Result<(), u32> {
        if !comp
            .ordered
            .iter()
            .flatten()
            .any(|f| f.conn == self.conn_id && !f.journal.failed())
        {
            return Err(super::E_IO);
        }
        let r = Request::decode(data).ok_or(p9::E_INVAL)?;
        // The existing cache performs the authority operation at most once.
        let _ = self.interaction_control(comp, i, data);
        let transaction = self.fids[i]
            .as_ref()
            .unwrap()
            .interaction
            .ok_or(p9::E_INVAL)?;
        if transaction.request != r {
            return Err(p9::E_INVAL);
        }
        let result = transaction
            .result
            .and_then(|b| Reply::decode(&b, r).ok_or(super::E_IO));
        comp.ordered_emit(self.conn_id, Body::decision(r, result))
    }
}
