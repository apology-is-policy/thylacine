#!/usr/bin/env python3
"""Compile actual Tapestry source with controlled ownership-rule mutations.
Requires current host libhalcyon dependency (cargo host gate first), Yip lease.
"""
from pathlib import Path
import argparse,shutil,subprocess,tempfile
ROOT=Path(__file__).resolve().parent.parent
CASES=[
 ('ordered-tag','ordered.rs','self.read.is_some_and(|s| s.tag == tag)','self.read.is_some_and(|s| s.tag <= tag)','ordered::tests::simultaneous_write_does_not_borrow_pending_read'),
 ('admission-tag','admission.rs','self.failed || s.tag != tag || result <= 0','self.failed || result <= 0','admission::tests::payload_borrow_requires_matching_read_completion'),
 ('seat-tag','seat.rs','self.failed || s.tag != tag || result <= 0','self.failed || result <= 0','seat::tests::payload_borrow_requires_matching_read_completion'),
 ('event-region','ring.rs','completed_bytes(region..region+n)','completed_bytes(0..region+n)','ring::tests::completion_borrows_only_its_region_after_identity_check'),
]
def main():
 p=argparse.ArgumentParser();p.add_argument('--logs',type=Path,required=True);a=p.parse_args();a.logs.mkdir(parents=True,exist_ok=True)
 deps=ROOT/'build/usr-rs/aarch64-apple-darwin/debug/deps';libs=list(deps.glob('liblibhalcyon-*.rlib'));assert libs
 lib=max(libs,key=lambda p:p.stat().st_mtime_ns)
 with tempfile.TemporaryDirectory(prefix='tapestry-borrow-') as tmp:
  root=Path(tmp)
  for name,path,old,new,test in CASES:
   dest=root/name;shutil.copytree(ROOT/'usr/lib/libtapestry/src',dest)
   p=dest/path;s=p.read_text();assert old in s;s=s.replace(old,new,1);p.write_text(s)
   exe=root/(name+'-test')
   r=subprocess.run(['rustc','--test','--edition=2021','--crate-name','tapestry','--target','aarch64-apple-darwin',str(dest/'lib.rs'),'-L','dependency='+str(deps),'--extern','libhalcyon='+str(lib),'-o',str(exe)],capture_output=True,text=True,timeout=90)
   (a.logs/(name+'-build.log')).write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
   r=subprocess.run([str(exe),'--exact',test,'--nocapture'],capture_output=True,text=True,timeout=20)
   text=r.stdout+r.stderr;(a.logs/(name+'.log')).write_text(text)
   assert r.returncode!=0 and '1 failed' in text and test in text and 'panicked' in text,text
   print('PASS intended mutation:',name,test,flush=True)
if __name__=='__main__':main()
