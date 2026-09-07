# 153 — Nocturne: `nocturned`, the virtio-snd driver + the mixed-voice graph core

**Status:** N-1 AS-BUILT (2026-09-05, @562cbe50) + N-2a-1 AS-BUILT (2026-09-05)
+ N-2b-1 (the zero-copy Weft ring substrate + map witness) AS-BUILT (2026-09-06)
+ N-2b-2a (the ring PERIOD protocol + the ring-fed mixer) AS-BUILT (2026-09-06).
The design is `docs/NOCTURNE.md`; this chapter is what exists in the tree.
**N-1**: one warden-bound daemon owning the `virtio-sound` function, one
playback stream, one 9P tree, one boot probe, one host witness. **N-2a-1**: the
graph core's first half — multiple **voices** (independent S16LE-stereo streams)
**mixed** in float32 to the one sink, exposed through `nodes/new` + per-voice
`audio`/`ctl`/`info`. The internal graph is byte-copy (the designed fallback
below the Weft hybrid threshold, `docs/NOCTURNE.md` §6.5); the per-node Weft
ring (`nodes/<id>/data`), ports, links, ears/descants and the policy are N-2b /
N-3 / N-4. `nodes/` is a deliberately minimal voice surface — the ports/links/
descant ABI that `docs/NOCTURNE.md` §9/§10 leaves for the operator is NOT built
here.

Source: `usr/nocturned/src/{main,snd,server}.rs`, `usr/nocturne-probe/src/main.rs`,
`kernel/devdev.c` (the mount stub), `usr/joey/joey.c` (the mount + probe),
`usr/warden/src/main.rs` (the manifest), `tools/run-vm.sh`
(`THYLACINE_AUDIODEV`), `tools/audio-verdict.py`, `tools/test-audio.sh`.
Audit surface: `docs/AUDIT-TRIGGERS.md` "Nocturne N-1". Manual:
`docs/manual/41-audio.md`.

---

## Purpose

Prove the substrate every Nocturne candidate needs and give the tree a working
Plan 9 audio device: the guest can play PCM through virtio-sound to whatever
host backend QEMU was given, the driver path is exercised on every boot by the
probe ladder, and a deterministic host-side witness (the `wav` backend + a
spectral verdict) judges what was actually played. Nothing here decides the
design questions still open for the operator; the driver sits behind the
per-period fill callback (`next_period`) exactly where the N-2 mixer will plug
in.

## The process shape

`nocturned` is a Menagerie driver (`libdriver::Driver`): the warden matches
its manifest to the `virtio-pci:25` function, confers an allowance narrowed to
that function's `(bus,dev,fn)` + its INTx INTID + a 256 KiB DMA cap (I-34), and
spawns it persistent with `MAY_POST_SERVICE`. `probe` brings the device up
(`snd::VirtioSnd::open`); `serve` posts `/srv/nocturne`, spawns the **cycle
thread** (which owns the device), writes the one `READY` line the warden waits
for, and then *becomes* the **control thread** serving 9P. Since N-2c
`nocturned` runs two threads sharing the graph under one try-lock (D-1c; see
"The cycle/control thread split" below) -- the cycle thread on the device IRQ,
the control thread on the listener + 9P connections. joey mounts `/srv/nocturne` at `/dev/nocturne` (MREPL over
the devdev mount stub) when the service exists and logs
`joey: /srv/nocturne absent (no virtio-sound function); skipping` otherwise.

```
manifest (usr/warden/src/main.rs)
driver "nocturned" { binds = ["virtio-pci:25"]; needs { pci = "node"; irq = "node:interrupts"; dma = "pool: 256 KiB" }
                      serves = "/dev/nocturne"; restart = on-crash; lifecycle = persistent }
```

## The device half (`snd.rs`)

**Transport.** `PciDev::claim(25, BAR_WINDOW_VA)` maps the function's BARs at a
private 6 MiB window (`0x0200_0000`, clear of libdriver's `DriverVa` bump
region); the four capability regions come from `region(Common|Notify|Isr|Device)`
with their lengths checked before any register access (`CCFG_MIN_LEN` 0x38,
`SND_CFG_MIN_LEN` 12). The handshake is the VIRTIO 1.2 modern sequence
(ACKNOWLEDGE → DRIVER → features → FEATURES_OK → queues → DRIVER_OK) accepting
only `VIRTIO_F_VERSION_1`. Two virtqueues are configured — `controlq` (0) and
`txq` (2), 64 entries each; `eventq` and `rxq` stay disabled (QEMU's device
implements no eventq; capture is N-3). INTx only (both MSI-X vectors parked at
`NO_VECTOR`); the ISR byte is read-to-clear on every reap.

**The DMA pool** (`DMA_POOL_SIZE` = 8 pages + 4 × 2048 B = 40 KiB, allocated
via `libdriver::alloc_dma` and touched page-by-page before the device sees it):

