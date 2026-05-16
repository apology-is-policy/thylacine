# Stratum → Thylacine — STRATUM-API-V1 Response

Stratum's point-by-point response to `thylacine/docs/STRATUM-API-V1.md`. For
each ask it states what shipped, the exact as-built surface (CLI / wire /
error codes), where the implementation deviates from the spec and *why*, how
it is verified, and what remains on Thylacine's side.

Pairs with `THYLACINE-V1-PLAN.md` (the Stratum-side execution plan) and
`STRATUM-API-V1.md` (the tree-local mirror of your spec). This document lives
in Stratum's tree; copying it into the Thylacine repo is a coordination step
for you / a Thylacine agent per §11 of the spec — Stratum does not write into
the Thylacine tree unilaterally.

- **Stratum tip at time of writing**: `6bfdbbc`
- **Date**: 2026-05-16
- **Test state**: ctest 62/62; Rust e2e 33/33 + concurrent_ctl 2/2
- **Audit rounds covering this work**: R137–R148

---

## 0. Summary

All five v1.0 hard-dependency asks (A1–A5) are **delivered and audited**. A6
is **not started**, by agreement — your §8 scopes it to v1.x. Two asks landed
with deliberate, documented deviations (A2 error model; A5 rollback
mechanism); one open cross-repo seam gates *live* corvus interop (the Q11
4-byte wire — see §9). Everything else is interop-ready against the Stratum
side.

| Ask | v1.0 dep | Status | As-built home | Audit |
|---|---|---|---|---|
| **A1** pool serial binding | hard | **Delivered** | `super.h::ub_pool_serial[16]`; `stm_fs_mount`; `stratumd --bind-pool-serial` | R137 |
| **A2** multi-stratumd-per-pool | hard | **Delivered** — coordinator model | `stratumd --role coordinator\|client` | R139–R142 |
| **A3** corvus UNWRAP + WRAP wire | hard | **Delivered** | `corvus_client/`; `keyschema` CORVUS slots; mount + provision paths | R144, R145, R148 |
| **A4** session-close → DEK eviction | hard | **Delivered** | `cmd/stratumd/corvus_notify.{c,h}` | R138 |
| **A5** rollback compromise marking | soft | **Delivered** — marker real; rollback *mechanism* stubbed | `/ctl/` mark/unmark/rollback verbs | R146 |
| **A6** per-dataset key rotation | v1.x | **Not started** (as scoped) | — | — |

Stratum followed the implementation order you recommended (A1 → A4 → A2 → A3 →
A5), spec-first for the invariant-bearing asks (A2 / A4 / A5 each got a TLA+
model before code), one adversarial audit round per chunk.

---

## 1. A1 — Pool serial binding — **DELIVERED**

**You asked (§3)**: a 16-byte `pool_serial` superblock field, CSPRNG, written
once at create; `stratumd --bind-pool-serial <32-hex>`; the four-case
comparison matrix; `STM_ESERIAL`; an unbound (all-zero) carve-out for
pre-existing pools.

**Delivered** — commit `6fb4d79`, audit close `f89e5a9` (R137).

- **`ub_pool_serial[16]`** carved from the head of `ub_reserved` at superblock
  offset 3504 (`ub_reserved` shrank 560 → 544 bytes). **No on-disk
  format-version bump and no feature flag** — pre-Thylacine pools have an
  all-zero `ub_reserved`, so your "all-zero = unbound" semantic works with
  zero migration cost. (This is stronger than the §9.1 feature-flag proposal:
  a binary that predates the field still mounts pre-Thylacine pools, and
  Thylacine pools simply have a non-zero value there.)
- Covered by the existing `ub_csum` (xxHash3 over the superblock) — tamper
  detection on the field is **inherited**, no new tamper surface.
