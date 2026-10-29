from pathlib import Path
import re,json,subprocess,datetime,hashlib,difflib
D=Path(__file__).resolve().parent;R=D.parents[3]
M=[
('malformed-scope','if(result<0 && o->capture_error && !o->protocol->abort_reason)return -T_E_BUSY;','', 'malformed capture requires scope abort'),
('request-identity','r->identity==o->request.identity','(u32)r->identity==(u32)o->request.identity','full request identity'),
('owner-ledger','r->owners==1 ? r : NULL','true ? r : NULL','physical owner required'),
('ready-admission','n->state!=LOOM_SERVICE_READY ||','false ||','admission requires READY'),
('request-borrow','if(loom_request_borrow(&l->service_requests,l->service_slots,l->service_nodes,o->request))','if(false)','enqueue owns active borrow and list'),
('list-tail','p->op_tail=o;++p->op_count;','p->op_tail=NULL;++p->op_count;','enqueue owns active borrow and list'),
('queue-bound','if(p->op_count>=l->service_requests.capacity)','if(false)','queue capacity refusal'),
('driver-begin','if(!o->protocol->driving || o->phase!=LRPC_QUEUED','if(o->phase!=LRPC_QUEUED','sole driver required'),
('wire-expiry','if(r->deadline && r->deadline<=now)return -T_E_TIMEDOUT;\n    o->callback_seen','if(false)return -T_E_TIMEDOUT;\n    o->callback_seen','expiry before wire'),
('callback-arm','o->rpc.on_complete=loom_rpc_capture;o->phase=LRPC_INFLIGHT;','o->rpc.on_complete=NULL;o->phase=LRPC_INFLIGHT;','callback armed before submit'),
('early-settle','(o->phase!=LRPC_INFLIGHT || !o->callback_seen)))','(o->phase!=LRPC_INFLIGHT)))','inflight callback retains owner'),
('pending-sender','o->rpc.sending || (o->phase!=LRPC_QUEUED','false || (o->phase!=LRPC_QUEUED','pending sender blocks settlement'),
('early-terminal','o->callback_seen=true;','o->callback_seen=true;o->phase=LRPC_TERMINAL;','callback copies bytes without terminal'),
('duplicate-callback','o->phase!=LRPC_INFLIGHT || o->callback_seen','o->phase!=LRPC_INFLIGHT','duplicate callback fails closed'),
('finish-borrow','r->phase!=LREQ_ACTIVE || r->borrows!=1 || o->rpc.on_complete','r->phase!=LREQ_ACTIVE || false || o->rpc.on_complete','all physical borrowers must end'),
('commit-expiry','        if(r->deadline && r->deadline<=now)return -T_E_TIMEDOUT;','        (void)now;','deadline beats captured success'),
('uncaptured-success','if(!o->callback_seen || o->capture_error || o->reply.result<0)','if(false)','queued cancellation cannot fabricate success'),
('physical-owner-drop','o->phase=LRPC_TERMINAL;\n    return 0;','o->phase=LRPC_TERMINAL;l->service_requests.records[o->request.index].owners=0;\n    return 0;','negative terminal ends borrow retains storage'),
('early-unlink','r->phase!=LREQ_DELIVERED || r->borrows','r->phase!=LREQ_TERMINAL || r->borrows','CQ backpressure retains discoverable owner'),
('unlink-count','--p->op_count;o->next=NULL;','o->next=NULL;','middle unlink retains neighbors'),
('retry-borrow','o->rpc.on_complete=NULL;o->callback_seen=false;o->phase=LRPC_QUEUED;','o->rpc.on_complete=NULL;o->callback_seen=false;o->phase=LRPC_QUEUED;--r->borrows;','retry preserves one obligation and borrow'),
]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
pre=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_requests.h>
#include <thylacine/loom_service_reply.h>
#include <thylacine/errno.h>
#define PAGE_SIZE 4096
struct p9_rpc {void (*on_complete)(struct p9_rpc *,int,struct p9_dispatch_result *);bool sending;};
struct Loom {struct loom_slot_table *service_slots;struct loom_service_node_table *service_nodes;struct loom_request_table service_requests;bool service_closing;};
struct loom_service_operation;
struct loom_service_protocol {struct Loom *owner;struct loom_service_ref scope;bool driving,connect_bound,indexed,locally_retired;s32 abort_reason;struct loom_service_operation *op_head,*op_tail;u32 op_count;};
static void extinction(const char *m){fprintf(stderr,"INVARIANT %s\n",m);exit(86);}
'''
def main():
 resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True)
 assert re.search(r'^mac +HELD by you',resources,re.M)
 import os
 st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
 out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 paths=[D/'kernel/loom_service_rpc.c',D/'kernel/include/thylacine/loom_service_rpc.h',D/'kernel/include/thylacine/loom_service_operation.h',D/'kernel/include/thylacine/loom_service_protocol.h',D/'fixture.c',D/'run-controls.py']+[R/f for f in ['kernel/loom_service_requests.c','kernel/loom_service_nodes.c','kernel/loom_service_slots.c','kernel/loom_service_reply.c','kernel/include/thylacine/loom_service_requests.h','kernel/include/thylacine/loom_service_nodes.h','kernel/include/thylacine/loom_service_reply.h','kernel/include/thylacine/9p_session.h']]
 hashes={str(p.relative_to(R)):sha(p) for p in paths};original=paths[0].read_text()
 header=paths[2].read_text();declaration=header[header.index('enum loom_rpc_phase'):header.index('// Caller retains l;')]
 prototypes=re.sub(r'^#.*\n','',paths[1].read_text(),flags=re.M)
 sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();results=[]
 for name,old,new,want in [('control',None,None,None)]+M:
  if old is not None:assert original.count(old)==1,(name,original.count(old))
  s=original if old is None else original.replace(old,new)
  source=pre+declaration+prototypes+re.sub(r'^#include[^\n]*\n','',s,flags=re.M)+paths[4].read_text()
  p=out/(name+'.c');p.write_text(source);exe=out/(name+'.exe')
  (out/(name+'.diff')).write_text(''.join(difflib.unified_diff(original.splitlines(True),s.splitlines(True),fromfile='kernel/loom_service_rpc.c',tofile=name+'.c')))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I'+str(R/'kernel/include'),str(p),*[str(p) for p in paths[6:10]],'-o',str(exe)]
  (out/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
  build=subprocess.run(cmd,capture_output=True);(out/(name+'-build.log')).write_bytes(build.stdout+build.stderr);assert not build.returncode,(name,build.stderr.decode())
  run=subprocess.run([str(exe)],capture_output=True,timeout=20);(out/(name+'.log')).write_bytes(run.stdout+run.stderr)
  assert (run.returncode==1 and run.stderr==f'FAIL {want}\n'.encode()) if want else (run.returncode==0 and not run.stderr),(name,run.returncode,run.stdout,run.stderr)
  results.append(dict(case=name,expected=want,rc=run.returncode,source_sha256=sha(p),binary_sha256=sha(exe)))
  print('PASS',name,want or run.stdout.decode().strip(),flush=True)
 assert hashes=={str(p.relative_to(R)):sha(p) for p in paths}
 (out/'result.json').write_text(json.dumps(dict(inputs=hashes,results=results,limits='Actual RPC owner transitions and real request/node/slot/reply modules; simplified host Loom/protocol/RPC layout boundary. Sequential only. No actual client submission, authority, wire or concurrent-engine proof.'),indent=2)+'\n');print(out)
if __name__=='__main__':main()
