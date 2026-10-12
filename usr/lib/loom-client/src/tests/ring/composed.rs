use super::*;
struct CqFixture {
    memory: Box<UnsafeCell<[u64; 24]>>,
    reader: paired_cq::PairedCq,
    tail: u32,
}
impl CqFixture {
    fn new() -> Self {
        let memory = Box::new(UnsafeCell::new([0u64; 24]));
        let base = memory.get().cast::<u8>();
        let reader = unsafe { paired_cq::PairedCq::new(base, 192, 0, 64, 96, 2) }.unwrap();
        Self {
            memory,
            reader,
            tail: 0,
        }
    }
    fn post(&mut self, cookie: u64, result: i32, flags: u32, receipt: BufferReceipt) {
        use std::sync::atomic::{AtomicU32, Ordering};
        let base = self.memory.get().cast::<u8>();
        let head = unsafe { &*base.add(16).cast::<AtomicU32>() }.load(Ordering::Acquire);
        assert!(self.tail.wrapping_sub(head) < 2);
        unsafe {
            base.add(64 + (self.tail as usize & 1) * 16)
                .cast::<paired_cq::Completion>()
                .write(paired_cq::Completion {
                    user_data: cookie,
                    result,
                    flags,
                });
            base.add(96 + (self.tail as usize & 1) * 32)
                .cast::<BufferReceipt>()
                .write(receipt);
        }
        self.tail = self.tail.wrapping_add(1);
        unsafe { &*base.add(20).cast::<AtomicU32>() }.store(self.tail, Ordering::Release);
    }
    fn head(&self) -> u32 {
        use std::sync::atomic::{AtomicU32, Ordering};
        unsafe { &*self.memory.get().cast::<u8>().add(16).cast::<AtomicU32>() }
            .load(Ordering::Acquire)
    }
}
#[test]
fn composed_paired_delivery_keeps_a_across_b_final_and_teardown() {
    use request_book::{Kind, RequestBook};
    let (p, mut root, c) = fixture();
    let pool = unsafe { c.attach_pool(id(0, 1), &[range(0), range(8)]) }.unwrap();
    let mut book = RequestBook::new(2).unwrap();
    let cookie = book
        .reserve(
            id(3, 4),
            Kind::PooledRead {
                pool: id(0, 1),
                limit: 8,
                multi: true,
            },
        )
        .unwrap();
    book.publish(cookie).unwrap();
    let mut cq = CqFixture::new();
    cq.post(cookie, 8, 5, receipt(0, 1, 0, 1));
    let record = cq.reader.reap().unwrap().unwrap();
    assert_eq!(cq.head(), 1);
    assert_eq!(p.returns.get(), 0);
    let a = book.accept(record).unwrap();
    assert_eq!(a.cookie, cookie);
    assert_eq!(a.scope, id(3, 4));
    let a = unsafe { pool.accept(a.record.cqe.result, a.record.cqe.flags, a.record.receipt) }
        .unwrap()
        .unwrap();
    cq.post(cookie, 8, 5, receipt(0, 1, 1, 1));
    let b = book.accept(cq.reader.reap().unwrap().unwrap()).unwrap();
    let b = unsafe { pool.accept(b.record.cqe.result, b.record.cqe.flags, b.record.receipt) }
        .unwrap()
        .unwrap();
    assert_eq!(a.bytes(), &[0; 8]);
    assert_eq!(b.bytes(), &[1; 8]);
    assert!(b.try_return().is_ok());
    assert_eq!(root.live(), 1);
    let zero = BufferReceipt {
        pool: id(0, 0),
        member: 0,
        reserved: 0,
        lease: 0,
    };
    cq.post(cookie, 0, 0, zero);
    let final_record = book.accept(cq.reader.reap().unwrap().unwrap()).unwrap();
    assert!(final_record.terminal);
    assert!(
        unsafe {
            pool.accept(
                final_record.record.cqe.result,
                final_record.record.cqe.flags,
                final_record.record.receipt,
            )
        }
        .unwrap()
        .is_none()
    );
    assert_eq!(book.take_terminal(cookie).unwrap().result, 0);
    assert_eq!(book.occupied(), 0);
    assert_eq!(root.live(), 1);
    assert_eq!(a.bytes(), &[0; 8]);
    assert_eq!(cq.head(), 3);
    assert_eq!(p.returns.get(), 1);
    drop(c);
    p.retired.set(true);
    assert!(!root.progress());
    drop(root);
    drop(pool);
    drop(cq);
    assert_eq!(p.freed.get(), 0);
    assert_eq!(a.bytes(), &[0; 8]);
    drop(a);
    assert_eq!(p.freed.get(), 1);
}
#[test]
fn composed_abandoned_stream_and_corrupt_record_retain_owned_bytes() {
    use request_book::{Kind, RequestBook};
    let (p, mut root, c) = fixture();
    let pool = unsafe { c.attach_pool(id(0, 1), &[range(0), range(8)]) }.unwrap();
    let mut book = RequestBook::new(2).unwrap();
    let cookie = book
        .reserve(
            id(3, 4),
            Kind::PooledRead {
                pool: id(0, 1),
                limit: 8,
                multi: true,
            },
        )
        .unwrap();
    book.publish(cookie).unwrap();
    let mut cq = CqFixture::new();
    cq.post(cookie, 8, 5, receipt(0, 1, 0, 1));
    let delivery = book.accept(cq.reader.reap().unwrap().unwrap()).unwrap();
    let a = unsafe {
        pool.accept(
            delivery.record.cqe.result,
            delivery.record.cqe.flags,
            delivery.record.receipt,
        )
    }
    .unwrap()
    .unwrap();
    assert_eq!(book.abandon(cookie).unwrap(), Some(id(3, 4)));
    // Simulated executor owns ABORT dispatch; test only the post-abandon delivery path.
    p.rc.set(-2);
    cq.post(cookie, 8, 5, receipt(0, 1, 1, 1));
    let delivery = book.accept(cq.reader.reap().unwrap().unwrap()).unwrap();
    assert!(delivery.abandoned);
    drop(
        unsafe {
            pool.accept(
                delivery.record.cqe.result,
                delivery.record.cqe.flags,
                delivery.record.receipt,
            )
        }
        .unwrap()
        .unwrap(),
    );
    assert_eq!(root.pending(), 1);
    assert_eq!(root.live(), 1);
    assert_eq!(a.bytes(), &[0; 8]);
    cq.post(cookie, 8, 5, receipt(0, 2, 0, 2));
    let bad = cq.reader.reap().unwrap().unwrap();
    assert_eq!(cq.head(), 3);
    assert!(book.accept(bad).is_err());
    assert_eq!(book.occupied(), 1);
    assert_eq!(root.pending(), 1);
    assert_eq!(root.live(), 1);
    // Corruption must retire the owner after CQ ack, never refund the active request.
    drop(c);
    p.retired.set(true);
    assert!(!root.progress());
    drop(root);
    drop(pool);
    drop(book);
    drop(cq);
    assert_eq!(p.freed.get(), 0);
    assert_eq!(a.bytes(), &[0; 8]);
    p.rc.set(0);
    drop(a);
    assert_eq!(p.freed.get(), 1);
}
