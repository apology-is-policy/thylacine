use super::*;
#[test]
fn deadline_prepared_has_no_timer_or_scope_abort() {
    let (p, mut d) = typed_driver(2);
    let mut r = d.read(id(3, 7), range(0)).unwrap();
    r.set_deadline(Some(5)).unwrap();
    assert_eq!(d.next_deadline(), None);
    assert!(r.request_cancel().is_err());
    assert_eq!(d.expire(99, 64), 0);
    drop(r);
    assert_eq!(p.aborts.get(), 0);
    assert_eq!(d.occupied(), 0);
}
#[test]
fn deadline_expiry_requests_once_without_refund() {
    let (p, mut d) = typed_driver(2);
    let mut r = d.read(id(3, 7), range(0)).unwrap();
    r.set_deadline(Some(5)).unwrap();
    let cookie = unsafe { r.publish().unwrap() };
    assert_eq!(d.next_deadline(), Some(5));
    assert_eq!(d.expire(4, 64), 0);
    assert_eq!(d.expire(5, 64), 1);
    assert_eq!(p.last_abort.get(), id(3, 7));
    assert_eq!(d.next_deadline(), None);
    assert_eq!(d.expire(99, 64), 0);
    assert!(r.try_complete().unwrap().is_none());
    assert!(d.read(id(3, 7), range(8)).is_err());
    assert_eq!(d.occupied(), 1);
    drop(r);
    assert_eq!(p.aborts.get(), 1);
    assert_eq!(d.occupied(), 1);
    unsafe {
        d.receive(typed_record(cookie, -2, 0)).unwrap();
    }
    assert_eq!(d.occupied(), 0);
    assert!(d.read(id(3, 7), range(8)).is_ok());
}
#[test]
fn deadline_terminal_before_expiry_wins() {
    let (p, mut d) = typed_driver(1);
    let mut r = d.write(id(0, 1), range(0), &[2; 8]).unwrap();
    r.set_deadline(Some(4)).unwrap();
    let cookie = unsafe { r.publish().unwrap() };
    unsafe {
        d.receive(typed_record(cookie, 3, 0)).unwrap();
    }
    assert_eq!(d.next_deadline(), None);
    assert_eq!(d.expire(9, 64), 0);
    assert!(!r.request_cancel().unwrap());
    assert_eq!(r.try_complete().unwrap().unwrap().result, 3);
    assert_eq!(p.aborts.get(), 0);
}
#[test]
fn deadline_cancel_does_not_overwrite_later_success() {
    let (p, mut d) = typed_driver(1);
    let mut r = d.write(id(0, 1), range(0), &[2; 8]).unwrap();
    r.set_deadline(Some(4)).unwrap();
    let cookie = unsafe { r.publish().unwrap() };
    assert!(r.request_cancel().unwrap());
    assert!(!r.request_cancel().unwrap());
    assert_eq!(d.expire(9, 64), 0);
    unsafe {
        d.receive(typed_record(cookie, 8, 0)).unwrap();
    }
    assert_eq!(r.try_complete().unwrap().unwrap().result, 8);
    drop(r);
    assert_eq!(p.aborts.get(), 1);
}
#[test]
fn deadline_active_deadline_cannot_be_extended() {
    let (_p, d) = typed_driver(1);
    let mut r = d.read(id(0, 1), range(0)).unwrap();
    r.set_deadline(Some(4)).unwrap();
    r.set_deadline(None).unwrap();
    r.set_deadline(Some(9)).unwrap();
    unsafe {
        r.publish().unwrap();
    }
    assert!(r.set_deadline(Some(90)).is_err());
    assert!(r.set_deadline(None).is_err());
    assert_eq!(d.next_deadline(), Some(9));
}
#[test]
fn deadline_reused_cell_does_not_inherit_old_timer() {
    let (p, mut d) = typed_driver(1);
    let mut a = d.write(id(0, 1), range(0), &[1; 8]).unwrap();
    a.set_deadline(Some(4)).unwrap();
    let cookie = unsafe { a.publish().unwrap() };
    unsafe {
        d.receive(typed_record(cookie, 8, 0)).unwrap();
    }
    a.try_complete().unwrap().unwrap();
    let mut b = d.write(id(0, 2), range(0), &[2; 8]).unwrap();
    unsafe {
        b.publish().unwrap();
    }
    assert_eq!(d.next_deadline(), None);
    assert_eq!(d.expire(99, 64), 0);
    assert_eq!(p.aborts.get(), 0);
    drop(a);
    assert_eq!(p.aborts.get(), 0);
    drop(b);
    assert_eq!(p.aborts.get(), 1);
}
#[test]
fn deadline_all64_cells_expire_in_bounded_rotating_visits() {
    let p = Probe::new();
    let b = ManyBacking {
        buffers: std::array::from_fn(|_| Box::new(UnsafeCell::new([0; 8]))),
        p: p.clone(),
    };
    let mut d = fixed_requests::FixedDriver::new(b, 128).unwrap();
    let mut requests = Vec::new();
    for i in 0..64 {
        let mut r = d
            .read(
                id(i, 1),
                PoolMember {
                    buffer_index: i,
                    ..range(0)
                },
            )
            .unwrap();
        r.set_deadline(Some(5)).unwrap();
        unsafe {
            r.publish().unwrap();
        }
        requests.push(r);
    }
    assert_eq!(d.expire(5, 0), 0);
    for n in 1..=64 {
        assert_eq!(d.expire(5, 1), 1);
        assert_eq!(p.aborts.get(), n);
    }
    assert_eq!(d.next_deadline(), None);
    assert_eq!(d.expire(5, usize::MAX), 0);
    assert_eq!(d.occupied(), 64);
    drop(requests);
    assert_eq!(p.aborts.get(), 64);
}
#[test]
fn deadline_closed_driver_does_not_issue_more_aborts() {
    let (p, mut d) = typed_driver(1);
    let mut r = d.read(id(0, 1), range(0)).unwrap();
    r.set_deadline(Some(0)).unwrap();
    unsafe {
        r.publish().unwrap();
    }
    d.close();
    assert_eq!(d.next_deadline(), None);
    assert_eq!(d.expire(99, 64), 0);
    assert!(!r.request_cancel().unwrap());
    drop(r);
    assert_eq!(p.aborts.get(), 0);
    assert_eq!(p.closes.get(), 1);
}
#[test]
fn deadline_expiry_has_no_internal_allocation() {
    let (_p, mut d) = typed_driver(1);
    let mut r = d.read(id(0, 1), range(0)).unwrap();
    r.set_deadline(Some(5)).unwrap();
    unsafe {
        r.publish().unwrap();
    }
    LATE_ALLOCS.with(|x| x.set(0));
    WATCH_ALLOCS.with(|x| x.set(true));
    let next = d.next_deadline();
    let n = d.expire(5, 64);
    WATCH_ALLOCS.with(|x| x.set(false));
    assert_eq!(next, Some(5));
    assert_eq!(n, 1);
    assert_eq!(LATE_ALLOCS.with(|x| x.get()), 0);
}
#[test]
fn deadline_publication_notifies_local_progress() {
    let (_p, mut d) = typed_driver(1);
    let signal = ProgressSignal::new();
    d.bind_progress(signal.clone());
    let mut r = d.read(id(0, 1), range(0)).unwrap();
    r.set_deadline(Some(8)).unwrap();
    assert!(!signal.take());
    unsafe {
        r.publish().unwrap();
    }
    assert!(signal.take());
    assert_eq!(d.next_deadline(), Some(8));
    assert_eq!(d.expire(8, 64), 1);
    assert!(signal.take());
}
