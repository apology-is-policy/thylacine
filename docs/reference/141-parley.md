# 141 — parley, the LSP/DAP client substrate (`usr/lib/parley`)

**Status**: 8e-1a/1b/1c + 8e-2a/2b landed. `json` / `frame` / `jsonrpc` / `lsp`
are pure and host-tested; `transport` is behind the `backend` feature and proven
in-guest by `/parley-probe`. The DAP envelope (`dap`) is 8e-3.

Charter: `docs/GO-IDE-DESIGN.md` (NOVEL #13). UX + architecture:
`docs/NORA-IDE-UX.md` §6 (the async client — the load-bearing decision).
Consumer: `usr/nora` (`src/lsp_host.rs`), documented at
`docs/reference/113-nora.md`.

Kernel byte-unchanged throughout; no new §28 invariant (the arc consumes I-39).

---

## Purpose

A "parley" is a formal exchange between two parties under an agreed protocol —
which is exactly LSP and DAP. The crate is the dialogue layer between Nora and
the servers it drives (`gopls`, and at 8e-3 `Ambush`).

It exists because Nora's pre-8e subprocess model is a **one-shot filter**
(`gofmt_source`: write-all → close stdin → read to EOF → reap). That model
*deadlocks* a server which must stay alive across many request/response
round-trips: the client is blocked reading a stream the server will not close,
while the server waits for the next request the client cannot send. Every layer
below exists to replace that with a persistent, multiplexed, never-blocking
dialogue.

## Crate shape

| Module | Sub-chunk | Needs a platform? | What it is |
|---|---|---|---|
| `json` | 8e-1a | no | The tree's first JSON codec (`serde_json` is std-only). |
| `frame` | 8e-1b | no | The `Content-Length` streaming decoder. |
| `jsonrpc` | 8e-1b | no | The JSON-RPC 2.0 message grammar. |
| `lsp` | 8e-2a | no | The LSP **client policy** (this doc's focus). |
| `transport` | 8e-1c | yes (`backend`) | Persistent child + `poll(2)` multiplexer. |

`backend` is OFF by default, so `cargo test -p parley --target <host>` compiles
only the pure layers and runs them on the host with no device dependency. The
device build turns it on through the consumers (`parley-probe`, `nora`) via
Cargo's workspace feature unification.

```
cargo test  -p parley --target aarch64-apple-darwin   # 51 pure-layer tests
cargo build -p parley                                 # no_std, aarch64-unknown-none
```

---

## `lsp` — the client policy layer

`jsonrpc` is the protocol *grammar*; `lsp` is the *client*. It mints request
ids, remembers what each outstanding id asked for, turns a classified `Incoming`
into an `Action`, and owns the per-file diagnostic store.

**It performs no I/O.** Every method that produces traffic returns the `Value`
to send. That is what makes the whole client — handshake, dispatch, staleness,
diagnostics — exercisable on the host with no process, and it keeps the device
wiring a thin loop.

### The handshake

```rust
let mut cl = lsp::Client::new();
srv.send(&cl.initialize(&root_uri))?;      // -> Action::Ready on the response
srv.send(&cl.initialized())?;              // then open documents
```

### Dispatch

```rust
match cl.handle(jsonrpc::classify(Value::parse(&body)?)?) {
    Action::Send(reply)      => srv.send(&reply)?,   // answer a server request
    Action::Ready            => { /* handshake done */ }
    Action::Diagnostics(uri) => { /* cl.diagnostics_for(&uri) changed */ }
    Action::Definition(loc)  => { /* jump (8e-2c) */ }
    Action::Hover(text)      => { /* popup (8e-2c) */ }
    Action::Completion(items)=> { /* list  (8e-2c) */ }
    Action::Log(msg)         => { /* window/logMessage */ }
    Action::Failed(msg)      => { /* one of our requests errored */ }
    Action::Ignored          => {}
}
```

### Decisions worth knowing

**Latest-wins for cursor-following requests.** Minting a `hover` / `definition`
/ `completion` request DROPS any outstanding request of the same kind, so the
superseded reply classifies as `Ignored`. A stale answer that *looks* current —
a hover popup for a symbol the cursor already left — is worse than no answer.
Distinct kinds never supersede each other. (`Initialize` / `Shutdown` are
one-shot and exempt.)

**Position encoding is negotiated, not assumed.** LSP counts `character` in
**UTF-16 code units** by default. Treating that as a byte offset silently
corrupts every position on a line containing any non-ASCII character. The client
advertises `general.positionEncodings: ["utf-8", "utf-16"]` (LSP 3.17) and
records the server's choice; `char_to_byte` / `byte_to_char` convert under
whichever it picked, defaulting to the spec's `utf-16` when the server says
nothing. Both **clamp**: past-end yields the line length, and an offset landing
mid-character rounds **up** to a boundary — so the result is always a safe slice
index even when a server reports a position against text we have already edited.

**Server requests are always answered.** `workspace/configuration` gets one
empty config object per requested item ("use your defaults"); the
`registerCapability` / `workDoneProgress` / `*/refresh` family gets `null`;
anything else gets a protocol-correct `MethodNotFound`. Silence would hang a
server that blocks on the reply.

**A failed cursor request is an empty result, not an error.** "No symbol under
the cursor" is routine; surfacing it as an error would spam the status line. The
pending slot is cleared on failure too — otherwise that request kind wedges
forever.

**Everything unknown is `Ignored`, never an error.** An unmatched response, an
unhandled notification, a malformed message: the editor must survive whatever a
server says.

**`didChange` sends the whole document** (sync kind Full). Incremental sync
requires client and server to agree edit-for-edit; one dropped or misordered
change desynchronizes them *silently*, and the server then reports diagnostics
against text the user never typed. Full sync is self-correcting by construction.

**Result parsers accept every shape the spec permits** — definition:
`Location | Location[] | LocationLink[] | null`; hover: `MarkupContent |
MarkedString | MarkedString[]`; completion: `items[] | {items} | null`.

**`path_to_uri` percent-encodes per byte** (a path is bytes, not necessarily
UTF-8 text). `uri_to_path` accepts `file://`, bare `file:`, and the `localhost`
authority.

---

## `transport` — the persistent child

`Server` owns one child with all three stdio slots piped: framed requests to
stdin, framed responses streamed off stdout through a `frame::Decoder`, and
stderr **drained and discarded** so a chatty server can neither scribble the
editor's alt-screen nor wedge on a full stderr pipe. Piping stdin also means the
child can never steal the editor's keystrokes.

`Mux` blocks on a caller-supplied `(fd, tag)` set in one `poll(2)` and reports
which fds woke. The `PollSet` is **rebuilt on every call**, so a restarted or
closed server can never leave a stale fd registered — the set is tiny (fd 0 plus
a couple of servers), so the rebuild is free and the bug class is structurally
impossible. A negative fd is skipped, which is how a dead server drops out.

Tags are opaque to `parley`: it supplies the multiplex *mechanism*, the client
supplies the *meaning*.

---

## The nora wiring (8e-2b/2c, `usr/nora/src/lsp_host.rs`)

```
poll(2) over { fd 0, gopls.stdout, gopls.stderr }
   |                    |
   | keystroke          | framed message
   v                    v
PollSource.poll(Zero)   Server::pump + next_frame
   |                    |
   v                    v
Editor::handle_key      Client::handle -> Action
   |                    |
   +--------> dirty ----+--> redraw (one frame per wake)
```

One `poll(2)` covers input and every server, so an arriving diagnostic wakes the
loop **exactly like a keystroke**. This is load-bearing: `kaua::event` never
produces a `Tick`, so a message nothing polls for is a message that never
repaints.

`PollSource` keeps doing its own drain sweep (called with `PollTimeout::Zero`
now that the mux established readability), so the paste and split-escape
handling (#106-F2, #173) is byte-for-byte unchanged.

**Lifecycle.** `Lsp::start` returns `Option` — gopls absent, spawn refused, or a
non-Go buffer all mean "no language server", a fully supported state in which
nora behaves exactly as it did before 8e. A dead server is reaped, dropped from
the poll set, and the editor carries on. On exit nora sends the LSP goodbye,
closes stdin, then kills and waits unconditionally: a server that ignores the
protocol goodbye must not outlive the editor holding the workspace open.

**Document sync fires at typing boundaries, never per keystroke** — on save, and
on leaving Insert mode. A full-document `didChange` per keypress is exactly the
byte-storm `NORA-IDE-UX` §7 warns about, doubly so because nora renders inside
Aurora's row-granular fbcon. The change test itself is O(1): `TextBuffer::rev()`
(8e-2b) is bumped by every content mutation and by nothing else, so the sync
check costs a `u64` compare and serializes the document only when it really
changed.

**The outbound direction (8e-2c).** A key that wants an answer sets
`Editor::lsp_request`; the loop drains it with `take_lsp_request` and hands it
to `Lsp::request`, which syncs first (O(1) when nothing changed, so the server
is always answering about the text on screen), converts the cursor, and sends.
The conversion is the mirror of the diagnostic one: nora's CHARACTER column →
byte offset → the count the negotiated encoding asks for. A request is silently
dropped when there is no server, the handshake has not finished, or no document
is open — pressing `gd` in a buffer gopls has never seen is a no-op, not an
error to dismiss.

An answer can itself raise a *file* request: a cross-file definition parks the
target position and asks the binary to open the file. The loop therefore drains
`take_request` after the LSP arm as well as per-key — without that second drain
the jump parks forever.

**Environment.** `libthyla_rs::process::Command` has no envp at v1.0, so gopls
inherits nora's environment wholesale — which is what we want: the 8d port
proved gopls needs `PATH` (to resolve `go` via `LookPath`) and
`CAP_CSPRNG_READ` (crypto/rand at init), and both arrive from login → ut → nora
by inheritance. A per-`Command` env override would be a kernel-ABI item, not a
workaround to invent in the editor.

**Workspace root** is the nearest ancestor holding a `go.mod` (bounded search),
falling back to the file's own directory.

### Diagnostics in the editor

`nora::diag` is deliberately **protocol-free**: the engine knows "line L, byte
columns C..E, this severity, this message" and nothing about LSP. `lsp_host`
does the conversion, and that is where the negotiated encoding is spent —
`character` offsets become byte columns against the actual line text. A
diagnostic whose line is past the end of the buffer (the server is a version
behind our edits) is **dropped**, not clamped to the last line where it would
mark innocent code.

Rendering (`nora::view`):
- the **gutter number** is recolored — rust for an error, gold for a warning —
  which wins over the current-line tint (the cursor's position is already
  obvious; the error is the scarce signal). Deliberately a recolor rather than
  an extra marker column: gutter width is shared with the wrapped renderer, and
  changing it would reflow every visual row;
- the **status line** shows the message for the diagnostic on the cursor's line
  — that is the whole "inline" surface: put the cursor on a marked line and read
  it. An explicit status message still outranks it (the reply to something the
  user just did must not be buried);
- an **`NE MW` tally** rides the right slot so an off-screen diagnostic is still
  visible as a count.

Diagnostics clear on every buffer switch (`Editor::load_active`) — they are keyed
to the file that *was* showing, and carrying them across would paint another
file's errors on these lines.

---

## Tests

| Suite | Count | Command |
|---|---|---|
| parley pure layers | 51 | `cargo test -p parley --target <host>` |
| nora lib (engine + diag + view) | 142 | `cargo test -p nora --no-default-features --lib --target <host>` |
| in-guest transport | — | `/parley-probe` (boot-fatal) |
| in-guest LIVE LSP round-trip | — | `/bin/lsp-probe` (boot-fatal where gopls is baked) |

`lsp` tests cover: the handshake (and the encoding default when the server is
silent), diagnostics store/replace/clear, severity mapping and defaults,
supersession (a stale hover must not paint), cross-kind independence, every
result shape, failed requests, unknown/unmatched messages, server-request
answers including a string id echoed verbatim, document-sync message shape,
position conversion across all three encodings, clamping, and URI round-trips.

`nora` adds: the `Diagnostics` model (most-severe-per-line, tie-breaking,
counts, wrap-around navigation), the render behavior (gutter recolor confined to
the marked line, warning vs error color, status-line message, explicit status
outranking, no-diagnostic baseline), and `rev_bumps_on_every_content_mutation_only`
— which enumerates every `TextBuffer` mutator, because a mutator added later
without a bump silently costs a document sync.

### The live round-trip (`/bin/lsp-probe`, 8e-2)

Every test above is *synthetic*: the client is fed messages the test itself
wrote. That validates the client against its own assumptions about the server,
which is the shape that passes everything and fails on first contact. The
existing gopls coverage did not close it either — the joey `go8d` probe drives
gopls as a **command** (`gopls check`, `gopls definition`), never over the LSP
stdio protocol, and `parley-probe`'s only server is `/parley-echo`, which
returns bytes and speaks no protocol.

`lsp-probe` closes that: it writes a module with a deliberate undefined
identifier at a known line, spawns the real `/goroot/bin/gopls` over piped
stdio **exactly as `lsp_host` does** (bare `Command::new`, no args, inherited
env and caps — so a wrong invocation in nora fails here), and drives
`initialize` → `initialized` → `didOpen` → `publishDiagnostics`. It asserts the
diagnostic carries the planted identifier, at `Severity::Error`, **on the
planted line** — the line is what proves the range decoded rather than that
something error-shaped arrived. An empty first publish is waited through, not
failed: gopls commonly publishes `[]` before the type-check finishes.

The PASS line reports `(<ms>, publishes=N, auto-replies=N)`. Those counters are
load-bearing, not decoration:

- `auto-replies` reads **0**, and that is correct rather than a gap:
  `Client::initialize` declares no `workspace.configuration` and no dynamic
  registration, so gopls has nothing to ask. The `Action::Send` arm is wired and
  defensive but **unexercised** — a capability that invites server requests
  would light it up. The counter makes that a measured fact in the boot log
  instead of an assumption.
- `publishes` distinguishes a real round-trip from a lucky one, and would show a
  regression that silently stops gopls talking to us as a number rather than as
  nothing at all.

Revert-probed both ways: with the expected line deliberately wrong the probe
reports `FAIL -- diagnostic line 3, expected 4` and joey's gate fails the boot
(`TEST EXIT=1` + `EXTINCTION`). That second half matters — the first cut of the
wiring printed the failure and reaped `status=1` but had **no gate**, so
`tools/test.sh` still exited 0. A probe that cannot fail the build is a vacuous
green; the revert-probe is what caught it.

Measured cost: ~100 ms, inside the same joey block as `go8d` (which needs the
same `PATH` + `CAP_CSPRNG_READ`/`CAP_LOCK_PAGES` env), so it adds no meaningful
boot time.

---

## Known caveats / seams

- **`textDocument/references` is not implemented.** The scripture keymap
  reserves `gr` for it; the request has the same shape as `definition` but
  returns N locations, which wants a picker to land in.
- **A cross-file definition's COLUMN is approximate.** The jump target lives in
  a file nora has not read, so there is no line text to convert the server's
  offset against; the offset is used as a character column. Exact whenever the
  target line is ASCII (every ordinary Go declaration) and at worst a few
  columns off on a line with multi-byte characters before the symbol. The LINE
  is always right and `set_cursor` clamps, so the miss is cosmetic. Converting
  exactly would mean parking the raw LSP position through the file load and
  teaching the editor the position encoding -- protocol knowledge the
  editor/host split deliberately keeps out of the engine.
- **One server, one language.** `lsp_host` speaks Go only (`.go` → gopls). The
  client itself is language-agnostic; a registry of `(pattern, server)` is the
  generalization point.
- **Sync is Full, at typing boundaries.** Incremental sync and live
  as-you-type diagnostics are a deliberate non-goal here (see the rationale
  above); if they land they need the same revision counter as their trigger.
- **No cancellation.** A superseded request is dropped client-side but
  `$/cancelRequest` is not sent, so the server still computes an answer nobody
  wants. Harmless, and cheap to add when it shows up in a profile.
- **No restart-on-crash.** A gopls that dies takes diagnostics with it until the
  editor is restarted. Automatic restart wants a backoff so a crash-looping
  server cannot become a spawn storm.
- **Workspace root is per-launch.** Opening a file from another module with
  `:e` keeps the original root; gopls copes, with reduced results outside it.
