"""Outils d'analyse statique de SR2_pc.exe (exe modifie par Juiced, base 0x400000).
usage : sr2.py uf <va> [max_insn]   desassemble une fonction (jusqu'au ret/int3 de fin)
        sr2.py dis <va> <n>         desassemble n instructions
        sr2.py calls <va>           appels directs (E8) et sauts (E9) vers va
        sr2.py refs <va>            instructions contenant l'adresse absolue va (imm32/disp32)
        sr2.py refrange <lo> <hi>   idem pour une plage d'adresses (resume par adresse)
        sr2.py func <va>            debut probable de la fonction contenant va
"""
import struct, sys, capstone, bisect
PATH = "/mnt/user-data/uploads/Saints Row 2/SR2_pc.exe"
d = open(PATH, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe+6)[0]; optsz = struct.unpack_from('<H', d, pe+20)[0]
BASE = struct.unpack_from('<I', d, pe+24+28)[0]
SECS = []
for i in range(nsec):
    o = pe+24+optsz+40*i
    name = d[o:o+8].rstrip(b'\0').decode(); vs, va, rs, ro = struct.unpack_from('<IIII', d, o+8)
    SECS.append((name, BASE+va, vs, rs, ro))
TEXT = SECS[0]
def off(va):
    for n, a, vs, rs, ro in SECS:
        if a <= va < a+max(vs, rs):
            o = va-a
            return ro+o if o < rs else None
    return None
def read(va, n):
    o = off(va)
    return d[o:o+n] if o is not None else b'\0'*n
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
def dis(va, n):
    out = []
    code = read(va, n*16)
    for ins in md.disasm(code, va):
        out.append(ins)
        if len(out) >= n: break
    return out
def fmt(ins):
    return "%08X  %-24s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str)
def uf(va, maxn=400):
    n = 0; a = va; maxtarget = va
    while n < maxn:
        code = read(a, 16)
        ins = next(md.disasm(code, a), None)
        if ins is None: print("%08X  ??" % a); break
        print(fmt(ins)); n += 1; a += ins.size
        if ins.mnemonic.startswith('j') and ins.op_str.startswith('0x'):
            t = int(ins.op_str, 16)
            if va <= t < va+0x4000: maxtarget = max(maxtarget, t)
        if ins.mnemonic in ('ret', 'retn') and a > maxtarget: break
        if ins.mnemonic == 'jmp' and a > maxtarget: break
def text_bytes():
    n, a, vs, rs, ro = TEXT
    return a, d[ro:ro+rs]
def calls(target):
    a, tb = text_bytes(); res = []
    for op in (0xE8, 0xE9):
        i = tb.find(bytes([op]))
        while i != -1:
            if i+5 <= len(tb):
                rel = struct.unpack_from('<i', tb, i+1)[0]
                if (a+i+5+rel) & 0xffffffff == target: res.append((a+i, 'call' if op == 0xE8 else 'jmp'))
            i = tb.find(bytes([op]), i+1)
    return sorted(res)
def refs(target):
    a, tb = text_bytes(); pat = struct.pack('<I', target); res = []
    i = tb.find(pat)
    while i != -1:
        res.append(a+i); i = tb.find(pat, i+1)
    return res
def insn_containing(va):
    # recule pour trouver l'instruction qui contient va : d'abord les formes modrm disp32
    # (octet precedent = mod 00, r/m 101), sinon les formes moffs/imm32 a un octet d'opcode
    order = []
    if (read(va-1, 1)[0] & 0xC7) == 0x05: order += [2, 3, 4]
    order += [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
    for back in order:
        for ins in md.disasm(read(va-back, 16), va-back):
            if ins.address <= va < ins.address+ins.size and ins.address+ins.size >= va+4: return ins
            break
    return None
def func_start(va):
    # cherche en arriere un alignement cc/90 suivi d'un prologue courant
    a = va
    for back in range(0, 0x3000):
        b = read(va-back-1, 1)[0]
        if b in (0xCC, 0x90, 0xC3) or read(va-back-3, 1)[0] == 0xC2:
            s = va-back
            if read(s, 1)[0] not in (0xCC, 0x90): return s
    return None
def ctx(target, before=24, after=6):
    # instructions autour de target : cherche un point de depart qui retombe sur target
    for back in range(before+16, before-1, -1):
        out = []; hit = None
        for ins in md.disasm(read(target-back, back+after*16), target-back):
            out.append(ins)
            if ins.address == target: hit = len(out)-1
            if hit is not None and len(out) > hit+after: break
        if hit is not None:
            for ins in out: print(fmt(ins))
            return
    print("pas d'alignement trouve pour %08X" % target)
if __name__ == '__main__':
    cmd = sys.argv[1]; A = lambda i: int(sys.argv[i], 16)
    if cmd == 'uf': uf(A(2), int(sys.argv[3]) if len(sys.argv) > 3 else 400)
    elif cmd == 'dis':
        for ins in dis(A(2), int(sys.argv[3])): print(fmt(ins))
    elif cmd == 'calls':
        for a, k in calls(A(2)): print("%08X %s" % (a, k))
    elif cmd == 'refs':
        for r in refs(A(2)):
            ins = insn_containing(r); print(fmt(ins) if ins else "%08X ?" % r)
    elif cmd == 'refrange':
        lo, hi = A(2), A(3); a, tb = text_bytes(); cnt = {}
        for i in range(0, len(tb)-4):
            if tb[i+3] != (lo >> 24): continue
            v = struct.unpack_from('<I', tb, i)[0]
            if lo <= v < hi: cnt.setdefault(v, []).append(a+i)
        for v in sorted(cnt): print("%08X : %d refs, ex. %s" % (v, len(cnt[v]), ' '.join('%08X' % x for x in cnt[v][:6])))
    elif cmd == 'ctx': ctx(A(2), int(sys.argv[3]) if len(sys.argv) > 3 else 24, int(sys.argv[4]) if len(sys.argv) > 4 else 6)
    elif cmd == 'func': print("%08X" % func_start(A(2)))
