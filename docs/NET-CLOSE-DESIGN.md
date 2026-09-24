# TCP close and transport retirement

Status: design and implementation APPROVED by the operator, 2026-09-17.
Implemented; three-backend byte verification and all eight application
regression gates pass. The full default/UBSan by SMP4/SMP8 gate passes 40/40.

## Observed failure

The `pci-net-load` scenario submits 8,388,608 bytes through the resident NIC.
The guest reports successful send completion, but the host verifies only
8,333,292 bytes before EOF. All received bytes have the expected value.
`Net::slot_unref` immediately removes the smoltcp socket when the last fid
clunks, dropping its transmit buffer. The existing throughput probe measures
accepted writes and does not check receipt, so it cannot detect this loss.
The failing serial, step and peer logs are preserved under `work/pci-net-load-loss.*`.

The public connection's lifetime and the transport's closing lifetime need
separate owners. Delaying the benchmark or adding a sleep to applications is
not a repair.

## Proposed contract

The last fid clunk still releases connection number N, its generation, pending
requests and Weft mapping. It must not discard bytes already accepted into an
established TCP stream. Instead, netd closes the send side and transfers the
socket handle plus its stack identity into a bounded retirement collection.
No old fid, slot number or newly minted connection can access that socket.
No kernel syscall, 9P verb or client-side close wait is added.

The ordinary event loop continues polling retired sockets, including transmit
retries, acknowledgements, FIN exchange and smoltcp's TIME-WAIT. It releases a
socket once smoltcp reaches Closed. A 30-second absolute retirement deadline
bounds an unresponsive peer; expiry aborts and removes the transport and
increments a diagnostic counter. A deadline is a network failure disposition,
not proof that outstanding bytes arrived. Application protocols still need
peer acknowledgements when delivery itself matters.

Unconnected/listening TCP sockets and other protocols release immediately.
Established and already-closing TCP sockets retain their transport state.
Unread receive bytes can be discarded after the last owner leaves; they must
not pin a full receive window and prevent the peer's FIN from progressing.

## Bounds and ownership

There remain at most 16 publicly owned connection slots. At most 64 TCP
transports, including active, accepted and retiring sockets, exist in total.
Admission reserves transport capacity before allocating a new TCP socket;
accepted-listener replacement follows the same gate. Clone reports the existing
ENOMEM resource refusal. As with existing public-slot exhaustion, a deferred
accept keeps its call buffered until replacement capacity is available; it
never allocates beyond the bound or discards an older stream. Capacity exhaustion
refuses new admission using the existing resource-error path, never drops an
older stream's accepted data to make room. Retirement metadata is reserved at
service construction so last close cannot require an allocation.

The two 64 KiB TCP buffers contribute at most 8 MiB at this bound. Increase
netd's explicit heap to 16 MiB and account separately for 9P buffers, stack
metadata, DNS, and pending accepts. (Done; since B-1c netd's heap grows on
demand (thyla-heap), and the 8 MiB bound stands on its own.) Weft mappings are detached at last clunk
and are not retained for TCP closing. Retired handles belong to either the
NIC SocketSet or the loopback SocketSet; moving or reusing a public slot cannot
change that choice. The single netd event loop serializes all transitions.

The next polling deadline includes retirement expiry even when the network is
idle. Stats distinguish live public connections from retained transports and
count deadline failures. Process death still drops all service-owned state;
this does not promise network delivery after the driver itself dies.

## Verification

1. Reproduce the current short host receipt with the unchanged 8 MiB workload.
2. Verify every byte at the host after immediate client close, on shared INTx,
   ITS and GICv2m; no application sleep or relaxed byte count.
3. Exercise both loopback and NIC close paths, including a peer that sends FIN
   after its receive EOF, unread incoming data, and a close during handshake.
4. Fill the retirement bound, verify new admission refuses without losing an
   older stream, then verify capacity returns after completion or deadline.
5. Prove deadline reaping and last-clunk Weft detach independently; retain
   connection generation and pending-operation cancellation controls.
6. Rerun existing networking, Haul, application and SMP gates. Single-agent
   self-review remains the operator's requested review arrangement.

