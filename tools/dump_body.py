import dnfile, sys
from dncil.cil.body import CilMethodBody
from dncil.cil.body.reader import CilMethodBodyReaderBase
from dncil.clr.token import Token, StringToken
from dnfile.enums import MetadataTables
S=lambda x: str(getattr(x,'value',x))
pe = dnfile.dnPE(sys.argv[1]); md = pe.net.mdtables
class R(CilMethodBodyReaderBase):
    def __init__(s, pe, row): s.pe=pe; s.offset=pe.get_offset_from_rva(row.Rva)
    def read(s,n): d=s.pe.get_data(s.pe.get_rva_from_offset(s.offset),n); s.offset+=n; return d
    def tell(s): return s.offset
    def seek(s,o): s.offset=o; return o
owner={}
for td in md.TypeDef.rows:
    for m in td.MethodList: owner[m.row_index]=S(td.TypeName)
def tn(tok):
    if isinstance(tok, StringToken):
        try: return repr(pe.net.user_strings.get(tok.rid).value)
        except Exception: return '<str>'
    try: t=MetadataTables(tok.table).name; r=getattr(md,t).rows[tok.rid-1]
    except Exception: return str(tok)
    if t=='MethodDef': return f"{owner.get(tok.rid,'?')}::{S(r.Name)}"
    if t=='MemberRef':
        c=r.Class.row; return f"{S(getattr(c,'TypeName',None) or getattr(c,'Name','?'))}::{S(r.Name)}"
    if t=='Field': return f"fld:{S(r.Name)}"
    if t in('TypeRef','TypeDef'): return f"type:{S(r.TypeName)}"
    return f"{t}:{tok.rid}"
full = '--full' in sys.argv
ops=('call','callvirt','newobj','ldstr','ldc.i4','ldc.i4.s','ldc.i4.0','ldc.i4.1','ldc.i4.2','ldc.i4.3','ldc.i4.4','ldc.i4.5','ldc.i4.m1','stfld','ldfld','stsfld','ldsfld','ret','brtrue.s','brfalse.s','brtrue','brfalse','ldftn','switch','beq','bne.un','beq.s','bne.un.s')
for spec in [a for a in sys.argv[2:] if not a.startswith('--')]:
    tdname, mname = spec.split('::')
    for td in md.TypeDef.rows:
        if S(td.TypeName)!=tdname: continue
        for mi in td.MethodList:
            m=mi.row
            if S(m.Name)!=mname or not m.Rva: continue
            print(f"##### {tdname}::{mname}")
            try: body=CilMethodBody(R(pe,m))
            except Exception as e: print("  err", e); continue
            for ins in body.instructions:
                op=ins.operand; s=tn(op) if isinstance(op,Token) else (op if op is not None else '')
                if full or ins.opcode.name in ops: print(f"  {ins.offset:04x} {ins.opcode.name:10s} {s}")
