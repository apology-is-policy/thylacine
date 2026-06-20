# NET-PERF — the network performance profiling mini-arc

Charter for a measured, pure-profiling pass over the Thylacine network stack:
**measure, attribute, and rank** where latency and throughput are spent, compare
against the host baseline, and produce a ranked map of the reserve. The
optimizations are *out of scope here* — this arc produces the evidence; each
recommendation it ranks becomes its own follow-on arc the user greenlights.

Triggered by the observation (2026-06-20) that HTTPS `curl` is noticeably slower
in-guest (HVF) than native `curl` on the host, with the question: **is that
QEMU's virtual device, or an inefficiency in our path?**

---

## 1. Goal + the questions to answer

- Loopback TCP throughput: ours vs the host's.
- TLS 1.3 handshake time: ours vs the host's.
- Where is the **reserve** (headroom) and what is **sub-optimal**?
- The headline: **is HTTPS-slow-on-HVF QEMU's device, or us?**

The deliverable (NP-4) is an **attribution map** — an HTTPS fetch decomposed into
DNS + connect + TLS + HTTP, each cost attributed to one of {poll-wake floor, 9P
overhead, software crypto, real-network RTT, QEMU device/slirp} — plus a
**ranked list of optimization candidates** with their estimated headroom. No
stack changes land in this arc (so the numbers stay honest).

---

## 2. The grounded hypothesis (verified constants, to be quantified)

The code already points at the levers; the arc's job is to *quantify* them, not
discover them.

### 2.1 The prime latency lever: netd is timeout-polled, not RX-IRQ-driven

netd's serve loop (`usr/netd/src/main.rs`) is:

```
loop {
    net.poll(&mut device);          // (1) advance smoltcp; deliver any RX
    drain deferred replies          //     poll_accepts/data/ready/connects
    t_poll(pollfds, delay);         // (2) block on the 9P fds OR `delay` ms
    dispatch any 9P request         //     a client Twrite/Tread woke us
}
```

