# TEST-PLAN: bind (A4)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`bind [-abr] SOURCE MOUNTPOINT` -- Plan 9 namespace bind into the calling
Proc's territory. AUDIT-BEARING kernel surface (territory mount; ARCH I-1/
I-3) -- the executor should prosecute carefully.

IMPORTANT (semantics): the bind affects only THIS Proc's namespace, and a
standalone `bind` exits immediately -- so the effect is observable only by a
process that inherits the namespace (a shell builtin, or a child spawned
after the bind). For a CLI binary, the test must observe the namespace from
WITHIN the same Proc (e.g. a wrapper that binds then walks) or from a
spawned child.

Preconditions: SOURCE `/a` and MOUNTPOINT `/b` both exist as directories;
`/a` contains a file `marker`.

| # | argv | expected (within the same/inherited namespace) | exit |
|---|---|---|---|
| T1 | `["bind","/a","/b"]` | `/b/marker` now resolves (== `/a/marker`) | 0 |
| T2 | `["bind","-b","/a","/b"]` | union: `/b` searches `/a` first | 0 |
| T3 | `["bind","/a","/nonexistent-mp"]` | error (mountpoint absent) | 1 |
| T4 | `["bind","/nonexistent-src","/b"]` | error (source open fails) | 1 |
| T5 | `["bind","/a"]` (one operand) | (stderr) usage | 1 |

OPEN QUESTION for the executor (a gap probe): SOURCE is opened with File
(OREAD). If the kernel requires different rights (e.g. O_PATH) for a mount
source, T1 fails -- record that as a gap (mount-source rights undocumented).
