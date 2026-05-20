# 66. /sbin/corvus server loop + wire codec (P5-corvus-bringup-b)

Second impl chunk of the corvus key-agent daemon. Extends the P5-corvus-bringup-a skeleton (which ran the hardening syscalls and exited) into a single-peer **Spoor server** that accepts binary-framed verb requests on its rx pipe (fd 0), dispatches AUTH + SESSION_CLOSE, returns response frames on its tx pipe (fd 1), and exits 0 on rx EOF.

The wire format matches CORVUS-DESIGN.md §6.4. AUTH at this sub-chunk is a **skeleton without crypto verification** — the passphrase is parsed but never checked; Argon2id integration lands at P5-corvus-bringup-c. The contribution of this chunk is the structural wire surface + session-table management + the spec's `AuthSuccess` / `SessionClose` actions wired through to real code paths.

---

## Wire format

Per CORVUS-DESIGN.md §6.4.

### Request frame

```
offset 0     verb_id           u8
offset 1     protocol_version  u8   = 1   (STRATUM-API-V1.md Q11)
offset 2     payload_len       u16 little-endian
offset 4..   payload           verb-specific
```

A frame carrying `protocol_version != 1` is refused with `BadFormat` and the connection terminates — the frame's shape may change across versions, so the stream cannot be safely re-synced. Landed at P5-corvus-srv-impl-b1 (was a 3-byte header at -b through -d).

### Response frame

```
offset 0     status            u8     (0=OK, 1=BadAuth, 2=PermissionDenied,
                                       3=NotFound, 4=RateLimited,
                                       5=BadFormat, 6=InternalError)
offset 1     payload_len       u16 little-endian
offset 3..   payload           status-specific
```

### Verbs implemented

| `verb_id` | Name | Request payload | Response payload (status=OK) |
|---|---|---|---|
| `1` | AUTH | `user_len u8` + `user` + `pass_len u16 LE` + `passphrase` | `33-byte session token ("s" + 32 hex chars)` |
| `3` | SESSION_CLOSE | `token` (33 bytes) | (empty) |

Every other `verb_id` returns status=`BadFormat`. UNWRAP (`4`), CHANGE_PASSPHRASE (`2`), USER_CREATE (`5`), USER_DELETE (`6`), ADMIN_ELEVATE (`7`), RECOVER (`8`), ROTATE_KEY (`9`) land at future sub-chunks.

### Limits

- `MAX_USER_LEN = 32`
- `MAX_PASS_LEN = 256`
- `MAX_PAYLOAD_LEN = 291` (the AUTH-frame worst case)
- A frame whose `payload_len > MAX_PAYLOAD_LEN` is rejected with `BadFormat` and the connection terminates (corvus exits non-zero rather than try to drain an oversize payload).

---

## Session token format

`33 bytes ASCII = "s" + 32 hex chars`. The 32 hex chars encode 16 bytes (128 bits) of CSPRNG entropy from `t_getrandom`. The leading `'s'` byte is a visual marker (CORVUS-DESIGN.md §4.2: "Sessions are identified by an opaque token (`s` + 128 bits of CSPRNG hex)"); the actual entropy is the 32 hex chars.

The raw entropy buffer is wiped via `t_explicit_bzero` immediately after hex-encoding — the hex form lives in the session table; the raw bytes have no further use.

---

## Session table

Single-slot at this skeleton. The static-global `SESSION: Session` record holds:

```rust
struct Session {
    active: bool,
    user_len: u8,
    user: [u8; 32],
    token: [u8; 33],
}
```

The accessor wrappers (`session_active`, `session_install`, `session_token_matches`, `session_clear`) operate via raw pointers (`core::ptr::addr_of!` / `addr_of_mut!`) to avoid Rust 1.77+'s `static_mut_refs` lint and to keep the code Miri-honest if corvus's server loop is ever multiplexed in the future.

Spec correspondence (specs/corvus.tla):

