import sys, pickle, bisect
tg=pickle.load(open('calltargets.pkl','rb'))
starts=sorted(tg)
for a in sys.argv[1:]:
    va=int(a,16); o=starts[bisect.bisect_right(starts,va)-1]
    print('%08X dans %08X (%d appelants : %s)'%(va,o,len(tg[o]),' '.join('%08X'%x for x in tg[o][:12])))
