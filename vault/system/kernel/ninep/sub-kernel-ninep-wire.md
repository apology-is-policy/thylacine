---
id: sub-kernel-ninep-wire
type: sub
title: "9P wire codec (9p_wire)"
parent: moc-kernel-ninep
code: [kernel/9p_wire.c, kernel/include/thylacine/9p_wire.h]
audit: hard
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: [docs/9P-EXTENSIONS.md]
created: 2026-07-31
updated: 2026-07-31
---
## Purpose

The pure byte-level marshal/unmarshal layer for the 9P2000.L dialect plus the
Thylacine/Stratum extension ops. The lowest layer of the kernel 9P stack:
[[sub-kernel-ninep-session]] composes it into the tag/fid state machine,
[[sub-kernel-ninep-client]] drives it over [[sub-kernel-ninep-transport]].
The codec holds **no kernel state** — no allocation, no locking, no I/O;
every function reads/writes a caller-supplied buffer and returns a byte count
or a negative error, so it is reentrant from any context.

## Contract

Three layers of API, all in `9p_wire.h`:

- **Primitives**: `p9_pack_u8/u16/u32/u64/qid/str` and the matching
  `p9_unpack_*`; `p9_peek_header` (size/type/tag without consuming the body);
  `p9_unpack_dirent` (one record out of an Rreaddir stream).
- **Builders** (`p9_build_t*`): version, attach, walk, clunk, flush, lopen,
  lcreate, read, write, getattr, setattr, readdir, statfs, fsync, the
  mutation family (symlink/mknod/rename/readlink/link/mkdir/renameat/
  unlinkat), and the extensions — `p9_build_twalkgetattr` (POUNCE),
  `p9_build_tweft`/`p9_build_rweft`, `p9_build_tweftio`/`p9_build_rweftio`
  (Weft; both directions exist because netd builds the R-side).
- **Parsers** (`p9_parse_r*`): one per R-message, plus `p9_parse_tweft`/
  `p9_parse_tweftio` for the server side of the kernel-issued extension ops.

Error convention: non-negative = bytes written/consumed (0 = parser success);
negative = refused (NULL out-param, buffer too small, malformed frame, wrong
opcode, string over the u16 cap). The caller short-circuits on the first
negative; there is no separate errno channel at this layer.

## Mechanism

Two disciplines carry the codec's whole soundness argument:

**Strict-equality framing.** Every parser enforces `header.size ==
frame_length` AND that the body cursor lands exactly on `frame_length` after
the last field. Truncated frames and trailing bytes are both rejected —
trailing bytes defend against a server emitting hidden extra payload (shape-
change masking / covert channel; the Stratum R111 P3 F-10 doctrine).

**Caller-cap bounds before exposure.** Server-supplied counts are bounded
against the caller's capacity BEFORE any write or pointer exposure:
`p9_parse_rwalk` bounds `nwqid` against `qid_cap` (and `P9_MAX_WALK`) before
unpacking a single qid; `p9_parse_rread`/`p9_parse_rreaddir` bound `count`
against `data_cap` before exposing `*data_ptr`; `p9_parse_rwalkgetattr`
additionally requires the body length to be exactly `nwqid *
P9_WGA_BODY_LEN` (153 — one Rgetattr body per walked component).

Zero-copy outputs (`p9_unpack_str`'s `out_ptr`, Rread/Rreaddir's `data_ptr`,
Rreadlink's target, Rwalkgetattr's `body`) alias the INPUT buffer — the
caller must not free or reuse the receive buffer while consuming them. In
practice every consumer parses out of the client's per-rpc reply buffer,
whose lifetime the client manages (the #841 `done_reply_buf` discipline
lives in [[sub-kernel-ninep-client]]).

## Data structures

Wire shapes (all integers little-endian, explicitly byte-shifted — matches
the AArch64 host but stays portable):

| Construct | Layout |
|---|---|
| Header (every msg) | `[size:u32][type:u8][tag:u16]` = `P9_HDR_LEN` 7 |
| 9P string | `[len:u16][bytes]` — never NUL-terminated; `P9_NAME_MAX` 255 per name |
| QID | `[type:u8][version:u32][path:u64]` = `P9_QID_LEN` 13 |
| Rwalkgetattr element | one 153-byte Rgetattr body (`P9_WGA_BODY_LEN`) |
| Tweft (142) | `[fid:u32]`; Rweft (143) = `struct p9_weft_geom` (share_id u64 + ring_size u32 + ring_entries u32, 16 B) |
| Tweftio (144) | `[fid][off][len][dir]` u32×4; `dir` = `WEFT_DIR_WRITE` 0 / `WEFT_DIR_READ` 1; Rweftio (145) = `[count:u32]` |
| Tflush (108) | `[oldtag:u16]`; Rflush (109) header-only |

In-memory structs mirrored from the wire: `p9_qid`, `p9_attr` (the Rgetattr
statx shape), `p9_setattr` (60-byte Tsetattr body), `p9_statfs`,
`p9_weft_geom`. Their sizes/offsets are pinned where consumers depend on
them (the Loom ABI asserts live with the Loom surface).

