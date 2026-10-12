//! Adapter-facing paths the original suite left unexercised.
use super::*;

#[test]
fn timer_read_future_cancel_waits_for_actual_terminal() {
    let (p, mut d) = future_driver();
    let mut f = d.read(id(0, 1), range(0)).unwrap();
    f.set_deadline(Some(5)).unwrap();
    let cookie = unsafe { f.publish().unwrap() };
    let (n, w) = counter();
    assert!(poll(&mut f, &w).is_pending());
    assert!(f.request_cancel().unwrap());
    assert!(!f.request_cancel().unwrap());
    assert_eq!(d.expire(5, 64), 0);
    assert_eq!(n.0.load(Ordering::SeqCst), 0);
    assert!(poll(&mut f, &w).is_pending());
    unsafe {
        d.receive(typed_record(cookie, 8, 0)).unwrap();
    }
    assert!(matches!(poll(&mut f, &w), Poll::Ready(Ok(r)) if r.result() == 8));
    assert_eq!(p.aborts.get(), 1);
}

#[test]
fn wait_event_wake_is_reported_and_releases_the_subscription() {
    let mut e = quiet_queue();
    let mut w = Events::new();
    w.reason = WakeReason::Event;
    let t = local_wait::turn(&mut e, &mut w, 1, None);
    assert_eq!(t.outcome, Outcome::Woke(WakeReason::Event));
    assert_eq!((w.waits, w.sleeps, w.disarms), (1, 1, 1));
    assert!(e.needs_progress());
}
