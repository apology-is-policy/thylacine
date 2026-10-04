#!/usr/bin/env python3
"""Compile the three actual AS-0 ABI mirrors and compare independent byte vectors.

Blind to syscall decoding, permission checks, runtime cancellation and races.
Run under the repository's host-resource lease. Temporary binaries are removed.
"""
from pathlib import Path
import argparse, os, shutil, struct, subprocess, tempfile

ROOT = Path(__file__).resolve().parents[1]
CONSTANTS = [
    ('SERVICE_ABI_VERSION', 1), ('SETUP_PRIVATE_SERVICE', 4),
    ('REGISTER_SERVICE_TARGET', 2), ('REGISTER_SERVICE_SLOT', 3),
    ('REGISTER_ABORT_SCOPE', 4), ('REGISTER_QUERY_SERVICE_SLOT', 5),
    ('REGISTER_REAP_SERVICE_SLOT', 6), ('OP_SERVICE_CONNECT', 20),
    ('SERVICE_SLOT_COUNT', 64), ('SERVICE_TARGET', 1), ('SERVICE_SCOPE', 2),
    ('SERVICE_FID', 3), ('SERVICE_EMPTY', 0), ('SERVICE_RESERVED', 1),
    ('SERVICE_ADMITTED', 2), ('SERVICE_VERSION', 3), ('SERVICE_ATTACH', 4),
    ('SERVICE_READY', 5), ('SERVICE_ABORTING', 6), ('SERVICE_RETIRED', 7),
    ('SERVICE_LOCAL_RETIRED', 1), ('SERVICE_BYTES_SENT', 2),
]
# Independent wire construction; no parsed header/generator input.
def expected():
    ref = struct.pack('<IIQ', 17, 0, 0x1020304050607080)
    target = struct.pack('<IHH iI', 288, 1, 0, 31, 3) + b'abc' + bytes(253) + ref
    reserve = struct.pack('<IHHII', 48, 1, 0, 3, 0) + ref + ref
    control = struct.pack('<IHH', 32, 1, 0) + ref + bytes(8)
    snapshot = struct.pack('<IHH', 64, 1, 0) + ref + struct.pack('<II', 2, 7) + ref + struct.pack('<IIiI', 3, 2, -125, 1)
    return [struct.pack('<' + 'I' * len(CONSTANTS), *(v for _, v in CONSTANTS)), ref, target, reserve, control, snapshot]

def c_source(kernel):
    prefix = '' if kernel else 't_'
    macro = 'LOOM_' if kernel else 'T_LOOM_'
    header = 'thylacine/loom_service_abi.h' if kernel else 'thyla/loom_service.h'
    constants = ','.join(macro + name for name, _ in CONSTANTS)
    return f'''#include <{header}>
#include <stdio.h>
static void emit(const void *p, unsigned long n) {{
 const unsigned char *b=p; for(unsigned long i=0;i<n;i++) printf("%02x",b[i]); puts("");
}}
int main(void) {{
 unsigned int constants[]={{ {constants} }};
 struct {prefix}loom_service_ref r={{17,0,0x1020304050607080ULL}};
 struct {prefix}loom_service_target t={{0}}; t.size=288; t.version=1; t.registry_fd=31; t.name_len=3; t.name[0]='a'; t.name[1]='b'; t.name[2]='c'; t.result=r;
 struct {prefix}loom_service_reserve v={{0}}; v.size=48; v.version=1; v.kind=3; v.scope=r; v.result=r;
 struct {prefix}loom_service_control c={{0}}; c.size=32; c.version=1; c.object=r;
 struct {prefix}loom_service_snapshot s={{0}}; s.size=64; s.version=1; s.object=r; s.kind=2; s.state=7; s.scope=r; s.active_ops=3; s.pending_terminals=2; s.reason=-125; s.status_flags=1;
 emit(constants,sizeof constants); emit(&r,sizeof r); emit(&t,sizeof t); emit(&v,sizeof v); emit(&c,sizeof c); emit(&s,sizeof s);
 return 0;
}}
'''

def rust_source(module):
    constants = ','.join('abi::' + name for name, _ in CONSTANTS)
    return f'''#[path={str(module)!r}] mod abi;
fn emit<T>(v:&T) {{ let b=unsafe{{std::slice::from_raw_parts(v as *const T as *const u8, std::mem::size_of::<T>())}}; for x in b {{print!("{{:02x}}",x);}} println!(); }}
fn main() {{
 let constants:[u32;{len(CONSTANTS)}]=[{constants}];
 let r=abi::SlotRef{{slot:17,reserved:0,incarnation:0x1020304050607080}};
 let mut t=abi::Target{{size:288,version:1,flags:0,registry_fd:31,name_len:3,name:[0;256],result:r}}; t.name[..3].copy_from_slice(b"abc");
 let v=abi::Reservation{{size:48,version:1,flags:0,kind:3,reserved:0,scope:r,result:r}};
 let c=abi::Control{{size:32,version:1,flags:0,object:r,reserved:0}};
 let s=abi::Snapshot{{size:64,version:1,flags:0,object:r,kind:2,state:7,scope:r,active_ops:3,pending_terminals:2,reason:-125,status_flags:1}};
 emit(&constants);emit(&r);emit(&t);emit(&v);emit(&c);emit(&s);
}}
'''.replace("#[path='", '#[path="').replace("'] mod abi;", '"] mod abi;')

def run(root):
    clang = os.environ.get('CC', 'clang')
    rustc = os.environ.get('RUSTC', 'rustc')
    for tool in (clang, rustc):
        if not shutil.which(tool): raise RuntimeError(f'missing compiler: {tool}')
    want = [v.hex() for v in expected()]
    with tempfile.TemporaryDirectory(prefix='loom-service-abi-') as tmp:
        tmp = Path(tmp)
        for kernel, label, inc in [(True, 'kernel', root/'kernel/include'), (False, 'native-c', root/'usr/lib/libt/include')]:
            source = tmp/(label+'.c'); source.write_text(c_source(kernel))
            exe = tmp/label
            subprocess.run([clang,'-std=c11','-Wall','-Wextra','-Werror','-I',str(inc),str(source),'-o',str(exe)],check=True)
            got = subprocess.check_output([str(exe)],text=True).splitlines()
            assert got == want, f'ABI vector mismatch: {label}'
            # Freestanding target compilation validates ARM64 layout as well.
            probe=tmp/(label+'-target.c'); probe.write_text('#include <'+('thylacine/loom_service_abi.h' if kernel else 'thyla/loom_service.h')+'>\n')
            subprocess.run([clang,'--target=aarch64-none-elf','-ffreestanding','-std=c11','-fsyntax-only','-I',str(inc),str(probe)],check=True)
        source=tmp/'mirror.rs'; source.write_text(rust_source(root/'usr/lib/libthyla-rs/src/loom/service_abi.rs'))
        exe=tmp/'rust'; subprocess.run([rustc,'--edition=2021','-Dwarnings',str(source),'-o',str(exe)],check=True)
        got=subprocess.check_output([str(exe)],text=True).splitlines()
        assert got==want,'ABI vector mismatch: Rust'
    print('PASS: 3 actual mirrors; 22 constants, 5 records and every declared size/offset; ARM64 C layouts.')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--root',type=Path,default=ROOT)
    run(parser.parse_args().root.resolve())
