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

**As-built mechanism (RESOLVED 2026-06-01): §9.8.** The legate is the existing `cap`-device
two-phase grant generalized to clearance cap-sets (corvus registers, the Proc redeems, the
kernel stamps `caps |= clearance ∩ self_restriction`); scope = a `legate_scope_id` subtree
that is fully torn down (group-terminate, reusing #809/#811) on the legate root's exit or
`valid_until` expiry; `CAP_KILL` / `CAP_DAC_OVERRIDE` / `CAP_CHOWN` are the elevation-only
caps split out of `CAP_HOSTOWNER`. No local crypto.

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
- **`stripes` = the per-Proc unforgeable identity tag** -- a plain monotonic `u64`, set
  once at `proc_alloc`, read via `SYS_SRV_PEER` (thematic fit: the markings that identify
  the individual thylacine). It answers "is this the same Proc?" -- it is NOT itself a
  structured credential. The runtime *authority* lives in **separate Proc fields** the
  kernel reports alongside it: `caps` (the capability set) and `principal_id` (the durable
  identity, corvus-authoritative); a legate adds the ephemeral `legate_*` annotation (§9.8).
  A legate = same `principal_id`, with the clearance caps stamped on `caps` and the scope in
  `legate_scope_id` -- the durable identity never changes. *(As-built reconcile 2026-06-01:
  the earlier "stripes is a token stamped with {principal-id, caps, clearance annotations}"
  framing was a design-time over-claim; the kernel keeps these as distinct fields that
  `SYS_SRV_PEER` returns together -- see §9.1 + §9.8.)*
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

**CORRECTED 2026-05-31 (A-3 ground truth; the channel above was the wrong one).** The
2026-05-28 sketch named **`n_uname`** as the local channel. Ground truth (two Explore
passes, building A-3) showed that is wrong against the actual stack: **Stratum ignores
`n_uname`** (`server.c:1007-1008`) and reconciles identity via **`SO_PEERCRED`** only,
which pouch already marshals from the kernel's **`SYS_srv_peer`** (kernel-stamped,
*unforgeable*; the connecting Proc's durable `principal_id` via its stripes). So the
load-bearing trusted-local channel is **`SO_PEERCRED`-carries-principal**, not
`n_uname`-asserted (the pouch shim's pre-A-1a `ucred.uid = 0` stub is fixed to carry
`principal_id`). `n_uname` forwarding is kept (cheap) but **demoted to the v1.x
foreign/authenticated path** (a server with no `SO_PEERCRED` -- remote/TCP -- where the
corvus-`Tauth`/trust-stamp gate then matters). The **trust-stamp gate is a v1.x SEAM**:
at v1.0 every attach is local, so the presented identity is the unforgeable kernel-stamp
and there is no untrusted-assertion to gate. Full mechanism + invariants: **§9.7**. The
bullets above are preserved as the superseded 2026-05-28 record.

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

### 3.7.1 The v1.0 privilege model (RESOLVED 2026-05-30, A-2d scripture)

§3.7 fixed the *model* (kernel-VFS enforcement). A-2d pins the *concrete policy*,
voted by the user 2026-05-30 after a Plan 9 + Linux-VFS + capability-microkernel
prior-art pass (Plan 9 and Linux agree on the load-bearing rules; the capability
microkernels — Fuchsia/seL4/Genode — have no FS-rwx model to copy, enforcing via
namespace + capability routing instead).

**The access-check algorithm (owner-first POSIX).** For a Proc `p` accessing a
file with stat `st`, wanting `want ⊆ {R=4, W=2, X=1}`:
1. `p->caps & CAP_HOSTOWNER` → allow (the DAC-override; see below).
2. else `p->principal_id == st.uid` → owner bits `(st.mode >> 6) & 7`.
3. else `gid_member(p, st.gid)` → group bits `(st.mode >> 3) & 7`.
4. else → other bits `st.mode & 7`.
5. allow iff `(bits & want) == want`.

Owner-first is **authoritative** (POSIX): a Proc that owns a file is judged on
owner bits *only*, even when group/other would grant more — it can always `chmod`
itself the bit, since it owns the file. Group membership =
`st.gid == p->primary_gid || st.gid ∈ p->supp_gids[0..supp_gid_count)`
(`GID_INVALID` is never a member).

**Enforcement points** (the chokepoints the kernel already mediates — userspace
never talks to a backing directly):
- **walk** (`SYS_WALK_OPEN`, per component): **X** (search) on the source directory
  `src`. The walk is already one-component-per-call (`dev->walk(src, nc, names, 1)`
  with the `w->nqid != 1` partial-walk guard), so this is one `stat_native(src)`
  per step — **cheap, not N extra round-trips** (the "server-side batched walk
  makes POSIX X-checks expensive" worry dissolves).
- **open**: **R** and/or **W** on the walked target per `omode` (OREAD→R, OWRITE→W,
  ORDWR→R|W). `O_PATH` (`SYS_WALK_OPEN_OPATH`) is **exempt** from the R/W gate
  (§9.4: a walk-only handle has no access semantics) — the **X-search on the path
  to reach it still applies**.
- **create** (`SYS_WALK_CREATE`): **W and X** on the parent directory.
- **read/write are NOT re-checked**: POSIX open-time snapshot — a later `chmod`
  does not retroactively revoke an open fd.

The handle RIGHT (capability axis) and the rwx check (identity axis) are **both**
required and orthogonal — a Proc may hold a `RIGHT_READ` handle to a file its
identity lacks `other-r` on (handed via a privileged path); §3.7 requires both.

**The privilege model — `CAP_HOSTOWNER` is the unified v1.0 fs-admin authority**
(voted: fold-into-`CAP_HOSTOWNER`, over mint-finer-caps-now and no-override-at-all):

| operation | who may |
|---|---|
| rwx access override (DAC-override) | `CAP_HOSTOWNER` |
| `chmod` (mode bits) | file owner OR `CAP_HOSTOWNER` |
| `chown` (change uid — give-away) | `CAP_HOSTOWNER` **only** (owner may NOT give a file away) |
| `chgrp` (change gid) | file owner → a group **they belong to**; OR `CAP_HOSTOWNER` → any group |

`CAP_HOSTOWNER` is **elevation-only** (caps.h: never in `CAP_ALL`, never
rfork-grantable, conferred only via the `cap` device to a console-attached Proc
after the system passphrase). It is therefore a **capability, not an identity** —
so the DAC-override **preserves I-22** (no *identity* carries ambient
super-authority; the override rides the console-gated capability the legate model
will later refine). **No `principal_id` — not even `PRINCIPAL_SYSTEM` — bypasses
the check; only the capability does.**

**No-give-away `chown`** matches Plan 9 (fileserver-owner only) and Linux
(`CAP_CHOWN`): it prevents quota-dodging and accountability-laundering.
setuid/setgid/sticky are already rejected at A-2a (S5 no-setuid); A-2d does not
revisit them.

**Alternatives considered + rejected.** (a) Mint `CAP_DAC_OVERRIDE` + `CAP_CHOWN`
as separate elevation-only bits now — rejected: no v1.0 caller (the fs-admin
legate is A-4), cuts against the convergence bar's build-iff-caller rule (§8.1),
adds audit surface for no near-term use. (b) No rwx override at all (purest I-22) —
rejected: awkward admin/backup/recovery, and a hostowner could `chown`-to-self then
read anyway, so the denial buys friction, not security. **A-4's legate model
splits a finer `fs-admin` clearance (`CAP_DAC_OVERRIDE` + `CAP_CHOWN`) out of
`CAP_HOSTOWNER` — an additive seam** (the cap representation already supports new
bits; no ABI break).

**Honest scope.** Enforcement is **real and per-principal on devramfs now**: it
reports system-owned, world-readable (root `0555`, files cpio-mode `0644`/`0755`),
so the `PRINCIPAL_SYSTEM` boot chain owns everything it touches (owner-rwx, no
brick), while a non-system principal gets `other-r`+`other-x` (read+traverse) but
not write. This is **testable today** via A-1a's `CAP_SET_IDENTITY` (joey spawns a
child as `principal_id=1000` and proves denied-write / allowed-read) — **not
blocked on login (A-5)**. On **dev9p**, rwx enforcement is **DEFERRED to A-3**
(user-signed-off 2026-05-30). Ground truth: Stratum's host-bake stamps pool entries
owned by the *host* uid (`stratum-fs` writes files `0644` / dirs `0755`; the server
stamps owner = the `SO_PEERCRED` auth_uid, `server.c:2019`), and the Thylacine boot
chain runs as `PRINCIPAL_SYSTEM` *without* `CAP_HOSTOWNER` — so under uniform
enforcement joey-as-*other* cannot write the pool, and the post-pivot creates
(`/var/lib/corvus`, the cross-reboot `susan`) would be **DENIED → boot brick**.
Meaningful dev9p enforcement structurally requires A-3 (per-user stratumd + F-4
identity presentation + `n_uname` trust reconciling the pool's stored uids with the
runtime identity). So at v1.0 dev9p stays **handle-RIGHT-gated only** (capability
axis — status quo, no regression); the identity-axis rwx activates at A-3.
Mechanism: the chokepoint is **Dev-gated by a `Dev.perm_enforced` flag**
(`devramfs` = true, `dev9p` = false; the A-3 activation is a one-line flip).

**Folds + flags.** A-2b's create-check (W+X on parent) lands here. A-2a audit
**F2** (gate `dev9p_stat_native` on the `Rgetattr` valid mask) is closed here — the
enforcement reads that stat, so a missing-`valid`-bit garbage mode would
mis-enforce. A-2c's mount-cape stays a **seam** (no permissionless backing is
mounted at v1.0). (Verified during impl: `/system.key` in devramfs is already
`0400` (build.sh `chmod 0400`), reported owner = `PRINCIPAL_SYSTEM` -- so a
non-system principal cannot read it post-enforcement and the boot chain (owner)
still can. The earlier "0644 -> tighten to 0600" flag was a wrong guess; no change
needed.)

