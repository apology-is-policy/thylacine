# Working on Thylacine

Thylacine is a real ARM64, Plan 9-derived operating system. Preserve its
namespace, capability, lifetime, and concurrency invariants. Finish the user's
requested work, including verification and documentation, before reporting it
complete.

## Sources of truth

Read `CLAUDE.md` for the full project discipline. It is the shared engineering
reference despite its agent-specific filename. Start with `docs/VISION.md`,
`docs/ARCHITECTURE.md`, the relevant design and phase status, and the owning
subsystem dossiers in `vault/`. Do not copy their facts into this file.

Instruction precedence: system/developer instructions and the user's explicit
directions take precedence over repository guidance. Claude-only effort,
settings, context-budget and hook mechanics do not apply to another agent.
Do not impersonate Claude in commit attribution or run Claude settings tools
to infer another agent's operating state.

## Current operator decisions (Haul integration, 2026-09-17)

- Bring the Imperium changes required by Haul into `main`; this does not
  authorize importing unrelated aux work.
- Keep this work single-agent for now. Perform and record self-review, but
  never call it an independent adversarial audit. Do not launch reviewer
  subagents to satisfy the general audit rule without a later user direction.
- Claude-specific setting gates are waived.

These decisions persist for this task. They are not permanent policy changes
for future tasks with different instructions.

## Current operator decisions (aux and Halcyon integration, 2026-09-17)

- Integrate the remaining committed aux work, including media viewers, audio,
  DOSBox and the manual reader, with current main and Halcyon. Preserve aux
  uncommitted edits. Complete the manual reader and write operator sections.
- Design graphical SAK authorization and document the trusted display boundary.
- Continue single-agent; Claude-specific settings gates remain waived.
- Share real screenshots as the new interfaces work in Halcyon.

## Working safely in this repository

- Inspect `git status`, branch and worktrees before changing anything. Preserve
  the user's uncommitted and untracked work. Use an isolated worktree when
  integration would otherwise collide with it.
- Read before editing. Reproduce failures, keep evidence, and trace causes
  across layers. Do not dismiss an unexplained failure as a timing flake.
  Use `docs/DEBUGGING-PLAYBOOK.md` for elusive or cross-layer failures.
- Record discovered defects and their disposition in the task's status note.
  Do not turn a passing narrow test into a claim of whole-system correctness.
- Explain new invariant-bearing mechanisms in design prose and code comments.
  The general new-spec requirement is suspended as documented in `CLAUDE.md`;
  existing relevant clean/mutant models and explicitly re-enabled specs still
  matter. In particular, Imperium has its own model.
- Review locks, publication order, peer-thread access, borrowed lifetimes,
  error cleanup, capabilities, namespace effects and fail-closed paths.
  User authorization for single-agent work changes review staffing, not rigor.

## Build and verification

Run commands from the checkout being tested. Do not mutate its built image
while a VM test is using it. Preserve failed-run logs before rerunning.

```sh
tools/build.sh kernel --config ci       # interactive-test image
tools/test.sh                          # boot/unit/probe gate
LS_CI_ATTEMPTS=1 tools/test-interactive.sh <scenario>
tools/ci-smp-gate.sh                   # default/UBSan x smp4/smp8, ten boots each
```

`tools/test-interactive.sh` may run scenarios in parallel; set `LS_CI_JOBS=1`
when diagnosis requires a serial run. Haul's `haul-npxf` and `haul-post`
scenarios require a real npxf fixture; SKIP is not PASS. `haul-hangup` supplies
its own fixture. Read each harness's options and assertions before relying on
its result. Native Rust boot probes run in the guest; do not assume every
crate can execute as a host test.

Use the repository's build configuration and vendored dependencies. Avoid
unrelated formatting, lockfile churn or broad upgrades. Run tests appropriate
to the changed mechanisms and distinguish measured results from unrun checks.

## Documentation and delivery

The technical reference is `vault/`; `docs/reference/` is frozen legacy.
Find the owning dossier, then update it alongside code:

```sh
go -C vault/meta/quaestor run . owner <paths> --root "$(pwd)"
go -C vault/meta/quaestor run . render --root "$(pwd)"
go -C vault/meta/quaestor run . lint --root "$(pwd)"
```

Create a dossier for new subsystems, follow `vault/meta/schema.md`, and update
relevant user-facing design/help and phase status. Generated vault views must
be rendered, not hand-edited. An exception trailer is not a substitute for
updating an invariant that actually changed.

Use concise ASCII commit messages describing the final change and validation.
Never fabricate authorship or audit approval. Before integrating into `main`,
check again for concurrent changes and preserve the user's work. Report what
landed, what was verified, and any remaining limitations plainly.

### Additional operator direction during aux integration

- Flag architectural workarounds and mismatches explicitly, with alternatives.
- Design full shared PCI IRQ and MSI-X support now; see
  `docs/PCI-INTERRUPTS-DESIGN.md` and the Vault decision
  `dec-2026-09-17-pci-interrupt-domains`. The design's binding ownership/ABI
  contract and full implementation are APPROVED by the operator (2026-09-17).
  Implement both shared INTx and MSI-X backends, migrate drivers and verify.
- Update the Vault alongside the work and regenerate/lint derived views.

- The operator approved the Lex curiata visual specification on 2026-09-17.
  Follow `docs/HALCYON-TRUSTED-EPISODE.md`; distinguish that visual approval
  from implementation of a trusted graphics sink.

- The operator approved the bounded TCP close design and implementation on
  2026-09-17. Follow `docs/NET-CLOSE-DESIGN.md`: private transport retirement,
  admission bounds, deadline diagnostics and byte-verified backend tests.
