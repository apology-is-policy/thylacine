#!/usr/bin/env python3
"""Actual client functions and full header; real transport/session/wire codecs.
Host doubles replace scheduler/allocator/cache. Native fixture uses real client.
Acquire Yip before running. This does not qualify private Loom ownership.
"""
from pathlib import Path
import argparse,os,shlex,subprocess,tempfile,re
ROOT=Path(__file__).resolve().parent.parent
NAMES=['map_error','client_copy','client_mark_dead_locked','client_send_progress_signal','client_orphan_fid_locked','ownerless_dispatch_locked','client_honour_locked','demux_frame_locked','p9_client_submit_async','p9_client_progress_bind','p9_client_progress_abort','p9_client_progress_step','p9_client_init','p9_client_destroy','p9_client_handshake','client_max_read_count','p9_client_read']
MUTANTS=[
 ('overwrite-tx','if (c->progress && c->progress->io.tx)', 'if (false)', 'pending TX refuses buffer overwrite'),
 ('starve-rx','if (p->io.tx && !p->rx_next)', 'if (p->io.tx)', 'blocked sender cannot starve reply'),
 ('early-reply','(!owner || owner->sending)', '(!owner)', 'malformed or premature reply aborts once'),
 ('no-tx-mark','        rpc->sending = true;', '        rpc->sending = false;', 'malformed or premature reply aborts once'),
 ('lost-terminal','            r->on_complete(r, async_status, NULL);', '            (void)async_status;', 'abort completes pending TX once'),
 ('double-terminal','            r->on_complete(r, async_status, NULL);', '            r->on_complete(r, async_status, NULL); r->on_complete(r, async_status, NULL);', 'abort completes pending TX once'),
 ('retain-tx-borrow','        c->progress->sending = NULL;', '        (void)c->progress;', 'abort detaches TX before retirement'),
 ('blocking-entry','if (c->progress) CLIENT_UNLOCK_RET(c, -P9_E_INVAL);', 'if (false) CLIENT_UNLOCK_RET(c, -P9_E_INVAL);', 'blocking engine reached'),
]
def function(s,name):
 m=re.search(r'^(?:static )?[a-zA-Z_][\w *]*\b'+name+r'\([^;]*?\)\s*\{',s,re.M);assert m,name
 start=m.start();i=m.end();depth=1
 # Braces inside this source's comments balance; discard comments before locating end.
 while depth:
  if s.startswith('//',i):i=s.index('\n',i);continue
  if s.startswith('/*',i):i=s.index('*/',i)+2;continue
  if s[i]=='{':depth+=1
  if s[i]=='}':depth-=1
  i+=1
 return s[start:i]
def main():
 p=argparse.ArgumentParser();p.add_argument('--logs',type=Path,required=True);p.add_argument('--sanitize',action='store_true');p.add_argument('--mutants',action='store_true');a=p.parse_args();a.logs.mkdir(parents=True,exist_ok=True)
 source=(ROOT/'kernel/9p_client.c').read_text();actual='\n\n'.join(function(source,n) for n in NAMES)
 template=(ROOT/'tools/host-tests/9p-client-progress.c').read_text();assert template.count('/* ACTUAL_CLIENT */')==1
 with tempfile.TemporaryDirectory(prefix='9p-client-progress-') as tmp:
  for label,old,new,want in [('clean',None,None,None)]+(MUTANTS if a.mutants else []):
   altered=actual
   if old:
    assert old in altered,label
    altered=altered.replace(old,new)
   src=Path(tmp)/(label+'.c');src.write_text(template.replace('/* ACTUAL_CLIENT */',altered));exe=src.with_suffix('')
   cmd=[os.environ.get('CC','clang'),'-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I',str(ROOT/'kernel/include'),'-I',str(ROOT/'kernel/test'),str(src),str(ROOT/'kernel/9p_transport.c'),str(ROOT/'kernel/9p_session.c'),str(ROOT/'kernel/9p_wire.c'),'-o',str(exe)]+shlex.split(os.environ.get('CFLAGS',''))
   if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
   with (a.logs/(label+'-build.log')).open('wb') as out:subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT,check=True)
   r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20);(a.logs/(label+'.log')).write_text(r.stdout+r.stderr)
   if want:assert r.returncode==1 and want in r.stderr,(label,r.returncode,r.stdout,r.stderr)
   else:assert r.returncode==0,(r.returncode,r.stdout,r.stderr)
   print('PASS '+label+(': '+want if want else ': actual private client fixture'),flush=True)
if __name__=='__main__':main()