`delay` is clamped to **`IDLE_POLL_MIN_MS = 50` … `IDLE_POLL_MAX_MS = 1000`** ms
whenever a reply is deferred (`main.rs:87-88`). The NIC's RX interrupt is **not a
pollable fd** (`main.rs:500`: "wakeups would need a kernel ABI surface;
SYS_IRQ_WAIT blocks") — so:

- **Client-driven** ops wake netd *instantly*: a `Twrite`/`Tread` makes the 9P fd
  readable, `t_poll` returns at once, and the next loop top runs `net.poll`.
- **Peer-driven** completions (a SYN-ACK, a TLS handshake flight, an HTTP
  response arriving on the **NIC**) have **no 9P event** to wake netd. netd sits
  in `t_poll` until the timeout — up to **50 ms** — before the next `net.poll`
  picks up the RX.

**The load-bearing nuance — why loopback is fast but the NIC is slow:** on the
resident **loopback** stack, the data is produced by a *9P write* (the peer's
`send` is a Twrite), which wakes netd; the next loop-top `net.poll` delivers it
immediately. So loopback never hits the floor. On the **NIC**, the data arrives
from outside the guest with no 9P event — so each external-RX completion waits up
to 50 ms. A TLS fetch over the NIC has ~3 such waits (connect SYN-ACK + the
handshake flight + the HTTP response), so **~100-150 ms of pure timer latency**
stacks on the real RTT. On HVF the CPU is near-instant, so this fixed 50 ms floor
*dominates* — exactly the reported symptom. The host kernel's TCP stack is
IRQ-driven and has no equivalent floor.

**Candidate fix (a follow-on arc, NOT here):** a pollable **IRQ-readiness fd** (an
"irq-fd", the analog of Linux `eventfd`/`timerfd`) so netd can
`t_poll([srv_fd, irq_fd], …)` and wake on *either* a 9P request *or* a NIC RX.
This is a small kernel ABI addition; the profiling arc quantifies its payoff
before we build it.

### 2.2 Throughput levers

| Constant | Value | Effect |
|---|---|---|
| `TCP_RX_BUF`/`TCP_TX_BUF` (`server.rs:78`) | **4096** | the TCP window is 4 KiB; throughput ≈ window/RTT, and the poll floor inflates effective RTT → caps hard |
| `SRV_MSIZE` (`server.rs:72`) | **8192** | the 9P frame; bulk transfer is ≤ msize per RPC |
| `DATA_CHUNK` (`server.rs:98`) | **4096** | ≤ 4 KiB moved per `data` read/write = one netd round-trip + context switch per 4 KiB |

So bulk transfer is 4 KiB per 9P round-trip through a separate Proc. Candidate
fixes (follow-on): larger socket windows (64 KiB+), larger msize.

### 2.3 The crypto lever (HTTPS-specific)

rustls with the **RustCrypto** provider (pure-Rust, `no_std`) — almost certainly
**not** using the ARMv8 AES/SHA extensions. Bulk AES-GCM throughput is then
software-bound (matters for large HTTPS downloads, not the handshake). Candidate
fix (follow-on): hardware crypto intrinsics, if the aarch64 target exposes them.

---

## 3. The methodology — three axes of isolation (no in-guest profiler needed)

1. **Loopback isolates OUR stack.** The resident `lo` interface (net-8a) is
   guest → netd → smoltcp-loopback → netd → guest: **no NIC, no slirp, no QEMU
   device, no real network.** It measures the 9P + netd + smoltcp + crypto
   overhead purely (and, per §2.1, *without* the RX-wake floor). If loopback is
   already slow vs the host loopback, the inefficiency is *ours*.

2. **TCG vs HVF is a free profiler.** A fixed-time cost (the 50 ms floor) is the
   *same wall-clock* on both. A CPU-bound cost (crypto, smoltcp processing) is
   ~10-50× slower on TCG. So a metric ~equal across TCG/HVF ⇒ poll/timer/IO-bound;
   a metric much worse on TCG ⇒ CPU-bound. The contrast splits CPU from IO with
   no instrumentation. **Run every in-guest metric under both accels.**

3. **NIC-via-`guestfwd` isolates QEMU.** Loopback throughput vs throughput to a
   *host* server reached over the NIC = the QEMU virtio-net + slirp overhead. This
   directly answers "device or us?" (and is the deterministic outbound-TCP-over-
   NIC test already owed at **#258**).

All in-guest timing uses the **LS-K `CLOCK_MONOTONIC`** (`time::Instant`).

---

## 4. The metric surfaces

| | Metric | Isolates | Host baseline (standard tools, user-voted) |
|---|---|---|---|
| **M1** | per-9P-op RTT — a 1-byte ping-pong over an established loopback conn | 9P + netd + smoltcp per-op cost | a unix-socket ping-pong (tiny C/Rust) |
| **M2** | loopback bulk throughput (transfer N MiB) | 4 KiB window + `DATA_CHUNK` + 9P-per-buffer | `iperf3 -c 127.0.0.1` |
| **M3** | TCP connect latency (loopback **and** NIC) | the RX-wake floor (NIC) vs our connect-processing (lo) | host `connect()` |
| **M4** | TLS 1.3 handshake time over loopback | crypto (CPU, TCG-inflated) vs round-trip-wait (poll) | `openssl s_client -connect` (timed) |
| **M5** | crypto micro-bench: X25519, AES-128-GCM seal/open, sig-verify (ops/s) | software crypto (no ARMv8 AES/SHA) | `openssl speed` |
| **M6** | NIC-path throughput + latency (guest → host server via `guestfwd`) | the QEMU device + slirp delta (vs M2/M3 loopback) | the same host server, host-local |

The TCG-vs-HVF contrast applies to every in-guest row.

---

## 5. Sub-chunk sequence (pure profiling — user-voted)

- **NP-0 — scripture (this doc).** The methodology + metric definitions +
  baselines + the grounded hypotheses + the attribution framework. No code.
- **NP-1 — the in-guest `netperf` tool.** A native libthyla-rs benchmark
  (sibling to `net-echo`): **M1** (per-op RTT), **M2** (loopback throughput),
  **M3** (connect latency, loopback) over the resident `lo` stack, with
  `CLOCK_MONOTONIC` instrumentation. Driven both as a joey boot probe (a short
  fixed run, logged) and runnable from the shell with bigger parameters. Run
  under TCG + HVF.
- **NP-2 — TLS + crypto.** **M4** (instrument the existing net-echo two-thread
  TLS-over-loopback E2E to time the handshake, split crypto-time vs wait-time) +
  **M5** (a crypto micro-bench over the rustls/RustCrypto primitives). TCG + HVF.
- **NP-3 — the NIC path + host baselines.** **M6** via a slirp `guestfwd` to a
  small host server (this *is* the #258 deterministic outbound-TCP-over-NIC test —
  fold it in) + the host baseline harness (iperf3 loopback, a socket ping-pong,
  `openssl s_client`/`speed`). The apples-to-apples comparison numbers.
- **NP-4 — the report.** Decompose an HTTPS fetch into DNS + connect + TLS + HTTP;
  attribute each segment to {poll-wake floor / 9P overhead / software crypto /
  real RTT / QEMU device}; rank the reserve with estimated headroom; name the
  optimization candidates (RX-driven netd via an irq-fd; larger windows + msize;
  hardware crypto). Lands as a NET-PERF "Results" section + the ranked backlog
  (feeding the RW-11 v1.x perf backlog, #62). **No stack changes.**

---

## 6. The "device or us?" answer framework

The loopback path has **no** QEMU device, slirp, or real network — it is pure
Thylacine. The decision tree the numbers resolve:

- **Loopback already slow** (vs host loopback) ⇒ the inefficiency is **ours**
  (9P overhead, 4 KiB windows, software crypto — the CPU/throughput levers).
- **Loopback fast, NIC slow** ⇒ the latency is the **RX-wake floor** (our netd
  poll model, §2.1) and/or the **QEMU device** — and M6-vs-M2 splits those two:
  if NIC throughput ≪ loopback throughput at matched RTT, it's the device; if NIC
  *latency* ≫ loopback latency by ~multiples of 50 ms, it's our floor.

The strong prior (to be confirmed): **loopback is fine, the NIC latency is our
50 ms RX-wake floor**, and the QEMU device adds a smaller throughput tax on top.
If so, the #1 ranked recommendation is the irq-fd / RX-driven netd.

---

## 7. Tracking

- Tasks: NP-0 (this) → NP-1 → NP-2 → NP-3 (folds #258) → NP-4.
- Phase: post-net-utils, before #231 REVENANT (the user's "next session"). Slots
  alongside the v1.x perf backlog (RW-11 #62); the ranked output *feeds* it.
- Cross-refs: `docs/NET-DESIGN.md` (the stack), `docs/reference/121-netd.md`
  (the serve loop + the #257 deferred-connect), `docs/reference/122-net.md` +
  `123-tls.md` + `124-net-utils.md` (the client surfaces), ARCH §22.6 (LS-K
  CLOCK_MONOTONIC), `tools/ci-smp-gate.sh` (the TCG/HVF run harness pattern).

---

## 8. NP-1 results (measured, the resident `lo` stack)

The in-guest `netperf` tool (`usr/netperf` -- a native libthyla-rs `/net`
client; joey boot probe + shell-runnable as `netperf [iters] [mib] [conns]`)
over netd's resident loopback. Boot-probe run (M1 = 100 RTTs, M2 = 32 KiB,
M3 = 50 dials), TCG and HVF -- the axis-2 contrast (a CPU-bound cost is ~3x
faster on HVF; a timer/IO-bound cost is *equal* on both):

| Metric | TCG | HVF | Bound |
|---|---|---|---|
| **M1** per-op RTT (1 B ping-pong, 4 ops/rt) | mean **480 us** / min 428 / max 606 | mean **158 us** / min **44 us** / max 388 | **CPU-bound** (~3x on HVF): the 9P + netd + smoltcp + context-switch per-op cost |
| **M2** loopback bulk throughput | **50 KiB/s** (32 KiB / 629 ms) | **52 KiB/s** (32 KiB / 606 ms) | **timer/IO-bound** (accel-invariant): the per-4-KiB-window POLLOUT-readiness cadence |
| **M3** TCP connect latency | mean **3735 us** / min 3413 / max 5034 | mean **1085 us** / min 740 / max 1842 | **CPU-bound** (~3.4x on HVF): netd connect processing (clone + connect verb + PendingConnect + establish + accept) |

### The headline finding (M2): loopback bulk *send* is timer-paced, not CPU-paced

M2's throughput is **identical on TCG and HVF** (~50 KiB/s) despite HVF's
~10-50x faster CPU -- conclusive evidence the bulk-send ceiling is **not** a
copy/CPU cost. The cause: **netd's data write is non-blocking** -- a full 4 KiB
TCP send window returns a 0-count `Rwrite` (`server.rs` `data_send` ->
`send_slice().unwrap_or(0)`), never a deferred reply -- so a bulk sender must
poll POLLOUT on the `ready` sibling per window, and that readiness delivery is
**timer-paced** (the dev9p.poll idle-pump / netd's `IDLE_POLL_MIN_MS` 50 ms,
#221/#220): ~75-90 ms per 4 KiB window -> ~40-50 KiB/s regardless of transfer
size or accel. (The exact decomposition of the ~80 ms is NP-4's; the
attribution -- *not CPU* -- is settled by the accel-invariance.)

This **extends** the charter's "netd is timer-paced" thesis (§2.1): loopback
avoids the floor on small **request/response** (M1: a data write self-wakes
netd -> 480 us, no floor) but **not** on bulk **send** (the POLLOUT-readiness
path has no self-wake -> the per-window timer cadence dominates). Two faces of
one root cause -- and the reason the same stack is fast for an HTTP request but
slow for a bulk body.

**Candidate optimizations** (ranked at NP-4, feeding #62; NOT built in this
pure-profiling arc):
1. A **blocking-write** path (netd defers the `Rwrite` until the window has
   room, mirroring the existing blocking-read `poll_data`) -- removes the
   per-window POLLOUT round-trip entirely. Likely the single largest bulk lift.
2. **Event-driven POLLOUT** delivery (wake the parked poll the instant the
   window reopens, not on the idle-pump timer) -- the #221 fix.
3. Larger socket windows (64 KiB+) + msize (§2.2) -- fewer windows per MiB
   amortizes the per-window cost.

The M1/M3 CPU-bound results say per-op latency (request/response, connect) is
optimizable by reducing per-op CPU work (fewer 9P round-trips per operation,
leaner netd dispatch); HVF already shows the ~3x headroom a faster path buys.

### The `netperf` tool (as-built)

- `usr/netperf/src/main.rs` -- native libthyla-rs, no_std, no TLS dep (113 KiB,
  under `SYS_SPAWN_BLOB_MAX` so it is NOT stripped). Three metrics over the
  resident `lo` stack, timed with the LS-K `CLOCK_MONOTONIC` (`time::Instant`):
  M1 single-threaded 1 B ping-pong (the net-8b `loopback_e2e` pattern in a timed
  loop); M2 a two-Thread drain-server + main-writer (the net-8c-2 pattern minus
  TLS) with POLLOUT backpressure per window; M3 connect-then-drain-the-backlog.
- The one libthyla-rs surface add: `TcpStream::ready_fd()` (open
  `/net/tcp/N/ready`, the QTPOLL readiness sibling -- mirrors `UdpSocket`/
  `IcmpSocket`), so the M2 writer can wait for POLLOUT. Client code only; netd
  already serves the file (`FK_READY`).
- Wiring: `usr/Cargo.toml` member + `tools/build.sh::usr_rs_bins` (no strip) +
  the joey post-net probe (bare `t_spawn`, console-direct `t_putstr`, no caps).
  Gates the boot on a successful run (the numbers are data, not a threshold).

### Reproduce

Boot probe (logged every boot) or, from the shell, `netperf 1000 4 200`
(1000 RTTs / 4 MiB / 200 dials) for a long baseline (the 4 MiB M2 takes ~100 s
at ~40 KiB/s -- shell only, never the boot). The TCG-vs-HVF contrast:
`THYLACINE_ACCEL={tcg,hvf} tools/test.sh` then
`grep 'netperf M' build/test-boot.log`.

---

## 9. NP-2 results (TLS handshake + crypto micro-bench)

The in-guest `tlsperf` tool (`usr/tlsperf` -- native libthyla-rs; joey boot probe
spawned WITH `CAP_CSPRNG_READ` + shell-runnable `tlsperf [handshakes]`). **M4**
times a full TLS 1.3 handshake over netd's resident loopback (the net-8c-2
two-Thread TlsStream <-> TlsServerStream pattern, timing ONLY the
`TlsStream::connect` -- the TCP connect is untimed; M3 owns it). **M5**
micro-benches the EXACT RustCrypto primitives rustls-rustcrypto uses (ops/sec) --
which on aarch64 here have NO AES/SHA intrinsics, so they are the software-crypto
cost. Boot-probe run, TCG and HVF (the axis-2 contrast):

| Metric | TCG | HVF | TCG/HVF |
|---|---|---|---|
| **M4** TLS 1.3 handshake (cert verify incl.) | **29.1 ms** | **2.55 ms** | 11.4x |
| **M5** AES-128-GCM seal 1 KiB | 3020 op/s (331 us) | 47547 op/s (21 us) | 15.7x |
| **M5** AES-128-GCM open 1 KiB | 3097 op/s (323 us) | 47479 op/s (21 us) | 15.3x |
| **M5** SHA-256 1 KiB | 12393 op/s (81 us) | 186177 op/s (5.4 us) | 15.0x |
| **M5** X25519 agree | 1679 op/s (595 us) | 17620 op/s (57 us) | 10.5x |
| **M5** ECDSA-P256 verify | 164 op/s (6.1 ms) | 2371 op/s (422 us) | 14.5x |

### The findings

**(a) TLS is CPU-bound, all the way down.** Every M5 primitive AND the M4
handshake are ~10-16x faster on HVF -- so the TLS cost is *software crypto*, not
IO/timer (the opposite of M2's bulk send). The ~11x handshake ratio tracks the
~10-16x crypto ratios: the handshake is dominated by the asymmetric ops.

**(b) The handshake is asymmetric-crypto-bound.** On HVF a handshake is ~2.5 ms;
the asymmetric primitives a TLS 1.3 ECDHE-ECDSA handshake runs -- ~2 ECDSA-P256
verifies (the leaf + CertVerify, 422 us each) + the server's ECDSA-P256 sign
(~similar) + 2 X25519 agrees (57 us) -- sum to ~1.5 ms, the bulk of the 2.5 ms
(the rest is the loopback round-trips + the symmetric key schedule). ECDSA-P256
verify (422 us HVF / 6.1 ms TCG) is the single slowest primitive.

**(c) Crypto is NOT the HTTPS bulk-throughput bottleneck today -- the M2 transport
is.** AES-128-GCM runs at ~47k ops/s of 1 KiB = **~47 MB/s** software (HVF),
~1000x ABOVE M2's ~50 KiB/s loopback bulk ceiling (section 8). So an HTTPS bulk
download is gated by the M2 POLLOUT-readiness timer cadence, NOT by the cipher.
Hardware AES/SHA would lift the *crypto* ceiling ~5-10x but would not move bulk
HTTPS until the M2 transport floor is fixed first.

### Candidate optimizations (ranked at NP-4, feeding #62; NOT built here)

1. **The M2 transport fix first** (blocking-write / event-driven POLLOUT, section
   8) -- the actual HTTPS bulk bottleneck; crypto has ~1000x headroom over it.
2. **Hardware AES/SHA intrinsics** (the ARMv8 crypto extension) -- ~5-10x on the
   symmetric record layer + the SHA-heavy handshake transcript/HKDF; matters for
   bulk HTTPS *after* lever 1.
3. **TLS session resumption / 0-RTT** -- avoids the ~1.5 ms asymmetric handshake
   cost on repeat connections (the dominant per-connection HTTPS latency).

### The `tlsperf` tool (as-built)

- `usr/tlsperf/src/main.rs` -- native libthyla-rs, no_std, links the `tls` crate
  (rustls + rustls-rustcrypto; ~553 KiB stripped, in `tls_strip_bins`) and the M5
  crypto crates pinned to the workspace-resolved versions (x25519-dalek 2.0.1 /
  aes-gcm 0.10.3 / sha2 0.10.9 / p256 0.13.2), so the bench measures the SAME
  primitives TLS pays for (one copy, no second build). Spawned WITH
  `CAP_CSPRNG_READ` (the handshake's randomness via the `tls`-registered custom
  getrandom -> SYS_GETRANDOM).
- M4 reuses the net-8c-2 two-Thread pattern (a server Thread + the main client
  Thread interleave through netd over the loopback); M5 is pure CPU (no network).

### Reproduce

`tlsperf` (boot probe) or `tlsperf 100` (100 handshakes) from the shell. The
TCG-vs-HVF contrast: `THYLACINE_ACCEL={tcg,hvf} tools/test.sh` then
`grep 'tlsperf M' build/test-boot.log`.

---

## 10. NP-3 results (the NIC path M6 + host baselines) — the "device or us" answer

M6 measures the guest reaching a **host** server over the **virtio-net NIC +
slirp** (`tools/np3-bench.sh` runs a 3-port host server and a slirp `guestfwd`
maps `10.0.2.100:7820/+1/+2` -> the host's `127.0.0.1`; the in-guest `netperf
nic` boot probe drives it). Comparing M6 (NIC) to M1/M2/M3 (the resident `lo`
stack) isolates the QEMU device + slirp delta; the host baselines are the
apples-to-apples reference. This *is* the deterministic outbound-TCP-over-NIC
test (#258).

| Metric | TCG | HVF | Host baseline (loopback) |
|---|---|---|---|
| **M6 rtt** (1B echo over NIC) | **min 349 us** / max **51400 us** / mean 10547 | **min 50104 / mean 50437 / max 50862 us** (ALL floor) | B1 51 us (min 27) |
| **M6 floor** (host echo delayed 5 ms) | **51418 us** (consistent) | **50428 us** (consistent) | — |
| **M6 connect** (dial to ESTABLISHED) | 3925 us | 1164 us | B3 47 us (min 20) |
| **M6 throughput** (32 KiB send to sink) | 4788 KiB/s | **15968 KiB/s** | B2 3156 **MiB/s** |
| (for reference: M1 lo rtt) | 524 us | 158 us | |
| (for reference: M2 lo bulk) | 51 KiB/s | 52 KiB/s | |
| (for reference: M3 lo connect) | 3859 us | 1085 us | |

Host crypto (B4, openssl, ARMv8 AES/SHA): AES-128-GCM **~8.7 GB/s**, SHA-256
~2.5 GB/s, ECDSA-P256 ~20200 verify/s + ~60000 sign/s, X25519 ~36300/s. Host
TLS 1.3 handshake (B5, openssl s_time): **~0.31 ms** (3209 conn/s).

### The headline finding (M6 rtt): the slowness is OURS (the RX-wake floor), and it bites HARDEST on HVF

The charter (section 2.1) hypothesized a 50 ms RX-wake floor; M6 **confirms and
sharpens it**. A request/response over the NIC is: the guest *writes* (a 9P
Twrite wakes netd, which TXes) then *reads* (a 9P Tread wakes netd, which checks
RX). Two outcomes:

- **The reply is already in the RX ring** when the read's wake reaches netd ->
  delivered at once -> ~**350 us** (the device + 9P + smoltcp per-op cost; the
  M6-rtt *min*). The QEMU virtio-net + slirp device is **fast** — comparable to
  the in-guest `lo` (M1).
- **The read beats the reply** -> netd defers the read + parks in `t_poll(>=50
  ms)`; the reply lands on the NIC RX during the park but there is **no 9P event
  and no pollable IRQ fd to wake `t_poll`** (`netd/src/main.rs:495-501`,
  `IDLE_POLL_MIN_MS=50`), so it is delivered at the 50 ms timeout -> ~**51 ms**
  (the M6-rtt *max*).

**This is why HTTPS is slow on HVF specifically.** On the fast HVF CPU the guest
issues the read so quickly (the whole write->read cycle is microseconds) that the
reply — a real slirp+host transit of ~1 ms — has **not yet arrived**, so EVERY
round-trip defers and pays the floor: M6-rtt is `min == mean == max == 50 ms`.
On the ~50x slower TCG CPU the write->read cycle is hundreds of us, so the reply
is **often already there** -> bimodal (min 350 us, max 51 ms). The **M6-floor**
probe (the host echoes after 5 ms, forcing the reply past the park) confirms it
deterministically: a 5 ms reply costs **~51 ms** on both accels. A TLS fetch over
a real network has ~5-6 such round-trips (SYN-ACK + handshake flights + the HTTP
request/response), so on HVF that is **~250-300 ms of pure floor** stacked on the
real RTT — the reported symptom. The device is not at fault; our poll model is.

### M6 connect, M6 throughput: the device handles connect + bulk fine

- **M6 connect** (~3.9 ms TCG / ~1.2 ms HVF) ~= **M3 lo connect** — CPU-bound
  netd connect-processing, no floor (slirp's SYN-ACK is near-instant, so the
  connect read rarely parks). The device adds nothing to connect latency.
- **M6 throughput** (~5 MiB/s TCG / ~16 MiB/s HVF) is **~100x faster than M2 lo
  bulk** (~50 KiB/s). Both are POLLOUT-paced sends; the difference is the
  *drain*. M6's host sink drains over the NIC instantly, so netd's 4 KiB send
  buffer rarely fills -> the writer rarely hits `Ok(0)` -> rarely waits POLLOUT.
  M2's in-guest loopback drain (a 2nd thread reading via netd's `lo` + the
  timer-paced dev9p.poll POLLOUT readiness, #221) is the actual M2 bottleneck —
  **not the send, and not the device**. So the M2 50 KiB/s ceiling is a
  *loopback-drain* artifact, not the NIC's bulk capability.

### The "device or us" verdict (the user's question)

**Us, decisively.** The QEMU virtio-net + slirp device is fast (M6-rtt min ~350
us ~= the in-guest `lo`; M6 bulk ~16 MiB/s). The slowness is the **netd RX-wake
floor** (no pollable NIC-IRQ fd -> a NIC-arriving reply waits up to netd's 50 ms
idle poll), which on a fast CPU (HVF / real hardware) hits **every** round-trip.
Ranked levers (the NP-4 backlog, feeding #62):

1. **A pollable IRQ-readiness fd (the #1 lever).** A small kernel ABI (the
   eventfd/timerfd analog) so netd can `t_poll([srv_fd, irq_fd], ...)` and wake
   on a NIC RX *or* a 9P request. This eliminates the per-round-trip 50 ms floor
   — the single biggest real-RTT / HVF latency lever. (NET-PERF section 2.1's
   candidate, now quantified: ~50 ms x N round-trips per fetch.)
2. **Hardware AES/SHA** (NP-2 lever 2): ~100x on the symmetric record layer +
   the SHA-heavy handshake (B4 ~8.7 GB/s HW vs guest ~47 MB/s SW). Secondary to
   lever 1 for latency; matters for bulk HTTPS *after* the floor is fixed.
3. **The M2 loopback-drain path** (the dev9p.poll POLLOUT timer cadence, #221) +
   larger socket windows + msize — for in-guest bulk; the NIC bulk is already
   fine.

### The `np3-bench.sh` harness (as-built)

- `tools/np3-bench.sh` runs a 3-port host server (echo / +1 delayed-echo 5 ms /
  +2 sink — one port per metric so each gets its own slirp `guestfwd` connection;
  same-dest-port dials coalesce onto one rule's connection), the host baselines
  (a socket ping-pong / bulk / connect mirroring M1/M2/M3; `openssl speed` +
  `s_time` for B4/B5), then boots the guest under TCG + HVF with the guestfwd on
  and greps the M6 lines. Re-runnable; ground-truths the guest banner instead of
  trusting the HVF exit code (the #34 virtio-input QMP flake makes HVF `test.sh`
  exit 1 with a fully healthy guest).
- `netperf nic <ip> <port> [iters] [kib] [conns]` is the in-guest tool (a
  best-effort joey boot probe `netperf nic 10.0.2.100 7820`): a **bounded**
  reachability gate (`TcpStream::connect_timeout`, 600 ms, polls the QTPOLL
  `ready` sibling for POLLOUT = ESTABLISHED) so a standard boot (no guestfwd, no
  host server) **SKIPs fast** and never hangs; numbers only when np3-bench's host
  server is up. The one libthyla-rs add is `TcpStream::connect_timeout`.
- The guestfwd is **env-gated** in `run-vm.sh` (`THYLACINE_GUESTFWD`) -> the
  standard boot / SMP gate QEMU launch is byte-identical.

### Caveats / surfaced seams

- **M6 throughput is send-completion, not drain-confirmed** (a < 1/N_windows
  overcount): a half-close (`shutdown`) + read-the-ack would confirm the host
  drained, but **netd hangs a read after `shutdown(SHUT_WR)` when the peer
  replies post-FIN** (#264 — a real netd half-close read-path bug, surfaced here,
  latent for v1.0 full-duplex clients). The probe measures send-completion to
  sidestep it.
- M6 connect dials coalesce onto one slirp guestfwd connection (same dest:port),
  so it measures netd connect-processing with an instant SYN-ACK — the SYN-ACK
  floor does not manifest because slirp answers at once (a real-RTT SYN-ACK
  would, like the data floor).

### Reproduce

`tools/np3-bench.sh` (build + host baselines + M6 under TCG and HVF). Or, from a
logged-in shell with a host server + guestfwd up: `netperf nic 10.0.2.100 7820`.
The guest M6 lines: `grep 'netperf M6' build/test-boot.log`.
