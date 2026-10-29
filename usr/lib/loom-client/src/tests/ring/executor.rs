use super::*;
use retirement_executor::{Error as ExecError, RetirementExecutor};
fn managed(
    e: &mut RetirementExecutor<Backing>,
) -> (Rc<Probe>, retirement_executor::ManagedClient<Backing>) {
    let p = Probe::new();
    let c = e
        .admit(|| fixed_requests::FixedDriver::new(Backing::new(p.clone()), 1))
        .unwrap();
    (p, c)
}
#[test]
fn executor_capacity_precedes_factory_and_survives_abandon() {
    assert!(matches!(
        RetirementExecutor::<Backing>::new(0),
        Err(ExecError::Capacity)
    ));
    assert!(matches!(
        RetirementExecutor::<Backing>::new(65),
        Err(ExecError::Capacity)
    ));
    let mut e = RetirementExecutor::new(1).unwrap();
    let (p, c) = managed(&mut e);
    let calls = Cell::new(0);
    assert!(matches!(
        e.admit(|| {
            calls.set(1);
            unreachable!()
        }),
        Err(ExecError::Full)
    ));
    assert_eq!(calls.get(), 0);
    drop(c);
    assert_eq!(p.retires.get(), 1);
    for _ in 0..3 {
        assert_eq!(e.progress(64).reclaimed, 0);
    }
    assert_eq!(e.occupied(), 1);
    assert_eq!(p.freed.get(), 0);
    p.retired.set(true);
    assert_eq!(e.progress(1).reclaimed, 1);
    assert_eq!(p.closes.get(), 1);
    assert_eq!(p.freed.get(), 1);
    let (q, c) = managed(&mut e);
    assert_eq!(e.occupied(), 1);
    drop(c);
    q.retired.set(true);
    assert_eq!(e.progress(1).reclaimed, 1);
}
#[test]
fn executor_live_request_blocks_reuse_despite_retirement_proof() {
    let mut e = RetirementExecutor::new(1).unwrap();
    let (p, c) = managed(&mut e);
    let mut r = c.read(id(2, 3), range(0)).unwrap();
    unsafe {
        r.publish().unwrap();
    }
    drop(c);
    p.retired.set(true);
    assert_eq!(e.progress(1).reclaimed, 0);
    assert!(matches!(
        r.try_complete(),
        Err(fixed_requests::Error::Closing)
    ));
    drop(r);
    assert_eq!(p.aborts.get(), 1);
    assert_eq!(e.progress(1).reclaimed, 1);
    assert_eq!(p.freed.get(), 1);
}
#[test]
fn executor_held_terminal_result_blocks_reuse_and_preserves_bytes() {
    let mut e = RetirementExecutor::new(1).unwrap();
    let (p, mut c) = managed(&mut e);
    let mut r = c.read(id(0, 1), range(8)).unwrap();
    let cookie = unsafe { r.publish().unwrap() };
    unsafe {
        c.receive(typed_record(cookie, 4, 0)).unwrap();
    }
    let done = r.try_complete().unwrap().unwrap();
    drop(r);
    drop(c);
    p.retired.set(true);
    assert_eq!(e.progress(1).reclaimed, 0);
    assert_eq!(done.bytes().unwrap(), &[1; 4]);
    drop(done);
    assert_eq!(e.progress(1).reclaimed, 1);
    assert_eq!(p.freed.get(), 1);
}
#[test]
fn executor_rotation_is_bounded_and_does_not_starve_later_cells() {
    let mut e = RetirementExecutor::new(3).unwrap();
    let (a, ca) = managed(&mut e);
    let (b, cb) = managed(&mut e);
    let (c, cc) = managed(&mut e);
    drop(ca);
    drop(cb);
    drop(cc);
    b.retired.set(true);
    c.retired.set(true);
    assert_eq!(e.progress(0).visited, 0);
    assert_eq!(e.progress(1).reclaimed, 0);
    assert_eq!(e.progress(1).reclaimed, 1);
    assert_eq!(b.freed.get(), 1);
    assert_eq!(e.progress(1).reclaimed, 1);
    assert_eq!(c.freed.get(), 1);
    assert_eq!(a.freed.get(), 0);
    a.retired.set(true);
    let x = e.progress(usize::MAX);
    assert_eq!(x.visited, 3);
    assert_eq!(x.reclaimed, 1);
    assert_eq!(x.occupied, 0);
}
#[test]
fn executor_never_reclaims_attached_client() {
    let mut e = RetirementExecutor::new(1).unwrap();
    let (p, c) = managed(&mut e);
    p.retired.set(true);
    assert_eq!(e.progress(1).reclaimed, 0);
    assert_eq!(e.occupied(), 1);
    drop(c);
    assert_eq!(e.progress(1).reclaimed, 1);
}
#[test]
fn executor_shutdown_closes_once_keeps_live_read_storage() {
    let mut e = RetirementExecutor::new(1).unwrap();
    let (p, mut c) = managed(&mut e);
    let mut r = c.read(id(0, 1), range(8)).unwrap();
    let cookie = unsafe { r.publish().unwrap() };
    unsafe {
        c.receive(typed_record(cookie, 2, 0)).unwrap();
    }
    let done = r.try_complete().unwrap().unwrap();
    drop(r);
    drop(e);
    assert_eq!(p.closes.get(), 1);
    assert!(matches!(c.read(id(0, 1), range(0)), Err(ExecError::Closed)));
    drop(c);
    assert_eq!(p.freed.get(), 0);
    assert_eq!(done.bytes().unwrap(), &[1; 2]);
    drop(done);
    assert_eq!(p.freed.get(), 1);
    assert_eq!(p.closes.get(), 1);
}
#[test]
fn executor_failed_factory_does_not_leak_cell() {
    let mut e = RetirementExecutor::new(1).unwrap();
    let p = Probe::new();
    assert!(matches!(
        e.admit(|| fixed_requests::FixedDriver::new(Backing::new(p.clone()), 0)),
        Err(ExecError::Driver(_))
    ));
    assert_eq!(e.occupied(), 0);
    assert_eq!(p.closes.get(), 1);
    assert_eq!(p.freed.get(), 1);
    let (p, c) = managed(&mut e);
    drop(c);
    p.retired.set(true);
    assert_eq!(e.progress(1).reclaimed, 1);
}
#[test]
fn executor_drop_and_progress_do_not_allocate() {
    let mut e = RetirementExecutor::new(1).unwrap();
    let (p, c) = managed(&mut e);
    LATE_ALLOCS.with(|x| x.set(0));
    WATCH_ALLOCS.with(|x| x.set(true));
    drop(c);
    assert_eq!(e.progress(1).reclaimed, 0);
    p.retired.set(true);
    assert_eq!(e.progress(1).reclaimed, 1);
    WATCH_ALLOCS.with(|x| x.set(false));
    assert_eq!(LATE_ALLOCS.with(|x| x.get()), 0);
}
