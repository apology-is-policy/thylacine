# Corvus — Thylacine's Key Agent (Design)

`corvus` is the v1.0 key agent for Thylacine. It authenticates users via passphrase, holds their wrapping keypairs in mlock'd RAM after a successful login, validates per-dataset DEK requests from per-user `stratumd` processes, and gates admin authority through the hostowner model. It is a Thylacine-native daemon — not a port — drawing on Plan 9's `factotum`, Stratum's `janus`, and contemporary per-user encryption models (FileVault, ZFS native encryption, fscrypt) for design influence, but designed from scratch for Thylacine's Spoor-based transport and capability-based privilege model.

The name is from the corvid genus: ravens are intelligent, long-memoried, gather and curate shiny things, and have a deep mythological lineage as bearers of knowledge (Huginn and Muninn, Odin's ravens for "thought" and "memory"). The semantic fit — the daemon hoards keys and returns them when asked — is direct. Australian ravens (*Corvus coronoides*, *C. mellori*, *C. bennetti*) live across the inland; the bird keeps the project's outback aesthetic without being uniquely-outback in the way bunyip would have been.

This document is binding scripture. Implementation deviations either update it first (with user signoff) or are reverted. It supplements (does not replace) ARCHITECTURE.md §5 (boot path), §11 (syscalls), §14 (Stratum mount lifecycle), §15 (capabilities), and §28 (invariants). Cross-references at the end.

This document was hardened against the findings of an Opus-4.7-max-effort adversarial security audit performed before scripture commit (audit transcript in the project notebook). Where the audit identified hard-to-undo lock-in risks, those resolutions are inline.

---

## 1. Mission

Thylacine boots into a Stratum-backed root. Some Stratum datasets are encrypted; corvus is what holds the secrets that unlock them. Without corvus, a logged-in user's home directory is just a sealed box of AEAD-encrypted blocks.

**Why a daemon, not in-kernel:** secrets handling is a discrete responsibility with a hostile-input adversary model (passphrases, dataset blob parsing, Argon2id timing). Isolating it in a low-privilege userspace process gives us:
- a single auditable surface for key handling (rather than scattering crypto across the kernel and every consumer);
- the ability to constrain it (`mlock`-equivalent pages, no core dumps, no debug attach) without imposing those constraints on the rest of userspace;
- a clean revocation point (kill `corvus`, all *in-corvus* DEKs are gone from RAM at once; per-user stratumd processes hold their own DEK caches with their own revocation discipline — see §4.5);
- proof-of-concept for the kind of small-trusted-component design Thylacine's capability model encourages.

This matches Plan 9's `factotum` and Stratum's `janus` shape. corvus is *not* a novel design — it's the right shape, well-proven, made native to Thylacine.

---

## 2. Relationship to factotum, janus, and contemporaries

corvus is a fresh implementation, not a port. It draws on:

- **Plan 9's `factotum`** — the architectural shape: dedicated process, file-based control interface (Plan 9's `/mnt/factotum/ctl`), peer-authenticated, holds secrets across application sessions. corvus's `/srv/corvus/ctl` is the direct descendant; the control verbs (`auth`, `change-passphrase`, `session-close`) mirror factotum's conventions where the semantics match.
- **Stratum's `janus`** (`stratum/v2/src/janus/`) — sister-project prior art for the wrap chain: Argon2id-derived KEK, AEAD-wrapped hybrid keypair (ML-KEM-768 + X25519 hybrid for post-quantum readiness), per-dataset DEKs wrapped under the keypair. Corvus reuses Stratum's *cryptographic primitives* (the libsodium-and-friends layer) but **not** Stratum's wire format, state file format, peer authentication mechanism, or process discipline. Each of those gets a Thylacine-native answer.
- **Contemporary per-user encryption** — macOS FileVault (per-user secure tokens unlocking a per-volume key), ZFS native encryption (per-dataset keys, KEK wrapping), Linux fscrypt (per-directory keys loaded into kernel keyring). corvus + Stratum together deliver the ZFS shape: per-dataset DEKs, KEK from passphrase.

What's *not* reused from Stratum's janus:
- The 9P-over-Unix-socket transport. Corvus serves a Spoor (see §6).
- The `SO_PEERCRED` / `LOCAL_PEERCRED` peer authentication. Corvus uses Proc identity (see §6.2).
- The single-pool, single-user daemon discipline. Corvus is multi-user from day one (see §3 D3).
- The Stratum janus state file layout (magic `JPAS`). Corvus uses its own state file format (magic `CRVS`, version 1, similar structure but Thylacine-tagged). Different magic prevents accidental cross-loading.

Stratum needs new APIs to compose with corvus (per-user stratumd; etc.). Those are enumerated in §13. The user explicitly committed to evolving Stratum to fit Thylacine's needs.

---

## 3. The six design decisions

This section names the load-bearing v1.0 commitments. Each has a v1.x extension path; the v1.0 choice is what binds implementation.

### D1: Bootloader / kernel location — *classic ESP*

**v1.0 commitment.** Kernel + initrd live as files on a UEFI ESP (FAT32 partition). The bootloader (systemd-boot-equivalent, or a thin Thylacine-native loader) reads them at boot. ESP is *not* Stratum.

**Atomic kernel update is via two-kernel rotation on the ESP:**
- `/boot/kernel-current` + `/boot/initrd-current` — the running kernel.
- `/boot/kernel-previous` + `/boot/initrd-previous` — the prior install.
- The bootloader menu shows both. Default = current.
- If current fails, user picks previous at the menu (no automatic fallback — keeping it explicit at v1.0 to avoid silent regression).

**`kernel-previous` retention** (audit F4 resolution): `kernel-previous` is deleted after the first successful boot of `kernel-current` that runs for ≥ 5 minutes. Specifically: joey checks an "ok-marker" on the ESP after 5 minutes uptime; if absent, sets it. If a subsequent boot finds the marker missing for a kernel-current, that kernel is considered failed and the bootloader's "stale kernel-previous" is retained one more cycle. This is the v1.0 substitute for an automatic-fallback mechanism (which lands with `corvid-boot` in v1.x); it bounds the rollback-attack window to "between an update and the first successful 5-minute boot of the new kernel."

**Userspace rollback:**
- Pre-update: `stratum snapshot create system@pre-kernel-update`.
- Update: copy new kernel+initrd to ESP, rotate `current` → `previous`.
- If new kernel boots successfully (5-min ok-marker), userspace post-install proceeds and `previous` is aged out next update cycle.
- If new kernel fails: at bootloader menu, pick `kernel-previous`, then `stratum rollback system@pre-kernel-update`.

**v1.x extension** — `corvid-boot`: a Stratum-aware bootloader that reads kernel files from `/stratum/system/boot/`, presents snapshot list at the menu, supports full Arch-style atomic updates with snapshot rollback at the boot level, and provides TPM-NVRAM-backed monotonic rollback protection. Out of v1.0 scope.

### D2: System pool encryption — *integrity-only, with boot-key binding*

**v1.0 commitment.** The system pool (`thylacine/system`) is not AEAD-encrypted at the dataset level. Stratum's Merkle tree gives integrity. The `.key` sidecar on the ESP holds the file-backend wrap key used non-interactively by `stratumd-system` at boot.

**Boot-key device binding** (audit F2 resolution): at install time, `boot_key` is derived as `HKDF(install_salt, pool_serial || install_uuid || master_secret)`. `pool_serial` is a 16-byte ID written to a fixed offset in the pool's superblock at create time; `install_uuid` is generated per install; `install_salt` is in `/boot/system.key.salt` on the ESP. At boot, `stratumd-system` reads the pool's superblock, verifies `pool_serial` matches what `/boot/system.key.salt` was bound for; refuses to mount if not. **An attacker who swaps the block device for one they crafted gets a `pool_serial` mismatch and stratumd refuses to mount; the system halts at a recovery prompt.** The attacker can still read `/boot/system.key.salt` from the ESP, but the salt alone doesn't unlock anything.

**Tradeoff.** A physical-disk attacker can still *read* system binaries / libraries / config — they can't *substitute the pool device*. This protects against the swap-the-disk evil-maid. It does NOT protect against modifying the bootloader, kernel, or initrd in place (see D6 — that's a separate threat addressed there).

**v1.x extension** — optional system-pool encryption ("hardened mode"): system pool's wrap key is itself wrapped by an Argon2id-derived KEK. corvus runs in the initrd, prompts for a system passphrase at boot, unwraps the system pool's DEK, then transfers state across the root-pivot.

### D3: User datasets — *always encrypted*

**v1.0 commitment.** Every user dataset (`thylacine/users/<name>`) is AEAD-encrypted. Its DEK is wrapped by the user's hybrid keypair; the keypair is wrapped by the user's passphrase-derived KEK. corvus owns the unwrap chain after authentication.

A user's home directory is unreadable on disk without the passphrase. Multiple users have **cryptographically mutually-encrypted homes** by construction: michael's `hybrid_sk` does not unwrap susan's wrapped DEKs.

**Runtime isolation between concurrently-logged-in users** (audit F17 resolution): cryptographic isolation gives the offline-attacker property. Runtime isolation (michael's processes cannot read susan's data while both are logged in) derives from Thylacine's territory + per-user stratumd model (§3 D4 + §6). Per-user stratumd processes hold per-user DEK caches; one user's stratumd cannot serve another user's dataset.

**v1.x extension** — alternative auth backends per user (FIDO2, smartcard, hardware token, remote escrow). The passphrase backend is the v1.0 default; backend type is a per-user setting.

### D4: Boot ordering — *Order C with per-user stratumd post-login*

**v1.0 commitment.** The boot order is:

