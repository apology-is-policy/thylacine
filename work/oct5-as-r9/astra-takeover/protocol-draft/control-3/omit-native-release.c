
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define KERNEL_TESTS
#define SRVCONN_MSIZE 32768u
#define SRVCONN_BULK_MSIZE 131072u
#define P9_CLIENT_OUT_BUF_MAX 32768u
#define PRINCIPAL_SYSTEM 0xfffffffeu
#define KP_ZERO 1
#define LOOM_MAGIC 999
#define T_E_INVAL 22
#define T_E_BUSY 16
#define T_E_TIMEDOUT 110
#define T_E_ACCES 13
#define T_E_NOMEM 12
#define T_E_CANCELED 125
#define CHECK(c,m) do { if(!(c)){fprintf(stderr,"FAIL %s\n",m);exit(21);} } while(0)
typedef struct { int held; } spin_lock_t;
static int locks,mode,live,conn_live,borrowed,init_calls,bind_calls;
struct AddrSpace { spin_lock_t lock;u32 pages,budget; };
struct Loom { u64 magic,service_creator;struct AddrSpace *service_as;spin_lock_t lock;bool service_closing;int refs; };
struct ProcServiceIdentity { struct { u32 principal_id; } access;u64 stripes;int pid; };
struct loom_service_ref { u32 slot,reserved;u64 incarnation; };
struct SrvServiceTarget { int value; };
struct SrvConn { u32 msize;u64 stripes; };
struct SrvConnectAdmission { struct SrvConn *conn;bool published; };
struct p9_transport_ops { int (*send)(void*,const u8*,size_t);int (*recv)(void*,u8*,size_t);int (*close)(void*);void *ctx; };
struct p9_client_progress { struct SrvConn *conn; };
struct p9_client { u8 out_buf_inline[P9_CLIENT_OUT_BUF_MAX];bool initialized;u8 *recv,*send;struct p9_client_progress *progress; };
struct loom_service_protocol {
 struct Loom *owner;struct ProcServiceIdentity identity;struct SrvConnectAdmission admission;
 struct p9_client client;struct p9_client_progress progress;
 u8 *recv_storage,*bulk_send_storage;u32 charged_pages;
};
static struct AddrSpace a={.pages=10,.budget=500};
static struct Loom owner={.magic=LOOM_MAGIC,.service_creator=987,.service_as=&a,.refs=1};
static struct SrvServiceTarget native={7};
static struct SrvConn conn;
static void *allocs[3];static size_t sizes[3];static int allocated;
static void spin_lock(spin_lock_t *l) { CHECK(!l->held,"recursive lock");l->held=1;++locks; }
static void spin_unlock(spin_lock_t *l) { CHECK(l->held,"unheld unlock");l->held=0;--locks; }
static bool loom_is_private(const struct Loom *l) { return l && l->service_as; }
static void loom_ref(struct Loom *l) { CHECK(l==&owner && l->refs>0,"retain live exact Loom");++l->refs; }
static void loom_unref(struct Loom *l) {
 CHECK(!locks && l==&owner && l->refs>0,"drop Loom outside locks");
 if(--l->refs==0) CHECK(!live && !conn_live && !borrowed && a.pages==10,"last Loom drop follows all releases");
}
static bool proc_service_snapshot_by_stripes(u64 stripes,const struct AddrSpace *as,struct ProcServiceIdentity *id) {
 CHECK(!locks && stripes==987 && as==&a,"actual creator image snapshot");
 if(mode==3)return false;
 *id=(struct ProcServiceIdentity){.access={mode==18?PRINCIPAL_SYSTEM:42},.stripes=stripes,.pid=55};return true;
}
static int loom_private_target_get(struct Loom *l,struct loom_service_ref ref,const struct SrvServiceTarget **out) {
 CHECK(!locks && ref.incarnation==123 && !borrowed,"exact target borrow");
 if(mode==4 || l->service_closing)return -T_E_CANCELED;
 ++borrowed;loom_ref(l);*out=&native;return 0;
}
static int loom_private_target_put(struct Loom *l,struct loom_service_ref ref) {
 CHECK(ref.incarnation==123 && borrowed==1,"exact target release");--borrowed;loom_unref(l);return 0;
}
static int devsrv_service_prepare(const struct SrvServiceTarget *t,const struct ProcServiceIdentity *id,struct SrvConnectAdmission *out) {
 CHECK(!locks && t==&native && borrowed==1 && id->stripes==987 && id->pid==55,"native prepare uses captured identity");
 *out=(struct SrvConnectAdmission){0};if(mode==5)return -T_E_ACCES;
 CHECK(!conn_live,"fresh exclusive native connection");conn_live=1;
 conn=(struct SrvConn){.msize=(mode==1 || mode==10)?SRVCONN_BULK_MSIZE:SRVCONN_MSIZE,.stripes=id->stripes};
 if(mode==6)conn.msize=17;
 out->conn=&conn;return 0;
}
static u32 srvconn_msize(const struct SrvConn *c) { CHECK(conn_live && c==&conn,"live connection msize");return c->msize; }
static void devsrv_connect_release(struct SrvConnectAdmission *ad) {
 CHECK(!locks && conn_live && ad->conn==&conn && !ad->published,"native release outside locks and unpublished");
 conn_live=0;*ad=(struct SrvConnectAdmission){0};
}
static u32 burrow_backing_pages(size_t bytes) { u32 p=1;while((size_t)p*4096<bytes)p*=2;return p; }
static bool addrspace_charge_pages(struct AddrSpace *as,u32 pages,bool exempt) {
 CHECK(as==&a && as->lock.held && !live && conn_live,"whole charge before allocations");
 CHECK(exempt==(mode==18),"charge uses captured principal exemption");
 if(!exempt && as->pages+pages>as->budget)return false;as->pages+=pages;return true;
}
static void addrspace_uncharge_pages(struct AddrSpace *as,u32 pages) {
 CHECK(as==&a && as->lock.held && !live && !conn_live && owner.refs>0,"refund after storage before owner drop");
 CHECK(as->pages==10+pages,"exact charge refund");as->pages-=pages;
}
static void *kmalloc(size_t bytes,int flag) {
 CHECK(!locks && flag==KP_ZERO && allocated<3 && a.pages>10 && owner.refs>=2,"allocation after charge and owner pin");
 void *p=calloc(1,bytes);CHECK(p,"host allocator");allocs[allocated]=p;sizes[allocated++]=bytes;++live;return p;
}
static void kfree(void *p) {
 CHECK(!locks && p && live>0,"free outside locks");bool found=false;
 for(int i=0;i<allocated;++i)if(allocs[i]==p){allocs[i]=NULL;found=true;break;}
 CHECK(found,"free exact live allocation");--live;free(p);
}
static int p9_client_init_preallocated(struct p9_client *c,u32 fid,u32 msize,struct p9_transport_ops ops,u8 *recv,size_t recv_cap,u8 *send,u32 send_cap) {
 ++init_calls;CHECK(!locks && !borrowed && fid==0 && recv_cap==msize && send_cap==msize && recv && send,"exact preallocated client storage");
 CHECK(ops.send(NULL,NULL,0)<0 && ops.recv(NULL,NULL,0)<0 && ops.close(NULL)<0,"legacy adapters never wait");
 if(mode==11)return -1;
 c->initialized=true;c->recv=recv;c->send=send;return 0;
}
static struct SrvConn *p9_srvconn_progress_ops(struct SrvConn *c) { CHECK(c==&conn && conn_live,"exclusive native try adapter");return c; }
static int p9_client_progress_bind(struct p9_client *c,struct p9_client_progress *p,struct SrvConn *cn,u32 principal,u64 deadline) {
 ++bind_calls;CHECK(!locks && c->initialized && principal==(mode==18?PRINCIPAL_SYSTEM:42) && deadline==1000,"progress uses identity and absolute deadline");
 c->progress=p;p->conn=cn;if(mode==13)owner.service_closing=true;return mode==12?-1:0;
}
static void p9_client_destroy(struct p9_client *c) {
 CHECK(!locks,"client cleanup outside locks");
 if(!c->initialized)return;
 CHECK(conn_live && live>=2 && owner.refs>=1,"protocol abort before backing release");
 c->initialized=false;c->progress=NULL;
}
// Preparation is invisible to the peer. The Loom reference pins the exact
// creator image while this owner holds page charges; native admission separately
// retains its registry/domain and ordinary weighted connection credits.

