# Thylacine Identity, Access & Privilege — Design Input (working)

**Status:** **BINDING SCRIPTURE** — promoted 2026-05-28 (user-signed-off). All
design forks F-0..F-7 (§5) are resolved; the five-axis model (§1), the legate /
clearance elevation model (§3.1), and the identity hybrid (§3.3) govern
implementation. §8 defines the **convergence detour** (the binding arc this work
belongs to) and its governing bar. Deviations update this document first; no code
lands that contradicts it. Follows the CLAUDE.md "design → scripture commit → then
code" pattern.

**Author of record for the design conversation:** Michal + Claude, 2026-05-28.

---

## 0. Why this arc exists

The Utopia shell arc (Phase 7) was paused at U-6d-a (tip `861c871`). Reason: the
shell's *next* features — output redirects, coreutils, the identity-aware prompt,
`$HOME` / `cd ~`, `chmod` / `chown` / `ls -l`, owner-scoped `kill` / `ps` — all sit
on an identity + permission + privilege substrate that **does not yet exist at the
Thylacine layer**. We discovered this organically: the redirect-create
investigation walked straight into "create a file owned by *whom*, on *whose*
authority?" and found no answer.

What we found absent (verified in-tree, 2026-05-28):
- **No user identity in the kernel.** No `uid`/`gid`/`owner`/`uname` on a Proc.
  The only per-Proc identity is `stripes` (a capability tag) + the console bit.
- **No permission mutation.** No `chmod`/`chown`/`wstat`/`setattr`. `mode` is
  observable read-only via `struct t_stat`; nothing sets or changes it.
- **No current-user concept** to enforce permissions against. The live system is
  effectively single-user (joey-as-init → one system stratumd → pivot → probes).

What is *already built* and this arc composes with:
- **corvus** (key agent; passphrase auth; per-user wrap chains; mlock'd; `/srv/corvus`).
- **`stripes`** — kernel-stamped, unforgeable per-Proc identity via `SYS_SRV_PEER`.
- **The `cap` device + `CAP_HOSTOWNER`** (elevation-only) + `CAP_HW_CREATE`.
- **Per-Proc Territory** (namespace; I-1 isolation, I-3 mount DAG).
- **Handle rights** (`RIGHT_*`; I-2 monotonic reduction on rfork, I-6 on transfer).
- **Per-user stratumd** — Stratum ask **A2 is DELIVERED + audited** (coordinator
  model, `stratumd --role coordinator|client`; see STRATUM-API-V1-RESPONSE.md).
  The multi-user foundation is **not blocked upstream**.

This arc **absorbs the suspended Phase 5 tail** (P5-login, per-user stratumd
wiring, P5-hostowner-c) and adds the new Thylacine-side identity/permission
surface. See §6.

---

## 1. The five-axis model (backbone)

The load-bearing principle: keep these **five axes strictly orthogonal**, each
doing exactly one job. Linux's pain comes from overloading (`uid` is identity
*and* ownership *and* privilege-via-0; capabilities partly overlap identity;
namespaces bolted on late; MAC layered on top). Plan 9 is cleaner but
under-specified for our needs. Thylacine keeps them clean:

1. **Visibility** — the *Territory* (namespace). What you can *name*. Per-Proc.
   The *first-class* access control: "Susan can't see Michal's mount" = "it isn't
   in Susan's territory." (I-1.)
2. **Identity** — *who you are*: an authenticated principal (name + group
   memberships), established by corvus at login, carried as an unforgeable token
   (`stripes` is the kernel handle). What the kernel rwx check + Stratum's dataset-scope enforce against, and what
   privileged actions are *attributed to*.
3. **Permission** — what you may *do to a named object*: owner/group/other rwx
   (or ACL), enforced **by the Thylacine kernel at the FS-access chokepoint** (the
   Dev / 9P-client layer — an `inode_permission`-equivalent) against your
   `principal_id` + groups, on the mode/uid/gid the backing provides (Stratum/ext4
   via Rgetattr) or the mount-cape synthesizes (permissionless backings). **This is
   the Linux-VFS model — a conscious deviation from Plan 9's server-enforces model**
   (rationale + the Stratum finding that forced it: §3.7). Dataset-scope (Stratum;
   coarse, attach-time) is the *outer* defense-in-depth boundary; the kernel rwx
   check is the *inner* one. A backing that also enforces makes the kernel check a
   floor, never a conflict.
4. **Capability** — what *privileged operations* you can perform: the `CAP_*` set
   + handle rights. Orthogonal to identity — "michal" with or without
   `CAP_HW_CREATE`. **Elevation = gaining a capability, scoped and audited, NOT
   becoming a different user** (no uid 0, no setuid).
5. **Confidentiality** — encryption-at-rest: corvus + per-dataset DEKs. Protects
   the *offline/disk* attacker; orthogonal to permission, which protects the
   *online* attacker. (Scenario S3 proves these must be separate.)

Rule: each scenario decomposes into these five; never let one axis silently do
another's job.

---

## 2. SOTA verdict (researched 2026-05-28)

Per-axis, the model is **at-par with the current gold standard** (not behind):

| Axis | SOTA gold standard | Thylacine | Verdict |
|---|---|---|---|
| Visibility | Fuchsia per-component namespace; Plan 9 | per-Proc Territory | at-par (Fuchsia *independently reinvented* Plan 9 ns) |
| Identity / no-root | Fuchsia & run0 (no setuid / no ambient auth); macOS no-root-by-default | `stripes` + keyless hostowner, no uid 0 | at-par; ahead of Linux |
| Capability | seL4 & Fuchsia ocap handles; Capsicum | handle rights + I-2 reduction | at-par (ocap family) |
| Elevation (JIT) | AWS STS AssumeRole, Azure PIM, Windows 11 JIT, run0 | legate + clearance levels (§3.1) | at-par mechanically; **novel as a kernel primitive** |
| Confidentiality | FileVault/fscrypt/LUKS **+ TPM/Secure Enclave** | corvus + DEKs (mlock'd RAM) | at-par on model; **behind on hardware-backed storage** |

**Novelty (honest):** the JIT/clearance machinery lives entirely in the
enterprise-IT/PAM control plane (CyberArk, Azure PIM, Windows 11 Admin
Protection) and cloud IAM — **not in OS kernels**. The capability-OS world
(seL4, Fuchsia, Genode/Sculpt) has ocap + namespaces but **no multi-user login +
human JIT-elevation model**. So no shipping general-purpose, multi-user OS
unifies all five axes with JIT scoped/ephemeral elevation as a *first-class
kernel primitive*. Each ingredient is individually proven; the **integration is
new** — the de-risked kind of novel (proven parts, novel synthesis).

**Where SOTA is honestly ahead (→ v1.x, not design flaws):**
- Hardware-backed secret storage (TPM / Secure Enclave / Credential Guard). corvus
  is mlock'd RAM only. Design the corvus key interface so a hardware backend slots
  in later without re-architecting.
- Hardware memory-capabilities (CHERI/Morello) — orthogonal layer; would later
  harden the S4 DMA hazard specifically. Far-future hardware substrate.
- Formal-verification depth (seL4 is functionally proven; we do TLA+ + audits).
  Not apples-to-apples (seL4 is mechanism-only), but they're ahead on assurance.

**Ideas adopted from SOTA (folded into the model):**
- **STS intersection semantics**: a legate's caps = (clearance max) ∩ (optional
  per-activation self-restriction). Just I-2 monotonic reduction at activation.
- **pledge/unveil/Capsicum reduction as the dual primitive**: privilege
  *reduction* is as valuable as elevation. We have reduction-on-rfork (I-2); add a
  voluntary in-process drop for untrusted-code sandboxing (S5).
- **PIM "eligible vs active" + re-auth-once-per-session** ergonomics — mirror the
  familiar UX.

Sources: CSA (JIT/ZSP), AWS STS docs, Azure PIM docs, systemd run0, Fuchsia
"Secure", seL4, Genode/Sculpt, OpenBSD pledge/unveil (arXiv 2405.06447), CHERI
(Cambridge), Windows 11 Administrator Protection JIT.

---

## 3. Locked decisions

### 3.1 Elevation — the *legate* model (F-3: RESOLVED)

**Hostowner** is a *keyless authority*, not a user:
- Set up at install (physical presence): the hostowner sets the system password;
  the system key is created. **Hostowner has no account and cannot log in.**
- Hostowner authority == possession of the system key + console attachment.
  Exercised at install (physical) or live (via the manage-clearance level, which
  is itself gated by the system key).
