# TRUSTED-PATH.md — the SAK, the kernel-arbitrated episode, one mechanism per renderer

**Binding scripture.** Adopted 2026-06-15 (the aux architecture session;
user-ratified). The canonical design for the trusted path **generalized off the
serial substrate** — the uniform interface (the four-role boundary, the
renderer contract, the request-then-confirm flow, the medium-independent output
path) that login (A-5), imperium elevation (A-4), and the installer all ride.

It **extends A-4c / invariant I-27** (`IDENTITY-DESIGN.md §9.8`; ARCH §17.1, §28).
Grounded in the EXISTING serial trusted path (`kernel/cons.c`, `kernel/proc.c`):
the SAK is already a PL011 BREAK, and the kernel already moves the trusted
authority to corvus on it. This generalizes that — *unchanged in structure* — to a
graphical renderer (Aurora) and beyond.

**One reconciliation, stated up front (the design prose corrected against the live
code):** the SAK does NOT make corvus the console *owner*. Post-LS-5 / RW-7 R2-F1
(the owner/attach split, `@2608c88`), `proc_console_sak` (`kernel/proc.c`) clears
`g_console_owner` to NULL and grants console-**ATTACH** (`PROC_FLAG_CONSOLE_ATTACHED`)
to the trusted authority (corvus, `g_console_trusted_proc`) — it deliberately does
*not* make corvus the owner. The two are distinct axes (§3): the **owner** is the
`interrupt` (Ctrl-C) target; the **attach** is the elevation/trusted gate (the
`devcap` redeem keys on `PROC_FLAG_CONSOLE_ATTACHED`). Making corvus the owner would
re-introduce the bug RW-7 R2-F1 fixed — a post-SAK Ctrl-C posting `interrupt` to
corvus and killing the trusted path until reboot. This doc uses the corrected
mechanism throughout.

Cross-refs: `docs/MENAGERIE.md` §9 (the trusted-tier keyboard the graphical SAK
rides) + §7 (the framebuffer source); `docs/AURORA.md` (the renderer that honors
enter/leave-trusted); `docs/INSTALLER.md` (the first-credential-mint episode);
`IMPERIUM-DESIGN.md` §4 (the fasces / `/proc/self` imperium flag); ARCH §17.1
(console) + §28 (I-27).

---

## 1. Thesis: one trusted path, below every renderer

The trusted path is the unspoofable channel to corvus — what makes login, imperium
elevation, and the installer's first-credential mint trustworthy. The governing
decision: **put the entire mechanism in the kernel, beneath the renderer, so the
renderer is interchangeable.** Serial proves this works today with *zero* terminal
cooperation — the host terminal holds no capability, registers no callback, and the
kernel still delivers the guarantee. We keep that asymmetry and generalize it: the
same SAK + the same forced hand-over to corvus, whether the renderer is a serial
terminal, Aurora, or eventually Halcyon.

---

## 2. The invariant (I-27, generalized off serial)

On the secure-attention signal, the kernel guarantees:

1. **Input** goes only to corvus — no userspace program in the keystroke path.
2. **Output** the user sees is corvus's — no program can draw a fake prompt.
3. The transition is **forced by the kernel** on the user's signal; no userspace
   program can prevent it, fake it, or stay in the I/O path during it.

This is A-4c's I-27, lifted off the serial-specific substrate. The §28 invariant
text generalizes to "medium-independent"; the framebuffer enforcement (the trusted
sink + the key-combo scan + the renderer suspension) lands at impl, like every
reserved-then-enforced invariant.

---

## 3. The trust structure (kept from A-4c — with the owner/attach split honored)

- **The kernel owns the attention signal and the console state machine.** In
  `cons.c` the SAK is a BREAK — a line condition, explicitly *not data, not gated by
  termios* (the I-27 line-condition property must not be gated by a mode flag). The
  privileged transition is deferred to the `console_mgr` kproc (process context;
  `proc_console_sak` takes `g_proc_table_lock`).
