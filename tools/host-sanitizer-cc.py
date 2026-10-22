#!/usr/bin/env python3
"""The C compiler for a host fixture, and the proof that its sanitizer starts.

Apple clang's ASan runtime hangs at startup on macOS 26, before main; the
Homebrew LLVM toolchain's does not. An explicit CC always wins. Without one a
sanitized fixture takes Homebrew LLVM, and when that is absent the run stops in
its own state (exit 3, "SANITIZER TOOLING") instead of falling back to the
toolchain that hangs. A sanitized fixture first proves its runtime starts, on
an empty program, so a startup death can never read as a case's result. The
compiler used, its version and the probe's outcome go to <logs>/compiler.txt.
Loaded by path (importlib) from the tools/test-*.py drivers.
"""
import os,shutil,subprocess,sys,tempfile
from pathlib import Path
LLVM_CC=Path(os.environ.get('LLVM_PREFIX','/opt/homebrew/opt/llvm'))/'bin/clang'
TOOLING_EXIT=3
SANITIZE=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
PROBE_TIMEOUT=20

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
