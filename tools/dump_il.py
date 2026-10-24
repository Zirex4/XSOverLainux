import sys, dnfile
from dnfile.enums import MetadataTables
from dncil.cil.body import CilMethodBody
from dncil.cil.body.reader import CilMethodBodyReaderBase
from dncil.clr.token import Token, StringToken
S = lambda x: str(getattr(x,'value',x))
class Reader(CilMethodBodyReaderBase):
    def __init__(self, pe, row):
        self.pe = pe; self.offset = pe.get_offset_from_rva(row.Rva)
    def read(self, n):
        d = self.pe.get_data(self.pe.get_rva_from_offset(self.offset), n); self.offset += n; return d
    def tell(self): return self.offset
    def seek(self, off): self.offset = off; return off
path, targets = sys.argv[1], sys.argv[2:]
pe = dnfile.dnPE(path); md = pe.net.mdtables
owner = {}
for td in md.TypeDef.rows:
    for m in td.MethodList: owner[m.row_index] = f"{S(td.TypeNamespace)}.{S(td.TypeName)}"
def tokname(tok):
    if isinstance(tok, StringToken):
        try: return repr(pe.net.user_strings.get(tok.rid).value)
        except Exception: return "<str>"
    try:
        t = MetadataTables(tok.table).name; tbl = getattr(md, t); r = tbl.rows[tok.rid-1]
    except Exception: return str(tok)
    if t == 'MethodDef': return f"{owner.get(tok.rid,'?')}::{S(r.Name)}"
    if t == 'MemberRef':
        cls = r.Class.row; cn = S(getattr(cls,'TypeName',None) or getattr(cls,'Name','?')); return f"{cn}::{S(r.Name)}"
    if t == 'Field': return f"fld:{S(r.Name)}"
    if t in ('TypeRef','TypeDef'): return f"type:{S(r.TypeName)}"
    if t == 'MethodSpec': return "methodspec"
    return f"{t}:{tok.rid}"
tgt = {}
for row in md.ImplMap.rows:
    if S(row.ImportName) in targets: tgt[row.MemberForwarded.row_index] = S(row.ImportName)
for rid, m in enumerate(md.MethodDef.rows, 1):
    if S(m.Name) in targets: tgt[rid] = S(m.Name)
print("targets:", tgt)
for rid, m in enumerate(md.MethodDef.rows, 1):
    if not m.Rva: continue
    try: body = CilMethodBody(Reader(pe, m))
    except Exception: continue
    hits = [i for i in body.instructions if isinstance(i.operand, Token) and i.operand.table == MetadataTables.MethodDef.value and i.operand.rid in tgt]
    fld_targets = [t[4:] for t in targets if t.startswith('fld:')]
    if fld_targets:
        for i in body.instructions:
            if isinstance(i.operand, Token) and i.operand.table in (MetadataTables.Field.value, MetadataTables.MemberRef.value):
                n = tokname(i.operand)
                if any(n.endswith('::'+f) or n == 'fld:'+f for f in fld_targets): hits.append(i)
    if not hits: continue
    print(f"\n##### {owner.get(rid,'?')}::{S(m.Name)}  (hits {[tgt.get(h.operand.rid, tokname(h.operand)) for h in hits]})")
    for ins in body.instructions:
        op = ins.operand
        s = tokname(op) if isinstance(op, Token) else (op if op is not None else '')
        print(f"  {ins.offset:04x} {ins.opcode.name:12s} {s}")
