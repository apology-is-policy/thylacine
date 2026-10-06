
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
static u32 burrow_charge_claim_locked(struct Burrow *v, const struct AddrSpace *as) {
    if (!as) return 0;
    // charge_pages -- not charge_as_id -- is the "held" sentinel: a charge of
    // zero pages is meaningless, so zero pages IS "nothing held". The key is
    // the ADDRESS SPACE that paid (B-1a' audit F4): a pid survives exec, and a
    // handle that outlives the outgoing space (a non-CLOEXEC Loom) would
    // otherwise refund against the successor's space, which never paid.
    // A kernel pin can retain the exact payer after its final Proc exits;
    // callers then use this AddrSpace-keyed form without a dead Proc pointer.
    // A record whose descriptor really died is never claimed: its counter died
    // too, and the physical pages return to the pool when storage is freed.
    if (v->charge_pages != 0 && v->charge_as_id == as->id) {
        u32 pages       = v->charge_pages;
        v->charge_as_id = 0;
        v->charge_pages = 0;
        return pages;
    }
    return 0;
}

u32 burrow_charge_claim_in(struct Burrow *v, const struct AddrSpace *as) {
    if (!v || !as) return 0;
    if (v->magic != VMO_MAGIC)
        extinction("burrow_charge_claim on corrupted BURROW (use-after-free?)");
    spin_lock(&v->lock);
    u32 pages = burrow_charge_claim_locked(v, as);
    spin_unlock(&v->lock);
    return pages;
}

void burrow_charge_restore_in(struct Burrow *v, const struct AddrSpace *as, u32 pages) {
    if (!v || !as || pages == 0) return;
    if (v->magic != VMO_MAGIC)
        extinction("burrow_charge_restore on corrupted BURROW (use-after-free?)");
    spin_lock(&v->lock);
    // The record must still be the one WE cleared. A held record here means
    // something re-charged the region between the claim and the restore, which
    // cannot happen: burrow_charge_record runs once, at creation, on a Burrow no
    // other path has a reference to yet. Extinct rather than pick a loser --
    // silently refusing would DROP the caller's `pages` on the floor, and a lost
    // charge is an under-count, the direction that inflates a Proc's budget.
    if (v->charge_pages != 0) {
        spin_unlock(&v->lock);
        extinction("burrow_charge_restore: the region was re-charged mid-settle");
    }
    v->charge_as_id = as->id;
    v->charge_pages = pages;
    spin_unlock(&v->lock);
}

bool burrow_unref_settled_in(struct Burrow *v, const struct AddrSpace *payer,
                             u32 *out_refund) {
    if (out_refund) *out_refund = 0;
    if (!v) return false;                      // NULL-safe, mirroring burrow_unref_freed
    if (v->magic != VMO_MAGIC)
        extinction("burrow_unref of corrupted BURROW (use-after-free?)");
    spin_lock(&v->lock);
    if (v->handle_count <= 0) {
        spin_unlock(&v->lock);
        extinction("burrow_unref of zero-ref BURROW");
    }
    v->handle_count--;
    bool should_free = (v->handle_count == 0 && v->mapping_count == 0);
    // Claim ONLY on the drop that ends the occupancy. A nonfinal drop leaves the
    // record in place for whoever does end it -- the old code cleared it here
    // and put it back afterwards, which is exactly how a racing final holder
    // came to read an empty record and refund nothing.
    u32 refund = burrow_charge_claim_locked(v, payer);
    spin_unlock(&v->lock);

    if (should_free)
        burrow_free_internal(v);
    if (out_refund) *out_refund = refund;
    return should_free;
}

struct Burrow *burrow_release_mapping_settled_deferred(struct Burrow *v,
                                                       const struct AddrSpace *payer,
                                                       u32 *out_refund) {
    if (out_refund) *out_refund = 0;
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
    // A shared-out region settles on the sharer's detach even when the drop does
    // not free it: the mapping that survives is in a Proc that cannot name the
    // payer, so there is no later settler. Read under this same lock -- monotonic
    // false -> true, so observing it here rather than before the drop can only
    // ADD a reason to settle, never miss one that mattered.
    bool shared_out = v->shared_out;
    u32  refund = (should_free || shared_out) ? burrow_charge_claim_locked(v, payer) : 0;
    spin_unlock(&v->lock);

    if (out_refund) *out_refund = refund;
    return should_free ? v : NULL;
}

bool burrow_unref_freed(struct Burrow *v) {
    return burrow_unref_settled_in(v, NULL, NULL);
}

struct Burrow *burrow_release_mapping_deferred(struct Burrow *v) {
    return burrow_release_mapping_settled_deferred(v, NULL, NULL);
}

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
