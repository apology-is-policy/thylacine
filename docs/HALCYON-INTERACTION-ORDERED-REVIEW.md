# Ordered ownership stream self-review

Author review only; no independent adversarial audit is claimed.

The source journal serializes invalidation and admission at the compositor owner.
Rendering's coalesced events are not consulted for permission. Ready binds a
fresh stream identity; contiguous sequence, exact unused bytes and fixed record
length are mandatory. Overflow discards the unread history and clears published
contexts. Reopening does not restore them. Snapshot, ACK and same-epoch subject
changes are emitted before the corresponding decision; leaf reuse is guarded by
the binding locator and controller route incarnation.

The client has one read and one write with separate registered-buffer slices.
It handles either CQE order, holds an early decision until successful Rwrite,
and rejects malformed, duplicate-sequence or unsolicited decisions. A failed
write cannot release a previously received success. Ring destruction precedes
buffer/fid destruction. One parked read is removed by flush, clunk, version reset
or connection teardown; delayed transport replies cannot become new authority.
The fixed journal is bounded to 8 KiB per connection (eight connections). This
is only its incremental charge, not a complete session/kernel memory ledger.

The Halcyon adapter uses authenticated records only. It resolves leaf plus binding
to the exact local route, including provisional controllers. Unknown old bindings
are ignored. Reset retires controllers and payloads while preserving only the
previous HSC normal-seat membership; it cannot grant a seat or revive scopes.
The independent HSC lane is pumped first. Any ordered channel fault ends the
posted service according to its existing fatal-owner policy. Application dispatch
is off, so unexpected decisions still fail closed rather than being routed to an
unfinished client adapter. Publication must still revalidate the application peer.

Findings repaired: R1 selector confused declared session with console-renderer;
fresh kernel identity comparison now follows the declared-session contract.
R2 receipt decoding assumed seat generation was nonzero. Lictor's initial normal
seat is zero; it is an explicit Some(0), distinct from no membership. Real native
publication demonstrated the failure. Both codec and producer tests now exercise
zero and a named mutant rejects it. Focus remains nonzero because layout epochs
start at one. R3 the initial mutation witness used unwrap rather than an assertion;
the harness refused to count that failure, and the witness was corrected.

Measured results and graphics status are in HALCYON-INTERACTION-STATUS and
work/oct2-hi-ordered. Failed native/build/harness evidence remains. Temporary
kernel tracing was removed. App dispatch, deadlines, reply cancellation,
aggregate resource accounting, populated two-client native tests and user-facing
modal painting remain integration work; this checkpoint does not activate them.
