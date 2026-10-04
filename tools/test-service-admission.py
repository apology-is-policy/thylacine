#!/usr/bin/env python3
"""AS-2b actual C admission/permission bodies with controlled lifetime fixtures.
Yip lease required. Native tests separately cover actual kernel integrations.
"""
from pathlib import Path
import argparse,os,shlex,subprocess,tempfile
ROOT=Path(__file__).resolve().parent.parent
MUTANTS=[
 ('wrong-image',' || p->as != c->as','', 'old image cannot borrow replacement'),
 ('dead-creator','p->state != PROC_STATE_ALIVE || ','', 'zombie cannot admit'),
 ('no-generation','(!generation || svc->generation == generation)','(!generation || true)', 'target refuses replacement generation'),
 ('slot-aba','if (!svc || (expected && svc != expected))','if (!svc || (expected && false))','target refuses same generation in another slot'),
 ('publish-rebind','svc->generation == a->generation && svc->state','true && svc->state','publication refuses replacement'),
 ('raw-escape','if (strict && (out->mode','if (false && strict && (out->mode','strict target refuses raw byte mode'),
 ('provider-charge','srvconn_create_in(view->domain,','srvconn_create_in(view->source ? view->source->domain : view->domain,','route charges consumer view'),
 ('drop-groups','out->supp_gid_count = n;','out->supp_gid_count = 0;','supplementary group retained'),
 ('owner-fallthrough','if (id->principal_id == st->uid)','if (false && id->principal_id == st->uid)','owner-first permissions'),
 ('no-teardown','if (!a->published) srvconn_teardown(a->conn);','if (false) srvconn_teardown(a->conn);','cancel prepare tears local connection'),
]
def main():
 p=argparse.ArgumentParser();p.add_argument('--logs',type=Path,required=True);p.add_argument('--mutants',action='store_true');p.add_argument('--sanitize',action='store_true');a=p.parse_args();a.logs.mkdir(parents=True,exist_ok=True)
 def source(path,start,end):
  s=(ROOT/path).read_text();return s[s.index(start):s.index(end,s.index(start))]
 pieces={
  'IDENTITY_DECL':source('kernel/include/thylacine/proc.h','struct ProcAccessIdentity {','// Direct capture'),
  'PERMISSIONS':source('kernel/perm.c','bool perm_identity_from_proc','unsigned perm_want_for_omode'),
  'SNAPSHOT':source('kernel/proc.c','bool proc_service_identity_snapshot','// proc_caps_by_stripes'),
  'ADMISSION_DECL':source('kernel/include/thylacine/devsrv.h','struct SrvServiceTarget {','// F2 close (P5-corvus'),
  'AUTHORIZED':source('kernel/devsrv.c','bool devsrv_srv_connect_authorized','s64 devsrv_open_errno'),
  'ADMISSION':source('kernel/devsrv.c','struct srv_post_snapshot {','struct Spoor *devsrv_open_connect'),
 }
 fixture=(ROOT/'tools/host-tests/service-admission.c').read_text()
 for key,value in pieces.items():fixture=fixture.replace('/* ACTUAL_'+key+' */',value)
 with tempfile.TemporaryDirectory(prefix='service-admission-') as tmp:
  for name,old,new,expected in [('clean',None,None,None)]+(MUTANTS if a.mutants else []):
   s=fixture
   if old:assert s.count(old)==1,(name,s.count(old));s=s.replace(old,new)
   f=Path(tmp)/(name+'.c');f.write_text(s);exe=f.with_suffix('')
   cmd=[os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror','-O1','-pthread',str(f),'-o',str(exe)]+shlex.split(os.environ.get('CFLAGS',''))
   if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
   with (a.logs/(name+'-build.log')).open('wb') as out:subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT,check=True)
   r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=15);(a.logs/(name+'.log')).write_text(r.stdout+r.stderr)
   if expected:assert r.returncode==1 and expected in r.stderr,(name,r.returncode,r.stdout,r.stderr)
   else:assert r.returncode==0,(r.stdout,r.stderr)
   print('PASS '+name+(': '+expected if expected else ': actual-source admission'),flush=True)
if __name__=='__main__':main()
