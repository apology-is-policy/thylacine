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

## 5. The four roles (the interface)

- **Kernel**: owns SAK detection (per medium), the console-attach state machine + the
  forced grant to corvus, the medium-aware trusted **sink** (§7), the unforgeable
  indicator (§8), and the `proc_is_console_attached` gate. Its renderer-facing
  surface is the thin **enter/leave-trusted** signal.
- **corvus**: the sole trusted authenticator. Runs every episode — login auth,
  imperium provincia + per-cap-key auth, the installer's credential mint. Produces
  **medium-independent cell-grid content**; never touches a pixel or a UART.
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
- **Graphical**: a reserved key-combo (Ctrl-Alt-Del-class). The trusted-path input
  device is either kernel-owned (the UART) or, on a board, a **trusted-tier system
  keyboard driver** (MENAGERIE.md §7 kernel-resident / system tier — never a
  third-party driver). That driver delivers raw HID events *through the kernel*,
  which owns the combo scan before any delivery to the console-attached corvus.

  **The honest residual**: a trusted-but-userspace keyboard driver sees every
  keystroke, including the passphrase typed to corvus — so it is a TCB member,
  minimal + audited like corvus. A compromised trusted driver is a keylog/DoS risk
  (it can withhold the combo → the SAK does not respond, which is *detectable*, not a
  spoof) — but it can **never forge a trusted episode**, because only the kernel
  grants console-attach to corvus. (Pre-USB, the trusted path simply stays on serial.)

---

## 7. The output path: corvus produces cells, the kernel's sink rasterizes

The medium is a **DTB boot fact** (the same I-15 view `cons.c` already uses for
`arm,pl011`; the graphical case adds a `simple-framebuffer` / virtio-gpu node). It
is bound once at boot — there is no runtime "am I serial or graphical" branch in the
SAK path. (Binding refinement 2026-07-17, TAPESTRY.md §18.7: the framebuffer episode
binds only to KERNEL-REACHABLE linear framebuffers — simplefb-class; a
virtio-gpu-only medium (QEMU) keeps the SERIAL trusted path, which QEMU always has —
the kernel cannot paint through a userspace-owned GPU without violating ARCH §17.2's
no-graphics-in-kernel posture.) The trusted output is a medium-aware **sink** with two backends:

```
  corvus --(medium-INDEPENDENT cell grid: rows x cols of glyph+attr)--> kernel trusted sink
                                                                         |- UART backend       -> bytes / ANSI (cursor + SGR)
                                                                         |- framebuffer backend -> blit baked-font glyphs
```

- **corvus** does *layout* (it knows the content — the provincia, the fasces, the
  password field) and emits a **cell grid** that has no idea of the medium.
- **The kernel's trusted sink** does *rasterization*, picking the backend bound at
  boot. On UART it emits ANSI/bytes; on a framebuffer it blits a baked font.

Using a cell grid (not corvus-emitted ANSI) is deliberate: it keeps a VT-escape
*parser* out of the kernel — the kernel only ever rasterizes cells.

**Why this is small, and pays for itself twice.** The kernel already renders text to
the UART (the boot banner; the Halls-of-Extinction crash dump) — the UART backend is
essentially what exists. The new piece is the framebuffer backend (a baked font + a
cell→pixel blit), and it is the *same* sink a **graphical Halls dump** needs (a board
with no serial must still surface a panic). Both are the one problem: *the kernel
must put trustworthy text on the medium when userspace is untrusted (an episode) or
dead (a crash).*

This realizes the strong model: during a framebuffer episode **no userspace maps the
framebuffer at all** — the renderer is suspended, corvus hands only cells, the kernel
is the sole painter.

---

## 8. The unforgeable indicator + the serial asymmetry (stated honestly)

- **Framebuffer**: the kernel owns the pixels, so the indicator is *real* — a
  kernel-drawn band the renderer cannot reproduce.
- **Serial**: the host terminal is outside our trust boundary, so no on-screen
  indicator can be truly unforgeable (any program can print "TRUSTED"). The anchor
  there is the chain itself: the user pressed BREAK → the kernel attached corvus →
  corvus is the kernel-guaranteed *sole writer* → the bytes you see are corvus's.
  Sound *if* you trust your own serial terminal, which for serial-console admin you
  do.

Two threat models, both honest. §11 confines the weaker one to dev/recovery.

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

So a **production image's trusted path is framebuffer-only — the strong, unforgeable
one** — and the serial-output asymmetry (§8) is confined to **dev / recovery**. This
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

---

## 13. Thematic naming + the Halls kinship

Standard security terms stay (SAK, trusted path, I-27 — readers expect them). One
kinship worth recording: the **kernel trusted sink** is the sibling of **Halls of
Extinction** — both are the kernel rendering trustworthy text on a machine whose
userspace cannot be trusted (an episode) or is dead (a crash). They should share the
framebuffer text backend (the baked font + the cell/glyph blit).

---

## 14. Dependencies / lane split

**Kernel owes (main-track):**
1. The graphical SAK key-combo detection (the trusted-tier keyboard event path + the
   kernel-owned scan); the serial BREAK exists.
2. The medium-aware **trusted sink** (UART backend ~= exists via banner/Halls; the
   framebuffer baked-font blit is new, shared with a graphical Halls).
3. The unforgeable framebuffer indicator; the enter/leave-trusted renderer signal;
   the medium binding at boot (DTB).
4. The production **posture flag** (serial interactive/trusted-path disable) —
   build/BSP tooling.

**corvus owes (main-track userspace):** the medium-independent cell-grid trusted
renderer; the pending-request consumption (imperium); the per-cap-key auth.

**Renderer (Aurora):** honor enter/leave-trusted + full suspension.

---

## 15. Status

- **2026-06-15**: scripture adopted (this doc + the I-27 generalization note in ARCH
  §28 + IDENTITY-DESIGN §9.8 + CLAUDE.md). No code. The framebuffer trusted sink + the
  graphical SAK build with the Aurora renderer + the MENAGERIE board input path; the
  serial path is live today (A-4c).
