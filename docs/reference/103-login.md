# 103 — login + the boot->session transition (A-5a) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-login-doc-absorb`).
The live login session: the native `/sbin/login`, the three boot->session
transition syscalls, joey's transformation from a one-shot boot-test harness into
the long-running session supervisor, and the per-user encrypted home. Its content
lives, code-verified and current, in:

- **`/sbin/login` + the per-user encrypted-home DEK lifecycle + the home bind +
  the `!recover` UX** — login never holds a raw DEK (it forwards only the opaque
  33-byte token), the conn-bound `install-dek` lease, and the home served by a
  **second stratumd run as the user**:

      vault/system/stratum/sub-stratum-session.md
      (title: "The per-user encrypted home — a second stratumd, and the DEK's
       lifetime"; owns usr/login/src/main.rs)

- **joey the session supervisor** — the kernel half (the console trust root of the
  login chain, the console-attached/owner root, the SAK, the one-call guard) and
  the userspace half (pivot-root, stratumd + corvus bringup, the getty loop that
  re-spawns `/sbin/login`):

      vault/system/kernel/boot/sub-kernel-joey.md       (kernel half)
      vault/system/stratum/sub-stratum-boot.md          (userspace supervisor)

- the **three boot->session syscalls** — `SYS_CONSOLE_OPEN`=64 and
  `SYS_CONSOLE_RELINQUISH`=63 (the I-27 console-attach front door / the
  bringup->session relinquish), and `SYS_BOOT_COMPLETE`=62 (the one-shot,
  console-attached gate that prints the banner):

      vault/system/kernel/console-gfx/sub-kernel-cons.md
      vault/system/kernel/console-gfx/sub-kernel-devdev.md
      vault/system/boundary/registries/abi-boot-banner.md   (the boot-complete banner)

- the **passphrase recovery** (corvus's `!recover` / `RECOVER` verb):

      vault/system/userspace/services/sub-corvus.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold.** The login mechanism (the DEK
  never held by login, the second-stratumd-as-user, the conn-bound lease) is in
  `sub-stratum-session`; joey's supervisor role splits kernel/userspace across
  `sub-kernel-joey` / `sub-stratum-boot`; the three syscalls are the console
  front-door (I-27) and the boot-complete banner.
- **The content is distributed** across the dossiers above; the I-27 trusted-path
  invariant is `sub-kernel-cons`'s.
