"""Read-only FAT16 path walker for host tests, never a device driver."""
import struct

def fat_path(image,path):
 bps=struct.unpack_from('<H',image,11)[0];spc=image[13]
 reserved=struct.unpack_from('<H',image,14)[0];copies=image[16]
 entries=struct.unpack_from('<H',image,17)[0];sectors=struct.unpack_from('<H',image,22)[0]
 assert bps==512 and spc and copies in (1,2)
 root=(reserved+copies*sectors)*bps;root_end=root+((entries*32+bps-1)//bps)*bps
 cluster_bytes=bps*spc
 def content(cluster):
  seen=set();parts=[]
  while 2<=cluster<0xfff8:
   assert cluster not in seen;seen.add(cluster)
   at=root_end+(cluster-2)*cluster_bytes
   assert at+cluster_bytes<=len(image)
   parts.append(image[at:at+cluster_bytes]);cluster=struct.unpack_from('<H',image,reserved*bps+2*cluster)[0]
  assert cluster>=0xfff8
  return b''.join(parts)
 directory=image[root:root_end]
 for index,name in enumerate(path):
  assert len(name)==11;entry=None
  for pos in range(0,len(directory),32):
   candidate=directory[pos:pos+32]
   if not candidate or candidate[0]==0:break
   if candidate[0]!=0xe5 and candidate[11]!=15 and candidate[:11]==name:entry=candidate;break
  assert entry is not None,repr(name)
  cluster=struct.unpack_from('<H',entry,26)[0];size=struct.unpack_from('<I',entry,28)[0]
  data=content(cluster)
  if index+1<len(path):assert entry[11]&16;directory=data
  else:assert not entry[11]&16;assert size<=len(data);return data[:size]
 raise AssertionError('empty path')