- **Written at format time** by `stm_fs_format`: a caller-supplied non-zero
  serial is used verbatim (your installer's explicit-supply mode, §3.5); an
  all-zero caller value triggers CSPRNG generation of 16 bytes.
- **Compared at mount** in `stm_fs_mount`, immediately after the superblock
  scan and **before** pool / sync / allocator construction — the
  read-and-compare-before-any-data posture your §3.8 asked for.
- The §3.3 four-case matrix is implemented exactly: both-zero → mount + a
  one-line warning; disk-zero & arg-nonzero → `STM_ESERIAL`; both-nonzero-equal
  → mount; both-nonzero-unequal → `STM_ESERIAL`. The "tampered serial" and
  "all-zero arg" rows of §3.6 collapse into those cases.
- **Error**: `STM_ESERIAL = -209`.
- **CLI**: `stratumd --bind-pool-serial <32-hex>` → `stm_fs_mount_opts.expected_pool_serial`
  (absent = read but no compare).

**Deviations / notes**

- The error code is `-209`, not the `-208` an early draft of our plan named —
  `-208` was already assigned. Full registry in §8.
- **Q1 is only partially addressed.** The audit-log half (Q3) ships: an
  `STM_ESERIAL` mismatch logs the **full** serial to `/ctl/events`
  (admin-readable) and the first 8 hex chars to stderr. The read-only
  `/ctl/pools/<uuid>/serial-info` admin kind is **forward-noted as TLY-A1b and
  not yet built.** If you want that diagnostic surface in v1.0, say so — it is
  a small chunk.
- Q2 (`migrate-serial` tool) — deferred to v1.x as you recommended; pre-existing
  pools work via the unbound carve-out.

**Verification**: 13 tests — the §3.6 matrix in `test_super.c` / `test_fs.c`.
R137 audit clean (2 P2 fixed). Spec-first verdict: not needed (the invariant
`disk.ub_pool_serial == caller.expected` is a bytewise compare).

**Your side**: invoke `stratumd-system` with `--bind-pool-serial X`; derive
`boot_key` from the same `pool_serial` per §3.5. The explicit-supply mode is
the supported installer path (your installer generates the serial and passes
it to both ends).

---

## 2. A2 — Multi-stratumd-per-pool — **DELIVERED** (coordinator model)

**You asked (§4)**: per-stratumd `--datasets-allowed` scoping (glob `*` / `**`);
pool-level coordination for allocator / journal; crash isolation across
processes; `STM_EDATASETNOTALLOWED` + `STM_EPOOLLOCKED`.

**Delivered** — design `724acd6`, spec `d1b6384`, impl-1..4
(`ad55173` / `9d954ce` / `0c16d91` / `231d42d`), audits R139–R142.

§4.3 left the coordination model to Stratum. We picked **Option A — the
pool-level coordinator daemon** (our plan §4):

- **Coordinator** — `stratumd --role coordinator` opens the pool device
  *exclusively* and owns allocator / sync / journal / `/ctl/`. It is the sole
  writer to the device. Gated by `--user-policy` (a uid → dataset-pattern
  table).
- **Per-user client** — `stratumd --role client --coordinator-socket <path>
  --datasets-allowed <pattern> [--datasets-allowed <pattern> ...]`. The client
  **mounts nothing itself**; it is a raw 9P-frame proxy that forwards a
  kernel's 9P2000.L connection to the coordinator, intercepting `Tattach` to
  validate `aname` against the pattern set.
- `--datasets-allowed` is repeatable; glob `*` (one level) / `**` (recursive)
  per your Q4. Enforcement is at **Tattach time** (Q5): a non-matching `aname`
  is refused **before** the frame reaches the coordinator. The proxy is the
  authoritative gate; the coordinator re-checks against `--user-policy` —
  bilateral.
- An empty `--datasets-allowed` under `--role client` is **refused at startup**
  unless `--allow-empty-datasets-allowed` is given (the opt-in for tests /
  non-Thylacine single-stratumd use). Without this gate a forgotten flag
  silently became an open relay — closed at R140.
- **Bilateral SO_PEERCRED** (impl-3, `0c16d91`): the coordinator refuses an
  unknown-uid peer *before* spawning a worker; the client can pin the
  coordinator's uid via `--coordinator-uid <N>` (defends against socket-bind
  impersonation in a shared-writable socket directory). Fail-closed on
  peer-credential resolution failure.

**Error-model deviation — please read.** Stratum did **not** add
`STM_EDATASETNOTALLOWED` or `STM_EPOOLLOCKED`:

- A dataset-not-allowed refusal surfaces as **`Rlerror(EACCES)` on the 9P wire
  at Tattach**. That is the dialect the kernel 9P client already speaks; a
  Stratum-internal status code would never reach it. Functionally identical to
  your `STM_EDATASETNOTALLOWED` (hard failure, no retry, caller misconfigured
  patterns) — delivered in the consumer's wire vocabulary.
- `STM_EPOOLLOCKED` has **no failure mode to represent.** Option A's
  coordinator is the *single* device owner — there is no multi-writer
  allocator / journal contention class, so there is never a transient
  pool-lock to back off on. The contention that `STM_EPOOLLOCKED` modeled is
  *designed out*, not surfaced as a retryable error.

