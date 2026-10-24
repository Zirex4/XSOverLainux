import sys, dnfile
from dnutil import *
pe = dnfile.dnPE(sys.argv[1]); md = pe.net.mdtables
for row in md.ImplMap.rows:
    dll = S(row.ImportScope.row.Name); m = row.MemberForwarded.row
    try: ret, params = decode_method_sig(md, B(m.Signature))
    except Exception as e: ret, params = '?', [repr(e)]
    print(f"{dll}!{S(row.ImportName)}  ->  {ret} {S(m.Name)}({', '.join(params)})")
