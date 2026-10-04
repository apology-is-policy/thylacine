# Two clipboard prerequisites: operator review

October 4, 2026. PROPOSED, not implemented or ratified. Astra, single-agent.

## Recommendation in ordinary language

Extend Loom so an application can start a service connection, keep rendering,
and cancel the whole private connection even if the server stops halfway through
a reply. Cancellation closes that connection locally and frees storage only once
its actual users have finished. It cannot promise that a remote write never
happened. Nora/clipboard then use this general facility instead of accumulating
blocked helper threads. The scope is native /srv services first, not every device
or network mount at once.

Replace128 MiB as a universal shared-mapping limit with budgets derived from the
machine's capacity and explicit accounts. Count a buffer's real RAM once, keep
its allocation charge alive after its creator exits, and separately limit what
another application can keep pinned. A compositor creating buffers for a client
must spend a restricted budget for that session. Warn applications before pressure
becomes critical, reclaim hidden pixels safely, and explain a refusal accurately.

These are established patterns adapted to the existing OS. The specifications
contain primary references and source-level fit; no novelty or formal verification
claim is made. No new names are needed: Loom remains Loom, Burrow remains Burrow.

## Concrete specifications

- [Asynchronous service lifecycle](ASYNC-SERVICE-LIFECYCLE.md): ownership,
  namespace, proposed Loom operations, partial framing, cancellation/retirement,
  backpressure, accounting, C/Rust client contract and AS-0..AS-4 delivery gates.
- [Shared-memory accounting](SHARED-MEMORY-ACCOUNTING.md): physical/sponsor/
  retention/metadata ledgers, account hierarchy and vouchers, backend constraints,
  pressure, precise errors and MM-0..MM-4 migration gates.

## Binding decisions to ratify

1. **Extend Loom with private service scopes.** Terminal cancellation affects
   only a freshly created private connection, never an inherited/shared mount.
   The new private ring initially refuses cross-Proc shared address spaces and
   forbids creating an RFMEM alias while it is live: otherwise a sibling could
   author requests by writing the same ring memory. Ordinary application threads
   still work. Ring-local fids are not exportable in v1. This is narrower than
   a universal async filesystem API and makes the initial authority proof tractable.
2. **Use durable hierarchical memory accounts with restricted server-allocation
   vouchers.** Defaults remain elastic, up to capacity, with aggregate enforcement;
   they are not guaranteed per-user reservations. Orphaned buffers and device pins
   remain charged. Applications receive usage authority, not control over siblings.
3. **Use cooperative pressure recovery and explicit failure.** No automatic
   process killing or forced unmapping. Initial pressure thresholds/headroom are
   tunable 1/8 warning,1/32 critical with 1/64 hysteresis; hierarchy depth 16 bounds
   metadata work. These are transparent proposed defaults, not hardware truths.

The operator has asked for both designs; this document does not mistake that
request for approval of every new ABI/authority/resource-policy term. Repository
DESIGN-FORKS requires user signoff followed by a scripture-only commit before
implementation. On ratification, reserve numeric ABI values with Main/Aux and
commit mirrors before writing consumers. Existing protected authority drafts
remain separate and unchanged.

## Delivery order and cost

First land async semantics/ABI, nonblocking connection progress, retirement and
C/Rust clients. The smaller production clipboard feature can then use it while
memory-accounting work proceeds in shadow mode. Keep the already implemented
hidden-buffer repair and 128 MiB protection until the replacement ledger has been
qualified. Neither side project requires abandoning the modal UI design.

Memory accounting starts with physical backing and durable ownership, then covers
all retaining references and server funding, then pressure and graphics admission.
Only the final switch removes the fixed 128 MiB admission rule. This is broader
than changing a constant: raw DMA currently bypasses the user pool, and a partial
migration could falsely advertise protection while leaving that path open.

Async likewise requires changing progress inside the 9P client; placing today's
blocking handshake in a worker would not deliver the stated cancellation promise.
The contracts intentionally expose those costs rather than hiding them in adapters.
No reliable calendar estimate is asserted before the models/ABI review.

## Single-agent design review

- Kept creation identity distinct from inherited navigation capabilities; no
  global service-name bypass, raw transport authority, or clipboard-specific syscall.
- Found and corrected the RFMEM ring-writer hole in the initial draft. A caller
  PID check cannot identify who changed shared memory; private-mode admission
  restrictions are now explicit rather than claiming impossible isolation.
- Cancellation does not equal target completion or remote rollback. Reserved
  terminal storage makes full-CQ and memory-pressure cancellation possible.
- Local retirement is distinct from SrvConn's final reference: a server holding
  a torn endpoint may retain charge. Retry remains bounded by normal admission.
- Admission and retirement are peer-independent; kernel scheduling/hardware faults
  are not falsely advertised as a hard real-time bound.
- Distinguished actual physical pages, sponsor charges, foreign-retention claims,
  address-space overhead and device aperture. Whole-object pins defeat subrange
  undercharging; immutable object identity defeats wrapper aliases.
- Sponsor death, fork, account closure, quota lowering and backend fences do not
  refund live resources. Parent limits bound descendants without minting budgets.
- Explicitly kept the64 MiB per-object backend envelope separate; removing 128 MiB
  aggregate policy does not establish 4K triple-buffer support.
- New accounts overlap existing per-AS holder budgets by design, not accidental
  double physical charging. Kernel cache sponsorship and raw reserve classification
  need explicit inventories in MM-0, not assumptions based on PRINCIPAL_SYSTEM.
- Unknown numeric ABI fields and allocation sizeof totals are implementation-entry
  checks, not silently allocated identifiers. Tests, models and runtime evidence
  are specified but not claimed run for these proposed systems.

No independent audit is claimed. Before activation, the actual implementation
must pass the adversarial cases and current repository gates in both specs.
