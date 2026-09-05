# NOCTURNE.md — the Thylacine audio system

**Status: N-0 research pass + design CANDIDATE (aux track, Fable 5.1, effort
max, 2026-09-05). NOT YET RATIFIED.** The top-level name is the operator's
("The audio system will be called Nocturne", 2026-09-05). Everything below the
name is a candidate: the prior-art digest is measured or cited, the design is
recommended, and the decisions in §9 were provisionally taken under the
standing autonomy rule (`memory/feedback_operator_away_fable_autonomy.md`:
a heritage-aligned recommended design is auto-accepted, forks self-resolve,
every such call is NAMED for the operator to overturn). The operator has
hands-on PipeWire and PulseAudio development experience and asked that the
known quirks of both be **designed around, not inherited** — so each SOTA
section ends with "take / shed", and §10 lists the residue only the operator
can settle.

The operator's one hard requirement, verbatim intent (2026-09-05): *"permissioned
programs would be able to hook onto the graph and add their own DSP modules,
e.g. a convolution filter."* §6.6–6.8 are the answer.

No code was written in this pass. The arc's kernel surfaces (a ring consumer,
a scheduling lease, a driver) fire THE EFFORT GATE when they open; this pass
is design, run at max regardless.

---

## 0. The candidate in ten lines

1. **Nocturne is a file server.** One daemon, `nocturned`, serves `/dev/nocturne`;
   audio authority is exactly what a Proc can name in its namespace (the
   `/net`, `/dev/tapestry`, `/dev/warp` idiom — no new capability bit).
2. **The audio system is a graph**: nodes with ports, links between ports, one
   pull-driven cycle per device period, dependency-counted activation (the
   PipeWire/JACK2 model), a float32 graph rate set by the sink.
3. **Clients are nodes in their own Proc.** A playing program is a *voice*, a
   recording one an *ear*, a program-supplied DSP stage a *descant*. Each node
   moves samples over a shared-Burrow ring granted with the node's data fid
   (`SYS_WEFT_SHARE`/`SYS_WEFT_MAP`, the mechanism Weft and Warp already use).
4. **DSP code never enters the server.** A descant runs in its owner's Proc.
   This is the one place every shipping system compromises (WASAPI APOs and
   PipeWire's filter-chain load plugins into the engine; CoreAudio loads AUs
   in-process by default) and the place Thylacine will not.
5. **The graph never waits on a client** (JACK2 async by construction, per
   node): a late node's output is substituted (bypass / hold / mute) and its
   xrun counted; repeated misses auto-bypass THAT node. The device stream is
   never underrun by a client. This is the core of the candidate invariant
   **I-46**.
6. **Deadline scheduling is a lease, not a class**: `nocturned` may confer a
   bounded periodic CPU guarantee (period, capacity, deadline) on a node's
   thread — the Zircon deadline-profile idea shaped as a Thylacine allowance
   (revocable, non-transferable, bounded). It is the arc's one kernel lift and
   lands last (N-4), spec-first.
7. **Policy is data, not a process**: a declarative route/volume policy in
   `/lib/nocturne` + `$HOME/lib/nocturne` read by the daemon (the aurora-config
   tier idiom). WirePlumber's session-manager process and its Lua are shed.
8. **The driver is a node.** The virtio-sound driver sits behind the same ring
   protocol as every other node (Genode 24.02's inversion), so it can be split
   into its own warden-bound Proc without touching the graph, and a bare-metal
   Pi HDMI/USB driver is "another sink node", never a redesign.
9. **Heritage compatibility is free**: `/dev/nocturne/audio` and `/dev/nocturne/volume`
   keep Plan 9's `audio(3)` semantics (a `write` of s16le stereo plays; the
   `volume` grammar), so `bind` puts a 9front-shaped `/dev/audio` in any namespace.
10. **The witness is deterministic**: QEMU's `wav` audio backend records what
    the guest played to a file; a tone probe + a DFT check + positive/negative
    controls make every gate CI-able on the mac and on thyla-pi with no host
    audio hardware in the loop. thyla-pi under KVM + the `pipewire` backend is
    the real-silicon leg.

---

## 1. Naming

**Nocturne** (ratified by the operator) — a night piece; the thylacine is
nocturnal and crepuscular. The sub-names below are PROPOSED and held for
signoff per CLAUDE.md §"Thematic naming" (propose, never rename load-bearing
identifiers unilaterally). Each carries a plain fallback so the design reads
without the colour.

| Proposed | Plain fallback | What it names | Why the word |
|---|---|---|---|
| `nocturned` | the audio daemon | the graph + mixer + clock server, a warden-bound driver Proc (`tapestryd`/`netd` idiom) | daemon-suffix convention |
| `/dev/nocturne` | `/dev/audio-graph` | the served tree | `/dev/tapestry`, `/dev/warp` precedent |
| **voice** | play stream / sink-input | a client's playback node | what a program contributes to the piece |
| **ear** | record stream / source-output | a client's capture node | the listening end |
| **descant** | filter / insert | a program-supplied DSP node inserted into the graph (the operator's convolution filter) | a descant is an independent line sung *above* the melody by another voice — a layer added by a different performer, which is exactly what a program-supplied insert is |
| **tempo** | driver node / clock | the node whose device period paces the cycle | the thing every player follows |
| **conductor** | policy | the declarative routing/volume policy (data, §6.10) | reads the score, tells the players where to sit — and is not itself a player |
| **cadence lease** | deadline lease / sched lease | the bounded periodic CPU guarantee a node thread may be granted (§6.7) | a cadence is a rhythmic period with a resolution point — a deadline |
| **quantum** / **period** | (plain) | frames per cycle | PipeWire/JACK vocabulary; kept plain deliberately |

Load-bearing terms stay plain: `node`, `port`, `link`, `sink`, `source`,
`xrun`, `latency`, `rate`, `format`. (The `yip` word — the thylacine's bark —
is taken by the inter-track relay and stays there.)

---

## 2. Where scripture stands, and what the operator's direction changes

Audio is a **v1.0 non-goal** in every binding document, and the deferral
horizon is not even consistent:

| Document | Line | Says |
|---|---|---|
| `docs/VISION.md` §9 | 321 | "**Audio at v1.0.** No sound system. Rationale: a half-baked audio stack is the kind of stub the project explicitly rejects; ship it properly post-v1.0 or not at all." |
| `docs/ROADMAP.md` §12.3 | 1432 | v**2.0** candidates: "**Audio stack**: VirtIO sound device + userspace audio server." |
| `docs/ROADMAP.md` §12.5 | 1447 | Ruled out: "Sound / Bluetooth / hardware sensors at v1.0. Deferred to v**1.x**." |
| `docs/ROADMAP.md` §12.5 | 1448 | "**Real-time scheduling at v1.0.** EEVDF gives soft latency bounds; hard RT is v2.x." |
| `docs/COMPARISON.md` | 112, 329, 345 | matrix row "Sound at v1.0 … ✗ (deferred)"; the deferred list twice |
| `docs/manual/00-overview.md` | 87 | "Audio at v1.0. Deferred to post-v1.0." |
| `docs/TAPESTRY.md` §2, §3, §9, §10 | 47, 69, 206–208, 225–226 | "every multimedia fast-path — present, input, audio — is the same shape"; the SDL backend's `SDL_audio -> a future audio server (the same shape; no virtio-sound driver exists yet)"; unblock item 4 "**virtio-sound** (for audio) — does not exist; scope alongside the audio server when game audio is in view." |
| `docs/NOVEL.md` | 177 | "present / input / audio are all *the same shape* — a 9P server + multishot + registered buffers" |
| `docs/LOOM.md` §10 | 501 | the same sentence |

What the operator's 2026-09-05 direction changes: audio moves from "post-v1.0
or not at all" to **an arc that starts now, built properly** — the VISION
rationale is honoured by building it to the same bar as everything else, not
by keeping the non-goal. The shape scripture already committed (a 9P server +
Loom multishot + registered/shared buffers, the SDL seam, virtio-sound as the
device) is **kept**, not re-litigated; this document fills in what "the same
shape" left open for audio: the graph, the clock, the deadline discipline, the
DSP insertion, and the policy.

**Scripture reconciliations owed at the scripture commit** (the design-first
pattern: land these WITH this document, before any code): VISION §9 (relax the
non-goal, dated, pointing here), ROADMAP §12.3 + §12.5 (one horizon: the
Nocturne arc, pointing here; keep the hard-RT line and note the lease is soft +
bounded), COMPARISON (three lines), manual overview (line 87), TAPESTRY §9/§10
(point item 4 here), NOVEL (the §11 candidate), AUX-ROADMAP (a Nocturne row),
ARCH §28 + CLAUDE.md (an **I-46 RESERVED** row — reserved in the table itself,
because I-41's history shows a number reserved only in its own doc is a number
that can be double-allocated).

---

## 3. Requirements

**From the operator (binding):**

- R-1 **Permissioned programs extend the graph with their own DSP modules**
  (a convolution filter is the worked example). "Permissioned" = a capability
  decides who may insert where; "their own" = the program's code, running as
  the program.
- R-2 The known PipeWire and PulseAudio quirks are designed around (§5.2, §5.3
  "shed").
- R-3 Top-notch: the same rigour as the rest of the tree (spec where an
  invariant is load-bearing, audit before merge, gates that cannot pass on a
  broken fixture).

**Derived (this pass):**

- R-4 **Game audio first**: DOSBox-X (its SDL mixer wants 48 kHz, 1024-frame
  blocks, 25 ms prebuffer — `third_party/dosbox-x/src/dosbox.cpp:3509–3519`)
  and TyrQuake get sound through the SDL seam with no game changes. N-2a-3
  gave TyrQuake sound (the software build's `snd_null` -> `snd_sdl`; the play
  scenarios' `-nosound` dropped; quarry's PLAY vs BENCH split) and made
  `SDL_AUDIO_DRIVER_DUMMY` an auto-selectable fallback rather than the only
  driver. DOSBox-X sound is **N-2a-4** (it builds only through the clade C++
  fork and compiles `nosound`, so retiring `0004-thylacine-force-dummy-audio`
  needs a clade rebuild + a build-config change -- deferred to a clade host).
- R-5 **Multiple simultaneous voices** mixed to one sink, per-voice gain, a
  system volume, mute; **capture** (an ear) for the same reasons in mirror.
- R-6 **Glitch-free under load**: a busy console, a compile, another program's
  misbehaving node must not make the sink underrun.
- R-7 **Latency is a stated, measured budget**, not a hope (§6.9 gives targets
  to measure against; VISION §4.5 gets a row when the numbers exist).
- R-8 **No new kernel surface until it is earned**: the ring, the share, the
  IRQ path, the 9P server, the poll/Loom wait all exist. The one lift (the
  cadence lease) is last and spec-first.
- R-9 **Real hardware is a sink node, not a redesign** (thyla-pi under KVM
  first; bare-metal Pi HDMI/USB later).
- R-10 **Linux binaries under VIVARIUM get audio** through one compatibility
  protocol server (§6.11), not per-program patches.

**Out of scope for the arc (named so nobody assumes them):** MIDI, Bluetooth
audio, USB audio class (needs xHCI — MENAGERIE §12), HDMI/DisplayPort audio
on bare metal (a v1.x driver), hardware DSP offload, surround formats beyond
stereo in the first cut (the graph is N-channel by construction; the first
sinks are stereo), audio over the network (free later via 9P export, as
9front's mixfs shows — §5.1).

---

## 4. Ground truth in the tree and on the two hosts

Everything in this section was read or run in this pass (2026-09-05).

### 4.1 The mechanisms Nocturne rides (all landed)

- **The grant-is-the-share ring.** `SYS_WEFT_SHARE(ring_va, ring_size)`
  registers a whole ANON Burrow (RW, no-exec) in the caller and mints a
  kernel-scoped `share_id` that is "inert in any other hand … so no capability
  is required to mint one"; `SYS_WEFT_MAP(data_fd, hint)` on the peer side
  issues `Tweft(F)` to whichever 9P server serves the fid, claims the returned
  `share_id` consume-once, and maps the ring — idempotent
  (`kernel/include/thylacine/syscall.h:1546–1568`). A **userspace** 9P server
  answering `Tweft` is proven: netd (Weft-6b) and tapestryd's Warp host3d ring
  (the "F1 weft arm" row in `docs/AUDIT-TRIGGERS.md`). The kernel consumer
  discipline is I-30 lifted to the descriptor: snapshot + bounds-validate every
  client-written slot, never re-read it post-check (`docs/reference/125-weft.md`).
- **The wait/wake path.** Loom (`libthyla_rs::loom::Ring::{setup, register_handles,
  register_buffers, try_submit, enter, reap, submit_one_wait}`,
  `usr/lib/libthyla-rs/src/loom.rs:413–612`); the Weft-4 readiness ring for a
  syscall-free busy-poll edge; `SYS_POLL` (ms timeout); `SYS_TORPOR_WAIT`
  (`timeout_us`, `syscall.h:657`). There is **no nanosleep-class syscall** — a
  timer-clocked node must use torpor's microsecond timeout (bounded below by
  the 1 kHz tick when a CPU is busy; the tickless one-shot when idle,
  `arch/arm64/timer.c:161`). The audio clock is therefore the **device IRQ**,
  never a sleep loop (§6.3).
- **The event-fid lesson.** libtapestry reads events SINGLE-SHOT on purpose:
  "a multishot READ re-arms into the SAME registered slice, so a shot landing
  before the client drains the prior one overwrites it … Until Loom grows a
  provided-buffer pool (the io_uring buf_ring analog; a G-6 seam), the client
  re-arms after each drain" (`usr/lib/libtapestry/src/lib.rs:40–45`). The
  period tick is a droppable class (like FRAME), so multishot is acceptable
  for it; node-lifecycle events are not, and use single-shot.
- **The driver idiom.** `libdriver::Driver { probe(res); serve(self, res) }`
  (`usr/lib/libdriver/src/driver.rs:32`), warden bind manifests
  (`usr/warden/src/main.rs`): tapestryd binds `virtio-pci:16` + `:18` with
  `needs { pci, irq, dma = "pool: 32 MiB" }`, `serves = "/dev/tapestry"`,
  `lifecycle = persistent`, `gather = all`, `caps = ["csprng"]`; netd binds
  `virtio-pci:1`, `serves = "/net"`. tapestryd's `serve` is one `t_poll` loop
  over the `/srv` listener, the client connections, and the IRQ fd
  (`usr/tapestryd/src/main.rs:659`). Native threads exist
  (`libthyla_rs::thread::spawn_raw`, `thread.rs:85`) but **no native daemon
  uses one today** — Nocturne would be the first multi-threaded native daemon
  (§6.2, D-1c).
- **The scheduler.** Three strict bands INTERACTIVE/NORMAL/IDLE
  (`kernel/include/thylacine/sched.h:31–34`), simplified EEVDF within a band,
  1000 Hz tick, 6-tick default slice; **no cross-band aging** ("a CPU-bound
  INTERACTIVE thread starves NORMAL on its CPU", ARCH §8.3). INTERACTIVE
  promotion is sticky, one-way, and reachable only through `kobj_irq_wait`
  (implicitly `CAP_HW_CREATE`-gated) and the trusted console read
  (`kernel/irqfwd.c:375`, `kernel/cons.c:1714,1957`). So a **driver's IRQ
  thread is INTERACTIVE by construction; a client node's thread is NORMAL** and
  can be delayed by a slice (6 ms) or more under load. ROADMAP §12.5 keeps hard
  RT at v2.x. That is the whole reason §6.7 exists.
- **Capabilities + roles.** `CAP_HW_CREATE` … `CAP_JIT` (`kernel/include/thylacine/caps.h`);
  five `SPAWN_PERM_*` bits; clearances conferred corvus-gated via the `cap`
  device (I-2). tapestryd's per-connection actor model (owner principal, the
  `SYS_SRV_PEER` role stamp, `Actor::Session`) is the precedent for "who may
  edit what" (§6.8).
- **Latency numbers already measured**: IRQ-to-userspace p99 < 5 µs
  (`irq-bench`, TAPESTRY §2 — with the RW-11 caveat that it measures the
  test-mode path); one daemon context switch per present ~1–5 µs.

### 4.2 What is stubbed today

`SDL_config.h:11` — "audio = dummy (no virtio-sound yet — TAPESTRY.md §10 item
4)"; patch 0004 forces `SDL_AUDIODRIVER=dummy` so DOSBox-X's combined
`SDL_Init` does not `E_Exit` (0004 stands until N-2a-4 -- DOSBox is not built on the dev host); TyrQuake ran `-nosound` until N-2a-3 and carries a guard
patch for an upstream NULL deref that only `-nosound` exposes
(`usr/ports/tyrquake/patches/0001`); DX-1 stubs opusfile/speexdsp
(`usr/ports/dosbox-x/glue/thylacine-audio-stubs.c`) — "Building real
libopusint is a DX-3 (audio) concern". The SDL port directory
(`usr/ports/sdl2/thylacine/`) has video, events, OpenGL and Vulkan files and
**no audio file** — the on-ramp is one new backend file (§6.11).

