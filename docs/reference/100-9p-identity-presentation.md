# 100 — 9P identity presentation (A-3)

**Status:** A-3a LANDED (the reconciliation mechanism). A-3b (dev9p rwx enforcement
activation + the A-2d F1/F2 closes) and A-3c (per-user stratumd mechanism proof +
trust-stamp seam) append to this doc as they land. Design: `IDENTITY-DESIGN.md §9.7`
+ the §3.5 F-4 correction + the §3.7.1 activation note.

---

## Purpose

A-3 reconciles a 9P file server's notion of *who is connecting* with Thylacine's
durable per-Proc `principal_id` (A-1a), so that the kernel's rwx enforcement layer
(A-2d) can be activated against `dev9p` without bricking the boot. It also corrects
the F-4 design: the load-bearing local identity channel is **`SO_PEERCRED`**, not the
9P `n_uname` field.

This page documents the A-3a *mechanism* (M1, M2, M4 of §9.7). The enforcement
*activation* (M3: the `dev9p.perm_enforced` flip + F1/F2) is A-3b.

## The F-4 correction (why SO_PEERCRED, not n_uname)

The 2026-05-28 F-4 sketch said "forward the principal-id as `n_uname` to the
trusted-local stratumd." Ground truth (two Explore passes, 2026-05-31) showed that is
the wrong channel against the actual stack:

- The Stratum 9P server **ignores `n_uname`** at Tattach (`server.c:1007-1008`,
  literally `/* ...ignore. */`) and stamps file ownership from the connection's
  **`SO_PEERCRED` uid** only (`server.c:2019` Tlcreate → `s->auth_uid`).
- pouch already marshals `getsockopt(SO_PEERCRED)` onto the kernel's **`SYS_srv_peer`**
  (`0006-pouch-sockets.patch`), and the 40-byte `srv_peer_info` has carried
  `principal_id` since A-1a — but the marshal hardcoded `ucred.uid = 0` (a pre-A-1a
  stub written when "Thylacine has no uid model").

So the trusted-local identity channel is **`SO_PEERCRED`-carries-principal**: the
server reads the connecting Proc's kernel-stamped `principal_id` (unforgeable — see
below) via the existing peer-cred path. `n_uname` is kept (cheap) but **demoted to the
v1.x foreign/authenticated path** (a server with no `SO_PEERCRED` — remote/TCP — where
the corvus trust-stamp gate then matters; none exists at v1.0).

## Mechanism (A-3a)

### M1 — `SO_PEERCRED` carries the principal (pouch boundary-line)

`usr/lib/pouch/patches/0006-pouch-sockets.patch` — the `getsockopt(SO_PEERCRED)` shim
marshals the kernel's `SYS_srv_peer` result into a `struct ucred`:

```c
struct { int pid; unsigned int uid; unsigned int gid; } ucred = {
    .pid = (int)(info.stripes & 0x7fffffff),
    .uid = info.principal_id,   /* A-3: was 0 */
    .gid = info.primary_gid,    /* A-3: was 0 */
};
```

`info` is `pouch_srv_peer_info` (the 40-byte mirror of the kernel `srv_peer_info`).
The principal is **kernel-stamped**: `SYS_srv_peer` reads the peer Proc's durable
`principal_id` via its `stripes` tag (`kernel/syscall.c::sys_srv_peer_for_proc`), so a
connecting Proc **cannot forge** it. This is the property that lets a trusted-local
server (a per-user stratumd) believe the presented identity without `Tauth` (the
distributed-credential case stays v1.x).

Effect: stratumd-in-Thylacine's `peer_creds()` (`src/cmd/stratumd/peer_creds.c`, the
`__thylacine__` arm → `getsockopt(SO_PEERCRED)`) now returns the connecting Proc's
principal as `auth_uid`, so its create-owner stamp records Thylacine principals.

### M2 — host-bake stamps `PRINCIPAL_SYSTEM` (Stratum `--bake-owner-uid` + build.sh)

The host-bake (`tools/build.sh::build_stratum_pool_fixture`) runs `stratumd` +
`stratum-fs write` as the host build user, so without intervention the baked corpus is
owned by the host uid (e.g. 501 / 1000), not a Thylacine principal. Stratum gains a
`--bake-owner-uid <u32>` / `--bake-owner-gid <u32>` flag (`src/cmd/stratumd/run.c`)
that, when set, overrides `s->auth_uid` / `s->auth_gid` at the FS create chokepoint
(`src/cmd/stratumd/serve.c::stm_stratumd_serve_client`, via a one-shot file-scope
publish in `stm_stratumd_run`). `build.sh` passes `--bake-owner-uid 4294967294`
(`PRINCIPAL_SYSTEM`) + `--bake-owner-gid 4294967294` (`GID_SYSTEM`).