- Kills the "root account to phish / ssh-into." Strengthens the existing
  `CAP_HOSTOWNER` model.

**Clearance level** = a **policy object**, granted to users (eligibility) by
hostowner; kept **few and coarse** (e.g. hw-dev, fs-admin, user-admin,
clearance-admin):

```
clearance_level := {
    name,
    caps          : set of CAP_* granted while active,
    auth_required : re-auth | distinct-secret | system-key | hostowner-co-sign,
    time_bound,
    scope_kind,
}
```

`auth_required` scales with stakes: low-stakes → re-auth with your own login
secret; high-stakes (hw, kernel) → a distinct per-level secret; the top
(manage-clearance) → the system key. This generalizes "every level has a
passphrase" → "levels declare how hard they are," avoiding passphrase-fatigue.

**Becoming a legate** (just-in-time elevation):
- A logged-in user can list clearance levels + their caps.
- The user requests a level (or a sequence) for an **execution scope**, satisfying
  each level's `auth_required` (via a **trusted path** for high-stakes — console +
  secure-attention mediated by corvus; never a spoofable in-shell prompt).
- On success: an **ephemeral, in-memory principal forked from the durable user**:
  - **Identity stays the durable user** (michal) — for *attribution* (audit reads
    "michal, via legate-X, did Y") and *ownership-on-create* (files he makes are
    owned by michal). The legate carries the clearance-set + an ephemeral
    session-id as annotation. Do **not** mint a fresh disconnected principal.
  - **Capabilities gain the clearance's set**, optionally **intersected** with a
    per-activation self-restriction (STS-style; I-2).
  - **Bounded to the execution scope**; evaporates on scope exit.
- **Execution scope (v1.0) = the legate Proc + its descendants + an optional time
  bound.** Caps stamp on the legate Proc, attenuate to children (I-2), gone on
  exit. *Resource-scoped* caps ("write only this file / MMIO only this device")
  are a **v1.x** refinement. **The differentiator to design carefully: scope =
  capability ∩ namespace ∩ time** — tighter containment than cloud PIM (which has
  no namespace axis).

**Grant / revoke** via corvus per-user wrap chains: grant = level-X unlock
material wrapped under the user's key; revoke = delete that wrap (per-user, no
shared-secret rotation). An active legate keeps already-stamped caps until
scope-exit; revocation blocks *future* elevation.

**Local enforcement = kernel cap-stamp + `stripes`.** No cryptographic clearance
proof for the local case — it would duplicate what the kernel cap-stamp + the
unforgeable `stripes` peer-identity already provide. **Cryptographic clearance
proof is reserved for the distributed / CPU-server case** (a legate acting on a
remote server that does not trust the local kernel) and is designed then.

**Axis hygiene:** a clearance secret is an **authorization factor**, NOT a
data-encryption key. Clearance (axis 4) and DEKs (axis 5) stay separate.

**Three locked decisions:**
1. Execution scope = process-subtree + time-bound for v1.0 (resource-scoping v1.x).
2. Clearance level = policy object with a per-level `auth_required`.
3. Defer cryptographic clearance proof to the distributed/CPU-server design.

**SOTA grounding:** ≈ AWS STS AssumeRole (temporary, scoped, intersection) +
Azure PIM (eligible vs active, MFA, time-bound) + run0 (no-setuid, broker,
trusted-path/new-PTY) + Fuchsia/seL4 (ocap, no ambient authority) + Plan 9
(namespace, factotum, cap device, no-setuid). Novel = the synthesis as a
multi-user OS kernel primitive.

### 3.2 Permissionless backings (F-1: RESOLVED)

Every FS backing **must speak 9P** (already our posture). A backing with no native
permission model (FAT32, a raw network share) **self-declares as non-POSIX** (a
flag in the version/attach handshake or a `/ctl` probe — exact mechanism TBD in
spec). For such mounts, Thylacine imposes a **uniform owner/group/mode for the
whole mount** — a lightweight *mount-cape* (policy on the mount node, set by the
mounter, checked at the namespace layer).

- Permission-*aware* servers (Stratum) self-enforce; Thylacine imposes nothing.
  The overlay fires **only** for self-declared-permissionless backings.
- Accept the limitation: per-file divergence inside a FAT mount is impossible
  (one mode for the subtree). Satisfies S1 (mount group=A, group-r, owner-rwx).
- Consequence: the namespace layer gains a *uniform* permission check for these
  mounts (it currently does pure visibility). Much simpler than per-path policy.
  **(Update 2026-05-28: the mount-cape is the permissionless-backing metadata
  source within the unified kernel permission layer — see §3.7; F-1 is subsumed
  into that layer, not a separate mechanism.)**

### 3.3 Identity — the hybrid model (F-0: RESOLVED 2026-05-28)

