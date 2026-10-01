#!/usr/bin/env python3
"""Run Corvus's production session table on the host, with named negatives."""
from pathlib import Path
import re, subprocess, tempfile
root = Path(__file__).resolve().parent.parent
main = (root/'usr/corvus/src/main.rs').read_text()
crypto = (root/'usr/lib/corvus-crypto/src/lib.rs').read_text()
constants = ''
for name in ('MAX_CONNS', 'MAX_USER_LEN', 'TOKEN_LEN'):
    constants += re.search(r'const '+name+r': usize = \d+;', main)[0]+'\n'
for name in ('X25519_KEY_LEN','MLKEM_EK_LEN','MLKEM_DK_LEN','KEYPAIR_LEN'):
    constants += re.search(r'(?:pub )?const '+name+r': usize = [^;]+;', crypto)[0]+'\n'
source = (root/'usr/corvus/src/sessions.rs').read_text()
mutants = {
    'foreign-close-clears-owner': ('connection != 0 && s.connection == connection', 'connection != 0 && s.connection != 0'),
    'allows-owner-rebinding': ('!self.slots.iter().any(|s| s.connection != 0 && s.owner == owner)', 'true'),
    'forgets-secret-wipe': ('core::ptr::write_volatile(b, 0);', 'core::hint::black_box(b);'),
    'accepts-token-collision': ('|| self.find(token).is_some()', '|| false'),
}
with tempfile.TemporaryDirectory(prefix='thyla-corvus-sessions-') as tmp:
    tmp = Path(tmp)
    for name, mutation in [('clean', None), *mutants.items()]:
        body = source
        if mutation:
            assert body.count(mutation[0]) == 1, name
            body = body.replace(*mutation)
        (tmp/'sessions.rs').write_text(body)
        (tmp/'test.rs').write_text(constants+'mod sessions;\n')
        subprocess.run(['rustc','--edition=2021','--test',str(tmp/'test.rs'),'-o',str(tmp/'test')],check=True)
        r = subprocess.run([str(tmp/'test')],capture_output=True,text=True)
        if mutation:
            assert r.returncode != 0 and 'test result: FAILED' in r.stdout, (name,r.stdout,r.stderr)
            print(name+': intended assertion failure',flush=True)
        else:
            print(r.stdout,flush=True)
            r.check_returncode()
