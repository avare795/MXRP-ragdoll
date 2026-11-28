"""mkramp.py - v1.8.119 : rampes FMX (pack PROCworks FMX Ramp, fichiers du joueur) -> src/ramp_model.inc

Usage : python3 mkramp.py <dossier du pack>
3 formes (cadre + tablier), 4 couleurs (une texture par couleur, mêmes UV).
Repère du modèle : x = largeur (centrée), y = longueur (0 = bas de la rampe, vers le haut du saut),
h = hauteur (0 = sol). Profil du dessus du tablier (pour rouler et marcher dessus) : RAMP_PROF points.
"""
import sys, zlib, math
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import fbx
import numpy as np
from PIL import Image

D = sys.argv[1].rstrip('/') + '/'   # pack PROCworks
D2 = sys.argv[2].rstrip('/') + '/' if len(sys.argv) > 2 else None  # pack « FMX Ramps » (FMX Landing)
OUT = __file__.rsplit('/', 2)[0] + '/src/ramp_model.inc'
FBX = D + 'PROCworks FMX Wood ramps.fbx'
TEX = ['FMX Texture.tga', 'FMX Metal Texture.tga', 'FMX Metal BLUE Texture.tga', 'FMX Metal Texture RED.tga']
TEXP = [D + t for t in TEX]
COLORS = ['bois', 'noir', 'bleu', 'rouge']
# forme : (nom, fichier, cadre, tablier, repere, texture fixe (-1 : couleur choisie), haut du cote du pilote)
SHAPES = [('Kicker', FBX, 'TRKCONC_FSX- SupaKicker.001', 'TRKCONC_RED-FMX- SupaKicker.003', 'yup', -1, False),
          ('F-Head', FBX, 'TRKCONC_FSX - F-Head.001', 'TRKCONC_FSX - F-Head.002', 'yup', -1, False),
          ('Grand saut', FBX, 'TRKCONC_DistanceRecord.001', 'TRKCONC_DistanceRecord.003', 'yup', -1, False)]
if D2:
    # v1.8.121 : réception (le haut, côté rampe, près du pilote ; la pente descend devant lui)
    SHAPES.append(('Reception', D2 + 'FMX Landing/fmxLanding.fbx', 'WLLCONC_fmxLanding', 'TRKASPH_fmxLanding', 'zup', len(TEX), True))
    TEXP.append(D2 + 'FMX Landing/LandingT.tga')
    # v1.8.123 : pack thefluffygecko (dans « FMX Ramps ») : chaque forme a sa texture ; (cadre None : tablier seul)
    G = D2 + 'thefluffygecko fmx ramps/'
    for nm, frame, deck, tex in [('Comp 8 ft', 'WLLCONC_Comp', 'TRKCONC_Comp', '8ftComp'),
                                 ('Super kicker', 'WLLCONC_SuperK', 'TRKCONC_SuperK', '8Superk'),
                                 ('Saut 120 ft', 'WLLCONC_LongJump', 'TRKCONC_LongJump', '120ftLongJump'),
                                 ('Quarter pipe', None, 'TRKSOIL_QuarterPipe', 'QuarterPipe'),
                                 ("Piste d'elan", None, 'TRKCONC_Runner', 'RunnerT')]:
        SHAPES.append((nm, G + 'ramps.fbx', frame, deck, 'zup', len(TEXP), False))
        TEXP.append(G + 'textures/' + tex + '.tga')
PROF = 96
TSZ = 512


def walk(n):
    yield n
    for c in n.get('ch', []):
        yield from walk(c)


_cache = {}
def load(path):
    if path not in _cache:
        top = fbx.parse(path)
        T = {t['name']: t for t in top}
        objs = {c['props'][0]: c for c in T['Objects']['ch']}
        conns = [c['props'] for c in T['Connections']['ch']]
        models = {str(o['props'][1]).split('\x00')[0]: o for o in objs.values() if o['name'] == 'Model'}
        _cache[path] = (objs, conns, models)
    return _cache[path]


def props(m):
    r = {}
    for n in walk(m):
        if n['name'] == 'P' and str(n['props'][0]).startswith('Lcl'):
            r[n['props'][0]] = n['props'][4:7]
    return r


