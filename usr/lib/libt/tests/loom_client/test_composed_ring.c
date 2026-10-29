/* Keep fixtures identical to the qualified ring tests; renamed main is not run. */
#define main historical_ring_cases
#include "test_ring_payload.c"
#undef main
#include <thyla/loom_client/request_book.h>
struct cq_fixture { uint64_t memory[24];struct t_paired_cq reader;uint32_t tail; };
static void cq_init(struct cq_fixture *c){memset(c,0,sizeof(*c));assert(!t_paired_init(&c->reader,c->memory,sizeof(c->memory),0,64,96,2));}
static void cq_post(struct cq_fixture *c,uint64_t cookie,int result,unsigned flags,struct t_loom_service_buffer_receipt receipt){
 unsigned char *base=(void *)c->memory;uint32_t head=atomic_load_explicit((_Atomic uint32_t *)(void *)(base+16),memory_order_acquire);assert(c->tail-head<2);
 struct t_owned_cqe entry={cookie,result,flags};memcpy(base+64+(c->tail&1u)*16,&entry,sizeof(entry));memcpy(base+96+(c->tail&1u)*32,&receipt,sizeof(receipt));++c->tail;atomic_store_explicit((_Atomic uint32_t *)(void *)(base+20),c->tail,memory_order_release);
}
static uint32_t cq_head(struct cq_fixture *c){return atomic_load_explicit((_Atomic uint32_t *)(void *)((unsigned char *)c->memory+16),memory_order_acquire);}
static struct t_request_delivery reap(struct cq_fixture *cq,struct t_request_book *book){struct t_paired_record r;struct t_request_delivery d;assert(t_paired_reap(&cq->reader,&r)==1);assert(!t_request_accept(book,&r,&d));return d;}
static struct t_ring_payload *transfer(struct t_ring_pool *p,struct t_request_delivery d){struct t_ring_payload *v=NULL;int rc=t_ring_accept(p,d.record.cqe.result,d.record.cqe.flags,d.record.receipt,&v);assert(rc>=0);return v;}
int main(void){
 struct probe p={0};struct t_ring_owner *root;struct t_ring_client *client;struct t_ring_pool *pool;struct t_ring_payload *a,*b;struct t_request_book book_storage,*book=&book_storage;struct t_request_entry entries[2];struct t_loom_service_pool_member ranges[2]={range(0),range(8)};struct t_request_spec spec={.kind=T_REQ_POOLED_READ,.limit=8,.multi=1,.pool={0,0,1}};uint64_t cookie;struct cq_fixture cq;struct t_owned_cqe terminal;struct t_loom_service_buffer_receipt zero={0};
 fixture(&p,&root,&client);assert(!t_ring_attach(client,id(0,1),ranges,2,&pool));assert(!t_request_create(book,entries,2));assert(!t_request_reserve(book,id(3,4),spec,&cookie));assert(!t_request_publish(book,cookie));cq_init(&cq);
 cq_post(&cq,cookie,8,5,receipt(0,1,0,1));struct t_request_delivery d=reap(&cq,book);assert(cq_head(&cq)==1&&!p.returns&&d.cookie==cookie&&d.scope.slot==3&&d.scope.incarnation==4);a=transfer(pool,d);
 cq_post(&cq,cookie,8,5,receipt(0,1,1,1));b=transfer(pool,reap(&cq,book));view(a,0);view(b,1);assert(!t_ring_return(b)&&t_ring_live(root)==1);
 cq_post(&cq,cookie,0,0,zero);d=reap(&cq,book);assert(d.terminal&&!transfer(pool,d));assert(!t_request_take_terminal(book,cookie,&terminal)&&!terminal.result&&!t_request_occupied(book));assert(t_ring_live(root)==1&&cq_head(&cq)==3&&p.returns==1);view(a,0);
 t_ring_client_drop(client);p.retired=1;assert(!t_ring_progress(root));t_ring_owner_drop(root);t_ring_pool_drop(pool);t_request_destroy(book);assert(!p.freed);view(a,0);t_ring_abandon(a);assert(p.freed==1);
 p=(struct probe){0};fixture(&p,&root,&client);assert(!t_ring_attach(client,id(0,1),ranges,2,&pool));assert(!t_request_create(book,entries,2));assert(!t_request_reserve(book,id(3,4),spec,&cookie));assert(!t_request_publish(book,cookie));cq_init(&cq);
 cq_post(&cq,cookie,8,5,receipt(0,1,0,1));a=transfer(pool,reap(&cq,book));struct t_loom_service_ref scope;assert(t_request_abandon(book,cookie,&scope)==1&&scope.slot==3&&scope.incarnation==4);
 p.rc=-2;cq_post(&cq,cookie,8,5,receipt(0,1,1,1));d=reap(&cq,book);assert(d.abandoned);t_ring_abandon(transfer(pool,d));assert(t_ring_pending(root)==1&&t_ring_live(root)==1);view(a,0);
 cq_post(&cq,cookie,8,5,receipt(0,2,0,2));struct t_paired_record bad;assert(t_paired_reap(&cq.reader,&bad)==1&&cq_head(&cq)==3);assert(t_request_accept(book,&bad,&d)<0&&t_request_occupied(book)==1);assert(t_ring_pending(root)==1&&t_ring_live(root)==1);
 t_ring_client_drop(client);p.retired=1;assert(!t_ring_progress(root));t_ring_owner_drop(root);t_ring_pool_drop(pool);t_request_destroy(book);assert(!p.freed);view(a,0);p.rc=0;t_ring_abandon(a);assert(p.freed==1);
 cores_idle();puts("C two composed CQ request payload scenarios PASS");return 0;
}
