# 153 — Nocturne N-1: `nocturned`, the virtio-snd driver + the Plan 9 audio file

**Status:** N-1 AS-BUILT (2026-09-05; the hash is in the N-1 close row of
`docs/AUX-ROADMAP.md`). The design is `docs/NOCTURNE.md`; this chapter is what
exists in the tree: one warden-bound daemon owning the `virtio-sound` function,
one playback stream, one 9P tree (`/dev/nocturne/{audio,info,ctl}`), one boot
probe, one host-side witness. The graph, the rings, voices/ears/descants, the
mixer and the policy are N-2 onward (`docs/NOCTURNE.md` §8).

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
(`snd::VirtioSnd::open`); `serve` posts `/srv/nocturne`, writes the one `READY`
line the warden waits for, and runs a single poll loop over the listener, the
9P connections and the IRQ fd (the IRQ handle is pollable: readable at a
pending count ≥ 1). joey mounts `/srv/nocturne` at `/dev/nocturne` (MREPL over
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
hanging the warden's bind ladder.

**Idle stop** (`stop()`): after `IDLE_STOP_PERIODS` (48, ≈0.5 s) consecutive
silence periods with an empty FIFO, `PCM_STOP` + `PCM_RELEASE` are issued, the
flushed completions are reaped **without** re-posting, every in-flight bit is
cleared, and `PCM_PREPARE` re-arms the stream for the next `start`. An idle
machine therefore pays no periodic interrupt.

## The server half (`server.rs`)

The `/srv/nocturne` tree is a static three-file directory; framing + dispatch
mirror `usr/ptyfs` (one `t_read` per readable event, every complete frame
dispatched, `Disp::{Reply,Deferred,Fatal}`).

| Path | qid | Mode | Read | Write |
|---|---|---|---|---|
| `/` | 0 | `0555` dir | `Treaddir` lists the three | — |
| `audio` | 3 | `0666` | 0 bytes (output-only, `audio(3)`) | S16LE stereo 48 kHz into the FIFO |
| `info` | 2 | `0444` | the audiostat words + counters (offset-served text) | `EPERM` |
| `ctl` | 1 | `0644` | one description line | `flush` (drops the FIFO); anything else `EINVAL` |

**The FIFO + the parked write.** `Shared.fifo` is a `VecDeque<u8>` capped at
`FIFO_CAP` = 64 KiB (≈340 ms). A `Twrite` to `audio` pushes what fits and, if
anything remains, PARKS: the bytes are copied into a `PendingWrite {tag, fid,
data, done}` on the connection, `Disp::Deferred` withholds the reply, and every
serve-loop pass (`poll_writes`, after the device pump freed room) pushes more
until the write completes, at which point `Rwrite(total)` is sent — Plan 9's
blocking write. Order is preserved (a write behind a parked one queues behind
it); `MAX_PENDING_WRITES` = 8 per connection bounds the park list (`ENOMEM`
beyond); a clunk drops that fid's parked writes, `teardown` drops them all,
and `Tflush(oldtag)` cancels exactly the parked write with that tag (bytes
already accepted stay queued and play). `next_period` hands the device whole
frames only and pads a partial period with silence.

`info` renders:

```
device virtio-snd stream 0 playback
format s16c2r48000
bufsize 2048            # the preferred write unit (one period)
buffered <fifo + device-reported latency_bytes>
period-bytes 2048
buffer-bytes 8192
periods 4
started 0|1
periods-played N
silence-periods N       # periods fed silence (an empty FIFO)
tx-errors N
bad-used N
latency-bytes N
bytes-in N
flushes N
```

## The probe and the witness

`/nocturne-probe` (native, libthyla-rs) opens `/dev/nocturne/audio` for write
and streams 0.5 s of 1 kHz, 0.5 s of 2 kHz, then 0.2 s of silence in 8 KiB
chunks (an exact 48-entry sine table: 1 kHz at 48 kHz is 48 samples per cycle;
index step 2 is 2 kHz — no floating point), reads `info` back, and prints
`NOCTURNE-PROBE PASS` iff `periods-played` is non-zero. joey runs it in the
boot-probe ladder right after the mount, **fatal when the mount is up**
(`joey: nocturne-probe OK (1 kHz + 2 kHz over /dev/nocturne/audio; Nocturne N-1)`).

`tools/test-audio.sh` runs the verdict's 7-case selftest, boots once with
`THYLACINE_AUDIODEV=wav` (QEMU's `wav` backend records everything the guest
plays to `build/audio-tone.wav`; it is playback-only, hence `streams=1`),
requires the guest-side `joey: nocturne-probe OK` line, and judges the FILE
with `tools/audio-verdict.py`: 20 ms windows, RMS, a Goertzel per bin at the
expected tones and four control bins; PASS iff the tone span is contiguous
(≤ 10 % silent windows inside it), ≥ 15 windows are dominated 10:1 by 1 kHz
and ≥ 15 by 2 kHz, the 1 kHz median index precedes the 2 kHz one (the positive
control: a different tone lands in a different bin, in the right order), and
the capture ENDS with ≥ 0.2 s of silence with nothing loud outside the span
(the negative control: an empty FIFO yields silence, never a repeated buffer
or noise). Two facts about QEMU's `wav` backend shaped this: it appends only
while the guest's stream runs, so the file begins with the first period played
(no silent prefix exists to check), and it patches the RIFF/data sizes only on
a clean exit, which the harness never gives it — the reader ignores the header
sizes and takes every frame after the `data` chunk header. The nine-case
selftest proves discrimination: the signature passes with and without a
prefix and at 44.1 kHz; the reversed order, silence, a single tone, a missing
tail, noise after the tones and a gapped span all fail.

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
| device | bogus used id | dropped, `bad-used`++ (never re-posted) |
| device | status ≠ `S_OK` | `tx-errors`++ (the slot is still re-posted) |

## Known caveats / seams

- No cooperative quiesce-on-remove: the warden's `DeviceRemoved` is a forced
  group-terminate that skips `Drop` (the netdev precedent; MENAGERIE §10).
- Concurrent writers interleave bytes (one stream; the N-2 mixer fixes this).
- One format/rate; conversion arrives with the mixer (N-2).
- The virtio-pci-modern constants are a private copy of netdev's (a hoist seam).
- `eventq`/`rxq` unset; capture is N-3.
- The wav witness covers playback only (QEMU's `wav` backend has no capture
  voice).
