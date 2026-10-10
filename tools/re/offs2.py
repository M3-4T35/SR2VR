import sys,pickle,bisect,struct
argv=sys.argv[1:]; sys.argv=['x','x']
exec(open('sr2.py').read().split("if __name__")[0])
tg=pickle.load(open('calltargets.pkl','rb')); starts=sorted(tg)
a0,tb=text_bytes()
for f in argv:
    field=int(f,16); pat=struct.pack('<I',field); i=tb.find(pat)
    while i!=-1:
        ins=insn_containing(a0+i)
        if ins and ('+ 0x%x]'%field) in ins.op_str:
            o=starts[bisect.bisect_right(starts,ins.address)-1]
            print('%08X(%2d)  %s'%(o,len(tg[o]),fmt(ins)))
        i=tb.find(pat,i+1)
