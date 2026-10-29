from pathlib import Path
import subprocess,json,re,hashlib,datetime
R=Path('/Users/northkillpd/projects/thylacine-corona');F=Path(__file__).resolve().parent
# Reuse checked boundary doubles, build the actual current source before fixing.
D=R/'work/oct5-as-r9/astra-takeover/close-draft'
env={'__file__':str(D/'engine-control.py')}
exec((D/'engine-control.py').read_text().split("out=D/('engine-controls-'")[0],env)
pre=env['pre'];ex=env['e']['c'].post.extract
for name in ['loom_private_close_changed','loom_ref','loom_unref']:
 pre=pre.replace(ex.fun(pre,name),'',1)
fn=ex.fun(pre,'loom_private_protocol_release_unused')
new=fn.replace(' releases++;free(p);', ''' CHECK(loom_engine_close_visit(p->owner)==0 && !p->owner->service_requests.detached,
       "rollback retains close barrier through physical protocol destruction");
 releases++;free(p);''')
assert new!=fn;pre=pre.replace(fn,new)
body=lambda p:re.sub(r'^#include[^\n]*\n','',p.read_text(),flags=re.M)
source=(R/'kernel/loom_service_connect.c').read_text()
old='''    connect_rollback_locked(l,p->scope,p->request);
    p->connect_bound = false;
    spin_unlock(&l->lock);
    return loom_private_protocol_release_unused(p);'''
new='''    struct loom_service_ref scope=p->scope;
    struct loom_request_ref request=p->request;
    p->connect_bound = false;
    spin_unlock(&l->lock);
    // Retain the PREPARED obligation and scope pins while real cursor/storage
    // destruction runs unlocked. A closing worker must still see this owner.
    // The public wrapper retains l across the consuming final protocol drop.
    if (loom_private_protocol_release_unused(p)) extinction("CONNECT lost unused protocol ownership");
    spin_lock(&l->lock);
    connect_rollback_locked(l,scope,request);
    spin_unlock(&l->lock);
    return 0;'''
assert source.count(old)==1;fixed=source.replace(old,new)
(F/'fixed-connect.c').write_text(fixed)
results=[];sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
for tag,text in [('before',source),('after',fixed)]:
 out=F/(tag+'-2');out.mkdir(exist_ok=False);src=out/'control.c';src.write_text(pre+re.sub(r'^#include[^\n]*\n','',text,flags=re.M)+body(R/'kernel/loom_service_engine.c')+env['post']+env['main'])
 exe=out/'control';cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-but-set-variable','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I',str(R/'kernel/include'),str(src),str(R/'kernel/loom_service_requests.c'),str(R/'kernel/loom_service_slots.c'),str(R/'kernel/loom_service_nodes.c'),'-o',str(exe)]
 b=subprocess.run(cmd,capture_output=True,text=True);(out/'build.log').write_text(b.stdout+b.stderr);assert b.returncode==0,b.stderr
 (out/'command.json').write_text(json.dumps(cmd)+'\n')
 r=subprocess.run([str(exe),'21'],capture_output=True,text=True,timeout=15);(out/'raw.log').write_text(r.stdout+r.stderr)
 want='rollback retains close barrier through physical protocol destruction\n' if tag=='before' else 'PASS close control\n'
 assert r.returncode==(11 if tag=='before' else 0) and r.stdout==want and not r.stderr,(tag,r.returncode,r.stdout,r.stderr)
 results.append({'case':tag,'rc':r.returncode,'output':r.stdout,'actual_connect_sha256':hashlib.sha256(text.encode()).hexdigest()})
(F/'result.json').write_text(json.dumps({'defect':'CLOSE-R1','scope':'Deterministic actual-C engine visit inside checked protocol destruction boundary; no native claim.','results':results},indent=2)+'\n');print(json.dumps(results,indent=2))
