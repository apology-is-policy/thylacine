//! Compile the shipped compositor admission body against controlled boundary
//! adapters. This tests its ordering, not the kernel syscall implementation.
//! rustc --edition=2021 --test tools/host-tests/interaction-admission.rs -o /tmp/hi-admission
extern crate alloc;
extern crate self as libhalcyon;
extern crate self as libthyla_rs;
#[path = "../../usr/lib/libthyla-rs/src/pty_interaction.rs"]
mod abi;
#[path = "../../usr/lib/libhalcyon/src/interaction_control.rs"]
pub mod interaction_control;
#[path = "../../usr/lib/libhalcyon/src/interaction_events.rs"]
pub mod interaction_events;
const E_IO:u32=5;
pub use abi::TPtyInteractionState;
use interaction_control::{Op, Reply, Request};
use std::cell::RefCell;
pub const T_POLLIN: u16 = 1;
pub const T_POLLHUP: u16 = 16;
pub const T_POLLERR: u16 = 8;
pub const T_POLLNVAL: u16 = 32;
pub struct TPollFd {
    pub fd: i32,
    pub events: u16,
    pub revents: u16,
}
pub mod ninep {
    pub const E_PERM: u32 = 1;
    pub const E_NOENT: u32 = 2;
    pub const E_NOMEM: u32 = 12;
    pub const E_BUSY: u32 = 16;
    pub const E_INVAL: u32 = 22;
}
pub mod err {
    #[derive(Clone, Copy, Debug)]
    pub enum Error {
        WouldBlock,
        Denied,
        Missing,
        Full,
    }
    impl Error {
        pub fn as_errno(self) -> i32 {
            match self {
                Self::WouldBlock => 11,
                Self::Denied => 1,
                Self::Missing => 2,
                Self::Full => 12,
            }
        }
    }
}
#[derive(Default)]
struct Kernel {
    state: TPtyInteractionState,
    ack_fails: bool,
    watch_fails: bool,
    ready: bool,
    watch_eof: bool,
    unbinds: usize,
    closed: usize,
    checks: usize,
}
thread_local! {static K:RefCell<Kernel>=RefCell::new(Kernel::default());}
pub mod pty_observer {
    use super::*;
    #[derive(Clone, Copy)]
    pub struct BindingId(u64);
    impl BindingId {
        pub fn from_locator(n: u64) -> Result<Self, err::Error> {
            if n == 0 || n > i64::MAX as u64 {
                Err(err::Error::Denied)
            } else {
                Ok(Self(n))
            }
        }
        pub fn locator(self) -> u64 {
            self.0
        }
        pub fn state(self) -> Result<TPtyInteractionState, err::Error> {
            K.with(|k| {
                let k = k.borrow();
                if k.state.binding_id == self.0 && k.state.flags & 1 != 0 {
                    Ok(k.state)
                } else {
                    Err(err::Error::Missing)
                }
            })
        }
        pub fn unbind(self) -> Result<(), err::Error> {
            K.with(|k| k.borrow_mut().unbinds += 1);
            Ok(())
        }
        pub fn watch(self) -> Result<Watch, err::Error> {
            K.with(|k| {
                if k.borrow().watch_fails {
                    Err(err::Error::Full)
                } else {
                    Ok(Watch)
                }
            })
        }
        pub fn acknowledge(self, epoch: u64, subject: u64) -> Result<(), err::Error> {
            K.with(|k| {
                let mut k = k.borrow_mut();
                if k.ack_fails || k.state.foreground_epoch != epoch || subject != 30 {
                    return Err(err::Error::Denied);
                }
                k.state.flags = 3;
                k.state.subject_stripes = subject;
                Ok(())
            })
        }
        pub fn check(self, epoch: u64, subject: u64) -> Result<(), err::Error> {
            K.with(|k| {
                let mut k = k.borrow_mut();
                k.checks += 1;
                if k.state.foreground_epoch == epoch
                    && k.state.subject_stripes == subject
                    && k.state.flags == 3
                {
                    Ok(())
                } else {
                    Err(err::Error::Denied)
                }
            })
        }
    }
    pub struct Watch;
    impl Watch {
        pub fn as_raw_fd(&self) -> i32 {
            100
        }
        pub fn read(&mut self) -> Result<Option<TPtyInteractionState>, err::Error> {
            K.with(|k| {
                let mut k = k.borrow_mut();
                if k.watch_eof {
                    Ok(None)
                } else if k.ready {
                    k.ready = false;
                    Ok(Some(k.state))
                } else {
                    Err(err::Error::WouldBlock)
                }
            })
        }
    }
    impl Drop for Watch {
        fn drop(&mut self) {
            K.with(|k| k.borrow_mut().closed += 1);
        }
    }
}
mod pane {
    pub enum Kind {
        Leaf { surface: Option<usize> },
    }
}
struct Node {
    kind: pane::Kind,
}
struct Layout {
    epoch: u64,
    leaf: u32,
    node: Node,
    focused: Option<usize>,
}
impl Layout {
    fn slot_of_id(&self, n: u32) -> Option<usize> {
        (n == self.leaf).then_some(0)
    }
    fn get(&self, n: usize) -> Option<&Node> {
        (n == 0).then_some(&self.node)
    }
    fn focused_surface(&self) -> Option<usize> {
        self.focused
    }
}
struct Surface {
    owner_conn: u64,
    gen: u32,
}
struct Gpu {
    seat: u64,
    phase: u32,
}
impl Gpu {
    fn seat_state(&self) -> Result<(u64, u32), ()> {
        Ok((self.seat, self.phase))
    }
}
struct Comp {
    layout: Layout,
    gpu: Gpu,
    surface: Surface,
    declared: u64,
    interaction_seat: Option<u64>,
    interactions: [Option<interaction::Binding>; 2],
    ordered: [Option<interaction::Feed>; 2],
    next_ordered: u64,
}
impl Comp {
    fn session_declared(&self, c: u64) -> bool {
        c == self.declared
    }
    fn surf(&self, n: usize) -> Option<&Surface> {
        (n == 0).then_some(&self.surface)
    }
}
struct Fid {
    interaction: Option<interaction::Transaction>,
}
struct Conn {
    conn_id: u64,
    fids: [Option<Fid>; 1],
}
#[path = "../../usr/tapestryd/src/interaction.rs"]
mod interaction;
fn setup() -> (Comp, Request) {
    K.with(|k| {
        *k.borrow_mut() = Kernel {
            state: TPtyInteractionState {
                version: 1,
                flags: 1,
                binding_id: 10,
                binder_pid: 20,
                foreground_epoch: 1,
                ..Default::default()
            },
            ..Default::default()
        }
    });
    (
        Comp {
            layout: Layout {
                epoch: 1,
                leaf: 2,
                node: Node {
                    kind: pane::Kind::Leaf { surface: Some(0) },
                },
                focused: Some(0),
            },
            gpu: Gpu { seat: 1, phase: 0 },
            surface: Surface {
                owner_conn: 3,
                gen: 1,
            },
            declared: 3,
            interaction_seat: None,
            interactions: [None, None],
            ordered: [None,None],
            next_ordered: 1,
        },
        Request {
            op: Op::Bind,
            request: 1,
            leaf: 2,
            binder_pid: 20,
            binding: 10,
            foreground: 0,
            subject: 0,
            controller: 0,
            context: 0,
            epoch: 0,
        },
    )
}
fn active() -> (Comp, Request) {
    let (mut c, r) = setup();
    c.interaction_request(3, r).unwrap();
    let p = Request {
        op: Op::Publish,
        binder_pid: 0,
        foreground: 1,
        subject: 30,
        controller: 1,
        context: 1,
        epoch: 1,
        ..r
    };
    c.interaction_request(3, p).unwrap();
    (c, p)
}
fn check(c: &mut Comp, p: Request) -> bool {
    c.interaction_request(3, Request { op: Op::Check, ..p })
        .is_ok()
}
fn watch(c: &mut Comp, revents: u16) {
    c.interaction_ready(&[TPollFd {
        fd: 100,
        events: T_POLLIN,
        revents,
    }]);
}
#[test]
fn only_declared_exact_surface_and_real_host() {
    let (mut c, r) = setup();
    assert!(c.interaction_request(4, r).is_err());
    c.surface.owner_conn = 4;
    assert!(c.interaction_request(3, r).is_err());
    c.surface.owner_conn = 3;
    assert!(c
        .interaction_request(
            3,
            Request {
                binder_pid: 99,
                ..r
            }
        )
        .is_err());
    assert!(c.interaction_request(3, Request { leaf: 99, ..r }).is_err());
    c.interaction_request(3, r).unwrap();
    assert!(c.interaction_request(3, r).is_err());
}
#[test]
fn watcher_allocation_failure_leaves_no_half_binding() {
    let (mut c, r) = setup();
    K.with(|k| k.borrow_mut().watch_fails = true);
    assert!(c.interaction_request(3, r).is_err());
    assert!(c.interactions.iter().all(Option::is_none));
    K.with(|k| {
        let mut k = k.borrow_mut();
        assert_eq!(k.unbinds, 0);
        k.watch_fails = false
    });
    c.interaction_request(3, r).unwrap();
}
#[test]
fn successful_admission_carries_fresh_focus_and_seat() {
    let (mut c, p) = active();
    c.layout.epoch = 5;

    let r = c
        .interaction_request(
            3,
            Request {
                op: Op::Check,
                request: 99,
                ..p
            },
        )
        .unwrap();
    assert_eq!((r.request, r.focus, r.seat, r.foreground), (99, 5, 1, 1));
}
#[test]
fn background_and_wrong_context_never_reach_kernel_check() {
    let (mut c, p) = active();
    c.layout.focused = None;
    assert!(!check(&mut c, p));
    c.layout.focused = Some(0);
    for q in [
        Request { subject: 31, ..p },
        Request { controller: 2, ..p },
        Request { context: 2, ..p },
        Request { epoch: 2, ..p },
    ] {
        assert!(!check(&mut c, q));
    }
    K.with(|k| assert_eq!(k.borrow().checks, 0));
    assert!(check(&mut c, p));
}
#[test]
fn failed_replacement_revokes_old_context() {
    let (mut c, p) = active();
    K.with(|k| k.borrow_mut().ack_fails = true);
    assert!(c
        .interaction_request(3, Request { controller: 2, ..p })
        .is_err());
    assert!(!check(&mut c, p));
}
#[test]
fn publish_generation_and_context_replay_refused() {
    let (mut c, p) = active();
    assert!(c.interaction_request(3, p).is_err());
    let newer = Request {
        epoch: 2,
        context: 2,
        ..p
    };
    c.interaction_request(3, newer).unwrap();
    assert!(!check(&mut c, p));
    assert!(check(&mut c, newer));
}
#[test]
fn foreground_a_b_a_cannot_reuse_controller() {
    let (mut c, p) = active();
    K.with(|k| {
        let mut k = k.borrow_mut();
        k.state.foreground_epoch = 3;
        k.state.flags = 1;
        k.ready = true
    });
    watch(&mut c, T_POLLIN);
    assert!(!check(&mut c, p));
    assert!(c
        .interaction_request(
            3,
            Request {
                foreground: 3,
                epoch: 2,
                ..p
            }
        )
        .is_err());
    let next = Request {
        foreground: 3,
        controller: 2,
        ..p
    };
    c.interaction_request(3, next).unwrap();
    assert!(check(&mut c, next));
}
#[test]
fn fresh_state_detects_handover_without_poll_delivery() {
    let (mut c, p) = active();
    K.with(|k| {
        let mut k = k.borrow_mut();
        k.state.foreground_epoch = 2;
        k.state.flags = 1;
    });
    assert!(!check(&mut c, p));
}
#[test]
fn surface_reuse_retires_watch_and_binding() {
    let (mut c, p) = active();
    c.surface.gen += 1;
    c.interaction_sweep();
    assert!(!check(&mut c, p));
    K.with(|k| assert_eq!((k.borrow().closed, k.borrow().unbinds), (1, 1)));
}
#[test]
fn hup_and_eof_retire_but_wouldblock_does_not() {
    let (mut c, p) = active();
    watch(&mut c, T_POLLIN);
    assert!(check(&mut c, p));
    watch(&mut c, T_POLLHUP);
    assert!(!check(&mut c, p));
    drop(c);
    let (mut c, p) = active();
    K.with(|k| k.borrow_mut().watch_eof = true);
    watch(&mut c, T_POLLIN);
    assert!(!check(&mut c, p));
}
#[test]
fn sak_and_exhausted_focus_fail_closed() {
    let (mut c, p) = active();
    c.gpu.phase = 1;
    assert!(!check(&mut c, p));
    c.gpu.phase = 0;
    assert!(!check(&mut c, p));
    c.interaction_request(3, Request { controller: 2, ..p })
        .unwrap();
    c.layout.epoch = u64::MAX;
    assert!(!check(&mut c, Request { controller: 2, ..p }));
}
#[test]
fn reusable_fid_exact_replay_and_decode_gate() {
    let (mut c, r) = setup();
    let mut conn = Conn {
        conn_id: 3,
        fids: [Some(Fid { interaction: None })],
    };
    assert!(conn.interaction_control(&mut c, 0, b"HIA1").is_err());
    conn.interaction_control(&mut c, 0, &r.encode()).unwrap();
    let saved = conn.fids[0].as_ref().unwrap().interaction;
    assert!(conn.interaction_control(&mut c, 0, &r.encode()).is_ok());
    assert_eq!(conn.fids[0].as_ref().unwrap().interaction, saved);
    // Same ID with a changed body is neither a retry nor a new decision.
    assert!(conn.interaction_control(&mut c, 0, &Request {leaf:99,..r}.encode()).is_err());
    // A newer failed Bind is remembered, so removing the original binding
    // cannot turn a retry of that failure into a successful new Bind.
    let newer = Request {request:r.request+1,..r};
    assert!(conn.interaction_control(&mut c, 0, &newer.encode()).is_err());
    c.interactions = [None,None];
    assert!(conn.interaction_control(&mut c, 0, &newer.encode()).is_err());
    assert!(conn.interaction_control(&mut c, 0, &r.encode()).is_err());
    conn.interaction_control(&mut c, 0, &Request {request:newer.request+1,..r}.encode()).unwrap();
}

