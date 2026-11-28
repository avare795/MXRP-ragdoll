"""mkfurn.py - v1.8.178 : mobilier (table pliante, chaises de camping) depuis les .glb du joueur
-> src/furn_model.inc (même format que les bottes : x (e1), y (e2), h (haut), normale, u, v, texture)

Sources (fichiers glb envoyés par le joueur) :
  table  : "Folding event table 1800 x 760 mm (6 ft)", Innerscene Corp, CC0
  chaise : "Camping Chair", lpfeffer (sketchfab), CC-BY-4.0
  chaise : "Camping Chair (FREE!!!)", SyntaX (sketchfab), CC-BY-4.0
usage : python3 tools/mkfurn.py <dossier des glb>
"""
import sys, os, io, zlib
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(__file__))
import glb
import fast_simplification as fs

SRC = sys.argv[1] if len(sys.argv) > 1 else '/mnt/user-data/uploads'
OUT = os.path.join(os.path.dirname(__file__), '..', 'src', 'furn_model.inc')

# v1.8.181 : tailles réglées sur le pilote du jeu (cuisse 0,365 m, tibia 0,389 m, cheville 0,14 m : genou à
# ~0,53 m, hanches à ~0,87 m) : assise des chaises à 0,44 m, table à 0,78 m (avant : x 1,25 au jugé, trop grand)
GROW = 1.0
TABLE_H = 0.78
SEAT_H = 0.44
CELL = 0.0015  # v1.8.187 : regroupement des sommets (m)
ITEMS = [
    # nom, fichier, hauteur voulue (m) ou None, triangles max, taille texture
    ('table', 'ae1f75bf-folding-event-table-1800-x-760-mm-6-ft.glb', None, 7000, 128),
    ('chaise', 'camping_chair.glb', None, 5000, 512),
    # v1.8.188 : chaise bois retirée (modèle trop lourd : 168 000 triangles, texture abîmée en le simplifiant)
    # v1.8.189 : chaise rouge (fbx du joueur, 23 000 triangles, en cm ; filet des côtés en gris uni)
    ('chaise3', '2/cgt_fmcg_camping_089.fbx', None, 6000, 512),
]


def tri_normals(P, F):
    n = np.cross(P[F[:, 1]] - P[F[:, 0]], P[F[:, 2]] - P[F[:, 0]])
    return n


def vert_normals(P, F):
    fn = tri_normals(P, F)
    vn = np.zeros_like(P)
    for k in range(3):
        np.add.at(vn, F[:, k], fn)
    return vn / np.maximum(np.linalg.norm(vn, axis=1, keepdims=True), 1e-12)


def islands(F, nv):
    """îles d'UV : triangles reliés par des sommets communs (le glb coupe les sommets aux coutures)"""
    par = np.arange(nv)
    def find(a):
        while par[a] != a:
            par[a] = par[par[a]]
            a = par[a]
        return a
    for a, b, c in F:
        ra, rb, rc = find(a), find(b), find(c)
        par[rb] = ra
        par[find(rc)] = ra
    return np.array([find(k) for k in range(nv)])


