from pathlib import Path
import re,subprocess,json,hashlib,datetime,importlib.util
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('extract',R/'work/oct5-as-r9/final-reclaim-control.py');ex=importlib.util.module_from_spec(spec);spec.loader.exec_module(ex)
s=(D/'kernel/loom.c').read_text();functions=['loom_retire_init','loom_exec_latch','loom_private_close_notify','loom_private_close_changed','loom_handle_closed','loom_private_enqueue','loom_private_pending','loom_private_retire_one','loom_ref','loom_unref']
bodies=[ex.fun(s,n) for n in functions];prototypes='\n'.join(b[:b.index('{')].strip()+';' for b in bodies)
queue=re.sub(r'^#include.*\n','',(D/'kernel/loom_service_close_queue.c').read_text(),flags=re.M)
pre=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_close_queue.h>
#define LOOM_MAGIC 123
#define CHECK(c,s) do {if(!(c)){puts(s);exit(11);}} while(0)
typedef struct {int held;} spin_lock_t;
struct Rendez {int unused;};
struct loom_slot_table {bool closing;};
struct Loom {u32 magic,refcount;void *service_as;bool service_closing;spin_lock_t lock;
 struct loom_slot_table *service_slots;u64 service_close_visits;struct loom_close_work service_close_work;
 struct Loom *service_retire_next;int cq_waiters;struct Rendez sqpoll_park;int progress;bool event;};
static spin_lock_t service_retire_lock;
static struct Rendez service_retire_wait;
static struct Loom *service_retire_head;
static struct Loom **service_retire_tail;
static bool service_retire_ready;
static struct loom_close_queue service_close_queue;
static int locks,wakes,polls,visits,destroys;
static void extinction(const char *s) {CHECK(false,s);}
static void spin_lock_init(spin_lock_t *s){s->held=0;}
static void spin_lock(spin_lock_t *s){CHECK(!locks && !s->held,"no queue/Loom lock nesting");s->held=1;locks++;}
static void spin_unlock(spin_lock_t *s){CHECK(locks==1 && s->held,"matched lock release");locks--;s->held=0;}
static void rendez_init(struct Rendez *r){r->unused=0;}
static void wakeup(struct Rendez *r){(void)r;CHECK(!locks,"scheduler wake outside locks");wakes++;}
static void poll_waiter_list_wake(int *p){(void)p;CHECK(!locks,"poll wake outside locks");polls++;}
static bool loom_is_private(const struct Loom *l){return l && l->service_as;}
static void loom_slots_close(struct loom_slot_table *t){CHECK(locks==1,"close latch serialized");t->closing=true;}
static void loom_free(struct Loom *l){(void)l;CHECK(false,"private owner entered legacy free");}
static void loom_private_close_changed(struct Loom *);
static void loom_private_destroy(struct Loom *l){CHECK(!locks && l->refcount==0 &&
 (l->service_close_work.state==LCLOSE_NEW || l->service_close_work.state==LCLOSE_DONE),"final free only after queue root ended");l->magic=0;destroys++;}
static int loom_engine_close_visit(struct Loom *l){CHECK(!locks && l->refcount && l->service_closing,"driver uses live root outside Q");visits++;
 if(l->event){l->event=false;loom_private_close_changed(l);}return l->progress;}
