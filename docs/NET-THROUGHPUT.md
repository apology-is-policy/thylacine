# NET-THROUGHPUT — charter for a deep throughput session (research + design)

Status: **research charter, no code.** Written 2026-06-20 at the user's
direction, after the NET-PERF profiling arc (NP-0..NP-4, `docs/NET-PERF.md`)
answered the latency question ("HTTPS slow on HVF = the netd RX-wake floor, ours
not the device"). This doc captures the **throughput** findings, the measured
baseline, the levers we already hold, and — deliberately seeded but **not yet
deep-researched** — the novel / SOTA realms a substantial research pass should
evaluate next session. The user's framing:

> A more focused, very deep session on throughput, via mechanisms we already
> have in hand (window tuning, Loom amortization), but also zoom out — don't be
> afraid of extra work and research if there's anything in the novel realms that
> is a natural extension of Thylacine's existing mechanisms, or something wholly
> new that fits ideologically or on NOVEL grounds (more work to implement, but
> would reward us).

The next session's job is **Section 4 (Tier C)**: go deep, read the SOTA, judge
the fit against Thylacine's mechanisms, surface the NOVEL candidate(s), and bring
back a design fork. Sections 1-3 are the grounded starting point.

---

## 1. The throughput model (code-verified)

The single most useful equation, and it lands exactly on the measurement:

> **stream throughput = TCP window / cross-process round-trip time**
> = 4 KiB / ~250 us = **~16 MiB/s** (M6 NIC, HVF: 32 KiB / 2.0 ms).

Every term is verified in the tree:

- **The window/chunk is 4 KiB.** `TCP_TX_BUF = TCP_RX_BUF = 4096`, `DATA_CHUNK =
  4096`, `SRV_MSIZE = 8192` (`usr/netd/src/server.rs:72/78/98`). msize is 8 KiB
  but the 4 KiB smoltcp send buffer caps each accepted write at 4 KiB, so it is
  the binding constraint. **One 9P RPC moves at most 4 KiB.**
- **Each RPC is a full cross-process round-trip.** A guest `write()` is: `SYS_WRITE`
  trap -> the kernel dev9p client builds a `Twrite` into a **per-client `out_buf`**
  (`kernel/9p_client.c:977`) -> sent over the SrvConn **byte ring** -> the guest
  thread **blocks** -> the scheduler runs **netd** (a separate Proc) -> netd
  `SYS_READ`s the ring -> `data_send` -> `smoltcp::send_slice` into the
  pre-allocated TX `SocketBuffer` (`server.rs:1834`) -> NIC TX -> netd `SYS_WRITE`s
  the `Rwrite` -> the scheduler **wakes the guest**. That is **~3 syscalls + 2
  context switches + the TCP ACK that reopens the 4 KiB window**, per 4 KiB.
- **netd is single-threaded.** One serve loop, no `thread_spawn`; every
  connection's I/O serializes through that one round-trip engine.

### What this corrects

The intuition was "the 9P model is chatty and allocates a lot." Split:

- **Chatty: yes — this is the whole story.** Small window over a cross-process
  round-trip. The throughput ceiling is the bandwidth-delay product, full stop.
- **Allocation: no — not on the hot path.** The bulk-send path is **alloc-free**
  per write: the kernel client reuses a per-client `out_buf`; netd's TCP
  `send_slice` copies into a **pre-allocated** smoltcp `SocketBuffer`. The
  allocations that exist (the 4 KiB rx/tx socket buffers, the SrvConn rings) are
  **per-connection**, amortized over the whole transfer — not per byte. (UDP/ICMP
  *do* alloc a packet buffer per datagram; TCP bulk does not.) Drop the
  allocation angle; it is not the throughput killer.
- **Copies: ~half a dozen, but cheap.** user -> syscall buffer -> `out_buf` ->
  ring -> netd -> smoltcp -> frame -> NIC, vs the host's *one* `memcpy`. At ~28
  KiB of copying per 4 KiB chunk this is a few microseconds at memory bandwidth —
  dwarfed by the ~250 us round-trip. Copies matter for CPU at high packet rates,
  not for this ceiling.

### The architectural floor (be honest about it)

The host does loopback at **3156 MiB/s** because it is a monolithic kernel doing
**one in-process `memcpy`** — no process boundary, no round-trip. Ours is the
Plan 9 / capability-microkernel choice: **the stack is a separate Proc reached by
9P.** That buys the isolation + namespace properties (a confined Proc reaches
only the `/net` its territory grants; a NIC-driver bug cannot corrupt the kernel)
— but it imposes a per-chunk cross-process cost a monolithic stack never pays.
**We will not reach 3 GB/s.** The honest target is **hundreds of MiB/s**, and the
levers attack the two terms of `window / RTT`: enlarge the window, and amortize
or eliminate the round-trip.

---

## 2. The benchmark baseline (from NET-PERF.md sections 8/10)

| Path | Throughput | What it isolates |
|---|---|---|
| **M2** loopback bulk (in-guest, 2-thread) | **~50 KiB/s** | the loopback-drain POLLOUT *timer* cadence (#221) — a *separate* artifact, NOT the real send rate |
| **M6** NIC bulk -> host sink (HVF) | **~16 MiB/s** | the real send rate: 4 KiB window / ~250 us cross-proc round-trip |
| **M6** NIC bulk (TCG) | ~4.8 MiB/s | the same, CPU-slowed |
| **host** loopback (B2) | **3156 MiB/s** | monolithic in-kernel memcpy (the unreachable ceiling) |

Note the M2 vs M6 gap is instructive: **M2 (50 KiB/s) is not the send rate** — it
is the in-guest loopback drain's POLLOUT readiness being delivered on a ~50-80 ms
timer (#221), so it under-reports wildly. M6 (16 MiB/s, host sink draining fast)
is the honest single-stream rate. Any throughput work must measure on M6-class
paths (a fast external drain), not M2, or use a fixed #221 first.

Re-measure with `tools/np3-bench.sh` (the M6 path); add a large-transfer mode +
window/msize sweep to `usr/netperf nic` when the levers land.

---

## 3. Tier A + B — levers we already hold (in-hand mechanisms)

### Tier A — tune what exists (cheap, high-certainty)

1. **Grow the window + msize (the dominant, near-free lever).** 4 KiB ->
   64 KiB+ TCP buffers + a larger `SRV_MSIZE`/`DATA_CHUNK` => ~16x more bytes per
   round-trip at the *same* RTT => ~16x throughput (16 -> ~250 MiB/s ballpark).
   Audit-bearing only against the #841/#845 9P back-pressure invariants and the
   #65 per-Proc memory cap (bigger per-conn buffers). The single biggest win for
   the least work; almost certainly the first thing the deep session lands.
2. **Loom batching (amortize the round-trip).** Loom already exists (the
   io_uring-inverted SQ/CQ ring, KObj_Loom) and is already wired for `/net`
   (net-6a-3: a Loom READ on a `/net` fid completes via the kernel dev9p async
   client). Today each 4 KiB pays one trap + one context-switch round-trip; a
   guest that submits N writes into the SQ ring and reaps N CQEs pays **one**
   boundary crossing for the batch. Loom does not make a single RPC cheaper — it
   makes the *per-op* round-trip cost vanish. This is the throughput analog of
   what the irq-fd is for latency, and it is the principal reason Loom matters
   here (it is NOT the latency-floor fix). Needs: a native `libthyla_rs` Loom
   `/net` writer/reader that batches, + netd draining a batch per wake.

### Tier B — natural extensions of existing mechanisms (more work, same ideology)

3. **Zero-copy via Loom registered buffers.** Loom already has **registered
   buffers** (Loom-6a/6b: the I-30 buffer pin — a user page the kernel reads/
   writes directly). A Loom WRITE against a registered buffer can skip the
   `user -> out_buf` copy (the kernel reads the payload in place from the pinned
   page). This is io_uring's `IORING_OP_SEND_ZC` model and it is **already half-
   present** in the Loom design — worth a deep look at how far the pin reaches
   (does it reach netd's smoltcp buffer, or stop at the kernel client?).
4. **A shared-page 9P-data transport (zero-copy rings).** The SrvConn transport
   is a **byte-copy ring** (out_buf -> ring -> netd). A shared-*page* transport —
   the guest and netd share the payload page, netd reads it in place, no copy —
   is the virtio/vhost descriptor model applied to the netd boundary. Thylacine
   already has the substrate: **Burrow** (shared VMOs) + **Loom** (shared rings).
   So "the guest's TCP payload lands in a page netd maps, not a ring it copies"
   is a natural extension, not a new primitive. This is the path that removes the
   per-chunk copies *and* (with batching) the per-op round-trip.
5. **Loom as the netd transport itself.** Today kernel-dev9p-client <-> netd is
   the SrvConn byte ring + per-op syscalls on both sides. If netd drained a
   Loom-style shared ring directly (the guest's writes land in a shared SQ that
   netd polls/drains, no per-op trap on either side), the guest->netd path
   unifies on Loom. Big, but the most ideologically coherent end-state (one ring
   substrate for FS *and* net async I/O).

---

## 4. Tier C — the research mandate (zoom out; the next session's deep work)

These are **candidates to evaluate, not asserted fits.** The mandate: read the
SOTA, judge each against Thylacine's mechanisms (Loom, Burrow, the capability/
namespace model, 9P, the separate-Proc netd), and bring back either a "natural
extension" design or a NOVEL.md candidate with a build/cost estimate. Follow the
CLAUDE.md "research prior art before surfacing a design fork" discipline: the
Plan 9 heritage first, then the capability-uK SOTA, then the fit, then the novel
angle.

### 4.1 The dataplane-OS research lineage (the closest SOTA)

The modern high-performance-networking research line is **"OS as the control
plane, data as a direct path"** — which maps unusually well onto a capability OS:

- **Arrakis** (OSDI 2014, "The Operating System is the Control Plane"). The OS
  sets up the I/O path at grant time (isolation enforced *once*, at setup), then
  the app does data-path I/O directly to the device, bypassing the kernel
  per-op. **This is almost exactly Thylacine's capability model said in
  performance terms**: the kernel/netd grants the capability (control plane), the
  data flows direct (data plane). The key transferable idea: *isolation at
  grant-time, not per-operation*. Evaluate: can a confined Proc be granted a
  *direct, capability-scoped* path to its own flows, with netd/the kernel only on
  the control plane?
- **Snap** (Google, SOSP 2019, "a microkernel approach to host networking"). A
  userspace networking **service process** (exactly netd's shape) that apps reach
  via a fast shared-memory transport + a polling engine. **Direct proof that
  "net as a separate Proc" can be fast** with the right app<->service transport —
  the single most relevant system to study, because it is netd's architecture
  done at Google scale. Study its transport (shared-memory rings, the polling/
  scheduling model) as the template for the netd<->Proc fast path.
- **Demikernel** (SOSP 2021). A portable kernel-bypass *dataplane OS* with a
  zero-copy async API (the "datapath OS" / libOS shape). Relevant for the **API
  surface** of a zero-copy async net path — and notably its API is queue/
  completion shaped, i.e. Loom-shaped.
- **IX** (OSDI 2014) + **Shenango/Caladan** (NSDI'19 / OSDI'20). Protected
  dataplane + run-to-completion (IX); fine-grained core allocation for us-scale
  tail latency (Shenango/Caladan). Less directly portable (they assume dedicated
  dataplane cores), but the "dedicated polling dataplane" idea relates to netd's
  serve loop and the latency-floor work.
- **io_uring zero-copy send** (`IORING_OP_SEND_ZC`) + registered buffers/files.
  The production SOTA for batched zero-copy async I/O. Loom is io_uring inverted;
  this is the reference for how far the registered-buffer + batching path goes.
- **virtio/vhost** + **RDMA one-sided ops**. The shared-descriptor zero-copy
  transport (virtio) and registered-memory one-sided reads/writes (RDMA) — the
  mechanics for Tier B #4 (shared-page rings).

### 4.2 The Plan 9 heritage (do this first, per discipline)

How did the lineage move 9P fast over a *local* boundary? Plan 9's `srv` + a
shared segment; the `#|` pipe; `exportfs` performance; the **IL** transport (the
lightweight reliable protocol Plan 9 built specifically to carry 9P without TCP's
overhead). Plan 9 did not chase Gb/s, but the *local-9P-fast-path* idiom (shared
memory between the client and the file server) is heritage-grounded and may
legitimize the Tier B #4/#5 direction as "Plan 9-correct," not a deviation.

### 4.3 The NOVEL synthesis to chase

The headline candidate the deep session should try to crystallize:

> **A capability-scoped, zero-copy, batched network dataplane** — the fusion of
> Arrakis's control/data-plane split (isolation at grant time) + Snap's userspace
> net service (netd) + Loom's shared async ring (the data plane) + Thylacine's
> capability/namespace model (the control plane) + io_uring's registered-buffer
> zero-copy. The result: a confined Proc gets a **direct, zero-copy,
> capability-scoped** path to its flows. netd/the kernel does the control-plane
> setup (the capability check, the flow grant, the policy); the bytes then flow
> through a shared ring **without per-op mediation**. The security of the
> capability model **with** the performance of kernel-bypass.

If that synthesis holds up under research, it is a strong **NOVEL.md** candidate
(it is exactly "wholly new, fits ideologically, more work, would reward us"):
Thylacine's whole thesis is "complexity only where verified" + the capability/
namespace model; a dataplane whose *isolation is the capability grant* and whose
*speed is the absence of per-op mediation* is that thesis applied to throughput.
It is also genuinely novel ground: the dataplane-OS research (Arrakis/Snap) and
the capability-uK world (Fuchsia netstack, seL4, Genode) have not, to current
knowledge, been fused into "the capability grant *is* the zero-copy dataplane
setup." Worth a real literature check to confirm the gap.

### 4.4 What the deep session should produce

1. A short literature digest (the systems above + a Fuchsia-netstack / seL4 /
   Genode networking scan for the capability-uK angle), judged for Thylacine fit.
2. A decomposed throughput target: how far Tier A (windows) gets us, then Tier B
   (Loom batching + zero-copy), then the Tier C synthesis — each with a measured
   or modeled headroom number, on the M6 path.
3. A design fork surfaced to the user (the CLAUDE.md design-conversation pattern):
   how ambitious — tune (A), extend (B), or build the capability-dataplane (C);
   what is v1.0 vs a NOVEL post-v1.0 item — landing as a scripture/NOVEL commit
   before any code.
4. A benchmark plan: extend `usr/netperf nic` + `np3-bench.sh` with a
   large-transfer + window/msize sweep, so each lever is measured, not asserted.

---

## 5. Sequencing

This sits in the **net-optimization arc that precedes #231 REVENANT** (user
direction: "before revenant we have to make net on par with the host"). That arc
has two halves, both currently in research/design, not yet committed to an
implementation order:

- **Latency** — the RX-wake floor (NET-PERF N1: the pollable irq-fd / RX-driven
  netd). The approach fork (Tier 0 adaptive-poll / Tier 1 irq-fd / Tier 2
  Loom-unified) is **open** — the user deferred it to dig into throughput first.
- **Throughput** — this charter. The deep research + design session.

"On par with the host" is achievable for **latency** (IRQ-driven netd -> ~0 wake,
like the host) but **not literally** for **throughput** (separate-Proc 9P vs
in-kernel memcpy); the throughput goal is "hundreds of MiB/s + the right
architecture," which is exactly why it earns a deep, research-heavy session
rather than a window bump.

Cross-refs: `docs/NET-PERF.md` (the measured baseline + the latency arc),
`docs/LOOM.md` + `docs/reference/107-loom.md` (the async ring + registered
buffers), `docs/NET-DESIGN.md` (the stack), `docs/reference/121-netd.md`
(the serve loop), RW-11 #62 (the v1.x perf backlog this feeds).
