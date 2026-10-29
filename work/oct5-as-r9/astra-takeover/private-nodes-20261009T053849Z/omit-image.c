
#include <stdio.h>
#include <stdlib.h>
#include <thylacine/loom_service_nodes.h>
#include <thylacine/errno.h>
#define LOOM_MAGIC 123
#define PROC_MAGIC 456
#define CHECK(c,s) do { if(!(c)) { puts(s);exit(11); } } while(0)
typedef struct { int held; } spin_lock_t;
static int held;
struct AddrSpace { int unused; };
struct Proc { u32 magic;struct AddrSpace *as;u64 stripes; };
struct Loom { u32 magic;spin_lock_t lock;struct AddrSpace *service_as;u64 service_creator;
 bool service_closing;struct loom_slot_table *service_slots;struct loom_service_node_table *service_nodes; };
static void spin_lock(spin_lock_t *l) {CHECK(!l->held && !held,"lock reentry");l->held=1;held++;}
static void spin_unlock(spin_lock_t *l) {CHECK(l->held && held==1,"lock release");l->held=0;held--;}
static bool loom_is_private(struct Loom *l) {return l && l->service_as;}
static void loom_exec_latch(struct Loom *l) {
 spin_lock(&l->lock);l->service_closing=true;loom_slots_close(l->service_slots);spin_unlock(&l->lock);
}
// Instrument module call boundaries only; the production modules are linked
// unmodified. This checks that the actual extracted wrapper holds its lock.
static int checked_prepare(struct loom_slot_table *s,struct loom_service_node_table *n,u32 k,
 struct loom_service_ref p,struct loom_service_ref *out) {
 CHECK(held==1,"prepare requires owner lock");return loom_service_node_prepare(s,n,k,p,out);
}
static int checked_publish(struct loom_slot_table *s,struct loom_service_node_table *n,struct loom_service_ref r) {
 CHECK(held==1,"publish requires owner lock");return loom_service_node_publish(s,n,r);
}
static int checked_rollback(struct loom_slot_table *s,struct loom_service_node_table *n,struct loom_service_ref r) {
 CHECK(held==1,"rollback requires owner lock");return loom_service_node_rollback(s,n,r);
}
static int checked_reap_unused(struct loom_slot_table *s,struct loom_service_node_table *n,struct loom_service_ref r) {
 CHECK(held==1,"reap requires owner lock");return loom_service_node_reap_unused(s,n,r);
}
#define loom_service_node_prepare checked_prepare
#define loom_service_node_publish checked_publish
#define loom_service_node_rollback checked_rollback
#define loom_service_node_reap_unused checked_reap_unused

int loom_private_node_prepare(struct Loom *l, struct Proc *p,
                              const struct loom_service_reserve *in,
                              struct loom_service_ref *out) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l) ||
        !p || p->magic != PROC_MAGIC || !in || !out) return -T_E_INVAL;
    if (p->stripes != l->service_creator)
        return -T_E_ACCES;
    if (in->size != sizeof(*in) || in->version != LOOM_SERVICE_ABI_VERSION ||
        in->flags || in->reserved || in->result.slot || in->result.reserved ||
        in->result.incarnation) return -T_E_INVAL;
    spin_lock(&l->lock);
    int rc = l->service_closing ? -T_E_CANCELED :
        loom_service_node_prepare(l->service_slots, l->service_nodes,
                                  in->kind, in->scope, out);
    spin_unlock(&l->lock);
    return rc;
}
int loom_private_node_publish(struct Loom *l, struct loom_service_ref ref) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l)) return -T_E_INVAL;
    spin_lock(&l->lock);
    int rc = l->service_closing ? -T_E_CANCELED :
        loom_service_node_publish(l->service_slots, l->service_nodes, ref);
    spin_unlock(&l->lock);
    return rc;
}
int loom_private_node_rollback(struct Loom *l, struct loom_service_ref ref) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l)) return -T_E_INVAL;
    spin_lock(&l->lock);
    int rc = loom_service_node_rollback(l->service_slots, l->service_nodes, ref);
    spin_unlock(&l->lock);
    return rc;
}
int loom_private_node_reap_unused(struct Loom *l, struct loom_service_ref ref) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l)) return -T_E_INVAL;
    spin_lock(&l->lock);
    int rc = loom_service_node_reap_unused(l->service_slots, l->service_nodes, ref);
    spin_unlock(&l->lock);
    return rc;
}

#include "loom_node_owner_fixture.h"
int main(void) {
 struct AddrSpace a={0};struct Proc p={.magic=PROC_MAGIC,.as=&a,.stripes=99};
 struct loom_slot_table slots={0};struct loom_service_node_table nodes={0};
 struct Loom l={.magic=LOOM_MAGIC,.service_as=&a,.service_creator=99,.service_slots=&slots,.service_nodes=&nodes};
 const char *error=loom_node_owner_fixture(&l,&p);
 CHECK(!held && !l.lock.held,"wrapper leaves lock balanced");
 if(error) {puts(error);return 11;}
 puts("PASS node owner transactions");return 0;
}