| Page | Holds |
|---|---|
| 0–2 | controlq desc / avail / used |
| 3–5 | txq desc / avail / used |
| 6 | control request (+0) and response (+2048) |
| 7 | per-slot TX metadata: xfer header at `s*64`, status at `s*64+32` |
| 8… | per-slot TX payload, `PERIOD_BYTES` each |

Build-time `const _: () = assert!(...)` pins the layout (a 64-entry ring's
desc/avail/used each fit a page; `3 * PERIODS <= 64`; `PERIOD_BYTES % 4 == 0`).

**Stream negotiation** (`negotiate_stream`): `PCM_INFO` for stream 0 (the
response must say OUTPUT, offer `S16` and `48000`, and admit 2 channels — the
features/formats/rates words are logged), `SET_PARAMS { buffer 8192, period
2048, features 0, channels 2, S16, 48000 }` — byte-identical to QEMU's device
defaults — then `PREPARE`. `START` is deferred to the first data.

**The period clock.** Each TX slot `s` is a fixed 3-descriptor chain
`3s → 3s+1 → 3s+2` = xfer header (4 B) → payload (2048 B) → status (8 B,
device-written). `start()` primes all four slots through the `next_period`
callback and issues `PCM_START`; `pump()` reaps the txq used ring — every
completion is one 10.7 ms period — and re-posts the slot with the next period.
The used `id` is **device-controlled and validated** (`id % 3 == 0`,
`id / 3 < PERIODS`, the slot's in-flight bit set) before it names a slot;
anything else increments `bad_used` and is dropped without a re-post.
`latency_bytes` is clamped to `BUFFER_BYTES`; a non-`S_OK` status increments
`tx_errors`. Control round-trips (`ctrl_rpc`) poll the controlq used ring with
1 ms sleeps for at most 2 s, so a dead device fails `probe` loudly instead of
hanging the warden's bind ladder. A timed-out round-trip may still
land a late completion; `ctrl_drain_stale` drains any such outstanding
completion (bounded) before each new RPC, so a prior timeout cannot desync
the control queue's request/response slots (the audit P2 F3).

**Idle stop** (`stop()`): after `IDLE_STOP_PERIODS` (48, ≈0.5 s) consecutive
silence periods with an empty FIFO, `PCM_STOP` + `PCM_RELEASE` are issued, the
flushed completions are reaped **without** re-posting, every in-flight bit is
cleared, and `PCM_PREPARE` re-arms the stream for the next `start`. An idle
machine therefore pays no periodic interrupt.

## The server half (`server.rs`)

Framing + dispatch mirror `usr/ptyfs` (one `t_read` per readable event, every
complete frame dispatched, `Disp::{Reply,Deferred,Fatal}`). The listener stays armed at all times; when
the connection table is full (`MAX_CONNS` = 32) a fresh accept is closed
immediately (fail-fast EOF) rather than left to stall on the srvconn handshake
deadline (the audit P2 F2). N-2a-1 grows the static N-1 tree into a voice
graph:

| Path | qid | Mode | Read | Write |
|---|---|---|---|---|
| `/` | 0 | `0555` dir | `Treaddir` lists `ctl info volume audio nodes` | — |
| `audio` | 3 | `0666` | 0 bytes (output-only, `audio(3)`) | S16 stereo 48 kHz into **voice 0** |
| `info` | 2 | `0444` | device words + counters + `voices N` | `EPERM` |
| `ctl` | 1 | `0644` | one description line | `flush` (drops voice 0); else `EINVAL` |
| `volume` | 6 | `0444` in the mount / `0666` on `-ctl` | `audio <l> <r>` + `mix <l> <r>` (Plan 9 `volume(3)`) | READ-ONLY in the mounted playback tree; writable only on `/srv/nocturne-ctl`, gated (N-3a-3) |
| `nodes/` | 4 | `0555` dir | `Treaddir` lists `new` + each live voice id | — |
| `nodes/new` | 5 | `0666` | the id of the voice this open minted | (open is the mint) |
| `nodes/<id>/audio` | vpath | `0666` | 0 bytes | S16 stereo 48 kHz into voice `<id>` |
| `nodes/<id>/ctl` | vpath | `0644` | one line | `gain <percent>` / `flush` / `remove` |
| `nodes/<id>/info` | vpath | `0444` | that voice's stats | `EPERM` |
| `nodes/<id>/data` | vpath | `0666` | `EINVAL` (a Weft map fid) | `EPERM` (driven by SYS_WEFT_MAP) |

Voice paths encode `VBIT | (id << 4) | leaf` (leaf 0 = dir, 1/2/3/4 =
audio/ctl/info/data) — the tapestry `surf_n`/`surf_fk` idiom, so one `u64` qid
names both the voice and the file within it.

**Voices + the mixer.** `Shared.voices` is a `Vec<Voice>` (cap `MAX_VOICES` =
16). Voice 0 is persistent (`owner = -1`, the root `audio` file); every other
voice is minted by opening `nodes/new` and is owned by that connection's handle.
Each `Voice` carries its own `VecDeque<u8>` FIFO (cap `FIFO_CAP` = 64 KiB ≈
340 ms), a linear `gain` (default 1.0, set via `ctl gain <percent>` — Plan 9
`volume(3)` 0..100+ style, clamped to 1000 %), and byte/flush counters.
`next_period` MIXES: for each voice it pops whole frames into a `[f32; 1024]`
accumulator scaled by that voice's gain, then clamps the sum to the S16 range
once (the only bound on a hot mix — the f32 accumulator makes N unity voices
un-overflowable before the clamp, the I-14 posture at the graph layer). An empty
voice contributes silence; the pass returns whether ANY voice supplied real
data (the idle-stop counts silence).

**Authority (the F1 owner gate).** A write or `ctl` to a non-zero voice is
accepted only from the connection that minted it: the handler resolves the
voice's `owner` (the minting conn's handle) and returns `EPERM` for a live
voice owned by another connection, `EBADF` for an unknown id. Voice 0 is the
world-shared sink and is exempt -- any connection may write it (the `audio(3)`
root file). This bounds a voice to its owner so one client cannot inject into
or starve another's stream (I-46 authority, the audit P1); the `/dev/nocturne`
mount is one shared connection, so programs sharing that mount also share its
voices by construction -- per-exit isolation is the direct-connection path
below.

