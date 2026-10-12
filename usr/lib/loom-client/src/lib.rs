//! The native Loom client cores: payload leases, the paired CQ reader, request
//! correlation, fixed I/O and its futures, retirement, and the creator-thread loop.
#![no_std]
// Until the native adapter composes the cores, their only caller is the tests.
#![cfg_attr(not(test), allow(dead_code))]

#[cfg(test)]
extern crate std;

pub mod service_abi;

mod fixed_futures;
mod fixed_requests;
mod lease_ledger;
mod local_tasks;
mod local_wait;
mod owned_mapping;
mod paired_cq;
mod request_book;
mod retirement_executor;
mod ring_payload;
mod task_loop;

#[cfg(test)]
mod tests;