- **On the SAK, `proc_console_sak` does exactly:** (1) revoke `PROC_FLAG_CONSOLE_ATTACHED`
  from the current owner; (2) set `g_console_owner = NULL`; (3) grant
  `PROC_FLAG_CONSOLE_ATTACHED` to the trusted authority (corvus, `g_console_trusted_proc`)
  — **never** making it the owner. Fail-safe: with no trusted Proc alive, the attach
  is simply not granted (no Proc can redeem elevation until a trusted login claims
  the console).
- **corvus is the sole trusted authenticator** (`g_console_trusted_proc`); it is the
  only Proc ever marked `PROC_FLAG_CONSOLE_ATTACHED` during a session.
- **The console-OWNER (Ctrl-C target) is a separate axis**, re-established when login
  spawns the session shell (`SPAWN_PERM_CONSOLE_OWNER`); during the login/episode
  window there is no foreground terminate target (`g_console_owner == NULL`).
- **Shells are never trusted and never console-attached.** They cannot impersonate
  the trusted path because they are never in it.

This structure does not change. Only the *signal* and the *output medium*
generalize (§§6, 7).

---

## 4. The correction: no shell ever "invokes" the SAK (ratified)

A natural-but-wrong model gives the shell a capability to invoke the SAK and pass
callbacks. The trust direction is backwards, and the reason is the whole point:

**The shell is the untrusted party the SAK protects the user *from*.** The SAK
exists so the user can forcibly yank the console *away* from whatever is running —
possibly a fake login or a fake fasces — and be *guaranteed* they reach corvus. If
"invoke SAK" were a capability userspace held, a malicious program could fire it to
make the user believe they are on the trusted path when they are not — exactly the
spoof the SAK defends against. And callbacks *into* the shell during the episode
would splice untrusted code into the trusted loop.

So: **the only invoker is the user's keypress, caught by the kernel.** The callback
instinct is not wrong — it belongs to the **renderer** (a one-way
kernel → renderer "enter/leave trusted mode" signal), never to the shell.

---

## 5. The roles (the interface)

- **Kernel**: owns SAK detection (per medium), the console-attach state machine + the
  forced grant to corvus, the episode generation and trusted-service binding (§7),
  and the `proc_is_console_attached` gate. Its renderer-facing
  surface is the thin **enter/leave-trusted** signal.
- **corvus**: the sole trusted authenticator. Runs every episode — login auth,
  imperium provincia + per-cap-key auth, the installer's credential mint. Produces
  **medium-independent content**; serial ANSI emission remains in Corvus.
- **Trusted display/input service**: owns graphical rasterization, hardware and
  physical input routing; an explicit TCB member under sections 7 and 8.
- **Renderer** (serial term / Aurora / Halcyon): honors enter/leave-trusted; on
  entry it is **fully suspended** (the strong model — it leaves the TCB entirely).
  Not in the trust loop, hence interchangeable.
- **Shell**: **no SAK capability.** It may *post an elevation request* (a hint that
  grants nothing) and *read* the `/proc/self` imperium flag to render the fasces.
  That is the entire shell surface.

---

## 6. The attention signal, per medium

- **Serial**: a PL011 BREAK — a line condition, kernel-caught, unforgeable by data.
  Exists today (`cons.c` `DR.BE`).
- **Graphical**: a reserved key-combo. AS BUILT it is either Control + either Alt
  + Delete **or F10** (the second final key because Delete is absent from compact
  and laptop keyboards), scanned by the KERNEL from the trusted input owner's
  key reports (`proc_seat_op` SEAT_INPUT; the codes are named in `seat.h`). No
  compositor, shell or theme takes part in deciding what attention is, which is
  also why the chord is not found by searching Halcyon's key maps. The trusted-path input
  device is either kernel-owned (the UART) or, on a board, a **trusted-tier system
  keyboard driver** (MENAGERIE.md §7 kernel-resident / system tier — never a
  third-party driver). That driver delivers raw HID events *through the kernel*,
  which owns the combo scan before any delivery to the console-attached corvus.

  **The honest residual**: a trusted-but-userspace keyboard driver sees every
  keystroke, including the passphrase typed to corvus — so it is a TCB member,
  minimal + audited like corvus. A compromised trusted driver is a keylog/DoS risk
  (it can withhold or fabricate input reports). The kernel alone grants trusted
  attachment, but cannot distinguish fabricated reports from real keys after a
  trusted input driver is compromised. The driver is therefore part of the TCB. (Pre-USB, the trusted path simply stays on serial.)

