"""strref.py <texte> [exact] : adresses des chaines contenant <texte> et instructions qui les citent"""
import sys, struct, pickle, bisect
sys_argv = sys.argv[:]; sys.argv = ['x', 'x']
exec(open("sr2.py").read().split("if __name__")[0])
needle = sys_argv[1].encode(); exact = len(sys_argv) > 2
tg = pickle.load(open('calltargets.pkl', 'rb')); starts = sorted(tg)
def func(va):
    i = bisect.bisect_right(starts, va) - 1
    return starts[i] if i >= 0 else 0
a0, tb = text_bytes()
for n, a, vs, rs, ro in SECS:
    blob = d[ro:ro+rs]; i = blob.find(needle)
    while i != -1:
        s = i
        while s > 0 and 32 <= blob[s-1] < 127: s -= 1
        e = blob.find(b'\0', i); text = blob[s:e].decode('latin1')
        va = a + s
        if not exact or text == sys_argv[1]:
            pat = struct.pack('<I', va); refs = []; j = tb.find(pat)
            while j != -1:
                refs.append(a0 + j); j = tb.find(pat, j + 1)
            print("%08X %-40s %s" % (va, text[:40], ' '.join("%X(f %X)" % (r, func(r)) for r in refs[:8])))
        i = blob.find(needle, e)
