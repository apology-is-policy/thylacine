# 87 — pouch fstat + lseek [ABSORBED INTO THE VAULT]

Absorbed at the pouch sweep (`chg-2026-08-01-pouch-sweep`). Its content
now lives, code-verified and current, in:

    vault/system/boundary/pouch-seam/sub-pouch-fs.md

(the `t_stat` translation, `open()` → the patched `openat()`, and the
whole path surface that grew around them.)

**What this file got WRONG by the time it was absorbed.** It documents
`struct t_stat` as **80 bytes** with a 16-row layout table ending at
`gid@76`. The struct has been **88 bytes** since #100 appended
`devno@80`, and all three pouch mirrors of it carry
`_Static_assert(sizeof(struct t_stat) == 88)`.

The document states, correctly, the rule it went on to break: "A future
kernel field add MUST bump both the size and the assertions; an old
consumer reading an 80-byte slot from a larger future producer would
silently see zeros in the new fields." The field add happened; the
mirrors were updated; this record of them was not — which makes it a
seventh mirror, and the only one with no assert to catch it.

Also stale: "devramfs subdir walks still deferred — `/etc/stratum/` and
similar nested paths fail with `-ENOENT`". devramfs has synthetic
directories and the resolver crosses mounts.

Binding design (unchanged): `docs/POUCH-DESIGN.md`.
