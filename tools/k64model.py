#!/usr/bin/env python3
"""k64model — glTF 2.0 (.glb) skinned or static model -> .k64m blob for the kgfx microcode.

Called by `k64bake.py model in.glb out.k64m`. Pipeline:

  1. load      all mesh primitives, merged into one mesh in skin (bind) space; a model
               without a skin is static: every primitive goes into model space through its
               node's world transform and the whole mesh hangs off one identity joint (one
               MTX for the model, one pose, no animation)
  2. atlas     every material into one small CI texture (body texture scaled, flat
               colours painted as blocks), UVs remapped; vertex colours white
  3. decimate  quadric edge collapse down to --tris (seams, borders kept; bone boundaries last)
  4. skin      top-2 joints per vertex, weight quantized to quarters -> "virtual bones"
               (a, b, w): the CPU blends w*skin[a] + (1-w)*skin[b] per frame, which is
               exact linear-blend skinning for that weight
  5. chunk     triangles into groups of <= 32 vertex cache slots; inside a chunk the vertices
               are grouped by virtual bone: MTX + LIGHT + VTX per group, then TRI/TRI2
               (vertices are transformed to clip space at VTX time, so a triangle can mix
               slots loaded under different matrices)
  6. animate   every animation sampled at --fps: local rotation (quaternion, Q15) and
               translation per joint and frame
  7. write     big-endian blob, layout in include/k64/model.h

numpy + PIL only.
"""
import json, struct, sys, heapq, math
from collections import Counter
import numpy as np

# ------------------------------------------------------------------------------------------
# glTF loading

CTYPE = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
NCOMP = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


class Gltf:
    def __init__(self, path):
        data = open(path, 'rb').read()
        magic, ver, length = struct.unpack_from('<4sII', data, 0)
        if magic != b'glTF' or ver != 2:
            sys.exit('k64model: %s is not a glTF 2.0 binary' % path)
        off, self.bin = 12, b''
        while off < length:
            clen, ctype = struct.unpack_from('<II', data, off)
            chunk = data[off + 8: off + 8 + clen]
            if ctype == 0x4E4F534A:
                self.j = json.loads(chunk)
            elif ctype == 0x004E4942:
                self.bin = chunk
            off += 8 + clen

    def view(self, i):
        bv = self.j['bufferViews'][i]
        o = bv.get('byteOffset', 0)
        return self.bin[o: o + bv['byteLength']], bv.get('byteStride', 0)

    def acc(self, i):
        a = self.j['accessors'][i]
        dt, nc = CTYPE[a['componentType']], NCOMP[a['type']]
        n = a['count']
        buf, stride = self.view(a['bufferView'])
        o = a.get('byteOffset', 0)
        isz = np.dtype(dt).itemsize
        if stride and stride != isz * nc:
            raw = np.frombuffer(buf, np.uint8)[o: o + stride * (n - 1) + isz * nc]
            arr = np.lib.stride_tricks.as_strided(raw, (n, isz * nc), (stride, 1)).copy()
            arr = arr.view(dt).reshape(n, nc)
        else:
            arr = np.frombuffer(buf, dt, n * nc, o).reshape(n, nc)
        arr = arr.astype(np.float64) if dt == np.float32 else arr.copy()
        if a.get('normalized') and dt != np.float32:
            arr = arr.astype(np.float64) / np.iinfo(dt).max
        return arr if nc > 1 else arr[:, 0]

    def image(self, i):
        from PIL import Image
        import io
        buf, _ = self.view(self.j['images'][i]['bufferView'])
        return Image.open(io.BytesIO(buf))


def node_local(n):
    if 'matrix' in n:
        return np.array(n['matrix'], np.float64).reshape(4, 4).T
    t = np.array(n.get('translation', [0, 0, 0]), np.float64)
    r = np.array(n.get('rotation', [0, 0, 0, 1]), np.float64)
    s = np.array(n.get('scale', [1, 1, 1]), np.float64)
    return trs_matrix(t, r, s)