'''
# external declaration must match the extracted definition.
pre=pre.replace('static void loom_private_close_changed(struct Loom *);','void loom_private_close_changed(struct Loom *);')
main=r'''
int main(void) {
 struct loom_slot_table sa={0},sb={0},sc={0};
 struct Loom a={.magic=LOOM_MAGIC,.refcount=1,.service_as=&sa,.service_slots=&sa},
             b={.magic=LOOM_MAGIC,.refcount=1,.service_as=&sb,.service_slots=&sb},
             c={.magic=LOOM_MAGIC,.refcount=1,.service_as=&sc,.service_slots=&sc};
 loom_retire_init();CHECK(service_retire_ready && !loom_private_pending(NULL),"initial queues empty");
 loom_exec_latch(&a);CHECK(a.service_closing && sa.closing && !wakes && !polls && a.refcount==1 &&
                         a.service_close_work.state==LCLOSE_NEW,"exec latch remains leaf only without queue root");
 loom_handle_closed(&a);CHECK(a.refcount==2 && a.service_close_work.state==LCLOSE_READY,"handle close acquires one independent queue root");
 loom_handle_closed(&a);CHECK(a.refcount==2,"duplicate close does not duplicate root");
 loom_unref(&a);CHECK(a.refcount==1,"queue retains owner after final handle drop");
 loom_private_retire_one();CHECK(a.refcount==1 && a.service_close_work.state==LCLOSE_PARKED && !loom_private_pending(NULL) && visits==1,"blocked close parks with one root");
 int prior=wakes;loom_private_close_changed(&a);CHECK(wakes==prior+1 && loom_private_pending(NULL),"external event wakes parked retirement");
 a.event=true;loom_private_retire_one();CHECK(a.refcount==1 && a.service_close_work.state==LCLOSE_READY && visits==2,"event during worker observation retained");
 loom_private_retire_one();CHECK(a.service_close_work.state==LCLOSE_PARKED && !loom_private_pending(NULL) && visits==3,"event requests only one recheck");
 loom_handle_closed(&b);loom_unref(&b);b.progress=1;
 loom_unref(&c);CHECK(!c.refcount && c.service_close_work.state==LCLOSE_NEW && service_retire_head==&c,"zero ref enqueue never resurrects close root");
 loom_private_retire_one();CHECK(c.magic==0 && destroys==1 && b.service_close_work.state==LCLOSE_READY && b.refcount==1,"busy close cannot starve independent final destruction");
 a.progress=2;b.progress=2;loom_private_close_changed(&a);
 loom_private_retire_one();CHECK(b.refcount==0 && b.service_close_work.state==LCLOSE_DONE && a.refcount==1,"completion transfers one queue root outside lock");
 loom_private_retire_one();CHECK(a.refcount==0 && a.service_close_work.state==LCLOSE_DONE && b.magic==0,"second ring progress and first final free share turn");
 loom_private_retire_one();CHECK(a.magic==0 && destroys==3 && !loom_private_pending(NULL),"all final references physically retire once");
 CHECK(!locks,"balanced wrappers locks");puts("PASS actual close wrappers");return 0;
}
'''
code='\n'.join(bodies)
mutants=[
 ('missing-root','if (started) loom_ref(l);','if (false) loom_ref(l);','handle close acquires one independent queue root'),
 ('duplicate-root','if (started) loom_ref(l);','loom_ref(l);','duplicate close does not duplicate root'),
 ('resurrection','// refcount is already zero. Do not call the root-taking handle-close path.\n    loom_private_close_notify(l);','// bad resurrection\n    loom_handle_closed(l);','zero ref enqueue never resurrects close root'),
 ('leaf-wake','    loom_slots_close(l->service_slots);','    loom_slots_close(l->service_slots);\n    wakeup(&l->sqpoll_park);','scheduler wake outside locks'),
 ('leak-root','if (action==LCLOSE_DROP) loom_unref(closing);','if (false && action==LCLOSE_DROP) loom_unref(closing);','completion transfers one queue root outside lock'),
 ('early-root-drop','if (action==LCLOSE_DROP) loom_unref(closing);','if (action!=LCLOSE_AGAIN) loom_unref(closing);','blocked close parks with one root'),
 ('drop-under-Q','spin_unlock(&service_retire_lock);\n            if (action==LCLOSE_DROP) loom_unref(closing);','if (action==LCLOSE_DROP) loom_unref(closing);\n            spin_unlock(&service_retire_lock);','no queue/Loom lock nesting'),
 ('starve-destroy','if (l) loom_private_destroy(l);','if (l && !work) loom_private_destroy(l);','busy close cannot starve independent final destruction')]
out=D/('wrapper-controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir();results=[]
for tag,old,new,want in [('control',None,None,None)]+mutants:
 text=code
 if old:assert text.count(old)==1,(tag,text.count(old));text=text.replace(old,new)
 src=out/(tag+'.c');src.write_text(pre+prototypes+queue+text+main);exe=out/(tag+'.exe')
 cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip(),'-I',str(D/'kernel/include'),'-I',str(R/'kernel/include'),str(src),'-o',str(exe)]
 with (out/(tag+'-build.log')).open('wb') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
 assert r.returncode==0,(tag,'build',str(out))
 r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15);s=r.stdout+r.stderr;(out/(tag+'.log')).write_text(s)
 assert r.returncode==(11 if want else 0) and r.stdout==(want or 'PASS actual close wrappers')+'\n' and not r.stderr,(tag,r.returncode,s,str(out))
 results.append(dict(case=tag,rc=r.returncode,expected=want,source_sha256=hashlib.sha256(src.read_bytes()).hexdigest(),log_sha256=hashlib.sha256(s.encode()).hexdigest()));print('PASS',tag,flush=True)
(out/'result.json').write_text(json.dumps({'results':results,'inputs':{'kernel/loom.c':hashlib.sha256((D/'kernel/loom.c').read_bytes()).hexdigest(),'kernel/loom_service_close_queue.c':hashlib.sha256((D/'kernel/loom_service_close_queue.c').read_bytes()).hexdigest(),'driver':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()},'limits':'Extracted actual queue/root/retirer wrapper C with checked lock, scheduler, close-progress and allocator boundaries. Sequential simulated event interleavings; not native SMP or allocator qualification.'},indent=2)+'\n');print(out)
