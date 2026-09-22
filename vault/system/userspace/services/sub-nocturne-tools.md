---
id: sub-nocturne-tools
type: sub
title: "Nocturne clients and audio verification — payload and authority witnesses"
parent: moc-userspace
code:
  - tools/audio-verdict.py
  - tools/test-audio.sh
  - tools/test-game-audio.sh
  - tools/test-nocturne-capture.sh
  - tools/test-nocturne-tap.sh
  - tools/test-nocturne-volume.sh
  - tools/test-ring-audio.sh
  - tools/test-ring-voice.sh
  - tools/test-sdl-audio.sh
  - usr/nocturne-capture-probe/Cargo.toml
  - usr/nocturne-capture-probe/src/main.rs
  - usr/nocturne-probe/Cargo.toml
  - usr/nocturne-probe/src/main.rs
  - usr/nocturne-tap-probe/Cargo.toml
  - usr/nocturne-tap-probe/src/main.rs
  - usr/nocturne-vol-probe/Cargo.toml
  - usr/nocturne-vol-probe/src/main.rs
  - usr/nocturne-vol/Cargo.toml
  - usr/nocturne-vol/src/main.rs
  - usr/ring-voice-probe/Cargo.toml
  - usr/ring-voice-probe/src/main.rs
  - usr/sdl-audio-probe/sdl-audio-probe.c
audit: light
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: [docs/NOCTURNE.md]
created: 2026-09-17
updated: 2026-09-17
---
## Purpose

Supply the `nocturne-vol` operator client and the byte, ring, SDL, tap and
capture witnesses for [[sub-nocturned]]. These clients exercise the public
protocol and its authority boundary, rather than owning audio hardware.

## Contract

`nocturne-vol` reads levels or submits one validated volume-control line on a
fresh `/srv/nocturne-ctl` connection. The server decides whether that peer may
change sink controls. The shared playback mount does not substitute for this
per-client authority check.

## Mechanism

Playback probes submit known tones through byte writes, Weft rings or SDL.
Host gates capture the resulting waveform; `audio-verdict.py` distinguishes
simultaneous tones from sequential playback and checks the silent tail.
Capture probes require actual period bytes and separately verify denied user
and shared-mount access. Under the null audio backend, silence is expected;
non-silence is not a valid witness that the capture path ran.

## Data structures

The probes use bounded sample buffers and protocol records. Ring producers
share the server's published ring geometry and maintain producer/consumer
indices. The volume tool builds a single textual command and leaves range and
control-name validation to the server.

## Concurrency

Audio generation and server consumption are concurrent. Ring probes obey the
published index protocol; byte writers use ordinary 9P lifetime. The host gate
owns its VM and audio capture file, with cleanup on success or failure.

## Invariants enforced

No new kernel invariant is introduced. Positive authorized capture and negative
user/shared-mount probes witness the server's identity/authority contract.
Playback data, not success counters alone, witnesses progress.

## Error paths

Open, write, read and authority failures are surfaced by the client. Capture
must produce bytes within the gate's deadline; an absent or stalled source is
not accepted merely because the backing audio device produces silence.

## Performance

These are bounded verification workloads and a one-shot control client, not a
steady-state mixer. Audio backend timing and sample rate are stamped by the
individual gate. The volume tool does not run in the device cycle thread.

## Prosecution

Distinguish wrong routing, muted/duplicated streams, sequential rather than
simultaneous tones, missing silent tails and ambient authority through a
shared mount. Twelve byte/ring/SDL/capture cases pass across v2m, ITS and INTx
in the integration record; this does not claim every auxiliary script was run.

## Seams

[[sub-nocturned]] owns protocol and authority decisions; [[sub-sdl-port]] owns
the SDL audio adapter. The host waveform is evidence of the composed path.

## Caveats

QEMU's null capture backend proves flow and permissions, not physical microphone
fidelity. Host capture is a test artifact and does not add a guest authority.

## Provenance

(generated from incoming change records)
