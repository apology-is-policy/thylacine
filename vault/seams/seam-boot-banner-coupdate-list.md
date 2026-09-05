---
id: seam-boot-banner-coupdate-list
type: seam
title: "The boot-banner co-update list names one file that cannot break and omits fourteen that can"
status: open
surface: [sub-substrate-gates]
opened-by: chg-2026-08-16-boot-banner-mirror-set
tracker: "yip to main 2026-08-16; sequel to main#244"
created: 2026-08-16
updated: 2026-08-16
---
## Owed

`TOOLING.md §10` and `CLAUDE.md`'s boot-banner contract both state a
**four-file lockstep** for any change to `Thylacine boot OK` or the
`EXTINCTION:` prefix. main#244 (user-voted, 2026-08-15) removed the one
member that provably never existed — `tools/agent-protocol.md` — and pointed
at `TOOLING.md §10` instead. The correction is right and incomplete on two
counts, both measured against the tree at `85c1ee9c`:

- **`tools/run-vm.sh` remains the list's first member and consumes neither
  string** (zero matches). It is a QEMU *launcher*: it builds a command line
  and hands over an interactive UART, and never reads boot output. It cannot
  break, so following the instruction there yields nothing to do — the exact
  damage main#244 names for the phantom, one member over.
- **Fourteen files match one or both literals and none is named**, in any
  version of the list: `test.sh`, `smp-multiboot.sh`, `test-cross-reboot.sh`,
  `test-fault.sh`, `ci-idle-gate.sh`, `np3-bench.sh`, `verify-kaslr.sh`,
  `warp/boot-probe.sh`, and six expect scripts (`interactive/lib.exp`,
  `dap-nora.exp`, `flood-174.exp`, `freeze-172.exp`, `ls-gfx-font.exp`,
  `warp/quarry-wedge.exp`). Two more mention the strings in comments only
  (warp-host.sh, interactive/go8d.exp) — they go stale rather than
  break. 14 + 2 = the 16 files under `tools/` carrying either string.

Two adjacent under-scopings, same registry, found in the same pass:

- **`tools/stall-watch.py`'s `KASLR_RE` and `tools/verify-kaslr.sh`'s offset
  grep both parse `kernel base:`**, which [[abi-boot-banner]] called
  informational and free-to-evolve until this change. `verify-kaslr.sh` is the
  ROADMAP §4.2 exit-criterion gate for [[inv-i16]].
- **`tools/test-fault.sh`'s `expected_marker` case matches seven extinction
  MESSAGE bodies**, not just the prefix. The comment above it says "Keep the
  case below in sync with this" — an instruction to a person, in the file,
  which is the form [[dec-2026-08-15-cutover]] ratified as insufficient.

## What closes it

Main's call, since the two documents are theirs. Three shapes, cheapest first:

1. **Repoint rather than enumerate.** Replace the file list in both documents
   with "the `mirrors` set of `abi-boot-banner`" — one authority, already
   R6-enforced at change time, and the vault note now carries the full set.
2. **Enumerate and accept the drift**, i.e. paste the fourteen. Costs a
   fifteenth stale list the moment someone writes another `.exp`.
3. **Derive it.** A lint rule that greps the tree for the two literals and
   diffs the hit set against `mirrors`. This is the only option that is
   safe-by-default rather than safe-if-remembered, and it would have failed on
   the day `dap-nora.exp` was written. It is also cheap: the literals are
   fixed strings, and the comment-only pair is the one judgement call, which
   an explicit allowlist in the note handles.

The vault half is already done — the registry's own copy of the phantom, the
false "nothing matches on it", and the incomplete `mirrors` field were all
fixed at `chg-2026-08-16-boot-banner-mirror-set`.

## Risk while open

Low blast radius, high embarrassment, and asymmetric by consumer. Nobody is
proposing to change these strings; they have been stable since the thematic
rename. But if someone does, the loud failures (`test.sh`, `smp-multiboot.sh`,
`verify-kaslr.sh`) fire immediately and the silent one does not:
`stall-watch.py` has `if m:` with no `else`, so an unparsed banner leaves
`syms.slide` at `None` and the watcher simply stops symbolizing — losing
exactly the diagnostic it exists to provide, at exactly the moment a guest has
stalled.

The standing risk is the one main#244 already articulated and that this seam
shows survived its own fix: a mandatory list with an unfollowable member
teaches the reader the list is advisory. Removing the fictional member while
leaving the inert one preserves the lesson intact.
