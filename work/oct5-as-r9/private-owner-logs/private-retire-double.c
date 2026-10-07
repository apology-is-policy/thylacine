
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint64_t u64;

#define VMO_MAGIC 0x4255525257214f21ull

typedef int spin_lock_t;
static void spin_lock(spin_lock_t *l)   { (*l)++; }
static void spin_unlock(spin_lock_t *l) { (*l)--; }

/* The real extinction does not return. The double records and returns, and
   every leg asserts no extinction fired -- so a path that would have killed the
   system is a failure here rather than a silent continuation. */
static int g_extinct; static const char *g_extinct_msg = "";
static void extinction(const char *m) { g_extinct = 1; g_extinct_msg = m; }

static int g_underflow;

struct AddrSpace { u64 id; u32 page_count; u32 private_rings; spin_lock_t lock; };

struct Burrow {
    u64 magic;
    spin_lock_t lock;
    int handle_count;
    int mapping_count;
    u64 charge_as_id;
    u32 charge_pages;
    bool shared_out;
    int destroyed;
};

static void burrow_free_internal(struct Burrow *v) { v->magic = 0; v->destroyed = 1; }

static void addrspace_uncharge_pages(struct AddrSpace *as, u32 n) {
    if (n > as->page_count) { g_underflow = 1; return; }
    as->page_count -= n;
}

static void burrow_charge_record(struct Burrow *v, const struct AddrSpace *as, u32 pages) {
    v->charge_as_id = as->id;
    v->charge_pages = pages;
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
    u32 refund = should_free ? burrow_charge_claim_locked(v, payer) : 0;
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

/* The ported form: the refund is whatever the settled drop decided. */
static void retire_settled(struct Burrow *ring, struct AddrSpace *as, u32 metadata, u32 backing) {
    (void)backing;
    u32 refund = 0;
    (void)burrow_unref_settled_in(ring, as, &refund);
    spin_lock(&as->lock);
    addrspace_uncharge_pages(as, metadata + refund);
    spin_unlock(&as->lock);
    as->private_rings--;
}

/* THE MUTANT: refunds the ring's pages whether or not this drop freed them.
   This is the implementation the draft's all-final fixture could not catch. */
static void retire_unconditional(struct Burrow *ring, struct AddrSpace *as, u32 metadata, u32 backing) {
    u32 refund = 0;
    (void)burrow_unref_settled_in(ring, as, &refund);
    (void)refund;
    spin_lock(&as->lock);
    addrspace_uncharge_pages(as, metadata + backing);
    spin_unlock(&as->lock);
    as->private_rings--;
}

#define METADATA 1u
#define BACKING  3u

static void setup(struct Burrow *ring, struct AddrSpace *as, int mapped) {
    memset(ring, 0, sizeof *ring);
    memset(as, 0, sizeof *as);
    g_extinct = 0; g_underflow = 0;
    as->id = 0x1234;
    as->page_count = 0;
    ring->magic = VMO_MAGIC;
    ring->handle_count = 1;            /* the Loom's kernel handle ref */
    ring->mapping_count = mapped;      /* a surviving user mapping, or none */
    /* loom_create_private charges metadata + backing, then records backing on
       the ring so the refund follows the Burrow and never a Proc pointer. */
    as->page_count += METADATA + BACKING;
    as->private_rings = 1;
    burrow_charge_record(ring, as, BACKING);
}

typedef void (*retire_fn)(struct Burrow *, struct AddrSpace *, u32, u32);

static int leg_final(retire_fn retire) {
    struct Burrow ring; struct AddrSpace as;
    setup(&ring, &as, 0);
    u32 base = 0;
    retire(&ring, &as, METADATA, BACKING);
    if (g_extinct)   { fprintf(stderr, "final: extinction %s\n", g_extinct_msg); return 14; }
    if (g_underflow) { fprintf(stderr, "final: page_count underflow\n");         return 15; }
    if (!ring.destroyed) { fprintf(stderr, "final: ring not freed\n");           return 10; }
    if (as.page_count != base) {
        fprintf(stderr, "final: page_count %u, want %u\n", as.page_count, base); return 10;
    }
    return 0;
}

static int leg_nonfinal(retire_fn retire) {
    struct Burrow ring; struct AddrSpace as;
    setup(&ring, &as, 1);
    retire(&ring, &as, METADATA, BACKING);
    if (g_extinct)   { fprintf(stderr, "nonfinal: extinction %s\n", g_extinct_msg); return 14; }
    if (g_underflow) { fprintf(stderr, "nonfinal: page_count underflow\n");         return 15; }
    if (ring.destroyed) { fprintf(stderr, "nonfinal: ring freed under a live mapping\n"); return 11; }
    /* The metadata came back; the ring's pages did not, because they still
       carry a mapping. An unconditional refund lands at BACKING lower. */
    if (as.page_count != BACKING) {
        fprintf(stderr, "nonfinal: page_count %u, want %u\n", as.page_count, BACKING); return 11;
    }
    /* The tail: the mapping teardown is the drop that ends the occupancy, and
       it must settle the charge the retirement deliberately left recorded. */
    u32 refund = 0;
    struct Burrow *dead = burrow_release_mapping_settled_deferred(&ring, &as, &refund);
    if (dead != &ring) { fprintf(stderr, "nonfinal tail: mapping drop did not free\n"); return 12; }
    if (refund != BACKING) {
        fprintf(stderr, "nonfinal tail: refund %u, want %u\n", refund, BACKING); return 12;
    }
    addrspace_uncharge_pages(&as, refund);
    if (as.page_count != 0) {
        fprintf(stderr, "nonfinal tail: page_count %u, want 0\n", as.page_count); return 12;
    }
    return 0;
}

/* A record paid by a DIFFERENT address space must not be refunded to this one.
   Not a mutant check -- it pins the exact-payer rule the retirement relies on
   when a ring outlives the creator's image. */
static int leg_exact_payer(retire_fn retire) {
    struct Burrow ring; struct AddrSpace as;
    setup(&ring, &as, 0);
    ring.charge_as_id = 0x9999;        /* somebody else paid */
    retire(&ring, &as, METADATA, BACKING);
    if (g_extinct) { fprintf(stderr, "exact-payer: extinction %s\n", g_extinct_msg); return 14; }
    /* settled refunds nothing, so only metadata comes back: BACKING remains. */
    if (as.page_count != BACKING) {
        fprintf(stderr, "exact-payer: page_count %u, want %u\n", as.page_count, BACKING);
        return 13;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <settled|unconditional> <leg>\n", argv[0]); return 2; }
    retire_fn retire = strcmp(argv[1], "settled") == 0 ? retire_settled
                     : strcmp(argv[1], "unconditional") == 0 ? retire_unconditional : NULL;
    if (!retire) { fprintf(stderr, "unknown impl %s\n", argv[1]); return 2; }
    if (strcmp(argv[2], "final") == 0)       return leg_final(retire);
    if (strcmp(argv[2], "nonfinal") == 0)    return leg_nonfinal(retire);
    if (strcmp(argv[2], "exact-payer") == 0) return leg_exact_payer(retire);
    fprintf(stderr, "unknown leg %s\n", argv[2]);
    return 2;
}