A principal has **two separate fields**: a durable **identity** (who — for
attribution, ownership, permission-enforcement) and a dynamic **authority** (the
capability set + namespace — what can be done now). They travel together on a
running Proc but never collapse. Forced by F-3: the legate ("same human, more
capability") is only expressible if identity and authority are separate fields —
incompatible with Linux's uid-is-authority and with pure ocap's no-durable-identity.

- **Durable identifier = an opaque principal-id.** The kernel deals in a
  fixed-size id; **corvus is the authoritative resolver** for
  id ↔ name ↔ groups ↔ keys ↔ clearance-eligibility. Matches 9P2000.L numeric
  uid; supports rename (name is an attribute); fast kernel compares. (Rejected:
  name-string-as-identity — awkward in-kernel; rename = identity change.)
- **Groups are first-class.** Both permission (file group-perm) and
  clearance-eligibility (hostowner grants to a group) may target a user OR a
  group. Groups are the reusable indirection.
- **`stripes` = the per-Proc runtime credential**: an unforgeable token stamped
  with `{principal-id it acts as, current capability set, clearance annotations}`.
  principal-id is the durable identity (corvus-authoritative). A legate = same
  principal-id, fresh stripes-context carrying the added caps + scope. (Thematic
  fit: stripes are the markings that identify the individual thylacine.)
- **Anti-tangle invariant (I-22 candidate):** *no identity carries ambient
  super-authority.* There is no superuser identity. Hostowner is an **authority
  source** (the system key), not an identity. Every identity's only ambient
  authority is its minimal baseline (its own home/territory + its own stratumd);
  all elevated power comes solely via the legate (scoped, audited). This prevents
  decay back into uid-0.
- **Distributed face (v1.x):** a corvus-issued verifiable credential
  (`{id, groups, caps, expiry}`; SPIFFE/macaroon-shaped) carries the same identity
  to remote 9P / CPU-servers. Consistent with deferring crypto-proof to the
  distributed case (local = kernel-stamped stripes).

### 3.4 Permission vocabulary (F-2: RESOLVED 2026-05-28)

v1.0: **mode-bits** (owner/group/other rwx) as the durable per-file baseline —
9P2000.L carries it natively, Stratum stores it, universal tooling, and it is the
F-1 mount-cape's vocabulary too. **Dynamic / cross-user sharing uses the namespace
+ capability axes** (Plan 9-native: bind a subtree into a sharee's namespace with
the access the bind grants), NOT per-file ACLs. The awkward multi-group case ("A
reads, B writes") = namespace composition, not an ACL.

**The durable-ACL door is kept open for v1.x** (user direction 2026-05-28:
"enough, but don't close the door"). This is nearly free, by architecture:
- The kernel **does not enforce file permissions** (server-enforced, Plan 9
  model) — it is inherently agnostic to the vocabulary. Durable ACLs are a Stratum
  + management-ABI concern; **no kernel-enforcement change** is needed to add them.
- ACLs ride the **9P2000.L xattr family** (Stratum supports xattrs end-to-end) —
  the wire is already ACL-capable.
- Guardrails so we don't accidentally close the door: the native `t_stat` +
  chmod/chown/wstat ABI is **versioned / reserved-extensible**; the create path
  (F-5) must not preclude default-ACL inheritance; nothing kernel-side hard-codes
  "permission == owner + group + 9 bits".

### 3.5 Ownership-on-create + 9P identity presentation (F-5 + F-4: RESOLVED 2026-05-28)

**F-5 — ownership-on-create.** `SYS_WALK_CREATE` stamps:
- owner = the caller's **durable principal-id** — *never* the clearance/legate
  annotation (F-3: a legate's files are owned by the durable user).
- group = the **parent directory's group** (Plan 9 / BSD inheritance —
  collaboration-friendly; the natural partner to F-2's namespace-sharing).
- mode = a default, masked by a umask-equivalent.
- Intended consequence: create cannot stamp an owner until identity exists →
  `SYS_WALK_CREATE` is formally gated on login. The original redirect-create
  thread lands here.

**F-4 — 9P identity presentation.** Trust spectrum:
- **Trusted-local** (a corvus-provisioned per-user stratumd, scoped to one
  principal's datasets per Stratum A2): the kernel **forwards the principal-id as
  `n_uname`, asserted** — safe because that server only serves that principal.
- **Trust gate:** corvus **stamps the `/srv` posting** at provision time (or
  registers it with the kernel); the kernel **refuses to forward an asserted
  identity to any server lacking that stamp**.
- **Untrusted / remote** (multi-principal or cross-machine): attaches as **`none`**
  (unauthenticated, minimal privilege) in v1.0. Authenticated presentation (the
  corvus-signed verifiable credential / `Tauth`) is **v1.x** — same deferral as the
  distributed credential + CPU-server scenario.
- v1.0 boundary: the filesystem is local stratumds; cross-machine authenticated
  9P is not a v1.0 deliverable.

### 3.6 Hardware-capability granularity + ad-hoc elevation gate (F-6 + F-7: RESOLVED 2026-05-28)

Confirmed in-tree 2026-05-28: **no SMMU/IOMMU in the kernel** — DMA is unconstrained
at v1.0; a `KObj_DMA` holder can read/write *any* physical memory. HW caps are
therefore kernel-corruption-grade power.

**F-6 — granularity.** v1.0 keeps **`CAP_HW_CREATE` blanket.** Resource-scoping (a
per-device PA/IRQ/DMA allowlist) is deferred to **v1.x** — it is a specific
instance of the resource-scoped-capability refinement already deferred in F-3
(v1.0 execution scope = process-subtree + time). Forcing a device allowlist into
v1.0 would contradict that. Containment for the ad-hoc case comes from the legate's
process-subtree + time bound (F-3) + the dev-mode gate (F-7), not an allowlist. The
HW-capability representation is kept **allowlist-extensible** (door open). Vetted
boot drivers (stratumd) already use blanket `CAP_HW_CREATE` — status-quo-consistent.

**F-7 — gate.** Ad-hoc legate HW elevation requires an explicit **dev-mode**: a
persistent, hostowner-set, audited machine flag declaring "this is a development
box." HW-cap clearance activation is **refused outright when dev-mode is off**, even
with the correct secret. **Boot-time HW grants (`kproc → joey → stratumd`, the
vetted boot chain) are exempt** — dev-mode gates only the interactive/legate path.
Rationale: a phished hostowner secret alone must not yield "DMA anywhere"; the
machine must be *declared* a dev box.

**v1.x — constrain DMA (retires the core F-7 risk):** an ARM **SMMUv3** driver + a
DMA-API change (handles carry device-IOVAs, not raw PAs) + per-device SMMU contexts
tied to `KObj_DMA` lifecycle. This is the VFIO-equivalent that makes the
userspace-driver model safe against a buggy/hostile driver — letting the kernel
stop *fully trusting* its drivers. See §2 (hardware-substrate v1.x list).

**REVISED 2026-05-28 (roadmap-detour decision):** SMMU is being **pulled into
v1.0** (the detour's stop B). Two reasons: (a) with SMMU in v1.0 the **F-7
dev-mode flag is fully throw-away** — A's clearance-level policy + resource-scoped
HW caps (which SMMU makes enforceable) subsume both *authorization* and
*containment*, so the dev-mode flag is **dropped, not built**; (b) SMMU fixes the
foundational "kernel fully trusts every userspace driver" gap (itself a C-class
real miss). Cost: longer detour; a v1.x→v1.0 shift. A (identity) and B (SMMU) are
largely independent; the resource-scoped-HW-cap refinement completes on top of
both.

### 3.7 Permission enforcement: the kernel-VFS model (axis 3; RESOLVED 2026-05-28)

**The Stratum finding that forced this** (agent + spot-verified 2026-05-28): a 9P
server is not guaranteed to enforce file rwx, and our reference server does not.
Stratum performs **no per-file owner/group/other rwx check at all** (exhaustive
grep: zero permission idioms in its FS/9P layer; open/read/write gate only on
open-mode consistency); `Tsetattr` chmod/chown is **unconditional** (`server.c:2544`,
no owner check); identity is the socket **`SO_PEERCRED` uid, not the 9P `n_uname`**
(which it ignores); and there is **no root bypass** (vacuously — no rwx check
exists). What Stratum *does* enforce is **dataset-scope** (which uid may attach
which dataset subtree; refusal = `Rlerror(EACCES)` at Tattach). This is
intentional: Stratum is also a standalone portable encryptor (VeraCrypt-shaped) and
must stay free of any one consumer's identity model.

**Decision: Thylacine enforces rwx in the kernel, at the FS-access chokepoint — the
Linux-VFS model, not Plan 9's server-enforces model.** Two decisive reasons:
1. **Heterogeneous backings.** ext4, FAT, and foreign 9P servers do not (or cannot)
   self-enforce. The only layer uniform across all backings is the Thylacine kernel,
   which already mediates **every** FS op (userspace never talks to a backing
   directly). That chokepoint is exactly where Linux's VFS enforces; ours is
   airtight by construction.
2. **Storage-tool independence.** Pushing enforcement into the server would couple
   Stratum (and every backing) to Thylacine's identity model. Keeping the policy in
   the OS keeps storage tools portable.

**Mechanism — one uniform kernel permission layer.** At walk/open/create the kernel
runs an `inode_permission`-equivalent: the file's mode/uid/gid (from the Dev's
`stat` — Rgetattr for `dev9p`, the in-kernel table for `devramfs`, the **mount-cape**
for permissionless backings) is checked against the Proc's `principal_id` + groups.
**The F-1 mount-cape is no longer a special case — it is simply the metadata source
for permissionless backings within this one layer.** No identity is special (I-22):
no `principal_id` bypasses the check (we never special-case a uid — unlike a server,
which we would otherwise have to trust not to). Audit-bearing (privilege boundary;
lands in Stop A-2).

**Conscious heritage note.** On axis 3, Thylacine is **Linux-shaped (the VFS
enforces)**, not Plan-9-shaped (the server enforces). Deliberate: axis 1 (namespace)
stays pure Plan 9; axis 4 (capability) is Fuchsia/seL4; axis 3 takes the Linux-VFS
model because it is the only one that survives heterogeneous backings. Each axis
takes its best ancestor — the project's "combine Plan 9 + Fuchsia + seL4" identity,
made honest about axis 3.

**Within-dataset reality + seam.** Stored mode bits are now *enforced* by the
kernel (not advisory). `chmod`/`chown` manage those stored bits via `Tsetattr`;
because Stratum applies them unconditionally, the **ownership-change policy
(only-owner-may-chmod, privileged-chown) is enforced kernel-side too** (the same
permission layer), not by Stratum. ACLs remain the v1.x seam (the kernel layer
reads them when present). Dataset-scope presentation (the `SO_PEERCRED`-equivalent
for Stratum's outer boundary) is the A-3 cross-project seam.

---

## 4. Scenario catalog

Briefs use the Plan 9 / Linux / Thylacine shape. S1–S4 walked 2026-05-28; S5+ are
the remaining walks (brief any in full on request).

- **S1** — Michal mounts FAT32 / network-9P in his namespace; default-deny Susan;
  read to group A; rwx to hostowner; a group-A user elevates to modify. *Touches
  all five axes; drove F-1 (resolved), F-2, F-3 (resolved). Visibility is free
  (Susan's territory lacks the mount).*
- **S2** — Susan's home is a generic 9P mount. *Pins F-4: identity presented to a
  9P server must be authenticated (corvus token / Tauth) for untrusted servers,
  not an asserted `n_uname`; trusted-local (per-user stratumd) may assert because
  corvus provisioned it.*
- **S3** — Stratum unencrypted volume; Susan still must log in to write files she
  owns. *Proves axes 3 (permission) ⊥ 5 (encryption). Login establishes identity;
  the server enforces permission against it regardless of encryption. Drives F-5:
  create stamps owner = authenticated caller.*
- **S4** — Michal elevates to test a kernel driver; can hostowner grant HW caps?
  *Mechanically yes (cap device). Advisable only with scoping + audit +
  containment — DMA is the sharp edge (no IOMMU at v1.0). Drives F-6
  (resource-scoped HW caps) + F-7 (dev-mode gate).*

**S5+ — remaining scenarios to close the space (categorized):**
- Identity edge cases: unauthenticated principal (`none`), service identity
  (corvus/stratumd run as what?), machine identity (remote mutual auth), same user
  two sessions.
- Groups & roles: who creates/administers groups; primary vs supplementary;
  per-machine vs portable across a Thylacine network.
- Permission depth: default perms on create (umask), inheritance, append-only /
  immutable / sticky, **explicit rejection of setuid**.
- Capability lifecycle: delegation (I-2), revocation, time-bound, the grant graph.
- Cross-user interaction (big gap): share file/dir; connect to another user's
  `/srv`; **send a note/kill across users**; **/proc (devproc) visibility**.
- Mount governance: who may mount a USB FAT / remote 9P; raw block-device access
  as a capability; bind within own namespace (free) vs new backing (gated).
- Daemon & persistence: survive-logout daemons; `/srv` ownership across logout;
  logout teardown (A4 DEK eviction + reap + mount teardown).
- Device access: input devices (keylogging), GPU/framebuffer, console/PTY, network.
- Network / distributed: Thylacine as a 9P *server* (who attaches as whom);
  **CPU-server model** (run on a remote Thylacine *as yourself* — Plan 9's killer
  feature; our namespace+capability model is ideally shaped for it).
- Lifecycle & bootstrap: first-boot (mint the first hostowner), install flow,
  lost-passphrase (encrypted data gone *by design*), recovery/single-user mode,
  RECOVER phrase (hostowner-c).
- Accountability: attribute privileged actions to a human (corvus + Stratum audit
  logs); tamper-evidence.
- Untrusted-code sandboxing (a **strength** to flaunt): run a binary with an empty
  namespace + zero caps; falls out of axes 1+4 for free — no containers bolt-on.
- World/public data: `/bin`, system libs (system pool, integrity-only) — readable
  by all authenticated users? by `none`?

---

## 5. Open forks (the remaining design agenda)

- **F-0 (foundational; colors everything)** — Is identity a *username + groups*
  model (Plan 9/Unix-shaped) or do we make a principal *literally a bundle of
  capabilities + key-ownership + a namespace*, with "username" a human-readable
  label on that bundle? *Lean (SOTA-informed): a hybrid — a durable named
  principal (human-readable name + group memberships, for multi-user UX) that the
  kernel represents at runtime as a capability-bearing token (`stripes`). Not pure
  ocap (too alien for a login OS — Fuchsia has no users), not Linux uid (tangled).
  The name is a label on the bundle.* **RESOLVED 2026-05-28 → see §3.3.**
- **F-2** — Permission vocabulary: Unix mode-bits vs ACLs. Shapes the stat/setattr
  ABI. *Lean: ACLs are where realistic sharing goes (mode-bits can't express
  "group A reads AND group B writes AND susan denied"); mode-bits-first +
  ACL-extension is the lower-risk path.* **RESOLVED 2026-05-28 → §3.4 (mode-bits
  v1.0 + namespace/cap dynamic sharing; durable-ACL door kept open).**
- **F-4** — 9P identity presentation. **RESOLVED 2026-05-28 → §3.5** (assert
  principal-id only to corvus-stamped trusted-local servers; untrusted = `none`;
  authenticated remote = v1.x).
- **F-5** — Ownership-on-create. **RESOLVED 2026-05-28 → §3.5** (owner = caller's
  durable principal-id, never the clearance; group inherits parent dir; mode =
  default & umask).
- **F-6** — HW capability granularity. **RESOLVED 2026-05-28 → §3.6**: v1.0 blanket
  `CAP_HW_CREATE` (resource-scoping deferred to v1.x for consistency with F-3);
  contained by legate scope + dev-mode; representation allowlist-extensible.
- **F-7** — Ad-hoc HW elevation. **RESOLVED 2026-05-28 → §3.6**: requires explicit
  dev-mode (persistent hostowner-set audited machine flag); boot-chain HW grants
  exempt. (No SMMU/IOMMU at v1.0 confirmed → gate is load-bearing.)

**All forks F-0 through F-7 RESOLVED 2026-05-28.** §5 retained as the audit trail.
Remaining before promotion to binding scripture: walk the §4 S5+ scenario catalog
(exercise the model; surface any refinement) + draft implementation sequencing.

---

## 6. Scope: what this arc absorbs + builds

**Absorbs (suspended Phase 5 tail):**
- P5-login (`/sbin/login`): authenticate via corvus → spawn the user's per-user
  stratumd (`--role client`, scoped to their dataset) → bind their home into their
  territory.
- Per-user stratumd wiring (Stratum A2 delivered; verify it is merged into the
  `thylacine-pouch-arm` branch Thylacine cross-builds).
- P5-hostowner-c (RECOVER verb).
- corvus live-interop seam: the Q11 4-byte request header (A3; corvus decoder
  3→4 bytes).
- A2 as-built deviation to honor: dataset-scope refusal = `Rlerror(EACCES)` at
  `Tattach` (kernel 9P client keys on that).

**New Thylacine-side surface:**
- Current-user identity in the Proc model + group memberships.
- The rwx/identity stat surface (owner/group/mode in `t_stat`) + a
  `wstat`/`chmod`/`chown`-equivalent syscall.
- `SYS_WALK_CREATE` (owner = authenticated caller).
- The clearance-level / legate machinery (corvus-side grants + kernel cap-stamp +
  ephemeral principal).
- The trusted-path mechanism (console + secure-attention).
- The uniform mount-cape for non-POSIX backings.
- A voluntary capability/namespace *reduction* primitive (pledge/unveil dual).

**Explicit v1.x deferrals:**
- Hardware-backed secret storage (TPM/Secure Enclave); design corvus key iface for
  later slot-in.
- Resource-scoped capabilities (F-6 may land the scoping hook earlier).
- Cryptographic clearance proof for the distributed/CPU-server case.
- CHERI/Morello hardware-capability substrate.

---

## 7. Cross-references

- `CORVUS-DESIGN.md` — key agent, hostowner model (D5), boot ordering (D4),
  Stratum API §13.
- `STRATUM-API-V1.md` + `STRATUM-API-V1-RESPONSE.md` — A1–A5 delivered; A2
  coordinator model; Q11 4-byte header; EACCES-at-Tattach.
- `ARCHITECTURE.md §28` — invariants (I-1 namespace isolation, I-2/I-6 capability
  monotonic reduction, I-3 mount DAG).
- `ROADMAP.md §7.2a` — the Corvus arc (login, hostowner, per-user stratumd).
- `docs/phase7-status.md` — Utopia shell arc (paused at U-6d-a).

---

## 8. The convergence detour (binding arc)

The Utopia shell arc is paused at U-6d-a. Before it resumes, one comprehensive
detour brings the system to a clean boundary line where **it really is what it
says it is**. This section is the binding definition of that arc.

### 8.1 Governing bar — convergence, not maximalism

The detour closes when **scripture == reality**: every claim in VISION /
ARCHITECTURE / ROADMAP is either **true-because-built** or
**honestly-scoped-with-its-reason**. Two refinements bound the work (user-set
2026-05-28):

1. **Build to the extent of the foreseeable future.** Build a mechanism iff there
   is a concrete near-term caller OR a verified threat on the v1.0 target.
   Purely-speculative or hardware-absent needs are **not** built now — building
   them would add unverified complexity, violating the project's binding
   conviction ("complexity only where verified") and the no-speculative-generality
   rule.
2. **Design every seam so extension is additive.** Versioned structs, reserved
   fields, capability/policy objects instead of fixed bitfields, clean indirection
   points — so a foreseeable-but-not-yet item slots in later **without an ABI break
   or re-architecture**.

**Per-item test:** *concrete caller or verified threat?* → **BUILD** (with an exit
criterion). *Foreseeable but not yet?* → **SEAM** (design the extension point;
scripture records the trigger that activates it). This is explicitly **not**
"build everything maximally."

### 8.2 New invariant (lands in ARCHITECTURE.md §28)

**I-22 — No identity carries ambient super-authority.** There is no superuser
identity. `hostowner` is an authority *source* (the system key), not an identity.
Every identity's only ambient authority is its minimal baseline (its own
home/territory + its own stratumd); all elevated power is acquired solely via the
legate (scoped, audited, ephemeral). Enforcement: no syscall grants authority by
identity alone; capabilities are explicit and acquired only by grant or legate
activation. This is the discipline that prevents decay back into uid-0.

### 8.3 The arc — one arc, ordered, not split

Three stops, executed as a single arc (no v1.x deferral of fundamentals, no
distribution across other arcs):

- **Stop A — identity / access / privilege** (this document). The hybrid identity
  + groups, the legate/clearance elevation model, `SYS_WALK_CREATE` with
  ownership-on-create, the **kernel rwx-permission layer** (Linux-VFS model, §3.7 —
  the mount-cape folds into it), the trusted path, `CAP_KILL` (resolving the open
  ARCH question). Absorbs the suspended Phase 5
  tail: P5-login, per-user stratumd wiring (Stratum A2 ready), P5-hostowner-c, the
  corvus Q11 4-byte seam.
- **Stop B — DMA isolation (SMMUv3), in v1.0.** SMMUv3 driver + DMA-API change
  (`KObj_DMA` hands out device-IOVAs, not raw PAs) + per-device SMMU context
  lifecycle + audit. Makes the userspace-driver trust model safe; the F-7 dev-mode
  flag is dropped (subsumed). A and B are largely independent.
- **Stop C — the real-misses sweep dispositions** (§8.4). Every finding from the
  2026-05-28 kernel completeness sweep, dispositioned BUILD / SEAM / DOC.

### 8.4 Sweep-findings disposition (2026-05-28; the kernel CORE verified sound)

The completeness sweep confirmed the kernel's core mechanisms are correct
(TLB/cache maintenance, W^X via PXN/UXN, SMP barriers, kernel-stack guards, MMIO
attributes, GIC ordering, stack canaries, LSE). The gaps below are an incomplete
user-facing syscall surface, specific hardening backstops, two design items, and
scripture over-claims.

**BUILD (concrete caller / verified threat) — with exit criterion:**
- **Syscall-surface completion** (the meta-finding: wrappers + rights-gates over
  already-audited internals). Clock read (`timer_now_ns` exists); FS mutation +
  durability `rename`/`unlink`/`mkdir`/`rmdir`/`readdir`/`fsync`/`setattr`/`statfs`
  (the 9P client `p9_client_*` implements all of them). Exit: the v1.0 coreutils
  VISION promises (`date sleep ls mv rm mkdir touch ln ps`) are runnable; `fsync`
  gives a real durability barrier.
- **Kernel rwx-permission layer** (the Linux-VFS enforcement point, §3.7 — replaces
  the old "server enforces" assumption; Stratum does not enforce file rwx). At
  walk/open/create, check the file's mode/uid/gid (Dev `stat` / mount-cape) against
  the Proc's `principal_id` + groups; no uid bypass (I-22). Exit: a Proc cannot
  read/write a file its identity + the mode forbid; chmod/chown ownership-change
  policy enforced kernel-side.
- **Real exit-status** (replace the 0/1 collapse with a structured status). Exit:
  `exit 42` surfaces verbatim through `wait`.
- **Demand-zero anonymous paging** (replace eager `alloc_pages` backing). Exit:
  `burrow_attach(N)` commits pages on first touch, not up front.
- **PAN enable** (kernel cannot silently deref user memory). Exit: a kernel deref
  of a user VA without an explicit uaccess bracket faults.
- **PAC key real entropy** (reuse the KASLR seed / RNDR, not `CNTPCT_EL0`).
- **Resource/DoS floor** (minimal per-Proc page + thread + child caps). Exit: a
  fork/thread/memory bomb is bounded, not box-extincting.
- **Orphan reaper** (kthread draining kproc's adopted zombies). Exit: orphaned
  grandchildren are reaped; no permanent Proc/kstack leak.
- **`CAP_KILL` + cross-process signaling** (resolve the open ARCH question; part of
  Stop A's capability model).
- **Cheap speculation detection** (CSV2/CSV3 reporting + SSBS where applicable).

**SEAM (foreseeable but not yet) — design the extension point; scripture records
the trigger:**
- **COW** — leave a clean write-fault-on-shared-page branch; activates when a
  `fork`-style address-space-sharing caller exists (none today: rfork RFMEM
  extincts).
- **Full resource quota / cgroup-equivalent** — the minimal floor's per-Proc
  counters are read by a future policy layer.
- **Resource-scoped HW caps** — the `CAP_HW_CREATE` representation carries an
  optional allowlist (empty = blanket); SMMU makes scoping enforceable.
- **ACLs** — the stat/setattr ABI is versioned + reserved so named-user/group ACL
  entries ride the 9P xattr family later.
- **Hardware-backed key storage (TPM/Secure Enclave)** — corvus's key interface is
  backend-pluggable.
- **Verifiable-credential identity** (distributed / CPU-server) — the local
  kernel-stamped identity has the credential projection point.
- **KPTI / direct-map randomization** — gated on a threat-eval of the v1.0 target;
  build iff the target speculates such that it's needed; otherwise keep TTBR-switch
  points clean so it is additive.
- **Stack auto-grow + wider guard gap** — fixed 256 KiB stack + 1-page guard is the
  v1.0 baseline; the VMA layout leaves room to grow.
- **RNG chacha20 stir + reseed** — over the RNDR baseline.
- **`/proc/<pid>/fd` + `/proc/<pid>/mem`** — ride the readdir surface built above.

**DOC (scripture over-claims to reconcile — no code):**
- VISION/ARCH claim **MTE + CFI** enabled by default at v1.0 → both are deferred
  (toolchain: CFI post-v1.0, MTE Phase 8). Amend to match the (honest) boot banner.
- ARCH §6.1/§16 + ROADMAP Phase-2 exit criteria claim **demand-paging + COW +
  stack-growth** delivered → as-built is eager-anon / no-COW / fixed-stack. Reconcile
  (demand-zero becomes true once built above; COW + auto-grow become seams).
- ROADMAP §6.1 claims RNG **"RNDR + chacha20 stir"** → RNDR-only; record the stir as
  a seam.
- Fix doc drift: the CLAUDE.md / ARCH audit-trigger table references
  `arch/arm64/ipi.c`, which does not exist (IPI logic is in `gic.c` / `sched.c`).

**VERIFY (follow-up adversarial pass owed):** SMP wait/wake memory-ordering vs I-9;
spurious/spoofed-IRQ isolation (pairs with Stop B); capability-check uniformity
across all 54 syscall handlers (spot-clean, not exhaustive).

When this arc closes, every VISION/ARCH/ROADMAP claim is true-or-honestly-scoped,
and every foreseeable extension is pre-fitted. Then the Utopia shell arc resumes
on a sound foundation (next chunk U-6d-b).

---

## 9. Sub-chunk ABI pins (design-first)

Precise ABIs pinned before code, per "design -> scripture -> code." The byte-exact
layouts here are the contract; the implementation's `_Static_assert`s must match.

### 9.1 A-1 — identity model ABI (RESOLVED 2026-05-28)

**Proc identity record** (new fields on `struct Proc`):
- `u32 principal_id` — durable identity. **Inherited** across rfork/spawn.
- `u32 primary_gid`.
- `u32 supp_gids[15]` + `u8 supp_gid_count` — up to 16 groups total (1 primary + 15
  supplementary), fixed-size to bound the Proc; corvus is the authority for full
  membership (the kernel caches the active set).
- `stripes` stays per-Proc, fresh, unforgeable (unchanged). `caps` stays
  subset-inherited (unchanged).
- `struct Proc` grows **168 -> 240 bytes** (the identity block appends after
  `handler_va`; existing offsets unchanged). `PROC_SUPP_GIDS_MAX = 15`. The fields
  are KP_ZERO at `proc_alloc` (so the transient default is `INVALID`/0, fail-closed);
  `proc_init` stamps kproc = `{SYSTEM, GID_SYSTEM}`; `rfork_internal` copies the
  parent's identity into the child (the inherit path); the spawn thunk optionally
  overrides via `proc_apply_identity` (the single audited mutation site).

**Reserved principal-ids / gids** (none privileged — I-22):
- `0` = INVALID (never assigned; a Proc with id 0 pre-login is a bug).
- `PRINCIPAL_SYSTEM = 0xFFFFFFFE` — the boot/kernel-proc identity (kproc, joey,
  pre-login). Holds caps via the boot chain, NOT via identity.
- `PRINCIPAL_NONE = 0xFFFFFFFF` — unauthenticated / "nobody" (Plan 9 `none`); lowest
  baseline.
- Real users/roles: corvus-assigned in `[1, 0xFFFFFFFD]` (corvus policy, e.g.
  >= 1000). Same reserved scheme for gids (`GID_SYSTEM` / `GID_NONE`).
- `proc_alloc` defaults a new Proc's identity to **inherit the parent's**; the boot
  chain (kproc) is `PRINCIPAL_SYSTEM`, so everything is SYSTEM until login stamps a
  real identity. (Deliberately no `uid==0`-as-root — sidesteps the Unix-root and
  Stratum-`/ctl`-uid-0 baggage; a future Thylacine-hostowner -> Stratum-`/ctl`
  mapping is a separate A-5/admin concern.)

**Identity establishment — at spawn, gated, fail-closed** (REFINED 2026-05-28 from the
earlier "stamp-a-running-child" proposal, which had a stamp-before-it-runs race;
gate semantics + spawn-args byte layout pinned at A-1a):
- Identity is applied in the spawn path's child entry (the `sys_spawn_full_argv`
  thunk) BEFORE the child enters EL0 — consistent with how the console-attach /
  `MAY_POST_SERVICE` perm bits are stamped today. "Race-free" means: there is no
  separate post-spawn identity syscall, and userspace NEVER observes a pre-identity
  child (the override lands before `userland_enter`; an in-kernel child that carries
  the inherited identity for the few instructions before its thunk runs has opened
  no `/srv` connection and is not yet any server's peer). The `CAP_SET_IDENTITY`
  check is done in the PARENT (the caller's caps), not the child.
- The rich-spawn ABI (`struct sys_spawn_args`, used by `SYS_SPAWN_FULL_ARGV`) is
  extended append-only (**56 -> 80 bytes**; every existing offset unchanged):
  - `principal_id`   `u32` @56
  - `primary_gid`    `u32` @60
  - `supp_gids_va`   `u64` @64  — user-VA of up to `PROC_SUPP_GIDS_MAX` (15) `u32` gids
  - `supp_gid_count` `u32` @72  — 0..15
  - `identity_flags` `u32` @76  — `SPAWN_IDENTITY_SET = 1u << 0`; other bits reserved 0
- **Gate (fail-closed).** `identity_flags & SPAWN_IDENTITY_SET == 0` -> the child
  INHERITS the parent's identity (the default; no cap needed). The flag set AND the
  caller holds **`CAP_SET_IDENTITY` (`1ull << 5`)** -> the child is born with the
  requested identity. The flag set WITHOUT the cap -> the syscall **returns -1** (the
  request is rejected loudly; no child is spawned). This refines the earlier looser
  "without the cap -> inherit" wording to **fail-closed**: a privilege-boundary
  request made without authority must fail, never silently downgrade to an identity
  the caller did not ask for. So login (holding `CAP_SET_IDENTITY`, conferred down
  the boot chain) spawns the user's shell *born with* the user's identity; ordinary
  processes do not set the flag and inherit. **No standalone `SYS_SET_IDENTITY`
  syscall.**
- **Settable-value policy.** A SET request validates: `principal_id` and
  `primary_gid` MUST be in `[1, 0xFFFFFFFD]` OR the `NONE` sentinel; `INVALID (0)`
  and the `SYSTEM` sentinel are REJECTED with -1 (you cannot forge the system
  identity nor stamp the never-valid 0). `supp_gid_count <= 15`; supplementary gids
  are validated by the **same predicate** -- INVALID and SYSTEM both rejected (A-1a
  R1 F1: rejecting only `0` on the supplementary axis while the primary axis rejected
  SYSTEM was an I-22 asymmetry -- a capped login could smuggle the system group into a
  user's supplementary set, which becomes authority once A-2d enforces group rwx).
  corvus is the authority for which real gids a login may legitimately request; the
  kernel only bounds + sanity-checks (full supp-gid policy is a corvus concern, A-1b).
- `CAP_SET_IDENTITY` is FORK-GRANTABLE (a member of `CAP_ALL`): it flows kproc ->
  joey -> login down the vetted boot chain; rfork's mask-AND means the ordinary user
  Proc that login spawns does NOT receive it (login omits it from the shell's
  `cap_mask`), so a user cannot spawn processes as another user.
- Self-de-escalation (a daemon dropping its own identity to `PRINCIPAL_NONE`) is a
  v1.x **seam** (you can always become *less*; not yet built).

**`srv_peer_info` ABI extension** (24 -> 40 bytes; append-only, existing offsets
unchanged). **CORRECTION 2026-05-28 (verified against the tree):** the original pin
placed `principal_id` at @20 as "was pad" — but @20 is the LIVE `alive` field
(`SYS_SRV_PEER` writes it; corvus reads it; `specs/corvus.tla` ConnOpPeerWasLive).
The new fields therefore append AFTER `alive`; size is 40, not 32:

```
struct srv_peer_info {
    u64 stripes;        // @0   (unchanged)
    u64 caps;           // @8   (unchanged)
    u32 console;        // @16  (unchanged)
    u32 alive;          // @20  (unchanged; LIVE — 1 iff an ALIVE Proc carries stripes)
    u32 principal_id;   // @24  (new; PRINCIPAL_NONE when alive == 0)
    u32 primary_gid;    // @28  (new; GID_NONE when alive == 0)
    u32 flags;          // @32  (new; reserved, 0 at v1.0)
    u32 _reserved;      // @36  (new; explicit pad to 40, reserved 0)
};   // sizeof == 40; offsets pinned by _Static_assert
```

`principal_id` / `primary_gid` are resolved FRESH per query (same walk that yields
`caps` + `alive`); a dead/reaped peer fail-closes to `PRINCIPAL_NONE` / `GID_NONE`
(the SrvConn captures only `stripes` + `console` immutably at mint — capturing the
human-meaningful identity immutably is an A-3 presentation concern, not A-1a).
Supplementary groups are NOT in the struct (variable count) — a consumer resolves
them via corvus by `principal_id`.

**corvus identity DB** (corvus-side; schema *shape* pinned here, byte-exact CRVS
format pinned in `CORVUS-DESIGN.md` when the A-1b corvus code lands):
- **User record:** `{principal_id, name, primary_gid, supp_gids[], wrap-chain
  (existing), clearance-eligibility[] (A-4)}`.
- **Group record:** `{gid, name}`.
- **Resolution verbs** (cross-component ABI on `/srv/corvus`): `RESOLVE_ID`
  (id -> name + gids) and `RESOLVE_NAME` (name -> id). Pinned ABIs.
- **CRVS on-disk format:** version bump v1 -> v2 (adds the identity DB); corvus
  reads v1 as "no identity DB" and writes v2. Byte format = A-1b detail.

**A-1 implementation split:**
- **A-1a (kernel):** Proc identity fields + inheritance + reserved values + the
  spawn-args identity extension + `CAP_SET_IDENTITY` + the `srv_peer_info`
  extension. Audit-bearing. Tests: inheritance (identity inherits while caps reduce
  to NONE and stripes stay fresh — the I-22 demonstration); `proc_apply_identity`
  sets fields; a capped Proc spawns a child with a SET identity (accepted, clean
  exit); an uncapped caller's SET request is rejected with **-1 (fail-closed)**;
  reserved-value reject (INVALID / SYSTEM -> -1); the peer-snapshot data path that
  feeds `srv_peer_info` exposes `principal_id` / `primary_gid`; kproc is SYSTEM.
- **A-1b (corvus):** the identity DB + `RESOLVE_*` verbs + CRVS v2 + **real
  persistence** (user-chosen 2026-05-28: the full identity DB with UPG +
  `GROUP_CREATE`, and on-disk persistence rather than an in-memory-only seam).
  **Reordered (user-chosen 2026-05-28): A-1b now FOLLOWS the FS-mutation
  foundation (§9.2), because real persistence requires `SYS_WALK_CREATE` +
  `SYS_FSYNC` + `SYS_READDIR`, none of which existed at A-1a.** The corvus byte
  format + `RESOLVE_ID` / `RESOLVE_NAME` / `GROUP_CREATE` wire ABIs + the
  `USER_CREATE` append-only extension get their own design-first pin in
  `CORVUS-DESIGN.md` immediately before the A-1b code. Tests:
  `RESOLVE_ID`/`RESOLVE_NAME` round-trip; v1 -> v2 read/upgrade; group
  membership; persistence (write the DB, fsync, re-read it within the boot).

### 9.2 FS-mutation foundation ABI (RESOLVED 2026-05-28; precedes A-1b)

The three filesystem-mutation syscalls A-1b's real persistence needs (and the
A-2 coreutils + the shell need shortly after). **Pulled forward ahead of A-1b**
per the sequencing decision above: A-2b's `SYS_WALK_CREATE` plus the "G1
FS-mutation" sweep items (`fsync`, `readdir`) land as one audit-bearing
foundation, THEN A-1b builds persistence on top. The kernel 9P client already
implements the wire half (`p9_client_lcreate` / `p9_client_mkdir` /
`p9_client_fsync` / `p9_client_readdir`, verified 2026-05-28) and the `Dev`
vtable already has a `.create` slot (today a `dev9p_create` stub); this
foundation is **syscall wrappers + the real `dev9p_create` + two new `Dev`
vtable slots (`.fsync`, `.readdir`) + rights gates + tests + audit**, not new
protocol work. Numbers continue from `SYS_PIVOT_ROOT = 53`.

**`SYS_WALK_CREATE = 54`** -- the create-then-open sibling of `SYS_WALK_OPEN`.
Single-component create in a directory Spoor; returns an opened `KOBJ_SPOOR` fd
to the created object (file OR directory), matching `walk_open`'s envelope.

```
SYS_WALK_CREATE(parent_fd, name_va, name_len, omode, perm) -> opened_fd / -1
  x0 = parent_fd   hidx_t; KOBJ_SPOOR with RIGHT_WRITE (create mutates the
                   directory) -- OR the SYS_WALK_OPEN_FROM_ROOT sentinel
                   ((u64)-1) for the caller's Territory root.
  x1 = name_va     user-VA of the single component name.
  x2 = name_len    1 .. SYS_WALK_OPEN_NAME_MAX (64); reject '/' and '\0'.
  x3 = omode       Plan 9 open mode for the returned fd (OREAD/OWRITE/ORDWR
                   + optional OTRUNC); SYS_WALK_OPEN_OMODE_VALID mask.
  x4 = perm        u32 Plan 9 perm. Low 9 bits = rwxrwxrwx mode. The DMDIR bit
                   (0x80000000) selects directory creation (Tmkdir) instead of
                   a file (Tlcreate). All other DM* bits reserved 0 -> reject.
```

- **Returned fd rights:** `RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER` (same as
  `walk_open`). The underlying fid's omode is what the server enforces.
- **Mechanism:** `dev->create` (the real `dev9p_create`): `perm & DMDIR` ?
  `p9_client_mkdir(mode = perm & 0777, gid = caller primary_gid)` then walk+open
  the new directory : `p9_client_lcreate(flags from omode | O_CREAT [+ O_TRUNC],
  mode = perm & 0777, gid = caller primary_gid)`. For `devramfs` (the
  kernel-test target) the existing in-memory create path is reused.
- **Ownership-on-create (SEAM at this chunk):** the create carries the caller's
  `primary_gid` into the 9P `gid` field. Full owner attribution = caller
  `principal_id` is recorded where the KERNEL mediates the metadata -- the
  mount-cape for self-declared-non-POSIX backings (A-2c) and the kernel rwx
  layer (A-2d); for a Stratum-backed dev9p file the owner is the connection
  identity (A-3 per-user stratumd completes per-user attribution). This chunk
  builds the create MECHANISM; the rwx-enforcement semantics ride on top in A-2.
- **Error cases (-1):** parent not `KOBJ_SPOOR` / missing `RIGHT_WRITE`; FROM_ROOT
  with no pivoted root; backing Dev has no `.create`; name bounds / `/` / `\0`;
  omode outside the valid mask; perm has a reserved DM* bit; the server rejects
  (name exists -> Rlerror; out of space; permission); handle table full.
- **Rights gate here is the HANDLE right (`RIGHT_WRITE` on the parent); the
  per-file rwx permission check is A-2d, not this chunk (I-22 stands -- no id
  bypass exists yet because no rwx enforcement exists yet).**

**`SYS_FSYNC = 55`** -- the durability barrier. Required on an integrity-FS OS:
"write then fsync" is the contract that makes a persisted record durable
(corvus's identity DB at A-1b; any later durable writer).

```
SYS_FSYNC(fd, datasync) -> 0 / -1
  x0 = fd          hidx_t; KOBJ_SPOOR with RIGHT_WRITE.
  x1 = datasync    u32; 0 = full fsync (data + metadata), 1 = datasync.
```

- **Mechanism:** new `Dev.fsync(struct Spoor *c, u32 datasync) -> int` vtable
  slot. `dev9p_fsync` -> `p9_client_fsync(fid, datasync)` -> Stratum `Tsync`.
  In-memory Devs (`devramfs`) implement `.fsync` as no-op success (nothing to
  flush). A Dev with no `.fsync` slot -> -1.
- **Error cases (-1):** fd not `KOBJ_SPOOR` / missing `RIGHT_WRITE`; no `.fsync`
  slot; server Rlerror.

**`SYS_READDIR = 56`** -- directory enumeration. Needed for A-1b load-time user
enumeration and for the A-2 coreutils `ls`.

```
SYS_READDIR(fd, buf_va, buf_len) -> bytes_written (>=0) / -1
  x0 = fd          hidx_t; KOBJ_SPOOR opened on a directory, RIGHT_READ.
  x1 = buf_va      user-VA out buffer.
  x2 = buf_len     1 .. SYS_RW_MAX (4096).
```

- **Stateful via the Spoor offset** (the same offset `SYS_READ` / `SYS_LSEEK`
  advance): each call returns the next run of entries and advances the offset;
  0 bytes returned == end of directory.
- **Buffer format:** the raw 9P2000.L `Treaddir` dirent byte stream -- per entry
  `qid(13) + offset(8) + type(1) + name_len(2 LE) + name`. The caller parses it
  (libthyla-rs provides a dirent iterator). Pinning the wire dirent format
  (rather than a Thylacine-native struct) keeps the syscall a thin pass-through
  of `p9_client_readdir`; a native `struct t_dirent` is a v1.x convenience seam.
- **Mechanism:** new `Dev.readdir(struct Spoor *c, void *buf, long n, s64 off)
  -> long` vtable slot (NULL-permitted, like `.poll` / `.stat_native`).
  `dev9p_readdir` -> `p9_client_readdir`. The 9P2000.L Treaddir `offset` is a
  resume COOKIE, not a byte position, so the `SYS_READDIR` handler parses the
  returned run for the last entry's cookie and stores THAT in the Spoor offset.
  `devramfs` leaves `.readdir` NULL at v1.0 (ramfs enumeration deferred; the
  load-bearing readdir is dev9p, the disk-backed Stratum FS).
- **Error cases (-1):** fd not `KOBJ_SPOOR` / not a directory / missing
  `RIGHT_READ`; `buf_len` 0 or > `SYS_RW_MAX`; no `.readdir` slot; server Rlerror.

**Implementation split:** **FS-alpha** = `SYS_WALK_CREATE` (real `dev9p_create`
with the DMDIR fold + `devramfs_create` verify + the primary_gid stamp + libt /
libthyla-rs wrappers + tests). **FS-beta** = `SYS_FSYNC` + `SYS_READDIR` (the
two new `Dev` vtable slots + dev9p/devramfs impls + wrappers + tests). **Then
one focused adversarial audit** over the whole create/write/fsync/readdir
surface (the AEGIS/mallocng-adjacent create+write path the detour flags
"prosecute hard"), two-commit close. **Then A-1b** persistence rides on top.

### 9.3 FS-gamma — rename + unlink (RESOLVED 2026-05-29; precedes A-1b)

Two more filesystem-mutation syscalls, pulled forward ahead of A-1b per the
2026-05-29 design decision (user-chosen). A-1b's corvus identity DB persists
durably via the classic **write-tmp + fsync + atomic rename-swap** pattern
rather than an append-only log, and that pattern needs `rename` (atomic
replace) + `unlink` (stale-tmp cleanup). Both are also owed for the A-2
coreutils (`mv` / `rm` / `rmdir`), so building them here serves two callers. As
with §9.2, the kernel 9P client already implements the wire half
(`p9_client_renameat` / `p9_client_unlinkat`, verified 2026-05-29) — but those
functions are **implemented yet unexercised**: no syscall has ever driven them,
and `dev9p.c::dev9p_remove` is a `void` stub noting "Remove maps to Tunlinkat on
the parent; deferred to syscall chunk." So this foundation is **syscall wrappers
+ two new `Dev` vtable slots + the real `dev9p_rename` / `dev9p_unlink` + rights
gates + tests + the first end-to-end audit of `p9_client_renameat` /
`unlinkat`**, not new protocol. Numbers continue from `SYS_READDIR = 56`.

**Why rename-swap over append-log** (the substrate decision). corvus's identity
DB is the login authority; a torn write must never lock the system out. With no
`SYS_RENAME` at v1.0, a whole-file rewrite cannot be made atomic, so the
original §9.2 plan implied an append-only log. The user chose instead to **pull
`rename` forward** (owed for coreutils regardless) and give corvus the classic
atomic-swap substrate: simpler load (parse one complete file), and future-proof
for the mutation-heavy identity DB to come (`USER_DELETE`, group-membership
edits, A-4 clearance grants), where an append-log would need tombstones +
compaction. The cost is one audit-bearing FS chunk on the critical path before
A-1b. (The append-log alternative — zero new syscalls, crash-safe by
construction — was rejected as the weaker substrate for a mutable DB.)

**`SYS_RENAME = 57`** — atomically rename/move a single component from one
directory to another (or within one). POSIX `rename(2)` / 9P2000.L `Trenameat`
shape: the destination, if it exists, is **atomically replaced**. This
atomic-replace is exactly what corvus's DB-swap relies on (`rename(identity.db.tmp
→ identity.db)`).

```
SYS_RENAME(olddir_fd, oldname_va, oldname_len, newdir_fd, newname_va, newname_len) -> 0 / -1
  x0 = olddir_fd   hidx_t; KOBJ_SPOOR (directory) with RIGHT_WRITE -- OR the
                   SYS_WALK_OPEN_FROM_ROOT sentinel ((u64)-1) for the caller's
                   Territory root.
  x1 = oldname_va  user-VA of the source single-component name.
  x2 = oldname_len 1 .. SYS_WALK_OPEN_NAME_MAX (64); reject '/' and '\0'.
  x3 = newdir_fd   hidx_t; KOBJ_SPOOR (directory) with RIGHT_WRITE -- OR FROM_ROOT.
  x4 = newname_va  user-VA of the destination single-component name.
  x5 = newname_len 1 .. 64; reject '/' and '\0'.
```

- **Mechanism:** the handler resolves `olddir_fd` and `newdir_fd` to the
  caller's looked-up dir Spoors (RIGHT_WRITE on each), requires them on the
  **same Dev** (and, for dev9p, the same `p9_client` session), then
  `dev->rename(od, oldname, nd, newname)` → `p9_client_renameat(client, od->fid,
  oldname, nd->fid, newname)`. **No clone-walk** (unlike `SYS_WALK_CREATE`):
  `Trenameat` operates on the dirfids *by name* and neither consumes nor
  transitions them, so the handler runs the op directly on the looked-up Spoors
  — the `SYS_FSYNC` / `SYS_READDIR` pattern, not the create pattern. (The
  fid-exclusive *by-fid* `Trename` is not used here; renameat is the dir+name
  verb.) **Refinement vs the original pin** (corrected during impl, 2026-05-29):
  the first §9.3 draft specified a `SYS_WALK_CREATE`-style clone-walk; that is
  unnecessary because renameat/unlinkat never transition the dirfid — the direct
  form is both simpler and correct.
- **Same-directory rename** (corvus's swap): `olddir_fd == newdir_fd` (or both
  FROM_ROOT) is the common case; the same dir Spoor is used for both
  (`p9_client_renameat` tolerates `olddirfid == newdirfid`).
- **Rights gate:** `RIGHT_WRITE` on **both** dir fds (both directories are
  mutated). FROM_ROOT resolves to the Territory root as in `SYS_WALK_CREATE`.
- **Cross-Dev reject:** A and B on different Devs → -1. A 9P `renameat` is within
  one server; a cross-Dev move is copy-then-unlink, not a v1.0 primitive.
- **Error cases (-1):** either fd not `KOBJ_SPOOR` / not a directory / missing
  `RIGHT_WRITE`; FROM_ROOT with no pivoted root; name bounds / `/` / `\0` on
  either name; cross-Dev; backing Dev has no `.rename`; server `Rlerror` (source
  `ENOENT`; dest is a non-empty directory; `EXDEV`; permission; out of space).

**`SYS_UNLINK = 58`** — remove a single component (a non-directory, or an empty
directory) from a parent directory. 9P2000.L `Tunlinkat`.

```
SYS_UNLINK(parent_fd, name_va, name_len, flags) -> 0 / -1
  x0 = parent_fd   hidx_t; KOBJ_SPOOR (directory) with RIGHT_WRITE -- OR FROM_ROOT.
  x1 = name_va     user-VA of the single-component name to remove.
  x2 = name_len    1 .. SYS_WALK_OPEN_NAME_MAX (64); reject '/' and '\0'.
  x3 = flags       u32; 0 = remove a non-directory; SYS_UNLINK_REMOVEDIR
                   (0x200, mirrors P9_UNLINK_AT_REMOVEDIR) = rmdir an EMPTY
                   directory. Any other bit set -> reject.
```

- **Mechanism:** resolve `parent_fd` to the looked-up dir Spoor (no clone-walk,
  as for rename); `dev->unlink(parent, name, flags)` →
  `p9_client_unlinkat(client, parent->fid, name, flags)`.
- **Rights gate:** `RIGHT_WRITE` on the parent dir fd.
- **flags:** validated against `{0, SYS_UNLINK_REMOVEDIR}`; any other bit → -1.
  The flag selects file-vs-directory removal mode (a mismatch → server `Rlerror`).
- **Error cases (-1):** parent not `KOBJ_SPOOR` / not a directory / missing
  `RIGHT_WRITE`; FROM_ROOT with no pivoted root; name bounds / `/` / `\0`;
  reserved flag bit; backing Dev has no `.unlink`; server `Rlerror` (`ENOENT`;
  `ENOTEMPTY` for rmdir on a non-empty dir; `EISDIR` / `ENOTDIR` mode mismatch;
  permission).

**`Dev` vtable additions** (both NULL-permitted, like `.fsync` / `.readdir` —
only Devs that genuinely back them set them; no 13-Dev churn):

```
int (*rename)(struct Spoor *olddir, const char *oldname,
              struct Spoor *newdir, const char *newname);
int (*unlink)(struct Spoor *parent, const char *name, u32 flags);
```

- `dev9p` sets both → `p9_client_renameat` / `p9_client_unlinkat`. The
  pre-existing `void (*remove)(struct Spoor *)` Plan 9 slot is **left as-is** —
  its shape (no name, no error return, target-not-parent) is wrong for a syscall
  that must report failure on a parent+name target; `SYS_UNLINK` uses the new
  `.unlink`, not `.remove`.
- `devramfs` leaves both NULL at v1.0 (ramfs mutation deferred; the load-bearing
  target is dev9p / the disk-backed Stratum FS — consistent with `.readdir`
  being dev9p-only). Kernel loopback tests drive dev9p (as FS-alpha/beta did).
- **Cross-Dev invariant** (rename takes two dir fds): the handler validates
  `olddir` and `newdir` share a Dev before calling `dev->rename`; a cross-Dev
  rename is rejected at the handler and never reaches a Dev op (`dev9p_rename`
  adds the same-`p9_client`-session check, since two dev9p mounts are distinct
  sessions and a 9P renameat is within one session).

**The durability detail rename-swap relies on.** A `rename(tmp → real)` makes the
name swap atomic, but the *durability* of the dirent change needs a barrier on
the containing directory. corvus's swap sequence is therefore: `SYS_WALK_CREATE`
tmp → write → `SYS_FSYNC(tmp)` → `SYS_RENAME(tmp → real)` → **`SYS_FSYNC` on the
parent directory fd** (→ `dev9p_fsync` → `p9_client_fsync(dir_fid)` → Stratum
`Tsync` on the directory). Whether Stratum honors `Tsync`-on-a-directory as a
metadata barrier is the load-bearing property; the A-1b cross-reboot persistence
test is its end-to-end proof (a user created in boot N must resolve in boot
N+1). A crash between create-tmp and rename leaves a stale `*.tmp`; corvus's load
path `SYS_UNLINK`s any stale tmp before its first write (idempotent cleanup).

**Implementation split:** one chunk — `SYS_RENAME` + `SYS_UNLINK` together (the
two new vtable slots + dev9p impls + handlers + libt / libthyla-rs wrappers +
kernel loopback tests + a joey E2E against real Stratum: create→rename→read-back
under the new name; create→unlink→gone; mkdir→rmdir-via-REMOVEDIR). **Then one
focused adversarial audit** (the first end-to-end exercise of
`p9_client_renameat` / `unlinkat`; the rename-swap durability path; the
cross-Dev + same-session reject; fid lifecycle on every error path).
**Then A-1b** builds corvus persistence on the atomic-swap substrate.
