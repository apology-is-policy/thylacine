# 153 — Nocturne: `nocturned`, the virtio-snd driver + the mixed-voice graph core

**Status:** N-1 AS-BUILT (2026-09-05, @562cbe50) + N-2a-1 AS-BUILT (2026-09-05).
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

Framing + dispatch mirror `usr/ptyfs` (one `t_read` per readable event, every
complete frame dispatched, `Disp::{Reply,Deferred,Fatal}`). N-2a-1 grows the
static N-1 tree into a voice graph:

| Path | qid | Mode | Read | Write |
|---|---|---|---|---|
| `/` | 0 | `0555` dir | `Treaddir` lists `ctl info audio nodes` | — |
| `audio` | 3 | `0666` | 0 bytes (output-only, `audio(3)`) | S16 stereo 48 kHz into **voice 0** |
| `info` | 2 | `0444` | device words + counters + `voices N` | `EPERM` |
| `ctl` | 1 | `0644` | one description line | `flush` (drops voice 0); else `EINVAL` |
| `nodes/` | 4 | `0555` dir | `Treaddir` lists `new` + each live voice id | — |
| `nodes/new` | 5 | `0666` | the id of the voice this open minted | (open is the mint) |
| `nodes/<id>/audio` | vpath | `0666` | 0 bytes | S16 stereo 48 kHz into voice `<id>` |
| `nodes/<id>/ctl` | vpath | `0644` | one line | `gain <percent>` / `flush` / `remove` |
| `nodes/<id>/info` | vpath | `0444` | that voice's stats | `EPERM` |

Voice paths encode `VBIT | (id << 4) | leaf` (leaf 0 = dir, 1/2/3 =
audio/ctl/info) — the tapestry `surf_n`/`surf_fk` idiom, so one `u64` qid names
both the voice and the file within it.

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
- Independent voices now MIX (N-2a-1); voice 0 (the root `audio` file) and every
  `nodes/new` voice sum cleanly. What is NOT built: the zero-copy Weft ring
  (`nodes/<id>/data`; N-2b), ports/links, ears/`source` (capture; N-3),
  descants + the cadence lease (N-4), and per-format/rate conversion at voice
  entry (D-3; N-2a-1 accepts only S16 stereo 48 kHz — a voice at another shape
  is a future entry-conversion seam).
- Voices minted through the shared `/dev/nocturne` mount persist for the mount's
  life; per-exit lifetime needs a direct `/srv/nocturne` connection -- what the
  SDL backend does (N-2a-2, reference 142).
- `nodes/<id>/ctl gain` is Plan 9 `volume`-style percent, not the dB grammar the
  design's `volume` file (N-3) will carry; the per-link/stage dB gains are N-3+.
- The virtio-pci-modern constants are a private copy of netdev's (a hoist seam).
- The serve loop is still single-threaded; the cycle/control thread split
  (`docs/NOCTURNE.md` §6.2 D-1c) is N-2c.
- The wav witness covers playback only (QEMU's `wav` backend has no capture
  voice); the capture-side witness needs a non-wav backend (N-3).
