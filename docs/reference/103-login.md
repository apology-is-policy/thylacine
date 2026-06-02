# 103 -- login + the boot->session transition (A-5a)

As-built reference for the live login session: the native `/sbin/login`, the
three boot->session-transition syscalls, and joey's transformation from a
one-shot boot-test harness into the long-running session supervisor.

Design: `IDENTITY-DESIGN.md` section 9.9. Audit-trigger row: `ARCHITECTURE.md`
section 25.4 (A-5). Landed: A-5a-alpha (`97a3af5`, kernel substrate) +
A-5a-beta (login binary + getty-loop).

## Purpose

A-5a wires A-1 (identity) + A-2/A-3 (rwx + per-user 9P) + A-4 (clearance / kill /
trusted path) into the user-facing login flow. The boot shape changes from
"joey runs boot tests, exits, the kernel prints the banner" to "joey is the
long-running init that getty-loops `/sbin/login`." Because joey no longer exits
on success, the `Thylacine boot OK` banner can no longer ride its reap -- so the
kernel gains a boot-complete handshake, and joey relinquishes its boot
console-attach so corvus stays the sole console-attached Proc during a session
(I-27).

## Public API -- the three syscalls

```
SYS_BOOT_COMPLETE      = 62   // x0 -> 0 / -1
SYS_CONSOLE_RELINQUISH = 63   // x0 -> 0 / -1
SYS_CONSOLE_OPEN       = 64   // x0 -> fd / -1
```

- **`SYS_BOOT_COMPLETE`** -- init signals its boot-test asserts passed; the
  kernel prints `Thylacine boot OK` via `boot_mark_complete()`. One-shot (atomic
  exchange; a 2nd call is a no-op) + gated on the caller being console-attached,
  so a spawned child cannot emit a premature banner (-> a false test PASS). No
  args. `kernel/syscall.c::sys_boot_complete_handler`; banner in
  `kernel/main.c::boot_mark_complete` (the `Thylacine boot OK` string lives
  there).
- **`SYS_CONSOLE_RELINQUISH`** -- the caller drops its OWN
  `PROC_FLAG_CONSOLE_ATTACHED` and, if it is `g_console_owner`, clears the owner
  pointer. Self-only (the handler passes the caller's Proc; it cannot revoke
  another Proc); gated on the caller being console-attached.
  `kernel/proc.c::proc_console_relinquish` (under `g_proc_table_lock`).
- **`SYS_CONSOLE_OPEN`** -- attach `/dev/cons` (`devcons`) and install a
  `KOBJ_SPOOR` handle with `RIGHT_READ | RIGHT_WRITE`. The getty (joey) hands
  this to `/sbin/login` as fd 0/1/2. `devcons_read` ignores the Spoor and drains
  the global RX ring, so any opened handle is a valid console reader. v1.0
  ungates the open (the console is single-reader-guarded -- a 2nd concurrent
  read returns -1); a console-open capability gate is a v1.x seam.
  `kernel/syscall.c::sys_console_open_for_proc`.

libt wrappers: `t_boot_complete()` / `t_console_relinquish()` / `t_console_open()`
(`usr/lib/libt/include/thyla/syscall.h`).

libthyla-rs: `Command::identity(principal_id, primary_gid, supp_gids)`
(`usr/lib/libthyla-rs/src/process.rs`) -- stamps `SPAWN_IDENTITY_SET` + the
identity triple instead of inheriting the caller's. The supp-gids array is an
owned `Vec` local, so `supp_gids_va` stays valid through the `t_spawn_full_argv`
SVC (owned-value drop is at scope-end, not NLL last-use).

## `/sbin/login` (usr/login)

Native libthyla-rs (no_std), built into the ramfs (`usr_rs_bins`). `t_spawn`
resolves binaries from devramfs regardless of the pivoted root
(`sys_spawn_for_proc` -> `devramfs_lookup`), so login lives in the initrd, not
the disk-backed FS.

Flow (`rs_main`):
1. Read `{user, passphrase}` from fd 0 (byte-at-a-time line reads, so one
   underlying read cannot straddle two lines -- correct for both `/dev/cons` and
   a pipe).
2. `t_srv_connect("corvus", "ctl")` (bounded retry for the accept-queue race).
3. AUTH (verb 1) -> 33-byte session token; on bad auth, "login incorrect" + exit 1.
4. RESOLVE_NAME (verb 12) -> `principal_id` + `primary_gid`; RESOLVE_ID (verb 11)
   -> `supp_gids` (missing/short -> empty, non-fatal).
