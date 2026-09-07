# Imperium -- power-user clearance, the legate's command authority

**Status: ACCEPTED design (2026-06-08); landed in the canonical docs 2026-06-09.
REVISIT DONE 2026-09-07 (§11) -- the IM phase is OPEN on the aux track**
(operator-directed; the four forks ratified, §11.9). imperium is the post-LS
**identity-arc + Utopia-shell capstone**, not LS-arc work. Originated in the
auxiliary track (`aux/userspace-apps`, `usr/apps/IMPERIUM-DESIGN.md`); brought
here as the canonical design. §§1-9 are the rationale; **§11 is the as-built
design the code lands against.**

When built, the design folds into the A-4 / legate scripture
(`docs/reference/102-legate.md`, `IDENTITY-DESIGN.md section 9.8`, invariant
I-25) + `CORVUS-DESIGN.md` (the clearance auth) + a `NOVEL.md` angle (the
"abdication atomically de-escalates the entire elevated subtree -> an elevated
shell that is *safer* than a root shell" property -- advances Angle #9's
capability-elevation contract). The kernel lift -- the fork-propagating legate
scope (section 2) -- is built + modeled on the aux track since 2026-09-07
(§11.4; **spec-first re-enabled for that privilege surface**, like the SMP
redesign); it is the
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
proves the tool shape + makes the fasces concrete. **Superseded 2026-09-07:** no
skeleton is built; the arc lands the real tool at IM-4 (§11.6, §11.8).

## 11. As-built revisit (2026-09-07) -- the IM phase opened; the four forks ratified

**Status change.** The revisit §0 scheduled is DONE. The IM phase is OPEN on the
**aux** track (operator-directed 2026-09-07; the kernel lift §0 reserved for the
main agent is aux's -- main is on the Halcyon stabilization arc, disjoint
surfaces, declared on yip). Effort max on Fable 5.1. This section is the
as-built design the code lands against; §§1-9 above stay the rationale.

### 11.1 Ground truth the revisit verified (aux-3, post-merge of origin/main)

- **The legate substrate (A-4a) is complete**: the two-phase grant/redeem,
  `proc_become_legate` (`kernel/proc.c`), the scope tag inherited on `rfork`,
  the teardown at the ZOMBIE chokepoint and the EL0-tail expiry. At v1.0 every
  clearance cap is elevation-only, so `rfork` strips them and a scope MEMBER is
  unelevated (the `~CAP_ELEVATION_ONLY` strip in `rfork_internal`).
- **The SAK mechanism (A-4c-2) is live**: PL011 BREAK -> `sak_pending` ->
  `console_mgr` -> `proc_console_sak` (revoke the owner's attach, `owner = NULL`,
  attach corvus; posts NO note since RW-7 R2-F2; idempotent under a BREAK flood).
- **The episode is absent**: post-SAK corvus is attached and does nothing. It has
  no notes fd and no console handling; `AUTH_REQ_DISTINCT_SECRET` exists in its
  level table and is REFUSED ("A-4c not yet built").
- **Console gates as built**: input is a single-reader slot (`reader_busy`,
  first reader wins); `cons_output_write` is UNGATED (any Proc writes the UART);
  the renderer feed (`cons_feed_write`) is unconditional.
- **Orphan reaping**: init (joey) adopts and reaps orphans with a wait-any
  WNOHANG sweep, so the "#855 kproc leak" caveat of the legate reference is
  closed while init lives.
- **The harness can press the SAK**: LS-CI's serial is a `mux=on` chardev
  socket carrying the qemu monitor, so `Ctrl-A b` sends a BREAK. The A-4c-2
  "no BREAK injectable" note is stale; the whole arc gets a real E2E.
- **corvus's wire can defer a reply**: a verb handler that stages no response
  leaves the client's read parked -- exactly what the *lex curiata* needs.

### 11.2 The gaps between §§1-9 and the tree (each BUILT or CLOSED below)

- **G1** no episode (expected; IM-1 + IM-3 build it).
- **G2** output exclusivity is NOT enforced -- TRUSTED-PATH §8's "kernel-guaranteed
  sole writer" is asserted, not built. I-27 property 2.
- **G3** input exclusivity is NOT enforced -- the shell's parked read would drain
  the typed secret. I-27 property 1.
