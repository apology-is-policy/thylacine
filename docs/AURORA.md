# AURORA.md — the textual environment, and the glowing surface it renders on

**Binding scripture.** Adopted 2026-06-15 (the aux architecture session;
user-ratified). Carries the **VISION-level statement**: *Thylacine ships two
first-class user environments — **Aurora** (textual) and **Halcyon** (graphical) —
and the user chooses.* Aurora is a complete, designed, shippable environment in its
own right, not a Halcyon precursor.

The renderer rides **Tapestry** (`docs/TAPESTRY.md`, scripture — the Loom-woven
framebuffer present-path, T-1 no-torn-scanout) over a framebuffer (`simplefb` /
virtio-gpu); the environment is native `libthyla-rs`. Depends on:
`docs/TRUSTED-PATH.md` (the SAK suspension hook), `docs/MENAGERIE.md` (the scanout +
USB-keyboard drivers), `docs/INSTALLER.md` (the environment-choice screen), Kaua
(`docs/KAUA.md`, the app-side TUI lib), Utopia (the shell). Cross-refs: VISION §1.2
+ §3.3 (the two-environments identity), ARCH §17 (Halcyon, the graphical sibling),
NOVEL.md #4.

---

## 1. Thesis: two environments, and Aurora is the one that *is* the thesis

Thylacine's founding conviction is **"the shell is sufficient as a UI"** (VISION
§1.2). Aurora + Utopia is the purest expression of it: a complete, beautiful user
environment that is *only* the shell — no window system, no compositor, no
concession. Aurora is not a stopgap before Halcyon; it is a **first-class, permanent
sibling** — arguably *more* on-mission than Halcyon, which is the "we can also do
graphics" environment.

Two consequences make this strategically load-bearing, not just elegant:

- **It de-risks the entire ship.** Halcyon is the highest-risk, last phase; ROADMAP
  §10/§11 already keeps v1.0-rc.1 as the textual fallback. Naming that fallback **the
  Aurora environment** gives it a real product identity — a complete, designed thing
  that ships whether or not Halcyon ever lands. The riskiest phase stops being
  load-bearing for a beautiful v1.0.
- **It serves permanent audiences**: servers + headless-with-a-screen, low-end
  boards (or any board whose GPU driver lags), power users, and the
  security-conscious (no compositor = a smaller TCB + an even cleaner trusted path).
  Many people want a great text environment *forever*.

---

## 2. "Aurora" names two things

Like a desktop-environment name, **Aurora** denotes both:

- **the renderer** — the glowing surface that turns text (and a little Sixel) into
  pixels on a real screen (§3), and
- **the environment** — that renderer plus session multiplexing, a status surface,
  and the Utopia + native-TUI suite it hosts (§5).

The renderer is the smaller, more mechanical half; the environment is what the user
actually lives in.

---

## 3. The renderer — the glowing surface

A native `libthyla-rs` program that owns a framebuffer and paints a text grid into
it:

- **Surface**: presents via **Tapestry** (the Loom-woven framebuffer fast-path, T-1
  no-torn-scanout). At bring-up the framebuffer is the firmware-provided **`simplefb`**
  (RPi mailbox) or **virtio-gpu** (QEMU) — a linear buffer, so pixels work on day one
  with *no GPU driver*.
- **Font**: a **baked rasterized bitmap font** (a PSF-style glyph atlas compiled in)
  — crisp, fast, no runtime TrueType/FreeType dependency, `no_std`-clean. A
  Thylacine-custom face can ship for the Bonfire identity.
- **VT parser**: Aurora is the *screen-side* of the terminal protocol — it
  **interprets** the VT escape stream (cursor moves, SGR colour) and rasterizes the
  resulting **cell grid** to pixels. (Kaua is the *app-side*; §4.)
- **Sixel** (§8): raster-in-text, for the installer's identity mark and themed accents.

The cell grid (rows × cols of glyph+attr) is the rendering core; everything else is a
producer (the VT parser) or a consumer (the blit) of it. Note the kinship with the
**trusted sink** (`TRUSTED-PATH.md` §7) and **Halls** — three places the system
rasterizes a cell grid to a framebuffer (an episode / a crash / the live environment);
they should share the baked-font blit.