**Crash isolation (§4.4)**: each proxied connection is served by a detached
worker; a client crash closes its sockets and the coordinator reaps that
connection's in-flight work without disturbing other clients. impl-4
(`231d42d`) added a three-scenario crash-recovery sweep.

**Q6** (multi-stratumd audit log): resolved by Option A — the coordinator owns
`/ctl/`, so one process emits `/ctl/events`. **Q7** (per-stratumd resource
caps): deferred to your OS layer (cgroups / Proc handles).

**Verification**: `specs/multi_stratumd.tla` with invariants
`ClientAdmitsDataset`, `CrossClientIsolation`, `TattachPatternEnforced`,
`ClientCrashIsolation` + 4 cfgs (3 buggy); R139–R142 (R139's spec-first pass
caught + fixed 2 P0s before they could ship). **A2-impl-5 — the §4.6 72-hour
stress run — is deferred**: it needs real-Linux disk compute and has not been
run. It is queued, not cancelled (see §9).

**Your side**: spawn one `--role coordinator` per pool; spawn each per-user
stratumd as `--role client --coordinator-socket … --datasets-allowed
users/<u> --datasets-allowed users/<u>/**`. Killing a per-user client evicts
that user structurally — exactly the §4.1 mitigation of F16.

---

## 3. A3 — corvus UNWRAP + WRAP wire — **DELIVERED**

**You asked (§5)**: a corvus key-agent client speaking the `CRVS` binary wire;
**UNWRAP** (verb 4) at mount; **WRAP** (verb 10) at provisioning; the §5.5
error matrix; `--corvus-session-token-file`; the Q11 version byte.

A3 landed in four movements, because **§5.3 step 1 — "stratumd reads the
dataset metadata; finds the `wrapped_dek + key_id`" — assumed a per-dataset
wrapped-DEK store that Stratum did not have.** v2 derived DEKs from a master
key + `dataset_id`; there was nowhere to put a corvus envelope. Stratum built
that store.

### 3.1 — UNWRAP codec + transport

Commits `1eb7480`, `58a7253`; audit `b948cf8` (R144).

New module `v2/src/corvus_client/` — pure C, a **sibling to (not a replacement
for)** the existing janus client. Frame encode/decode with strict
bound-checks; a token loader (33-byte exact length, `mlock` + `MADV_DONTDUMP`,
`O_CLOEXEC`); a bounded transport (non-blocking connect + poll timeout,
`SO_*TIMEO` steady-state) so a wedged corvus cannot hang stratumd; a retry
wrapper with the Q9 backoff schedule **[100, 500, 2000] ms**, retries clamped
to 3.

### 3.2 — the per-dataset wrapped-DEK store

Keyslot series — design `bb86875`, spec `6d4cc80`, impl `2d68c08` / `2ff2f09`,
audit `cfb8048` (R145).

Rather than invent a new structure, Stratum **reused its pool-global
`keyschema`** — already a LUKS-style `(dataset_id, key_id)` slot table with
AEAD-wrapped blobs and a rotation `state` field. A `wrapper_identity` byte
(`PASSPHRASE` / `JANUS` / `CORVUS`) routes each slot to its unwrapper; a
`CORVUS` slot's blob is the corvus envelope. On-disk format version
`STM_UB_VERSION` 26 → 27.

### 3.3 — mount-time UNWRAP wiring

Commit `2ff2f09`; audit `cfb8048` (R145).

At `stm_sync_open`, a CURRENT `CORVUS` keyslot routes through
`stm_corvus_unwrap`. The unwrap runs **inside mount**, so a slot that will not
unwrap aborts the mount with **no data block ever served** — the
`key_schema.tla::MountResolvesKeyBeforeData` invariant; fail-fast, never a
half-mount, your §5.3 contract. CLI: `--corvus-socket`,
`--corvus-session-token-file`. The 33-byte token is loaded into an `mlock`'d
buffer for the mount window and zeroed immediately afterward.

R145 added a **token-file mode gate**: the session-token file must be
owner-only `0400` / `0600` or the mount refuses `STM_EACCES` — it is a bearer
credential, and this enforces your §10.3 commitment from the Stratum side.

### 3.4 — WRAP provisioning path

Commits `82e231b` / `8d6fe6c` / `e27db75` / `c0fc8ff` / `bfc3a4d`; audit
`6bfdbbc` (R148).

The path that *produces* a corvus envelope (your §5.10):