```
1. UEFI loads bootloader from ESP.
2. Bootloader loads kernel-current + initrd-current.
3. Kernel starts; exec /sbin/joey from initrd.
4. joey: locate system pool device (DTB or initrd config).
5. joey: read /boot/system.key + /boot/system.key.salt from the initrd-staged ESP.
6. joey: fork stratumd-system against the system pool with the file-backed wrap key.
7. stratumd-system: verify pool_serial binding; mount the pool; serve a Spoor.
8. joey: kernel-side 9P mount of /sysroot via stratumd-system's Spoor.
9. joey: pivot root to /sysroot.
10. joey: explicit_bzero the in-memory copy of /boot/system.key.
11. joey: start /sbin/corvus (from /sysroot; state at /var/lib/corvus/).
12. joey: start /sbin/login (console login; or Halcyon login screen at Phase 8).
13. User enters credentials at console.
14. login → corvus (auth verb) → session token S.
15. login → kernel: rfork a per-user stratumd-michael, inheriting the session token's user identity via the kernel-mediated session-handle transfer.
16. stratumd-michael: opens its own connection to the pool's block device (multi-stratumd-per-pool model — see §4.2.3 + §13).
17. stratumd-michael → corvus (unwrap verb with session token): "give me users/michael's DEK".
18. corvus: verify session→user→dataset ownership, return DEK.
19. stratumd-michael: mounts users/michael, serves its Spoor.
20. login → kernel: mount stratumd-michael's tree at /home/michael in michael's territory.
21. login → rfork user shell with michael's session inherited.
22. User session begins.
```

**Per-user stratumd** is the architecturally-cleaner answer to multi-user isolation (audit F1 resolution Option A). One stratumd process per logged-in user dataset, plus one for the system pool. Each user's stratumd has its user's Proc identity; corvus's check on every unwrap is `peer_proc.user == owner_of(dataset)`. Kill michael's stratumd → michael's DEK is gone from RAM by construction (audit F16 mitigation, structural).

The Stratum-side requirement for this is multi-stratumd-per-pool support: multiple stratumd processes sharing one block device, each restricted to specific datasets. Enumerated in §13.

corvus only handles *user* keys at v1.0. The system pool is mounted before corvus exists.

### D5: Hostowner model — *no UID 0; console + system passphrase + recovery phrase*

**v1.0 commitment.** There is no UID 0 / root user. Privilege is determined by:
- **Capabilities** on the Proc (the Phase 4 `CAP_HW_CREATE` etc. surface, extended with `CAP_HOSTOWNER`).
- **Authentication** with the system passphrase (a credential separate from user passphrases, verified by corvus against a system-pool wrap chain).
- **Console attachment** — local console is the trust anchor.

The "hostowner" is whoever holds the system passphrase. They can:
- Create / delete users (`corvus admin user-create`).
- Snapshot / rollback the system pool.
- Update the kernel.
- Examine the audit log.

`corvus` gates these operations via `CAP_HOSTOWNER`, granted to a Proc only after the system passphrase is verified through a console-attached corvus session. Remote sessions (future sshd) cannot acquire `CAP_HOSTOWNER` without an explicit per-session grant from a console-attached hostowner.

**Recovery phrase** (audit F6 resolution): at install time, corvus generates a fresh 24-word BIP-39-style recovery phrase. The phrase derives a backup KEK via Argon2id; the backup KEK re-wraps the corvus admin keypair. The phrase is **displayed once to the install operator and never persisted**. If the system passphrase is lost, the recovery phrase resets it:

```
corvus admin recover --recovery-phrase '<24 words>' --new-system-passphrase '<new>'
```

Lost both → no admin authority forever (reinstall recovery). This is intentional: the recovery phrase IS the disaster-recovery instrument.