_Static_assert(sizeof(struct loom_service_protocol) > 2048,
               "protocol owner must use buddy allocation for exact charging");
_Static_assert(SRVCONN_MSIZE == P9_CLIENT_OUT_BUF_MAX,
               "default send storage is inside the charged client");

#ifdef KERNEL_TESTS
static u32 protocol_fault;
void loom_private_protocol_fault_arm_for_test(u32 stage) {
    __atomic_store_n(&protocol_fault, stage, __ATOMIC_RELAXED);
}
u32 loom_private_protocol_fault_pending_for_test(void) {
    return __atomic_load_n(&protocol_fault, __ATOMIC_RELAXED);
}
static u32 protocol_fault_take(void) {
    return __atomic_exchange_n(&protocol_fault, 0, __ATOMIC_RELAXED);
}
#else
static u32 protocol_fault_take(void) { return 0; }
#endif

// init requires a complete legacy vtable. It must never be a blocking adapter:
// progress_bind replaces byte delivery with exclusive native try operations.
// Accidental legacy access fails locally, rather than introducing a peer wait.
static int protocol_no_send(void *ctx, const u8 *buf, size_t len) {
    (void)ctx; (void)buf; (void)len; return -1;
}
static int protocol_no_recv(void *ctx, u8 *buf, size_t len) {
    (void)ctx; (void)buf; (void)len; return -1;
}
static int protocol_no_close(void *ctx) { (void)ctx; return -1; }

