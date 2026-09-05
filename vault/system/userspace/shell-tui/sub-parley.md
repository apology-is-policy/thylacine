---
id: sub-parley
type: sub
title: "parley — the LSP/DAP client substrate: a pure protocol stack with one platform-touching layer"
parent: moc-userspace-shell-tui
code:
  - usr/lib/parley/src/lib.rs
  - usr/lib/parley/src/json.rs
  - usr/lib/parley/src/frame.rs
  - usr/lib/parley/src/jsonrpc.rs
  - usr/lib/parley/src/lsp.rs
  - usr/lib/parley/src/dap.rs
  - usr/lib/parley/src/dapc.rs
  - usr/lib/parley/src/transport.rs
audit: light
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: []
created: 2026-08-03
updated: 2026-08-03
---
## Purpose

The dialogue layer between the editor and the two servers it talks to: `gopls`
over LSP and Ambush over DAP. A *parley* is a formal exchange between parties
under an agreed protocol, which is exactly what both are.

Seven modules in a deliberate stack. At the bottom, the tree's **first JSON
codec** — there was none, because `serde_json` is std-only and the
"hand-rolled JSON" in `httpd`/`curl` was HTTP `Content-Length` parsing, not
JSON. Above it, `Content-Length` framing shared by both protocols; then the two
*grammars* (`jsonrpc` for LSP, `dap` for the debug adapter, which is a
different envelope entirely); then the two *clients* that hold the policy — id
allocation, the pending map, response dispatch, the diagnostic store. Only
`transport` touches the platform.

