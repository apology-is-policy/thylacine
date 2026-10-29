#!/usr/bin/env python3
"""Exercise the production registry factory's ownership at allocation boundaries.

Primitive allocation/refcount stubs inject failures; real kernel tests cover
namespace mounts, role/delegation gates and connection routing separately.
"""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parent.parent
s=(root/'kernel/devsrv.c').read_text()
m=re.search(r'^struct Spoor \*devsrv_session_root\([^;]*?\) \{.*?^}\n',s,re.M|re.S)
assert m
body=m.group()
shim=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;
#define T_E_ACCES 13
#define T_E_INVAL 22
#define T_E_NOMEM 12
#define SRV_MAX_ROUTES 16
#define SRV_NAME_MAX 32
#define SRV_REGISTRY_MAGIC 0x73657276696365ULL
#define CWALKONLY 1
#define QTDIR 128
struct srv_route {u32 name_len,reserved;u8 name[32];};
struct SrvDomain {int ref;};
struct SrvRegistry {u64 magic;int ref;struct SrvDomain *domain;struct SrvRegistry *source;u32 route_count;struct srv_route routes[16];};
struct Proc {bool factory;};
struct Spoor {char dc;void *aux;u32 flag;struct {u8 type;} qid;};
static struct SrvRegistry boot={.magic=SRV_REGISTRY_MAGIC,.ref=1};
static struct SrvRegistry *g_boot_srv_registry=&boot;
static unsigned allocation,fail_at,live;
static void *allocate(size_t n) {if(++allocation==fail_at)return NULL;void *p=calloc(1,n);assert(p);live++;return p;}
static void release(void *p) {assert(p&&live);live--;free(p);}
static bool proc_may_create_srv_registry(struct Proc *p){return p&&p->factory;}
static bool srv_name_eq(const char *a,u8 al,const char *b,u8 bl){return al==bl&&!memcmp(a,b,al);}
static struct SrvDomain *srv_domain_create(int *err){struct SrvDomain *d=allocate(sizeof(*d));if(d)d->ref=1;else *err=-T_E_NOMEM;return d;}
static void srv_domain_unref(struct SrvDomain *d){if(d){assert(d->ref==1);release(d);}}
static struct SrvRegistry *srv_registry_create(void){struct SrvRegistry *r=allocate(sizeof(*r));if(r){r->magic=SRV_REGISTRY_MAGIC;r->ref=1;}return r;}
static void srv_registry_ref(struct SrvRegistry *r){assert(r&&r->ref>0);r->ref++;}
static void srv_registry_unref(struct SrvRegistry *r){if(!r)return;assert(r->ref>0);assert(r!=&boot||r->ref>1);if(--r->ref==0){srv_domain_unref(r->domain);srv_registry_unref(r->source);release(r);}}
static struct Spoor *devsrv_attach_registry(struct SrvRegistry *r){struct Spoor *c=allocate(sizeof(*c));if(c){c->dc='s';c->aux=r;c->qid.type=QTDIR;srv_registry_ref(r);}return c;}
static void close_root(struct Spoor *c){srv_registry_unref(c->aux);release(c);}
'''
tests=r'''
int main(void){
 struct Proc p={true};int err=0;
 struct Spoor source={.dc='s',.aux=&boot,.flag=CWALKONLY,.qid={QTDIR}};
 struct srv_route route={.name_len=3,.name="net"};
 for(fail_at=1;fail_at<=3;fail_at++){
  allocation=0;assert(!devsrv_session_root(&p,&source,&route,1,&err));
  assert(err==-T_E_NOMEM&&live==0&&boot.ref==1);
 }
 fail_at=0;allocation=0;
 struct Spoor *root=devsrv_session_root(&p,&source,&route,1,&err);
 assert(root&&err==0&&(root->flag&CWALKONLY)&&live==3&&boot.ref==2);
 struct SrvRegistry *reg=root->aux;
 assert(reg->ref==1&&reg->route_count==1&&reg->routes[0].name[0]=='n');
 route.name[0]='x';assert(reg->routes[0].name[0]=='n');
 close_root(root);assert(live==0&&boot.ref==1);
 p.factory=false;allocation=0;
 assert(!devsrv_session_root(&p,&source,&route,1,&err)&&err==-T_E_ACCES&&allocation==0);
 p.factory=true;route.reserved=1;
 assert(!devsrv_session_root(&p,&source,&route,1,&err)&&err==-T_E_INVAL&&allocation==0);
}
'''
mutants={
 'leak-domain-on-registry-failure':('if (!reg) { srv_domain_unref(d);','if (!reg) {'),
 'leak-constructor-reference':('    srv_registry_unref(reg);','    /* missing constructor put */'),
 'omit-source-retain':('    srv_registry_ref(reg->source);','    /* missing source retain */'),
}
with tempfile.TemporaryDirectory(prefix='thyla-registry-factory-') as tmp:
 p=Path(tmp)
 for name,mutation in [('clean',None),*mutants.items()]:
  code=body
  if mutation:
   assert code.count(mutation[0])==1,name;code=code.replace(*mutation)
  (p/'test.c').write_text(shim+code+tests)
  subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
  r=subprocess.run([str(p/'test')],capture_output=True,text=True)
  if mutation:assert r.returncode!=0 and 'Assertion' in r.stderr,(name,r.stderr)
  else:r.check_returncode()
  print(name+(': intended assertion failure' if mutation else ': PASS'),flush=True)