**Op-number registry.** The standard 9P2000.L numbers plus, in the shared
cross-project space of `docs/9P-EXTENSIONS.md`: Stratum extensions Tbind 124
… Tfallocate 132 (and Tfadvise 134 / Tpin 136 / Tunpin 138 on the Stratum
side), the shared `P9_TWALKGETATTR` **140**/141, `P9_TWEFT` **142**/143,
`P9_TWEFTIO` **144**/145. The Weft pair was born 134/135/136/137 and
renumbered at #371 after latently colliding with Stratum's Tfadvise/Tpin —
the standing rule is: allocate from the shared registry document, never from
one project's enum alone. `P9_QTPOLL` (0x01) is the readiness-file qid bit
[[sub-kernel-ninep-dev9p-poll]] keys on; `P9_NOFID` 0xFFFFFFFF and
`P9_NOTAG` 0xFFFF are the sentinels; `P9_MAX_WALK` 16 caps a walk.

## Concurrency

None. The codec is stateless and lock-free by construction; concurrency is
entirely the caller's problem. This is load-bearing for its consumers: the
session dispatches under the client's `c->lock`, and a codec that allocated
or slept would break the on_complete seam contract
([[lock-9p-client-c-lock]]).

## Invariants enforced

No §28 invariant is enforced at this layer — the codec cannot see sessions.
It *serves* [[inv-i10]] and [[inv-i11]] (a mis-decoded tag or corrupted
`newfid` would break them upstream) and the I-14 boundary (hostile Rlerror
ecodes are bounded by the client's `map_error`, but the parser's strict
framing is what guarantees the ecode field IS the ecode field). Compile-time
pins: `_Static_assert`s on `P9_HDR_LEN`, `P9_QID_LEN`, sentinels, and every
`RX == TX + 1` opcode pairing.

## Error paths

Every function: negative on NULL required out-params, short caller buffers,
`slen > 0xFFFF`, `header.size != frame_length`, wrong opcode, trailing
bytes, `nwqid > qid_cap`/`P9_MAX_WALK`, Rread/Rreaddir `count > data_cap`,
Rwalkgetattr body-length mismatch. Builders cap total body length at the u32
`size` ceiling.

## Performance

Constant per-byte cost (shifts + stores); no allocation, no syscalls, no
locks. Never measured as a bottleneck — the wire time is dominated by the
transport and the server.

## Prosecution

What an auditor attacks here (changes to this surface ride the
[[sub-kernel-ninep-client]] audit-trigger family — the #841 spec-gate rule
"any change to tag/fid/outstanding semantics re-runs the buggy cfgs"
includes wire-level tag/fid encoding):

- **Parser bounds** on every server-supplied length/count — the caller-cap
  discipline above. POUNCE P-5 prosecuted `p9_parse_rwalkgetattr` exactly
  here (nwqid bound + the exact `nwqid * 153` body equality).
- **Strict-equality regressions** — a parser that tolerates trailing bytes
  reopens the shape-masking channel.
- **Zero-copy aliasing** — a new consumer that frees/reuses the input buffer
  while holding a parsed pointer (the #841 R1-F1 reply-buffer UAF was this
  class one layer up).
- **Op-number collisions** — a new extension op MUST be allocated in
  `docs/9P-EXTENSIONS.md` across both projects (#371 is the worked failure).
- **Builder/parser asymmetry** on the dual-sided extension ops (Tweft/
  Tweftio have kernel-side builders AND parsers; netd mirrors them in
  `libthyla_rs::ninep` — a drift breaks the pair silently).

## Seams

None open on this surface. (The lock/getlock and xattr families plus the
Stratum extension builders beyond what the client drives remain unbuilt —
absent features, not debt; they land with their consumers.)

## Caveats

- `p9_build_*` writes the entire output buffer through the message length —
  don't alias it with live data.
- No msize enforcement here: the codec builds/parses whatever it is handed;
  msize policy lives in the session (negotiation) and the client (payload
  clamps, CF-3).
- The in-kernel `QT*` constants happen to equal the wire `P9_QT*` values;
  `qid_type_p9_to_kernel` in [[sub-kernel-ninep-dev9p]] still copies
  bit-by-bit rather than relying on the coincidence.

## Provenance

(generated from incoming `touched` edges — see the Record plane; the wire
codec's shaping chunks include P5-wire/-io/-meta/-mutation, #845 Tflush,
Weft-6a/6b + the #371 renumber, and POUNCE P-1.)

## Tests

`kernel/test/test_9p_wire.c` — the round-trip + rejection battery: every
supported family round-trips, and every rejection path (truncated header,
size mismatch, wrong type, over-cap nwqid/count, malformed body) is driven
directly with synthesized frames. Representative names:
`test_9p_wire_twalk_zero_names_clone`, `test_9p_wire_tflush_round_trip`.
The suite runs in the default kernel test pass (boot-gated).