### 4.3 QEMU on the mac (the dev host)

`qemu-system-aarch64` 10.0.2 (Homebrew):

- `-audiodev help`: `none`, `coreaudio`, `dbus`, `wav`.
- Sound devices: `virtio-sound-pci` (alias `virtio-sound`), `virtio-sound-device`
  (MMIO), `intel-hda`/`ich9-intel-hda` + `hda-output`/`hda-duplex`/`hda-micro`,
  `AC97`, `ES1370`, `usb-audio`.
- Parse-verified this pass (a stopped throwaway VM, killed by PID):
  `-audiodev wav,id=snd0,path=<file>,timer-period=5000,out.buffer-length=20000,out.frequency=48000,out.channels=2,out.format=s16,out.fixed-settings=on`
  and `-device virtio-sound-pci,audiodev=snd0,streams=2` are accepted;
  `-audiodev coreaudio,id=snd1,out.buffer-count=4,out.buffer-length=10000` is
  accepted. With `streams=2` on the `wav` backend QEMU logs
  `audio: Could not create a backend for voice 'virtio-sound.in'` — **the wav
  backend is playback-only**, so wav-witnessed gates run `streams=1` (or
  tolerate that exact line).
- `tools/run-vm.sh` (AS-BUILT at N-1): `-device virtio-sound-pci,id=snd-pci0,
  audiodev=snd0,streams=1,disable-legacy=on` on every boot, placed after
  `rng_pci0` so its INTx line is distinct from the NIC's and the GPU's
  (measured: the function lands at PCI (0,6,0) on INTID 37 and the IRQ claim
  succeeds); `THYLACINE_AUDIODEV` = `none` (default) | `wav` (+
  `THYLACINE_AUDIO_WAV`) | `coreaudio` | `pipewire`/`pa`/`alsa`/`sdl`/`dbus`/
  `oss`/`jack`; `THYLACINE_NO_AUDIO=1` removes the device. The warden bind is
  `virtio-pci:25`. QEMU's device offers features lo=0x79000000 hi=0x101, one
  stream, no jacks, no chmaps; the stream reports formats 0xe0078 (S8/U8/S16/
  U16/S32/U32/FLOAT), rates 0x3fff (all fourteen), channels 1..2.

### 4.4 QEMU + PipeWire on thyla-pi (the real-silicon host)

`qemu-system-aarch64` 10.0.11 (Debian 13): `-audiodev` backends `none`,
`alsa`, `dbus`, `jack`, `oss`, `pa`, `pipewire`, `sdl`, `spice`, `wav`; the
same virtio-sound + HDA + AC97 device set. The Pi runs PipeWire + WirePlumber
as user services (`pipewire.service` active since 2026-06-18); user `cora` is
in `audio`. So the KVM leg is `-audiodev pipewire,id=snd0` with no host
setup at all.

### 4.5 The Pi 400 and Pi 500 as audio hardware

Measured on thyla-pi ("Raspberry Pi 400 Rev 1.0", kernel 6.18.34+rpt-rpi-v8):
`/proc/asound/cards` lists exactly `0 [vc4hdmi0]` and `1 [vc4hdmi1]`
(`vc4-hdmi`), each `MAI PCM i2s-hifi-0 : playback 1` — **playback-only HDMI on
both micro-HDMI ports, no headphone card**. The device tree carries
`hdmi@7ef00700`, `hdmi@7ef05700`, `i2s@7e203000`, `pwm@7e20c000`, `pwm@7e20c800`.

