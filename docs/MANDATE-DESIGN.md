# Mandate -- persistent attenuated delegation (the *mandatum*, the censor, the album)

**Status: ACCEPTED design (2026-06-18). Binding scripture.** The delegation tier
of the legate/imperium authority system: a *censor* persists a bounded, standing
grant of a subset of its imperium to another user. Builds on the canonical
self-elevation tier (`docs/IMPERIUM-DESIGN.md`), the trusted path
(`docs/TRUSTED-PATH.md`), and the legate substrate (`docs/reference/102-legate.md`
+ `IDENTITY-DESIGN.md` section 3.1 / 9.8). Implementation deviations update this
doc first (with user signoff) or are reverted. **No code yet** -- the build is
the post-net Imperium/Authority arc (section 12). The four design calls were
user-confirmed 2026-06-18 (sections 4, 6, 5, 7).

---

## 1. Thesis: the missing third leg

The legate substrate gives "elevate for a task, then de-escalate." Imperium
(`IMPERIUM-DESIGN.md`) makes that a power-user tier (a fork-propagating scope for
your own shell). Both are **self-elevation** -- you SAK, re-auth, and *get*
authority for an episode. Neither gives a user **standing, delegated** authority,
so every privileged act needs a SAK. That is impractical, especially for routine
network. The **mandate** is the missing leg: a trusted authority (a censor, or the
system at install) **pre-authorizes** a user for a bounded **standing** scope.

This is Roman public law, and the vocabulary is exact, not decoration: a
**mandatum** is a commission delegating authority within bounds the mandator
sets, and the mandatary can never exceed it (= I-2 attenuation); an **edictum** is
a magistrate's *published* edict, **annual** -- it lapsed when his term turned
over (= "invalid when the capability key changes" + `valid_until`); a **censor**
kept the census, enrolled and assigned citizens, and struck them (*nota
censoria*) -- literally user-management. The censor held *potestas* (censoria
potestas), **not imperium**, which is exactly right: user-management is its own
authority axis, distinct from the resource-command caps.

---

## 2. The unifying model: three conferrals, one substrate

`authority = capability ∩ namespace ∩ time`, anchored on the **durable identity**
(the user; never changes -- it carries attribution + ownership-on-create).
Authority reaches a Proc three ways:

| Conferral | Lifetime | Gate | Mechanism |
|---|---|---|---|
| **Birthright** (the floor) | standing | none (yours at login) | *a mandate* |
| **Self-elevation** (imperium) | episode (abdicate/term/exit) | SAK + per-cap key | fork-propagating legate scope |
| **Delegation** (the mandate) | standing until revoked/expired | issued by a censor in a SAK episode | persistent attenuated grant |

**The floor IS a mandate.** There is no separate birthright mechanism: a user's
standing authority is exactly *the union of the valid mandates they hold* -- some
issued at user-create (the floor), some added later by censors. **Imperium is
orthogonal** -- a JIT episode that *adds* authority on top of the standing
mandates. So the system is two conferral mechanisms (mandate = standing/delegated;
imperium = ephemeral/self-elevated) on one substrate.

---

## 3. The *mandatum* record

```
mandatum := {
  id            // corvus-assigned, unique
  subject       // principal_id it grants TO (the user)
  issuer        // principal_id that issued it (audit)
  issuer_level  // the issuer's clearance level (the revoke-rule comparison, section 7)
  domain        // network | fs | kill | ...   (the censor-scoping axis)
  scope         // the scope-spec: a namespace part (section 5) + (v1.x) a cap part (section 4)
  key_gen       // the generation of the per-domain clearance key it was issued under (section 9)
  term          // valid_until (absolute) | perpetual-until-revoked
}
```