#[test]
fn seat_generation_change_between_loop_and_request_revokes_context() {
    let (mut c, p) = active();
    c.gpu.seat += 1;
    assert!(!check(&mut c, p));
    assert!(c.interaction_request(3, Request { epoch: 2, ..p }).is_err());
    let next = Request { controller: 2, ..p };
    c.interaction_request(3, next).unwrap();
    assert!(check(&mut c, next));
}

#[test]
fn ordered_producer_replays_decision_without_repeating_kernel_authority() {
    use interaction_events::Body;
    let (mut c,r)=setup();c.ordered_install(3).unwrap();
    assert!(matches!(c.ordered_pop(3).unwrap().unwrap().body,Body::Ready(_)));
    let mut conn=Conn {conn_id:3,fids:[Some(Fid{interaction:None})]};
    conn.ordered_control(&mut c,0,&r.encode()).unwrap();
    assert!(matches!(c.ordered_pop(3).unwrap().unwrap().body,Body::Terminal{subject:0,..}));
    assert!(matches!(c.ordered_pop(3).unwrap().unwrap().body,Body::Decision{result:Ok(_),..}));
    let p=Request{op:Op::Publish,request:2,binder_pid:0,foreground:1,subject:30,controller:1,context:1,epoch:1,..r};
    conn.ordered_control(&mut c,0,&p.encode()).unwrap();
    assert!(matches!(c.ordered_pop(3).unwrap().unwrap().body,Body::Terminal{subject:30,..}));
    assert!(matches!(c.ordered_pop(3).unwrap().unwrap().body,Body::Decision{result:Ok(_),..}));
    c.layout.epoch=10;c.interaction_focus_lost(0);c.layout.epoch=12;c.interaction_focus_lost(0);
    let q=Request{op:Op::Check,request:3,..p};conn.ordered_control(&mut c,0,&q.encode()).unwrap();
    assert!(matches!(c.ordered_pop(3).unwrap().unwrap().body,Body::FocusLost{epoch:10,..}));
    assert!(matches!(c.ordered_pop(3).unwrap().unwrap().body,Body::FocusLost{epoch:12,..}));
    let decision=c.ordered_pop(3).unwrap().unwrap();
    let checks=K.with(|k|k.borrow().checks);conn.ordered_control(&mut c,0,&q.encode()).unwrap();
    assert_eq!(c.ordered_pop(3).unwrap().unwrap().body,decision.body);
    assert_eq!(K.with(|k|k.borrow().checks),checks);
    c.ordered_remove(3);assert!(c.interaction_request(3,q).is_err());
}