static void protocol_refund(struct Loom *l, u32 pages) {
    struct AddrSpace *as = l->service_as;
    spin_lock(&as->lock);
    addrspace_uncharge_pages(as, pages);
    spin_unlock(&as->lock);
}

int loom_private_protocol_release_unused(struct loom_service_protocol *p) {
    if (!p || !p->owner) return -T_E_INVAL;
    // This primitive cannot certify retirement of an admitted scope. In
    // particular, a transport abort alone does not end external parser borrows.
    if (p->admission.published) return -T_E_BUSY;
    struct Loom *l = p->owner;
    u32 pages = p->charged_pages;
    p9_client_destroy(&p->client);
    /* omit native release */
    if (p->bulk_send_storage) kfree(p->bulk_send_storage);
    if (p->recv_storage) kfree(p->recv_storage);
    kfree(p);
    protocol_refund(l, pages);
    loom_unref(l); // last access to l or its image
    return 0;
}

int loom_private_protocol_prepare(struct Loom *l, struct loom_service_ref target,
                                  u64 deadline, u64 now,
                                  struct loom_service_protocol **out) {
    u32 fault = protocol_fault_take();
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l) || !out)
        return -T_E_INVAL;
    if (deadline && deadline <= now) return -T_E_TIMEDOUT;
    struct ProcServiceIdentity identity;
    if (!proc_service_snapshot_by_stripes(l->service_creator, l->service_as, &identity))
        return -T_E_ACCES;
    const struct SrvServiceTarget *native;
    int rc = loom_private_target_get(l, target, &native);
    if (rc) return rc;
    // Convert the target borrow into an independent protocol owner reference.
    // The native prepare retains everything it needs before dropping the borrow.
    loom_ref(l);
    struct SrvConnectAdmission admission;
    rc = devsrv_service_prepare(native, &identity, &admission);
    (void)loom_private_target_put(l, target);
    if (rc) { loom_unref(l); return rc; }
    u32 msize = srvconn_msize(admission.conn);
    if (msize != SRVCONN_MSIZE && msize != SRVCONN_BULK_MSIZE) {
        devsrv_connect_release(&admission); loom_unref(l); return -T_E_INVAL;
    }
    u32 pages = (u32)burrow_backing_pages(sizeof(struct loom_service_protocol)) +
                (u32)burrow_backing_pages(msize);
    if (msize > P9_CLIENT_OUT_BUF_MAX) pages += (u32)burrow_backing_pages(msize);
    struct AddrSpace *as = l->service_as;
    spin_lock(&as->lock);
    bool charged = addrspace_charge_pages(as, pages,
                                  identity.access.principal_id == PRINCIPAL_SYSTEM);
    spin_unlock(&as->lock);
    if (!charged) { devsrv_connect_release(&admission); loom_unref(l); return -T_E_NOMEM; }
    struct loom_service_protocol *p = fault == 1 ? NULL : kmalloc(sizeof(*p), KP_ZERO);
    if (!p) {
        devsrv_connect_release(&admission);
        protocol_refund(l, pages); loom_unref(l); return -T_E_NOMEM;
    }
    p->owner = l; p->identity = identity; p->admission = admission;
    p->charged_pages = pages;
    p->recv_storage = fault == 2 ? NULL : kmalloc(msize, KP_ZERO);
    if (!p->recv_storage) { rc = -T_E_NOMEM; goto fail; }
    u8 *send = p->client.out_buf_inline;
    if (msize > P9_CLIENT_OUT_BUF_MAX) {
        p->bulk_send_storage = fault == 3 ? NULL : kmalloc(msize, KP_ZERO);
        if (!p->bulk_send_storage) { rc = -T_E_NOMEM; goto fail; }
        send = p->bulk_send_storage;
    }
    struct p9_transport_ops unused = { .send=protocol_no_send, .recv=protocol_no_recv,
                                      .close=protocol_no_close, .ctx=NULL };
    rc = p9_client_init_preallocated(&p->client, 0, msize, unused,
                                     p->recv_storage, msize, send, msize);
    if (rc) { rc = -T_E_INVAL; goto fail; }
    rc = p9_client_progress_bind(&p->client, &p->progress,
             p9_srvconn_progress_ops(p->admission.conn), identity.access.principal_id, deadline);
    if (rc) { rc = -T_E_INVAL; goto fail; }
    // Close during allocation cannot return a new usable prepared owner. A
    // subsequent close still requires the future publisher's locked recheck.
    spin_lock(&l->lock);
    bool closed = l->service_closing;
    spin_unlock(&l->lock);
    if (closed) { rc = -T_E_CANCELED; goto fail; }
    *out = p;
    return 0;
