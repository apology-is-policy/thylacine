# Design forks: prior-art research and the scripture-first commit

> Moved verbatim from `CLAUDE.md` on 2026-09-23 (the CLAUDE.md trim). Read this before surfacing a design fork to the user.
> Section headings keep their original levels.

### Research prior art before surfacing a design fork

Before you take a design fork to the user (the pattern below), do the homework that makes the fork legible -- and often dissolves it. A fork surfaced cold ("A or B?") makes the user do the research you should have done. In order:

1. **How does the heritage system solve it?** Thylacine is Plan 9-lineage: how do Plan 9 and its relevant daemons (e.g. factotum + secstore, devmnt's shared-mount, the per-process namespace) do this exact thing? We inherit its model, so its answer is usually load-bearing.
2. **What is the modern SOTA?** Look at the closest peers. For OS-level questions that is the capability microkernels -- Fuchsia, Genode, seL4, Hurd -- NOT Linux/macOS, whose global-VFS / ambient-authority answers frequently don't map onto Thylacine's per-Proc, capability-scoped model. Name the mechanism each uses, not just the product.
3. **How well does each fit Thylacine?** Ground the fit in VERIFIED facts about the tree -- which syscalls/mechanisms already exist (run the greps), what the section-28 invariants demand, what the lineage idiom is. Don't assume.
4. **Improvement / novel angle?** The best Thylacine answer is frequently a fusion of the Plan 9 idiom and the capability-microkernel SOTA. If the synthesis is genuinely new, it's a NOVEL.md candidate -- record it even when v1.0 defers building it.

Then surface the fork WITH the research attached: each option annotated by precedent, fit, and cost. Often the research collapses four options to one obvious choice -- make the call and report the reasoning instead of asking. Escalate only the residue the research genuinely can't resolve (a value/scope tradeoff that is the user's to weigh). Worked example: the A-1b "where does corvus's persistent storage live" fork -- Plan 9 factotum/secstore + devmnt shared-mount, the Fuchsia/Genode per-component-session SOTA, the verified facts (`SYS_MOUNT` exists; the 9P client serializes every RPC under one lock; spawn can pass a Spoor handle), and a novel "storage-as-a-spawn-capability" angle -- all gathered BEFORE re-posing the choice.

### Design conversation -> scripture commit (mid-project pattern)

When an implementation chunk surfaces a non-trivial design question -- a new mechanism, a load-bearing decision, an invariant not yet in scripture -- the workflow is:

1. **Stop the implementation.** Don't try to design-while-coding. Stop, surface to the user.
2. **Surface as a structured option set.** Not a yes/no; lay out 2-4 options with their consequences. Auto-mode bias is "make the call" -- but scripture-altering decisions are explicitly outside auto-mode and warrant the user's vote.
3. **Have the conversation in-session.** Iterate to user signoff in one round-trip where possible.
4. **Land the design as a SCRIPTURE COMMIT FIRST -- no code.** The commit updates `ARCHITECTURE.md` / `NOVEL.md` / phase-design docs / `CLAUDE.md` / `ROADMAP.md` as needed, and adds a memory-file index entry. The commit message names the design decision, the rationale, the alternatives considered, and the open questions resolved.
5. **THEN implement** in a subsequent commit that references the scripture commit's SHA in its message.
6. **THEN audit** (the standard pattern for audit-bearing implementations).

The pattern is "scripture before code, every time the code would otherwise determine the scripture." Examples that drove this pattern in Thylacine:
- P6-pouch-mem-design (`2fd9797`): the two-tier native memory interface, surfaced mid-implementation of `pouch-mem`; landed as scripture commit before the kernel-side syscalls.
- P6-pouch-compiler-rt-design (`bc97630`): the compiler-rt + `pouch-ld` requirement, surfaced by `pouch-hello-smoke`; landed as scripture before the rt build wiring.
- P6-pouch-signals-design (`237f096`): the fd-first notes substrate (novel angle), surfaced before the kernel notes implementation; landed as scripture + NOVEL.md update before the kernel-side code.

The pattern produces audit-traceable design history and makes the implementation auditable against a fixed reference. The scripture commit is short, focused, and reversible if implementation surfaces a flaw in the design.
