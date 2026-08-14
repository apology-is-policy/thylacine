---
id: inv-i45
type: inv
title: "I-45 — GPU work reaches only what its context owns"
number: I-45
guards: [sub-tapestryd]
validated-by: [prose]
strength: prose
created: 2026-08-14
updated: 2026-08-14
---
## Statement

Quoted from `docs/GPU-DESIGN.md` §8, which is the only place the
invariant is written out:

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

## Status — and read this before citing it

**`ARCHITECTURE.md` §28 has no I-45 row.** The table runs I-40, I-42,
I-43, I-44 and stops. Its only definition is the GPU-DESIGN §8 block
above, which is headed **"I-45 (proposed)"**.

That matters because §28 is what `CLAUDE.md` names authoritative, and
what the prosecutor prompt template tells an auditor to enumerate — so
a Warp round is pointed at a list omitting the invariant it was spawned
to prosecute, while `server.rs` prosecutes "the I-45 breach F6" by name
and an audit round closed it. The invariant is enforced in code, cited
by eight documents, and unregistered.

Enforcement is **staged per backend**, deliberately, following the
I-20/I-40 precedent of enumerating RESERVED → ENFORCED per half:

| axis | posture |
|---|---|
| guest exposure (virgl/Venus) | **enforced** — one context per client, no cross-context resource naming, submit-time capability pin |
| host isolation (virgl on QEMU) | **reserved, not enforced** — virglrenderer's per-context object tables do the bounding and the host is trusted; `GPU-DESIGN.md` §9.2 states plainly that its GL path runs unsandboxed in-process |
| v3d (Raspberry Pi) | **open** — where the invariant becomes ours to keep, and where the design departs from Linux (fork F3) |

The staged shape is exactly what a §28 row exists to record. §28 already
carries that wording for I-20 and I-40, so the mechanism is present and
simply unapplied here. Tracked as **#173**; `ARCHITECTURE.md` is main's
document, so the vault records the gap rather than closing it.

Until then, **treat a bare "I-45" citation as ambiguous**: it may mean
the enforced guest half or the whole proposed invariant, and those are
different claims. `GPU-DESIGN.md` is careful about this ("stated plainly
rather than claimed"); the citing documents mostly are not.

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
