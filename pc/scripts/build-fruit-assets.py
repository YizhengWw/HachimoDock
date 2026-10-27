"""Original supersampled fruit illustrations; stdlib-only PNG authoring.
Generator stays outside the component. No external art or network assets.
"""
import math, struct, zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]/'builtin-clawpkgs/fresh-fruit-slice/assets'
def png(name,kind):
    colors=[(229,63,57),(248,157,27),(111,180,43),(241,91,116)]
    rows=[]
    for y in range(32):
        row=bytearray([0])
        for x in range(32):
            values=[]
            for sy in range(4):
                for sx in range(4):
                    u=(x+(sx+.5)/4-16)/13;v=(y+(sy+.5)/4-18)/12
                    leaf=((u-.29)/.39)**2+((v+1.04)/.16)**2<1
                    stem=abs(u-.025)<.045 and -1.22<v<-.7
                    radius=u*u+v*v
                    inside=radius<1
                    if kind==0: inside=(u*u/(.94+.07*math.cos(v*5))**2+v*v<1) and not(abs(u)<.12 and v<-.82)
                    if leaf: rgba=(87,158,57,255)
                    elif stem: rgba=(99,65,35,255)
                    elif inside:
                        light=.72+.27*(1-(u+.4)**2/3-(v+.45)**2/3)
                        shine=math.exp(-((u+.38)**2/.055+(v+.38)**2/.13))*.25
                        base=colors[kind]
                        if kind==2 and math.sin(math.atan2(v,u)*10+radius*2)>.8: light*=.83
                        if kind==1: light+=.02*math.sin(x*14+y*19)
                        if kind==3:
                            if radius>.83: base=(64,148,67)
                            elif radius>.69: base=(228,224,164)
                            elif ((round(u*4)+round(v*4))%3==0 and abs(u*4-round(u*4))<.10 and abs(v*4-round(v*4))<.19):base=(62,39,35)
                        rgba=tuple(max(0,min(255,round(c*light+255*shine))) for c in base)+(255,)
                    else: rgba=(0,0,0,0)
                    values.append(rgba)
            alpha=sum(v[3] for v in values)/16
            rgb=[round(sum(v[i]*v[3]/255 for v in values)/(alpha/255*16)) if alpha else 0 for i in range(3)]
            row.extend(rgb+[round(alpha)])
        rows.append(row)
    def chunk(t,b):return struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
    data=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',32,32,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows),9))+chunk(b'IEND',b'')
    (ROOT/name).write_bytes(data)
if __name__=='__main__':
    ROOT.mkdir(parents=True,exist_ok=True)
    for i,name in enumerate(('apple.png','orange.png','lime.png','melon.png')):png(name,i)
