#include <thyla/loom_client/lease_ledger.h>
#include <assert.h>
#include <stdio.h>
static struct t_loom_service_ref pool(void){return (struct t_loom_service_ref){.slot=4,.incarnation=17};}
static struct t_loom_service_buffer_receipt receipt(unsigned member,uint64_t lease){return (struct t_loom_service_buffer_receipt){.pool=pool(),.member=member,.lease=lease};}
static struct t_lease_bank bank;
static size_t free_cells(void){size_t n=0;for(unsigned i=0;i<64;++i)n+=!bank.cells[i].owner;return n;}
static struct t_lease_ledger book(void){struct t_lease_ledger b={0};struct t_loom_service_pool_member r[2]={{.length=4},{.offset=4,.length=4}};uint64_t sizes[]={8};assert(!t_lease_init(&b,&bank,pool(),r,2,sizes,1));return b;}
int main(void){
 struct t_lease_ledger b=book();struct t_lease_token a,c,next,ignored;
 assert(t_lease_accept_read(&b,4,5,receipt(0,11),&a)==1);
 assert(t_lease_accept_read(&b,2,4,receipt(1,9),&c)==1);assert(a.length==4 && c.length==2 && b.outstanding==2);
 assert(!t_lease_accept_read(&b,0,0,(struct t_loom_service_buffer_receipt){0},&ignored));assert(b.outstanding==2);
 assert(!t_lease_begin_return(&b,&c));assert(!t_lease_finish_return(&b,&c,0));assert(b.outstanding==1);
 assert(t_lease_accept_read(&b,1,4,receipt(0,12),&ignored)==T_LEASE_BUSY);
 assert(t_lease_finish_return(&b,&a,0)==T_LEASE_STATE);assert(!t_lease_begin_return(&b,&a));
 assert(!t_lease_finish_return(&b,&a,-2));assert(b.outstanding==1);
 assert(t_lease_accept_read(&b,1,4,receipt(0,12),&ignored)==T_LEASE_BUSY);
 assert(!t_lease_begin_return(&b,&a));assert(!t_lease_finish_return(&b,&a,0));assert(!b.outstanding);
 assert(t_lease_begin_return(&b,&a)==T_LEASE_STATE);
 assert(t_lease_accept_read(&b,4,4,receipt(0,11),&ignored)==T_LEASE_STALE);
 assert(t_lease_accept_read(&b,4,4,receipt(0,12),&next)==1);
 assert(t_lease_begin_return(&b,&a)==T_LEASE_STALE);
 assert(!t_lease_begin_return(&b,&next));assert(!t_lease_finish_return(&b,&next,1));assert(b.outstanding==1);
 assert(!t_lease_finish_return(&b,&next,0));assert(!b.outstanding);
 struct t_lease_ledger x=book(),y=book();struct t_lease_token xt,yt;
 assert(t_lease_accept_read(&x,4,4,receipt(0,1),&xt)==1);assert(t_lease_accept_read(&y,4,4,receipt(0,1),&yt)==1);
 assert(t_lease_begin_return(&y,&xt)==T_LEASE_STALE);assert(y.outstanding==1);
 int32_t results[]={0,-1,5,4,4,4,0};uint32_t flags[]={4,4,4,0,1,6,1};
 for(unsigned i=0;i<7;++i){assert(t_lease_accept_read(&b,results[i],flags[i],receipt(0,13),&ignored)<0);assert(!b.outstanding);}
 struct t_loom_service_buffer_receipt bad=receipt(0,13);bad.pool.incarnation++;
 assert(t_lease_accept_read(&b,4,4,bad,&ignored)<0);bad=receipt(0,13);bad.reserved=1;
 assert(t_lease_accept_read(&b,4,4,bad,&ignored)<0);bad=receipt(0,13);bad.pool.reserved=1;
 assert(t_lease_accept_read(&b,4,4,bad,&ignored)<0);assert(t_lease_accept_read(&b,4,4,receipt(2,13),&ignored)<0);
 assert(t_lease_accept_read(&b,4,4,receipt(0,0),&ignored)<0);assert(!b.outstanding);
 assert(t_lease_accept_read(&b,0,0,receipt(0,13),&ignored)<0);
 assert(!t_lease_accept_read(&b,-125,0,(struct t_loom_service_buffer_receipt){0},&ignored));
 struct t_loom_service_pool_member r[2]={{.length=4},{.offset=3,.length=4}};uint64_t sizes[]={8};
 assert(t_lease_init(&b,&bank,pool(),r,2,sizes,1)==T_LEASE_GEOMETRY);r[1].offset=UINT64_MAX;
 assert(t_lease_init(&b,&bank,pool(),r,2,sizes,1)==T_LEASE_GEOMETRY);r[1].offset=4;r[1].buffer_index=1;
 assert(t_lease_init(&b,&bank,pool(),r,2,sizes,1)==T_LEASE_GEOMETRY);r[1].buffer_index=0;r[1].reserved=1;
 assert(t_lease_init(&b,&bank,pool(),r,2,sizes,1)==T_LEASE_GEOMETRY);r[1].reserved=0;r[1].length=0;
 assert(t_lease_init(&b,&bank,pool(),r,2,sizes,1)==T_LEASE_GEOMETRY);
 struct t_loom_service_pool_member all[64];struct t_lease_token tokens[64];uint64_t total[]={64};
 for(unsigned i=0;i<64;++i)all[i]=(struct t_loom_service_pool_member){.offset=i,.length=1};
 /* The bank is shared: 64 members need every cell free, and a refusal claims none. */
 assert(free_cells()==58);t_lease_release(&x);t_lease_release(&y);assert(free_cells()==62);
 assert(t_lease_init(&x,&bank,pool(),all,63,total,1)==T_LOOM_CLIENT_ENOSPC&&free_cells()==62);
 struct t_lease_token old=next;t_lease_release(&b);assert(free_cells()==64&&t_lease_begin_return(&b,&old)==T_LEASE_STALE&&t_lease_accept_read(&b,4,4,receipt(0,14),&ignored)==T_LEASE_RECEIPT&&t_lease_bind(&b,pool())==T_LEASE_STATE);
 assert(!t_lease_init(&b,&bank,pool(),all,64,total,1)&&!free_cells());
 for(unsigned i=0;i<64;++i)assert(t_lease_accept_read(&b,1,4,receipt(i,UINT64_MAX),&tokens[i])==1);
 assert(b.outstanding==64);assert(t_lease_accept_read(&b,1,4,receipt(64,1),&ignored)<0);
 for(unsigned i=0;i<64;++i){assert(!t_lease_begin_return(&b,&tokens[i]));assert(!t_lease_finish_return(&b,&tokens[i],0));}
 assert(!b.outstanding);assert(t_lease_accept_read(&b,1,4,receipt(0,1),&ignored)==T_LEASE_STALE);
 assert(t_lease_init(&b,&bank,pool(),all,65,total,1)==T_LEASE_GEOMETRY);
 /* A ledger reuses freed cells wherever they lie; each ordinal finds its own. */
 t_lease_release(&b);struct t_lease_ledger p={0},q={0},s={0};struct t_loom_service_pool_member two[2]={{.length=4},{.offset=4,.length=4}},three[3]={{.length=1},{.offset=1,.length=2},{.offset=3,.length=3}};uint64_t eight[]={8};
 assert(!t_lease_init(&p,&bank,pool(),two,2,eight,1)&&!t_lease_init(&q,&bank,pool(),two,2,eight,1));t_lease_release(&p);
 assert(!t_lease_init(&s,&bank,pool(),three,3,eight,1)&&free_cells()==59&&t_lease_cell(&s,0)==0&&t_lease_cell(&s,1)==1&&t_lease_cell(&s,2)==4);
 for(unsigned k=0;k<3;++k){assert(t_lease_accept_read(&s,(int32_t)k+2,4,receipt(k,1),&ignored)<0);assert(t_lease_accept_read(&s,(int32_t)k+1,4,receipt(k,1),&tokens[k])==1&&tokens[k].length==k+1);}
 assert(s.outstanding==3&&!q.outstanding);t_lease_release(&s);t_lease_release(&q);assert(free_cells()==64);
 puts("C lease ledger ownership and malformed-vector checks PASS");return 0;
}
