---
id: dec-2026-08-15-cutover
type: dec
title: "The per-surface cutover: the vault becomes the destination for as-built prose"
date: 2026-08-15
status: standing
decided-by: user-vote
affects: [arc-vault]
created: 2026-08-15
---
## Fork

The vault is absorbing `docs/reference/NN-*.md`: a swept surface's reference
doc becomes a stub pointing at its dossier. Until that finishes, both documents
exist for every surface and **only one of them is being maintained** — so main
keeps writing as-built prose into reference sections the vault has already
replaced, and the vault keeps sweeping prose main has already moved on from.

Measured while deciding this, not hypothesised: main landed the `#243`
sigtab-UAF reasoning into `docs/reference/147-execve.md`, a surface owned by
five dossiers, all stale. That was the sixth instance in a week, and a seventh
landed on the same file from the same session before the conversation finished.

The original plan put the switch at the *end* of the sweep backlog. The
question was whether to bring it forward, and if so, where main's edits go
while the vault is still on an unmerged branch.

## Research

The prior art here is our own, and it is the argument: **CLAUDE.md already
carried a rule of exactly this shape and it failed.** The condensed invariant
table sits under an explicit instruction to keep its row set in sync with
`ARCHITECTURE.md` §28, *and* a note recording that RW-10 had already repaired
that exact drift once. Instruction, plus precedent, plus a completed repair —
and the table had drifted four rows anyway.

That is the whole finding: **RW-10 fixed the instance and left nothing that
could fail.** Any protocol whose first step is "remember to tell the other
side" is the same construction and will decay the same way.

## Options

1. **Cross-worktree.** Main edits dossiers in `../thylacine-vault` directly.
   No merge needed, works immediately.
2. **Merge first.** `vault/bootstrap` lands on main, then one tree. Cleanest
   steady state.
3. **Handoff.** Main writes as today and tells the vault which surface moved;
   the vault does the dossier edit.

## The call

**(3) now, (2) as the endgame — and (3) only in the form that cannot be
forgotten.**

Main declined (1) on their own account, and the reason is worth preserving:
they are mid-arc across two long-running detached jobs and a peer tree, and
holding a second branch is the state in which they make the wrong-tree class of
mistake. Accepting (1) would have been optimism about their own discipline.

The load-bearing part is not the choice but the **binding**. The check is not a
new rule; it is one step inserted into `Maintenance discipline`, which is
already non-negotiable and already triggers on precisely the right condition —
"I am about to write a reference doc for a surface":

```bash
cd ~/projects/thylacine-vault && vault/meta/quaestor/quaestor owner <changed paths>
```

Exit 0, the vault carries that surface — the prose belongs there. Exit 1, no
dossier — write the reference section as today and file the sweep. Several
paths usually answer MIXED, where the exit status reports only half and the
summary names both sets.

(2) waits deliberately, and for a reason that helps it: neither side can
currently tell the user what the cutover **costs**. (3) generates that number
from real use, and "here is what the interim arrangement cost, measured" is a
far easier thing to sign off than "approve a large visible change to main's
tree".

## Rationale

**A rule that says "keep these in sync" is safe if remembered; only a check
that fails is safe by default.** The evidence is in the file the rule lives in.

Three consequences of that principle, all landed:

- The trigger **rides an already-mandatory step** rather than standing beside
  it. Nothing new has to be remembered, which is the transferable half of this
  decision and the reason it is recorded here rather than only in CLAUDE.md.
- `tools/check-invariants.py` (main's) fails the build if the invariant
  registries drift again — the failure mode that produced this principle can no
  longer recur silently. [[view-invariant-registry]] is the vault-side mirror.
- The check reports honestly when it **cannot** answer. It runs from the vault
  worktree, which drifts behind main, so a path this checkout has never seen is
  named as *unknown* rather than silently returned as "no dossier".

**Authorship, deliberately split.** The CLAUDE.md wording is main's, because it
rides main's step and in main's tree's scripture; the tool and its semantics are
the vault's. Each side owns the half it must actually keep working.

Ratified by the user 2026-08-15. Landed: main's step 0 at `3cd6ff52`; the
vault's side across `aafd140c`, `05904fd5`, `b470b434`, `405ba336`.
