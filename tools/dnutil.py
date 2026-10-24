import dnfile
from dnfile.enums import MetadataTables
def B(x):
    for a in ('value','raw_data'):
        v = getattr(x, a, None)
        if v is not None: return bytes(v)
    return bytes(x)
def S(x): return str(getattr(x,'value',x))
ELEM = {0x01:'void',0x02:'bool',0x03:'char',0x04:'i8',0x05:'u8',0x06:'i16',0x07:'u16',0x08:'i32',0x09:'u32',0x0a:'i64',0x0b:'u64',0x0c:'f32',0x0d:'f64',0x0e:'string',0x0f:'ptr',0x10:'byref',0x11:'valuetype',0x12:'class',0x13:'var',0x14:'array',0x15:'generic',0x16:'typedbyref',0x18:'intptr',0x19:'uintptr',0x1b:'fnptr',0x1c:'object',0x1d:'szarray',0x1e:'mvar',0x1f:'cmod_reqd',0x20:'cmod_opt',0x45:'pinned'}
def read_compressed(b, i):
    x = b[i]
    if x < 0x80: return x, i+1
    if x < 0xC0: return ((x & 0x3f) << 8) | b[i+1], i+2
    return ((x & 0x1f) << 24) | (b[i+1] << 16) | (b[i+2] << 8) | b[i+3], i+4
def typename_from_coded(md, tok):
    tag = tok & 3; idx = tok >> 2
    tbl = {0: md.TypeDef, 1: md.TypeRef, 2: md.TypeSpec}[tag]
    row = tbl.rows[idx-1]
    return S(getattr(row, 'TypeName', 'spec'))
def decode_type(md, b, i):
    e = b[i]; i += 1
    if e in (0x11, 0x12):
        tok, i = read_compressed(b, i); return typename_from_coded(md, tok), i
    if e in (0x0f, 0x10, 0x1d, 0x45):
        t, i = decode_type(md, b, i); return (ELEM[e] + '<' + t + '>'), i
    if e in (0x1f, 0x20):
        tok, i = read_compressed(b, i); return decode_type(md, b, i)
    if e == 0x15:
        t, i = decode_type(md, b, i); n, i = read_compressed(b, i); args=[]
        for _ in range(n): a, i = decode_type(md, b, i); args.append(a)
        return f"{t}<{','.join(args)}>", i
    if e == 0x14:
        t, i = decode_type(md, b, i); rank, i = read_compressed(b, i); ns, i = read_compressed(b, i)
        for _ in range(ns): _, i = read_compressed(b, i)
        nl, i = read_compressed(b, i)
        for _ in range(nl): _, i = read_compressed(b, i)
        return f"{t}[{','*(rank-1)}]", i
    return ELEM.get(e, hex(e)), i
def decode_method_sig(md, blob):
    i = 0; flags = blob[i]; i += 1
    if flags & 0x10: _, i = read_compressed(blob, i)
    n, i = read_compressed(blob, i)
    ret, i = decode_type(md, blob, i)
    params = []
    for _ in range(n):
        t, i = decode_type(md, blob, i); params.append(t)
    return ret, params
def decode_field_sig(md, blob):
    i = 1
    t, i = decode_type(md, blob, i); return t
