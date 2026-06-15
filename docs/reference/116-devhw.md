# 116 — devhw: the DTB hardware inventory, as a walkable tree

**Status**: devhw-1 landed (the Dev + the `lib/dtb.c` tree-walk API + kernel
tests). The namespace mount (`/hw`) + boot reachability is devhw-2. The privilege
layer that gates *acting* on what this tree reveals — the hardware allowance
(invariant I-34) — is the next Menagerie build-arc chunk; devhw itself adds no
new invariant.

Scripture: `docs/MENAGERIE.md` §7 (the DTB source) + ARCHITECTURE.md §22.7.
This is the **one discovery source the kernel provides**; every other source
(PCIe, USB, SDIO, overlay/EEPROM) is a userspace driver registered with the
warden.

---

## Purpose

`devhw` (`dc='H'`, name `"hw"`) publishes the parsed flattened device tree (FDT)
as a navigable namespace, so the warden + userspace drivers can **enumerate and
read** hardware rather than hardcode it. It is the honest enforcement of **I-15**
("the hardware view derives entirely from the DTB"): the only channel through
which a driver learns its device's MMIO window, interrupt, and `compatible`
strings.

```
/hw                         the FDT root node                (directory)
/hw/<node>                  a sub-node                       (directory)
/hw/<node>/<prop>           a property                       (file: raw bytes)
/hw/cpus/cpu@0/reg          example: a leaf property's value
```

Each FDT node is a directory whose entries are its sub-nodes (directories) and
its properties (files). A property file holds the property's **raw on-wire
bytes**, big-endian, exactly as the DTB stores them — the reader decodes (a
`reg` pair is `#address-cells + #size-cells` big-endian cells, etc.).

