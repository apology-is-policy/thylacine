from pathlib import Path
import json,subprocess,re,hashlib
D=Path(__file__).resolve().parent
out=D/'queue-control-2';out.mkdir(exist_ok=False)
h=(D/'kernel/include/thylacine/loom_service_close_queue.h').read_text()
s=(D/'kernel/loom_service_close_queue.c').read_text()
pre='''#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x,m) do { if (!(x)) { fprintf(stderr,"FAIL: %s\\n",m); exit(73); } } while (0)
_Noreturn static void extinction(const char *s) { fprintf(stderr,"EXTINCTION: %s\\n",s); exit(74); }
'''
fixture=r'''
static struct loom_close_queue q;
static struct loom_close_work a,b;
static unsigned roots;
static void init(void) { memset(&a,0,sizeof a);memset(&b,0,sizeof b);roots=0;loom_closeq_init(&q); }
static bool start(struct loom_close_work *w) { bool added=loom_closeq_start(&q,w);if(added)roots++;return added; }
static enum loom_close_finish finish(struct loom_close_work *w,bool done,bool more) {
 enum loom_close_finish r=loom_closeq_finish(&q,w,done,more);
 if(r==LCLOSE_DROP){CHECK(roots,"root release underflow");roots--;}
 return r;
}
int main(void) {
 init();
 CHECK(!loom_closeq_take(&q),"empty queue has no work");
 CHECK(!loom_closeq_kick(&q,&a) && a.state==LCLOSE_NEW,"kick cannot start a root");
 CHECK(start(&a) && roots==1,"first start owns one root");
 CHECK(!start(&a) && roots==1,"duplicate start retains exactly one root");
 CHECK(!loom_closeq_kick(&q,&a) && a.next==NULL,"ready kick cannot duplicate link");
 CHECK(start(&b) && roots==2,"second ring owns separate root");
 CHECK(loom_closeq_take(&q)==&a && a.state==LCLOSE_RUNNING && !a.next,"take transfers membership without dropping root");
 CHECK(finish(&a,false,true)==LCLOSE_AGAIN,"immediately useful work requeues");
 CHECK(loom_closeq_take(&q)==&b,"fair FIFO gives other ring a turn");
 CHECK(finish(&b,true,false)==LCLOSE_DROP && roots==1,"complete drops exactly one root");
 CHECK(!start(&b) && !loom_closeq_kick(&q,&b) && b.state==LCLOSE_DONE,"completed ring cannot restart");
 CHECK(loom_closeq_take(&q)==&a && !q.head && q.tail==&q.head,"tail returns to queue head after last take");
 CHECK(finish(&a,false,false)==LCLOSE_WAIT && a.state==LCLOSE_PARKED && roots==1 && !q.head,"blocked ring parks without self wake");
 CHECK(!loom_closeq_take(&q),"parked ring consumes no runnable turn");
 CHECK(loom_closeq_kick(&q,&a) && a.state==LCLOSE_READY && roots==1,"external put wakes parked ring using same root");
 CHECK(!loom_closeq_kick(&q,&a) && !a.next,"multiple parked kicks append once");
 CHECK(loom_closeq_take(&q)==&a && !a.dirty,"new turn consumes earlier kick");
 CHECK(!loom_closeq_kick(&q,&a) && a.dirty,"event between observation and park recorded");
 CHECK(finish(&a,false,false)==LCLOSE_AGAIN && q.head==&a,"concurrent external event prevents missed wake");
 CHECK(loom_closeq_take(&q)==&a && !a.dirty,"old dirty bit does not survive next take");
 CHECK(finish(&a,false,false)==LCLOSE_WAIT && !q.head,"dirty event grants one recheck not endless spin");
 CHECK(loom_closeq_kick(&q,&a) && loom_closeq_take(&q)==&a,"second external event starts next turn");
 CHECK(!loom_closeq_kick(&q,&a) && a.dirty,"completion can race harmless late kick");
 CHECK(finish(&a,true,true)==LCLOSE_DROP && roots==0 && a.state==LCLOSE_DONE && !q.head,"complete outranks stale kick and more flag");
 CHECK(!loom_closeq_kick(&q,&a) && !start(&a),"late kicks cannot resurrect completed root");
 puts("PASS queue control: FIFO, exact root actions, duplicate start, park quiet, before/after park kicks, completion, no restart");
 return 0;
}
'''
mutants={
 'duplicate-start':('if (w->state != LCLOSE_NEW) return false;','if (w->state == LCLOSE_DONE) return false;','duplicate start retains exactly one root'),
 'restart':('if (w->state != LCLOSE_NEW) return false;','if (w->state != LCLOSE_NEW && w->state != LCLOSE_DONE) return false;','completed ring cannot restart'),
 'lost-running-kick':('if (w->state == LCLOSE_RUNNING) w->dirty=true;','if (w->state == LCLOSE_RUNNING) w->dirty=false;','event between observation and park recorded'),
 'lost-parked-kick':('if (w->state != LCLOSE_PARKED) return false;','if (true || w->state != LCLOSE_PARKED) return false;','external put wakes parked ring using same root'),
 'self-wake':('if (more || w->dirty) {','if (true || more || w->dirty) {','blocked ring parks without self wake'),
 'stale-dirty':('w->next=NULL; w->state=LCLOSE_RUNNING; w->dirty=false;','w->next=NULL; w->state=LCLOSE_RUNNING;','old dirty bit does not survive next take'),
 'drop-early':('return LCLOSE_WAIT;','return LCLOSE_DROP;','blocked ring parks without self wake'),
 'drop-leak':('return LCLOSE_DROP;','return LCLOSE_WAIT;','complete drops exactly one root'),
 'head-steal':('*q->tail=w; q->tail=&w->next;','w->next=q->head; q->head=w; if(!w->next)q->tail=&w->next;','take transfers membership without dropping root'),
 'stale-tail':('if (!q->head) q->tail=&q->head;','if (false) q->tail=&q->head;','tail returns to queue head after last take'),
}
results=[]
for name in ['clean']+list(mutants):
 code=s;want=None
 if name!='clean':
  old,new,want=mutants[name];assert code.count(old)==1,(name,code.count(old));code=code.replace(old,new)
 src=pre+re.sub(r'^#include.*\n','',h,flags=re.M)+re.sub(r'^#include.*\n','',code,flags=re.M)+fixture
 f=out/(name+'.c');f.write_text(src);binary=out/(name+'.exe')
 cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip(),'-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(f),'-o',str(binary)]
 with (out/(name+'-compile.log')).open('wb') as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
 assert r.returncode==0,(name,'compile')
 with (out/(name+'.log')).open('wb') as log:r=subprocess.run([str(binary)],stdout=log,stderr=subprocess.STDOUT,timeout=5)
 text=(out/(name+'.log')).read_text()
 assert (r.returncode==0 and 'PASS queue control:' in text) if want is None else (r.returncode==73 and 'FAIL: '+want in text),(name,r.returncode,text)
 results.append(dict(case=name,expected=want,rc=r.returncode,source_sha256=hashlib.sha256(src.encode()).hexdigest(),log_sha256=hashlib.sha256(text.encode()).hexdigest()))
 print('PASS',name,flush=True)
(out/'result.json').write_text(json.dumps({'inputs':{p:hashlib.sha256((D/p).read_bytes()).hexdigest() for p in ['kernel/loom_service_close_queue.c','kernel/include/thylacine/loom_service_close_queue.h','queue-control.py']},'results':results,'limits':'Actual draft C queue transitions; simulated ref actions, serialized event interleavings. Kernel wrapper locks, real reference ownership, protocol retirement and native races still pending.'},indent=2)+'\n')
