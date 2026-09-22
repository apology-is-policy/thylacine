//! Bounded binary payload inside the broker's 9P ctl stream. No native-layout
//! structs, addresses or trailing data. Decoding snapshots the entire request
//! before validation or hardware access.
use alloc::vec::Vec;
pub const MAX_PACKET: usize = 65536;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Malformed;
pub struct Reader<'a> { bytes: &'a [u8], off: usize }
impl<'a> Reader<'a> {
    pub fn new(bytes: &'a [u8]) -> Result<Self, Malformed> {
        if bytes.len() > MAX_PACKET { return Err(Malformed); }
        Ok(Self { bytes, off: 0 })
    }
    pub fn take(&mut self, n: usize) -> Result<&'a [u8], Malformed> {
        let end = self.off.checked_add(n).ok_or(Malformed)?;
        let bytes = self.bytes.get(self.off..end).ok_or(Malformed)?;
        self.off = end;
        Ok(bytes)
    }
    pub fn finish(self) -> Result<(), Malformed> {
        if self.off == self.bytes.len() { Ok(()) } else { Err(Malformed) }
    }
    pub fn remaining(&self) -> usize { self.bytes.len() - self.off }
}
pub trait Wire: Sized {
    fn put(&self, out: &mut Vec<u8>);
    fn get(r: &mut Reader<'_>) -> Result<Self, Malformed>;
}
macro_rules! integer {
    ($($t:ty),*) => {$(impl Wire for $t {
        fn put(&self, out: &mut Vec<u8>) { out.extend_from_slice(&self.to_le_bytes()); }
        fn get(r: &mut Reader<'_>) -> Result<Self, Malformed> {
            let bytes = r.take(core::mem::size_of::<Self>())?;
            Ok(Self::from_le_bytes(bytes.try_into().map_err(|_| Malformed)?))
        }
    })*};
}
integer!(u8, u16, u32, u64, i64);
impl Wire for usize {
    fn put(&self, out: &mut Vec<u8>) { (*self as u64).put(out); }
    fn get(r: &mut Reader<'_>) -> Result<Self, Malformed> { usize::try_from(u64::get(r)?).map_err(|_| Malformed) }
}
impl Wire for bool {
    fn put(&self, out: &mut Vec<u8>) { (*self as u8).put(out); }
    fn get(r: &mut Reader<'_>) -> Result<Self, Malformed> {
        match u8::get(r)? { 0 => Ok(false), 1 => Ok(true), _ => Err(Malformed) }
    }
}
impl Wire for () {
    fn put(&self, _: &mut Vec<u8>) {}
    fn get(_: &mut Reader<'_>) -> Result<Self, Malformed> { Ok(()) }
}
impl<T: Wire> Wire for Option<T> {
    fn put(&self, out: &mut Vec<u8>) {
        self.is_some().put(out);
        if let Some(value) = self { value.put(out); }
    }
    fn get(r: &mut Reader<'_>) -> Result<Self, Malformed> {
        if bool::get(r)? { Ok(Some(T::get(r)?)) } else { Ok(None) }
    }
}
impl<T: Wire, E: Wire> Wire for Result<T, E> {
    fn put(&self, out: &mut Vec<u8>) {
        self.is_ok().put(out);
        match self { Ok(value) => value.put(out), Err(error) => error.put(out) }
    }
    fn get(r: &mut Reader<'_>) -> Result<Self, Malformed> {
        if bool::get(r)? { Ok(Ok(T::get(r)?)) } else { Ok(Err(E::get(r)?)) }
    }
}
impl<T: Wire> Wire for Vec<T> {
    fn put(&self, out: &mut Vec<u8>) {
        (self.len() as u32).put(out);
        for value in self { value.put(out); }
    }
    fn get(r: &mut Reader<'_>) -> Result<Self, Malformed> {
        let n = u32::get(r)? as usize;
        // All broker vector element types consume at least one wire byte. This
        // bound is conservative for larger types, and forbids allocation bombs.
        if n > r.remaining() { return Err(Malformed); }
        let mut values = Vec::new();
        for _ in 0..n { values.push(T::get(r)?); }
        Ok(values)
    }
}
macro_rules! tuple {
    ($($name:ident),+) => {
        impl<$($name: Wire),+> Wire for ($($name,)+) {
            #[allow(non_snake_case)]
            fn put(&self, out: &mut Vec<u8>) { let ($($name,)+) = self; $($name.put(out);)+ }
            fn get(r: &mut Reader<'_>) -> Result<Self, Malformed> { Ok(($($name::get(r)?,)+)) }
        }
    };
}
tuple!(A, B);
tuple!(A, B, C);
tuple!(A, B, C, D);
tuple!(A, B, C, D, E, F, G);

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rejects_noncanonical_tags_lengths_and_trailing_data() {
        assert_eq!(bool::get(&mut Reader::new(&[2]).unwrap()), Err(Malformed));
        assert_eq!(Vec::<u64>::get(&mut Reader::new(&[255; 4]).unwrap()), Err(Malformed));
        assert_eq!(Reader::new(&[1]).unwrap().finish(), Err(Malformed));
        let bytes = [1, 0, 0, 0, 0]; // one u64, but only one payload byte
        assert_eq!(Vec::<u64>::get(&mut Reader::new(&bytes).unwrap()), Err(Malformed));
    }
    #[test]
    fn every_truncated_record_is_refused() {
        let mut encoded = Vec::new();
        let value: Result<Option<(u64, u32)>, u8> = Ok(Some((u64::MAX, 3)));
        value.put(&mut encoded);
        for n in 0..encoded.len() {
            let mut r = Reader::new(&encoded[..n]).unwrap();
            assert!(<Result<Option<(u64, u32)>, u8>>::get(&mut r).is_err());
        }
        let mut r = Reader::new(&encoded).unwrap();
        assert_eq!(<Result<Option<(u64, u32)>, u8>>::get(&mut r), Ok(value));
        r.finish().unwrap();
    }
}
