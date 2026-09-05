---
id: inv-i45
type: inv
title: "I-45 — GPU work reaches only what its context owns"
number: I-45
guards: [sub-tapestryd]
validated-by: [prose]
strength: prose
created: 2026-08-14
updated: 2026-08-15
---
## Statement

Quoted from `docs/GPU-DESIGN.md` §8, where the invariant is written out
in full. `ARCHITECTURE.md` §28 now carries the registry row (see Status);
GPU-DESIGN remains the long form.

> **I-45. GPU work reaches only what its context owns.** A submission
> executes only against buffers attached to the submitting context,
> bounded by address-translation hardware the trusted server programs —
> never by inspection of the command stream. A context's buffers live
> until the last client unmap *and* the last in-flight submission naming
> them retires (the I-7/#847 dual count extended across the device-side
> reference). Context teardown — including client death — quiesces that
> context's work without disturbing other contexts' results, and
> releases its GPU address space only after quiesce. A fault
> attributable to a context is fatal to that context and to nothing
> else.

Note what the bound is *not*: the command stream is never inspected.
Parsing a hostile guest's command stream to decide what it may touch is
the design this rejects — the boundary is address translation programmed
by the trusted server, which holds regardless of what the stream says.

## Status

**Registered in both tables since main's `5da054e4`**, and the row names its
halves rather than asserting the whole invariant — the staged
ENFORCED/RESERVED shape `ARCHITECTURE.md` §28 already used for I-20 and I-40,
which is what `GPU-DESIGN.md` §8 itself points at:

| axis | posture |
|---|---|
| guest exposure (virgl/Venus) | **enforced** — one context per client, no cross-context resource naming, submit-time capability pin |
| host isolation (virgl on QEMU) | **reserved, not enforced** — virglrenderer's per-context object tables do the bounding and the host is documented trusted; `GPU-DESIGN.md` §9.2 records that its GL path runs unsandboxed in-process |
| v3d (Raspberry Pi) | **unbuilt** — where the invariant becomes ours to keep, and where the design departs from Linux (fork F3) |

**Why the split is load-bearing and not pedantry.** Before the row existed, the
only written definition was `GPU-DESIGN.md` §8 — headed "I-45 **(proposed)**"
and describing the same bound as *enforced* 360 lines earlier. Both claims were
in circulation and they are different claims, so a bare "I-45" citation was
genuinely ambiguous between the enforced half and the whole proposal. The row
makes a reader pick.

The gap that produced this note is now closed at both ends and, more to the
point, made unable to recur silently: `tools/check-invariants.py` fails the
build if `CLAUDE.md`'s row set drifts from §28 or if an `I-NN` cited in
`AUDIT-TRIGGERS.md` has no §28 row. That check exists because the drift had
already been repaired once (RW-10) under a standing instruction to keep the
tables in sync, and came back anyway — the repair fixed the instance and left
nothing that could fail. [[view-invariant-registry]] is the vault-side mirror.

## As built

[[sub-tapestryd]] carries the whole of the enforced half. The
mechanisms, each described in that dossier:

- **Slot poisoning.** A context whose fenced chain never retired does
  not free its slot, because the device context id is derived from the
  slot index — recycling it would hand a live device context to the next
  client and let a stale stream execute against a stranger's work. This
  is the "fatal to that context and nothing else" clause, and it is the
  breach one audit round found and closed.
- **The leak posture.** Under a wedge, backings are parked rather than
  freed: the device may still be reading them. They are freed only on a
  *vindication* — the device retiring the abandoned chain, which is the
  proof it has finished. That is the "buffers live until the last
  in-flight submission naming them retires" clause, with the honest
  admission that a device which never retires means the pages are never
  recovered.
- **Deferred retire.** A destroy arriving with fences in flight hides
  the context from every client immediately and completes when quiesced
  — "quiesces that context's work" without blocking the compositor,
  which is also the console.
- **The `(slot, gen)` consent pin** on `present-to`, so a display
  consent cannot outlive the surface incarnation it named.

## Relations

Composes [[inv-i1]] (per-Proc namespace), [[inv-i5]] (hardware handles
non-transferable), [[inv-i7]] (the #847 dual count, which this extends
across the device-side reference), [[inv-i12]] (W^X — GPU command
buffers are never CPU-executable; hardware GL needs no `CAP_JIT` since
shaders compile to GPU ISA, so I-42 is not in this path at all),
[[inv-i32]] (the buffer budget axis), [[inv-i34]] (the driver's own
allowance), [[inv-i37]] (the Weft share discipline), and [[inv-i40]]
(the weave lifetime rules this extends).
