//! The part of Corvus a host can test: the AUTH record table and the bounds it holds.
#![no_std]

pub use corvus_crypto::KEYPAIR_LEN;

pub const TOKEN_LEN: usize = 33;
pub const MAX_USER_LEN: usize = 32;
pub const MAX_CONNS: usize = 8;

pub mod sessions;