---

## 4. The swappable `/dev/cons` backend, and the Kaua boundary

The terminal protocol has two ends, and naming them separates the concerns:

- **Kaua** is the *app-side* TUI library: the installer, Utopia, nora *emit* a VT
  escape stream.
- **Aurora** is the *screen-side* renderer: it *interprets* that stream into pixels.

They meet at the VT-escape-stream **wire**, which is exactly the boundary `/dev/cons`
already is (the #57b `devdev` console + the LS-8 line discipline). So **`/dev/cons`
becomes a swappable-backend device** (the Plan 9 cons-served model): UART-backed on
serial/QEMU (the host terminal interprets), or **Aurora-backed** on a graphical board
(Aurora interprets + blits). The shell writes `/dev/cons` and never knows the
difference — which is why the installer and Utopia run on Aurora *unchanged*. The
swappable backend is the kernel-owed plumbing (`cons.c` gains a backend selector
bound at boot from the DTB medium fact, like the trusted sink); the existing
UART-backed `/dev/cons` is the serial backend.

---

## 5. The environment — the first-class layer

A bare terminal only has to draw one shell's text. A first-class *environment* on a
single framebuffer wants more — the scope bump the "sibling to Halcyon" framing earns:

- **Session multiplexing / virtual-terminal switching** — multiple Utopia sessions
  (and full-screen TUI apps: nora, a file browser) on one framebuffer, switched by a
  reserved chord (the Ctrl-Alt-Fn / tmux instinct). The reserved chord must *not*
  collide with the SAK combo (§6).
- **A status surface** — a thin band: the session list, the clock, identity, and
  (under imperium) the fasces, read from the unforgeable `/proc/self` flag.
- **Session + launch management** — start/switch/close sessions; spawn the Utopia
  shell + the TUI suite; the login → session-leader hand-off (A-5).

This is the "GNOME/KDE" half of the name — Aurora-the-environment is what login drops
you into, and what the installer offers as a choice.

---

## 6. The trusted-path hook (already pinned)

Aurora is a renderer, so the trusted path treats it like any other: `TRUSTED-PATH.md`
§5/§7 pin it — on a SAK episode Aurora is **fully suspended** (the strong model), the
kernel takes the framebuffer and paints corvus's cells itself, and Aurora resumes on
leave. Aurora is *below* the mechanism, never in the trust loop. Two obligations fall
on Aurora specifically: its session-switch chord must not shadow the SAK combo (the
kernel owns the SAK scan regardless, but the UX must not invite confusion), and it
must honor enter/leave-trusted promptly. This is the one security-load-bearing
obligation on Aurora.

---

## 7. Two lights — the relationship to Halcyon

**Aurora is the crisp first light** (minimal, textual, keyboard-driven); **Halcyon is
the full calm day** (rich, graphical, media). Both are names for serene, beautiful
times — the dawn, the halcyon days — so the choice is "which light," not "the
cut-down one vs the real one." Concretely:

- **Shared substrate**: both run on the same Tapestry present-path, the same
  framebuffer, the same Menagerie drivers, the same trusted path. Aurora is most of
  the way to Halcyon's *plumbing* already.
- **Continuity, not replacement**: when Halcyon lands, it **hosts Aurora terminals as
  windows** — Aurora-the-renderer becomes a pane inside Halcyon. Aurora does not
  disappear under the day; it becomes part of it.
- **The choice** is offered at install (the `INSTALLER.md` environment screen) and
  switchable later. At a v1.0 where Halcyon is deferred, Aurora is simply *the*
  environment — proper, not a fallback.

---

## 8. Sixel — the pre-Halcyon raster bridge

Sixel lets the textual environment emit basic raster (the installer identity mark,
themed accents) without a window system. It is portable: Aurora renders it on a
board, and a Sixel-capable host terminal renders the *same bytes* in QEMU — with a
**capability-detect** (the DA handshake Kaua already does for size) + an
ASCII/Unicode **fallback** for non-Sixel host terminals. It is explicitly the
*textual-era* bridge: Halcyon brings the real `/dev/draw` compositor; Sixel just
gives Aurora a themed mark in the meantime.

---

## 9. Bring-up / proving

- **QEMU first** (the zero-hardware-risk proving ground): virtio-gpu (output) +
  virtio-input (keyboard) both exist today, so the whole renderer + environment proves
  out in QEMU before any board.
- **Real board**: *output* is easy — the firmware's `simplefb` linear buffer. *Input*
  is the gate — a USB keyboard needs the Menagerie USB source (a larger driver). So
  output and input desync naturally with the driver work; a board can show Aurora
  (output) before it accepts a USB keyboard (input), and serial covers input in the
  interim.

---

## 10. Invariants + surface

- **T-1 (no torn scanout)**: Aurora presents through Tapestry, inheriting the
  invariant (TAPESTRY.md §6, reserved-then-enforced); cell updates land atomically per
  frame.
- **Trusted-path renderer obligation** (`TRUSTED-PATH.md`): Aurora must honor full
  suspension on a SAK episode and must not map the framebuffer or read input during
  one. The one security-load-bearing obligation on Aurora.
- **PTY (I-20) relationship**: the *console* case is cons-served (Aurora serves
  `/dev/cons`), which does not need the full PTY master/slave path; true PTYs (for a
  terminal multiplexer running arbitrary programs) remain the Phase-8 I-20 surface.
  Aurora's session multiplexing (§5) is cons-level, not PTY-level, at v1.0 — note the
  boundary.
- Aurora is otherwise **not itself a large security surface** — it is a renderer + an
  environment; its TCB-relevant edges (the trusted keyboard driver, the SAK) live in
  the trusted-path + Menagerie docs. **No new §28 invariant.**

---

## 11. Naming rationale (locked)

**Aurora** = the **first light** before Halcyon's full day; **aurora australis**, the
**southern** light — Tasmania's own sky, the thylacine's home; and literally a
**glow**, which is what a screen is. It pairs with **Halcyon** (the calm day) as two
names for serene, beautiful times — the dawn and the halcyon days — so the
two-environment choice reads as "which light." (User-chosen, LOCKED.) Sub-part names
(the multiplexer, the status band) stay plain or are held.

---

## 12. Dependencies / lane split

**Userspace (native `libthyla-rs`):**
1. The **renderer**: the cell grid, the baked font + blit, the VT parser, the Sixel
   decoder + capability-detect/fallback.
2. The **environment**: session multiplexing / VT switching, the status surface,
   session + launch management.
3. The Tapestry-client present path (the `RingLoom` shape in `libtapestry`).

**Kernel / main-track owes:**
4. Scanout access: `simplefb` (firmware framebuffer) + the virtio-gpu / real GPU
   driver via `MENAGERIE.md`.
5. The **USB keyboard** (trusted-tier) source/driver (Menagerie) — the board input
   gate.
6. The `/dev/cons` swappable-backend plumbing (Aurora-backed vs UART-backed; the
   backend bound at boot from the DTB medium fact).
7. The trusted-path **enter/leave-trusted** signal + full-suspension handling
   (`TRUSTED-PATH.md`).
8. **VISION/ROADMAP ratification** of the two-environments statement + Aurora as a
   named, shippable deliverable (this commit).

**Consumers**: the installer (renders on Aurora; offers the environment choice),
login (drops into the Aurora environment), Utopia + the native TUI suite (run on it
unchanged).

---

## 13. Status

- **2026-06-15**: scripture adopted (this doc + the VISION two-environments statement
  §1.2/§3.3 + ARCH §17 + ROADMAP + NOVEL.md #4). No code. The renderer + environment
  build on Tapestry + the Menagerie scanout/USB-keyboard drivers + the trusted-path
  hook + the `/dev/cons` swappable backend; QEMU (virtio-gpu + virtio-input) is the
  zero-hardware-risk proving ground.