**The parked write** is unchanged from N-1 but per-voice: a `Twrite` to a
voice's `audio` pushes what fits and PARKS the rest in a `PendingWrite {tag,
fid, voice, data, done}`; `poll_writes` drains parked writes in order after the
pump frees room and replies `Rwrite(total)` on completion (Plan 9's blocking
write). `MAX_PENDING_WRITES` = 8 per connection; a clunk drops that fid's
parked writes; `Tflush(oldtag)` cancels exactly that parked write (accepted
bytes stay queued and play).

**Voice lifetime.** A voice minted through `nodes/new` dies when the connection
that made it closes (`teardown` → `drop_conn_voices`, the tapestry
surface-lifetime idiom) or on an explicit `ctl remove`; voice 0 never dies. NB:
the `/dev/nocturne` **mount** is joey's one shared connection, so voices minted
via the mount are owned by the mount conn and persist — correct for the boot
probe; a client wanting per-exit lifetime connects **directly** to
`/srv/nocturne` (the libtapestry idiom) -- the SDL audio backend does exactly
this (N-2a-2, `docs/reference/142-sdl-port.md` "Audio: the Nocturne backend"):
its `CloseDevice`, or the program's death, drops the connection and the voice.

`info` renders `device`, `format`, `voices N`, `bufsize`, `buffered`,
`period-bytes`, `buffer-bytes`, `periods`, `started`, `periods-played`,
`silence-periods`, `tx-errors`, `bad-used`, `latency-bytes`; `nodes/<id>/info`
renders `voice`, `gain` (percent), `buffered`, `bytes-in`, `flushes`, `owner`.

**The sink volume + the whole-sink authority gate (N-3a-2).** The root
`volume` file speaks Plan 9 `volume(3)` over the active sink: a write is one or
more lines `audio <v>` / `audio <l> <r>` / `mix <v>` / `mix <l> <r>`, values
0..100 (100 = unity), and a read renders the current `audio`/`mix` per channel.
The two controls are a sink gain STAGE on `Graph` (`sink_audio[2]`,
`sink_mix[2]`, default unity): `next_period` scales each channel of the final
float mix by `(sink_audio/100) * (sink_mix/100)` BEFORE the single I-14 clamp,
so the stage only ever attenuates and can never push the mix past the clamp.
`dev <name>` (sink selection) is N-3b (one sink today); an unknown control is
`EINVAL`, leaving the gain unchanged.

The sink is SYSTEM-owned, so whole-sink authority is the two-axis rule of
I-26/I-39 (NOCTURNE.md 6.8, I-46): a `volume` write is admitted iff the
connection's peer is `PRINCIPAL_SYSTEM`, holds `CAP_HOSTOWNER`, holds the
`CAP_AUDIO_GRAPH` clearance (the corvus-gated "audio-graph" level, N-3a-1), OR
its session OWNS the console (`SRV_PEER_FLAG_CONSOLE_OWNER` -- the person at the
keyboard, N-3a-3). The gate reads the peer FRESH via `SYS_SRV_PEER` on each write
-- never an accept-time snapshot -- because caps mutate: a clearance can be
redeemed or expire after the connection opens. A dead/unknown peer fails closed.

**Two posts, because a mount cannot carry per-writer identity (N-3a-3).**
`t_srv_peer` resolves the peer of the SERVER-SIDE connection, and 9P binds
identity at ATTACH, per connection -- so a write through joey's ONE shared
`/dev/nocturne` mount always carries the MOUNTER (SYSTEM), never the writing
program (the N-3a-2 F1 bypass: any user could change system audio). Per-writer
authority is therefore impossible through the shared mount. nocturned splits the
tree by whether the operation carries authority (the Warp precedent -- an
authority surface is never globally mounted):

- **`/srv/nocturne`** -- the mounted PLAYBACK tree (`audio`, `nodes/`, `info`,
  `ctl`, read-only `volume`). Namespace IS the capability; the mounter's identity
  is irrelevant because nothing here is authority-gated. Its `volume` is `0o444`,
  so the kernel dev9p rwx gate refuses a write-open THROUGH the mount, and
  `h_write` refuses a `P_VOLUME` write on any non-control connection even if an
  owner/root open slips the mode. `cat /dev/nocturne/volume` still works.
- **`/srv/nocturne-ctl`** -- the SINK-AUTHORITY post, reached by a controller
  over its OWN connection (`open=connect`, never mounted). The volume node here
  is `0o666` and writable, and the peer IS the writer, so `volume_authorized`
  judges the real caller. The native `nocturne-vol` tool connects here; the
  console-owner session sets the volume with no grant, others need the clearance.

The witness (`/nocturne-vol-probe`, `tools/test-nocturne-volume.sh`) proves the
split BOTH ways -- and the arm the N-3a-2 witness lacked is the one that matters:
a USER writing volume THROUGH the mount is REFUSED (the F1 regression), a SYSTEM
write on `-ctl` is ACCEPTED + the grammar/F3 round-trip holds, the mount `volume`
READS, and a user-principal child's `-ctl` write is REFUSED (EPERM). The
`CAP_AUDIO_GRAPH` axis is covered by `test_devcap.clearance_audio_graph` and the
console-owner axis by the kernel `proc_identity.peer_snapshot_console_owner`
test.

## The zero-copy ring (N-2b-1)

A voice grows a `data` leaf: a Weft map fid, not a byte file. A client opens it
THROUGH the `/dev/nocturne` mount (`SYS_WEFT_MAP` needs a dev9p fd -- a direct
`/srv/nocturne` srvconn will not do) and calls `SYS_WEFT_MAP(data_fd)`; the
kernel issues `Tweft(fid)`, nocturned's `h_weft` lazily allocates the voice's
ring and replies `Rweft(share_id, size, entries)`, and the kernel maps the ring
into the calling Proc. nocturned reuses the kernel Weft substrate UNCHANGED --
`sys_weft_share_for_proc` is generic and `CAP_HW_CREATE`-gated, which nocturned
holds as a warden-bound driver -- so there is no kernel change and `weft.tla` is
not in play; the lifecycle mirrors netd's `weft_ensure` verbatim.

**The ring** (`Shared::weft_ensure`): one ANON Burrow per voice, laid out by
`weftlib::init_ring` as `RING_HDR (64) + READY_HDR (128) + DESC[RING_ENTRIES]
(16 each) + payload`. `RING_ENTRIES` = 8 period slots (K >= 3); `RING_SIZE` =
20480 (5 pages) gives a payload of 20160 B >= 8 x `PERIOD_BYTES`. nocturned is
the CONSUMER (reads finished periods on the device clock), the client the
PRODUCER (`prod_tail`/`cons_head` ownership). N-2b-1 built the allocate + share
+ map half; N-2b-2a adds the period producer/consumer protocol over the ring and
the ring-fed mixer (below).

**Lifetime** (I-7/I-37): the ring is a field on the `Voice`
(`Option<RingVoice>`); `RingVoice`'s `Drop` runs `t_weft_unshare(share_id)` +
`t_burrow_detach(va, size)` when the voice is removed (a connection's teardown,
or `ctl remove`), releasing nocturned's side. The kernel's #847 dual count keeps
the pages alive until the client's mapping is gone too, so there is no
in-flight-page UAF regardless of teardown order. A `Vec<Voice>` reallocation
MOVES a `RingVoice` (a Rust move is not a drop), so a voice push never
spuriously unshares.

**Authority** (I-37/I-46a): `h_weft` gates on the minting connection (the F1
owner gate -- `EPERM` for another owner, `EBADF` for a gone voice) and refuses
voice 0 (`EINVAL`; the world-shared byte sink has no single-producer ring).
Because `/dev/nocturne` is one shared kernel dev9p session, the owner gate cannot
isolate mounted clients from each other (the pre-existing F5 limitation); the
kernel's CONSUME-ONCE share claim is the SPSC producer-uniqueness backstop --
only the first Proc to `SYS_WEFT_MAP` a given voice's ring claims it, a second
mapper gets a claim failure. W^X holds (the ring is ANON RW-only; `SYS_WEFT_SHARE`
rejects EXEC). The `data` leaf is never byte-read/written (`EINVAL`/`EPERM`).

## The ring period protocol (N-2b-2a)

The payload region is K = `RING_ENTRIES` FIXED period slots of `PERIOD_BYTES`
each; slot i sits at `payload_off + (i % K) * PERIOD_BYTES`. This is a fixed-slot
SPSC over the Weft ring -- a safer drain mode than netd's addr-based descriptor
ring -- and the shared primitives live in `libthyla-rs::weft`
(`slot_produce` / `slot_consume` / `slot_pending`), one source for the cross-Proc
memory ordering that both nocturned (consumer) and a client (producer) obey.

**The consumer** (`Shared::next_period`, the mixer): for each voice with a ring,
one `RingVoice::consume_period` call drains ONE period. `slot_consume` loads
`prod_tail` (Acquire), returns 0 if the ring is empty (`cons_head == prod_tail`,
i.e. silence this period), else snapshots `desc[i].len` ONCE and validates it
(`0 < len <= PERIOD_BYTES`, `len % FRAME == 0`; a bad len is DROPPED --
`dropped++`, `cons_head` advances, silence), copies `len` bytes from the
CONSUMER-COMPUTED offset (never `desc.addr` -- no client address is ever on the
read path, the I-30 discipline), then Release-bumps `cons_head`. The mixer sums
those S16 frames into the float32 accumulator scaled by the voice's gain, exactly
as it does a byte-FIFO voice; a voice is byte OR ring, never both, and voice 0
never has a ring.

**The producer** (a client): `slot_produce` loads `cons_head` (Acquire), returns
false WITHOUT touching the ring if it is FULL (`prod_tail - cons_head >= K`),
else writes the period into slot `prod_tail % K`, sets `desc[i].len`, and
Release-bumps `prod_tail` LAST. The Release/Acquire pair on `prod_tail` carries
the payload+len across the Proc boundary, so the consumer reads a slot only after
the producer published it (no torn period); the full-check stops the producer
overwriting a slot the consumer has not freed (`cons_head` is bumped AFTER the
copy, so the two never touch the same slot at once). Back-pressure on a full ring
is the caller's policy -- `/ring-voice-probe` yields (`t_yield`) and retries with
a stall backstop; a blocking producer (`torpor`) is the v1.x refinement.

**Start / stop with a ring** (`Shared::has_playable`): the device start condition
and the idle-stop read published-but-undrained ring periods (`slot_pending`) in
addition to byte-FIFO bytes, so a ring-only voice (empty FIFO) still starts and
holds the stream. A producer writing to a STOPPED stream generates no poll event
(the ring is shared memory, not an fd), so the serve loop's poll timeout is
shortened to `IDLE_POLL_MS` = 100 ms, which bounds a stopped stream's ring-notice
latency; a running stream wakes on the device IRQ every period, and byte writes
wake on the connection fd, so neither depends on the timeout. The wake POKE that
removes even the 100 ms latency (the producer signalling the consumer on a
stopped stream) is N-2b-2b.

## The cycle/control thread split (N-2c)

Through N-2b `nocturned` was single-threaded: one poll loop pumped the device
*and* served 9P. N-2c splits it into two threads (D-1c), so the audio clock is
never delayed by 9P work -- the foundation the in-cycle descants of N-4 require.

**The two threads.** `serve` leaks the `Shared` graph to `'static`, spawns the
**cycle thread** (`cycle_run`, `main.rs`) with a fresh page-aligned 128 KiB
`t_burrow_attach` stack, then the original thread runs `control_run`:

