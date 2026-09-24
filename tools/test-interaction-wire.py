#!/usr/bin/env python3
"""Compile the public C envelope mirror and check its canonical byte fixture."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = r'''
#include <stdio.h>
#include "halcyon_interaction.h"
#define JOIN_(a,b) a##b
#define JOIN(a,b) JOIN_(a,b)
#define CHECK(e) typedef char JOIN(check_,__LINE__)[(e) ? 1 : -1]
CHECK(HIN_VERSION == 1 && HIN_HEADER_BYTES == 24);
CHECK(HIN_OFF_MAGIC == 0 && HIN_OFF_VERSION == 4 && HIN_OFF_OPERATION == 6);
CHECK(HIN_OFF_LENGTH == 8 && HIN_OFF_FLAGS == 12 && HIN_OFF_REQUEST_ID == 16);
CHECK(HIN_MAX_RECORD == 32768 && HIN_MAX_TEXT == 1048576 && HIN_MAX_CHUNK == 16384);
CHECK(HIN_MAX_LABEL == 64 && HIN_MAX_QUERY == 4096 && HIN_FLAG_RESPONSE == 1);
CHECK(HIN_HELLO == 1 && HIN_BIND_CONTROLLER == 2 && HIN_REPORT_MODE == 3);
CHECK(HIN_GET_CLIPBOARD == 4 && HIN_READ_CLIPBOARD == 5 && HIN_BEGIN_COPY == 6);
CHECK(HIN_WRITE_COPY == 7 && HIN_COMMIT_COPY == 8 && HIN_CANCEL == 9 && HIN_UNBIND_CONTROLLER == 10);
CHECK(HIN_INS == 1 && HIN_NOR == 2 && HIN_VIS == 3 && HIN_CMD == 4 && HIN_APP == 5);
CHECK(HIN_DENIED == 1 && HIN_GONE == 2 && HIN_TOO_LARGE == 7);
CHECK(HIN_BAD_HANDLE == 9 && HIN_CONFLICT == 11 && HIN_NO_MEMORY == 12);
CHECK(HIN_BUSY == 16 && HIN_INVALID == 22 && HIN_UNSUPPORTED == 95 && HIN_TIMEOUT == 110);
int main(void) {
    return fwrite(hin_hello_fixture, 1, sizeof hin_hello_fixture, stdout) == HIN_HEADER_BYTES ? 0 : 1;
}
'''
expected = bytes.fromhex('48494e31 0100 0100 18000000 00000000 0807060504030201')
with tempfile.TemporaryDirectory(prefix='hin-wire-') as tmp:
    src, exe = Path(tmp) / 'fixture.c', Path(tmp) / 'fixture'
    src.write_text(source)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra',
                    '-Werror', '-I', str(root / 'usr/lib/libhalcyon/include'),
                    str(src), '-o', str(exe)], check=True)
    actual = subprocess.check_output([str(exe)])
    if actual != expected:
        raise SystemExit(f'HIN1 C fixture mismatch: {actual.hex()}')
print('HIN1 C constants and Hello fixture: PASS (Rust encoder fixture is a separate host test)')

# All request/response bodies are also constructed by an independent C encoder.
# The same immutable vectors are checked by the Rust encoder unit test.
with tempfile.TemporaryDirectory(prefix='hin-bodies-') as tmp:
    exe = Path(tmp) / 'bodies'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra',
                    '-Werror', '-I', str(root / 'usr/lib/libhalcyon/include'),
                    str(root / 'tools/interactive/interaction-wire-fixture.c'),
                    '-o', str(exe)], check=True)
    actual = subprocess.check_output([str(exe)])
    expected = (root / 'usr/lib/libhalcyon/fixtures/interaction-v1.hex').read_bytes()
    if actual != expected:
        raise SystemExit('HIN1 C request/response bodies differ from frozen vectors')
print('HIN1 C bodies: PASS (20 frozen request/response vectors shared with Rust)')
