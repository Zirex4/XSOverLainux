import sys, dnfile
from dnutil import *
pe = dnfile.dnPE(sys.argv[1]); md = pe.net.mdtables
fields = {}
for td in md.TypeDef.rows:
    if S(td.TypeName) in sys.argv[2:]:
        for f in td.FieldList: fields[f.row_index] = (S(td.TypeName), S(f.row.Name))
for r in md.FieldMarshal.rows:
    p = r.Parent
    if p.table.name == 'Field' and p.row_index in fields:
        print(fields[p.row_index], B(r.NativeType).hex())
# class layout
for r in md.ClassLayout.rows:
    t = r.Parent.row
    if S(t.TypeName) in sys.argv[2:]: print("layout", S(t.TypeName), "pack", r.PackingSize, "size", r.ClassSize)