**A-3 activation (RESOLVED 2026-05-31; §9.7).** The dev9p deferral above is lifted at
A-3. The reconciliation the deferral named -- "the pool's host-baked uids don't match
the runtime principal" -- is solved by stamping the pool **`PRINCIPAL_SYSTEM`-owned** at
host-bake (a Stratum `--bake-owner-uid` flag) and making **`SO_PEERCRED` carry the
connecting principal** (fix the pouch shim's pre-A-1a `ucred.uid = 0`), so the
kernel-side `perm_check` is coherent and the boot chain (owner) is not bricked. Then
`dev9p.perm_enforced` flips to **true** and the A-2d F1 (handle-rights-from-omode) + F2
(perm_check on rename/unlink) prerequisites close in the same pass. User-voted
2026-05-31 (`SO_PEERCRED` channel; flip-now). The post-2026-05-30 statement "dev9p stays
handle-RIGHT-gated only at v1.0" is **superseded by A-3** -- it held only for the A-2d
landing window.

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
- **Capability-scoped service storage (A-1.7, 2nd-order detour pulled before
  A-1b; lead angle #10).** A system service is *handed* a storage-root `KObj_Spoor`
  at spawn (reduced to `R|W`, no `TRANSFER`) and confines all persistence to it --
  no ambient FS authority beyond the handed capability (I-23). Built once, with
  corvus as first consumer. Full design: `ARCHITECTURE.md §3.6` + `NOVEL.md §3.10`;
  work-list + resume pointer: `docs/detour-status.md` A-1.7.

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

### 9.4 FS-delta -- O_PATH walkable directory handles (RESOLVED 2026-05-29; precedes A-1.7)

**The gap (empirically surfaced building A-1.7).** `SYS_WALK_OPEN` and
`SYS_WALK_CREATE` both `Tlopen` the fid they return. 9P2000.L forbids `Twalk`
from an opened fid, so a returned handle CANNOT be the base for creating or
walking a child -- only the non-opened territory root (`FROM_ROOT`, the attach
root) is a valid walk/create base. Net effect at v1.0: only single-component
entries at the territory root can be created -- **no nested directories, and no
confined writable subtree** (A-1.7's capability would be an opened subdir,
unusable as a create base). It also latently breaks `libthyla-rs::File::open`
for multi-component paths (it walks from opened intermediates). Confirmed
empirically: `mkdir var` at `FROM_ROOT` -> fd 0; `mkdir lib` UNDER it -> -1.

This conflation of *walk* and *open* is a Plan 9 divergence: Plan 9 `walk` clones
an UNOPENED Chan (the walkable namespace base); `open` is a separate step. Linux
keeps the same separation as `O_PATH` (a directory handle with no I/O, usable as
the `dirfd` base for `openat`/`mkdirat`/`renameat`/`unlinkat`). Fuchsia/Genode: a
directory handle IS the walkable capability. FS-delta restores the lineage norm
-- it is a correction, not a new feature.

**The primitive.** A new omode flag `T_OPATH = 0x80` (the first bit past the
documented Plan 9 omode set; `SYS_WALK_OPEN_OMODE_VALID` 0x13 -> 0x93). When set
on `SYS_WALK_OPEN`, the handler walks to the component but SKIPS the `dev->open`
(`Tlopen`) step, returning a NON-OPENED, walkable `KObj_Spoor` (rights envelope
`R|W|TRANSFER`, unchanged). Such a handle is the valid base for `SYS_WALK_CREATE`
/ `SYS_WALK_OPEN` / `SYS_RENAME` / `SYS_UNLINK` / `SYS_FSYNC` of CHILDREN
(clone-walk from a non-opened fid is legal); it is NOT opened for byte I/O. The
access bits (OREAD/...) are ignored when `T_OPATH` is set. Only `SYS_WALK_OPEN`
gains the flag; `SYS_WALK_CREATE` is unchanged (the mkdir pattern below
create-then-reopens).

**`mkdir -p` pattern.** For each component: `SYS_WALK_CREATE`(parent_opath, name,
DMDIR) [opened] -> `SYS_CLOSE` -> `SYS_WALK_OPEN`(parent_opath, name, `T_OPATH`)
[non-opened = the next parent]. The first parent is `FROM_ROOT` (non-opened).
Idempotent: if the dir exists the create fails (EEXIST) and the `T_OPATH`
walk_open still yields the handle. This is exactly A-1.7's storage-tree build
(`/var/lib/corvus`) + corvus's chroot target (a non-opened storage handle so
corvus can create `identity.db` under its chrooted root).

**ABI:** `T_OPATH = 0x80`; `SYS_WALK_OPEN_OMODE_VALID = 0x93`. No new syscall
number -- a flag on the existing `SYS_WALK_OPEN`. libt + libthyla-rs gain the
constant. dev9p needs NO change (the walk already produces the non-opened fid;
the handler just skips the open).

**Audit:** `sys_walk_open_handler` is an FS-mutation audit-trigger surface; the
O_PATH path is reviewed with A-1.7 (its consumer) -- the no-open path must still
bound the name, reject `..`/`/`/`\0`, stamp rights identically, and leave no
half-walked Spoor on error. **No new spec** per the 2026-05-23 spec-to-code
broadening. Lands BEFORE A-1.7.

### 9.5 A-2a -- file metadata: owner/group + chmod/chown (RESOLVED 2026-05-30)

The metadata read+write ABI that the kernel rwx-enforcement layer (A-2d) reads.
Two coordinated changes: `struct t_stat` gains owner/group (the read side), and a
new `SYS_WSTAT` drives 9P `Tsetattr` (the chmod/chown write side). As with section
9.2/9.3, the kernel 9P client already implements the wire half
(`p9_client_getattr` / `p9_client_setattr`, verified 2026-05-30) -- these were
**implemented yet unexercised** (no syscall had driven them) -- so this is syscall
wrappers + dev-vtable plumbing, NOT new protocol. **This chunk builds the
MECHANISM only; the per-file rwx PERMISSION enforcement is A-2d.** I-22 stands at
A-2a: nothing enforces rwx yet, so there is nothing to bypass.

**`struct t_stat` 72 -> 80 bytes.** Two `u32` fields appended after the 16b-gamma
tail (existing offsets unchanged): `uid` at 72 (owner principal-id), `gid` at 76
(owning group). Pinned by `_Static_assert`s on size + both offsets; mirrored in
libt + libthyla-rs (`Metadata::uid()`/`gid()`) + the pouch `0010` fstat patch
(which now translates them into musl's `st_uid`/`st_gid`, and whose hand-rolled
`struct t_stat` grows to 80 -- the kernel writes 80 bytes, so the patch's stack
buffer MUST match). No persistent on-disk consumer of this ABI exists; every
consumer rebuilds in lockstep, so the growth is a clean extension, not a break.

- **Metadata source per Dev:** `dev9p` gains a real `stat_native` (it had only the
  `.stat` -1 stub) that drives `Tgetattr` and maps `p9_attr.{mode,uid,gid,...}`
  into `t_stat` -- the load-bearing fstat for the disk-backed Stratum FS, and the
  source A-2d's enforcement reads. `devramfs` reports every entry as
  `PRINCIPAL_SYSTEM` / `GID_SYSTEM` (the read-only boot FS has no per-file owner
  table; it is system-owned, world-readable). For a Stratum file the server-
  reported uid/gid is the *connection* identity -- per-user attribution completes
  at A-3 (per-user stratumd).

**`SYS_WSTAT = 59`** -- the chmod/chown mechanism. Register-passed (no user
buffer): a v1.0 chmod/chown sets only mode/uid/gid, which fit in x1..x4.

```
SYS_WSTAT(fd, valid, mode, uid, gid) -> 0 / -1
  x0 = fd     hidx_t; KOBJ_SPOOR with RIGHT_WRITE (setattr mutates metadata).
  x1 = valid  u32 bitmask of T_WSTAT_MODE|UID|GID; >= 1 bit; reserved bit -> -1.
  x2 = mode   u32; new permission bits when T_WSTAT_MODE. The 9 rwx bits ONLY --
              setuid/setgid/sticky (07000) + any bit outside 0777 are REJECTED
              (-1). setuid is explicitly unsupported (section S5).
  x3 = uid    u32; new owner principal-id when T_WSTAT_UID. PRINCIPAL_INVALID -> -1.
  x4 = gid    u32; new group when T_WSTAT_GID. GID_INVALID -> -1.
```

- **T_WSTAT_* == P9_SETATTR_*** (bit values chosen equal; pinned by
  `_Static_assert` in `dev9p.c`, the only TU seeing both) so `dev9p_wstat_native`
  maps the mask with no translation. New NULL-permitted `Dev.wstat_native` slot
  (like `.rename`/`.unlink`/`.fsync`); `devramfs` leaves it NULL (read-only boot
  FS) -> `SYS_WSTAT` on a devramfs fd returns -1. Borrows the caller's fid and
  allocates no transient fid, so the section-9.2 failed-create UAF/fid-leak class
  structurally cannot arise.
- **Rights gate here is the HANDLE right (`RIGHT_WRITE`); the per-file
  owner-only-chmod / privileged-chown POLICY is A-2d** (the kernel rwx layer). At
  A-2a, Stratum applies `Tsetattr` unconditionally (section 3.7), so a chmod/chown
  on a dev9p fd succeeds against the server -- the kernel-side ownership-change
  policy that gates it lands with enforcement in A-2d.
- **Error cases (-1):** fd not `KOBJ_SPOOR` / missing `RIGHT_WRITE`; valid 0 or a
  reserved bit; mode outside 0777; uid/gid INVALID; Dev has no `.wstat_native`;
  server Rlerror. **Audit-bearing:** the `Tgetattr`/`Tsetattr` 9P metadata
  read+write surface (AEGIS-adjacent write path) -> full round.

**`chmod`/`chown` userspace shape:** libt + libthyla-rs surface `t_chmod(fd, mode)`
= `t_wstat(fd, T_WSTAT_MODE, ...)` and `t_chown(fd, uid, gid)` = `t_wstat(fd,
T_WSTAT_UID|T_WSTAT_GID, ...)` over the one syscall.

**No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in
this section + `docs/reference/99-fs-permission.md` + the CLAUDE.md audit-trigger
row + the runtime test suite (dev9p getattr/setattr loopback round-trips +
devramfs system-owned sentinels + joey's `/system.key` reject-path probe). **A-2d
(the kernel rwx-enforcement layer + the chmod/chown ownership-change policy) is
scripture-first: it lands as a section-3.7 refinement with the chown-privilege
model BEFORE its code.**

### 9.6 A-2d -- kernel rwx enforcement + the chmod/chown policy (RESOLVED 2026-05-30)

The enforcement layer that reads A-2a's metadata. The *policy* is §3.7.1; this is
the *implementation contract*. **No new syscall, no new cap bit, no ABI struct
change** -- A-2d is enforcement logic at existing chokepoints, reading the existing
`t_stat` (A-2a) and the existing `CAP_HOSTOWNER` (caps.h) + Proc identity (A-1a:
`principal_id` / `primary_gid` / `supp_gids[0..supp_gid_count)`).

**Two new kernel helpers** (placement -- `kernel/perm.c` vs folding into
`kernel/proc.c` -- decided at impl):
```
bool proc_in_group(const struct Proc *p, u32 gid);
    // gid == p->primary_gid || gid in p->supp_gids[0..supp_gid_count).
    // GID_INVALID is never a member.

int  perm_check(const struct Proc *p, const struct t_stat *st, unsigned want);
    // want subset of PERM_R(4)|PERM_W(2)|PERM_X(1).
    // returns 0 (allowed) / -1 (denied), per the §3.7.1 owner-first algorithm.
    // (p->caps & CAP_HOSTOWNER) short-circuits to 0 -- the DAC-override.
```

**Enforcement-point insertions** (`kernel/syscall.c`). Every insertion is gated on
the Dev's `perm_enforced` flag (the dev9p deferral, §3.7.1: `devramfs` = true,
`dev9p` = false). When the flag is false the chokepoint **skips the rwx check
entirely** (status-quo handle-RIGHT gating) -- so at v1.0 only devramfs access is
enforced; the A-3 dev9p activation is the one-line `dev9p.perm_enforced = true` flip.
- `sys_walk_open_handler`: after `src` resolves + before `dev->walk`,
  `perm_check(p, &src_stat, PERM_X)` on `src` (search the directory). After the
  walk + before `dev->open`, `perm_check(p, &nc_stat, want(omode))` on the target
  (skipped when `SYS_WALK_OPEN_OPATH` -- but the X-search above is NOT skipped).
  Both stats via `spoor_stat_native`.
- `sys_walk_create_handler`: after `parent` resolves,
  `perm_check(p, &parent_stat, PERM_W|PERM_X)`.
- `sys_wstat_handler`: the ownership-change policy (the §3.7.1 table). Reads the
  current owner via `spoor_stat_native` first, then per `valid` bit: MODE needs
  owner-or-`CAP_HOSTOWNER`; UID needs `CAP_HOSTOWNER`; GID needs
  (owner AND `proc_in_group(p, new_gid)`) or `CAP_HOSTOWNER`. The policy gate runs
  BEFORE the `dev->wstat_native` (Tsetattr) call. **Closes A-2a F2 in the same
  pass** (gate `dev9p_stat_native` on the `Rgetattr` valid mask so the owner read
  is trustworthy). The wstat policy is **also gated on `perm_enforced`** -- at v1.0
  it is dormant (devramfs `wstat_native` is NULL -> `SYS_WSTAT` returns -1 before
  the policy ever runs; dev9p is deferred), unit-tested via the policy helper, and
  activates with dev9p at A-3.

**Where the stat comes from.** `spoor_stat_native(c, &st)` (the A-2a read side):
`dev9p` → `Tgetattr`; `devramfs` → the in-kernel table (system-owned, world-r). A
Dev with no `stat_native` slot cannot be permission-checked → the access is
**denied (fail-closed)** at the enforcement point. At v1.0 every user-reachable
walk/open-capable Dev (`devramfs`, `dev9p`) implements `stat_native`, so
fail-closed is a future-Dev backstop, not a live denial. The kernel-internal
devramfs walks of the boot chain do not pass through these user syscalls (the
syscall layer is the only enforced surface, per §3.7's chokepoint model).

**Testability (now, not gated on login A-5).** New tests:
- devramfs enforcement via `CAP_SET_IDENTITY`: a child stamped `principal_id=1000`
  opens a devramfs file `OREAD` (allowed -- `other-r`), traverses a dir (allowed --
  `other-x`), and is **denied** `OWRITE`; a `PRINCIPAL_SYSTEM` peer is allowed
  (owner-rwx).
- `perm_check` unit table (owner/group/other × r/w/x × allow/deny; owner-first
  authority; `CAP_HOSTOWNER` override).
- `proc_in_group` (primary, supplementary, non-member, `GID_INVALID`).
- wstat policy: owner chmods own file (allow); non-owner chmods (deny); owner
  chowns-away (deny); `CAP_HOSTOWNER` chowns (allow); owner chgrps to own group
  (allow) / to a foreign group (deny).
- dev9p deferral: a unit asserts `dev9p`'s `perm_enforced == false` (the A-3
  activation point) -- a dev9p access is NOT rwx-gated at v1.0 (handle-RIGHT only).
- joey boot regression: the existing `/system.key` probe asserts the system
  principal still reads it post-enforcement (guard against bricking the boot chain);
  the post-pivot dev9p creates (`/var/lib/corvus`, `susan`) still succeed (dev9p
  unenforced at v1.0), proven by the existing corvus bringup + cross-reboot tests.

**Audit-bearing** (privilege boundary; the CLAUDE.md + ARCH §25.4 row lands in this
scripture commit). The rwx layer is the first real exercise of I-22's enforcement
obligation. Prosecute: the owner-first algorithm (no group/other leak when owner
bits deny); the `CAP_HOSTOWNER` override placement (only the capability, never an
identity -- I-22); the fail-closed NULL-`stat_native` path; the wstat policy
(no-give-away `chown`; chgrp-to-own-group only); the `O_PATH` R/W exemption (must
NOT also skip the X-search); the boot-chain-survival (devramfs world-r/x → no brick).

**No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in
§3.7.1 + this section + the CLAUDE.md audit-trigger row + the audit + the runtime
tests.

**Audit R1 (2026-05-30): CLEAN at v1.0 (0 P0 / 1 P1 / 1 P2 / 1 P3).** The active
surface verified sound (I-22: no `principal_id` bypasses; owner-first algorithm;
fail-closed; wstat policy; O_PATH R/W-exempt-but-X-enforced; the A-2a F2
valid-mask fix; nc lifetime; boot-chain survival). Both the P1 and the P2 are
**dormant at v1.0** (devramfs is read-only + world-readable; dev9p is
`perm_enforced=false`) -- there is no actively-reproducible escalation in the
merged state -- and are deferred to A-3 as **named activation prerequisites**:

- **F1 (P1) -- handle rights outrun the checked omode.** `sys_walk_open_handler`
  installs the KOBJ_SPOOR handle with a hardcoded `R|W[|TRANSFER]` envelope
  independent of `omode` (a pre-existing behavior; the install site already
  carries a "future enforcing Dev must derive rights" contract comment). With an
  enforced Dev, an OREAD/OEXEC open of a file the caller has only `r--`/`--x` on
  yields a writable/readable handle that `perm_check` never validated (SYS_READ/
  SYS_WRITE re-check only the RIGHT, by the open-time-snapshot design). **A-3 MUST
  derive the handle rights from `omode`** (OREAD->RIGHT_READ, OWRITE->RIGHT_WRITE,
  ORDWR->R|W, OEXEC->RIGHT_READ; O_PATH keeps the F5 navigation envelope) before
  flipping `dev9p.perm_enforced`. Dormant now: devramfs is world-readable, so no
  execute-only-no-read target exists.
- **F2 (P2) -- dir-mutation ops are RIGHT-gated only.** `sys_rename_handler` /
  `sys_unlink_handler` gate on `RIGHT_WRITE` with no `perm_check`, unlike
  `sys_walk_create_handler` (which checks W|X on the parent). An O_PATH-born `R|W`
  handle to a directory the caller has no `other-w` on could then rename/unlink
  its entries once dev9p enforces. **A-3 MUST add `perm_check(parent, W|X)`** to
  rename + unlink in lockstep with the flip. Dormant now: devramfs leaves
  `.rename`/`.unlink` NULL; dev9p is unenforced.
- **F3 (P3): subsumed** -- the wstat owner-read-then-setattr TOCTOU is the
  pre-existing surface-wide lockless `handle_get` window, not worsened by A-2d;
  rides the deferred handle-lifetime hardening pass.

The activation points carry loud in-code prerequisite notes (`kernel/dev9p.c`'s
`.perm_enforced` + the `syscall.c` handle-rights install site) so the A-3 flip
cannot miss F1/F2. Full record: `memory/audit_a2d_closed_list.md`.

### 9.7 A-3 -- 9P identity presentation + dev9p enforcement activation + per-user stratumd (RESOLVED 2026-05-31)

A-3 makes dev9p rwx enforcement real -- the activation §3.7.1 deferred. The blocker
§3.7.1 named was "the pool's host-baked uids don't match the runtime principal." A-3
reconciles them, then flips `dev9p.perm_enforced = true` and closes the A-2d
activation prerequisites (F1 + F2). It also **corrects F-4** (§3.5): the load-bearing
local identity channel is **`SO_PEERCRED`** (kernel-stamped, unforgeable), not the 9P
`n_uname` (which Stratum ignores). Two user votes (2026-05-31): the `SO_PEERCRED`
channel (over literal-F-4 `n_uname`); flip enforcement now (over defer-to-login).

**Ground truth that reshaped the design** (2026-05-31, two Explore passes + spot-verify):
- Stratum **ignores `n_uname`** at Tattach (`server.c:1007-1008`, literally "ignore");
  file ownership is stamped from the connection's **`SO_PEERCRED` uid** only
  (`server.c:2019` Tlcreate -> `s->auth_uid`; `Tsetattr` chown at `server.c:2554` is
  unconditional).
- pouch already marshals `getsockopt(SO_PEERCRED)` onto the kernel's **`SYS_srv_peer`**
  (`0006-pouch-sockets.patch`), and the 40-byte `srv_peer_info` already carries
  `principal_id` (A-1a) -- **but the shim hardcodes `ucred.uid = 0`** (a pre-A-1a stub:
  "Thylacine has no uid model").
- **Stratum A2 (`--role client` per-user proxy) is merged** on `thylacine-pouch-arm`
  (`run.c:301-317`); dataset-scope refusal is `Rlerror(EACCES)` at Tattach
  (`serve.c:137-241`). The detour-status "verify Stratum A2 first" prereq is satisfied.

**M1 -- `SO_PEERCRED` carries the principal (the local identity channel; pouch boundary-line).**
Fix the `0006-pouch-sockets.patch` SO_PEERCRED shim to marshal `ucred.uid =
info.principal_id` and `ucred.gid = info.primary_gid` (was `0`/`0`). `info` is the
`pouch_srv_peer_info` the shim already reads from `SYS_srv_peer`. Effect:
stratumd-in-Thylacine's `peer_creds()` returns the connecting Proc's principal as
`auth_uid`, so its create-owner stamp records Thylacine principals. **The principal is
kernel-stamped** (`SYS_srv_peer` reads the peer Proc's durable `principal_id` via its
stripes) -- a connecting Proc **cannot forge** it. This preserves I-22 (no identity
self-elevation) and is the property that lets the trusted-local server believe the
presented identity without `Tauth`.

**M2 -- host-bake stamps `PRINCIPAL_SYSTEM` (Stratum + build.sh).** The host-bake runs
`stratumd` + `stratum-fs write` as the host build user, so baked files are owner = host
uid (501/1000), not a Thylacine principal. Add a **`--bake-owner-uid <u32>` /
`--bake-owner-gid <u32>`** flag to `stratumd` that overrides `s->auth_uid`/`s->auth_gid`
at/just-before `stm_9p_server_create` for that session. **Not an on-disk-format change**
-- `si_uid`/`si_gid` already exist (`inode.h:202-203`); only the stamped VALUE changes
(no `STM_UB_VERSION` bump). `tools/build.sh::build_stratum_pool_fixture` passes
`--bake-owner-uid 4294967294` (`PRINCIPAL_SYSTEM`) + `--bake-owner-gid <GID_SYSTEM>`.
Modes are the existing stamps (files `0644`, dirs `0755`, `/system.key` `0400`); since
the boot chain is `PRINCIPAL_SYSTEM` = the owner, owner-bit access covers read /
traverse / write everywhere it needs -- no brick. v1.0 bakes no user files; users are
created at runtime by corvus, owned by their creator's principal via M1.

**M3 -- flip `dev9p.perm_enforced = true` + close F1 + F2 (kernel).**
- The flip: `kernel/dev9p.c` `.perm_enforced = true`; replace the A-3-prerequisites
  deferral comment with the activated state.
- **F1 (A-2d P1) -- derive handle rights from `omode`.** `sys_walk_open_handler` installs
  the KOBJ_SPOOR handle rights from `omode`, not a hardcoded `R|W[|TRANSFER]`:
  `OREAD -> RIGHT_READ`; `OWRITE -> RIGHT_WRITE`; `ORDWR -> RIGHT_READ|RIGHT_WRITE`;
  `OEXEC -> RIGHT_READ` (read-implied; documented); `+OTRUNC -> | RIGHT_WRITE`. A
  normally-opened handle keeps `RIGHT_TRANSFER`.
  - **`T_OPATH` keeps the A-1.7/F5 born-`R|W` navigation envelope, no `TRANSFER`** -- an
    O_PATH walk is a directory/capability base (a service confined to a handed storage
    root creates under it), so it MUST stay `R|W` regardless of the (absent) access mode.
    This is the one case that does NOT derive from `omode & 3`. Preserving it is
    load-bearing for A-1.7 (I-23) -- do not regress the storage-capability base.
- **F2 (A-2d P2) -- `perm_check` on dir mutation.** `sys_rename_handler` (BOTH the old and
  new parent dirs) + `sys_unlink_handler` (the parent dir) gain
  `if (dir->dev->perm_enforced) { spoor_stat_native; perm_check(p, &st, PERM_W|PERM_X); }`,
  mirroring `sys_walk_create_handler`.

**M4 -- `n_uname` forwarding (belt-and-suspenders; kernel).** The two Tattach sites
(`syscall.c:1113`, `:1313`) substitute the calling Proc's `principal_id` for the
`n_uname` field (the userspace-supplied value becomes vestigial; the syscall ABI is
unchanged -- the arg stays, ignored). Against Stratum this is a **no-op** (Stratum
ignores n_uname; M1 is the live channel); it is forward-compat for a future **foreign**
9P server that honors n_uname but has no `SO_PEERCRED` (a remote/TCP transport -- none
exists at v1.0).

**M5 -- trust-stamp gate is a v1.x SEAM (no v1.0 caller).** F-4's "corvus stamps the
`/srv` posting; the kernel forwards an asserted identity ONLY to stamped servers"
guarded against leaking identity to an untrusted server. Under M1 that concern is
**structurally absent at v1.0**: every v1.0 attach is **local** (a `SrvConn`), so the
presented identity is the kernel-stamped `SYS_srv_peer` principal (unforgeable, and
already revealed to any server the Proc connects to -- that is how `SO_PEERCRED` works).
There is no `n_uname`-assert-to-untrusted-server path because there are no remote
transports. So the trust-stamp registry is **not built** (convergence bar §8.1:
build-iff-caller). **Seam trigger (recorded):** when a v1.x remote/foreign 9P transport
lands, the kernel MUST gate the M4 `n_uname` assertion on a corvus-stamped trust bit on
the service/connection before asserting identity to a server whose peer it does not
kernel-stamp. Ground truth: no `trusted_for_identity_fwd` field exists on
`SrvService`/`SrvConn` today -- the seam is a clean add.

**M6 -- per-user stratumd (`--role client`) mechanism; consumer = A-5.** Stratum A2 is
merged. A-3 proves the mechanism is reachable: a probe attaches an out-of-scope dataset
and observes `Rlerror(EACCES)` at Tattach. The per-login spawn of a user's `--role
client` stratumd (scoped to that user's datasets, so its `SO_PEERCRED`-stamped creates
are owner = that user) is **A-5 (login)** -- no v1.0 caller exists pre-login. **v1.0
shared-mount note:** the boot chain attaches one mount (joey's `SrvConn`); pre-login
every Proc is `PRINCIPAL_SYSTEM`, so the single connection stamping `PRINCIPAL_SYSTEM`
is correct. The "shared mount stamps the attacher's principal" limitation is exactly
what per-user stratumd resolves at login -- and the kernel `perm_check` (per-Proc, at
the chokepoint) already denies a non-system Proc's write to a SYSTEM-owned dir BEFORE
the create reaches the server, so enforcement is per-Proc-correct even over the shared
mount.

**Invariants.**
- **I-22 preserved** -- the `SO_PEERCRED` principal is kernel-stamped (`SYS_srv_peer`),
  not client-asserted; no identity self-elevates. `CAP_HOSTOWNER` remains the only
  DAC-override (a capability, not an identity).
- **I-2 / I-4 / I-6 unaffected** -- M1-M6 add no capability, no handle-transfer path, no
  rights expansion. F1 *narrows* the handle-rights envelope (omode-derived) --
  monotonic-reduction-friendly.
- **A-1.7 (I-23) preserved** -- F1's `T_OPATH` carve-out keeps the storage-capability
  base born `R|W`.
- **No-brick** -- M2 makes the boot chain the owner of the baked corpus; `boot OK` +
  cross-reboot PASS are the gate.

**Testability (now, not gated on login A-5).**
- A `CAP_SET_IDENTITY` non-system child (`principal_id=1000`) attempting to
  create/`OWRITE` under a SYSTEM-owned dev9p directory is **denied** at the kernel
  chokepoint (other-bits lack W); the boot chain (owner) succeeds. Same proof shape as
  the A-2d devramfs test, now on dev9p.
- F1: an `OREAD` open of a dev9p file yields a `RIGHT_READ`-only handle (a subsequent
  `SYS_WRITE` fails the RIGHT gate); an `O_PATH` walk still yields `R|W` (the A-1.7 base).
- F2: a non-`other-w` directory cannot be rename/unlink-mutated by a non-owner once
  dev9p enforces.
- M6: out-of-scope dataset attach -> `EACCES`.
- joey boot regression + cross-reboot: the post-pivot creates (`/var/lib/corvus`, the
  cross-reboot `susan`) still succeed (boot chain owns the SYSTEM pool).

**Split.** **A-3a** (reconciliation: M1 pouch + M2 Stratum/build.sh + M4 n_uname) ->
**A-3b** (activation: M3 flip + F1 + F2) -> **A-3c** (M6 mechanism proof + M5 seam) ->
**one focused audit** over the privilege boundary (AEGIS-adjacent write path again --
prosecute hard). A-3a precedes A-3b (the flip bricks without the reconciliation in place).

**Audit-bearing** (privilege boundary; the CLAUDE.md + ARCH §25.4 rows land in this
scripture commit). Prosecute: kernel-stamped-vs-forgeable identity (I-22); F1's
rights-narrowing surfacing any latent wrong-`omode` caller AND preserving the `T_OPATH`
base; F2 covering both rename parents; the host-bake SYSTEM-owned no-brick; the dev9p
flip end-to-end; the Stratum `--bake-owner-uid` override placement (must apply to every
create on the bake session, must NOT leak into a non-bake stratumd).

**No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in §3.5
(corrected F-4) + §3.7.1 + this section + the audit-trigger rows + the audit + the
runtime tests.

### 9.8 A-4 -- clearance + legate elevation + CAP_KILL + trusted path (RESOLVED 2026-06-01)

The highest-stakes privilege surface of the arc. Two user votes 2026-06-01, taken
after a Plan 9 + capability-microkernel + trusted-path prior-art pass (Plan 9
`/proc`-ctl, seL4/Zircon/Genode derived authority, NT/AIX SAK, Genode Nitpicker):
**(1) cross-process kill = BOTH the namespace `/proc/<pid>/ctl` surface AND a narrow
elevation-only `CAP_KILL`; (2) the trusted path = build the kernel SAK now, pulling
the kernel console RX path forward.** The recurring fusion: the **Plan 9 idiom as the
spine** (per-Proc namespace, files-as-interface, kernel-owned console,
ownership-as-authority), the **capability-microkernel SOTA as the rigor** (kernel-
stamped derived authority, capability-IS-permission, group-granularity, tiny TCB),
**corvus as the clearance/identity authority**, and the **#809/#811 die-at-EL0-checkpoint
machinery as the shared enforcement primitive** -- with **no local crypto** (the kernel
cap-stamp is the unforgeable substrate, exactly as seL4 CNodes / Zircon handles are).

**New capabilities** (`kernel/include/thylacine/caps.h`; bits append after
`CAP_SET_IDENTITY = 1<<5`):
- `CAP_GRANT_CLEARANCE = 1<<6` -- **fork-grantable** (joins `CAP_ALL`). Authorizes
  registering a pending *clearance* grant on the `cap` device (the legate analog of
  `CAP_GRANT_HOSTOWNER`). corvus alone holds it (conferred in corvus's spawn mask, like
  `CAP_GRANT_HOSTOWNER`); no ordinary Proc receives it.
- `CAP_DAC_OVERRIDE = 1<<7` -- **elevation-only**. The fs-admin DAC-override split out of
  `CAP_HOSTOWNER` (the `perm_check` bypass), now grantable as a finer clearance (§3.7.1
  foreshadowed this split).
- `CAP_CHOWN = 1<<8` -- **elevation-only**. chown/chgrp-any, split out of `CAP_HOSTOWNER`.
- `CAP_KILL = 1<<9` -- **elevation-only**. The cross-identity kill override (the third
  authority axis on `/proc/<pid>/ctl`, A-4b).
- **`CAP_ELEVATION_ONLY` expands** `{CAP_HOSTOWNER}` -> `{CAP_HOSTOWNER, CAP_DAC_OVERRIDE,
  CAP_CHOWN, CAP_KILL}` (bits 3,7,8,9). All four are acquired ONLY through the `cap`
  device and are **rfork-stripped**. `CAP_ALL` gains only `CAP_GRANT_CLEARANCE` (bit 6);
  the elevation-only four stay excluded.
- **Why `CAP_KILL` is elevation-only (rfork-stripped), not fork-grantable:** a kill-anyone
  right must not leak to a legate's children; the supervisor/debugger Proc itself holds it;
  killing your OWN children never needs it (parent authority already covers that, A-4b). A
  clearance's caps that DO flow to a subtree (§3.1 "attenuate to children") are the
  *fork-grantable* members of its set; the elevation-only members stay on the legate root.

**A-4-pre -- close the P5-hostowner I-2 hole** (the named A-4 prerequisite; detour-status
trip-hazard; surfaced by the A-1a audit):
- `kernel/proc.c::rfork_internal` (`child->caps = parent_caps & caps_mask`) ANDs
  additionally with `~CAP_ELEVATION_ONLY` (the now-4-bit set), matching the `caps.h`
  contract it currently violates. Closes the real I-2 hole (a `CAP_HOSTOWNER`-elevated
  parent could `rfork`/`SYS_SPAWN_WITH_CAPS` a child inheriting `CAP_HOSTOWNER`, bypassing
  the console-gated `ADMIN_ELEVATE`). Regression test + sibling-elevation sweep (`devcap`
  redeem is correctly console+grant gated; `SYS_SPAWN_WITH_CAPS` routes through
  `rfork_internal` -> the one fix covers it). Lands first; folds into the A-4a audit.

**A-4a -- clearance levels + the legate.**

*Clearance level* (corvus-held policy object; persisted in corvus's storage cap
`/var/lib/corvus`):
```
clearance_level := {
    name          : utf8 (bounded),
    caps          : structured cap-set -- a versioned TLV, NOT a bare u64, so v1.x
                    resource-scoping (per-file / per-device allowlists) is ADDITIVE,
    auth_required : enum { re_auth, distinct_secret, system_key, hostowner_cosign },
    time_bound    : u64 ns (0 = none),
    scope_kind    : enum { subtree }     (v1.0; resource-scoped is v1.x),
}
```
Per-user eligibility = the level's unlock material wrapped under the user's hybrid keypair
(CRVS wrap). Grant = create the wrap; revoke = delete it (per-user, no shared-secret
rotation). An active legate keeps already-stamped caps until scope exit; revoke blocks
*future* activation (scripture §3.1).

*corvus verbs* (binary frames, CORVUS-DESIGN §6.4; verb_ids continue from 13):
- `CLEARANCE_LIST = 14` -- levels the session's user is eligible for + their caps.
- `CLEARANCE_ACTIVATE = 15` -- request `{level, self_restriction_mask, valid_until_req}`;
  corvus verifies the level's `auth_required` (the trusted path for high-stakes, A-4c),
  then registers the kernel cap-device pending clearance grant against the caller's
  stripes; returns OK + the assigned `legate_session_id`.
- `CLEARANCE_GRANT = 16` / `CLEARANCE_REVOKE = 17` -- hostowner manages eligibility
  (`CAP_HOSTOWNER`-gated, like `USER_CREATE` / `GROUP_CREATE`).

*Kernel legate mechanism* (the `cap` device generalized + the redeem):
- The `cap` device **`grant`** file generalizes from "CAP_HOSTOWNER only" to an arbitrary
  clearance grant `{cap_mask, target_stripes, valid_until, session_id}`. Gate: writer holds
  `CAP_GRANT_CLEARANCE` (corvus) for a clearance grant, OR `CAP_GRANT_HOSTOWNER` for the
  legacy hostowner grant (unchanged). corvus cannot write another Proc's caps directly (it
  is userspace) -- this two-phase grant is the only path, exactly as `ADMIN_ELEVATE` works
  today (CORVUS-DESIGN §5.5). *As-built bridge (A-4a-3):* corvus is chrooted to its storage
  cap, so it reaches the cap device by **syscall**, not a `/cap` file walk -- the clearance
  grant rides a dedicated `SYS_CAP_GRANT_CLEARANCE` (the grant-side analog of the existing
  `SYS_CAP_GRANT` hostowner bridge); the redeem rides the existing `SYS_CAP_USE` (no new
  redeem syscall -- its handler kind-branches to the clearance core).
- The `cap` device **`use`** file (redeem): the activating Proc redeems its own pending
  grant; the kernel stamps `current->caps |= (granted_cap_mask & self_restriction)` and
  records the legate context. Gate: a pending grant exists for the writer's own stripes.
  Console-attach is NOT required for ordinary clearance (only the hostowner grant requires
  it); high-stakes auth is enforced corvus-side via `auth_required` before the grant is
  ever registered.
- New `struct Proc` fields (append after the identity block; KP_ZERO -> not-a-legate):
  - `u32 legate_session_id` -- ephemeral attribution id (0 = not a legate). Audit reads
    "principal P via legate-session N". Durable `principal_id` UNCHANGED (scripture §3.1:
    do not mint a fresh principal -- the legate is the same human, more authority).
  - `u32 legate_scope_id` -- the scope this Proc belongs to (0 = none); set on the legate
    root at redeem; **INHERITED across rfork** (a child of a legate-scoped Proc joins the
    scope). The subtree-membership tag.
  - `u64 legate_valid_until` -- ns deadline (0 = none); copied from the grant.
- *Self-restriction* (STS-style, I-2): the redeem's `self_restriction` mask can only reduce
  the granted set (`& self_restriction`); the Proc voluntarily narrows below the ceiling.
- *Evaporation = scope teardown.* On the legate root's exit, OR when `now >
  legate_valid_until` (checked at the EL0-return tail -- the same checkpoint #811
  generalizes), the kernel walks all Procs carrying this `legate_scope_id` and
  `proc_group_terminate`s each (reusing the #809/#811 cascade). An elevated window leaves
  NO elevated Proc running past it -- stronger than sudo (whose backgrounded jobs survive);
  the security-correct call. [Alternative considered: cap-drop-to-baseline instead of
  teardown -- gentler, but leaves elevated-but-orphaned Procs + an audit gap; rejected for
  v1.0.]
- *No local crypto* (scripture §3.1): the kernel cap-stamp + the cap-device grant
  (authorized by `CAP_GRANT_CLEARANCE`, held only by corvus) is the enforcement; corvus's
  authority to register the grant is the trust root, and it verified `auth_required` first.
  Distributed crypto-proof stays a v1.x seam (§8.4).

**A-4b -- cross-process kill: `/proc/<pid>/ctl` (namespace) + `CAP_KILL` (capability), the
two-axis fusion.**
- `/proc/<pid>/ctl` ALREADY exists + is writable (`devproc.c`; today a stub whose own
  comment anticipates "parses the verb kill / stop / start / notepg + dispatches via the
  notes layer"). A-4b makes it real: a write of `kill\n` / `killgrp\n` resolves `<pid>` ->
  `struct Proc` and posts the kill (single-thread: `notes_post`; multi-thread:
  `proc_group_terminate` -- both built + audited, #809/#811). `stop`/`start` stay stubbed
  (scheduler integration, ARCH [OPEN Q 7.6.D], Phase 7). No `/proc/<pid>/note` file at v1.0
  -- kill rides `ctl` (Plan 9 idiom; the note-file is ARCH [OPEN Q 7.6.A]'s later form).
- *Ownership model + enforcement SITE for `/proc`* (RECONCILED 2026-06-01, user vote --
  supersedes the design-time "perm_enforced=true + gate-at-open" sketch): devproc gains
  `stat_native` reporting the TARGET Proc's `principal_id` (uid) + `primary_gid` (gid) +
  per-file mode (`ctl` = `0600` owner-rw, the Plan 9 "process files owned + read-protected
  by the owner" rule; `status`/`cmdline`/`ns` = `0444`). devproc keeps `perm_enforced =
  FALSE` and the two-axis authority is checked at the **write** site (`devproc_write`, when
  the `kill`/`killgrp` verb is consumed), NOT at open. WHY the reconcile: the SHARED open
  chokepoint (`sys_walk_open_handler`) hard-rejects on `perm_check` failure BEFORE
  `devproc.open` runs, so a `CAP_KILL`-holding non-owner (ctl is `0600`) could never reach
  the devproc `CAP_KILL` check the open-gate model itself requires -- "perm_enforced=true +
  gate-at-open" and "`CAP_KILL` checked in the devproc path" cannot BOTH hold as-built.
  Write-time enforcement resolves it cleanly: the composite axis lives in one place, is
  fresh at the kill, COMPLETELY wires `CAP_KILL` (for when A-4c makes the supervisor
  clearance grantable -- not a half-version), and touches no shared code. Open of `ctl` is
  gated by namespace visibility only (I-1) -- an unauthorized opener holds a powerless
  handle; the WRITE is the gate.
- *Two-axis authority* (the A-2d pattern -- the capability axis AND the identity axis,
  orthogonal -- generalized to kill): a `kill`/`killgrp` write is authorized iff the caller
  is the target's OWNER (owner-rwx on the `0600` ctl file -- the owner always holds the
  w-bit, so this reduces to "same `principal_id` as the target"; covers killing your own
  processes/subtree, and the parent-of-same-identity-child case §7.6.3 is now expressible as
  ownership, not a special case) OR the caller holds `CAP_HOSTOWNER` (the unified admin) OR
  `CAP_KILL` (the cross-identity override for a debugger/supervisor that is neither owner nor
  hostowner). devproc enforces this I-26 set DIRECTLY in `devproc_write` (it does NOT route
  through the generic `perm_check` DAC-override -- consistent with `perm_enforced=false`):
  `CAP_DAC_OVERRIDE` (the generic fs-rwx admin, A-4) is therefore deliberately NOT a kill
  axis -- the A-4 capability split keeps fs-admin and process-kill ORTHOGONAL (mirrors Linux
  `CAP_DAC_OVERRIDE` vs `CAP_KILL`); an fs-admin is not implicitly a process-killer. (At
  v1.0 ctl's mode is fixed at `0600` and devproc has no `wstat_native`, so "owner-rwx" and
  "same principal" are identical and immutable; if a future ctl mode gained group/other
  w-bits, the owner-rwx computation would generalize -- still WITHOUT the DAC-override.) No
  identity bypasses (I-22 -- `CAP_HOSTOWNER`/`CAP_KILL` are capabilities, never identities);
  containment is namespace visibility (I-1 -- a Proc that cannot walk to `/proc/<pid>` cannot
  kill it).
- *Dispatch* (RECONCILED): both `kill` and `killgrp` terminate the target Proc's
  thread-group via `proc_group_terminate` UNIFORMLY (single + multi thread), under
  `g_proc_table_lock` via the `proc_for_each` resolve+authorize+kill idiom (the audited
  `sys_postnote` pattern -- the target is alive under the lock, so no reap-UAF). WHY uniform
  (supersedes the design-time "single-thread: notes_post; multi-thread: proc_group_terminate"
  split): post-#811, `proc_group_terminate` is the ONLY termination primitive whose
  death-wake is TOTAL (universal death-interruptible sleep, §8.8.1) -- `notes_post("kill")`
  wakes only note-pollers, so a single-thread target blocked in a non-notes sleep (e.g.
  `poll(-1)`) would NOT wake and the kill would hang until that sleep completed on its own.
  v1.0 has no cross-Proc process groups (no `setpgrp`), so `kill` and `killgrp` both map to
  "terminate this Proc's thread-group"; a distinct cross-Proc `killgrp` is a v1.x seam
  (pending process groups). `stop`/`start` stay stubbed (scheduler integration, ARCH [OPEN Q
  7.6.D]). USER-REACHABILITY of `/proc/<pid>/ctl` is a documented Utopia-lane namespace seam,
  NOT part of A-4b (user-confirmed 2026-06-01): devproc is kernel-internal at v1.0 (joey's
  territory is a single root -- `chroot` to devramfs, `pivot_root` to Stratum FS -- with no
  synthetic Dev grafted in), and reaching a 3-component path that crosses the
  devramfs->devproc mount boundary needs (a) a boot-path `SYS_MOUNT` of devproc at `/proc`
  AND (b) the production multi-component, mount-crossing path resolver (Plan 9 `namec`) --
  the v1.0 `SYS_WALK_OPEN` is single-component + dev-internal (it rejects `/`). That resolver
  is a namespace subsystem serving `/proc` + `/ctl` + `/dev` + `/net` uniformly (ROADMAP
  Utopia). `SYS_POSTNOTE` already gives userspace the parent-kill path today. A-4b therefore
  lands + KERNEL-UNIT-TESTS the mechanism + the two-axis authority (the load-bearing privilege
  logic is fully exercisable in-kernel: construct caller/target Procs, assert the authority
  decision + the `group_exit_msg` dispatch observable).
- Resolves ARCH **[OPEN Q 7.6.B]** (`CAP_KILL`) + advances **[OPEN Q 7.6.A]** (namespace
  posting -- the `ctl` form lands; the `note`-file form stays future).

**A-4c -- trusted path: the kernel SAK + the console-RX pull-forward** (per the 2026-06-01
vote: build the SAK now):
- *Part 1 -- kernel UART console RX pull-forward* (Phase-4-G-deferred infra, pulled into
  A-4 because the SAK has no input chokepoint without it): `arch/arm64/uart.c` gains RX IRQ
  handling (GIC-registered) feeding a kernel console input ring; `cons.c::devcons_read`
  becomes a real blocking read on that ring via a `Rendez` (today it returns EOF); `Ctrl-C`
  (0x03) on the RX path posts an `interrupt` note to the console-attached Proc (ARCH §7.6.5
  "future cons ^C"). On the kernel UART console Dev (`dc='c'`) -- NOT the userspace
  VirtIO-input (graphical keyboard) path, which stays userspace (ARCH §17.1: "keyboard via
  UART or VirtIO input"); the two coexist. Audit-bearing (a new EL0-bound input path).
- *Part 2 -- the SAK + trusted-path handoff*: a kernel-recognized Secure Attention Key
  sequence sits at the RX chokepoint, inspected BEFORE any byte reaches an EL0 reader. On
  recognition the kernel (a) revokes the console-attach bit from the current holder via a
  new `proc_revoke_console_attached` (the missing unset side of the set-once
  `PROC_FLAG_CONSOLE_ATTACHED`; the SAK is the sole revoker) + posts it a notify note; (b)
  re-grants the console-attach bit to corvus (or the kernel's trusted login Proc); so (c)
  the immediately-following elevation / login prompt is provably the TCB's, because only
  the console-attach holder may redeem `CAP_HOSTOWNER` or a high-stakes clearance (the
  cap-device redeem gate). Requires a single kernel "current console owner" pointer (none
  today -- only the scattered per-Proc flag). NT/AIX SAK + Plan-9 kernel-owned-console +
  Genode tiny-TCB discipline; the graphical Nitpicker-style labeled trusted screen is a
  Halcyon seam.
- *SAK sequence*: pinned in the A-4c impl + reference doc -- a control sequence that cannot
  occur in normal input (a `BREAK`, or a reserved multi-byte escape).

*As-built resolution* (2026-06-01, the implementer's calls within the approved shape, after a
GIC/UART/notes/poll ground-truth pass + a Plan 9 / Linux-serial-SysRq / NT-AIX-SAK prior-art
pass; landed scripture-first, no code):
- **SAK = serial BREAK.** Of the two named options, BREAK is chosen: it is a *line condition,
  not a data byte*, so EL0 data cannot forge it (the PL011 raises the break flag, DR bit-10
  `BE`, only on a real line break -- never from a written byte), and its recognizer is
  *stateless* (no multi-byte state machine to starve or partially-spoof). Together these make
  the audit obligation "the SAK recognizer cannot be starved/spoofed by crafted input"
  STRUCTURAL rather than a property to defend. It is the canonical serial trusted-attention
  signal (Linux magic-SysRq-over-serial; the NT Ctrl-Alt-Del / AIX SAK "un-typeable attention"
  lineage). QEMU's PL011 sets `DR.BE` on a host break event, so `Ctrl-A b` over `-serial
  mon:stdio` is a real interactive test path. The "reserved multi-byte escape" alternative is
  REJECTED: more recognizer surface + a byte sequence is producible by any writer to the input
  stream, weakening the "cannot occur in normal input" property.
- **Deferred-action via a console-manager kthread.** The RX IRQ handler runs in IRQ context,
  where `notes_post` (plain `spin_lock`, NOT irqsave) and `poll_waiter_list_wake` (ditto) are
  UNSAFE to call -- only `wakeup()` on a `Rendez` is IRQ-safe. So the handler does ONLY: drain
  the PL011 RX FIFO into a console input ring (under an irqsave ring lock); classify each
  entry (`BREAK` -> SAK-pending; `Ctrl-C` 0x03 -> interrupt-pending, cooked-consumed; other ->
  data); then `wakeup()` the data `Rendez` (a blocked reader) AND the console-manager `Rendez`
  (any pending deferred action). A dedicated kproc kernel thread (`console_mgr`) sleeps on its
  `Rendez` and, in *process context*, performs the privileged/blocking work: post the
  `interrupt` note to the current console owner (Ctrl-C), and the SAK revoke/re-grant. This
  mirrors the kernel's "defer privileged work out of IRQ context" discipline (note delivery at
  the EL0-return tail).
- **Console-owner pointer.** A single kernel `g_console_owner` (`struct Proc *`) protected by
  `g_proc_table_lock` (the proc-lifecycle lock -- closes the TOCTOU vs the owner exiting).
  Initialized to joey at boot (the boot console-attach anchor, joey.c:104). `exits()` clears
  `g_console_owner` when the exiting Proc is the owner (no dangling pointer).
  `proc_revoke_console_attached(p)` clears `PROC_FLAG_CONSOLE_ATTACHED` via `__atomic_and_fetch`
  -- the SAK runs on the kthread (a DIFFERENT thread than the owner), so the proc_flags
  single-writer convention requires atomicity here (the documented multi-thread lift);
  `proc_mark_console_attached` is made correspondingly atomic.
- **SAK transition (I-27 handoff; as-built post-RW-7 `@2608c88` — ATTACH and OWNER are
  distinct roles).** Under `g_proc_table_lock`, on the kthread: (1) revoke
  `PROC_FLAG_CONSOLE_ATTACHED` from the live current owner (no note is posted — a dedicated
  `hangup`/`console-revoked` note name is the RW-7 R2-F3 v1.x notes SEAM; until then the
  attach-bit revoke is the SAK's observable effect on the old owner); (2) set
  `g_console_owner = NULL` unconditionally — corvus is the login AUTHORITY, never a Ctrl-C
  target; making it the owner meant a post-SAK Ctrl-C posted `interrupt` to corvus and killed
  the trusted path until reboot (RW-7 R2-F1). The Ctrl-C owner is re-established when login
  spawns the session shell (`SPAWN_PERM_CONSOLE_OWNER`); during the login window there is no
  foreground terminate target; (3) re-grant the ATTACH bit only (elevation authority, never
  ownership) to corvus via the single `g_console_trusted_proc` pointer set when joey
  establishes corvus. FAIL-SAFE: if no trusted proc is registered/alive, the SAK is
  REVOKE-ONLY (no Proc can then redeem elevation until a trusted login claims the console --
  the security-correct default). Idempotent under a BREAK flood. Guarantees the post-SAK
  redeemer is the TCB's (the devcap redeem gate keys on `PROC_FLAG_CONSOLE_ATTACHED`,
  devcap.c:310).
- **Data wait = single `Rendez` + single-reader busy-guard.** `poll_waiter_list_wake` is not
  IRQ-safe, so the data-ready wake uses a `Rendez` + `wakeup()` (IRQ-safe). The console is a
  single-reader resource; a 2nd concurrent blocking `devcons_read` returns -1 (rather than the
  single-waiter `Rendez`'s second-sleeper extinction) via a busy-guard under the ring lock. (A
  v1.x multi-reader console lifts to an IRQ-safe multi-waiter wake.)
- **Ctrl-C + BREAK are cooked-consumed** (generate the note / SAK; not enqueued as data) -- v1.0
  has no consctl/termios (cons.c is held for the Phase-5 PTY surface), so cons is otherwise raw.
- **PL011 IRQ = SPI 33** (QEMU virt), hardcoded as the platform fallback in `arch/arm64/uart.c`
  (mirrors the existing base-address fallback; DTB `interrupts`-property parsing does not exist
  and is a Lazarus/portability follow-up -- a SEAM, not a half-version, since v1.0 targets QEMU
  virt). Reserved in `irqfwd_init` (like the timer PPI 30) so userspace `SYS_IRQ_CREATE` cannot
  claim it (SPI 33 >= 32 is otherwise claimable). Coexists with the userspace virtio-input path
  (ARCH §17.1) -- separate IRQ lines, separate Devs.
- **Test strategy (harness-honest).** The integration harness cannot inject UART RX bytes (or a
  BREAK) non-interactively: `-serial mon:stdio` run with `< /dev/null`, one PL011, no QMP
  serial-byte channel; a bidirectional-socket retrofit would touch the boot-banner test ABI
  (off-limits). So A-4c is proven by (a) in-kernel unit tests driving the RX handler + ring +
  BREAK/Ctrl-C recognizer + `g_console_owner` transitions + `proc_revoke_console_attached`
  synthetically (the 671-suite pattern), (b) boot survival (GIC attach/enable + IMSC.RXIM
  unmask without an IRQ storm), and (c) the interactive `Ctrl-A b` manual path (a real host
  BREAK -> DR.BE -> SAK). "Real hardware IRQ end-to-end" is validated by (b)+(c), not by
  injected automated input.
- **Split (as-built):** A-4c-1 (console RX -- RX IRQ + ring + blocking `devcons_read` + Ctrl-C +
  the `console_mgr` kthread + `g_console_owner` + the `exits()` clear-hook) lands + audits
  first; A-4c-2 (the BREAK->SAK revoke/re-grant + `g_console_trusted_proc` + the I-27 handoff)
  lands + audits second. **Both LANDED** (c-1 impl `426c10e` / audit `e043334`; c-2 the SAK
  trusted-path handoff -- `proc_console_sak` + `g_console_trusted_proc` + the
  `SPAWN_PERM_CONSOLE_TRUSTED` corvus designation; see `docs/detour-status.md` A-4c). The
  SAK-revoke courtesy note reuses the `interrupt` name (the closed notes table has no dedicated
  "console-revoked" name; a distinct one is a v1.x notes SEAM, additive when a consumer needs
  to distinguish SAK-revoke from Ctrl-C). c-2 impl + audit close `a0f6163` (R1 CLEAN, 0/1/0/2
  all fixed; the A-4 arc is DONE). **A-4-pre + A-4a + A-4b + A-4c all LANDED + audited CLEAN.**

**New invariants** (land in ARCHITECTURE.md §28; mirrored here):
- **I-25 -- legate scope is bounded + fully revoked.** A legate's elevated caps are bounded
  to its `legate_scope_id` subtree, attenuate to children by I-2, and are FULLY revoked (the
  scope torn down via group-terminate) on the legate root's exit or `valid_until` expiry; no
  elevated Proc outlives the scope; the durable identity is unchanged by elevation.
- **I-26 -- cross-process control authority is explicit + two-axis.** A `kill`/`killgrp`
  write to `/proc/<pid>/ctl` is authorized only by owner-rwx on the ctl file (identity axis)
  OR `CAP_HOSTOWNER` OR `CAP_KILL` (capability axis); no identity carries ambient kill
  authority (composes I-22); containment is namespace visibility (I-1).
- **I-27 -- trusted path: the elevation prompt is unspoofable.** After a kernel SAK, the
  console-attach bit is held only by corvus / the trusted login Proc, and only the
  console-attach holder may redeem `CAP_HOSTOWNER` / a high-stakes clearance -- so an
  interposer that drew a fake prompt cannot complete an elevation; the SAK keystroke is
  recognized in the kernel RX path before EL0 delivery.

**Split.** A-4-pre (I-2 fix) -> A-4a (clearance + legate) -> A-4b (`/proc` ctl kill +
`CAP_KILL`) -> A-4c-1 (console RX) -> A-4c-2 (SAK + trusted path). Each is audit-bearing
(privilege boundary + the input path) and gets a focused adversarial round -- the
highest-stakes privilege surface; prosecute hard. A-4-pre lands first and folds into A-4a's
round.

**Audit-bearing** (the CLAUDE.md + ARCH §25.4 rows land in this scripture commit).
Prosecute: I-2 (no elevation-only leak across rfork), I-25 (no cap leak past scope; no
orphaned elevated Proc; teardown is exactly-once + reuses #809/#811 correctly), I-26 (no
identity bypass of the kill gate; namespace containment; parent-case still works), I-27 (no
interposer completes an elevation; the SAK is unspoofable + recognized pre-EL0), I-22 (the
override is always a capability, never an identity), the cap-device grant lifecycle (no
grant replay, no cross-stripes redeem, `valid_until` honored, `self_restriction` only
reduces), and the console RX path (no ring overflow, no missed-wakeup on the `Rendez`,
correct Ctrl-C delivery, the SAK recognizer cannot be starved or spoofed by crafted input).

**Seams** (foreseeable, additive): resource-scoped caps (the structured `caps` TLV carries
an allowlist; SMMU [Stop B] makes it enforceable); distributed clearance crypto-proof (the
CPU-server case, §8.4); the graphical trusted screen (Halcyon / Nitpicker); a finer
per-target kill *handle* (vs the blanket `CAP_KILL`) if a concrete consumer appears.

**No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in §3.1 +
this section + the ARCH §28 invariants (I-25 / I-26 / I-27) + the audit-trigger rows + the
per-sub-chunk audits + the runtime tests.

### 9.9 A-5 -- login + session lifecycle + per-user encrypted home (RESOLVED 2026-06-02)

The capstone integration of the arc: it wires A-1 (identity), A-2 (rwx), A-3 (9P identity
presentation + per-user stratumd + dev9p enforcement) and A-4 (clearance / legate / CAP_KILL
/ trusted path) into the user-facing login flow. **No new kernel primitives** -- the
substrate is all built (`CAP_SET_IDENTITY` + `SYS_SPAWN_FULL_ARGV`'s `SPAWN_IDENTITY_SET`,
the SAK + the console-owner model, the legate + `CAP_KILL`, the devcap redeem gate, corvus's
`AUTH`/`UNWRAP`/`SESSION_CLOSE`/`CLEARANCE_*` verbs, the A-3c per-user `--role client`
stratumd, the A-4c-1 `/dev/cons` blocking read). Three user votes 2026-06-02, taken after a
Plan 9 prior-art pass (factotum/secstore/authsrv as the key agent that *delegates* secrets;
the per-Proc namespace AS the session, built at login + evaporating with the session's procs;
the session-leader-reaps-its-group idiom) + a capability-microkernel SOTA pass (Fuchsia
account-handler + zxcrypt per-account volume; Genode vault-session capability; seL4 CapDL
CSpace-subtree-as-scope -- all converge on "a dedicated credential component owns the
key-unwrap and hands the FS a *capability* to already-unlocked storage; the session process
never holds the raw key") + a Stratum per-user-encryption-readiness verification (background
agent):
- **(1) Full encrypted home** -- build per-user AEAD-encrypted `/home` now (the
  chunk-completeness default), not defer.
- **(2) stratumd-asks-corvus for the DEK** -- the storage layer delegates the unwrap to
  corvus; login never holds the raw DEK (the factotum idiom; no secret transits argv/files).
- **(3) userspace session-leader** -- logout = login reaps its group via the A-4b
  `CAP_KILL`/`proc_group_terminate` path + unmount + corvus `SESSION_CLOSE`; NO new kernel
  session construct (the per-Proc territory + the kill path ARE the mechanism, Plan 9 idiom).
- **(4, refining)** after the Stratum verdict sharpened "Full": **at-rest + session-scoped
  encryption** (home unreadable on disk without the passphrase + DEK evicted at logout -- the
  exact scripture property + the Linux/macOS LUKS/FileVault norm), NOT the stronger
  *coordinator-blind* property (the system FS provably cannot read a logged-in user's data),
  which exceeds what the scripture asks and is recorded as a v1.x NOVEL (seams below).

The recurring fusion: **corvus = the factotum/key-agent** (holds the identity DB + the
keypair, owns every unwrap); **the per-Proc territory = the session** (built at login,
torn down with the session's procs); **the A-4c SAK** makes the login + elevation prompt
unspoofable; **the A-4b CAP_KILL + the #811 wake-total cascade** = the session-teardown
primitive; **the kernel cap-stamp (`CAP_SET_IDENTITY`)** = the unforgeable identity substrate.

**Calls made without a vote** (resolved by the decision-rules + ground truth, not escalated):
- **`/sbin/login` is NATIVE (libthyla-rs)** -- authored-within-Thylacine, no foreign POSIX
  code (ARCH §3.5 native/ported rule; the old phase5 "blocked on pouch" note predates
  libthyla-rs's `t_srv_connect`/`t_spawn_full_argv` + corvus-as-9P-server). The shell it
  execs is `ut` (the native Utopia shell, functional through U-6d-a), reading `/dev/cons`
  via the A-4c-1 console RX.
- **Single-console-serial at v1.0** -- one physical console + one SAK => one login at a time;
  corvus's singleton session (`SRV_CONN_PER_PROC_MAX = 1`) fits exactly. Concurrent
  multi-session corvus is a v1.x lift (seams); the exit criterion "a second user cannot read
  the first's home" is about *isolation*, which serial logins prove.

**A-5a -- login core** (native `/sbin/login`; SAK-gated prompt -> corvus AUTH -> identity
stamp -> shell; the session leader). Stratum-INDEPENDENT; lands first.
- *Flow*: joey (post-pivot) spawns `/sbin/login`. login opens `/dev/cons` (the A-4c-1
  blocking read), draws the prompt, reads `{user, passphrase}` -> corvus `AUTH` (verb 1;
  template joey.c's corvus client) -> on `STATUS_OK`, corvus holds the user's keypair in RAM
  + returns the 33-byte session token. login resolves `principal_id`/`primary_gid`/`supp_gids`
  (corvus `RESOLVE_NAME`, verb 12) and spawns `ut` via `SYS_SPAWN_FULL_ARGV` with
  `SPAWN_IDENTITY_SET` + that identity triple (login holds `CAP_SET_IDENTITY`, fork-granted
  kproc -> joey -> login) -> the shell is born AS the user, not `PRINCIPAL_SYSTEM`. login is
  the session leader and waits on the shell.
- *Console/trusted-path invariant (the A-4c-2 forward-note made concrete -- I-27 carry)*:
  during a user session, **corvus is the SOLE Proc that may hold
  `PROC_FLAG_CONSOLE_ATTACHED`**; login and the user shell are NEVER console-attached (so a
  foreground shell cannot redeem `CAP_HOSTOWNER`, and the SAK's "revoke the owner, re-grant
  corvus" leaves corvus the sole attached Proc). The boot console-attach anchor (joey) must
  **relinquish** its attach at the bringup->session boundary, else a post-SAK state is
  `{joey, corvus}` both-attached and I-27 breaks. As-built choice (A-5a impl): joey persists
  as the v1.0 session supervisor (it already reaps corvus/stratumd) + relinquishes
  console-attach via a small gated kernel op at the boundary, OR exits to a session reaper;
  the relinquish is a new touch on the A-4c audit surface. Both preserve the invariant.
- *Ctrl-C-to-shell*: an OPTIONAL `SPAWN_PERM_CONSOLE_OWNER` (mirroring
  `SPAWN_PERM_CONSOLE_TRUSTED`) sets `g_console_owner` to the spawned shell WITHOUT the attach
  bit, so interrupt delivery reaches the foreground shell. A-5a may include it (small) or
  defer it as a Utopia-lane polish seam -- not an exit criterion (the SAK protects elevation
  regardless of who is owner).
- *Logout / teardown (userspace session-leader)*: the shell exits -> login (1) reaps it;
  (2) group-terminates any session descendants via the A-4b kill path (the #811 wake-total
  cascade); (3) tears down the per-user mounts (A-5b); (4) corvus `SESSION_CLOSE` (verb 3)
  zeroes the keypair (+ A-5b evicts the DEK); (5) loops to the prompt (getty-style) or exits.

**A-5a IMPL STATUS (LANDED).** The user chose the "live session now" boot shape
(2026-06-02): joey becomes the persistent init. A-5a-alpha (`97a3af5`) added the
kernel substrate -- three syscalls `SYS_BOOT_COMPLETE` (62; one-shot +
console-attached-gated banner via `boot_mark_complete`), `SYS_CONSOLE_RELINQUISH`
(63; self-drop of the console-attach, I-27), `SYS_CONSOLE_OPEN` (64; a R|W
`/dev/cons` KOBJ_SPOOR handle) -- + the `boot_mark_complete` banner move (the
`Thylacine boot OK` string is unchanged; it now fires on init's signal, not
joey's reap; TOOLING.md section 10 + CLAUDE.md updated) + 3 kernel unit tests
(686/686). A-5a-beta added the native `/sbin/login` (`usr/login`, reads creds
from fd 0 -> corvus AUTH -> RESOLVE_NAME/ID -> spawns `ut` stamped via the new
`Command::identity()`), joey's `do_login_e2e` seeded CI proof (michael ->
uid=1000 gid=1000 -> ut spawned + reaped, gated on login's exit code) + the
`session_getty_loop` (open `/dev/cons` -> spawn login -> wait -> loop). The boot
log now reaches a live `Thylacine login:` prompt. As-built reference:
`docs/reference/103-login.md`. The interactive `/dev/cons` read is proven by the
seeded mechanism + the prompt + an interactive run (the A-4c harness-cannot-
inject precedent). `SPAWN_PERM_CONSOLE_OWNER` was deferred (under I-27 the
non-attached login cannot pass the console-attached-gated perm; the SAK protects
elevation regardless). **A-5a audit R1 CLEAN** (Opus prosecutor + self-audit
converged; 0 P0 / 0 P1 / 2 P2 / 3 P3, NOT dirty): F1 (a post-banner extinction
was masked by test.sh now that joey runs past the banner -> test.sh now scans
for EXTINCTION with precedence + a post-banner grace window) + F2
(`SYS_CONSOLE_OPEN` was ungated -> now gated on console-attach; joey opens
`/dev/cons` before relinquishing + reuses the one handle) + F4 (comment) FIXED;
F3 (E2E doesn't assert ut's stamped principal_id -- needs a self-id syscall) +
F5 (getty backoff -- needs a sleep primitive) DEFERRED-justified. Matrix:
default + UBSan + smp8 GREEN (smp8 wants BOOT_TIMEOUT=1200 on M2 -- A-5a adds a
2nd Argon2id before the banner). NEXT: A-5b.

**A-5b -- per-user encrypted home** (Flavor 1: at-rest + session-scoped). Depends on the
Stratum-side sub-chunk below.
- *Property*: each user's home dataset is AEAD-encrypted at rest, its DEK a fresh CSPRNG-random
  32 bytes sealed under the user's hybrid keypair (corvus WRAP) -- "unreadable on disk without
  the passphrase." The DEK is in cleartext only during the user's session; evicted at logout.
  The system coordinator (a trusted process) handles plaintext only while the user is logged
  in (the mainstream-OS norm), NOT the coordinator-blind property (v1.x NOVEL).
- *Stratum-side* (IN-SCOPE per the Stratum-coordination grant; verified to need NO on-disk
  format or wire-ABI break; lands on `thylacine-pouch-arm` with its own audit): (1)
  **deferred-unwrap soft-skip** -- `stm_sync_open`'s CURRENT-keyslot hard-fail
  (`src/sync/sync.c:561/652`) becomes a *scoped* soft-skip for datasets outside the mounting
  identity's scope, so the system coordinator boots with user-sealed datasets
  present-but-unreadable; (2) **runtime DEK install/evict** -- a consumer that, given a user's
  session token, calls the shipped `stm_corvus_unwrap` (`include/stratum/corvus_client.h:502`)
  to fetch the user's DEK and install it into the live `deks` map, and the inverse (evict +
  zero) at logout (the missing consumer side -- the resolver `sync_resolve_current_dek_locked`
  already exists); (3) a **token-forward** path so the coordinator reaches corvus with the
  user's token at login.
- *Realizing the "stratumd-asks-corvus" vote* (verdict Q3: the `--role client` proxy does NO
  crypto -- the coordinator does -- so the DEK consumer is the coordinator).
- **Open A-5b impl-design item -- RESOLVED 2026-06-02 (ground-truth pass; no vote -- the fork
  dissolved).** The framed worry ("login holding the session connection blocks a second
  (coordinator) connection") was a misread of the connection model. Verified across both trees:
  - `SRV_CONN_PER_PROC_MAX = 1` is **per-Proc**, not per-corvus-total (`kernel/devsrv.c:360,372`;
    CORVUS-DESIGN §6.2 / line 643). Two distinct Procs each hold their own `/srv/corvus`
    connection -- login's connection does NOT block the coordinator's.
  - corvus's session is **token-keyed, not connection-keyed**: `session_token_matches` is a
    constant-time compare against the global `SESSION` slot with no peer/stripes check
    (`usr/corvus/src/main.rs:1586`). The C-8 "no Proc impersonation" gate stamps the AUTH
    channel's Proc identity; UNWRAP's 33-byte bearer token is the *deliberate* cross-Proc
    credential, and UNWRAP is NOT console-attached-gated (only AUTH / elevation are).
  - corvus's `VERB_UNWRAP` (verb 4) **already implements the bearer-token-forward pull**
    (`token(33) + dataset + key_id(8) + wrapped(1217) -> 32-byte DEK`, C-7 owner-gated), and
    CORVUS-DESIGN §6.3 ("Session reach across Procs", line 678) already blesses exactly this:
    login forwards token S to the per-user stratumd, which opens its own connection and
    presents S. Stratum's shipped `stm_corvus_unwrap` (`include/stratum/corvus_client.h:502`)
    already drives that path at mount time (`stm_sync_open` / `sync_unwrap_cb`), and
    `sync_dek_insert` (install) already exists.
  - **The call (CORRECTED 2026-06-02 -- deeper ground truth + a user vote): the coordinator
    PULLS** the DEK over its OWN corvus connection with the login-forwarded token (the §6.3
    bearer-token-forward). corvus PUSH stays rejected (it inverts corvus's server/key-agent
    role + corvus does not hold the storage layout). BUT the earlier same-day "no corvus
    connection-model lift / all Stratum-side" conclusion was INCOMPLETE: no pouch stratumd has
    ever reached corvus, and realizing the pull requires TWO enabling changes (neither alters
    the fixed security property):
    1. **pouch->corvus transport enablement.** corvus serves its verbs on the `ctl` sub-fid
       (reached via `Twalk("ctl")`); login's native `t_srv_connect(b"corvus", b"ctl")` walks to
       it, but the pouch `connect()` passes `path_len = 0` and `sun_path_to_name` rejects any
       `/`, so a pouch client never binds the ctl fid. Fix: teach the pouch sockets connect to
       parse `/srv/<name>/<walk>` and forward the walk component (the kernel's 2-arg
       `SYS_srv_connect` already drives `Tversion+Tattach+Twalk+Tlopen` -- login proves it). No
       kernel / corvus / Stratum-client change; the raw-byte `stm_corvus_unwrap` + the wire
       codec + the 1217-byte envelope are all verified compatible and unchanged. (`0006-pouch-
       sockets.patch`, audit-bearing boundary-line.)
    2. **corvus session-ownership lift (user-voted Option B, 2026-06-02).** corvus's impl clears
       its single global AUTH SESSION on ANY connection close (`close_conn` -> `session_clear`,
       `usr/corvus/src/main.rs:3352`), a v1.0 single-client shortcut. A transient coordinator
       connection's close would wipe login's session mid-session -- breaking A-4 legate
       elevation (which presents the same token). The lift: stamp the session-OWNING connection
       at AUTH; clear the SESSION only on THAT connection's close or an explicit SESSION_CLOSE;
       a non-owning bearer-token connection's close no longer clears it. This realizes the
       §4.2/§6.2 "token bound to the creating Proc; closing it auto-closes the session" intent
       precisely (the single-global-slot impl approximated it under the single-client
       assumption) and is the first step of the v1.x multi-session direction. Audit-bearing
       (corvus session model); CORVUS-DESIGN §6.2 updated this commit.
  - **The genuinely-new plumbing** (verified NO on-disk-format / wire-ABI break -- the keyslot
    envelope is opaque to the on-disk layout, the deks map is in-RAM, UNWRAP/WRAP are
    wire-v1-stable): the two enabling changes above, PLUS all Stratum-side: (1) the
    **deferred-unwrap soft-skip** -- the coordinator boots with user-sealed CURRENT keyslots
    present-but-locked when no session token is available (the predicate is half-present at
    `src/sync/sync.c:555-561`; keep R42's hard-fail at `628/652` for an *attempted* unwrap that
    fails -- that is tamper, not deferral); (2) a runtime DEK **install** (`stm_corvus_unwrap`
    with the forwarded token, then `sync_dek_insert`) + **evict** (`sync_dek_remove` + zero --
    today only the unmount-wide `sync_dek_wipe_all` exists) on the long-lived coordinator; (3) a
    coordinator **runtime control surface** login drives -- realized as THREE writable per-dataset
    `/ctl` kinds (the `mark-snapshot-compromised` handler is the action+commit+revert template),
    reached over a 2nd 9P attach to the coordinator's `--ctl-listen` socket; each authorized by the
    token (bearer cred + corvus's C-7 owner gate) + the SYSTEM peer as defense-in-depth (NOT the
    ctl-admin gate -- login is `PRINCIPAL_SYSTEM`, not uid 0): **`provision-dek`** (idempotent
    ensure-home -- `stm_fs_create_dataset_corvus` mints the dataset + a fresh DEK + the corvus WRAP
    into the CURRENT keyslot, folding `STM_EEXIST -> OK` so a returning user is a no-op) at login,
    then **`install-dek`** (UNWRAP the keyslot into the live `deks` map) per login, then
    **`evict-dek`** (zero + remove) at logout (`install-dek`/`evict-dek` landed #826b-2 as kinds
    32/33; `provision-dek` is the new kind 34, separate-verb shape user-voted 2026-06-04); (4) the
    one-shot `stratumd --provision-corvus-dataset` is the build/fixture + standalone provisioner
    ONLY -- NOT the runtime path (the F3 correction below: a 2nd stratumd opening the live pool
    would corrupt it). login never holds the raw DEK -- it forwards only the opaque token; the
    coordinator unwraps + installs.
  - The *security property* (at-rest + session-scoped + login-never-holds-the-raw-DEK +
    evict-at-logout) is fixed; all the above is the plumbing within it.
- *Thylacine-side*: login spawns the user's `--role client` stratumd (A-3c, dataset-scoped via
  `--datasets-allowed` -> the user-vs-user access boundary, `Rlerror(EACCES)` out-of-scope),
  drives the coordinator **`provision-dek` (ensure-home; idempotent) then `install-dek`** over its
  persistent `/ctl` connection with the user's token, and binds `/home/<user>` into the user's
  territory (`SYS_ATTACH_9P_SRV` + `SYS_MOUNT`/bind -- the joey 16c template). Logout: unmount +
  coordinator `evict-dek` + corvus `SESSION_CLOSE`.
- **A-5b #827b -- the per-user proxy's service-post authority (RESOLVED 2026-06-04, user-voted).**
  The *Thylacine-side* line above has login spawn a `--role client` proxy that posts
  `/srv/home-<user>` (login then `SYS_ATTACH_9P_SRV` + binds it). But posting a `/srv` service
  requires `PROC_FLAG_MAY_POST_SERVICE`, and the `SYS_SPAWN_*` grant gate (`kernel/syscall.c`)
  requires the *granter* be console-attached -- which login deliberately is NOT (I-27); joey spawns
  login with `perm_flags = 0` today. So the design as written could not stand up the proxy, and the
  gate was the only friction: the session's `/srv` is ALREADY private (per-territory `/srv`, stalk-3
  / I-1), so a per-user proxy posting `/srv/home-<user>` in the session territory is isolated -- the
  Plan 9 "a server posts into its own (per-process) namespace" idiom, which has no console gate; the
  capability-microkernel SOTA (seL4/Genode/Fuchsia) instead has the parent mint the channel and hand
  the child an endpoint. **Decision: make `MAY_POST_SERVICE` delegable ONE explicit hop.** The grant
  gate becomes per-bit: a Proc that *already holds* `PROC_FLAG_MAY_POST_SERVICE` may confer it on a
  child (in addition to a console-attached granter); `SPAWN_PERM_CONSOLE_TRUSTED` (the SAK trust
  anchor) stays console-attached-ONLY. joey (console-attached) confers `MAY_POST_SERVICE` on login;
  login re-confers it on the per-user proxy. The bit is STILL never `rfork`-propagated (it is
  conferred only by an explicit `perm_flags` decision at each spawn, exactly as before) -- so the
  "kernel-stamped, not a cap" discipline (ARCH `spawn_with_perms`) holds; the delegation is rooted at
  the console-attached joey and is one hop wide. **I-27 unchanged** (login stays
  non-console-attached; it gains a service-post authority, NOT the console). **I-2 unaffected**
  (`MAY_POST_SERVICE` is a `proc_flags` bit, not a `cap_mask` cap; the fork-grantable cap set still
  only reduces). Alternatives considered + rejected for v1.0: (a) a *distinct* delegated-grant bit
  (`SPAWN_PERM_GRANT_POST_SERVICE`, so login may grant without itself being a poster -- cleaner
  separation but more surface; deferred); (b) a connected-`SrvConn`-pair primitive (login mints a
  pair, hands one endpoint to the proxy as an fd, NO `/srv` post at all -- the capability-pure /
  coordinator-blind direction, the SAME family as the v1.x NOVEL seam below; most new surface,
  deferred to v1.x). **Audit-bearing** (the privilege-gate change -- prosecute the per-bit separation
  + that the one-hop delegation cannot widen into rfork-propagation; CLAUDE.md + ARCH §25.4 row).
- **A-5b #827b-beta -- the per-user child-dataset attach (`ds:<name>` aname) + the init
  `MAY_POST_SERVICE` grant-root (RESOLVED 2026-06-04, user-voted).** The *Thylacine-side* line
  ("binds `/home/<user>` ... `SYS_ATTACH_9P_SRV`") left the attach **aname** unspecified, and the
  #827b-beta impl surfaced that it is load-bearing: a per-user home is a **separate Stratum child
  dataset** (its own DEK -- per-user encryption REQUIRES a separate dataset, since Stratum encrypts
  per-dataset), but Stratum's 9P `Tattach` bound a connection ONLY to the coordinator's fixed
  `root_dataset` (the aname kinds were `DEFAULT` / `ABS_PATH "/..."` / `SPEC "spec:..."`; none select a
  *different* dataset). `t_attach_9p_srv(aname="users/michael")` -> `Rlerror(EINVAL)`. Homework: Plan 9
  treats **aname as the tree-to-attach** (a file server serving many trees picks one by aname); ZFS
  auto-mounts a child dataset at its path in the parent (a graft); Stratum had neither. **Decision
  (user-voted, of 3 options): add a Stratum 9P aname form `ds:<name>`** that binds the connection's
  root dataset to the named child dataset (resolved by name under the coordinator's root). Plan-9
  idiomatic, ADDITIVE (a new aname kind; no wire break -- old anames unchanged). Rejected: the ZFS-style
  graft-into-`/home` (larger Stratum primitive: a dirent->child-dataset-root link + a cross-dataset
  Twalk; deferred to v1.x if a unified browseable `/home` is wanted) and deferring per-user encryption
  (home as a plain subdir of the root dataset -- abandons A-5b's core at-rest-per-user property; rejected).
  **Access control (3 orthogonal gates, defense-in-depth):** (a) the per-user proxy's
  `--datasets-allowed ds:<user>` admits ONLY the user's own aname (set by the trusted, console-rooted
  login chain -- the I-1 user-vs-user boundary; a foreign `ds:<other>` -> `Rlerror(EACCES)` at the proxy
  before the coordinator sees it); (b) the child dataset root is born `0700` user-owned, so the kernel's
  A-3 dev9p rwx denies a wrong-principal connection AFTER attach; (c) the dataset's DEK must be installed
  (a locked / un-provisioned dataset's root-inode read fails) -- so an attach without a live unlock is
  inert. login attaches `ds:<user>` (the dataset NAME = the username, as minted by `provision-dek`); the
  proxy is spawned `--datasets-allowed ds:<user>`. The kernel `SYS_ATTACH_9P_SRV` aname is opaque
  passthrough (bounded by `SYS_ATTACH_ANAME_MAX`); the `ds:` semantics live entirely in the Stratum 9P
  server. **The init `MAY_POST_SERVICE` grant-root:** joey relinquishes its console-attach at the
  bringup->session boundary (I-27), but the getty then spawns a FRESH `/sbin/login` per session that must
  each receive `MAY_POST_SERVICE` -- and a non-console-attached granter can confer it only if it *holds*
  it (the one-hop gate). So **joey (init) is stamped `MAY_POST_SERVICE` in `joey_thunk`** (alongside its
  console-attach + console-owner stamps) -- init is the persistent service-posting grant-root, exactly as
  it is the console-trust + capability root. The bit is a `proc_flags` perm (never `rfork`-propagated,
  never a `cap_mask` cap) -> I-2 untouched; it grants nothing to children automatically (each spawn
  decides per `perm_flags`); `CONSOLE_TRUSTED` is still never conferred to login -> I-27 untouched.
  **Audit-bearing** (the `ds:` resolver is a new attach surface -- prosecute the name->dataset resolution
  [no id confusion / no cross-parent escape], the 3 access gates compose [no gate alone is load-bearing
  for cross-user isolation -- the proxy allow-list is], and that an un-DEK'd dataset attach is inert).
- *Provisioning (F3 correction, user-voted 2026-06-02; SUPERSEDES the earlier host-bake-homes
  line).* Home datasets are **NOT host-baked**: corvus's `handle_wrap` has no `CAP_HOSTOWNER`
  admin path (it takes a live conn), so a DEK cannot be sealed to a user without that user's
  **live session**, which does not exist at build time. Therefore **login-driven first-login
  provisioning is the SOLE path** -- at first login (session live, token in hand) login drives the
  coordinator's idempotent `provision-dek` verb, which mints + WRAPs + seals the home to the
  just-authenticated user. The shipped `stratumd --provision-corvus-dataset` one-shot
  (`src/cmd/stratumd/serve.c`) stays the build/fixture + standalone provisioner (e.g. the corvus
  dataset itself); it is **not** used at runtime against the live pool -- the pool is opened
  `O_RDWR` with no `O_EXCL`/flock (`src/block/posix.c:579`), so a 2nd concurrent stratumd writer
  would corrupt it (the AEGIS/mallocng-adjacent path). The running coordinator is the single
  serialized writer (`fs->global`), so runtime provisioning MUST route through its `/ctl` surface.

**A-5c -- RECOVER paper phrase + hostowner-c** (DESIGN RESOLVED 2026-06-05; two user votes
after a Plan 9 / capability-microkernel / per-user-encryption prior-art pass + a ground-truth
map of corvus's existing key chain). The account-recovery round-trip: a paper recovery phrase
that re-derives the user's keypair when the passphrase is lost -- plus P5-hostowner-c (the
already-sketched system-passphrase recovery, CORVUS-DESIGN section 5.6 / verb 8 / the
`system-recovery-wrap` in the section 8 layout / C-20). A-5c **formalizes + unifies** what was
latently designed; it is corvus + login-UX only -- **no kernel surface, no Stratum surface**
(see "Why no Stratum/kernel surface" below).

*The mechanism -- a recovery keyslot.* Today corvus wraps each subject's hybrid keypair
(X25519 + ML-KEM-768, 3648 B) once, under a passphrase-derived KEK (`hybrid.corvus` for a
user; `system-wrap` for the admin). A-5c adds a **second wrap of the same keypair** under a
*recovery-phrase*-derived KEK -- a sibling file (`recovery.corvus` for a user; the existing
`system-recovery-wrap` for the admin). This is the LUKS keyslot model: one volume key (here,
the keypair), N independent factors that each unwrap it. Recovering the keypair transitively
recovers everything, because every dataset DEK is a hybrid-PKE envelope encapsulated to that
keypair's *public* keys -- one recovery wrap restores access to an unbounded set of encrypted
datasets, no per-DEK escrow. (Novel angle vs FileVault's per-volume escrow; recorded as a
NOVEL.md candidate: "keyslot recovery on a fan-out hybrid-PQ keypair.")

*Why no Stratum/kernel surface.* Recovery re-wraps the keypair under a NEW passphrase KEK, but
the keypair *value* is unchanged (same X25519/ML-KEM keys). The DEK envelopes stored in Stratum
were encapsulated to those public keys, so they remain valid -- recovery restores private-key
access to the *existing* envelopes. Post-recovery, the A-5b login DEK-pull (AUTH with the new
passphrase -> unwrap keypair -> UNWRAP envelope -> mount) works unchanged. So A-5c touches no
on-disk Stratum format and no syscall.

*Prior art (the homework).* **Heritage** -- Plan 9 factotum is an in-memory agent with no
paper recovery; secstore, on a forgotten password, lets the admin reset the *account* but the
stored secrets are *lost* -- **no escrow of user data**; the Plan 9 idiom deliberately does not
let the admin decrypt your secrets. **SOTA** -- the per-user-encryption peers corvus already
cites: **LUKS** (one volume key, up to 8 keyslots -- the load-bearing analogy), **FileVault**
(a *personal* recovery key, user-held, plus an *optional* institutional/escrow key offered as
an explicit deployment choice, never a default), **ZFS/fscrypt** (keep-your-key, no recovery).
**Fit** -- purely additive to corvus: same primitives (Argon2id + AEGIS-256), same atomic
rename-swap persistence (A-1.6), same mlock/wipe discipline; CORVUS-DESIGN section D5 already
commits the "24-word BIP-39, Argon2id (recovery preset -- heap-bounded; section 5.6),
displayed-once, re-wraps the keypair" pattern for the *system* phrase -- A-5c generalizes that
exact pattern to users and wires the verb.

*The two votes (2026-06-05).*
- **Trust model = user-held only** (NOT hostowner escrow). The recovery phrase is the sole
  non-passphrase way into a user's data; the hostowner has NO verb that recovers/decrypts a
  user's home. This preserves D3's flaunted "cryptographically mutually-encrypted homes"
  property *against a malicious hostowner* and matches the Plan 9 no-escrow heritage. The
  FileVault institutional-escrow option (option C) is recorded as a v1.x NOVEL seam (a managed-
  device story), gated behind an explicit install flag if ever built.
- **Enrollment = mandatory at account create.** USER_CREATE mints the recovery wrap and returns
  the 24-word phrase in its OK response; every account is recoverable by default (the phrase is
  the disaster-recovery instrument, mirroring the system-phrase model). The opt-in alternative
  (default = no recovery, "data gone by design" purity) is rejected as the v1.0 default; a user
  who wants that purity simply destroys their phrase.

*The unified verb.* The existing reserved verb 8 RECOVER (CORVUS-DESIGN section 6.4) gains a
`subject_kind` discriminator: `subject_kind=0` = system (hostowner-c -- the section 5.6 flow,
resets the system passphrase), `subject_kind=1` = user (the new path, resets a named user's
passphrase). One verb, one crypto shape, two subjects.

*Gates.* RECOVER(user) requires **only phrase-knowledge + the AUTH rate-limit** -- NO session
token (the user has LOST their passphrase, so they cannot AUTH) and NO `CAP_HOSTOWNER` (a
user-held recovery that needed the admin would not be user-held). The 256-bit phrase IS the
bearer credential; the rate-limit (C-16) defends the online-guess channel. RECOVER(system)
additionally requires **console attachment** (corvus checks the peer console bit, like
ADMIN_ELEVATE) -- system recovery resets *admin authority*, so it stays a physical-console
trusted-path operation; a remote bearer of the system phrase cannot reset the system passphrase.

*Honest scope (the provisioning-time window).* USER_CREATE is admin-driven (CAP_HOSTOWNER), so
the operator who provisions a user transiently sees that user's initial recovery phrase -- the
same trust window as the admin-set initial passphrase. This is NOT a standing escrow (no verb
or stored wrap lets the admin recover later), and it closes the moment the user rotates: a
successful RECOVER rolls a FRESH phrase shown only to the user at the console, and a user may
proactively RECOVER to establish an operator-unknown phrase. A forced first-login passphrase +
phrase rotation (so the initial secrets are never operator-visible) is the near-term UX seam.

*Invariants.* A-5c adds NO new ARCH section 28 kernel invariant (it composes the existing set).
The new properties are corvus invariants -- CORVUS-DESIGN C-20 generalized + new C-27 (the
per-user recovery keyslot: minted at create, displayed once, never persisted, wraps the same
keypair as the passphrase slot) + C-28 (no-escrow: corvus stores no copy of any user's keypair
or DEK recoverable by any authority other than that user's own passphrase OR own recovery
phrase -- the faithful realization of the user-held vote).

*Split.* A-5c-a (the recovery keyslot crypto + RECOVER(user) + USER_CREATE mandatory enrollment
+ `recovery.corvus` + corvus unit tests) -> A-5c-b (hostowner-c: RECOVER(system) + the system
recovery wrap mint + the console gate) -> A-5c-c (login/UX recovery path + the boot E2E:
create -> capture phrase -> "lose" passphrase -> RECOVER -> re-login with the new passphrase ->
home decrypts) -> one focused audit. AEGIS/mallocng-adjacent (recovery crypto) -- prosecute
hard: phrase brute-force vs the rate-limit, the no-escrow property, mlock/wipe of phrase + KEK,
the no-session bypass, the console gate on system recovery, the rename-swap atomicity of the
twin wraps.

*A-5c-b provisioning -- host-bake the system identity (user-voted 2026-06-05).* The A-5c-b
system path needs a real system identity -- an admin hybrid keypair + `system-wrap` (the keypair
wrapped under the system passphrase) + `system-recovery-wrap` (the keypair wrapped under a system
recovery phrase). None exists today: `ADMIN_ELEVATE` byte-compares the passphrase against the
hardcoded `SYSTEM_PASSPHRASE = b"thylacine"` placeholder. The user chose **host-bake** (over a
first-boot console prompt and over keeping the placeholder): the system identity is minted at
BUILD time and baked into the pool's `/var/lib/corvus/` -- mirroring how `pool.img`, `system.key`,
and the boot corpus are already host-baked (CLAUDE.md "Thylacine mkfs RNG seed pinning";
build.sh `build_stratum_pool_fixture`). Host-bake is deterministic, reproducible, and adds no
interactive boot step; corvus-at-first-boot auto-mint (non-reproducible) and an interactive
prompt were rejected.

*Mechanism.* corvus's crypto crates are all pure-Rust (`argon2`, `aegis` pure-rust, `ml-kem`,
`x25519-dalek`, `sha2`) -- host-buildable. corvus is `no_std`/aarch64 and cannot run on the host,
so the system identity is minted by a small **host-target `corvus-mint` tool** that reuses the
corvus crypto verbatim through a **shared `no_std` `corvus-crypto` lib** (extracted from
`usr/corvus/src/main.rs`: the CRVS wrap layout + `argon2id_kek` + the AEGIS wrap/unwrap + the
BIP-39 codec + keypair generation, parameterized over an RNG -- corvus supplies a `t_getrandom`
RNG, `corvus-mint` supplies the host `OsRng`). One crypto source, two link targets: the host
minter and the on-device corvus produce/consume **byte-identical** wraps -- no drift (the
compile-time-invariant / no-second-implementation discipline). `build.sh` builds `corvus-mint`
for the host triple (the `build_stratum_host_tools` precedent), runs it to emit the three files,
then creates the `/var/lib/corvus` dir-chain in the pool top-down (stratum-fs `mkdir` is
single-level, no `mkdir -p`) and writes them SYSTEM-owned (`--bake-owner-uid` =
`PRINCIPAL_SYSTEM`); joey's runtime `mkdir_or_open` of the same chain then no-ops (idempotent).

*The build-time system passphrase stays the known `thylacine` constant at v1.0.* The WRAP is now
cryptographically real (`argon2id(passphrase, system_salt) -> KEK -> AEGIS-unwrap system-wrap ->
tag-verify`, the section 5.5 flow that was always the design), but the SECRET is the source-pinned
constant -- so joey's `ADMIN_ELEVATE` boot E2E (which sends `thylacine`) stays green and the build
is reproducible. This is strictly NOT a regression (the same known secret, now wrapped instead of
byte-compared) and is consistent with every other host-baked secret. The v1.x installer supplies a
real per-install system passphrase + displays the system recovery phrase once; only the
source-pinned constant changes. The host minter emits the system recovery phrase to the build log
(forensic, like the mkfs seed) at v1.0. `ADMIN_ELEVATE`'s real-unwrap is a **privilege surface --
audit-bearing** (A-5c-c's focused round).

*Seams (foreseeable, additive).* hostowner co-sign of a high-stakes recovery
(`AUTH_REQ_HOSTOWNER_COSIGN`, composing with the A-4c SAK trusted path); forced first-login
enrollment (operator-never-sees-the-phrase); Shamir k-of-n multi-hostowner recovery (D5 already
flags it); optional institutional escrow (the rejected option C); a single-user / recovery
*boot* mode (a bigger boot/Utopia item, not the corvus verb).

The concrete corvus surface (the verb 8 payload, the `recovery.corvus` format, the USER_CREATE
response change, the section 5.6 generalized flow) is pinned in CORVUS-DESIGN sections 5.6 / 6.4
/ 8 / 9.

**Invariants.** A-5 adds NO new ARCH §28 kernel invariant -- it COMPOSES the existing set;
the new properties are session-scoped, recorded here + audit-bearing:
- **I-1** -- each session's `/home` is bound in the user's own territory; another user's
  session cannot walk to it.
- **I-22** -- the logged-in user is a `principal_id`, never a superuser; rwx (A-2d/A-3) +
  dataset-scope (A-3c) are the only access gates; `CAP_HOSTOWNER` stays a console-gated
  capability, never an identity.
- **I-27** -- preserved + sharpened: corvus is the sole console-attached Proc during a session.
- **A-5 DEK session-lifetime property** (a corvus + Stratum property, not a kernel invariant):
  a user's DEK exists in cleartext only between that user's `AUTH` and `SESSION_CLOSE`; logout
  evicts + zeroes it; disk theft without the passphrase yields only ciphertext.

**Split.** A-5a (login core) -> A-5b (encrypted home; includes the Stratum-side sub-chunk) ->
A-5c (RECOVER + hostowner-c). Each audit-bearing; A-5a is Stratum-independent and lands first.

**Audit-bearing** (the CLAUDE.md + ARCH §25.4 rows land in this scripture commit). Prosecute:
I-27 (login/shell never console-attached; the joey relinquish preserves the sole-attached
invariant; no interposer between the SAK and the corvus prompt); the identity stamp
(`CAP_SET_IDENTITY` gate; no principal forge; login stamps only what corvus authenticated);
the DEK handoff (login never holds the raw DEK; the token-forward leaks no secret via
argv/files; the coordinator install/evict has no UAF/leak; eviction actually zeroes -- the
AEGIS/mallocng-adjacent class, prosecute hard); user-vs-user isolation (susan's session cannot
unwrap or attach michael's dataset); the session teardown (no orphaned session Proc; the kill
cascade is total per #811); the Stratum-side deferred-unwrap (a soft-skipped dataset is
provably unreadable until its DEK is installed; the install validates the token).

**Seams** (foreseeable, additive): concurrent multi-session corvus (v1.x -- multiple VTs /
network logins); **the coordinator-blind encryption property** (v1.x NOVEL -- "the FS
provably cannot read a logged-in user's data," via per-user devices or a crypto-split-proxy;
the Stratum verdict's escalation-worthy path); a dedicated "console-revoked"/"hangup" note
name (the SAK reuses `interrupt`); job control / Ctrl-Z (Utopia lane); the trust-stamp gate
for foreign/remote 9P (A-3 M5 seam).

**No new spec** per the 2026-05-23 broadening -- prose validation in this section + the
per-sub-chunk audits + the runtime + cross-reboot tests + the boot-path login E2E.
