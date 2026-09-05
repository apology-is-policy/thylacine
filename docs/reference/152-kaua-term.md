# 152 — kaua-term: the per-tile terminal process and the seam record stream

**Status**: AS-BUILT at KT-1 (the seam, 2026-09-03) + the KT-1 audit rounds
1-2 (2026-09-05). Design: `docs/KAUA-TERM.md` (the ratified process topology
Y and the FEED-CELLS seam B). Owner: main. Audit: the `KT-1: the kaua-term
seam` row in `docs/AUDIT-TRIGGERS.md`; the round records are
`vault/record/audits/adt-kt1-r{1,2}.md`.

## Purpose

`kaua-term` is the crash-isolated terminal a session tile runs: one process
per tile, spawned by the per-user halcyond AS the user (`--session`,
HALCYON.md §14.2/§14.11), holding the pts, running the VT parser over the
app's output, and shipping halcyond a pre-digested RECORD stream instead of
bytes. halcyond never parses VT for a tile; the untrusted parse happens in a
process whose death is a held affordance, not a dead environment.

## Public API (`usr/kaua-term/src/lib.rs`, `wire.rs`)

```rust
pub enum Record { CellDiff { changed: Vec<(u16,u16,Cell)>, cursor: (u16,u16,bool) },
                  ScrollOff { rows: Vec<Vec<Cell>> }, Control(Control), Mode(ScreenMode) }
pub enum Control { Osc1936Raw { serial: u32, frame: Vec<u8> }, Title(String), Bell, Exit(i32), WinsizeAck }
pub struct Producer;               // vt bytes -> records, with a shadow screen
impl Producer {
    pub fn new(vt: &Vt) -> Producer;
    pub fn feed(&mut self, vt: &mut Vt, bytes: &[u8], out: &mut Vec<Record>);
    pub fn feed_into(&mut self, vt: &mut Vt, bytes: &[u8], out: &mut Vec<Record>,
                     sink: &mut dyn FnMut(&mut Vec<Record>));
    pub fn resized(&mut self, vt: &Vt, out: &mut Vec<Record>);
}
pub mod wire {                     // the framed codec, both directions
    pub const MAX_FRAME: usize;    // 4 MiB: the consumer's decoder bound
    pub const MAX_TITLE: usize = 256;
    pub fn encode_record(r: &Record, out: &mut Vec<u8>);
    pub fn parse_record(tag: u8, payload: &[u8]) -> Result<Record, WireError>;
    pub enum Input { Key(KeyEvent), Resize { cols: u16, rows: u16 }, Text(Vec<u8>) }
                                   // Text (H-4d-2): bytes typed as ONE record
    pub fn encode_input(i: &Input, out: &mut Vec<u8>);
    pub struct FrameDecoder;       // length-prefixed frames, TooLarge past MAX_FRAME
}
```

The bin (`main.rs`): `kaua-term [--beacon none|cells|rich] <cols> <rows> [prog
[args...]]` -- fd 0 is
the DOWN channel (halcyond's `Input` frames), fd 1 the UP channel (the
`Record` frames), fd 2 inherited. It opens a pts pair through `ptyhold`,
spawns `prog` (default `/bin/ut`, `--home` forwarded verbatim) on the slave,
and runs two threads: the INPUT thread (fd 0 -> keys re-encoded to the
master, `Resize` -> `pending_resize` + the pts winsize) and the OUTPUT thread
(master -> `Vt` -> `Producer` -> records -> fd 1).