`devhw` is **read-only** (an inventory): `create` / `write` / `rename` /
`unlink` all fail. It is `perm_enforced = false` — **visibility, not
authority**. Reading the hardware layout is not a privileged act (a driver must
see its device's `reg`/`interrupts` to bind), and the DTB is not secret. The
privilege boundary is the *allowance* (I-34), which gates minting a
`KObj_MMIO`/`IRQ`/`DMA` handle over a discovered range — not reading this tree.

---

## Layering

Two layers, deliberately split:

- **`lib/dtb.c` — the FDT tree-walk API.** Owns all FDT-format knowledge.
  Exposes the node hierarchy by structure-block byte offsets (below). Every
  entry point bounds-checks its caller-supplied offset before forming a
  pointer.
- **`kernel/devhw.c` — the Dev / namespace mapping.** Maps namespace paths +
  qids onto those offsets, implements the `struct Dev` vtable, emits dirents.
  No FDT-format parsing lives here.

This mirrors the point-lookup split that already existed (`dtb_get_compat_reg`
et al. in `lib/dtb.c`): the new functions are the **enumeration** counterpart
the point-lookup API could not provide.

---

## Public API (`lib/dtb.c`)

```c
#define DTB_NODE_ROOT 0u   // the root node's structure-block offset

struct dtb_node_entry {
    bool        is_node;   // true: a sub-node (dir); false: a property (file)
    u32         off;       // struct-block offset of the child BEGIN_NODE / FDT_PROP token
    const char *name;      // unit-name (sub-node) or property name; NUL-terminated; into the DTB blob
    u32         namelen;   // strlen(name)
    const u8   *data;      // property value (NULL for a sub-node)
    u32         datalen;   // property length (0 for a sub-node)
};

// Validate `node_off` names a BEGIN_NODE; return its unit-name (root's is "").
bool dtb_node_at(u32 node_off, const char **out_name, u32 *out_namelen);

// Iterate a node's DIRECT contents (sub-nodes + properties) in document order.
// *cursor starts at 0; each call fills *out + advances *cursor (an opaque
// resume offset, strictly increasing, never 0 after the first entry); returns
// false at end-of-node. Single-entry-per-call.
bool dtb_node_iter(u32 node_off, u32 *cursor, struct dtb_node_entry *out);

// Read a property by the structure-block offset of its FDT_PROP token.
bool dtb_prop_at(u32 prop_off, const char **out_name,
                 const u8 **out_data, u32 *out_len);

// Parent node offset (for ".."). Root is its own parent. O(tree) (the FDT
// stores no parent pointer); ".." is normally resolver-handled (stalk).
bool dtb_node_parent(u32 node_off, u32 *out_parent_off);
```

All four return `false` (a clean rejection, never a wild read) if `dtb_init`
has not run or the offset is out of range / mistyped.

---

## Qid encoding (`kernel/devhw.c`)

A qid.path is a structure-block byte offset plus a one-bit type tag:

```
bit 63 = 0  ->  a NODE directory ;  low bits = the node's BEGIN_NODE offset
bit 63 = 1  ->  a PROPERTY file  ;  low bits = the property's FDT_PROP offset
```

- The root node's `BEGIN_NODE` is at offset 0, so the root qid.path is **0** —
  the conventional Dev-root path (`dev_simple_attach` hands it out).
- Offsets are `< size_struct` (`< 4 GiB`; realistically `< 1 MiB`), so the
  63-bit field never overflows and node/property qids never collide.
- Offsets are **stable for the kernel's lifetime**: the relocated DTB buffer
  (`dtb_relocate_to_buffer`) is immutable, so a qid handed out at walk time
  resolves identically forever.

Helpers: `hw_qid_is_prop` / `hw_qid_off` / `hw_qid_for_node` /
`hw_qid_for_prop`.

---

## Implementation notes

- **`walk`** follows the established reuse-`nc` contract (the #57a lesson, copied
  verbatim from `devdev`): a non-NULL `nc` is the caller's pre-clone and is
  returned AS `wq->spoor` with its qid advanced; a 0-element walk returns `nc`
  unchanged with `nqid == 0` — the shape `clone_walk_zero` needs to cross a
  mount (devhw-2). `nc == NULL` is the direct-call shape (kernel tests).
  `walk_one` resolves `.` (stays), `..` (via `dtb_node_parent`), and a name (by
  scanning the node's direct contents via `dtb_node_iter`).
- **`read`** returns a property's raw bytes at the byte offset; a directory
  Spoor returns `-1` (readdir held). `dtb_prop_at` re-validates the offset.
- **`readdir`** emits the Thylacine 9P2000.L dirent wire format
  (`qid(13) + offset(8 LE) + type(1) + name_len(2 LE) + name`) the
  `SYS_READDIR` handler parses. The dirent **offset cookie is the
  `dtb_node_iter` resume cursor** — a structure-block byte offset. This is sound
  against the handler's load-bearing pagination contract because the cursor is
  **strictly increasing** across the enumeration and **never 0** (a node's body
  begins past its own `BEGIN_NODE` token), so the handler's strict-monotonic /
  non-zero-cookie requirement (and its #955 non-advancing-cursor guard) hold.
  Whole entries only; a first entry that does not fit returns `-1` (per the ABI,
  `0` means end-of-directory).
- **`stat_native`** reports `T_S_IFDIR | 0555` (size 0) for a node directory and
  `T_S_IFREG | 0444` (size = property length) for a property file; owner
  `PRINCIPAL_SYSTEM` / `GID_SYSTEM`.
- **Non-seekable** (`seekable = false`). `SYS_LSEEK` is rejected. Sequential
  property reads still work (`sys_read` tracks + advances `c->offset`), and —
  load-bearing — a directory fd's readdir cursor (a raw byte offset in
  `c->offset`) cannot be `lseek`'d to a mid-token position and made to misparse.
  An inventory/control Dev needs no explicit seek (matches devproc / devctl).

### FDT subtree iteration

`dtb_node_iter` positions a streaming walker (`struct fdt_walker` + the existing
`walker_next`, which bounds every read at `cur + 4 <= end`) at the node body and
decodes one token: a direct `FDT_PROP` yields a property entry; a direct
`FDT_BEGIN_NODE` yields a sub-node and then **skips that child's whole subtree**
(advancing until the relative depth returns to 0) so the resume cursor lands on
the next direct entry; an `FDT_END_NODE` (the node's own close) or `FDT_END`
ends the node. Document order is preserved.

---

## Data structures

`struct dtb_node_entry` (above) — a transient per-call descriptor, not stored.
`name` / `data` point into the kernel-lifetime DTB blob; callers must not write
through them. No new persistent state; `devhw` holds no per-instance data (the
DTB buffer is the single source).

---

## Spec cross-reference

No formal module. The invariant `devhw` enforces is **I-15** (hardware view from
DTB), validated by prose + the tests. The FDT-parse soundness is bounded reads +
the firmware-trusted-DTB threat model (below), not a TLA+ obligation.

---

## Tests (`kernel/test/test_devhw.c`, 10 cases)

| Test | Covers |
|---|---|
| `devhw.bestiary_smoke` | registration (`dc='H'`, name `"hw"`), `perm_enforced == false` |
| `devhw.attach_returns_root` | root Spoor: qid.path 0, QTDIR |
| `devhw.walk_node_and_prop` | a sub-node walks to QTDIR; a property to QTFILE |
| `devhw.walk_deep_and_dotdot` | `/cpus/cpu@0`; `..` climbs to the parent (`dtb_node_parent`) |
| `devhw.walk_miss` | unknown name misses; no walk descends from a property leaf |
| `devhw.walk_reuse_nc` | the reuse-`nc` path: returned spoor IS `nc`, qid advanced (#57a) |
| `devhw.prop_read` | raw bytes at offset; EOF at/past len; partial reads; read-on-dir `-1` |
| `devhw.stat_native` | dir = IFDIR; prop = IFREG + size; SYSTEM-owned |
| `devhw.readdir_cookie_contract` | **load-bearing**: chunked == single-call; cookies strictly increasing + non-zero |
| `devhw.iter_api` | direct `dtb_node_iter` (nodes + props present, cursor advances); bad offsets rejected |

Tests run against the real boot DTB (QEMU virt under the harness; production is
`KERNEL_TESTS=OFF`, so relying on the QEMU-virt shape — `#address-cells`,
`/cpus`, `cpu@0` — is sound).

---

## Error paths

- `walk` to a missing name → `nqid` short (the miss). From a property leaf →
  miss.
- `read` on a directory → `-1`; at/past EOF → `0`; `off < 0` / `n < 0` → `-1`.
- `readdir` on a property file → `-1`; first entry larger than the buffer →
  `-1` (never `0`, which is EOD); empty node → `0` (EOD).
- `stat_native` on an invalid qid (a bad offset) → `-1`.
- `write` / `create` / `rename` / `unlink` → `-1` / NULL (read-only).
- `dtb_node_at` / `dtb_prop_at` / `dtb_node_iter` / `dtb_node_parent` on an
  out-of-range or mistyped offset → `false`.

---

## Performance

Reads are O(props-in-node) to seek a property by re-scanning from the node body;
`readdir` is O(1) per entry via the byte-offset cursor (no re-scan across calls);
`dtb_node_parent` is O(tree) (re-walk; only the rare `..`). Nodes have a handful
of entries; the whole DTB is ~1 KiB (QEMU virt) to ~30 KiB (real boards). No
allocation on the hot path; no locks (the DTB buffer is immutable post-boot).

---

## Known caveats / footguns

- **Firmware-trusted DTB (v1.0 threat model).** The DTB comes from firmware /
  QEMU and is validated at `dtb_init` (magic + version). A *hostile* DTB is a
  hostile bootloader, which is out of scope at v1.0 (you trust your firmware).
  The tree-walk functions still bound every read defensively (offset within the
  block, token-type check, bounded name scan) so a *malformed* DTB is rejected,
  not followed — but the design does not defend against a deliberately
  adversarial DTB. The warden's discovery-source-trust prosecution (MENAGERIE
  §11) is the place that question is answered, when userspace sources land.
- **Property bytes are raw + big-endian.** `/hw/.../reg` is `#address-cells +
  #size-cells` big-endian cells, not a decoded `(base, size)` — the reader
  decodes. This is deliberate: `devhw` publishes the DTB faithfully; cell-size
  interpretation belongs to the consumer (the warden / `libdriver`).
- **Name collisions** between a property and a sub-node of the same name are
  resolved in document order by `walk` (first match wins). FDT names make this
  effectively impossible (properties are lowercase-dashed; nodes are
  `name@addr`); it is noted, not defended.
- **Not yet mounted.** devhw-1 registers the Dev but does not graft `/hw` into
  the boot namespace — that is devhw-2 (the `/srv`/`/proc`/`/dev` mount idiom +
  the post-pivot re-graft). Until then `devhw` is reachable only by the kernel
  tests (direct `attach`/`walk`).
