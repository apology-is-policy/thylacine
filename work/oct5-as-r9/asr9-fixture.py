#!/usr/bin/env python3
"""AS-R9 controlled schedules: the old race, the repair, and a positive control.

Host double, not the guest. What it does and does not establish:

  DOES   -- that the SHIPPED pre-fix functions (extracted verbatim from the base
            commit) and the REPAIRED ones (extracted verbatim from the working
            tree) behave differently under one named interleaving, with the
            implementation as the single changed variable.
  DOES NOT -- prove anything about ARM weak memory, real SLUB timing, or that
            the interleaving is reachable from a particular syscall pair. The
            lock here is a cooperative counter, not a spinlock.

The allocator double mirrors the real free: burrow_free_internal clobbers magic
and hands the slot to SLUB, which does not zero it -- so a freed slot may be
(POISON) still free with magic 0, or (RECYCLE_*) already reissued as a fresh
Burrow with a valid magic. No freed host memory is ever dereferenced: the slot
is a static object whose state the double rewrites.
"""
import json, os, pathlib, shlex, subprocess, sys

W    = pathlib.Path(__file__).resolve().parent
ROOT = W.parents[1]
BASE = '5ff62b78809846af4780ec41f82d1676e7584e80'

def extract(src, name):
    """Pull `name`'s full definition verbatim: its signature line (column 0)
    through the matching column-0 '}'. Multi-line signatures handled."""
    lines = src.splitlines(keepends=True)
    for i, ln in enumerate(lines):
        if ln[:1].isalpha() and (f'{name}(' in ln) and not ln.lstrip().startswith('//'):
            # a definition, not a prototype: its line or a later one ends in '{'
            j, buf = i, []
            while j < len(lines):
                buf.append(lines[j])
                if lines[j].rstrip('\n') == '}':
                    body = ''.join(buf)
                    if '{' in body:
                        return body
                    break
                if lines[j].rstrip().endswith(';') and '{' not in ''.join(buf):
                    break          # prototype -- keep looking
                j += 1
    raise SystemExit(f'FAIL: could not extract {name}')

PRELUDE = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define VMO_MAGIC 0x42555252

enum { EXIT_OK=0, EXIT_RESTORE_DONE=1, EXIT_STALE_RESTORE=42, EXIT_FABRICATED=43,
       EXIT_UNDERCOUNT=44, EXIT_LOST_SETTLE=45, EXIT_OTHER_EXT=99 };

struct AddrSpace { u64 id; };
struct Burrow { u32 magic; int lock; int handle_count, mapping_count;
                u64 charge_as_id; u32 charge_pages; bool shared_out; };

static void (*after_unlock)(void);
static int  freed_times;
static enum { POISON, RECYCLE_CLEAN, RECYCLE_CHARGED } recycle = POISON;
static u32  refunded_to_payer;     /* pages the PAYER got back */
static u32  refunded_to_other;     /* pages an UNRELATED space got back */

static void extinction(const char *why) {
    fprintf(stderr, "extinction: %s\n", why);
    if (strstr(why, "burrow_charge_restore on corrupted")) exit(EXIT_STALE_RESTORE);
    if (strstr(why, "re-charged mid-settle"))              exit(EXIT_FABRICATED);
    exit(EXIT_OTHER_EXT);
}
static void spin_lock(int *l) { if (*l) { fprintf(stderr,"deadlock\n"); abort(); } *l = 1; }
static void spin_unlock(int *l) {
    if (!*l) abort();
    *l = 0;
    void (*hook)(void) = after_unlock; after_unlock = NULL;
    if (hook) hook();
}
/* Mirrors burrow_free_internal's tail: clobber magic, return the slot to an
   allocator that does not zero it. RECYCLE_* models the slot being reissued
   before the stale restore lands. */