---

## 7. The output path: Corvus content, isolated trusted hardware service

**Approved refinement, 2026-09-18.** The operator approved the isolated trusted
display/input service in `GRAPHICAL-SAK-OWNERSHIP.md`, subject to portable backend
obligations in `GRAPHICAL-SAK-PORTABILITY.md`. This supersedes the former
kernel-only framebuffer rasterizer requirement. The service is an explicit TCB
member; ordinary Tapestry, Halcyon and Beacon are outside the trusted episode.
Corvus retains identity, policy, key verification and grant authority.

Boot policy binds the trusted seat and service identity. Hardware discovery supplies
resources, not runtime self-authorization. The service permanently owns presentation
and physical input, with a bounded broker for normal graphics. The kernel binds
Corvus and the service to an episode generation; secrets are accepted only after
exclusive display/input acknowledgement. No raw hardware ownership is lent back
to Tapestry. The logical contract is independent of PCI, virtio, HDMI and USB.

Corvus sends bounded semantic content. The service uses baked fonts and palette,
private trusted backing, and an immutable ordinary-frame snapshot or neutral field.
All planes, cursors, outputs, capture paths, DMA access and pending submissions
are part of the exclusion proof. A simple-framebuffer node or CPU mapping alone
is insufficient evidence. Backends must pass conformance tests before enablement.

**As built:** the serial episode gates UART output to the attached trusted
process. The QEMU graphical backend uses Lictor as the boot-trusted physical
GPU/input owner, with Corvus semantic frames and kernel generation/visibility
checks. Tapestry holds only the normal broker role. Grants remain held until
acknowledged restoration; owner failure cancels them before redemption. A failed
episode (a deadline, a refused device step, a malformed frame) cancels its grant
and then RECOVERS: the owner restores normal output and the kernel returns the
seat to normal, releasing nothing. A seat failure never closes a serial episode
-- it closes only the episode the seat itself opened. The
current backend uses the neutral field because private workspace capture is not
implemented. Serial authorization requires `thylacine.serial-sak=1`; the QEMU dev
launcher selects it unless `THYLACINE_SERIAL_SAK=0`.

Lictor cannot be assumed available during a kernel panic; graphical Halls output
needs a separate crash-ownership contract. Pi 400/Pi 500 remain unqualified.

---

## 8. The attention chain, not an uncopyable picture

Ordinary software can reproduce any dialog's appearance outside a trusted episode.
The graphical anchor is the physical attention gesture followed by kernel-bound
exclusive display/input routing. No screenshot, colour or logo proves authorization.
A trusted service compromise can observe keys and control pixels; boot firmware,
necessary non-isolated DMA drivers and physical input hardware are explicit trust
assumptions. QEMU additionally trusts its host and virtual device implementation.

Serial likewise trusts the host terminal and relies on BREAK, trusted attachment
and sole-writer enforcement. Section 11 limits interactive serial to dev/recovery.
Graphics failure cannot silently change that production policy.

---

## 9. The imperium flow: request, then SAK-confirm

This resolves the fasces tension with one flow, and the same path serves login and
the installer:

1. `imperium --request chown kill` → posts a **pending request** to corvus. Grants
   nothing; the shell is still untrusted.
2. **The user presses the SAK.** The kernel clears the owner + attaches corvus (§3).
   *This* is the dramatic toggle — driven by the user's keypress, never by the shell.
