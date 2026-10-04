#!/usr/bin/env python3
"""Actual AddrSpace declaration/lifecycle under controlled owner/pin schedules.
Acquire Yip before running. Allocator, VMA drain and page-table destruction are
host doubles; native tests separately cover process authority and device reset.
The source slice stops before clone helpers: this does not qualify the MMU.
"""
from pathlib import Path
import argparse,os,shlex,subprocess,tempfile
ROOT=Path(__file__).resolve().parent.parent
MUTANTS=[
 ('pin-as-owner','void addrspace_pin(struct AddrSpace *as) {\n    addrspace_lifetime_get(as);','void addrspace_pin(struct AddrSpace *as) {\n    addrspace_ref(as);','pin is not an owner'),
 ('no-owner-get','int pre = __atomic_fetch_add(&as->owners, 1, __ATOMIC_ACQ_REL);','int pre = __atomic_load_n(&as->owners, __ATOMIC_ACQUIRE);','owner increments both counts'),
 ('skip-drain','if (pre == 1) vma_drain_in(as);','if (false) vma_drain_in(as);','last owner drains before last pin'),
 ('drain-every-owner','if (pre == 1) vma_drain_in(as);','vma_drain_in(as);','nonfinal owner preserves mappings'),
 ('pin-leak','void addrspace_unpin(struct AddrSpace *as) {\n    addrspace_lifetime_put(as);','void addrspace_unpin(struct AddrSpace *as) {\n    (void)as;','final pin frees once without another drain'),
 ('drop-before-drain','if (pre == 1) vma_drain_in(as);\n    addrspace_lifetime_put(as);','addrspace_lifetime_put(as);\n    if (pre == 1) vma_drain_in(as);','extinction: AddrSpace final lifetime drop before mapping drain'),
 ('early-destroy','if (pre > 1) return;','if (false) return;','extinction: AddrSpace final lifetime drop with live owners'),
]
def main():
 p=argparse.ArgumentParser();p.add_argument('--mutants',action='store_true');p.add_argument('--sanitize',action='store_true');p.add_argument('--logs',type=Path,required=True);a=p.parse_args();a.logs.mkdir(parents=True,exist_ok=True)
 source=(ROOT/'kernel/addrspace.c').read_text()
 source=source[source.index('static u64 g_addrspace_next_id;'):source.index('// =============================================================================')]
 header=(ROOT/'kernel/include/thylacine/addrspace.h').read_text()
 decl=header[header.index('struct AddrSpace {'):header.index('// Allocate an address space')]
 template=(ROOT/'tools/host-tests/addrspace-lifetime.c').read_text()
 assert template.count('/* ACTUAL_ADDRSPACE_DECL */')==1
 assert template.count('/* ACTUAL_ADDRSPACE_LIFECYCLE */')==1
 template=template.replace('/* ACTUAL_ADDRSPACE_DECL */',decl)
 with tempfile.TemporaryDirectory(prefix='addrspace-lifetime-') as tmp:
  for name,old,new,want in [('clean',None,None,None)]+(MUTANTS if a.mutants else []):
   s=source
   if old:
    assert s.count(old)==1,(name,s.count(old));s=s.replace(old,new)
   c=Path(tmp)/(name+'.c');c.write_text(template.replace('/* ACTUAL_ADDRSPACE_LIFECYCLE */',s));exe=c.with_suffix('')
   cmd=[os.environ.get('CC','clang'),'-std=c11','-O1','-Wall','-Wextra','-Werror','-pthread',str(c),'-o',str(exe)]+shlex.split(os.environ.get('CFLAGS',''))
   if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
   with (a.logs/(name+'-build.log')).open('wb') as out:subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT,check=True)
   r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15);(a.logs/(name+'.log')).write_text(r.stdout+r.stderr)
   if want:assert r.returncode==1 and want in r.stderr,(name,r.returncode,r.stdout,r.stderr)
   else:assert r.returncode==0,(r.stdout,r.stderr)
   print('PASS '+name+(': '+want if want else ': actual-source lifetime'),flush=True)
if __name__=='__main__':main()
