//! Adapter-facing paths the original suite left unexercised.
use super::*;

#[test]
fn delivery_reports_its_own_reserved_kind() {
    let kinds = [
        Kind::Control,
        Kind::FixedRead(8),
        Kind::FixedWrite(8),
        pooled(false),
    ];
    let mut b = RequestBook::new(kinds.len()).unwrap();
    let ids: Vec<u64> = kinds
        .iter()
        .map(|&k| b.reserve(id(1, 1), k).unwrap())
        .collect();
    for &i in &ids {
        b.publish(i).unwrap();
    }
    // Reverse order, so a kind read from the wrong entry cannot match.
    for (&i, &k) in ids.iter().zip(&kinds).rev() {
        let r = if matches!(k, Kind::PooledRead { .. }) {
            payload(i, 4)
        } else {
            rec(i, 0, 0)
        };
        assert_eq!(b.accept(r).unwrap().kind, k);
    }
}
