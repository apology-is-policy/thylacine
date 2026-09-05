---
id: sub-kaua-term
type: sub
title: "kaua-term — the crash-isolated per-tile terminal and its record stream"
parent: moc-userspace-shell-tui
code:
  - usr/kaua-term/src/lib.rs
  - usr/kaua-term/src/wire.rs
  - usr/kaua-term/src/main.rs
  - usr/kaua-term/Cargo.toml
audit: hard
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: ["docs/KAUA-TERM.md"]
created: 2026-09-05
updated: 2026-09-05
---
## Purpose

`kaua-term` is the crash-isolated terminal a session tile runs: ONE process
per tile, spawned by the per-user [[sub-halcyond]] AS the user (`--session`),
holding the pts, running the [[sub-lib-vt]] parser over the app's output, and
shipping halcyond a pre-digested RECORD stream instead of raw bytes. The point
is the isolation boundary: halcyond never parses VT for a tile, so the
untrusted parse of an app's output — a format-fuzz surface — happens in a
process whose death is a held affordance (a tile that can be restarted), not a
compromised session compositor. This is the KT-1 seam (`docs/KAUA-TERM.md`
topology Y + the FEED-CELLS seam B).

## Contract

The library (`lib.rs` + `wire.rs`, host-buildable) is a pure event model:

- `Producer::feed(vt, bytes, out)` turns a chunk of app output into `Record`s,
  applying each byte's cell effect through the vt and emitting `CellDiff` /
  `ScrollOff` / `Mode` / `Control` in the order the terminal produced them.
- `Producer::feed_into(vt, bytes, out, sink)` is the same, but SHIPS through
  `sink` whenever the cells held in `out` reach the bound — the form the bin
  must use for bulk output.
- `Producer::resized(vt, out)` resyncs the shadow screen after a geometry
  change and emits the full diff.
- `wire::encode_record` / `parse_record` are the framed codec up (records) and
  `encode_input` / `FrameDecoder` down, with `MAX_FRAME` = 4 MiB the decoder's
  hard bound and `MAX_TITLE` = 256 the parse-time title cap. The DOWN channel
  carries `Input::Key` / `Resize` and (H-4d) `Input::Text(Vec<u8>)` -- a byte run
  the compositor's tile menu types (`^E^U<cmd>\n`) that the INPUT thread writes to
  the master *verbatim* under the key lock, not re-encoded like a key. The UP
  channel's `Control` gained `Osc1936Raw { serial, frame }` -- the raw Beacon
  frame plus its span serial -- and the wire cell is now 17 bytes (`ch`/`fg`/`bg`
  u32, `attrs` u8, `span` u32; the trailing `span` is what the H-4d Beacon
  threading added).