def quat_matrix(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def trs_matrix(t, r, s):
    m = np.eye(4)
    m[:3, :3] = quat_matrix(r) * s[None, :]
    m[:3, 3] = t
    return m


def srgb_to_byte(c):
    c = max(0.0, min(1.0, c))
    v = 12.92 * c if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055
    return int(round(v * 255))


class Mesh:
    """Merged triangle mesh. Per vertex: pos (n,3), nrm (n,3), uv (n,2) atlas 0..1,
    joints (n,4) skin joint indices, weights (n,4); tris (m,3)."""


def load(g, atlas_w, atlas_h, ncol=0):
    from PIL import Image
    j = g.j
    static = not j.get('skins')
    par = {c: i for i, n in enumerate(j['nodes']) for c in n.get('children', [])}

    def world(i):
        M = np.eye(4)
        while i is not None:
            M = node_local(j['nodes'][i]) @ M
            i = par.get(i)
        return M
    prims = []                                             # (primitive, node world matrix)
    for ni, node in enumerate(j['nodes']):
        if 'mesh' not in node:
            continue
        for p in j['meshes'][node['mesh']]['primitives']:
            if p.get('mode', 4) != 4:
                continue
            prims.append((p, world(ni)))
    # Atlas: textured materials share the top of the atlas by area of their texture; flat
    # materials get an 8x8 block each along the bottom row.
    mats = j.get('materials', [])
    textured, flat = [], []
    for p, _ in prims:
        m = mats[p['material']] if 'material' in p else {}
        pbr = m.get('pbrMetallicRoughness', {})
        if 'baseColorTexture' in pbr and 'TEXCOORD_0' in p['attributes']:
            img = j['textures'][pbr['baseColorTexture']['index']]['source']
            textured.append((p, img))
        else:
            col = pbr.get('baseColorFactor', [1, 1, 1, 1])
            flat.append((p, tuple(srgb_to_byte(c) for c in col[:3])))
    imgs = sorted({i for _, i in textured}, key=lambda i: -g.image(i).size[0] * g.image(i).size[1])
    canvas = Image.new('RGB', (atlas_w, atlas_h), (0, 0, 0))
    rects = {}
    # Largest texture: full width, rows above the 8-pixel strip; the others: 8x8 cells in the
    # strip; flat colours after them.
    strip_y = atlas_h - 8
    cell = 0
    for k, i in enumerate(imgs):
        if k == 0:
            r = (0, 0, atlas_w, strip_y)
        else:
            r = (cell * 8, strip_y, 8, 8); cell += 1
        im = g.image(i).convert('RGB').resize((r[2], r[3]), Image.LANCZOS)
        canvas.paste(im, (r[0], r[1]))
        rects[i] = r
    flat_cells = {}
    for _, col in flat:
        if col not in flat_cells:
            r = (cell * 8, strip_y, 8, 8); cell += 1
            canvas.paste(Image.new('RGB', (8, 8), col), (r[0], r[1]))
            flat_cells[col] = r
    if cell * 8 > atlas_w:
        sys.exit('k64model: too many materials for the atlas strip')
    if ncol:
        # CI atlas: the textures get ncol - (flat colours) palette entries, the flat colours one
        # each (quantizing the whole atlas by pixel count would merge the small flat blocks)
        if len(flat_cells) >= ncol:
            sys.exit('k64model: %d flat colours do not fit a %d-colour palette' % (len(flat_cells), ncol))
        a = np.array(canvas)
        texm = np.zeros(a.shape[:2], bool)
        for x, y, w, h in rects.values():
            texm[y:y + h, x:x + w] = True
        if texm.any():
            k = ncol - len(flat_cells)
            q = Image.fromarray(a[texm][None]).quantize(colors=k, method=Image.Quantize.MEDIANCUT,
                                                        dither=Image.Dither.NONE)
            a[texm] = np.array(q.convert('RGB'))[0]
        used = texm.copy()
        for x, y, w, h in flat_cells.values():
            used[y:y + h, x:x + w] = True
        a[~used] = a[used][0]                              # unused strip: no palette entry of its own
        canvas = Image.fromarray(a)

    pos, nrm, uv, jt, wt, tris = [], [], [], [], [], []
    base = 0
    for p, X in prims:
        a = p['attributes']
        P = g.acc(a['POSITION'])
        N = g.acc(a['NORMAL']) if 'NORMAL' in a else None
        I = g.acc(p['indices']).astype(np.int64).reshape(-1, 3) if 'indices' in p else np.arange(len(P)).reshape(-1, 3)
        if static:
            J = np.zeros((len(P), 4), np.int64)
            W = np.zeros((len(P), 4)); W[:, 0] = 1
            P = P.astype(np.float64) @ X[:3, :3].T + X[:3, 3]
            if N is not None:
                N = N.astype(np.float64) @ np.linalg.inv(X[:3, :3])
            if np.linalg.det(X[:3, :3]) < 0:               # mirrored node: keep the winding
                I = I[:, [0, 2, 1]]
        else:
            J = g.acc(a['JOINTS_0']).astype(np.int64)
            W = g.acc(a['WEIGHTS_0']).astype(np.float64)
        tex = next((i for q, i in textured if q is p), None)
        if tex is not None:
            T = g.acc(a['TEXCOORD_0'])
            x, y, w, h = rects[tex]
            mn, mx = T[I].min(1), T[I].max(1)
            nrep = int((np.floor(mn) + 1 < mx - 1e-6).any(1).sum())
            if nrep:
                print('k64model: warning: %d triangles cross a texture repeat (the atlas cannot '
                      'repeat: they are stretched over the wrap)' % nrep, file=sys.stderr)
            # wrap into 0..1 (keeping 1.0: the far edge), then into the rect (half-texel inset
            # so bilinear stays inside)
            T = np.where((T >= 0) & (T <= 1), T, T - np.floor(T))
            U = (x + 0.5 + T[:, 0] * (w - 1)) / atlas_w
            V = (y + 0.5 + T[:, 1] * (h - 1)) / atlas_h
        else:
            col = next(c for q, c in flat if q is p)
            x, y, w, h = flat_cells[col]
            U = np.full(len(P), (x + w / 2) / atlas_w)
            V = np.full(len(P), (y + h / 2) / atlas_h)
        if N is None:
            N = np.zeros_like(P)
            fn = np.cross(P[I[:, 1]] - P[I[:, 0]], P[I[:, 2]] - P[I[:, 0]])
            for c in range(3):
                np.add.at(N, I[:, c], fn)
        N = N / np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-12)
        pos.append(P); nrm.append(N); uv.append(np.stack([U, V], 1)); jt.append(J); wt.append(W)
        tris.append(I + base)
        base += len(P)
    m = Mesh()
    m.pos, m.nrm, m.uv = np.concatenate(pos), np.concatenate(nrm), np.concatenate(uv)
    m.joints, m.weights = np.concatenate(jt), np.concatenate(wt)
    m.tris = np.concatenate(tris)
    return m, canvas


