import sys
sys.argv_saved=sys.argv[:]
argv=sys.argv[1:]; sys.argv=['x','x']
exec(open('sr2.py').read().split("if __name__")[0])
import struct
a0, tb = text_bytes()
off=int(argv[0],16); limit=int(argv[1]) if len(argv)>1 else 40
pat=struct.pack('<I',off); n=0
i=tb.find(pat)
while i!=-1 and n<limit:
    ins=insn_containing(a0+i)
    if ins and ('+ 0x%x]'%off) in ins.op_str:
        print(fmt(ins)); n+=1
    i=tb.find(pat,i+1)