- **WRAP codec** (verb 10) in `corvus_client`. The WRAP *request* carries the
  plaintext 32-byte DEK — a confidentiality exposure UNWRAP never had. The
  encoded request frame (token + DEK) is scrubbed via a non-elidable
  `volatile`-write loop on every exit path, before `free`.
- `stm_sync_add_dataset_key_corvus` — CSPRNG-generate a DEK → `stm_corvus_wrap`
  → store a `CORVUS` keyslot → insert into the DEK map.
- `stm_fs_create_dataset_corvus` — composes dataset-create + the sync call
  under one lock, with rollback on WRAP failure.
- **One-shot provisioning mode**: `stratumd <fs> --provision-corvus-dataset
  <name> --corvus-dataset-path <path> [--provision-parent <id>]
  --corvus-socket … --corvus-session-token-file …` → mount, create the
  corvus-encrypted dataset, init its root, unmount, exit. Binds no listening
  socket.
- **Dataset binding is the UTF-8 path string** per §5.10 — the `CORVUS` slot
  records `corvus_dataset_path`; `STM_UB_VERSION` 27 → 28; mount-time UNWRAP
  sends the *stored* path so the AD it presents matches the AD WRAP bound
  (`key_schema.tla::UnwrapUsesWrapBinding`). There is no decimal `dataset_id`
  on the corvus wire.

### 3.5 — error matrix, Q-resolutions, the open seam

Error matrix (§5.5), as-built — corvus status → Stratum code:

| corvus status | Stratum code | retry? |
|---|---|---|
| 1 BadAuth | `STM_ECORVUSAUTH` | fatal |
| 2 PermissionDenied | `STM_ECORVUSPERM` | fatal |
| 3 NotFound | `STM_ECORVUSNOTFOUND` | fatal |
| 4 RateLimited | `STM_ECORVUSRATELIMITED` | retried (Q9 backoff) |
| 5 BadFormat | `STM_ECORVUSBADFORMAT` | fatal |
| 6 InternalError | `STM_ECORVUSINTERNAL` | retried once; persistent → fatal |

- **Q8** — token is opaque to Stratum: it reads N bytes from the file and
  replays them unparsed.
- **Q10** — no separate health-check verb; the UNWRAP request *is* the check.
- **Q11 — the 4-byte header — is the single open cross-repo seam.** Stratum
  took Q11 (`protocol_version u8 = 1` after `verb_id`). Stratum's UNWRAP **and**
  WRAP encoders therefore emit a **4-byte request header**. The corvus parser
  shipped today is **3-byte**. Live Stratum ↔ corvus interop is gated on
  corvus landing the 3 → 4-byte request-decoder change — Thylacine-side work.
  Until then Stratum exercises the wire only against a Stratum-side
  verb-aware fake corvus. See §7 for byte-exact frames and §9.

**Verification**: `corvus_client` 72/72 unit tests; `test_corvus_mount.c`
mount e2e + `test_corvus_provision.c` provisioning e2e against a verb-aware
fake corvus; `key_schema.tla` extended with `MountResolvesKeyBeforeData` +
`UnwrapUsesWrapBinding` + 2 buggy cfgs; R144 + R145 + R148. R144 found and
fixed a P1 — a session-token leak into freed heap on the dial-failure path.

**Your side**: (1) land the corvus 3 → 4-byte request decoder (Q11); (2)
confirm whether corvus serves WRAP and UNWRAP on one socket or two — Stratum
dials a single `--corvus-socket` for both today (R148 forward-note); (3)
install the session-token file mode `0400`, owner = the stratumd user.

---

## 4. A4 — Session-close → DEK eviction — **DELIVERED**

**You asked (§6)**: a `/srv/corvus/notify` consumer; the `SESSION_CLOSED`
frame; on receipt, evict the DEK and unmount; strict / tolerant policy (Q12).

**Delivered** — commit `f37c518`, audit close `eea5ff0` (R138).

New module `v2/src/cmd/stratumd/corvus_notify.{c,h}` — an optional consumer
thread inside stratumd, spawned when `--corvus-user <name>` is set. It
subscribes to the notify socket, parses frames with strict bounds
(`payload_len + 3 == buf_len` exact equality — truncated *and*
oversize-trailing both refused; `user_len ≤ 32`; control-byte refusal on the
`user` string per the line-injection doctrine), and on a matching
`SESSION_CLOSED { user == corvus_user }` sets the daemon's `stop_flag`.