5. Emit the resolution marker `login: <user> uid=N gid=M` via `t_putstr` (the
   UART -> the boot log AND, at v1.0 single-console, the live console).
6. Spawn `ut` via `Command::identity(pid, gid, &supp).caps(LOCK_PAGES |
   CSPRNG_READ)` with fd 0/1/2 inherited -- the shell is born AS the user, with
   the two benign user caps but NOT `CAP_SET_IDENTITY` (a shell is not an
   identity-stamper).
7. Wait the shell (the session leader). Its exit IS logout: SESSION_CLOSE (verb
   3, which zeroes the keypair) + close the conn + exit 0.

login itself runs as `PRINCIPAL_SYSTEM` (inherited from joey), holds
`CAP_SET_IDENTITY | CAP_LOCK_PAGES | CAP_CSPRNG_READ` (granted by joey), and is
NEVER console-attached (joey relinquished; spawn does not confer the bit). On
any failure login exits non-zero; the getty respawns it.

## joey -- the session supervisor (usr/joey/joey.c)

After all boot-test asserts:
1. `do_login_e2e()` -- the NON-interactive CI proof: spawn login with fd 0 = a
   pipe seeded with `michael`'s creds (fd 1/2 = the same read-end; login emits
   its marker via the UART, so no capture pipe is needed and the
   "ut floods a pipe joey isn't draining" deadlock cannot arise). Gate: login
   exits 0 only if the whole cycle (AUTH + resolve + spawn-ut-stamped + reap +
   SESSION_CLOSE) succeeded; the `uid=1000 gid=1000` marker in the boot log
   confirms the resolved identity.
2. `t_boot_complete()` -> the banner.
3. `t_console_relinquish()` -> I-27.
4. `session_getty_loop()` -- never returns: open `/dev/cons`, spawn login on it,
   wait the session, loop. In the harness, login blocks reading `/dev/cons`
   (no input) -- harmless, the harness killed QEMU at the banner.

## Invariants

- **I-27** (sharpened): during a user session corvus is the SOLE
  console-attached Proc. login + the shell are never attached; joey relinquishes
  its boot attach. A post-SAK state is `{corvus}`, never `{joey, corvus}`.
- **I-22**: the logged-in user is a `principal_id`, never a superuser. login
  stamps only what corvus authenticated (the `CAP_SET_IDENTITY` gate); the shell
  holds no elevation/identity/grant caps.
- **Banner ABI** (TOOLING.md section 10): the `Thylacine boot OK` string is
  unchanged; only WHEN it fires moved (init's `SYS_BOOT_COMPLETE`, not joey's
  exit). The one-shot + console-attached gate prevent a spurious banner.

## Tests

- Kernel unit (`kernel/test/test_cons.c`, registered in `test.c`):
  `proc.console_relinquish` (clears own attach + the owner pointer; idempotent +
  NULL-safe), `proc.console_relinquish_other` (self-only -- never clears a
  different Proc's owner pointer), `cons.console_open` (open -> KOBJ_SPOOR R|W
  handle -> read drains the RX ring, end-to-end). 686/686.
- Boot-path E2E (`usr/joey/joey.c::do_login_e2e`): michael authed -> uid=1000
  gid=1000 -> ut spawned stamped + reaped + session closed; gated on login's exit
  code, runs every boot before the banner.
- The live `/dev/cons` interactive read is NOT CI-tested (the harness cannot
  inject UART RX -- the A-4c precedent); it is exercised by the seeded E2E's
  mechanism + the `Thylacine login:` prompt appearing in the boot log + an
  interactive run.

## Status / caveats

- A-5a is the login CORE. Per-user encrypted `/home` (the user's `--role client`
  stratumd + the DEK-via-corvus handoff) is A-5b; RECOVER + hostowner-c is A-5c.
- v1.0 single-console-serial: login writes prompts via fd 1 and the marker via
  `t_putstr` (the UART == the console). Multi-console / multi-session is a v1.x
  lift (prompts would route to fd 1 only; concurrent corvus sessions).
- `SPAWN_PERM_CONSOLE_OWNER` (Ctrl-C-to-the-shell) was DEFERRED: under I-27 the
  shell is spawned by a non-console-attached login, which cannot pass the
  console-attached-gated perm. The SAK protects elevation regardless of the
  Ctrl-C owner; a job-control/owner story is a Utopia-lane seam.