- **Cycle thread** -- owns the device (`VirtioSnd`, moved into a leaked
  `CycleCtx`). One iteration per device period: try_lock the graph, `pump`
  (reap completions + refill each freed slot from `Graph::next_period`, saving
  the mixed period), decide start/stop, publish stats, then wait on the period
  IRQ (`t_poll(irq_fd, IDLE_POLL_MS)`). It never touches 9P.
- **Control thread** -- the listener + connections + framing + dispatch +
  parked-write retry. It touches the graph only under the blocking lock and
  never across a 9P reply. It never touches the device.

**One lock, taken two ways** (`libthyla_rs::sync::Mutex`, reference 81). The
graph is `Shared { graph: Mutex<Graph>, wake: AtomicU32 }`; `Graph` holds the
voices, the id allocator, and the device stats/started the cycle publishes.

- The **cycle** `try_lock`s and NEVER blocks. On a miss (the control thread is
  mid-edit) it **replays the last mixed period** so the device never underruns:
  voices do not advance that period (they advance next period), so no data is
  lost and at most one ~10.7 ms period repeats. This is D-1c's "run last cycle's
  plan" in its minimal faithful form. Collisions are vanishingly rare (the
  control thread holds the lock only for microsecond-scale graph edits, never
  across I/O, vs a 10.7 ms period).
