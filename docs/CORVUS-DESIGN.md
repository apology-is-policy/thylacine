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

Stratum needs new APIs to compose with corvus (per-user stratumd; corvus-validated `Tattach.afid` — but actually superseded by Option A per §3 D4; etc.). Those are enumerated in §13. The user explicitly committed to evolving Stratum to fit Thylacine's needs.

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
├── peer/                                  ← kernel-stamped, unforgeable (§6.2)
│   ├── proc                  (R)   peer Proc's PID + identity tag
│   └── caps                  (R)   peer Proc's capability bits
└── ops/                                   ← admin / control verbs
    ├── unwrap                (RW)  per-user-stratumd → corvus DEK unwrap (BINARY FRAMES)
    ├── change-passphrase     (W)   per-user passphrase rotation (BINARY FRAMES)
    ├── user-create           (W)   admin op
    ├── user-delete           (W)   admin op
    ├── rotate-key            (W)   per-dataset key rotation
    └── recover               (W)   recovery-phrase-based system passphrase reset
```

The `/ops/unwrap` verb replaces what was previously called "Stratum janus wire compatibility." It is a Thylacine-native verb that carries:
- Session token (so corvus can verify session → user → dataset chain).
- Dataset ID + key_id (the targeting).
- Wrapped DEK blob (passed through from Stratum's keyschema).

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
       |  ask corvus to wrap it under new user's keypair → wrapped_dek
       |  store wrapped_dek in dataset metadata
done. user can now log in.
```

User deletion is symmetric. The dataset destruction is a Stratum operation; corvus removes the user's state files; audit log records.

### 5.5 Hostowner elevation

```
[console user] runs: corvus admin elevate
       |
       v
[client]  BINARY-FRAME admin-elevate { session_token, system_passphrase }
       v
[corvus]
       |  verify session is from a console-attached Proc
       |  argon2id(system_passphrase, system_salt) → system_KEK
       |  AEAD-unwrap system-wrap → verify magic + tag (cheap admin-keypair unwrap)
       |  explicit_bzero passphrase + KEK + admin-keypair
       |  grant CAP_HOSTOWNER to session (capability set bumped on the Spoor)
       |  audit-log: admin-elevate sid=... user=...
       |  return OK
```

A session WITHOUT CAP_HOSTOWNER (i.e., any session not elevated) cannot execute admin verbs. The verb authorization is checked **per-call** on the session's current cap set.

