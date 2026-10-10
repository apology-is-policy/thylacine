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