def tris_of(path, name, frame):
    """triangles ((x, y, h), normale, uv) en mètres ; x largeur, y longueur, h hauteur (repère direct)"""
    objs, conns, models = load(path)
    m = models[name]
    pr = props(m)
    s = pr.get('Lcl Scaling', (100.0, 100.0, 100.0))
    t = pr.get('Lcl Translation', (0.0, 0.0, 0.0))
    g = None
    for c in conns:
        if c[0] == 'OO' and c[2] == m['props'][0] and c[1] in objs and objs[c[1]]['name'] == 'Geometry':
            g = objs[c[1]]
    V = fbx.find(g, 'Vertices')[0]['props'][0]
    idx = fbx.find(g, 'PolygonVertexIndex')[0]['props'][0]
    ln = fbx.find(g, 'LayerElementNormal')[0]
    N = fbx.find(ln, 'Normals')[0]['props'][0]
    nmap = fbx.find(ln, 'MappingInformationType')[0]['props'][0]
    NI = fbx.find(ln, 'NormalsIndex')
    NI = NI[0]['props'][0] if NI else None
    lu = fbx.find(g, 'LayerElementUV')[0]
    UV = fbx.find(lu, 'UV')[0]['props'][0]
    UVI = fbx.find(lu, 'UVIndex')[0]['props'][0]
    out, poly = [], []
    for k, i in enumerate(idx):
        last = i < 0
        vi = ~i if last else i
        if nmap in ('ByPolygonVertex', b'ByPolygonVertex'):
            q = NI[k] if NI is not None else k
            n = N[q * 3:q * 3 + 3]
        else:
            n = N[vi * 3:vi * 3 + 3]
        u = UVI[k]
        p = V[vi * 3:vi * 3 + 3]
        if frame == 'yup':
            # PROCworks : Y en haut, Z la longueur ; (le cadre et le tablier ne different que par la translation verticale)
            pos = (p[0] * s[0] / 100.0, p[1] * s[1] / 100.0 + t[1] / 100.0, p[2] * s[2] / 100.0)
            nn = (n[0] / s[0], n[1] / s[1], n[2] / s[2])
            P = (-pos[0], pos[2], pos[1]); NN = (-nn[0], nn[2], nn[1])
        else:
            # Z en haut, X la longueur, Y la largeur (mètres) : largeur = -Y, longueur = X (repère direct)
            pos = (p[0] * s[0] / 100.0, p[1] * s[1] / 100.0, p[2] * s[2] / 100.0)
            P = (-pos[1], pos[0], pos[2]); NN = (-n[1], n[0], n[2])
        l = math.sqrt(sum(x * x for x in NN)) or 1.0
        poly.append((P, tuple(x / l for x in NN), (UV[u * 2], 1.0 - UV[u * 2 + 1])))
        if last:
            for j in range(1, len(poly) - 1):
                out.append((poly[0], poly[j], poly[j + 1]))
            poly = []
    return out


res = []
for nm, path, frame, deck, rep, ftex, high_first in SHAPES:
    ft, dt = (tris_of(path, frame, rep) if frame else []), tris_of(path, deck, rep)
    allp = [v[0] for tr in ft + dt for v in tr]
    xs = [p[0] for p in allp]; ys = [p[1] for p in allp]; hs = [p[2] for p in allp]
    cx = (min(xs) + max(xs)) / 2; y0 = min(ys); h0 = min(hs)
    if 'thefluffygecko' in path:
        h0 = 0.0  # (v1.8.123 : sol du modèle à z = 0 ; le bas du cadre est enterré)
        if frame is None and max(hs) - min(hs) < 0.3:
            h0 = -0.05  # (v1.8.124 : piste d'élan : 5 cm au-dessus du sol, sinon cachée sous la terre)
    # sens : le haut du saut vers +y
    dp = [v[0] for tr in dt for v in tr]
    lo = [p for p in dp if p[1] < (min(ys) + max(ys)) / 2]
    hi = [p for p in dp if p[1] >= (min(ys) + max(ys)) / 2]
    flip = (max(p[2] for p in lo) > max(p[2] for p in hi) + 1e-3) != high_first
    L = max(ys) - min(ys)

    def tf(v):
        (x, y, h), n, uv = v
        y = y - y0
        if flip:
            y = L - y
            x = -x
            n = (-n[0], -n[1], n[2])
        return ((x - (cx if not flip else -cx), y, h - h0), n, uv)
    ft = [tuple(tf(v) for v in tr) for tr in ft]
    dt = [tuple(tf(v) for v in tr) for tr in dt]
    # profil du dessus du tablier : hauteur la plus haute à chaque abscisse y (vertex du tablier près du milieu)
    dp = np.array([v[0] for tr in dt for v in tr])
    W = max(abs(p[0]) for tr in ft + dt for v in tr for p in [v[0]])
    prof = []
    for k in range(PROF + 1):
        y = L * k / PROF
        best = None
        for tr in dt:
            ps = [v[0] for v in tr]
            ylo = min(p[1] for p in ps); yhi = max(p[1] for p in ps)
            if not (ylo - 1e-4 <= y <= yhi + 1e-4):
                continue
            # hauteur du triangle sur la ligne y (bords coupés) : max des intersections
            for a in range(3):
                p, q = ps[a], ps[(a + 1) % 3]
                if (p[1] - y) * (q[1] - y) <= 0 and abs(q[1] - p[1]) > 1e-6:
                    t = (y - p[1]) / (q[1] - p[1])
                    h = p[2] + t * (q[2] - p[2])
                    best = h if best is None or h > best else best
                elif abs(p[1] - y) < 1e-4:
                    best = p[2] if best is None or p[2] > best else best
        if best is None and k > 0:
            # (au bout, le tablier s'arrête un peu avant le cadre : on garde sa dernière hauteur)
            ymax = max(v[0][1] for tr in dt for v in tr)
            best = max(v[0][2] for tr in dt for v in tr if v[0][1] > ymax - 0.05)
        prof.append(max(0.0, best if best is not None else 0.0))
    # bout bas : au sol (le tablier commence au ras du sol)
    # v1.8.130 : marche à l'entrée (Kicker : 10 cm) : montée douce sur les 60 premiers cm (la roue tapait dedans)
    if prof[0] > 0.02:
        kk = max(1, int(round(0.6 / L * PROF)))
        for k in range(kk):
            prof[k] = prof[kk] * k / kk
    res.append((nm, ft + dt, W, L, max(hs) - h0, prof, ftex))
    print(nm, 'tris', len(ft) + len(dt), 'largeur %.2f longueur %.2f hauteur %.2f' % (2 * W, L, max(hs) - h0),
          'profil', ['%.2f' % p for p in prof], file=sys.stderr)

