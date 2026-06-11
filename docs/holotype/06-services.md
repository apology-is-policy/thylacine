# HOLOTYPE RW-6 — Services (corvus FULL + joey/login STANDARD)

**Status: CLOSED.** 0 P0 / 0 P1 / 5 P2 / 14 P3. Four P2 fixed in-arc, one P2
deferred to #876 (v1.0-unreachable). NOT a dirty close (P1+P2 = 5 < 6; every fix
localized — no lock-order lift, no wait/wake protocol change, no primitive removal).

## Scope

| Surface | Tier | Files |
|---|---|---|
| corvus crypto core | FULL | `usr/lib/corvus-crypto/src/lib.rs` + `bip39_wordlist.rs` |
| corvus protocol/session/auth | FULL | `usr/corvus/src/main.rs` (9P dispatch, token, session, AUTH/UNWRAP/WRAP) |
| corvus identity/clearance DB + legate bridge | FULL | `usr/corvus/src/main.rs` (identity.db, clearance.db, ID allocator, CLEARANCE verbs) |
| joey (init) | STANDARD | `usr/joey/joey.c` |
| login | STANDARD | `usr/login/src/main.rs` |

A-5c recovery crypto was reviewed DELTA (audited at a5c; re-examined only deltas +
neighbor interaction). The wrap/AEGIS/argon2id/keypair CORE was re-prosecuted fresh.

## Method

4× Fable-max `holotype-reviewer` prosecutors, defensively framed (auditing our own
auth daemon for soundness defects; no exploit development, per the standing security
constraint), split R1 crypto core / R2 protocol+session+auth / R3 identity+clearance DB /
R4 joey+login. + an Opus self-audit on the two highest-stakes surfaces (crypto core +
session/auth), double-covered with R1+R2. All four reviewers self-reported
`claude-fable-5` at MODEL(start) AND MODEL(end) — no mid-run fallback on any.

The do-not-re-report preamble was the three prior closed lists (a5c, 828, a1b) + the
C-1..C-28 invariants (CORVUS-DESIGN §9). The convergences:
- **R1-F1 = self-SA-3** (unbounded Argon2 cost) — independent; R1's all-users-DoS +
  OOM-abort impact escalated my P3 to P2.
- **R2-F5 = R3-F2** (RECOVER_FAILS cap one short of the subject universe).
- **R2-F3 = self-SA-5** (transient secret-response/in_buf residue).
- self-SA-2 (aegis_unwrap tag-fail wipe) was a case R1 dismissed as benign — kept the
  narrow correct-key/corrupt-tag leg (the parallel-prosecution value).

## The picture

