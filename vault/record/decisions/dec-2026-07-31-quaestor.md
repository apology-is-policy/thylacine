---
id: dec-2026-07-31-quaestor
type: dec
title: "Vault tooling: a Go quaestor with an MCP layer, one schema authority"
date: 2026-07-31
status: standing
decided-by: user-vote
affects: [arc-vault]
created: 2026-07-31
---
## Fork

How should the vault's tooling grow beyond the commit-0 `lint.py` — in
particular, should there be an MCP server offering tailored Obsidian
operations for the knowledge/state workflow and the PZB2 fork, and in
what shape/language/name?

## Research

Generic Obsidian MCP servers exist (REST-plugin-backed or vault-file
servers), but all would either bypass the schema (raw file edits with no
closure/immutability discipline) or drag in an Obsidian-app dependency,
violating R8 (the app is a viewer; nothing load-bearing may require it).
The vault's operations are already fully defined by schema section 8 —
the tailored tool surface IS the linter's vocabulary (lint, closed
preambles, closure flips, typed creation), so the MCP layer belongs ON
the schema authority, not beside it. The R6 mirror-drift hazard applies
to the schema itself: two implementations of the checks WILL diverge.

## Options

1. Generic Obsidian MCP + keep lint.py separate — rejected (schema
   bypass; app dependency; two authorities).
2. Extend lint.py with subcommands + a Python MCP shim — workable, but
   the user vetoed the language and the "lint" name undersold the role.
3. **A Go binary, `quaestor`, one module: lint + new + query + backlinks
   + close + closed + render + id + serve (MCP stdio), replacing lint.py
   only after a parity gate** — chosen.

## The call

User-voted (2026-07-31): Go ("I would like to use Go"), the name
**quaestor** (user-picked over curator/keeper/registrar/vaultctl),
two-layer shape with ONE schema authority, no Obsidian-app dependency.
Binding constraints ratified with it: the restricted YAML-subset parser
is ported EXACTLY (never a real YAML library — the narrow grammar is
schema law); lint.py retires only behind a parity gate — identical
verdicts over the full corpus AND identical failures under every
sabotage-probe class — and the probes then live on as the committed
`go test` suite. The MCP transport is hand-rolled stdlib JSON-RPC over
stdio (zero dependencies, matching the parser's no-drift rationale).

## Rationale

Quaestor extends the deliberate Roman security register (legate,
imperium): quaestors kept the treasury and the public records — the
registrar role exactly — and *quaerere* (to seek) gives `query` its
name. See [[gls-quaestor]]. One binary means the pre-commit gate, the
note factory, the prosecutor preambles, and the MCP tools can never
disagree about what the schema says.