**The tier advertisement (`--beacon`, H-4d-2a).** The tile's host renders the
transcript, so the kaua-term -- the pts master -- declares the tier to the
program it hosts: `--beacon <tier>` is written to this process's own
`/env/BEACON` (`write_env_beacon`) BEFORE `spawn_on_slave`, so the app's env,
a deep copy at that instant, carries it. Absent = `none`, always written
(fail-closed: a host that declared nothing renders no frames, so the app must
not emit them; a caller's inherited `BEACON=rich` never leaks through). The
pts SLAVE answers `'t'` to `SYS_FD_DEVCLASS` (the kernel's pts registry), and
the Beacon gate reads the pair (BEACON.md 12.4 as amended): `ut` arms its
zones, coreutils color + present objects, `halcyon welcome` goes out rich --
inside a tile. halcyond's session compositor passes `--beacon rich`. A bad
tier word is a usage error (exit 2, `kaua-term: --beacon takes
none|cells|rich`).

**The `Text` record (H-4d-2).** A chosen verb's command line arrives from
halcyond as one `Input::Text(bytes)` (tag 2; the payload is the bytes,
possibly empty) and is written to the master verbatim (`write_master`, under
the same lock as keys) -- the compositor's `^E ^U <cmd>\n`, one record so the
bounded down-queue drops it whole or not at all. Host test:
`input_round_trips` covers a command line and an empty `Text`.

**The cell span (H-4d-2b).** Every cell on the wire carries `vt::Cell.span`
(the wire cell is 17 bytes: ch, fg, bg, attrs, span): the serial of the last
Beacon frame the VT forwarded before the cell was written (0 = none; blanks
from erase / scroll fill carry 0). The VT advances the serial on every OSC
whose code is 1936 (a numeric selector -- no body is read, R5) and reports it
on the boundary (`Boundary::Osc { serial, body }`), which the producer ships
as `Control::Osc1936Raw { serial, frame }`. halcyond feeds the frames in
stream order and notes the span state after each under that serial, so a
cell resolves to its obj / em / hdr however late it scrolls off. Explicit on
the wire so a dropped or oversize frame can never shift later cells onto the
wrong span (a wrong span is the anti-clickjack class: a verb run on the wrong
object). Host tests: vt's
`beacon_frames_stamp_cells_with_the_span_serial`, the cell round-trip with a
span.

## The record stream and its order

`Vt::feed_until` (capture mode) returns at every boundary -- a scroll, a
bell, an OSC, an alt-screen enter/leave -- having applied the byte's cell
effect. The producer flushes a pending CellDiff before acting on each
boundary, so the ORDER between records is load-bearing and guaranteed: a
Beacon zone frame lands between exactly the cells it separates, a scrolled
row precedes the screen that no longer holds it. Consecutive scrolls coalesce
into one `ScrollOff`, bounded by `scroll_cap()` -- `min(MAX_FRAME/2,
SCROLL_ACC_BYTES = 256 KiB) / per_row` -- so one record fits the consumer's
decoder AND the producer's own heap (the record is held as cells, serialized,
then framed: three copies). Alt-screen enter/leave each push a FULL CellDiff
after resetting the shadow: the consumer keeps ONE grid, so the alt screen's
blank rows must overwrite the main's text and the restored main must
overwrite the alt's last frame (B-F3).

**Per-feed shipping (`feed_into`, round 2 B2-F4).** The per-RECORD cap does
not bound how many ScrollOffs one `feed` piles up before anything is written:
the rows a chunk yields are the VT's to decide, not the chunk size's -- `ESC [
36 S` is five bytes and thirty-six rows, so a 4 KiB read can yield ~30K rows
(tens of MiB of cells) before the first write. The bin feeds through
`feed_into` with a sink that serializes + writes + clears `out` each time a
capped ScrollOff lands; `feed` is the sink-less form for tests. A resize
(`apply_resize`, run before AND after the parked master read -- B-F6: the
bytes that read returns are usually the app's SIGWINCH repaint at the new
size) first drains the vt's pending boundaries through
`Producer::drain_pending` -- rows only, NO screen diff, because the shadow
still has the old geometry and a diff against it would address the new cells
at the old pitch whenever the cell count is unchanged (round 3 F3: 80x24 ->
96x20) -- so a shrink's scrolled-off rows precede the resized screen's full
diff in the same emit (B2-F7), then `resized` resyncs the shadow and emits the
full diff. The sink's trigger is the CELLS held in `out` (`cells_in`,
ScrollOff rows + CellDiff entries) reaching `SCROLL_ACC_BYTES`, checked after
every boundary: the alt-screen arms push a full screen per toggle, and eight
bytes of `?1049h`/`?1049l` per toggle made a 4 KiB read 512 screens (45 MiB
at 128x36, 32 MiB from 320 bytes at 4K) before round 3 F1 -- the bound is per
record CLASS, or it is not a bound.

## Bounds

The heap is a lazy `ThylaAllocN<32 MiB>` span (B-F4: 4 MiB could not hold one
capped ScrollOff and its two serializations). `MAX_TITLE` caps an OSC 0/2
title at 256 bytes at parse (B-F7). A zero-count master write is raw-mode
back-pressure: `write_all` retries it 200 x 1 ms (parked, not spun) under the
master-write lock before dropping the remainder (B-F8; the lock is held
across the nap, so a CPR reply waits at most a bounded 400 ms). A down-channel
frame the decoder refuses (`TooLarge`) ends the process cleanly (`'down`
break -> `t_exit_group`; B-F14) -- halcyond never emits one, so the arm is
robustness only.

## Concurrency

Two threads, one lock: the master-write futex mutex (`torpor`) serializes the
input thread's key writes against the output thread's terminal replies (CPR,
DSR, DA) -- a reply mid-keystroke would corrupt the app's stdin. Master reads
are lock-free. Two relaxed atomics -- `app_cursor` (the input thread's key
encoding) and `pending_resize` (posted, then applied on the output thread) --
are benign: a late observation costs one frame and self-corrects.

## Tests

`cargo test -p kaua-term --no-default-features --target aarch64-apple-darwin`
(28): the codec round-trips and the malformed/oversize/truncated frames; the
producer's boundary order; `bulk_scroll_splits_into_bounded_scrolloffs`; the
alt-screen full diffs; `feed_into_ships_each_capped_scrolloff_so_a_chunk_never_
piles_them_up` (the sink never sees more than the bound plus one capped
record; the sink-less control accumulates everything);
`feed_into_ships_alt_screen_full_diffs_too` (256 toggle pairs in one chunk:
the sink never holds more than the bound plus one screen); `a_shrink_ships_
its_scrolled_off_rows_before_the_full_celldiff`;
`an_equal_count_resize_ships_no_stale_geometry_diff` (exactly [ScrollOff,
CellDiff(full)]). These pin the LIBRARY; the bin's use of the sink and the
resize call order are guest-only. In-guest: `ls-gfx-session` (the tile spawn, the ingest,
the caps-probe, the zoom survival, the lone-tile and 1264/1280 geometry legs).

## Caveats / footguns

- `feed` accumulates the whole chunk's records; only the bin may use the
  accumulating form through a shipping sink. A new caller that feeds bulk
  output through `feed` re-opens B2-F4.
- The producer's shadow is `cols x rows` cells; a LENGTH mismatch in
  `emit_celldiff` resyncs silently -- a guard only: the bin's call order
  (`drain_pending` without a diff, then `resized`) never diffs across a
  geometry change, equal cell counts included.
- fd 2 is inherited by the untrusted parser (B-F11, open: `Stdio::Null` is
  unimplemented in libthyla-rs).
- The encode-straight-into-`out` refactor (one fewer copy per record) is
  open; the bounds above hold without it.
