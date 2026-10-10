import sys,re
exec(open('sr2.py').read().split("if __name__")[0])
a0,tb=text_bytes()
def count(sig):
    parts=sig.split()
    rx=b''.join(b'.' if p.startswith('?') else re.escape(bytes([int(p,16)])) for p in parts)
    return [a0+m.start() for m in re.finditer(rx,tb,re.S)]
if __name__=='__main__':
    for s in sys.argv[1:]:
        r=count(s); print(len(r),' '.join('%08X'%x for x in r[:6]),'|',s[:60])