**The organizing property is that policy is pure.** Every client method that
would produce traffic *returns the message* rather than sending it. So the
whole of both clients — the handshakes, staleness, dispatch, body parsing —
runs on the host with no process and no device, and the device wiring is a thin
loop. That is what makes 73 of this crate's tests real rather than aspirational
(see [[sub-kaua]] for the same `cfg_attr(not(test), no_std)` arrangement, and
task #105 for the three crates where it is absent).

## Contract

`json::Value::parse` takes untrusted bytes and returns a value or a positioned
error; `Value`'s `Display` is compact, insertion-ordered serialization.
`frame::encode`/`Decoder` wrap and unwrap the `Content-Length` envelope,
streaming — a header or body may split across reads and several frames may
arrive in one. `jsonrpc::classify` / `dap::classify` reduce a parsed value to
the three things a client must react to. `lsp::Client` / `dapc::Client` mint
ids, remember what each outstanding id asked for, and turn an incoming message
into an `Action` for the host.

**Both `handle` methods are total.** Every input yields an action, and an
unknown, unmatched, superseded or malformed message yields `Action::Ignored`
rather than an error. The editor must survive whatever a server says. The one
category that is never silent is a server-initiated *request*: silence would
hang a server blocking on the reply, so an unrecognized method gets the
protocol-correct refusal (`-32601` for LSP, a `success:false` response for
DAP).

## Mechanism

**Latest-wins, on the LSP side only.** Minting a hover / definition /
completion request *drops* any outstanding request of the same kind, so the
superseded reply classifies as `Ignored` instead of painting an answer for a
cursor position the user has already left. A stale answer that looks current is
worse than no answer.

DAP deliberately does not do this, and the header says why: every request
carries a unique `seq`, a response matches exactly one pending entry, so a
stale reply is impossible by construction. The `Variables(i64)` command variant
carries its `variablesReference` precisely so several `variables` requests can
be outstanding at once while a lazy tree expands. (See Caveats — the same
choice moves which side has a *structural* bound on the pending map, which the
header does not mention.)

**Position encoding is negotiated, not assumed.** LSP counts `character` in
UTF-16 code units by default, which silently corrupts every non-ASCII line if
the editor treats it as bytes. The client advertises `utf-8`, records what the
server picked, and converts under whichever it chose. The default before the
handshake is `Utf16` — the LSP default, not the requested one — with the reason
written at the field: a server that ignores the request, or a handshake that
never completes, still converts correctly.

**Full document sync, not incremental.** `did_change_full` sends the whole
buffer on every change. The reason is written at the method: incremental sync
requires client and server to agree edit-for-edit, and one dropped or
misordered change desynchronizes them *silently*, after which the server
reports diagnostics against text the user never typed. Full sync is
self-correcting by construction.

## Data structures

`json::Value` is the usual sum type with one deliberate split: `Int(i64)` for
integers that fit and `Float(f64)` otherwise. A JSON-RPC `id` or DAP `seq` is
an integer and must round-trip exactly, so integral numbers never become a
lossy `f64` — pinned by a test that parses 2^53+1 and asserts it back out
unchanged.

Objects are an insertion-ordered `Vec<(String, Value)>`, not a map: messages
are small, order makes serialization deterministic and diffable in tests, and
`no_std` has no hash map without a dependency. Lookup is a linear scan.

Both clients hold `pending: Vec<(Id, Kind)>` and the negotiated state
(`PositionEncoding` + `ready` for LSP, `Capabilities` + `configured` for DAP).
`frame::Decoder` holds a byte buffer and `body_len: Option<usize>` — `None`
means "scanning for a header", `Some(n)` means "awaiting n body bytes".

## Concurrency

None. Every type here is single-threaded by construction and holds no lock, no
atomic and no interior mutability; `Client` methods take `&mut self` and the
borrow checker serializes them. The host owns the event loop.

`transport::Mux` is the only thing that blocks: it rebuilds its `PollSet` on
every `poll` call from the caller-supplied `(fd, tag)` set, so a restarted or
closed server can never leave a stale fd registered. The set is a handful of
entries, so the rebuild is free and the stale-fd class is structurally
impossible rather than carefully avoided.

## Invariants enforced

None of the section-28 kernel invariants — parley is a client library that
speaks to processes over pipes. It holds no capability, maps nothing, and
touches no device. A defect here corrupts the editor's own view of its servers;
the kernel and the servers validate independently.

The properties it does enforce are its own: bounded parse recursion, exact
integer round-trip, total dispatch, and the framing/length discipline below.

## Error paths

`json::Error` carries a byte offset and a static description. `frame::FrameError`
carries only a message, and its doc states the intended response: a framing
error means the byte stream is no longer trustworthy, so the caller tears the
connection down.

Two caps guard the decoder. `MAX_HEADER_BYTES` (8 KiB) bounds a peer that never
sends the terminating blank line. `MAX_BODY_BYTES` (64 MiB) is checked against
the declared `Content-Length` *before* any body is buffered, so an absurd
declaration is refused without allocating. See Caveats for the band where the
second one cannot fire.

Parse recursion is bounded at `MAX_DEPTH` = 128 via an explicit `enter()`
counter, so an endlessly-nested array or object cannot exhaust the stack. The
depth is not decremented on error paths, which is correct because an error
aborts the whole parse.

## Performance

Not a hot path: LSP/DAP traffic is keystroke-idle and human-paced. `Decoder`'s
`find_sep` rescans from the buffer start on each call, which is quadratic in
principle but bounded by the 8 KiB header cap — once a body length is known, no
scanning happens at all. `Value::get` is a linear scan over a small object.
`transport::Server::pump` does one 16 KiB `read` per readable report and lets
poll re-arm while bytes remain, so a second read never blocks on a
now-empty pipe.

## Prosecution

Not audit-bearing, and the reason is structural rather than a filing decision:
there is no privilege boundary in this crate. But it is the crate that eats
**untrusted bytes from another process**, so the hazards are the parser ones —
bounds, recursion, and allocation — and they were prosecuted on those terms.

Traced sound: the depth guard (`enter()` covers both container arms, and a
container's early-return-on-empty decrements correctly); the `\uXXXX` surrogate
pair arithmetic (the combined codepoint cannot exceed `0x10FFFF`, and
`char::from_u32` guards it anyway); `parse_hex4`'s bounds check preceding four
unchecked indexes; the UTF-8 copy path, which computes a length from the lead
byte, bounds-checks it against the buffer, *then* validates with `from_utf8` —
so a continuation byte or a truncated sequence errors rather than slicing out
of range; `parse_content_length`'s `checked_mul`/`checked_add` accumulation; and
both position conversions, traced by hand across an astral character in both
directions and under all three encodings (`char_to_byte` rounds a mid-character
offset *up* to the next boundary, so the result is always a safe slice index,
and the doc says so).

## Seams

- **`transport` has no host tests, by design.** It needs `libthyla-rs`, so it
  sits behind the `backend` feature and is proven end to end in-guest by
  `parley-probe`: spawn `parley-echo` as a persistent child, frame-send a
  request, poll, pump, decode, assert the round-trip. That probe exists, is
  built into the ramfs, and is boot-fatal via joey — the claim that the
  coverage lives elsewhere is one that checks out.
- Only integer request ids are minted. JSON-RPC permits string ids; a *server*
  request's id is echoed back verbatim (`Incoming::Request` keeps it as a raw
  `Value`), so string ids work in the direction they occur.
- `Pending::Shutdown`'s response maps to `Action::Ignored`, so a host cannot
  distinguish "shutdown acknowledged" from "unmatched id". Harmless today —
  the documented sequence is to send `exit` after it either way.
- v1.0 resolution of a definition result takes the first `Location` of an array
  and ignores the rest.

## Caveats

**The body cap is sixteen times the heap, so it cannot fire in the band where
it is needed.** `MAX_BODY_BYTES` is 64 MiB; `libthyla_rs::alloc`'s
`INITIAL_HEAP_SIZE` is 4 MiB and *fixed* — the header is explicit that a
growable heap is a v1.x consideration. So a declared `Content-Length` above 64
MiB is refused cleanly, as designed, but any declaration between roughly 4 and
64 MiB passes the check and the decoder then buffers toward it until the
allocator fails. An allocation failure in a native binary reaches the default
`no_std` handler, which panics, which `libthyla-rs`'s panic handler turns into
`t_exits(1)` — so the editor exits silently instead of producing the
`FrameError` whose whole documented purpose is to let the caller tear the
connection down and say why. The effective threshold is lower still, because a
frame costs roughly two to three times its own size at peak: the decoder's
buffer, the copy `next_frame` drains out of it, and the parsed tree on top.
Filed as task #120. Whether real `gopls` traffic reaches that band is a
consumer question (nora, task #117); the guard being unreachable is a property
of this crate regardless.

**And the untested cap is the one that is wrong.** `oversized_header_errors`
covers `MAX_HEADER_BYTES`; nothing covers `MAX_BODY_BYTES`. Eleven framing
tests, and the gap is exactly the constant that does not work.

**The manifest is a construction snapshot.** `Cargo.toml` lists three modules
under "Hosts, incrementally" — one of them tagged "this slice" — while the
crate has seven; `frame`, `lsp`, `dap` and `dapc` are absent. It also names the
pure layers as "(json, jsonrpc)" where `lib.rs` says "(json, jsonrpc, dap)",
and `frame` is pure too. Same shape as task #108 in libutopia and #113's
palette rename: an accurate note about the state of construction, left behind
by the construction. Filed as task #121.

**`parse_position` guards one end of its range and truncates the other.** It
does `.unwrap_or(0).max(0) as u32`, so a negative line or character clamps to
0 — but a value above `u32::MAX` wraps: line 2^32 silently becomes line 0.
Half a guard, on the side where the wrong answer looks legitimate. No memory
consequence (`Position` is just two integers), and gopls will not do it. Filed
as task #122.

**The pending map is structurally bounded on one side and behaviorally bounded
on the other, and the header explains the difference in the other register.**
LSP's supersession caps the three cursor-following kinds at one outstanding
each, so its map is bounded by construction. DAP has no supersession — rightly,
and the header says why in terms of *staleness*: unique seq, exact match, no
stale reply possible. But the same choice means a DAP entry is removed only by
a matching response, so a request that is never answered stays forever. The
author was thinking about leaks (`on_response` takes the pending entry before
checking `success`, with a comment saying a failed response must clear the slot
too) — the never-answered leg is simply not discussed. Bounded in practice by
the session outliving the adapter; worth stating rather than inferring.

**Duplicate JSON keys resolve differently here than in Go.** `Value::get`
returns the *first* match while retaining all pairs; Go's `encoding/json` —
what gopls and Delve use — takes the *last*. Only observable if a parsed object
is re-serialized back to the peer, which no current path does.

**The serializer has no depth bound although the parser does.** A parsed value
is capped at 128, but `Value` can be *constructed* arbitrarily deep, and
`Display` recurses without a counter. No current builder nests beyond a fixed
handful.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
