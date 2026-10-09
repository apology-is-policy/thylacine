
#include <stdio.h>
#include <stdlib.h>
#include <thylacine/loom_service_nodes.h>
#define CHECK(c,m) do{if(!(c)){puts(m);exit(11);}}while(0)
struct Loom {struct loom_slot_table *service_slots;struct loom_service_node_table *service_nodes;};
static void extinction(const char *s){puts(s);exit(86);}
static void loom_private_nodes_destroy(struct Loom *l) {
    if (!l->service_nodes || !l->service_slots)
        extinction("private Loom lost node storage");
    const u32 kinds[2] = { LOOM_SERVICE_FID, LOOM_SERVICE_SCOPE };
    for (u32 pass = 0; pass < 2; ++pass) {
        for (u32 i = 0; i < LOOM_SERVICE_SLOT_COUNT; ++i) {
            struct loom_service_slot *s = &l->service_slots->slots[i];
            if (s->phase == LSLOT_FREE || s->kind != kinds[pass]) continue;
            struct loom_service_ref ref = { .slot = i, .incarnation = s->incarnation };
            struct loom_service_node *n=&l->service_nodes->nodes[i];
            if (kinds[pass]==LOOM_SERVICE_SCOPE && false) {
                // Protocol/index/request cleanup precedes final zero-ref free.
                // Unused children were removed above; no caller can still own
                // a provisional child here. READY/ABORTING never use this path.
                if (s->phase!=LSLOT_LIVE || s->pins || n->parent.slot ||
                    n->parent.reserved || n->parent.incarnation || n->reserved)
                    extinction("private Loom retained pinned retired scope");
                *n=(struct loom_service_node){0};
                if (loom_slot_retire(l->service_slots,ref) || loom_slot_reap(l->service_slots,ref))
                    extinction("private Loom lost retired scope metadata");
                continue;
            }
            int rc = s->phase == LSLOT_PROVISIONAL ?
                loom_service_node_rollback(l->service_slots, l->service_nodes, ref) :
                loom_service_node_reap_unused(l->service_slots, l->service_nodes, ref);
            if (rc) extinction("private Loom retained active or invalid node");
        }
    }
    if (!loom_service_nodes_empty(l->service_nodes))
        extinction("private Loom retained unowned node");
}
int main(int argc,char **argv){
 int mode=argc>1?atoi(argv[1]):0;
 struct loom_slot_table slots={0};struct loom_service_node_table nodes={0};
 struct Loom l={&slots,&nodes};struct loom_service_ref parent={0},child={0},none={0};
 CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,none,&parent) &&
       !loom_service_node_publish(&slots,&nodes,parent),"parent preparation");
 nodes.nodes[parent.slot].state=LOOM_SERVICE_READY;
 if(mode<=1){
  CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,parent,&child),"provisional child pins parent");
  if(mode==1)CHECK(!loom_service_node_publish(&slots,&nodes,child),"live unused child pins parent");
  CHECK(slots.slots[parent.slot].pins==1,"exact parent pin");
 }
 nodes.nodes[parent.slot].state=LOOM_SERVICE_RETIRED;
 if(mode==2)nodes.nodes[parent.slot].state=LOOM_SERVICE_READY;
 if(mode==3)nodes.nodes[parent.slot].state=LOOM_SERVICE_ABORTING;
 if(mode==4)slots.slots[parent.slot].pins=1;
 if(mode==5)nodes.nodes[parent.slot].parent.incarnation=1;
 if(mode==6)nodes.nodes[parent.slot].reserved=1;
 if(mode==7)slots.slots[parent.slot].phase=LSLOT_PROVISIONAL;
 loom_slots_close(&slots);loom_private_nodes_destroy(&l);
 CHECK(loom_service_nodes_empty(&nodes),"retired scope and unused child removed");
 for(u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;i++)CHECK(slots.slots[i].phase==LSLOT_FREE && !slots.slots[i].pins,"child and parent slots empty");
 puts("PASS child before retired parent cleanup");return 0;
}
