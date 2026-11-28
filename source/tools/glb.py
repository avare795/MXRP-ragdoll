import json,struct,numpy as np
CT={5120:np.int8,5121:np.uint8,5122:np.int16,5123:np.uint16,5125:np.uint32,5126:np.float32}
NC={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}
def load(path):
    d=open(path,'rb').read()
    l=struct.unpack('<I',d[12:16])[0]; j=json.loads(d[20:20+l])
    off=20+l; bl=struct.unpack('<I',d[off:off+4])[0]; bin_=d[off+8:off+8+bl]
    def acc(i):
        a=j['accessors'][i]; bv=j['bufferViews'][a['bufferView']]
        n=NC[a['type']]; dt=np.dtype(CT[a['componentType']])
        start=bv.get('byteOffset',0)+a.get('byteOffset',0); stride=bv.get('byteStride',0)
        if stride and stride!=n*dt.itemsize:
            raw=np.frombuffer(bin_,np.uint8,count=stride*a['count'],offset=start).reshape(a['count'],stride)[:,:n*dt.itemsize]
            arr=np.frombuffer(raw.tobytes(),dt).reshape(a['count'],n)
        else:
            arr=np.frombuffer(bin_,dt,count=a['count']*n,offset=start).reshape(a['count'],n)
        arr=arr.astype(np.float64)
        if a.get('normalized'):
            arr/= float(np.iinfo(dt).max)
        return arr
    def img(i):
        im=j['images'][i]; bv=j['bufferViews'][im['bufferView']]
        return bin_[bv.get('byteOffset',0):bv.get('byteOffset',0)+bv['byteLength']]
    def qmat(q):
        x,y,z,w=q
        return np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],[2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],[2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]])
    def local(n):
        if 'matrix' in n: return np.array(n['matrix']).reshape(4,4).T
        M=np.eye(4)
        R=qmat(n.get('rotation',[0,0,0,1])); S=np.diag(n.get('scale',[1,1,1]))
        M[:3,:3]=R@S; M[:3,3]=n.get('translation',[0,0,0]); return M
    prims=[]
    def walk(ni,P):
        n=j['nodes'][ni]; M=P@local(n)
        if 'mesh' in n:
            for p in j['meshes'][n['mesh']]['primitives']:
                pos=acc(p['attributes']['POSITION']); nor=acc(p['attributes']['NORMAL']) if 'NORMAL' in p['attributes'] else None
                uv=acc(p['attributes']['TEXCOORD_0']) if 'TEXCOORD_0' in p['attributes'] else np.zeros((len(pos),2))
                idx=acc(p['indices']).astype(np.int64).ravel() if 'indices' in p else np.arange(len(pos))
                P4=np.c_[pos,np.ones(len(pos))]@M.T; pos=P4[:,:3]
                if nor is not None:
                    N=nor@np.linalg.inv(M[:3,:3]); nor=N/np.maximum(np.linalg.norm(N,axis=1,keepdims=True),1e-9)
                prims.append(dict(pos=pos,nor=nor,uv=uv,idx=idx.reshape(-1,3),mat=p.get('material',0)))
        for c in n.get('children',[]): walk(c,M)
    sc=j['scenes'][j.get('scene',0)]
    for r in sc['nodes']: walk(r,np.eye(4))
    return j,prims,img