- The **control** thread `lock`s (blocking) for each graph op -- mint, push,
  gain, flush, remove, weft-ensure, render -- in a tight scope released before
  the reply, so the lock is never held across a 9P `send`.

**The wake poke** (`Shared::poke_cycle` / `cycle_park`). A stopped stream has no
IRQ, so the cycle thread parks on the `wake` futex with an `IDLE_POLL_MS`
backstop. When a byte write makes a voice playable (`h_write` / `poll_writes`)
AND the stream is stopped, the control thread bumps `wake` + `torpor::wake_one`,
so the stream starts within the poke's latency instead of the 100 ms backstop --
preserving the single-threaded era's instant byte-start. This is a *same-Proc*
wake (torpor works intra-Proc). A ring producer in *another* Proc still cannot
poke (no fd, no cross-Proc torpor), so its stopped-stream start rides the
backstop -- the N-2b-2b cross-Proc primitive remains the future removal of even
that. Register-then-observe on `wake` closes the poke-vs-park race (no lost
start).

**Why it is sound** (self-audit + the audit round): the `voices` Vec structure
is mutated only by the single control thread and only read by the cycle; every
graph field is touched under the one lock in both threads, so there is no data
race; the device is exclusively the cycle's; the poke never loses a start; and
there is exactly one lock, so no lock-order/deadlock surface. The mutex itself
is proven under real 4-CPU contention by the `alloc-smoke` sync leg
(reference 81).

