# TEST-PLAN: echo (A1)

Status: **authored, NOT executed.** Verification done: `cargo build
--release` clean link + disassembly of the argv-capture path.

## What it is
`echo [-n] [STRING...]` -- writes the operands to fd 1, space-separated,
with a trailing newline unless `-n` is the first operand. Bytes pass
through verbatim (no `-e` backslash interpretation). First app to exercise
the aux-rt argv workaround (DOC-GAP G03) end to end.

## Build verification already performed (no boot)
- Clean link. Disassembly confirms the entry chain is byte-correct:
  `_start: bl rs_main` (sp unchanged) -> `rs_main: ldr x0,[sp]; add x1,sp,#8;
  b aux_main` (argc into x0, &argv[0] into x1, captured BEFORE aux_main's
  prologue) -> `aux_main` reads argc/argv from x0/x1. So argv is delivered
  correctly without booting.
- One PT_LOAD `R E` (echo allocates nothing; the allocator is DCE'd).

## Output channel + visibility caveat (DOC-GAP G06)
Output goes to **fd 1** via `t_write(1, ...)`. At v1.0 a native app has no
terminal-backed fd 0/1/2 (utopia stmt.rs); fd 1 is meaningful only when
WIRED -- inside a shell pipeline, under a redirect, or inherited from a
parent that set it. So:
- In a pipeline `echo hi | cat`, `cat` sees `hi\n` on its stdin. OBSERVABLE.
- Standalone with no wiring, the `t_write(1,..)` returns an error (fd 1 not
  open); aux_rt::out swallows it (best-effort) and echo still exits 0.
  Nothing appears on the serial console (that is the UART, a different
  channel). EXPECTED at v1.0.

## Cases (for the in-VM executor)

| # | argv | Expected fd-1 bytes | Expected exit |
|---|---|---|---|
| T1 | `["echo","hello","world"]` | `hello world\n` | 0 |
| T2 | `["echo"]` (no operands) | `\n` | 0 |
| T3 | `["echo","-n","hi"]` | `hi` (no newline) | 0 |
| T4 | `["echo","-n"]` | `` (empty, no newline) | 0 |
| T5 | `["echo","a","","b"]` (empty operand) | `a  b\n` (two spaces) | 0 |
| T6 | `["echo","-nx"]` (not exactly `-n`) | `-nx\n` (treated as operand) | 0 |
| T7 | pipeline `echo one two | cat` | `cat` emits `one two\n` | both 0 |

### How to wire T1-T6 for observation
These write to fd 1. To observe non-interactively, run under a parent that
inherits fd 1 to an observable sink, OR pipe into a reader (`| cat`) as in
T7. Confirms argv delivery (the operands round-trip) + the `-n` flag.

### Recommended executor harness
Spawn via `Command::new("echo").arg("hello").arg("world").stdout(pipe_wr)`
and read the pipe_rd end; assert the bytes. This both wires fd 1 (so output
is observable) and proves the argv path through SYS_SPAWN_FULL_ARGV matches
the aux-rt callee-side reader.
