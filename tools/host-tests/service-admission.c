// AS-2b actual-source admission/DAC fixture. Registry, connection allocator and
// Proc table are controlled doubles; native boot separately checks real refs,
// weighted admission, listener rebind and process-table integration.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#define CHECK(x,s) do { if (!(x)) { fputs(s "\n",stderr); exit(1); } } while(0)
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64, caps_t;
#define PROC_SUPP_GIDS_MAX 15
#define GID_INVALID 0
#define PERM_R 4u
#define PERM_W 2u
#define PERM_X 1u
#define CAP_HOSTOWNER 1u
#define CAP_DAC_OVERRIDE 2u
#define CAP_TCB_DIAL 4u
#define T_E_NOENT 2
#define T_E_INVAL 22
#define T_E_ACCES 13
#define T_E_NOMEM 12
#define T_E_NOSPC 28
#define T_E_OPNOTSUPP 95
#define PROC_STATE_ALIVE 1
#define PROC_STATE_ZOMBIE 2
#define SRV_NAME_MAX 32
#define SRV_STATE_LIVE 2
#define SRV_REGISTRY_MAGIC 0xabcdefULL
#define CWALKONLY 1
#define QTDIR 128
struct AddrSpace { int identity; };
/* ACTUAL_IDENTITY_DECL */
struct Proc { struct AddrSpace *as; u64 stripes; caps_t caps; u32 principal_id,primary_gid,supp_gids[15]; u8 supp_gid_count; int state,pid; bool console; };
struct t_stat { u32 uid,gid,mode; };
static u64 proc_stripes(const struct Proc *p) { return p ? p->stripes : 0; }
static bool proc_is_console_attached(const struct Proc *p) { return p->console; }
static struct Proc *table[4];
static pthread_mutex_t proc_lock=PTHREAD_MUTEX_INITIALIZER;
static void proc_for_each(int (*cb)(struct Proc *,void *), void *arg) {
    pthread_mutex_lock(&proc_lock);
    for(unsigned i=0;i<4;i++) if(table[i]&&cb(table[i],arg)) break;
    pthread_mutex_unlock(&proc_lock);
}
/* ACTUAL_PERMISSIONS */
/* ACTUAL_SNAPSHOT */
typedef pthread_mutex_t spin_lock_t;
typedef int irq_state_t;
static _Thread_local int locked;
static irq_state_t spin_lock_irqsave(spin_lock_t *l) { pthread_mutex_lock(l); locked++; return 0; }
static void spin_unlock_irqrestore(spin_lock_t *l, irq_state_t s) { (void)s; locked--; pthread_mutex_unlock(l); }
enum srv_mode { SRV_MODE_9P, SRV_MODE_BYTE };
struct SrvDomain { int number; };
struct SrvRegistry;
struct SrvConn { unsigned ref; bool torn,byte_mode,cape,remote; struct SrvDomain *domain; u64 stripes,poster; int pid; bool console; u32 msize; };
struct SrvService { struct SrvRegistry *reg; u8 name_len; char name[32]; int state; u64 generation,poster_stripes; enum srv_mode mode; u32 ring_msize; bool cape,remote,cap_posted; unsigned backlog_count; struct SrvConn *backlog[2]; int accept_rendez,poll_list; };
struct SrvRegistry { u64 magic; unsigned refs; spin_lock_t lock; struct SrvRegistry *source; struct SrvDomain *domain; struct SrvService svc[4]; };
struct Spoor { char dc; void *aux; u32 flag; struct { u8 type; } qid; };
/* ACTUAL_ADMISSION_DECL */
static unsigned allocations,frees,wakes;
static bool fail_allocate;
static bool srv_name_eq(const char *a,u8 n,const char *b,u8 m) {return n==m&&!memcmp(a,b,n);}
static int srv_route_index(const struct SrvRegistry *v,const char *n,u8 len) {return v->source&&len==5&&!memcmp(n,"route",5)?0:-1;}
static struct SrvService *srv_lookup_in(struct SrvRegistry *r,const char *n,u8 len) {
    for(unsigned i=0;i<4;i++) if(srv_name_eq(r->svc[i].name,r->svc[i].name_len,n,len)) return &r->svc[i];
    return NULL;
}
static void srv_registry_ref(struct SrvRegistry *r) { CHECK(r->refs,"retain live registry");r->refs++; }
static void srv_registry_unref(struct SrvRegistry *r) { CHECK(r->refs,"registry balance");r->refs--; }
static struct SrvConn *srvconn_create_in(struct SrvDomain *d,int *err,u64 stripes,int pid,bool console,u64 poster,u32 msize) {
    CHECK(!locked,"allocation outside registry lock");
    if(fail_allocate){*err=-T_E_NOMEM;return NULL;}
    struct SrvConn *c=calloc(1,sizeof(*c)); CHECK(c,"fixture alloc"); allocations++;
    *c=(struct SrvConn){.ref=1,.domain=d,.stripes=stripes,.pid=pid,.console=console,.poster=poster,.msize=msize};return c;
}
static void srvconn_ref(struct SrvConn *c) {CHECK(c->ref,"retain connection");c->ref++;}
static void srvconn_unref(struct SrvConn *c) {CHECK(c->ref,"connection balance");if(!--c->ref){CHECK(!locked,"free outside lock");frees++;free(c);}}
static void srvconn_teardown(struct SrvConn *c) {CHECK(!locked,"teardown outside lock");c->torn=true;}
static void srvconn_set_byte_mode(struct SrvConn *c) {c->byte_mode=true;}
static void srvconn_set_cape(struct SrvConn *c) {c->cape=true;}
static void srvconn_set_remote(struct SrvConn *c) {c->remote=true;}
static int srv_backlog_push_locked(struct SrvService *s,struct SrvConn *c) {
    CHECK(locked,"publication lock");if(s->state!=SRV_STATE_LIVE||s->backlog_count==2)return -1;
    s->backlog[s->backlog_count++]=c;return 0;
}
static void wakeup(int *x) {(void)x;CHECK(!locked,"wakeup outside registry lock");wakes++;}
static void poll_waiter_list_wake(int *x) {wakeup(x);}
/* ACTUAL_AUTHORIZED */
/* ACTUAL_ADMISSION */
static void post(struct SrvRegistry *r,unsigned slot,const char *name,u64 gen) {
    struct SrvService *s=&r->svc[slot];CHECK(!s->backlog_count,"fixture empty slot");
    *s=(struct SrvService){.reg=r,.generation=gen,.poster_stripes=88,.state=SRV_STATE_LIVE,.ring_msize=8192};
    s->name_len=(u8)strlen(name);memcpy(s->name,name,s->name_len);
}
static void drain(struct SrvService *s) {while(s->backlog_count)srvconn_unref(s->backlog[--s->backlog_count]);}
static void identity_cases(void) {
    struct AddrSpace old={1}, next={2};
    struct Proc p={.as=&old,.stripes=55,.pid=123,.state=PROC_STATE_ALIVE,.principal_id=42,.primary_gid=9,.supp_gid_count=1,.supp_gids={8},.console=true};table[0]=&p;
    struct ProcServiceIdentity id;
    CHECK(proc_service_snapshot_by_stripes(55,&old,&id),"live exact creator found");
    CHECK(id.stripes==55&&id.pid==123&&id.console_attached,"creator provenance");
    struct t_stat st={.uid=41,.gid=8,.mode=0040};
    CHECK(!perm_check_identity(&id.access,&st,PERM_R),"supplementary group retained");
    p.supp_gids[0]=7;p.caps=CAP_DAC_OVERRIDE;
    st.mode=0;CHECK(perm_check_identity(&id.access,&st,PERM_R)==-1,"old admission cannot acquire new grant");
    CHECK(proc_service_snapshot_by_stripes(55,&old,&id)&&!perm_check_identity(&id.access,&st,PERM_R),"new admission captures grant");
    CHECK(perm_check_identity(&id.access,&st,0)==-1,"empty permission fails closed");
    p.caps=0;CHECK(perm_check(&p,&st,PERM_R)==-1,"new ordinary open observes revocation");
    p.principal_id=41;st.mode=0044;CHECK(perm_check(&p,&st,PERM_R)==-1,"owner-first permissions");
    CHECK(!proc_in_group(&p,GID_INVALID),"invalid group refused");
    p.as=&next;CHECK(!proc_service_snapshot_by_stripes(55,&old,&id)&&!id.stripes,"old image cannot borrow replacement");
    CHECK(proc_service_snapshot_by_stripes(55,&next,&id),"new image found");
    p.state=PROC_STATE_ZOMBIE;CHECK(!proc_service_snapshot_by_stripes(55,&next,&id),"zombie cannot admit");
    table[0]=NULL;CHECK(!proc_service_snapshot_by_stripes(55,&next,&id),"dead identity cannot admit");
    CHECK(!proc_service_snapshot_by_stripes(0,&next,&id)&&!proc_service_snapshot_by_stripes(55,NULL,&id),"invalid identity refused");
}
static void admission_cases(void) {
    struct SrvDomain domain={1};
    struct SrvRegistry provider={.magic=SRV_REGISTRY_MAGIC,.refs=1,.lock=PTHREAD_MUTEX_INITIALIZER};
    struct SrvRegistry view={.magic=SRV_REGISTRY_MAGIC,.refs=1,.lock=PTHREAD_MUTEX_INITIALIZER,.source=&provider,.domain=&domain};
    struct Spoor root={.dc='s',.aux=&view,.flag=CWALKONLY,.qid={QTDIR}};
    post(&provider,0,"route",1);post(&view,0,"own",1);
    struct ProcServiceIdentity id={.stripes=55,.pid=123,.console_attached=true};
    struct SrvServiceTarget t;struct SrvConnectAdmission a;
    CHECK(!devsrv_service_target_init(&root,"route",5,&t),"routed target capture");
    CHECK(t.view==&view&&t.service==&provider.svc[0],"retained navigation view plus provider slot");
    CHECK(!devsrv_service_prepare(&t,&id,&a),"prepare routed admission");
    CHECK(a.conn->domain==&domain,"route charges consumer view");
    CHECK(!provider.svc[0].backlog_count&&!wakes,"prepare is invisible");
    CHECK(a.conn->stripes==55&&a.conn->pid==123&&a.conn->console&&a.conn->poster==88,"captured provenance stamped");
    srvconn_ref(a.conn);struct SrvConn *held=a.conn;
    devsrv_connect_release(&a);CHECK(held->torn,"cancel prepare tears local connection");srvconn_unref(held);
    fail_allocate=true;CHECK(devsrv_service_prepare(&t,&id,&a)==-T_E_NOMEM&&!a.conn,"allocation failure rolls back");fail_allocate=false;
    CHECK(!devsrv_service_prepare(&t,&id,&a),"prepare successful publish");
    CHECK(!devsrv_connect_publish(&a),"publish succeeds");CHECK(devsrv_connect_publish(&a)==-T_E_INVAL,"publish only once");
    devsrv_connect_wake(&a);CHECK(wakes==2,"wake after publication");
    devsrv_connect_release(&a);CHECK(provider.svc[0].backlog_count==1,"backlog owns real ref");drain(&provider.svc[0]);
    // Rebinding between reservation and commit cannot target the new server.
    CHECK(!devsrv_service_prepare(&t,&id,&a),"prepare replacement race");
    provider.svc[0].generation++;
    CHECK(devsrv_connect_publish(&a)==-T_E_NOENT,"publication refuses replacement");devsrv_connect_release(&a);
    CHECK(devsrv_service_prepare(&t,&id,&a)==-T_E_NOENT,"target refuses replacement generation");
    // Generations are per SLOT. Same name at a different slot with generation1
    // must not match the old target; the retained exact slot closes this ABA.
    post(&provider,0,"other",2);post(&provider,1,"route",1);
    CHECK(devsrv_service_prepare(&t,&id,&a)==-T_E_NOENT,"target refuses same generation in another slot");
    devsrv_service_target_clear(&t);
    CHECK(!devsrv_service_target_init(&root,"route",5,&t),"explicit recapture replacement");
    provider.svc[1].cap_posted=true;CHECK(devsrv_service_prepare(&t,&id,&a)==-T_E_NOENT,"resident route refuses cap-posted provider");provider.svc[1].cap_posted=false;
    provider.svc[1].mode=SRV_MODE_BYTE;CHECK(devsrv_service_prepare(&t,&id,&a)==-T_E_OPNOTSUPP,"strict target refuses raw byte mode");provider.svc[1].mode=SRV_MODE_9P;
    provider.svc[1].remote=true;CHECK(devsrv_service_prepare(&t,&id,&a)==-T_E_OPNOTSUPP,"strict target refuses remote");provider.svc[1].remote=false;
    provider.svc[1].cape=true;CHECK(devsrv_service_prepare(&t,&id,&a)==-T_E_OPNOTSUPP,"strict target refuses cape");provider.svc[1].cape=false;
    // Explicitly fill the backlog; refusal cannot leak an extra endpoint ref.
    for(unsigned i=0;i<2;i++){CHECK(!devsrv_service_prepare(&t,&id,&a)&&!devsrv_connect_publish(&a),"fill backlog");devsrv_connect_release(&a);}
    CHECK(!devsrv_service_prepare(&t,&id,&a)&&devsrv_connect_publish(&a)==-T_E_NOSPC,"full backlog refuses");devsrv_connect_release(&a);drain(&provider.svc[1]);
    devsrv_service_target_clear(&t);
    char longer[33];memset(longer,'x',sizeof(longer));CHECK(devsrv_service_target_init(&root,longer,33,&t)==-T_E_NOENT,"no name truncation");
    CHECK(devsrv_service_target_init(&root,"../own",6,&t)==-T_E_INVAL,"no path escape");
    root.flag=0;CHECK(devsrv_service_target_init(&root,"own",3,&t)==-T_E_OPNOTSUPP,"navigation descriptor required");
    CHECK(view.refs==1&&provider.refs==1&&allocations==frees,"all local ownership balanced");
    CHECK(!devsrv_srv_connect_authorized(true,false,false,0)&&devsrv_srv_connect_authorized(true,false,false,CAP_TCB_DIAL),"ordinary byte capability gate unchanged");
}
int main(void){identity_cases();admission_cases();puts("PASS actual admission source: identity, DAC, route/domain, publication, ABA, cancellation, quota and reference balance");}
