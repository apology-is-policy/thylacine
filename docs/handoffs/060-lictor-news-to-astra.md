# Handoff 060 -- Lictor tells the compositor when to ask (to Astra)

**From**: corona, 2026-11-18. **To**: Astra. **Why you**: Lictor is yours; the
operator's word of 2026-11-11 lets the other agents change it while you are away
and asks for a note here each time.

## Why

The ci image idled at 15-36% CPU (OPEN-BUGS 2026-11-18T13:47Z). A census inside
Lictor counted ~372 broker calls a second at idle: every tapestryd serve pass
asked for the seat, drained all three input devices and asked five fence
questions, and the seat coordinator asked for the seat every 10 ms. Each call was
three 9P round trips with a cross-CPU wake each way. The operator chose an
event-driven broker (vote 147).

## What changed in Lictor

- `src/news.rs` (new, host-tested): the news rule. Classes SEAT (the seat pair),
  INPUT (an input queue) and GPU (a fence list, or the engine's state and stall
  count). The seat and the engine are news when they differ from what the reader
  was told. A queue is news when a take would return something AND a take has run
  since the reader was last told while it was ready, so the take, not the reply,
  rearms it: an undrained device is told once and never spins a reader.
- `src/backend/server.rs`: `news` (path 3, read-only) in the broker tree. A
  Tread's offset is the interest mask; it parks (one per connection) until its
  interest has news and is answered with a u32 of the classes. Tflush and Tclunk
  drop it. `Conn::notify` answers at the end of every pass.
- `src/main.rs`: takes are counted per queue when they run (InputDrain,
  TakeCompletions, TakeVindications); the facts are read after every request of
  the pass; the loop polls at 1 ms while `fenced_outstanding > 0` on a live
  engine in a normal seat (the pace tapestryd used to keep by asking), else 10 ms.
- `src/backend/intake.rs`: the input thread writes a byte to a pipe the serve loop
  polls when it queues a batch, so input news waits on its 4 ms pace only.
- `src/backend/gpu.rs`: `fence_news()`, whether each take would return something.
- `src/proxy/news.rs` (new): `Watch`, the client half -- its own connection, a
  thread parked in the news read, a local pipe to poll. A pollable (QTPOLL) file
  was not used: the kernel samples a remote file with a round trip on every poll
  pass, which would have cost tapestryd one per pass.
- `src/rpc_client.rs`: a reply that fits one read is read in one (2 round trips
  per call, not 3); `open()` for any file of the tree.

Every news bit is something the compositor could already ask for, read where the
pass answered its requests, so news reveals nothing the requests did not -- in
particular nothing about input the seat kept back from a trusted screen, which
leaves the queue before the pass ends (I-27).

## What changed in tapestryd

The serve loop asks for the seat, drains input and takes fences only on news; its
fence pumps still run every pass; an episode waits on the watch instead of a
10 ms sleep loop; the 1 ms clamp is gone. The seat coordinator asks for the seat
only on news and blocks while it has no lanes in a normal seat.

## Measured

q-eb1 (2026-11-18, ci image, HVF): a census inside Lictor counted ~3 calls/s at
idle after boot OK, all present traffic, against ~372/s before; the strict idle
gate read 8.4% and 9.5% (15-36% before, every run red). Audit round 51 (Opus,
same family): 0 P0, 0 P1, 0 P2, 4 P3, all closed: no fenced 1 ms pace in an
episode; no queue is news in an episode (a vindication's time is not otherwise
visible to the compositor then); no redundant poll on GPU news; the Watch cleans
up if its thread cannot start.

## Not changed

Lictor's own loop still wakes at 100 Hz with nothing to do (the seat endpoint is
not pollable), and the input thread still polls the eventqs every 4 ms (vote 72).
