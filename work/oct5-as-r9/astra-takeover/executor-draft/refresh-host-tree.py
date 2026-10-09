from pathlib import Path
import shutil
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
shadow=D/'host-tree';(shadow/'kernel').mkdir(exist_ok=True,parents=True);(shadow/'work/oct5-as-r9/astra-takeover').mkdir(parents=True,exist_ok=True)
for p in (R/'kernel').rglob('*'):
 dst=shadow/p.relative_to(R)
 if p.is_dir():dst.mkdir(exist_ok=True,parents=True)
 elif not dst.exists():dst.symlink_to(p)
for name in ['arch','specs','tools']:
 dst=shadow/name
 if not dst.exists():dst.symlink_to(R/name)
for p in (R/'work/oct5-as-r9').iterdir():
 if p.is_file() and p.suffix in ['.py','.sh']:shutil.copy2(p,shadow/'work/oct5-as-r9'/p.name)
for base in ['kernel','work']:
 for p in (D/base).rglob('*'):
  if p.is_file():
   dst=shadow/p.relative_to(D);dst.parent.mkdir(exist_ok=True,parents=True)
   if dst.is_symlink():dst.unlink()
   shutil.copy2(p,dst)
p=shadow/'kernel/loom_service_engine.c';p.write_text(p.read_text()+(D/'readiness.inc').read_text())
print('shadow prepared',shadow)