**Console-attachment check**: corvus reads the peer Proc's identity from the kernel-stamped `/srv/corvus/peer/proc`; the kernel marks a Proc as "console-attached" iff it was spawned from joey's console-login chain. Future sshd / remote-login chains do not get this mark. (Implementation: a console-bit in the Proc's capability set, set at fork-time by joey and never propagatable across an rfork to a different territory.)

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

## 6. Transport: Spoor with Proc identity

### 6.1 corvus listens on a Spoor

corvus owns `/srv/corvus/` — a kernel-mediated 9P tree backed by a userspace Dev hosted by the corvus daemon. The kernel uses the same Spoor-pair-as-transport mechanism that Phase 5 built for stratumd: corvus is a 9P *server* in userspace; clients reach it via dev9p-shape mediation.

### 6.2 Proc identity binding

Each Spoor open to `/srv/corvus/` carries a kernel-stamped peer identity: PID, capability set, console-attachment bit, plus an unforgeable per-Proc identity tag generated at Proc creation. Corvus reads these via the synthetic file `/srv/corvus/peer/`. The identity is set by the kernel when the Spoor is created and **cannot be rewritten by userspace** — including by cross-Proc transfer via 9P (the new holder gets THEIR own identity, not the prior holder's).

The Proc identity tag (a 64-bit value) is the kernel's distinct-Proc identifier; corvus uses it as the binding key for sessions. A session is "owned by" the Proc with identity tag T; only T can issue close. If T's Proc exits, the session is auto-closed (corvus listens on Proc-death notifications via a kernel admin Spoor).

### 6.3 Cross-Proc session transfer

A login session can be transferred to a child Proc (e.g., login → user shell → user app). The 9P RIGHT_TRANSFER mechanism (ARCH I-4) governs this. On transfer:
- The new holder Proc inherits the session's **user identity binding** (so corvus's owner check passes for that user's datasets).
- The new holder's **capability set is its own** — NOT the parent's session caps. The session's caps + the Proc's caps are **orthogonal** (audit F10 resolution).

**Authorization rule** (audit F10 + new C-11): for any operation against corvus, the auth check is "does this operation require Proc capability X? Then check the Proc has X. Does this operation require session ownership over user Y? Then check the session's bound user == Y." Operations check the auth they need; they do not check the cross-product. A CAP_HOSTOWNER Proc that inherits michael's session can execute admin verbs (because it has CAP_HOSTOWNER) AND unwrap michael's DEKs (because the session owns michael). **This is feature, not bug** — admins can read user data on Plan-9-style systems; we make this property explicit.

### 6.4 Wire format: binary frames

`/srv/corvus/ctl` and `/srv/corvus/ops/*` use **binary frames**, not line-based text (audit F8 resolution). Every frame:

```
[0]      verb_id           u8  (see verb table)
[1..3)   payload_len       u16 LE
[3..)    payload           verb-specific
```

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

Response frame:

```
[0]      status            u8  (0=OK, 1=BadAuth, 2=PermissionDenied, 3=NotFound, 4=RateLimited, 5=BadFormat, 6=InternalError)
[1..3)   payload_len       u16 LE
[3..)    payload           status-specific (e.g., session token on AUTH OK, DEK on UNWRAP OK)
```

The recv buffer for any frame carrying a passphrase / DEK is mlock'd. Corvus calls `sys_explicit_bzero` on the buffer immediately after parsing the relevant fields into typed storage and again after the typed storage is consumed.

**No newline / shell-injection hazards**: binary frames don't parse text; passphrases can contain any byte except length-overflow. A passphrase containing `\n` or `\0` or `=` works fine.

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

These are runtime + audit guarantees at v1.0. A future `specs/corvus.tla` formalizes the session state machine + capability arithmetic.

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

---

## 10. Implementation arc

Seven chunks for v1.0, plus a v1.x deferral for Secure Boot. Roughly in order:

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
- Verbs: AUTH, CHANGE_PASSPHRASE, SESSION_CLOSE, UNWRAP, USER_CREATE, USER_DELETE, ADMIN_ELEVATE, RECOVER, ROTATE_KEY.
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

System passphrase + admin capability flow.

Scope:
- Define `CAP_HOSTOWNER`; add to caps.h.
- corvus ADMIN_ELEVATE verb.
- Console-attached login can elevate via system passphrase.
- Console-attachment kernel bit on Proc capability set.
- Recovery flow (RECOVER verb) implemented.
- Admin verbs (user-create, snapshot, kernel-update) require CAP_HOSTOWNER.

Exit criteria: non-hostowner session cannot call user-create; console session that runs `corvus admin elevate` can. Audit log records elevation. Recovery phrase generated at install displays once; recovery verb successfully resets passphrase using paper-recovery-phrase.

Audit-bearing (capability gates, recovery surface).

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
- ARCHITECTURE.md §11 — syscall surface (this doc adds: sys_mlockall, sys_set_dumpable, sys_set_traceable, sys_explicit_bzero, sys_getrandom).
- ARCHITECTURE.md §14.3a — Stratum mount lifecycle (this doc updates: system pool is integrity-only at v1.0; corvus + per-user stratumd post-pivot).
- ARCHITECTURE.md §15 — capabilities (this doc adds CAP_HOSTOWNER, CAP_LOCK_PAGES, CAP_CSPRNG_READ, plus console-attachment bit).
- ARCHITECTURE.md §28 — global invariants list (this doc adds C-1..C-20; cross-reference from §28).
- ROADMAP.md §7 — Phase 5 chunk list (this doc adds seven chunks; v1.x extensions in §11 here become future-phase entries in ROADMAP).
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
