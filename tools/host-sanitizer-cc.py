#!/usr/bin/env python3
"""The C compiler for a host fixture, and the proof that its sanitizer starts.

Apple clang's ASan runtime hangs at startup on macOS 26, before main; the
Homebrew LLVM toolchain's does not. An explicit CC always wins. Without one a
sanitized fixture takes Homebrew LLVM, and when that is absent the run stops in
its own state (exit 3, "SANITIZER TOOLING") instead of falling back to the
toolchain that hangs. A sanitized fixture first proves its runtime starts, on
an empty program, so a startup death can never read as a case's result. The
compiler used, its version and the probe's outcome go to <logs>/compiler.txt.
A case's verdict (check) refuses any sanitizer report before it reads the exit
status, so UBSan's recoverable reports cannot ride along with a wanted
assertion; build logs open with the argv, environment and input hashes.
Loaded by path (importlib) from the tools/test-*.py drivers; run directly, it
tests check() against real reports (--logs DIR).
"""
import argparse,hashlib,json,os,re,shlex,shutil,subprocess,sys,tempfile
from pathlib import Path
LLVM_CC=Path(os.environ.get('LLVM_PREFIX','/opt/homebrew/opt/llvm'))/'bin/clang'
TOOLING_EXIT=3
SANITIZE=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-g']
PROBE_TIMEOUT=20
# A sanitizer's own report, in either stream: a run that carries one is not a
# result, whatever its exit status and whatever assertion it also printed.
REPORT=re.compile(r'runtime error:|==\d+==(?:ERROR|WARNING): \w*Sanitizer|SUMMARY: \w*Sanitizer')
RECEIPT_ENV=('CC','CFLAGS','LLVM_PREFIX','ASAN_OPTIONS','UBSAN_OPTIONS')

def _stop(logs,lines,why):
    lines.append('SANITIZER TOOLING: '+why)
    (logs/'compiler.txt').write_text('\n'.join(lines)+'\n')
    print('SANITIZER TOOLING: '+why,file=sys.stderr,flush=True)
    sys.exit(TOOLING_EXIT)

def select(logs,sanitize=True):
    """Return (cc, extra arguments) for this fixture's builds."""
    logs=Path(logs);logs.mkdir(parents=True,exist_ok=True)
    explicit=os.environ.get('CC')
    if explicit:cc,lines=explicit,['source: CC='+explicit]
    elif not sanitize:cc,lines='clang',['source: default clang (unsanitized)']
    elif LLVM_CC.exists():cc,lines=str(LLVM_CC),['source: Homebrew LLVM']
    else:_stop(logs,['source: none'],f"no CC and no LLVM clang at {LLVM_CC}; Apple clang's ASan runtime hangs at startup")
    found=shutil.which(cc)
    if not found:_stop(logs,lines,f'{cc} is not an executable compiler')
    version=subprocess.run([found,'--version'],capture_output=True,text=True)
    if version.returncode:_stop(logs,lines,f'{cc} --version exited {version.returncode}')
    lines+=['path: '+str(Path(found).resolve())]+version.stdout.strip().splitlines()
    extra=[]
    sdk=subprocess.run(['xcrun','--show-sdk-path'],capture_output=True,text=True) if shutil.which('xcrun') else None
    if sdk and sdk.returncode==0:
        extra=['-isysroot',sdk.stdout.strip()];lines.append('sysroot: '+extra[1])
    if sanitize:
        with tempfile.TemporaryDirectory(prefix='sanitizer-probe-') as tmp:
            src=Path(tmp)/'probe.c';exe=Path(tmp)/'probe'
            src.write_text('int main(void) { return 0; }\n')
            built=subprocess.run([cc,'-std=c11',str(src),'-o',str(exe)]+SANITIZE+extra,capture_output=True,text=True)
            if built.returncode:_stop(logs,lines,f'{cc} cannot build a sanitized program: {built.stderr.strip()[:400]}')
            try:ran=subprocess.run([str(exe)],capture_output=True,text=True,timeout=PROBE_TIMEOUT)
            except subprocess.TimeoutExpired:_stop(logs,lines,f'{cc}: an empty sanitized program did not finish in {PROBE_TIMEOUT} s (runtime startup)')
            if ran.returncode:_stop(logs,lines,f'{cc}: an empty sanitized program exited {ran.returncode}: {ran.stderr.strip()[:400]}')
        lines.append('probe: an empty sanitized program ran and exited 0')
    (logs/'compiler.txt').write_text('\n'.join(lines)+'\n')
    return cc,extra

