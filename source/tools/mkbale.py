import sys, zlib, math
sys.path.insert(0,'/tmp/claude-0/tools')
import fbx
import numpy as np
from PIL import Image
HB='/mnt/user-data/uploads/Trackside Assets/Trackside Assets/'
RB='/mnt/user-data/uploads/RealisticBale/RealisticBale/'
def walk(n):
    yield n
    for c in n.get('ch',[]): yield from walk(c)
def load(path):
    top=fbx.parse(path); T={t['name']:t for t in top}
    objs={c['props'][0]:c for c in T['Objects']['ch']}
    conns=[c['props'] for c in T['Connections']['ch']]
    return objs,conns
def model_props(m):
    r={}
    for n in walk(m):
        if n['name']=='P' and str(n['props'][0]).startswith('Lcl'): r[n['props'][0]]=n['props'][4:7]
    return r
def geom_tris(g, scale, alpha=None):
    V=fbx.find(g,'Vertices')[0]['props'][0]; idx=fbx.find(g,'PolygonVertexIndex')[0]['props'][0]
    ln=fbx.find(g,'LayerElementNormal')[0]; N=fbx.find(ln,'Normals')[0]['props'][0]
    nmap=fbx.find(ln,'MappingInformationType')[0]['props'][0]
    NI=fbx.find(ln,'NormalsIndex'); NI=NI[0]['props'][0] if NI else None
    lu=fbx.find(g,'LayerElementUV')[0]; UV=fbx.find(lu,'UV')[0]['props'][0]; UVI=fbx.find(lu,'UVIndex')[0]['props'][0]
    lm=fbx.find(g,'LayerElementMaterial'); M=fbx.find(lm[0],'Materials')[0]['props'][0] if lm else [0]
    mmap=fbx.find(lm[0],'MappingInformationType')[0]['props'][0] if lm else 'AllSame'
    P=[V[i*3:i*3+3] for i in range(len(V)//3)]
    tris=[]; poly=[]; pi=0
    for k,i in enumerate(idx):
        last=i<0; vi=~i if last else i
        if nmap in ('ByPolygonVertex',b'ByPolygonVertex'):
            q=NI[k] if NI is not None else k; n=N[q*3:q*3+3]
        else: n=N[vi*3:vi*3+3]
        u=UVI[k]; uv=(UV[u*2],1.0-UV[u*2+1])
        p=P[vi]
        poly.append(((p[0]*scale[0],p[1]*scale[1],p[2]*scale[2]),(n[0]/scale[0],n[1]/scale[1],n[2]/scale[2]),uv))
        if last:
            mat=M[pi] if mmap in ('ByPolygon',b'ByPolygon') else M[0]
            for j in range(1,len(poly)-1): tris.append((mat,poly[0],poly[j],poly[j+1]))
            poly=[]; pi+=1
    return tris
def nrm(n):
    l=math.sqrt(sum(x*x for x in n)) or 1; return tuple(x/l for x in n)
VARS=[]
objs,conns=load(HB+'hayBales.fbx')
def children(pid,kind):
    return [objs[c[1]] for c in conns if c[0]=='OO' and c[2]==pid and c[1] in objs and objs[c[1]]['name']==kind]
def mat_tex(matid):
    for c in conns:
        if c[0]=='OP' and c[2]==matid and c[3]=='DiffuseColor':
            tid=c[1]
            for c2 in conns:
                if c2[0]=='OO' and c2[2]==tid and objs[c2[1]]['name']=='Video': return objs[c2[1]]['props'][1].split('\x00')[0]
TEXFILES={'hay.png':0,'HayStrap.jpg':1,'hay wrapped.png':2,'Bale.tga':3}
order={'Cube':0,'Cube.005':1,'Cylinder':2}
mods=[o for o in objs.values() if o['name']=='Model']
for m in sorted(mods,key=lambda m: order.get(m['props'][1].split('\x00')[0],9)):
    name=m['props'][1].split('\x00')[0]
    pr=model_props(m); sc=[x/100.0 for x in pr['Lcl Scaling']]
    g=children(m['props'][0],'Geometry')[0]
    mats=[mat_tex(x['props'][0]) for x in children(m['props'][0],'Material')]
    tris=geom_tris(g,sc)
    out=[]
    for (mat,*vs) in tris:
        tf=mats[mat] if mat<len(mats) else mats[0]
        out.append((TEXFILES[tf.split('\\')[-1].split('/')[-1]],[(p,nrm(n),uv) for p,n,uv in vs]))
    VARS.append((name,out))
# realistic bale
objs,conns=load(RB+'Bale.fbx')
m=[o for o in objs.values() if o['name']=='Model'][0]
pr=model_props(m); sc=[x/100.0 for x in pr['Lcl Scaling']]
g=[o for o in objs.values() if o['name']=='Geometry'][0]
al=np.asarray(Image.open(RB+'Texture/Bale.tga').convert('RGBA').split()[-1].resize((512,512)))
tris=geom_tris(g,sc); out=[]
drop=0
for (mat,*vs) in tris:
    us=[min(511,max(0,int((uv[0]%1.0)*512))) for p,n,uv in vs]; vv=[min(511,max(0,int((uv[1]%1.0)*512))) for p,n,uv in vs]
    cu=sum(us)//3; cv=sum(vv)//3
    if al[cv,cu]<128 or min(al[vv[i],us[i]] for i in range(3))<60: drop+=1; continue
    out.append((3,[(p,nrm(n),uv) for p,n,uv in vs]))
print('realistic dropped',drop,'kept',len(out),file=sys.stderr)
VARS.append(('Bale',out))
# blender coords: z up. center, put bottom at 0
VARS=[v for v in VARS if v[0] in ('Cylinder','Bale')]
TREMAP={2:0,3:1}
VARS=[(n,[(TREMAP[t],vs) for t,vs in tr]) for n,tr in VARS]
res=[]
for name,tris in VARS:
    ps=[p for t,vs in tris for p,n,uv in vs]
    mn=[min(p[k] for p in ps) for k in range(3)]; mx=[max(p[k] for p in ps) for k in range(3)]
    print(name,len(tris),'size',[round(mx[k]-mn[k],2) for k in range(3)],file=sys.stderr)
    res.append((name,tris,mn,mx))
import pickle; pickle.dump(res,open('/tmp/claude-0/bale/vars.pkl','wb'))
# ---- sortie : bale_model.inc ----
SCALE={'Cube':1.0,'Cube.005':1.0,'Cylinder':0.75,'Bale':1.0}
NAMES={'Cube':'carree sanglee','Cube.005':'carree','Cylinder':'ronde','Bale':'petite'}
o=['/* bale_model.inc - genere par tools/mkbale.py : bottes de foin (hayBales.fbx et Bale.fbx du joueur) */\n']
dims=[]; arrs=[]; lows=[]
for vi,(name,tris,mn,mx) in enumerate(res):
    s=SCALE[name]; cx=(mn[0]+mx[0])/2; cy=(mn[1]+mx[1])/2; z0=mn[2]
    rows=[]
    for t,vs in tris:
        for p,n,uv in vs:
            rows.append(((p[0]-cx)*s,(p[1]-cy)*s,(p[2]-z0)*s,n[0],n[1],n[2],uv[0],uv[1],t))
    hx=(mx[0]-mn[0])/2*s; hy=(mx[1]-mn[1])/2*s; H=(mx[2]-mn[2])*s
    low=None
    if name=='Cylinder':
        # v1.8.89 : cylindre simple a bords arrondis ; v1.8.92 : seulement en qualite optimisee
        import math as M
        high_rows=rows
        R=hx; seg=32; bev=0.07; rows=[]
        rings=[(0.0,R-bev),(bev*0.3,R-bev*0.3),(bev,R),(H-bev,R),(H-bev*0.3,R-bev*0.3),(H,R-bev)]
        def P(h,r,k): a=2*M.pi*k/seg; return (M.cos(a)*r,M.sin(a)*r,h)
        def N(h,r,k,i):
            a=2*M.pi*k/seg
            if i in (0,): nz=-0.7
            elif i==len(rings)-1: nz=0.7
            else: nz=0.0
            l=M.sqrt(1+nz*nz); return (M.cos(a)/l,M.sin(a)/l,nz/l)
        for i in range(len(rings)-1):
            (h0,r0),(h1,r1)=rings[i],rings[i+1]
            for k in range(seg):
                q=[(h0,r0,k,i),(h0,r0,k+1,i),(h1,r1,k+1,i+1),(h1,r1,k,i+1)]
                vs=[(P(h,r,kk),N(h,r,kk,ii),(kk/seg*4.0,h)) for h,r,kk,ii in q]
                for t in ((0,1,2),(0,2,3)):
                    for j in t:
                        p,n,uv=vs[j]; rows.append((p[0],p[1],p[2],n[0],n[1],n[2],uv[0],uv[1],0))
        for h,r,nz in ((0.0,R-bev,-1.0),(H,R-bev,1.0)):
            for k in range(seg):
                a0=P(h,r,k); a1=P(h,r,k+1); c=(0.0,0.0,h)
                tri=(c,a0,a1) if nz>0 else (c,a1,a0)
                for p in tri: rows.append((p[0],p[1],p[2],0.0,0.0,nz,p[0]*0.5,p[1]*0.5,0))
        low=rows; rows=high_rows
    dims.append((hx,hy,H,1 if name=='Cylinder' else 0,len(rows)//3))
    arrs.append(rows)
    lows.append(low if low is not None else rows)
o.append('#define BALE_VARIANTS %d\n#define BALE_TEXS 2\n'%len(res))
o.append('/* demi-largeur (e1), demi-longueur (e2), hauteur, ronde (rayon = demi-largeur), triangles */\n')
o.append('static const float k_bale_dim[BALE_VARIANTS][4] = {\n'+''.join('{%.4ff,%.4ff,%.4ff,%d.0f},\n'%d[:4] for d in dims)+'};\n')
o.append('static const int k_bale_tris[BALE_VARIANTS] = { %s };\n'%', '.join(str(d[4]) for d in dims))
for vi,rows in enumerate(arrs):
    o.append('/* %s : x (e1), y (e2), h (haut), nx, ny, nz, u, v, texture */\nstatic const float k_bale_m%d[%d][9] = {\n'%(NAMES[res[vi][0]],vi,len(rows)))
    o.extend('{%.4ff,%.4ff,%.4ff,%.3ff,%.3ff,%.3ff,%.4ff,%.4ff,%d},\n'%r for r in rows)
    o.append('};\n')
o.append('static const float (*const k_bale_m[BALE_VARIANTS])[9] = { %s };\n'%', '.join('k_bale_m%d'%i for i in range(len(arrs))))
# v1.8.92 : maillages simplifies (qualite optimisee)
for vi,rows in enumerate(lows):
    if rows is arrs[vi]: continue
    o.append('static const float k_bale_l%d[%d][9] = {\n'%(vi,len(rows)))
    o.extend('{%.4ff,%.4ff,%.4ff,%.3ff,%.3ff,%.3ff,%.4ff,%.4ff,%d},\n'%r for r in rows)
    o.append('};\n')
o.append('static const float (*const k_bale_low[BALE_VARIANTS])[9] = { %s };\n'%', '.join(('k_bale_l%d'%i) if lows[i] is not arrs[i] else ('k_bale_m%d'%i) for i in range(len(arrs))))
o.append('static const int k_bale_tris_low[BALE_VARIANTS] = { %s };\n'%', '.join(str(len(r)//3) for r in lows))
TF=[HB+'Textures/HayBales/hay wrapped.png',RB+'Texture/Bale.tga']
comps=[];avg=[]
TSZ=[128,512]
for ti,f in enumerate(TF):
    im=Image.open(f).convert('RGB').resize((TSZ[ti],TSZ[ti]),Image.LANCZOS)
    a=np.asarray(im)
    if f.endswith('Bale.tga'):
        al=np.asarray(Image.open(f).convert('RGBA').split()[-1].resize((TSZ[ti],TSZ[ti])))
        m=al>=128
        fill=np.median(a[m].reshape(-1,3),axis=0).astype(np.uint8)
        a=a.copy(); a[~m]=fill
    avg.append(tuple(int(x) for x in np.median(a.reshape(-1,3),axis=0)))
    co=zlib.compressobj(9,zlib.DEFLATED,-15); comps.append(co.compress(a.tobytes())+co.flush())
for k,c in enumerate(comps):
    o.append('static const unsigned char k_bale_tex_z%d[%d] = {\n'%(k,len(c)))
    for i in range(0,len(c),40): o.append(','.join(str(b) for b in c[i:i+40])+',\n')
    o.append('};\n')
o.append('static const unsigned char *const k_bale_tex_z[BALE_TEXS] = { %s };\n'%', '.join('k_bale_tex_z%d'%k for k in range(len(comps))))
o.append('static const int k_bale_tex_zlen[BALE_TEXS] = { %s };\n'%', '.join(str(len(c)) for c in comps))
o.append('static const int k_bale_tex_size[BALE_TEXS] = { %s };\n'%', '.join(str(x) for x in TSZ))
# couleur des icones (0xBBGGRR) : texture principale de chaque variante
main=[0,1]
o.append('static const unsigned long k_bale_var_rgb[BALE_VARIANTS] = { %s };\n'%', '.join('0x%02X%02X%02XUL'%(avg[m][2],avg[m][1],avg[m][0]) for m in main))
open('/home/claude/project-mxbikes/plugins/ragdoll/src/bale_model.inc','w').write(''.join(o))
print('dims',dims,file=sys.stderr)
print('tex bytes',[len(c) for c in comps],file=sys.stderr)
