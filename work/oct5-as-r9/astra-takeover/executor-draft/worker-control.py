from pathlib import Path
import re,json,subprocess,hashlib,datetime
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
# Build actual readiness/CONNECT/ledgers and extracted private worker functions.
e={'__file__':str(D/'control.py')}
exec((D/'control.py').read_text().rsplit("\nout=D/('controls-'",1)[0],e)
pre=e['pre'].replace('u32 cq_head,cq_tail,cq_mask;', 'u32 cq_head,cq_tail,cq_mask,flags;').replace('void *sqpoll;', 'void *sqpoll;u64 service_creator;int sqpoll_join;')
pre+=r'''
#define LOOM_RING_SQ_NEED_WAKEUP 1u
struct Thread {int marker;};
static struct Thread fake_thread;
static int fault,charged,created,freed,readied,sleeps;
static u64 fake_time;
static void loom_sqpoll_main(void *arg) {(void)arg;}
static void *kproc(void) {return NULL;}
static bool proc_sqpoll_ticket_charge(u64 stripes,const void *as,struct ProcSqpollTicket *t) {
 CHECK(!held && as && !t->stripes,"ticket charge outside Ll exactly once");
 if(fault==1)return false;t->stripes=stripes;charged++;return true;
}
static void proc_sqpoll_ticket_release(struct ProcSqpollTicket *t) {
 CHECK(!held,"ticket refund outside Ll");if(t->stripes){CHECK(charged==1,"single ticket refund");charged--;t->stripes=0;}
}
static struct Thread *thread_create_with_arg(void *proc,void(*entry)(void*),void *arg) {
 (void)proc;(void)entry;(void)arg;CHECK(!held && charged==1,"ticket precedes thread allocation");
 if(fault==2)return NULL;created++;
 if(fault==3){close_owner(test_ring);loom_private_close_changed(test_ring);}
 return &fake_thread;
}
static void thread_free(struct Thread *t) {
 CHECK(!held && t==&fake_thread && charged==1,"thread reclaimed before ticket refund");
 CHECK(!readied || test_ring->sqpoll_exited,"running thread exits before physical free");freed++;
}
static void ready(struct Thread *t) {
 CHECK(!held && t==test_ring->sqpoll && !test_ring->service_engine->worker_starting,"publish worker and release start token before ready");readied++;
}
static void wakeup(int *r) {(void)r;CHECK(!held,"wake outside Ll");}
static u64 timer_now_ns(void) {return fake_time;}
static int sleep(int *r,int(*cond)(void*),void *arg) {
 (void)r;CHECK(!held && test_ring->sqpoll_stopping && charged==1,"join stops and retains charge");sleeps++;
 test_ring->sqpoll_exited=true;CHECK(cond(arg),"join observes actual exit");return 0;
}
static int tsleep(int *r,int(*cond)(void*),void *arg,u64 deadline) {
 (void)r;(void)cond;(void)arg;(void)deadline;
 CHECK(!held && test_ring->service_engine->wait_active,"sleep owns registered wait sweep");sleeps++;
 struct loom_ring_hdr *h=(void *)(test_ring->ring_kva+test_ring->hdr_off);
 CHECK(h->flags&LOOM_RING_SQ_NEED_WAKEUP,"NEED_WAKEUP published before sleep");
 close_owner(test_ring);loom_private_close_changed(test_ring);return 0;
}
'''
main=r'''
int main(int argc,char **argv) {
 int which=argc>1?atoi(argv[1]):0;mode=0;
SETUP
 test_ring=&l;l.service_creator=42;
 if(which==4) {
  fake_time=3;loom_private_executor_loop(&l);
  CHECK(sleeps==1 && !index.wait_active && !index.wait_mask && !(ring.hdr.flags&LOOM_RING_SQ_NEED_WAKEUP),"private idle branch registers sleeps and disarms on close");
 }else{
  fault=which;
  int rc=loom_private_start_executor(&l);
  if(which==0) {
   CHECK(!rc && charged==1 && created==1 && readied==1 && l.sqpoll==&fake_thread,"one charged private worker");
   CHECK(loom_private_start_executor(&l)==-T_E_BUSY && charged==1 && created==1,"duplicate start refused");
   close_owner(&l);loom_private_close_changed(&l);
   CHECK(loom_engine_close_visit(&l)==0,"close completion waits for actual worker exit");
   loom_private_executor_join(&l);
   CHECK(sleeps==1 && freed==1 && !charged && !l.sqpoll && !index.worker_ticket.stripes,"join then exact ticket refund");
   CHECK(loom_engine_close_visit(&l)==2,"worker exit completes close");
  }else{
   CHECK(rc==(which==1?-T_E_AGAIN:which==2?-T_E_NOMEM:-T_E_CANCELED) && !charged && !l.sqpoll && !readied && !index.worker_starting,"start refusal settles token and ticket");
   CHECK(created==freed,"unpublished thread physically reclaimed");
  }
 }
 puts("PASS worker control");return 0;
}
'''.replace('SETUP',e['setup'])
loom=(D/'kernel/loom.c').read_text()
worker='\n'.join(e['extract'](loom,fn) for fn in ['loom_private_executor_exited','loom_private_executor_join','loom_private_executor_loop','loom_private_start_executor'])
mutants=[
 ('no-start-serialization','l->sqpoll || l->service_engine->worker_starting','false',0,'ticket charge outside Ll exactly once'),
 ('publish-after-ready','if (!rc) l->sqpoll=kt;','if (!rc) (void)kt;',0,'publish worker and release start token before ready'),
 ('forget-recheck','if (!rc && l->service_closing) rc=-T_E_CANCELED;','if (false) rc=-T_E_CANCELED;',3,'start refusal settles token and ticket'),
 ('drop-thread-rollback','if (kt) thread_free(kt);','if (kt) (void)kt;',3,'unpublished thread physically reclaimed'),
 ('forget-stop','__atomic_store_n(&l->sqpoll_stopping,true,__ATOMIC_RELEASE);','(void)l;',0,'join stops and retains charge'),
 ('skip-join','while (!__atomic_load_n(&l->sqpoll_exited,__ATOMIC_ACQUIRE))','while (false)',0,'running thread exits before physical free'),
 ('skip-disarm','loom_engine_wait_disarm(l);','(void)l;',4,'private idle branch registers sleeps and disarms on close'),
 ('forget-announce','__atomic_fetch_or(&h->flags,LOOM_RING_SQ_NEED_WAKEUP,__ATOMIC_RELEASE);','(void)h;',4,'NEED_WAKEUP published before sleep'),
]
out=D/('worker-controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir();results=[]
for tag,old,new,case,want in [('control',None,None,None,None)]+mutants:
 code=worker
 if old:assert code.count(old)==1,(tag,code.count(old));code=code.replace(old,new)
 src=out/(tag+'.c');src.write_text(pre+e['connect']+e['engine']+e['post']+code+main)
 exe=out/tag;cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-but-set-variable','-fsanitize=address,undefined','-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip(),'-I',str(R/'kernel/include'),str(src),str(R/'kernel/loom_service_requests.c'),str(R/'kernel/loom_service_slots.c'),str(R/'kernel/loom_service_nodes.c'),'-o',str(exe)]
 (out/(tag+'-command.json')).write_text(json.dumps(cmd)+'\n')
 with (out/(tag+'-build.log')).open('wb') as f:ret=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
 assert ret.returncode==0,(tag,'build',out)
 for n in range(5) if case is None else [case]:
  ret=subprocess.run([str(exe),str(n)],capture_output=True,text=True,timeout=10);output=ret.stdout+ret.stderr;(out/(tag+'-'+str(n)+'.log')).write_text(output)
  assert ret.returncode==(11 if want else 0) and ret.stdout==(want or 'PASS worker control')+'\n' and not ret.stderr,(tag,n,ret.returncode,output,out)
  results.append(dict(tag=tag,case=n,rc=ret.returncode,source_sha256=hashlib.sha256(src.read_bytes()).hexdigest(),log_sha256=hashlib.sha256(output.encode()).hexdigest()))
 print('PASS',tag,flush=True)
(out/'result.json').write_text(json.dumps({'results':results,'inputs':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [D/'kernel/loom.c',D/'worker-control.py',D/'control.py']},'limits':'Extracted actual private executor admission/loop/join; actual readiness and ledgers; deterministic thread/ticket/poll boundary doubles. No native thread scheduling proof.'},indent=2)+'\n');print(out)
