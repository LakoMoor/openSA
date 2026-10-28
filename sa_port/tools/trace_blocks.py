import pefile,capstone,struct,sys
pe=pefile.PE('E:/openSA/gta_sa.exe',fast_load=True)
base=pe.OPTIONAL_HEADER.ImageBase
def rd(va,n):
    try: return pe.get_data(va-base,n)
    except: return b''
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32); md.detail=True
start=int(sys.argv[1],16); lim=int(sys.argv[2]) if len(sys.argv)>2 else 6000
seen={}; q=[start]
while q and len(seen)<lim:
    a=q.pop()
    while a not in seen:
        d=rd(a,16)
        ins=next(md.disasm(d,a),None)
        if not ins: break
        seen[a]=ins
        m=ins.mnemonic
        if m=='jmp' and ins.op_str.startswith('0x'): a=int(ins.op_str,16); continue
        if m.startswith('j') and ins.op_str.startswith('0x'): q.append(int(ins.op_str,16))
        if m in('ret','retn'): break
        a+=ins.size
import re
out=[]
for a in sorted(seen):
    i=seen[a]; s=f'{a:x}: {i.mnemonic} {i.op_str}'
    mm=re.search(r'\[(0x[0-9a-f]+)\]',i.op_str)
    if mm and i.mnemonic in('fld','fmul','fadd','fsub','fdiv','fcomp','fcom','fsubr','fdivr','movss','mulss','addss','subss','comiss','fld'):
        v=int(mm.group(1),16)
        if 0x800000<=v<0xC00000:
            b=rd(v,4)
            if len(b)==4: s+=f'   ; {struct.unpack("<f",b)[0]:.5g}'
    out.append(s)
print('\n'.join(out))
