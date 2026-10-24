import re,sys,os,subprocess,collections
# Where the decompilation sources live: the disasm/ submodule of this repository (a checkout of pret/pokestadium), or the folder
# given as the first argument. Nothing here reads the ROM; this script only scans C and assembly source text.
here=os.path.dirname(os.path.abspath(__file__))
decomp=sys.argv[1] if len(sys.argv)>1 else os.path.join(here,'..','..','..','disasm')
roots=[os.path.join(decomp,d) for d in ('src','asm','include')]
pat=re.compile(r'\b(g?)s?DPSetCombine(Mode|LERP)\s*\(')
inv=collections.Counter(); where={}
def balanced(t,i):
    d=1;j=i
    while j<len(t) and d:
        c=t[j]
        if c=='(':d+=1
        elif c==')':d-=1
        j+=1
    return t[i:j-1]
files=0
for r in roots:
    for dp,dn,fn in os.walk(r):
        for f in fn:
            if not f.endswith(('.c','.h','.inc','.s','.cpp')): continue
            p=os.path.join(dp,f)
            try: t=open(p,errors='replace').read()
            except: continue
            files+=1
            for m in re.finditer(r'\b(g?s?DPSetCombine(?:Mode|LERP))\s*\(',t):
                name=m.group(1); a=balanced(t,m.end())
                args=[x.strip() for x in re.sub(r'\s+',' ',a).split(',')]
                if not name.startswith('gs'):   # gDPSet...(pkt, ...)
                    args=args[1:]
                if '//' in a or '/*' in a: continue
                key=(('Mode' if 'Mode' in name else 'LERP'),tuple(args))
                inv[key]+=1; where.setdefault(key,p)
print('files',files,'unique invocations',len(inv),file=sys.stderr)
lines=['#include <ultra64.h>','extern int printf(const char*,...);']
keys=list(inv)
for i,k in enumerate(keys):
    kind,args=k
    lines.append('Gfx v%d[] = { gsDPSetCombine%s(%s) };'%(i,kind,', '.join(args)))
lines.append('int main(){')
for i,k in enumerate(keys):
    lines.append('printf("%%u %%08X%%08X\\n", %d, v%d[0].words.w0, v%d[0].words.w1);'%(i,i,i))
lines.append('return 0;}')
open('gen.c','w').write('\n'.join(lines))
import pickle; pickle.dump((keys,inv,where),open('inv.pkl','wb'))
