import pefile,capstone,struct,json,sys,re
pe=pefile.PE('E:/openSA/gta_sa.exe')
B=0x400000
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32); md.detail=False
def rd(a,n): return pe.get_data(a-B,n)
def u32(a): return struct.unpack('<I',rd(a,4))[0]
handlers=[u32(0x8A6168+4*i) for i in range(27)]
CP=0x464080; GV=0x464790; SP=0x464370; RT=0x463D50
cache={}
def ins_at(a):
    if a in cache: return cache[a]
    r=list(md.disasm(rd(a,16),a,1))
    cache[a]=r[0] if r else None
    return cache[a]
def walk(entry,opc,depth=0):
    """follow fall-through path; return (count, flags)"""
    a=entry; n=0; flags=set(); prev=None; seen=set(); steps=0
    pend=None; regs={}; prevmov=''; hist=[]
    while steps<400:
        steps+=1
        if a in seen: flags.add('loop'); break
        seen.add(a)
        i=ins_at(a)
        if i is None: flags.add('bad'); break
        m=i.mnemonic; hist.append(i)
        if m=='push' and i.op_str.lstrip('-').replace('0x','').isalnum():
            try: pend=int(i.op_str,0)
            except: pend=regs.get(i.op_str)
        elif m=='push':
            pend=regs.get(i.op_str)
        if m=='mov' and re.match(r'e\w\w, (0x[0-9a-f]+|\d+)$',i.op_str):
            r_,v_=i.op_str.split(', '); regs[r_]=int(v_,0)
        elif m=='mov' and re.match(r'[a-d]l, (0x[0-9a-f]+|\d+)$',i.op_str):
            r_,v_=i.op_str.split(', '); regs['e'+r_[0]+'x']=int(v_,0)
        elif m=='movzx' and re.match(r'[sd]i, [a-d]l$',i.op_str):
            a1,b1=i.op_str.split(', '); regs['e'+a1]=regs.get('e'+b1[0]+'x')
        elif m=='xor' and len(set(i.op_str.split(', ')))==1: regs[i.op_str.split(', ')[0]]=0
        if m=='call':
            t=int(i.op_str,0) if i.op_str.startswith('0x') else None
            if t==CP:
                if pend is None: flags.add('var')
                else: n+=pend
            elif t==GV or t==RT: n+=1
            elif t==SP:
                if pend is None: flags.add('var')
                else: n+=pend
            elif t is not None and depth<3 and prevmov in ('ecx, esi','ecx, ebp'):
                key=(t,opc if depth==0 else 0)
                x,fx=walk(t,opc,depth+1)
                n+=x; flags|=fx
            pend=None
        if '[esi + 0x14]' in i.op_str: flags.add('ip'); ipn=globals().get('ipn',0)
        if m=='ret': break
        if m=='jmp':
            if i.op_str.startswith('0x'): a=int(i.op_str,0); continue
            mt=re.search(r'\[(e\w\w)\*4 \+ (0x[0-9a-f]+)\]',i.op_str)
            if not mt: flags.add('indirect'); break
            T=int(mt.group(2),0); base_=0; cnt_=None; bm_=None
            for j in reversed(hist[-14:]):
                if j.mnemonic=='movzx' and 'byte ptr' in j.op_str and bm_ is None:
                    bm_=int(re.search(r'\+ (0x[0-9a-f]+)\]',j.op_str).group(1),0)
                if j.mnemonic=='cmp' and cnt_ is None and re.match(r'e\w\w, 0x[0-9a-f]+$',j.op_str) and not j.op_str.startswith('esp'):
                    cnt_=int(j.op_str.split(', ')[1],0)
                if j.mnemonic=='lea' and cnt_ is None and base_==0:
                    x_=re.search(r'\[e\w\w ([-+]) (0x[0-9a-f]+)\]',j.op_str)
                    if x_: base_=int(x_.group(2),0) if x_.group(1)=='-' else -int(x_.group(2),0)
                if j.mnemonic in('add','sub') and base_==0 and cnt_ is None and re.match(r'e\w\w, -?0x',j.op_str) and not j.op_str.startswith('esp'):
                    v_=int(j.op_str.split(', ')[1],0)
                    base_=-v_ if j.mnemonic=='add' else v_
            if cnt_ is None: flags.add('indirect'); break
            k_=opc-base_
            if k_<0 or k_>cnt_: flags.add('default'); break
            idx_=rd(bm_+k_,1)[0] if bm_ else k_
            a=u32(T+4*idx_); hist.append(i); continue
        if m=='cmp' and re.match(r'e\w\w, (0x[0-9a-f]+|\d+)$',i.op_str) and not i.op_str.startswith('esp'):
            imm=int(i.op_str.split(', ')[1],0)
            nx=ins_at(a+i.size); skip=i.size
            for _ in range(4):
                if nx and not nx.mnemonic.startswith('j') and nx.mnemonic in ('mov','push','lea','movzx','movsx','pop'):
                    mm_=re.match(r'([a-d])l, (0x[0-9a-f]+|\d+)$',nx.op_str) if nx.mnemonic=='mov' else None
                    if mm_: regs['e'+mm_.group(1)+'x']=int(mm_.group(2),0)
                    skip+=nx.size; nx=ins_at(a+skip)
                else: break
            if nx and nx.mnemonic.startswith('j') and nx.mnemonic!='jmp' and abs(imm-opc)<100 and imm>=100:
                x,y=opc,imm
                cond={'je':x==y,'jne':x!=y,'jg':x>y,'jge':x>=y,'jl':x<y,'jle':x<=y,'ja':x>y,'jae':x>=y,'jb':x<y,'jbe':x<=y}.get(nx.mnemonic)
                if cond is not None:
                    a = int(nx.op_str,0) if cond else a+skip+nx.size
                    continue
        prevmov=i.op_str if (m=='mov' and i.op_str in ('ecx, esi','ecx, ebp')) else ('' if m not in ('push','lea') else prevmov)
        a+=i.size
    return n,flags