## The probe and the witness

`/nocturne-probe` (native, libthyla-rs) is the N-2a-1 **mixing** witness: it
mints two voices through `/dev/nocturne/nodes/new`, opens each voice's `audio`,
and writes 1 kHz on one and 2 kHz on the other in **interleaved** 40 ms chunks
(`CHUNK_FRAMES` = 1920 = 48×40 = 24×80, a whole number of both cycles so reused
chunks splice seamlessly). Because each write parks when its voice's FIFO fills,
the interleave paces both voices to realtime and keeps both FIFOs fed — so the
mixer sums 1 kHz + 2 kHz into every device period. After ~1.2 s of the chord it
writes a silent tail, reads both voices' `info` + the root `info`, and prints
`NOCTURNE-PROBE PASS` iff both voices took all their bytes and `periods-played`
is non-zero. joey runs it in the boot-probe ladder right after the mount,
**fatal when the mount is up** (`joey: nocturne-probe OK`). The tone table is an
exact 48-entry sine (1 kHz at 48 kHz is 48 samples/cycle; step 2 is 2 kHz — no
floating point). The mint uses the shared mount, so the two voices persist past
the probe (see Voice lifetime above); a boot smoke does not care.

`tools/test-audio.sh` runs the verdict selftest, boots once with
`THYLACINE_AUDIODEV=wav` (QEMU's `wav` backend records everything the guest
plays to `build/audio-tone.wav`; playback-only, hence `streams=1`), requires the
guest-side `joey: nocturne-probe OK` line, and judges the FILE with
`tools/audio-verdict.py --chord`: 20 ms windows, RMS, a Goertzel per bin at the
two expected tones and four control bins; PASS iff ≥ 15 windows carry **BOTH**
tones at once (each expected bin 10:1 over every control bin, in the SAME
window — the mixing proof), one contiguous active span (≤ 10 % gaps), and the
capture ENDS with ≥ 0.2 s of silence, nothing loud outside (an empty mix yields
silence, never a repeated buffer or noise). Two facts about QEMU's `wav` backend
shaped the reader: it appends only while the guest's stream runs (so the file
begins with the first period played — no silent prefix to check) and it patches
the RIFF/data sizes only on a clean exit, which the harness never gives it (so
the reader ignores the header sizes and takes every frame after the `data`
header). The selftest proves discrimination on both verdicts: the chord passes
with/without a prefix and at 44.1 kHz, while a **sequential** capture (each tone
alone in its own windows — the N-1 shape), a single tone, silence, a missing
tail, noise after the tones and a gapped span all FAIL — the sequential-fails-
chord case is the control proving the witness checks *simultaneity* (mixing),
not mere presence.

`/ring-voice-probe` is the zero-copy ring witness, in two phases with one PASS.
N-2b-1 (the substrate): mint a voice, `SYS_WEFT_MAP` its `data` leaf through the
mount, validate the geometry (`WEFT_MAGIC`, K slots, a K-period payload), and run
two controls -- a second map is idempotent (same VA), and voice 0's map is
REFUSED. N-2b-2a (the period protocol): mint a SECOND ring voice and STREAM a
1 kHz + 2 kHz chord THROUGH the two rings (`weft::slot_produce` per period,
`t_yield` back-pressure), 1 kHz into voice A and 2 kHz into voice B in lockstep so
nocturned's mixer sums two RING voices into every device period. Guest-side
corroborators: `dropped == 0` on both rings (no period failed the consumer's len
validation) and `periods-played > 0`. `tools/test-ring-voice.sh` boots it WITHOUT
a capture (the substrate + the producer/consumer path; a wedged ring fails the
probe). `tools/test-ring-audio.sh` boots with `THYLACINE_AUDIODEV=wav` AND
`thylacine.ringprobe`, which makes joey run `/ring-voice-probe` INSTEAD of the
byte `/nocturne-probe` -- the two are exclusive (one wav, one chord span), so the
ring is the ONLY audio in the capture and any chord present came through the
zero-copy path -- then judges the FILE with `audio-verdict.py --chord` (the same
mixing verdict as the byte path). That exclusivity is the control that this
witnesses the DATA path, not the byte path.

## QEMU wiring (`tools/run-vm.sh`)

