import sys, dnfile
from dnutil import *
pe = dnfile.dnPE(sys.argv[1]); md = pe.net.mdtables
want = sys.argv[2:]
for td in md.TypeDef.rows:
    name = S(td.TypeName); ns = S(td.TypeNamespace)
    if not (name in want or any(w[1:].lower() in name.lower() for w in want if w.startswith("~"))): continue
    print(f"=== {ns}.{name}")
    for f in td.FieldList:
        r = f.row
        try: t = decode_field_sig(md, B(r.Signature))
        except Exception as e: t = '?'
        print(f"    field {t} {S(r.Name)}")
    print("    methods:", ", ".join(S(m.row.Name) for m in td.MethodList)[:800])