fail:
    (void)loom_private_protocol_release_unused(p);
    return rc;
}

int main(int argc,char **argv) {
 mode=argc>1?atoi(argv[1]):0;
 if(mode==7 || mode==18)a.budget=10;
 if(mode==16)owner.service_as=NULL;
 if(mode==17)owner.service_closing=true;
 struct loom_service_ref ref={.incarnation=123};
 struct loom_service_protocol *p=(void*)(uintptr_t)1;
 u32 fault=mode==8?1:mode==9?2:mode==10?3:mode==2?1:0;
 loom_private_protocol_fault_arm_for_test(fault);
 int rc=loom_private_protocol_prepare(&owner,ref,1000,mode==2?1000:1,&p);
 CHECK(!loom_private_protocol_fault_pending_for_test(),"fault consumed even before allocation");
 if(mode==2) CHECK(rc==-T_E_TIMEDOUT && !allocated && !init_calls,"expired deadline refuses before native allocation");
 if((mode>=2 && mode<=13) || mode==16 || mode==17) {
  CHECK(rc<0 && p==(void*)(uintptr_t)1,"refusal preserves output");
  CHECK(!live && !conn_live && !borrowed && owner.refs==1 && a.pages==10,"refusal restores every ownership axis");
  puts("PASS refusal");return 0;
 }
 CHECK(rc==0 && p && p!=(void*)(uintptr_t)1,"positive prepare control");
 CHECK(owner.refs==2 && !borrowed && conn_live && init_calls==1 && bind_calls==1,"prepared independent Loom lifetime");
 u32 expected=burrow_backing_pages(sizeof(*p))+burrow_backing_pages(conn.msize)*(mode==1?2u:1u);
 CHECK(p->charged_pages==expected && a.pages==10+expected,"complete protocol charge");
 CHECK(p->client.send==(mode==1?p->bulk_send_storage:p->client.out_buf_inline) &&
       sizes[0]==sizeof(*p) && sizes[1]==conn.msize && (mode!=1 || sizes[2]==conn.msize),"class sized allocations without inline duplication");
 if(mode==14) {
  p->admission.published=true;
  CHECK(loom_private_protocol_release_unused(p)==-T_E_BUSY,"published cleanup refused");
  CHECK(live==2 && conn_live && owner.refs==2,"published refusal preserves ownership");
  p->admission.published=false;
 }
 if(mode==15)loom_unref(&owner);
 CHECK(!loom_private_protocol_release_unused(p),"unused release");
 CHECK(!live && !conn_live && !borrowed && a.pages==10 && owner.refs==(mode==15?0:1),"final exact accounting and lifetime");
 puts("PASS prepared ownership");return 0;
}
