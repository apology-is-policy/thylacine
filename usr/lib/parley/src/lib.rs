// parley -- the LSP/DAP client substrate for the Nora Go IDE (Stage 8e).
//
// The dialogue layer between Nora and the language/debug servers (gopls +
// Ambush). See docs/NORA-IDE-UX.md section 6 (the async client architecture).
//
// Built incrementally:
//   - json      (8e-1a) -- a no_std JSON value + parser + serializer.
//   - jsonrpc   (8e-1b) -- Content-Length framing + the JSON-RPC 2.0 model.
//   - transport (8e-1c) -- persistent-child + PollSet driver (libthyla-rs).
//
// `#![cfg_attr(not(test), no_std)]`: no_std for the aarch64-thylacine userspace
// target, but std under `cargo test` so the pure layers (json, jsonrpc) run on
// the host without libthyla-rs (the kaua pattern).

#![cfg_attr(not(test), no_std)]

extern crate alloc;

pub mod frame;
pub mod json;
pub mod jsonrpc;
