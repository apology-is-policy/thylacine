# Audio — Nocturne

**Status: N-2c (2026-09-06).** The audio system is being built (`docs/NOCTURNE.md`).
What exists today: a Plan 9-shaped audio device file you can write PCM to,
several programs mixing at once through their own *voices*, per-voice gain, and
SDL programs playing through it automatically. Since N-2c the driver runs the
audio clock on its own thread, separate from the one serving the device files --
an internal change (nothing you do differs) that keeps the sound steady while
the graph is edited and lays the ground for low-latency effects. Capture, the
graph's ports and links, and the games' sound switch-on arrive in the following
chunks; this page grows with them.

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
| `volume` | `0666` | the current `audio`/`mix` levels per channel (Plan 9 `volume(3)`) | `audio 50` / `audio 70 90` / `mix 40` (0..100; `audio 0` mutes) -- if you are allowed (below) |
| `nodes/new` | `0666` | the id of the voice this open minted | opening it is the mint |
| `nodes/<id>/audio` | `0666` | 0 bytes | S16LE stereo 48000 Hz into voice `<id>` |
| `nodes/<id>/ctl` | `0644` | a one-line description | `gain <percent>` / `flush` / `remove` |
| `nodes/<id>/info` | `0444` | that voice's gain, queued bytes, totals | not writable |
| `nodes/<id>/data` | `0666` | a zero-copy ring map fid (not byte I/O) | `SYS_WEFT_MAP` it (see below); reading/writing it as bytes is refused |

The stream starts on the first write to any voice and stops on its own after
about half a second with every voice silent, so an idle machine pays no periodic
interrupt. Up to 16 voices mix at once.

## Setting the volume

The root `volume` file is the system volume, in Plan 9 `volume(3)` grammar over
the active sink. Write one or more lines:

```
audio 50        # both channels to 50% (about -6 dB)
audio 70 90     # left 70%, right 90%
mix 40          # the master (mix) stage to 40%
audio 0         # mute
audio 100       # full (unity)
```

and read it back for the current levels:

```
% cat /dev/nocturne/volume
audio 100 100
mix 100 100
```

The effective gain per channel is `audio` times `mix` (both 0..100, 100 =
unity), applied to the mixed output -- `audio` the playback level, `mix` the
master, like Plan 9's mixfs. Per-stream volume is a voice's own `ctl gain`
(above); this file is the whole sink.

**Who may set it.** The sink is system-owned, so changing the system volume
needs authority beyond merely seeing `/dev/nocturne`: you must be the person at
the console (the trusted-path session), the system, an admin (`CAP_HOSTOWNER`),
or hold the `audio-graph` clearance (granted by an administrator through the
`cap` device). An ordinary app that only plays a voice cannot change the whole
sink; a program that is refused gets `permission denied`. The volume OSD and a
session's own shell (at the keyboard) can.

Authority is judged per connection: the volume works from a program connected
directly to `/srv/nocturne` under its OWN identity (an audio app, the OSD). A
write through a shared `/dev/nocturne` mount instead carries the mounter's
identity -- so in a container that shares the host's mount, the write counts as
the host's, not the container's. Give a container its own audio connection if
you want its volume changes judged as the container's.

## Zero-copy ring (advanced)

Writing a voice's `audio` copies your bytes into the server. A program that wants
no copy — a game engine, a synth — can instead map that voice's ring and write
audio periods straight into shared memory the server plays from. Open the voice's
`data` file **through the `/dev/nocturne` mount** and `SYS_WEFT_MAP` it (a direct
`/srv/nocturne` connection cannot — the map needs a mounted fd); the server hands
back a shared ring you write fixed-size periods into, advancing a producer index
the server reads on the audio clock. One program produces per ring (the kernel
grants the ring to the first mapper and no other), so a ring voice is byte-fed
*or* ring-fed, never both. This is a low-level interface; most programs should use
`audio` or an audio library (SDL, below). The wire layout and the producer/consumer
protocol are in the developer reference (`docs/reference/153-nocturne.md`).

## Programs: SDL

A program built on the SDL2 port gets sound with no code of its own: SDL's
`thylacine` audio driver (`usr/ports/sdl2/thylacine/SDL_thylacineaudio.c`) is
selected automatically whenever `/srv/nocturne` exists, and `SDL_OpenAudioDevice`
mints the program a private voice. Ask SDL for any rate and format you like —
it converts to the device's 48 kHz S16 stereo on the way out. The voice lives
exactly as long as the program: it is reaped the instant the program exits,
even by a crash, so nothing lingers in `nodes/`.

```
SDL_AUDIODRIVER=thylacine   # the default when sound exists; `dummy` to silence a program
```

Latency is up to ~340 ms (the voice's queue depth) on this first, byte-copy
path; fine for music and effects, noticeable for tight rhythm games. A later
chunk (the zero-copy ring, N-2b) trims it.

Quake has sound: `tyr-quake` (and `tyr-glquake`) play through the SDL driver,
so the game's effects come out of whatever host backend you chose below. Pass
`-nosound` to silence it; `quarry`'s bench lanes do that themselves so a
benchmark's frame rate is not a property of the sound path.

DOSBox-X games have sound too (N-2a-4): the emulator's built-in mixer (a
SoundBlaster 16 + OPL, the cards a DOS game expects) reaches Nocturne through the
same SDL driver, so a DOS game's music and effects come out of your chosen host
backend. A game's own setup decides which card it uses -- the shipped Duke
Nukem 3D config selects the SoundBlaster, and `cat music.s16 > /dev/nocturne/audio`
still works alongside it (each is its own voice). DOSBox-X is built through a
separate toolchain, so it is present only in images baked with that toolchain.

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
- Two programs writing the SAME file (`audio`, or one voice's `audio`)
  interleave their bytes; give each program its own voice (above) and they mix.

## Troubleshooting

- **`/dev/nocturne` does not exist.** The VM was started with
  `THYLACINE_NO_AUDIO=1`, or the warden found no `virtio-pci:25` function; the
  boot log then says `joey: /srv/nocturne absent`.
- **Nothing is heard on the host.** `THYLACINE_AUDIODEV` is `none` (the
  default). Use `coreaudio` on the mac or `pipewire` on thyla-pi.
- **An SDL program is silent.** `/srv/nocturne` was absent when it started
  (see the first item): SDL fell back to its `dummy` driver, so the program
  runs without sound. Sound cannot be added to a running program; restart it
  once the device exists.
- **The writer stalls forever.** The device stopped consuming; `info` will show
  `tx-errors` or `bad-used` growing. Report it with the boot log's
  `nocturned:` lines.

## See also

`docs/NOCTURNE.md` (the design), `docs/reference/153-nocturne.md` (the
as-built driver and server), `docs/reference/142-sdl-port.md` (the SDL audio
backend), `tools/test-audio.sh` + `tools/test-sdl-audio.sh` (the wav witnesses).
