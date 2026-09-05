---
id: chg-2026-08-03-parley-sweep
type: chg
title: "parley — a cap set sixteen times above the heap it is meant to protect"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-parley
  - moc-userspace-shell-tui
established:
  - sub-parley
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 42: parley, the LSP/DAP client substrate — json, frame, jsonrpc, lsp,
dap, dapc, transport, lib. 8 files, 3919 lines. Main advanced to `d669299c`
(#130's residue); merged before starting, and the merge is its own finding
below. L-1 absent on the THIRTIETH check.

**THE SUBJECT IS A PURE PROTOCOL STACK WITH ONE PLATFORM-TOUCHING LAYER.**
Seven modules: the tree's first JSON codec at the bottom (there was none —
`serde_json` is std-only, and the "hand-rolled JSON" in httpd/curl was HTTP
`Content-Length`, not JSON), Content-Length framing above it, two grammars
(jsonrpc for LSP, dap for the debug adapter — a genuinely different envelope),
two clients holding the policy, and `transport` alone behind a `backend`
feature. `audit: light`, and honestly so: no privilege boundary exists here.
But it is the crate that eats untrusted bytes from another process, so the
hazards are the parser ones — bounds, recursion, allocation — and it was
prosecuted on those.

The organizing property is that **policy is pure**: every client method that
would produce traffic RETURNS the message instead of sending it, so both
clients are exercised on the host with no process. That is why 73 tests here
are real. Confirmed by running them, which also re-confirms the b41 census
correction (#105).

**F1 -- THE BODY CAP IS SIXTEEN TIMES THE HEAP, SO IT CANNOT FIRE IN THE BAND
WHERE IT IS NEEDED (#120).** `frame.rs` sets `MAX_BODY_BYTES` = 64 MiB, checked
against the declared `Content-Length` BEFORE any body is buffered — a genuinely
good shape, and it does refuse an absurd declaration without allocating.
`libthyla_rs::alloc` sets `INITIAL_HEAP_SIZE` = 4 MiB and the header is explicit
that it does not grow ("Growable heap is a v1.x consideration").

So the guard covers only declarations ABOVE 64 MiB. Anything between roughly 4
and 64 MiB passes the check, and the decoder then buffers toward it until the
allocator fails — which in a native binary reaches the default `no_std`
handler, panics, and lands in libthyla-rs's panic handler as `t_exits(1)`. The
editor exits silently, instead of producing the `FrameError` whose documented
purpose is to let the caller *tear the connection down and say why*. The
effective threshold is lower still: a frame costs 2–3x its size at peak (the
decoder's buffer, the copy `next_frame` drains out of it, the parsed tree on
top).

The comment above the constant is not wrong, which is the interesting part: "a
hostile Content-Length cannot make us buffer forever" is TRUE. It just is not
the operative bound. **A cap is only a cap if it sits below the point where the
system stops working** — above that, the real limit is enforced by process
death, and the diagnosable failure the module designed for is unreachable.

**F2 -- AND THE UNTESTED CAP IS THE ONE THAT IS WRONG (#123).**
`oversized_header_errors` covers `MAX_HEADER_BYTES` — pushes one byte past it
with no separator, asserts the error. Eleven framing tests, and the single one
absent is `MAX_BODY_BYTES`. The gap did not cause F1, but it is why F1 reads as
settled: a confident sentence above a constant nothing exercises.

**F3 -- THE MANIFEST IS A CONSTRUCTION SNAPSHOT (#121).** `Cargo.toml` lists
three modules under "Hosts, incrementally" — one still tagged "this slice" —
while the crate has seven; frame, lsp, dap and dapc are absent, all four having
landed after the comment. It also disagrees with `lib.rs` about which layers
are pure ("json, jsonrpc" vs "json, jsonrpc, dap"; frame is pure too). The #108
/ #113 shape, arriving in the file a reader opens FIRST to learn what a crate
is.

**F4 -- HALF A GUARD (#122).** `parse_position` does `.unwrap_or(0).max(0) as
u32`. The `.max(0)` handles a negative line or character; the cast truncates the
other end silently, so line 2^32 becomes line 0. Someone thought about range and
wrote half of it. No memory consequence, and gopls will not do it — recorded
because the guarded half is what makes the unguarded half invisible.

**THE COUNTERWEIGHTS ARE UNUSUALLY STRONG, AND ONE OF THEM IS A CLAIM THAT
CHECKS OUT.** `transport` has zero host tests and SAYS so, pointing at an
in-guest probe: spawn `parley-echo`, frame-send, poll, pump, decode, assert the
round-trip. **That probe exists**, is built into the ramfs, and joey runs it
boot-fatal. After eighteen batches of "the coverage lives elsewhere" turning out
to mean "nowhere", a deferral that names its destination and the destination
being real is worth recording as loudly as a defect.

Three more. `json.rs`'s tests were written by someone who knew the hazards —
`depth_guard` drives MAX_DEPTH and MAX_DEPTH+1, `id_fidelity` parses 2^53+1 and
asserts it survives, which is the whole reason `Int`/`Float` are separate
variants. `dap::classify` defaults `success` to FALSE with the reason stated: a
client must never assume an unconfirmed request succeeded. And
`did_change_full` explains why full sync beats incremental — one dropped or
misordered change desynchronizes silently, after which the server reports
diagnostics against text the user never typed — a reason written where the
decision is, the b34 shape answered correctly for the second batch running.

**F5 -- AN OBSERVATION, NOT A DEFECT: THE SAME DESIGN NOTE THAT JUSTIFIES A
DIFFERENCE CHANGES SOMETHING IT DOES NOT MENTION.** LSP supersedes
hover/definition/completion, so at most one of each is outstanding and its
pending map is bounded BY CONSTRUCTION. DAP deliberately does not, and the
header says why — unique seq, exact match, no stale reply possible — which is
correct and well argued in the register of *staleness*. But the same choice
means a DAP entry is removed only by a matching response, so its bound is
behavioral rather than structural. The author was demonstrably thinking about
leaks: `on_response` takes the pending entry BEFORE checking `success`, with a
comment that a failed response must clear the slot too. The never-answered leg
is simply not in view. Bounded in practice by the session outliving the adapter
— recorded because it is the kind of property that should be stated rather than
inferred by the next reader.

**THE MERGE WAS ITS OWN FINDING, AND IT IS THE ARC'S OWN SHAPE AGAIN (#119).**
`docs/reference/09-test-harness.md`'s absorption stub conflicted for the SECOND
time. That is the tripwire working exactly as the stub predicts in its own text
— and it is also the signal. `kernel/test/test.c` is unowned (no note names it
in `code:`), the stub itself defers it, and so main keeps writing harness
knowledge into a tombstone because **the harness still has nowhere else to
write**. Folded rather than dropped, per the #125 precedent: three items from
`d669299c` now sit in a PENDING ABSORPTION block with their destinations worked
out.

One of those three is worth naming here, because it is this arc's lesson
arriving from the main track independently: the wait-predicate table listed
"assert a flag cleared" as an observable with no precondition, and the
correction is that **a cleared flag means the act STARTED, not that it
FINISHED**. Sound only where the clearer provably completes the act — a property
of the specific consumer, to be re-established per site and never inherited from
the table. Same root as `burrow_handle_count() == 0` not meaning "the pages were
freed". A shape stated generally, inherited by a site the generality did not
cover.

LEDGER, read off the rendered view — after `render`, per the rule the last five
entries have been sharpening. Corpus 838 -> **840**. Coverage 236 -> **244 owned
of 421**, 56% -> **57%**; unswept lines 63136 -> **59217** (-6.2%). `usr/lib`
15/26 -> **23/18**.

And the rule earned its keep in the smallest possible way. I had drafted 58%,
which is what 244/421 rounds to — the view floors, and says 57%. Arithmetically
defensible, and still not what the artifact reports. **The ledger is a reading
of a generated file, not a calculation about it**, and the two agree until they
do not.

Two more from the writing itself, both caught by `lint` rather than by me: a
prose example of a deeply-nested JSON array is literally wikilink syntax, so a
dossier about a JSON codec creates dangling links by describing its own subject;
and I linked `sub-nora` before writing it, which the Present plane refuses.
Neither is a code defect — recorded because the tool caught what the author did
not, which is the same relationship this arc keeps documenting between tests and
claims.
