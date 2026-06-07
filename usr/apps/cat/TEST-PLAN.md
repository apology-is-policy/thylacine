# TEST-PLAN: cat (A1)

Status: authored, NOT executed. Verified: `cargo build --release` clean.

`cat [FILE...]`. Concatenates files (or stdin) to fd 1. fd-1 visibility
requires wiring (pipeline/redirect/inherit) -- see DOC-GAP G06. Paths must
be ABSOLUTE (libthyla-rs File; relative -> error).

Preconditions: a readable file `/etc/hosts` (or any baked ramfs file) with
known bytes; for stdin cases, wire fd 0 to a pipe.

| # | argv (+ wiring) | fd-1 output | exit |
|---|---|---|---|
| T1 | `["cat","/FILE"]` | exact bytes of /FILE | 0 |
| T2 | `["cat","/A","/B"]` | bytes(A) then bytes(B) | 0 |
| T3 | `["cat"]`, fd 0 = pipe with `hi\n` | `hi\n` | 0 |
| T4 | `["cat","-"]`, fd 0 = pipe `x` | `x` | 0 |
| T5 | `["cat","/A","-","/B"]`, stdin piped | A, then stdin, then B | 0 |
| T6 | `["cat","/nonexistent"]` | (stderr) `cat: /nonexistent: <err>` | 1 |
| T7 | `["cat","relative/path"]` | (stderr) error (relative rejected) | 1 |
| T8 | pipeline `echo hello \| cat` | `hello\n` | both 0 |

Note: a binary file round-trips byte-exactly (no text translation).
