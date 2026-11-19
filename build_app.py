#!/usr/bin/env python3
from pathlib import Path
import subprocess
H=Path(__file__).resolve().parent
p=H/'ebsprobe'
flags=['clang','--target=aarch64-unknown-windows','-ffreestanding','-fno-builtin','-fno-stack-protector','-fshort-wchar','-mgeneral-regs-only','-mstrict-align','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter']
for source in ['loader.c','transition.S','irq_vectors.S']:
 subprocess.run(flags+['-c',str(p/source),'-o',str(p/(source+'.obj'))],check=True)
subprocess.run(['lld-link','/subsystem:efi_application','/entry:efi_main','/nodefaultlib','/machine:arm64','/base:0xa1000000','/dynamicbase','/timestamp:0x6af4c4de','/filealign:512','/map:'+str(p/'loader.map'),'/out:'+str(p/'loader.efi'),str(p/'loader.c.obj'),str(p/'transition.S.obj'),str(p/'irq_vectors.S.obj')],check=True)
print('BUILT',p/'loader.efi',(p/'loader.efi').stat().st_size)
