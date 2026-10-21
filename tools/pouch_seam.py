"""The Pouch seam's errno decoder, staged from a series-applied musl tree for a host test.

A host test must run its code under Pouch's own __syscall_ret, never a mock:
upstream musl's decoder reads a -1 as EPERM, and Pouch's reads it as EIO
(0001), so a -EPERM handed to the seam passes a mock and fails on the device.
"""
import re
import subprocess


def stage_syscall_ret(musl, out, cc, inputs):
    """Write out/syscall-ret-under-test.c from musl's src/internal/syscall_ret.c.

    The "syscall.h" include is replaced by the two values its static assert
    pins, read from the same tree; every input read is appended to inputs."""
    src = musl/'src/internal/syscall_ret.c'
    arch = musl/'arch/aarch64/syscall_arch.h'
    sysin = musl/'arch/aarch64/bits/syscall.h.in'
    inputs += [src, arch, sysin]
    unimpl = re.findall(r'(?m)^#define POUCH_SYSCALL_UNIMPL\s+(\S+)$', arch.read_text())
    assert len(unimpl) == 1, unimpl
    macros = subprocess.run([cc, '-E', '-dM', '-x', 'c', str(sysin)],
                            capture_output=True, text=True, check=True).stdout
    io_setup = re.findall(r'(?m)^#define __NR_io_setup (\S+)$', macros)
    assert len(io_setup) == 1, io_setup
    s = src.read_text()
    assert s.count('#include "syscall.h"\n') == 1, src
    assert s.count('if (r == (unsigned long)-1) {') == 1, 'not the Pouch seam: ' + str(src)
    s = s.replace('#include "syscall.h"\n',
                  '#define POUCH_SYSCALL_UNIMPL %s\n#define SYS_io_setup %s\n' % (unimpl[0], io_setup[0]))
    (out/'syscall-ret-under-test.c').write_text(s)