The bin (`main.rs`): `kaua-term [--beacon none|cells|rich] <cols> <rows> [prog
[args...]]` — fd 0 is the DOWN channel (halcyond's `Input` frames), fd 1 the UP
channel (the `Record` frames), fd 2 inherited. `--beacon` (default `none`, H-4d)
is written to the tile's *own* `/env/BEACON` before the slave spawn, so the shell
it hosts inherits the render tier this tile's renderer declared -- the word
[[sub-utopia-interactive]]'s `env_beacon_tier` then reads. It opens a pts pair
through [[sub-ptyhold]], spawns `prog` (default `/bin/ut`, `--home` forwarded
verbatim) on the slave, and runs two threads: INPUT (fd 0 keys re-encoded to the
master, `Input::Text` written verbatim; `Resize` -> the pts winsize) and OUTPUT
(master -> vt -> Producer -> records -> fd 1).

## Mechanism

**Boundary-accurate record order (the load-bearing property).**
`Vt::feed_until` (the vt's capture mode) returns at every boundary — a scroll,
a bell, an OSC, an alt-screen enter/leave — having applied the byte's cell
effect. The producer flushes a pending `CellDiff` before acting on each
boundary, so the ORDER between records is guaranteed: a Beacon zone frame lands
between exactly the cells it separates, a scrolled row precedes the screen that
no longer holds it. Consecutive scrolls coalesce into one `ScrollOff`.

**The bounds are per record CLASS, not per read — this is the security core.**
The number of rows a chunk yields is the VT's to decide, not the chunk's size:
`ESC [ 36 S` is five bytes and thirty-six rows, so a 4 KiB read can synthesize
~30K rows (tens of MiB of cells) before a single byte is written. Two bounds
close this, and both had to be found the hard way (rounds 2-3):

- `scroll_cap()` = `min(MAX_FRAME/2, SCROLL_ACC_BYTES = 256 KiB) / per_row`
  bounds ONE `ScrollOff` so it fits the consumer's 4 MiB decoder AND the
  producer's own heap (a record is held as cells, serialized, then framed —
  three copies).
- `feed_into`'s sink ships (serialize + write + clear `out`) whenever
  `cells_in(out)` — ScrollOff rows PLUS CellDiff entries — reaches
  `SCROLL_ACC_BYTES`, checked after EVERY boundary. The check must count every
  record class that scales with the screen: alt-screen enter/leave each push a
  FULL screen diff, and eight bytes of `?1049h`/`?1049l` per toggle made a
  4 KiB read 512 screens (45 MiB at 128x36) until round 3 F1 folded them into
  the same trigger. A bound on one class is not a bound.

**The alt screen against a one-grid consumer.** The consumer keeps ONE grid,
so an alt-screen enter must push a full CellDiff of blank rows (to overwrite
the main's text) and a leave a full CellDiff of the restored main (to overwrite
the alt's last frame). The producer resets its shadow and emits the full diff
on each toggle.

**Resize ordering vs a SIGWINCH repaint.** `apply_resize` runs BEFORE and AFTER
the parked master read (B-F6: the bytes that read returns are usually the app's
SIGWINCH repaint at the NEW size). It first drains the vt's pending boundaries
through `Producer::drain_pending` — ROWS ONLY, no screen diff, because the
shadow still holds the old geometry and a diff against it would address the new
cells at the old pitch whenever the cell count is unchanged (round 3 F3:
80x24 -> 96x20) — so a shrink's scrolled-off rows precede the resized screen's
full diff in one emit, then `resized` resyncs the shadow.

## Data structures

`Record` = `CellDiff { changed: Vec<(u16,u16,Cell)>, cursor }` | `ScrollOff {
rows: Vec<Vec<Cell>> }` | `Control(Control)` | `Mode(ScreenMode)`. `Control` =
`Osc1936Raw(Vec<u8>)` (a Beacon frame, passed through opaque) | `Title(String)`
| `Bell` | `Exit(i32)` | `WinsizeAck`. `Producer` holds a shadow screen
(`cols x rows` cells) it diffs against.

`wire`: length-prefixed frames; `FrameDecoder` compacts its buffer and yields
`Err(TooLarge)` on a declared length past `MAX_FRAME` (an unrecoverable stream
desync). `MAX_FRAME` = 4 MiB, `MAX_TITLE` = 256.

The bin's shared state: `ThylaAllocN<32 MiB>` global allocator (B-F4: 4 MiB
could not hold one capped `ScrollOff` and its two serializations),
`master_write: WriteLock` (a `torpor` futex), and two relaxed atomics —
`app_cursor` and `pending_resize`.

## Concurrency

Two threads, one lock. The master-write futex (`WriteLock`) serializes the
input thread's key writes against the output thread's terminal replies (CPR,
DSR, DA); a reply interleaved mid-keystroke would corrupt the app's stdin.
Master reads are lock-free. The two atomics are benign by design: `app_cursor`
(the input thread's key-encoding mode) and `pending_resize` (posted by input,
applied on output) — a late observation costs one frame and self-corrects. A
zero-count master write is raw-mode back-pressure: `write_all` retries 200 x
1 ms parked (not spun) under the lock, so a CPR reply waits at most a bounded
~400 ms before the remainder is dropped (B-F8).

## Invariants enforced

None of the enumerated §28 invariants directly — kaua-term is a userspace
client above the privilege boundary. What it enforces is the KT-1 seam's
security posture, prosecuted below: the untrusted parse is confined to this
process (crash-isolation), its output is bounded per record class (a malicious
or pathological app cannot exhaust halcyond's or its own heap), and it is
spawned AS the user with no ambient authority (`.caps(!T_CAP_SET_IDENTITY)` at
the halcyond spawn — I-2/I-22, enforced on the spawn side). It sits on the
`docs/AUDIT-TRIGGERS.md` "KT-1: the kaua-term seam" row; the round records are
`vault/record/audits/adt-kt1-r{1,2,3}.md`.

## Error paths

A down-channel frame the decoder refuses (`TooLarge`) ends the process cleanly
(`'down` break -> `t_exit_group`, B-F14) — halcyond never emits one, so the arm
is robustness against a corrupt stream. A parse error on an up-record is a
wire-format defect on kaua-term's own output, so it is a bug, not a runtime
path. The bin's exit ships a `Control::Exit(code)` before closing fd 1 so the
tile shows the status rather than a dead grid.

## Performance

One 32 MiB heap span, lazily backed. The three-copies cost of a record (cells
-> serialized -> framed) is what `scroll_cap` and the sink budget are sized
against. The steady state is a `CellDiff` per boundary batch plus coalesced
`ScrollOff`s; the sink bounds the transient working set to `SCROLL_ACC_BYTES`
plus one capped record regardless of how much the app dumps at once.

## Prosecution

- **The decoder against a hostile frame.** Malformed / oversize / truncated /
  interleaved down-frames: a length past `MAX_FRAME` yields `TooLarge` and
  tears down; a payload under-running its declared length is refused; a title
  past `MAX_TITLE` is truncated at parse. State the bytes.
- **The bounds against a pathological app.** `ESC [ N S` (N rows per few
  bytes) and `?1049h`/`?1049l` toggles (a full screen per eight bytes) are the
  amplifiers; `feed_into`'s sink must trigger on `cells_in` after EVERY
  boundary, counting BOTH ScrollOff rows and CellDiff entries, or a 4 KiB read
  piles tens of MiB before the first write.
- **The alt screen vs a one-grid consumer.** Enter and leave each ship a full
  diff; a missing one leaves the alt's frame bleeding through the restored
  main or vice versa.
- **The resize ordering.** `drain_pending` (rows only) must precede `resized`
  (the full diff), or an equal-cell-count resize diffs the new cells at the old
  pitch. The producer's shadow-length mismatch guard is a silent resync — a
  belt only; the bin's call order never diffs across a geometry change.
- **The master-write lock.** The two writers (keys, terminal replies) must not
  interleave a single write; the back-pressure nap is held under the lock, so
  the CPR latency bound is the reason the nap is bounded.
- **The identity of the spawned app.** Prosecuted on the halcyond spawn side
  (`.caps(!T_CAP_SET_IDENTITY)`), but kaua-term's own `Command::new` for the
  slave inherits caps by default — the KT-1 audit's recurring footgun.

## Seams

- **fd 2 is inherited by the untrusted parser** (B-F11, OPEN): `Stdio::Null`
  is unimplemented in libthyla-rs, so the parser process shares the parent's
  stderr rather than a null sink.
- **The encode-straight-into-`out` refactor** (one fewer copy per record) is
  open; the bounds above hold without it.

## Caveats

- **`feed` accumulates the whole chunk's records; only the bin may use the
  accumulating form through a shipping sink.** A new caller that feeds bulk
  output through `feed` (not `feed_into`) re-opens the round-2 B2-F4 flood.
- **The producer's shadow is `cols x rows` cells**; a length mismatch in
  `emit_celldiff` resyncs silently. It is a guard only — the bin's call order
  (`drain_pending` without a diff, then `resized`) never diffs across a
  geometry change, equal cell counts included.

## Tests

`cargo test -p kaua-term --no-default-features --target aarch64-apple-darwin`
(30 host tests: `lib.rs` 23 + `wire.rs` 7 — the reference doc's "28" predates
rounds 2-3). They pin: the codec round-trips + the malformed/oversize/truncated
frames; the producer's boundary order; `bulk_scroll_splits_into_bounded_
scrolloffs`; the alt-screen full diffs;
`feed_into_ships_each_capped_scrolloff_so_a_chunk_never_piles_them_up` (the
sink never holds more than the bound plus one capped record; the sink-less
control accumulates everything — the one-variable pair);
`feed_into_ships_alt_screen_full_diffs_too` (256 toggle pairs in one chunk);
`a_shrink_ships_its_scrolled_off_rows_before_the_full_celldiff`;
`an_equal_count_resize_ships_no_stale_geometry_diff` (exactly [ScrollOff,
CellDiff(full)]). The bin's use of the sink and the resize call order are
guest-only. In-guest: `ls-gfx-session` (the tile spawn, the ingest, the
caps-probe, the zoom survival, the lone-tile and 1264/1280 geometry legs).

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
