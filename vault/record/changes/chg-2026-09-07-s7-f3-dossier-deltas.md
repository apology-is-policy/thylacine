---
id: chg-2026-09-07-s7-f3-dossier-deltas
type: chg
title: "s7 F3 dossier deltas: the ptyfs slave-write park + nora's self-naming exit markers + halcyond's screenmode witness"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched:
  - sub-ptyfs
  - sub-nora-host
  - sub-halcyond
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
The s7 F3 chunk (main @70f91be3, "ptyfs: park a full-ring slave write instead of
Rwrite 0") fixed the "one frame then exit" defect: a native slave writer
(nora/prowl/quarry via kaua's `write_all`) turns a 0-count `Rwrite` into a fatal
`WriteZero`, so a first frame larger than the 4 KiB `s2m` ring killed the writer.
Three dossiers owed a fold; recorded here as one chunk.

## sub-ptyfs (audit:hard) -- the output-side park

The mechanism, folded as the analog of the existing `PendingRead`/`poll_reads`
read-park:

- **Data structures**: `Conn` now carries a second flat park queue,
  `Vec<PendingWrite>`; a `PendingWrite{fid, slot_n, tag, data}` owns its un-acked
  input bytes (a `Vec`, so it is moved not copied -- `poll_writes` indexes and
  `remove`s).
- **Concurrency**: a full-ring write with the master present parks (`self.defer`
  -> `Disp::Deferred`, no reply); `poll_writes` at the serve-loop top *after*
  `poll_reads` (`main.rs:84`) sees the room a master read freed in the same pass
  (I-9 output half); it replies the count that fit (>=1 short write the guest
  loops on), keeps parked on 0-with-master-present, unparks with `Rwrite 0` on
  master-gone (a closed tile never drains -> the writer should exit). FIFO per
  pts: `h_write` refuses to `slave_write` ahead of an already-parked write for
  the slot.
- **Invariants**: I-9 extended to `poll_writes`; I-20 byte conservation across a
  park (never acks bytes the ring did not take, re-runs the ONLCR cook on retry);
  I-10 (each parked op pins its 9P tag until the deferred reply).
- **Caveats**: the audit's lone P2 -- a parked op pins a shared kernel 9P tag,
  and every Proc shares ONE /dev/pts client, so ~64 parked ops starve the kernel
  `P9_SESSION_MAX_OUTSTANDING`=64 pool for all pts users (a pre-existing class the
  parked reads already carried). Real fix: a kernel per-Proc outstanding-tag
  quota, enqueued not closed.
- **Provenance/Tests**: landed with F3, audited 0 P0 / 0 P1 / 1 P2 / 6 P3 (Fable
  5.1); a mechanism on the existing ptyfs audit surface, so AUDIT-TRIGGERS is
  unchanged; the guest-observable behaviour is the `s7-nora-probe.exp` gate, not
  the `server::selftest` battery.

## sub-nora-host (audit:light) -- the self-naming exit markers

nora emits one `nora: EXIT path=<name> code=<n> [err=<e>]` line via `t_putstr`
on each of its six exit paths (`redraw1`/`redraw2`/`eof`/`pollnone`/`pollerr`/
`quit`), the F3 discriminating witness -- "one frame then exit" vs a clean hold
are two different `path=` values. Folded into Error paths with the #243 caveat
(`t_putstr` is a direct, non-line-serialized console write, the
[[seam-extinction-line-unserialized]] class; acceptable for a serial-only
diagnostic).

## sub-halcyond (audit:hard) -- the screenmode witness

The session tile's OSC ingest, under `#[cfg(feature = "test-mode")]`, emits
`halcyond: session tile leaf=<n> screenmode -> AltScreen`/`-> Normal` when it
matches a `Record::Mode` just before `tile.apply` -- the compositor-side proof
that a hosted app entered its alt screen and restored it, inert without the
feature and with no render effect. Folded at the existing `Record::Mode(AltScreen)`
modal-boundary description; `updated:` bumped 09-06 -> 09-07 (clears the STALE).
