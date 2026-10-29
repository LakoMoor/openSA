import json,struct,sys
j=json.load(open('E:/openSA/tools/sa.json'))
db={}
for e in j['extensions']:
    for c in e['commands']:
        va=any(i.get('type')=='arguments' for i in c.get('input',[]))
        n=c['num_params']-(1 if va else 0)
        db[int(c['id'],16)]=(n,va,c['name'])
d=open('E:/Games/steamapps/common/Grand Theft Auto San Andreas/data/script/main.scm','rb').read()
SZ={1:4,2:2,3:2,4:1,5:2,6:4,7:6,8:6,9:8,0xA:2,0xB:2,0xC:6,0xD:6,0xF:16,0x10:2,0x11:2,0x12:6,0x13:6}
def arg(p):
    t=d[p]; p+=1
    if t==0xE: return p+1+d[p]
    if t in SZ: return p+SZ[t]
    raise ValueError(t)
def run(p,end):
    k=0
    while p<end:
        op=struct.unpack_from('<H',d,p)[0]&0x7fff; q=p+2
        if op not in db: return p,k,'noop %x'%op
        n,va,_=db[op]
        try:
            if op==0x5B6: q+=128; n=0
            for _ in range(n): q=arg(q)
            if va:
                while d[q]!=0: q=arg(q)
                q+=1
        except Exception as e: return p,k,'arg %x %s'%(op,e)
        p=q;k+=1
    return p,k,None
if __name__=='__main__':
    print(run(int(sys.argv[1]),int(sys.argv[2])))
