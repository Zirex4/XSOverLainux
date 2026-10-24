import sys, dnfile
from dncil.cil.body import CilMethodBody
from dncil.cil.body.reader import CilMethodBodyReaderBase
S = lambda x: str(getattr(x,'value',x))
class Reader(CilMethodBodyReaderBase):
    def __init__(self, pe, row): self.pe = pe; self.offset = pe.get_offset_from_rva(row.Rva)
    def read(self, n): d = self.pe.get_data(self.pe.get_rva_from_offset(self.offset), n); self.offset += n; return d
    def tell(self): return self.offset
    def seek(self, off): self.offset = off; return off
pe = dnfile.dnPE(sys.argv[1]); md = pe.net.mdtables
for td in md.TypeDef.rows:
    for m in td.MethodList:
        r = m.row
        if S(r.Name) in sys.argv[2:] and r.Rva:
            body = CilMethodBody(Reader(pe, r))
            print(f"{S(td.TypeName)}::{S(r.Name)} size={body.size} handlers={[(h.try_start, h.try_end, h.handler_start) for h in body.exception_handlers]}")