res={}
import re
for h,hp in enumerate(handlers):
    a=hp; base=0; cnt=None; bmap=None; tab=None
    for _ in range(60):
        i=ins_at(a); m=i.mnemonic; o=i.op_str
        mm=re.match(r'(e\w\w), dword ptr \[esp \+ 4\]',o) if m=='mov' else None
        if m=='lea':
            x=re.search(r'\[e\w\w ([-+]) (0x[0-9a-f]+)\]',o)
            if x and base==0 and cnt is None: base=(int(x.group(2),0) if x.group(1)=='-' else -int(x.group(2),0))
        if m in('add','sub') and cnt is None and not o.startswith('esp') and base==0 and re.match(r'e\w\w, -?0x',o):
            v=int(o.split(', ')[1],0)
            if m=='add':
                v=v-(1<<32) if v>=1<<31 else v
                base=-v
            else: base=v
        if m=='cmp' and cnt is None and re.match(r'e\w\w, 0x[0-9a-f]+$',o):
            cnt=int(o.split(', ')[1],0)
        if m=='movzx' and 'byte ptr' in o:
            bmap=int(re.search(r'\+ (0x[0-9a-f]+)\]',o).group(1),0)
        if m=='jmp' and '*4' in o:
            tab=int(re.search(r'\+ (0x[0-9a-f]+)\]',o).group(1),0); break
        a+=i.size
    print(h,hex(hp),'base',base,'cnt',cnt,'bmap',hex(bmap) if bmap else None,'tab',hex(tab) if tab else None,file=sys.stderr)
    if tab is None or cnt is None: continue
    for k in range(cnt+1):
        idx=rd(bmap+k,1)[0] if bmap else k
        t=u32(tab+4*idx)
        n,f=walk(t,base+k)
        res[base+k]=(n,sorted(f),t)
json.dump({str(k):v for k,v in res.items()},open('E:/openSA/tools/scm_args.json','w'))
print(len(res))
