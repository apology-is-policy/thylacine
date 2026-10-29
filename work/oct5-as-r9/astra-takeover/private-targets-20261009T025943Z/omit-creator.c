
#include <stdio.h>
#include <stdlib.h>
#include <thylacine/loom_service_slots.h>
#include <thylacine/errno.h>
#define THYLACINE_DEVSRV_H
struct SrvServiceTarget { void *view,*service;u64 generation;u8 name_len;char name[32]; };
#include <thylacine/loom_service_targets.h>
#define LOOM_MAGIC 123
#define PROC_MAGIC 456
#define KOBJ_SPOOR 4
#define CHECK(c,s) do { if(!(c)) { puts(s);exit(11); } } while(0)
typedef struct { int held; } spin_lock_t;
static int held,registry_refs=1,handle_refs=1,retired,capture_error,handle_kind=KOBJ_SPOOR;
struct AddrSpace { int unused; };
struct Proc { u32 magic;struct AddrSpace *as;u64 stripes; };
struct Loom { u32 magic;spin_lock_t lock;struct AddrSpace *service_as;u64 service_creator;
 bool service_closing;struct loom_slot_table *service_slots;struct loom_service_target_table *service_targets;int refs; };
struct Handle { int kind;void *obj; };
static struct AddrSpace a,other;
static struct Loom *close_during_capture;
static void spin_lock(spin_lock_t *l) { CHECK(!l->held,"lock reentry");l->held=1;held++; }
static void spin_unlock(spin_lock_t *l) { CHECK(l->held,"lock release");l->held=0;held--; }
static void extinction(const char *s) {puts(s);exit(12);}
static bool loom_is_private(struct Loom *l) { return l && l->service_as; }
static void loom_private_targets_destroy(struct Loom *);
static void loom_ref(struct Loom *l) {CHECK(held==1 && l->refs>0,"owner pin under lock");l->refs++;}
static void loom_unref(struct Loom *l) {
 CHECK(!held && l->refs>0,"owner drop outside lock");
 if (!--l->refs) { loom_private_targets_destroy(l);retired++; }
}
static int handle_get(struct Proc *p,int fd,struct Handle *h) {
 (void)p;CHECK(!held,"handle resolution outside lock");
 if(fd!=7)return -1;h->obj=&a;h->kind=handle_kind;handle_refs++;return 0;
}
static void handle_put(struct Handle *h) {
 CHECK(!held && h->obj==&a && handle_refs>1,"handle release outside lock");handle_refs--;h->obj=NULL;
}
static int devsrv_service_target_init(void *root,const char *name,u32 len,struct SrvServiceTarget *out) {
 CHECK(!held && handle_refs==2 && root==&a && len==1 && name[0]=='x',"capture through retained actual handle");
 if(capture_error)return capture_error;
 *out=(struct SrvServiceTarget){.view=&a,.service=&other,.generation=42,.name_len=1,.name={'x'}};
 registry_refs++;
 if(close_during_capture) {close_during_capture->service_closing=true;loom_slots_close(close_during_capture->service_slots);close_during_capture=NULL;}
 return 0;
}
static void devsrv_service_target_clear(struct SrvServiceTarget *t) {
 CHECK(!held,"registry drop outside lock");
 if(t->view) {CHECK(t->view==&a && registry_refs>1,"registry ref owned");registry_refs--;}
 *t=(struct SrvServiceTarget){0};
}

