#!/usr/bin/env python3
"""Host regression fixtures for the actual kernel DTB MSI parser.

Requires native clang and dtc. Stores binaries/fixtures under work/ only.
No VM or guest disk is modified. Linux and macOS linker GC drops the unused
physical-relocation path; the tested parser is compiled directly from lib/dtb.c.
"""
from pathlib import Path
import os
import platform
import subprocess
repo = Path(__file__).resolve().parents[1]
os.chdir(repo)
root = Path('work/msi-dtb-cases')
root.mkdir(parents=True, exist_ok=True)

harness = root / "host.c"
harness.write_text('#include <stdio.h>\n#include <stdlib.h>\n#include <thylacine/dtb.h>\nint main(int argc, char **argv) {\n    if (argc != 3) return 2;\n    FILE *f = fopen(argv[1], "rb");\n    if (!f) return 2;\n    unsigned char blob[65536];\n    size_t n = fread(blob, 1, sizeof(blob), f); fclose(f);\n    if (n < 40 || !dtb_init((paddr_t)blob)) return 3;\n    struct dtb_pci_msi r = {0};\n    bool got = dtb_pci_msi_route(0x28, &r);\n    bool expect = argv[2][0] == \'1\';\n    if (got != expect) return 4;\n    if (got && (r.kind != DTB_MSI_ITS || r.pa != 0x8080000 || r.size != 0x20000 || r.device_id != 0x428)) return 5;\n    if (got) {\n        bool parent = dtb_msi_parent_matches(r.node, 0x8000000);\n        if (parent != (argv[2][1] == 112)) return 6;\n        if (dtb_msi_parent_matches(r.node, 0xa000000)) return 7;\n    }\n    return 0;\n}\n')
link_gc = "-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections"
exe = root / "host"
subprocess.run(["clang", "-I", "kernel/include", "-ffunction-sections", "-fdata-sections", link_gc, str(harness), "lib/dtb.c", "-o", str(exe)], check=True)
base='''/dts-v1/;
/ {
 #address-cells = <2>; #size-cells = <2>;
 pcie { compatible = "pci-host-ecam-generic"; msi-map = <0 7 0x400 0x10000>; };
 intc { #address-cells = <1>; #size-cells = <1>; ranges = <0 0 0x8000000 0x100000>;
  its { compatible = "arm,gic-v3-its"; phandle = <7>; msi-controller; #msi-cells = <1>; reg = <0x80000 0x20000>; };
 };
};
'''
cases=[('translated',base,True),
('disabled-controller',base.replace('its {','its { status = "disabled";'),False),
('disabled-parent',base.replace('intc {','intc { status = "disabled";'),False),
('disabled-host',base.replace('pcie {','pcie { status = "disabled";'),False),
('status-okay',base.replace('its {','its { status = "okay";'),True),
('missing-ranges',base.replace('ranges = <0 0 0x8000000 0x100000>;',''),False),
('outside-range',base.replace('reg = <0x80000 0x20000>','reg = <0xf0000 0x20000>'),False),
('unknown-controller',base.replace('arm,gic-v3-its','vendor,unknown'),False),
('wrong-msi-cells',base.replace('#msi-cells = <1>','#msi-cells = <2>'),False),
('missing-controller-flag',base.replace('msi-controller;',''),False),
('ambiguous-map',base.replace('<0 7 0x400 0x10000>','<0 7 0x400 0x10000 0 7 0x400 0x100>'),False),
('partial-map',base.replace('<0 7 0x400 0x10000>','<0 7 0x400>'),False),
('duplicate-host',base.replace('intc {','pcie2 { compatible = "pci-host-ecam-generic"; }; intc {'),False),
('duplicate-phandle',base.replace('intc {','other { phandle = <7>; }; intc {'),False),
('missing-route',base.replace('msi-map = <0 7 0x400 0x10000>;',''),False),
('its-no-sideband',base.replace('msi-map = <0 7 0x400 0x10000>;','msi-parent = <7>;'),False),
('bad-register-length',base.replace('reg = <0x80000 0x20000>','reg = <0x80000>'),False),
('ambiguous-ranges',base.replace('<0 0 0x8000000 0x100000>','<0 0 0x8000000 0x100000 0 0 0x8000000 0x100000>'),False),
('invalid-mask',base.replace('msi-map =','msi-map-mask = <0x10000>; msi-map ='),False),
('duplicate-status',base.replace('its {','its { status = "okay"; status = "disabled";'),False),
('duplicate-mask',base.replace('msi-map =','msi-map-mask = <0xffff>; msi-map-mask = <0>; msi-map ='),False),
('duplicate-address-cells',base.replace('#address-cells = <1>;','#address-cells = <1>; #address-cells = <2>;'),False),
('duplicate-phandle-property',base.replace('phandle = <7>;','phandle = <7>; phandle = <8>;'),False),
('duplicate-compatible',base.replace('compatible = "pci-host-ecam-generic";', 'compatible = "pci-host-ecam-generic"; compatible = "vendor,other";'),False),
('duplicate-map',base.replace('msi-map = <0 7 0x400 0x10000>;', 'msi-map = <0 7 0x400 0x10000>; msi-map = <0 7 0 0x10000>;'),False),
('duplicate-ranges',base.replace('ranges = <0 0 0x8000000 0x100000>;', 'ranges = <0 0 0x8000000 0x100000>; ranges;'),False)]
parent_base = base.replace('intc {', 'intc { compatible = "arm,gic-v3"; reg = <0 0x8000000 0 0x10000 0 0x80a0000 0 0x80000>;')
cases += [('parent-gic', parent_base, '1p'),
          ('parent-other-gic', parent_base.replace('reg = <0 0x8000000', 'reg = <0 0x9000000'), True),
          ('parent-duplicate-reg', parent_base.replace('intc {', 'intc { reg = <0 0x9000000 0 0x10000>;'), True)]
failed=[]
for name,dts,expected in cases:
 src=root/(name+'.dts');dtb=root/(name+'.dtb');src.write_text(dts)
 c=subprocess.run(['dtc','-f','-I','dts','-O','dtb',str(src),'-o',str(dtb)],capture_output=True)
 (root/(name+'.dtc.log')).write_bytes(c.stderr)
 assert dtb.exists(),name
 r=subprocess.run([str(exe),str(dtb),expected if isinstance(expected,str) else ('1' if expected else '0')])
 print(name, 'PASS' if r.returncode==0 else 'FAIL '+str(r.returncode))
 if r.returncode:failed.append(name)
raise SystemExit(bool(failed))
