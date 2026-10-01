#!/usr/bin/env python3
"""Compile the D7 C/Rust mirrors and compare with an independent byte oracle."""
from pathlib import Path
import os,re,shlex,struct,subprocess,tempfile
root=Path(__file__).resolve().parent.parent
expected=struct.pack('<QQII32s',127,1<<10,3,0,b'foo')
cc=shlex.split(os.environ.get('CC','/opt/homebrew/opt/llvm/bin/clang --target=aarch64-none-elf'))
objcopy=os.environ.get('OBJCOPY','/opt/homebrew/opt/llvm/bin/llvm-objcopy')
with tempfile.TemporaryDirectory(prefix='srv-registry-abi-') as tmp:
 tmp=Path(tmp)
 for name,headers,include,route,sysno,perm in [
  ('kernel','#include <thylacine/syscall.h>\n#include <thylacine/devsrv.h>','kernel/include','srv_route','SYS_SRV_REGISTRY_NEW','SPAWN_PERM_SESSION_REGISTRY'),
  ('libt','#include <thyla/syscall.h>','usr/lib/libt/include','t_srv_route','T_SYS_SRV_REGISTRY_NEW','T_SPAWN_PERM_SESSION_REGISTRY')]:
  (tmp/'fixture.c').write_text(headers+f'''\nstruct fixture {{unsigned long long sysno, perm; struct {route} route;}};
_Static_assert(sizeof(struct fixture)==56,"no padding");
__attribute__((used,section(".srv_abi"))) const struct fixture fixture={{ {sysno}, {perm}, {{3,0,"foo"}} }};
''')
  subprocess.run(cc+['-std=c11','-ffreestanding','-Werror','-I',str(root/include),'-I',str(root/'arch/arm64'),'-c',str(tmp/'fixture.c'),'-o',str(tmp/'fixture.o')],check=True)
  subprocess.run([objcopy,'--dump-section',f'.srv_abi={tmp}/bytes',str(tmp/'fixture.o')],check=True)
  assert (tmp/'bytes').read_bytes()==expected,name
  print(name+': syscall, authority bit and route byte layout PASS',flush=True)
 s=(root/'usr/lib/libthyla-rs/src/lib.rs').read_text()
 consts='\n'.join(re.search(r'pub const '+n+r': [^;]+;',s)[0] for n in ('T_SYS_SRV_REGISTRY_NEW','T_SPAWN_PERM_SESSION_REGISTRY'))
 record=re.search(r'pub struct SrvRoute \{[^}]+\}',s)[0]
 (tmp/'fixture.rs').write_text(consts+'\n#[repr(C)]\n'+record+'''\nfn main() {
 let r=SrvRoute{name_len:3,reserved:0,name:{let mut a=[0;32];a[..3].copy_from_slice(b"foo");a}};
 assert_eq!(core::mem::size_of::<SrvRoute>(),40);
 use std::io::Write;
 let mut o=std::io::stdout();
 o.write_all(&(T_SYS_SRV_REGISTRY_NEW as u64).to_le_bytes()).unwrap();
 o.write_all(&(T_SPAWN_PERM_SESSION_REGISTRY as u64).to_le_bytes()).unwrap();
 o.write_all(unsafe{core::slice::from_raw_parts((&r as *const SrvRoute).cast::<u8>(),40)}).unwrap();
}''')
 subprocess.run(['rustc','--edition=2021',str(tmp/'fixture.rs'),'-o',str(tmp/'fixture')],check=True)
 assert subprocess.check_output([str(tmp/'fixture')])==expected,'Rust'
 print('Rust: syscall, authority bit and route byte layout PASS',flush=True)
