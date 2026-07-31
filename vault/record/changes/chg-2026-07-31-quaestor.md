---
id: chg-2026-07-31-quaestor
type: chg
title: "Quaestor: the Go vault registrar + MCP layer; lint.py retired"
date: 2026-07-31
arc: arc-vault
commits: ["(pending)"]
touched:
  - home
  - dashboard
  - view-invariants
  - view-seams
  - view-audit-triggers
  - view-roadmap
  - view-closed-sub-kernel-ninep-client
  - view-closed-sub-kernel-ninep-attach
  - view-closed-sub-kernel-ninep-transport
  - view-closed-sub-kernel-ninep-dev9p-poll
established:
  - dec-2026-07-31-quaestor
  - gls-quaestor
closed: []
opened: []
mirrors-checked: []
depth: rich
---
## What

The vault tooling rewritten as **quaestor** — a stdlib-only Go module at
`vault/meta/quaestor/` (per [[dec-2026-07-31-quaestor]]) — and `lint.py`
retired. The lint core is a law-exact port: the restricted YAML-subset
parser (grammar pinned in `front_test.go`, including the
quoted-item-keeps-its-comment and duplicate-key-last-wins behaviors
verified against the reference implementation), every validate/staged
check with verbatim message strings, and byte-identical view renderers.
New subcommands on the same authority: `new` (typed factory over
`vault/meta/templates/`), `query`, `backlinks`, `close` (closure-flip
transaction that refuses non-closure fields, checks enums, and requires
edge targets to exist), `id`, and `serve` — a hand-rolled
newline-delimited JSON-RPC MCP layer exposing nine vault tools, wired
project-scoped via `.mcp.json`. The shared pre-commit hook now runs
`quaestor lint --staged` (lint.py fallback kept for historical
checkouts; fails loudly if Go is absent). Schema §8 rewritten (scripture,
user-ratified): names quaestor, freezes the parser-law and
single-authority constraints, and adds `vault/journal/` — a
linter-ignored, git-ignored operator scratch for Obsidian dailies and
canvases (the first user daily note had landed at vault root and broken
`lint --all`; it moved to `vault/journal/2026-07-31.md`). Present-plane
prose (home, dashboard, the view preambles, the view template) repointed
from `lint.py` invocations to quaestor; Record-plane bodies keep their
historical lint.py mentions (R3 — they were true).

## Why

User-voted (the [[dec-2026-07-31-quaestor]] fork): Go, the quaestor name,
one schema authority with the MCP layer ON it rather than a generic
Obsidian server beside it (R6 mirror-drift applied to the schema itself;
R8 keeps the app a viewer). `new` retires the per-session gen_*.py
heredoc generators; `closed`/`query` become the prosecutor-prompt and
sweep-workflow primitives; `backlinks` is the substrate for the deferred
Provenance renderer.

## Alternatives rejected

A generic Obsidian MCP + lint.py kept (schema bypass; app dependency;
two authorities). yaml.v3 or any real YAML parser (would silently widen
the accepted grammar — the narrow subset is schema law). An MCP SDK
dependency (mark3labs/mcp-go) for `serve` (the protocol subset needed is
~200 lines of stdlib; zero deps keeps the PZB2 scaffold portable).
Retiring lint.py without a gate (the whole point of the parity bar).

## Verification

The parity gate: **15/15** — baseline `--all` over the 162-note corpus
(identical sorted verdict lines + exit codes; the single documented
normalization is the stale-view remediation hint's tool name), all four
committed closed-preambles byte-identical, and nine sabotage classes
failing identically under BOTH implementations (dangling edge [whose
first probe QUIETLY PASSED — the fnd's real `fixed-by:` later in the
block overwrote the injected duplicate key, so the harness gained
per-probe fail-expectations and the law gained a test], stale view,
dropped dossier section, unterminated flow list, deferred-without-seam,
dangling wikilink, Record body edit, non-closure field,
closure-without-chg) plus the allowed-case probe (active-arc mutation
passes in both). The probes now live as the committed `go test` suite
(23 tests: parser law + every probe class + factory/close/backlinks/MCP
tool layer), itself revert-probed (two deliberate linter breaks →
exactly the matching tests failed, restored green). MCP smoked live over
the real corpus (initialize → tools/list → seam query → attach
preamble). `go vet` + `gofmt` clean; `quaestor lint` green at
162/0/0; this commit is the hook's first live `quaestor lint --staged`
gate.
