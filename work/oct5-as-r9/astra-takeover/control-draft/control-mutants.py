from pathlib import Path
import json,re,subprocess,hashlib,datetime,os
D=Path(__file__).resolve().parent;env={'__file__':str(D/'control-regression.py')}
exec((D/'control-regression.py').read_text().split("out=D/('controls-'")[0],env)
R=env['R'];pre=env['pre'];connect=env['connect'];control=env['control'];ex=env['ex']
old_rollback='''    connect_rollback_locked(l,p->scope,p->request);
    p->connect_bound = false;
    spin_unlock(&l->lock);
    return loom_private_protocol_release_unused(p);'''
start=connect.index('    struct loom_service_ref scope=p->scope;');end=connect.index('\n}',start)
mutants=[
 ('creator','control','p->as!=l->service_as || p->stripes!=l->service_creator','p->as!=l->service_as',0,'foreign creator refuses untouched output'),
 ('image','control','p->as!=l->service_as || p->stripes!=l->service_creator','p->stripes!=l->service_creator',0,'foreign image denied'),
 ('query-input','control','in->kind || in->state','false || in->state',0,'output fields rejected without write'),
 ('stale','control','s->phase==LSLOT_LIVE && s->incarnation==ref.incarnation','s->phase==LSLOT_LIVE',0,'stale query leaves output unchanged'),
 ('refused-output','control','if(!rc)*out=result;','*out=result;',0,'stale query leaves output unchanged'),
 ('control-flags','control','in->flags || in->reserved || in->object.reserved','false || in->reserved || in->object.reserved',0,'unknown flags denied'),
 ('prepared-count','control','r->phase==LREQ_PREPARED || r->phase==LREQ_ACTIVE','r->phase==LREQ_ACTIVE',2,'PREPARED owner counted'),
 ('delivered-count','control','if(r->phase==LREQ_TERMINAL)++result.pending_terminals;','if(r->phase>=LREQ_TERMINAL)++result.pending_terminals;',3,'DELIVERED is storage not active obligation'),
 ('raw-bytes','control','if(r->borrows)result.status_flags|=LOOM_SERVICE_BYTES_SENT;','if(false)result.status_flags|=LOOM_SERVICE_BYTES_SENT;',4,'unindexed in-flight driver reports possible bytes'),
 ('first-reason','connect','if (!n->reason) n->reason=p->abort_reason;','(void)n;',3,'abort retains prior success and diagnostics'),
 ('sticky-bytes','connect','if (p->progress.io.bytes_sent) n->status_flags|=LOOM_SERVICE_BYTES_SENT;','(void)p->progress.io.bytes_sent;',3,'READY has retained terminal and sticky bytes'),
 ('late-abort','connect','if (n->state==LOOM_SERVICE_ABORTING && n->reason<0) connect_latch(p,n,n->reason);','(void)n;',4,'CONNECT lost success obligation'),
 ('early-rollback','connect',connect[start:end],old_rollback,2,'destruction boundary retains observable ownership'),
 ('failure-phase','connect','p->failure_phase=attempted_phase; n->failure_phase=attempted_phase;','p->failure_phase=attempted_phase;',7,'retained actual failure phase'),
 ('early-retired','control','if(!s->pins) {n->state=LOOM_SERVICE_RETIRED;','if(true) {n->state=LOOM_SERVICE_RETIRED;',2,'abort denies raw publication'),
]
out=D/('mutants-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir();results=[]
sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
for tag,unit,old,new,case,want in mutants:
 c=control;k=connect;s={'control':c,'connect':k}[unit];assert s.count(old)==1,(tag,s.count(old));s=s.replace(old,new)
 if unit=='control':c=s
 else:k=s
 src=out/(tag+'.c');src.write_text(pre+env['nodewrap']+k+env['engine']+env['post']+c+env['dest']+env['main'])
 exe=out/tag;cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-but-set-variable','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I',str(D/'kernel/include'),'-I',str(R/'kernel/include'),str(src),str(R/'kernel/loom_service_requests.c'),str(R/'kernel/loom_service_slots.c'),str(D/'kernel/loom_service_nodes.c'),'-o',str(exe)]
 (out/(tag+'-command.json')).write_text(json.dumps(cmd)+'\n');b=subprocess.run(cmd,capture_output=True,text=True);(out/(tag+'-build.log')).write_text(b.stdout+b.stderr);assert b.returncode==0,(tag,b.stderr)
 r=subprocess.run([str(exe),str(case)],capture_output=True,text=True,timeout=15,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});(out/(tag+'.log')).write_text(r.stdout+r.stderr)
 assert r.returncode==11 and r.stdout==want+'\n' and not r.stderr,(tag,out,r.returncode,r.stdout,r.stderr)
 results.append({'case':tag,'mode':case,'expected':want,'rc':r.returncode,'source_sha256':hashlib.sha256(src.read_bytes()).hexdigest()});print('PASS mutant',tag,flush=True)
(out/'result.json').write_text(json.dumps({'cases':results,'scope':'Draft actual C mutation sensitivity, not native qualification'},indent=2)+'\n');print(out)
