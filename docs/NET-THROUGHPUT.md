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
   (`SRVCONN_RING_CAP` = 8192 pre-Weft-0, now 65536 = 2× msize) and the kernel client is **synchronous
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

### 5.4 The latency picture (Weft's guest-wake half vs NET-PERF N1's netd-notice floor)

The net-optimization arc has two halves: **latency** (NET-PERF N1) and **throughput** (this
doc). N1 is precise (NET-PERF §2.1, ground-truthed by M6): the measured ~50 ms floor is
**netd's own poll timeout** — netd cannot wait on the NIC IRQ and the 9P request fds together
(`SYS_IRQ_WAIT` blocks; it is not pollable), so it polls the NIC on a ~50 ms timer and an RX
frame waits up to that long before netd even *notices* it. That floor is on the
**netd-notice** side, and the guest-facing readiness ring does **not** move it — netd still
polls the NIC the same way.

What Weft's readiness ring removes is the **second** crossing — the **guest-wake** round-trip.
Once netd *has* the bytes, a native client observes RX-readiness over the shared cache line
(the Shenango poke, §5.2-2) instead of a 9P reply round-trip + an elected-reader wake: a reply
that lands while the client busy-polls is seen at memory speed, and a parked client is woken by
its Loom CQ completion. So **Weft's data path removes the throughput round-trip and its
readiness ring removes the guest-wake round-trip — both real, both on the *guest* side.**