Stored in corvus's **album** (`/var/lib/corvus/album/<subject>` -- the praetor's
*album* where edicts were posted), corvus-owned, Stratum-backed (integrity from
Stratum's Merkle + corvus's chroot). corvus is the sole writer. **No
cryptographic signature** (section 9): the gen-counter + corvus-owns-the-store +
the kernel-stamped unforgeable `stripes` give unforgeability for the local case;
real signing is the reserved distributed/CPU-server upgrade (IDENTITY-DESIGN
section 3.1's "cryptographic clearance proof reserved for the distributed case").

---

## 4. Two enforcement substrates -- namespace-first (CONFIRMED call 1)

- **Namespace-shaped** (network, FS views): the mandate grants a **narrowed
  namespace view**; login installs it, netd / the namespace enforces, **the
  kernel is untouched**. Realizes NET-DESIGN section 2 ("one netd, many narrowed
  views") + section 8 (the `/net` view IS the firewall).
- **Cap-resource-scoped** (kill-own-group, dac-this-subtree): a *scoped* cap.
  This requires the kernel to represent scoped caps in a **new per-Proc
  scoped-cap table, distinct from the `cap_mask` bits** -- because a full
  `CAP_KILL` bit is stripped at every fork (I-2), so a *scoped* kill that must
  propagate within a user's session has to be a different representation, checked
  at the resource gate, with its own propagation rule. That is a real kernel lift.

**v1 is namespace-only** -- the motivating case (granular network), and **zero
kernel lift** (existing mount/bind + netd's existing `srv_peer` principal
awareness). **Cap-resource-scope is v1.x** (the scoped-cap table). The record is
designed for both from day one; only the cap half waits. This makes the first
mandate build a corvus + netd + login + tool job.

---

## 5. The `/net` scope grammar + the two enforcement tiers (CONFIRMED call 3)

Grammar (small, auditable, Plan 9 ndb/dial-adjacent -- an allow-list,
default-deny). Each rule = `<direction> <proto> <remote-CIDR-or-*>!<port-range-or-*>`:

```
connect tcp *!80              # outbound TCP to any host, port 80
connect tcp *!443
connect udp *!53              # DNS
announce tcp *!8080           # may listen on local 8080
connect tcp 10.0.0.0/8!*      # any port to a CIDR
```

Two enforcement tiers:

- **Coarse** (free, pure namespace): *which `/net` subtrees* the user's territory
  mounts (tcp yes, udp no). Protocol-level. Existing mount/bind machinery.
- **Fine** (the port/CIDR level): **netd enforces per-principal** in the
  `connect`/`announce` verb handler -- it has the principal (`srv_peer`), checks
  the rule-set, rejects a non-matching dial. **This IS the NET-DESIGN section-8
  `/net/filter` seam**, built as part of the mandate arc (MA-3) -- the
  convergence point with the net arc. Keep the seam reserved + netd
  principal-aware through net-3..net-8 so this slots in without a netd rewrite.

(Rate/quota and host-by-name are v1.x grammar extensions; v1 is CIDR + port-range.)

---

## 6. The default floor (CONFIRMED call 2)

**The floor is a per-install default-mandate *template*, set at the founding (the
installer), overridable per-user by a censor within the censor's domain.** That
dissolves the secure-vs-usable tension -- no single floor for all installs. The
shipped default (a workstation citizen):

```
fs:   home(rw)  /bin(rx)  /tmp(rw)
net:  connect tcp *!>=1024 ; connect udp *!53     # outbound non-privileged + DNS
      (no inbound/announce; no privileged ports; no elevation caps)
```

A hardened-appliance install sets a tighter template (e.g. `net: (none)` ->
deny-all -- the section-8 container default). The floor is applied at `enroll`
(section 7) and is itself a mandate (issuer = the founding/system), so it obeys
the same lifecycle + the issuer-clearance revoke rule (a normal censor cannot
revoke the system-issued floor; only system authority can).

---

## 7. The censor (the *censoria potestas*) -- CONFIRMED call 4

The **censor = the existing user-admin clearance level**; its authority is
`user-management ∩ {the domains it may govern}` (a network-censor governs only
users' network mandates -- the management authority is itself `cap ∩ namespace ∩
time`-scoped, self-similar). Operations, all within the censor's **own held
subset** (I-2 attenuation -- never widen past what it holds, never touch a domain
it does not govern):

- **enroll** -- create a user + apply the floor template (optionally a
  per-user-tightened one).
- **grant / extend** -- add a mandate (or widen an existing one) to a user.
- **revoke** -- delete a mandate, **bounded by the issuer-clearance rule**.

**The issuer-clearance rule (CONFIRMED):** *revoke/extend requires
`actor.clearance >= mandate.issuer_level`, within the domain, never widening past
the actor's own subset.* You can undo what you-or-anyone-below-you did, **never
what is above you**; **system foundings are sacrosanct to a normal censor** (only
system authority unwinds them). Roman-faithful: a censor administered the rolls
but could not overturn status conferred by *lex*. The comparison is just
"issuer-clearance vs actor-clearance" in the existing cursus-honorum lattice.

Censor-creation is itself the top **manage-clearance** level (system-key-gated):
the founding mints clearance-admin, which mints censors. A **self-mandate** is
forbidden (self-elevation is the imperium/SAK path; the mandate path grants
*others* -- harmless by I-2 anyway, but cleaner).

---

## 8. Issuance (a SAK episode) + redemption (silent) -- the usability asymmetry

- **Issuance** happens *inside a censor imperium episode*: the censor SAK-elevates
  once (the *lex curiata*, IMPERIUM-DESIGN section 3) into the user-management
  clearance, then issues mandates; corvus checks I-2 + the issuer-clearance rule +
  writes the album. The highest-stakes grants (an axe-bearing cap, v1.x) get a
  per-grant trusted-path confirm; routine namespace grants within the session do
  not re-SAK.
- **Redemption** is *silent*: at the subject's next login, corvus filters their
  album to valid mandates (term not expired **and** `key_gen == current` per
  domain) and installs the union -- the namespace views (login mounts the `/net`
  view + FS binds) + (v1.x) the scoped caps -- and publishes the network scope to
  netd. **No SAK, no re-auth.** Sound because the kernel trusts only corvus, and
  the install is bounded by *what-was-issued ∩ the redeemer's verified `stripes`*.

The asymmetry is the whole point: deliberation happens once, at issue (a conscious
SAK act -- the 4th trusted-path consumer beside login/imperium/installer); the
citizen just *has* their bounded standing authority day-to-day.

---

## 9. Revocation -- three layers (the generation model is the middle one)

1. **Explicit, per-mandate** -- a censor deletes it (issuer-clearance-bounded);
   live effect via a corvus -> login/netd push + (for caps) the existing
   #809/#811 group-terminate.
2. **Mass, per-key-generation** -- an admin rotates a domain's clearance key
   (manage-clearance, system-key-gated) -> the domain's `key_gen` bumps ->
   corvus refuses every stale-gen mandate. *This is "invalid when the capability
   key changes."* Each authority **domain** (network, fs, kill) has a clearance
   key with a `u64` generation; a mandate embeds the `key_gen` it was issued
   under; redemption checks `mandate.key_gen == domain.key_gen`.
3. **Live teardown** -- caps: group-terminate the scope (v1.x); namespace:
   re-narrow the live view + drop the principal's offending connections at netd.

No cryptographic signatures (the gen-counter + corvus-owns-the-album + `stripes`
suffice locally; IDENTITY-DESIGN section 3.1). Real signing is the reserved
distributed-case upgrade (corvus's hybrid keypair signs an edict a remote node
that does not trust the local kernel can verify).

---

## 10. Surfaces (note how little kernel there is for v1)

- **Kernel**: **none** for mandate-namespace v1 (mount/bind + `srv_peer` exist).
  The imperium arc carries its own one kernel lift (the fork-propagating scope,
  already modeled in IMPERIUM-DESIGN section 9.1). Mandate-**cap**-scope is the
  v1.x kernel lift (the per-Proc scoped-cap table + its propagation rule, which
  composes/extends I-2).
- **corvus**: the album store; per-domain clearance keys + generations; the
  issue / redeem / revoke logic; the netd-policy publish.
- **netd**: per-principal fine `/net` enforcement in the connect/announce handlers
  (the section-8 seam).
- **userspace**: `censor` (the admin tool -- enroll/grant/extend/revoke/set-default),
  `edict` (read-only "what authority do I hold?"), the libthyla-rs corvus-client,
  login (the redemption consumer).

---

## 11. Invariant + spec obligation

**New invariant I-35 (mandate attenuation + revocation):** a mandate grants <= its
issuer's held authority (the I-2 analog for *persistent* delegation); is revoked
by deletion OR a key-generation bump (no stale-`key_gen` mandate is ever
installed); revoke/extend requires `actor.clearance >= mandate.issuer_level`
within the domain; the subject's durable identity is unchanged (a mandate adds
scoped authority, never a new principal); a network mandate composes I-1/I-28
(the `/net` view is the firewall).

**Spec-first: `specs/mandate.tla`** (re-enabling point (a) -- a new
invariant-bearing, SMP-race-bearing surface). The central hazard is the
**rotate-vs-install race** (corvus bumps a domain `key_gen` while a login is
mid-install of a now-stale mandate -- the ASID-rollover / death-wake class).
Model: no-stale-install + attenuation (no mandate grants more than its issuer) +
the revoke-rule. Clean cfg + buggy cfgs (`stale_install`, `over_attenuate`,
`revoke_above_clearance`). Written + TLC-green BEFORE the MA-1 impl.

---

## 12. The roadmap -- one arc, three phases, post-net

The **Imperium/Authority arc**, sharing the trusted-path foundation. Sequenced
AFTER net (user-set 2026-06-18): `net-3..net-8` -> **IM (serial)** -> **MA** ->
(v1.x: MA-cap, the graphical SAK, the stateful packet filter).

- **IM -- imperium (self-elevation).** The serial trusted-path *episode* (corvus
  cell-grid renderer + the per-cap DISTINCT_SECRET clearance keys + the *lex
  curiata* flow, ON the A-4c serial SAK mechanism that already exists) -> the
  fork-propagating legate scope (kernel, modeled) -> fasces + abdicate ->
  [pomerium + dictator, 2nd sub-chunk]. Builds the canonical `IMPERIUM-DESIGN.md`.
- **MA -- mandate (delegation, namespace axis).**
  - MA-1: corvus album + per-domain keys/generations + the issue/redeem/revoke
    logic + `specs/mandate.tla` (spec-first).
  - MA-2: the `censor` tool + the libthyla-rs corvus-client + login redemption
    (install the namespace views).
  - MA-3: the netd per-principal fine `/net` enforcement (the section-8
    `/net/filter` seam -- the port/CIDR granularity).
  - MA-4: the focused audit + the SMP gate + close.
- **MA-cap (v1.x).** The kernel scoped-cap table + cap-resource-scope mandates
  (kill-own-group, dac-this-subtree).

---

## 13. Roman vocabulary (load-bearing vs flavor)

**Load-bearing (added):** **mandatum** (the record), **censor** (the
user-management role), **album** (the store). Joining the existing **legate** /
**imperium** + the flavor-where-it-clarifies **provincia** / **abdicate** /
**term** / **cursus honorum** / **pomerium** / **fasces** / **lex curiata**. The
`edictum` framing (an annual, lapsing, published edict) is the conceptual model
for the mandate's term + generation-revocation; "edict" survives as the user-tool
name (`edict`, the read-only "what do I hold").

---

## 14. The novel angle (NOVEL.md candidate)

One OS unifying **JIT self-elevation (imperium) + persistent attenuated delegation
(mandate) + a medium-independent trusted path (SAK)** on a single `capability ∩
namespace ∩ time` substrate, **with network authority expressed as
namespace-shaped mandates rather than packet-filter rules.** Each peer holds only
a fragment: cloud PIM (AWS STS / Azure PIM) has elevation but no namespace axis;
macaroons/biscuit have attenuatable tokens but no kernel enforcement; seL4 has
capability mint/revoke but no identity/persistence; Fuchsia routes capabilities
but at build time, not runtime-delegatable; Plan 9 has namespace restriction but
no scoped-cap delegation. The fusion -- runtime, kernel-enforced, identity-bearing,
namespace-and-capability, self-elevation-and-delegation -- is new.

---

## 15. Dependencies / cross-references

- **`docs/IMPERIUM-DESIGN.md`** -- the self-elevation tier; the SAK *lex curiata*
  entry the mandate issuance reuses; the fork-propagating scope.
- **`docs/TRUSTED-PATH.md`** -- the SAK episode (mandate issuance is its 4th
  consumer); the serial mechanism (A-4c) the IM phase builds the episode on.
- **`IDENTITY-DESIGN.md` section 3.1** -- the legate model; the reserved v1.x
  resource-scoping (= the mandate's scoped caps) + "scope = cap ∩ namespace ∩
  time" + "grant/revoke via corvus per-user wrap chains" (= the album); "no local
  crypto, reserved for the distributed case" (= Fork A).
- **`docs/reference/102-legate.md`** -- the as-built legate/cap-device two-phase
  grant the mandate generalizes from ephemeral to persistent.
- **`NET-DESIGN.md` section 2 / 8** -- "one netd, many narrowed views" + the
  `/net` firewall + the `/net/filter` v1.x seam = the mandate's network
  enforcement.
- **`CORVUS-DESIGN.md`** -- the trust root; the `/cap` grant/use flow; the
  per-user wrap chains; the album lives in corvus's store.
- **ARCH section 28** -- I-35; section 25.2 -- `mandate.tla`; section 25.4 -- the
  mandate audit-trigger surface (corvus album + netd enforcement + the censor),
  enumerated at MA-1.

---

## 16. Open seams (v1.x, by design)

- **Cap-resource-scope mandates** (the kernel scoped-cap table) -- MA-cap.
- **Distributed / CPU-server crypto** (corvus signs an edict a remote node
  verifies) -- the reserved cryptographic-clearance case (IDENTITY-DESIGN 3.1).
- **The stateful packet filter** (per-connection allow/deny beyond the namespace
  view) -- the NET-DESIGN section-8 / section-18 `/net/filter` v1.x seam's richer
  form.
- **Rate / quota / host-by-name** in the `/net` grammar.
- **The graphical SAK** (TRUSTED-PATH section 6) -- issuance/elevation on a
  framebuffer, beyond the serial dev/recovery path.
