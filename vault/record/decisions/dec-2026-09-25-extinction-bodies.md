---
id: dec-2026-09-25-extinction-bodies
type: dec
title: "Only the EXTINCTION prefix and the bodies a tool matches are ABI"
date: 2026-09-25
status: standing
decided-by: user-vote
affects: [abi-boot-banner, sub-kernel-joey]
created: 2026-09-25
---
## Fork

B-1d moved joey from the initrd root to `bin/joey`
([[dec-2026-09-25-initrd-bin-directory]]), and five extinction bodies in
`kernel/joey.c` still name `/joey`. Two binding documents disagreed about
whether those bodies could follow the program:

- `CLAUDE.md` ("Boot banner and `EXTINCTION:` strings are tooling ABI:
  rewording one is a format break (escalate)") and `docs/agent/BOOT-BANNER.md`
  ("changing one is a **format break** ...: surface it, do not just sweep")
  made every body ABI.
- [[abi-boot-banner]], which BOOT-BANNER.md names as the authority on the
  strings' consumers, made the prefix ABI and the body prose, except the
  bodies `tools/test-fault.sh` matches.

B-1d's WIP 8 and 9 renamed the five on the note's reading. The round-3 close
reverted them, because when two binding documents disagree, choosing the
permissive reading is the operator's call ([[fnd-b1d-r3-s2]]).

## Research

- **Who matches a body.** `tools/test-fault.sh`'s `expected_for` matches six
  bodies across its eight variants; the note lists them. Every other consumer
  the note records matches the `EXTINCTION:` prefix. A census of 1046 files at
  the round-3 close found no tool matching any of the five joey bodies.
- **What already guards the set.** The note's `mirrors` set is checked at
  change time, and `quaestor owner` reports a changed file that matches an ABI
  literal of the note, so a body that gains a consumer is found when the change
  is made.
- **What the broad rule costs.** The five name a path that no longer exists, in
  the message an operator reads when the kernel stops.

## Options

1. **The note's rule stands and the five are renamed.** CLAUDE.md and
   BOOT-BANNER.md are edited to it: the prefix and each body a tool matches are
   ABI, and rewording one needs the operator's sign-off; other bodies are
   prose.
2. **A one-off rename under the broad rule.** Every later rewording still comes
   to the operator, and the note is edited to say the broad rule binds.
3. **Keep `/joey` and the broad rule.**

## The call

Option 1 (operator, 2026-09-25). The five bodies name `bin/joey`, in the change
that implements this vote. `CLAUDE.md`, `docs/agent/BOOT-BANNER.md` and
[[abi-boot-banner]] state one rule: the `EXTINCTION:` prefix is ABI, and so is
each body a tool matches. The note keeps that set, and a body that gains a
consumer joins it.

## Rationale

An escalation earns its cost when it protects a consumer. The broad rule asked
for sign-off on bodies no program reads, while the bodies programs do read are
already listed and checked where changes are made. The note's rule keeps every
protection that has a consumer behind it and lets a message name the program
where it now lives.
