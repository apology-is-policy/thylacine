# INSTALLER.md — the founding: laying Thylacine onto a disk

**Binding scripture.** Adopted 2026-06-15 (the aux architecture session;
user-ratified). The fourth and last of the architecture-suite adoptions
(Menagerie → trusted-path → Aurora → installer). The installer **mints the system
root-of-trust** — the single most security-load-bearing first-run in the OS — so it
is **audit-bearing** (§11; its root-of-trust-mint surface joins ARCH §25.4 at impl).

Builds on the COMPLETE host-bake (`tools/build.sh::build_stratum_pool_fixture` +
`corvus-mint`, A-5c-b), the Stratum pool/dataset surface, the corvus identity arc
(A-1..A-5c — the system identity + per-user DEK + the BIP-39 recovery phrase), the
A-4 clearance/legate model (I-2/I-25), `SYS_PIVOT_ROOT`, and the driver framework
(`docs/MENAGERIE.md`, to reach a target disk). Renders on **Aurora** (`docs/AURORA.md`
— the textual environment that superseded the working name "vitrine"). Native
`libthyla-rs` — the installer is **policy over existing mechanisms, never a kernel
concern**.

---

## 1. Thesis: the host-bake, promoted to a runtime program

Thylacine already has an "installer": the host-side bake that runs `stratum-mkfs`,
populates a fresh pool with the system corpus, and mints the corvus identity
(`corvus-mint`, A-5c-b) — the thing that makes today's bootable `pool.img`. Scripture
already names the runtime successor: "a Thylacine `/sbin/installer` that runs in an
alternate boot path would prove the same flow via the runtime 9P write surface."

So the installer is **not a new mechanism — it is the bake promoted from build-time
to a runtime userspace program**, driving the *same* Stratum + corvus code paths
from inside Thylacine. One install-core; the only genuinely new surface is the
front-end (a TUI) and the live environment that hosts it.

It carries unusual weight: **the installer is the first thing anyone ever sees of
Thylacine.** It must be beautiful, rich, and speak one coherent design language (the
Bonfire palette, the thylacine/specimen identity). The first impression is a product
requirement, not a nicety (§§9, 10).

---

## 2. The framing: if it boots into a working system, the pool already exists

The intuition "we boot a working system but cannot mount a disk to make accounts"
conflates two deployment shapes — and the disambiguation IS the design:

- **A pre-baked image.** The medium carries a boot partition (firmware + kernel + the
  universal DTB set) AND a Stratum pool. It boots to a working system. What is
  missing is not a disk — it is a **user account provisioned in the pool that is
  already there.** The fix is **first-boot personalization**, not a separate
  installer. (The rpi-imager / Fuchsia-paver model.)
- **A live / generic medium installing onto a *different* disk** (the Pi's NVMe, a
  USB SSD). Now there is genuinely no target pool, and you want the **classic TUI
  installer** that partitions + formats + populates a target. (The FreeBSD
  `bsdinstall` model.)

Both are real; they are the same install-core invoked two ways.

---

## 3. The install core (one program, three callers)

Given a target block device, the core performs **the founding**:

1. **Lay down a Stratum pool** (`stratum-mkfs` over the target, reached through the
   driver framework — SDHCI / NVMe / USB / virtio-blk).
2. **Populate the system corpus** (joey, corvus, stratumd, the coreutils, Aurora,
   UTOPIA, ...) — the same Twrite/Tcreate path the host-bake uses today.
3. **Provision corvus identity**: the system identity (the hostowner root-of-trust),
   the first user account (keypair + encrypted home dataset + sealed DEK), and
   **surface the BIP-39 recovery phrase** for the user to record (A-5c mandatory
   enrollment).
4. **Set up clearance / legate** (the *cursus honorum*: which principals are eligible
   for which clearances + imperium — the hostowner credential is born here).
5. **Write the boot artifacts** (the boot/firmware partition: firmware + kernel +
   DTBs + the pool-unlock wiring).

This exact sequence already exists as the host-bake. The three callers differ only in
*when* and *which disk*: the **host bake** (build time, the QEMU/flashable image), the
**first-boot wizard** (runtime, the pool you booted from), and the **live installer**
(runtime, a separate target disk).

---

## 4. The two runtime shapes — pick the default per medium

- **SD / Pi → pre-baked image + a first-boot wizard** (the default). The universal
  image carries everything *except* the per-user secrets (you cannot bake someone's
  passphrase or recovery phrase at the factory). First boot detects "no user yet" and
  runs the wizard: create the first user, set the hostowner credential, display the
  recovery phrase, provision the encrypted home, set clearance eligibility. Then it is
  a normal installed system.
