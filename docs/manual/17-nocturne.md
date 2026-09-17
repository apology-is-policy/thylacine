# Nocturne

Nocturne is Thylacine's audio service. It mixes program-owned voices into the
machine's playback stream and provides separately authorized controls for
system volume and recording. SDL applications use it through the Thylacine
audio backend. On machines with no supported sound device, the audio tree is
absent rather than emulated by a silent device.

## In Practice

### Play raw samples

The shared playback file accepts signed 16-bit little-endian stereo samples
at 48 kHz. A file in that exact format can be played with:

```sh
cat music.s16 > /dev/nocturne/audio
```

A WAV, MP3, or other encoded file cannot be written directly as raw samples.
The write waits when the queue is full. Completion means the last samples
were queued; some device-buffered audio may still be playing.

Inspect the format, buffering and driver counters with:

```sh
cat /dev/nocturne/info
```

To discard the shared voice's queued samples:

```sh
echo flush > /dev/nocturne/ctl
```

### Set system volume

```sh
nocturne-vol
nocturne-vol audio 50
nocturne-vol audio 70 90
nocturne-vol mix 40
```

The first command prints the levels. One number sets both channels; two set
left and right separately. Values range from 0 to 100. The `audio` playback
stage and `mix` master stage multiply, so setting both to 50 produces a
quarter of the original sample amplitude. Set `audio 0` to mute playback.

Reading levels requires no special authority. Changing them requires the
console-owner session, system or hostowner authority, or an explicit
`audio-graph` clearance. A refusal does not change the volume. Use the tool
rather than writing the mounted `volume` file, which is read-only.

### Recognize recording authority

Nocturne exposes a mixed-output `tap` and an optional device-input `source`
on its direct control service. Both require recording authority checked
against the actual connecting program. They are not readable through the
shared playback file. Device input is started on demand and reports an
unavailable-device error when the hardware has no capture stream.

These interfaces are for programs that maintain their own direct connection;
there is no general microphone recorder command in this integration.

## Technical Details

The warden binds `nocturned` to a virtio-sound device. Its mounted playback
tree is `/dev/nocturne`; the direct per-client control endpoint is
`/srv/nocturne-ctl`. Keeping those paths separate prevents the system identity
of a shared mount from granting every application control over other
programs' sound or access to their recordings.

Applications create independent voices, each with its own samples and gain.
Voices minted over a private connection are owned by that connection and are
removed when it closes. The server refuses another connection's writes to
a voice. The SDL backend manages this lifetime for ported applications.

The real-time mixing cycle, the control service and device interrupt handling
run separately. Editing the control graph must not hold up the audio cycle.
An idle stream can stop its device clock; an active stream fills a missing
period with silence rather than stalling the device. Inspect the underrun
and error counters when diagnosing broken playback.

Whole-sink authority is an explicit elevation-only capability. Ordinary
forks do not inherit it, and an Imperium propagating scope does not grant it.
System volume and capture use the server's kernel-stamped peer identity and
capabilities rather than a name supplied by the application.
