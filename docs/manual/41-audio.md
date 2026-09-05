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

## Multiple streams (voices)

`/dev/nocturne/audio` is one *voice*. Two programs writing it at once would
interleave their bytes, so a program that wants its own stream mints its own
voice: open `nodes/new`, and its read gives you the new voice's id. Then write
that voice's `audio`:

```
id=`cat /dev/nocturne/nodes/new`
cat music.s16 > /dev/nocturne/nodes/$id/audio
```

Every voice is mixed into the one sink, so several can play at once — a game's
effects over a music player, say. Each voice has its own gain (a percent, 100 =
unity, Plan 9 `volume`-style):

```
echo gain 50 > /dev/nocturne/nodes/$id/ctl   # this voice at half volume
echo flush   > /dev/nocturne/nodes/$id/ctl   # drop just this voice's queue
echo remove  > /dev/nocturne/nodes/$id/ctl   # done with it
```

A voice you minted through the `/dev/nocturne` mount lives as long as the mount
does; a program that connects to `/srv/nocturne` directly gets a voice that dies
when it exits. `cat /dev/nocturne/nodes/$id/info` shows that voice's gain, queued
bytes, and totals; the root `info` gains a `voices N` line.

## Reference

| File | Mode | Read | Write |
|---|---|---|---|
| `audio` | `0666` | returns 0 bytes (an output-only device, per `audio(3)`) | S16LE stereo 48000 Hz into voice 0; whole frames (4 bytes) consumed, a trailing partial dropped; blocks when the queue (64 KiB, ~340 ms) is full |
| `info` | `0444` | the device text above, plus `voices N` | not writable |
| `ctl` | `0644` | a one-line description | `flush` |
| `nodes/new` | `0666` | the id of the voice this open minted | opening it is the mint |
| `nodes/<id>/audio` | `0666` | 0 bytes | S16LE stereo 48000 Hz into voice `<id>` |
| `nodes/<id>/ctl` | `0644` | a one-line description | `gain <percent>` / `flush` / `remove` |
| `nodes/<id>/info` | `0444` | that voice's gain, queued bytes, totals | not writable |

The stream starts on the first write to any voice and stops on its own after
about half a second with every voice silent, so an idle machine pays no periodic
interrupt. Up to 16 voices mix at once.

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
