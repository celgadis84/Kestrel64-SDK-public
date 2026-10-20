# Generates the demo 12 model (deterministic, committed): shrine.glb, a static glTF scene with
# no skin. It exercises what the static path of the baker has to handle: a node hierarchy with
# translation, rotation and non-uniform scale, a mirrored node (negative scale), a textured
# material (embedded PNG) and flat-coloured ones, and a primitive without normals.
import io
import json
import math
import os
import struct

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def box(sx, sy, sz):
    """Axis-aligned box from (-sx, 0, -sz) to (sx, 2 sy, sz), 24 vertices, flat faces, the
    texture once per face (the baker's atlas cannot repeat a texture)."""
    P, N, T, I = [], [], [], []
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
             ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    s = np.array([sx, sy, sz])
    for n, u, v in faces:
        n, u, v = (np.array(x, float) for x in (n, u, v))
        b = len(P)
        for a, c in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = (n + a * u + c * v) * s + [0, sy, 0]
            P.append(p); N.append(n)
            T.append(((a + 1) / 2, (1 - c) / 2))
        I += [b, b + 1, b + 2, b, b + 2, b + 3]
    return np.array(P), np.array(N), np.array(T), np.array(I)


def disc(r, n=24, rings=4):
    """Flat disc in rings (no long slivers from the centre)."""
    P = [(0, 0, 0)]
    for k in range(1, rings + 1):
        P += [(r * k / rings * math.cos(2 * math.pi * i / n), 0, -r * k / rings * math.sin(2 * math.pi * i / n))
              for i in range(n)]
    I = []
    for i in range(n):
        I += [0, 1 + i, 1 + (i + 1) % n]
    for k in range(1, rings):
        a, b = 1 + (k - 1) * n, 1 + k * n
        for i in range(n):
            j = (i + 1) % n
            I += [a + i, b + i, b + j, a + i, b + j, a + j]
    return np.array(P, float), np.array([(0, 1, 0)] * len(P), float), None, np.array(I)


def torus(R, r, nu=20, nv=10):
    """Welded torus; no normals (the baker computes them)."""
    P, I = [], []
    for i in range(nu):
        a = 2 * math.pi * i / nu
        for j in range(nv):
            b = 2 * math.pi * j / nv
            P.append(((R + r * math.cos(b)) * math.cos(a), r * math.sin(b), (R + r * math.cos(b)) * math.sin(a)))
    for i in range(nu):
        for j in range(nv):
            a, b = i * nv + j, ((i + 1) % nu) * nv + j
            c, d = ((i + 1) % nu) * nv + (j + 1) % nv, i * nv + (j + 1) % nv
            I += [a, d, c, a, c, b]
    return np.array(P, float), None, None, np.array(I)


def wedge(w, h, d):
    """Right-angled ramp (asymmetric along x, so the mirrored copy shows): flat faces."""
    v = [(0, 0, -d), (w, 0, -d), (0, h, -d), (0, 0, d), (w, 0, d), (0, h, d)]
    tris = [(0, 2, 1), (3, 4, 5), (0, 1, 4), (0, 4, 3), (0, 3, 5), (0, 5, 2), (1, 2, 5), (1, 5, 4)]
    P, N, I = [], [], []
    for t in tris:
        a, b, c = (np.array(v[k], float) for k in t)
        n = np.cross(b - a, c - a); n /= np.linalg.norm(n)
        I += [len(P), len(P) + 1, len(P) + 2]
        P += [a, b, c]; N += [n] * 3
    return np.array(P), np.array(N), None, np.array(I)


