#!/usr/bin/env python3
"""Host double for the private-owner retirement accounting.

WHAT THIS ESTABLISHES. loom_private_destroy's charge arithmetic, in the final
and nonfinal ring-drop cases, with the retirement implementation as the single
changed variable. It answers the one thing the native fixture cannot answer
until a lease lands: whether `metadata + refund` restores the image's page
count exactly, and whether the nonfinal leg actually discriminates.

WHAT IT DOES NOT ESTABLISH. Nothing about ARM weak memory, real SLUB timing,
real locking, or reachability from any syscall. The lock is a counter and the
schedule is single-threaded and cooperative. The AS-R9 double's boundary
statement applies here verbatim.

WHY IT IS NOT CIRCULAR, AND EXACTLY HOW FAR THAT GOES. The three Burrow
settlement functions are EXTRACTED verbatim from kernel/burrow.c rather than
restated here, so the DECISION under test is the shipped one; only its
environment is doubled, and each extracted body is asserted to be a verbatim
substring of its source before use.

BUT retire_settled below is NOT extracted -- it is a hand transcription of
loom_private_destroy's accounting sequence (astra, yip 0161 note 32). So the
single changed variable in this matrix is MY TRANSCRIPTION of the retirement,
not the shipped destructor, and a divergence between the two would be invisible
here. What this therefore bounds is the ARITHMETIC of that sequence. The
discriminating test of the ACTUAL loom_private_destroy path is the native
regression with the real destructor mutated, which needs the guest; this does
not stand in for it and the matrix below should not be read as if it did.

THE MATRIX IS THE POINT. An unconditional refund must PASS the final leg and
FAIL the nonfinal one. Requiring the pass is what proves the new leg -- and not
some unrelated breakage -- is what supplies the discrimination the draft's
all-final fixture lacked.
"""
import os, re, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SRC = (ROOT / "kernel/burrow.c").read_text()

def extract(signature):
    """Pull one function body out of burrow.c by brace matching."""
    i = SRC.index(signature)
    j = SRC.index("{", i)
    depth = 0
    for k in range(j, len(SRC)):
        if SRC[k] == "{":
            depth += 1
        elif SRC[k] == "}":
            depth -= 1
            if depth == 0:
                body = SRC[i:k + 1]
                assert body in SRC, "extraction is not a substring of its source"
                return body
    raise SystemExit("unbalanced braces extracting %r" % signature[:40])

CLAIM = extract("static u32 burrow_charge_claim_locked(")
UNREF = extract("bool burrow_unref_settled_in(")
RELEASE = extract("struct Burrow *burrow_release_mapping_settled_deferred(")

PRELUDE = r"""
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
"""

RETIRE = r"""
/* TRANSCRIBED, not extracted: this mirrors loom_private_destroy's accounting
   sequence by hand, so it tests the arithmetic of that sequence rather than the
   shipped function. A drift between the two is invisible here, which is why the
   native regression mutates the REAL destructor. */
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
"""

# The extracted bodies reach for kernel helpers the double supplies; the only
# substitution is the memory fence, which has no meaning in a single-threaded
# host program (the same substitution tools/test-loom-receipts.py makes).
BODIES = "\n".join([CLAIM, UNREF, RELEASE]).replace(
    '__asm__ __volatile__("dsb ish" ::: "memory");',
    "/* fence elided: single-threaded host double */")

PROGRAM = PRELUDE + "\n" + BODIES + "\n" + RETIRE

# settled must pass every leg. unconditional must PASS final (that is the
# draft fixture's blind spot, demonstrated) and FAIL nonfinal with code 11.
MATRIX = [
    ("settled",       "final",       0,  "the final drop refunds metadata + ring"),
    ("settled",       "nonfinal",    0,  "a nonfinal drop refunds metadata only, tail settles the ring"),
    ("settled",       "exact-payer", 0,  "a record paid by another image is not refunded here"),
    ("unconditional", "final",       0,  "BLIND SPOT: an unconditional refund passes the all-final fixture"),
    ("unconditional", "nonfinal",    11, "DISCRIMINATION: the nonfinal leg catches it"),
]

def main():
    cc = os.environ.get("CC", "/opt/homebrew/opt/llvm@22/bin/clang")
    sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True)
    cflags = ["-isysroot", sdk.stdout.strip()] if sdk.returncode == 0 else []
    logs = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    logs.mkdir(parents=True, exist_ok=True)
    (logs / "private-retire-double.c").write_text(PROGRAM)

    with tempfile.TemporaryDirectory(prefix="private-retire-") as tmp:
        exe = Path(tmp) / "double"
        src = Path(tmp) / "double.c"
        src.write_text(PROGRAM)
        build = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                                *cflags, str(src), "-o", str(exe)],
                               capture_output=True, text=True)
        (logs / "private-retire-build.log").write_text(build.stdout + build.stderr)
        if build.returncode != 0:
            print("BUILD FAILED -- see private-retire-build.log")
            print(build.stderr[-3000:])
            return 1

        bad = 0
        out = []
        for impl, leg, want, why in MATRIX:
            r = subprocess.run([str(exe), impl, leg], capture_output=True, text=True)
            ok = r.returncode == want
            if not ok:
                bad += 1
            line = "%-4s %-13s %-12s rc=%-3d want=%-3d  %s" % (
                "PASS" if ok else "FAIL", impl, leg, r.returncode, want, why)
            print(line)
            out.append(line + ("\n" + r.stderr if r.stderr else ""))
        (logs / "private-retire-matrix.log").write_text("\n".join(out) + "\n")
        print("\n%d/%d matrix rows as predicted" % (len(MATRIX) - bad, len(MATRIX)))
        return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