# ------------------------------------------------------------------------------------------
# Decimation: quadric error metric, half-edge collapses (v moves onto u, no new positions).
#
# Vertices ("wedges") are welded by position; a triangle corner keeps its wedge, so UV and
# normal seams survive. A collapse v -> u remaps each wedge of v to the wedge of u it shares a
# triangle with across the edge; it is refused when that map is incomplete or ambiguous (v on
# a seam the edge does not follow), when it would move a border, break the link condition or
# flip a triangle.
# Collapses that join vertices skinned to different virtual bones come second: only once no
# other collapse is left (the floor), and their cost adds how far the removed vertex moves
# when it takes the kept one's skin, over the sampled frames (skin: virtual bone -> (F, 4, 4)
# matrices; without it they stay refused).

def decimate(m, target, vb, skin=None):
    key = np.round(m.pos * 1024).astype(np.int64)
    _, wpos, = np.unique(key, axis=0, return_inverse=True)[:2]
    wpos = wpos.reshape(-1)
    npos = int(wpos.max()) + 1
    P = np.zeros((npos, 3))
    P[wpos] = m.pos
    tris = [list(t) for t in m.tris]
    alive = [True] * len(tris)
    ptris = [set() for _ in range(npos)]
    for ti, t in enumerate(tris):
        a, b, c = (wpos[w] for w in t)
        if a == b or b == c or a == c:
            alive[ti] = False
            continue
        for w in t:
            ptris[wpos[w]].add(ti)
    Q = np.zeros((npos, 4, 4))

    def plane(t):
        a, b, c = (P[wpos[w]] for w in t)
        n = np.cross(b - a, c - a)
        ar = np.linalg.norm(n)
        if ar < 1e-12:
            return None, 0
        n = n / ar
        return np.append(n, -n.dot(a)), ar

    for ti, t in enumerate(tris):
        if not alive[ti]:
            continue
        pl, ar = plane(t)
        if pl is None:
            continue
        K = np.outer(pl, pl) * ar
        for w in t:
            Q[wpos[w]] += K
    # Border edges get a perpendicular constraint plane so they keep their shape.
    def edge_tris(a, b):
        return ptris[a] & ptris[b]
    for p in range(npos):
        for ti in ptris[p]:
            for w in tris[ti]:
                x = wpos[w]
                if x > p and len(edge_tris(p, x)) == 1:
                    pl, _ = plane(tris[ti])
                    e = P[x] - P[p]
                    n = np.cross(e, pl[:3])
                    ln = np.linalg.norm(n)
                    if ln < 1e-12:
                        continue
                    n /= ln
                    bp = np.append(n, -n.dot(P[p]))
                    K = np.outer(bp, bp) * (e.dot(e)) * 10
                    Q[p] += K; Q[x] += K
    ver = [0] * npos
    dead = [False] * npos
    area = [0.0] * npos
    for ti, t in enumerate(tris):
        if alive[ti]:
            ar = plane(t)[1]
            for w in t:
                area[wpos[w]] += ar
    serr = {}
    floor = False

    def skin_err(p, a, b):
        # squared distance between p skinned by a and by b, worst sampled frame
        k = (p, a, b)
        if k not in serr:
            h = np.append(P[p], 1)
            d = (skin[a] - skin[b]) @ h
            serr[k] = float((d[:, :3] ** 2).sum(1).max())
        return serr[k]
    PL = P.tolist()

    def tri_normal(a, b, c):
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        return (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)

    def neighbours(p):
        s = set()
        for ti in ptris[p]:
            for w in tris[ti]:
                s.add(wpos[w])
        s.discard(p)
        return s

    def is_border(p):
        return any(len(edge_tris(p, x)) == 1 for x in neighbours(p))

    def try_collapse(v, u, apply):
        shared = edge_tris(v, u)
        if not shared:
            return None
        # border: v on a border may only slide along a border edge
        if len(shared) == 1:
            if not is_border(v):
                return None
        elif is_border(v):
            return None
        if len(shared) > 2:
            return None
        # link condition
        common = neighbours(v) & neighbours(u)
        if len(common) != len(shared):
            return None
        wmap = {}
        moved = 0.0
        for ti in shared:
            t = tris[ti]
            wv = next(w for w in t if wpos[w] == v)
            wu = next(w for w in t if wpos[w] == u)
            if wmap.get(wv, wu) != wu:
                return None
            if vb[wv] != vb[wu]:
                if skin is None:
                    return None
                moved = max(moved, skin_err(v, vb[wv], vb[wu]))
            wmap[wv] = wu
        for ti in ptris[v]:
            for w in tris[ti]:
                if wpos[w] == v and w not in wmap:
                    return None
        # flips
        for ti in ptris[v]:
            if ti in shared:
                continue
            t = tris[ti]
            a, b, c = (PL[wpos[w]] for w in t)
            n0 = tri_normal(a, b, c)
            a, b, c = (PL[u] if wpos[w] == v else PL[wpos[w]] for w in t)
            n1 = tri_normal(a, b, c)
            l0 = math.sqrt(n0[0] ** 2 + n0[1] ** 2 + n0[2] ** 2)
            l1 = math.sqrt(n1[0] ** 2 + n1[1] ** 2 + n1[2] ** 2)
            if l1 < 1e-9 * max(l0, 1e-9) or n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] < 0.3 * l0 * l1:
                return None
        h = np.append(P[u], 1)
        cost = h.dot(Q[v] + Q[u]).dot(h)
        cost = (1 if moved and not floor else 0, cost + moved * area[v])
        if not apply:
            return cost
        for ti in shared:
            alive[ti] = False
            for w in tris[ti]:
                ptris[wpos[w]].discard(ti)
        for ti in list(ptris[v]):
            tris[ti] = [wmap[w] if wpos[w] == v else w for w in tris[ti]]
            ptris[u].add(ti)
        ptris[v] = set()
        Q[u] += Q[v]
        area[u] += area[v]
        dead[v] = True
        return len(shared)

    heap = []
    def push(p):
        for x in neighbours(p):
            for a, b in ((p, x), (x, p)):
                c = try_collapse(a, b, False)
                if c is not None:
                    heapq.heappush(heap, (c, a, b, ver[a], ver[b]))
    for p in range(npos):
        push(p)
    ntris = sum(alive)
    while ntris > target:
        if not heap or (heap[0][0][0] and not floor):
            # Collapses only get pushed for the neighbourhood of the last one: an edge refused
            # earlier (link, flip) may have become legal elsewhere, look again. Past the floor
            # the cross-bone collapses join the others in one cost order.
            floor = bool(heap)
            heap = []
            for p in range(npos):
                if not dead[p] and ptris[p]:
                    push(p)
            if not heap:
                break
        c, a, b, va, vbv = heapq.heappop(heap)
        if dead[a] or dead[b] or ver[a] != va or ver[b] != vbv:
            continue
        gone = try_collapse(a, b, True)
        if gone is None:
            continue
        ntris -= gone
        # Only the quadric of b changed: edges elsewhere keep their cost, and whether they are
        # still legal is checked again when they come out of the heap.
        ver[b] += 1
        push(b)
    out = [tris[ti] for ti in range(len(tris)) if alive[ti]]
    used = sorted({w for t in out for w in t})
    remap = {w: i for i, w in enumerate(used)}
    r = Mesh()
    idx = np.array(used)
    r.pos, r.nrm, r.uv = m.pos[idx], m.nrm[idx], m.uv[idx]
    r.joints, r.weights = m.joints[idx], m.weights[idx]
    r.tris = np.array([[remap[w] for w in t] for t in out], np.int64)
    r.vb = [vb[w] for w in used]
    return r