3. corvus, now the console-attached trusted authority, reads the pending request,
   displays the **provincia** (its cell grid → the kernel sink → the medium), and
   authenticates against the per-cap keys.
4. The **fasces** is then rendered by the shell from the *unforgeable* `/proc/self`
   imperium flag — read-only, so it can never draw more rods than are held. The
   prompt is the convenience mirror; the SAK + corvus episode is the truth
   (consistent with `IMPERIUM-DESIGN.md` §4).

---

## 10. The consumers

| Consumer | The episode does |
|---|---|
| **login** (A-5) | auth a principal; stamp the identity. login/UTOPIA are never console-attached — they authenticate *via* corvus. |
| **imperium** (A-4) | show the provincia; per-cap-key auth; confer the (fork-propagating) legate scope. |
| **installer** | mint the first hostowner credential + the user account (lighter — no prior trusted state to protect). |

One path; three consumers; every renderer.

---

## 11. Production posture: serial is dev/recovery, framebuffer is production (ratified)

The UART has two separable roles, and only one is an attack surface:

- **Diagnostic OUTPUT** (boot banner; a fatal Halls dump): kept even in production,
  but **silent-by-default / crash-only**. Output is not an attack surface, and a
  board whose display fails is otherwise a silent brick. (The `Thylacine boot OK` /
  `EXTINCTION:` tooling ABI is a dev-build concern.)
- **Interactive INPUT — a serial getty, the serial trusted path**: **off by default
  in a production image.** On a Pi the UART pins are on the GPIO header, so a serial
  login or a BREAK→corvus prompt is a physical-access attack vector.

So a **production image's trusted path is graphical-only, using the exclusive
trusted service path**. Interactive serial is confined to **dev / recovery**. This
is a per-image **posture flag** (a BSP/build setting), not silicon removal: dev/QEMU
= UART console + serial trusted path ON; production (display-equipped) = serial
interactive/trusted-path OFF, framebuffer-only, output crash-only.

**Caveat (flagged, not blocking)**: this assumes a **display**. A **headless**
appliance has no framebuffer, so its trusted path must be serial-hardened or
network — a distinct deployment posture. v1.0 production default = display-equipped
(the RPi-desktop/maker target); headless is a later posture.

Recovery composes: a forgotten passphrase (the FileVault-model disk key,
`INSTALLER.md` §7) is recovered by entering the A-5c phrase on the **framebuffer**
console — no serial needed on a display-equipped box.

---

## 12. Invariants + audit surface

**Composes / generalizes**: **I-27** (the trusted path — now medium-independent), the
A-4c console-attach machinery (`g_console_owner` / `g_console_trusted_proc` /
`PROC_FLAG_CONSOLE_ATTACHED` / `proc_console_sak`), the imperium `/proc/self` flag,
the MENAGERIE system-driver tier (the trusted keyboard). No new §28 *number* — I-27
generalizes; the framebuffer enforcement lands at impl.

**New surfaces to prosecute** (main-track, audit-bearing — the privilege spine of
login + elevation; join §25.4 at the sub-chunk that lands each):

- **The key-combo reservation** when input arrives via a userspace (trusted-tier)
  driver: the kernel owns the scan; a compromised trusted driver can DoS/keylog but
  cannot forge an episode (only the kernel grants console-attach to corvus).
- **The kernel trusted sink** (the cells→blit / cells→ANSI rasterizer): bounds, no VT
  parser, the medium bound once at boot; shares the framebuffer backend with Halls.
- **The enter/leave-trusted renderer signal** + full suspension: no renderer draws,
  maps the framebuffer, or reads input during an episode.
- **The posture gating**: the production flag actually disables serial
  interactive/trusted-path; output-only cannot be escalated to input.
- **The owner/attach discipline** (the reconciliation above): the SAK clears the
  owner + attaches corvus, never owns corvus — re-validate that no medium re-opens
  the RW-7 R2-F1 hazard.