The function is present on every boot (`virtio-sound-pci,id=snd-pci0,
audiodev=snd0,streams=1,disable-legacy=on`), placed after `rng_pci0` and
before the poll-mode mouse so its INTx line is distinct from the NIC's and the
GPU's (the `nocturned: ... intid=N` boot line is the witness; an exclusivity
clash fails the IRQ claim at probe). The backend is `THYLACINE_AUDIODEV`:
`none` (default), `wav` (+ `THYLACINE_AUDIO_WAV`, fixed 48 kHz S16 stereo),
`coreaudio`, `pipewire`/`pa`/`alsa`/`sdl`/`dbus`/`oss`/`jack`;
`THYLACINE_NO_AUDIO=1` removes the device.

## Measured (2026-09-05, HVF, the default build)

Boot lines (every boot, `build/test-boot.log`):

```
warden: bind virtio-pci:25 (0.6.0) -> nocturned inst=0 [mmio=0 irq=1 dma=0x40000 pci=Some((0, 6, 0))] restart=OnCrash
nocturned: virtio-snd features lo=0x79000000 hi=0x00000101 jacks=0 streams=1 chmaps=0 intid=37
nocturned: stream 0: dir=0 ch=1..2 features=0x0 formats=0xe0078 rates=0x3fff
nocturned: serving /srv/nocturne (virtio-snd playback; s16c2r48000 period 2048 B x 4)
warden: nocturned pid=2216 up (READY) -> serving (persistent; left running)
joey: /dev/nocturne mounted (nocturned tree)
joey: nocturne-probe OK (1 kHz + 2 kHz over /dev/nocturne/audio; Nocturne N-1)
```

The probe's `info` read, taken the instant its last write returned (the FIFO
still holds what the device has not yet drained):

```
periods-played 77   silence-periods 0   tx-errors 0   bad-used 0
latency-bytes 0     buffered 64512      bytes-in 230400   started 1
```

QEMU's device reports `latency_bytes` = 0 in every status (a fact about the
device, recorded, not relied on). The wav capture from `tools/test-audio.sh`:
319488 bytes = 1.66 s at 48 kHz S16 stereo — the probe's 1.2 s plus the
idle-stop silence — and the verdict
`PASS: 1000 Hz x 25 windows (median 12), 2000 Hz x 25 windows (median 37);
silent tail 33 windows (prefix 0); ambiguous 0; rate 48000; 83 windows total`.
The whole `tools/test-audio.sh` run (selftest + one boot + verdict) took a
single boot's wall time.

## Error paths

| Where | Condition | Result |
|---|---|---|
| `probe` | claim / region / IRQ / DMA / handshake / PCM_INFO / SET_PARAMS / PREPARE fails or times out (2 s) | `Err(Hardware)` → `EXIT_PROBE`; the warden's restart policy applies; joey sees `/srv/nocturne` absent (soft) |
| `serve` | `/srv/nocturne` post fails | `Err(Hardware)` → `EXIT_SERVE` |
| `audio` write | FIFO full | parked (deferred `Rwrite`), `ENOMEM` past 8 parked writes per connection |
| `audio`/`info`/`ctl` | bad verb / not writable | `EINVAL` / `EPERM` |
| voice `audio`/`ctl` | write/`ctl` to a non-zero voice from a non-owning connection | `EPERM` (owned elsewhere) / `EBADF` (unknown id) |
| `serve` accept | connection table full (`MAX_CONNS` = 32) | accepted then closed at once (fail-fast EOF) |
| `Tweft` on `data` | not the owning connection / gone / voice 0 / not a `data` leaf | `EPERM` / `EBADF` / `EINVAL` / `EINVAL` |
| `Tweft` on `data` | ring alloc / share fails (OOM, registry full) | `ENOMEM` |
| `data` leaf | `Tread` / `Twrite` (it is a map fid) | `EINVAL` / `EPERM` |
| device | bogus used id | dropped, `bad-used`++ (never re-posted) |
| device | status ≠ `S_OK` | `tx-errors`++ (the slot is still re-posted) |

## Known caveats / seams

- No cooperative quiesce-on-remove: the warden's `DeviceRemoved` is a forced
  group-terminate that skips `Drop` (the netdev precedent; MENAGERIE §10).
- Independent voices now MIX (N-2a-1); voice 0 (the root `audio` file) and every
  `nodes/new` voice sum cleanly, whether a voice feeds bytes (the FIFO) or the
  zero-copy ring (N-2b-2a). What is NOT built: the back-pressure wake POKE
  (N-2b-2b -- a producer on a stopped stream waits up to `IDLE_POLL_MS`),
  ports/links, ears/`source` (capture; N-3), descants + the cadence lease (N-4),
  and per-format/rate conversion at voice entry (D-3; N-2a-1 accepts only S16
  stereo 48 kHz -- a voice at another shape is a future entry-conversion seam).
- Voices minted through the shared `/dev/nocturne` mount persist for the mount's
  life; per-exit lifetime needs a direct `/srv/nocturne` connection -- what the
  SDL backend does (N-2a-2, reference 142).
