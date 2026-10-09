#!/usr/bin/env python3
"""Install reviewed draft only after the preceding exact-source gate is verified."""
from pathlib import Path
import hashlib,json,subprocess
root=next(p for p in Path(__file__).resolve().parents if (p/'kernel/loom.c').exists())
draft=Path(__file__).resolve().parent
source='45d26893bd8b5e16a3934eed008ae4804f5bff38'
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()==source
assert not subprocess.check_output(['git','diff','HEAD'],cwd=root)
result=json.loads((root/'work/oct5-as-r9/astra-takeover/current-gate-20261009T065820Z-result.json').read_text())
assert result['source']==source and result['clean_boots']==50 and result['expected_tests']==1847
assert result['gate_rc']==0 and result['runner_rc']==0
host=json.loads((draft/'control-5/result.json').read_text())
assert host['source_sha256']==hashlib.sha256((draft/'loom_service_protocol.c').read_bytes()).hexdigest()
assert host['runner_sha256']==hashlib.sha256((draft/'protocol-control.py').read_bytes()).hexdigest()
for src,dst in [('loom_service_protocol.c','kernel/loom_service_protocol.c'),
                ('loom_service_protocol.h','kernel/include/thylacine/loom_service_protocol.h'),
                ('protocol-control.py','work/oct5-as-r9/protocol-control.py')]:
 path=root/dst;assert not path.exists();path.write_bytes((draft/src).read_bytes())
p=root/'kernel/CMakeLists.txt';s=p.read_text();assert s.count('    loom_service_requests.c\n')==1
p.write_text(s.replace('    loom_service_requests.c\n','    loom_service_requests.c\n    loom_service_protocol.c\n'))
p=root/'kernel/test/test_loom.c';s=p.read_text();p.write_text('#include <thylacine/loom_service_protocol.h>\n#include <thylacine/srvconn.h>\n'+s+'\n'+(draft/'protocol-fixture.inc').read_text())
p=root/'kernel/test/test.c';s=p.read_text();anchor='void test_loom_private_requests(void);'
assert s.count(anchor)==1;s=s.replace(anchor,anchor+'\nvoid test_loom_private_protocol(void);')
anchor='    { "loom.private_requests",           test_loom_private_requests,           false, NULL },'
assert s.count(anchor)==1;s=s.replace(anchor,anchor+'\n    { "loom.private_protocol",           test_loom_private_protocol,           false, NULL },')
p.write_text(s)
p=root/'vault/system/kernel/async/sub-kernel-loom.md';s=p.read_text();anchor='  - kernel/loom_service_requests.c\n'
assert s.count(anchor)==1;p.write_text(s.replace(anchor,anchor+'  - kernel/loom_service_protocol.c\n  - kernel/include/thylacine/loom_service_protocol.h\n'))
print('Installed charged prepublication protocol owner and native fixture. No private activation.')
