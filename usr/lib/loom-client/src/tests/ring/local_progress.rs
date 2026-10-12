use super::*;
#[test]
fn local_admission_signals_before_request_publication() {
    let mut e = FutureExecutor::new(1).unwrap();
    assert!(!e.needs_progress());
    let (_p, a) = future_client(&mut e);
    assert!(e.needs_progress());
    assert_eq!(e.progress_notified(1).visited, 1);
    assert!(!e.needs_progress());
    drop(a);
    assert!(e.needs_progress());
}