- Ported GAME audio reaches Nocturne through that SDL backend, WITNESSED at
  N-2a-4 (2026-09-06): DOSBox-X's built-in mixer (SoundBlaster 16 + OPL; the
  shipped Duke Nukem 3D `DUKE3D.CFG` selects `FXDevice=0`/`MusicDevice=0` = the
  SB) and tyr-glquake's `snd_sdl` both play through it. `tools/test-game-audio.sh`
  is the W-4 wav witness (`audio-verdict.py --music`); a clade-baked game image
  (tyr-glquake at `/clade/bin`) boots `--production` to skip the boot-fatal clade
  gates, and the witness accepts that `THYLA_BOOT_PROBES=OFF` boot as a clean-wav
  guarantee (the audio probe -- the only boot-time audio source -- never runs).
  The thyla-pi SILICON audio leg is N-6 (the game `.exp` force `hvf`).
- The N-2b zero-copy ring shares the F5 limitation: a ring voice minted via
  the shared `/dev/nocturne` mount is owned by the one kernel dev9p connection,
  so the `h_weft` owner gate does not isolate mounted clients from each other --
  the kernel's consume-once share claim is what keeps the ring single-producer.
  The N-2b batched holotype audit (N-2b-1 + N-2b-2a, the first cross-Proc DATA
  path in nocturned; I-37) CLOSED CLEAN: 0 P0 / 0 P1 / 0 P2, 3 P3 fixed. The
  prosecutor re-derived the cross-Proc SPSC memory ordering (each Release/Acquire
  required and present -- no torn read, no in-flight overwrite), the consumer's
  memory safety against a fully-hostile ring (own trusted geometry bounds every
  index; desc.len is the only client value on the read path, snapshot-once +
  validated), the #847 dual-count lifetime, and the consume-once SPSC backstop --
  all sound. P3 fixes: the probe control-(b) attribution (id==0 AND owner), a
  time-based (not yield-count) back-pressure stall bound, a "sole producer thread"
  safety note, and a compile-time K*PERIOD_BYTES-fits-the-payload assert.
- The whole N-1..N-2a-4 surface was adversarially audited (round 1 Fable +
  round 2 Opus; `memory/audit_nocturne_closed_list.md`). Deferred by design
  (F5, P2): a voice minted via the shared mount is box-wide and outlives its
  writer until the mount closes -- a bounded, graceful DoS (`ENOMEM` at
  `MAX_VOICES` = 16, never a crash); the proper fix (per-voice fid-refcount
  lifetime or a per-principal connection cap) is an N-2c/N-3 design decision.
- `nodes/<id>/ctl gain` is Plan 9 `volume`-style percent, not the dB grammar the
  design's `volume` file (N-3) will carry; the per-link/stage dB gains are N-3+.
- The virtio-pci-modern constants are a private copy of netdev's (a hoist seam).
- The cycle/control thread split (`docs/NOCTURNE.md` §6.2 D-1c) LANDED at N-2c
  (@dd07836a). The N-2c holotype audit closed CLEAN (0 P0/P1/P2, 2 P3, both
  deferred): **F2 [P3, bounded]** -- the cycle holds the graph lock across
  `snd.start()`/`stop()`, which issue virtio-snd control RPCs, so a wedged device
  freezes the control plane for a BOUNDED interval (each rpc caps at
  `CTRL_WAIT_STEPS`=2000 x 1 ms; `stop()` = 3 rpcs ~= 6 s worst case) -- not a
  deadlock (the device never takes the graph lock), not a regression (the
  pre-N-2c serve loop also blocked on device rpcs). The optional refinement
  (snapshot the priming periods under the lock, drop it, then do the device
  transition) is a v1.x item; `start()` genuinely needs the lock (it primes via
  `next_period`), `stop()` does not. **F1 [P3, pre-existing N-2a, NOT N-2c]** --
  see the mixer caveat below.
- **F1 (pre-existing, deferred):** `has_playable` returns true whenever the
  cross-voice byte SUM is non-zero, but `next_period` drains only whole frames,
  so a sub-frame residue (1-3 bytes, e.g. a client writing a byte count not
  divisible by `FRAME`=4 then stopping) sticks in the FIFO and keeps
  `has_playable` true forever -- the idle-stop never fires and the cycle wakes
  every period re-mixing silence. Byte-identical in the parent 46d51fbb (N-2a
  mixer behavior); N-2c only moved the caller into the cycle thread. Fix (a
  focused follow-up, with a residue-idle-stop regression witness): make
  "playable" per-voice -- `voices.iter().any(|v| v.fifo.len() >= FRAME || ring
  pending)` (the per-voice `>= FRAME` test, not the cross-voice sum, since two
  voices with 2 residual bytes each sum to 4 while neither holds a frame).
- The wav witness covers playback only (QEMU's `wav` backend has no capture
  voice); the capture-side witness needs a non-wav backend (N-3).