The netd-notice floor (the measured 50 ms) is closed by the **orthogonal** pollable
NIC-IRQ-readiness fd (NET-PERF's "#1 lever"): a small kernel ABI making netd's NIC IRQ
pollable, so its `t_poll` wakes on a NIC RX *or* a 9P request — a **separate net-perf chunk,
not part of the Weft dataplane.** (So the earlier "Tier 0/1/2 latency fork" resolves to *two
complementary mechanisms*, not one: the readiness ring is the guest-side wake; the NIC-IRQ fd
is the netd-side wake. The honest claim is that Weft closes the guest-side latency it owns —
not that it subsumes N1's netd-notice floor.)

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

- **Weft-0 (Tier A, v1.0 — the cheap win) — LANDED.** Grew, coherently: `TCP_TX_BUF`/
  `TCP_RX_BUF` 4 KiB → **64 KiB** (the window), and `SRV_MSIZE`/`DATA_CHUNK`/`SRVCONN_MSIZE`/
  `SRVCONN_RING_CAP`/the kernel-client `P9_CLIENT_OUT_BUF_MAX` to a **32 KiB** per-op payload
  (an 8× reduction in cross-process crossings per MiB). The msize ceiling is **32 KiB, not the
  64 KiB the §3 table models**: `SRVCONN_RING_CAP` is an inline `buf[]` (×2: c2s + s2c) in
  `struct SrvConn`, so the ring scales the struct (2× msize → a ~129 KiB kmalloc at 32 KiB —
  order-6, but allocated at connection setup [mostly boot, unfragmented] and graceful-fail on
  OOM); the 64 KiB+ payload ceiling comes from the **Weft shared-page dataplane** (Weft-3),
  which retires the byte-copy ring entirely — so growing the inline ring further is throwaway
  work. corvus (the other SrvConn service) is unaffected: it negotiates 9P `min()` down to its
  4 KiB `SERVER_MSIZE`. The netd per-op recv scratch moved stack → heap (a 32 KiB stack array
  would overflow netd's 256 KiB stack and terminate the NIC owner). Audit-light: no new
  invariant; the #841/#845 back-pressure + the #65 memory cap (netd's buffers are ~2 MiB ≪ the
  256 MiB floor) hold; the SrvConn 2× ratio is preserved so the Loom async pipelining depth is
  unchanged. Proven: build + boot (933/933 + the live `net-8b` /net E2E + `loom-stress` dev9p
  round-trip + 0 EXTINCTION) + the SMP gate (0 corruption, 40 boots). The win is **modeled**
  (8× → ~128 MiB/s, Tier A's low end) and proven to work end-to-end by the live `net-8b` E2E
  (a real /net round-trip at the 32 KiB msize / 64 KiB window); the clean **empirical**
  large-transfer measurement is owed to the Weft-7 benchmark — the 32 KiB M6 probe over slirp
  is too small to isolate a steady-state rate, and the slirp guestfwd that feeds it is inert
  under HVF on the dev host (the probe SKIPs gracefully; a measurement-infra gap, not a guest
  fault — the NIC itself works: DHCP lease + 24/24 ARP each boot). *Independent of Weft-1+.*
- **Weft-1 (spec-first) — LANDED.** `specs/weft.tla` (model-first, TLC-green BEFORE any
  impl). One module pins I-37 — the Loom I-29/I-30 pin GENERALIZED to the cross-Proc shared
  page + the notification-terminal release — over a single flow (`active`→`torndown`) + N
  registered payload pages, with the F_NOTIF holder set {netd,nic,ack}, the #847 share refs
  {guest,netd}, and the flow cap pinned-at-grant vs the live (rebindable) binding. 13 safety
  invariants + the `EventuallyReleased` liveness witness. Six cfgs: `weft.cfg` (clean,
  TLC-green, 1412 distinct, depth 22), `weft_liveness.cfg` (green), and the **four named
  buggy cfgs**, each a short executable counterexample on its named invariant —
  `premature_release` → `PinHeldWhileInFlight` (the F_NOTIF UAF: pin dropped at op-terminal
  with {nic,ack} pending), `recheck_per_op` → `NoPerOpMediation` (the reviewer-attack per-op
  cap re-resolve, which also breaks `ActedUnderFlowPin` under a rebind), `ring_toctou` →
  `DescPinnedToSnapshot` (mutate-after-consume), `share_outlives_flow` → `ShareBoundedByFlow`
  (teardown leaves a #847 ref). The four buggy cfgs are the durable Weft-7-audit regressions.
  Model maps to the planned impl sites (`specs/SPEC-TO-CODE.md::weft.tla`); the impl is OWED
  across Weft-2..7.
- **Weft-2 (the cross-Proc Burrow-share substrate) — LANDED.** The kernel mechanism a per-flow
  shared page rests on: one Burrow mapped into *two* Procs (guest + netd) with the #847
  dual-refcount (I-7) holding it alive while *either* maps it and freeing it when *both*
  drop. The map mechanism already exists — `burrow_map(struct Proc *p, ...)` takes an
  explicit `p` and the #847 refcount is intrinsically SMP-safe — but **no Burrow has ever
  been reachable from two Procs**, so the load-bearing new work is the proof that the
  dual-refcount is sound under two Procs *concurrently* mapping/unmapping one Burrow (the
  SMP-safety the whole dataplane assumes), landed as a kernel unit test + a focused
  reasoning pass, plus a thin `burrow_share_into(dst, v, ...)` helper. **No EL0 ABI at
  Weft-2** (see the delivery model below) — this is the substrate; the EL0 surface is
  inherently per-flow and lands at Weft-6. Validates `weft.tla` Init (the share mapped into
  both via the dual ref) + Teardown (drop both → free).

  *As-built:* `kernel/burrow.c::burrow_share_into(dst, v, vaddr, prot)` maps the WHOLE ANON
  Burrow (`length = v->size`; ANON-only — cross-Proc MMIO/DMA is a distinct unaudited surface,
  fail-closed) into a second Proc via `burrow_map`, with the cross-Proc #847 proof + lock-order
  reasoning in the function header and `docs/reference/20-burrow.md`. 4 kernel unit tests
  (`burrow.share_into_{cross_proc,alive_while_either_maps,frees_on_last_drop,constraints}`)
  exercise the two-Proc reachability, both teardown orders (free iff ALL refs drop), the
  mapping-only liveness (grant-is-the-share, h==0), and the W^X/overlap rejects — 937/937, boot
  OK. The `burrow.tla` spec gate re-ran green (clean + the 3 buggy cfgs each still violate),
  confirming Weft-2 extends the modeled #847 mechanism without changing it. No EL0 ABI.

  **The delivery model — grant-is-the-share (user-voted 2026-06-20).** Opening the flow's
  `/net/<proto>/N/data` fid maps the per-flow shared Burrow into the guest; **no Burrow
  handle ever crosses Procs** — the kernel maps both sides, the #847 refs are kernel-
  internal, and the capability *is* holding the namespace-gated flow fid (I-1/I-28). This
  is the convergent answer — Plan 9 (mmap the server-backed file), Fuchsia IOBuffer
  (RFC-0218, "the grant *is* the peered ring"), seL4 (a coordinator maps both address
  spaces) — and the one where the spec's `ShareBoundedByFlow` / `NoStaleShareAccess` are
  easiest to uphold: there is no free-floating, dup-able Burrow handle to leak, retain past
  the flow, or mis-target. **Rejected:** *capability delegation* (the Burrow handle crosses
  netd→guest, guest `SYS_BURROW_ATTACH`es it — reuses the audited attach, but opens the
  deferred I-4 handle-transfer path and makes a dup-able cross-Proc handle new surface to
  police, hardening `ShareBoundedByFlow`); *an explicit `SYS_FLOW_SHARE`/`SYS_FLOW_MAP` pair
  decoupled from the open* (most ABI, least "the grant *is* the share"). The EL0 surface —
  `SYS_WEFT_SHARE = 81` (netd registers a Burrow as a flow's ring) + `SYS_WEFT_MAP = 82`
  (the guest maps the flow's ring → VA), keyed on the `/net` data fid — is **flow-bound, so
  it lands at Weft-6** (the per-flow cap binding), *not* Weft-2: the `/net` SrvConn is
  shared across *all* flows (joey mounts `/net` over one kernel dev9p session), so a
  per-flow share cannot key on the SrvConn — it keys on connection N, which is the Weft-6
  wiring. **`weft.tla` is unchanged:** grant-is-the-share *realizes* Init (the model is
  delivery-agnostic by construction — the grant is implicit in Init), the cross-Proc
  dual-refcount SMP-safety is `burrow.tla`'s (#847) domain, and the flow-containment is
  I-28's; no new invariant-bearing mechanism is introduced, so the model written model-first
  at Weft-1 stands (spec-first satisfied).
- **Weft-3 (the descriptor ring + in-place payload) — LANDED.** The virtio split-ring
  between guest and netd; the consumer reads payload in place at the validated offset; the
  hybrid `< threshold` fallback to the byte-copy ring.

  *As-built (the substrate; no EL0 ABI):* `kernel/include/thylacine/weft.h` pins the
  on-shared-page ABI — `weft_desc` (16 B, `{addr,len,flags}`) + `weft_ring_hdr` (64 B, the
  split-ring control words), both `_Static_assert`-pinned, single-writer per region
  (`prod_tail` guest-owned / `cons_head` kernel-owned — the `loom_ring_hdr` discipline, leg
  (5)). `kernel/weft.c::weft_ring_drain` is the kernel snapshot-and-bounds-validate consumer
  — the spec's `Consume`: it acquire-loads `prod_tail`, COPIES each in-flight descriptor to
  kernel memory (the TOCTOU snapshot, the `loom_drain_sq` `ksqe = sqes[i]` pattern),
  bounds-validates it against the registered payload region (`(u64)addr + len <=
  payload_size`, no u32 wrap; reserved flags clear; len > 0), and acts only on that snapshot
  — never re-reading the shared slot (`DescPinnedToSnapshot` / `ActedDescValidated`). The
  geometry is the kernel's private `weft_ring_view` (the `l->sq_entries` precedent — a guest
  scribble of the header mirror cannot move it). `weft_should_ring` is the hybrid `< 1024 B`
  threshold. The kernel is the validator (it pinned the ring Burrow at grant, the I-30
  pin — RDMA-HCA-shaped, not per-op *mediation*). 6 kernel tests
  (`weft.ring_{basic,toctou_snapshot,bounds_reject,multi_split,layout_constraints}` +
  `weft.should_ring_threshold`) drive a producer/consumer over a `burrow_share_into`
  cross-Proc shared page: the in-place read, the snapshot's immunity to a post-consume
  mutation, the bounds gate, the single-writer split-ring property, and the layout
  fail-closed. 943/943, boot OK, 0 EXTINCTION; the `weft.tla` spec gate re-ran green (clean
  1412 distinct + each buggy cfg violates its named invariant — `ring_toctou` →
  `DescPinnedToSnapshot`). The live guest↔netd wiring (the dev9p Weft-Twrite path that calls
  the drain) + the EL0 share delivery is Weft-6. Reference: `docs/reference/125-weft.md`.
- **Weft-4 (the readiness ring + the latency convergence) — LANDED.** The single-cache-line
  readiness poke; the substrate that retires the 50 ms RX-wake (NET-PERF N1).

  *As-built (the substrate; no EL0 ABI):* `kernel/include/thylacine/weft.h` adds the
  `weft_ready_hdr` (128 B, **two cache lines, single-writer-per-word** — `ready_seq`/
  `ready_mask` producer-owned, `wait_seq`/`wait_active` consumer-owned, on separate lines so
  the producer and consumer never false-share), placed in the per-flow Burrow's control region
  after the descriptor-ring header (`ready_off` mirrored at `weft_ring_hdr` offset 32).
  `kernel/weft.c` adds the four direction-agnostic decision primitives: `weft_ready_signal`
  (the producer poke — bump `ready_seq` + read the park-intent, returning *wake-needed*),
  `weft_ready_observe` (the consumer fast-path — the lock-free Shenango cache-line acquire-read),
  `weft_ready_arm_park` (the consumer **register-then-observe** — publish the park-intent then
  re-read `ready_seq`, returning *safe-to-park*), and `weft_ready_unpark`. The no-lost-wake is
  the **store-buffer register-then-observe** (`specs/weft_readiness.tla`, I-9): each side does a
  seq-cst store then a seq-cst load on opposite words — the StoreLoad barrier the SB litmus needs
  (the Linux `set_current_state()`+`smp_mb()`) — so in the global seq-cst order at least one side
  sees the other's write and an edge in the park window is never lost. The producer never writes
  the consumer-owned words (its wake is a Rendez wakeup, wired at Weft-6); a parked consumer is
  re-scheduled, then clears its own `wait_active` on resume. 3 kernel tests
  (`weft.ready_{signal_observe,park_handshake,arm_park_sees_race}`) drive the producer/consumer
  primitives over a `burrow_share_into` cross-Proc page: the poke's cross-page visibility, the
  wake decision (armed → wake), and the observe re-check (a pre-arm edge → don't park). 946/946,
  boot OK, 0 EXTINCTION.

  **Spec-first** (RE-ENABLED for Weft): the readiness ring is a *new* I-9 surface — the **push**
  counterpart of the dev9p.poll elicited **pull** (`net_poll.tla`), which *eliminates* the probe.
  Per the `cons_poll.tla` / `net_poll.tla` / Loom-5 precedent (a new wake mechanism gets its own
  focused module, leaving the audited `weft.tla` untouched), `specs/weft_readiness.tla` (clean +
  liveness `EventuallyDrained` + the `BUGGY_OBSERVE_BEFORE_ARM` counterexample on `NoLostReadyWake`)
  is written + TLC-green BEFORE the impl. The live park/wake integration (the `sleep`/`wakeup` on
  the decision, per direction) is **Weft-6**; the readiness ring closes NET-PERF N1 once wired.
  Reference: `docs/reference/125-weft.md`.
- **Weft-5 (the `F_NOTIF` zero-copy-send contract) — LANDED.** The two-CQE send completion
  (a result CQE `LOOM_CQE_MORE` = "queued", then a notification CQE `LOOM_CQE_F_NOTIF` =
  "buffer reusable"); the I-30 buffer pin released at the **notification**-terminal — the last
  of {netd stack done, NIC DMA done, peer ACK} — never op-terminal; the fallback-copied
  indicator (`WEFT_NOTIF_COPIED`, the `IORING_SEND_ZC_REPORT_USAGE` analog, so netd is never
  silently-wrong). Kernel-internal substrate (no EL0 ABI, autonomy-OK): `struct weft_notif` is
  the kernel-private per-send holder tracker (`weft.tla` `holders[b]`) — the holder bitmask is
  the complete state, and the release gate (`weft_notif_clear` → `WEFT_NOTIF_RELEASE`) fires
  **exactly once** on the last-holder transition, so the spec's `ReleasePremature` (the io_uring
  `ubuf_info` UAF) is *structurally unreachable* through the API. Impl-against-the-existing-spec
  (`weft.tla` already models the full F_NOTIF lifecycle: `NetdAct`/`HolderRelease`/`ReleaseClean`/
  `ReleasePremature`/`PinHeldWhileInFlight`/`NoInFlightReuse`) — **no new module**; the spec gate
  re-ran green (clean 1412 distinct + the `weft_buggy_premature_release` → `PinHeldWhileInFlight`
  counterexample). 3 kernel tests, 949/949 + boot OK + 0 EXTINCTION. The **live two-CQE posting**
  (the `weft_notif` on the flow's `loom_async_op`, the holders driven by real netd/NIC/ACK events)
  is wired at Weft-6. Reference: `docs/reference/125-weft.md`.
- **Weft-6 (the per-flow capability binding + the native API) — DESIGN LOCKED (the keying
  fork resolved 2026-06-20, §6.1; impl OWED).** The EL0 delivery: bind the ring setup to the
  `/net` data fid's I-30 pin (the "grant *is* the dataplane" realization), **lazily and keyed
  on the 9P fid** (Option B, §6.1 — superseding the earlier eager "data-fid auto-map" sketch).
  The mechanism:
  - **`Tweft(fid F) → Rweft(share_id, geometry)`** — a new Thylacine-private 9P op
    (`P9_TWEFT = 142` / `P9_RWEFT = 143` since the #371 renumber -- born 134/135,
    which latently collided with Stratum `Tfadvise`/`Tpin`; the cross-project
    registry is `docs/9P-EXTENSIONS.md`; the **#845 `Tflush` precedent** — a kernel-client-issued op the netd 9P server handles). The kernel
    dev9p client issues it on the shared `/net` client the **first** time a flow goes
    zero-copy.
  - **`SYS_WEFT_SHARE(ring_va, ring_size) → share_id / -1`** (netd-side, syscall 81). On
    `Tweft(F)` netd resolves `F → connection N` (its server fid table),
    `SYS_BURROW_ATTACH`-allocates the per-flow ring in its own AS, initializes the headers
    (`weft_ring_init_hdr` + `weft_ready_init_hdr`), then `SYS_WEFT_SHARE` resolves
    `ring_va → netd's backing ANON Burrow`, validates ANON + size + **RW-only (W^X, like
    `SYS_BURROW_ATTACH`)**, takes the **I-30 pin** (`burrow_ref`), mints a kernel-scoped
    `share_id`, and returns it; netd embeds `share_id` + geometry in `Rweft`.
  - **`SYS_WEFT_MAP(data_fd, hint_va) → ring_va / -1`** (guest-side, syscall 82). The native
    client calls it lazily, on the first large transfer: resolve `data_fd → (client, F)` via
    `dev9p_client_fid`; if unbound, issue `Tweft(F)` → `Rweft(share_id, geom)`; join
    `share_id → the pinned Burrow`; `burrow_share_into(guest, burrow, hint_va, RW)`; record
    the binding in the data Spoor's `dev9p_priv`; return `ring_va` (idempotent — a second call
    returns the cached VA). **The `share_id` is kernel-internal (netd→kernel via `Rweft`),
    never handed to the guest** — so a guest cannot forge one, and the kernel honors a
    `share_id` only when it arrived on the kernel's own `Tweft` to that netd (the RDMA-`rkey`
    shape, §4.6).
  - **Teardown / lifetime:** `share_id` is a transient kernel token, consumed exactly once at
    the kernel's `SYS_WEFT_MAP` completion, GC'd (netd-pin released) if the guest dies before
    mapping. The binding lives in `dev9p_priv`; the data-fid clunk (`dev9p_close`) drops
    **both** #847 refs (the guest mapping + netd's registration pin) → the ring Burrow freed
    (I-7). This *is* the `ShareBoundedByFlow` / `NoStaleShareAccess` realization.
  - **Going live here:** the descriptor ring (Weft-3), the readiness ring's park/wake (Weft-4
    — the Rendez the substrate reserved), and the `F_NOTIF` two-CQE holders (Weft-5 — driven
    by real netd-stack / NIC-DMA / peer-ACK events) all wire up. Small/control writes stay on
    the byte-copy 9P path (`weft_should_ring` < 1024 B); **only a flow that goes zero-copy ever
    issues `Tweft` + builds a ring.**
  - **The native `libthyla_rs::net` API:** the Demikernel-shaped `push` / `pop` / `wait` over
    the ring, composing with the existing `TcpStream` / `TcpListener` (which hold the data fd);
    small payloads fall back to `read` / `write` (the hybrid threshold).
  - **`weft.tla` stays UNCHANGED:** the `Tweft` / `share_id` setup is plumbing the model
    abstracts as `Init` (the Weft-2/3/5 impl-against-existing-spec precedent); the new
    correlation property (a `share_id` consumed exactly once, no cross-flow mis-binding) is
    validated by prose + the **Weft-7** audit (the broadened suspension — Weft's spec-first is
    re-enabled only for the I-37 buffer-lifetime *core*); if 6a surfaces real subtlety, the
    spec is extended *first*. **Sub-split:** 6a (kernel ABI: the 2 syscalls + `Tweft`/`Rweft` +
    the `dev9p_priv` binding + the `share_id` correlation + teardown + kernel tests) / 6b (netd:
    handle `Tweft` + alloc/register the ring + drive the desc ring on large Twrite/Tread + the
    live readiness park/wake + the `F_NOTIF` posting) / 6c (the native API + the in-guest E2E).
    **Weft-6a-1 LANDED** (`Tweft`/`Rweft` + `p9_client_weft`; `P9_TWEFT = 142` / `P9_RWEFT =
    143` since #371 -- born 134/135). **Weft-6a-2 LANDED** — the `share_id` registry in `kernel/weft.c` + `SYS_WEFT_SHARE
    = 81` / `SYS_WEFT_MAP = 82` + the `dev9p_priv->weft` binding + the `dev9p_close` teardown +
    the `proc.c` owner-death GC. The registration pin is held by the binding; the guest mapping
    is reclaimed by `vma_drain` (the Loom-ring precedent), so `dev9p_close` drops only the pin.
    5 kernel tests; `weft.tla` unchanged (the `share_id` correlation is the `Init`/`Teardown`
    plumbing the model abstracts — the impl-against-existing-spec posture). The full live
    `SYS_WEFT_MAP` E2E (a real `Tweft` to netd's handler) is owed to Weft-6b/6c; the
    consumed-exactly-once / no-cross-flow-mis-binding / netd-pin-GC prosecution is Weft-7. See
    `docs/reference/125-weft.md` "The EL0 delivery". **Weft-6b-1 LANDED** — the netd half (pure
    userspace; the kernel is byte-unchanged): `usr/netd/src/server.rs::h_weft` (dispatched on
    `P9_TWEFT`) resolves `F → connection N` (fail-closed unless an opened `/net/<proto>/N/data`
    fid of a live slot), `weft_ensure(N)` lazily `SYS_BURROW_ATTACH`es + `init_ring`s + `SYS_-
    WEFT_SHARE`es a per-flow 256 KiB/64-entry ring (one ring + one `share_id` per flow, stored
    in `Slot.weft`, idempotent on a repeat `Tweft`), and answers `Rweft(share_id, geom)`;
    `slot_unref` detaches netd's ring mapping at the last clunk (the netd half of
    `ShareBoundedByFlow`). The shared ABI substrate the native client (6c) reuses lands here too:
    `libthyla_rs::weft` (the `repr(C)` ring-ABI mirror + `init_ring`, the `loom.rs` precedent) +
    `libthyla_rs::ninep` `Tweft`/`Rweft` codec. This makes grant-is-the-share LIVE end to end —
    proven in-guest by `net-echo`'s `weft_e2e` (a real `SYS_WEFT_MAP` round-trip over the
    resident loopback: a ring VA + the visible geometry mirror + an idempotent second map,
    `net-echo: weft-6b MAP E2E PASS`), with the soak leak-baseline confirming no teardown leak.
    The live DATA DRIVE's wire mechanism (the new `Tweftio` op) is resolved in **§6.2**
    (user-voted 2026-06-20). **Weft-6b-2 LANDED** — the live TX zero-copy drive (`Tweftio`/
    `Rweftio` + the kernel write fast-path + the binding ring view + netd's `h_weftio`; a large
    write whose buffer points into the ring moves zero-copy, proven by `net-echo`'s `weft_tx_e2e`,
    `net-echo: weft-6b TX E2E PASS`). **Weft-6b-3a LANDED** — the symmetric RX direction
    (`Tweftio` `dir=READ` + the kernel read fast-path + netd's recv-into-ring + the net-6a-1-shaped
    blocking defer: a weft `recv()` parks on an empty socket and `poll_weftio` delivers the held
    `Rweftio` when bytes arrive, so it blocks instead of spuriously returning 0; proven by
    `weft_rx_e2e`, `net-echo: weft-6b RX E2E PASS`). `weft.tla` stays unchanged for both (the data
    drive is the symmetric read/write direction of the modeled `Consume`; the 4 buggy cfgs re-ran
    green). The standalone 6b-3b (readiness park/wake) + 6b-3c (F_NOTIF posting) legs are
    **folded into one coherent Weft-6c chunk** (user-directed 2026-06-20) so each new mechanism
    ships with its native-API consumer + a real in-guest data E2E — see Weft-6c below.
- **Weft-6c (the native `libthyla_rs::net` push/pop/wait API + the live F_NOTIF + the readiness
  busy-poll) — DESIGN (2026-06-20; the user folded the standalone 6b-3b/6b-3c legs into one
  coherent native-async chunk).** The native API **rides Loom** (§4.4/§4.5: command queue = SQ,
  completion queue = CQ, the Demikernel PDPIX contract Loom already presents) — so it **composes
  the existing Loom + Weft ABI and mints no new syscall / wire op:**
  - **`pop` (recv)** = a `LOOM_OP_READ` SQE on the registered `/net` data fid whose registered
    buffer is the `SYS_WEFT_MAP`'d ring region. The kernel's `loom_submit_payload`, after
    `dev9p_client_fid` resolves the Spoor, detects the `dev9p_priv->weft` binding (the buffer is
    in the ring) and routes the async op to `p9_client_weftio(READ)` (the 6b-3a zero-copy
    fast-path: netd recvs in place into the ring, the kernel does **no** copy-out) instead of the
    byte-copy `loom_build_read`. The terminal CQE carries the byte count; the bytes are already
    in the guest's ring.
  - **`push` (send)** = a `LOOM_OP_WRITE` SQE → `loom_build_weftio(WRITE)` (the 6b-2b zero-copy
    fast-path). **AS-BUILT (6c-1) — the honest F_NOTIF realization:** a weft WRITE is a zero-copy
    send *across the guest↔netd boundary*, but at v1.0 netd's `h_weftio` COPIES the ring into its
    socket buffer, so the slice is reusable the instant `Rweftio` returns — the io_uring SEND_ZC
    "copied" path: a **single terminal CQE, no `LOOM_CQE_MORE`** (`loom.tla` single-shot), which
    *is* the "buffer reusable" signal. There is no deferral and no `loom_async_op.notif` field at
    v1.0. The deferred two-CQE (a result CQE with `MORE` "queued", then a `LOOM_CQE_F_NOTIF`
    "reusable" CQE when the **last** of {netd stack done, NIC DMA done, peer ACK} clears — the I-30
    pin held to notification-terminal, I-37 leg 2) is the **v1.x true-zero-copy seam**: it needs a
    netd-holds-the-page TX path (no internal copy) + a netd→kernel holder-clear channel, neither of
    which exists at v1.0. The consumer reads `LOOM_CQE_MORE` to decide whether to wait for the
    notification — clear ⇒ reusable now (v1.0), set ⇒ wait (v1.x) — so the native API is forward-
    compatible without change. `weft.tla` already models both (the copied case =
    `weft_notif_arm_copied` → not-in-flight; the deferred case = `HolderRelease`/`ReleaseClean`).
  - **`wait`** = `SYS_LOOM_ENTER(min_complete)` — the **existing** Loom CQ wait
    (`loom_wait_for_completions` on `cq_waiters`; `loom.tla` I-9 no-missed-CQ-wake). **No new weft
    wait/wake syscall**: the park is the Loom CQ wait, woken by the CQE post. This is the answer to
    "wire the Weft-4 Rendez": the consumer's park *is* the Loom CQ wait, not a separate weft Rendez.
  - **The readiness ring** (Weft-4) is the **syscall-free busy-poll edge**: a native client
    `weft_ready_observe`s `ready_seq` (the Shenango cache-line acquire, no syscall) to skip the
    SQE/ENTER when data is already in its ring; netd `weft_ready_signal`s the edge after a recv.
    The substrate's `arm_park` / Rendez-park leg (a netd→kernel *direct* wake) stays
    **validated-not-wired** — a v1.x ultra-low-latency direct-park mode, which is the one shape
    that *would* mint the cross-Proc weft wait/wake the Loom-ride avoids. The readiness ring
    removes the **guest-wake** round-trip (§5.4); the netd-notice 50 ms floor is the orthogonal
    NIC-IRQ-fd chunk.
  - **netd: PULL-first.** The guest's `LOOM_OP_READ` drives the `Tweftio`; netd recvs into the
    ring on demand (the audited 6b-3a path) + writes the readiness edge for the busy-poll fast
    path — netd is **byte-unchanged in the kernel** and adds only the readiness write. **PUSH**
    (netd pre-fills the per-flow ring on NIC RX, decoupled from the guest read — the §5.2-2 "netd
    writes RX frames into the ring" end-state) is a **v1.x latency seam**; PULL-first delivers the
    native async API + zero-copy + Loom batching without the netd redesign.
  - **`weft.tla` UNCHANGED** (the READ/WRITE drive is the symmetric `Consume`; the F_NOTIF posting
    is the modeled `HolderRelease`/`ReleaseClean`, already TLC-green at Weft-5; the 4 buggy cfgs
    re-run). **`weft_readiness.tla` UNCHANGED** (the busy-poll observe is the modeled
    `ConsumerProcess`; the arm_park-park leg stays validated-not-wired). **`loom.tla` +
    `loom_multishot`/`loom_order` cfgs re-run** (the async completion path the weft op rides).
    **No new spec module** (the Loom-ride composes audited mechanisms).
  - **Sub-split:** **6c-1 LANDED** (kernel — the weft-binding detection + routing in
    `loom_submit_payload` to `loom_build_weftio` for a weft-bound `/net` data fd; the honest
    single-CQE copied-send realization; kernel tests; spec re-runs). **6c-2 LANDED** (pure
    userspace; the kernel byte-unchanged) — `libthyla_rs::net::WeftFlow`, the native
    Demikernel-shaped `push`/`pop`/`wait` over Loom + `SYS_WEFT_MAP` + the registered whole-ring
    buffer (the park *is* the Loom CQ wait; single-in-flight over the one payload region); the
    `libthyla_rs::weft` readiness-drive primitives (`ready_signal`/`ready_observe`, the
    `weft_readiness.tla` mirror) + netd's `WEFT_READY_RX` edge on each recv-into-ring (the §6
    syscall-free busy-poll); proven in-guest by `net-echo`'s `weft_async_e2e` (`net-echo: weft-6c
    async E2E PASS`) — a bidirectional zero-copy round-trip over the resident `lo` (push verified
    server-side, pop verified out of `rx_buf`, the readiness seq advancing, single-in-flight
    enforced); 965/965 + boot OK + 0 EXTINCTION + the spec gate (weft + weft_readiness + loom
    clean). The deferred two-CQE true-ZC path stays the v1.x seam. **6c-3 = Weft-7** (the focused
    buffer-lifetime-UAF + `Tweft`/`share_id`-correlation + F_NOTIF-holder audit + SMP gate + the §8
    throughput benchmark + #269 M6 + docs).
- **Weft-7 (the focused audit + SMP gate + benchmark).** Prosecute the buffer-lifetime UAF
  (the §4.6 hazard, the F1-class), the no-per-op-mediation property, the cross-Proc Burrow
  lifetime, **and the new Weft-6 `Tweft` / `share_id` correlation** (the consumed-exactly-once
  token, the no-cross-flow-mis-binding, the netd-pin GC if the guest dies before mapping); the
  throughput measurement (§8); docs.

### 6.1 The Weft-6 keying decision (resolved 2026-06-20)

The grant-is-the-share vote (§5.2, Weft-2) settled *that* opening the data fid establishes the
share and *that* no Burrow handle crosses Procs — but not *how* the ring setup correlates
across the netd↔kernel boundary. The grounding pass found the wall: the kernel knows a data fd
as `(p9_client, 9P fid F)`; netd knows it as `connection N`. The bridge is the fid `F` (the
kernel client picks it in `Twalk(newfid=F)`; netd binds `F→N` server-side), but the kernel's
`dev9p_priv` does not cache netd's qid, so it cannot recover `N` from a fd alone. Three viable
shapes were surfaced as a structured choice; the user picked **B**:

| Option | Mechanism | Precedent | Why / why not |
|---|---|---|---|
| A — eager auto-map at open | netd allocs the ring at the data-fid `Tlopen`; the kernel auto-maps it into the guest at open-completion (no explicit map syscall) | Fuchsia IOBuffer RFC-0218 ("the grant returns the peered ring"); the most literal grant-is-the-share | a ring per data-fd open, **even for small-payload flows that never go zero-copy** — tension with the §4.8 hybrid threshold |
| **B — lazy fid-keyed, new `Tweft` op (CHOSEN)** | the ring is built on the first large transfer, keyed on the fid `F` both sides share; a new `Tweft(F)→Rweft(share_id)` op + `SYS_WEFT_SHARE` / `SYS_WEFT_MAP` | **virtio-9p `p9_client_zc_rpc`** (the transport sets up the ZC region per-RPC) + RDMA (registration mints the `rkey`-capability, §4.6) + Plan 9 (the fid is the per-open handle) | **only B never charges a small-payload flow a ring** (composes the hybrid threshold); `dev9p_client_fid` already returns `F`; no qid-caching; no new token namespace to police. Cost: one new 9P op + the netd↔kernel correlation window (prosecuted at Weft-7) |
| C — token via ctl file | netd publishes a flow token into a `weft` ctl file; the guest claims the ring with it | Snap's bootstrap-regions-via-handshake | no new 9P op, but the token is a **new capability surface to police** (softer than "the fid IS the cap"); most guest-side moving parts |

**Why B over A** (A was the literal reading of the earlier "data-fid auto-map" sketch): the
§4.8 heritage rule keeps small / control payloads on the byte-copy ring, so most flows never
need a shared page; A's eager per-open ring is wasted setup for exactly those flows, while B's
laziness materializes a ring *only* when a flow actually goes zero-copy. B keeps the
grant-is-the-share spirit (holding the data fid is what *authorizes* the ring) while deferring
the *materialization* to first use. **Why B over C:** B keys on the fid — the identity both
sides already share — so there is no new token capability to mint, leak, or forge; the
`share_id` is a kernel-internal join key (netd→kernel via `Rweft`, never handed to the guest),
whereas C's ctl token is a softer, more-surface-to-police variant of the same idea.

### 6.2 The Weft-6b-2 data-drive decision (resolved 2026-06-20)

Weft-6b-1 made the per-flow ring's *mapping* live (a `Tweft` on first use builds +
maps the ring). Weft-6b-2 makes the *data* live: a large payload travels through that
shared ring instead of the 9P body. The substrate settled most of the shape — the guest
produces; the kernel is the I-30 validator-once (`weft_ring_drain`); §4.8's "the
Tread/Twrite still happens, only the payload moves off-band" — leaving one question:
**how the kernel signals netd to act on the kernel-validated descriptor.** Three shapes
were surfaced; the user picked **A**:

| Option | Mechanism | Why / why not |
|---|---|---|
| **A — new `Tweftio` op (CHOSEN)** | the kernel issues a Thylacine-private 9P op carrying the kernel-validated descriptor; netd reads/writes the ring **in place** + replies the count | keeps the kernel the I-30 validator-once **and** a 9P op mediating every transfer (4.8-faithful; the `Tweft` precedent — a kernel-client-issued op netd handles); one new wire op |
| B — reuse Twrite/Tread | the descriptor rides the body; netd infers weft mode from a count-vs-body-length mismatch | no new op, but the signal is an overloaded convention (no flag field in `Twrite`; a 16-byte real-payload edge to guard) |
| C — netd drains its own ring | netd drains its OWN mapping (woken by the readiness poke); the kernel leaves the per-op data path | most zero-copy, but netd becomes the per-op descriptor validator (the TOCTOU leaves the kernel TCB), there is **no** Tread/Twrite for data (a §4.8 *side-channel* — the explicit violation), and the substrate's kernel-side `weft_ring_drain` goes unused — a re-architecture |

**Why A:** it preserves the two properties the whole NOVEL rests on — the kernel is the
validator-once (the RDMA-HCA shape, §4.6/§5.3) and the 9P file abstraction mediates every
access (§4.8: a side-channel read/written *without* a Tread/Twrite is the violation A
avoids and C commits). B's overload is fragile (the count-vs-body-length signal has a
real-payload edge case); C's re-architecture moves descriptor validation off the kernel
TCB onto netd.

**The as-decided mechanism (impl OWED across 6b-2a/6b-2b):**

- **`Tweftio(fid, off, len, dir) → Rweftio(count)`** — `P9_TWEFTIO = 144` /
  `P9_RWEFTIO = 145` since #371 (just past `Tweft`/`Rweft` 142/143; the parity `RX = TX + 1`). The
  kernel dev9p data path issues it on a large transfer on a weft-bound fd; netd's server
  handles it (the `Tweft`/`Tflush` precedent). `dir` = WRITE (TX) / READ (RX). Body 16 B:
  `[fid:u32][off:u32][len:u32][dir:u32]`; reply `[count:u32]` (4 B). Reuses the audited
  tag/demux machinery (no `9p_client.tla` change — the buggy cfgs re-run green).
- **The kick is the existing `SYS_WRITE`/`SYS_READ`** on the data fd (**no new syscall**;
  lineage-correct — virtio-9p's `p9_client_zc_rpc` reuses the same request, payload
  off-band). The native client writes the payload into the ring's payload region (its own
  mapping) and issues `SYS_WRITE(data_fd, ring_va + payload_off + O, L)` — the buffer points
  *into* the bound ring. `dev9p_write`, on `priv->weft != NULL && weft_should_ring(L)`,
  derives the descriptor `{O = buf - (guest_va + payload_off), L}` from the (trusted,
  register-passed) syscall args, validates it against the binding's kernel-private
  `weft_ring_view` (the same `weft_desc_valid` bounds gate — but sourced from a trusted
  syscall arg, so inherently free of the descriptor-ring TOCTOU), and issues `Tweftio`. A
  small write (`< 1024 B`) or a buffer outside the ring stays on the byte-copy path (the
  hybrid threshold).
- **netd's `h_weftio`** resolves `fid → connection N`, reads `ring_va + payload_off + off`
  for `len` **in place** (TX → `data_send` to smoltcp; RX → `data_recv` *into* the ring),
  and replies `Rweftio(count)`.
- **The kernel-private `weft_ring_view`** is stored in the binding (computed at
  `SYS_WEFT_MAP` via `weft_ring_layout` from the shared Burrow's contiguous KVA) — the
  trusted geometry the validate reads, never the guest-mutable shared header mirror.

**The descriptor RING (`weft_ring_drain`) vs the synchronous syscall-arg path:** 6b-2
drives the *synchronous* path — one descriptor per `SYS_WRITE`, the buf-in-ring *is* the
descriptor (no shared-ring slot to snapshot, so inherently TOCTOU-free; the validate is
the trusted-arg sibling of the ring drain). The descriptor RING + `weft_ring_drain`'s
snapshot discipline is the **batched/async** submission the native `libthyla_rs::net` API
drives through Loom at **Weft-6c** (post N descriptors, one boundary crossing, the kernel
drains all N — the throughput amortization). Both validate through `weft_desc_valid`
against the kernel view; the synchronous path is the first live data movement.

**Split:** **6b-2a** (the `Tweftio`/`Rweftio` wire codec + `p9_client_weftio` + the session
arm + dispatch — kernel + the `libthyla-rs`/`ninep` mirror, mirroring 6a-1) / **6b-2b** (the
`dev9p_write` weft path + the `weft_ring_view` in the binding + netd's `h_weftio` + the
in-guest **TX** E2E over net-echo's loopback). **RX** (the `dir=READ` half, which needs
netd's net-6a-1 blocking-read defer) + the **readiness park/wake** (the Weft-4 Rendez) +
the **`F_NOTIF`** two-CQE posting are **Weft-6b-3**. `weft.tla` stays unchanged (the
synchronous descriptor is a trusted syscall arg validated by the modeled `weft_desc_valid`
gate; no new invariant-bearing mechanism — the impl-against-existing-spec posture of
Weft-2/3/5/6a).

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

**Weft-7 as-built (the MW loopback bench, #269 M6) — CORRECTED 2026-06-21 (#290).** `netperf`'s
**MW** mode is the deterministic in-guest realization of item 4 (the Weft delta) over the
resident `lo`: the M2 twin with the SEND side on a `WeftFlow` (push/wait over Loom → `Tweftio`
→ netd reads the ring in place) vs `TcpStream::write` (byte-copy). Run head-to-head with M2 at
the SAME 256 KiB and instrumented to split the data-move cost from the readiness-stall cost
(an earlier write-up here claimed "weft is ~10× slower ... pays per-op Loom+Tweftio overhead",
which was WRONG): **(1)** weft is NOT slower — the aggregate is a dead heat (MW ~2394 vs M2
~2382 KiB/s); the "~4.3× slower" was a SIZE artifact (M2 had been at 32 KiB, too small to fill
the 4 KiB window, so it never stalled, vs MW at 256 KiB which did). **(2)** Weft's DATA-MOVE is
~2× faster (it moves the bytes in ~half the ops — each push absorbs more than the window;
per-op cost is EQUAL, so there is NO Loom/`Tweftio` penalty). **(3)** ~95% of BOTH aggregates is
the POLLOUT readiness-stall and it is IDENTICAL for the two transports (M2 ~100620 µs, MW
~101519 µs — transport-independent): a bulk sender fills the 4 KiB window then waits for the
writable edge, which on the resident `lo` stack is driven by `net.poll` with no 9P frame to
wake on. NEITHER is a weft defect. The bench now reports a `weft_breakdown` line for both M2+MW.

**#221 LANDED 2026-06-21 (the readiness-stall fix; ~6× loopback throughput).** Root cause:
netd's serve loop forced a flat 50 ms re-poll while any connection had a pending probe, and
smoltcp exposes no prompt timer for the loopback window-update — so the unblock waited up to
50 ms/stall. Fix (netd-only, off the audited kernel surface): honor smoltcp's `poll_delay`
clamped to `[1 ms, 2 ms]` while a probe is pending (vs the idle 50 ms floor). Result: M2 byte-copy
2370 → **14267 KiB/s**, MW weft 2436 → **15073 KiB/s** (~6×), and MW now edges M2 — weft's
~2× faster data-move finally surfaces in the aggregate once the shared stall shrinks. The
residual stall (~12 ms over the transfer) is smoltcp's own loopback window-update round-trip.

NOTE — **#288 ("grow netd's 4 KiB socket tx buffer") is mis-scoped (found #291).** netd's TCP
socket buffers are ALREADY 64 KiB and msize is 32 KiB (Weft-0 #267). The real ~4 KiB/op cap is
the KERNEL `SYS_RW_MAX = 4096` bounce buffer on the byte-copy path (every `SYS_WRITE` is capped
at 4 KiB) -- which **weft BYPASSES** (it moves the ring slice in place, ~8 KiB/op here, the
source of its ~2× data-move edge). So the only residual byte-copy lever is growing `SYS_RW_MAX`
(a kernel-wide change that does NOT help weft, the future path); weft's own ceiling is the
loopback drain / window-update timing, not a buffer size. The full window/msize sweep + the
NIC-path Weft delta (items 1-3, 5) stay the v1.x measurement backlog.

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
