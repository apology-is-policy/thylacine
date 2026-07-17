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
  `KOBJ_SPOOR` handle with `RIGHT_READ | RIGHT_WRITE`. **GATED on the caller
  being console-attached** (audit F2): `/dev/cons` is a single-reader global
  (`devcons_read` drains one ring; first reader wins), so an ungated open would
  let a user Proc steal/deny the getty's console input. The getty (joey) opens
  it while still attached -- BEFORE `SYS_CONSOLE_RELINQUISH` -- and hands the one
  handle to each `/sbin/login` (login + the user shell are never attached, so
  they cannot open it; post-SAK corvus is the attached Proc).
  `kernel/syscall.c::sys_console_open_handler` (gate) +
  `sys_console_open_for_proc` (core).

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
5a. **Seed the session environment** (`seed_session_env`, Go Stage 6 -- the
   Unix login(1) role): write `/env/HOME` (= `/home/<user>`), `/env/USER`,
   `/env/PATH` (= `/bin:/goroot/bin`, mirroring ut's static `$path`) into
   login's own per-Proc /env; the shell and every session child inherit a deep
   copy at spawn (`env_clone_into`). Values only -- the corvus token and the
   DEK lease must NEVER enter /env (it is readable by every descendant).
   Best-effort: a failed seed prints a marker and degrades toolchain defaults
   (Go derives GOCACHE/GOPATH/GOENV from `$HOME`), never the login.
5b. **The per-user /tmp is bound by `ut`, not login** (Go Stage 6): login runs
   as `PRINCIPAL_SYSTEM`, which cannot create -- or even X-search -- inside the
   0700 user-owned home, so the `~/tmp` mkdir + the MREPL bind over `/tmp`
   live in the shell (`ut`'s `bind_user_tmp`, gated on `--home`), which is
   born AS the user and whose private territory scopes the bind to exactly
   the session subtree. Rationale (why a bind at all): the global `/tmp`
   rides the shared SYSTEM-attach system mount, where a create's OWNER is
   the attach identity (9P identity is per-Tattach; only the gid travels
   per-op) -- a user's mode-0700 `MkdirTemp` under it (Go's `$WORK`; any
   POSIX `mkdtemp`) landed SYSTEM-owned and the user could not create
   inside it. The underlying per-op-identity limitation of the shared
   system mount is A-3's documented v1.x foreign/authenticated-attach seam;
   sessions never see it through the bind.
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

## The `!recover` passphrase-recovery UX (A-5c-c)

A user who has forgotten their passphrase types `!recover` at the
`Thylacine login:` prompt (intercepted by `rs_main` right after the username
read; `!` cannot begin a corvus username, charset `[A-Za-z0-9._-]`, so the
sentinel can never shadow a real user). `do_recover_flow` then:

1. Reads `{recover user, recovery phrase, new passphrase}` (three line reads).
2. `connect_corvus()` and drives `RECOVER` (verb 8, `subject_kind = 1`):
   `subject_kind(1) | user_len(1) | user | phrase_len(2 LE) | phrase |
   new_pass_len(2 LE) | new_pass`. **No session token, no capability, no console
   attach** -- the user has lost the passphrase, so phrase-knowledge + corvus's
   rate limit are the entire gate (this is exactly why the recovery path is
   distinct from a login: it needs none of login's authority).
3. On `OK`, corvus has reset the passphrase **and rolled a fresh recovery
   phrase**; login surfaces the fresh phrase (`phrase_len(2 LE) | fresh_phrase`)
   to the tty with "write it down; the old one no longer works", emits the
   boot-log marker `login: recovery ok (...)`, and exits 0. No session is
   created -- the getty re-prompts and the user logs in with the new passphrase.
4. On any non-`OK` (bad phrase / unknown user / rate-limited / transport),
   login prints a generic "recovery failed" + a `login: recovery FAILED (...)`
   marker and exits non-zero.

Secret hygiene: the recovery phrase, the new passphrase, the RECOVER payload
buffer, and the returned fresh phrase are all `t_explicit_bzero`'d on every path
(login already `set_dumpable(0)`/`set_traceable(0)` at startup). The fresh
phrase rides the response buffer to the tty (the user's secret to record); it is
scrubbed immediately after display.

`RECOVER` re-wraps the **keypair**, not the dataset DEK (the keypair value is
unchanged), so a user who recovers keeps access to their existing encrypted home
-- the per-dataset DEK envelopes (encapsulated to the unchanged public keys) stay
valid. See `docs/reference/105-corvus-recovery.md`.

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

## A-5b: the per-user encrypted-home DEK lifecycle (#827a-login)

After identity resolution and before spawning the shell, login drives the boot
coordinator's `/ctl` over a 9P attach held for the whole session:

1. **attach** -- `t_open("/srv/stratum-ctl", T_ORDWR)` (open=connect -> a byte
   conn; the coordinator posts `/ctl` byte-mode) then `t_attach_9p_srv(conn,
   NULL, 0, 0)` -> the dev9p `/ctl` root. The byte-conn fd is closed after the
   attach commits (`kernel_attached` keeps the rings alive). The new
   `libthyla_rs::t_attach_9p_srv` wrapper (SYS_ATTACH_9P_SRV = 52) is the native
   bridge; bounded-retry covers the accept race.
2. **provision-dek** (idempotent ensure-home) -- a single Twrite of
   `{owner_uid u32 LE, owner_gid u32 LE, name_len u8, name, path_len u8,
   corvus_path, token[33]}` to `datasets/provision-dek`. `name` = the username
   (single component); `corvus_path` = `users/<user>`; `owner` = the user's
   `principal_id`/`primary_gid` (the home root is born user-owned 0700, F1
   isolation); `token` = the corvus AUTH token. The coordinator mints a DEK +
   WRAPs it via corvus + commits; `STM_EEXIST -> OK` for a returning user.
3. **name->id bridge** (provision-dek is write-only) -- `t_readdir datasets`,
   and for each numeric `<id>` read `datasets/<id>/properties`, match the
   `name: <user>` line -> the home dataset id.
4. **install-dek** -- a 33-byte-token Twrite to `datasets/<id>/install-dek`
   UNWRAPs the DEK into the live map. The lease is CONN-BOUND, so login holds
   the `/ctl` attach for the session.
5. **logout** -- `evict-dek` (zeroes the session DEK), drop the `/ctl` attach,
   then the corvus `SESSION_CLOSE`.

login NEVER holds the raw DEK -- it forwards only the opaque corvus token; the
coordinator UNWRAPs/WRAPs over its OWN corvus connection (the #829 bearer-token
session-ownership lift). The DEK lifecycle is **fatal** (a login that cannot
provision/unlock the encrypted home is a failed login -- pam_mount parity), so
the `do_login_e2e` exit-code gate covers the whole path.

**Three cross-layer reconciliations this first end-to-end exercise required**
(folds into the #828 audit):

- **kernel rwx on `/ctl`** -- A-3 flipped `dev9p.perm_enforced=true`, so login's
  write to the 0200 DEK nodes hits the kernel owner-first check. The coordinator
  now reports those three SYSTEM-gated nodes (`provision/install/evict-dek`) as
  owned by `system_uid` in `getattr_at` (Stratum `src/ctl/synfs.c`), so the
  kernel check is coherent with the server's own SYSTEM gate -- login runs as
  PRINCIPAL_SYSTEM and is the owner. (The A-3 "wire owner == authorized
  principal" reconciliation, applied to the control surface.)
- **`send`/`recv` pouch shim** -- the Stratum corvus client's `write_all` uses
  `send(..., MSG_NOSIGNAL)`; pouch shimmed `read`/`write` but not `send`/`recv`.
  `0006-pouch-sockets.patch` adds `send`/`recv` dispatch shims (tagged socket fd
  -> the slot's kernel write/read; MSG_NOSIGNAL is a no-op since a closed
  srvconn write returns an error, not a signal; other flags EOPNOTSUPP).
- **`dial_corvus` fcntl tolerance** -- the corvus client set O_NONBLOCK via
  `fcntl(F_GETFL/F_SETFL)` for the bounded-connect poll; Thylacine has no fcntl
  (`__NR_fcntl` is the 0xFFFF ENOSYS sentinel). `dial_corvus` now degrades to a
  blocking connect when the non-blocking setup is unavailable (the pouch v1.0
  socket model is blocking by design; the `/srv` connect-walk is a fast local
  open).

## A-5b: the per-user home bind (#827b-beta)

After the DEK is installed, login binds the user's encrypted home at
`/home/<user>` (`usr/login/src/main.rs::bind_home` / `unbind_home`), BEFORE
spawning the shell, so the shell inherits the mount (the kernel deep-copies the
mount table on spawn). The home is a SEPARATE Stratum child dataset (its own
DEK); the path:

1. **Spawn the per-user proxy AS the user**: `Command::new("stratumd")
   .identity(pid,gid,&supp).perm(MAY_POST_SERVICE).caps(CSPRNG_READ)
   .args(["--role","client","--listen","/srv/home-<user>",
   "--coordinator-socket","/srv/stratum-fs","--datasets-allowed","ds:<user>",
   "--single-session"])`. The proxy's connection to the coordinator carries the
   user's `SO_PEERCRED` (per-user ownership); `--datasets-allowed ds:<user>`
   admits ONLY the user's own attach (the I-1 boundary); `--single-session`
   serves login's one attach then exits (the cooperative teardown lever -- login
   cannot kill the user-owned proxy). `MAY_POST_SERVICE` is conferred via the
   one-hop delegation (login holds it from joey).
2. **Wait for + attach**: bounded retry (3000 x 1 ms torpor-yield, the joey
   stratumd-boot pacing) `open=connect /srv/home-<user>` then
   `t_attach_9p_srv(conn, "ds:<user>", ...)` -- the Stratum `ds:<name>` aname
   form (a new attach kind) binds the connection root to the named child
   dataset. The proxy's stderr is captured + drained to the boot log (joey's
   stratumd diagnostics pattern); the read end is dropped after a successful
   bind so steady-state proxy stderr can't stall on a full pipe.
3. **Mount**: mkdir `/home` + `/home/<user>` (the joey `mkdir_or_open` idiom),
   then `t_mount("/home/<user>", attach_root, T_MREPL)`.
4. **Teardown (logout)**: `t_unmount("/home/<user>")` + close the attach -> the
   single-session proxy's upstream EOFs -> it exits -> `proxy.wait()` reaps it;
   then evict-dek + corvus SESSION_CLOSE.

FATAL: a home that cannot be bound is a failed login (the boot E2E gates the
whole path). Marker: `login: home <user> bound at /home/<user>`.

The Stratum `ds:<name>` aname (`src/9p/server.c::h_attach` + the
`stm_fs_lookup_child_dataset` / `stm_dataset_lookup_child_by_name` resolver) is
the per-user-encryption mechanism: 3 orthogonal access gates -- the proxy
`--datasets-allowed` (the I-1 user-vs-user boundary), the child dataset root's
`0700` owner (the A-3 kernel rwx after attach), and the installed DEK (an
un-unlocked dataset's root stat fails -> the attach is inert).

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
  gid=1000 -> A-5b DEK lifecycle (`login: dek michael ds=2 home provisioned +
  unlocked`) -> ut spawned stamped + reaped + session closed; gated on login's
  exit code (DEK-fatal), runs every boot before the banner.
- Recovery-UX boot-path E2E (`usr/joey/joey.c::do_recover_e2e`, A-5c-c-2):
  fresh-pool-gated. joey captures michael's enrolled phrase from the USER_CREATE
  OK frame, then spawns `/sbin/login` with a seeded pipe feeding the recovery
  dialogue `!recover\nmichael\n<phrase>\n<pass_michael>\n`. login drives a LIVE
  `RECOVER(user)` (boot log `login: recovery ok (...)`), resetting michael's
  passphrase to `pass_michael` (RESTORE) so the subsequent `do_login_e2e` still
  authenticates against the re-wrapped `hybrid.corvus` -- independently proving
  the re-wrap is a valid argon2 round-trip AND the home DEK still unlocks (the
  keypair value is unchanged). Idempotent across reboots: boot N+1 (persistent
  pool) `login !recover E2E skipped`. Marker `joey: login !recover E2E OK`.
- Stratum host ctest `test_corvus_provision::dek_nodes_report_system_owner`:
  the three SYSTEM-gated DEK nodes report uid/gid == system_uid via getattr;
  world-readable `properties` stays uid/gid 0; unset system_uid -> the invalid
  sentinel (fail-closed). 11/11 ctl/corvus/stratumd/proxy.
- The live `/dev/cons` interactive read is NOT CI-tested (the harness cannot
  inject UART RX -- the A-4c precedent); it is exercised by the seeded E2E's
  mechanism + the `Thylacine login:` prompt appearing in the boot log + an
  interactive run.

## Status / caveats

- A-5a is the login CORE. The DEK-via-corvus handoff (provision/install/evict
  over the coordinator's `/ctl`) is **A-5b #827a-login (DONE)** -- see the DEK
  lifecycle section above. The per-user `--role client` proxy + the `/home`
  bind is **#827b-beta (DONE)** -- see the per-user-home-bind section above; the
  shell now boots with `/home/<user>` mounted from the user's encrypted child
  dataset (`login: home <user> bound at /home/<user>`). The A-5b formal audit is
  **#828** (DEK handoff + the `ds:<name>` resolver + the grant-gate + the proxy
  serve-one boundary-line -- AEGIS/mallocng-adjacent + privilege). RECOVER +
  hostowner-c is A-5c.
- v1.0 single-session simplification: the proxy posts `/srv/home-<user>` into
  the INHERITED (boot) `/srv` registry, not a freshly-minted per-session one --
  fine for single-session v1.0 (the name is user-scoped, no collision), and the
  per-session-`/srv` isolation (the stalk-3a F2 mortal-registry concern) is a
  v1.x lift surfaced here.
- v1.0 single-console-serial: login writes prompts via fd 1 and the marker via
  `t_putstr` (the UART == the console). Multi-console / multi-session is a v1.x
  lift (prompts would route to fd 1 only; concurrent corvus sessions).
- `SPAWN_PERM_CONSOLE_OWNER` (Ctrl-C-to-the-shell) was DEFERRED: under I-27 the
  shell is spawned by a non-console-attached login, which cannot pass the
  console-attached-gated perm. The SAK protects elevation regardless of the
  Ctrl-C owner; a job-control/owner story is a Utopia-lane seam.

### A-5b #828 audit hardening

The A-5b formal audit (#828; three Opus prosecutors + a self-audit; 0 P0 + 0 P1 +
4 P2 + 6 P3) verified the core architecture SOUND and closed four P2s:

- **Secret hygiene** (A-F1): login now `t_explicit_bzero`s the cleartext
  passphrase, the corvus session token, and the AUTH/provision payload buffers
  the moment they are consumed (mallocng recycles freed heap), and sets
  `t_set_dumpable(0)` + `t_set_traceable(0)` at startup -- matching corvus's
  discipline (CORVUS-DESIGN.md 4.1.1). `mlockall` is intentionally omitted (it
  needs `CAP_LOCK_PAGES`, which login only holds to pass down, and v1.0 has no swap).
- **Proxy reap on bind failure** (C-F1): the three `bind_home` failure paths that
  occur AFTER a successful attach (`/home` mkdir, `/home/<user>` mkdir, `t_mount`)
  now `proxy.wait()` after closing the attach -- the single-session proxy EOF-exits,
  so the reap cannot hang, and it does not orphan to kproc as a permanent zombie.
  The fourth path (attach never succeeded -- no EOF/kill lever) keeps the documented
  drop and is tracked (kproc orphan reaper, task #855).
- **DEK eviction is crash-safe** (A-F2): `provision-dek` installs the minted DEK
  into the live map (init_dataset_root needs it) AND now records a conn-bound DEK
  lease, so conn-destroy auto-evicts the DEK on ANY login exit (clean / install-fail
  / crash). Without the lease, a provision not followed by a successful install-dek
  left the cleartext DEK resident until coordinator unmount.
- **Username charset** (B-F1): corvus `handle_user_create` rejects any byte outside
  `[A-Za-z0-9._-]`, so the derived `ds:<user>` proxy `--datasets-allowed` glob
  pattern (the load-bearing attach gate) can never carry a glob metacharacter or a
  path separator.

Tracked v1.x follow-ups: #855 (a kproc/init orphan reaper + a bounded single-session
proxy accept, for the bind_home no-lever path) and #856 (a `wait`-by-pid surface, so
a mid-session proxy death cannot mis-reap the shell). Closed list:
`memory/audit_828_closed_list.md`.