**Eviction discipline (§6.2 / Q14)** — the consumer **only sets the flag**; it
never unmounts itself. The daemon's main thread observes the flag, drains the
accept loops, and unmounts in the documented order, which zeroes the DEK via
`stm_sync_close`. This two-stage "consumer → flag → main-thread tears down"
separation preserves the AEAD nonce-uniqueness invariant
(`disk ss_gen > fs->gen`) under crash-equivalent shutdown. For the typical
Thylacine per-user stratumd (serves exactly one user), `SESSION_CLOSED` →
`stop_flag` → clean unmount → process exit **is** your §6.2 step 3.

`notify_kind` 2 (`USER_KEY_ROTATED`) and 3 (`ADMIN_FORCE_EVICT`) parse without
error but are treated as no-ops — forward-compatible; v1.x wires their
semantics.

**Strict / tolerant (Q12)**: `--corvus-notify-mode {strict,tolerant}` (default
**tolerant**); `--corvus-notify-timeout <seconds>` (default **30**). On
notify-socket EOF, tolerant mode opens a reconnect window; on expiry it falls
back to strict (evict + unmount + exit). Error `STM_ECORVUSGONE = -210`.

**CLI**: `--corvus-user`, `--corvus-notify-socket` (default
`/srv/corvus/notify`), `--corvus-notify-mode`, `--corvus-notify-timeout`.
`--corvus-user` absent = the consumer is disabled (back-compat for
non-Thylacine deployments).

**Verification**: 18 tests against a faked corvus emitter; soft-spec
`specs/eviction.tla` (4 invariants — `DataSealedOnExit`, `StopBeforeUnmount`,
`InFlightWritesDrainBeforeUnmount`, `ConsumerAlwaysEscalatesOnPersistedEOF` —
3 buggy cfgs); R138 (1 P1 fixed — a token-on-stack wipe).

**Your side**: emit `SESSION_CLOSED` before `login` returns control (§10.4);
invoke each per-user stratumd with `--corvus-user <name>`.

---

## 5. A5 — Snapshot rollback compromise marking — **DELIVERED** (marker real; rollback mechanism stubbed)

**You asked (§7)**: mark snapshots rollback-compromised; a persistent marker;
`stratum rollback` consults the marker and refuses (or warns + `--force`); a
list verb; a corvus-invokable programmatic path; a corvus-specific principal
(Q16).

### Premise correction — read this first

§7 assumes `stratum rollback` exists. **In Stratum v2 it does not.** v2 shipped
the snapshot *index* only: snapshots record an `extent_txg` generation number
for birth-txg delete accounting, but v2 has **no per-dataset metadata trees**,
so there is no recoverable past tree to roll back *to*. Real rollback is a
foundational re-architecture — the post-Thylacine "Phase 9.7" on Stratum's
roadmap.

So A5 shipped **marker-first**: the compromise marker, the mark / unmark /
rollback verb surface, the corvus principal, and the marker-consultation gate
are **all real, wired, and tested.** The rollback verb's *data mutation* is a
stub.

### Delivered

Design `58946f0` / `7e94630`, spec `12d10ab`, impl `2829c59` / `86b2f53` /
`e74d387` / `9e81643`, docs `8326eeb`, audit `58a1387` (R146).

- **Marker** — `STM_SNAP_FLAG_ROLLBACK_COMPROMISED`, a bit in the existing
  `stm_snapshot_entry.flags`. No on-disk format change, no UB-version bump.
  Persistent in the snapshot index (survives reboot / export-import — your
  §7.6 "Survival" test).
- **Reason text** — *not* stored in a new on-disk structure. It flows to the
  `/ctl/events` audit log; the persistent state is the flag bit alone.
- **Verb-shape deviation** — §7.2 / §7.5 proposed a `stratum-admin dataset
  mark-rollback-compromised` CLI plus a JSON frame on `/srv/stratum-ctl/admin`.
  Stratum implemented these as **write verbs on the existing `/ctl/` synthetic
  filesystem** — Stratum's established admin surface, line-oriented (no JSON).
  corvus (or an operator) writes one short line to a file in the `/ctl/` tree:
  - `/ctl/datasets/<id>/mark-snapshot-compromised` — write `<sid>`
  - `/ctl/datasets/<id>/unmark-snapshot-compromised` — write `force <sid>` (the
    `force` token is mandatory — clearing a compromise flag is itself
    sensitive; Q15)
  - `/ctl/datasets/<id>/rollback-snapshot` — write `<sid>` or `force <sid>`
  - the `/ctl/datasets/<id>/snapshots/<sid>` info kind gained a
    `compromised: yes|no` line — your §7.4 listing.
