#!/usr/bin/env python3
"""Actual AddrSpace declaration/lifecycle and mapped-device quiesce under
controlled owner/pin schedules. Acquire Yip before running. Allocator, VMA
drain, page-table destruction and the device resets are host doubles; native
tests separately cover process authority and the devices themselves. The
source slice stops before clone helpers: this does not qualify the MMU.
"""
from pathlib import Path
import argparse,os,shlex,subprocess,tempfile,importlib.util
ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('progress_fixture',ROOT/'tools/test-9p-client-progress.py');helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
spec=importlib.util.spec_from_file_location('host_sanitizer_cc',ROOT/'tools/host-sanitizer-cc.py');sanitizer_cc=importlib.util.module_from_spec(spec);spec.loader.exec_module(sanitizer_cc)
# Destroy's tail: the last owner's drain, then its lifetime reference.
DESTROY_TAIL='    vma_drain_in(as);\n    addrspace_lifetime_put(as);\n}'
QUIESCE='int reset = addrspace_quiesce_mapped_devices(as);'
MUTANTS=[
 ('private-sharing','if (as->private_rings) { spin_unlock(&as->lock); return false; }','if (false) { spin_unlock(&as->lock); return false; }','private guard refuses sharing'),
 ('already-shared','if (__atomic_load_n(&as->owners, __ATOMIC_ACQUIRE) != 1 ||','if (false ||','shared image refuses private guard'),
 ('guard-no-pin','    addrspace_lifetime_get(as);\n    ++as->private_rings;','    ++as->private_rings;','guard pins lifetime without adding owner'),
 ('guard-leak','    --as->private_rings;','    (void)as->private_rings;','final guard releases pin and exclusion'),
 ('pin-as-owner','void addrspace_pin(struct AddrSpace *as) {\n    addrspace_lifetime_get(as);','void addrspace_pin(struct AddrSpace *as) {\n    addrspace_ref(as);','pin is not an owner'),
 ('no-owner-get','int pre = __atomic_fetch_add(&as->owners, 1, __ATOMIC_ACQ_REL);','int pre = __atomic_load_n(&as->owners, __ATOMIC_ACQUIRE);','owner increments both counts'),
 ('skip-drain',DESTROY_TAIL,'    addrspace_lifetime_put(as);\n}','last owner drains before last pin'),
 ('drain-every-owner','    if (pre == 1) return true;\n','    if (pre == 1) return true;\n    vma_drain_in(as);\n','nonfinal owner preserves mappings'),
 ('pin-leak','void addrspace_unpin(struct AddrSpace *as) {\n    addrspace_lifetime_put(as);','void addrspace_unpin(struct AddrSpace *as) {\n    (void)as;','final pin frees once without another drain'),
 ('drop-before-drain',DESTROY_TAIL,'    addrspace_lifetime_put(as);\n    vma_drain_in(as);\n}','extinction: AddrSpace final lifetime drop before mapping drain'),
 ('early-destroy','if (pre > 1) return;','if (false) return;','extinction: AddrSpace final lifetime drop with live owners'),
 ('release-drops-lifetime','    if (pre == 1) return true;\n','    if (pre == 1) { addrspace_lifetime_put(as); return true; }\n','last owner drains before last pin'),
 ('release-early-last','if (pre == 1) return true;','if (pre <= 2) return true;','extinction: addrspace_destroy of an AddrSpace still owned'),
 ('no-quiesce',QUIESCE,'int reset = 0;','last owner stops each mapped device once, before the drain'),
 ('quiesce-after-drain',[(QUIESCE,'int reset = 0;'),(DESTROY_TAIL,'    vma_drain_in(as);\n    reset += addrspace_quiesce_mapped_devices(as);\n    addrspace_lifetime_put(as);\n}')],None,'last owner stops each mapped device once, before the drain'),
]
def main():
 p=argparse.ArgumentParser();p.add_argument('--mutants',action='store_true');p.add_argument('--sanitize',action='store_true');p.add_argument('--logs',type=Path,required=True);a=p.parse_args();a.logs.mkdir(parents=True,exist_ok=True)
 source=(ROOT/'kernel/addrspace.c').read_text()
 source=source[source.index('static u64 g_addrspace_next_id;'):source.index('// =============================================================================')]
 header=(ROOT/'kernel/include/thylacine/addrspace.h').read_text()
 decl=header[header.index('struct AddrSpace {'):header.index('// Allocate an address space')]
 # The release/destroy pair is declared after the slice ends; addrspace_unref
 # calls both before their definitions.
 split='bool addrspace_release(struct AddrSpace *as);\nvoid addrspace_destroy(struct AddrSpace *as);\n'
 assert header.count(split)==1;decl+=split
 template=(ROOT/'tools/host-tests/addrspace-lifetime.c').read_text()
 assert template.count('/* ACTUAL_ADDRSPACE_DECL */')==1
 assert template.count('/* ACTUAL_ADDRSPACE_LIFECYCLE */')==1
 assert template.count('/* ACTUAL_QUIESCE */')==1
 template=template.replace('/* ACTUAL_ADDRSPACE_DECL */',decl).replace('/* ACTUAL_QUIESCE */',helper.function((ROOT/'kernel/proc.c').read_text(),'addrspace_quiesce_mapped_devices'))
 cc,cc_extra=sanitizer_cc.select(a.logs,a.sanitize)
 with tempfile.TemporaryDirectory(prefix='addrspace-lifetime-') as tmp:
  for name,old,new,want in [('clean',None,None,None)]+(MUTANTS if a.mutants else []):
   s=source
   for o,n in (old if isinstance(old,list) else [(old,new)] if old else []):
    assert s.count(o)==1,(name,o,s.count(o));s=s.replace(o,n)
   c=Path(tmp)/(name+'.c');c.write_text(template.replace('/* ACTUAL_ADDRSPACE_LIFECYCLE */',s));exe=c.with_suffix('')
   cmd=[cc,'-std=c11','-O1','-Wall','-Wextra','-Werror','-pthread',str(c),'-o',str(exe)]+cc_extra+shlex.split(os.environ.get('CFLAGS',''))
   if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
   with (a.logs/(name+'-build.log')).open('wb') as out:subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT,check=True)
   r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15);(a.logs/(name+'.log')).write_text(r.stdout+r.stderr)
   if want:assert r.returncode==1 and want in r.stderr,(name,r.returncode,r.stdout,r.stderr)
   else:assert r.returncode==0,(r.stdout,r.stderr)
   print('PASS '+name+(': '+want if want else ': actual-source lifetime'),flush=True)
if __name__=='__main__':main()