- **G4** the renderer feed is an injection path during an episode (a halcyond
  session's keyboard reaches the RX ring through userspace).
- **G5** under propagation the A-4a "benign unelevated straggler" (an `rfork`
  racing the teardown walk) holds the caps: an I-25 violation.
- **G6** nested redeems: `CAP_JIT` is user-default and every GL program activates
  it; the A-4a F2 "fresh scope per redeem" would RE-TAG an imperium member on a JIT
  activation and let it escape the imperium teardown while holding the caps.
- **G7** no SAK -> corvus signal. **G8** no per-level secret (the DISTINCT_SECRET
  wrap is unbuilt). **G9** spawn-time cap masks must compose with propagation.

### 11.3 The episode -- kernel, I-27 ENFORCED on the serial medium (IM-1)

State: `g_cons.episode_active` (atomic) + the saved termios word + an episode
`Rendez`.

- **BEGIN** (the `console_mgr` SAK dispatch): `proc_console_sak()` returns whether
  a trusted Proc got attached; iff so `cons_episode_begin()`: discard the cooked
  partial line (a SAK mid-line abandons the line -- pre-SAK bytes must never be
  the first bytes of the secret), save termios, force RAW (`ICANON|ECHO|ISIG|ICRNL`
  off), mark active, wake the data rendez so the parked shell reader re-evaluates.
  `proc_console_sak` posts the **`sak` note** to the trusted Proc under
  `g_proc_table_lock` (the established `exits -> notes_post` order).
- **READ, non-attached, while active**: never holds the reader slot; parks on the
  episode rendez until inactive, then re-acquires. **READ, attached**: normal.
- **WRITE, non-attached, while active** (both the `devcons` and the `devdev`
  door): parks on the episode rendez before the writer role -- **FREEZE**, never
  drop (ratified F2). Kernel writers (`cons_kernel_writer_begin`: diag lines,
  Halls) are unaffected; echo is off so the IRQ-context emit path is quiet.
- **FEED** (`cons_feed_write`): refused (-1) while active (G4). **DRAIN** (the
  renderer mirror): unchanged -- the bytes are public and the mirror cannot be
  typed into.
- **END**: `SYS_CONSOLE_EPISODE_END` (reserved 110; gated: the caller is
  console-attached AND active): restore termios, clear active, wake every parked
  reader/writer. ALSO cleared at the ZOMBIE chokepoint when the trusted Proc dies
  (fail-safe: the untrusted world unfreezes; no secret is in flight because the
  only reader is dead). A repeat SAK during an episode is idempotent. **No kernel
  timeout**: an episode ended behind corvus's back would route the next
  keystrokes -- the secret -- to the shell; corvus bounds its own prompt (a poll
  timeout) and ENDs. A hung corvus is a hung TCB, the class of corvus dying at boot.
- **After END** corvus stays console-attached (I-27 as today); the console OWNER
  is re-established by login / the session as today.
- **Lock order**: `g_cons.lock` is never held across `proc_console_sak`
  (unchanged); the chokepoint END takes the cons leaf lock under
  `g_proc_table_lock` -- a new edge with no reverse edge (`cons_input_read`
  queries the owner OUTSIDE `g_cons.lock`).
- **The serial sink (ratified F1)**: the kernel's contribution on serial is the
  OUTPUT-EXCLUSIVITY gate above; corvus composes the provincia as a
  medium-INDEPENDENT cell grid and rasterizes it to ANSI in userspace through its
  console handle. The kernel cell ABI (TRUSTED-PATH §7's rasterizing sink) lands
  with the framebuffer backend (v1.x) and consumes the same composer. Same bytes
  on the wire, same security on serial (§8: the anchor is the chain, not the
  pixels), a smaller TCB delta, and no ABI designed blind to the only backend that
  needs it.

**As-built refinements (IM-1, landed 2026-09-07). Each is a delta from the
bullets above, settled during the kernel reads and flagged here so the operator
can veto any of them:**

1. **The ARM gate.** A SAK opens an episode only if the trusted Proc has
   declared itself an episode CONSUMER -- `SYS_CONSOLE_EPISODE(ARM)` (110, op
   1; gate: the caller IS `g_console_trusted_proc`). Unarmed, a SAK is the
   A-4c-2 handoff exactly as before (attach corvus, no freeze, no note).
   Without the gate IM-1 alone would freeze the console on every BREAK with
   nobody to END it -- a regression in the window between IM-1 and IM-3. corvus
   arms once at startup (IM-3). The arm is a property of the trusted IDENTITY:
   it dies with the Proc and is cleared when the authority changes; it
   survives the Proc's own relinquish.
2. **Every SAK, not the first.** The `sak` note + BEGIN fire on EVERY SAK that
   finds an armed, alive trusted Proc and no open episode. The pre-IM-1
   idempotent early return (trusted attached, owner NULL) would have swallowed
   every SAK after the first of a session, because the owner stays NULL until
   the next login. The handoff half stays idempotent (a repeat re-grants
   nothing); the episode half is decided on every SAK; a SAK during an open
   episode is a no-op (no restart, no second note). `proc_console_sak` emits
   one `cons: SAK (<decision>)` diagnostic per SAK -- the harness's witness
   (`tools/interactive/im1-sak-lever.exp`: Ctrl-A b on the muxed serial IS a
   BREAK; A-4c-2's "no BREAK injectable" note was stale).
3. **The pre-SAK OWNER is handed back at END.** The SAK remembers the console
   owner it unseats (`g_console_owner_pre_sak`; cleared at that Proc's death
   and by its own relinquish, so a bringup-era SAK can never re-install init
   as the Ctrl-C target); END restores it into an EMPTY owner slot only (a
   claim made during the episode wins), one-shot. Without it every imperium
   episode would leave the session without a Ctrl-C target until the next
   login. Supersedes "the console OWNER is re-established by login / the
   session as today" above.
4. **One syscall, two ops.** `SYS_CONSOLE_EPISODE = 110` carries ARM (1) and
   END (2); the reserved `SYS_CONSOLE_EPISODE_END (110)` above is the END op.
   The END gate is the trusted IDENTITY plus an open episode -- not the attach
   bit: the SAK attached the caller, and a relinquish ends the episode by
   itself (item 6).
5. **BEGIN discards ALL pending input** -- the ring's committed lines as well
   as the cooked partial line: a completed-but-undrained line is pre-SAK input
   by the same argument (the shell had not consumed it, and the SAK is the
   operator declaring that what follows is for the trusted prompt). Documented
   residue: a PL011 holdback byte parked by #174 back-pressure (a FULL ring at
   the SAK instant) is pumped in by the attached reader's first drain.
6. **Three fail-safe closes, all under `g_proc_table_lock`:** the trusted
   Proc's death (the ZOMBIE chokepoint; the arm dies with it), its own
   `SYS_CONSOLE_RELINQUISH` (it could never END through the gate again; the
   arm persists), and a CHANGE of trusted authority (`proc_set_console_-
   trusted`). Each hands the pre-SAK owner back. BEGIN itself runs under the
   same lock hold that ALIVE-checked the trusted Proc, so no episode opens
   behind a consumer that just left; the `sak` note is posted AFTER BEGIN (a
   refused post closes the episode again -- an episode nobody was told about
   is a frozen console), then the caught-note wake. So BEGIN, END and the
   fail-safe closes all take the cons leaf lock under `g_proc_table_lock`:
   one edge, no reverse (cons queries the table only with its own lock
   released; no sleep cond takes a lock).
7. **The frozen world is wider than read / write / feed.** A non-attached
   consctl WRITE is refused (the native twin of C2-k1b F2: an inherited
   consctl fd must not flip ECHO back on under the prompt; a renderer's
   `serialsilent 1` must not blank the provincia on the medium the SAK just
   restored) -- G4 generalized. And a non-attached POLL reports no readiness
   AND is not woken per key byte: its hook parks on a separate list the
   per-byte RX relay never walks, because `sys_poll` returns to userspace on
   any hook wake, so one return per keystroke would have handed the shell the
   secret's length and cadence with the readiness word reading 0. It is woken
   once, at END.
8. **A frozen reader re-takes its slot by WAITING**, never by the
   single-reader guard's -1 (the authority may still be mid-read at END, and
   a -1 reads to the shell as its console going away); an attached reader
   during an episode waits the same way behind a vacating non-attached
   holder. Every other contender keeps the documented -1. Consequence: two
   non-attached readers frozen together both get the console after END, in
   turn, where pre-episode the second would have been refused.

### 11.4 The propagating legate scope -- kernel, I-25 STRENGTHENED, spec-first (IM-2)

- The grant gains `flags` (PROPAGATING): a new **`SYS_CAP_GRANT_IMPERIUM`**
  (reserved 111; `x0` mask, `x1` stripes, `x2` valid_for, `x3` session, `x4`
  flags) beside `SYS_CAP_GRANT_CLEARANCE`, and a 40-byte `/cap/grant` form
  (length-discriminated, additive). The redeem (`SYS_CAP_USE`, unchanged) sets
  `PROC_FLAG_LEGATE_PROPAGATING` on the root and records **`legate_caps`** (the
  redeemed set) on the Proc.
- **`rfork`**: `child->caps = (parent_caps & mask) & ~(CAP_ELEVATION_ONLY & ~flow)`
  with `flow = parent->legate_caps` iff the parent's scope is propagating, else 0;
  `child->legate_caps = flow`; the propagating property is inherited as a MEMBER
  property, never the ROOT flag. `CAP_HOSTOWNER` is never clearance-grantable, so
  it never flows. The spawn mask still bounds everything (G9).
- **Straggler close (G5)**: the child's scope + caps inheritance is finalized
  under `g_proc_table_lock` at table insert; a parent already terminating
  (`group_exit_msg` set) fails the `rfork`. A member of a torn-down scope cannot
  mint a child.
- **One scope per Proc, set once (G6; retires A-4a F2)**: a redeem on a Proc that
  already carries a scope ORs the caps but keeps its tag / session / root status,
  and `legate_valid_until` becomes the EARLIER nonzero deadline (conservative). A
  PROPAGATING grant redeemed by a Proc already in ANY scope is REFUSED --
  propagating scopes never nest; abdicate first. JIT under imperium therefore
  works: the member keeps the imperium tag, and `CAP_JIT` -- elevation-only and
  outside `legate_caps` -- still does not flow to its children.
- **Teardown**: unchanged in mechanism (root death / expiry -> group-terminate
  every tag holder) -- now LOAD-BEARING for privilege, since members are elevated.
- **`/proc/<pid>/imperium`** (0400 + the owner-or-`CAP_HOSTOWNER` gate at the
  read site -- the `sched`/`environ` posture; refinement 3 below):
  `scope N session N propagating 0|1 rods N axe 0|1 caps 0xHEX until NS`, with
  `rods = popcount(legate_caps)` (what flows to this Proc's children) and
  `axe = CAP_KILL held`; `caps` is the elevation-only set as HELD (a further
  redeem's extras show there and not in `rods`). This is the §4 "unforgeable
  kernel flag"; `/proc/self` (the #66 cluster) folds in if cheap.
- **Spec**: `specs/imperium.tla`, written + TLC-green BEFORE the impl (spec-first
  RE-ENABLED for this surface, per §0; LANDED 2026-09-07 with IM-2): roots /
  members, propagation on fork, the clean exit + the kill-then-die window + the
  expiry sweep as three distinct teardown shapes, the fork-vs-teardown race, the
  nested redeem; invariants `NoElevatedOutlivesScope` (keyed on the root Proc's
  IDENTITY at the join -- the `anchor` -- not on the scope number a re-tag
  moves), `FlowOnlyUnderPropagating`, `OneScopePerProc`, `ScopeTraitsSetOnce`,
  `MembersNeverRoot`, `PropagatingIsScopeWide`, `FlowNeverWidens`, + the
  `ScopeEventuallyEmpty` liveness; buggy cfgs `imperium_buggy_straggler`,
  `imperium_buggy_retag`, `imperium_buggy_flow_without_flag`,
  `imperium_buggy_nest` (each trips exactly its named invariant; the clean
  model: 157,839 distinct states, liveness checked).

**As-built refinements (IM-2, landed 2026-09-07). Each is a delta from the
bullets above, settled during the kernel reads + the TLC runs and flagged here
so the operator can veto any of them:**

1. **The propagating property is a legate-block field, not a `proc_flags`
   bit.** `Proc.legate_flags` (`LEGATE_FLAG_PROPAGATING`), beside the new
   `Proc.legate_caps`, both appended at the struct tail (392 -> 408 bytes; no
   existing offset moved). The bullets named `PROC_FLAG_LEGATE_PROPAGATING`, but
   `proc_flags` never inherit across rfork and this property MUST (it is a
   member property, scope-wide: `imperium.tla::PropagatingIsScopeWide`), so it
   lives with the tag it travels with. `PROC_FLAG_LEGATE_ROOT` is unchanged and
   still never inherits.
2. **A PROPAGATING grant is bounded to `CAP_GRANTABLE_IMPERIUM` =
   `DAC_OVERRIDE | CHOWN | KILL`** -- exactly the imperium level of §11.5.
   `CAP_DEBUG` (a debugger's own debuggee would hold the debug authority; I-39
   is per-grant), `CAP_JIT` (I-42's "non-heritable" letter) and
   `CAP_AUDIO_GRAPH` (I-46's per-program whole-sink authority) stay plain
   clearances: their heritability clauses hold by construction in the kernel,
   not by corvus's policy alone. A propagating grant naming any other bit is
   refused at register.
3. **`/proc/<pid>/imperium` is 0400 + the read-site owner-or-`CAP_HOSTOWNER`
   gate** (`devproc_owner_or_hostowner`, the `sched`/`environ` predicate), not
   the 0444 the bullet said: `status` is UNGATED (0444 to every pid), so "the
   two-axis gate like `status`" described a gate that file does not have. Who
   is elevated and what flows is a disclosure about another user's authority;
   `CAP_DAC_OVERRIDE` is not a read axis; a denied read formats nothing.
4. **A child's `legate_caps` is the flow it actually HOLDS** (`flow &
   child->caps`), not the parent's offer: a spawn mask that omits a flowing bit
   (G9) narrows the flowing set for the whole subtree below and never widens it
   (`FlowNeverWidens`), and `rods` counts caps the Proc really carries.
5. **The stamp runs under the cap-table lock, before the consume.**
   `proc_become_legate` is called inside `cap_redeem_grant_for_writer`'s
   `g_cap_grants.lock` hold (it takes no lock and cannot sleep): the lock is
   what serializes two redeems by peer threads of one Proc, which could
   otherwise both read scope 0 and mint two roots, the second overwriting the
   first's flowing set. Stamping before `cap_clear_locked` means a refusal
   (the nest check, an invalid flag) never loses the grant.
6. **The straggler close is the parent's `group_exit_msg` re-checked under the
   SAME `g_proc_table_lock` hold as `proc_link_child`.** The child's tag + caps
   are still copied outside the lock (as at A-4a): the child is invisible to
   the sweep until the link, and the sweep marks the PARENT, so the flag is
   the witness. A refused rfork rolls the fully-built child back
   (`thread_free` of the never-readied thread, then `proc_free`); the -1 never
   reaches the parent's userspace, which dies at that syscall's own return
   tail. Uniform for every terminating parent, not only the sweep's.
7. **The Linux-phenotype fork mask is `CAP_ALL | CAP_ELEVATION_ONLY`** (was
   `CAP_ALL`; bit-identical before the carve): a Linux child of an imperium
   sub-shell is exactly as elevated as a native child spawned with a full
   mask. A phenotype conferring LESS authority is as much an I-43 breach as
   one conferring more, and the carve, not the mask, is what bounds the flow.
8. **The further redeem's deadline** is the earlier nonzero one, as designed;
   as built the field is stored and loaded atomically (relaxed) because a
   peer thread's rfork copies it and its own EL0 tail reads it.
9. **The one-syscall window is inherited, stated, not new.** A flagged Proc
   completes the syscall it is in (the die-check is on the EL0 return tail;
   `userland_enter` has none), so a member marked by the sweep can finish one
   elevated syscall per thread -- the pre-existing I-24 semantics, now
   privilege-bearing. The spec header says so; the audit row prosecutes it.
10. **The model grew a fourth counterexample + four invariants** beyond the
    three the bullet named: `imperium_buggy_nest` (a PROPAGATING further
    redeem admitted: the flag flips / the flowing set widens ->
    `ScopeTraitsSetOnce`), plus `MembersNeverRoot`, `PropagatingIsScopeWide`,
    `FlowNeverWidens`. The first TLC run tripped `NoElevatedOutlivesScope` on a
    root re-tagging ITSELF (two steps) rather than the member escape the cfg
    promised; keying the invariant on the anchor root's identity gave the
    four-step escape and is the more faithful reading of I-25.
11. **A consequence for §11.6 to design around:** a shell already in ANY
    legate scope (a `jit` clearance, say) cannot obtain imperium -- its
    `usr/imperium` child inherits that scope as a member and the PROPAGATING
    redeem is refused. That is the ratified "propagating never nests;
    abdicate first" rule, recorded here so IM-4's UX says so instead of
    failing silently.

### 11.5 The *lex curiata* -- corvus (IM-3)

- **The `imperium` clearance level**: caps `DAC_OVERRIDE | CHOWN | KILL`,
  `auth_required = DISTINCT_SECRET`, `time_bound = 4h`, PROPAGATING. Eligibility
  is admin-granted (`CLEARANCE_GRANT`), like audio-graph. The request's cap-set is
  the self-restriction subset (the existing `self_restrict`, STS-style):
  `imperium chown dac` is the level restricted to {CHOWN, DAC}; `imperium kill`
  carries the axe. Multi-level composition is v1.x.
- **The capability key (G8; ratified F4)**: at `CLEARANCE_GRANT` of a
  DISTINCT_SECRET level the hostowner supplies the user's initial per-(user,
  level) key; corvus stores a CRVS wrap (argon2id(key) -> AEAD over a random
  32-byte token; the tag is the verifier) as a new additive record kind in
  `clearance.db`. A VERIFIER, never a DEK (axis hygiene, IDENTITY-DESIGN §3.1).
  Attempts are rate-limited (the C-16 discipline). User rotation of their own key
  is v1.x.
- **`IMPERIUM_REQUEST` (verb 19)** `{level, self_restrict, term_req}`: eligibility
  + the level must be DISTINCT_SECRET + ONE pending slot system-wide (a second
  request -> BUSY; 60 s without a SAK -> TIMEOUT). The reply is DEFERRED -- staged
  only when the episode concludes: OK{session_id, caps} / DENIED / TIMEOUT. The
  requester's stripes are re-read LIVE at confer time (`SYS_SRV_PEER`, C-22); a
  dead requester gets no grant.
- **The episode consumer**: the `sak` note (corvus adds its notes fd to the poll
  set) -> `SYS_CONSOLE_OPEN` (gated on attached; corvus qualifies post-SAK) ->
  compose the provincia as a cell grid -> rasterize to ANSI through the console
  handle -> read the key (raw, no echo, bounded, 60 s poll timeout) -> verify ->
  `SYS_CAP_GRANT_IMPERIUM(..., PROPAGATING)` -> stage the reply ->
  `SYS_CONSOLE_EPISODE_END` -> close the handle + wipe. No pending request:
  "trusted path: nothing pending -- press any key" -> END. Audit-log lines for
  request / confer / deny / timeout.
- `CLEARANCE_ACTIVATE(_SELF)` on a DISTINCT_SECRET level stays REFUSED: the only
  path is the SAK episode.

**As-built refinements (IM-3, landed 2026-09-07). Each is a delta from the
bullets above, settled during the corvus / kernel reads and flagged here so
the operator can veto any of them:**

1. **The deferred reply is a PARKED `Tread`, not an empty one.** §11.1's "a
   verb handler that stages no response leaves the client's read parked" was
   wrong as stated: `dispatch_tread` drained zero bytes into an `Rread` of
   count 0, which the kernel client hands to userspace as EOF. As built,
   `IMPERIUM_REQUEST` marks the connection `awaiting_deferred`; a `Tread` on
   `ctl` that finds nothing staged is recorded (tag + count) and answered
   later, when the episode stages the reply (9P permits a delayed R; since
   #841 the srvconn client blocks with no steady-state deadline, death-
   interruptible, so the park is safe). A `Tflush` of the parked tag (the
   kernel client's abandon on a note-interrupted read), a `Tversion` reset, or
   the connection's close drops the park AND the pending request.
2. **The key wrap is its own compact layout, CRVS-KV v1 (136 bytes)**, not the
   3752-byte CRVS v1 whose ciphertext field is fixed at the keypair length:
   magic `CRKV` + version + the argon2 cost triple (the same emit envelope
   `crvs_v1_unpack` enforces) + salt + nonce + a 32-byte sealed token + the
   32-byte tag. AD = `"thylacine-corvus-capkey-v1"` || len(subject) || subject
   || len(level) || level -- length-prefixed so no two (subject, level) pairs
   share an AD; a distinct prefix domain-separates it from the passphrase and
   recovery wraps. Interactive argon2id preset (2 / 16 MiB / 1).
3. **`clearance.db` version 2.** Record kind 2 = KEY {kind, subject_len,
   level_len, subject, level, wrap_len u16, wrap}. The reader accepts v1 and
   v2 (a v1 reader fails closed on v2, the posture a corrupt db already has);
   the writer emits v2. A key record whose eligibility is gone is inert (the
   request requires both) and tolerated on load; the writer never produces
   one (REVOKE removes both together, rolled back together).
4. **`CLEARANCE_GRANT`'s key tail is shaped by the level**: required for a
   DISTINCT_SECRET level, refused for a RE_AUTH one; a DISTINCT_SECRET level
   is grantable to a USER subject only (a group cannot hold one key).
   Idempotent on retry: eligibility present + verifier present + the supplied
   key verifies -> OK with no rewrite (the boot ladder re-grants every boot); a
   different key is the hostowner's reset of that subject's key -- the v1.0
   rotation path. User self-rotation stays v1.x.
5. **`IMPERIUM_REQUEST` needs no live login session.** The SAK plus the
   distinct key ARE the authentication; a session would be a weaker second
   factor bolted onto a stronger one (verb 18's session requirement exists
   because a session is its ONLY proof). The gates: a live peer whose principal
   is a corvus user; a DISTINCT_SECRET level (a RE_AUTH level is refused: use
   18); eligible; enrolled; a non-empty self-restriction; the one slot.
6. **The rate limit is the RECOVER discipline**: wrong keys per (user, level),
   in memory, `IMPERIUM_FAIL_MAX` = 5, checked BEFORE the KDF at confer (a
   locked subject is shown a panel and asked nothing), reset on success,
   cleared at restart. A decline (empty key / Ctrl-C) and a prompt timeout are
   not wrong keys and are not counted.
7. **The episode runs INLINE in corvus's single thread**, bounded by the 60-s
   prompt: its other clients' 9P messages wait in the kernel rings for its
   duration. Simpler to audit than a state machine interleaved with the poll
   loop; a documented residue.
8. **Two wire-additive statuses**: `Timeout` (7) and `Busy` (8).
9. **A SAK with nothing pending** renders "nothing pending -- press any key",
   waits for one byte (bounded), and ENDs -- the §4 "when in doubt, hit the
   SAK" check, and what `im1-sak-lever.exp` now exercises.
10. **ARM at startup, not fatal.** corvus opens its notes fd and ARMs after
    loading its databases; a refused ARM (a spawn outside the boot chain --
    not the trusted authority) is a WARN and the lex curiata is unavailable
    while everything else serves.
11. **The provincia is an 8-bit ASCII cell grid** (glyph + attribute per
    cell; `usr/corvus/src/provincia.rs`) rasterized to CR LF rows with minimal
    SGR in userspace -- no Unicode box drawing: the harness decodes the serial
    stream as iso8859-1 and the v1.x framebuffer sink consumes a byte grid.
12. **Confer re-derives the requester LIVE** (C-22): `SYS_SRV_PEER` alive, the
    same stripes and principal as at request time, still eligible, verifier
    present -- any miss denies; a requester whose connection is gone gets no
    grant and no reply.
13. **The key prompt**: raw, unechoed; DEL/BS edit, Ctrl-U kills the line,
    CR/LF submits; Ctrl-C or an EMPTY key DECLINES (denied, uncounted); a key
    past `MAX_PASS_LEN` is denied; the whole prompt is bounded at 60 s ->
    `Timeout`. The prompt's clock is `CLOCK_MONOTONIC` with an idle-slice
    fallback (a broken clock still bounds it; a keystroke flood cannot
    exhaust it). Every buffer is wiped on every path; the console handle is
    closed at END.
14. **`CLEARANCE_LIST`'s wire is unchanged** (no propagating TLV): the level's
    `propagating` flag is corvus-internal; IM-4's `--list` may add a TLV tag
    (additive by construction).
15. **libthyla-rs gained `t_console_open`** (`SYS_CONSOLE_OPEN` = 64; no
    wrapper existed -- joey and login are C). No kernel change in IM-3.
16. **The boot ladder** grants michael the `imperium` level with the fixture
    key `imperium-key-michael-v1` (idempotent per 4) and probes the deny
    paths: a RE_AUTH grant with a key tail, a DISTINCT_SECRET grant without
    one, a DISTINCT_SECRET grant to a group (all `BadFormat`), and verb 19
    from `PRINCIPAL_SYSTEM` twice (`PermissionDenied` both times: the first
    refusal took no slot). The boot prover is `usr/imperium-probe`, run from
    a login session by `tools/interactive/im3-lex-curiata.exp` (confer /
    wrong key / busy / timeout / the session survives); `im1-sak-lever.exp`
    now asserts `cons: SAK (episode)` and the empty episode.

### 11.6 Userspace -- the sub-shell model (IM-4)

- **`usr/imperium`** (native libthyla-rs; thin and untrusted, §3.1):
  `imperium [caps...]` -> IMPERIUM_REQUEST -> "confer with the SAK" -> blocks on
  the deferred reply -> on OK `cap::use_grant(caps)` (becomes the propagating
  legate ROOT) -> spawns `ut` (fd 0/1/2 inherited, the same identity) -> waits ->
  exits, and the root's death sweeps straggling background jobs. No caps = the
  full level. `imperium --list` / `edict` are read-only (CLEARANCE_LIST + the
  /proc flag). No controlling tty -> fail fast (UX, not the gate: a script's
  request does nothing until a human presses BREAK).
- **`ut`**: the fasces in the prompt from `/proc/<pid>/imperium` (§4: one rod per
  held cap, the axe glyph when CAP_KILL is held, `#`); `abdicate` = exit iff a
  legate member (else "not under imperium").
- Sub-shell first (§6's recommendation); the in-place toggle is v1.x.

**As-built refinements (IM-4, landed 2026-09-07). Each is a delta from the
bullets above, settled during the implementation + the kernel reads, and flagged
here so the operator can veto any of them:**

1. **The whole chunk is PURE USERSPACE -- no kernel change, confirmed not
   assumed.** IM-4 CONSUMES the IM-2 propagation and the IM-3 confer. The one
   load-bearing question the IM-3 boot prover did not answer -- does
   `Command::spawn` propagate the propagating-legate scope + caps to the spawned
   `ut`? -- was settled by reading the kernel: every `SYS_SPAWN` variant routes
   through `rfork_with_caps(RFPROC, ...)` -> `rfork_internal`, whose carve
   (`child->caps = (parent & mask) & ~(CAP_ELEVATION_ONLY & ~flow)` + the
   `legate_scope`/`legate_caps`/`legate_flags` copy) keeps the flow, and
   `proc_exec_replace` swaps only the address space / phenotype / sigtab -- it
   never touches `caps` or the legate fields. So `imperium` spawning `/bin/ut`
   yields an elevated, propagating MEMBER in the root's scope that dies with it.
2. **The `/proc/<pid>/imperium` parser is a STANDALONE crate, `usr/lib/fasces`,
   not a libutopia module.** libthyla-rs's `_start` inline asm uses ELF
   directives (`.type`, `.size`) the macOS assembler rejects, so ANY crate
   depending on it (libutopia included) cannot host-compile -- which means
   libutopia's own `#[cfg(test)]` modules have never run on host. A pure,
   dependency-free crate host-tests cleanly (7 tests, the `corvus-crypto`
   `cfg_attr(not(test), no_std)` pattern) AND lets the thin `imperium` tool
   avoid pulling in the whole shell. One parser for three consumers (the prompt,
   `abdicate`, the tool) -- no drift on a kernel-defined ABI line.
3. **The prompt fasces** renders one rod (`‖` U+2016, capped at 6) per held
   elevation cap, the securis (`⚔` U+2694) when CAP_KILL is held, then `#` -- in
   a warning hue (palette Sand) when the axe is present, ember (Glyph) otherwise.
   It is ONE self-resetting SGR (no mid-token escape), so an E2E `-ex` match on
   the `#` marker is not split by a color run (the IM-3 bold-value trap). Read
   ONCE (`probe_imperium`, gated like `open_notes` on a live session): a shell's
   imperium status is fixed for its life (born into a scope or never in one;
   `abdicate`/exit ENDS it rather than de-escalating in place).
4. **`abdicate`** exits iff `/proc/<pid>/imperium` reports a nonzero scope -- the
   kernel's unforgeable flag, read through the SAME `read_own_imperium` the
   prompt uses (no shell-side belief that could disagree). Not under a scope ->
   "not under an imperium scope", status 1, no exit. In an `imperium` sub-shell
   `exit` and `abdicate` both end the session (the root's death sweeps the
   scope); `abdicate` is the guarded, named form and the one that refuses in an
   ordinary shell.
5. **The tool fails fast on a non-interactive fd 0** (`fd_devclass` not `'c'`/
   `'t'`) BEFORE posting a request: a script's request would otherwise park
   forever on a reply no human is there to confer. UX, not the gate -- the SAK
   remains the gate (§3.1).
6. **The tool refuses when already in a scope** (reads its own `/proc` flag):
   the kernel refuses the nested propagating redeem (11.4 consequence 11,
   "never nests"); the tool says "abdicate first" instead of surfacing a bare
   redeem failure.
7. **`imperium --list` (also `edict`, `-l`) ships the /proc-flag half only** --
   the current holdings (scope, caps by name, the axe, propagating). The
   eligibility half ("what you could become") needs a NEW corvus verb
   (`CLEARANCE_LIST_SELF`, the verb-18 identity shape, read-only) -- DEFERRED as
   the one user-input fork and surfaced to the operator, because this session
   ran on the Opus fallback (which stops at user-input items rather than adding a
   corvus wire verb). The /proc half needs no corvus change.
8. **The sub-shell gets no `--home`.** The user's home is a shell variable in
   the outer `ut`, not exported to `/env`, so the spawned `ut` runs at the
   inherited cwd (its prompt shows the absolute path; a bare `cd` goes to `/`).
   A v1.x nicety (export `HOME`); the fasces -- the point of the elevated shell
   -- works regardless. Documented, not silent.
9. **The tool re-checks the grant is not wider than requested** before redeeming
   (`granted & !self_restrict == 0`) -- defense in depth; corvus already bounds
   it, but the tool re-verifies its own request.

### 11.7 Honest scope

**Open presentation choice for the v1.x framebuffer sink (operator question,
2026-09-07).** When the kernel trusted sink lands, the episode does NOT suspend
userspace threads -- it freezes the console world only (the audio cycle, netd
and the compositor's clients keep running; I-46 forbids a stall) -- and what the
compositor loses is the scanout and the keyboard: the kernel is the sole
painter for the episode and scans the SAK combo through the Menagerie
trusted-tier keyboard. Whether it presents corvus's cell grid FULL-FRAME or as
a centered panel over a DIMMED SNAPSHOT of the last frame (the secure-desktop
shape) is undecided and sound either way, on one condition: the backdrop must
be a kernel-owned COPY of the last frame, never the compositor's live buffer
(§8: the anchor is the chain, not the pixels). The dimmed-snapshot panel keeps
the operator's context and is the recommendation; corvus supplies the panel
content through the same composer the serial path uses, the kernel decides the
framing. Settle it in the framebuffer-sink chunk, not before.

On a virtio-gpu-only medium the trusted path is SERIAL (TRUSTED-PATH §7,
2026-07-17): the SAK is the BREAK, corvus's prompt goes out the UART, and a
graphical session sees it only as the drain MIRROR -- untrusted and unwritable,
because the feed is blocked. The graphical SAK, the kernel framebuffer sink and
the trusted-tier keyboard stay v1.x. Pomerium + dictator (§5) stay the second
sub-chunk (IM-6).

### 11.8 The plan

| Step | Lands | Bar |
|---|---|---|
| **IM-0** | this section + TRUSTED-PATH §7/§12/§15 + ARCH §25.2/§28 + CLAUDE.md rows + ROADMAP / phase7-status + ERRORS.md `sak` + SPEC-TO-CODE | scripture, no code |
| **IM-1** | the episode (§11.3): cons.c / proc.c / syscall.c / notes.c + kernel unit tests + the LS-CI BREAK lever with a positive and a negative control | audit:hard (I-27); suite; SMP gate |
| **IM-2** | `specs/imperium.tla` first, then §11.4 in proc.c / devcap.c / devproc.c / caps.h + tests | audit:hard (I-2/I-25); spec green; suite; SMP gate |
| **IM-3** | §11.5 in corvus + a boot prover (request -> harness BREAK -> confer -> redeem) | audit:hard (crypto + privilege) |
| **IM-4** | §11.6: `usr/imperium` + ut `abdicate` + the fasces + the manual page | host tests + boot |
| **IM-5** | `ls-imperium.exp` (login -> `imperium chown dac` -> BREAK -> provincia -> key -> fasces -> `chown` works -> a background job -> `abdicate` -> the job is dead, the fasces gone, `chown` denied; deny arms: wrong key, ineligible user, a script cannot confer) + the batched holotype rounds per double-distance + the SMP gate | audits clean; both mirrors |
| **IM-6** | pomerium + dictator | later |

### 11.9 The ratified forks (operator, 2026-09-07 -- each the recommendation)

- **F1** serial sink = the kernel output-exclusivity gate + corvus-side cell->ANSI
  rasterization; the kernel cell ABI lands with the framebuffer sink.
- **F2** non-attached console I/O during an episode = FREEZE (park until END).
- **F3** the SAK -> corvus signal = a new `sak` known-note (default IGNORE; a note
  NAME is ABI, registered in `docs/ERRORS.md`).
- **F4** the imperium key = hostowner-set at `CLEARANCE_GRANT`; user rotation v1.x.
- Riding the same signoff: `SYS_CONSOLE_EPISODE_END` (110) +
  `SYS_CAP_GRANT_IMPERIUM` (111), the 40-byte `/cap/grant` form,
  `specs/imperium.tla` spec-first, aux owning the kernel lift.

### 11.10 Invariants + what the audits prosecute

- **I-27 (the episode)**: input reaches only the attached Proc; output is only the
  attached Proc's; feed blocked; raw forced; END and death lift the freeze;
  repeat-SAK idempotent; no timeout corvus does not know about; the lock order.
- **I-25 strengthened**: no elevated Proc outlives its scope INCLUDING propagated
  members and the straggler; one scope per Proc; propagating never nests.
- **I-2**: caps flow only from `legate_caps` under a propagating scope; HOSTOWNER
  never; spawn masks still bound; JIT / DEBUG / AUDIO do not flow unless in the
  level.
- **I-22**: the durable identity is unchanged; the sub-shell is the same principal.
- **corvus**: the deferred reply is conn-bound and stripes are re-read live; the
  key wrap is a verifier, not a DEK; rate limit; secret hygiene (the key + console
  buffers wiped, the console handle closed at END).
- **DoS**: one pending slot with expiry; the freeze is bounded by corvus's prompt
  timeout; an episode can be started only by BREAK, never by software.