- **Corvus principal (Q16)** — `stratumd --corvus-admin-uid <uid>` names a
  second, narrower principal. That uid may write `mark-snapshot-compromised`
  **only**; `unmark`, `rollback`, and every other admin verb stay
  strict-operator-admin. Rationale: corvus must *raise* the F13 / C-13 alarm
  autonomously, but only the operator may clear it or force through it.
- **Consultation gate** — `rollback-snapshot` on a marked snapshot without
  `force` returns **`STM_ECOMPROMISED = -217`** *before* the mechanism runs.
  The `snapshot.tla::RollbackBlockedIffCompromised` invariant is genuinely
  exercised: the lookup + check happen under one exclusive lock hold, so a
  racing `unmark` cannot tear it.
- **The stub** — the rollback *mechanism* itself returns `STM_ENOTSUPPORTED`.
  v2 cannot roll a dataset back yet, marked or not. When Phase 9.7 lands
  per-dataset trees, only the stub body is replaced — the `/ctl/` verb
  surface, admin gate, marker consultation, and `force`-prefix parsing are all
  already in place.

### What this means for your F13 / C-13 closure

The **defence** you specified — "a compromised snapshot cannot be silently
rolled into" — is **fully in place**: a marked snapshot's rollback is refused
with `STM_ECOMPROMISED`, and an unmarked snapshot's rollback also cannot
proceed (the mechanism is stubbed). The marker is honored end to end. What is
not yet possible is rollback *at all*; when it becomes possible it will
already be marker-aware. corvus's own discipline (refuse to re-attach to a
known-compromised wrap chain) is, as your §7.3 notes, enforced corvus-side
regardless.

**Verification**: `snapshot.tla` rollback-admission extension + buggy cfg; ~9
tests inline; R146 (2 P2 fixed — mark/unmark now commit only on a real change
and revert the in-RAM toggle if the commit fails; the dataset_id is checked
against the qid path so a snapshot from a different dataset returns
`STM_ENOENT`).

