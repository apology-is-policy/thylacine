//! virtio-serial's pure half: the virtio console's control protocol and the driver's end of
//! the line to the Mac. Both are host-tested; the device and the syscalls are in main.rs.
#![cfg_attr(not(test), no_std)]
extern crate alloc;

pub mod console;
pub mod relay;
