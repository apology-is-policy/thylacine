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
| `audio` | `0666` | *refused* -- recording is the gated `tap` (below), never the shared mount | S16LE stereo 48000 Hz into voice 0; whole frames (4 bytes) consumed, a trailing partial dropped; blocks when the queue (64 KiB, ~340 ms) is full |
| `info` | `0444` | the device text above, plus `voices N` | not writable |
| `ctl` | `0644` | a one-line description | `flush` |
| `volume` | `0444` (read-only in the mount) | the current `audio`/`mix` levels per channel (Plan 9 `volume(3)`) | not through the mount -- writes go to `nocturne-vol` / `/srv/nocturne-ctl` (below) |
| `tap` (on `/srv/nocturne-ctl`) | `0444` | the mixed sink output as S16LE stereo -- recording; gated (see "Recording" below), one reader at a time | not writable |
| `source` (on `/srv/nocturne-ctl`) | `0444` | a capture device (mic / line-in) as S16LE stereo -- recording; same gate, one reader at a time, on-demand; `ENODEV` if no capture device | not writable |
| `nodes/new` | `0666` | the id of the voice this open minted | opening it is the mint |
| `nodes/<id>/audio` | `0666` | 0 bytes | S16LE stereo 48000 Hz into voice `<id>` |
| `nodes/<id>/ctl` | `0644` | a one-line description | `gain <percent>` / `flush` / `remove` |
| `nodes/<id>/info` | `0444` | that voice's gain, queued bytes, totals | not writable |
| `nodes/<id>/data` | `0666` | a zero-copy ring map fid (not byte I/O) | `SYS_WEFT_MAP` it (see below); reading/writing it as bytes is refused |

The stream starts on the first write to any voice and stops on its own after
about half a second with every voice silent, so an idle machine pays no periodic
interrupt. Up to 16 voices mix at once.

## Setting the volume

Read the current system volume with `cat` (public info):

```
% cat /dev/nocturne/volume
audio 100 100
mix 100 100
```

Set it with `nocturne-vol` (Plan 9 `volume(3)` grammar over the active sink):

```
% nocturne-vol audio 50      # both channels to 50% (about -6 dB)
% nocturne-vol audio 70 90   # left 70%, right 90%
% nocturne-vol mix 40        # the master (mix) stage to 40%
% nocturne-vol audio 0       # mute
% nocturne-vol audio 100     # full (unity)
% nocturne-vol               # print the current levels
```

The effective gain per channel is `audio` times `mix` (both 0..100, 100 =
unity), applied to the mixed output -- `audio` the playback level, `mix` the
master, like Plan 9's mixfs. Per-stream volume is a voice's own `ctl gain`
(above); this is the whole sink.

**Who may set it.** The sink is system-owned, so changing the system volume
needs authority beyond merely seeing `/dev/nocturne`: you must be the session at
the keyboard (the console-owner session -- the trusted-path idiom), the system,
an admin (`CAP_HOSTOWNER`), or hold the `audio-graph` clearance (granted by an
administrator through the `cap` device). An ordinary app that only plays a voice
cannot change the whole sink; a program that is refused gets `permission denied`.

**Why `nocturne-vol` and not `echo > /dev/nocturne/volume`.** The mounted
`/dev/nocturne/volume` is read-only: a mount is a single connection and carries
the mounter's identity (the system), so a write through it could never be judged
as *you*. `nocturne-vol` instead opens its own connection to the sink-authority
service `/srv/nocturne-ctl`, where the audio server sees the real caller -- so
your clearance, or your being the session at the keyboard, is what the gate
checks. A container that is not given `/srv/nocturne-ctl` simply cannot change
the host volume, which is the point.

## Recording the system audio

A read of the sink **tap** captures the mixed output -- everything the machine is
playing, as S16LE stereo at the sink rate. Because that is recording *every*
program's audio, it is an eavesdropping surface, so it is gated exactly like the
volume (the trusted-path idiom): you must be the session at the keyboard, the
system, an admin (`CAP_HOSTOWNER`), or hold the `audio-graph` clearance.

The tap lives on the sink-authority service, not the mount -- a read of
`/dev/nocturne/audio` is refused, for the same reason the mounted `volume` is
read-only (a shared mount carries the mounter's identity, so it could never judge
the real reader). Open `/srv/nocturne-ctl/tap` over your own connection:

```
% cat /srv/nocturne-ctl/tap > recording.pcm    # raw S16LE stereo @ the sink rate
```

The bytes are raw PCM (no header); wrap them in a WAV container with the sink's
rate + `s16le` + 2 channels to play them back. Notes:

- **One reader at a time.** A second concurrent open fails with *device busy*.
- **Realtime, not buffered.** The tap holds only a fraction of a second; a reader
  that cannot keep up loses the oldest audio (it is a live monitor, not a
  recorder-of-record). Write to a fast sink.
- **Silence is not filled.** While nothing is playing the read simply waits (the
  sink stops after about half a second of silence) and resumes the moment audio
  plays, so a long silent gap is not represented in the captured stream.
- **Authority is re-checked continuously.** If you stop being the session at the
  keyboard (or your clearance is revoked) mid-recording, the next read fails --
  recording follows the trusted path, it does not outlive it.

## Recording from a capture device (microphone / line-in)

If the machine has an audio **capture** device, its input stream is read from
`/srv/nocturne-ctl/source` -- the same gate as the tap, for the same reason
(recording is an eavesdropping surface). You must be the session at the keyboard,
the system, an admin (`CAP_HOSTOWNER`), or hold the `audio-graph` clearance:

```
% cat /srv/nocturne-ctl/source > mic.pcm       # raw S16LE stereo @ the capture rate
```

The same notes as the tap apply -- one reader at a time, realtime (drop-oldest,
not a recorder-of-record), silence-not-filled, authority re-checked per read --
plus two that are specific to a hardware capture device:

- **On-demand.** The capture device is started only while you hold `source` open
  and stopped when you close it. Nothing is captured -- the microphone is not even
  running -- unless an authorized reader is actively recording.
- **Absent on a box with no capture device.** Opening `source` returns *no such
  device* (`ENODEV`) if the machine exposes no capture input; that is distinct
  from *permission denied*, which is what an unauthorized caller always gets
  (whether or not a device exists). `source` never appears on the `/dev/nocturne`
  mount -- only on `/srv/nocturne-ctl`.

Tapping *another program's* voice (an `ear` on a specific stream rather than the
whole mix or the device) is a separate, finer-grained capture that is not yet
available.

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
