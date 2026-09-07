# 138 — gpud: the resident virtio-gpu driver (G-1) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-gpud-doc-absorb`).
**This was already a retired historical record** — gpud (`usr/gpud`, the G-1
stage-0 device-owning half) was absorbed by tapestryd at G-3 and the crate +
manifest entry were deleted (one exclusive claimant per function; `usr/gpud` no
longer exists in the tree). Its living content — the measured transport-pivot
rationale, the pattern-persists gate, and the command machinery generalized into
the successor — lives, code-verified and current, in:

- the **successor compositor** — tapestryd's `gpu.rs` IS this driver's command
  machinery generalized to per-surface resources; the `virtio-pci:16` binding via
  the warden `gather` manifest; and the transport-pivot rationale preserved
  verbatim-in-spirit ("the six populated virtio-mmio slots share one page whose
  lifetime belongs to stratumd, so a second persistent MMIO claimant is
  structurally impossible"):

      vault/system/userspace/services/sub-tapestryd.md   (audit: hard)

- the **pattern-persists / gpu-gate** — `screendump.sh -v`, the post-banner
  liveness-compare verdict step every `test.sh` boot runs:

      vault/system/substrate/sub-substrate-gates.md

- the **warden manifest** — the `gather`-mode bind + the I-34 allowance narrowed
  to exactly the bound functions:

      vault/system/userspace/boot-chain/sub-warden.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold of an already-retired scaffold.** The
  doc's own banner said "RETIRED at G-3"; every living atom moved to the successor.
  The G-1-specific machinery it describes (the self-contained ~700-line
  `usr/gpud/src/main.rs`, the crash-probe re-home to the `restart-test` synthetic
  node, the cursorq-configured-but-unused detail) describes deleted code and is
  correctly left as history.
- **The transport decision is the durable finding** — QEMU-virt packs six
  virtio-mmio slots into one 4-KiB page, so a resident MMIO claimant starves
  stratumd's disk claim (`rc=-207`, boot-fatal); the fix (virtio-gpu-**pci**,
  per-function BARs) is what tapestryd inherited. That reasoning is carried by
  sub-tapestryd, not lost with the scaffold.
