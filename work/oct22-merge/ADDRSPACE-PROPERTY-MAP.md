# AddrSpace host fixture: the old -> new property map

Written for Astra's condition on yip 0243 ("an explicit old->new property map
FIRST"). The map was recorded in corona's design memory before any mutant in
tools/test-addrspace-lifetime.py moved (2026-10-22, before d7d77ebed); this
file copies it into the tree so it can be reviewed without that memory.

## The two shapes

- Corona before the merge: `addrspace_unref` = owners--; `if (pre == 1)
  vma_drain_in(as)`; `addrspace_lifetime_put(as)`.
- Merged (main's split, corona's two levels): `addrspace_unref(as)` =
  `if (addrspace_release(as)) addrspace_destroy(as)`.
  - release: owners--; the LAST owner returns true and KEEPS its lifetime
    reference; a non-last owner drops its lifetime reference.
  - destroy: extinction if owners != 0; `addrspace_quiesce_mapped_devices`
    (proc.c, RW-7 R3-F1); `vma_drain_in`; `addrspace_lifetime_put`.
  - exec (proc.c) runs its handle-table device reset between the two.

## Each property, its old counterexample, its new one

| Property (assertion text) | Before the merge | Merged | Status |
|---|---|---|---|
| private guard refuses sharing | private-sharing | same anchor | unchanged |
| shared image refuses private guard | already-shared | same anchor | unchanged |
| guard pins lifetime without adding owner | guard-no-pin | same anchor | unchanged |
| final guard releases pin and exclusion | guard-leak | same anchor | unchanged |
| pin is not an owner | pin-as-owner | same anchor | unchanged |
| owner increments both counts | no-owner-get | same anchor | unchanged |
| final pin frees once without another drain | pin-leak | same anchor | unchanged |
| extinction: AddrSpace final lifetime drop with live owners | early-destroy (`pre > 1`) | same anchor | unchanged |
| nonfinal owner preserves mappings | drain-every-owner: drain on every unref | a NON-LAST release that drains (`vma_drain_in` after release's `if (pre == 1) return true;`) | moved, same assertion |
| last owner drains before last pin | skip-drain: unref without its `if (pre == 1)` drain | destroy without its drain | moved, same assertion |
| extinction: AddrSpace final lifetime drop before mapping drain | drop-before-drain: put before drain in unref | put before drain in destroy | moved, same assertion |

## Contract changes the split makes (named, each with a counterexample)

- C1. The last owner's release keeps the lifetime reference for destroy: the
  space is ownerless but held between the two. Counterexample
  `release-drops-lifetime` (the last release also puts its lifetime), killed
  in the full run by "last owner drains before last pin", which sees the drop
  only through `addrspace_unref`.
  CORRECTION (Astra's review, yip 0243 turn 4): this line first said "a new
  serial case drives release/destroy directly". No such case existed at
  9d00b5ee3 -- the line described a planned case, not the fixture. The case
  now exists (`split_interval`, run last in the full run and alone with the
  argument `split`). It drives release and destroy apart, with a pin held and
  a virtio device mapped. It asserts:
  - after release: "the last owner's release reports last";
  - "between release and destroy the last owner's lifetime is held" (owners
    0, ref 2);
  - "release tears nothing down before destroy" (mappings, drains, resets,
    frees and destroys untouched);
  - after the pin drops: "the retained owner reference outlives a pin dropped
    between release and destroy";
  - after destroy: "destroy stops the mapped device before the drain" and
    "destroy frees exactly once".

  Its controls:
  - `release-drops-lifetime@split` (C1's counterexample, on that case alone),
    killed by "between release and destroy the last owner's lifetime is
    held";
  - `teardown-in-release` (the device quiesce moved from destroy into the
    last release, which every earlier case cannot tell apart), killed by
    "release tears nothing down before destroy";
  - `early-destroy@split` (a lifetime drop that frees under a live
    reference), killed by the kernel's own "extinction: AddrSpace final
    lifetime drop before mapping drain".

  `clean@split` is the focused positive control.
- C2. destroy refuses a space that is still owned. Counterexample
  `release-early-last` (release reports "last" one owner early, `pre <= 2`),
  killed by "extinction: addrspace_destroy of an AddrSpace still owned".
- C3. The last owner stops every device mapped in the space (MMIO, PCI full,
  PCI DMA-only) once, before the drain; a non-last owner stops none. The real
  `addrspace_quiesce_mapped_devices` is extracted from proc.c (no quiesce
  stub); only the device resets themselves are host doubles (hardware).
  Counterexamples `no-quiesce` and `quiesce-after-drain`, both killed by
  "last owner stops each mapped device once, before the drain".

The fixture's VMA list is real nodes the quiesce walk follows (the old
`(void *)1` sentinel cannot survive a walk), and the drain double has the
kernel's external linkage so `skip-drain` compiles and meets its assertion.

## Evidence

host-20261022T131737Z at b07adeb81: `test-addrspace-lifetime.log` (clean +
15/15, unsanitized) and `test-addrspace-lifetime-san.log` (clean + 15/15,
Homebrew LLVM ASan/UBSan); each driver run requires rc 1 and the wanted text
for every mutant. That record predates C1's case and is kept unchanged.

followup-20261022T141732Z at 5b09462a5 (Astra's review): `addrspace-lifetime.log`
(unsanitized) and `addrspace-lifetime-san.log` (Homebrew LLVM ASan/UBSan with
`-fno-sanitize-recover=all`) each pass `clean`, `clean@split` and 18/18
mutants, every one killed by its named assertion. The verdict refuses any
sanitizer report, so a mutant killed by a report instead of its assertion
would count as a survivor. Each case's build log in `addrspace-lifetime*/`
opens with its argv, environment and the sha256 of the source it compiled;
`checker.log` is the verdict's own five controls on real reports (three
refused with the report seen, two accepted).
