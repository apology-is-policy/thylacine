---
id: fnd-b1d-r3-s2
type: fnd
title: "Five EXTINCTION bodies were reworded on the permissive reading of two binding documents that disagree"
round: adt-b1d-r3
severity: P2
status: fixed
surface: [sub-kernel-joey, sub-substrate-remote-host]
threatens: []
fixed-by: chg-2026-09-25-b1d-round3-close
regression: "none: a rule about wording, not a behaviour; the operator's question is in OPEN-BUGS"
created: 2026-09-25
---
## Prosecution

The bin/ move changed the program's path from `/joey` to `bin/joey`, and WIP 8
and 9 reworded five EXTINCTION bodies in `kernel/joey.c` to match: the three
lookup failures, the wait failure and the non-zero exit. The reading behind
it: [[abi-boot-banner]] makes only the `EXTINCTION:` prefix ABI, plus the
bodies `tools/test-fault.sh` matches, and no tool matched these five. But
CLAUDE.md ("rewording one is a format break (escalate)") and
`docs/agent/BOOT-BANNER.md` ("changing one is a format break ... surface it,
do not just sweep") state the stricter rule, and BOOT-BANNER.md names the note
as the authority on the strings' consumers, not on whether a change needs the
operator. When two binding documents disagree, choosing the permissive one is
the operator's decision. The main session's self-audit found it; the round did
not report it.

## Disposition

Fixed by revert: the five bodies say `/joey` again, with a comment at the
lookup giving the reason. The rename, and which document's rule stands, are
queued for the operator. The boot log's plain lines (joey's `rforking child
for` and `pid=`) keep `/bin/joey`; they are not EXTINCTION strings or the
banner.