def decimate(P, F, UV, target, N0=None, cell_size=None, solid=False):
    """v1.8.187 : simplification île par île : sommets regroupés par cases de 4 mm SANS mélanger les îles
    d'UV (les coutures restent nettes : avant, des triangles à cheval sur deux îles étiraient la texture),
    bords des îles gardés ; UV et normale de chaque sommet = moyenne de ceux d'origine qu'il remplace"""
    isl = np.zeros(len(P), np.int64) if solid else islands(F, len(P))  # solid : pièce d'une couleur (pieds)
    cell = np.floor(P / (cell_size or CELL)).astype(np.int64)
    key = np.c_[cell, isl]
    _, wid, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
    inv = inv.ravel()
    WP = np.zeros((len(wid), 3))
    cnt = np.zeros(len(wid))
    np.add.at(WP, inv, P)
    np.add.at(cnt, inv, 1)
    WP /= cnt[:, None]
    WF = inv[F]
    WF = WF[(WF[:, 0] != WF[:, 1]) & (WF[:, 1] != WF[:, 2]) & (WF[:, 0] != WF[:, 2])]
    if target >= len(WF):
        P2, F2 = WP, WF
        mapping = np.arange(len(WP))
    else:
        P2, F2, col = fs.simplify(WP, WF, target_count=target, return_collapses=True, preserve_border=not solid)
        _, _, mapping = fs.replay_simplification(WP.astype(np.float32), WF.astype(np.int32), col)
    uv2 = np.zeros((len(P2), 2))
    n2 = np.zeros((len(P2), 3))
    c2 = np.zeros(len(P2))
    nv = mapping[inv]
    ok = (nv >= 0) & (nv < len(P2))
    np.add.at(uv2, nv[ok], UV[ok])
    np.add.at(c2, nv[ok], 1)
    if N0 is not None:
        np.add.at(n2, nv[ok], N0[ok])
    uv2 /= np.maximum(c2, 1)[:, None]
    F2 = np.asarray(F2)
    if N0 is not None:
        n2 /= np.maximum(np.linalg.norm(n2, axis=1, keepdims=True), 1e-12)
    else:
        n2 = vert_normals(P2, F2)
    # normale de la face là où la moyenne s'en écarte trop (pièces fines)
    fn = tri_normals(P2, F2)
    fn = fn / np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-12)
    N3 = n2[F2.ravel()].reshape(-1, 3, 3)
    dot = (N3 * fn[:, None, :]).sum(2)
    N3[dot < 0.3] = np.repeat(fn[:, None, :], 3, axis=1)[dot < 0.3]
    P3 = P2[F2.ravel()]
    UV3 = uv2[F2.ravel()]
    F3 = np.arange(len(P3)).reshape(-1, 3)
    return P3, F3, UV3, N3.reshape(-1, 3)


FBX_TEX = os.environ.get('FURN_FBX_TEX', '/tmp/chair3/tex/textures')


def load_fbx(path):
    """fbx par assimp (sommets déjà dans le repère du modèle, y haut) -> même forme que glb.load"""
    import assimp_py as a
    sc = a.import_file(path, a.Process_Triangulate | a.Process_PreTransformVertices | a.Process_GenNormals |
                       a.Process_FlipUVs)
    prims = []
    for m in sc.meshes:
        P = np.array(m.vertices, float).reshape(-1, 3) * 0.01  # cm -> m
        N = np.array(m.normals, float).reshape(-1, 3) if m.normals else None
        UV = np.array(m.texcoords[0], float).reshape(len(P), -1)[:, :2]
        F = np.array(m.indices, np.int64).reshape(-1, 3)
        prims.append(dict(pos=P, nor=N, uv=UV, idx=F, mat=m.material_index, name=m.name))
    return prims


