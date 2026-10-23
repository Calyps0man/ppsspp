# Verify required PSL1GHT post-link transformations before building a SELF.
import struct, sys
from pathlib import Path
b=Path(sys.argv[1]).read_bytes()
def need(ok,why):
    if not ok: raise SystemExit("Invalid PS3 ELF: "+why)
need(b[:6]==b"\x7fELF\x02\x02","expected big-endian ELF64")
need(b[7]==102,"Cell LV2 OSABI missing; run sprxlinker")
shoff=struct.unpack_from(">Q",b,40)[0]
stride,count,names_index=struct.unpack_from(">HHH",b,58)
need(stride==64,"unexpected section header size")
headers=[struct.unpack_from(">IIQQQQIIQQ",b,shoff+i*stride) for i in range(count)]
def data(h): return b[h[4]:h[4]+h[5]]
names=data(headers[names_index])
sections={names[h[0]:].split(b"\0",1)[0].decode():h for h in headers}
for name in (".lib.stub",".rodata.sceFNID",".opd"):
    need(name in sections,"missing "+name)
stubs=data(sections[".lib.stub"]); ids=sections[".rodata.sceFNID"]
need(len(stubs)>0 and len(stubs)%44==0,"invalid import stub table")
starts=[struct.unpack_from(">I",stubs,i+20)[0] for i in range(0,len(stubs),44)]
for index,offset in enumerate(range(0,len(stubs),44)):
    actual=struct.unpack_from(">H",stubs,offset+6)[0]; start=starts[index]
    end=min([v for v in starts if v>start]+[ids[3]+ids[5]])
    need(ids[3]<=start<end<=ids[3]+ids[5],"import table outside FNID section")
    need((end-start)%4==0 and actual==(end-start)//4,"incorrect import count; run sprxlinker")
    need(actual>0,"empty system-library imports")
opds=data(sections[".opd"])
need(len(opds)>0 and len(opds)%24==0,"invalid function descriptors")
for offset in range(0,len(opds),24):
    entry,toc,native=struct.unpack_from(">QQQ",opds,offset)
    need(native==((entry<<32)|(toc&0xffffffff))&0xffffffffffffffff,"native descriptor not fixed; run sprxlinker")
print("PS3 ELF post-link checks passed: Cell OSABI,",len(starts),"import tables,",len(opds)//24,"function descriptors")
