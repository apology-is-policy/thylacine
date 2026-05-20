# 74. /sbin/corvus 9P2000.L server transport (P5-corvus-srv-impl-b3b)

The fourth and final transport rewrite of the corvus key-agent daemon. The wire format (CORVUS-DESIGN.md §6.4 binary verb frames) and the verb semantics (AUTH / USER_CREATE / UNWRAP / WRAP / SESSION_CLOSE) are unchanged at the dispatch layer; **only the I/O transport changes**. Where -b through -d ran a single peer over a kernel-managed pipe pair (corvus's fd 0 + fd 1), -b3b makes corvus a real **9P2000.L server**: corvus posts `/srv/corvus`, joey reaches it via `SYS_SRV_CONNECT("corvus", "ctl")`, and verb frames flow over the resulting `KObj_Srv` client handle — `read` / `write` syscalls in joey land at the kernel-side `SrvConn` ring transport, which surfaces as `Tread` / `Twrite` 9P frames the corvus server side handles.

This is the chunk that retires the fd 0/1 pipe-pair harness for good. The verb test sequence (USER_CREATE × 2 + AUTH wrong + AUTH ok + WRAP + UNWRAP × 5 + SESSION_CLOSE) runs end-to-end over the new transport, plus the **Q11 negative regression** (verb frame with bad protocol_version → BadFormat + corvus tear-down + joey reconnects + verifies fresh AUTH).

The predecessor doc `66-corvus-server.md` describes the pipe-pair-era server loop; this doc supersedes the transport sections of that doc. The wire format + session table + Argon2id / AEGIS-256 / ML-KEM-768 / X25519 / SHA-256 / DEK-envelope verb implementations described in earlier chapters (66 + 68 + 69) are unchanged at b3b.

---

## Architecture

```
┌───────────────────────┐                        ┌────────────────────────────┐
│ joey (Proc 1454,      │                        │ /sbin/corvus (Proc 1456,   │
│  CONSOLE_ATTACHED)    │                        │  MAY_POST_SERVICE)         │
│                       │                        │                            │
│  t_spawn_with_perms() ──── PROC_FLAG_* ───────►│  rs_main:                  │
│                       │    stamp by kernel     │    heap_init               │
│                       │    in spawn thunk      │    mlockall                │
│                       │                        │    set_dumpable(0)         │
│                       │                        │    t_post_service("corvus")│
│                       │                        │     │                      │
│                       │                        │    listener: KObj_Srv      │
│  t_srv_connect(       │                        │                            │
│   "corvus", "ctl")    ◄──── kernel drives ─────│  srv_server_loop(listener):│
│   │                   │    Tversion + Tattach  │    t_poll([listener,       │
│   │                   │    + Twalk("ctl")      │             conns...])     │
│   │                   │    + Tlopen(O_RDWR)    │    on listener-ready:      │
│   │                   │                        │      t_srv_accept(listener)│
│   ▼                   │                        │       → conn KObj_Spoor    │
│  client KObj_Srv      │                        │    on conn-ready:          │
│                       │                        │      t_read → 9P Tmsg      │
│                       │                        │      dispatch_one →        │
│                       │                        │        Rmsg → t_write      │
│  t_write(conn, frame) ─── kernel SrvConn ─────►│  9P Twrite → /ctl file:    │
│                       │   c2s ring transport   │   accumulate verb frame    │
│                       │                        │   → dispatch_verb          │
│                       │                        │   → stage Rmsg with reply  │
│  t_read(conn, hdr+pl) ◄── kernel SrvConn ──────│  9P Tread → drain reply    │
│                       │   s2c ring transport   │   bytes from pending       │
└───────────────────────┘                        └────────────────────────────┘
```

**Two distinct fid spaces** sit at the two sides of every connection:
- joey's side: the `KObj_Srv` client handle's hidden 9P client (the kernel-owned `p9_client` inside the `SrvConn`). joey never sees fids; it only sees a "fd" (handle index) returned by `t_srv_connect`. `t_write` and `t_read` on that handle translate to `Twrite` and `Tread` on a single 9P fid the kernel walked + opened during the handshake.
- corvus's side: a per-conn `[Option<FidEntry>; MAX_FIDS_PER_CONN]` table (heap-allocated, size 8). corvus's 9P server tracks fids as the client (i.e., the kernel-internal client) walks/opens/clunks them.

The reason this works: `t_srv_connect` synchronously drives the full handshake. By the time `t_srv_connect` returns to joey, the kernel-internal client has minted a fid, walked to `/ctl`, opened it for read+write, and the corvus server has installed a fid table entry that maps that fid to the `/ctl` QID. From then on, every `t_write` from joey is a `Twrite` on that fid; every `t_read` is a `Tread`.

---

## /srv/corvus namespace at the server

corvus's namespace exposed via /srv/corvus is intentionally minimal — at v1.0 there is exactly one file: `/ctl`. Every verb frame flows through it; no per-verb file granularity. The Plan-9-shaped expansion (one file per verb, named after the verb) is a v1.x option but is not needed at v1.0.

```
/srv/corvus/         QTDIR  (the root the handshake's Twalk("ctl") begins from)
└── ctl              QTFILE (the verb-frame file)
```

**QIDs** (per CORVUS-DESIGN.md §6 + 9P2000.L semantics):
| Path | type | version | path |
|---|---|---|---|
| `/` | `QTDIR` (0x80) | 0 | 1 |
| `/ctl` | `QTFILE` (0x00) | 0 | 2 |

QID type bits match 9P2000.L convention. The version is 0 (corvus never bumps; the file's content is the live verb stream, not a versioned blob).

**File semantics**:
- `Tread` on `/ctl` drains pending response bytes (server-staged Rmsg payloads). corvus IGNORES the `offset` field — `/ctl` is message-oriented, not byte-stream; the offset would be racy on a verb-by-verb pattern. corvus tracks its own `pending_response_off` per Conn.
- `Twrite` to `/ctl` appends bytes to the per-Conn request accumulator. Once a complete verb frame is observed (4-byte header + payload_len bytes), corvus dispatches the verb and stages the response on the same Conn's `pending_response`. The subsequent `Tread`(s) drain it.

This is "message-oriented over a byte stream Tread/Twrite" — the 9P client (kernel-internal) sees a regular file; the server treats it as a verb frame queue.

---

## Pure-Rust 9P2000.L codec

`usr/corvus/src/p9.rs` (NEW, ~370 LOC) implements the 9P codec corvus's side needs — Tmsg parsers + Rmsg builders for the 8 operations the kernel-internal client drives during handshake + Tread/Twrite (no inner ops, no Rauth-success, no extended attributes etc. at v1.0).

**Mtype constants** (mirroring `kernel/9p_wire.h`):
- `P9_TVERSION = 100`, `P9_RVERSION = 101`
- `P9_TAUTH = 102`, `P9_RAUTH = 103`
- `P9_TATTACH = 104`, `P9_RATTACH = 105`
- `P9_TWALK = 110`, `P9_RWALK = 111`
- `P9_TLOPEN = 12`, `P9_RLOPEN = 13`
- `P9_TREAD = 116`, `P9_RREAD = 117`
- `P9_TWRITE = 118`, `P9_RWRITE = 119`
- `P9_TCLUNK = 120`, `P9_RCLUNK = 121`
- `P9_RLERROR = 7` (the Linux-flavored unified error response)

**Sentinels**:
- `P9_QTDIR = 0x80` (directory QID type bit)
- `P9_QTFILE = 0x00` (regular-file QID type bit; explicit zero for clarity)
- `P9_NOFID = 0xFFFFFFFF`
- `P9_NOTAG = 0xFFFF`
- `P9_HDR_LEN = 7` (size[4] + type[1] + tag[2])
- `P9_QID_LEN = 13` (type[1] + version[4] + path[8])
- `P9_MAX_WALK = 16` (POSIX-equivalent component cap)
- `P9_NAME_MAX = 255`

**Types**:
- `struct Qid { kind: u8, version: u32, path: u64 }`
- `struct Header { size: u32, mtype: u8, tag: u16 }`

**Primitive packers / unpackers** are explicit little-endian, no `byteorder` crate dependency — `pack_u8 / 16 / 32 / 64`, `unpack_u8 / 16 / 32 / 64`, `pack_str` (length-prefix + bytes), `unpack_str` (zero-copy reference into the input buffer; lifetime-bound to the caller's slice), `pack_qid`.

**Tmsg parsers** (server-side; corvus parses what the kernel-internal client sends):
- `parse_tversion(payload) -> (msize, version_str)`
- `parse_tattach(payload) -> (fid, afid, uname, aname, n_uname)`
- `parse_twalk(payload) -> (fid, newfid, wnames: Vec<&str>)`
- `parse_tlopen(payload) -> (fid, flags)`
- `parse_tread(payload) -> (fid, offset, count)`
- `parse_twrite(payload) -> (fid, offset, data: &[u8])`
- `parse_tclunk(payload) -> fid`

**Rmsg builders** (server-side; corvus emits responses):
- `build_rversion(buf, tag, msize, version)`
- `build_rattach(buf, tag, qid)`
- `build_rwalk(buf, tag, qids: &[Qid])`
- `build_rlopen(buf, tag, qid, iounit)`
- `build_rread(buf, tag, data: &[u8])`
- `build_rwrite(buf, tag, count: u32)`
- `build_rclunk(buf, tag)`
- `build_rlerror(buf, tag, errno: u32)`

The 9P frame layout — 4-byte size header + 1-byte mtype + 2-byte tag + msg-specific payload — is constructed by the builder writing the size LAST after computing total length. Mirrors `kernel/9p_wire.c`'s pattern.

---

## Per-connection state

`usr/corvus/src/main.rs::Conn` is the server-side per-connection record:

```rust
struct Conn {
    handle: i64,                                 // KObj_Spoor accept fd
    #[allow(dead_code)]                          // reserved for ConnOpIdentityIsKernelTruth checks
    peer: TSrvPeerInfo,                          // kernel-stamped {stripes, caps, console, alive}
    version_done: bool,                          // Tversion seen → msize negotiated
    msize: u32,                                  // SERVER_MSIZE = 4096
    fids: [Option<FidEntry>; MAX_FIDS_PER_CONN], // 8 fid slots
    in_buf: Vec<u8>,                             // 9P frame ingress buffer
    out_buf: Vec<u8>,                            // 9P frame egress buffer
    pending_request: Vec<u8>,                    // verb-frame request accumulator
    pending_response: Vec<u8>,                   // verb-frame response stage area
    pending_response_off: usize,                 // bytes drained from pending_response so far
}
```

**FidEntry** is a small union over what each fid maps to — at v1.0, just `Root` (the namespace root QID 1) or `Ctl` (the `/ctl` file, QID 2). At v1.x with multiple verb files, `FidEntry` would gain variants.

`MAX_FIDS_PER_CONN = 8` is a v1.0 cap. The kernel-internal client uses only 2 fids per connection (one for the attach-root, one for the walked `/ctl`); 8 leaves headroom for v1.x.

**Constants** (mirror `kernel/srvconn.h`'s `SRVCONN_MSIZE`):
- `SERVER_MSIZE: u32 = 4096` — must equal the kernel-side `SRVCONN_MSIZE`; the kernel's `srvconn_drive_client_handshake` proposes 4096 and the server's Tversion handler accepts it. A drift here would surface as a handshake failure at the very first Tversion.
- `MAX_CONNS = 8` — max simultaneous accepted connections corvus tracks. At v1.0 joey is the only client and the kernel-side `SRV_CONN_PER_PROC_MAX = 1` already gates this; the userspace cap is defensive.
- `REQ_HDR_LEN = 4` — verb-frame request header (verb_id u8 + protocol_version u8 + payload_len u16).
- `RESP_HDR_LEN = 3` — verb-frame response header (status u8 + payload_len u16).

---

## Server loop

```
SESSION (global at v1.0) — single session token, one user
listener = t_post_service("corvus", 6)

conns: Vec<Conn>                      ← acquired via t_srv_accept

loop:
    poll_fds = [listener, conns[0], conns[1], ...]
    t_poll(poll_fds, count, -1)       ← block until any ready
    if listener.revents & POLLIN:
        new_handle = t_srv_accept(listener)
        peer       = t_srv_peer(new_handle)
        conns.push(Conn::new(new_handle, peer))
    for conn in conns (reverse order so .remove(i) doesn't shift):
        if conn.revents & (POLLHUP|POLLERR):
            close_conn(conn)
            continue
        if conn.revents & POLLIN:
            service_conn(conn)         ← reads up to SERVER_MSIZE bytes,
                                          drains every complete 9P Tmsg
        if conn.revents & POLLOUT && conn.out_buf nonempty:
            drain conn.out_buf via t_write
```

The single corvus thread services every connection cooperatively. v1.0's `SRV_CONN_PER_PROC_MAX = 1` means only one client connection at any moment, but the loop is structured for multi-connection at v1.x without code reshaping — only the global `SESSION` would need to become per-Conn-keyed.

---

## Tmsg dispatch

`dispatch_one(conn, hdr, payload)` consumes one parsed Tmsg + produces one Rmsg in `conn.out_buf`. The 8 ops:

### Tversion
- Parse `msize` + `version_str`.
- If `version_str != "9P2000.L"`: reply `Rlerror(EPROTONOSUPPORT = 93)`.
- Otherwise: `conn.msize = min(msize_proposed, SERVER_MSIZE)`; `version_done = true`; reply `Rversion(msize_negotiated, "9P2000.L")`.
- Tversion resets every existing fid (per 9P2000 spec). v1.0 corvus's `conn.fids[]` is empty pre-Tversion so this is a no-op; documented for safety.

### Tattach
- Parse `fid, afid, uname, aname, n_uname`.
- `afid` MUST be `NOFID` (no AUTH file at v1.0).
- Install `fid → FidEntry::Root` in `conn.fids[]`.
- Reply `Rattach(qid: ROOT)`.

### Twalk
- Parse `fid, newfid, wnames`.
- `fid` must resolve to an existing entry (typically `Root`).
- For each name in `wnames`:
  - If walking from `Root` and name=`"ctl"`: descend to the ctl QID.
  - Else: reply `Rlerror(ENOENT)` (partial walks: the spec requires reporting the QID list of the prefix that succeeded, but v1.0 takes the conservative path of refusing any nonexistent name).
- Install `newfid → FidEntry::<resolved>`.
- Reply `Rwalk(qids: prefix list)`.

### Tlopen
- Parse `fid, flags`.
- The fid's entry must support open. `Ctl` opens for read+write (or any subset).
- Reply `Rlopen(qid, iounit = 0)` (iounit=0 means "use msize - hdr_len as the natural read/write unit").

### Tread
- Parse `fid, offset, count`.
- Only `Ctl` supports Tread at v1.0.
- IGNORE the `offset` field — `/ctl` is message-oriented. Drain bytes from `conn.pending_response[conn.pending_response_off..]`, up to `count` bytes.
- If `pending_response` is exhausted (drained == response length), reset `pending_response = empty` + `pending_response_off = 0`. The next Tread on an empty `pending_response` returns 0 bytes (matches POSIX EOF-on-empty-message semantics).
- Reply `Rread(data: drained bytes)`.

### Twrite
- Parse `fid, offset, data`.
- Only `Ctl` supports Twrite at v1.0.
- Append `data` to `conn.pending_request`.
- `try_dispatch_verb(conn)`:
  - If `pending_request.len() >= 4`:
    - parse header → verb_id, protocol_version, payload_len.
    - If `protocol_version != 1`: stage `Rlerror`-equivalent verb-frame `{ status=BadFormat(5), payload_len=0 }` AND tear down (set conn EOF). The Q11 discipline — stream cannot be safely re-synced across a version mismatch.
    - If `pending_request.len() < 4 + payload_len`: incomplete frame, return — wait for more bytes.
    - Else: extract complete verb frame, drain it from `pending_request`, call the verb handler (`handle_auth` / `handle_user_create` / `handle_unwrap` / `handle_wrap` / `handle_session_close`), stage the response in `conn.pending_response` (3-byte header + payload).
- Reply `Rwrite(count: data.len() as u32)`.

### Tclunk
- Parse `fid`.
- Drop the fid's slot in `conn.fids[]`.
- Reply `Rclunk`.

### Tauth
- Reply `Rlerror(ENOSYS = 38)`. corvus uses kernel-stamped peer identity (the `TSrvPeerInfo` from `t_srv_peer`), not the 9P AUTH dance.

---

## Verb handlers (unchanged from -d)

The five verb handlers (`handle_auth`, `handle_user_create`, `handle_unwrap`, `handle_wrap`, `handle_session_close`) are byte-equivalent at the dispatch level to the -d pipe-pair era. Their signatures changed from `(payload) -> Result<Vec<u8>, Status>` to `(payload: &[u8], response: &mut Vec<u8>)` — they now write the response payload into a caller-owned buffer (the Conn's `pending_response`) rather than returning it by value, and the 3-byte response header is now written by the caller's `stage_response` helper.

The cryptographic semantics are identical (Argon2id passphrase hashing + AEGIS-256 sealing of the user state + ML-KEM-768 + X25519 hybrid PKE for the DEK envelope + SHA-256 for derived keys). See:
- `docs/reference/68-corvus-crypto.md` — Argon2id + AEGIS-256 + `CRVS` state file + USER_CREATE.
- `docs/reference/69-corvus-unwrap.md` — Hybrid PKE + WRAP + UNWRAP envelope.

---

## joey orchestration changes

`usr/joey/joey.c::main`'s corvus block changed:

**Before (-d era)**:
```c
long rfd, wfd;
t_pipe(&rfd_corvus, &wfd_corvus);
t_pipe(&rfd_joey, &wfd_joey);
long pid = t_spawn_full("corvus", ..., {rfd_corvus, wfd_joey}, 2, T_CAP_LOCK_PAGES|T_CAP_CSPRNG_READ);
t_close(rfd_corvus); t_close(wfd_joey);  // joey drops its child-side copies
// joey reads from rfd_joey, writes to wfd_corvus
```

**After (b3b)**:
```c
const char corvus_name[] = "corvus";
unsigned int no_fds[1] = { 0 };
long corvus_pid = t_spawn_with_perms(
    corvus_name, sizeof(corvus_name) - 1,
    no_fds, 0,                                    // no inherited fds
    T_CAP_LOCK_PAGES | T_CAP_CSPRNG_READ,
    T_SPAWN_PERM_MAY_POST_SERVICE);               // grants the post-gate
long conn_fd = connect_corvus();                  // bounded retry around t_srv_connect
// joey reads + writes on conn_fd; transport: kernel-side SrvConn → /srv/corvus/ctl
```

`connect_corvus()` uses a bounded retry loop around `t_srv_connect("corvus", "ctl")` (the kernel handshake fails until corvus actually posts the service) with a `t_poll` yield in between — 60 iterations × 1000 ms timeout = 60 s total yield budget. On emulated QEMU AArch64 (TCG-interpreted), corvus's `exec_setup` allocates a 24 MiB BSS (the linked_list_allocator working memory; HEAP_BUF in `usr/corvus/src/main.rs`) which the buddy allocator rounds to order-13 = 32 MiB contiguous, KP_ZERO-zeroes ~4 million u64 stores; this takes upwards of ten seconds before corvus reaches `t_post_service`. The retry budget is generous to accommodate the worst-case emulator + scheduling interleave; on hardware (or HVF/KVM) the first iteration typically succeeds.

The yield mechanism (`t_poll` on a never-ready pipe-read end) puts joey's thread into `tsleep`; the kernel scheduler picks corvus's thread to run on a different vCPU. This is the standard wait-for-startup idiom in the absence of a kernel "wait-for-service-post" syscall (which the v1.x evolution might add but is not needed at v1.0).

---

## Spec ↔ code mapping

`specs/corvus.tla` (the connection layer was added in P5-corvus-srv-design):

| Spec action | Source location |
|---|---|
| `MarkMayPost(p)` | parent calls `SYS_SPAWN_WITH_PERMS(..., T_SPAWN_PERM_MAY_POST_SERVICE)` → kernel thunk stamps `proc_mark_may_post_service` BEFORE `exec_setup` (P5-corvus-srv-impl-b3a) |
| `PostService(p)` | corvus calls `t_post_service("corvus", 6)` → kernel `sys_post_service_for_proc` mints a `KObj_Srv` listener in corvus's handle table |
| `ConnAccept(client, server)` | client calls `SYS_SRV_CONNECT` → kernel mints SrvConn + drives handshake + returns client `KObj_Srv` handle; server's `t_srv_accept(listener)` returns the server-endpoint `KObj_Spoor` |
| `ConnTeardown(c)` | client closes its handle → `handle_close` → `srvconn_teardown` (idempotent LIVE→TORN, EOF both rings, wake) |
| `ConnOpIdentityIsKernelTruth` | corvus calls `t_srv_peer(conn_handle)` to read the kernel-stamped `TSrvPeerInfo` (immutable identity by-value; live caps via `proc_caps_by_stripes`) |
| `AuthSuccess(p, u)` | `usr/corvus/src/main.rs::handle_auth` (unchanged from -c) |
| `SessionClose(p)` | `usr/corvus/src/main.rs::handle_session_close` (unchanged from -c) |

| Spec invariant | Source enforcement |
|---|---|
| `ServicePosterEverMarked` (kernel side; spec C-21) | `sys_post_service_for_proc` checks `proc_may_post_service(p)`; -EPERM otherwise |
| `SrvHandleNonTransferable` (specs/handles.tla `SrvKObjs`) | `handle_dup` rejects every `!kobj_kind_is_transferable` kind including KOBJ_SRV |
| `SessionUserImmutable` (C-3) | `Session::user` is set only at `session_install`; one mutation per AUTH success |

---

## Tests

The boot path IS the regression for b3b. `tools/test.sh` watches for "Thylacine boot OK", gated on joey's clean exit, which in turn requires:

1. `corvus: starting (P5-corvus-srv-impl-b3b)` line.
2. `corvus: ready (hardening applied; serving /srv/corvus)` line.
3. `joey: connected /srv/corvus/ctl fd=N` line.
4. The 9-verb test sequence (`USER_CREATE michael` + `USER_CREATE susan` + `AUTH(wrong) → BadAuth` + `AUTH(ok)` + `WRAP` + `UNWRAP × 5` + `SESSION_CLOSE`), each emitting its own success line.
5. `joey: Q11-negative refused BadFormat + tore down conn (expected)`.
6. `joey: reconnect AUTH + SESSION_CLOSE ok (Q11 recovery verified)`.
7. `joey: corvus-d hybrid-PKE round-trip verified via /srv/corvus (b3b)`.

A regression at any step prints a tagged failure marker and joey returns non-zero, the kernel extincts on joey's non-zero exit, the boot test fails.

No new kernel-internal unit tests added — every kernel surface b3b touches (devsrv + srvconn + handle + syscall + poll) is already covered by the suites that landed at P5-corvus-srv-impl-{a1,a2,a3a,a3b,a3c,b1,b2,b3a} and P5-poll-{a,b}. b3b is the integration that composes them. **Total kernel-test count unchanged at 511/511 PASS** × default + UBSan.

---

## Status

Implemented + green at the b3b commit + the P5-corvus-srv-impl audit close. Default + UBSan suites both pass 511/511; corvus's USER_CREATE × 2 + AUTH × 2 + WRAP + UNWRAP × 5 + SESSION_CLOSE + Q11 negative + reconnect round-trip runs end-to-end on every boot, all over the `/srv/corvus` transport.

The pipe-pair harness (corvus's fd 0/1 + joey's `t_spawn_full` with inherited fds) is **retired**. corvus no longer has fd 0 / fd 1 — its handle table at startup is empty; only `t_post_service`'s returned listener inhabits it. joey's `t_spawn_with_perms("corvus", ..., perm_flags=T_SPAWN_PERM_MAY_POST_SERVICE)` passes 0 inherited fds.

### Audit-close changes (P5-corvus-srv-impl audit)

- **F1 (P1) — deadline-bounded production path**. The kernel-side production-path `sys_srv_connect_for_proc` + the KOBJ_SRV arms of `sys_read_for_proc` / `sys_write_for_proc` now call `srvconn_set_client_deadline` before each blocking op. Two new constants in `<thylacine/srvconn.h>`: `SRVCONN_HANDSHAKE_DEADLINE_NS = 5s` (Tversion + Tattach + Twalk + Tlopen — kernel-only 9P shuffling, no corvus crypto on this path) and `SRVCONN_OP_DEADLINE_NS = 30s` (steady-state ops can stir Argon2id + AEGIS + ML-KEM on emulated targets). Pre-fix the production paths skipped the deadline-setting step the kernel-internal test correctly demonstrated, leaving `client_deadline_ns = 0` which `tsleep` reads as "no deadline" — a hung corvus wedged its peer indefinitely.
- **F3 (P2) — Q11 BadFormat actually tears down**. New per-Conn `tear_down_after_drain: bool` set by `try_dispatch_verb` on both the Q11 (unknown `protocol_version`) and the oversize-payload paths. After the BadFormat reply has been fully drained by the next Tread, `service_conn` returns `Ok(false)` and the conn closes. Pre-fix the reply was staged but no tear-down fired; the joey test passed only because joey explicitly closed the conn. STRATUM-API-V1.md Q11 contract (stream cannot be safely re-synced across a version mismatch) is now enforced from both sides.
- **F4 (P2) — fail-closed on `t_srv_peer` failure**. In `srv_server_loop` after `t_srv_accept`, corvus checks `t_srv_peer`'s return value. On non-zero it closes the just-accepted handle and continues — does NOT push a Conn with zero-initialized peer identity. Forward-compat for the admin verbs landing at P5-hostowner-b.
- **F9 (P3) — accumulator reset on overflow**. `dispatch_twrite` now clears `pending_request` on the bound-exceeded path, allowing recovery from a pathological client. Pre-fix the conn was wedged on every subsequent Twrite.

---

## Known caveats

- **Single-client v1.0 cap**: the kernel's `SRV_CONN_PER_PROC_MAX = 1` means joey can hold at most one open connection to corvus at a time. joey closes each connection before opening the next (the Q11 negative regression's reconnect after teardown explicitly tests this path). The corvus-side `MAX_CONNS = 8` and the loop's multi-connection structure are forward-compat for v1.x multi-peer.
- **Global SESSION at v1.0**: the session token + authenticated-user record is a `static mut SESSION: Option<Session>` global. With at most one connection at a time + at most one authenticated session in flight, this is sound. A multi-peer corvus (v1.x) lifts `SESSION` into a per-Conn `Option<Session>` so each peer gets its own. The corvus.tla `SessionTransfer` action is the formal hook for this.
- **/ctl message-oriented semantics**: Tread offsets are IGNORED. A client (other than the kernel-internal one) attempting to do random-access reads on `/ctl` would observe the message-queue semantic, not byte-stream. At v1.0 the only client is the kernel-internal `p9_client`; v1.x exposes /ctl to external 9P speakers and the offset semantic should be documented in the CORVUS-DESIGN.md namespace section.
- **No Tflush handler**: a Tflush from the client (the kernel-internal client doesn't issue Tflush at v1.0) would be unanswered. The connection would proceed but with the in-flight Tmsg's response unmatched at the client. The kernel-side client always waits for the response of every outstanding request synchronously (it's a single-flight client; `srvconn_drive_client_handshake` and `srvconn_client_read` / `srvconn_client_write` all block until reply). Tflush is needed if/when the client becomes pipelined.
- **t_spawn_with_perms perm-gate is one-way**: once stamped, `PROC_FLAG_MAY_POST_SERVICE` cannot be revoked. corvus retains the post-gate until proc exit. This matches the spec's `MarkMayPost` one-way semantic (joey's stamp on corvus is monotonic; rfork from corvus does NOT propagate it). The narrower revocation (corvus exits after first post-service) is a v1.x option.
- **Build-tooling trap**: `tools/build.sh disk` rebuilds the virtio-blk backing image (`build/disk.img`) but does NOT regenerate the ramfs cpio (`build/ramfs.cpio`). The cpio is the initrd QEMU loads at `-initrd`; if joey/corvus source changes are not reflected in the cpio, the kernel boots a stale binary. The canonical rebuild invocation is `tools/build.sh kernel` (which calls `build_userspace + build_ramfs + build_disk + build_kernel` in sequence). When iterating on joey or corvus, prefer `tools/build.sh kernel`. The bug class is named in `docs/phase5-status.md` Trip hazards.

---

## References

- `docs/CORVUS-DESIGN.md §6` — /srv/corvus namespace + connection layer + 9P-server semantics.
- `docs/CORVUS-DESIGN.md §6.4` — Binary verb-frame format (unchanged at b3b).
- `docs/CORVUS-DESIGN.md §10.2` — Transport rationale (why 9P, not raw pipe).
- `specs/corvus.tla` — Connection-layer actions (`ConnAccept`, `ConnTeardown`, `MarkMayPost`, `PostService`, `ConnOpIdentityIsKernelTruth`).
- `specs/handles.tla` — `SrvKObjs` partition (non-transferable + non-hardware).
- `docs/reference/66-corvus-server.md` — Verb wire format + session table (pipe-pair era, transport sections superseded by this doc).
- `docs/reference/68-corvus-crypto.md` — Argon2id + AEGIS-256 + USER_CREATE.
- `docs/reference/69-corvus-unwrap.md` — Hybrid PKE + WRAP + UNWRAP envelope.
- `docs/reference/70-devsrv.md` — Kernel `devsrv` Dev + `/srv` service registry + `SYS_POST_SERVICE`.
- `docs/reference/71-srvconn.md` — Per-connection SrvConn transport.
- `docs/reference/72-poll.md` — `SYS_POLL` mechanism (corvus's server-loop primitive).
- `docs/reference/73-sys-spawn-with-perms.md` — `SYS_SPAWN_WITH_PERMS` (joey grants the post-gate).
