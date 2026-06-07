# TEST-PLAN: hexdump (A3)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`hexdump [FILE...]` -- canonical hex+ASCII dump (`hexdump -C` / xxd style).
No operand (or "-") dumps stdin.

Precondition: `/H` contains the 12 bytes `Hello world\n`.

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["hexdump","/H"]` | `00000000  48 65 6c 6c 6f 20 77 6f  72 6c 64 0a              |Hello world.|` then `0000000c` | 0 |
| T2 | `["hexdump"]`, stdin piped `/H` bytes | same as T1 | 0 |
| T3 | `["hexdump","/missing"]` | (stderr) error | 1 |
| T4 | empty file | only the final `00000000\n` offset line | 0 |

Layout: 8-hex offset, two 8-byte hex groups (split by an extra space), a
2-space gutter, then `|ascii|` (non-printables shown as `.`). A trailing
offset line marks the total length.
