//! Kernel endpoint mirror; contains no raw pixels, user pointers or GPU commands.
pub const STATUS: u64 = 1;
pub const INPUT: u64 = 2;
pub const ACK: u64 = 3;
pub const FRAME: u64 = 4;
pub const KEY: u64 = 5;
pub const RESTORED: u64 = 6;
pub const FAIL: u64 = 7;
pub const QUERY: u64 = 8;
pub const VISIBLE: u64 = 9;
pub const MASK: u64 = 10;
pub const GRANT: u64 = 11;
pub const CLIENT: u64 = 12;
#[repr(C)]
pub struct Message {
    pub generation: u64,
    pub sequence: u64,
    pub phase: u32,
    pub length: u32,
    pub code: u32,
    pub value: u32,
    pub data: [u8; 512],
}
impl Default for Message {
    fn default() -> Self {
        Self { generation: 0, sequence: 0, phase: 0, length: 0, code: 0, value: 0, data: [0; 512] }
    }
}
const _: () = assert!(core::mem::size_of::<Message>() == 544);
const _: () = assert!(core::mem::offset_of!(Message, data) == 32);
#[cfg(feature = "guest")]
pub fn call(op: u64, msg: &mut Message) -> Result<(), ()> {
    let mut result = op as i64;
    unsafe {
        core::arch::asm!("svc #0", inlateout("x0") result,
            in("x1") msg as *mut Message as u64,
            in("x8") libthyla_rs::T_SYS_TRUSTED_SEAT, options(nostack));
    }
    if result == 0 { Ok(()) } else { Err(()) }
}
impl Drop for Message {
    fn drop(&mut self) {
        // INPUT/KEY can carry credentials. Scrub even on an error return.
        let p = self as *mut Self as *mut u8;
        for i in 0..core::mem::size_of::<Self>() {
            unsafe { core::ptr::write_volatile(p.add(i), 0); }
        }
    }
}
