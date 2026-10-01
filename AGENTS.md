# Working on Thylacine

## Astra's permanent checkout (operator-directed, 2026-09-24)

`/Users/northkillpd/projects/thylacine-astra`, branch `codex/astra`, is Astra's
permanent worktree. Main and Aux work concurrently in their own checkouts.
Use Yip from this checkout as `astra`; do not run its commands from another
agent's checkout or override their identity. `yip ring` and `yip read` deliver
messages; `yip say` replies and `yip note` records one-way updates. Check the
line at startup and between work phases; Codex does not execute Claude hooks.

Before builds, VM tests, model checking or other substantial host work, acquire
the corresponding `mac` or `pi` resource with `yip hold`. If queued, continue
source work and use `yip watch` for notifications. Never infer availability
from CPU idleness, and never steal an unexpired lease. Release as soon as the
resource work ends. Use only this checkout's build artifacts and VM processes.
Coordinate main landings through Yip before integrating or final verification.

The September 24 request is tracked in `docs/ASTRA-2026-09-24-STATUS.md`:
Lex curiata visual fidelity, F10 as primary SAK, and reconciliation of the
registry, Tapestry and remaining Lictor/ut notes. Existing single-agent and
Claude-settings overrides below remain in force; communication with the
already-running Main and Aux agents is explicitly authorized.

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

The current quaestor has no note-body update command. Resolve/read the owning
dossier through quaestor, edit its Markdown body and `updated:`/`code:` fields
directly, then render and lint through quaestor. This is the supported body-edit
workflow, clarified with Main on September 24; do not mistake the stale
"note update" wording in DOC-DISCIPLINE for an available command. Use the CLI
with this checkout's `--root`; the app's legacy quaestor MCP registration may
still point at a retired checkout. Keep committed Record bodies append-only.

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

## Current operator request (npxf and graphical SAK, 2026-09-18)

- Modernize the separate npxf project with OpenSSL, CMake, Linux/macOS support,
  and its new apology-is-policy/npxf remote. Document it as Haul's supported
  host-side example.
- Implement graphical SAK/Imperium using the approved Lex curiata visual design.
  The operator approved the isolated trusted display/input service on 2026-09-18.
  Follow GRAPHICAL-SAK-OWNERSHIP and GRAPHICAL-SAK-PORTABILITY. Research and
  design for Pi 400/Pi 500 and future display backends; do not overfit to QEMU.
- Continue single-agent and preserve the earlier review/settings overrides.

## User authority implementation (operator-approved, 2026-09-24)

The operator approved implementing `docs/USER-AUTHORITY-DESIGN.md` after review
of `44d158c3`. Work follows UA-P0/P1 and UA-0..UA-7. Keep source and evidence
status in `docs/USER-AUTHORITY-STATUS.md`; do not repeat design permission
questions already resolved by that approval. Numeric ABI reservations and model
validation precede their consumers. Yip 0117 transfers debug-taint repair to Astra, conditional on the Aux
round-3-cleared seal SHA. Coordinate all shared kernel surfaces.

## Halcyon interaction correction (operator-approved, 2026-09-24)

The operator approved kernel-backed terminal ownership and a sealed terminal
host for HI-1, preserving ordinary job control. Follow
`docs/HALCYON-INTERACTION-PTY-ABI.md` and the review beside it. This is the
explicit exception to the earlier no-new-kernel-mechanism scope. Keep existing
single-agent/draft-preservation rules. Aux's cleared H3+C base 0cb5b244 is
integrated as c252a7f3. Kernel ownership/lifecycle checkpoint 97bf1077 and expanded
regression checkpoint 5ad9ad27 are on Astra; consult the status for current runtime
evidence and remaining integration. Kaua Control subtag 6 is Aux's ScreenErased,
and 7 is Astra's binding announcement (Yip 0108 turn 22). Cleared TC-1a 1cc9a300 is integrated through Main 473cd0c0 in the September 25
reconciliation. As of October 1, Aux's TC-1b is cleared and included in Main
8746a8a24. Its ScreenErased subtag remains 6 and Astra's binding subtag remains 7.
The separate waiters-stops and stay-stopped work is still under review; do not
infer its clearance from the TC-1b landing. Consult the current interaction
status and Yip before further imports.
No live clipboard is delivered by the kernel/protocol/storage checkpoints alone.

## Temporary verification waiver (operator, 2026-10-01)

For Astra only, on October 1 and October 2, 2026 (Europe/Prague), the operator
suspends the 50-boot, ASan, UBSan and SMP gates. Do not launch those gates during
this window or describe them as passed for new changes. Keep focused functional,
lifetime, isolation and quota tests, ordinary single-CPU boot/runtime checks,
applicable model counterexamples, self-review, documentation and normal hooks.
The waiver expires at the start of October 3; it does not alter Main/Aux policy
or waive investigation of an observed failure. Record the actual verification
and the waived gates in each checkpoint.

## Session registry completion (operator-approved, 2026-10-01)

The operator approved `docs/SRV-SESSION-REGISTRY-DESIGN.md`: the login-only
factory role/syscall, fixed resident routes, scoped posting lifecycle, and
16-per-session / 48-session-combined / 64-global connection bounds, with at most
16 retained session domains. Implement through focused tests and real login/
Halcyon regressions. Keep the four protected drafts separate. This ratifies the
new D7 contract; do not ask again for these authority/ABI/resource-policy terms.
