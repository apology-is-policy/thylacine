//! The native Loom client cores: payload leases, the paired CQ reader, request
//! correlation, fixed I/O and its futures, retirement, and the creator-thread loop;
//! and the native adapter that drives a private ring with them.
// The tests run with std, as the cores' original test binaries did; every other
// build, the guest's included, compiles them no_std.
#![cfg_attr(not(test), no_std)]
// The adapter does not compose every core (the futures over fixed I/O, the task
// loop and the retirement executor); those are reached only by the tests.
#![cfg_attr(not(test), allow(dead_code))]

pub mod native;
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
mod ring_records;
mod task_loop;

#[cfg(test)]
mod tests;
