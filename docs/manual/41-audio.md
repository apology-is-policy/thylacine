# Audio — Nocturne

**Status: N-1 (2026-09-05).** The audio system is being built (`docs/NOCTURNE.md`).
What exists today is the heritage floor: a Plan 9-shaped audio device file you
can write PCM to. Mixing several programs, volume control, capture, the graph,
and game sound arrive in the following chunks; this page grows with them.

## Overview

Sound on Thylacine is served by `nocturned`, a userspace driver bound by the
warden to the machine's `virtio-sound` function (device id 25). It mounts its
tree at `/dev/nocturne`. On a machine without a sound function the tree is
absent and everything below reports "no such file" — there is no stub device.

```
/dev/nocturne/
  audio    write: play S16LE stereo PCM at 48000 Hz. read: 0 bytes (output only)
  info     read: the audiostat words (bufsize, buffered) + the driver's counters
  ctl      read: a one-line description; write: `flush` (drop the queued audio)
```

This is the shape of Plan 9's `audio(3)`: one file to write samples to, one to
read the buffered-byte count from. `bind /dev/nocturne/audio /dev/audio` gives a
namespace a 9front-style `/dev/audio`.

## Getting started

Play a raw PCM file (signed 16-bit little-endian, stereo, 48 kHz — the format
`pcmconv` on 9front calls `s16c2r48000`):

```
cat music.s16 > /dev/nocturne/audio
```

The write blocks while the driver's queue is full, so the command returns when
the last bytes have been *queued*, not when they have been heard: a further
~40 ms of audio is still in flight in the device at that point.

See what the device is doing:

```
cat /dev/nocturne/info
```

```
device virtio-snd stream 0 playback
format s16c2r48000
bufsize 2048
buffered 64512
period-bytes 2048
buffer-bytes 8192
periods 4
started 1
periods-played 77
silence-periods 0
tx-errors 0
bad-used 0
latency-bytes 0
bytes-in 230400
flushes 0
```

`bufsize` is the preferred write unit (one period); `buffered` is the number of
bytes queued for output (the driver's own queue plus what the device reports it
still holds) — the one number Plan 9 uses as its latency interface. A
`silence-periods` count that grows while you are playing means your writer is
not keeping up (an underrun: the driver fed silence to keep the stream's time).

Drop whatever is queued (a stuck player, a wrong file):

```
echo flush > /dev/nocturne/ctl
```

## Reference

| File | Mode | Read | Write |
|---|---|---|---|
| `audio` | `0666` | returns 0 bytes (an output-only device, per `audio(3)`) | S16LE stereo 48000 Hz; whole frames (4 bytes) are consumed, a trailing partial frame is dropped; blocks when the queue (64 KiB, ~340 ms) is full |
| `info` | `0444` | the text above | not writable |
| `ctl` | `0644` | a one-line description of the device | `flush` |

The stream starts on the first write and stops on its own after about half a
second of silence, so an idle machine pays no periodic interrupt.

## Choosing the host backend (QEMU)

The guest always has the device; the host decides where the sound goes.
`tools/run-vm.sh` reads:

| `THYLACINE_AUDIODEV` | Effect |
|---|---|
| `none` (default) | no host sound; the guest plays into the void (the driver path still runs on every boot) |
| `wav` | record everything the guest plays to `THYLACINE_AUDIO_WAV` (default `build/audio-capture.wav`) — the gate witness, playback-only |
| `coreaudio` | the mac's speakers |
| `pipewire`, `pa`, `alsa`, `sdl`, `dbus`, `oss`, `jack` | the Linux host's sound server (thyla-pi runs PipeWire) |

`THYLACINE_NO_AUDIO=1` removes the device entirely.

## Differences from Linux

- There is no `/dev/snd`, no ALSA and no PulseAudio socket yet. Linux binaries
  under VIVARIUM get audio through a PulseAudio-protocol server in a later
  chunk (`docs/NOCTURNE.md` §6.11).
- One format, one rate at N-1. Sample-rate and format conversion at the device
  boundary arrive with the mixer (N-2); until then convert on the way in.
- Only one program's writes are meaningful at a time: two writers interleave
  their bytes in the queue. The mixer (N-2) makes concurrent writers mix.

## Troubleshooting

- **`/dev/nocturne` does not exist.** The VM was started with
  `THYLACINE_NO_AUDIO=1`, or the warden found no `virtio-pci:25` function; the
  boot log then says `joey: /srv/nocturne absent`.
- **Nothing is heard on the host.** `THYLACINE_AUDIODEV` is `none` (the
  default). Use `coreaudio` on the mac or `pipewire` on thyla-pi.
- **The writer stalls forever.** The device stopped consuming; `info` will show
  `tx-errors` or `bad-used` growing. Report it with the boot log's
  `nocturned:` lines.

## See also

`docs/NOCTURNE.md` (the design), `docs/reference/153-nocturne.md` (the
as-built driver and server), `tools/test-audio.sh` (the wav witness).
