#!/usr/bin/env python3
"""Linux host only. Rebuild in a fresh directory; never flash or replace inputs."""
from pathlib import Path
import hashlib,shutil,subprocess,sys,tempfile
H=Path(__file__).resolve().parent
D=Path(tempfile.mkdtemp(prefix='rebuild-',dir=H))
for name in ['ebsprobe','base','qemu-test']:
    shutil.copytree(H/name,D/name,ignore=shutil.ignore_patterns('*.o','*.obj','*.elf','*.efi','*.map','out','__pycache__','*.so'))
for p in H.glob('*.py'):shutil.copy2(p,D/p.name)
for p in H.glob('*.ld'):shutil.copy2(p,D/p.name)
subprocess.run([sys.executable,str(D/'build_release.py'),str(D/'base/Mu-ruby-v0.38b-gicgate.img')],cwd=D,check=True)
name='Mu-ruby-v0.40-native-switch.img'
expected=hashlib.sha256((H/name).read_bytes()).hexdigest()
got=hashlib.sha256((D/name).read_bytes()).hexdigest()
assert got==expected,'Rebuilt image differs'
print('BYTE-IDENTICAL',got)
print('New output directory:',D)