corvus is a mature surface — three prior clean closes (a5c recovery, 828 DEK-handoff,
a1b identity-DB). A fresh FULL pass found **no P0/P1**: the secret-handling spine
(AEAD/KEM soundness, AD domain separation, fresh salt+nonce, secret hygiene on the
happy paths, the C-7 ownership gate before crypto, the #829 session-ownership, the
constant-time token compare, the C-15 seeded gate) is sound. The five P2 are residual
hardening the prior closes did not reach, two of them direct *extensions* of closed
findings (R3-F1 is the a1b-F1 sibling on the unguarded UPG group-writer; R4-F1 is the
#828 A-F1 "scrub on every path" with two uncovered early-return legs).

The sharpest finding is **R2-F2**: two privileged console-gated verbs (ADMIN_ELEVATE,
RECOVER(system)) gated on the *cached* at-accept `conn.peer.console` while every sibling
verb re-queries `SYS_SRV_PEER` live — and the justifying "console is immutable" comment
is factually false (`proc_revoke_console_attached` flips the bit on a live Proc). A flat
C-22 violation resting on a false soundness premise. The kernel backstops ADMIN_ELEVATE
(it re-verifies console at `/cap/use` redemption) and the 24-word phrase backstops
RECOVER(system), so neither is a full break (→ P2), but the trusted-path defense-in-depth
failed and the fix makes corvus consistent + C-22-compliant.

## Findings + dispositions

See `docs/holotype/00-register.md` (HT06.*) for the full table. Summary:

**P2 fixed in-arc (4):**
- **F1** (R1-F1+SA-3) — `crvs_v1_unpack` envelope-bounds the on-disk Argon2 cost so a
  tampered/bit-rotted header cannot wedge (t_cost) or OOM-abort (m_cost) the daemon.
  Regression `crvs_unpack_rejects_out_of_envelope_cost`.
- **F2** (R2-F2) — `peer_live_info` helper; ADMIN_ELEVATE + RECOVER(system) re-query the
  live peer (console + stripes), fail-closed. E2E-validated.
- **F3** (R3-F1) — `handle_user_create` mirrors `handle_group_create`'s group-count guard
  before the UPG push (closes the a1b-F1 sibling boot-brick).
- **F4** (R4-F1) — login scrubs the passphrase + the session token on the two uncovered
  early-return legs.

**P2 deferred (1):**
- **F5** (R2-F1) — AUTH has no rate-limit (C-16 unenforced on AUTH). v1.0-UNREACHABLE
  (`session_active()` blocks AUTH during a live session; no-session windows hold only
  trusted Procs under the single-session model; activates with the multi-peer surface).
  The usable fix needs the time-decay that is **#876**'s defining scope; a naive
  lock-until-restart counter would regress login UX. #876 strengthened to name the
  AUTH-KDF-DoS + the multi-peer activation. Surfaced to user.

**P3 fixed in-arc (9):** F6 keygen early-return wipe; F7 aegis_wrap hard assert; F8
aegis_unwrap tag-fail wipe; F10 response/in_buf residue scrub; F11 recover cap MAX_USERS+1;
F12 clearance.db header `_Static_assert`; F13 identity_db_parse backend reject; F14 getty
respawn backoff; F15 joey relinquish-fail persist-reap; F16 wordlist sorted-strict const
assert.

**P3 doc/track (5):** F9 crate KEK residue (upstream-bounded, mlock-mitigated, documented);
F17 pipelining mis-pair (behavior-changing, tracked); F18 bootstrap re-entrant
(storage-trust-gated, confers no hostowner; documented + surfaced — the robust fix is a
deliberate provisioned-marker design choice); F19 AD-binds-version (v1.x forward-compat);
F20 login stack-token sibling (kernel-zeroed at teardown, ~nil; accepted for C-5 consistency).

## Validation

- `corvus-crypto` host tests: 13/13 (12 prior + `crvs_unpack_rejects_out_of_envelope_cost`,
  non-vacuous — pre-fix `from_bytes` accepted out-of-envelope cost). The wordlist
  sorted-strict const assert compiles (the wordlist IS strictly ascending).
- Default kernel build + boot E2E: **816/816 PASS, 0 EXTINCTION.** The full corvus surface
  is exercised and green: AUTH, UNWRAP (hybrid-PKE round-trip), **ADMIN_ELEVATE ok**,
  **RECOVER(system) ok** (both the verbs R2-F2 changed), RECOVER(user) via the login UX,
  CLEARANCE_LIST/ACTIVATE → legate (the legate-prover), the GROUP_CREATE pre-elevate gate,
  the `/sbin/login` E2E (michael authed, ut spawned stamped + reaped, session closed).
- SMP gate (default+UBSan × smp4/smp8): 0 corruption (userspace-only changes; the kernel is
  byte-identical to RW-5's gate-passed tree).

## Self-audit note

The two-surface self-audit (crypto core + session/auth) found 0 P0/P1/P2 itself — every
self-finding was P3 defense-in-depth (SA-2/SA-4/SA-5 + the SA-3 that R1 independently
escalated). It disproved SA-1 (the typed secret keys DO zeroize on drop — `zeroize`
features on) and confirmed the constant-time token compare, the C-7 ordering, and the #829
ownership gating still hold. The parallel prosecution earned its keep: the self-audit kept
SA-2 (a correct-key/corrupt-tag leak) that R1 dismissed as benign, and R1's impact analysis
escalated the shared cost-bound finding from my P3 to the merged P2.
