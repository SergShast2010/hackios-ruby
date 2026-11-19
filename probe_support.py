import re,xml.etree.ElementTree as ET
import qemu_helpers as q
class RSP(q.RSP):
 def __init__(self,path):
  super().__init__(path);self.regs={};self.command('qSupported:qXfer:features:read+')
  nextnum=[0]
  def xml(name):
   data='';off=0
   while True:
    chunk=self.command(f'qXfer:features:read:{name}:{off:x},1000');assert chunk[0] in 'ml';data+=chunk[1:];off+=len(chunk[1:])
    if chunk[0]=='l':return ET.fromstring(data.replace('xi:',''))
  def visit(e):
   for child in e:
    tag=child.tag.rsplit('}',1)[-1]
    if tag=='include':visit(xml(child.attrib['href']))
    elif tag=='reg':
     number=int(child.get('regnum',nextnum[0]));nextnum[0]=number+1;self.regs[child.get('name')]=number
    else:visit(child)
  visit(xml('target.xml'))
 def registers(self):
  want=['pc','sp','cpsr']+[f'x{i}' for i in range(31)]+['SCTLR_EL1','VBAR_EL1','SP_EL0','SP_EL1','TTBR0_EL1','TTBR1_EL1','TCR_EL1','MAIR_EL1','TPIDR_EL1','CNTFRQ_EL0','CNTV_CTL_EL0','CNTV_CVAL_EL0','ICC_SRE_EL1','ICC_PMR_EL1','ICC_IGRPEN1_EL1','ICC_CTLR_EL1','ICC_BPR1_EL1','ICC_AP1R0_EL1','ESR_EL1','FAR_EL1','ELR_EL1','SPSR_EL1']
  out={}
  for name in want:
   actual=next((s for s in [name,name.removesuffix('_EL1')] if s in self.regs),None)
   if actual is None:continue
   val=self.command(f'p{self.regs[actual]:x}')
   if re.fullmatch('[a-fA-F0-9]+',val):out[name]=int.from_bytes(bytes.fromhex(val),'little')
  return out
 def write(self,addr,data):
  # QEMU GDB PacketSize is 4096 chars; an ADT must not be one giant M packet.
  for off in range(0,len(data),1024):super().write(addr+off,data[off:off+1024])
 def read(self,addr,n):
  out=bytearray()
  for off in range(0,n,1024):
   data=self.command(f'm{addr+off:x},{min(n-off,1024):x}')
   assert not data.startswith('E'),(hex(addr+off),n,data)
   out.extend(bytes.fromhex(data))
  return bytes(out)
 def string(self,addr,n=512):
  return self.read(addr,n).split(b'\0',1)[0].decode(errors='replace')
