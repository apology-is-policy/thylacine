#!/usr/bin/env python3
"""Self-audit fix: ONE implementation per drop decision.

The repair left the {0,0} dual-counter free decision written twice -- once in
burrow_unref_freed / burrow_release_mapping_{freed,deferred} and again in the
settled forms. That is the same drift hazard the commit message praises avoiding
for the CLAIM, left in place for the DROP. Make the unsettled forms thin wrappers
over the settled ones, so there is exactly one copy of each decision.

The settled forms' extinction bodies are reworded to the HISTORICAL texts so the
wrappers keep their exact existing messages. Verified safe: none of the six
extinction bodies tools/test-fault.sh matches is a burrow drop message, and no
file under tools/, usr/ or specs/ matches any of these strings, so they are prose
per the 2026-09-25 operator vote.
"""
import sys, pathlib
R = pathlib.Path(__file__).resolve().parents[2]
edits = []
def ed(p, old, new, label): edits.append((R / p, old, new, label))
B = 'kernel/burrow.c'

# --- settled forms adopt the historical message texts
for frm, to, lbl in [
    ('extinction("burrow_unref_settled of corrupted BURROW (use-after-free?)")',
     'extinction("burrow_unref of corrupted BURROW (use-after-free?)")', 'unref corrupted msg'),
    ('extinction("burrow_unref_settled of zero-ref BURROW")',
     'extinction("burrow_unref of zero-ref BURROW")', 'unref zero-ref msg'),
    ('extinction("burrow_release_mapping_settled(NULL)")',
     'extinction("burrow_release_mapping(NULL)")', 'mapping NULL msg'),
    ('extinction("burrow_release_mapping_settled of corrupted BURROW (use-after-free?)")',
     'extinction("burrow_release_mapping of corrupted BURROW (use-after-free?)")', 'mapping corrupted msg'),
    ('extinction("burrow_release_mapping_settled of zero-mapping BURROW")',
     'extinction("burrow_release_mapping of zero-mapping BURROW")', 'mapping zero msg'),
]:
    ed(B, frm, to, lbl)

# --- burrow_unref_freed becomes a wrapper
ed(B,
"""bool burrow_unref_freed(struct Burrow *v) {
    if (!v) return false;                      // NULL-safe
    if (v->magic != VMO_MAGIC)
        extinction("burrow_unref of corrupted BURROW (use-after-free?)");
    // #847: decrement + the dual-counter free decision under v->lock; the
    // free runs OUTSIDE the lock (leaf discipline -- see file header).
    spin_lock(&v->lock);
    if (v->handle_count <= 0) {
        spin_unlock(&v->lock);
        extinction("burrow_unref of zero-ref BURROW");
    }
    v->handle_count--;
    // Dual-check: free only when BOTH counts reach 0. Maps to the spec's
    // NoUseAfterFree iff invariant — premature free violates (counts > 0 ∧
    // pages dead); delayed free violates (counts = 0 ∧ pages alive). Exactly
    // one racing unref/release_mapping sees the 0,0 edge.
    bool should_free = (v->handle_count == 0 && v->mapping_count == 0);
    spin_unlock(&v->lock);

    if (should_free)
        burrow_free_internal(v);
    return should_free;
}
""",
"""// #847: decrement + the dual-counter free decision under v->lock, free OUTSIDE
// it (leaf discipline -- see file header). Dual-check: free only when BOTH
// counts reach 0, which maps to the spec's NoUseAfterFree iff invariant --
// premature free violates (counts > 0 and pages dead), delayed free violates
// (counts = 0 and pages alive), and exactly one racing unref/release_mapping
// sees the 0,0 edge.
//
// AS-R9 self-audit: that decision now lives in ONE place --
// burrow_unref_settled_in -- and this is it with no payer, so the {0,0} rule
// cannot drift between the settled and unsettled spellings. A charge-settling
// caller wants the settled form directly; this one exists for the callers that
// hold no charge record.
bool burrow_unref_freed(struct Burrow *v) {
    return burrow_unref_settled_in(v, NULL, NULL);
}
""",
'burrow_unref_freed -> wrapper')

# --- burrow_release_mapping_freed becomes a wrapper
ed(B,
"""bool burrow_release_mapping_freed(struct Burrow *v) {
    if (!v)                       extinction("burrow_release_mapping(NULL)");
    if (v->magic != VMO_MAGIC)
        extinction("burrow_release_mapping of corrupted BURROW (use-after-free?)");
    // #847: symmetric with burrow_unref -- decrement + dual-check under
    // v->lock, free outside.
    spin_lock(&v->lock);
    if (v->mapping_count <= 0) {
        spin_unlock(&v->lock);
        extinction("burrow_release_mapping of zero-mapping BURROW");
    }
    v->mapping_count--;
    bool should_free = (v->handle_count == 0 && v->mapping_count == 0);
    spin_unlock(&v->lock);

    if (should_free)
        burrow_free_internal(v);
    return should_free;
}
""",
"""// #847: symmetric with burrow_unref -- decrement + dual-check under v->lock,
// free outside. AS-R9 self-audit: the decision lives once, in
// burrow_release_mapping_settled_deferred; this is it with no payer, plus the
// inline free its non-deferred contract promises.
bool burrow_release_mapping_freed(struct Burrow *v) {
    struct Burrow *dead = burrow_release_mapping_settled_deferred(v, NULL, NULL);
    if (dead)
        burrow_free_internal(dead);
    return dead != NULL;
}
""",
'burrow_release_mapping_freed -> wrapper')

# --- burrow_release_mapping_deferred becomes a wrapper (it had NO callers left)
ed(B,
"""struct Burrow *burrow_release_mapping_deferred(struct Burrow *v) {
    if (!v)                       extinction("burrow_release_mapping(NULL)");
    if (v->magic != VMO_MAGIC)
        extinction("burrow_release_mapping of corrupted BURROW (use-after-free?)");
    spin_lock(&v->lock);
    if (v->mapping_count <= 0) {
        spin_unlock(&v->lock);
        extinction("burrow_release_mapping of zero-mapping BURROW");
    }
    v->mapping_count--;
    bool should_free = (v->handle_count == 0 && v->mapping_count == 0);
    spin_unlock(&v->lock);
    return should_free ? v : NULL;
}
""",
"""// AS-R9 self-audit: migrating vma_free_deferred to the settled form left this
// with no callers at all AND a second copy of the {0,0} decision. Kept as the
// no-payer spelling of the settled form rather than deleted, because it is the
// published name for "drop a mapping, hand back the dead Burrow, settle
// nothing" and a caller holding no charge record should not have to pass NULLs.
struct Burrow *burrow_release_mapping_deferred(struct Burrow *v) {
    return burrow_release_mapping_settled_deferred(v, NULL, NULL);
}
""",
'burrow_release_mapping_deferred -> wrapper')

texts, fail = {}, False
for path, old, new, label in edits:
    t = texts.get(path)
    if t is None: t = texts[path] = path.read_text()
    n = t.count(old)
    if n != 1:
        print(f'ABORT [{label}]: anchor occurs {n} times, expected 1'); fail = True
    else:
        texts[path] = t.replace(old, new, 1); print(f'  ok  [{label}]')
if fail:
    print('NOTHING WRITTEN'); sys.exit(1)
for path, t in texts.items():
    path.write_text(t); print(f'wrote {path.relative_to(R)}')
