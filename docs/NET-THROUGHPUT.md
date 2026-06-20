# NET-THROUGHPUT — the capability network dataplane (design + the Weft arc)

Status: **DESIGN, arc committed.** 2026-06-20 the user voted *"commit the dataplane
arc now"* — the most ambitious of the three forks this session surfaced. This doc
was a research charter (NP-4 → §"charter"); a five-lineage literature pass this
session (the dataplane-OS world, the capability-µK world, the io_uring/virtio/RDMA
mechanics, the Plan 9 heritage, and a precise grounding of Thylacine's own Loom /
Burrow / netd) turned it into the design below. **No code lands this session** —
this is the scripture commit (the design-conversation → scripture-commit pattern):
the design, the NOVEL angle, the reserved invariant (I-37) + spec, the sequencing,
and the benchmark plan. The build is the **Weft arc** (post-net, before REVENANT).

The user's framing that opened the session:

> A focused, very deep session on throughput, via mechanisms we already have in
> hand (window tuning, Loom amortization), but also zoom out — don't be afraid of
> extra work and research if there's anything in the novel realms that's a natural
> extension of Thylacine's existing mechanisms, or something wholly new that fits
> ideologically or on NOVEL grounds.

The answer the research converged on: **a capability-scoped, zero-copy, batched
network dataplane** — the bytes of a flow travel through a per-flow shared page set
up *once* at grant time, netd drops out of the per-packet loop, and the isolation
is the capability grant itself. The literature confirms the gap is real and the
heritage confirms the move is Plan-9-orthodox. Throughput tracks from **16 MiB/s →
hundreds of MiB/s** without touching the trust boundary, and the same mechanism
closes the latency floor (the RX-wake problem) as a side effect.

---

## 1. The throughput model (code-verified)

The single most useful equation, and it lands exactly on the measurement:

> **stream throughput = TCP window / cross-process round-trip time**
> = 4 KiB / ~250 us = **~16 MiB/s** (M6 NIC, HVF).

But the round-trip is worse than one crossing. There are **two serial byte-copy
crossings** per chunk, and a single-frame-in-flight serialization on the second:

1. **app ↔ kernel** — `SYS_WRITE`/Loom trap; the payload copied `user → out_buf`
   (`kernel/9p_client.c:977`) then `out_buf → SrvConn ring`.
2. **kernel ↔ netd** — the `SrvConn` transport is a **byte-copy ring**
   (`SRVCONN_RING_CAP = 8192`, 2× msize) and the kernel client is **synchronous
   single-frame-in-flight** (holds `p9_client.lock` across send-then-receive); netd
   then copies the bytes *out* of the ring and smoltcp copies them *in* to the
   pre-allocated `SocketBuffer` (`usr/netd/src/server.rs:1834`).
3. plus the **TCP ACK** that reopens the 4 KiB window.

netd is **single-threaded** — one serve loop, no `thread_spawn`; every connection's
I/O serializes through that one round-trip engine.

### What this corrects (the original hypothesis)

The intuition was "the 9P model is chatty and allocates a lot." Verdict:

- **Chatty: yes — and it is the whole story, doubled.** A small window over *two*
  serial cross-process crossings. The ceiling is the bandwidth-delay product.
- **Allocation: no.** The TCP bulk-send path is **alloc-free** per write: the kernel
  client reuses a per-client `out_buf`; netd's `send_slice` copies into a
  **pre-allocated** smoltcp `SocketBuffer`. The allocations that exist (the rx/tx
  socket buffers, the rings) are **per-connection**, amortized over the transfer.
  (UDP/ICMP alloc a packet buffer per datagram; TCP bulk does not.) Drop the
  allocation angle.
- **Copies: ~half a dozen, cheap per byte but they pin the architecture.** user →
  syscall buffer → `out_buf` → ring → netd → smoltcp → frame → NIC, vs the host's
  *one* `memcpy`. At memory bandwidth the copies are microseconds vs the ~250 us
  round-trip — they matter for CPU at high packet rates, not for *this* ceiling.
  They *do* become the bottleneck once the round-trip is removed (Tier C).

### The architectural floor (be honest about it)

