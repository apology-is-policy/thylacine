#!/usr/bin/env python3
"""Compile actual native descriptor handlers with owner-boundary mocks."""
from pathlib import Path
import argparse,subprocess,json,hashlib,re
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);a=p.parse_args();r=Path(__file__).resolve().parent.parent;a.output.mkdir(parents=True,exist_ok=False)
s=(r/'kernel/syscall.c').read_text();parts=[]
for name in ['sys_handle_flags_set_in','sys_handle_dup_to_in','sys_handle_flags_set_handler','sys_handle_dup_to_handler']:
 start=s.index('static s64 '+name+'(');end=s.index('\n}',start)+2;parts.append(s[start:end])
(a.output/'handlers.c').write_text('\n'.join(parts)+'\n')
files=[r/'kernel/syscall.c',r/'tools/host-tests/native-descriptor-ops.c',Path(__file__)]
# Pin independently compiled native mirrors; the guest exercises actual dispatch.
for relative,prefix in [('kernel/include/thylacine/syscall.h','SYS_'),('usr/lib/libt/include/thyla/syscall.h','T_SYS_'),('usr/lib/libthyla-rs/src/lib.rs','T_SYS_')]:
 f=r/relative;body=f.read_text();files.append(f)
 for name,value in [('HANDLE_FLAGS_SET',130),('HANDLE_DUP_TO',131)]:
  assert re.search(r'\b'+prefix+name+r'(?:\s*:\s*u64)?\s*(?:=\s*|\s)'+str(value)+r'\b',body), (relative,name,value)
(a.output/'inputs.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},indent=2)+'\n')
for opt in ('0','2'):
 exe=a.output.resolve()/('test-O'+opt);cmd=['/opt/homebrew/opt/llvm/bin/clang','-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip(),'-O'+opt,'-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-I'+str(a.output.resolve()),str(files[1]),'-o',str(exe)]
 (a.output/f'compile-O{opt}.json').write_text(json.dumps(cmd)+'\n')
 with (a.output/f'compile-O{opt}.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 res=subprocess.run([str(exe)],capture_output=True,text=True);(a.output/f'run-O{opt}.log').write_text(res.stdout+res.stderr);res.check_returncode();print(res.stdout,end='')