**v1.x extension** — multi-hostowner via Shamir k-of-n threshold (Stratum's OS-INTEGRATION.md already supports this pattern). v1.0 single hostowner; v1.x extends.

### D6: Boot-chain trust posture — *v1.0 explicit acceptance; Secure Boot in v1.x*

**v1.0 commitment** (audit F3 resolution): **v1.0 does NOT defend against an attacker with one-time physical write access to the ESP.** Such an attacker can modify the bootloader, the kernel, or the initrd in place. On next boot, the modified boot chain runs and can harvest user passphrases, escalate to admin, or substitute arbitrary userspace.

This is documented honestly: v1.0's threat model is "remote attacker + dishonest user-data-attacker (offline disk-read)", **not** "evil maid with write access". Users for whom this matters should:
- Use full-disk-encrypted external media for the ESP, OR
- Defer Thylacine deployment to v1.x with Secure Boot, OR
- Apply physical-security controls (locked laptop, TPM-locked boot, supervised access).

**v1.x extension** — P5-secure-boot chunk (deferred to v1.1 or later; see §10 + §11). Requires:
- Signing infrastructure (Thylacine kernel team key, MOK enrollment).
- UEFI Secure Boot configuration discipline.
- Signed initrd.
- TPM-NVRAM monotonic counter for anti-rollback (binds to F4 v1.x work).
- Documentation for users.

Until those land, the v1.0 commitment is to **not pretend** there's boot-chain integrity.

---

## 4. The corvus daemon

### 4.1 Process model

corvus runs as a single userspace daemon, started by joey post-pivot. It:
- Runs in its own Proc (no shared address space with anything else).
- Runs as a dedicated `corvus` system user (uid range 1-99 reserved system uids, exact uid TBD at provisioning).
- Holds `CAP_LOCK_PAGES` + `CAP_CSPRNG_READ` capabilities; no others.
- Has the v1.0 hardening syscalls applied at startup (see §4.1.1).

**Runtime invariants** (formalized as C-1..C-15 in §9):
- corvus never writes plaintext DEK or hybrid_sk to disk.
- corvus pages are mlock'd + DONTDUMP'd.
- Every plaintext passphrase received over `/ctl` is `explicit_bzero`'d before any other code path runs.
- On logout, in-RAM secrets are wiped before the close call returns (this is RAM-wipe, NOT cryptographic forward secrecy — see C-5).

#### 4.1.1 v1.0 hardening syscalls

corvus relies on these syscalls being available in v1.0 (audit F5 resolution). ARCH §11 must enumerate them; this design doc names them so the syscall surface authors know what's required:

| Syscall | Purpose | Used by |
|---|---|---|
| `sys_mlockall(flags)` | Pin all currently-mapped and future-mapped pages, preventing swap-out | corvus startup |
| `sys_set_dumpable(0)` | Disable core-dump for this Proc | corvus startup |
| `sys_set_traceable(0)` | Prevent debug-Spoor attach to this Proc | corvus startup |
| `sys_explicit_bzero(ptr, len)` | Compiler-barrier'd memset that the optimizer can't elide | corvus secret wipe |
| `sys_getrandom(buf, len, flags)` | Read from kernel CSPRNG; blocks if not seeded | corvus salt + token gen |

These are small, well-bounded syscalls. They land in v1.0 ARCH §11 + corresponding kernel implementations; P5-corvus-port's exit criteria gate on their availability (see §10).

`sys_set_traceable(0)` replaces the audit's questioning of "ptrace-equivalent blocked" — v1.0 introduces the syscall + the kernel-side discipline that no Proc with `traceable=0` accepts a debug Spoor attach.

### 4.2 Spoor interface

corvus serves a 9P tree at `/srv/corvus/`:

```
/srv/corvus/
├── ctl                       (RW)  control channel: auth requests, admin ops, session ops (BINARY FRAMES, see §6.4)
├── version                   (R)   daemon build info
├── status                    (R)   number of held keys, active sessions
├── sessions/
│   └── <sid>/
│       ├── owner             (R)   user this session belongs to
│       ├── caps              (R)   capability bits granted to this session
│       ├── since             (R)   when authenticated (monotonic timestamp)
│       └── close             (W)   write to terminate
├── users/
│   └── <name>/
│       ├── exists            (R)   "1" if user exists
│       ├── backends          (R)   newline-separated ("passphrase", "file", ...)
│       └── state             (R)   wrap-state path (admin-only)
├── peer/                                  ← optional client-facing introspection (§6.3)
│   ├── proc                  (R)   this Proc's PID + identity tag (stripes)
│   └── caps                  (R)   this Proc's capability bits
└── ops/                                   ← admin / control verbs
    ├── unwrap                (RW)  per-user-stratumd → corvus DEK unwrap (BINARY FRAMES)
    ├── wrap                  (RW)  DEK wrap under a user's hybrid keypair (BINARY FRAMES)
    ├── change-passphrase     (W)   per-user passphrase rotation (BINARY FRAMES)
    ├── user-create           (W)   admin op
    ├── user-delete           (W)   admin op
    ├── rotate-key            (W)   per-dataset key rotation
    └── recover               (W)   recovery-phrase-based system passphrase reset
```

The `/ops/unwrap` verb replaces what was previously called "Stratum janus wire compatibility." It is a Thylacine-native verb that carries:
- Session token (so corvus can verify session → user → dataset chain).
- Dataset ID + key_id (the targeting).
- Wrapped DEK blob — corvus's hybrid-PKE DEK envelope (§6.5), produced by the WRAP verb. Not Stratum's keyschema: Stratum stores corvus-format envelopes verbatim.

Returns the unwrapped DEK if the session-user owns the dataset; refuses otherwise. **C-7 ("corvus refuses unwrap for users not currently logged in") is achievable here**: the session token in the request maps to a user; corvus's dataset-ownership table maps the dataset to a user; corvus refuses if they don't match.

Sessions are identified by an opaque token (`s` + 128 bits of CSPRNG hex, audit F15 resolution). Tokens are bound to the Proc that created them (§6.2); closing the Proc auto-closes the session. The Proc can pass the session via a session-handle Spoor to a child Proc (cross-Proc handle transfer; §6.3).

**Idle timeout** (audit F15 resolution): a session with no activity for 30 minutes is auto-closed by corvus. Activity = any `/ops/` verb or session-bound walk. Sliding window. Hard cap: 24 hours regardless of activity; user must re-auth.

### 4.3 Backends

v1.0 ships two backends:

- **passphrase** — Argon2id over (passphrase, per-user-salt) → KEK → AEGIS-256-unwrap the hybrid keypair. Default for interactive users.
- **file** — read an unwrapped hybrid keypair from a file. For non-interactive deployments (the system pool's `.key` sidecar uses the file backend, fed by `stratumd-system` directly at boot, *not* by corvus; corvus handles only user keys at v1.0).

**Argon2id parameter policy** (audit F12 resolution): default `t_cost=2, m_cost=64 MiB, parallelism=1` (interactive preset). Per-user override to `sensitive` preset (`t_cost=4, m_cost=1 GiB`) available for high-assurance users. **Passphrase entropy gate**: corvus rejects passphrases scoring below zxcvbn-equivalent estimate of 80 bits at user-create and change-passphrase. Parameters are stored alongside wrap state and **re-randomized on every passphrase rotation** (fresh salt + fresh AEAD nonce + params re-evaluated against current default — audit F7 resolution).

**State file format**:

```
[0..4)       magic 'CRVS'  (Thylacine-native; not Stratum's JPAS)
[4..8)       version = 1
[8..12)      t_cost           u32 LE
[12..20)     m_cost_kib       u64 LE
[20..24)     parallelism      u32 LE
[24..40)     argon2id_salt    16 B
[40..72)     aead_nonce       32 B (re-randomized on each rewrap)
[72..ct_end) ciphertext       hybrid_pk || hybrid_sk encrypted
[ct_end..)   AEAD_tag         32 B
```

AD for the AEAD wrap is `"thylacine-corvus-v1" || user_name || backend_id` (binds the wrap to user, format version, and backend; a wrap cannot be moved between users or backends — audit-suggested binding).

v1.x adds: **fido2**, **smartcard**, **escrow** (network corvus / janus). The backend type is a per-user setting; a user can have multiple backends, with success-on-any-validate.

### 4.4 Lifecycle

```
joey starts corvus
  ↓ corvus loads /var/lib/corvus/config
  ↓ corvus calls sys_mlockall + sys_set_dumpable(0) + sys_set_traceable(0)
  ↓ corvus opens its Spoor at /srv/corvus/
  ↓ corvus verifies kernel CSPRNG is seeded (sys_getrandom with non-blocking probe)
  ↓ corvus loads dataset-ownership table from /var/lib/corvus/datasets/
  ↓ idle: 0 held keys, 0 sessions

user logs in
  ↓ login → corvus: BINARY-FRAME auth { user, passphrase }
  ↓ corvus: rate-limit check (5/min per user; audit F9 resolution)
  ↓ corvus: argon2id → KEK → unwrap hybrid keypair → store in mlock'd slab
  ↓ corvus: zeroize passphrase buffer
  ↓ corvus: generate session token (128 bits from CSPRNG); bind to peer Proc
  ↓ corvus: audit-log (encrypted; see §4.5.1)
  ↓ corvus: return session token

per-user stratumd starts
  ↓ stratumd-michael Proc → corvus: BINARY-FRAME unwrap { session_token, dataset_id, key_id, wrapped_blob }
  ↓ corvus: verify session token; verify session.user == owner_of(dataset_id)
  ↓ corvus: unwrap blob under user's keypair; return DEK
  ↓ stratumd-michael: cache DEK; mount dataset

user logs out
  ↓ login → corvus: BINARY-FRAME session-close { session_token }
  ↓ corvus: wipe keypair, wipe cached DEKs for this session
  ↓ corvus: audit-log
  ↓ corvus: respond OK
  ↓ stratumd-michael: detect session-close; explicit_bzero its own DEK cache; exits

corvus crash recovery (audit F11 resolution)
  ↓ corvus crashes (signal, OOM, malformed input parse error → graceful_die)
  ↓ joey detects via Proc-exit; restarts corvus
  ↓ corvus starts in clean state; all sessions are GONE
  ↓ existing per-user stratumd processes detect corvus death (Spoor closure)
  ↓ each per-user stratumd: continues serving from cached DEK until unmount
  ↓ user's next operation that requires re-unwrap fails → user is prompted to re-auth
  ↓ restart rate limit: > 5 crashes / minute → joey refuses restart, kernel extinction
```

### 4.5 Security boundaries

| Boundary | Attack | Mitigation |
|---|---|---|
| user → corvus | bad passphrase | Argon2id rate-limit (5/min/user); audit-log |
| stratumd → corvus | request for DEK the user doesn't own | corvus verifies session → user → dataset ownership chain |
| local Proc → corvus | impersonate another Proc | Spoor handles Proc-bound; cross-Proc transfer monotonically reduces caps; session token is per-Proc |
| disk attacker | read corvus state | state file AEAD-encrypted with KEK derived from user passphrase |
| disk attacker | read system passphrase verifier | system passphrase wrap state at `/var/lib/corvus/system-wrap` is itself Argon2id-AEAD-protected; physical-disk reader gets the verifier + params; offline-attacks at Argon2id cost |
| disk attacker | substitute the pool device (evil maid) | boot-key device binding (D2); stratumd-system refuses mount on serial mismatch |
| disk attacker | modify bootloader / kernel / initrd | NOT defended at v1.0 (D6); v1.x Secure Boot |
| hibernate / coredump leak | RAM scraping | `mlockall` + `set_dumpable(0)`; pages never to disk |
| ptrace / debug attach | RAM scraping via debugger | `set_traceable(0)`; no Proc can attach a debug Spoor |
| timing on Argon2id | passphrase oracle | Argon2id's per-guess constant cost + rate limit |
| audit-log disk-fill DoS | failed-auth spam | rate-limit at 5/min/user; bounded audit-log size with rotation |
| snapshot rollback after passphrase change | user rolls back to compromised passphrase | corvus pins prior snapshots of its state directory as "compromised on rollback" via stratum admin verb (audit F13; spec; impl v1.x) |
| stratumd holds DEK in RAM | killing corvus doesn't evict | structural mitigation: per-user stratumd means kill-stratumd-michael wipes michael's DEK |
| concurrent multi-user | user A reads user B's data | territory isolation (ARCH §15.1) + per-user-stratumd visibility scope; corvus refuses cross-user unwraps |
| session token guess | brute force | 128 bits of CSPRNG entropy; idle-timeout; absolute-timeout |
| wire-format injection | embedded `\n` in passphrase | binary frames (§6.4); no parsing ambiguity |
| audit log discloses activity | physical-disk attacker reads who-logged-in-when | audit log encrypted under system-passphrase KEK (§4.5.1) |

#### 4.5.1 Audit log

`/var/log/corvus.audit.gz` — append-only, encrypted under a system-passphrase-derived KEK, rotated at 10 MiB (keeping 7 rotations). Entries are line-delimited JSON. Every entry includes:
- monotonic timestamp (kernel CLOCK_MONOTONIC; corvus does not trust wall-clock for audit ordering)
- event type (`auth-success`, `auth-failure`, `session-close`, `unwrap-success`, `unwrap-refused`, `user-create`, `passphrase-changed`, `admin-elevate`, `crash`)
- session-id (if applicable)
- user (if applicable)
- dataset (if applicable)
- never a plaintext or hashed passphrase

The audit log is readable only after corvus authenticates the reader with the system passphrase. Encryption ensures a physical-disk attacker who can read the system pool gets ciphertext only; they learn that the audit log exists and its size, but not its contents.

---

## 5. Authentication flow

### 5.1 Login

```
[console]  user types: michael ↵  ********↵
       |
       v
[/sbin/login]
       |  Twrite /srv/corvus/ctl
       |    BINARY-FRAME { verb: 1=AUTH, user_len=7, user="michael",
       |                   passphrase_len=12, passphrase=<bytes> }
       |  (corvus mlocks the recv buffer; explicit_bzero's it after parse)
       v
[corvus]
       |  rate-limit check: < 5 failed attempts in last 60s for michael
       |  argon2id(passphrase, salt) → KEK
       |  AEAD-unwrap hybrid.corvus → keypair (in mlock'd slab)
       |  explicit_bzero passphrase + KEK
       |  generate session_token = "s" + 128 bits CSPRNG hex
       |  bind session to peer Proc (PID, identity tag)
       |  audit-log encrypted append: auth-success user=michael sid=...
       |  Tread response: BINARY-FRAME { status=OK, token=... }
       v
[login]
       |  rfork stratumd-michael, inheriting the session token
       |  stratumd-michael: opens block device (limited cap: dataset-restricted)
       |  stratumd-michael → corvus:
       |    BINARY-FRAME unwrap { session_token, dataset=users/michael, key_id, wrapped }
       v
[corvus]
       |  verify session_token → user=michael
       |  verify dataset users/michael → owner=michael
       |  unwrap → DEK → return BINARY-FRAME { status=OK, dek=... }
       v
[stratumd-michael]
       |  cache DEK; mount users/michael; serve Spoor for it
       v
[login]
       |  kernel mounts stratumd-michael's tree at /home/michael
       |    (in michael's territory only)
       |  rfork user shell; bind /home/michael as $HOME
       |  exec /bin/rc
```

### 5.2 Logout

```
user logs out (shell exits or Halcyon close)
       |
       v
[login]  Twrite /srv/corvus/sessions/s7/close
       v
[corvus]
       |  audit-log: session-close s7 user=michael
       |  wipe keypair (explicit_bzero)
       |  Tread: OK
       v
[stratumd-michael]
       |  detect session-close (corvus emits a close notification on /ops/notify)
       |  explicit_bzero own DEK cache
       |  unmount users/michael
       |  exits cleanly
       v
[joey] reaps stratumd-michael; releases its block-device dataset claim
```

### 5.3 Passphrase change

```
user → corvus-passwd CLI or Halcyon settings
       |
       v
[client] BINARY-FRAME { verb: 2=CHANGE_PASSPHRASE,
                        user, old_passphrase, new_passphrase }
       v
[corvus]
       |  verify zxcvbn(new_passphrase) >= 80 bits — else reject (audit F12)
       |  verify old (argon2id + AEAD-unwrap)
       |  generate FRESH salt (audit F7)
       |  generate FRESH AEAD nonce (audit F7)
       |  re-evaluate Argon2id params against current default; bump if outdated
       |  derive new KEK from new_passphrase + fresh_salt + (new) params
       |  AEAD-re-wrap keypair with new KEK + fresh nonce → ciphertext
       |  write atomically (tmp + fsync + rename) /var/lib/corvus/users/michael/hybrid.corvus
       |  also write fresh wrap-state with new params + salt
       |  emit stratum-admin-verb to prune prior snapshots of /var/lib/corvus/users/michael
       |  audit-log: passphrase-changed user=michael
       |  return OK
```

The hybrid keypair itself is unchanged — only the wrap is rewritten. Existing DEKs remain valid; no bulk re-encryption.

### 5.4 Adding a user

```
hostowner authenticated session, has CAP_HOSTOWNER
       |
       v
[admin]  BINARY-FRAME user-create { user, initial_passphrase, backend }
       v
[corvus]
       |  verify caller has CAP_HOSTOWNER
       |  verify zxcvbn(initial_passphrase) >= 80 bits
       |  generate hybrid keypair (CSPRNG-seeded; ML-KEM-768 + X25519)
       |  argon2id(initial_passphrase, fresh_salt, default_params) → KEK
       |  AEAD-wrap keypair with KEK + fresh_nonce → state file
       |  write /var/lib/corvus/users/<user>/hybrid.corvus + wrap-state
       |  add (<user>, "users/<user>") entry to dataset-ownership table
       |  audit-log: user-create user=<user> by_hostowner=sid7
       v
[admin]  Twrite /srv/stratum-ctl/.../create-dataset { dataset=users/<user>, encrypted=1 }
       v
[stratumd]
       |  generate DEK (CSPRNG)
       |  ask corvus to wrap it (WRAP verb, §6.4 / §6.5; the hostowner's
       |    CAP_HOSTOWNER authorizes the admin WRAP path — the new user
       |    has no session yet) under the new user's keypair → wrapped_dek
       |  store wrapped_dek in dataset metadata
done. user can now log in.
```

User deletion is symmetric. The dataset destruction is a Stratum operation; corvus removes the user's state files; audit log records.

### 5.5 Hostowner elevation

Elevation confers `CAP_HOSTOWNER` on a console session's Proc once the system passphrase is verified. corvus cannot mutate a Proc's capability set directly — corvus is userspace. Elevation is therefore a **two-phase, file-mediated grant** through a kernel **capability device** (`cap`), modelled on Plan 9's `cap(3)` — the `caphash` / `capuse` device through which factotum conferred authority on a process. corvus is the factotum-pattern agent; the `cap` device is its kernel-side counterpart.

```
[console user] runs: corvus admin elevate
       |
       v
[client]  BINARY-FRAME admin-elevate { session_token, system_passphrase }
       v
[corvus]  verify session is from a console-attached Proc (peer identity, §6.2)
       |  argon2id(system_passphrase, system_salt) → system_KEK
       |  AEAD-unwrap system-wrap → verify magic + tag
       |  explicit_bzero passphrase + KEK + admin-keypair
       |  ── PHASE 1: register the grant ──
       |  write { cap: CAP_HOSTOWNER, tag: T } → `cap` device, `grant` file
       |     [kernel gate: writer holds CAP_GRANT_HOSTOWNER]
       |     kernel records a pending grant keyed by peer identity tag T
       |  audit-log: admin-elevate sid=... tag=T
       v  return OK
[client]  ── PHASE 2: redeem the grant ──
       |  write { cap: CAP_HOSTOWNER } → `cap` device, `use` file
       |     [kernel gate: a pending grant exists for the WRITER's own
       |      identity tag, AND the writer holds PROC_FLAG_CONSOLE_ATTACHED]
       |  kernel: current->caps |= CAP_HOSTOWNER ; consume the pending grant
       v
done — the console Proc (identity tag T) now holds CAP_HOSTOWNER
```

The Proc that issues ADMIN_ELEVATE is the Proc that redeems and is elevated: corvus registers the grant against that Proc's kernel-stamped identity tag, and only that Proc can redeem it. The capability lands on the **Proc**, not on corvus's session record (the session is a userspace abstraction; `CAP_HOSTOWNER` is a kernel Proc capability). Because the target redeems on its own behalf, the kernel writes `current->caps` — there is no cross-Proc capability write.

#### 5.5.1 The `cap` device

A kernel device, `cap`, exposes two write-only files:

- **`grant`** — corvus writes `{ cap, identity_tag }`. The kernel gates the write on the writer holding `CAP_GRANT_HOSTOWNER` and records a *pending grant* in a small bounded table keyed by identity tag. At most one pending grant per tag (a re-register replaces it); pending grants carry a short expiry and are dropped on the target Proc's death. v1.0 accepts only `cap == CAP_HOSTOWNER`; the device is general so future elevation-only capabilities can reuse it.
- **`use`** — any Proc writes a redeem request. The kernel looks up a pending grant for *the writer's own* kernel-stamped identity tag; if one exists **and** the writer holds `PROC_FLAG_CONSOLE_ATTACHED`, it ORs the granted capability into `current->caps` and consumes the pending grant. One-shot.

**Two capabilities, two roles.**

- `CAP_GRANT_HOSTOWNER` — an ordinary *fork-grantable* capability (a member of `CAP_ALL`). It authorizes writing the `grant` file. joey holds it via `CAP_ALL` and confers it on corvus in corvus's spawn mask; ordinary user Procs never receive it. This is the kernel-enforced "only corvus may register grants."
- `CAP_HOSTOWNER` — an *elevation-only* capability, deliberately excluded from `CAP_ALL` (§3 D5; P5-hostowner-a). It is the authority the admin verbs check. It enters a Proc's capability set **only** by redeeming a pending grant through `use`, and it is **never** conferred by `rfork`: `rfork` strips every elevation-only capability from the child. (Without the strip, a `CAP_HOSTOWNER` Proc that spawns a child would leak the capability to a child that does not carry `PROC_FLAG_CONSOLE_ATTACHED` — a Proc holding `CAP_HOSTOWNER` without console attachment, which `specs/corvus.tla`'s `HostownerRequiresConsole` forbids.)

**Defense in depth.** The two authorization gates are deliberately independent and enforced in different trust domains:

- corvus verifies the **system passphrase** — a check only corvus can perform; the kernel has no notion of the passphrase.
- the kernel verifies **console attachment** at `use`-redemption — a check that holds even if corvus is buggy or compromised. A compromised corvus can register grants for arbitrary identity tags, but the kernel only lets a *console-attached* Proc redeem. corvus elevating a network process is therefore structurally impossible; a corvus compromise is bounded to the local physical console. This is why the console gate is kernel-enforced rather than left to corvus — it is what makes `HostownerRequiresConsole` robust against corvus's own correctness.

A session without `CAP_HOSTOWNER` cannot execute admin verbs; verb authorization is checked **per-call** on the Proc's current capability set.

**Console attachment.** The kernel marks a Proc console-attached iff it was spawned through joey's console-login chain (`PROC_FLAG_CONSOLE_ATTACHED`, P5-hostowner-a); the bit is never propagated across `rfork`, so a future sshd / remote-login chain cannot inherit the trust anchor. corvus reads the peer Proc's console bit via the kernel-stamped peer identity (§6.2) to fail fast on a non-console session; the kernel re-checks it at `use`-redemption as the load-bearing gate.

**Heritage and originality.** The two-phase register/redeem shape is Plan 9's `cap(3)`. The Thylacine adaptation is substantive: Plan 9's device changed a process's *user identity* (uid strings), but Thylacine has no uid 0 (D5) — the `cap` device grants a *capability bit* and binds the grant to the unforgeable per-Proc identity tag (§6.2). And Plan 9 used an HMAC over a factotum↔kernel shared secret because factotum's registration channel was untrusted; here the `grant` write is itself `CAP_GRANT_HOSTOWNER`-gated, so the kernel trusts the channel directly — no token and no kernel-side cryptography.

### 5.6 Recovery phrase

```
[hostowner forgets system passphrase]
       |
       v
[console] runs: corvus admin recover
            paper recovery phrase: --recovery-phrase '<24 words>'
            new system passphrase:  --new-system-passphrase '<new>'
       v
[corvus]
       |  argon2id(recovery_phrase, recovery_salt, sensitive_params) → recovery_KEK
       |  AEAD-unwrap recovery-wrap (alongside system-wrap; same admin keypair, two wraps)
       |  if successful: explicit_bzero recovery_KEK
       |  argon2id(new_system_passphrase, fresh_salt, default_params) → new_system_KEK
       |  AEAD-re-wrap admin keypair under new_system_KEK → new system-wrap
       |  write atomically (tmp + fsync + rename) /var/lib/corvus/system-wrap
       |  ALSO re-wrap the recovery wrap with a FRESH recovery_phrase
       |  (generate new 24 words, display to user, force them to write it down)
       |  audit-log: recovery-used; system-passphrase-rotated
```

The recovery phrase is a printed-paper instrument. Loss of both system passphrase AND recovery phrase = system reinstall required (intentional disaster-recovery floor).

---

## 6. Transport: the `/srv` service and per-connection sessions

corvus is a full **9P2000.L server** in userspace, reached **per-connection**: every client Proc that opens `/srv/corvus` gets its own kernel↔corvus 9P session. The transport is created and owned by the kernel; the per-caller identity corvus authorizes against is stamped by the kernel and read through a syscall, never sourced from the client. The design converged across four adversarial design-audit rounds (r1–r5); this section is r5.

### 6.1 The `/srv` device and service registration

`/srv` is a kernel device — a new `Dev`, **`devsrv`** (Plan 9's `#s` heritage; ARCH §9.4). It is deliberately a *distinct* `Dev` from `dev9p` (the Dev that backs kernel-mounted 9P trees). The separation is structural, not cosmetic: every Spoor walked out of `/srv` carries `devsrv`'s device character, so a `/srv/corvus` connection Spoor is a **`KObj_Srv`** kernel object — a kind that is **non-transferable** (ARCH §18.2; the same I-5-style structural rule as `KObj_MMIO`). A client cannot 9P-transfer its corvus connection to another Proc — the connection is pinned to the Proc that opened it. That non-transferability is what makes the kernel-stamped peer identity (§6.3) unforgeable across a 9P walk.

corvus posts its service exactly once at startup:

```
SYS_POST_SERVICE(name) → service_handle
```

registers the calling Proc as the 9P *server* for `/srv/<name>`. corvus calls `SYS_POST_SERVICE("corvus")`. The **kernel creates and owns all transport** — corvus never allocates or holds a transport pipe (invariant **C-23**).

Posting *or rebinding* the name `corvus` is gated on a one-way `proc_flags` bit that **joey** stamps on the corvus Proc it spawns (and re-stamps on every corvus restart). joey is the sole setter; the bit is never propagated across `rfork` (the same discipline as `PROC_FLAG_CONSOLE_ATTACHED`, §5.5). An ordinary Proc cannot post or hijack `/srv/corvus`. When corvus dies, the kernel **tombstones** the name rather than freeing it: only a Proc carrying joey's marker may rebind it, so a malicious Proc cannot race corvus's restart to claim `/srv/corvus`. (One mechanism, not two: the marker *is* the rebind authority.)

### 6.2 Per-connection sessions

Every client Proc that opens `/srv/corvus` gets **its own kernel↔corvus 9P session** — a connection. There is no shared session.

When client Proc P opens `/srv/corvus/<path>`:
1. The kernel mints a fresh transport pair (a pipe pair) — kernel-owned, never placed in any handle table.
2. The kernel builds a dedicated *synchronous* `p9_client` over that transport.
3. The kernel enqueues the new connection on corvus's bounded accept queue.
4. corvus accepts it (`SYS_SRV_ACCEPT`, below).
5. The kernel drives `Tversion` + `Tattach` + `Twalk(<path>)` on the connection.
6. The kernel records the connection→P binding and installs a non-transferable `KObj_Srv` Spoor in P's handle table.
7. `open` returns to P.

The connection is created on P's first open and reused for P's lifetime; the create is guarded by a per-Proc lock (check-then-create, so two threads of P racing the first open get one connection). One connection per Proc — a per-Proc cap of 1, plus a global cap on live `/srv/corvus` connections (~256, corvus's `MAX_USERS` order); past the global cap, `open` fails fast.

**Why per-connection and not a shared session.** Thylacine's kernel 9P client (`kernel/9p_client.c`) is synchronous — it holds the per-client lock across the blocking `p9_transport_exchange`. A *single shared* `p9_client` for all corvus clients would serialize every client behind that one lock: one slow corvus operation (an Argon2id verify) would wedge every other client, and a client blocked in corvus would hold the lock indefinitely. Per-connection sidesteps this entirely — each connection has its own `p9_client` and its own private lock, so connections are independent. It is also the shape stratumd's server is built for (concurrent accept; per-connection handling). Async/pipelined 9P (ARCH §21) is real owed work but is **not** a prerequisite here: per-connection parallelism is connection-level, and stratumd's own client library is itself one-op-at-a-time at v2.0.

```
SYS_SRV_ACCEPT(service_handle) → connection_handle
```

corvus blocks until a client opens `/srv/corvus`, then receives the server end of one fresh kernel-minted connection. The accept queue is bounded; past it a client's `open` fails fast rather than queueing unboundedly.

**`tsleep`.** The kernel's blocking 9P ops on a `/srv` connection must be deadline-bounded — a corvus that hangs (deadlock, livelock) must not wedge its clients forever. The kernel's `Rendez` `sleep()` is indefinite; r5 adds **`tsleep`** — a `Rendez` sleep with a deadline, woken by `wakeup` or by the timer (ARCH §8.8). A hung corvus → the client's wait expires → `open` (or the in-flight op) fails `-ETIMEDOUT`; the client is never wedged. `tsleep` is owed for the Phase-5 `poll`/`futex` work regardless; it lands as its own small prerequisite chunk, **P5-tsleep**.

**Teardown ordering.** On corvus death (or a connection close) the kernel (1) sets every affected connection's transport to `ERROR` and EOFs both pipe ends, so any kernel thread blocked in `do_recv` / `tsleep` on that connection wakes and returns `-EIO`; (2) fences client threads out of the dead connection; (3) frees the `p9_client` and the transport pipes. A corvus *crash* wakes blocked clients immediately via the EOF/ERROR path — the `tsleep` deadline is the backstop for a corvus *hang*, where no EOF is forthcoming.

**corvus is single-threaded.** corvus `poll`s its listener plus its N accepted connection fds and serves one 9P message at a time, each connection's protocol state held in a strictly isolated per-connection arena (a fid bug on one connection cannot reach another). (`poll` is the multi-fd wait syscall — the **P5-poll** chunk, ARCH §23.3; a prerequisite of P5-corvus-srv-impl-b, since Thylacine has no kernel "wait on N sources" primitive otherwise.) A slow operation (Argon2id) adds latency to other connections; this is accepted for the infrequent login path and documented — a v1.x corvus may multi-thread. Per-connection arena isolation is the load-bearing property; single-threadedness is an implementation simplification, not a security boundary.

### 6.3 Kernel-stamped peer identity

corvus must know *who* is on the other end of each connection — to bind sessions, gate admin verbs, refuse cross-user unwraps. That identity is **stamped by the kernel** and read by corvus through a syscall; it never comes from the client and is never cached on corvus's own fid state. This is invariant **C-22**.

**`stripes`.** Every Proc carries a `stripes` value — a `u64`, the kernel's per-Proc identity tag (the thylacine's stripe pattern: every animal's is unique). It is drawn from a monotonic kernel counter at `proc_alloc`, fresh for every Proc — an `rfork` child gets a *different* `stripes` from its parent, so it is structurally distinct, not inherited. `stripes == 0` is a reserved fail-closed sentinel (an unstamped or torn read reads 0 and authorizes nothing). `stripes` is immutable for the Proc's life. It is the kernel's answer to "is this the same Proc?" — stable, unforgeable, not recycled while the Proc is alive.

**`SYS_SRV_PEER`.** corvus reads a connection's peer identity with:

```
SYS_SRV_PEER(connection_handle) → { stripes, console, caps }
```

- `stripes` and the `console`-attachment bit are the peer Proc's **immutable** identity. The kernel-side connection object captures them **by value at bind time** — it holds no raw `Proc*` for the immutable fields, so a peer that exits cannot turn a later read into a use-after-free.
- `caps` is the peer's **mutable** capability set. The kernel reads it **live** on each call, under the process-table lock, with a dead-Proc guard: if the peer Proc has exited (zombie / reaped), `SYS_SRV_PEER` returns a **fail-closed** result — never stale `caps`, never a UAF.

The call is gated to the service's poster — only corvus may query peers of `/srv/corvus` connections.

corvus calls `SYS_SRV_PEER` **per request**; it never caches the result on its own fid state. For an admin verb, corvus re-queries `caps` immediately before the irreversible effect, closing the TOCTOU window between "decided to allow" and "performed." (At v1.0 there is no cap-drop syscall, so `caps` only grows and the unsafe direction is unreachable; the v1.x obligation — when cap-drop lands, keep the re-query — is recorded here so it is not lost.)

**Session reach across Procs.** A corvus *session* (the post-AUTH auth state) is identified by its opaque bearer token (§4.2; `s` + 128 bits). The token is data, not a handle: `login` authenticates over *its* connection, receives token S, and passes S — as plain bytes, in a request payload — to the per-user `stratumd` it spawns. `stratumd-michael` opens *its own* `/srv/corvus` connection (its own `stripes`, its own kernel-stamped identity) and presents S in its UNWRAP frame. corvus validates S → michael, checks michael owns the dataset, and serves. The session moved between Procs **by bearer token**, not by transferring a connection Spoor — `KObj_Srv` is non-transferable, so there is no handle to transfer.

**Authorization orthogonality** (audit F10; **C-11**) is unchanged by r5: every operation checks exactly the authority it needs. A verb that needs Proc capability X checks the connection peer's live `caps` for X (via `SYS_SRV_PEER`); a verb that needs session ownership of user Y checks the bound token's user. The two are orthogonal — corvus does not check the cross-product. A `CAP_HOSTOWNER` Proc presenting michael's token may run admin verbs (it has the cap) *and* unwrap michael's DEKs (the token owns michael); on a Plan-9-heritage system an admin reading user data is a feature, made explicit here.

### 6.4 Wire format: binary frames

`/srv/corvus/ctl` and `/srv/corvus/ops/*` use **binary frames**, not line-based text (audit F8 resolution). A **request** frame (peer → corvus) carries a 4-byte header:

```
[0]      verb_id           u8  (see verb table)
[1]      protocol_version  u8  = 1   (Stratum↔corvus wire version — STRATUM-API-V1 Q11)
[2..4)   payload_len       u16 LE
[4..)    payload           verb-specific
```

`protocol_version` is the negotiated wire-version byte (resolved with Stratum as STRATUM-API-V1.md Q11; Stratum's UNWRAP/WRAP encoders emit it); corvus refuses an unknown version with `BadFormat`. **Response** frames (corvus → peer) carry no version byte — the 3-byte header below.

Verb table:

| verb_id | Name | Payload |
|---|---|---|
| 1 | AUTH | `user_len u8` + `user` + `pass_len u16` + `passphrase` |
| 2 | CHANGE_PASSPHRASE | `user_len u8` + `user` + `old_len u16` + `old` + `new_len u16` + `new` |
| 3 | SESSION_CLOSE | `token` (33 bytes: "s" + 32-char hex) |
| 4 | UNWRAP | `token` (33) + `dataset_len u8` + `dataset` + `key_id u64 LE` + `wrapped_len u16` + `wrapped` |
| 5 | USER_CREATE | `user_len u8` + `user` + `pass_len u16` + `passphrase` + `backend u8` |
| 6 | USER_DELETE | `user_len u8` + `user` |
| 7 | ADMIN_ELEVATE | `token` + `sys_pass_len u16` + `sys_passphrase` |
| 8 | RECOVER | `phrase_len u16` + `phrase` + `new_sys_pass_len u16` + `new_sys_passphrase` |
| 9 | ROTATE_KEY | `token` + `dataset_len u8` + `dataset` |
| 10 | WRAP | `token` (33) + `dataset_len u8` + `dataset` + `key_id u64 LE` + `dek_len u16` + `dek` |

Response frame:

```
[0]      status            u8  (0=OK, 1=BadAuth, 2=PermissionDenied, 3=NotFound, 4=RateLimited, 5=BadFormat, 6=InternalError)
[1..3)   payload_len       u16 LE
[3..)    payload           status-specific (e.g., session token on AUTH OK, DEK on UNWRAP OK)
```

The recv buffer for any frame carrying a passphrase / DEK is mlock'd. Corvus calls `sys_explicit_bzero` on the buffer immediately after parsing the relevant fields into typed storage and again after the typed storage is consumed.

**No newline / shell-injection hazards**: binary frames don't parse text; passphrases can contain any byte except length-overflow. A passphrase containing `\n` or `\0` or `=` works fine.

### 6.5 WRAP and the DEK envelope

WRAP (verb_id=10) is the inverse of UNWRAP: it wraps a 32-byte dataset DEK under a user's hybrid keypair, producing the **DEK envelope** — the `wrapped` blob UNWRAP later consumes. WRAP realizes the "ask corvus to wrap it under [the] user's keypair" step of §5.4 (dataset creation).

WRAP has two authorization paths. Its **normal path** is **C-7-gated** like UNWRAP — a session may WRAP only for a dataset it owns (use case: a logged-in user re-sealing their own dataset's DEK). But WRAP *creates* a seal, and dataset provisioning (§5.4) must seal a DEK for a user not yet logged in — no session for them exists, and the hostowner's session does not own `users/<newuser>`. So WRAP also has an **admin path**: a session holding `CAP_HOSTOWNER` may WRAP for any dataset, bypassing C-7. This is the provisioning path — the hostowner (console-attached + system-passphrase-verified, §5.5) seals the new user's DEK. The admin path requires corvus to hold the recipient's *public* key independent of that user's session — the public half of each user's hybrid keypair is retained accessible, while the private half stays passphrase-wrapped. The `CAP_HOSTOWNER` gate materializes the `AdminVerb` authorization shape of `specs/corvus.tla`.

Status: the C-7 normal path is as-built since P5-corvus-bringup-d. `CAP_HOSTOWNER` + the console-attachment foundation landed at P5-hostowner-a; the WRAP admin-path check lands with ADMIN_ELEVATE at P5-hostowner-b. The bilateral contract for WRAP is `STRATUM-API-V1.md §5.10`.

The DEK envelope is a **Thylacine-native KEM-DEM hybrid owned by corvus** — not Stratum's keyschema. Stratum stores corvus-produced envelopes verbatim in dataset metadata; the blob is corvus's format end to end. It binds a post-quantum and a classical KEM so it stays secure if *either* primitive holds:

```
[0]            envelope_version  u8 = 1
[1..1089)      mlkem_ct          1088 B   ML-KEM-768 ciphertext
[1089..1121)   x25519_eph_pk     32 B     X25519 ephemeral public key
[1121..1153)   aead_nonce        32 B     AEGIS-256 nonce
[1153..1185)   dek_ct            32 B     AEGIS-256(DEK) ciphertext
[1185..1217)   dek_tag           32 B     AEGIS-256 tag
```

Construction:
- **KEM**: ML-KEM-768 (FIPS 203) encapsulation to the recipient's encapsulation key yields `(mlkem_ct, ss_pq)`; a fresh X25519 ephemeral → recipient-static ECDH yields `ss_cl`.
- **KEK combiner**: `kek = SHA-256("thylacine-corvus-dek-kdf-v1" || ss_pq || ss_cl || x25519_eph_pk || mlkem_ct)`. The transcript bind ties the KEK to the exact ciphertext.
- **DEM**: AEGIS-256 encrypts the 32-byte DEK under `kek`, AD = `"thylacine-corvus-dek-v1" || dataset || key_id` (`key_id` is the LE u64 from the WRAP/UNWRAP frame) — so a wrapped DEK cannot be replayed against a different dataset or key generation.

UNWRAP reverses this: ML-KEM-768 decapsulate (FIPS 203 implicit rejection — a bad ciphertext yields a wrong shared secret, not an error) + X25519 ECDH reproduce `kek`; the AEGIS-256 tag check is the integrity gate. UNWRAP's C-7 ownership check fires *before* any crypto.

WRAP's OK-response payload is the 1217-byte envelope; UNWRAP's OK-response payload is the 32-byte DEK.

---

## 7. Kernel update flow

### 7.1 Normal update

```
[user]  $ stratum snapshot create system@pre-kernel-update
[user]  $ thyla-pkg upgrade kernel
        ↓ verify kernel package signature (v1.0 best-effort; v1.x with Secure Boot enforces)
        ↓ writes kernel-next + initrd-next into ESP
        ↓ verify ESP write succeeded; fsync
        ↓ rotate: kernel-previous := kernel-current
                  kernel-current  := kernel-next
                  (delete the older kernel-previous if present)
        ↓ updates bootloader entry (defaults to kernel-current)
[user]  $ reboot
[kernel boots]
        ↓ on 5-minute uptime, joey sets /boot/ok-marker
        ↓ on next clean shutdown, kernel-previous is eligible for pruning after one more update cycle
```

### 7.2 Recovery from a failed kernel

```
[user]  reboots, hold key during bootloader countdown
[bootloader menu]
        ↓ user picks "kernel-previous"
[kernel-previous boots]
[user]  $ stratum rollback system@pre-kernel-update
        ↓ reboot
[kernel-previous boots again with pre-update userspace]
[user]  files a bug.
```

Manual recovery. Automatic fallback is v1.x with `corvid-boot` (heartbeat-driven).

### 7.3 ESP retention policy

- `kernel-current` + `initrd-current` — always present.
- `kernel-previous` + `initrd-previous` — present until the **first 5-minute clean boot of kernel-current**, then deleted on the **next** update cycle (so there's always at least one rollback target).
- `kernel-prev-prev` is never retained.
- ESP holds at most: 2 kernels (~50 MiB each = 100 MiB) + 2 initrd (~5 MiB each = 10 MiB) + bootloader + system.key + system.key.salt + bootloader config + ok-marker = comfortably under 200 MiB on a 512 MiB ESP.

---

## 8. Pool layout

Stratum pool `thylacine`:

```
thylacine                              [pool]
├── system                             [dataset, integrity-only, file-key]
│   ├── /                              ... root, mounted at boot by stratumd-system
│   ├── /usr                           ... binaries
│   ├── /lib                           ... libs (libc, libt, libstratum, libcorvus)
│   ├── /etc                           ... config
│   ├── /var
│   │   ├── lib
│   │   │   └── corvus
│   │   │       ├── config             ... daemon config
│   │   │       ├── system-wrap        ... admin keypair wrapped by system passphrase
│   │   │       ├── system-recovery-wrap ... admin keypair wrapped by recovery phrase
│   │   │       ├── datasets/          ... dataset-ownership table
│   │   │       └── users
│   │   │           └── <name>
│   │   │               ├── hybrid.corvus     (AEAD-wrapped keypair, magic CRVS)
│   │   │               └── wrap-state        (Argon2id params + salt + backend type)
│   │   └── log
│   │       └── corvus.audit.gz        ... encrypted append-only audit
│   └── /srv                           ... mount points
├── users                              [dataset namespace]
│   ├── michael                        [dataset, encrypted, corvus-managed]
│   ├── guest                          [dataset, encrypted, corvus-managed]
│   └── ...
└── shared                             [dataset, optional v1.x]
```

The ESP partition is separate from the pool:

```
ESP (FAT32, ~512 MiB)
├── EFI/BOOT/BOOTAA64.EFI               ... bootloader
├── boot
│   ├── kernel-current
│   ├── initrd-current
│   ├── kernel-previous                 ... present after first update
│   ├── initrd-previous
│   ├── system.key                       ... wrap key for system pool
│   ├── system.key.salt                  ... HKDF salt for boot-key device binding
│   └── ok-marker                        ... touched on 5-min clean boot
└── EFI/loader/loader.conf              ... bootloader config
```

ESP is unencrypted at v1.0; physical-disk reader sees `system.key`. The boot-key device binding (D2) prevents pool-substitution; **see D6 for the explicit v1.0 acceptance that the ESP is otherwise not integrity-protected**.

For v1.x hardened mode: `/boot/system.key` becomes `/boot/system.key.wrap`, wrapped by an initrd-corvus passphrase prompt.

---

## 9. Invariants

These are runtime + audit guarantees at v1.0. `specs/corvus.tla` formalizes the session state machine, the capability arithmetic, and (P5-corvus-srv) the connection-transport layer.

| ID | Invariant | Enforced by |
|---|---|---|
| C-1 | corvus never writes plaintext DEK or hybrid_sk to disk | code review + audit |
| C-2 | corvus pages are mlock'd + DONTDUMP'd | `sys_mlockall` + `sys_set_dumpable(0)` at corvus startup |
| C-3 | A session's user-identity binding is immutable for the session's lifetime | session struct invariant |
| C-4 | Session capability transfers monotonically reduce (ARCH I-2/I-6 extended) | session cap arithmetic |
| C-5 | On logout, in-RAM secrets are wiped before the close call returns (RAM-wipe; NOT cryptographic forward secrecy — see §11) | logout discipline + `sys_explicit_bzero` |
| C-6 | The system passphrase is verifiable without unlocking any user keypair | separate wrap chain (system-wrap vs user wraps) |
| C-7 | corvus refuses unwrap for a (session, dataset) pair where session.user != owner_of(dataset) | unwrap verb implementation |
| C-8 | A Proc cannot impersonate another Proc to corvus | kernel-stamped Spoor-bound Proc identity (§6.2) |
| C-9 | Audit log entries are append-only, monotonically timestamped, encrypted, and contain no plaintext/hashed passphrase | audit-log discipline |
| C-10 | Argon2id parameters are persisted alongside wrap state; fresh salt + nonce on every rewrap | passphrase backend implementation |
| C-11 | Session-cap and Proc-cap authorization are orthogonal; operations check the auth they need, not the cross-product (audit F10) | verb authorization code |
| C-12 | Passphrases below 80-bit zxcvbn estimate are rejected at user-create and change-passphrase (audit F12) | corvus passphrase entropy gate |
| C-13 | Passphrase changes are durable against system-pool snapshot rollback: corvus emits a stratum admin verb to mark prior snapshots of /var/lib/corvus/users/<user>/ as compromised (audit F13; impl v1.x) | corvus + stratum coordination |
| C-14 | `/boot/system.key` is cryptographically bound to the pool device's unique serial via HKDF; stratumd-system refuses mount on serial mismatch (audit F2) | stratumd-system boot-time check |
| C-15 | corvus refuses to generate randomness until kernel CSPRNG is verified seeded (audit F20) | corvus startup `sys_getrandom` probe |
| C-16 | corvus rate-limits failed auth attempts to 5/min/user; persistent rate-limit state survives corvus restart via Stratum (audit F9) | corvus rate-limit discipline |
| C-17 | Session tokens are 128 bits of CSPRNG entropy; idle timeout 30 minutes (sliding); absolute timeout 24 hours (audit F15) | corvus session manager |
| C-18 | Per-user stratumd processes only mount datasets owned by their user; multi-stratumd-per-pool block-device access is dataset-restricted (audit F1 + new) | stratumd multi-mode + corvus ownership check |
| C-19 | corvus's audit log is encrypted under a system-passphrase-derived KEK; physical-disk read yields ciphertext only (audit F9) | audit-log encryption discipline |
| C-20 | Recovery phrase is generated at install, displayed once, never persisted; loss of both system passphrase and recovery phrase requires reinstall (audit F6) | install flow + recovery verb |
| C-21 | `CAP_HOSTOWNER` is elevation-only: it enters a Proc's capability set solely by redeeming a pending grant through the `cap` device's `use` file (for a console-attached Proc), and is never conferred by `rfork` | `cap` device + `rfork` elevation-only strip; `specs/handles.tla` |
| C-22 | Every `/srv/corvus` connection carries its peer Proc's kernel-stamped identity (`stripes`, console bit, caps); corvus obtains it via `SYS_SRV_PEER` per request — never from the client, never cached on corvus's fid state | `devsrv` kernel-stamped identity + `KObj_Srv` non-transferability; `specs/corvus.tla` (`ConnOpIdentityIsKernelTruth` / `ConnOpPeerWasLive`), `specs/handles.tla` (`WalkChildIsSrv`) |
| C-23 | The kernel is corvus's sole 9P client; every `/srv/corvus` connection's transport is kernel-created and kernel-owned, never in any handle table | `SYS_POST_SERVICE` (kernel owns all transport); `specs/corvus.tla` (`ConnTransportKernelOwned`), `specs/handles.tla` (`KObj_Srv` / `SrvHandlesAtOrigin`) |

---

## 10. Implementation arc

The implementation chunks for v1.0, plus a v1.x deferral for Secure Boot. Roughly in order:

### P5-corvus-design (this chunk)

Land this document + ARCHITECTURE.md cross-references + ROADMAP.md updates. No code.

Exit criteria: user signoff (with audit findings integrated as they are here); doc committed; ROADMAP §7 reflects the next six chunks.

### P5-stratumd-multi

**Stratum-side** chunk. Add multi-stratumd-per-pool support to Stratum. Specifically:
- `stratumd --datasets-allowed <pattern>` arg that restricts the stratumd's mount + read/write to specific datasets.
- Pool-level coordination lock for metadata operations (or per-extent locks; Stratum's existing transaction layer probably handles this — verify with Michal).
- Per-stratumd crash recovery (one stratumd dying doesn't wedge the pool for others).
- Integration tests covering: simultaneous mount of multiple datasets by separate stratumd instances on the same pool.

Exit criteria: 3 stratumd processes on one pool can mount `users/michael`, `users/guest`, and `system` simultaneously; killing one doesn't affect the others; no data corruption under concurrent write workload.

This chunk lives in Stratum's repo, not Thylacine's. Stratum coordination required.

### P5-stratumd-bringup

Thylacine-side. Bring up stratumd-system at boot.

Scope:
- joey extension: parse `/boot/system.key` + `/boot/system.key.salt`; verify pool serial; fork stratumd-system with file-backed key.
- Kernel-side 9P mount of /sysroot via stratumd-system's Spoor.
- pivot_root (or chroot at v1.0; full pivot at v1.x).
- explicit_bzero the in-memory key.
- Verify boot-key device binding (C-14).

Exit criteria: Thylacine boots into a Stratum-backed root (QEMU virtio-blk-backed system pool); `/bin/ls /` shows the system tree.

Audit-bearing (joey + new mount path).

### P5-corvus-bringup

Implement corvus from scratch (NOT a port; a Thylacine-native daemon using Stratum's libstratum-crypto primitives).

Scope:
- `usr/corvus/` — new userspace crate.
- v1.0 hardening syscalls applied at startup: mlockall, set_dumpable(0), set_traceable(0), getrandom-seeded probe.
- State file format with magic `CRVS` (§4.3).
- Binary frame wire codec (§6.4).
- Verbs: AUTH, CHANGE_PASSPHRASE, SESSION_CLOSE, UNWRAP, WRAP, USER_CREATE, USER_DELETE, ADMIN_ELEVATE, RECOVER, ROTATE_KEY.
- Passphrase backend with zxcvbn-entropy gate.
- File backend (for system pool).
- Encrypted audit log.
- Rate limiter.

Exit criteria (per audit F21 resolution):
- corvus starts; serves `/srv/corvus/`.
- v1.0 hardening syscalls verified applied (test: `sys_mlockall` returned 0; `/proc/self/status` (or equivalent) shows `set_dumpable=0`; debug Spoor attach refused).
- AUTH with correct passphrase returns session token of correct length + entropy.
- AUTH with wrong passphrase refused after rate-limit window.
- UNWRAP for a dataset owned by session.user returns DEK; UNWRAP for a dataset NOT owned by session.user returns PermissionDenied.
- SESSION_CLOSE wipes RAM (verified by post-close memory dump showing zeroes in the session's slab).
- Audit log entries are encrypted; readable only after system-passphrase verify.
- Recovery flow: recovery phrase generated at user-create; phrase resets passphrase; new phrase generated.

Audit-bearing (key handling, Spoor server, Proc identity check, encrypted audit log).

### P5-corvus-syscalls (sub-chunk, can land alongside P5-corvus-bringup)

Implement the v1.0 hardening syscalls in the kernel: mlockall, set_dumpable, set_traceable, explicit_bzero, getrandom. Each is small (~50-100 LOC kernel + libt stub). Updates ARCH §11.

Exit criteria: each syscall has kernel impl + libt stub + 3-5 kernel-internal tests (success, rejection, edge cases).

### P5-corvus-srv

The `/srv/corvus/` transport (§6). P5-corvus-bringup brought corvus up over a direct joey-driven pipe harness; this macro-chunk builds the real transport. Without it corvus has no kernel-stamped peer identity, and neither login nor hostowner can gate on a peer Proc. The design (r5) converged across four adversarial design-audit rounds; it decomposes into:

**P5-corvus-srv-design** *(this sub-chunk)* — r5 into scripture (§6 rewrite + C-22 / C-23) and the two TLA+ specs (`corvus.tla` connection layer + `handles.tla` `KObj_Srv` partition), TLC-verified clean + buggy. No code. Not audit-bearing.

**P5-tsleep** — the timeout-bearing `Rendez` sleep primitive (`tsleep`, ARCH §8.8). A small kernel prerequisite: the per-connection blocking 9P ops need a deadline so a hung corvus cannot wedge its clients (§6.2). Owed for the Phase-5 `poll` / `futex` work regardless.

**P5-corvus-srv-impl-a** — the kernel side: the `devsrv` Dev + `/srv`, the service registry, `SYS_POST_SERVICE` / `SYS_SRV_ACCEPT` / `SYS_SRV_PEER`, per-connection setup, the `KObj_Srv` kobj kind, and `stripes`. Audit-bearing (new Dev, transport mediation, Proc-identity stamping).

**P5-poll** — the multi-fd wait syscall (`SYS_POLL`; ARCH §23.3). A general Phase-5 primitive, not strictly part of this transport, but its prerequisite: corvus is single-threaded (§6.2) and `poll`s its `/srv/corvus` listener plus its accepted connections, and Thylacine has no other kernel "wait on N sources" primitive. Spec-first — `poll.tla`, the gate-tied Phase-5 spec; the poller sleeps on its own private `Rendez` and registers a `poll_waiter` hook per fd. Audit-bearing.

**P5-corvus-srv-impl-b** — the corvus side: corvus becomes a full 9P2000.L server over `/srv/corvus`, retiring the joey pipe harness. Depends on P5-poll. Audit-bearing.

Exit criteria: a client opening `/srv/corvus/` reaches corvus; corvus reads the client's kernel-stamped identity via `SYS_SRV_PEER`; the identity is per-connection and cannot be rewritten by userspace, including across a 9P walk (`KObj_Srv` is non-transferable); a hung corvus fails a client's `open` with `-ETIMEDOUT` rather than wedging it.

### P5-login

Login manager that drives corvus + spawns per-user stratumd.

Scope:
- `usr/login/` — console-based login.
- Reads username + passphrase from console.
- Talks to corvus over `/srv/corvus/ctl`.
- Asks the kernel to rfork stratumd-michael (with the session token + dataset-restriction args).
- Binds the resulting user-dataset Spoor into michael's territory at /home/michael.
- rfork's a user shell.

Exit criteria: from cold-boot, user can log in as "michael" (pre-provisioned), see /home/michael populated from `users/michael`, run `rc` interactively. Logout cleanly tears down stratumd-michael.

Not directly audit-bearing; depends on audit-bearing components.

### P5-hostowner

System passphrase + admin capability flow. Split into sub-chunks; depends on P5-corvus-srv for kernel-stamped peer identity.

**P5-hostowner-a** *(landed)* — kernel foundation: `CAP_HOSTOWNER` (elevation-only, excluded from `CAP_ALL`) + `PROC_FLAG_CONSOLE_ATTACHED` + the console-attachment marker, stamped on joey at boot.

**P5-hostowner-b** — the elevation mechanism:
- The kernel `cap` device (§5.5.1) — `grant` and `use` files.
- `CAP_GRANT_HOSTOWNER` (fork-grantable; added to `CAP_ALL`); joey confers it on corvus at spawn.
- `rfork` strips elevation-only capabilities from the child.
- corvus ADMIN_ELEVATE verb: passphrase verify → register the grant.
- Admin-verb gating: USER_CREATE / USER_DELETE / ROTATE_KEY require the peer Proc to hold `CAP_HOSTOWNER` (closes the deferred P5-corvus-bringup-d finding that USER_CREATE was ungated).
- WRAP admin path: a `CAP_HOSTOWNER` peer may WRAP for any dataset (§6.5; STRATUM-API-V1.md §5.10).

**P5-hostowner-c** — the RECOVER verb + recovery-phrase flow (§5.6).

Exit criteria: a non-hostowner session cannot call USER_CREATE; a console session that runs `corvus admin elevate` registers a grant, redeems it through `cap`/`use`, and emerges holding `CAP_HOSTOWNER`; a Proc that obtains a registered grant but lacks console attachment cannot redeem it; `rfork` of an elevated Proc yields a child without `CAP_HOSTOWNER`. Audit log records elevation. The RECOVER verb resets the system passphrase from the paper recovery phrase.

Audit-bearing (the `cap` device, capability gates, the `rfork` strip, recovery surface).

### P5-kernel-update

Atomic kernel-update flow.

Scope:
- `thyla-pkg kernel-update` script.
- Stratum snapshot creation.
- ESP file rotation (current → previous; new → current; old previous deleted).
- ok-marker discipline (5-min uptime sets marker; subsequent boot checks).
- Bootloader config update.
- Documentation in USER-MANUAL.md.

Exit criteria: user runs `thyla-pkg kernel-update`, reboots, lands on new kernel; if new kernel fails, previous is one menu pick away; ok-marker discipline tested in QEMU.

Not audit-bearing.

---

## 11. v1.x deferred (explicit non-goals at v1.0)

| Item | What | When |
|---|---|---|
| P5-secure-boot | UEFI Secure Boot + signed kernel + signed initrd; MOK enrollment; TPM-NVRAM anti-rollback (audit F3, F4) | v1.1 |
| Stratum-aware bootloader | "corvid-boot" — reads /stratum/system/boot/, snapshot menu, automatic-fallback heartbeat | v1.1 |
| System pool encryption | Argon2id passphrase at boot via initrd-corvus (audit F2 extension) | v1.x |
| TPM-sealed system key | Hands-free encrypted boot | v2 |
| Per-dataset rekeying (cryptographic forward secrecy) | Real forward-secrecy via Stratum's planned rekeying primitive (audit F14) | v1.x (Stratum-paced) |
| FIDO2 / smartcard / escrow backends | Alternative auth backends per user | v1.x |
| Multi-hostowner via Shamir k-of-n | Multiple admins; threshold elevation (audit F6 extension) | v1.x |
| Network corvus / janus | Remote key escrow for fleet deployments | v2 |
| GUI login | Halcyon login screen | v1.x with Halcyon |
| Multi-session per user | Same user logged in via multiple concurrent sessions | v1.x |
| Group accounts / shared dataset ACLs | Multi-user shared datasets with permissions | v1.x |
| sshd | Remote login over network | v1.x |

---

## 12. Open questions

These need resolution during implementation but don't block scripture commit.

- **Q1 — initrd contents and size budget:** minimum joey + stratumd-system + system.key + system.key.salt + bootloader config. Size target ~5-10 MiB. Measured at P5-stratumd-bringup.
- **Q2 — pivot_root vs chroot:** v1.0 chroot is simpler but leaves the initrd RAM-resident; pivot frees it. Defer to P5-stratumd-bringup.
- **Q3 — what runs as PID 1 post-pivot:** joey continues as PID 1 (re-execs from /sbin/joey on the Stratum tree), or the kernel re-establishes PID 1 to a different binary. Recommend joey-re-exec for continuity.
- **Q4 — Halcyon login UX:** when Halcyon lands (Phase 8), does it replace `/sbin/login` or compose with it? Halcyon-side question.
- **Q5 — corvus crash → stratumd cache:** the design says per-user stratumd keeps cached DEKs through corvus restart and re-auths users on next operation requiring fresh unwrap. Specify the "stratumd detects corvus dead, refuses to re-establish without user re-auth" protocol more rigorously at P5-corvus-bringup.
- **Q6 — install-time provisioning:** the install ISO needs a flow that (a) creates the pool, (b) creates the system passphrase, (c) generates the recovery phrase + displays it, (d) creates the initial user. Design the install UX as a separate document (`docs/INSTALL-DESIGN.md`) when we get there; out of scope for this doc.

Resolved during draft (no longer open):
- ~~Q1 of prior draft — state filename:~~ resolved as `hybrid.corvus` with magic `CRVS` (§4.3).
- ~~Audit-log format:~~ JSON-line, encrypted, rotated (§4.5.1).

---

## 13. Stratum API additions required

For corvus to compose with Stratum, Stratum needs the following changes. Michal will architect the Stratum side; this document specifies the requirements at design level.

**The detailed Thylacine-side spec — including concrete CLI shapes, wire frame layouts, error codes, behavioural contracts, test matrices, and open questions — lives in `docs/STRATUM-API-V1.md`.** This section is the high-level summary; the detailed spec is the source of truth for cross-project coordination.

### 13.1 Multi-stratumd-per-pool

Stratum must support multiple stratumd processes sharing one pool's block device, with per-stratumd dataset restriction:

- **CLI**: `stratumd --pool /dev/X --datasets-allowed <pattern>...` restricts the stratumd to only mount + serve listed datasets. Reads/writes outside the allowed datasets are refused at the stratumd level.
- **Coordination**: pool-level locking for metadata operations (allocator, free-space, journal). Stratum's existing transaction model should accommodate this; verify.
- **Crash recovery**: one stratumd crashing while holding a transaction does not wedge the pool for other stratumds. Stratum's crash-recovery already handles this for single-process restarts; verify the multi-process case.
- **Audit**: Stratum-side audit log records which stratumd performed which write.

### 13.2 corvus-validated dataset access

When stratumd needs to unwrap a DEK, it calls corvus. The wire format is corvus's UNWRAP verb (§6.4), NOT Stratum's existing janus wire. Stratum-side: stratumd's "key agent client" library learns to speak corvus's wire (binary frames over Spoor; session token in every request).

### 13.3 Session-close → DEK eviction notification

When a session ends, corvus sends a notification to the affected stratumd (or stratumds) via a `/srv/corvus/notify` Spoor. Stratumd subscribes; on notify, it `explicit_bzero`s its DEK cache for the affected session's user and unmounts the user's datasets.

### 13.4 Snapshot rollback compromise marking

When corvus rotates a passphrase, it should emit an admin verb to Stratum's `/ctl/` indicating "prior snapshots of /var/lib/corvus/users/<user>/ contain a now-compromised wrap and should not be used as rollback targets." Stratum-side: a new admin verb `dataset mark-rollback-compromised <dataset> <snapshot>...` that tags listed snapshots; subsequent rollback attempts to those snapshots prompt with a warning + require explicit override.

This protects against the F13 attack (admin rolling back to a pre-passphrase-change snapshot to reactivate a compromised passphrase).

### 13.5 Boot-key device binding

`stratumd` (any mode) accepts a `--bind-pool-serial <hex>` arg. At mount time, stratumd reads the pool superblock's `pool_serial` field; if the value doesn't match `--bind-pool-serial`, refuses to mount. This is the runtime check for C-14.

The install tool writes `pool_serial` to the superblock at create time.

### 13.6 Per-dataset key rotation hooks

For v1.x cryptographic forward secrecy (audit F14): Stratum's planned rekeying primitive (`Treflink` and the rotate API) needs to be addressable from corvus. corvus's ROTATE_KEY verb (§6.4) calls a Stratum admin verb to perform the rotation. Specify the API contract when rekeying lands in Stratum.

---

## 14. Cross-references

- `STRATUM-API-V1.md` — Thylacine-side detailed spec of the Stratum API additions enumerated in §13 of this doc. The detailed version with CLI shapes, wire frames, error codes, test matrices, and open questions.
- ARCHITECTURE.md §5 — boot sequence (this doc updates).
- ARCHITECTURE.md §11 — syscall surface (this doc adds: sys_mlockall, sys_set_dumpable, sys_set_traceable, sys_explicit_bzero, sys_getrandom; and §11.2c SYS_POST_SERVICE, SYS_SRV_ACCEPT, SYS_SRV_PEER for the §6 transport).
- ARCHITECTURE.md §8.8 — `tsleep`, the deadline-bounded `Rendez` sleep this doc's §6.2 transport depends on.
- ARCHITECTURE.md §9.4 — the `devsrv` Dev + `/srv` (this doc's §6.1).
- ARCHITECTURE.md §14.3a — Stratum mount lifecycle (this doc updates: system pool is integrity-only at v1.0; corvus + per-user stratumd post-pivot).
- ARCHITECTURE.md §15 — capabilities (this doc adds CAP_HOSTOWNER, CAP_GRANT_HOSTOWNER, CAP_LOCK_PAGES, CAP_CSPRNG_READ, the console-attachment bit, and the `cap` device).
- ARCHITECTURE.md §18 — kernel object kinds (this doc's §6.1 adds `KObj_Srv`, non-transferable).
- ARCHITECTURE.md §28 — global invariants list (this doc adds C-1..C-23; cross-reference from §28).
- ROADMAP.md §7 — Phase 5 chunk list (this doc adds the corvus-arc chunks; v1.x extensions in §11 here become future-phase entries in ROADMAP).
- `stratum/v2/docs/OS-INTEGRATION.md` — Stratum's expectations.
- `stratum/v2/src/janus/backend_passphrase.c` — design influence (not a port).
- Plan 9 manual: factotum(4), secstore(1), authsrv(8) — historical reference.

---

## 15. Naming rationale

Per CLAUDE.md "Thematic naming — keep an eye out":

`corvus` from *Corvus* (Latin genus name for ravens and crows). The naming pattern matches `janus` (Latin/Roman mythology) — same shape, same family, easy to read alongside.

Semantic fit:
- **Memory + intelligence**: corvids are documented tool-users and have multi-year spatial/social memory. The "this thing remembers your secrets accurately" semantic is empirically grounded.
- **Hoarding**: corvids cache food / objects for later retrieval. The "this thing holds your keys and returns them on request" semantic is direct.
- **Norse mythology**: Huginn (thought) and Muninn (memory) — Odin's ravens. A key agent that remembers secrets sits cleanly in that lineage.
- **Australian fauna**: *Corvus coronoides*, *C. mellori*, *C. bennetti* — all native, all outback-resident.

The folkloric "ravens hoard shiny things" meme is technically debated by ornithology but strong enough culturally to make the name *click* on first read.

Daemon binary: `corvus` (lowercase, matches `janus`).
Tree: `/srv/corvus/`.
State directory: `/var/lib/corvus/`.
Audit log: `/var/log/corvus.audit.gz`.
State file magic: `CRVS` (LE u32 = 0x53565243; distinct from Stratum janus's `JPAS` to prevent cross-loading).
Three-letter abbreviation in code: `crv` (matches `stm` for Stratum).

**The `cap` device** (§5.5.1, P5-hostowner-b) keeps its Plan 9 name. `cap` / `capability` is Plan 9-derived (the `cap(3)` device); per CLAUDE.md's naming discipline, Plan 9-derived concepts are not thematically renamed. Its files are `grant` (register) and `use` (redeem) — `use` mirrors Plan 9's `capuse`; `grant` replaces `caphash` because the Thylacine device carries no hash (the `grant` write is capability-gated, §5.5.1). `CAP_GRANT_HOSTOWNER` follows the established `CAP_*` convention.
