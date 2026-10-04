#!/usr/bin/env python3
"""Actual process-budget source, pthread lifetime lock double, named mutants."""
from pathlib import Path
import argparse,os,shlex,subprocess,tempfile
ROOT=Path(__file__).resolve().parent.parent
MUTANTS=[
 ('image','p->as == c->as &&','true &&','wrong image refused'),
 ('zombie','p->state == PROC_STATE_ALIVE &&','true &&','zombie refused'),
 ('terminating','!p->group_exit_msg &&','true &&','terminating refused'),
 ('cap','(proc_resource_exempt(p) ||','(true || proc_resource_exempt(p) ||','shared cap enforced'),
 ('occupied','if (!ticket->stripes) {','if (true) {','occupied ticket refused'),
 ('consume','ticket->stripes = 0;','/* lost consumption */','refund survives exec'),
 ('identity','if (p->stripes != c->stripes) return 0;','if (false) return 0;','missing creator refused'),
 ('overflow','p->loom_sqpoll_count < 0x7fffffff','p->loom_sqpoll_count <= 0x7fffffff','exempt overflow refused'),
]
def main():
 p=argparse.ArgumentParser();p.add_argument('--logs',type=Path,required=True);p.add_argument('--mutants',action='store_true');a=p.parse_args();a.logs.mkdir(parents=True,exist_ok=True)
 s=(ROOT/'kernel/proc.c').read_text();body=s[s.index('struct sqpoll_ticket_ctx {'):s.index('bool proc_child_cap_ok(')]
 s=(ROOT/'kernel/include/thylacine/proc.h').read_text();decl=s[s.index('struct ProcSqpollTicket {'):s.index('// proc_child_cap_ok')]
 fixture=(ROOT/'tools/host-tests/sqpoll-ticket.c').read_text().replace('/* ACTUAL_DECL */',decl).replace('/* ACTUAL_BODY */',body)
 with tempfile.TemporaryDirectory(prefix='sqpoll-ticket-') as tmp:
  for name,old,new,expected in [('clean',None,None,None)]+(MUTANTS if a.mutants else []):
   s=fixture
   if old:assert s.count(old)==1,(name,s.count(old));s=s.replace(old,new)
   f=Path(tmp)/(name+'.c');f.write_text(s);exe=f.with_suffix('')
   cmd=[os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror','-O1','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all','-g',str(f),'-o',str(exe)]+shlex.split(os.environ.get('CFLAGS',''))
   with (a.logs/(name+'-build.log')).open('wb') as out:subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT,check=True)
   r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=30);(a.logs/(name+'.log')).write_text(r.stdout+r.stderr)
   # Overflow mutant is stopped by UBSan before the boolean postcondition.
   message='signed integer overflow' if name=='overflow' else expected
   if expected:assert r.returncode!=0 and message in r.stderr,(name,r.returncode,r.stdout,r.stderr)
   else:assert r.returncode==0,(r.stdout,r.stderr)
   print('PASS '+name+(': '+message if expected else ''),flush=True)
if __name__=='__main__':main()