static void loom_private_targets_destroy(struct Loom *l) {
    if (!l->service_targets || !l->service_slots)
        extinction("private Loom lost target storage");
    for (u32 i = 0; i < LOOM_SERVICE_SLOT_COUNT; ++i) {
        struct loom_service_slot *s = &l->service_slots->slots[i];
        struct SrvServiceTarget *t = &l->service_targets->targets[i];
        if (s->phase != LSLOT_FREE && s->kind == LOOM_SERVICE_TARGET) {
            if (s->pins || !t->view ||
                (s->phase != LSLOT_PROVISIONAL && s->phase != LSLOT_LIVE))
                extinction("private Loom retained invalid target");
            devsrv_service_target_clear(t);
            *s = (struct loom_service_slot){0};
        } else if (t->view) {
            extinction("private Loom retained unowned target");
        }
    }
}
static struct SrvServiceTarget *loom_private_target_exact(struct Loom *l,
                                      struct loom_service_ref ref, u32 phase) {
    if (ref.reserved || !ref.incarnation || ref.slot >= LOOM_SERVICE_SLOT_COUNT)
        return NULL;
    struct loom_service_slot *s = &l->service_slots->slots[ref.slot];
    struct SrvServiceTarget *t = &l->service_targets->targets[ref.slot];
    return s->incarnation == ref.incarnation && s->kind == LOOM_SERVICE_TARGET &&
           s->phase == phase && t->view ? t : NULL;
}
int loom_private_target_prepare(struct Loom *l, struct Proc *p,
                               const struct loom_service_target *in,
                               struct loom_service_ref *out) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l) ||
        !p || p->magic != PROC_MAGIC || !in || !out) return -T_E_INVAL;
    if (p->as != l->service_as)
        return -T_E_ACCES;
    if (in->size != sizeof(*in) || in->version != 1 || in->flags ||
        in->registry_fd < 0 || !in->name_len || in->name_len > 255 ||
        in->result.slot || in->result.reserved || in->result.incarnation)
        return -T_E_INVAL;
    for (u32 i = in->name_len; i < sizeof(in->name); ++i)
        if (in->name[i]) return -T_E_INVAL;
    struct loom_service_ref ref;
    spin_lock(&l->lock);
    int rc = loom_slot_reserve(l->service_slots, LOOM_SERVICE_TARGET, &ref);
    spin_unlock(&l->lock);
    if (rc) return rc;

    struct SrvServiceTarget fresh = {0};
    struct Handle h;
    rc = handle_get(p, in->registry_fd, &h) ? -T_E_BADF : 0;
    if (!rc) {
        rc = h.kind == KOBJ_SPOOR ?
            devsrv_service_target_init(h.obj, (const char *)in->name,
                                       in->name_len, &fresh) : -T_E_OPNOTSUPP;
        handle_put(&h);
    }
    spin_lock(&l->lock);
    if (!rc && l->service_closing) rc = -T_E_CANCELED;
    if (rc) {
        // This unpublished ref is private to this retained transaction; close
        // cannot remove it, and no borrower can pin a provisional slot.
        (void)loom_slot_rollback(l->service_slots, ref);
    } else {
        l->service_targets->targets[ref.slot] = fresh;
        *out = ref;
    }
    spin_unlock(&l->lock);
    if (rc) devsrv_service_target_clear(&fresh);
    return rc;
}
int loom_private_target_publish(struct Loom *l, struct loom_service_ref ref) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l)) return -T_E_INVAL;
    spin_lock(&l->lock);
    int rc = loom_private_target_exact(l, ref, LSLOT_PROVISIONAL) ?
        loom_slot_publish(l->service_slots, ref) : -T_E_NOENT;
    spin_unlock(&l->lock);
    return rc;
}
static int loom_private_target_remove(struct Loom *l, struct loom_service_ref ref,
                                      bool rollback) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l)) return -T_E_INVAL;
    struct SrvServiceTarget detached = {0};
    spin_lock(&l->lock);
    struct SrvServiceTarget *t = loom_private_target_exact(l, ref,
                                      rollback ? LSLOT_PROVISIONAL : LSLOT_LIVE);
    int rc = !t ? -T_E_NOENT : l->service_slots->slots[ref.slot].pins ? -T_E_BUSY : 0;
    if (!rc) {
        // Validate phase/identity/zero pins before either ledger changes. All
        // transitions below are then infallible under the common owner lock.
        detached = *t;
        *t = (struct SrvServiceTarget){0};
        if (rollback) (void)loom_slot_rollback(l->service_slots, ref);
        else {
            (void)loom_slot_retire(l->service_slots, ref);
            (void)loom_slot_reap(l->service_slots, ref);
        }
    }
    spin_unlock(&l->lock);
    if (!rc) devsrv_service_target_clear(&detached);
    return rc;
}
int loom_private_target_rollback(struct Loom *l, struct loom_service_ref ref) {
    return loom_private_target_remove(l, ref, true);
}
int loom_private_target_reap(struct Loom *l, struct loom_service_ref ref) {
    return loom_private_target_remove(l, ref, false);
}
int loom_private_target_get(struct Loom *l, struct loom_service_ref ref,
                            const struct SrvServiceTarget **out) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l) || !out) return -T_E_INVAL;
    spin_lock(&l->lock);
    struct SrvServiceTarget *t = loom_private_target_exact(l, ref, LSLOT_LIVE);
    int rc = t ? loom_slot_pin(l->service_slots, ref, LOOM_SERVICE_TARGET) : -T_E_NOENT;
    if (!rc) { loom_ref(l); *out = t; }
    spin_unlock(&l->lock);
    return rc;
}
int loom_private_target_put(struct Loom *l, struct loom_service_ref ref) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l)) return -T_E_INVAL;
    spin_lock(&l->lock);
    int rc = loom_private_target_exact(l, ref, LSLOT_LIVE) ?
        loom_slot_unpin(l->service_slots, ref) : -T_E_NOENT;
    spin_unlock(&l->lock);
    if (!rc) loom_unref(l); // may enqueue final owner; never touch l again
    return rc;
}

