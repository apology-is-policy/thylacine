#!/usr/bin/env python3
"""Run actual kernel target replacement against host reference/lock owners."""
from pathlib import Path
import argparse,subprocess,json,hashlib,platform,re
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);p.add_argument('--cc',default='/opt/homebrew/opt/llvm/bin/clang');p.add_argument('--mutant',choices=['rights','flags','release']);a=p.parse_args();r=Path(__file__).resolve().parent.parent;a.output.mkdir(parents=True,exist_ok=False)
s=(r/'kernel/handle.c').read_text();start=s.index('int handle_dup_to_checked(');end=s.index('\nint handle_set_cloexec',start);s=s[start:end]
if a.mutant:
 old,new={'rights':('rights_t       rights = src->rights;','rights_t       rights = src->rights | 8;'),'flags':('cloexec_set_locked(t, new_h, cloexec);','cloexec_set_locked(t, new_h, !cloexec);'),'release':('if (had) handle_release_obj(old_kind, old_obj);','if (had) { locked=1; handle_release_obj(old_kind, old_obj); locked=0; }')}[a.mutant];assert old in s;s=s.replace(old,new)
(a.output/'target-under-test.c').write_text(s)
files=[r/'kernel/handle.c',r/'tools/host-tests/handle-target-ownership.c',Path(__file__)];(a.output/'inputs.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},indent=2)+'\n')
sdk=['-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()] if platform.system()=='Darwin' else []
for opt in ('0','2'):
 exe=a.output.resolve()/('test-O'+opt);cmd=[a.cc,*sdk,'-O'+opt,'-g','-fsanitize=address,undefined','-pthread','-Wall','-Wextra','-Werror','-I'+str(a.output.resolve()),str(files[1]),'-o',str(exe)]
 (a.output/f'compile-O{opt}.json').write_text(json.dumps(cmd)+'\n')
 with (a.output/f'compile-O{opt}.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 res=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30);s=res.stdout+res.stderr;(a.output/f'run-O{opt}.log').write_text(s)
 if a.mutant:assert res.returncode!=0 and re.search(r'Assertion.*failed',s),s
 else:res.check_returncode();print(s,end='')