def stone_png():
    im = Image.new('RGB', (32, 32))
    px = im.load()
    seed = 7
    for y in range(32):
        for x in range(32):
            seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
            mortar = y % 8 == 0 or (x + (8 if (y // 8) & 1 else 0)) % 16 == 0
            g = 70 if mortar else 150 + ((seed >> 16) & 31)
            px[x, y] = (g, g - 8, g - 22)
    b = io.BytesIO()
    im.save(b, 'PNG')
    return b.getvalue()


def quat(axis, ang):
    axis = np.array(axis, float) / np.linalg.norm(axis)
    s = math.sin(ang / 2)
    return [float(axis[0] * s), float(axis[1] * s), float(axis[2] * s), float(math.cos(ang / 2))]


class Writer:
    def __init__(self):
        self.bin = bytearray()
        self.j = {'asset': {'version': '2.0', 'generator': 'kestrel64-sdk make_scene.py'},
                  'scene': 0, 'scenes': [{'nodes': []}], 'nodes': [], 'meshes': [],
                  'materials': [], 'accessors': [], 'bufferViews': [], 'buffers': [],
                  'textures': [], 'images': [], 'samplers': [{}]}

    def view(self, data):
        self.bin += bytes((-len(self.bin)) & 3)
        self.j['bufferViews'].append({'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': len(data)})
        self.bin += data
        return len(self.j['bufferViews']) - 1

    def acc(self, arr, typ, ctype=5126):
        arr = np.asarray(arr)
        dt = '<f4' if ctype == 5126 else '<u2'
        a = {'bufferView': self.view(arr.astype(dt).tobytes()), 'componentType': ctype,
             'count': len(arr), 'type': typ}
        if typ == 'VEC3':
            a['min'] = arr.min(0).tolist(); a['max'] = arr.max(0).tolist()
        self.j['accessors'].append(a)
        return len(self.j['accessors']) - 1

    def material(self, color=None, png=None):
        pbr = {'metallicFactor': 0.0}
        if png is not None:
            self.j['images'].append({'bufferView': self.view(png), 'mimeType': 'image/png'})
            self.j['textures'].append({'source': len(self.j['images']) - 1, 'sampler': 0})
            pbr['baseColorTexture'] = {'index': len(self.j['textures']) - 1}
        else:
            pbr['baseColorFactor'] = list(color) + [1.0]
        self.j['materials'].append({'pbrMetallicRoughness': pbr})
        return len(self.j['materials']) - 1

    def mesh(self, geo, mat):
        P, N, T, I = geo
        attr = {'POSITION': self.acc(P, 'VEC3')}
        if N is not None:
            attr['NORMAL'] = self.acc(N, 'VEC3')
        if T is not None:
            attr['TEXCOORD_0'] = self.acc(T, 'VEC2')
        prim = {'attributes': attr, 'indices': self.acc(I, 'SCALAR', 5123), 'material': mat}
        self.j['meshes'].append({'primitives': [prim]})
        return len(self.j['meshes']) - 1

    def node(self, parent=None, **kw):
        self.j['nodes'].append(kw)
        k = len(self.j['nodes']) - 1
        if parent is None:
            self.j['scenes'][0]['nodes'].append(k)
        else:
            self.j['nodes'][parent].setdefault('children', []).append(k)
        return k

    def save(self, path):
        self.bin += bytes((-len(self.bin)) & 3)
        self.j['buffers'] = [{'byteLength': len(self.bin)}]
        js = json.dumps(self.j, separators=(',', ':')).encode()
        js += b' ' * ((-len(js)) & 3)
        out = struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(js) + 8 + len(self.bin))
        out += struct.pack('<II', len(js), 0x4E4F534A) + js
        out += struct.pack('<II', len(self.bin), 0x004E4942) + bytes(self.bin)
        open(path, 'wb').write(out)


w = Writer()
stone = w.material(png=stone_png())
grass = w.material(color=(0.18, 0.40, 0.12))
gold = w.material(color=(0.90, 0.62, 0.10))
red = w.material(color=(0.60, 0.10, 0.08))

w.node(mesh=w.mesh(disc(7), grass))
w.node(mesh=w.mesh(box(3.2, 0.25, 3.2), stone))                                 # plinth
ring = w.node(translation=[0, 0.5, 0], rotation=quat((0, 1, 0), math.pi / 4))     # pillar ring
pillar = w.mesh(box(0.5, 2.0, 0.5), stone)
for i in range(4):
    a = 2 * math.pi * i / 4
    w.node(ring, mesh=pillar, translation=[2.4 * math.cos(a), 0, 2.4 * math.sin(a)],
           scale=[1 + 0.2 * i, 1, 1])                                               # non-uniform
w.node(mesh=w.mesh(box(2.9, 0.2, 2.9), stone), translation=[0, 4.5, 0])            # lintel
w.node(mesh=w.mesh(torus(1.0, 0.3), gold), translation=[0, 2.2, 0],
       rotation=quat((1, 0, 0), math.pi / 2), scale=[1.2, 1.2, 1.2])               # no normals
ramp = w.mesh(wedge(1.5, 0.8, 1.0), red)
w.node(mesh=ramp, translation=[3.4, 0, 4.6])
w.node(mesh=ramp, translation=[-3.4, 0, 4.6], scale=[-1, 1, 1])                      # mirrored
w.save(os.path.join(HERE, 'shrine.glb'))