int main(void) {
 struct Proc p={.magic=PROC_MAGIC,.as=&a,.stripes=99};
 struct loom_slot_table slots={0};struct loom_service_target_table targets={0};
 struct Loom l={.magic=LOOM_MAGIC,.refs=1,.service_as=&a,.service_creator=99,.service_slots=&slots,.service_targets=&targets};
 struct loom_service_target in={.size=sizeof(in),.version=1,.registry_fd=7,.name_len=1,.name={'x'}};
 struct loom_service_ref ref={0},stale={0},prov={0};const struct SrvServiceTarget *borrow=NULL;
 p.as=&other;CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_ACCES,"target image authority");p.as=&a;
 p.stripes++;CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_ACCES,"target creator authority");p.stripes--;
 in.flags=1;CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"target header validated");in.flags=0;
 in.name[255]=1;CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"target tail validated");in.name[255]=0;
 in.registry_fd=8;CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_BADF && !ref.incarnation && slots.slots[0].phase==LSLOT_FREE,"bad fd unwinds slot");in.registry_fd=7;
 handle_kind=3;CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_OPNOTSUPP && handle_refs==1,"typed handle refused");handle_kind=KOBJ_SPOOR;
 capture_error=-T_E_NOENT;CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_NOENT && handle_refs==1 && registry_refs==1 && slots.slots[0].phase==LSLOT_FREE,"capture refusal unwinds");capture_error=0;
 CHECK(!loom_private_target_prepare(&l,&p,&in,&ref) && registry_refs==2 && handle_refs==1,"provisional owns registry only");
 CHECK(loom_private_target_get(&l,ref,&borrow)==-T_E_NOENT && !borrow,"provisional borrow refused");
 stale=ref;CHECK(!loom_private_target_rollback(&l,ref) && registry_refs==1,"rollback drops registry");
 CHECK(!loom_private_target_prepare(&l,&p,&in,&ref) && ref.incarnation>stale.incarnation,"rollback burns identity");
 CHECK(!loom_private_target_publish(&l,ref),"publish target");
 CHECK(loom_private_target_get(&l,stale,&borrow)==-T_E_NOENT,"stale identity refused");
 CHECK(!loom_private_target_get(&l,ref,&borrow) && l.refs==2 && slots.slots[ref.slot].pins==1 && borrow->view==&a && borrow->service==&other && borrow->generation==42,"borrow pins owner and exact target");
 CHECK(loom_private_target_reap(&l,ref)==-T_E_BUSY && registry_refs==2,"busy reap unchanged");
 CHECK(!loom_private_target_put(&l,ref) && l.refs==1 && !slots.slots[ref.slot].pins,"put releases owner pin");
 CHECK(loom_private_target_put(&l,ref)==-T_E_INVAL && l.refs==1,"double put unchanged");
 CHECK(!loom_private_target_reap(&l,ref) && registry_refs==1 && slots.slots[ref.slot].phase==LSLOT_FREE,"reap drops registry and slot");
 // Capture takes a registry ref before close wins the unlocked gap.
 close_during_capture=&l;
 CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_CANCELED && registry_refs==1 && handle_refs==1 && slots.slots[0].phase==LSLOT_FREE,"close during capture rolls back both ownerships");
 // New independent owner for close-after-prepare and final borrower schedules.
 struct loom_slot_table slots2={0};struct loom_service_target_table targets2={0};
 struct Loom r={.magic=LOOM_MAGIC,.refs=1,.service_as=&a,.service_creator=99,.service_slots=&slots2,.service_targets=&targets2};
 CHECK(!loom_private_target_prepare(&r,&p,&in,&ref) && !loom_private_target_publish(&r,ref),"second owner target");
 CHECK(!loom_private_target_prepare(&r,&p,&in,&prov),"second owner provisional");
 CHECK(!loom_private_target_get(&r,ref,&borrow),"second owner borrow");
 r.service_closing=true;loom_slots_close(&slots2);
 CHECK(loom_private_target_publish(&r,prov)==-T_E_CANCELED,"close wins publish");
 CHECK(loom_private_target_get(&r,ref,&borrow)==-T_E_CANCELED,"close blocks new borrow");
 loom_unref(&r);CHECK(!retired && r.refs==1 && registry_refs==3,"borrow holds closed owner");
 CHECK(!loom_private_target_put(&r,ref) && retired==1 && registry_refs==1 && handle_refs==1,"last borrow clears live and provisional targets");
 CHECK(!held,"no held locks");puts("PASS target transactions and final borrower ownership");return 0;
}
