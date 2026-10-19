#!/usr/bin/env python3
"""Compile actual native descriptor handlers with owner-boundary mocks."""
from pathlib import Path
import argparse,subprocess,json,hashlib,re,platform
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);p.add_argument('--cc',default='/opt/homebrew/opt/llvm/bin/clang');p.add_argument('--mutant',choices=['self-dup','mirror']);a=p.parse_args();r=Path(__file__).resolve().parent.parent;a.output.mkdir(parents=True,exist_ok=False)
s=(r/'kernel/syscall.c').read_text();parts=[]
for name in ['sys_handle_flags_set_in','sys_handle_dup_to_in','sys_handle_flags_set_handler','sys_handle_dup_to_handler']:
 start=s.index('static s64 '+name+'(');end=s.index('\n}',start)+2;parts.append(s[start:end])
handlers='\n'.join(parts)+'\n'
# self-dup drops the exact-target core's same-index refusal, which the test must catch.
if a.mutant=='self-dup':old='    if (old == target) return -(s64)T_E_INVAL;\n';assert handlers.count(old)==1;handlers=handlers.replace(old,'')
(a.output/'handlers.c').write_text(handlers)
files=[r/'kernel/syscall.c',r/'tools/host-tests/native-descriptor-ops.c',Path(__file__)]
# Pin independently compiled native mirrors; the guest exercises actual dispatch.
# mirror moves the kernel's HANDLE_DUP_TO by one, and only that pin may refuse it.
missing=[]
for relative,prefix in [('kernel/include/thylacine/syscall.h','SYS_'),('usr/lib/libt/include/thyla/syscall.h','T_SYS_'),('usr/lib/libthyla-rs/src/lib.rs','T_SYS_')]:
 f=r/relative;body=f.read_text();files.append(f)
 if a.mutant=='mirror' and prefix=='SYS_':body,n=re.subn(r'(\bSYS_HANDLE_DUP_TO\s*=\s*)131\b',r'\g<1>132',body);assert n==1
 for name,value in [('HANDLE_FLAGS_SET',130),('HANDLE_DUP_TO',131)]:
  if not re.search(r'\b'+prefix+name+r'(?:\s*:\s*u64)?\s*(?:=\s*|\s)'+str(value)+r'\b',body):missing.append((relative,name,value))
if a.mutant=='mirror':assert missing==[('kernel/include/thylacine/syscall.h','HANDLE_DUP_TO',131)],missing;print('PASS mirror mutant refused by the pin');raise SystemExit(0)
assert not missing,missing
(a.output/'inputs.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},indent=2)+'\n')
sdk=['-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()] if platform.system()=='Darwin' else []
for opt in ('0','2'):
 exe=a.output.resolve()/('test-O'+opt);cmd=[a.cc,*sdk,'-O'+opt,'-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-I'+str(a.output.resolve()),str(files[1]),'-o',str(exe)]
 (a.output/f'compile-O{opt}.json').write_text(json.dumps(cmd)+'\n')
 with (a.output/f'compile-O{opt}.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 res=subprocess.run([str(exe)],capture_output=True,text=True);out=res.stdout+res.stderr;(a.output/f'run-O{opt}.log').write_text(out)
 if a.mutant:assert res.returncode!=0 and re.search(r'Assertion.*failed',out),out
 else:res.check_returncode();print(res.stdout,end='')
if a.mutant:print('PASS '+a.mutant+' mutant refused by the handler test')