- **The trusted EPISODE (IM-1, `IMPERIUM-DESIGN.md` §11.3 + its as-built
  refinements; LANDED 2026-09-07)**: on a SAK that finds the trusted Proc alive
  and ARMED as the episode consumer (`SYS_CONSOLE_EPISODE` = 110, op ARM; unarmed,
  a SAK is the A-4c-2 handoff alone, so the kernel never freezes a console nobody
  can unfreeze) the kernel opens an episode — ALL pending input discarded (the
  ring's committed lines too, not only the partial line), RAW/no-echo forced,
  every non-attached console read / write / poll / consctl write / renderer feed
  FROZEN (parked or refused until END; a frozen poller is not even woken per
  keystroke), the `sak` note posted to the trusted Proc; ended only by the
  trusted Proc's op END, or fail-safe by its death, its own relinquish, or a
  change of authority (NEVER a kernel timeout — an END behind corvus's back would
  route the secret to the shell); the pre-SAK console OWNER is handed back at
  END. Prosecute: a non-attached reader that drains a post-BEGIN byte; a
  non-attached writer that reaches the UART; a feed or consctl byte that lands;
  a poll that returns per keystroke; an END from a non-trusted caller; a repeat
  SAK that restarts the prompt mid-secret; a lost wake on the three new parks;
  the lock order (`g_proc_table_lock` -> `g_cons.lock` is the one new edge:
  BEGIN, END and every fail-safe close take the cons leaf lock under the table
  lock, and nothing takes them the other way round).

---

## 13. Thematic naming + the Halls kinship

Standard security terms stay (SAK, trusted path, I-27). Lex curiata names the
conferral ceremony; Corvus remains the authenticator. The approved trusted
service may share pure baked-font rendering code with a future Halls crash sink,
but not assume that a userspace GPU driver survives a kernel crash. Crash ownership
requires its own design, without two simultaneous hardware owners.

---

## 14. Dependencies / lane split

**Kernel owes (main-track):** physical-source admission, attention detection,
episode generations, trusted-service binding, secret routing gates and owner-death
revocation. Production posture must disable serial interaction independently of
whether graphical hardware succeeds.

**Trusted hardware service owes:** bounded ordinary graphics brokerage, private
trusted rendering and all portable backend obligations in
`GRAPHICAL-SAK-PORTABILITY.md`. This is approved work, not current enforcement.

**corvus owes (main-track userspace):** the medium-independent cell-grid trusted
renderer; the pending-request consumption (imperium); the per-cap-key auth.

**Renderer (Aurora):** honor enter/leave-trusted + full suspension.

---

## 15. Status

- **2026-06-15**: scripture adopted (this doc + the I-27 generalization note in ARCH
  §28 + IDENTITY-DESIGN §9.8 + CLAUDE.md). No code. The framebuffer trusted sink + the
  graphical SAK build with the Aurora renderer + the MENAGERIE board input path; the
  serial path is live today (A-4c).
- **2026-09-07 (IM-1 + IM-3, aux)**: the serial trusted path is BUILT end to end. The
  kernel EPISODE (§12; I-27 enforced on serial) + corvus as its consumer: the §9 flow
  runs as designed -- `IMPERIUM_REQUEST` posts the pending request, the BREAK opens
  the episode, corvus composes the provincia as the §7 cell grid and rasterizes it in
  userspace (ratified fork F1), reads the per-cap key on the frozen console, and
  registers the propagating grant. `usr/corvus/src/provincia.rs` is the composer the
  v1.x framebuffer sink consumes. `IMPERIUM-DESIGN.md` §11.3 / §11.5 carry the
  as-built refinements.
- **2026-09-07**: the IM phase opened (aux; `IMPERIUM-DESIGN.md` §11). The serial
  EPISODE is IM-1; fork F1 ratified the serial sink as the kernel exclusivity gate
  + userspace rasterization (the §7 refinement); the `sak` note (F3) +
  `SYS_CONSOLE_EPISODE_END` ride the same signoff. The framebuffer sink + the
  graphical SAK stay v1.x.
