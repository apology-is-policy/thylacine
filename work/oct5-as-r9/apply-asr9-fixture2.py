#!/usr/bin/env python3
"""Add the shared_out discrimination pair to the fixture; migrate the
async-owner settle in test_addrspace.c onto the settled drop."""
import sys, pathlib
R = pathlib.Path(__file__).resolve().parents[2]
W = pathlib.Path(__file__).resolve().parent
edits = []
def ed(p, old, new, label): edits.append((p, old, new, label))

F = W / 'asr9-fixture.py'

ed(F,
"""struct leg { const char *name; int first_mapping, racer_mapping, race, recyc, want; };
static const struct leg legs[] = {
  /* name                     first  racer  race recycle         expected        */
  { "old-handle-handle",          0,    0,   1, POISON,          EXIT_STALE_RESTORE },
  { "old-mapping-handle",         1,    0,   1, POISON,          EXIT_STALE_RESTORE },
  { "old-handle-mapping",         0,    1,   1, POISON,          EXIT_STALE_RESTORE },
  { "old-recycled-clean",         0,    0,   1, RECYCLE_CLEAN,   EXIT_UNDERCOUNT    },
  { "old-recycled-charged",       0,    0,   1, RECYCLE_CHARGED, EXIT_FABRICATED    },
  { "control-no-race",            0,    0,   0, POISON,          EXIT_RESTORE_DONE  },
  { "new-handle-handle",          0,    0,   1, POISON,          EXIT_OK            },
  { "new-mapping-handle",         1,    0,   1, POISON,          EXIT_OK            },
  { "new-handle-mapping",         0,    1,   1, POISON,          EXIT_OK            },
  { "new-no-race",                0,    0,   0, POISON,          EXIT_OK            },
};
""",
"""struct leg { const char *name; int first_mapping, racer_mapping, race, recyc, shared, want; };
static const struct leg legs[] = {
  /* name                     first racer race recycle         shared expected       */
  { "old-handle-handle",          0,   0,   1, POISON,          0, EXIT_STALE_RESTORE },
  { "old-mapping-handle",         1,   0,   1, POISON,          0, EXIT_STALE_RESTORE },
  { "old-handle-mapping",         0,   1,   1, POISON,          0, EXIT_STALE_RESTORE },
  { "old-recycled-clean",         0,   0,   1, RECYCLE_CLEAN,   0, EXIT_UNDERCOUNT    },
  { "old-recycled-charged",       0,   0,   1, RECYCLE_CHARGED, 0, EXIT_FABRICATED    },
  { "control-no-race",            0,   0,   0, POISON,          0, EXIT_RESTORE_DONE  },
  { "new-handle-handle",          0,   0,   1, POISON,          0, EXIT_OK            },
  { "new-mapping-handle",         1,   0,   1, POISON,          0, EXIT_OK            },
  { "new-handle-mapping",         0,   1,   1, POISON,          0, EXIT_OK            },
  { "new-no-race",                0,   0,   0, POISON,          0, EXIT_OK            },
  /* The shared_out discrimination pair: one variable apart. A non-final mapping
     drop must settle when the region is shared out (the surviving foreign
     mapping cannot name the payer) and must RETAIN the record when it is not. */
  { "new-shared-out-settles",     1,   0,   0, POISON,          1, EXIT_OK            },
  { "new-not-shared-retains",     1,   0,   0, POISON,          0, EXIT_OK            },
};
""",
'fixture: add the shared_out pair to the leg table')

ed(F,
"""    if (L->racer_mapping) { b.handle_count = 1; b.mapping_count = 1;
                            if (L->first_mapping) abort(); }
""",
"""    if (L->racer_mapping) { b.handle_count = 1; b.mapping_count = 1;
                            if (L->first_mapping) abort(); }
    b.shared_out = L->shared != 0;
""",
'fixture: honour the shared flag')

ed(F,
"""    } else {
        /* No racer: this drop is non-final, so the record must still be intact. */
        if (got != 0)                   { fprintf(stderr,"settled on a non-final drop\\n"); return 70; }
""",
"""    } else if (L->shared) {
        /* Non-final drop of a SHARED-OUT region: it must settle anyway, because
           the mapping that survives is in a space that cannot name the payer, so
           no later settler exists. */
        if (dead)                       { fprintf(stderr,"shared-out leg freed unexpectedly\\n"); return 77; }
        if (got != PAID)                { fprintf(stderr,"shared-out non-final drop settled %u, want %u\\n",got,PAID); return EXIT_LOST_SETTLE; }
        if (b.charge_pages != 0)        { fprintf(stderr,"record not taken by the shared-out settle\\n"); return 78; }
        if (freed_times != 0)           { fprintf(stderr,"freed with a holder left\\n"); return 71; }
    } else {
        /* No racer: this drop is non-final, so the record must still be intact. */
        if (got != 0)                   { fprintf(stderr,"settled on a non-final drop\\n"); return 70; }
""",
'fixture: the shared-out branch')

ed(F,
"""  ('new-no-race',          new_exe,  0, 'repaired: non-final drop RETAINS the record; exact-payer still holds'),
""",
"""  ('new-no-race',          new_exe,  0, 'repaired: non-final drop RETAINS the record; exact-payer still holds'),
  ('new-shared-out-settles',new_exe,  0, 'repaired: shared-out non-final MAPPING drop settles (no later settler exists)'),
  ('new-not-shared-retains',new_exe,  0, 'repaired: same drop, NOT shared out -> record retained (the pair`s control)'),
""",
'fixture: run the shared_out pair')

# ---- test_addrspace.c: the async-owner settle goes through the settled drop
ed(R / 'kernel/test/test_addrspace.c',
"""    u32 settled = burrow_charge_claim_in(b, as);
    u32 before = __atomic_load_n(&as->page_count, __ATOMIC_ACQUIRE);
    bool freed = burrow_unref_freed(b);
    if (freed) addrspace_uncharge_pages(as, settled);
    else burrow_charge_restore_in(b, as, settled);
""",
"""    // AS-R9: settle THROUGH the drop. This is the case the exact-payer form
    // exists for -- the paying Proc is gone and only an AddrSpace pin names the
    // payer -- and it is exactly the shape the old claim-drop-restore sequence
    // made racy: a sibling holder's final drop between the drop and the restore
    // left this path writing through freed storage.
    u32 before = __atomic_load_n(&as->page_count, __ATOMIC_ACQUIRE);
    u32 settled = 0;
    bool freed = burrow_unref_settled_in(b, as, &settled);
    if (settled) addrspace_uncharge_pages(as, settled);
""",
'test_addrspace.c: async-owner settle via the settled drop')

texts, fail = {}, False
for path, old, new, label in edits:
    t = texts.get(path)
    if t is None: t = texts[path] = path.read_text()
    n = t.count(old)
    if n != 1:
        print(f'ABORT [{label}]: anchor occurs {n} times in {path.name}, expected 1'); fail = True
    else:
        texts[path] = t.replace(old, new, 1); print(f'  ok  [{label}]')
if fail:
    print('NOTHING WRITTEN'); sys.exit(1)
for path, t in texts.items():
    path.write_text(t); print(f'wrote {path}')
