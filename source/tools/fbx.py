import struct, zlib, sys
def parse(path):
    d=open(path,'rb').read()
    ver=struct.unpack('<I',d[23:27])[0]
    big = ver>=7500
    def node(o):
        if big:
            end,np,pl=struct.unpack('<QQQ',d[o:o+24]); o+=24
        else:
            end,np,pl=struct.unpack('<III',d[o:o+12]); o+=12
        nl=d[o]; o+=1
        if end==0: return None,o
        name=d[o:o+nl].decode('latin1'); o+=nl
        props=[]
        for _ in range(np):
            t=chr(d[o]); o+=1
            if t in 'YCIFDL':
                fmt={'Y':'<h','C':'<?','I':'<i','F':'<f','D':'<d','L':'<q'}[t]
                sz=struct.calcsize(fmt); props.append(struct.unpack(fmt,d[o:o+sz])[0]); o+=sz
            elif t in 'fdlib':
                n,enc,cl=struct.unpack('<III',d[o:o+12]); o+=12
                raw=d[o:o+cl]; o+=cl
                if enc==1: raw=zlib.decompress(raw)
                fmt={'f':'f','d':'d','l':'q','i':'i','b':'?'}[t]
                props.append(list(struct.unpack('<%d%s'%(n,fmt),raw)))
            elif t in 'SR':
                n=struct.unpack('<I',d[o:o+4])[0]; o+=4
                v=d[o:o+n]; o+=n
                props.append(v.decode('latin1') if t=='S' else v)
        children=[]
        while o<end:
            c,o2=node(o)
            if c is None: o=o2; break
            children.append(c); o=o2
        o=end
        return {'name':name,'props':props,'ch':children},o
    o=27; top=[]
    while o<len(d)-200:
        n,o=node(o)
        if n is None: break
        top.append(n)
    return top
def find(n,name): return [c for c in n['ch'] if c['name']==name]
