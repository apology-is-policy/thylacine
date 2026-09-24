# Astra: September 24 follow-up

Permanent checkout: `/Users/northkillpd/projects/thylacine-astra`, branch
`codex/astra`, initially main `5ed51ff5`. Yip identity `astra` on the shared
Thylacine line. Calls 0107 (Main) and 0108 (Aux) establish ownership and bases.
No independent review is claimed; the operator's single-agent instruction
remains in effect.

## Confirmed scope and coordination

- Restore the approved Lex curiata visual design; make Ctrl+Alt+F10 primary
  in instructions and graphical gates. The key is already accepted by the
  kernel alongside Delete; preserve that compatibility unless directed otherwise.
- Main transferred the startup Tapestry system-tier issue to Astra.
- Aux confirmed registry exhaustion and the misleading pouch bind errno are
  both open and unclaimed; Astra owns the combined repair.
- Aux owns seal semantics, devproc disclosure, proc fork seal propagation and
  the halcyond spawn seal. Astra must not interfere. Existing graphical SAK
  harnesses do not inspect the compositor's sealed disclosure files.
- Main will announce B-1c and the subsequent aux-3 d819d8f1 integration. Take
  those landings before final verification; do not import Aux's moving tip.

## Implemented and verified on the current base

Trusted UI typography/layout now follows the approved HTML: proportional
heading, two identity columns, capability area, lifetime, framed key field and
footer actions. Repository-owned Plex and Cornucopia alpha masks are baked,
so the trusted service never reads a user font or parses a TTF.
The original HTML uses platform system sans; Plex is the Halcyon type family
and supplies the portable build-owned equivalent.

## Architectural finding

The broker retains the previous normal GPU resource but has no private
completed-frame snapshot. `Seat::step` always passes `None` as backdrop.
Blurring shared live compositor backing would not satisfy the approved
immutable-capture contract. Resolve capture provenance before enabling blur;
the neutral private backdrop remains the current safe fallback.

## Verification

- Native host suite: **25/25 PASS**, including all 127 nonempty capability
  subsets across nine verdict states and maximum-length fields at 800x720.
- QEMU HVF: `ls-graphical-sak` **PASS (90 s)**; F10 used throughout.
- QEMU HVF at 800x720: `ls-graphical-sak-states` **PASS (185 s)**, including
  expiry, five-failure lockout and restored-workspace authority witnesses.
- QEMU HVF: `ls-graphical-sak-recover` **PASS (66 s)**, including held-chord
  deadline, fail-closed recovery and subsequent successful conferral.
- Fresh `tools/build.sh kernel` followed by `tools/test.sh`: **PASS**,
  **1667/1667 kernel tests**, boot banner reached. Alpine and Clade fixture
  gates were explicitly skipped and are not coverage.
- Actual request, masked, denied, lockout and recovered-workspace screenshots
  inspected. Evidence is under this checkout's `work/qemu-sak*` directories.
- Review is single-agent self-review, not an independent adversarial audit.
  Frame validation still precedes painting; no authority/seat ABI change,
  secret-byte handling change, normal compositor buffer access or font parser
  was introduced. Geometry, long text, capability coverage, mask bounds,
  request/verdict presentation and recovery were checked.

## Remaining queue

- Take Main's announced B-1c/aux-3 landings and verify integration before merge.
- Registry lifetime/headroom and bind errno reproduction and repair.
- Tapestry startup namespace sequencing reproduction and repair.
- Both original ut findings remain in code, also unchanged by Aux's stable
  d819d8f1: `eval/stmt.rs::exec_external` sets status 127 and errstr on spawn
  failure but returns `Ok(Normal)`; `repl.rs::run_line` prints only an `Err`.
  The foreground path still spawns a runnable child, then calls
  `jc_place_in_group`, then `run_foreground_jc` to hand over the terminal.
  Its own comment records birth-pgid as the structural follow-up. These are
  source-confirmed, not newly reproduced scheduling measurements.
  Aux additionally reports a duplicate pts prompt and unverified mount/unmount
  builtins; those are separate checks.
- Lictor test-mode default, small display admission, idle loop cost, compositor
  reaping and Pi hardware qualification remain open; no hardware claim made.
- Owning Vault dossiers are updated; two render passes report zero lint failures
  (the existing stale-dossier warning remains).

The mac lease has been released. Main is running his own close; no Astra VM or
build remains active. `docs/LICTOR-BACKDROP-REVIEW.md` is a concrete proposal
awaiting the operator's choice; no capture implementation is claimed.

## Fresh-checkout defect found and repaired

Commit `6e1ba9a6` restores four vendor source files hidden by generic ignore
rules, with exact exceptions. The Rust files match Cargo SHA-256 manifests;
the two libsodium MSVC scripts match the established Main/Aux copies. The
complete fresh image now builds. Aux owns the automatic vendor-ignore guard.
Main has been sent the independently cherry-pickable repair.

## Review / documentation

The source-of-truth dossiers are `sub-lictor`, `sub-imperium` and
`sub-substrate-build`. Their bodies and code ownership lists are co-updated;
quaestor's current supported workflow is owner/read, direct Markdown body
edit, render and lint (Main clarified the absent body-update API on Yip 0110).
The local Yip installer adjusts `.claude/settings.json` only to remove duplicate
legacy hooks while installing current hooks in ignored settings.local.json;
that host-specific change is deliberately excluded from implementation commits.
