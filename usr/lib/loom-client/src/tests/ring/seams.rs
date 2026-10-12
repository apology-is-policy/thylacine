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

#[test]
fn future_full_registration_table_answers_full() {
    let p = Probe::new();
    let b = ManyBacking {
        buffers: std::array::from_fn(|_| Box::new(UnsafeCell::new([0; 8]))),
        p: p.clone(),
    };
    let d = FutureDriver::new(fixed_requests::FixedDriver::new(b, 128).unwrap());
    let futures: Vec<_> = (0..64)
        .map(|i| {
            let r = PoolMember {
                buffer_index: i,
                ..range(0)
            };
            d.write(id(0, 1), r, &[3; 8]).unwrap()
        })
        .collect();
    assert_eq!(d.registrations(), 64);
    // Every buffer is in use as well; the table answers first, and it is full.
    assert!(matches!(
        d.write(id(0, 1), range(0), &[3; 8]),
        Err(fixed_requests::Error::Storage(ring_payload::Error::Full))
    ));
    drop(futures);
    assert_eq!(d.registrations(), 0);
    assert!(d.write(id(0, 1), range(0), &[3; 8]).is_ok());
}