static void burrow_free_internal(struct Burrow *v) {
    if (v->magic != VMO_MAGIC)  extinction("burrow_free_internal of corrupted BURROW");
    if (v->handle_count != 0)   extinction("burrow_free_internal with handle_count > 0");
    if (v->mapping_count != 0)  extinction("burrow_free_internal with mapping_count > 0");
    ++freed_times;
    v->magic = 0;
    if (recycle == RECYCLE_CLEAN) {
        *v = (struct Burrow){ .magic=VMO_MAGIC, .handle_count=1,
                              .charge_as_id=0, .charge_pages=0 };
    } else if (recycle == RECYCLE_CHARGED) {
        *v = (struct Burrow){ .magic=VMO_MAGIC, .handle_count=1,
                              .charge_as_id=9, .charge_pages=3 };
    }
}
'''

POSTLUDE = r'''
static struct Burrow b;
static struct AddrSpace payer = { 7 };   /* the space that actually paid */
static struct AddrSpace other = { 9 };   /* an unrelated space */
#define PAID 5u

/* ---- the racing holder: performs the FINAL drop inside the first holder's
   window (scheduled at the first holder's unlock). ---- */
static int racer_kind;                   /* 0 = handle drop, 1 = mapping drop */
static void racer(void) {
#ifdef SETTLED
    u32 got = 0;
    if (racer_kind == 0) { if (!burrow_unref_settled_in(&b, &payer, &got)) abort(); }
    else                 { if (!burrow_release_mapping_settled_deferred(&b, &payer, &got)) abort();
                           burrow_free_internal(&b); }
    refunded_to_payer += got;
#else
    /* Pre-fix: the racer claims first, as every old caller did, and finds the
       record the first holder momentarily cleared -- so it refunds nothing. */
    u32 got = burrow_charge_claim_in(&b, &payer);
    refunded_to_payer += got;
    if (racer_kind == 0) { if (!burrow_unref_freed(&b)) abort(); }
    else                 { if (!burrow_release_mapping_deferred(&b)) abort();
                           burrow_free_internal(&b); }
#endif
}

struct leg { const char *name; int first_mapping, racer_mapping, race, recyc, shared, want; };
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

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <leg>\n", argv[0]); return 2; }
    const struct leg *L = NULL;
    for (unsigned i = 0; i < sizeof legs / sizeof legs[0]; i++)
        if (!strcmp(legs[i].name, argv[1])) L = &legs[i];
    if (!L) { fprintf(stderr, "unknown leg %s\n", argv[1]); return 2; }
    recycle    = (int)L->recyc;
    racer_kind = L->racer_mapping;
    (void)refunded_to_other;   /* only the pre-fix legs read it back */
    (void)other;               /* only the repaired legs discriminate payers */

    /* Two holders: a handle + whichever ref the first dropper drops. The racer
       holds the OTHER one, so the first drop is always non-final. */
    b = (struct Burrow){ .magic = VMO_MAGIC,
                         .handle_count   = L->first_mapping ? 1 : 2,
                         .mapping_count  = L->first_mapping ? 1 : 0,
                         .charge_as_id   = payer.id,
                         .charge_pages   = PAID };
    if (L->racer_mapping) { b.handle_count = 1; b.mapping_count = 1;
                            if (L->first_mapping) abort(); }
    b.shared_out = L->shared != 0;

