//! Trusted display/input broker core. Hardware-specific code must discharge the
//! kernel-bound acknowledgement: a submitted flip alone never proves exclusive
//! display ownership. The backend must retire and verify the device operation.
#![no_std]
extern crate alloc;

pub mod objects;
pub mod endpoint;
pub mod wire;
pub mod model;
pub mod render;

pub mod skein;
pub mod keymap;
pub mod limits;
#[cfg(feature = "backend")]
pub mod backend;
pub mod gpu_api;
pub mod framing;
#[cfg(feature = "guest")]
pub mod rpc_client;
#[cfg(feature = "guest")]
pub mod proxy;