o = ['/* ramp_model.inc - genere par tools/mkramp.py : rampes FMX (pack PROCworks FMX Ramp, fichiers du joueur) */\n']
o.append('#define RAMP_SHAPES %d\n#define RAMP_COLORS %d\n#define RAMP_TEXS %d\n#define RAMP_PROF %d\n' % (len(res), len(TEX), len(TEXP), PROF))
o.append('/* texture fixe de la forme (-1 : la couleur choisie) */\nstatic const int k_ramp_ftex[RAMP_SHAPES] = { %s };\n' % ', '.join(str(r[6]) for r in res))
o.append('/* demi-largeur, longueur, hauteur, triangles */\n')
o.append('static const float k_ramp_dim[RAMP_SHAPES][4] = {\n' +
         ''.join('{%.4ff,%.4ff,%.4ff,%d.0f},\n' % (r[2], r[3], r[4], len(r[1])) for r in res) + '};\n')
o.append('static const char *const k_ramp_name[RAMP_SHAPES] = { %s };\n' % ', '.join('"%s"' % r[0] for r in res))
o.append('/* dessus du tablier : hauteur a y = longueur * k / RAMP_PROF */\n')
o.append('static const float k_ramp_prof[RAMP_SHAPES][RAMP_PROF + 1] = {\n' +
         ''.join('{%s},\n' % ','.join('%.4ff' % p for p in r[5]) for r in res) + '};\n')
for i, r in enumerate(res):
    rows = [(*v[0], *v[1], *v[2]) for tr in r[1] for v in tr]
    o.append('/* %s : x, y, h, nx, ny, nh, u, v, texture */\nstatic const float k_ramp_m%d[%d][9] = {\n' % (r[0], i, len(rows)))
    o.extend('{%.4ff,%.4ff,%.4ff,%.3ff,%.3ff,%.3ff,%.4ff,%.4ff,0},\n' % row for row in rows)
    o.append('};\n')
o.append('static const float (*const k_ramp_m[RAMP_SHAPES])[9] = { %s };\n' % ', '.join('k_ramp_m%d' % i for i in range(len(res))))
comps, avg = [], []
for f in TEXP:
    im = Image.open(f).convert('RGB').resize((TSZ, TSZ), Image.LANCZOS)
    a = np.asarray(im)
    avg.append(tuple(int(x) for x in np.median(a[:TSZ // 2].reshape(-1, 3), axis=0)))
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    comps.append(co.compress(a.tobytes()) + co.flush())
for k, c in enumerate(comps):
    o.append('static const unsigned char k_ramp_tex_z%d[%d] = {\n' % (k, len(c)))
    for i in range(0, len(c), 40):
        o.append(','.join(str(b) for b in c[i:i + 40]) + ',\n')
    o.append('};\n')
o.append('static const unsigned char *const k_ramp_tex_z[RAMP_TEXS] = { %s };\n' % ', '.join('k_ramp_tex_z%d' % k for k in range(len(comps))))
o.append('static const int k_ramp_tex_zlen[RAMP_TEXS] = { %s };\n' % ', '.join(str(len(c)) for c in comps))
o.append('#define RAMP_TEX_SIZE %d\n' % TSZ)
o.append('static const char *const k_ramp_color[RAMP_COLORS] = { %s };\n' % ', '.join('"%s"' % c for c in COLORS))
o.append('static const unsigned long k_ramp_rgb[RAMP_COLORS] = { 0x4A7DA8UL, 0x202020UL, 0xC04040UL, 0x2020D0UL };\n')
open(OUT, 'w').write(''.join(o))
print('tex bytes', [len(c) for c in comps], file=sys.stderr)
