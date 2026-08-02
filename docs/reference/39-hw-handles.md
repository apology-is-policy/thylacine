# 39 — Hardware handles + capability gating [ABSORBED INTO THE VAULT]

Absorbed at the hardware-capability sweep
(`chg-2026-08-02-devices-hwcap-sweep`). Its content now lives, code-verified and
current, in:

    vault/system/kernel/devices/sub-kernel-hwcap.md
    vault/invariants/inv-i5.md

(the three objects and the three different ways their exclusivity is enforced;
the kernel-range reservations and the sentinel owner that shares their table; the
deliberate virtio relaxation and its expiry condition; the claim-path ordering
that makes rollback total; and I-5 itself, whose home this is.)

**What this file got wrong is a failure mode the earlier absorptions had not
shown: its caveat list is meticulously maintained while its structural body is
frozen at P4.**

Eleven caveats, two of them rewritten with strikethrough when the bug was closed,
each cross-referenced to the audit that closed it. That half is genuinely
excellent, and several entries are still correct today. Meanwhile the body, four
sections above it:

| Where | Claim | Truth at absorption |
|---|---|---|
| "capability bitmask" | `CAP_ALL` is `CAP_HW_CREATE` — one capability | **twelve** are defined (`CAP_HW_CREATE` … `CAP_JIT`), and `CAP_ALL` deliberately excludes some |
| "`struct Proc` size bump" | "now 128 bytes", with the matching `_Static_assert` quoted | **400** |
| "hw rejection in `handle_dup`" | "covers all four hw kinds" | **five** — the bus-function kind joined the mask |
| "capability check + rollback" | the capability test shown as the whole creation gate | the allowance's two-step check (permit, then re-check at install) landed since, and is what bounds a narrowed driver |

So a reader who needs to know *whether* a bug was fixed is well served, and a
reader who needs to know *what the thing currently is* is misled on four counts.

The pattern is worth carrying forward when reading this tree's other documents:
**the parts that look most carefully maintained are evidence about where failures
happened, not about where the document is true.** A stale caveat gets someone
hurt and gets fixed; a stale struct size hurts nobody, so nobody looks. This
document decayed exactly where being wrong had no observer — which is the same
shape as the subsystem it describes, one level up.

**What it got right and the vault kept:** the overlap predicate and its
adjacency-is-not-overlap corner; the rollback discipline and the annotated
asymmetry in the create path; the reservation rationale for each kernel range;
and caveat 2, which names the missing detach API and is independently
corroborated by `seam-gic-handler-slot-never-cleared`.

Binding design (unchanged): `docs/ARCHITECTURE.md` section 13.

---

**If you are here to add something, add it to the dossier, not to this file.**
This stub replaces the whole document, so any edit here becomes a merge conflict
— which is the intended behaviour, and the only thing that keeps main-track
knowledge from being lost silently at the next merge.