def build(name, fname, height, tmax, tsz):
    if fname.endswith('.fbx'):
        prims = load_fbx(os.path.join(SRC, fname))
        for p in prims:
            if p['mat'] == 1:
                p['uv'] = np.tile([0.6, 0.5], (len(p['uv']), 1))  # filet : gris uni de la texture
            elif 'leg' in p.get('name', ''):
                p['uv'] = np.tile([0.164, 0.642], (len(p['uv']), 1))  # pieds : noir uni de la texture
        texfile = os.path.join(FBX_TEX, 'cgt_fmcg_camping_089.mat_chair.base_color.srgb.1001.png')
        j = {'materials': [{'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}}}]}
        img = lambda i: open(texfile, 'rb').read()
    else:
        j, prims, img = glb.load(os.path.join(SRC, fname))
    rows = []  # (P[3], N[3], uv[2])
    allp = np.vstack([p['pos'] for p in prims])
    s = 1.0
    if height:
        s = height / (allp[:, 1].max() - allp[:, 1].min())
    s *= GROW
    total = sum(len(p['idx']) for p in prims)
    tex = Image.open(io.BytesIO(img(j['materials'][0]['pbrMetallicRoughness']['baseColorTexture']['index']))).convert('RGB')
    if name == 'table':
        # atlas : granit (projection de dessus) en haut, gris et noir unis en bas
        g = np.asarray(tex.resize((tsz, tsz * 3 // 4), Image.LANCZOS))
        atlas = np.zeros((tsz, tsz, 3), np.uint8)
        atlas[:tsz * 3 // 4] = g
        cols = []
        for mi in (1, 2):
            im = Image.open(io.BytesIO(img(j['materials'][mi]['pbrMetallicRoughness']['baseColorTexture']['index']))).convert('RGB')
            cols.append(np.asarray(im).reshape(-1, 3).mean(0).astype(np.uint8))
        atlas[tsz * 3 // 4:, :tsz // 2] = cols[0]
        atlas[tsz * 3 // 4:, tsz // 2:] = cols[1]
        tex_out = atlas
    else:
        tex_out = np.asarray(tex.resize((tsz, tsz), Image.LANCZOS))
    mn = allp.min(0) * s
    mx = allp.max(0) * s
    for p in prims:
        P = p['pos'] * s
        F = p['idx']
        UV = p['uv']
        N = p['nor']
        if total > tmax:
            P, F, UV, N = decimate(P, F, UV, max(64, int(len(F) * tmax / total)), N,
                                   (0.002 if 'leg' in p.get('name', '') else 0.008) if name == 'chaise3' else None,
                                   name == 'chaise3' and 'leg' in p.get('name', ''))
        elif N is None:
            N = vert_normals(P, F)
        for t in F:
            for k in t:
                x, y, z = P[k]
                if name == 'table':
                    if p['mat'] == 0:
                        u = (x - mn[0]) / (mx[0] - mn[0])
                        v = (z - mn[2]) / (mx[2] - mn[2]) * 0.75
                    elif p['mat'] == 1:
                        u, v = 0.25, 0.875
                    else:
                        u, v = 0.75, 0.875
                else:
                    u, v = UV[k]
                rows.append(((x, -z, y), (N[k][0], -N[k][2], N[k][1]), (u, v)))  # glTF y haut -> z haut (main droite)
    # centré, pieds à 0
    ps = np.array([r[0] for r in rows])
    lo = ps.min(0)
    hi = ps.max(0)
    c = (lo + hi) / 2
    out = []
    for (pp, nn, uv) in rows:
        nl = np.linalg.norm(nn) or 1.0
        out.append((pp[0] - c[0], pp[1] - c[1], pp[2] - lo[2], nn[0] / nl, nn[1] / nl, nn[2] / nl, uv[0], uv[1]))
    fixed = out
    dim = ((hi[0] - lo[0]) / 2, (hi[1] - lo[1]) / 2, hi[2] - lo[2])
    print(name, 'tris', len(fixed) // 3, 'dim', [round(d, 3) for d in dim], file=sys.stderr)
    avg = tex_out.reshape(-1, 3).mean(0).astype(int)
    seat = (0.0, 1.0, 0.0, 0.0)
    P = np.array([r[:3] for r in fixed])
    if name == 'table':
        k = TABLE_H / dim[2]
    else:
        # dossier : là où la chaise est la plus haute ; l'avant est à l'opposé
        H = dim[2]
        top = P[P[:, 2] > 0.8 * H]
        fr = -top[:, :2].mean(0)
        fr = fr / (np.linalg.norm(fr) or 1.0)
        c = fr * 0.05
        near = P[(np.linalg.norm(P[:, :2] - c, axis=1) < 0.12) & (P[:, 2] > 0.25 * H) & (P[:, 2] < 0.62 * H)]
        sh = float(np.percentile(near[:, 2], 80)) if len(near) else 0.45 * H
        k = SEAT_H / sh
        if name == 'chaise3':
            k = 1.0 / H  # v1.8.189 : toile très creuse (assise mesurée basse) : 1 m de haut comme la chaise verte
        seat = (float(fr[0]), float(fr[1]), float(sh * k), 0.05 * k)  # (v1.8.186 : bassin contre le dossier : annulé, c'était bien)
        print(name, 'assise mesuree', round(sh, 3), 'avant', fr.round(2), file=sys.stderr)
    fixed = [(r[0] * k, r[1] * k, r[2] * k) + tuple(r[3:]) for r in fixed]
    dim = (dim[0] * k, dim[1] * k, dim[2] * k)
    print(name, 'echelle', round(k, 3), 'dim', [round(d, 3) for d in dim], file=sys.stderr)
    return fixed, dim, tex_out, avg, seat


def main():
    o = ['/* furn_model.inc - genere par tools/mkfurn.py : mobilier (table pliante : Innerscene Corp, CC0 ;\n'
         '   chaise de camping : lpfeffer sur Sketchfab, CC-BY-4.0 ; chaise rouge : cgt_fmcg_camping_089, fichier du joueur) */\n']
    res = [build(*it) for it in ITEMS]
    o.append('#define FURN_VARIANTS %d\n#define FURN_TEXS %d\n' % (len(res), len(res)))
    o.append('/* demi-largeur (e1), demi-longueur (e2), hauteur, ronde (0) */\n')
    o.append('static const float k_furn_dim[FURN_VARIANTS][4] = {\n' + ''.join(
        '{%.4ff,%.4ff,%.4ff,0.0f},\n' % r[1] for r in res) + '};\n')
    o.append('static const int k_furn_tris[FURN_VARIANTS] = { %s };\n' % ', '.join(str(len(r[0]) // 3) for r in res))
    o.append('/* chaises : avant (e1, e2), hauteur de l\'assise, avance du centre de l\'assise (0 : pas une chaise) */\n')
    o.append('static const float k_furn_seat[FURN_VARIANTS][4] = {\n' + ''.join(
        '{%.4ff,%.4ff,%.4ff,%.4ff},\n' % r[4] for r in res) + '};\n')
    for i, r in enumerate(res):
        o.append('static const float k_furn_m%d[%d][9] = {\n' % (i, len(r[0])))
        o.extend('{%.4ff,%.4ff,%.4ff,%.3ff,%.3ff,%.3ff,%.4ff,%.4ff,%d},\n' % (row + (i,)) for row in r[0])
        o.append('};\n')
    o.append('static const float (*const k_furn_m[FURN_VARIANTS])[9] = { %s };\n' % ', '.join(
        'k_furn_m%d' % i for i in range(len(res))))
    for i, r in enumerate(res):
        co = zlib.compressobj(9, zlib.DEFLATED, -15)
        z = co.compress(r[2].tobytes()) + co.flush()
        o.append('static const unsigned char k_furn_tex_z%d[%d] = {\n' % (i, len(z)))
        for k in range(0, len(z), 40):
            o.append(','.join(str(b) for b in z[k:k + 40]) + ',\n')
        o.append('};\n')
        print('tex', i, len(z), file=sys.stderr)
    o.append('static const unsigned char *const k_furn_tex_z[FURN_TEXS] = { %s };\n' % ', '.join(
        'k_furn_tex_z%d' % i for i in range(len(res))))
    o.append('static const int k_furn_tex_zlen[FURN_TEXS] = { %s };\n' % ', '.join(
        'sizeof(k_furn_tex_z%d)' % i for i in range(len(res))))
    o.append('static const int k_furn_tex_size[FURN_TEXS] = { %s };\n' % ', '.join(str(r[2].shape[0]) for r in res))
    o.append('static const unsigned long k_furn_rgb[FURN_VARIANTS] = { %s };\n' % ', '.join(
        '0x%02X%02X%02XUL' % (r[3][2], r[3][1], r[3][0]) for r in res))
    open(OUT, 'w').write(''.join(o))


if __name__ == '__main__':
    main()