This refines NET-DESIGN section 3.4 and its last-clunk implementation notes:
connection-number reuse remains a last-clunk action; private transport buffers
may outlive that namespace object under an explicit, bounded owner.

## Integration finding

The first repaired boot passed the deterministic close controls but the
50-dial loopback benchmark exhausted the new transport bound during TIME-WAIT.
Connection-churn probes retry only ENOMEM admission refusals, with a 35-second
deadline and explicit diagnostics; measured dial latency includes all waiting.
The 8 MiB host-receipt test still closes immediately and requires every byte.

The resource refusal also exposed lost open errors: `Dev.open` returns only a
pointer. Dev9p now records its bounded wire errno on the fresh, unpublished
walked Spoor (the existing create-error pattern); SYS_WALK_OPEN and stalk read
it before clunk. Invalid/unspecified and non-9P failures keep EIO. This restores
the existing errno contract without adding a syscall or changing the vtable.

## Measured verification

The final serial CI image passes the host-verified 8 MiB test on GICv2m/HVF,
ITS/TCG and shared INTx/TCG, with 1,570 kernel tests on each boot. Both new
open-errno controls and the netd retirement controls pass. Twelve existing
network/Weft/9P negative model configurations produce their expected
counterexamples. These QEMU tests do not prove behavior under physical network
loss or real hardware interrupts; the retirement deadline remains a failure
bound, not a delivery guarantee.

The full 40-boot default/UBSan by SMP4/SMP8 matrix passes, as does an additional
eight-CPU ITS/TCG UBSan boot. Manual, real-Pi Haul mount/post, hangup and the
DOSBox application gates pass after the repair.

## Refinement (2026-09-21): a TIME-WAIT retiree yields to admission

**The finding.** One boot in roughly eighty died before the login prompt:
`netperf: FAIL -- MW (connect)` -> `joey: /joey exited non-zero` -> EXTINCTION.
The probe's 50-dial churn phase fills the 64-transport bound with retirees, and
the phase after it opened its connection with a plain `connect`, so whether it
was admitted depended on whether a retiree happened to have aged out yet. The
same logs showed the larger cost, on EVERY boot: one of the 50 dials waits
9.81-9.85 s (the first TIME-WAIT expiring), where the mean dial is 2.8 ms.
Every image has booted ten seconds slower since the bound landed, and any
program that opens more than about six short connections a second is refused
for up to ten seconds at a time.

**What a TIME-WAIT retiree still holds.** Nothing this design protects. Both
FINs are exchanged and acknowledged, so there is no accepted byte left to
deliver and no close left to finish; what remains is 2MSL of quiet time that
guards a reused 4-tuple against delayed duplicates of the old connection. The
bounds section above says capacity exhaustion "never drops an older stream's
accepted data to make room". Releasing a TIME-WAIT retiree early drops none.

**The rule.** At the bound, admission first releases the OLDEST retiree that has
reached TIME-WAIT (removed from its SocketSet without an abort, so no RST is
emitted; a late segment for that 4-tuple meets no socket and is answered by the
stack as for any closed port). A retiree in any other state -- data still
queued, a FIN not yet acknowledged, a close in progress -- is never touched. If
no retiree is in TIME-WAIT, admission refuses with ENOMEM exactly as before.
This is the conventional trade (Linux bounds the same table with
`tcp_max_tw_buckets` and drops TIME-WAIT at the limit); the residual risk is a
4-tuple reused inside 2MSL while a duplicate of the old connection is still in
flight, which sequence validation already has to survive after any reboot.

**What does not change.** The 64-transport bound, the 30-second retirement
deadline, the data-preservation rule, and ENOMEM as the admission signal.
Connection-churn callers still retry it; the probe phase that did not now does.
The stats file gains a `timewait-yielded` counter so the pressure is visible.

**Verification.** `close_retirement_selftest` keeps its refusal leg (a bound
full of retirees none of which is in TIME-WAIT still refuses, and the queued
bytes of the real one are intact) and gains the yield leg: with one real
TIME-WAIT retiree among them, admission succeeds, exactly that retiree is gone,
and the retiree holding queued data is untouched. The boot probe's churn phase
is the live witness: its longest dial drops from ~9.8 s to milliseconds.

