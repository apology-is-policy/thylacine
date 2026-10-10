#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
#define KERNEL_TESTS 1
#define LOOM_SQE_LINK 1
#define LOOM_SQE_DRAIN 2
struct loom_sqe {
    u8  opcode;          // LOOM_OP_*
    u8  flags;           // LOOM_SQE_* (LINK / DRAIN / CQE_SKIP / MULTISHOT)
    u16 _resv0;
    u32 handle_idx;      // registered-handle index, or LOOM_HANDLE_RAW
    u64 offset;          // op offset (read / write / readdir)
    u32 len;             // op length / count
    u32 buf_idx_or_off;  // registered-buffer index or in-ring offset (Loom-6)
    u64 user_data;       // correlation token (echoed verbatim in the CQE)
    u64 _resv1[4];       // opcode-specific / future
};
struct loom_ring_hdr {
    u32 sq_head;         // kernel-owned: consumed (kernel advances)
    u32 sq_tail;         // user-owned:   produced (user advances)
    u32 sq_mask;         // sq_entries - 1 (immutable; set at setup)
    u32 sq_entries;      // immutable
    u32 cq_head;         // user-owned:   reaped (user advances)
    u32 cq_tail;         // kernel-owned: posted (kernel advances)
    u32 cq_mask;         // cq_entries - 1 (immutable)
    u32 cq_entries;      // immutable
    u32 flags;           // LOOM_RING_* runtime flags (SQ_NEED_WAKEUP -- Loom-4)
    u32 dropped;         // SQEs dropped (invalid index) -- diagnostics (Loom-3)
    u32 overflow;        // CQEs that could not post (CQ full) -- back-pressure
    u32 _pad[5];
};
struct Loom{u32 sq_head,sq_entries,admitting;void *chain;};
static struct loom_ring_hdr header;
static struct loom_sqe entries[1];static u32 indices[1],visits;
static void producer(struct Loom *l){(void)l;++visits;assert(header.sq_head==1);indices[0]=0;entries[0].user_data=0x222;entries[0].flags=LOOM_SQE_LINK;}
static void (*sq_reuse_test_hook)(struct Loom *)=producer;
static void exercise(bool bad){
 struct Loom owner={.sq_entries=1},*l=&owner;struct loom_ring_hdr *h=&header;struct loom_sqe *sqes=entries,ksqe={0};u32 *sq_array=indices;bool have,to_chain;
 header=(struct loom_ring_hdr){0};entries[0]=(struct loom_sqe){.user_data=0x111};indices[0]=bad?7:0;visits=0;
        u32 ring_pos = l->sq_head & (l->sq_entries - 1u);   // kernel-private mask
        u32 sqe_idx = __atomic_load_n(&sq_array[ring_pos], __ATOMIC_ACQUIRE);
        have = (sqe_idx < l->sq_entries);
        if (have) ksqe = sqes[sqe_idx];                     // COPY to kernel memory (TOCTOU)
        else __atomic_store_n(&h->dropped, h->dropped + 1u, __ATOMIC_RELEASE);
        // sq_head is permission for the producer to reuse BOTH the SQ index
        // and its 1:1 SQE slot. Copy (or reject) first, then release that slot.
        // l->lock excludes other consumers, but does not lock out userspace.
        l->sq_head++;
        __atomic_store_n(&h->sq_head, l->sq_head, __ATOMIC_RELEASE);
#ifdef KERNEL_TESTS
        void (*reuse_hook)(struct Loom *) = __atomic_load_n(&sq_reuse_test_hook, __ATOMIC_ACQUIRE);
        if (reuse_hook) reuse_hook(l);
#endif
        // Route under the SAME lock so the chain-non-empty test is consistent with
        // the snapshot we just took (the chain only grows during this call).
        to_chain = have && ((ksqe.flags & (LOOM_SQE_LINK | LOOM_SQE_DRAIN)) != 0 ||
                            l->chain != NULL);
        if (have) l->admitting++;                           // reserve until disposition

 assert(visits==1 && h->sq_head==1);assert(have==!bad);assert(h->dropped==(u32)bad);assert(l->admitting==(u32)!bad);
 if(have){assert(ksqe.user_data==0x111);assert(ksqe.flags==0 && !to_chain);}assert(entries[0].user_data==0x222);
}
int main(void){exercise(false);exercise(true);puts("SQ copied before reuse; invalid original index remains rejected PASS");}