def build(cmd,log):
    """Compile, the build log opening with the argv, its environment and each C input's sha256."""
    lines=['argv: '+shlex.join(cmd),'env: '+json.dumps({k:os.environ[k] for k in RECEIPT_ENV if k in os.environ},sort_keys=True)]
    lines+=[f'sha256: {hashlib.sha256(Path(x).read_bytes()).hexdigest()}  {x}' for x in cmd if x.endswith('.c') and Path(x).is_file()]
    with Path(log).open('wb') as out:
        out.write(('\n'.join(lines)+'\n').encode());out.flush()
        subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT,check=True)

def check(name,r,want,any_failure=False,by_sanitizer=False):
    """A case's verdict: no sanitizer report (unless the report IS the wanted kill); a control fails naming its assertion; a clean case exits 0."""
    reports=[line for line in (r.stdout+r.stderr).splitlines() if REPORT.search(line)]
    if by_sanitizer:
        assert r.returncode!=0 and reports and want in reports[0],(name,r.returncode,reports[:4],r.stderr)
        return
    assert not reports,(name,'sanitizer report',reports[:4])
    if want:assert (r.returncode!=0 if any_failure else r.returncode==1) and want in r.stderr,(name,r.returncode,r.stdout,r.stderr)
    else:assert r.returncode==0,(name,r.returncode,r.stdout,r.stderr)

# The checker's own controls, each a real runtime report (a pattern never seen
# to match would fail open): name, body of main, wanted text, built recoverable,
# whether check() must accept it.
CONTROLS=[
 ('ubsan-then-assertion','int x = INT_MAX; x += argc; fputs("wanted assertion\\n", stderr); return x != 0;','wanted assertion',True,False),
 ('ubsan-on-clean-exit','int x = INT_MAX; x += argc; printf("%d\\n", x); return 0;',None,True,False),
 ('asan-after-assertion','char *p = malloc(8); free(p); fputs("wanted assertion\\n", stderr); return p[argc];','wanted assertion',False,False),
 ('assertion-only','fputs("wanted assertion\\n", stderr); return argc;','wanted assertion',False,True),
 ('clean-only','return argc - 1;',None,False,True),
]

def self_test(logs):
    logs=Path(logs);cc,extra=select(logs,True);bad=0
    with tempfile.TemporaryDirectory(prefix='sanitizer-check-') as tmp:
        for name,body,want,recoverable,accept in CONTROLS:
            src=Path(tmp)/(name+'.c');exe=src.with_suffix('')
            src.write_text('#include <limits.h>\n#include <stdio.h>\n#include <stdlib.h>\nint main(int argc, char **argv) {\n    (void)argv;\n    '+body+'\n}\n')
            flags=[f for f in SANITIZE if not (recoverable and f=='-fno-sanitize-recover=all')]
            build([cc,'-std=c11','-O0',str(src),'-o',str(exe)]+flags+extra,logs/(name+'-build.log'))
            r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=PROBE_TIMEOUT);(logs/(name+'.log')).write_text(r.stdout+r.stderr)
            try:check(name,r,want);accepted=True
            except AssertionError:accepted=False
            seen=bool([line for line in (r.stdout+r.stderr).splitlines() if REPORT.search(line)])
            ok=accepted==accept and seen==(not accept)
            bad+=not ok
            print(('PASS ' if ok else 'FAIL ')+name+(': accepted' if accepted else ': rejected')+(', report seen' if seen else ', no report')+f', rc {r.returncode}',flush=True)
    return bad

if __name__=='__main__':
    p=argparse.ArgumentParser(description='Run the verdict check against real sanitizer reports.');p.add_argument('--logs',type=Path,required=True)
    sys.exit(1 if self_test(p.parse_args().logs) else 0)
