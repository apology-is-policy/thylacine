# Imperium -- power-user clearance, the legate's command authority

**Status: ACCEPTED design (2026-06-08); landed in the canonical docs 2026-06-09.
REVISIT scheduled at the END of the Life Support arc** (after `LS-test`) --
imperium is the post-LS **identity-arc + Utopia-shell capstone**, not LS-arc
work. Originated in the auxiliary track (`aux/userspace-apps`,
`usr/apps/IMPERIUM-DESIGN.md`); brought here as the canonical design.

When built, the design folds into the A-4 / legate scripture
(`docs/reference/102-legate.md`, `IDENTITY-DESIGN.md section 9.8`, invariant
I-25) + `CORVUS-DESIGN.md` (the clearance auth) + a `NOVEL.md` angle (the
"abdication atomically de-escalates the entire elevated subtree -> an elevated
shell that is *safer* than a root shell" property -- advances Angle #9's
capability-elevation contract). The kernel lift -- the fork-propagating legate
scope (section 2) -- is the main agent's to build + model (**spec-first
re-enabled for that privilege surface**, like the SMP redesign); it is the
natural completion of the member-bearing legate teardown that `102-legate.md`
already defers to v1.x (the spawn-during-teardown straggler + member-vs-reaper
notes, #855). Builds on the COMPLETE A-4a legate mechanism; the *lex curiata*
entry is the first real consumer of the A-4c trusted-path re-auth that
`102-legate.md` defers.

## 0. Naming note -- the Roman sub-theme is deliberate (user-endorsed)

Thylacine's primary naming theme is marsupial / extinction / Plan 9 (CLAUDE.md
"Thematic naming"). The **security / discipline / capability subsystem** is a
deliberate, user-endorsed exception: a Roman *imperium* sub-theme, anchored by
the kernel term **legate** (already in scripture). It earns its place -- Roman
public law is the canonical Western vocabulary for *delegated, bounded,
revocable command authority*, which is precisely what the legate / clearance
model is. (And, fittingly, the thylacine was alive and un-extinct two thousand
years ago, in Rome's day.) A future instance should therefore NOT
"marsupial-ize" imperium / provincia / fasces / pomerium / lex curiata etc. --
the Roman vocabulary stays. The load-bearing-vs-flavor split (section 8) still
governs which terms reach code vs which stay descriptive.

## 1. Thesis: one mechanism, two tiers

The legate clearance substrate (102-legate.md) gives us "elevate for a task,
then fully de-escalate." Today it is **root-only** -- the elevation-only caps
(`CAP_DAC_OVERRIDE | CAP_CHOWN | CAP_KILL`) stay on the Proc that redeemed; a
`rfork` child arrives unelevated (the `~CAP_ELEVATION_ONLY` strip + fresh
per-Proc `stripes`). That refusal-to-spray is the secure default and we keep it.

Imperium adds a deliberate, audited **second tier** for power users + developers,
flipping exactly one knob:

```
  default    (everyone)    : dumb utils + a `legate <verb>` gateway   -> NON-propagating legate (per-op, least-privilege)
  imperium   (power-user)  : `imperium --request <caps>` ... `abdicate` -> FORK-PROPAGATING legate (the shell subtree, your risk)
```

Same clearance mechanism; the only differences are **fork-propagation** (off /
on) and **scope granularity** (one util Proc / the shell subtree). Secure by
default; powerful on request; both bounded by the same clean teardown (I-25).

Why two tiers (not "utils self-elevate"): baking elevation into every coreutil
makes *every util* a privilege-bearing trusted surface. We keep the utils
**dumb** (no elevation logic) -- under imperium they inherit the caps and just
work; outside it they fail clean. Elevation lives in a *small number of audited
places* (the `legate` gateway + `imperium`), and is always **explicit** -- the
opposite of UAC-style reflexive "click yes."

## 2. The kernel lift: a fork-propagating legate scope

The one new mechanism. A legate scope gains a **propagating** mark (a scope-level
flag, NOT a per-cap attribute -- propagation is a property of the *episode*, not
the capability). Under a propagating scope:

- `rfork` does NOT strip the scope's elevated caps; the child inherits them + the
  `legate_scope_id` (a scope **member**, still not a `LEGATE_ROOT`).
- The existing I-25 teardown already group-terminates the whole `legate_scope_id`
  subtree, so abdication / `valid_until` / root-exit de-escalates **the entire
  propagated subtree atomically** -- every elevated command still running dies
  with the imperium. (sudo's backgrounded jobs survive a logout; a legate's
  cannot. This is the property that makes imperium *safer* than a root shell.)

Everything else (the grant/redeem, `proc_become_legate`, the scope teardown
triggers) is reused; propagation is the only addition. Non-propagating (default)
behavior is unchanged.

## 3. Entry -- the *lex curiata*

In Rome, imperium was not yours by winning the election; it was conferred by a
separate act (the *lex curiata de imperio*). Same shape: requesting imperium
grants nothing by itself -- it triggers the conferral.

```
michael@thylacine$ imperium --request chown dac kill
  <SAK toggles to the trusted path>
  ┌─ CONFERRING IMPERIUM ─ provincia ────────────────┐   (rendered by corvus,
  │  caps:  CAP_CHOWN   CAP_DAC_OVERRIDE   CAP_KILL    │    console-attached,
  │  axe:   YES (CAP_KILL -- power of life and death)  │    UNSPOOFABLE)
  │  term:  4h            pomerium: enforced           │
  │  authenticate against the capability keys:         │
  └────────────────────────────────────────────────────┘
```

- **The SAK (Secure Attention Key) is the deliberate, unspoofable moment.** It
  switches to the trusted path (corvus, the sole console-attached Proc, A-4c).
- **corvus displays the *provincia* -- the exact requested cap-set -- on the
  trusted path, BEFORE authentication.** This is the load-bearing security
  property: because the enumeration is on the unspoofable channel, no program can
  trick you into a wider imperium than you read. The "drama" is informed consent.
- **Authenticate against the capability keys.** Imperium is a `DISTINCT_SECRET`-
  tier clearance (per-level keys), not the bare session token -- the axe needs
  its own key. You can only raise an imperium you are (a) eligible for (the
  *cursus honorum*, corvus's `clearance.db`) and (b) can unlock. **Imperium is
  the first real consumer of the A-4c trusted-path re-auth** 102-legate.md defers.
- On success corvus registers a **propagating** clearance grant against the
  shell's stripes; the shell redeems it (`SYS_CAP_USE`) and becomes the
  fork-propagating legate root.

### 3.1 The `imperium` tool's workflow (a thin trigger; the TUI is corvus's)

The `imperium` tool (native `ut` / libutopia) is **untrusted and thin** -- it
posts an intent to corvus and tells you to hit the SAK. *Everything trusted --
the browse, the select, the provincia display, the per-cap-key auth -- is rendered
by **corvus** on the trusted path* (the cell-grid -> the kernel sink,
`TRUSTED-PATH.md` section 7), **never by the tool.** This is the section-3 property
generalized (TRUSTED-PATH section 4): if the tool drew the "authorize CAP_KILL?"
surface, a hostile program in your namespace could draw an identical fake and
harvest your confirmation or your cap-key. So the tool never renders the
authorization surface.

Two forms, both SAK-gated, both corvus-rendered:

- **`imperium` (no args) -- browse.** Posts a browse intent; you hit the SAK;
  corvus renders the **cursus-honorum browser** (your eligible levels + their
  caps, from `clearance.db`); you select; corvus shows the resulting provincia +
  term; you confirm + authenticate. The discovery form.
- **`imperium <caps...>` (the *lex curiata* above) -- direct.** Names the cap-set;
  the SAK; corvus shows the named provincia directly (skips the browse); confirm +
  auth. The expert/muscle-memory form.

**Elevation is human-only by construction, and the gate is the SAK, not the TUI:**
the SAK is a physical key the kernel catches, so no program can press it.
`imperium kill` invoked by a *script* merely posts a request that does nothing
until a human hits the SAK -- so both forms are equally script-safe; the browse
TUI is a usability choice, not the security boundary. (Belt-and-suspenders: the
tool may detect a non-interactive invocation -- no controlling tty -- and fail
fast with a helpful message, but that is UX, not the gate.)

**`imperium --list` (or `edict`) is the read-only sibling** -- "what do I hold /
what could I become" -- a *normal untrusted tool* reading the `/proc/self`
imperium flag + querying corvus read-only. Like the fasces prompt (section 4), it
is a convenience mirror, never the source of truth; the authoritative browse + the
grant are the trusted episode.

**The delegation companion.** Imperium is *self*-elevation. Its standing-delegation
sibling -- a *censor* persisting a bounded, revocable subset of its own imperium to
another citizen -- is the **mandate** (`docs/MANDATE-DESIGN.md`, invariant I-35),
the third leg of the authority system. Mandate *issuance* is the same trusted-path
episode from the censor's side (a SAK *lex curiata*); mandate *redemption* is the
silent login-time install -- the asymmetry that lets a citizen hold standing scoped
authority (e.g. network TCP:80) without a SAK per act.

## 4. The fasces -- the scale indicator

A magistrate's imperium was legible at a glance by the lictors before him: a
praetor 6, a consul 12, a dictator 24. So the elevated prompt shows a **fasces**
whose **rod count = the number of held caps**, and whose **axe (securis) = the
life-and-death caps** (`CAP_KILL`, anything destructive):

```
michael@thylacine$                  # no imperium (the plain prompt)
michael@thylacine ‖‖#               # imperium: 2 rods (chown, dac), NO axe
michael@thylacine ‖‖‖⟨securis⟩#     # imperium: 3 rods INCLUDING the axe -- danger
```

(`#` vs `$` keeps the classic elevated-prompt convention; the exact glyphs are
the shell's call -- Pale Fire / Halcyon.) The point is an instant, honest read
of "how dangerous is the shell I am typing into."

**Spoofing model (a real caution, not flavor).** The prompt fasces is rendered
by the *shell* from an unforgeable **kernel `/proc/self` imperium flag** (scale +
axe-bit). It is trusted *within* your session but a hostile program in your
namespace could draw a fake bundle. So the rule: **the grant + the true scale are
attested on the trusted path (SAK + corvus); the prompt is a convenience mirror
of the kernel flag, never the source of truth.** When in doubt, hit the SAK --
corvus tells you your real provincia.

## 5. The pomerium -- a safety boundary (axes come out inside the city)

Inside Rome's sacred boundary the lictors' axes were removed -- citizens had the
right of appeal, so no summary execution within the city. That maps onto a free
safety feature: designate protected scopes (a sensitive namespace, `/`, a
production dataset) as a **pomerium** where the **axe auto-lapses** -- the
destructive caps are suspended for operations *within* it, even under imperium.
An `rm` wielding the axe still cannot cross into a pomerium-protected tree.

The historical exception is the perfect escape hatch: the **dictator** kept his
axes inside the pomerium (absolute imperium, no appeal). So a **dictator** flag
(`imperium --dictator`) carries the axe across the boundary -- at the cost of an
extra, louder SAK confirmation. Default imperium respects the pomerium; the
dictator override is the explicit "yes, I really mean it."

(Staging note: the core imperium -- propagating scope + *lex curiata* entry +
fasces + abdicate -- stands alone. The pomerium + dictator are the richer safety
layer and can land as a second sub-chunk.)

## 6. Teardown -- *abdicatio*, the term, the exit

The episode ends three ways, each fully revoking the propagated subtree (I-25):

- **`abdicate`** (a dictator could lay down imperium early -- *abdicatio*): a
  shell builtin that ends the scope.
- **the term** (`valid_until`): imperium is an annual magistracy -- it
  auto-expires (a multi-hour bound), so a forgotten imperium shell is not a
  forgotten root shell.
- **shell exit**: the root dies -> the existing root-death teardown.

**Relinquish: sub-shell vs in-place.** A `sudo -s`-style legate **sub-shell**
(imperium spawns it; `exit`/`abdicate` relinquishes) reuses the *existing*
root-death teardown -- zero new kernel mechanism. The more elegant "your same
shell toggles into imperium in place" needs a new "de-escalate-the-root-without-
killing-it" teardown. **Recommend: ship the sub-shell first, evolve to in-place.**

## 6.5 `CAP_POST_SERVICE` — posting a service under imperium (DESIGNED 2026-09-10)

**Operator-ratified 2026-09-10.** The first capability designed *for* imperium
rather than inherited by it, and the reason is architectural rather than
thematic.

**The problem.** `haul` mounts a remote 9P tree. A mount lands in the calling
Proc's Territory and nowhere else (I-1), and Thylacine **clones** the namespace
at every fork where Plan 9 **shares** it — so a `mount` *command* can never
affect the shell that ran it. Plan 9's answer is `srv` + `mount /srv/foo`, which
works there only because of that sharing. Here the endpoint has to reach the
shell some other way, and `/srv` is the mechanism: its registry is namespace-
resident, so a child's post IS visible to the parent.

**Why the existing gate cannot be reused.** Posting to `/srv` is gated on
`PROC_FLAG_MAY_POST_SERVICE`. That is a **proc flag**, and proc flags "remain
set-once before EL0 by the Proc's own thread" (`kernel/proc.c:1768`) — the
single-writer rule that keeps `proc_flags` free of multi-writer RMW against the
console transitions. So it **cannot be conferred on a running shell by any
elevation mechanism**, imperium included. Nor would widening it at login help on
its own: proc flags do not propagate, so `haul` — a child of the shell — would
not inherit it.

**The shape.** A new elevation-only capability:

```c
#define CAP_POST_SERVICE (1ull << 13)     /* 12 is CAP_AUDIO_GRAPH on aux-3 */
/* joins CAP_ELEVATION_ONLY; MUST NOT join CAP_ALL (caps.h states the rule) */
```

**The bit number is 13, not 12, and how that was caught is the point.** The
first draft took bit 12 because 11 is main's high-water mark. Bit 12 is
`CAP_AUDIO_GRAPH` **on aux-3**, so "free" was true of this branch and false of
the project. A capability bit is a project-wide ABI allocation and main's
`caps.h` is not the register -- the union of the live branches is. Caught by
sweeping both sides during merge prep, hours after the design was ratified.

```c
```

Every property we want falls out of the existing machinery, which is the
argument for this shape over a bespoke one:

| Want | Falls out of |
|---|---|
| never held by default, never fork-grantable | excluded from `CAP_ALL` |
| a child never inherits it by accident | in `CAP_ELEVATION_ONLY` → stripped at every fork (I-2) |
| `haul` CAN inherit it when the user meant it | imperium's propagating scope is exactly the exemption from that strip (§2) |
| no program can widen it behind your back | the *lex curiata* — SAK, provincia shown on the trusted path before auth (§3) |
| **a posted service cannot outlive the imperium** | I-25 teardown group-terminates the scope subtree on `abdicate`/term/root-exit |

That last row is the one worth noticing: it answers, structurally, the "who
unmounts it, what is its lifetime" question the `/srv` design had left open. A
service posted under imperium dies with the imperium. sudo's backgrounded jobs
survive a logout; a legate's cannot, and neither can its services.

**The flag is KEPT, not replaced** (operator's choice of the two offered). The
two are different tiers of one gate, and `devsrv`'s check becomes
`PROC_FLAG_MAY_POST_SERVICE || CAP_POST_SERVICE`:

- **the flag** — spawn-time, non-propagating, kernel-stamped, joey-granted: the
  TCB's own servers (corvus, ptyfs, login's per-user home proxy).
- **the cap** — runtime, scope-propagating, user-elevated: the interactive path.

Two doors to one gate is a smell worth naming rather than hiding, but it mirrors
a pairing already in the tree: `PROC_FLAG_CONSOLE_ATTACHED` (a stamped role) and
`CAP_HOSTOWNER` (an elevated authority) coexist for the same reason. Retiring
the flag in favour of the cap was the rejected alternative: it would touch
joey's spawn of corvus/ptyfs, login's home proxy and its one-hop delegation, and
the `CONSOLE_OWNER` gate that keys off the flag — a wide blast radius on a
security-critical path for a tidiness win.

**Rejected outright: widening `MAY_POST_SERVICE` to the session shell.** The
syscall header states the containment in as many words — the shell, "lacking
MAY_POST_SERVICE, cannot itself re-designate the owner" — so widening it would
hand every program the user runs both service-posting AND the ability to
re-designate the console owner. I-27 would survive (the owner bit never confers
console-attach), but the widening is real and unnecessary given the above.

**Cross-track note, and a HARD SEQUENCING DEPENDENCY.** Imperium is the
auxiliary track's arc, and as of 2026-09-10 **it is not merged into main**:
`4c77db6e` (IM-2, the propagating scope) is an ancestor of `aux-3` only, which
is 126 commits ahead of main while main is 57 ahead of it. Main carries the A-4
legate (`legate_scope_id`, `legate_valid_until`) but **not** the propagating
mark.

That is not a citation detail — it decides what can be built. Elevation-only
caps are stripped at `rfork` **and** at `SYS_SPAWN_WITH_CAPS`, so without the
propagating scope `CAP_POST_SERVICE` cannot reach a child at all: an elevated
shell would hold it and `haul` — a child — would not. **The cap is inert for its
actual purpose until imperium is in main.** The first draft of this section said
"so this isn't a dependency on unbuilt work"; it is built, but not here, and
`git merge-base` was the one command that would have said so.

Consequences for sequencing:

- **The `ut` namespace builtins are UNBLOCKED** and should go first. They need
  no new capability — attaching to a posted service is not gated, only
  *posting* is — so they can be built and verified against a service that
  already exists (`/srv/ptyfs`, `/srv/stratum-fs`).
- **The cap + haul's post mode wait on the merge**, and the merge is the
  operator's call, not a thing either track should do unilaterally at 126/57
  divergence.
- This capability is *designed* here by the main track at the operator's
  direction and is not main's to land unilaterally into imperium's conferral
  set regardless.

## 7. Invariants + audit surface

- Extends **I-25**: a propagating legate's caps flow ONLY within its
  `legate_scope_id` subtree and the **entire** subtree de-escalates atomically on
  any teardown trigger; no elevated Proc outlives the scope; the durable identity
  is unchanged.
- **Pomerium invariant** (with section 5): the axe set is suppressed for ops
  resolving inside a pomerium-marked scope unless the dictator flag is set.
- **Audit-bearing surfaces** (prosecute hard -- this deliberately relaxes the
  spray-refusal): the `rfork` propagation path (a child must inherit caps ONLY
  under a propagating scope, never otherwise); the trusted-path *lex curiata*
  entry (the provincia displayed == the caps granted; no wider grant than shown);
  the propagated-subtree teardown (no straggler keeps a cap past abdication); the
  `/proc` imperium flag (unforgeable scale/axe read); the pomerium boundary check.

## 8. Roman vocabulary (load-bearing vs flavor)

Load-bearing (keep): **legate** (the elevated Proc, kernel term) + **imperium**
(the mode). Flavor that earns its place where it clarifies (held for signoff):
**provincia** (the requested cap-set / scope), **abdicate** (`abdicatio`,
relinquish), **term** (`valid_until`, the annual magistracy), **cursus honorum**
(the eligibility ladder in `clearance.db`), **pomerium** (the safety boundary),
**fasces** / **securis** (the scale indicator + its axe), **lex curiata** (the
conferral act). Don't force the rest.

## 9. Unblock list / dependencies

1. **Kernel: the fork-propagating legate scope** (section 2) -- the one new
   mechanism; spec-modeled (extends the I-25 teardown model).
2. **A-4c trusted-path re-auth** (the SAK + corvus prompt) -- imperium is its
   first real consumer; the *lex curiata* entry needs it.
3. **`DISTINCT_SECRET` per-level capability keys** in corvus (the "capability
   keys" you authenticate against) -- CORVUS-DESIGN clearance-auth extension.
4. **The `/proc/self` imperium flag** (scale + axe-bit) -- the prompt's source.
5. **Shell builtins** `imperium` / `abdicate` + the fasces prompt (native `ut` /
   libutopia).
6. **The pomerium scope registry** + the dictator override (section 5) -- the
   second sub-chunk.

## 10. The proof-of-concept (auxiliary track)

This document is the design. **Still unbuilt** -- and the path below is stale
(corrected 2026-08-16, aux#237: `usr/apps/**` no longer exists; a skeleton
would land in the main tree like every other promoted aux artifact). A
compile-only `imperium` skeleton (the
CLI parse + the fasces renderer [pure computation: cap-set -> the bundle] + the
corvus *lex curiata* / `cap::use_grant` flow behind a documented seam, modeled on
`usr/legate-prover/`) is the natural next aux artifact -- the same pattern as the
Tapestry `libtapestry` POC. It cannot RUN until the section-9 deps land, but it
proves the tool shape + makes the fasces concrete.