#[test]
fn ordered_overflow_retires_context_and_reopen_cannot_revive_it() {
    let (mut c,p)=active();c.ordered_install(3).unwrap();
    let first=c.ordered_pop(3).unwrap().unwrap();
    for _ in 0..65 {c.interaction_focus_lost(0);}
    assert_eq!(c.ordered_pop(3),Err(E_IO));
    assert!(!check(&mut c,p));
    c.ordered_remove(3);c.ordered_install(3).unwrap();
    let next=c.ordered_pop(3).unwrap().unwrap();assert_ne!(first.body,next.body);
    assert!(!check(&mut c,p));
}
#[test]
fn ordered_same_epoch_subject_and_surface_retirement_arrive_before_decision() {
    use interaction_events::Body;
    let (mut c,p)=active();c.ordered_install(3).unwrap();
    c.ordered_pop(3).unwrap();c.ordered_pop(3).unwrap();
    K.with(|k| {let mut k=k.borrow_mut();k.state.subject_stripes=99;k.ready=true;});
    let mut fds=alloc::vec::Vec::new();c.interaction_poll(&mut fds);
    for f in &mut fds {f.revents=T_POLLIN;}
    c.interaction_ready(&fds);
    assert!(matches!(c.ordered_pop(3).unwrap().unwrap().body,Body::Terminal{foreground:1,subject:99,..}));
    assert!(!check(&mut c,p));
    c.surface.gen+=1;c.interaction_sweep();
    assert!(matches!(c.ordered_pop(3).unwrap().unwrap().body,Body::Retired{binding:10,..}));
}

#[test]
fn ordered_initial_seat_zero_publishes_without_poison() {
    use interaction_events::Body;
    let (mut c,r)=setup(); c.gpu.seat=0; c.ordered_install(3).unwrap();
    c.ordered_pop(3).unwrap();
    let mut conn=Conn{conn_id:3,fids:[Some(Fid{interaction:None})]};
    conn.ordered_control(&mut c,0,&r.encode()).unwrap();
    c.ordered_pop(3).unwrap();
    assert!(matches!(c.ordered_pop(3).unwrap().unwrap().body,Body::Decision{result:Ok(Reply{seat:0,..}),..}));
    let p=Request{op:Op::Publish,request:2,binder_pid:0,foreground:1,subject:30,controller:1,context:1,epoch:1,..r};
    conn.ordered_control(&mut c,0,&p.encode()).unwrap();c.ordered_pop(3).unwrap();
    assert!(matches!(c.ordered_pop(3).unwrap().unwrap().body,Body::Decision{result:Ok(Reply{seat:0,..}),..}));
    assert!(check(&mut c,p));
}
