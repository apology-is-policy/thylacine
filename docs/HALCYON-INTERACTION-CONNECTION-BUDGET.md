# Halcyon connection admission: resource-policy review

Status: APPROVED by the operator, October 3, 2026 (option A).
Implementation pending; no quota or endpoint has changed yet.
Source baseline: Astra e61fced0163706822ee3b11e3f072dfd4eca6222.
This ratifies the numeric resource amendment to SRV-SESSION-REGISTRY-DESIGN. Registry ownership, peer identity and privileges stay
as ratified. Evidence lives in work/oct2-hi-budget/.

## Problem and measured costs

HALCYON-INTERACTION section 8 reserves 32 persistent controller connections,
two media connections and four handshakes. D7 subsequently ratified only 16
connections per session, 48 across sessions and 64 globally. Even the 38-slot
service alone exceeds the session limit. Direct graphical clients additionally
open their own EventRing connection to Tapestry; sharing Halcyon's connection
with unrelated applications would erase the kernel peer distinction.

A CPU1 graphical boot of the already-qualified paired image, with the welcome
and shell tiles ready, reports session=6, guests=6, all=17, domains=1 in
/ctl/9p-sessions. See baseline.png. This is one startup observation, not a
32-application or multi-user qualification. The diagnostic's bounded output
can truncate details; row count is not the admission count.

Actual ARM64 compiler layout, using the current headers and kernel target:

| Object | sizeof, bytes | Allocation observation |
| --- | ---: | --- |
| SrvConn | 312 | 512-byte kmalloc size class |
| p9_client | 41,856 | 64 KiB buddy allocation |
| p9_attached | 128 | 128-byte kmalloc size class |
| p9_srvconn_transport | 16 | 16-byte kmalloc size class |
| Spoor | 88 | excludes its separately owned data |
| p9_rpc | 72 | synchronous instance is stack-local; reply buffer separate |

These are compiled sizes, not allocator occupancy measurements. Slab pages,
fragmentation and other retained objects are not included in the size-class
figures. mm/slub.c rounds large allocations to a power-of-two page count.

| Per connection | Default msize 32 KiB | Bulk msize 128 KiB |
| --- | ---: | ---: |
| Two transport rings, each 2 x msize | 128 KiB | 512 KiB |
| Kernel p9_client allocation, when attached | 64 KiB | 64 KiB |
| Attached receive buffer | 32 KiB | 128 KiB |
| Additional outbound buffer (default is inline) | 0 | 128 KiB |
| Core subtotal, excluding small metadata | 224 KiB | 832 KiB |

This is NOT total connection memory. Separately count retained last replies,
in-flight RPC replies and spill buffers, fids/Spoors, deferred clunks, poll
state, server buffers and allocator overhead in the activation ledger. The
kernel's 64 outstanding tags permit much more transient memory than one idle
client; ring admission must not be advertised as a complete kernel-memory cap.
The existing 7.375 MiB HI payload/buffer ceiling is userspace-only and does not
include any of these kernel allocations.

## Alternatives and recommendation

1. **Weighted admission (recommended).** Keep the current maximum global ring
   allocation while allowing more small connections. Use the existing immutable
   service class and existing requesting-domain lifetime to charge resources.
   Changes the session's share and the small-connection count deliberately.
2. **Raise raw connection counts to 96/192/256.** Simpler counters, but allows
   48/96/128 MiB of rings per session/combined/globally when all are bulk.
   The global ring bound quadruples without helping clipboard traffic.
3. **Keep 16/48/64 and reduce persistent-client support.** Preserves the current
   admission policy, but cannot deliver the approved 32-controller reservation.
   Reconnecting on each operation also requires a different registration and
   foreground-lifetime protocol; it is not an adapter-only optimization.

Prior art informs the accounting boundary, not the proposed numerical values:

