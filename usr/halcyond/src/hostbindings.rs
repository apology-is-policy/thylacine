//! Trusted UI metadata handoff and remote terminal-observer obligations.
//!
//! Names never authorize: the sealed child's pipe supplies a locator, the UI
//! supplies its child PID and exact pane incarnation, and Tapestry validates
//! both against the kernel. Desired state coalesces, but remote cleanup cannot.
//! A raw successful Bind owns a remote observer even if Interaction rejects its
//! receipt after cancellation. Ordered retirement wins over any late receipt.
//! All storage is fixed; removals never need an empty queue slot. No app dispatch.
use crate::paneroute::{Route, Routes};
use libhalcyon::interaction_control::{Op, Request};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Host {
    pub route: Route,
    pub pid: u32,
    pub binding: u64,
}
#[derive(Clone, Copy)]
pub struct Desired {
    slots: [Option<Host>; 32],
}
impl Desired {
    pub fn empty() -> Self { Self { slots: [None; 32] } }
    pub fn retain(&mut self, routes: &Routes) {
        for slot in &mut self.slots {
            if slot.is_some_and(|h| !routes.current(h.route)) { *slot = None; }
        }
    }
    /// Internal UI entry point. Refuse replacement of a live host announcement.
    pub fn announce(&mut self, routes: &Routes, host: Host) -> bool {
        self.retain(routes);
        if !routes.current(host.route) || host.pid == 0
            || host.binding == 0 || host.binding > i64::MAX as u64 { return false; }
        if let Some(old) = self.slots.iter().flatten().find(|h| h.route == host.route) {
            return *old == host;
        }
        if self.slots.iter().flatten().any(|h| h.binding == host.binding) { return false; }
        let Some(slot) = self.slots.iter_mut().find(|s| s.is_none()) else { return false; };
        *slot = Some(host);
        true
    }
    fn contains(&self, host: Host) -> bool { self.slots.contains(&Some(host)) }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Action { pub host: Host, pub op: Op }
#[derive(Clone, Copy)]
struct Entry {
    host: Host,
    remote: bool,
    dead: bool,
    attempted: Option<(Op, u64)>,
}
#[derive(Clone, Copy)]
struct Flight { action: Action, request: Request, seat: u64, retired: bool }
pub struct Bindings {
    entries: [Option<Entry>; 32],
    flight: Option<Flight>,
}
const _: () = assert!(core::mem::size_of::<Desired>() <= 32 * 128);
const _: () = assert!(core::mem::size_of::<Bindings>() <= 4096);
impl Bindings {
    pub fn new() -> Self { Self { entries: [None; 32], flight: None } }
    /// Cleanup has priority. A full table of retired routes drains before new
    /// routes are admitted; it cannot lose cleanup by overwriting an entry.
    pub fn plan(&mut self, desired: &Desired, normal: Option<u64>) -> Option<Action> {
        if self.flight.is_some() { return None; }
        for slot in &mut self.entries {
            if slot.is_some_and(|e| !desired.contains(e.host) && !e.remote) { *slot = None; }
        }
        let seat = normal?;
        for e in self.entries.iter().flatten() {
            if !desired.contains(e.host) && e.remote && e.attempted != Some((Op::Unbind, seat)) {
                return Some(Action { host: e.host, op: Op::Unbind });
            }
        }
        for &h in desired.slots.iter().flatten() {
            // One actual observer per leaf. A new incarnation cannot bypass an
            // outstanding cleanup obligation even when another slot is free.
            if self.entries.iter().flatten().any(|e| e.host.route.leaf == h.route.leaf && e.host != h) {
                continue;
            }
            let index = if let Some(i) = self.entries.iter().position(|e| e.is_some_and(|e| e.host == h)) {
                i
            } else {
                let Some(i) = self.entries.iter().position(Option::is_none) else { continue; };
                self.entries[i] = Some(Entry { host: h, remote: false, dead: false, attempted: None });
                i
            };
            let e = self.entries[index].unwrap();
            if !e.remote && !e.dead && e.attempted != Some((Op::Bind, seat)) {
                return Some(Action { host: h, op: Op::Bind });
            }
        }
        None
    }
    pub fn started(&mut self, action: Action, request: Request, seat: u64) -> bool {
        if self.flight.is_some() || request.op != action.op || request.leaf != action.host.route.leaf
            || request.binding != action.host.binding || request.request == 0
            || request.binder_pid != if action.op == Op::Bind { action.host.pid } else { 0 }
            || !matches!(action.op, Op::Bind | Op::Unbind)
            || Request::decode(&request.encode()) != Some(request) { return false; }
        if !self.entries.iter().flatten().any(|e| e.host == action.host) { return false; }
        self.flight = Some(Flight { action, request, seat, retired: false });
        true
    }
    pub fn request(&self) -> Option<Request> { self.flight.map(|f| f.request) }
    /// Input is the raw ordered result, not Interaction's locally usable one.
    pub fn complete(&mut self, request: Request, result: Result<(), u32>) -> bool {
        let Some(f) = self.flight else { return false; };
        if f.request != request { return false; }
        let Some(e) = self.entries.iter_mut().flatten().find(|e| e.host == f.action.host) else { return false; };
        e.attempted = Some((f.action.op, f.seat));
        match f.action.op {
            Op::Bind if result.is_ok() && !f.retired => e.remote = true,
            // ENOENT is a confirmed absence; EPERM is not. A dead/removed
            // surface produces Retired in order before its refusal decision.
            Op::Unbind if result.is_ok() || result == Err(2) => e.remote = false,
            _ => {}
        }
        self.flight = None;
        true
    }
    pub fn retired(&mut self, leaf: u32, binding: u64) {
        for e in self.entries.iter_mut().flatten() {
            if e.host.route.leaf == leaf && e.host.binding == binding {
                e.remote = false;
                e.dead = true;
            }
        }
        if let Some(f) = self.flight.as_mut() {
            if f.request.leaf == leaf && f.request.binding == binding { f.retired = true; }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn host(routes: &mut Routes, token: u128, leaf: u32) -> Host {
        assert!(routes.insert(token, leaf));
        Host { route: *routes.get(&token).unwrap(), pid: leaf + 10, binding: token as u64 + 100 }
    }
    fn start(b: &mut Bindings, d: &Desired, seat: u64, id: u64) -> (Action, Request) {
        let a = b.plan(d, Some(seat)).unwrap();
        let r = Request { op: a.op, request: id, leaf: a.host.route.leaf,
            binder_pid: if a.op == Op::Bind { a.host.pid } else { 0 }, binding: a.host.binding,
            foreground: 0, subject: 0, controller: 0, context: 0, epoch: 0 };
        assert!(b.started(a, r, seat));
        (a, r)
    }
    #[test]
    fn exact_host_metadata_and_removal_at_capacity() {
        let mut rs = Routes::empty(); let mut d = Desired::empty();
        for i in 1..=32 { let h = host(&mut rs, i, i as u32); assert!(d.announce(&rs, h)); }
        let h = d.slots[0].unwrap();
        assert!(d.announce(&rs, h));
        assert!(!d.announce(&rs, Host { pid: 0, ..h }));
        assert!(!d.announce(&rs, Host { binding: h.binding + 1, ..h }));
        rs.remove_leaf(1); d.retain(&rs);
        let replacement = host(&mut rs, 1, 1);
        assert_ne!(h.route, replacement.route);
        assert!(!d.announce(&rs, h));
        assert!(d.announce(&rs, replacement));
        assert_eq!(d.slots.iter().flatten().count(), 32);
    }
    #[test]
    fn late_success_keeps_cleanup_and_replacement_cannot_bypass_it() {
        let mut rs = Routes::empty(); let mut d = Desired::empty(); let mut b = Bindings::new();
        let old = host(&mut rs, 1, 1); assert!(d.announce(&rs, old));
        let (_, r) = start(&mut b, &d, 0, 1);
        rs.remove_leaf(1); let new = host(&mut rs, 1, 1);
        assert!(d.announce(&rs, new));
        assert_eq!(b.plan(&d, Some(0)), None); // old request still in flight
        assert!(b.complete(r, Ok(()))); // raw success after local route cancellation
        let (a, r) = start(&mut b, &d, 0, 2);
        assert_eq!(a, Action { host: old, op: Op::Unbind });
        assert!(b.complete(r, Err(1))); // permission refusal is not absence
        assert_eq!(b.plan(&d, Some(0)), None);
        let (a, r) = start(&mut b, &d, 1, 3);
        assert_eq!(a.op, Op::Unbind);
        assert!(b.complete(r, Err(2)));
        let (a, _) = start(&mut b, &d, 1, 4);
        assert_eq!(a, Action { host: new, op: Op::Bind });
    }
    #[test]
    fn retirement_before_success_cannot_revive_or_rebind() {
        let mut rs = Routes::empty(); let mut d = Desired::empty(); let mut b = Bindings::new();
        let h = host(&mut rs, 1, 1); assert!(d.announce(&rs, h));
        let (_, r) = start(&mut b, &d, 0, 1);
        b.retired(1, h.binding);
        assert!(b.complete(r, Ok(())));
        assert_eq!(b.plan(&d, Some(1)), None);
        d = Desired::empty();
        assert_eq!(b.plan(&d, Some(1)), None); // no phantom Unbind
        assert!(b.entries.iter().all(Option::is_none));
    }
    #[test]
    fn refusal_is_bounded_and_suspend_keeps_remote_observer() {
        let mut rs = Routes::empty(); let mut d = Desired::empty(); let mut b = Bindings::new();
        let h = host(&mut rs, 1, 1); assert!(d.announce(&rs, h));
        let (_, r) = start(&mut b, &d, 0, 1);
        assert!(b.complete(r, Err(1)));
        for _ in 0..10 { assert_eq!(b.plan(&d, Some(0)), None); }
        assert_eq!(b.plan(&d, None), None);
        let (_, r) = start(&mut b, &d, 1, 2);
        assert!(b.complete(r, Ok(())));
        assert_eq!(b.plan(&d, None), None);
        assert_eq!(b.plan(&d, Some(2)), None); // no second Bind after SAK
        assert_eq!(b.plan(&Desired::empty(), None), None);
        assert_eq!(b.plan(&Desired::empty(), Some(2)).unwrap().op, Op::Unbind);
    }
    #[test]
    fn exact_completion_and_unrelated_retirement() {
        let mut rs = Routes::empty(); let mut d = Desired::empty(); let mut b = Bindings::new();
        let h = host(&mut rs, 1, 1); assert!(d.announce(&rs, h));
        let (a, r) = start(&mut b, &d, 0, 1);
        assert!(!b.started(a, r, 0));
        assert!(!b.complete(Request { binding: r.binding + 1, ..r }, Ok(())));
        b.retired(1, h.binding + 1);
        assert!(b.complete(r, Ok(())));
        assert!(!b.complete(r, Ok(())));
        assert_eq!(b.plan(&d, Some(0)), None);
        b.retired(1, h.binding + 1);
        assert_eq!(b.plan(&Desired::empty(), Some(0)).map(|a| a.op), Some(Op::Unbind));
    }
    #[test]
    fn full_remote_table_drains_without_losing_replacement_snapshot() {
        let mut rs = Routes::empty(); let mut d = Desired::empty(); let mut b = Bindings::new();
        for i in 1..=32 { let h = host(&mut rs, i, i as u32); assert!(d.announce(&rs, h)); }
        for i in 1..=32 { let (_, r) = start(&mut b, &d, 0, i); assert!(b.complete(r, Ok(()))); }
        for i in 1..=32 { rs.remove_leaf(i); }
        for i in 1..=32 { let h = host(&mut rs, i, i as u32); assert!(d.announce(&rs, h)); }
        for i in 33..=64 {
            let (a, r) = start(&mut b, &d, 0, i); assert_eq!(a.op, Op::Unbind);
            b.retired(r.leaf, r.binding); assert!(b.complete(r, Ok(())));
        }
        for i in 65..=96 {
            let (a, r) = start(&mut b, &d, 0, i); assert_eq!(a.op, Op::Bind);
            assert!(b.complete(r, Ok(())));
        }
        assert_eq!(b.plan(&d, Some(0)), None);
    }
}