- **NVMe / USB / internal disk → the live-environment TUI installer** (the power path).
  Boot a live medium, run the installer, it sets up a separate target disk, reboot into
  it.

Same install-core; the difference is only which disk and whether a target pool already
exists.

---

## 5. The live environment is nearly free

It is **devramfs — the pre-pivot bootstrap state — with the installer as `init`
instead of `login`.**

```
  normal boot:     devramfs -> pivot to disk-Stratum (SYS_PIVOT_ROOT) -> login
  installer boot:  devramfs -> (no target pool) -> installer -> creates disk-Stratum -> reboot -> normal boot
```

The installer literally **creates the thing that normal boot pivots into.** No new
boot mechanism — it is the existing pre-pivot state, made a destination rather than a
transient. The live image carries the installer + stratumd + corvus + the driver
framework in the initramfs.

---

## 6. Stratum collapses "partitioning"

Stratum is a pool/dataset filesystem (ZFS-like), so the installer does not
partition-and-format-many. The disk step is usually just a small **boot/firmware
partition** (FAT, for the Pi firmware + kernel + the universal DTB set) + **one
Stratum pool partition.** Per-user structure (homes) are *datasets inside the pool*,
minted by corvus as accounts are created. The "partitioning" screen is therefore
small — pick the disk, confirm the wipe, done.

---

## 7. The disk root-of-trust — passphrase-derived (resolved, ratified)

**The pool is unlocked by the hostowner passphrase at boot (the FileVault / LUKS
model), via corvus's system identity** — no separate `.key` sidecar to lose. The
chain reuses A-5c whole:

```
  hostowner passphrase -> Argon2id KEK -> unwrap the corvus system identity -> unseal the pool DEK -> mount Stratum -> login
```

- **Convenient + single-secret**: one passphrase, no key medium to carry or lose.
- **Honest tradeoff**: an attacker who images the disk can offline-attack the
  passphrase; Argon2id is the mitigation (the same preset A-5c already uses).
- **The recovery phrase does double duty**: the A-5c BIP-39 phrase (surfaced at
  account creation) is now *also* the "forgot the disk passphrase" backstop — one
  enrollment, two protections.
- **High-security option** (offered, not default): the two-factor `.key` on a separate
  removable medium (the Stratum `OS-INTEGRATION.md` model). **TPM / secure element**: a
  documented post-v1.0 seam (RPi5 has none by default).

**Stratum-side coordination (main-track, escalation-flagged):** the boot path unlocks
the pool from the passphrase-derived, corvus-sealed key rather than a baked sidecar —
a change to how the `.key` is **provisioned + presented to stratumd at mount**. Per
the Stratum coordination rules, whether this is an on-disk-format / `.key`-ABI break
(escalation-worthy) vs an ordinary provisioning change is adjudicated at impl, against
`stratum/v2/docs/OS-INTEGRATION.md §4`. The on-disk *envelope* already exists (A-5c
seals the system identity); the change is the *unlock path*, not necessarily the
format.

---

## 8. The first-boot wizard — the concrete flow

```
  welcome           Thylacine, the thylacine, the Bonfire palette; a Sixel identity mark (section 10)
  your account      username, passphrase (the hostowner + first user; the boot-unlock secret, section 7)
  recovery phrase   display the 24-word BIP-39 phrase; CONFIRM written (A-5c mandatory enrollment; the disk backstop)
  clearances        optional: eligibility for legate / imperium (the cursus honorum)
  founding          a PROGRESS view: mkfs -> populate -> provision -> seal
  done              reboot into your system
```

The wizard is short by design (Stratum and corvus do the heavy lifting); its job is
the *secrets that cannot be baked* + the first impression. The progress view is where
beauty matters most — a framed, paced "founding" with real per-step feedback, not a
spinner.

---

## 9. Beauty + the design language

The installer is the showcase of the Bonfire design language (UTOPIA-VISUAL) and the
Thylacine identity. It needs a richer widget vocabulary than a bare cell grid: a
framed dialog/popup, a progress bar/gauge, list/menu/radio, a masked text input, and
**Sixel** raster for the identity mark. This is a **Kaua widget-layer expansion**
(`docs/KAUA.md`) — the installer is the forcing function that grows the widget set the
rest of native Thylacine (UTOPIA, nora, future TUIs) then inherits.

---

## 10. Rendering — how the installer reaches a screen

A TUI needs a terminal to render into:

- **QEMU / dev**: the installer renders over the UART (serial) to the host's terminal
  emulator — a terminal is free.