**Your side**: run corvus under the uid you pass as `--corvus-admin-uid`; on a
passphrase rotation, write each affected `<sid>` to
`/ctl/datasets/<id>/mark-snapshot-compromised`, synchronously with the
rotation (§10.5 — the mark verb commits synchronously, so it is durable on
verb return). `/ctl/` must be enabled on the coordinator (it is opt-in via the
coordinator's ctl-listen socket).

---

## 6. A6 — Per-dataset key rotation — **NOT STARTED** (v1.x, as scoped)

§8 of your spec explicitly scopes A6 to v1.x ("No v1.0 deliverable"). Stratum
has not started it, by agreement. Two things worth knowing for when it lands:

- **The substrate is rotation-ready.** `keyschema` already carries a per-slot
  rotation `state` field and the `MonotonicKeyIds` / `RotationAtomic`
  invariants in `key_schema.tla`; the `CORVUS` slot's `key_id` is AD-bound, so
  the envelope format already supports rekey.
- **Per your §5.10**, corvus rotating a *user's* KEK (passphrase change) needs
  **no** dataset-slot re-WRAP — existing envelopes stay valid. Only per-dataset
  DEK *rekeying* (the `ROTATE_KEY` verb) is A6, and the `USER_KEY_ROTATED`
  notify (`notify_kind = 2`) stays a parse-tolerated no-op until then. Q17 /
  Q18 are deferred jointly.

---

## 7. The corvus wire — exact as-built frames

All multi-byte integers little-endian. **Request** verbs (Stratum → corvus)
carry the Q11 `protocol_version` byte → a **4-byte header**. **Responses** and
the **notify** frame (corvus → Stratum) carry no version byte — Stratum
consumes them exactly as your §5.2 / §6.2 specify.

### UNWRAP request — verb 4 (4-byte header)

```
off      field             type             notes
0        verb_id           u8               = 4
1        protocol_version  u8               = 1            ← Q11
2..4     payload_len       u16 LE           byte count after this field
4..37    token             u8[33]           "s" + 32 hex
37       dataset_len       u8
38..     dataset           u8[dataset_len]  utf-8, e.g. "users/michael"
38+dl..  key_id            u64 LE
46+dl..  wrapped_len       u16 LE
48+dl..  wrapped           u8[wrapped_len]  opaque envelope, stored verbatim
```

### WRAP request — verb 10 (4-byte header)

```
off      field             type             notes
0        verb_id           u8               = 10
1        protocol_version  u8               = 1            ← Q11
2..4     payload_len       u16 LE
4..37    token             u8[33]
37       dataset_len       u8
38..     dataset           u8[dataset_len]  utf-8 (the §5.10 binding identity)
38+dl..  key_id            u64 LE
46+dl..  dek_len           u16 LE           = 32
48+dl..  dek               u8[32]           plaintext DEK; scrubbed from the
                                            frame after send
```

### UNWRAP / WRAP response (3-byte header — no version byte)

```
off      field             type             notes
0        status            u8               0 OK · 1 BadAuth · 2 PermDenied
                                             3 NotFound · 4 RateLimited
                                             5 BadFormat · 6 InternalError
1..3     payload_len       u16 LE
3..      payload           u8[payload_len]  OK: UNWRAP → 32-byte DEK,
                                            WRAP → envelope; non-OK → 0 bytes
```

Stratum accepts a WRAP envelope in `(0, STM_CORVUS_ENVELOPE_MAX = 1280]` — that
is ≥ your stated 1217-byte size; the envelope is opaque, so there is **no hard
`== 1217` check** (a future envelope-size change does not break Stratum).

### SESSION_CLOSED notify frame — corvus → Stratum (3-byte header)

```
off      field             type             notes
0        notify_kind       u8               = 1 (SESSION_CLOSED)
1..3     payload_len       u16 LE
3..36    token             u8[33]           the closed session's token
36       user_len          u8
37..     user              u8[user_len]     utf-8
```

Consumed exactly as §6.2 specifies. `notify_kind` 2 / 3 parse but are no-ops
today.

---

## 8. Error code registry

Codes Stratum added for Thylacine v1.0 (`v2/include/stratum/types.h`):

| Code | Value | Ask | Returned when | Thylacine reads it as |
|---|---|---|---|---|
| `STM_ESERIAL` | -209 | A1 | on-disk `pool_serial` ≠ `--bind-pool-serial` | halt → recovery (joey, §3.4) |
| `STM_ECORVUSGONE` | -210 | A4 | notify socket EOF + tolerant timeout expired | corvus is down; stratumd has already evicted + exited |
| `STM_ECORVUSAUTH` | -211 | A3 | corvus UNWRAP → BadAuth | fatal — bad / expired token |
| `STM_ECORVUSPERM` | -212 | A3 | corvus UNWRAP → PermissionDenied | fatal — session ≠ dataset owner |
| `STM_ECORVUSNOTFOUND` | -213 | A3 | corvus UNWRAP → NotFound | fatal — corvus has no record |
| `STM_ECORVUSBADFORMAT` | -214 | A3 | corvus UNWRAP → BadFormat | fatal — wire bug |
| `STM_ECORVUSINTERNAL` | -215 | A3 | corvus UNWRAP → InternalError | retried; persistent → fatal |
| `STM_ECORVUSRATELIMITED` | -216 | A3 | corvus UNWRAP → RateLimited | retried with backoff |
| `STM_ECOMPROMISED` | -217 | A5 | rollback of a marked snapshot without `force` | expected refusal — the F13 defence firing |

**Codes the spec named that Stratum deliberately did not add** — see §2:

- `STM_EDATASETNOTALLOWED` — a dataset refusal is surfaced as wire-level
  `Rlerror(EACCES)` at Tattach (the dialect the kernel 9P client speaks).
- `STM_EPOOLLOCKED` — no contention class exists under the Option A
  single-writer coordinator; nothing to represent.

The pre-existing `STM_EACCES` is reused for the A3 token-file mode gate (token
file not owner-only `0400` / `0600` → mount refused).

---

## 9. Open cross-repo items

What is *not* closed, and on whose side:

1. **corvus 3 → 4-byte request decoder (Q11)** — *Thylacine-side.* This is the
   one hard blocker on **live** corvus interop. Stratum's UNWRAP + WRAP
   encoders emit the 4-byte header now; corvus must accept it. Until then
   interop is exercised only against a Stratum-side fake corvus.
2. **WRAP socket topology** — *needs a decision.* Does corvus serve WRAP and
   UNWRAP on one socket or two? Stratum dials a single `--corvus-socket` for
   both. If two, Stratum adds a second flag — small change, just confirm.
3. **Stratum Phase 9.7 — real snapshot rollback** — *Stratum-side, not yours.*
   Until it lands, `/ctl/.../rollback-snapshot` returns `STM_ENOTSUPPORTED`.
   The A5 marker, gate, and principal are unaffected and fully functional.
4. **A2-impl-5 — the §4.6 72-hour stress run** — *Stratum-side, deferred.*
   Needs real-Linux disk compute; queued, not cancelled.
5. **`/ctl/pools/<uuid>/serial-info` (Q1 diagnostic surface)** — *Stratum-side,
   forward-noted as TLY-A1b, not built.* A small chunk if Thylacine wants it in
   v1.0; the Q3 audit-log half already ships.
6. **A6 — per-dataset key rotation** — v1.x by your scoping; coordinate the
   `ROTATE_KEY` API contract when corvus's verb lands.

---

## 10. Bilateral contract — what Stratum's implementation expects of you (§10)

Stratum's code depends on the §10 commitments. Concretely:

- **§10.3 token-file mode** — Stratum *enforces* it. R145's mode gate refuses a
  session-token file that is not owner-only `0400` / `0600`. A `0444` token
  file fails the mount with `STM_EACCES`.
- **§10.3 corvus owns the envelope** — Stratum stores the `wrapped` blob (and
  the WRAP-response envelope) verbatim and never parses it.
  `STM_CORVUS_ENVELOPE_MAX = 1280` ≥ your 1217.
- **§10.3 token opacity** — Stratum reads the token bytes and replays them
  unparsed (Q8); it never inspects or logs them.
- **§10.4 `SESSION_CLOSED` before `login` returns** — Stratum's sub-second
  unmount budget assumes this ordering; the notify drives the eviction.
- **§10.5 mark synchronously with rotation** — Stratum's
  `mark-snapshot-compromised` verb commits synchronously, so it is durable on
  verb return; your "rotation incomplete until the marker call succeeds"
  contract is satisfiable.
- **§10.1 `pool_serial` is the binding anchor** — Stratum never mutates
  `ub_pool_serial` after format.

---

## 11. Integration checklist for Thylacine

Concrete steps, by ask:

**A1 / boot** — generate `pool_serial` in the installer; create the pool with
that explicit serial; derive `boot_key` from it; invoke `stratumd-system` with
`--bind-pool-serial X`.

**A2 / multi-stratumd** — run one `stratumd --role coordinator --user-policy …`
per pool; run each per-user `stratumd --role client --coordinator-socket …
--datasets-allowed users/<u> --datasets-allowed users/<u>/**`; expect
`Rlerror(EACCES)` at Tattach for a disallowed dataset (not a custom code).

**A3 / encrypted datasets** — provision each user dataset with `stratumd
--provision-corvus-dataset <name> --corvus-dataset-path users/<name>
--corvus-socket … --corvus-session-token-file …`; at user login, the per-user
stratumd mounts with `--corvus-socket` + `--corvus-session-token-file` and
UNWRAP runs inside the mount; install the token file mode `0400`. **Land the
corvus 3 → 4-byte request decoder before live interop.**

**A4 / logout** — invoke each per-user stratumd with `--corvus-user <name>`;
emit `SESSION_CLOSED` from corvus before `login` returns; pick
`--corvus-notify-mode` (default tolerant / 30 s is the recommended Q12 answer).

**A5 / rollback defence** — run corvus under the uid passed as
`--corvus-admin-uid`; on passphrase rotation, write each affected `<sid>` to
`/ctl/datasets/<id>/mark-snapshot-compromised`; enable `/ctl/` on the
coordinator. Note that the rollback *action* is `STM_ENOTSUPPORTED` until
Stratum Phase 9.7 — the marker defence is fully live regardless.

**A6** — nothing in v1.0.

---

## 12. References

- `~/projects/thylacine/docs/STRATUM-API-V1.md` — your spec (the asks).
- `v2/docs/STRATUM-API-V1.md` — tree-local mirror of the same.
- `v2/docs/THYLACINE-V1-PLAN.md` — the Stratum-side execution plan + the
  negotiated Q1–Q18 answers (§0).
- `v2/docs/OS-INTEGRATION.md` — Stratum's OS-author manual; Thylacine clauses
  folded in per chunk.
- `v2/docs/thylacine-keyslot-design.md`, `thylacine-keyslot-wrap-design.md`,
  `thylacine-a5-design.md` — per-ask design notes.
- Specs: `v2/specs/{multi_stratumd,eviction,key_schema,snapshot}.tla`.
- Audit findings: `v2/.audit_r{137..148}_findings.md` (untracked by
  convention).

---

**Document state**: as-built response at Stratum tip `6bfdbbc` (2026-05-16).
A1–A5 delivered + audited; A6 not started (v1.x). The one hard blocker on live
interop is the Q11 4-byte corvus request decoder (§9 item 1).

The thylacine runs again.
