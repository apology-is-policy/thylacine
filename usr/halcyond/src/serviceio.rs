//! Thin native shell for servicewire. Callers set nonblocking mode BEFORE
//! publishing a connection; this adapter must never be used for blocking fds.
use halcyond::servicewire::{Endpoint, IoError};
pub struct NativeEndpoint(pub i64);
fn result(n: i64) -> Result<usize, IoError> {
    if n == -11 {
        Err(IoError::Again)
    } else if n < 0 {
        Err(IoError::Closed)
    } else {
        Ok(n as usize)
    }
}
impl Endpoint for NativeEndpoint {
    fn read(&mut self, dst: &mut [u8]) -> Result<usize, IoError> {
        result(unsafe { libthyla_rs::t_read(self.0, dst.as_mut_ptr(), dst.len()) })
    }
    fn write(&mut self, src: &[u8]) -> Result<usize, IoError> {
        result(unsafe { libthyla_rs::t_write(self.0, src.as_ptr(), src.len()) })
    }
    fn now_ns(&self) -> u64 {
        libthyla_rs::time::monotonic_ns()
    }
}
