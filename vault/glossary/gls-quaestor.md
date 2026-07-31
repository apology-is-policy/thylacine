---
id: gls-quaestor
type: gls
title: "Quaestor"
refers-to: [dec-2026-07-31-quaestor]
created: 2026-07-31
updated: 2026-07-31
---
The vault's registrar: the Go binary at `vault/meta/quaestor/`, the one
schema authority (schema §8). Subcommands: `lint` (the pre-commit gate),
`new` (typed note factory), `query`, `backlinks`, `close` (closure-field
flips), `closed` (do-not-re-report preambles), `render` (generated
views), `id`, `serve` (the MCP layer over stdio).

## Naming rationale

Extends the project's deliberate Roman register for security- and
record-bearing machinery (legate, imperium — see the standing
Roman-vocabulary feedback): the *quaestores* were Rome's magistrates of
the treasury and keepers of the public records at the aerarium — the
registrar role exactly. The root *quaerere*, "to seek/inquire", also
names the `query` subcommand. User-picked 2026-07-31 over
curator/keeper/registrar/vaultctl; deliberately NOT marsupial-ized (the
thylacine register names the OS's own mechanisms, the Roman register
names the machinery of authority and record).