- **Real board**: there is no host terminal. The OS renders the console *itself* —
  this is **Aurora** (`docs/AURORA.md`): a userspace program that owns a framebuffer
  (via Tapestry; or, at bring-up, the firmware `simplefb` + virtio-gpu on QEMU),
  rasterizes a baked bitmap font, interprets the VT escape stream, and feeds keyboard
  input back. It serves the console (`/dev/cons`-served), so the installer and UTOPIA
  run on it unchanged, and supports Sixel (portable: Aurora renders it on a board, a
  Sixel-capable host terminal renders the same bytes in QEMU, with a DA-handshake
  capability-detect + ASCII fallback). ("vitrine" was the working name for this
  renderer; superseded by Aurora.)

---

## 11. Invariants + security surface (audit-bearing)

The installer **mints the system root-of-trust** — the single most
security-load-bearing first-run. Prosecute (at impl):

- **The hostowner credential is born correctly**: the passphrase → Argon2id →
  system-identity → pool-DEK chain (§7) seals with no weak-RNG, no predictable salt,
  and the secret hygiene A-5c already demands (mlock + `explicit_bzero` on every path,
  including error).
- **No secret is baked**: the per-user passphrase + recovery phrase are user-entered
  at first boot, never present in the image (verifiable: the image is reproducible and
  contains no per-user key material).
- **The recovery phrase is shown exactly once, never persisted** (A-5c **C-27**), and
  confirmed-recorded before the founding completes.
- **Idempotency / crash-safety**: the founding is a multi-phase durable write
  (mkfs → populate → provision → seal); a crash at any phase must leave either a
  clean retry-able state or a complete pool — never a half-sealed pool that bricks
  (the crash-injection discipline; rename-swap + fsync barriers — CLAUDE.md
  "Crash-injection + fault-injection testing").
- **Composes, invents no new invariant**: corvus **C-20/C-27/C-28** (identity + the
  no-escrow recovery), the A-4 clearance model (**I-2/I-25**), Stratum integrity
  (**I-14**), the driver-framework allowance (**I-34**, to reach the disk).

The root-of-trust-mint audit surface (the founding orchestration; the DEK seal; the
crash-safety of the multi-phase write) joins ARCH §25.4 at the sub-chunk that lands it.

---

## 12. QEMU proves it first

The entire loop — partition → mkfs → populate → corvus-provision → seal → write boot
artifacts → reboot into the installed system — runs in QEMU against a virtio-blk
target image *before any SD card*, with zero hardware risk. The live environment is
devramfs (already the dev boot's pre-pivot state); the target disk is a second
virtio-blk drive. Same proving-ground discipline as the rest of the suite.

---

## 13. Thematic naming (proposed)

The installer performs **the founding** — laying the system onto a fresh disk (the
Roman *colonia* sense, or the museum *accession* of a specimen into a collection —
ties to the holotype theme). Load-bearing terms stay plain (`/sbin/installer`,
`mkfs`, `provision`); the color is **the founding** for the act. (The renderer is
**Aurora**, no longer "vitrine".)

---

## 14. Dependencies / lane split

**Userspace (native `libthyla-rs`):**
1. The **install-core** (the founding orchestration over Stratum + corvus; the
   host-bake's runtime twin).
2. The **first-boot wizard** + the **live installer** TUIs.
3. The **widget expansion** (§9) — the Kaua widget growth.

**Kernel / main-track owes:**
4. The driver framework to reach a real target disk (`MENAGERIE.md`: SDHCI/NVMe/USB
   sources + drivers).
5. The **boot-medium assembly** (the universal image: boot partition + DTBs + the live
   initramfs) — build/tooling.
6. The **Stratum-side pool-unlock change** (passphrase-derived, corvus-sealed key at
   mount, §7) — Stratum coordination (escalation-flagged at impl).
7. Aurora's display path on real boards (`simplefb` / scanout) + the
   trusted-path-on-graphical-console reconciliation (`AURORA.md` + `TRUSTED-PATH.md`).

corvus identity, the recovery keyslot, clearance/legate, `SYS_PIVOT_ROOT`, and the
host-bake all already exist — the installer is their runtime front-end.

---

## 15. Status

- **2026-06-15**: scripture adopted (this doc + the ROADMAP first-boot entry + the
  §28 reserved audit-surface note). No code. The install-core builds on the live
  host-bake + corvus A-5c + SYS_PIVOT_ROOT; the front-end builds on Aurora + the Kaua
  widget expansion; QEMU (a virtio-blk target + devramfs live env) is the
  zero-hardware-risk proving ground. **This completes the four-doc architecture
  suite** (Menagerie + trusted-path + Aurora + installer); the build arc is next.