- **C-3 SessionUserImmutable** — the `user` field is written once at `session_install()` and never modified until `session_clear()` (which zeroes the entire record). There is no setter that mutates `user` in place. A future bug class equivalent to `BuggyAuthBindingMutate` is structurally unreachable at the Rust level — the API surface doesn't expose such a path.
- **One-session-per-peer** (spec's `AuthSuccess` precondition `~(\E s : s.owner_proc = p)`) — `handle_auth()` checks `session_active()` first; if true, returns `PermissionDenied`.

---

## Server loop

```
read_exact 4-byte header from fd 0
  ↓ if EOF → exit 0
  ↓ parse verb_id + protocol_version + payload_len
  ↓ refuse protocol_version != CORVUS_PROTOCOL_VERSION (BadFormat + terminate)
  ↓ refuse payload_len > MAX_PAYLOAD_LEN
read_exact `payload_len` bytes
dispatch by verb_id:
  ↓ VERB_AUTH(1)          → handle_auth
  ↓ VERB_SESSION_CLOSE(3) → handle_session_close
  ↓ other                  → status=BadFormat
write response frame to fd 1
explicit_bzero(payload buffer)  ← defence in depth
repeat
```

The payload buffer is `explicit_bzero`-wiped after every iteration so any plaintext secrets parsed from the frame (passphrases at -c onward) don't linger across iterations.

---

## Joey orchestration

`usr/joey/joey.c` is the test driver for this chunk. After the existing `/hello` orchestration, joey:

1. Creates 2 pipes via `t_pipe` (4 fds total).
2. Calls `t_spawn_full("corvus", 6, [c2s_rd, s2c_wr], 2, T_CAP_LOCK_PAGES | T_CAP_CSPRNG_READ)`. The kernel-side SYS_SPAWN_FULL handler bumps spoor refs for `c2s_rd` and `s2c_wr`, installs them at corvus's slots 0 and 1, AND's `cap_mask` with joey's caps, exec's `/sbin/corvus`.
3. Closes joey's copies of the child-side fds. corvus's slots keep the underlying Spoors alive via the kernel's ref bumps.
4. Builds an AUTH frame (`verb_id=1`, user=`"michael"`, pass=`"skeleton-passphrase-do-not-use"`, payload_len=36, total=39 bytes) and `t_write`s it to `c2s_wr`.
5. `t_read`s 3 header bytes, then `payload_len` body bytes from `s2c_rd`. Verifies status=0, payload_len=33, token[0]='s'.
6. Builds a SESSION_CLOSE frame (`verb_id=3`, payload=the token from step 5, payload_len=33, total=36 bytes) and `t_write`s it.
7. `t_read`s the response. Verifies status=0, payload_len=0.
8. Closes `c2s_wr`. corvus's rx side drops to ref=0; the pipe's read-side sees EOF; corvus's `server_loop` returns 0; corvus exits clean.
9. `t_wait_pid(corvus)`. Asserts status=0.
10. Closes `s2c_rd`.

Boot log on success:

```
joey: spawned /sbin/corvus pid=N
corvus: skeleton starting (P5-corvus-bringup-b)
corvus: ready (hardening applied; serving /srv/corvus/ over fd 0/1)
joey: AUTH ok (token=s3e02730...)
joey: SESSION_CLOSE ok
corvus: server_loop returned EOF; shutting down clean
joey: /sbin/corvus reaped status=0; AUTH + SESSION_CLOSE round-trip verified
```

The "joey: AUTH ok (token=...)" line prints the first 8 chars of the token (a glimpse, not the whole secret) for log readability.

---

## Spec ↔ code mapping

`specs/corvus.tla` (P5-corvus-spec at `c00de63`):

| Spec action | Source location |
|---|---|
| `AuthSuccess(p, u)` | `usr/corvus/src/main.rs::handle_auth` (steps: parse → check session_active → t_getrandom + hex-encode → session_install) |
| `SessionClose(p)` | `usr/corvus/src/main.rs::handle_session_close` (steps: parse token → session_token_matches → session_clear) |
| `SessionTransfer` | NOT YET — requires multi-peer support (lands when more than one Proc talks to corvus over distinct Spoor pairs). |
| `AdminElevate` / `Unwrap` / `AdminVerb` | NOT YET (verbs deferred). |
| `MarkConsoleAttached` | NOT YET — requires kernel-side `/srv/corvus/peer/` surface. |

| Spec invariant | Source enforcement |
|---|---|
| `SessionUserImmutable` (C-3) | `Session::user` is set only at `session_install()` (which is called only from `handle_auth()` when `!session_active()`). No setter exposes mid-life mutation. |
| `UnwrapOwnerOnly` (C-7) | NOT YET — UNWRAP verb is the test surface; lands at -d or later. |
| `AdminRequiresProcCap` (C-11 Proc-cap path) | NOT YET — admin verbs deferred. |
| `HostownerRequiresConsole` (§5.5) | NOT YET — admin-elevate deferred. |

---

## Rust runtime additions

`usr/lib/libthyla-rs/src/lib.rs` gained 4 new SVC wrappers + 4 new syscall consts + 1 forward-compat const:

- `T_SYS_PIPE = 8`, `T_SYS_READ = 9`, `T_SYS_WRITE = 10`, `T_SYS_CLOSE = 11`
- `T_SYS_SPAWN_FULL = 25` (declared for parity; not used by corvus itself yet)
- `t_pipe() -> (i64, i64)` — returns `(rd_fd, wr_fd)` tuple
- `t_read(fd, buf, len) -> i64`
- `t_write(fd, buf, len) -> i64`
- `t_close(fd) -> i64`

The C-side libt already had the equivalent stubs from P5-fd-* and P5-spawn-full; this chunk completes the Rust mirror.

---

## Tests

The boot path IS the regression. `tools/test.sh` watches for `Thylacine boot OK`, gated on joey's clean exit, gated on:
1. `joey: AUTH ok` line.
2. `joey: SESSION_CLOSE ok` line.
3. `corvus: server_loop returned EOF; shutting down clean` line.
4. `joey: /sbin/corvus reaped status=0; AUTH + SESSION_CLOSE round-trip verified` line.

A regression at any step prints a tagged failure marker (e.g., `joey: AUTH returned non-OK status=...`) and exits non-zero, breaking the boot test.

No dedicated kernel-internal tests added — the existing P5-fd-* and P5-spawn-full suites cover each syscall in isolation; this chunk composes them. Future sub-chunks add wire-level regression tests as the verb surface grows (e.g., a `test_corvus_wire.c` once UNWRAP lands).

**Timing note**: total boot from kernel-test-suite completion to `Thylacine boot OK` is ~15s under default (UBSan ~25s). The 442 kernel tests + joey orchestration with two execs (hello + corvus) + pipe round-trips + wait_pids occasionally brushes the default `BOOT_TIMEOUT=15`; if a CI run flakes, raise to `BOOT_TIMEOUT=60` (already standard for the UBSan matrix). A future P5-tsan-enable chunk will need it standard.

---

## Status

**Transport superseded at P5-corvus-srv-impl-b3b.** The pipe-pair harness (corvus's fd 0/1) is retired; corvus is now a real 9P2000.L server reached via `/srv/corvus`. See `docs/reference/74-corvus-9p-server.md` for the current transport. **The wire format + verb semantics + session table documented above are unchanged and still in force** — only the I/O delivery moved from pipes to `KObj_Srv` connection handles backed by the kernel-side `SrvConn` ring transport.

Verb implementations beyond this chunk's two-verb skeleton:
- AUTH crypto verification: Argon2id added at P5-corvus-bringup-c (`docs/reference/68-corvus-crypto.md`).
- USER_CREATE + state file (`CRVS` magic): P5-corvus-bringup-c.
- WRAP + UNWRAP (hybrid PKE DEK envelope): P5-corvus-bringup-d (`docs/reference/69-corvus-unwrap.md`).
- /srv/corvus 9P transport: P5-corvus-srv-impl-b3b (`docs/reference/74-corvus-9p-server.md`).

Still out of scope (deferred beyond b3b):
- CHANGE_PASSPHRASE / USER_DELETE / ADMIN_ELEVATE / RECOVER / ROTATE_KEY verbs.
- Encrypted audit log.
- Rate limiter.
- Idle / absolute session timeouts.

---

## Known caveats

- AUTH at this sub-chunk accepts any non-empty passphrase. Do NOT confuse the skeleton with a security boundary — `/sbin/corvus` doesn't gate anything sensitive at v1.0-rc.0; -c adds the gate.
- The session-table is single-slot. Cross-Proc session transfer (`SessionTransfer` in the spec) requires multi-slot expansion (one session per peer Proc), which the kernel-side identity surface doesn't yet expose to corvus.
- The boot test framework's `EXTINCTION:` detection at line-start catches a benign EL0 sync-exception that fires from joey's exec_setup path (pre-existing; visible on every Phase 5 boot). The test framework checks `BOOT_MARKER` first; the boot succeeds as long as `Thylacine boot OK` is emitted before the 15s/60s timeout.
- corvus's t_putstr emissions go to the kernel diagnostic UART (visible in the boot log), not to the s2c_wr pipe. The pipe carries only binary response frames. This is by design: the UART is for human-readable diagnostics; the pipe is for the wire protocol.

---

## References

- `docs/CORVUS-DESIGN.md §4.2` — Spoor interface + verb table.
- `docs/CORVUS-DESIGN.md §6.4` — Binary frame format.
- `docs/CORVUS-DESIGN.md §9` — Invariants C-3, C-5, C-7, C-11.
- `specs/corvus.tla` — Gating spec.
- `specs/SPEC-TO-CODE.md § corvus.tla` — Action↔impl-target table.
- `docs/reference/65-corvus-skeleton.md` — P5-corvus-bringup-a (the predecessor).
- `docs/reference/62-sys-spawn-with-fds.md` — Spawn-with-fds primitive (precursor to spawn_full).
- `docs/reference/64-sys-spawn-full.md` — Spawn-with-fds + spawn-with-caps unified.
- `docs/reference/58-corvus-syscalls.md` — Hardening syscalls (mlockall, set_dumpable, set_traceable, explicit_bzero, getrandom).