- [Plan 9 srv](https://9p.io/magic/man2html/3/srv) publishes references to already
  open channels. Thylacine instead mints a fresh, peer-stamped transport on a
  connect, so reusing the posted channel is not a compatible shortcut.
- [Genode resource trading](https://genode.org/documentation/genode-foundations/25.05/architecture/Resource_trading.html)
  charges session resources through explicit quota transfers. The useful
  principle here is charging the requester for retained service resources.
  Thylacine already has SrvDomain; this proposal does not add a donation system.
- [seL4 untyped memory](https://docs.sel4.systems/Tutorials/untyped.html) makes
  allocation authority explicit through untyped capabilities and retyping.
  Thylacine uses dynamic kernel allocation, so a bounded admission reservation
  is a better fit here than introducing an unrelated allocator capability ABI.

## Ratified contract

One credit covers 128 KiB of ring storage. Default-class connections cost one
credit; bulk-class connections cost four. Validate the immutable msize before
reservation; user-requested protocol negotiation cannot reduce that charge.

| Scope | Credit bound | Maximum default connections | Maximum bulk connections | Ring bytes |
| --- | ---: | ---: | ---: | ---: |
| One session domain | 96 | 96 | 24 | 12 MiB |
| Session domains combined | 192 | 192 | 48 | 24 MiB |
| Global, including boot | 256 | 256 | 64 | 32 MiB |

Each connection costs at least one credit, so these also bound object counts.
Keep live connection counters separately for diagnostics; do not relabel credits
as connections. Retained domains remain 16 and private names remain 16 per
registry. Boot borrows unused global capacity. Sessions cannot consume the
64-credit margin, enough ring capacity for 16 bulk or 64 default connections.
This is protection against session consumption, not a guarantee against boot
exhausting its own margin.

One session's ring share rises from 8 to 12 MiB; its maximum bulk count rises
from 16 to 24. Two sessions can consume the combined ring partition. This is
an explicit fairness tradeoff, not a guarantee of full desktops for every one
of the 16 retained domains. Saturation still returns prompt ENOSPC; no waiting,
eviction, priority bypass or new authority is introduced.

The 38 HI connections plus up to 32 application EventRings consume 70 credits
if all default class, leaving 26 credits for startup/resident/other demand.
The observed six startup connections are not assumed to cost six credits:
bulk connections cost four. A mixed-class native acceptance test must establish
the actual fit. Additional application-specific services can still exhaust it.

The ring maximum stays 32 MiB globally and 24 MiB for sessions combined. The
maximum core-buffer subtotal above changes from 52 MiB (64 bulk attachments)
to 56 MiB (256 default attachments), plus increased small metadata and other
excluded costs. Thus this is NOT a claim of unchanged total memory. Final
activation still requires the complete HI allocation ledger and native demand
checks; failed allocation remains possible below quota.

Reserve global, combined-session and domain credits atomically under the
existing admission lock, before any connection allocation. Capture the charge
on the minted connection and return that exact charge after final storage
destruction. Construction in flight, torn connections, poll retainers and 9P
attachments continue to hold it. Rollback returns all counters once. No
allocation, wakeup or final domain destruction occurs under admission lock.
Changing namespace or server identity never transfers the charge.

Diagnostics must report count and credit usage/limits distinctly. Provide a
bounded summary before detail rows so truncation cannot hide exhaustion; keep
the existing lock order and expose no new credentials or memory addresses.
Kernel limits do not reserve service slots: Halcyon's separate 32/2/4 pool and
per-peer handshake restrictions remain mandatory.

## Implementation and acceptance

First commit the ratified design changes, then implement weighted admission
and diagnostics in a separate commit. Preserve all four unrelated drafts.

- Extend the actual-source admission fixture for mixed weights, all three
  scopes, boot margin, invalid class, every allocation rollback, retained
  teardown, domain retirement and deterministic constructor interleavings.
  Mutants must catch unit-weight bulk charging, early return of credit,
  incomplete rollback and failure to enforce the combined partition.
- Use compiled ABI/layout checks and real CPU1 kernel fixtures. Exercise
  default-heavy and mixed bulk/default admission and complete recovery after
  close. October 1-2 SMP/sanitizer/50-boot waiver applies only within its dates.
- Measure startup, 32-controller/graphical demand, a second session, media and
  handshake pressure, SAK cancellation and cleanup using real peer connections
  as the application adapter becomes available. Do not present a synthetic
  connection test as completed clipboard or modal-UI qualification.
- Keep clipboard disabled until authenticated dispatch, exact peer checks,
  cancellation-before-HSC-ACK, allocation accounting and real two-client
  copy/paste checks are complete. No Main landing is implied.

## Measurement caveats

The first sampling script tried redirecting to /dev/cons from a non-console
graphical process. The kernel correctly refuses that; the script timed out.
The corrected command renders its output normally in the tile and exits zero.
Its capture is readable and the Expect log reports clean EOF. The outer Python
wrapper then hit PermissionError by signalling an already-exited process group.
Process inspection confirmed no remaining VM or wrapper, and the lease was
released by finally. The wrapper is corrected for future runs; this is recorded
as a measured screenshot with a cleanup-script failure, not a passing automated
runtime gate. No source or image rebuild was needed for these measurements.
