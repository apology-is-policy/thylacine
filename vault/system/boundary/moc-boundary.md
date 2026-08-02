---
id: moc-boundary
type: moc
title: "Boundary"
parent: home
created: 2026-08-01
updated: 2026-08-01
---
The surfaces that are deliberately neither kernel nor userspace: the ABIs
both halves mirror, the wire protocols they speak, the registries that
pin their numbers, and the translation layers that make a foreign
contract meet a Thylacine one. A boundary note's distinguishing property
is that **two independent implementations must agree** — so its dossiers
carry the agreement obligations (the mirrors, the static asserts, the
drift gates), not just one side's mechanism.

## Children

- [[moc-pouch-seam]] — pouch, the POSIX libc's boundary line: where
  Linux/POSIX shapes are translated into Thylacine syscalls (the first
  populated boundary area).

## Registries

The enumerated-value tables. Each is pinned by `_Static_assert` on the
kernel side and mirrored by hand everywhere else.

- [[abi-errno]] — `T_E_*`, 20 values, each equal to its POSIX number so a
  kernel return crosses into userspace untranslated. Holds the `-1`
  sentinel collision that makes `T_E_PERM` unreturnable.
- [[abi-caps]] — the 12 capability bits and the fork-grantable /
  elevation-only partition. Its coverage guard is a tautology.
- [[abi-handle-rights]] — 6 rights, 12 kobj kinds, 4 disjoint partitions.
  **The model for how a partition should be pinned**; its seven asserts
  catch both a kind in two masks and a kind in none.
- [[abi-note-names]] — the 9 deliverable note names in three classes, the
  reserved `snare:`/`tty:` prefixes, and the 32-byte `note_record`.

## Struct layouts

The records both halves declare independently. Each is mirrored by hand and
pinned by a per-copy assertion — which is exactly the weakness below.

- [[abi-t-stat]] — the 88-byte file-metadata record and its **seven**
  mirrors. A kernel growth fires exactly one of eight assertions: the
  kernel's own. Two mirrors carry no guard at all.
- [[abi-loom-ring]] — the five shared-memory ring structures, plus the three
  9P decode structs that Loom promoted into ABI by copying them out. The
  header both sides write is the least pinned of the five.

## Wire

- [[abi-ninep-wire]] — the 9P message-type space, shared with Stratum. Held
  by a cross-project allocation document rather than an assertion, because no
  compiler sees both trees. **The only registry here that verifies clean.**

## Contracts

- [[abi-boot-banner]] — the two strings that are ABI with the tooling.

## Cross-cutting

- The **mirror discipline** is this plane's recurring hazard: a struct or
  number duplicated in the kernel, `libt`, `libthyla-rs`, a pouch patch,
  and a language fork drifts silently, because a per-mirror
  `_Static_assert(sizeof == N)` verifies only THAT mirror against itself,
  never against the kernel. The `struct t_stat` growth to 88 bytes (#100)
  is the worked failure; see [[sub-pouch-fs]] and [[abi-t-stat]], whose
  guard set is unchanged since.
- **Each copy is pinned to itself; nothing pins the copies to each other.**
  Which is the same shape as the two bullets around it — a guard whose
  subject is narrower than its apparent claim. `sizeof(X) == 88` reads as
  "X matches the ABI" and means "X matches this literal."
  [[abi-ninep-wire]] is the counter-case: a shared number space that *could
  not* be held by an assertion, so it is held by a document with rules, an
  allocation authority, a burn policy, and backlinks from both projects —
  and it is the one that verifies clean.
- **Values are pinned; their descriptions are not.** The registries above
  are held by assertions that fire on a wrong number — and are surrounded
  by prose that has drifted from them repeatedly: `caps.h` says "all five"
  and lists six, `handle.h` says "nine kobj kinds" where the asserted count
  is twelve, and that particular staleness has already propagated into
  `docs/reference/19-handles.md`. Read the macro, never the sentence
  beside it.
- The syscall-abi and exec-contract areas land with their own sweeps. The
  enumerated-value, struct, and wire registries are done.
