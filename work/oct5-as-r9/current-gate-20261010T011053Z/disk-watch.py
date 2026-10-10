from pathlib import Path
import os,time,json,signal,datetime
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
pid=67665;minimum=10**12;stopped=False
while not (D/'runner.rc').exists():
 s=os.statvfs(R);free=s.f_bavail*s.f_frsize//1024**2;minimum=min(minimum,free)
 (D/'disk-watch.json').write_text(json.dumps(dict(owner='astra',pid=pid,minimum_free_mib=minimum,current_free_mib=free,floor_mib=8192,stop_margin_mib=64,stopped=stopped,utc=datetime.datetime.now(datetime.timezone.utc).isoformat()))+'\n')
 if free<=8256:
  stopped=True
  (D/'disk-watch.json').write_text(json.dumps(dict(owner='astra',pid=pid,minimum_free_mib=minimum,current_free_mib=free,floor_mib=8192,stop_margin_mib=64,stopped=True,reason='Stop owned runner process group before8192MiBfloor'))+'\n')
  try:os.killpg(pid,signal.SIGTERM)
  except ProcessLookupError:pass
  break
 try:os.kill(pid,0)
 except ProcessLookupError:break
 time.sleep(1)