The host does loopback at **3156 MiB/s** because it is a monolithic kernel doing
**one in-process `memcpy`** — no process boundary, no round-trip. Ours is the Plan 9
/ capability-microkernel choice: **the stack is a separate Proc reached by 9P.** That
buys the isolation + namespace properties (a confined Proc reaches only the `/net`
its territory grants; a NIC-driver bug cannot corrupt the kernel) — at a per-chunk
cross-process cost a monolithic stack never pays. **We will not reach 3 GiB/s.** Per
VISION §4.5/§6 the committed budget is "throughput at ~70-80% of bare metal, tail
latency not permitted to spike" — and the honest reading of "bare metal" for a
separate-Proc capability stack is *what that architecture can do*, not the host's
in-kernel memcpy. The target is **hundreds of MiB/s + the right architecture**, and
the levers attack the two terms of `window / RTT`: enlarge the window, and *amortize
or eliminate the round-trip and its copies*.

---

## 2. The benchmark baseline (NET-PERF.md §8/§10)

| Path | Throughput | What it isolates |
|---|---|---|
| **M2** loopback bulk (in-guest, 2-thread) | ~50 KiB/s | the loopback-drain POLLOUT *timer* cadence (#221) — a *separate* artifact, NOT the send rate |
| **M6** NIC bulk → host sink (HVF) | **~16 MiB/s** | the real send rate: 4 KiB window / ~250 us, two cross-proc crossings |
| **M6** NIC bulk (TCG) | ~4.8 MiB/s | the same, CPU-slowed |
| **host** loopback (B2) | 3156 MiB/s | monolithic in-kernel memcpy (the unreachable ceiling) |

M2 (50 KiB/s) is **not** the send rate — it is the in-guest loopback drain's POLLOUT
readiness on a ~50-80 ms timer (#221). M6 (host sink draining fast) is the honest
single-stream rate. **Throughput work measures on M6-class paths** (a fast external
drain), not M2 — or fixes #221 first.

---

## 3. The decomposed throughput target (A → B → C)

Modeled on the M6 = 16 MiB/s HVF baseline. Per the no-host-load discipline: these
are **modeled headroom numbers to validate by measurement**, not asserted results —
the benchmark plan (§8) measures each lever.

| Stage | Lever | Mechanism | Modeled headroom | Cost | v1.0? |
|---|---|---|---|---|---|
| Baseline | — | 4 KiB window / ~250 us / 2 crossings | **16 MiB/s** | shipped | — |
| **A** | window + msize | grow the binding 4 KiB → 64 KiB **and** `SRV_MSIZE`/`DATA_CHUNK`/`SRVCONN_RING_CAP`/the kernel-client msize coherently | ~8-16× → **~130-250 MiB/s** | days; audit-light | **yes (v1.0)** |
| **B** | Loom batching + app-side ZC send | amortize the per-op round-trip (N ops per boundary crossing); registered buffers kill the `user→out_buf` copy; the `F_NOTIF` two-CQE send contract | → netd's single-core drain ceiling, **several hundred MiB/s** *if the kernel↔netd path pipelines* | weeks; audit (the buffer-lifetime contract) | Weft arc |
| **C** | the **Weft** shared-page dataplane | a per-flow Burrow shared guest↔netd; netd reads/writes payload **in place**; the second crossing collapses to a **readiness poke** | → netd processing + memory bandwidth, **~GiB/s class** (the transport stops being the bottleneck) | the Weft arc + spec + audit | Weft arc |

**Honest caveats (model, then measure):**

- **A** is the dominant near-free lever, but the round-trip rises a little with bigger
  frames (more copy + more smoltcp work per RPC), so read it conservatively as
  **~130-190 MiB/s** until measured. It is Plan-9-orthodox (msize is *the* heritage
  throughput knob; §4.8) and audit-light (the #841/#845 9P back-pressure invariants +
  the #65 per-Proc memory cap, since bigger per-conn buffers × `MAX_SLOTS`).
- **B**'s ceiling rests on the load-bearing unknown the grounding surfaced: **does the
  kernel↔netd `SrvConn` path pipeline under batching, or re-serialize** (it is
  synchronous single-frame-in-flight today)? Measure before asserting B's number. If
  it re-serializes, B helps the app↔kernel crossing but the second crossing caps it —
  which is exactly the case **C** is built to dissolve.
- **C**'s ~GiB/s is bounded by netd's *single-core* smoltcp, which becomes the new
  bottleneck — a multi-core / per-flow-engine netd is a further v1.x lever (Snap's
  "engine group" model, §4.2).

---

## 4. Literature digest (judged for Thylacine fit)

The mandate was the CLAUDE.md prior-art discipline: Plan 9 heritage first, then the
capability-µK SOTA, then the fit, then the novel angle. Five research passes; every
lineage pointed at the same answer.

### 4.1 The transferability ranking

Ranked by how directly each system's core idea maps onto *Thylacine's exact model* —
a trusted NIC-owning Proc (netd) reached over a capability-scoped shared-memory ring,
**isolation kept in software**:

| Rank | System | The fit |
|---|---|---|
| **1** | **Snap** (Google, SOSP'19) | *Already netd's shape.* App↔Snap = per-app shared-memory **command/completion queues + zero-copy payload regions, bootstrapped by fd-passing** = **Loom (SQ/CQ) + Burrow + the Loom-setup syscall, ~1:1**. No special hardware on that path; protection set up once + per-app regions (our caps make it *stronger*). 38 Gbps/core. MicroQuanta = the "don't burn a core" scheduling policy. |
| **2** | **Shenango / Caladan** (MIT, NSDI'19 / OSDI'20) | *netd generalized.* A trusted Proc polls the NIC and **demuxes into per-consumer shared-memory queues**, with a **single-cache-line readiness poll** and **5 µs core reallocation**. This is *also the latency fix* (the RX-wake floor, §6.4). |
| **3** | **Demikernel** (MSR, SOSP'21) | *Right API, wrong isolation.* PDPIX (`push`/`pop`/`wait`/`qtoken` + buffer-ownership-passes-on-submit) = the exact Loom contract — but it **deletes** the cross-Proc boundary we keep. The cautionary boundary: *Loom already gives us this API; we pay one hop to keep the protection Demikernel sells to hardware.* |
| **4** | **Arrakis** (UW, OSDI'14) | *North-star framing, gated on hardware.* "Check once at grant, flow unmediated" = our capability model in perf terms (filters ≈ namespace grant, rate limiters ≈ quota) — but enforced in **NIC silicon (SR-IOV)**, and it *dissolves* netd. Adopt the framing, not the data path. |
| **5** | **IX** (Stanford/EPFL, OSDI'14) | Run-to-completion + adaptive batching + read-only-mapped RX buffer pool for netd's *internal* loop; the VT-x mechanism doesn't map to ARM64-virtio. |

### 4.2 Snap — the transport template (and the scheduling policy)

Snap is a userspace networking **service process** (exactly netd) reached by: a Unix
socket that **bootstraps shared-memory regions via fd-passing**, then **per-app command
+ completion queues** (spin-poll *or* notify) for ops, and **separate shared regions
for zero-copy request/response payloads**. That maps 1:1: command queue = SQ,
completion queue = CQ, payload regions = registered Burrows, the fd-passing bootstrap =
the Loom-setup/register syscalls. Isolation is enforced **once at setup** + per-app
regions; Snap runs as a non-root reduced-privilege user (our per-Proc capability model
is *stronger*). The one piece to port as policy, not mechanism: **MicroQuanta** — a
bounded high-priority CPU slice so a busy-polling reader doesn't starve the system,
which is the principled version of the #221 POLLOUT-timer / RX-wake pain.

### 4.3 Shenango / Caladan — the demux, and the latency convergence

The **IOKernel** (a trusted Proc on a dedicated core) **polls the NIC and demuxes each
packet into a per-app, per-core shared-memory queue** (MAC-hash → RSS-hash → enqueue),
with the readiness signal read from **a single cache line of shared memory per
consumer**. That *is* netd's RX/TX upgrade: replace the per-op 9P round-trip with
per-Proc Loom rings netd writes RX frames into and drains TX frames from, woken by a
cheap cache-line poke instead of the 50 ms RX-wake. Caladan adds **5 µs
congestion-driven core reallocation + voluntary park** — the principled answer to "does
netd burn a core?" and the convergence point with the *latency* half of this arc (§6.4).

### 4.4 Arrakis — the control/data framing (why it stays isolation-preserving)

"The OS is the control plane": the control plane installs a policy (transmit/receive
filters ≈ the namespace grant; rate limiters ≈ the quota) **once**, then the data flows
unmediated. This is literally Thylacine's capability model stated in performance terms.
Arrakis enforces it in **NIC hardware** (SR-IOV VFs + flow filters + IOMMU) and
*dissolves* netd (each app drives its own VNIC); Thylacine keeps a single NIC-owning
netd (the I-5 property), so it adopts the **framing** — "the capability is the filter,
installed once; netd is the software enforcer Arrakis pushes into the NIC" — not the
data path. (If Thylacine ever runs on SR-IOV hardware, a *capability-scoped VNIC* is a
genuine second NOVEL — recorded.)

### 4.5 Demikernel (the API) + IX (the loop discipline)

Demikernel's **PDPIX** (`push`/`pop`/`wait_any`/`qtoken`, DMA-heap buffers, ownership
passes on `push` and returns on completion) is the cleanest published statement of the
contract Loom should present to a native `libthyla_rs::net` client — **take it verbatim**
— but its in-process, mutually-trusting, hardware-enforced isolation is the precise thing
Thylacine refuses; it is the *cautionary boundary* that names what keeping isolation
costs (one cross-Proc hop). IX contributes netd's **internal loop discipline**:
run-to-completion + adaptive-batching-under-load + a read-only-mapped RX buffer pool.

### 4.6 io_uring / virtio / RDMA — the mechanics

Loom already sits at io_uring's **registered-buffer (`IORING_REGISTER_BUFFERS`) +
multishot** baseline. The three mechanics it lacks, ranked by leverage:

1. **The two-CQE `F_MORE` / `F_NOTIF` zero-copy-send contract.** A ZC send completing
   means only "queued" — the buffer is still in flight (the NIC may DMA from it; for TCP
   it is pinned until the peer ACKs). So the send posts a **result CQE** (`F_MORE`) and a
   later **notification CQE** (`F_NOTIF`) that says "buffer reusable." **The I-30 pin
   must release at *notification*-terminal, not op-terminal** — gated on the **last** of
   {netd stack done, NIC DMA done, peer ACK}. (`IORING_SEND_ZC_REPORT_USAGE` gives a
   fallback-copied indicator so netd can transparently fall back to a copy and *say so* —
   never silently-wrong.)
2. **A dedicated RX refill ring** (zcrx's clean ownership state machine): the CQE carries
   the chosen buffer id / offset, *not* the bytes; userspace returns consumed buffers via
   a separate refill ring. Cleaner than overloading the SQ.
3. **A virtio-style in-place descriptor ring** to replace the byte-copy hop to netd:
   `{burrow-relative addr, len, flags}` entries; the **split-ring unidirectional-region**
   discipline (each region written by exactly one side) buys lock-free SMP correctness for
   free. `used.len` = the RX short-read signal; indirect descriptors = scatter-gather batch.

**RDMA is the production proof of the framing:** memory registration mints an `rkey` that
**is** a zero-copy access capability — rights fixed at registration, monotonic, validated
once then ops flow unmediated, the Protection Domain = the isolation boundary. That is
**literally I-30** ("resolve + snapshot at submit, never re-resolve at completion"), with
the PD = the Territory/Loom domain. The single bug the whole design must prevent: **a
buffer reused before the data plane is done** — and for Loom "done" is the *last* of
{netd, NIC DMA, peer ACK}, the buffer-lifetime analog of Loom's borrow-guard UAF class.

### 4.7 The capability-µK scan + the NOVEL-gap verdict

Decompose the thesis into four properties and score every capability OS:

| System | zero-copy ring | no per-op stack mediation | per-**flow** grain | **software** grant = the dataplane setup |
|---|---|---|---|---|
| Fuchsia Fast-UDP | ✗ (copies) | ✗ (netstack drains every pkt) | ~socket | ✗ (zx_socket is a mediated pipe) |
| **Fuchsia IOBuffer** (RFC-0218) | ✓ | ✓ (direct-map mode) | n/a | ✓ — **but logging/tracing only, never networking** |
| seL4 sDDF / LionsOS | ✓ | ✗ (a virtualizer routes every pkt) | per-NIC raw frames | partial (per-NIC, static) |
| Genode packet_stream | ✓ | ✗ (nic_router routes every pkt) | per-NIC "virtual cable" | partial (per-session) |
| Arrakis | ✓ | ✓ | per-flow | ✗ — **NIC hardware (SR-IOV), no capability/namespace model** |
| Snap | ✓ | ✗ (the engine runs the transport) | flow→core (HW steer) | ✗ (engine-mediated) |

**The gap is real: no system is all four.** The closest *primitive* — **Fuchsia's
IOBuffer** ("the capability grant IS the zero-copy peered ring with optional
no-mediation") — exists but is scoped to **logging/tracing, never wired to the netstack**
(had they built sockets on it, the novelty would be largely refuted; they did not). The
closest *property-set* — **Arrakis** — is **NIC-hardware-enforced, no capability/namespace
abstraction at all**. Thylacine's genuine novelty is the **fusion**:

> **per-flow** (vs everyone-else's per-NIC) **×** **software capability enforcement** (vs
> Arrakis/Snap's NIC hardware) **×** **no per-op stack mediation** (vs sDDF/Genode/Fuchsia/
> Snap's per-packet router/engine/drain) **×** **the grant-is-the-setup primitive applied to
> networking** (which only IOBuffer embodies, and only for logging).

Two honesty caveats the design must carry (and answer): (1) the closest *primitive*
(IOBuffer) and the closest *property-set* (Arrakis) both exist — the contribution is the
*fusion + the software-only/per-flow framing*, not any single ingredient; (2) **"no per-op
mediation while per-flow in software" is the hardest property to keep** — the moment netd
must demux/police a shared NIC across flows without hardware filters, a software toucher
tends to creep back into the path (exactly what sDDF's virtualizer and Genode's nic_router
are). The design must *show* how the flow capability + Loom's submit-time pin + Burrow keep
enforcement at setup-time and at the capability boundary, not per-packet. **That is the
actual novel engineering, and the thing reviewers will probe** (§6.3).

### 4.8 The Plan 9 heritage verdict (it is lineage-correct)

A shared-page / zero-copy 9P **data** transport — where the Tread/Twrite *payload* moves
through a shared page while the 9P **control** messages stay ordinary marshalled T/R
frames — is **lineage-correct**, and it is the modern Plan 9 lineage's *standard* fast
local-9P transport, not a deviation:

- **Direct shipping precedent: virtio-9p** (Linux v9fs + QEMU, since 2007). 9P control
  message kept as a small inline frame; payload **≥ 1024 B moved through pinned shared
  pages** in a virtqueue ("the actual content is passed in zero-copy fashion" —
  `p9_client_zc_rpc`), with a **hybrid `< 1024 B` copy threshold**. `trans_rdma` is a
  *second* zero-copy-payload-under-9P transport in the same tree. Thylacine's guest↔netd
  boundary is the *same* paravirtual local boundary virtio-9p was built for.
- **9P is transport-agnostic by design** — its self-delimiting T/R framing has carried over
  TCP, IL, fd/pipe, virtio, and RDMA with byte-identical semantics. Optimizing only the
  byte-movement *under* 9P keeps the file abstraction mediating every access — faithful,
  not a violation. (A violation would be a side-channel buffer read/written *without* a
  Tread/Twrite; Weft does the opposite — the Tread/Twrite still happens, still carries the
  count, still returns through the reply; only the payload bytes travel through the page.)
- **The lineage *builds* custom 9P transports deliberately** — IL was "our protocol of
  choice," retired only for **WAN** reasons (long-distance, asymmetric) irrelevant to a
  same-host hop.

**Two heritage-grounded design rules** fall out: **keep the hybrid threshold** (small /
control payloads stay on the existing byte-copy ring — the orthodox Plan 9 baseline; the
shared page is for large Tread/Twrite payloads only), and **msize is the cheaper, composing
lever** — Tier A (raise msize) is the orthodox first move, the shared page is the deeper
one, and virtio-9p needed *both*.

---

## 5. The committed design: the Weft capability dataplane

**Weft** (ratified 2026-06-20 — the crosswise thread the shuttle carries through the warp
on a loom; here, the zero-copy data woven *through* Loom. Loom is the async control ring;
Weft is the zero-copy data path through it.)

### 5.1 The thesis (the precise NOVEL claim)

> A **purely-software, hardware-independent, per-flow capability-scoped zero-copy network
> dataplane**: granting a Proc its flow capability (a `/net/<proto>/N/data` fid) *also*
> establishes a per-flow shared-page ring between that Proc and netd; the flow's payload
> bytes then travel through the shared page with **no per-operation mediation by netd**;
> netd does the control-plane setup (the capability check, the flow grant, the policy) and
> drops out of the per-packet loop. **Isolation is the capability grant; speed is the
> absence of per-op mediation.**

This is the fusion of Snap's transport + Shenango's demux/scheduling, expressed in
**Loom + Burrow**, with **Arrakis's "check-once-at-grant" framing** describing *why* it
stays isolation-preserving, **Demikernel's API** at the native client, and **RDMA's
registration-is-the-capability** validating the I-30 conviction. The gap (§4.7) is real;
this is a NOVEL.md angle.

### 5.2 The mechanism

Three pieces, all extensions of existing Thylacine substrate (Loom rings, Burrow VMOs, the
I-30 submit-pin, the audited 9P client):

1. **A per-flow shared-page payload transport** (the virtio split-ring model, §4.6). A
   Burrow is shared guest↔netd via a **cross-Proc Burrow-share surface** (the missing
   primitive — the substrate exists: a Burrow already maps into two Procs via `burrow_map`,
   #847 dual-refcount keeps the pages alive while either maps; only the inter-Proc *share*
   syscall must be built). The 9P **control** messages (Twrite/Rwrite/Tread/Rread) stay on
   the existing path; the **payload** for a large Tread/Twrite travels through the shared
   page, netd reading/writing it **in place** — no copy out of the ring. **Hybrid
   threshold** (§4.8): `< threshold` payloads keep the byte-copy ring.
2. **A readiness ring** (the Shenango single-cache-line poke, §4.3). netd writes
   RX-readiness / TX-completion into a per-flow shared cache line the guest's Loom CQ /
   elected reader observes — replacing the 50 ms RX-wake poll. **This is the latency fix**
   (§6.4): the same shared-ring mechanism that removes the throughput round-trip removes the
   latency floor.
3. **The `F_NOTIF` zero-copy-send completion contract** (§4.6). A Weft send posts a result
   CQE then a notification CQE; the I-30 pin releases at notification-terminal — the **last**
   of {netd stack done, NIC DMA done, peer ACK} — so the page is never reused in flight.

### 5.3 Why it stays isolation-preserving (the reviewer attack, answered)

The reviewer attack (§4.7): *"no per-op mediation while per-flow in software" — a software
toucher creeps back.* The answer Weft must demonstrate:

- **The grant is per-flow and set up once.** The flow capability is the
  `/net/<proto>/N/data` fid — already a per-flow, namespace-scoped, capability-checked
  object (I-1/I-28: a Proc reaches only the `/net` its territory grants). Establishing Weft
  for that flow is one control-plane operation, gated by the existing dev9p `perm_check` +
  the I-30 submit-pin; the ring is bound to *that* fid's pin.
- **netd is the control plane, not a per-packet toucher.** netd sets up the ring at grant,
  installs the smoltcp socket, and thereafter the guest's payload lands in the shared page
  and netd's smoltcp reads it in place when it next polls — netd does **not** re-check a
  capability or copy per packet. The "demux across flows" that forces a software toucher in
  sDDF/Genode is *already done* by the per-flow grant: each flow has its **own** ring and
  its **own** smoltcp socket, so there is no shared queue to police. (smoltcp's
  per-connection socket *is* the per-flow demux, done at the protocol layer, not a capability
  re-check.)
- **The kernel is the validator-once, like RDMA's HCA** (§4.6). The kernel copies every SQE
  field before acting (I-30 ring-TOCTOU), resolves + pins the Burrow + the fid **once** at
  submit, and never re-reads a shared-ring field after the check. The shared page is reachable
  only within the owning flow's pin — the PD analog.
- **The trust boundary is unchanged.** netd still owns the NIC (I-5); a confined Proc still
  reaches only its granted flows (I-1/I-28); a buggy guest corrupts only its own ring view
  (the kernel validates everything). The cross-Proc hop is *not removed* — its *per-op cost*
  is removed; the isolation it buys is intact.

### 5.4 The latency convergence (this subsumes NET-PERF N1)

The net-optimization arc has two halves: **latency** (the RX-wake floor — NET-PERF N1, the
"netd parks for up to 50 ms because there is no pollable NIC-IRQ fd") and **throughput**
(this doc). They converge on the **same mechanism**: Weft's readiness ring (§5.2-2) is the
Shenango single-cache-line readiness poke — netd signals RX-ready over a shared line the
guest observes immediately, so a reply that lands while the guest is parked wakes it at
once. **Weft's data path removes the throughput round-trip; Weft's readiness ring removes
the latency floor.** So the dataplane arc closes both halves — the ideologically-coherent
end-state. (The separate "Tier 0/1/2 latency fork" the earlier session left open is resolved
*into* Weft: the pollable-readiness mechanism is the readiness ring, not a bolt-on irq-fd.)

### 5.5 Invariant I-37 + the spec reservation

**I-37 — Capability network dataplane integrity** (designed; OWED at the Weft build). A
per-flow zero-copy dataplane is sound iff: **(1)** the access bound is enforced **entirely
at setup/grant time** (the flow capability = the `/net` data fid's I-30 submit-pin + the
Burrow registration), **never per-packet**, and netd is out of the per-packet path (no
software toucher re-enters — §5.3); **(2)** the shared payload page's lifetime is correct
under the **multi-holder completion** — a registered buffer is not reused/freed until the
**last** of {netd stack done, NIC DMA done, peer ACK} releases it (the two-CQE `F_NOTIF`
contract; pin released at notification-terminal, not op-terminal) — no in-flight-page UAF /
cross-Proc corruption; **(3)** a confined Proc reaches only the flows its namespace grants,
the ring is per-flow (I-1/I-28), and netd owns the NIC (I-5 — the ring is the only data
path, never raw hardware); **(4)** the shared Burrow's lifetime is the #847 dual-refcount
(I-7); **(5)** the descriptor ring is **split-ring unidirectional** (one writer per region)
so SMP correctness holds without a per-op lock. **Generalizes I-29/I-30** (Loom completion
integrity + submit-time pin) to the cross-Proc shared buffer + the notification-terminal
release.

**Spec-first RE-ENABLED for this surface** (the fifth instance of re-enabling point (a); the
io_uring `ubuf_info` buffer-lifetime race is the famous, subtle class that benefits from
machine-checked exploration). **Model-first**: `specs/weft.tla` (clean +
buggy cfgs: `premature_release` [the F_NOTIF UAF — pin released at op-terminal not
notification-terminal], `recheck_per_op` [a per-packet capability re-check — the reviewer
attack, which must *fail* the no-mediation invariant if present], `ring_toctou` [a re-read
shared-ring field], `share_outlives_flow` [the Burrow surviving the flow's teardown]) is
written + TLC-green **before** the impl.

---

## 6. The Weft build sub-arc (post-net, before REVENANT)

One Tier-A piece lands first as a v1.0 win (independent of the dataplane); the rest is the
dataplane arc the user committed.

- **Weft-0 (Tier A, v1.0 — the cheap win, lands first).** Grow `TCP_TX_BUF`/`TCP_RX_BUF`
  (4 KiB → 64 KiB), `SRV_MSIZE`/`DATA_CHUNK`, `SRVCONN_RING_CAP` + `SRVCONN_MSIZE`, and the
  kernel-client msize — coherently. Measure on M6. Audit-light (back-pressure #841/#845 + the
  #65 per-Proc memory cap with bigger per-conn buffers). *Independent of Weft-1+; can land
  any time.*
- **Weft-1 (spec-first).** `specs/weft.tla` — model the shared-page transport + the
  `F_NOTIF` multi-holder buffer lifetime + the enforcement-at-setup / no-per-op-recheck +
  the split-ring discipline. Clean + the four buggy cfgs. Model-first, before any impl.
- **Weft-2 (the cross-Proc Burrow-share primitive).** The missing inter-Proc Burrow-share
  surface (the substrate exists; only the share syscall + the lifetime audit are new) — a
  Burrow mapped into both the guest and netd, #847 dual-refcount, I-5/I-1 gated.
- **Weft-3 (the descriptor ring + in-place payload).** The virtio split-ring between guest
  and netd; netd maps + reads/writes payload in place; the hybrid `< threshold` fallback to
  the byte-copy ring. Wire it under the existing 9P Tread/Twrite (the heritage split, §4.8).
- **Weft-4 (the readiness ring + the latency convergence).** The single-cache-line readiness
  poke; retire the 50 ms RX-wake; close NET-PERF N1.
- **Weft-5 (the `F_NOTIF` zero-copy-send contract).** The two-CQE send completion; the I-30
  pin released at notification-terminal; the fallback-copied indicator.
- **Weft-6 (the per-flow capability binding + the native API).** Bind the ring setup to the
  `/net` data fid's I-30 pin (the "grant *is* the dataplane" realization); the
  `libthyla_rs::net` native client over the Demikernel-shaped API (`push`/`pop`/`wait`).
- **Weft-7 (the focused audit + SMP gate + benchmark).** Prosecute the buffer-lifetime UAF
  (the §4.6 hazard, the F1-class), the no-per-op-mediation property, the cross-Proc Burrow
  lifetime; the throughput measurement (§8); docs.

---

## 7. The benchmark plan

Extend `usr/netperf nic` + `tools/np3-bench.sh` so each lever is **measured, not asserted**
(the NET-PERF M6 path is the harness):

1. **A large-transfer mode** in `netperf nic` (a multi-MiB stream to the host sink) reporting
   steady-state MiB/s — so a window/msize change shows a throughput delta, not just a per-op
   RTT delta.
2. **A window/msize sweep** (a `netperf nic --window N --msize M` matrix) measured on M6 (TCG
   + HVF) — validates the Tier A ~8-16× model and finds the knee.
3. **The kernel↔netd pipelining probe** (the load-bearing §3 unknown): a batched Loom-write
   stream vs serial, measuring whether the second crossing pipelines — *measure before
   asserting Tier B's number.*
4. **The Weft delta**: same large-transfer with the shared-page path on vs off (the hybrid
   threshold toggled), on M6, to measure C's headroom against A+B.
5. **The latency convergence check**: M6-rtt with the readiness ring on vs the 50 ms RX-wake
   off — confirms Weft-4 closes N1.

Per the no-host-load discipline: every timing boot is ground-truthed to the healthy guest
end-state; a nondeterministic result is a guest race to hunt, never "host load."

---

## 8. Sequencing

This is the **net-optimization arc** (ROADMAP: between **net-8** [done] and **REVENANT**
[#231]). The two halves both resolve into Weft: **Weft-0** (Tier A) is the immediate v1.0
window win; **Weft-1..7** is the dataplane that subsumes both throughput *and* the latency
floor (§5.4). "On par with the host" is achievable for **latency** (the readiness ring →
~0 wake, like the host) but **not literally** for **throughput** (separate-Proc 9P vs
in-kernel memcpy) — the goal is "hundreds of MiB/s + the right architecture," which is
exactly why it earns a deep, research-grounded arc rather than a window bump.

Cross-refs: `docs/NET-PERF.md` (the measured baseline + the latency arc N1), `docs/LOOM.md`
+ `docs/reference/107-loom.md` (the async ring + registered buffers + the I-30 pin),
`docs/NET-DESIGN.md` §12 (the committed "net rides Loom"), `docs/NOVEL.md` (the Weft angle),
`ARCHITECTURE.md` §28 I-37, RW-11 #62 (the v1.x perf backlog). The build tasks are #265's
children.
