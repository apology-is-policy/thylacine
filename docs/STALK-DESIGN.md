# STALK-DESIGN.md — the per-Proc pathname resolver

> **STATUS: SIGNED OFF (user, 2026-06-02). BINDING.** Scripture before code: this
> doc + the ARCHITECTURE.md invariant (I-28) + the audit-trigger row + the
> ROADMAP / detour-status sequencing land as the scripture commit; the
> implementation (stalk-1 ...) follows in subsequent commits that cite its SHA.
> Sub-decisions D1-D4 are resolved (section 11).

`stalk` is Thylacine's multi-component pathname resolver — the Plan 9 `namec`
(*name-to-channel*), renamed. The thylacine **stalks** its quarry along a path
through the bushland (the per-Proc namespace) to the prey (the target **Spoor**).
The metaphor is already half-built: a *spoor* is the trail an animal leaves; the
object a path resolves to is a `Spoor`; the resolver follows the trail.

```
stalk(p, start, "/srv/corvus/ctl", amode, omode) -> *Spoor
```

---

## 0. Why this exists now

Two drivers, one of them hard:

1. **A-5b isolation (the forcing function).** A-5b gives each user a per-user
   encrypted home served by a **per-user** stratumd. The current service
   registry is a single global flat table of 8 slots, **one poster per name**
   (`srv_reserve` rejects a second live poster of a name, `kernel/devsrv.c`).
   Two users' coordinators both posting `"stratum-fs"` **collide** — the global
   registry literally *cannot represent* per-user coordinators. The only escapes
   are per-session naming (a `/srv` scoped to the session) or leaking identity
   into global names (`stratum-fs-michael`, which lets one user enumerate
   another's services). So per-territory `/srv` is **required for the feature to
   work**, not just defense-in-depth.

2. **The deferred-since-P5 namespace spine.** Thylacine has the namespace *data
   structures* (`Territory.root_spoor` + `binds[8]` + `mounts[8]`) and a
   single-component `SYS_WALK_OPEN`, but **no multi-component resolver**:
   `SYS_WALK_OPEN` rejects `/`, `.`, `..` and never consults the mount table
   (`kernel/syscall.c`). Absolute paths, mount-crossing, `..`, `/proc`, `/net`,
   the shell's path handling — all wait on this. `stalk` is that resolver;
   namespace-resident `/srv` is its first consumer.

**User decision (2026-06-02):** build the full Plan-9 spine (vs. an fd-relative
shim or a narrow 2-level crosser). System over cost/scope.

---

## 1. Prior art -> the chosen model

- **Plan 9 (heritage).** `namec` resolves a path component-by-component; at each
  resulting channel it runs `domount` — *is this channel a mount point?* keyed by
  the channel's **identity** (type+dev+qid), not by a path string. `/srv` is the
  `#s` device *bound into the namespace*; isolation is **per-namespace bind +
  file owner/mode (rwx)**. A parent can hand a child a restricted `/srv`.
- **Fuchsia / Genode (capability SOTA).** No global broker: a component sees a
  service under `/svc` **only if its parent routed it**. Isolation is
  structural, not permission-gated.
- **Convergence for Thylacine** (Plan-9 namespace + A-3 capability rwx): both
  traditions point at the *same* answer — `/srv` is a namespace-resident Dev;
  **the session's parent (login) constructs each session's `/srv` view**; a
  service is reachable iff it is in *that session's* `/srv`. Plan-9-true and
  capability-true at once.

`stalk` adopts Plan 9's `domount` model directly: walk normally; after resolving
each Spoor, check whether it is a mount point **by Spoor identity**; if so, cross.

---

## 2. Ground truth (verified; drives the design)

| Fact | Citation | Consequence for `stalk` |
|---|---|---|
| `binds[]`/`mounts[]` are **read nowhere in a walk path** today (only clone / unref / cycle-check / diagnostics). | `kernel/territory.c` survey | `stalk` is a **clean first consumer**; no existing walk behavior to preserve. |
| Mount table keyed by **abstract `path_id_t = u32`**, no string mapping; only callers hardcode `t_mount(fd, 99, 0)`. | `territory.h:64-68`, `stub-driver.c:81` | Re-key to **mount-point Spoor identity** (Plan 9 domount); retire `path_id_t`. |
| `SYS_WALK_OPEN` is single-component (rejects `/ . ..`), never consults mounts. | `syscall.c:1571-1599` | `stalk` is the new multi-component path; generalizes the single-hop contract. |
| `dev9p_walk` batches up to `P9_MAX_WALK`=16 components per `Twalk`, rejects partial. | `dev9p.c:130-230` | Batching is *available* but **not used in v1.0** (see 4.3 — enforcement correctness). |
| `perm_check(p, &st, PERM_X)` + `spoor_stat_native` is the existing single-hop X-search. | `perm.c:18-52`, `syscall.c:1564-1568` | `stalk` runs this **per component** (the privilege boundary it must uphold). |
| `devsrv` IS a registered Dev (real `attach`/`walk`, stub `open`) but **mounted into no territory**. | `dev.c:123`, `devsrv.c` | Mount it per-territory; make `open` do the connect. |
| Root is `devramfs` at boot, `dev9p` after joey's `pivot_root`; children inherit `root_spoor` via `territory_clone`. | `joey.c:174-184`, `usr/joey/joey.c:2028` | `stalk` resolves from `root_spoor`; `/srv` mounts onto the session root. |

---

## 3. The data-structure change (mount table -> Spoor-identity keyed)

Today `PgrpMount = { Spoor *source; path_id_t target; u32 flags; }` (16B), keyed
by an abstract token. `stalk` needs to recognise a mount point **when it walks
onto it**, so re-key by mount-point identity -- the **full Plan 9 `(type, dev,
qid)`** triple cited in section 1, NOT just `(dc, qid.path)`:

```c
struct PgrpMount {
    struct Spoor *source;       // the grafted tree (holds one ref, as today)
    u64           mp_qid_path;  // mount-point Spoor's qid.path
    int           mp_dc;        // mount-point Spoor's Dev char (Plan 9 `type`)
    u32           mp_devno;     // mount-point Spoor's per-instance device number
    u32           flags;        // MREPL (v1.0) | MBEFORE/MAFTER/MCREATE (v1.x union)
    u32           _pad;         // 8-byte array-stride alignment -> sizeof = 32
};
```

**Why `devno` is load-bearing, not optional** (stalk-2 impl finding; this
corrects the section-1-vs-section-3 under-specification): `qid.path` is unique
only WITHIN a `(dc, devno)` instance. EVERY dev9p session shares `dc='9'` and
every Tattach root has `qid.path == 0`, so `(dc, qid.path)` alone **cannot**
distinguish two concurrent 9P sessions' mount points -- exactly the A-5b case
(corvus + a per-user stratum-fs, both mounted in one Territory). Plan 9's `Chan`
carries a per-instance `dev` number for this reason; Thylacine adds `u32 devno`
to `struct Spoor`, minted per attach session by `spoor_next_devno()` (dev9p
stamps the attach root; walked/cloned descendants inherit it; static
single-instance Devs leave it 0). The mount key is the full `(dc, devno,
qid.path)` triple.

`stalk`, after resolving a component to Spoor `S`, scans the table
(`territory.c::mount_lookup`) for an entry with `mp_dc == S->dc && mp_devno ==
S->devno && mp_qid_path == S->qid.path`; on match it crosses to a fresh
clone-walk of `source` (Plan 9 `domount`; a zero-element `Dev.walk` mints an
independent fid). `path_id_t` retires AS THE MOUNT KEY (it stays for the
unconsumed symbolic `binds[]` per D1); the size-pinned `Territory`
static_asserts re-bump 16->32 bytes/entry (deliberate; documented).

**A fourth amode -- `STALK_MOUNT`** (stalk-2 impl finding): `SYS_MOUNT` /
`SYS_UNMOUNT` resolve the mount point with the final component **NOT crossed**
(Plan 9 `Amount`), so re-mounting onto an already-mounted point keys on the SAME
underlying identity and MREPL replaces it (crossing the final element would key
the new mount on the source-root identity, producing a second entry instead of a
replace). Intermediate components still cross normally. `STALK_WALK` /
`STALK_OPEN` cross the final element (opening a walked mount point yields the
mounted root).

`SYS_MOUNT` / `SYS_UNMOUNT` become **path-keyed**: the kernel `stalk`s the path
to the mount-point Spoor, then records `key(mountpoint) -> source`. The
`t_mount(fd, 99, 0)` test callers migrate to `t_mount(fd, "/path", flags)`.

**Mount points must exist** (Plan 9 M1, not a synthetic overlay): `/srv` must be
a walkable directory on the session root. devramfs gains a `/srv` dir (and
`/proc`, `/net` as they arrive); the post-pivot FS root must likewise provide the
mount-point dirs (host-bake / first-login provisions them). See sub-decision D4.

**`binds[]` stay as-is** (path_id symbolic) and are **not consumed by `stalk` in
v1.0** — there is no v1.0 consumer of symbolic binds, and building
bind-in-walk + the Spoor-identity cycle-rework without a consumer is speculative
(VISION: complexity only where verified). Bind-in-walk is a v1.x refinement.
See sub-decision D1.

---

## 4. The `stalk` algorithm

### 4.1 Signature (Plan-9 `namec` shape)

```c
// amode: STALK_WALK (resolve only, no open — the O_PATH case)
//        STALK_OPEN (open the final component with omode)
//        STALK_CREATE (create-or-open the final component)
//        STALK_MOUNT (resolve to the mount-point parent, for SYS_MOUNT)
struct Spoor *stalk(struct Proc *p, struct Spoor *start,
                    const char *path, size_t pathlen,
                    int amode, u32 omode);
```

`start == NULL` (or a FROM_ROOT sentinel) + a leading `/` => resolve from
`p->territory->root_spoor`. A relative path resolves from `start` (a dirfd's
Spoor). Returns the resolved (and, for `STALK_OPEN`, opened) Spoor with one ref;
the syscall layer does `handle_alloc(KOBJ_SPOOR, rights_for_omode(omode) | ...)`.

### 4.2 The loop

```
toks  = tokenize(path)                 // split '/', collapse '//', classify '.' '..'
cur   = ref(absolute ? root_spoor : start)
cur   = cross_mounts(cur)              // the start itself may be a mount point
trail = [cur]                          // the stack of resolved Spoors -- '..' + cleanup
for each component name in toks:
    if name == "."  : continue
    if name == ".." : cur = pop_to_parent(trail)       // contained at root; cross-mount aware
                      continue
    // per-component X-search on the directory we are about to search
    if cur->dev->perm_enforced:
        spoor_stat_native(cur, &st); perm_check(p, &st, PERM_X)   // fail-closed
    nc = spoor_clone(cur)
    w  = cur->dev->walk(cur, nc, &name, 1)     // ONE component (no batch, v1.0)
    if walk failed: unwind(trail); clunk nc; return NULL
    cur = nc
    cur = cross_mounts(cur)                     // domount after every hop
    push cur onto trail
// the final component is the quarry:
quarry = cur
if amode == STALK_OPEN:
    if quarry->dev->perm_enforced:
        spoor_stat_native(quarry, &st); perm_check(p, &st, perm_want_for_omode(omode))
    quarry = quarry->dev->open(quarry, omode)
unwind(trail, except quarry)                    // spoor_clunk each intermediate
return quarry
```

### 4.3 Key correctness decisions

- **Per-component X-search (the privilege boundary).** Every directory traversed
  needs `PERM_X` for the caller's principal — the A-3 invariant generalized from
  the single-hop walk-open to N hops. This is what `stalk` must get right;
  getting it wrong is a path-traversal privilege escalation.
- **No batching in v1.0.** One component per `Dev.walk`, with a kernel X-check at
  each hop, for both devramfs and dev9p. Batching consecutive same-Dev components
  into one `Twalk` *skips* the kernel's intermediate X-checks (it would have to
  trust the server's per-component enforcement). Correctness-first: walk one at a
  time. (Cost: a deep dev9p path is N x [Tgetattr + Twalk]. Batching is a
  documented **v1.x perf optimization**, gated on a careful kernel-vs-server
  enforcement split.)
- **`..` containment (the security must-have).** `..` pops the in-call **trail**
  (the stack of resolved Spoors); at the bottom (the start Spoor; `root_spoor`
  for absolute paths) it **cannot escape** — `../../../...` can never resolve
  above the territory root (the chroot/pivot boundary, I-1). Full Plan-9
  cross-mount `..` fidelity (Chan->mh back-pointers persisted on a dirfd across
  separate `stalk` calls) is a v1.x refinement; v1.0's in-call containment is the
  audited invariant.
- **Lifetime / refcount.** Every resolved Spoor is a clone holding one ref, kept
  on the **trail**; at return, `unwind` `spoor_clunk`s every trail entry except
  the **quarry**, which carries the handle's ref. This is the UAF/leak-class
  hazard — the single-hop walk-open already does clone + clunk-on-fail; `stalk`
  generalizes it to N hops and is audited as such.
- **Mount crossing is the first live read of `mounts[]`.** `cross_mounts` runs on
  the start Spoor and after every hop.

---

## 5. The `/srv` consumer (namespace-resident service registry)

### 5.1 Per-territory service table

The global `g_srv_registry` becomes **per-session** (or per-territory). corvus
and each per-user stratumd post into the session's table; a service is visible
only to Procs whose territory's `/srv` is backed by that table. susan's `/srv`
has no entry for michael's coordinator — the isolation is structural, at the
naming layer, *above* the A-3 rwx layer that already denies the file reads.

### 5.2 `open(/srv/<name>)` = connect

`devsrv_open` (today a stub) performs the connect: mint a `SrvConn` + drive the
handshake.

- **9p-mode service** (corvus): `open` attaches (Tversion + Tattach) and returns
  a **dev9p-style root Spoor**, so `/srv/corvus/ctl` is a further normal dev9p
  walk. This **unifies with `SYS_ATTACH_9P_SRV`** (which already wraps a SrvConn
  into a dev9p root).
- **byte-mode service** (stratum-fs, pouch sockets): `open` returns a
  byte-stream Spoor; `devsrv_read`/`devsrv_write` already drive the rings, so
  pouch `read`/`write` work on the fd.

The returned connection handle is a **`KOBJ_SPOOR`** (Dev = devsrv or dev9p),
not the current `KObj_Srv`. `SO_PEERCRED` peer info rides in the Spoor's aux. The
`SRV_CONN_PER_PROC_MAX = 1` cap likely must **relax** (a session needs corvus
*and* its stratum-fs concurrently) — flagged for the stalk-3 sub-design.

### 5.3 Posting

Plan-9-true posting is `create(/srv/<name>)` -> the kernel mints a listener bound
to the caller and returns a `KObj_Srv` listener handle (Thylacine doesn't pass
fds via writes — handles are non-transferable — so "create + the kernel binds
your listener" replaces Plan 9's "create + write your channel fd"). Alternatively
keep `SYS_POST_SERVICE`. See sub-decision D2.

---

## 6. Syscall ABI

| Syscall | Shape | Disposition |
|---|---|---|
| `SYS_OPEN(start_fd, path, omode, flags)` | NEW — the multi-component `stalk` entry; `start_fd = -1` => root | add |
| `SYS_WALK_OPEN` | single-component | fold into `SYS_OPEN` (or keep as a fast path) |
| `SYS_MOUNT(path, source_fd, flags)` / `SYS_UNMOUNT(path)` | path-keyed | re-shape from `path_id` |
| `SYS_BIND` | path-keyed (symbolic) | v1.x (no v1.0 consumer; D1) |
| `SYS_SRV_CONNECT` | `(name, path)` connect | **retire** — pouch `connect("/srv/x")` becomes `SYS_OPEN("/srv/x", ORDWR)` once absolute paths resolve |
| `SYS_POST_SERVICE` / `_BYTE` | post by syscall | **retire** if D2 = post-by-create, else keep |

Retiring `SYS_SRV_CONNECT`/`POST_SERVICE` is an **ABI break** (no-compat-shim
style policy favours it) — needs signoff (D3).

---

## 7. Invariants & spec posture

- **I-1 (namespace isolation).** `stalk` reads only the caller's territory;
  per-session `/srv` makes cross-user services *unnameable*. Strengthened, not
  weakened.
- **I-3 (mount DAG / no cycle).** Preserved; the bind cycle-check stays. v1.0
  `stalk` follows mount edges that are cycle-free by construction (one mount per
  mount point; MREPL replaces).
- **Per-component X-search (A-3 / I-22-adjacent).** The new enforcement
  obligation — every directory hop checks `PERM_X` for the caller's principal.
- **`..` containment.** No resolution escapes `root_spoor`. (New, audit-critical.)
- **Spoor lifetime across N hops.** No UAF / no leak. (New, audit-critical.)

**Spec posture.** No new TLA+ module (spec-to-code suspended since 2026-05-23;
prose validation in this doc + the audit + the test suite). `specs/territory.tla`
+ `specs/namespace.tla` remain valid abstractions (they model mount points by an
abstract key; Spoor-identity keying is a refinement of that key) — their **buggy
cfgs stay pre-commit gates** and are re-run on every `stalk`/mount change.

---

## 8. Migration surface (every caller)

- **Abstract-path-id mount callers:** `usr/stub-driver`, `usr/attach-probe`
  (`t_mount(fd, 99, 0)` -> path string).
- **`/srv` clients (6 native + the pouch seam):** corvus post; joey ->
  corvus/ctl, joey -> stratum-fs, joey attach-9p; login -> corvus/ctl;
  legate-prover -> corvus/ctl; pouch sockets `bind`/`connect`/`accept`
  (`0006-pouch-sockets.patch`).
- **Single-component FROM_ROOT walkers** (joey post-pivot probe, etc.) -> keep
  working under `SYS_OPEN` (single component is the degenerate path).
- **pouch `openat`** -> generalize to absolute `SYS_OPEN`.

---

## 9. Sub-chunk plan (each lands independently; own tests; audit per stakes)

- **stalk-1 — resolver core.** tokenizer + multi-component walk *within one Dev*
  (no mount-cross) + per-component X-search + `.`/`..` containment + `SYS_OPEN`.
  Delivers absolute FS paths (`/sbin/login`, `/home/<user>`). Migrate FROM_ROOT
  callers. **Own audit** (path traversal).
- **stalk-2 — mount re-key + crossing (domount).** `PgrpMount` -> Spoor-identity;
  `SYS_MOUNT`/`UNMOUNT` path-keyed; `cross_mounts` in `stalk`; devramfs `/srv`
  `/proc` dirs. Migrate stub-driver/attach-probe. **Own audit** (mount-cross perm
  + lifetime).
- **stalk-3 — devsrv per-territory + namespace-resident `/srv`.** per-territory
  service table; mount devsrv at `/srv`; `open`=connect (9p->dev9p root,
  byte->stream); posting (D2); relax per-Proc cap; migrate all `/srv` clients +
  pouch seam; retire old syscalls (D3). **Own audit** (the isolation property +
  the connection-handle reconciliation; AEGIS/mallocng-adjacent via the A-5b DEK
  path it unblocks — prosecute hard).

Then A-5b's body (#826/#827/#829) resumes on top of namespace-resident `/srv`.

---

## 10. Audit plan

Path resolution is a **privilege boundary** — each sub-chunk gets a focused
adversarial round. Prosecution focus: `..` escape above root; per-component
X-search bypass (symlink/`..`/mount-cross tricks); Spoor lifetime across hops
(UAF / double-clunk / leak); per-territory isolation (cross-user service
nameability); mount-cross permission (crossing into a tree you lack X on);
union/MREPL edge cases; the connection-handle reconciliation (SO_PEERCRED, cap,
attach-9p unification); integer/bounds on the tokenizer.

---

## 11. Resolved sub-decisions (user-signed-off 2026-06-02)

| # | Decision | Resolution |
|---|---|---|
| **D1** | bind-in-walk now, or defer? | **DEFER to v1.x** — no v1.0 consumer of symbolic binds; the verified-complexity rule (the one place we do *not* over-build). `binds[]` + the cycle-check stay as the API; `stalk` does not consume them in v1.0. |
| **D2** | posting model | **post-by-`create(/srv/<name>)`** (Plan-9-true) — the kernel mints a listener bound to the caller and returns a `KObj_Srv` listener handle (handles are non-transferable, so "create + the kernel binds your listener" replaces Plan 9's "create + write your channel fd"). Symmetric with connect-by-open; removes `SYS_POST_SERVICE`. |
| **D3** | retire `SYS_SRV_CONNECT` / `POST_SERVICE`? | **RETIRE** after migration — `SYS_OPEN("/srv/x", ...)` subsumes connect; `create("/srv/x")` subsumes post; no-compat-shim style policy. ABI break, landed in stalk-3. |
| **D4** | mount-point existence | **M1 (Plan 9 domount)** — mount points must be walkable dirs; devramfs gains `/srv` (+ `/proc`, `/net` as they arrive); the post-pivot FS root provides them (host-bake / first-login provisions). Unifies bind/mount/union; no synthetic-overlay special case. |

---

## Naming rationale

`stalk` (user-voted 2026-06-02) for the resolver: the apex-predator verb whose
quarry is reached along a path — composing with `Spoor` (the trail/track it
arrives at), `extinction` (panic), and the `torpor` family. Plan 9's `namec` is
the heritage name; Thylacine substitutes the thematic verb, consistent with the
established `Chan -> Spoor` and `panic -> extinction` renames.

`trail` (user-signed-off 2026-06-02) — the in-call stack of resolved Spoors that
`stalk` follows and that `..` pops back along: the predator follows a *trail* of
spoors to its quarry. `quarry` — the target Spoor `stalk` returns: the thing
being stalked. The mount-crossing step (`cross_mounts` / `domount`) and the path
tokenizer keep their plain descriptive names — no outback word cleared the
clarity bar there, and the discipline is not to force it.