- **Not an on-disk-format change** — `si_uid` / `si_gid` already exist in the inode
  (`include/stratum/inode.h:202-203`); only the stamped *value* differs. No
  `STM_UB_VERSION` bump.
- **Disabled by default** — a `bake_owner_enabled` bool (memset-0 default false) gates
  it; a per-axis `(uid_t)-1` sentinel leaves that axis on peer creds. The runtime
  per-user stratumd leaves it disabled and stamps via `SO_PEERCRED` (M1).
- Modes are the existing host-bake stamps (files `0644`, dirs `0755`, `/system.key`
  `0400`); since the boot chain is `PRINCIPAL_SYSTEM` = the owner, owner-bit access
  covers read/traverse/write everywhere it needs — the no-brick property A-3b relies on.

### M4 — `n_uname` forwarding (kernel; belt-and-suspenders)

`kernel/syscall.c` — both `sys_attach_9p_handler` and `sys_attach_9p_srv_handler`
substitute the calling Proc's `principal_id` for the `n_uname` Tattach field:

```c
        aname_len > 0 ? aname_scratch : NULL, aname_len,
        p->principal_id);   /* A-3 M4: was (u32)n_uname */
```

The userspace-supplied `n_uname` arg is now vestigial (still validated for ABI
hygiene, then superseded; the syscall ABI is unchanged). Against Stratum this is a
no-op (M1 is the live channel); it is forward-compat for a foreign 9P server that
honors `n_uname` but has no `SO_PEERCRED`.

## Build infrastructure: `stratum_host_tools_stale`

`tools/build.sh` — A-3a surfaced (and fixed) a stale-consumer footgun: the host
stratum tools (`stratum-mkfs`/`stratumd`/`stratum-fs`, built host-native to bake the
pool) were rebuilt **only when a binary was missing**, so a Stratum source edit (the
`--bake-owner-uid` flag) silently shipped a stale `stratumd` and the bake failed with
`unknown option`. The new `stratum_host_tools_stale()` (sibling of A-2a's
`sysroot_is_stale()`) rebuilds when any Stratum C source/header is newer than the built
`stratumd`. Same stale-consumer class as the A-2a sysroot fix.

## Invariants

- **I-22 preserved** — the `SO_PEERCRED` principal is kernel-stamped (`SYS_srv_peer`),
  not client-asserted; no identity self-elevates. `CAP_HOSTOWNER` remains the only
  DAC-override (A-2d).
- **I-2 / I-4 / I-6 unaffected** — M1/M2/M4 add no capability, no handle-transfer path,
  no rights expansion.
- **No on-disk-format change** (M2) — only the stamped `si_uid`/`si_gid` value differs.

## Tests

A-3a is behaviorally inert at runtime (dev9p rwx enforcement is still off until A-3b),
so it changes only *recorded* ownership + the `n_uname` wire value — neither enforced
yet. Verified by: full build (sysroot + host stratumd + kernel + pool re-bake), suite
**637/637 PASS** (default + UBSan), **boot OK**, and **cross-reboot PASS** (the corvus
identity-DB persistence path rides the SO_PEERCRED-stamped stratumd creates). The
enforcement-behavior tests (the deny/allow proof, F1 rights-from-omode, F2) land at A-3b.

## Status

- **A-3a (this):** M1 (pouch SO_PEERCRED → principal) + M2 (host-bake SYSTEM-owned via
  the Stratum `--bake-owner-uid` flag + build.sh) + M4 (kernel `n_uname` = principal) +
  the `stratum_host_tools_stale` build fix. dev9p enforcement remains OFF.
- **A-3b (next):** flip `dev9p.perm_enforced = true`; close A-2d audit F1
  (SYS_WALK_OPEN handle rights derived from omode; `T_OPATH` keeps the A-1.7 born-`R|W`
  base) + F2 (`perm_check` on `sys_rename_handler` / `sys_unlink_handler`); the
  enforcement deny/allow tests.
- **A-3c (next):** per-user stratumd `--role client` mechanism proof (dataset-scope
  `EACCES`-at-Tattach); the corvus trust-stamp gate recorded as a v1.x seam.
