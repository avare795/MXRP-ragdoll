import sys, zlib, math
sys.path.insert(0,'/tmp/claude-0/tools')
import fbx
from PIL import Image
SRC='/tmp/claude-0/tc/x'
top=fbx.parse(SRC+'/source/Scene_for_SketchFab.fbx')
T={t['name']:t for t in top}
g=[c for c in T['Objects']['ch'] if c['props'][0]==2025073740432][0]
V=fbx.find(g,'Vertices')[0]['props'][0]
idx=fbx.find(g,'PolygonVertexIndex')[0]['props'][0]
ln=fbx.find(g,'LayerElementNormal')[0]; N=fbx.find(ln,'Normals')[0]['props'][0]
lu=fbx.find(g,'LayerElementUV')[0]; UV=fbx.find(lu,'UV')[0]['props'][0]; UVI=fbx.find(lu,'UVIndex')[0]['props'][0]
P=[V[i*3:i*3+3] for i in range(len(V)//3)]
xs=[p[0] for p in P]; ys=[p[1] for p in P]; zs=[p[2] for p in P]
cx=(min(xs)+max(xs))/2; cy=(min(ys)+max(ys))/2; z0=min(zs); H=0.70; s=H/(max(zs)-z0)
def tp(p): return ((p[0]-cx)*s,(p[1]-cy)*s,(p[2]-z0)*s)
tris=[]; poly=[]
for k,i in enumerate(idx):
    last=i<0
    vi=~i if last else i
    n=N[k*3:k*3+3]; u=UVI[k]; uv=(UV[u*2], 1.0-UV[u*2+1])
    poly.append((tp(P[vi]), tuple(n), uv))
    if last:
        for j in range(1,len(poly)-1): tris.append((poly[0],poly[j],poly[j+1]))
        poly=[]
# dimensions
pts=[tp(p) for p in P]
half=max(max(abs(p[0]),abs(p[1])) for p in pts if p[2]<0.06)
def rad(h0,h1):
    r=[math.hypot(p[0],p[1]) for p in pts if h0<=p[2]<=h1]; return max(r) if r else 0
plate=max(p[2] for p in pts if max(abs(p[0]),abs(p[1]))>half*0.9)
print('tris',len(tris),'half',half,'plate h',plate,'r just above plate',rad(plate+0.005,plate+0.08),'r top',rad(0.6,0.7), file=sys.stderr)
for h in [0.05,0.1,0.2,0.3,0.4,0.5,0.6,0.65,0.69]: print(h, rad(h-0.02,h+0.02), file=sys.stderr)
TEX=[('LP_blinn2_BaseColor.png',None),('text2_Base_color.png','text2_Mixed_AO.png'),('text3_Base_color.png','text3_Mixed_AO.png'),('text4_Base_color.png','text4_Mixed_AO.png'),('text5_Base_color.png','text5_Mixed_AO.png')]
comps=[];avg=[]
import numpy as np
for base,ao in TEX:
    im=Image.open(SRC+'/textures/'+base).convert('RGB').resize((512,512),Image.LANCZOS)
    a_=np.asarray(im).astype(np.float32)
    if ao:
        m=np.asarray(Image.open(SRC+'/textures/'+ao).convert('L').resize((512,512),Image.LANCZOS)).astype(np.float32)/255.0
        a_=a_*(0.35+0.65*m[:,:,None])
    a_=np.clip(a_,0,255).astype(np.uint8)
    # couleur moyenne du corps (moitie haute de la texture : les bandes et le corps)
    top=a_[:256,:,:].reshape(-1,3); avg.append(tuple(int(x) for x in np.median(top,axis=0)))
    co=zlib.compressobj(9,zlib.DEFLATED,-15); comps.append(co.compress(a_.tobytes())+co.flush())
o=[]
o.append('/* cone_model.inc - genere par mkcone.py : modele 3D du cone (Sketchfab, fourni par le joueur),\n   %d triangles, hauteur %.2f m ; texture 512x512 RGB compressee (deflate brut) */\n'%(len(tris),H))
o.append('#define CONE_MODEL_TRIS %d\n#define CONE_MODEL_HALF %.4ff\n#define CONE_MODEL_PLATE %.4ff\n#define CONE_TEX_W 512\n#define CONE_TEX_H 512\n#define CONE_VARIANTS %d\n'%(len(tris),half,plate,len(comps)))
o.append('/* par sommet : x, y (sol), h (hauteur), nx, ny, nz (z = haut), u, v */\nstatic const float k_cone_model[CONE_MODEL_TRIS * 3][8] = {\n')
for t in tris:
    for (p,n,uv) in t:
        o.append('{%.4ff,%.4ff,%.4ff,%.3ff,%.3ff,%.3ff,%.4ff,%.4ff},\n'%(p[0],p[1],p[2],n[0],n[1],n[2],uv[0],uv[1]))
o.append('};\n')
for k,c in enumerate(comps):
    o.append('static const unsigned char k_cone_tex_z%d[%d] = {\n'%(k,len(c)))
    for i in range(0,len(c),32): o.append(','.join(str(b) for b in c[i:i+32])+',\n')
    o.append('};\n')
o.append('static const unsigned char *const k_cone_tex_z[CONE_VARIANTS] = { %s };\n'%', '.join('k_cone_tex_z%d'%k for k in range(len(comps))))
o.append('static const int k_cone_tex_zlen[CONE_VARIANTS] = { %s };\n'%', '.join(str(len(c)) for c in comps))
o.append('/* couleur du corps (0xBBGGRR), pour les icones du menu */\nstatic const unsigned long k_cone_var_rgb[CONE_VARIANTS] = { %s };\n'%', '.join('0x%02X%02X%02XUL'%(b,g,r) for (r,g,b) in avg))
open(sys.argv[1],'w').write(''.join(o))
print('tex',[len(c) for c in comps],avg,file=sys.stderr)