Cited: the Pi 400 "lacks an audio output jack, with all audio required to be
sent over HDMI" — the options are HDMI, a USB audio adapter, Bluetooth, or PWM
audio remapped to GPIO 12/13/18/19 via the `audremap` overlay
([The Pi Hut](https://support.thepihut.com/hc/en-us/articles/360015222798-Speakers-for-Raspberry-Pi-400),
[Raspberry Pi forums](https://forums.raspberrypi.com/viewtopic.php?t=327420)).
The Pi 500's product page lists the 2.4 GHz quad-core Cortex-A76 (BCM2712),
"2 × micro HDMI port (supports up to 4Kp60)", "2 × USB 3.0 port and 1 × USB
2.0 port", "Bluetooth 5.0, BLE" and **no audio jack**
([raspberrypi.com](https://www.raspberrypi.com/products/raspberry-pi-500/));
the Pi 5 it is built on dropped the jack
([The Register](https://www.theregister.com/2023/09/28/raspberry_pi_5_revealed/)).
(One review summary claimed a jack on the Pi 500; that is the Raspberry Pi
**Monitor**, which has speakers and a 3.5 mm out — the keyboard has none.)

Consequence (R-9): on bare metal, the first real sink is **HDMI audio through
the vc4 MAI interface** (a DMA-fed FIFO on the HDMI block, clocked via the
VideoCore mailbox — a real driver, MENAGERIE §12 territory), the cheap one is
**PWM-on-GPIO** (a DMA to the PWM FIFO; needs an external filter/DAC HAT), and
the universal one is **USB audio class** behind xHCI. None of them changes the
graph. Under KVM none of them is needed.

---

## 5. Prior art

Ordered per CLAUDE.md §"Research prior art before surfacing a design fork":
heritage first, then SOTA, then the capability peers, then the substrate.

### 5.1 Heritage: Plan 9 and 9front

**The device** (`audio(3)`, Bell Labs 4th edition; 9front): `/dev/audio`
(write to play, read to record), `/dev/volume`, `/dev/audiostat`, and in
9front `/dev/audioctl`. Format: "a sequence of stereo samples, left sample
first. Each sample is a 16 bit little-endian two's complement integer; the
default sampling rate is 44.1 kHz." The file's `stat` length "represents the
number of bytes buffered for input or output" — Plan 9's whole latency
interface is *one number you can read*. 9front's `audiostat` adds a 32-byte
header with `bufsize` ("preferred write unit") and `buffered`; `volume` takes
`source [left right]` in 0–100 with the special sources `audio`, `speed`
(sample rate in Hz) and `delay` ("buffer limit in samples"). The SB16 sources
were `audio synth cd line mic speaker treb bass speed`, plus `reset`.
([Plan 9 audio(3)](https://raw.githubusercontent.com/plan9foundation/plan9/main/sys/man/3/audio),
[9front audio(3)](https://raw.githubusercontent.com/9front/9front/front/sys/man/3/audio))

**The multiplexer** (`audio/mixfs`, `audio(1)`): "Only one program can open
/dev/audio for writing at a time" (Bonnet, IWP9 2026 §1.6). mixfs "opts to
preserve the same interface: rather than add more complexity driver-side, it
serves a file tree which layers itself over the audio devices" — it mounts on
`/mnt/mix`, binds `/mnt/mix/audio` and `/mnt/mix/volume` over `/dev`, "mixes
samples from one or more writers and pushes the result to the audio device",
"will resample incoming audio to the format of the audio device output if it
does not match the default (s16c2r44100)", proxies `/dev/volume` adding a
`mix` soft-volume and a `dev` output-device switch, and (per the 2026 paper)
"handles reads from the audio file (ie. loopback) in the same way as writes".
Because it is a file server, exporting a mix is trivial — the paper's MP3
"radio" is `mixfs -s radio` + `aux/listen1` + `dd` in ~10 lines.
([9front audio(1)](https://raw.githubusercontent.com/9front/9front/front/sys/man/1/audio),
[9front wiki: Audio](http://wiki.9front.org/audio),
[Bonnet, *Audio and music production on Plan 9*, IWP9 2026](https://12e.iwp9.org/papers/audio.pdf) — fetched via thyla-pi)

**Conversion at the edges**: `pcmconv` / `libpcm` (format string `s16c2r44100`:
encoding letters `s# u# S# U# f# a8 µ8`, `c#` channels, `r#` rate), band-limited
FIR resampling; used by the Wolfenstein port, OPL emulation, and 9front's SDL2
audio implementation. There is no plugin system: "Instead of a plugins system,
pplay relies on small external programs such as pcmenv to modify parts of a
buffer and read back the result." The paper's conclusion is candid that a
DAW-class processing chain "requires a high amount of processing power,
exacerbating latency issues" and that VST-class hosts are "unlikely to ever
appear on Plan 9".

**Take**: the file-server shape (the graph as files; `bind` as routing;
export for free); `bufsize`/`buffered`/`delay` as the user-visible latency
vocabulary; conversion at the edges as a library the client links (pcmconv
becomes a libnocturne module). **Shed**: one writer per device; one fixed
rate; no per-stream latency; no routing graph; the driver-does-the-mixing
temptation.

### 5.2 PipeWire (the SOTA graph; the operator's primary reference)

**Objects**: core, nodes, ports, links (passive/active), devices, clients,
factories, modules; the server "does not perform any management of the graph;
… linking nodes is not done automatically" — a session manager does it, with
per-client object permissions R/W/X/M/L ("The session manager is responsible
for defining the list of permissions each client has").
([Overview](https://docs.pipewire.org/page_overview.html),
[Objects design](https://docs.pipewire.org/page_objects_design.html))

**Scheduling** (the part worth copying exactly): every node has an
*activation record* in shared memory — `status` (NEED_DATA/HAVE_DATA/OK),
`pending` ("number of unsatisfied dependencies needed to be able to run"),
`required` ("number of dependencies with other nodes"), state timestamps
(TRIGGERED/AWAKE/FINISHED), and for drivers the cycle's time, duration and
rate (quantum). The driver "will use a timer or some sort of interrupt from
hardware to start the cycle"; at cycle start it checks "Did [the previous
cycle] complete? Mark xrun on unfinished nodes", sets each follower's
`pending` to `required`, then decrements followers atomically — "When the
required field is 0, the eventfd is signaled and the node can be scheduled";
each finished node decrements its targets' `pending` in turn. Out-of-process
nodes are first-class: "writing to the remote client eventfd will wake the
client directly without going to the server first … remote clients … can
directly trigger peers and drivers". The model is pull-based: "the ALSA sink
… signaling PipeWire to start a new cycle when it has no more data to send: it
**pulls** data from the graph"; "If we fail to execute the entire graph in
time … the ALSA sink node will have no data and this will lead to an underrun."
([Scheduling](https://docs.pipewire.org/page_scheduling.html),
[Bootlin, *An introduction to PipeWire*](https://bootlin.com/blog/an-introduction-to-pipewire/))

**Session management** (WirePlumber): device enablement, profiles/routes,
client access control, node configuration, link policy, default-device
metadata — "a large number of different events" handled by Lua scripts.
([WirePlumber: Understanding Session Management](https://pipewire.pages.freedesktop.org/wireplumber/design/understanding_session_management.html))

**DSP**: `module-filter-chain` builds a chain of `ladspa`/`lv2`/`builtin`/
`sofa`/`ffmpeg`/`onnx` nodes "with 2 streams, a capture stream providing the
input … and a playback stream sending out the filtered stream"; the plugins
execute in whatever `pipewire` process loads the module (the daemon, unless
one starts a second instance for it), and the docs advise `node.async = true`
for heavy filters "to avoid interrupting the other RT threads".
([module-filter-chain](https://docs.pipewire.org/page_module_filter_chain.html))

**Take**: the object model; the pull cycle with a driver node; the activation
counters + direct peer wakes (this IS Loom CQE + the Weft readiness poke);
"mark xrun on the unfinished, proceed" at cycle start; per-node declared
latency; per-client permissions; the filter-chain's *two-stream* shape (a
descant is a capture+playback pair from the graph's view).
**Shed** (the operator's list, made concrete): a separate session-manager
process with a scripting engine (policy becomes data, §6.10); quantum flapping
(one client's request re-clocking the graph — the quantum is chosen by the
tempo from declared bounds and changes only on explicit reconfiguration); the
object explosion (a Thylacine client sees a directory, not a registry); loading
plugins into the daemon (never); the config-file sprawl (two tiers, one
grammar); "everything negotiates" — formats are fixed at the graph rate and
converted at the edge.

### 5.3 PulseAudio

Glitch-free timer-based scheduling: large buffers + high-resolution timers
instead of fragment interrupts, latency adapted per application; but it "will
only be enabled on mmap()-capable ALSA devices and where hrtimers are
available", "works reliably only on newest ALSA, newest kernel, newest
everything", and hardware caps bite (HDA "artificially cap[s] playback buffers
to 370 ms"). Flat volumes: "the sink HW volume is set to the same level as the
highest volume input stream … hardware mixer changes cannot be timed
accurately and thus this change of volumes can sometimes cause the resulting
output sound to be momentarily too loud or too soft." A monolithic daemon
with in-process modules; the native protocol over a Unix socket is the de
facto Linux application ABI (libpulse, ALSA's `pulse` plugin, SDL, mpv,
ffmpeg, browsers).
([Fedora: GlitchFreeAudio](https://fedoraproject.org/wiki/Features/GlitchFreeAudio),
[Arch: PulseAudio/Troubleshooting](https://wiki.archlinux.org/title/PulseAudio/Troubleshooting),
[pulse-daemon.conf(5)](https://manpages.ubuntu.com/manpages/trusty/man5/pulse-daemon.conf.5.html))

**Take**: per-stream requested latency as a first-class parameter; the native
protocol as the VIVARIUM compatibility surface (§6.11). **Shed**: flat
volumes (the sink's hardware volume is never slaved to a stream; gains are
graph stages); the timer as the only clock (the device IRQ is the clock; a
timer only paces timer-only sinks); in-process modules.

### 5.4 JACK / JACK2

Two engine modes. Synchronous: "the audio cycle is composed of: read audio
input buffers, execute the graph, write audio output buffers." Asynchronous:
"read audio input buffers, write audio output buffers computed at previous
cycle, execute the graph" — "Jack will proceed with whatever data it already
has, even if one or more of the clients are not finished in time", at the
cost that "running in asynchronous mode adds an extra period of latency", and
with a subtle desync: "clients that did finish in time will experience more
cycles than clients that didn't". Letz's measurement shows why the mode
matters: at 44.1 kHz / 64 frames the driver interrupt averaged 1451 µs "but
interrupt period is not regular", and "a non regular driver interrupt force[s]
… synchronous mode to be chosen (otherwise the graph may lack time to
finish)". JACK2 activates the graph as a data-flow of clients across
processors; the driver is the clock; xruns are counted and exposed.
([Letz, *What's new in JACK2?*, LAC 2009](https://lac.linuxaudio.org/2009/cdm/Thursday/01_Letz/01.pdf) — text via thyla-pi;
[Amlie, *Jack2 and asynchronous mode*](https://www.amlie.name/jack2-and-asynchronous-mode/))

**Take**: the graph never blocks on a client — but applied **per node** (the
late node's own output is substituted, its own counter bumps) rather than per
graph, which removes JACK's whole-graph desync; the extra-period cost is paid
only by nodes that opt out of the in-cycle contract (§6.6). **Shed**: the
choice between "one late client stalls everyone" and "everyone pays a period".

### 5.5 CoreAudio

"The HAL IOProc is a time-limited, high-priority thread … overload warning …
when the time allocated, by the HAL, expires before the IOProc returns"
(the cycle is dropped, the client warned); IOProcs run under Mach
`THREAD_TIME_CONSTRAINT_POLICY`; since 10.7 `coreaudiod` is the audio server
and, for kernel drivers, mixing happens in the IOAudio family. AUv3 plug-ins
"run in their own process, communicating with the host application via XPC.
There's an inherent latency to this, so on Mac … you are allowed to … load
… the audio unit … directly into the host". Guidance: no unbounded locks, no
allocation, no BSD calls in the IOProc.
([Apple QA1467](https://developer.apple.com/library/archive/qa/qa1467/_index.html),
[Apple forums: AudioUnit Extensions](https://developer.apple.com/forums/thread/25838),
[Gearspace thread on the HAL/coreaudiod](https://gearspace.com/threads/does-apples-core-audio-resample-ad-da-signal.1352575/page-13) — secondary)

**Take**: a per-cycle time-constraint for the client's thread (the cadence
lease, §6.7) and the "drop the cycle, warn, continue" posture. **Shed**:
in-process plugins as the default path.

### 5.6 WASAPI (Windows)

The shared-mode engine runs in `audiodg.exe`, spawned by the Audio Service —
"Windows Audio Device Graph Isolation" — and vendor DSP (APOs: stream SFX,
mode MFX, endpoint EFX stages) is loaded **into** it, and the failure mode is documented plainly: "If an APO hits an
exception, there's no blue screen of death, but the Windows audio engine
crashes. There's a watchdog timer … if a call gets stuck, the watchdog forces
a crash of the Windows audio engine." Exclusive mode, KS and ASIO bypass the
engine and every APO.
([Microsoft Learn: audio measures](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/audio-measures),
[dechamps/APO notes](https://github.com/dechamps/APO),
[Boutnaru on audiodg](https://medium.com/@boutnaru/the-windows-process-journey-audiodg-exe-windows-audio-device-graph-isolation-ea39546e205b))

**Take**: the *motive* (a DSP fault must not take the audio system with it)
and the watchdog. **Shed**: the mechanism — isolating the engine from the
service while loading third-party code into the engine isolates the wrong
thing. In Nocturne the DSP is isolated from the engine; the engine needs no
watchdog for it because it never waits for it.

### 5.7 Fuchsia

Drivers expose a **ring buffer as a VMO** shared with `audio_core`; the client
"start[s] audio at an offset … relying on zeroed-out VMO contents", and "Once
the ring buffer is started, it is not safe for the client to write data to the
ring buffer between read and play positions." Renderers carry a **reference
clock** (MONOTONIC + CONTINUOUS). Effects are, in v1, "loadable modules"
inside `audio_core`; the newer **`fuchsia.audio.effects`** protocol runs them
**out of process**: a `ProcessorCreator` returns a `Processor` bound to a
`ProcessorConfiguration` whose inputs/outputs are `fuchsia.mem.Range` VMO
regions, with `block_size_frames`, `max_frames_per_call`, per-output
`latency_frames` ("output signal shift relative to input") and
`ring_out_frames`; `Process(num_frames, options)` returns `per_stage_metrics`
(wall time, CPU time, queue time, page faults, lock contention). Zircon
schedules such threads with **deadline profiles** (capacity within a deadline
per period; "Deadline tasks always take precedence over eligible fair tasks";
feasibility: "the sum total of deadline demands must not exceed the
processor's capacity"; a *critical* class gets absolute priority under
oversubscription).
([Ring Buffer Behavior](https://fuchsia.dev/fuchsia-src/development/audio/ring_buffer),
[fuchsia.audio.effects](https://fuchsia.dev/reference/fidl/fuchsia.audio.effects),
[fuchsia.media](https://fuchsia.dev/reference/fidl/fuchsia.media),
[Zircon scheduler](https://fuchsia.dev/fuchsia-src/concepts/kernel/kernel_scheduling))

**Take**: the effects-processor contract (in/out shared ranges,
`latency_frames`, `block_size`, `max_frames_per_call`, per-call metrics) —
the closest existing shape to R-1, adopted almost verbatim as the descant
contract (§6.6); deadline profiles as the scheduler precedent (§6.7).
**Shed**: system-provisioned effects only — Nocturne's are user programs; the
FIDL round-trip per `Process` call (ours is a ring slot + a poke).

### 5.8 Genode 24.02 / 24.05 (the capability-OS rework)

Why they threw the old `Audio_out`/`Audio_in` sessions away: a fixed 44.1 kHz
rate that clashed with 48 kHz-only drivers; "buffer under-runs, which produce
audible noise" and "slow accumulation of buffered sample data, which increases
latency over time"; and "The mixer is a single client of the audio driver,
which makes the mixer dependent on the liveliness of the driver." The new
design **inverts the hierarchy**: the mixer is "a self-sufficient resource
multiplexer" offering **Play** and **Record** sessions, and "both audio drivers
and applications become clients", enabling "dynamic starting, removal, and
restarting of the driver, of even multiple drivers". Clients operate
"periodically"; the mixer "infers the used sample rates and periods by
observing the behavior of the clients", "measures the jitter of clients to
automatically adjust buffering parameters … while trying to optimize for low
latency", converts rates automatically, and routes by declarative XML policy
(`<mix name="left"><play label_suffix="left"/></mix>`). 24.05 "wrapped up the
transition".
([Genode 24.02 release notes](https://genode.org/documentation/release-notes/24.02),
[Genode 24.05 release notes](https://genode.org/documentation/release-notes/24.05))

**Take**: driver-as-client (the device sink is a node, D-1); declarative
policy, no session-manager process (D-2); jitter-measured, observation-driven
buffering as the *automatic* tier of latency control; driver restart without
cascading. **Shed**: nothing structural — Genode has no user-program DSP
insertion, which is Nocturne's addition.

### 5.9 seL4 sDDF (the driver-framework peer)

Sound is a device class in the sDDF design: "sound drivers communicate with
clients through two pairs of request / response queues: one for commands and
one for PCM transfer"; the stream life cycle is Take → Prepare → Start → Stop
→ Release; PCM requests carry `addr`/`len` into a shared data buffer and are
answered with `status` + `latency_bytes`; playback pre-buffers *n* buffers
before Start (a protocol shaped so that it is "required for compatibility
with Linux's … VirtIO sound implementation"); "a virtualiser could allow
multiple clients to access a stream at the same time by mixing". The
framework's memory model: "the driver never needs to access the actual data
transferred. The data region is therefore not mapped into the driver's
address space, reducing the trust required in the driver."
([Trustworthy Systems, *sDDF Design*](https://trustworthy.systems/projects/drivers/sddf-design.pdf) §4.2, §5.5 — text via thyla-pi;
[sDDF repository](https://github.com/au-ts/sddf))

**Take**: the command/PCM queue split and the pre-buffer protocol as the
shape of the driver-side ring (it is virtio-snd's shape too); the "driver need
not see the samples" posture, which maps onto Thylacine's DMA-handle model
when the driver is split out (D-1). **Shed**: nothing; sDDF has no mixer
policy or DSP.

### 5.10 The substrate: virtio-snd, QEMU's device, the host backends

**The spec** (virtio 1.2 §5.14, device ID 25): virtqueues `controlq`,
`eventq`, `txq`, `rxq`; configuration space `jacks`/`streams`/`chmaps`;
control families jack info/remap, PCM info, `SET_PARAMS` (`buffer_bytes`,
`period_bytes`, `features`, `channels`, `format`, `rate`), `PREPARE`,
`RELEASE`, `START`, `STOP`; the stream state machine INIT → PREPARE →
(START ↔ STOP) → RELEASE; I/O messages on `txq`/`rxq` = an xfer header with
`stream_id` + the PCM payload + a status with `latency_bytes`; features
`MSG_POLLING`, `EVT_SHMEM_PERIODS`, `EVT_XRUNS`; events jack connected, PCM
period elapsed, xrun.
([virtio 1.2 csd01](https://docs.oasis-open.org/virtio/virtio/v1.2/csd01/virtio-v1.2-csd01.html))

**QEMU's device** (`hw/audio/virtio-snd.c`, master): advertises formats
S8/U8/S16/U16/S32/U32/**FLOAT** and rates 5512 … 48000 … 384000; defaults
`buffer_bytes` 8192, `period_bytes` 2048, 2 channels, S16, 48000 (= 512-frame
periods, 10.7 ms; a 42.7 ms buffer); the TX status "is returned when the
entire buffer is consumed by the audio backend"; **"virtio_snd: event queue is
unimplemented"** — no xrun and no period-elapsed events; and there is **no
state gating** (SET_PARAMS/PREPARE/START/STOP/RELEASE are accepted in any
state). The docs add: `jacks` and `chmaps` are "Unimplemented"; "the first
stream is always a playback stream, an optional second is always a capture
stream, and adding more cycles stream directions"; the device landed in QEMU
8.2.
([QEMU virtio-snd docs](https://www.qemu.org/docs/master/system/devices/virtio/virtio-snd.html),
[QEMU source](https://gitlab.com/qemu-project/qemu/-/raw/master/hw/audio/virtio-snd.c))
Two consequences for the driver (N-1): the completion of a TX message **is**
the period clock (no eventq to wait on), so the driver keeps `buffer_bytes /
period_bytes` messages in flight and treats each completion IRQ as "one
period elapsed" — exactly the Linux driver's and sDDF's pre-buffer protocol;
and the device is young — a 2026 write-up of a QEMU virtio-snd heap overflow
([OSEC, 2026-03-17](https://osec.io/blog/2026-03-17-virtio-snd-qemu-hypervisor-escape/))
is a reminder that the *device's* responses are untrusted input to our driver
(bound every `latency_bytes`, every status, every config field — the I-14
posture for Rlerror, applied to virtio).

**Host backends**: `coreaudio` (mac, a human listening), `wav` (a file — the
gate witness, playback-only), `pipewire`/`pa`/`alsa`/`jack` (the Pi). QEMU
polls its backend on a timer (`timer-period`, default 10 ms per the manual)
and adds its own buffer (`out.buffer-length`), so **QEMU contributes tens of
milliseconds that Nocturne cannot remove** — the latency budget (§6.9) states
guest-side and end-to-end numbers separately.

### 5.11 DSP: the convolution filter as the worked example

Real-time convolution of a long impulse response is done by **partitioned
convolution** (Stockham 1966; real-time audio use by Torger and Farina 2001):
uniform partitions trade latency for efficiency; **non-uniform** partitions
(Gardner 1995) put short blocks — or direct convolution — at the head for
zero added latency and longer FFT blocks in the tail; García's optimal
partitioning uses a Viterbi search. The per-node consequences: a descant
declares its **algorithmic latency** (0 for a Gardner head, one partition for
uniform), its **block size** (an FFT block wants a fixed frame count per
call), and it has a large **working set** (the IR spectra) that must be
allocated *outside* the cycle. Plug-in ABIs (LADSPA, LV2, VST3, CLAP) are all
in-process `process(in[], out[], nframes)`; AUv3 is the only shipping
out-of-process one, and the DAW SOTA (Bitwig's per-plugin sandboxes) shows
out-of-process hosting is viable at pro latencies when the transport is
shared memory, not RPC.
([Academia: non-uniform partitioned overlap-save](https://www.academia.edu/1392194/A_Low_Latency_Implementation_of_a_Non_Uniform_Partitioned_Overlap_and_Save_Algorithm_for_Real_Time_Applications),
[Partitioned-Convolution (García)](https://github.com/michaelkrzyzaniak/Partitioned-Convolution),
[iamReverb: efficient real-time convolution](https://iamreverb.com/efficient-real-time-convolution-in-iamreverb/))

---

## 6. The candidate design

### 6.1 Thesis

**An audio system whose graph is a file server, whose clients — including
DSP — are capability-bounded nodes in their own processes, whose cycle never
waits on any of them, on the substrate the display and the network already
use.** Plan 9 gives the shape (a tree; `bind` is routing; export is free),
PipeWire gives the graph and its cycle, Genode gives the inversion (the driver
is a client), Fuchsia gives the out-of-process effect contract and the
deadline-profile idea, JACK2 gives the never-wait discipline, and Thylacine's
own Loom/Weft/Warp give the ring, the share and the poke. The one thing none
of them offers together — user-program DSP with isolation, deadline
containment, and namespace-as-capability — is the NOVEL claim (§11).

### 6.2 Components

```
  Pouch:   SDL  -> SDL_thylacineaudio.c ----------------------------+
           Linux binaries (VIVARIUM) -> nocturne-pulse (N-5) --------+
  Native:  libnocturne (voice / ear / descant clients; pcmconv-class conversion)
                        |  9P control (/dev/nocturne/...)   |  per-node shared ring
           nocturned  --+-------------------------------------+--  the graph:
             tempo (the clock)  . mixer  . gains . resampler-at-voice-entry
             sink node(s)  <- virtio-snd driver thread (IRQ-clocked; CAP_HW_CREATE)
             policy = /lib/nocturne/policy + $HOME/lib/nocturne/policy (the conductor)
```

- **`nocturned`** — a warden-bound driver Proc (`binds = ["virtio-pci:25"]`,
  `needs { pci, irq, dma = "pool: 2 MiB" }`, `serves = "/dev/nocturne"`,
  `lifecycle = persistent`, `restart = on-crash`), the tapestryd shape. It
  owns the graph, runs the cycle, mixes, and serves the tree.
  **D-1a (placement, v1)**: the virtio-snd driver code lives *inside*
  `nocturned` as the sink node's backend, because the warden's driver-is-a-leaf
  rule (a narrowed driver cannot spawn, I-34) and the audited single-Proc
  driver idiom make one Proc the cheapest sound configuration. **D-1b (the
  seam)**: the sink node talks to its backend only through the same ring
  protocol every node uses, so the driver can later move to its own Proc
  (`nocturne-snd`, `serves = "/dev/nocturne/sinks/virtio0"`) — Genode's
  inversion, realized when a second driver (Pi HDMI, USB) makes it worth a
  hop. **D-1c (threads)**: `nocturned` is the first native daemon with two
  threads — the **cycle thread** (IRQ-waiting, hence INTERACTIVE by the
  existing promotion; does only ring work, mixing, and pokes; never touches
  9P) and the **control thread** (the `t_poll` loop serving `/srv/nocturne`,
  graph edits, policy). They share the graph under a lock the cycle thread
  takes non-blockingly (a failed `try_lock` in the cycle = "graph edit in
  progress, run last cycle's plan"), and the control thread never holds it
  across a 9P reply. This is a §"Self-audit" multi-thread-per-Proc surface
  and is prosecuted as such.
- **libnocturne** (`usr/lib/libnocturne`, no_std, the libtapestry idiom):
  opens the tree, maps the node ring, waits on the period, and links
  `pcmconv`-class conversion so a client may speak s16/44.1k and let the
  library (or the server, D-3) convert.
- **The SDL audio backend** (`usr/ports/sdl2/thylacine/SDL_thylacineaudio.c`):
  SDL's push-model driver hooks (`OpenDevice` → a voice sized to SDL's
  `samples`; `WaitDevice`/`PlayDevice`/`GetDeviceBuf` → the ring; capture
  hooks → an ear). Retires patch 0004 and `-nosound`.
- **`nocturne-pulse`** (N-5): a PulseAudio native-protocol server on the
  socket libpulse looks for, translating streams to voices/ears (§6.11).

### 6.3 The graph and the cycle

**Node kinds**: `voice` (in: none; out: the client's stream), `ear` (in: a
tap; out: to the client), `descant` (in: 1..n ports; out: 1..n ports; run by
the owner), `sink` (in: the mix; a device or `null`/`file`), `source` (out:
device capture), and exactly one **tempo** per running graph — the node whose
period paces everyone (the active sink's device, or a torpor-timed clock for
a `null` sink; JACK/PipeWire's driver node).

**Ports** carry mono float32 at the graph rate; a stereo voice is two ports.
Links connect an output port to an input port; the mixer is the implicit sum
at any input with several links (a gain per link).

**The cycle** (one per device period; PipeWire §5.2 made local):

1. The sink's period IRQ completes a TX message → the cycle thread wakes
   (INTERACTIVE), reads the device position, **checks the previous cycle**:
   every node whose slot was not marked FINISHED gets an xrun, and its
   substitute policy applies (§6.6).
2. Sets each node's `pending = required` in its ring header, stamps the
   cycle counter + the graph position/quantum, and **pokes** the roots
   (voices/sources have no dependencies: their poke is "produce the next
   period") — the Weft-4 single-cache-line poke, plus a Loom CQE on the
   node's `event` fid for a parked client.
3. Client nodes run **in their own Procs**: read input slots, write output
   slots, mark FINISHED, decrement their targets' `pending` (through the
   server: a client never writes another client's ring; the server is the
   only writer of every header's dependency words — the I-30 posture: shared
   words are the client's *inputs* to a snapshot, never the server's state).
4. The mixer (in the cycle thread) consumes FINISHED outputs at the
   in-cycle deadline, substitutes for the rest, applies gains, converts to
   the sink format, and submits the next TX message before the device's
   buffer runs dry (two periods in flight, §6.9).

**In-cycle vs next-cycle nodes.** A node declares `mode = incycle | deferred`.
An *incycle* node is expected to finish within the same period (JACK sync
semantics, zero added latency) and needs a cadence lease to be reliable under
load (§6.7); a *deferred* node's output is consumed one period later (JACK2
async semantics, +1 period) and needs no lease. Voices from ordinary programs
default to deferred; the SDL backend uses deferred; a descant defaults to
deferred and a leased descant may switch to incycle. This is how the
"one late client vs everyone pays a period" fork is dissolved: **each node
picks its own side**.

### 6.4 The tree (`/dev/nocturne`)

```
/dev/nocturne/
  ctl              write: graph-level verbs (quantum <n>, rate <hz>, tempo <sink>, load-policy)
  info             read: rate, quantum, tempo, cycle counter, xrun totals, latency (bufsize/buffered, Plan 9 words)
  graph            read: the whole graph rendered (nodes, ports, links, gains, latencies) -- ns-render style
  policy           read: the effective conductor policy (system tier + user tier merged)
  volume           read/write: Plan 9 volume(3) grammar over the active sink (+ `mix` / `dev` like mixfs)
  audio            read/write: the Plan 9 audio(3) file -- a write is a s16le stereo 44.1k voice (default;
                   `#A`-compatible); a read is an ear on the sink tap. Exists so `bind /dev/nocturne/audio /dev/audio` works.
  nodes/
    new            open+write: `voice|ear|descant name=<s> ports=<in>x<out> rate=<hz> format=<fmt> latency=<frames>
                   mode=incycle|deferred quantum-min=<n> quantum-max=<n>` -> returns the id (the tapestry `create` idiom)
    <id>/
      ctl          write: start|stop|gain <db>|mode ...|bypass on|off|lease <period> <capacity>
      info         read: state, xruns, misses, substitute policy, declared latency, lease, owner principal
      ports/<p>/   info + `insert` (open by the authority of §6.8 -> a descant insertion point)
      data         the ring fid: SYS_WEFT_MAP(this fd) maps the node's ring (Tweft answered by nocturned)
      event        single-shot Loom READ of node events (xrun, bypassed, lease revoked, link changed);
                   the period poke rides the ring header, not this file
  links/
    new            open+write: `<src-node>:<port> <dst-node>:<port> gain=<db>` -> id
    <id>/ctl,info
  sinks/<name>/    the device sinks (owner SYSTEM): info, volume, ctl (`default`)
  sources/<name>/  device capture (same shape)
```

Everything is a file; `ls`, `cat`, `echo >` operate the graph from a shell;
`graph` is the introspection the operator asked PipeWire's `pw-dump` for
without the object registry. Removing a node = closing the connection that
created it (the tapestry surface-lifetime idiom) or `ctl remove`.

### 6.5 The data path (the ring)

One ANON Burrow per node, allocated by `nocturned`, shared to the owner via
`SYS_WEFT_SHARE` → `Tweft`/`Rweft` on the node's `data` fid → the client's
`SYS_WEFT_MAP`. Layout (all little-endian, page-aligned):

```
  hdr   : magic, version, node id, rate, quantum, slots (K, power of two),
          cycle counter (server-written), position (frames since start),
          status word per slot: EMPTY | FILLED | FINISHED | XRUN | BYPASSED,
          pending / required (server-written), poke word (Weft-4 shape),
          declared latency (client-written at create, snapshotted by the server),
          stats (xruns, misses, last-cycle CPU ns -- client-reported, advisory)
  in[]  : K slots x n_in ports x quantum float32     (server -> client; descants and ears)
  out[] : K slots x n_out ports x quantum float32    (client -> server; voices and descants)
```

Rules, all inherited: the server **snapshots** every client-written word
before acting and never re-reads a slot it has validated (I-30/Weft); a slot
the server is reading is not the client's to write (Fuchsia's "between read
and play positions"); `K` ≥ 3 so a deferred node always has one slot being
read, one being written, one spare; the ring is RW/no-exec (W^X, I-12) and
counted against the owner's page budget (I-32); the share dies with the
node's fid (I-7's dual count keeps the pages alive until the last mapper is
gone). A voice may also **write samples through the 9P `data` fid** with no
ring at all (the `audio` file does exactly this) — the byte-copy fallback
Weft keeps below its hybrid threshold; the ring is the fast path, not the
only path.

### 6.6 Deadline isolation: the descant contract (R-1's mechanism)

A descant is a Fuchsia-effects-shaped processor without the RPC:

- **Declared at creation** (snapshotted by the server): ports in/out, `latency`
  in frames (the algorithmic shift, Fuchsia's `latency_frames`; the
  convolution head), `block` (a multiple the node needs per call; the server
  may only hand it `quantum` if `quantum % block == 0`, else the node is
  refused or runs deferred with server-side blocking), `mode`, and `quantum-min/max`
  (its tolerable range; the tempo will not choose outside the intersection of
  every incycle node's range — no quantum flapping).
- **Per cycle**: the server fills `in[slot]`, stamps the slot FILLED, pokes;
  the descant processes into `out[slot]`, marks FINISHED. That is the entire
  protocol; there is no call, no reply, no allocation on the path.
- **Deadline**: for an incycle node, the mixer's read point (period end minus
  a guard of `quantum/8`); for a deferred node, the next cycle's read point.
- **A miss substitutes, never waits**: the mixer applies the node's
  *substitute policy* — `bypass` (pass its input through unprocessed; the
  default for a descant, so a slow reverb degrades to dry, not to silence),
  `hold` (repeat the last finished slot; the default for a voice, hides a
  single late period), or `mute`. The miss is counted on THAT node
  (`info` → `misses`, `xruns`), an event is posted to its owner, and the
  cycle proceeds on time.
- **Auto-bypass**: `N` consecutive misses (default 8) latch `bypassed = on`;
  the node keeps being fed (so it can catch up) but is not waited for at all;
  `M` consecutive on-time cycles (default 64) or an explicit `ctl bypass off`
  unlatch. The owner sees it in `info` and on `event`. **The graph's other
  nodes never observe any of this** except through the substituted samples.
- **Latency accounting** (JACK's latency API, per node): end-to-end latency
  along any path = Σ declared latencies + one quantum per deferred hop + the
  sink's buffer; `info` reports it in frames and ms; a node may read its own
  path latency to align (a convolution reverb with a 0-latency head reports
  0; a uniform-partition one reports one partition).
- **Where the DSP runs**: in the owner's Proc, on whatever thread the owner
  parks on the poke. `nocturned` never maps executable code, never calls into
  a client, never blocks on one. A descant that crashes takes its own Proc;
  the ring's fid dies with it; the mixer bypasses it on the next cycle (the
  "graph never waits" rule already covers a dead node: it simply never
  FINISHES). This is the isolation WASAPI's audiodg and PipeWire's filter-chain
  do not have.

### 6.7 The cadence lease (the arc's one kernel lift; N-4, spec-first)

The problem, measured in the tree: a client node's thread is NORMAL, the
band has no cross-band aging, and a busy CPU can delay it by a slice (6 ms)
or more — longer than a 256-frame period. Without help, *incycle* descants
are unreliable under load, and the only escape is a bigger quantum (fine for
games, not for the operator's DSP ambition).

The candidate: a **cadence lease** is a kernel-held, per-thread, bounded,
revocable grant of *periodic* scheduling priority: `(period_ns, capacity_ns,
deadline_ns)` with `capacity ≤ deadline ≤ period`. A leased thread is
placed in the INTERACTIVE band **only while it has capacity left in the
current period** and is demoted to NORMAL for the rest of the period once it
has consumed `capacity` (the throttling Zircon leaves to feasibility and
CoreAudio to a warning; Thylacine enforces it so a leased thread cannot starve
NORMAL — the ARCH §8.3 caveat stays bounded). Admission: `nocturned` holds the
lease-granting authority (a new fork-non-grantable capability `CAP_CADENCE`
or, better, a **cadence allowance** conferred by the warden manifest —
`needs { cadence = "budget: 50%" }` — so the sum of leases it may confer is
itself bounded, I-34's shape on the CPU axis); it confers a lease to a node's
thread through the node's `ctl lease` verb (the node names its own thread by
tid; the lease is minted by the kernel for that tid, non-transferable, I-5's
shape); revocation on node removal, owner death, or `nocturned`'s own death
(`proc_revoke_allowance`'s shape). Feasibility is the warden's budget, not a
global scheduler theory.

Alternatives weighed: (a) reuse the sticky INTERACTIVE promotion — rejected,
unbounded (a promoted client could starve the console); (b) bigger quanta and
deferred mode only — **the v1 fallback**, shipped first (N-2/N-3), sufficient
for games and for descants at 10–20 ms periods; (c) run DSP in the server —
rejected (R-1's isolation); (d) full EEVDF deadline class — the RW-13 lift,
out of scope. The lease is (b) plus a bounded, capability-shaped (a). It gets
`specs/cadence.tla` (feasibility never exceeded; a lease never outlives its
grantor or grantee; demotion at capacity; no promotion without a lease) with
buggy cfgs, before the kernel code — the surface is the scheduler, so THE
EFFORT GATE fires at N-4 regardless.

### 6.8 Authority (the I-46 candidate, first half)

**Namespace is the capability.** A Proc that cannot see `/dev/nocturne` has
no audio at all (a container with no audio simply omits the mount — the
`/net` firewall idiom, NET-DESIGN §8). Within the tree:

- **Create a voice/ear/descant**: any Proc that can open `nodes/new`. The node
  is owned by the creating Proc's principal (the tapestryd `owner_principal`
  stamp); its ring is shared to that Proc only.
- **Insert a descant into a target** (open `nodes/<t>/ports/<p>/insert`): the
  **owner** of the target node, OR a Proc holding the **`nocturne-graph`
  clearance** (conferred through the `cap` device, corvus-gated, the I-2 row's
  clearance path) — the two-axis rule of I-26/I-39, so an ordinary program can
  filter its *own* output (the convolution-reverb-on-my-game case) and a
  system-level program (an equalizer for the whole sink) needs a grant. A
  descant inserted at a sink's input is the "system EQ" case and needs the
  clearance because the sink is SYSTEM-owned.
- **Route** (`links/new` between nodes of different owners): the owner of the
  *source* end, or the clearance. Policy (§6.10) is applied by `nocturned`
  itself and needs no client authority.
- **Read a tap** (an ear on another program's voice or on the sink): the owner
  of what is tapped, or the clearance — recording is eavesdropping otherwise.
  `/dev/nocturne/audio` reads tap the sink and are therefore clearance-gated
  by default (9front's loopback is not; ours is a security decision).
- **Sinks/sources** are SYSTEM-owned; `volume`/`default` on them need the
  clearance or the console-owner session (the trusted-path idiom for "the
  person at the keyboard changes the volume").

No new capability bit is needed for *use*; the clearance is one entry in the
existing corvus-gated table; the cadence authority (§6.7) is the only new
kernel-side authority and it is an allowance, not a cap.

### 6.9 Formats, rate, quantum, latency budget

- **Graph rate = the tempo sink's rate**, default 48 kHz (QEMU's device
  default and every HDMI/USB sink; 44.1 kHz only when a device insists).
  Internal format **float32 planar** per port (PipeWire's; sums without
  clipping; descants get floats). Voices may deliver `s8/u8/s16/s32/f32` at
  any rate; **the server converts at voice entry** (D-3: mixfs and Genode
  resample in the mixer; clients need not know the graph rate) — libnocturne
  also offers client-side conversion for programs that prefer to pay it
  themselves. Descants run at the graph rate only.
- **Quantum**: default 256 frames (5.33 ms at 48 kHz); the tempo picks the
  smallest value within every incycle node's `[quantum-min, quantum-max]`
  intersection, clamped to `[64, 2048]`; changes only on `ctl quantum` or a
  node join/leave that empties the intersection (announced on `event`, one
  cycle of silence — never mid-cycle). The device keeps **two periods in
  flight** (buffer = 2 × quantum), so a driver-side miss of a full period is
  still covered.
- **Budget to measure** (VISION §4.5 row candidates; numbers become claims
  only when measured, with the host stamped): cycle wake (IRQ → cycle thread
  running) p99 < 20 µs (the existing IRQ budget); cycle work at 8 voices +
  2 descants ≤ quantum/4; guest-side voice-to-sink latency (deferred voice) =
  1 quantum + 2 periods ≈ 16 ms at 256; end-to-end under QEMU adds the host
  backend (tens of ms; `timer-period`/`out.buffer-length` tunable, measured
  by the wav witness's timestamps); on thyla-pi under KVM + PipeWire the host
  adds PipeWire's own quantum. Target for a leased incycle descant path: **no
  added latency beyond its declared frames**.

### 6.10 Volume, mute, and the conductor (policy as data)

- Gains are graph stages (per link, per node, per sink) in dB, applied in the
  mixer in float; the sink's *hardware* volume (virtio-snd has none; HDMI has
  none; USB has one) is a separate knob never slaved to a stream — PulseAudio's
  flat volumes are explicitly not built.
- `volume` speaks Plan 9's grammar (`audio 80`, `audio 70 90`, `mix 50`,
  `dev hdmi0`); the Aurora/Halcyon volume OSD writes it; a "system volume" is
  the sink's gain stage.
- **The conductor** is two files, not a process: `/lib/nocturne/policy`
  (system tier, SYSTEM-owned, the bake ships defaults) and
  `$HOME/lib/nocturne/policy` (user tier), one line grammar:
  `route  principal=<p> | program=<name> | label=<glob>  ->  sink=<name> [gain=<db>]`,
  `default sink <name>`, `default quantum <n>`, `descant <name> auto-bypass <N> recover <M>`.
  `nocturned` merges them at start and on `ctl load-policy`; `policy` renders
  the effective set; the user tier persists a user's volume/route choices the
  way `/lib/aurora/config` persists the OSD (AURORA-CONFIG §3.2 "the writer
  defines the tier"). Device hot-plug (a USB sink appearing) is a warden
  event → `nocturned` creates the sink node and re-applies the policy — no
  Lua, no rule engine, no second daemon. If a future Halcyon "sound settings"
  panel needs more, it writes the file.

### 6.11 Compatibility on-ramps

- **SDL2** (Pouch): `SDL_thylacineaudio.c` implementing SDL's audio driver
  interface — `OpenDevice` creates a voice with `samples`-frame slots at
  SDL's requested spec (the server converts), `WaitDevice` parks on the poke,
  `GetDeviceBuf` hands the current out-slot, `PlayDevice` marks it FILLED;
  capture mirrors with an ear. DOSBox-X's mixer (48000 Hz, 1024 frames, 25 ms
  prebuffer) and TyrQuake's `snd_sdl` then work unpatched; patch 0004 is
  deleted, the `-nosound` flags go, `SDL_AUDIO_DRIVER_THYLACINE 1` joins the
  dummy in `SDL_config.h`. The DX-1 opus/speex stubs become real (CD-DA
  music) in the same chunk or the next.
- **VIVARIUM (Linux binaries)**: **D-8** — serve the **PulseAudio native
  protocol** on `$XDG_RUNTIME_DIR/pulse/native` from a `nocturne-pulse`
  process (a phenotype-visible Unix socket, the widest ABI: libpulse, ALSA's
  `pulse` plugin, SDL/mpv/ffmpeg/browsers all speak it; PipeWire itself ships
  `pipewire-pulse` for the same reason). ALSA `/dev/snd/*` ioctl emulation is
  rejected (a kernel-shaped ABI with no capability story); the PipeWire native
  protocol is a later option if a needed program speaks only that.
- **Plan 9 programs / scripts**: `bind /dev/nocturne/audio /dev/audio` and
  9front's `play`-style pipelines just work (§6.4).
- **Halcyon/Aurora**: the volume OSD and a per-tile mute are `volume` writes
  and `nodes/<id>/ctl gain` on the tile's program's voices (found by owner
  principal + the compositor's session — the H-4b actor plumbing).

### 6.12 Real hardware

Ordered by cost, none touching the graph: (1) **thyla-pi under KVM** with
`-audiodev pipewire` → the Pi's PipeWire → `vc4-hdmi` (N-6: the first sound
from Thylacine on silicon; zero new drivers). (2) **PWM-on-GPIO** on bare
metal (a bcm2835 DMA channel feeding the PWM FIFO; needs a filter/DAC HAT; a
small driver; a sink node). (3) **HDMI audio** on bare metal via the vc4 MAI
interface (DMA + clock/infoframe setup through the VideoCore mailbox — the
MENAGERIE §12 "mailbox first" chain; a real driver arc). (4) **USB audio
class 1/2** behind xHCI (VL805 on the Pi 4/400, RP1 on the Pi 5/500 —
MENAGERIE §12's PCIe/xHCI seams; the universal sink once USB exists). Each is
a `sinks/<name>` node with the same ring protocol; the driver-as-client split
(D-1b) is what makes (2)–(4) additive.

### 6.13 Invariant I-46 (candidate) and the audit surfaces

**I-46 — Audio authority is bounded by the namespace + owner-or-clearance
axis, and the audio cycle never depends on an untrusted node.** (a) A Proc's
audio authority is exactly the `/dev/nocturne` names it can reach, further
gated per operation by the two-axis rule of §6.8 (owner OR the
`nocturne-graph` clearance); DSP code supplied by a program runs only in
that program's Proc — `nocturned` never maps, calls, or waits on client code.
(b) **No-stall**: every cycle completes at the sink's deadline regardless of
any client's behaviour (late, dead, hostile, or absent); a node's miss is
observable only in its own substituted output and counters. (c) **Ring
integrity**: the server acts only on snapshotted, bounds-validated
client-written words (I-30 lifted, as Weft did); a period's slot stays
mapped and backed from FILLED to consumed (the T-1/I-40 shape); the sink's
DMA never reads a page a client can still write. (d) **The cadence lease** is
bounded (capacity ≤ deadline ≤ period), non-transferable, revoked with its
node/owner/grantor, and never lets a leased thread exceed its capacity in a
period (the I-32/I-34 shape on the CPU axis).

Validation: `specs/nocturne_cycle.tla` (the cycle with late/dead/hostile
nodes: `CycleAlwaysCompletes`, `SubstituteIsLocal`, `BypassLatchSound`; buggy
cfgs `buggy_wait_on_client`, `buggy_reread_slot`, `buggy_bypass_unlatch`) and
`specs/cadence.tla` (§6.7) — spec-first re-enabled for both, per the
surface-by-surface precedent; the focused audits per chunk; the wav witness +
the slow-descant witness + the SMP gate.

**Audit-trigger rows to reserve at impl** (appended to `docs/AUDIT-TRIGGERS.md`
per chunk, not now): the virtio-snd driver (device-response bounding; the
in-flight-message accounting; DMA pool lifetime on death); `nocturned`'s ring
consumer + the two-thread graph lock; the descant substitute/bypass machinery;
the `Tweft` answer path in a second userspace server; the cadence-lease kernel
object + the scheduler demotion; the SDL audio boundary-line patch; the
`nocturne-pulse` protocol parser (untrusted wire input, the halcyond
transcript class).

---

## 7. Gates and witnesses (the instruments come before the code)

- **W-1 The wav witness.** `THYLACINE_AUDIODEV=wav` makes run-vm.sh add
  `-audiodev wav,id=snd0,path=build/audio-<label>.wav` + `virtio-sound-pci,streams=1`.
  A guest probe (`/nocturne-probe`, the joey ladder idiom) plays a 1 kHz sine
  for 2 s at −12 dBFS. The host check (`tools/audio-verdict.py`, stdlib only
  like `gfx_fp.py`): RMS in the window, a DFT bin at 1 kHz ≥ 20 dB above the
  median bin, and **controls**: the same probe at 2 kHz must move the peak
  (positive, one variable away); a silent guest must fail the RMS floor
  (negative); the verdict is keyed on the *file*, never on a guest log line
  (the #186 lesson). Deterministic; no host audio hardware; runs on the mac
  and the Pi. **AS-BUILT at N-1 (2026-09-05), with two corrections the subject
  taught the instrument:** QEMU's `wav` backend appends only while the guest's
  stream runs, so the capture BEGINS with the first period played -- there is
  no silent prefix to check -- and it patches the RIFF/data sizes only on a
  clean exit, which the harness never gives it (it kills QEMU), so the reader
  ignores the header sizes. The negative control is therefore the silent
  **tail** (the probe's 0.2 s + the driver's idle-stop silence: an empty FIFO
  must yield silence, never a repeated buffer or noise); the probe plays
  1 kHz then 2 kHz (the positive control + the order), the span must be
  contiguous (an underrun shows as a gap), and `tools/audio-verdict.py
  --selftest` proves the discrimination on nine synthetic cases. First real
  capture: 319488 bytes = 1.66 s, `PASS: 1000 Hz x 25 windows (median 12),
  2000 Hz x 25 windows (median 37); silent tail 33 windows; ambiguous 0`.
- **W-2 The no-stall witness.** Two voices + one descant whose owner is
  deliberately slow (sleeps a period every third cycle) and one whose owner
  is killed mid-stream: the wav still carries the tone unbroken (a DFT per
  100 ms window, every window passes), `info` shows the misses on the slow
  node only, `bypassed` latches on the dead one. A sabotage arm (`nocturned`
  built with the wait-on-client bug) must FAIL W-2 — a gate that cannot fail
  proves nothing.
- **W-3 Timing.** `info`'s xrun totals over 10 s at quantum 256 under a
  concurrent `go build`-class load must stay 0 for the driver (the sink never
  underruns); node misses are reported per node with the host conditions
  stamped (cores, concurrent gates — never "host load" as an explanation).
- **W-4 The SDL leg.** DOSBox-X's `ls-gfx-dosbox` scenario gains an audio
  arm: the Duke3D title music produces energy in the wav (RMS floor +
  spectral flatness below a bound, so a buzz is not music), with the existing
  video witness untouched.
- **W-5 Silicon.** thyla-pi under KVM with `pipewire`: `pw-top` on the host
  shows the QEMU stream; the wav witness runs there too (the Pi's QEMU has
  `wav`), so the silicon leg has the same deterministic verdict plus a human
  HDMI listen.
- The kernel chunk (N-4) adds `test_cadence` to the in-kernel suite + the SMP
  gate; every chunk runs LS-CI (with `LS_CI_JOBS=1` for the timing-thin
  arms) and the focused audit.

---

## 8. Sequencing (N-0 … N-7)

| Chunk | Delivers | Gate | Effort-gate |
|---|---|---|---|
| **N-0** (this) | `docs/NOCTURNE.md`; the scripture reconciliations (§2); I-46 reserved; the NOVEL candidate | — | design; no question |
| **N-1** virtio-snd driver + the wav witness — **LANDED 2026-09-05 (aux-3 @562cbe50)** | the `virtio-pci:25` bind + `nocturned` owning the device (`usr/nocturned`: the modern-PCI transport, the control/TX queues in one 40 KiB DMA pool, PCM_INFO/SET_PARAMS/PREPARE/START/STOP/RELEASE, 4 x 2048 B periods in flight, the device-controlled used id validated before it steers a slot, the idle STOP), the `/srv/nocturne` tree `{audio,info,ctl}` with the bounded FIFO + the deferred (blocking) Rwrite, `/nocturne-probe` in the boot ladder (FATAL when the mount is up), `kernel/devdev.c`'s `/dev/nocturne` stub, run-vm.sh's `THYLACINE_AUDIODEV`, `tools/audio-verdict.py` + `tools/test-audio.sh`; `docs/reference/153-nocturne.md` + `docs/manual/41-audio.md` | W-1 GREEN (boot probe: 77 periods played, 0 silence, 0 errors; the wav capture judged PASS) | ran at max; the focused holotype + the SMP gate are batched to the N-2 close (the double-the-distance rule) |
| **N-2** the graph core + voices + SDL (SUB-SPLIT; each sub-chunk lands + gates independently) | as below | as below | as below |
| &nbsp;&nbsp;**N-2a-1** multi-voice mixing (byte-copy) -- **LANDED 2026-09-05 (aux-3)** | `nocturned` multiple **voices** (per-voice bounded FIFO + gain) MIXED in float32 to the one sink; `nodes/new` + per-voice `audio`/`ctl`/`info`; voice 0 = the root `audio` file; the byte-copy fallback (section 6.5), NOT the Weft ring. The `nodes/` surface is minimal -- ports/links/descant ABI (section 9/10, operator's) NOT built | W-1 chord (both tones SAME window; `audio-verdict.py --chord`; sequential FAILS it) GREEN + full boot ladder green | ran at max; holotype + SMP gate batched to the N-2 close |
| &nbsp;&nbsp;**N-2b** the Weft zero-copy ring voice | `nodes/<id>/data` + SYS_WEFT_SHARE -> Tweft (the netd/tapestryd precedent); the poke/period protocol; the descant substrate | a ring-voice witness | ring consumer: **ASK unless max** |
| &nbsp;&nbsp;**N-2a-2** the SDL audio backend -- **LANDED 2026-09-05 (aux-3)** | `usr/ports/sdl2/thylacine/SDL_thylacineaudio.{c,h}` (SDL's `THYLACINEAUDIO_bootstrap`, registered ahead of DUMMY by patch 0002, auto-selected when `/srv/nocturne` is present; a voice minted over a DIRECT `/srv/nocturne` conn so it reaps on exit; the device format forced and SDL converts; pacing rides the voice's blocking write, no timer) + `/sdl-audio-probe` + `tools/test-sdl-audio.sh` (joey runs the SDL probe INSTEAD of the N-1 probe under `thylacine.sdlaudio`, so the wav is a clean SDL capture) | the SDL chord witness GREEN (`PASS(chord): 78 windows`, driver=thylacine) + the N-1 witness unchanged GREEN | ran at max; the game-sound flip split out as N-2a-3 |
| &nbsp;&nbsp;**N-2a-3** the Quake sound flip -- **LANDED 2026-09-05 (aux-3)** | TyrQuake's software build selects `snd_sdl` over `snd_null` (the GL build already had it); the play scenarios (`ls-gfx-quake`/`play`/`glquake`) drop `-nosound` + assert `Sound Initialized`; `quarry` splits `PLAY_ARGS` (sound) from `BENCH_ARGS` (`-nosound` kept); sdl2 patch `0003` makes DUMMY an auto-selectable fallback; `audio-verdict.py --music` + `tools/test-game-audio.sh` the W-4 witness (energy that is neither noise nor a buzz). Bench/wedge/venus KEEP `-nosound` | W-4 `--music` on `ls-gfx-quake` GREEN + the `--music` selftest + the two real chord captures as negative controls | ran at max |
| &nbsp;&nbsp;**N-2a-4** DOSBox-X sound (clade host) | retire `usr/ports/dosbox-x/patches/0004-thylacine-force-dummy-audio` AND flip the DX build's `nosound` config (`tools/dosbox-x-sources.py`); verify on a clade-capable host (thyla-pi) -- DOSBox builds only via `build_clade`/`stage_clade`, not a dev-host `build all`; + tyr-glquake sound (needs clade GL) + the owed DOSBox fullscreen re-run | W-4 `--music` on `ls-gfx-dosbox`/`duke3d` | re-enables DOSBox audio-init never run on Thylacine; ASK/max on the clade host |
| &nbsp;&nbsp;**N-2c** the cycle/control thread split | the two-thread daemon (D-1c): the IRQ-clocked cycle thread + the 9P control thread, a try-locked graph | a multi-thread self-audit surface | **ASK unless max** |
| **N-3** ears, policy, sinks | capture (`ear`, `source`), the conductor files, `sinks/`, hot-plug via the warden, the user tier, the Halcyon volume hook | W-1 capture arm (needs a non-wav backend: `coreaudio` loopback or the Pi's PipeWire null-sink), LS-CI | routine unless the tap authority changes |
| **N-4** descants + the cadence lease | the descant contract, substitute/bypass, latency accounting, `specs/nocturne_cycle.tla`; **the kernel lift**: `specs/cadence.tla` → `KObj_Cadence`/allowance + scheduler demotion; the convolution-filter demo (a native descant with a Gardner head; the operator's example) | W-2 (+ the sabotage arm), W-3, SMP gate, the focused audit (P0/P1 → dirty-close discipline) | scheduler + a new kobj: **max, spec-first, no exceptions** |
| **N-5** Linux compat | `nocturne-pulse` under VIVARIUM; mpv/ffplay/SDL-pulse binaries play | a curl-demo-style Linux binary leg | a wire parser: ASK unless max |
| **N-6** silicon | thyla-pi under KVM + `pipewire`; the wav + `pw-top` witness; a JOURNAL number set | W-5 | routine |
| **N-7** (v1.x seam) | bare-metal sinks (PWM, vc4 HDMI MAI, USB audio class) as they become reachable via MENAGERIE §12 | per driver | driver arcs |

N-1 and N-2 are the "sound in the games" milestone; N-4 is the operator's
requirement made real; N-5/N-6 are reach. Each chunk lands with its reference
section (`docs/reference/NNN-nocturne*.md` — or the vault dossier once the
cutover lands; run `quaestor owner` at the doc step), its manual chapter
(`docs/manual/41-audio.md`, the DOSBox chapter's grow-by-chapter precedent),
and its JOURNAL entry.

---

## 9. Decisions taken in this pass (auto-ratified provisionally; overturnable)

Each names its precedent, fit, cost and the alternative rejected, per the
research-before-fork rule.

- **D-1 Driver placement**: the virtio-snd driver lives inside `nocturned`
  (a) behind the node ring protocol (b), `nocturned` is two-threaded (c).
  Precedent: tapestryd (one Proc, gathered devices); Genode 24.02 (driver as
  client). Fit: the warden's leaf rule + one audited Proc. Cost: the first
  multi-thread native daemon (a §"Self-audit" surface). Rejected: a separate
  driver Proc from day one (a ring hop for one device, no second driver yet).
- **D-2 Policy is data**: two policy files merged by the daemon; no session
  manager process, no scripting. Precedent: Genode's XML mixer policy; the
  aurora-config tiers. Rejected: a WirePlumber-like process (the operator's
  named quirk), a Lua/rules engine.
- **D-3 Float32 planar at the sink's rate; server-side conversion at voice
  entry; descants at graph rate only.** Precedent: PipeWire (float, graph
  rate), mixfs + Genode (mixer resamples). Rejected: s16 internal (clipping
  sums, poor DSP), client-must-match-rate (Genode's old sessions' failure).
- **D-4 Async by construction, per node**: a late node is substituted
  (`bypass|hold|mute`), counted, auto-bypassed after N; each node chooses
  incycle/deferred. Precedent: JACK2 async, PipeWire xrun marking, Fuchsia
  metrics. Rejected: JACK sync (a late client stalls the sink), whole-graph
  async (everyone pays a period).
- **D-5 The cadence lease is the one kernel lift, lands at N-4, spec-first,
  as a warden-budgeted allowance on the CPU axis.** Precedent: Zircon deadline
  profiles, CoreAudio time-constraint threads, I-34. Rejected: sticky
  INTERACTIVE promotion (unbounded), no lease ever (fails the operator's
  incycle DSP ambition), full EEVDF deadline class now (RW-13's lift).
- **D-6 Authority = namespace ∩ (owner OR `nocturne-graph` clearance)**; no new
  cap bit; taps are clearance-gated. Precedent: I-26/I-39 two-axis, tapestryd's
  owner stamp, NET-DESIGN §8 firewall-by-namespace. Rejected: PipeWire-style
  per-object permission tables (a registry), a `SPAWN_PERM_AUDIO` bit (a bit
  where a name suffices).
- **D-7 Plan 9 compatibility files** (`audio`, `volume`, `bufsize/buffered`
  words) kept as first-class. Precedent: audio(3), mixfs. Cost: negligible.
- **D-8 Linux compat = the PulseAudio native protocol.** Precedent:
  pipewire-pulse. Rejected: ALSA ioctl emulation, PipeWire native first.
- **D-9 The wav backend is the gate witness**, playback-only, with a spectral
  verdict and both controls. Precedent: `tools/screendump.sh` + `gfx_fp.py`
  (the graphics arc's "agentic eyes"). Rejected: asserting on guest log
  lines (#186).
- **D-10 Names** (§1) proposed and HELD for signoff; the design reads with the
  plain fallbacks.
- **D-11 The device sink advertises FLOAT to the driver only if the backend
  does** (QEMU does); the driver negotiates S16 as the floor. Precedent: the
  virtio spec's PCM info bits.

---

## 10. Open questions for the operator (the residue research cannot settle)

1. **The sub-names** (§1): `voice`/`ear`/`descant`/`tempo`/`conductor`/
   `cadence` — keep, prune, or rename.
2. **Ambition level for incycle DSP**: is sub-10 ms leased in-cycle
   processing (N-4's cadence lease) wanted for v1.0 of the arc, or is
   deferred-mode DSP at 10–20 ms periods enough for now (N-4 then ships the
   contract + specs and defers the kernel lift)? This decides whether the
   scheduler lift is on the critical path.
3. **Default quantum/rate** (256 @ 48 kHz proposed) and whether the tempo may
   ever change the quantum automatically (proposed: never — only on `ctl` or
   a join/leave that empties the intersection).
4. **Tap authority**: should a user's *own* voices be tappable by that user's
   other programs without the clearance (proposed: yes — same principal is
   the owner axis), and should `/dev/nocturne/audio` reads (sink loopback)
   need the clearance (proposed: yes)?
5. **The policy grammar's identity keys**: principal + program name + label
   (proposed); anything Halcyon will need beyond that (per-tile?) is a later
   key, not a redesign.
6. **Linux compat protocol** (D-8, pulse) — confirm, given the operator's
   PipeWire experience; and whether libpulse's *capture* side matters early.
7. **Descant multi-input** (side-chains, e.g. a ducker): the port model allows
   it; confirm it is in scope for the first descant chunk or deferred.
8. **MIDI** and **Bluetooth**: named out of scope; confirm.
9. **The scripture touch to VISION §9** (relaxing a non-goal "requires
   re-opening this section"): the operator's direction did that verbally;
   confirm the wording proposed in the scripture commit.

---

## 11. The NOVEL candidate (for `docs/NOVEL.md` "Post-v1.0 candidates" or a new angle)

**Nocturne — an audio graph that is a file server, whose DSP is a
capability-bounded client, not a plugin.** Three claims, none made together
by a shipping system: (1) **audio authority is the namespace** — a Proc that
cannot see `/dev/nocturne` has no audio; insertion, routing and tapping are
owner-or-clearance on files, with no registry and no new capability bit
(PipeWire keeps a per-client permission table on a global object registry;
Genode opens typed sessions; Fuchsia routes FIDL capabilities). (2) **user
programs extend the processing graph with their own DSP, isolated**: a
descant runs in its owner's Proc over a shared ring, the server never loads,
calls, or waits on it; every shipping desktop stack loads third-party DSP
into the engine (WASAPI APOs in `audiodg`, PipeWire's filter-chain in the
daemon, CoreAudio AUs in-process by default) and Fuchsia's out-of-process
effects protocol is system-provisioned, not program-supplied. (3) **deadline
containment per node**: the cycle never waits; a late node degrades only
itself, and in-cycle guarantees are a bounded, revocable, warden-budgeted
*lease* on the scheduler rather than a real-time class — Zircon's deadline
profile shaped as I-34's allowance. On the same Loom/Weft substrate as the
display and the network, which is Angle #1 (9P totalized) reaching the last
subsystem Plan 9 left as a single writer on one file.

---

## 12. Cross-references

`docs/VISION.md` §4.5, §9, §13.2 · `docs/ROADMAP.md` §12.3, §12.5 ·
`docs/TAPESTRY.md` §2, §3, §9, §10, §18.4 · `docs/LOOM.md` §10 ·
`docs/NET-DESIGN.md` §8, §12, §13 · `docs/NET-THROUGHPUT.md` §4.6, §6 ·
`docs/reference/125-weft.md` · `docs/reference/107-loom.md` ·
`docs/reference/139-tapestryd.md` · `docs/reference/118-libdriver.md` ·
`docs/MENAGERIE.md` §4, §6, §12 · `docs/GPU-DESIGN.md` §8 (the I-45 halves
idiom) · `docs/AURORA-CONFIG.md` §3.2 · `docs/IDENTITY-DESIGN.md` §9.8 ·
`docs/VIVARIUM.md` · `docs/DOSBOX.md` (DX-3 "audio") ·
`memory/project_audio_arc_future.md` (the operator's requirement, verbatim).

Sources fetched in this pass are linked inline in §5; the two PDFs (Letz
2009, the sDDF design) and the IWP9 2026 paper were text-extracted on
thyla-pi with `pdftotext` because the mac lacks poppler and the paper's TLS
chain is incomplete.