#ifdef SETTLED
    (void)L->race;
    u32 got = 0;
    if (L->race) after_unlock = racer;
    bool dead = L->first_mapping
        ? (burrow_release_mapping_settled_deferred(&b, &payer, &got) != NULL)
        :  burrow_unref_settled_in(&b, &payer, &got);
    refunded_to_payer += got;
    if (dead) burrow_free_internal(&b);
    /* The repaired contract: the non-final drop settles NOTHING, the drop that
       frees settles EXACTLY the recorded pages, and no path restores. */
    if (L->race) {
        if (got != 0)                   { fprintf(stderr,"non-final drop settled %u\n",got); return 70; }
        if (refunded_to_payer != PAID)  { fprintf(stderr,"refund %u != %u\n",refunded_to_payer,PAID); return EXIT_LOST_SETTLE; }
        if (freed_times != 1)           { fprintf(stderr,"freed %d times\n",freed_times); return 71; }
    } else if (L->shared) {
        /* Non-final drop of a SHARED-OUT region: it must settle anyway, because
           the mapping that survives is in a space that cannot name the payer, so
           no later settler exists. */
        if (dead)                       { fprintf(stderr,"shared-out leg freed unexpectedly\n"); return 77; }
        if (got != PAID)                { fprintf(stderr,"shared-out non-final drop settled %u, want %u\n",got,PAID); return EXIT_LOST_SETTLE; }
        if (b.charge_pages != 0)        { fprintf(stderr,"record not taken by the shared-out settle\n"); return 78; }
        if (freed_times != 0)           { fprintf(stderr,"freed with a holder left\n"); return 71; }
    } else {
        /* No racer: this drop is non-final, so the record must still be intact. */
        if (got != 0)                   { fprintf(stderr,"settled on a non-final drop\n"); return 70; }
        if (b.charge_pages != PAID || b.charge_as_id != payer.id) {
            fprintf(stderr,"record not retained: as=%llu pages=%u\n",
                    (unsigned long long)b.charge_as_id, b.charge_pages); return 72; }
        if (freed_times != 0)           { fprintf(stderr,"freed with a holder left\n"); return 71; }
        /* Exact-payer discrimination, on the live record. */
        if (burrow_charge_claim_in(&b, &other) != 0) { fprintf(stderr,"wrong payer claimed\n"); return 73; }
        if (burrow_charge_claim_in(&b, &payer) != PAID) { fprintf(stderr,"payer could not claim\n"); return 74; }
        if (burrow_charge_claim_in(&b, &payer) != 0) { fprintf(stderr,"claimed twice\n"); return 75; }
    }
    return EXIT_OK;
#else
    u32 paid = burrow_charge_claim_in(&b, &payer);
    if (paid != PAID) abort();
    if (L->race) after_unlock = racer;
    bool dead = L->first_mapping ? (burrow_release_mapping_deferred(&b) != NULL)
                                 :  burrow_unref_freed(&b);
    if (L->race) {
        if (dead) abort();                 /* the racer must have freed it */
        if (freed_times != 1) abort();
        if (refunded_to_payer != 0) abort(); /* the racer found an empty record */
    }
    /* The pre-fix step 3: restore through a pointer whose last ref may be gone. */
    burrow_charge_restore_in(&b, &payer, paid);
    if (!L->race) return EXIT_RESTORE_DONE;   /* positive control: restore is fine */

    /* Survived the restore => the slot was recycled with a valid magic and an
       empty record, so the PAYER's charge is now planted on an UNRELATED region.
       Settle that region the way its own owner eventually would. */
    u32 planted = burrow_charge_claim_in(&b, &payer);
    refunded_to_other += planted;
    if (planted == PAID) {
        fprintf(stderr, "I-32 under-count: %u pages refunded against a region "
                        "the payer never bought (planted by the stale restore)\n", planted);
        return EXIT_UNDERCOUNT;
    }
    fprintf(stderr, "stale restore survived but planted nothing (%u)\n", planted);
    return 76;
