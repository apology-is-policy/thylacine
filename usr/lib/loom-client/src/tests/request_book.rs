#![allow(dead_code)]
use crate::service_abi;
use crate::{paired_cq, request_book};
use paired_cq::*;
use request_book::*;
use service_abi::*;
fn id(slot: u32, g: u64) -> SlotRef {
    SlotRef {
        slot,
        reserved: 0,
        incarnation: g,
    }
}
fn blank() -> BufferReceipt {
    BufferReceipt {
        pool: id(0, 0),
        member: 0,
        reserved: 0,
        lease: 0,
    }
}
fn rec(cookie: u64, result: i32, flags: u32) -> Record {
    Record {
        cqe: Completion {
            user_data: cookie,
            result,
            flags,
        },
        receipt: blank(),
    }
}
fn payload(cookie: u64, flags: u32) -> Record {
    Record {
        cqe: Completion {
            user_data: cookie,
            result: 8,
            flags,
        },
        receipt: BufferReceipt {
            pool: id(2, 9),
            member: 1,
            reserved: 0,
            lease: 7,
        },
    }
}
fn pooled(multi: bool) -> Kind {
    Kind::PooledRead {
        pool: id(2, 9),
        limit: 8,
        multi,
    }
}
#[test]
fn bounded_prepublication_and_terminal_retention() {
    let mut b = RequestBook::new(1).unwrap();
    let a = b.reserve(id(1, 1), Kind::Control).unwrap();
    assert!(b.accept(rec(a, 0, 0)).is_err());
    assert!(b.abandon(a).is_err());
    b.cancel_unpublished(a).unwrap();
    let c = b.reserve(id(1, 1), Kind::FixedWrite(8)).unwrap();
    assert_ne!(a, c);
    b.publish(c).unwrap();
    assert!(b.cancel_unpublished(c).is_err());
    assert!(b.publish(c).is_err());
    assert!(b.take_terminal(c).is_err());
    assert!(b.accept(rec(a, 0, 0)).is_err());
    assert!(b.accept(rec(c, 9, 0)).is_err());
    assert_eq!(b.occupied(), 1);
    let d = b.accept(rec(c, 8, 0)).unwrap();
    assert!(d.terminal);
    assert_eq!(d.record.cqe.result, 8);
    assert_eq!(b.reserve(id(1, 1), Kind::Control), Err(Error::Full));
    assert!(b.accept(rec(c, 8, 0)).is_err());
    assert_eq!(b.take_terminal(c).unwrap().result, 8);
    assert_eq!(b.occupied(), 0);
    assert!(b.take_terminal(c).is_err());
}
#[test]
fn multishot_more_and_final_keep_payload_separate() {
    let mut b = RequestBook::new(2).unwrap();
    let a = b.reserve(id(1, 3), pooled(true)).unwrap();
    b.publish(a).unwrap();
    let d = b.accept(payload(a, 5)).unwrap();
    assert!(!d.terminal && !d.abandoned);
    assert_eq!(d.record.receipt.lease, 7);
    assert!(b.take_terminal(a).is_err());
    let mut next = payload(a, 5);
    next.receipt.member = 0;
    next.receipt.lease = 8;
    b.accept(next).unwrap();
    let final_record = b.accept(rec(a, 0, 0)).unwrap();
    assert!(final_record.terminal);
    assert_eq!(d.record.receipt.lease, 7);
    assert_eq!(b.occupied(), 1);
    assert_eq!(b.take_terminal(a).unwrap().result, 0);
    assert_eq!(d.record.cqe.result, 8);
}
#[test]
fn malformed_receipts_flags_and_lengths_leave_active() {
    let mut b = RequestBook::new(1).unwrap();
    let a = b.reserve(id(1, 1), pooled(false)).unwrap();
    b.publish(a).unwrap();
    for i in 0..9 {
        let mut r = payload(a, 4);
        match i {
            0 => r.receipt.pool.incarnation += 1,
            1 => r.receipt.pool.slot += 1,
            2 => r.receipt.reserved = 1,
            3 => r.receipt.member = 64,
            4 => r.receipt.lease = 0,
            5 => r.cqe.flags = 5,
            6 => r.cqe.flags = 6,
            7 => r.cqe.flags = 0,
            _ => r.cqe.result = 9,
        };
        assert!(b.accept(r).is_err());
        assert_eq!(b.occupied(), 1);
    }
    assert!(b.accept(payload(a, 4)).unwrap().terminal);
    assert_eq!(b.take_terminal(a).unwrap().result, 8);
}
#[test]
fn abandoned_future_retains_until_terminal_and_scope_abort_once() {
    let mut b = RequestBook::new(2).unwrap();
    let a = b.reserve(id(3, 7), pooled(true)).unwrap();
    let sibling = b.reserve(id(3, 7), Kind::FixedRead(8)).unwrap();
    b.publish(a).unwrap();
    b.publish(sibling).unwrap();
    assert_eq!(b.abandon(a).unwrap(), Some(id(3, 7)));
    assert_eq!(b.abandon(a).unwrap(), None);
    assert_eq!(b.occupied(), 2);
    let d = b.accept(payload(a, 5)).unwrap();
    assert!(d.abandoned && !d.terminal);
    assert_eq!(b.occupied(), 2);
    let d = b.accept(rec(a, -900, 0)).unwrap();
    assert!(d.abandoned && d.terminal);
    assert_eq!(b.abandon(a).unwrap(), None);
    assert_eq!(b.occupied(), 2);
    assert_eq!(b.take_terminal(a).unwrap().result, -900);
    assert_eq!(b.occupied(), 1);
    b.accept(rec(sibling, -900, 0)).unwrap();
    assert_eq!(b.abandon(sibling).unwrap(), None);
    assert_eq!(b.take_terminal(sibling).unwrap().result, -900);
    assert_eq!(b.occupied(), 0);
}
#[test]
fn fixed_and_control_refuse_payload_more_and_oversize() {
    for kind in [Kind::Control, Kind::FixedRead(8), Kind::FixedWrite(8)] {
        let mut b = RequestBook::new(1).unwrap();
        let a = b.reserve(id(1, 1), kind).unwrap();
        b.publish(a).unwrap();
        assert!(b.accept(payload(a, 4)).is_err());
        assert!(b.accept(rec(a, 0, 1)).is_err());
        assert!(b.accept(rec(a, 9, 0)).is_err());
        let mut invalid = rec(a, -2, 0);
        invalid.receipt.lease = 1;
        assert!(b.accept(invalid).is_err());
        b.accept(rec(a, -2, 0)).unwrap();
        assert_eq!(b.take_terminal(a).unwrap().result, -2);
    }
}
#[test]
fn ring_local_identity_cannot_alias_other_book_cookie() {
    let mut a = RequestBook::new(1).unwrap();
    let mut b = RequestBook::new(1).unwrap();
    let ac = a.reserve(id(1, 1), Kind::Control).unwrap();
    let bc = b.reserve(id(1, 1), Kind::Control).unwrap();
    assert_ne!(ac, bc);
    a.publish(ac).unwrap();
    b.publish(bc).unwrap();
    assert!(a.accept(rec(bc, 0, 0)).is_err());
    assert!(b.accept(rec(ac, 0, 0)).is_err());
    a.accept(rec(ac, 0, 0)).unwrap();
    b.accept(rec(bc, 0, 0)).unwrap();
    a.take_terminal(ac).unwrap();
    b.take_terminal(bc).unwrap();
}
#[test]
fn geometry_maximum_capacity_and_cookie_exhaustion() {
    for n in [0, 3, 16384] {
        assert!(RequestBook::new(n).is_err());
    }
    let mut b = RequestBook::new(8192).unwrap();
    assert_eq!(b.reserve(id(64, 1), Kind::Control), Err(Error::Geometry));
    assert_eq!(b.reserve(id(0, 0), pooled(true)), Err(Error::Geometry));
    assert_eq!(
        b.reserve(
            id(0, 1),
            Kind::PooledRead {
                pool: id(1, 1),
                limit: 0,
                multi: true
            }
        ),
        Err(Error::Geometry)
    );
    let ids: Vec<_> = (0..8192)
        .map(|_| b.reserve(id(0, 1), Kind::Control).unwrap())
        .collect();
    assert_eq!(b.occupied(), 8192);
    assert_eq!(b.reserve(id(0, 1), Kind::Control), Err(Error::Full));
    for id in ids {
        b.cancel_unpublished(id).unwrap();
    }
    assert_eq!(b.occupied(), 0);
    request_book::exhausted_cookie_test();
}
