#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_ready.h>
#include <thylacine/loom.h>
#include <thylacine/loom_service_fids.h>
#include <thylacine/loom_service_pool_table.h>
#include <thylacine/errno.h>
#include <thylacine/poll.h>
static unsigned checks;
#define CHECK(x,m) do{if(!(x)){fprintf(stderr,"FAIL %s\n",m);exit(1);}++checks;}while(0)
void extinction(const char *m){fprintf(stderr,"INVARIANT %s\n",m);exit(86);}
static struct Loom l;
static struct loom_service_protocol p;
static struct loom_service_operation ops[3];
static struct loom_service_request records[3];
static struct loom_service_node_table nodes;
static struct loom_service_pool_table pools;
static struct loom_ring_hdr hdr;
static void reset(void){
 memset(&l,0,sizeof l);memset(&p,0,sizeof p);memset(ops,0,sizeof ops);
 memset(records,0,sizeof records);memset(&nodes,0,sizeof nodes);memset(&pools,0,sizeof pools);memset(&hdr,0,sizeof hdr);
 l.service_nodes=&nodes;l.service_pools=&pools;l.service_requests.records=records;l.service_requests.capacity=3;l.cq_entries=2;l.ring_kva=(u8 *)&hdr;
 p.owner=&l;p.indexed=p.connect_bound=true;p.client.session.magic=P9_SESSION_MAGIC;p.scope.incarnation=1;
 nodes.nodes[0].state=LOOM_SERVICE_READY;p.op_count=3;p.op_head=ops;p.op_tail=ops+2;
 for(unsigned i=0;i<3;i++){
  ops[i].owner=&l;ops[i].protocol=&p;ops[i].request.index=i;ops[i].request.identity=i+1;
  ops[i].phase=LRPC_QUEUED;ops[i].prev=i?ops+i-1:NULL;ops[i].next=i<2?ops+i+1:NULL;
  records[i].identity=i+1;records[i].owners=1;records[i].phase=LREQ_ACTIVE;
 }
}
static struct loom_ready_selection claim(u64 now,bool native){
 struct loom_ready_selection s={0};CHECK(!loom_ready_claim_locked(&p,now,native,&s) && p.driving,"claim owns exclusive driver");return s;
}
int main(void){
 struct loom_ready_selection s;struct loom_ready_interest a;
 reset();records[0].deadline=100;records[1].deadline=20;records[2].deadline=50;
 a=loom_ready_interest_locked(&p,10);CHECK(a.work && a.deadline==20,"earliest deadline scans past first runnable owner");
 for(unsigned i=0;i<6;i++){s=claim(10,false);CHECK(s.action==LREADY_SUBMIT && s.operation==ops+i%3,"round robin rotates every admitted owner");p.driving=false;}
 s=claim(10,false);struct loom_ready_selection untouched={.reason=123};
 CHECK(loom_ready_claim_locked(&p,10,true,&untouched)==-T_E_BUSY && untouched.reason==123,"second driver cannot claim escaped pointer");
 a=loom_ready_interest_locked(&p,10);CHECK(!a.work && !a.events && !a.deadline,"interest never reads live driver cursors");
 p.driving=false;p.users=1;a=loom_ready_interest_locked(&p,10);CHECK(!a.work && !a.events,"teardown users own return wake");
 reset();p.progress.io.tx=(void *)1;a=loom_ready_interest_locked(&p,1);
 CHECK(!a.work && a.events==(POLLIN|POLLOUT),"occupied TX parks queued owners on native readiness");
 CHECK(loom_ready_claim_locked(&p,1,false,&untouched)==-T_E_AGAIN && untouched.reason==123 && !p.driving,"no readiness means no driver claim");
 s=claim(1,true);CHECK(s.action==LREADY_IO && !s.operation,"native readiness selects one progress quantum");
 reset();for(unsigned i=0;i<P9_SESSION_MAX_OUTSTANDING;i++)p.client.session.outstanding[i].active=true;
 a=loom_ready_interest_locked(&p,1);CHECK(!a.work && a.events==POLLIN,"full tag pool waits for receive without submit spin");
 reset();for(unsigned i=0;i<3;i++){ops[i].pool_stream=true;ops[i].pool.slot=2;ops[i].pool.incarnation=7;records[i].deadline=100;}
 a=loom_ready_interest_locked(&p,1);CHECK(!a.work && a.deadline==100 && a.events==POLLIN,"empty pool retains deadline without runnable work");
 pools.bank.cells[0].phase=LMEMBER_AVAILABLE;pools.bank.cells[0].pool.slot=2;pools.bank.cells[0].pool.incarnation=6;
 CHECK(!loom_ready_interest_locked(&p,1).work,"old pool incarnation cannot wake stream");
 pools.bank.cells[0].pool.incarnation=7;s=claim(1,false);CHECK(s.operation==ops && s.action==LREADY_SUBMIT,"exact returned member enables oldest stream");p.driving=false;
 pools.bank.cells[0].phase=LMEMBER_LEASED;ops[1].pool_busy=true;CHECK(!loom_ready_claim_locked(&p,1,false,&s) && s.operation==ops+1,"local no-wire retry keeps busy member eligible");p.driving=false;ops[1].pool_busy=false;
 pools.bank.next_nonce=UINT64_MAX;CHECK(!loom_ready_claim_locked(&p,1,false,&s) && s.action==LREADY_SUBMIT,"exhausted leases are actionable without available member");
 reset();ops[0].fid_claim=P9_TCLUNK;ops[0].source.slot=1;nodes.nodes[1].fid_borrows=1;s=claim(1,false);CHECK(s.operation==ops+1,"CLUNK drain cannot block unrelated owner");p.driving=false;
 reset();for(unsigned i=0;i<3;i++)ops[i].phase=LRPC_INFLIGHT;
 ops[0].callback_seen=true;ops[0].rpc.sending=true;ops[1].callback_seen=true;s=claim(1,false);CHECK(s.operation==ops+1 && s.action==LREADY_ADVANCE,"early reply still waits for TX owner");
 reset();for(unsigned i=0;i<3;i++){ops[i].phase=LRPC_SETTLING;ops[i].pool_pending=true;records[i].deadline=50;}l.cq_tail=2;
 a=loom_ready_interest_locked(&p,1);CHECK(!a.work && a.deadline==50,"full CQ pending payload parks with deadline");
 s=claim(50,false);CHECK(s.action==LREADY_QUIESCE && s.reason==-T_E_TIMEDOUT,"deadline ends writers despite full CQ");p.driving=false;p.abort_reason=-T_E_TIMEDOUT;p.rpc_quiesced=true;
 s=claim(51,false);CHECK(s.action==LREADY_CANCEL,"negative commit remains runnable behind pending MORE");p.driving=false;
 for(unsigned i=0;i<3;i++){ops[i].phase=LRPC_TERMINAL;records[i].phase=LREQ_TERMINAL;}p.locally_retired=true;
 a=loom_ready_interest_locked(&p,51);CHECK(!a.work && !a.events && !a.deadline,"retired pending owners park without expired-deadline spin");
 hdr.cq_head=1;s=claim(51,false);CHECK(s.action==LREADY_DELIVER,"retired scope still delivers pool-owned MORE");p.driving=false;
 for(unsigned i=0;i<3;i++)ops[i].pool_pending=false;s=claim(51,false);CHECK(s.action==LREADY_POST,"negative terminal follows pending delivery");p.driving=false;
 records[0].phase=LREQ_DELIVERED;records[1].phase=LREQ_DELIVERED;records[2].phase=LREQ_DELIVERED;l.cq_tail=3;
 s=claim(51,false);CHECK(s.action==LREADY_FREE,"physical cleanup does not require CQ space");p.driving=false;l.service_closing=true;
 s=claim(51,false);CHECK(s.action==LREADY_DISCARD,"whole close drains owners even after local retirement");
 reset();p.client.dead=true;s=claim(1,false);CHECK(s.action==LREADY_QUIESCE && s.reason==-T_E_IO,"dead transport never admits another request");
 reset();ops[0].phase=LRPC_SETTLING;ops[0].pool_more_delivered=true;s=claim(1,false);CHECK(s.action==LREADY_REARM,"delivered MORE can rearm");p.driving=false;
 reset();ops[0].phase=LRPC_SETTLING;s=claim(1,false);CHECK(s.action==LREADY_COMMIT,"settled program reaches commit action");
 reset();records[2].deadline=5;s=claim(5,false);CHECK(s.action==LREADY_QUIESCE && s.operation==ops+2 && s.reason==-T_E_TIMEDOUT,"expired later owner preempts earlier submission");
 printf("PASS READY scheduler: %u checks\n",checks);return 0;
}