#endif
}
'''

def build(tag, src_text, extra):
    c = W / f'{tag}.c'
    c.write_text(PRELUDE + src_text + POSTLUDE)
    exe = W / tag
    cmd = [os.environ.get('CC', 'clang'), '-std=c11', '-Wall', '-Wextra', '-Werror',
           '-g', *extra, str(c), '-o', str(exe)] + shlex.split(os.environ.get('CFLAGS', ''))
    log = W / f'{tag}-build.log'
    with log.open('w') as f:
        r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        print(f'BUILD FAILED ({tag}) -- see {log.name}:'); print(log.read_text()); sys.exit(1)
    return exe

# ---- old source: from the BASE COMMIT, so my edits cannot launder the premise
old_src = subprocess.run(['git', '-C', str(ROOT), 'show', f'{BASE}:kernel/burrow.c'],
                         capture_output=True, text=True, check=True).stdout
OLD = ['burrow_unref_freed', 'burrow_release_mapping_deferred',
       'burrow_charge_claim_in', 'burrow_charge_restore_in']
old_text = '\n'.join(extract(old_src, n) for n in OLD)

# ---- new source: the working tree
new_src = (ROOT / 'kernel/burrow.c').read_text()
# Order matters: the settled forms are now the single implementation and the
# unsettled ones are wrappers over them, so a definition must precede its use or
# C99 + -Werror rejects the implicit declaration.
NEW = ['burrow_charge_claim_locked', 'burrow_charge_claim_in', 'burrow_charge_restore_in',
       'burrow_unref_settled_in', 'burrow_release_mapping_settled_deferred',
       'burrow_unref_freed', 'burrow_release_mapping_deferred']
new_text = '\n'.join(extract(new_src, n) for n in NEW)

print(f'extracted {len(OLD)} pre-fix functions from {BASE[:9]} ({len(old_text)} bytes)')
print(f'extracted {len(NEW)} repaired functions from the working tree ({len(new_text)} bytes)')
for n in NEW:
    assert extract(new_src, n) in new_src
print('all extracted bodies are verbatim substrings of their source')

old_exe = build('asr9-old', old_text, [])
new_exe = build('asr9-new', new_text, ['-DSETTLED'])

LEGS = [
  ('old-handle-handle',    old_exe, 42, 'non-final HANDLE drop, racing final handle drop -> stale restore'),
  ('old-mapping-handle',   old_exe, 42, 'non-final MAPPING drop, racing final handle drop -> stale restore'),
  ('old-handle-mapping',   old_exe, 42, 'non-final HANDLE drop, racing final MAPPING drop -> stale restore'),
  ('old-recycled-clean',   old_exe, 44, 'slot reissued empty -> charge planted on an unrelated region (I-32)'),
  ('old-recycled-charged', old_exe, 43, 'slot reissued charged -> FABRICATED "re-charged mid-settle"'),
  ('control-no-race',      old_exe,  1, 'CONTROL: same sequence, no racer -> restore completes normally'),
  ('new-handle-handle',    new_exe,  0, 'repaired: settled under one lock, refunded exactly once'),
  ('new-mapping-handle',   new_exe,  0, 'repaired: mapping drop settled under one lock'),
  ('new-handle-mapping',   new_exe,  0, 'repaired: handle drop, racing final mapping drop'),
  ('new-no-race',          new_exe,  0, 'repaired: non-final drop RETAINS the record; exact-payer still holds'),
  ('new-shared-out-settles',new_exe,  0, 'repaired: shared-out non-final MAPPING drop settles (no later settler exists)'),
  ('new-not-shared-retains',new_exe,  0, 'repaired: same drop, NOT shared out -> record retained (the pair`s control)'),
]
results, bad = [], 0
for name, exe, want, what in LEGS:
    p = subprocess.run([str(exe), name], capture_output=True, text=True, timeout=30)
    (W / f'leg-{name}.log').write_text(p.stdout + p.stderr)
    ok = (p.returncode == want)
    bad += (not ok)
    print(f'  {"PASS" if ok else "FAIL"}  {name:22s} exit={p.returncode:<3d} want={want:<3d} {what}')
    if not ok and p.stderr.strip():
        print(f'        stderr: {p.stderr.strip()[:300]}')
    results.append({'leg': name, 'exit': p.returncode, 'want': want,
                    'pass': ok, 'what': what, 'stderr': p.stderr.strip()})

print(f'\n{len(LEGS) - bad}/{len(LEGS)} legs as expected')

# ---------------------------------------------------------------- mutants
# A leg that has never been red proves nothing about its own sensitivity. Each
# mutation below is a single named edit to the REPAIRED source that must drive a
# NAMED leg to a SPECIFIC exit code. Requiring the exact code, not merely
# "nonzero", is what makes these discriminating rather than just detecting: a
# mutant that dies the wrong way is a finding about the fixture, not a pass.
MUTANTS = [
  ('M1-claim-on-every-drop',
   'u32 refund = should_free ? burrow_charge_claim_locked(v, payer) : 0;',
   'u32 refund = burrow_charge_claim_locked(v, payer);',
   'new-handle-handle', 70,
   'claiming on a NON-FINAL drop re-opens the window: the drop that frees finds nothing'),
  ('M2-ignore-shared-out',
   'u32  refund = (should_free || shared_out) ? burrow_charge_claim_locked(v, payer) : 0;',
   'u32  refund = should_free ? burrow_charge_claim_locked(v, payer) : 0;\n    (void)shared_out;',
   'new-shared-out-settles', 45,
   'a shared-out region has no later settler, so skipping it strands the charge'),
  ('M3-always-settle-mapping',
   'u32  refund = (should_free || shared_out) ? burrow_charge_claim_locked(v, payer) : 0;',
   'u32  refund = burrow_charge_claim_locked(v, payer);\n    (void)shared_out;',
   'new-not-shared-retains', 70,
   'settling a non-final, not-shared-out mapping drop steals the record'),
  ('M4-exact-payer-ignored',
   'if (v->charge_pages != 0 && v->charge_as_id == as->id) {',
   'if (v->charge_pages != 0) {',
   'new-no-race', 73,
   'dropping the AddrSpace check refunds a space that never paid for the region'),
  ('M5-claim-does-not-clear',
   '''        u32 pages       = v->charge_pages;
        v->charge_as_id = 0;
        v->charge_pages = 0;
        return pages;''',
   '''        u32 pages       = v->charge_pages;
        return pages;''',
   'new-no-race', 75,
   'without the clear the refund is no longer exactly-once'),
]
mut_results, mut_bad = [], 0
print('\nmutants (each must redden its named leg with its named code):')
for name, frm, to, leg, want, why in MUTANTS:
    if new_text.count(frm) != 1:
        print(f'  ABORT {name}: anchor appears {new_text.count(frm)}x in the repaired source'); mut_bad += 1
        mut_results.append({'mutant': name, 'error': 'anchor not unique'}); continue
    exe = build(f'mut-{name}', new_text.replace(frm, to, 1), ['-DSETTLED'])
    p = subprocess.run([str(exe), leg], capture_output=True, text=True, timeout=30)
    (W / f'mutant-{name}.log').write_text(p.stdout + p.stderr)
    ok = (p.returncode == want)
    mut_bad += (not ok)
    print(f'  {"PASS" if ok else "FAIL"}  {name:26s} {leg:24s} exit={p.returncode:<3d} want={want:<3d} {why}')
    if not ok:
        print(f'        a mutant that does not redden as predicted is a FIXTURE finding, not a pass')
        if p.stderr.strip(): print(f'        stderr: {p.stderr.strip()[:200]}')
    mut_results.append({'mutant': name, 'leg': leg, 'exit': p.returncode, 'want': want,
                        'pass': ok, 'why': why})

(W / 'asr9-fixture.json').write_text(json.dumps({
    'base': BASE, 'cc': os.environ.get('CC', 'clang'),
    'old_functions': OLD, 'new_functions': NEW,
    'legs': results, 'legs_all_passed': bad == 0,
    'mutants': mut_results, 'mutants_all_reddened': mut_bad == 0,
    'all_passed': bad == 0 and mut_bad == 0}, indent=2))
print(f'\n{len(LEGS)-bad}/{len(LEGS)} legs, {len(MUTANTS)-mut_bad}/{len(MUTANTS)} mutants'
      f' -> asr9-fixture.json')
sys.exit(1 if (bad or mut_bad) else 0)