# ------------------------------------------------------------------------------------------
# Skinning

def virtual_bones(m):
    """Top-2 joints per vertex, weight of the first quantized to quarters: (a, b, q) means
    q/4 * skin[a] + (4-q)/4 * skin[b]; a single joint is (a, a, 4)."""
    out = []
    for J, W in zip(m.joints, m.weights):
        o = np.argsort(-W, kind='stable')
        a, b = int(J[o[0]]), int(J[o[1]])
        wa, wb = W[o[0]], W[o[1]]
        q = int(round(4 * wa / (wa + wb))) if wb > 0 else 4
        if q >= 4:
            out.append((a, a, 4))
        elif q <= 0:
            out.append((b, b, 4))
        else:
            out.append((a, b, q) if a < b else (b, a, 4 - q))
    return out


class Skeleton:
    """Joints in parent-before-child order. parent[i] = joint index or -1 (then the parent is
    the constant root matrix). Matrices are 4x4 float64 (column vectors)."""

    def __init__(self, g):
        j = g.j
        if not j.get('skins'):                             # static: one identity joint
            self.nodes, self.remap, self.parent = [None], {0: 0}, [-1]
            self.root, self.invbind = np.eye(4), np.array([np.eye(4)])
            self.rest = [(np.zeros(3), np.array([0.0, 0.0, 0.0, 1.0]))]
            return
        skin = j['skins'][0]
        nodes = skin['joints']
        par = {}
        for i, n in enumerate(j['nodes']):
            for c in n.get('children', []):
                par[c] = i
        jset = set(nodes)
        order, seen = [], set()

        def visit(n):
            if n in seen:
                return
            if par.get(n) in jset:
                visit(par[n])
            seen.add(n)
            order.append(n)
        for n in nodes:
            visit(n)
        self.nodes = order
        self.remap = {nodes.index(n): k for k, n in enumerate(order)}   # skin index -> ours
        idx = {n: k for k, n in enumerate(order)}
        self.parent = [idx.get(par.get(n), -1) for n in order]
        roots = {par.get(n) for k, n in enumerate(order) if self.parent[k] < 0}
        if len(roots) != 1:
            sys.exit('k64model: skeleton has several roots')
        top = roots.pop()
        R = np.eye(4)
        while top is not None:
            R = node_local(j['nodes'][top]) @ R
            top = par.get(top)
        self.root = R
        ib = g.acc(skin['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)
        self.invbind = np.array([ib[nodes.index(n)] for n in order])
        self.rest = []
        for n in order:
            nd = j['nodes'][n]
            if 'matrix' in nd:
                sys.exit('k64model: joint with a matrix transform (not TRS)')
            s = np.array(nd.get('scale', [1, 1, 1]))
            if np.abs(s - 1).max() > 1e-3:
                sys.exit('k64model: joint %s has a rest scale (not supported)' % nd.get('name'))
            self.rest.append((np.array(nd.get('translation', [0, 0, 0]), np.float64),
                              np.array(nd.get('rotation', [0, 0, 0, 1]), np.float64)))

    def sample(self, g, anim, fps):
        """Local (t, q) per frame and joint: arrays (F, J, 3) and (F, J, 4)."""
        j = g.j
        a = j['animations'][anim]
        idx = {n: k for k, n in enumerate(self.nodes)}
        dur = 0
        chans = []
        for ch in a['channels']:
            n = ch['target'].get('node')
            path = ch['target']['path']
            if n not in idx:
                continue
            smp = a['samplers'][ch['sampler']]
            ti, to = g.acc(smp['input']), g.acc(smp['output'])
            interp = smp.get('interpolation', 'LINEAR')
            if interp == 'CUBICSPLINE':
                to = to.reshape(len(ti), 3, -1)[:, 1]
            dur = max(dur, ti[-1])
            if path == 'scale':
                if np.abs(to - 1).max() > 1e-2:
                    sys.exit('k64model: animated scale (not supported)')
                continue
            chans.append((idx[n], path, ti, to, interp))
        nf = max(1, int(round(dur * fps)))
        T = np.array([[t for t, _ in self.rest]] * nf)
        Qr = np.array([[q for _, q in self.rest]] * nf)
        for k, path, ti, to, interp in chans:
            for f in range(nf):
                t = f / fps
                i = int(np.searchsorted(ti, t, side='right')) - 1
                if i < 0:
                    v = to[0]
                elif i >= len(ti) - 1:
                    v = to[-1]
                else:
                    u = (t - ti[i]) / (ti[i + 1] - ti[i]) if interp != 'STEP' else 0
                    a0, a1 = to[i], to[i + 1]
                    if path == 'rotation' and a0.dot(a1) < 0:
                        a1 = -a1
                    v = a0 * (1 - u) + a1 * u
                    if path == 'rotation':
                        v = v / np.linalg.norm(v)
                if path == 'rotation':
                    Qr[f, k] = v
                else:
                    T[f, k] = v
        # each joint's quaternions on one hemisphere frame to frame (the runtime nlerps)
        for k in range(len(self.nodes)):
            for f in range(1, nf):
                if Qr[f, k].dot(Qr[f - 1, k]) < 0:
                    Qr[f, k] = -Qr[f, k]
        return T, Qr

    def skin_mats(self, T, Qr, root, invbind):
        G = []
        for k, p in enumerate(self.parent):
            L = np.eye(4)
            L[:3, :3] = quat_matrix(Qr[k])
            L[:3, 3] = T[k]
            G.append((G[p] if p >= 0 else root) @ L)
        return [G[k] @ invbind[k] for k in range(len(G))]


def vb_matrix(S, v):
    a, b, q = v
    return S[a] * (q / 4) + S[b] * (1 - q / 4)


def pose_positions(pos, vbidx, vbs, S):
    M = np.array([vb_matrix(S, v) for v in vbs])
    h = np.concatenate([pos, np.ones((len(pos), 1))], 1)
    return np.einsum('nij,nj->ni', M[np.array(vbidx)], h)[:, :3]


# ------------------------------------------------------------------------------------------
# Command list: meshlets of <= 32 vertices, grouped by virtual bone

VCACHE = 32
OP_MTX, OP_VTX, OP_TRI, OP_LIGHT, OP_RET = 0x11, 0x12, 0x13, 0x16, 0x1A
MTX_LIGHTS = 1                          # MTX flag: the light block follows the matrix


def chunk(m):
    nt = len(m.tris)
    vtris = [[] for _ in range(len(m.pos))]
    for ti, t in enumerate(m.tris):
        for v in t:
            vtris[v].append(ti)
    done = [False] * nt
    cen = m.pos[m.tris].mean(1)
    chunks = []
    nxt = 0
    while True:
        while nxt < nt and done[nxt]:
            nxt += 1
        if nxt >= nt:
            break
        verts, groups, tl = set(), set(), []
        cand = {nxt}
        while True:
            best, bs = None, None
            for ti in cand:
                new = [v for v in m.tris[ti] if v not in verts]
                if len(verts) + len(new) > VCACHE:
                    continue
                ng = len({m.vb[v] for v in new} - groups)
                score = (len(new) * 4 + ng * 6, ti)
                if bs is None or score < bs:
                    best, bs = ti, score
            if best is None and not cand and len(verts) + 3 <= VCACHE:
                # no neighbour left (unwelded or flat-shaded meshes): fill the cache with the
                # nearest triangle not done yet
                left = [t for t in range(nxt, nt) if not done[t]]
                if left:
                    c = cen[tl].mean(0)
                    best = min(left, key=lambda t: (len([v for v in m.tris[t] if v not in verts]),
                                                    float(((cen[t] - c) ** 2).sum()), t))
                    if len(verts) + len([v for v in m.tris[best] if v not in verts]) > VCACHE:
                        best = None
            if best is None:
                break
            done[best] = True
            tl.append(best)
            cand.discard(best)
            for v in m.tris[best]:
                if v not in verts:
                    verts.add(v)
                    groups.add(m.vb[v])
                    for t2 in vtris[v]:
                        if not done[t2]:
                            cand.add(t2)
        chunks.append((tl, verts))
    return chunks


def order_chunks(m, chunks):
    """Chunk order and virtual-bone group order inside each chunk, so that a chunk starts
    with the group the previous one ended with (one MTX + LIGHT less): greedy, the next chunk
    contains the current group, and the group a chunk ends with is the one most of the chunks
    left contain."""
    groups = [sorted({m.vb[v] for v in verts}) for _, verts in chunks]
    left = set(range(len(chunks)))
    cnt = Counter(g for gs in groups for g in gs)
    cur, seq = None, []
    while left:
        cands = [c for c in left if cur in groups[c]] or list(left)

        def key(c):
            others = [g for g in groups[c] if g != cur]
            return (-max([cnt[g] for g in others] + [0]), len(groups[c]), c)
        c = min(cands, key=key)
        left.discard(c)
        for g in groups[c]:
            cnt[g] -= 1
        others = [g for g in groups[c] if g != cur]
        last = max(others, key=lambda g: (cnt[g], -g)) if others else cur
        order = ([cur] if cur in groups[c] else []) + [g for g in others if g != last]
        if last != cur:
            order.append(last)
        seq.append((chunks[c], order))
        cur = order[-1]
    return seq


def build_cmds(m, chunks, flip):
    cmds, vout = [], []
    cur = None
    for (tl, verts), order in order_chunks(m, chunks):
        byvb = {}
        for v in sorted(verts):
            byvb.setdefault(m.vb[v], []).append(v)
        slot = {}
        for g in order:
            vs = byvb[g]
            if g != cur:
                cmds.append((OP_MTX << 24 | MTX_LIGHTS, g))   # matrix + light block
                cur = g
            dest = len(slot)
            cmds.append(((OP_VTX << 24) | (dest << 16) | (len(vs) << 8), len(vout) * 16))
            for v in vs:
                slot[v] = len(slot)
                vout.append(v)
        refs = []
        for ti in tl:
            a, b, c = (0x20 + slot[v] * 5 for v in m.tris[ti])   # K64_KGFX_VREF
            refs.append((a, c, b) if flip else (a, b, c))
        for i in range(0, len(refs), 2):
            a, b, c = refs[i]
            hi = (OP_TRI << 24) | (a << 16) | (b << 8) | c
            if i + 1 < len(refs):
                d, e, f = refs[i + 1]
                cmds.append((hi, (1 << 24) | (d << 16) | (e << 8) | f))
            else:
                cmds.append((hi, 0))
    cmds.append((OP_RET << 24, 0))
    return cmds, vout


def pack_normal(n):
    def tc(v, lo, hi):
        i = int(v)
        return lo if i < lo else hi if i > hi else i
    nx, ny, nz = tc(n[0] * 15.5, -16, 15), tc(n[1] * 31.5, -32, 31), tc(n[2] * 15.5, -16, 15)
    return ((nx & 31) << 11) | ((ny & 63) << 5) | (nz & 31)


# ------------------------------------------------------------------------------------------
# Bake

HDR = '>4sHHHHIIIIIIIIIfII6fII'   # layout: include/k64/model.h (k64_model_hdr_t)
HDR_SIZE = 96
EXTENT = 4096.0                   # model space: positions and the posed model within +-EXTENT


def bake(a):
    import k64bake
    g = Gltf(a.src)
    aw, ah, afmt = parse_tex(a.tex)
    m, atlas = load(g, aw, ah, {'ci4': 16, 'ci8': 256}.get(afmt, 0))
    sk = Skeleton(g)
    m.joints = np.vectorize(lambda x: sk.remap[int(x)])(m.joints)
    vb = virtual_bones(m)
    if g.j.get('animations') and g.j.get('skins'):
        T, Qr = sk.sample(g, 0, a.fps)
    else:
        T = np.array([[t for t, _ in sk.rest]])
        Qr = np.array([[q for _, q in sk.rest]])
    # skin matrices of every virtual bone over up to 16 frames, for the decimator's floor
    fr = sorted(set(np.linspace(0, len(T) - 1, min(len(T), 16)).round().astype(int)))
    Ss = [sk.skin_mats(T[f], Qr[f], sk.root, sk.invbind) for f in fr]
    skin = {v: np.array([vb_matrix(S, v) for S in Ss]) for v in set(vb)}
    full = m
    if a.tris and a.tris < len(m.tris):
        m = decimate(full, a.tris, vb, skin)
    else:
        m.vb = vb
    # LOD 0 = m; the others are decimated from the full mesh. They share the skeleton, the
    # virtual bones, the quantization and the atlas: only the vertices and the list differ.
    lods = [m]
    for t in a.lods:
        if t >= len(lods[-1].tris):
            sys.exit('k64model: --lods %d: not fewer triangles than the previous LOD (%d)' % (t, len(lods[-1].tris)))
        lods.append(decimate(full, t, vb, skin))
        if len(lods[-1].tris) > t * 1.1:
            print('k64model: warning: LOD %d stops at %d triangles (asked %d): the decimator keeps '
                  'texture seams and borders' % (len(lods) - 1, len(lods[-1].tris), t))
    vbs = sorted(set().union(*(set(x.vb) for x in lods)))
    vbi = {v: i for i, v in enumerate(vbs)}
    for x in lods:
        x.vb = [vbi[v] for v in x.vb]

    # s16 positions = mesh * qin; the dequantization goes into the inverse binds. The root is
    # scaled so the posed model (every frame) stays within +-EXTENT too: matrices near unit
    # scale keep the s15.16 MVP precise, and clip coordinates stay in range.
    qin = EXTENT / max(np.abs(x.pos).max() for x in lods)
    ext = 0
    for f in range(len(T)):
        S = sk.skin_mats(T[f], Qr[f], sk.root, sk.invbind)
        for x in lods:
            ext = max(ext, np.abs(pose_positions(x.pos, x.vb, vbs, S)).max())
    root = np.diag([EXTENT / ext] * 3 + [1]) @ sk.root
    deq = np.diag([1 / qin] * 3 + [1])
    invbind = [ib @ deq for ib in sk.invbind]
    for x in lods:
        x.qpos = np.clip(np.round(x.pos * qin), -32768, 32767).astype(np.int64)
        x.chunks = chunk(x)
        x.cmds, x.vout = build_cmds(x, x.chunks, a.flip)
    qpos, chunks, cmds, vout = m.qpos, m.chunks, m.cmds, m.vout

    tmax = max(np.abs(T).max(), 1e-6)
    tscale = 2.0 ** math.ceil(math.log2(tmax / 32000))
    lo, hi = np.full(3, 1e30), np.full(3, -1e30)
    for f in range(len(T)):
        S = sk.skin_mats(T[f], Qr[f], root, invbind)
        P = pose_positions(qpos.astype(np.float64), m.vb, vbs, S)
        lo, hi = np.minimum(lo, P.min(0)), np.maximum(hi, P.max(0))
    if a.preview:
        from PIL import Image
        atlas.resize((aw * 4, ah * 4), Image.NEAREST).save(a.preview + '_atlas.png')
        for f in (0, len(T) // 2):
            S = sk.skin_mats(T[f], Qr[f], root, invbind)
            preview(pose_positions(qpos.astype(np.float64), m.vb, vbs, S), m.tris,
                    '%s_f%d.png' % (a.preview, f), lo, hi)

    # ---- blob
    body = bytearray()

    def put(b, align=16):
        body.extend(bytes((-(len(body) + HDR_SIZE)) & (align - 1)))
        off = len(body) + HDR_SIZE
        body.extend(b)
        return off
    lodtab = bytearray()
    for L in lods:
        vbytes = bytearray()
        for v in L.vout:
            x, y, z = (int(c) for c in L.qpos[v])
            u, w = L.uv[v]
            vbytes += struct.pack('>hhhHBBBBhh', x, y, z, pack_normal(L.nrm[v]), 255, 255, 255, 255,
                                  int(round(u * aw * 32)), int(round(w * ah * 32)))
        L.off_verts = put(vbytes)
        L.off_cmds = put(b''.join(struct.pack('>II', h, l) for h, l in L.cmds))
        lodtab += struct.pack('>IIIII', len(L.cmds), L.off_cmds, L.off_verts, len(L.vout), len(L.tris))
    off_verts, off_cmds = m.off_verts, m.off_cmds
    off_lods = put(bytes(lodtab), 4)
    nj = len(sk.nodes)
    off_parent = put(struct.pack('>%db' % nj, *sk.parent), 4)

    def mat(M):
        # column-major, like k64_mat4_t. No denormals: the VR4300 traps on them as operands
        # (unimplemented operation, even with FCSR.FS set).
        v = M.T.reshape(-1)
        v = np.where(np.abs(v) < 1e-30, 0.0, v)
        return struct.pack('>16f', *v)
    off_invbind = put(b''.join(mat(M) for M in invbind))
    off_root = put(mat(root))
    off_vb = put(b''.join(struct.pack('>BBBB', x, y, q, 0) for x, y, q in vbs), 4)
    anim = bytearray()
    for f in range(len(T)):
        for k in range(nj):
            q = np.clip(np.round(Qr[f, k] * 32767), -32767, 32767).astype(int)
            t = np.clip(np.round(T[f, k] / tscale), -32768, 32767).astype(int)
            anim += struct.pack('>4h3hh', *q, *t, 0)
    off_anim = put(bytes(anim))
    off_tex = put(k64bake.tex_blob(atlas, afmt, a.src))
    hdr = struct.pack(HDR, b'K6M2', nj, len(vbs), len(T), a.fps, len(cmds), off_cmds, off_verts, len(vout),
                      off_parent, off_invbind, off_root, off_vb, off_anim, tscale, off_tex, len(m.tris),
                      *lo, *hi, len(lods), off_lods)
    assert len(hdr) <= HDR_SIZE
    hdr += bytes(HDR_SIZE - len(hdr))
    with open(a.dst, 'wb') as f:
        f.write(hdr + body)
    nsw = sum(1 for h, _ in cmds if h >> 24 == OP_MTX)
    nvtx = sum(1 for h, _ in cmds if h >> 24 == OP_VTX)
    print('k64model: %s: %d tris, %d verts (%d loaded by %d VTX), %d virtual bones (%d switches), '
          '%d chunks, %d commands, %d frames, %d bytes'
          % (a.dst, len(m.tris), len(m.pos), len(vout), nvtx, len(vbs), nsw, len(chunks), len(cmds), len(T),
             len(hdr) + len(body)))
    for i, L in enumerate(lods[1:], 1):
        print('k64model:   LOD %d: %d tris, %d verts loaded by %d VTX, %d commands'
              % (i, len(L.tris), len(L.vout), sum(1 for h, _ in L.cmds if h >> 24 == OP_VTX), len(L.cmds)))


def parse_tex(s):
    size, fmt = s.split(':')
    w, h = (int(x) for x in size.split('x'))
    return w, h, fmt


def preview(pos, tris, path, lo=None, hi=None, size=400):
    """Flat-shaded side + front orthographic views (development aid)."""
    from PIL import Image, ImageDraw
    im = Image.new('RGB', (size * 2, size), (40, 40, 60))
    d = ImageDraw.Draw(im)
    if lo is None:
        lo, hi = pos.min(0), pos.max(0)
    sc = 0.9 * size / (hi - lo).max()
    T = pos[tris]
    n = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
    for ax, dep, ox in ((2, 0, 0), (0, 2, size)):
        order = np.argsort(T[:, :, dep].mean(1))
        for i in order:
            sh = int(60 + 180 * abs(n[i].dot([0.4, 0.8, 0.45])))
            pts = [(ox + size / 2 + (p[ax] - (lo[ax] + hi[ax]) / 2) * sc, size * 0.95 - (p[1] - lo[1]) * sc)
                   for p in T[i]]
            d.polygon(pts, fill=(sh, sh, sh))
    im.save(path)
