#!/usr/bin/env python3
"""k64impostor — glTF 2.0 (.glb) -> .k64i impostor (N views of a model around Y).

Called by `k64bake.py impostor in.glb out.k64i [--views 8] [--cell 32x64] [--format rgba16]
[--light x,y,z] [--ambient 0.45] [--preview out.png]`.

Each view is an orthographic render of the model (bind pose for a skinned one) seen from
the horizon at angle phi_k = 2 pi k / N, measured from +Z toward +X (view 0 looks at the
model's front from +Z). The camera's right is (cos phi, 0, -sin phi), which is the right of
a k64_kgfx_billboard_y quad facing an eye in that direction, so the runtime draws view k on
a Y billboard through the model's vertical axis (include/k64/impostor.h).

Rendering: 4x4 supersampled z-buffer rasterizer (numpy), albedo from the material atlas of
k64model.load (textures and flat colours), Lambert from a fixed model-space light plus
ambient, both faces lit from the side the camera sees. Alpha = coverage >= 1/2; the colour
of transparent texels is bled in from their opaque neighbours so bilinear filtering does
not pull a dark fringe in at the alpha-compare edge.

.k64i layout (big-endian):
  0  'K6I1'   4 u16 views   6 u16 reserved   8 u32 stride (bytes per cell, 8-aligned)
  12 f32 half_w   16 f32 height   20 f32 y0   24 f32 cx   28 f32 cz      (model units)
  32 cells: views x .k64t blob (k64bake.py tex layout), cell k at 32 + k * stride
The quad of an instance stands on (cx, y0, cz), half_w each side, height tall.

numpy + PIL only.
"""
import math, struct, sys
import numpy as np

SS = 4                                                  # supersampling per axis


def render_view(m, alb, phi, c, hw, y0, h, W, H, light, ambient):
    """Returns (rgb float (H, W, 3), coverage (H, W)) of one view."""
    S = W * SS, H * SS
    d = np.array([math.sin(phi), 0.0, math.cos(phi)])   # toward the camera
    r = np.array([math.cos(phi), 0.0, -math.sin(phi)])  # camera right
    P = m.pos - c
    sx = (P @ r + hw) / (2 * hw) * S[0]
    sy = (y0 + h - m.pos[:, 1]) / h * S[1]
    sz = P @ d                                          # larger = nearer
    zb = np.full((S[1], S[0]), -np.inf)
    col = np.zeros((S[1], S[0], 3))
    ah, aw = alb.shape[:2]
    for t in m.tris:
        x, y, z = sx[t], sy[t], sz[t]
        x0, x1 = max(int(math.floor(x.min())), 0), min(int(math.ceil(x.max())), S[0] - 1)
        y0_, y1 = max(int(math.floor(y.min())), 0), min(int(math.ceil(y.max())), S[1] - 1)
        if x0 > x1 or y0_ > y1:
            continue
        area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])
        if abs(area) < 1e-12:
            continue
        px, py = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0_, y1 + 1) + 0.5)
        w0 = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area
        w1 = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area
        w2 = 1 - w0 - w1
        ins = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not ins.any():
            continue
        zz = w0 * z[0] + w1 * z[1] + w2 * z[2]
        sub = zb[y0_:y1 + 1, x0:x1 + 1]
        win = ins & (zz > sub)
        if not win.any():
            continue
        uv = w0[..., None] * m.uv[t[0]] + w1[..., None] * m.uv[t[1]] + w2[..., None] * m.uv[t[2]]
        n = w0[..., None] * m.nrm[t[0]] + w1[..., None] * m.nrm[t[1]] + w2[..., None] * m.nrm[t[2]]
        n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-12)
        n = np.where((n @ d)[..., None] < 0, -n, n)     # the face the camera sees
        lit = ambient + (1 - ambient) * np.clip(n @ light, 0, 1)
        u = np.clip((uv[..., 0] * aw).astype(int), 0, aw - 1)
        v = np.clip((uv[..., 1] * ah).astype(int), 0, ah - 1)
        c_ = alb[v, u] * lit[..., None]
        sub[win] = zz[win]
        col[y0_:y1 + 1, x0:x1 + 1][win] = c_[win]
    cov = np.isfinite(zb).astype(float)
    # box-filter down to the cell
    cov4 = cov.reshape(H, SS, W, SS).mean((1, 3))
    rgb = (col * cov[..., None]).reshape(H, SS, W, SS, 3).sum((1, 3))
    rgb /= np.maximum(cov.reshape(H, SS, W, SS).sum((1, 3)), 1e-9)[..., None]
    return rgb, cov4


def bleed(rgb, opaque, rounds=8):
    """Fill transparent texels with the mean of their already filled 8-neighbours."""
    rgb, have = rgb.copy(), opaque.copy()
    H, W = have.shape
    for _ in range(rounds):
        if have.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros((H, W))
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                sh = np.zeros_like(have)
                sr = np.zeros_like(rgb)
                ys, yd = slice(max(dy, 0), H + min(dy, 0)), slice(max(-dy, 0), H + min(-dy, 0))
                xs, xd = slice(max(dx, 0), W + min(dx, 0)), slice(max(-dx, 0), W + min(-dx, 0))
                sh[yd, xd] = have[ys, xs]
                sr[yd, xd] = rgb[ys, xs]
                acc += sr * sh[..., None]
                cnt += sh
        new = ~have & (cnt > 0)
        rgb[new] = acc[new] / cnt[new][:, None]
        have |= new
    return rgb


def bake(a):
    from PIL import Image
    import k64model
    from k64bake import tex_blob
    W, H = (int(v) for v in a.cell.split('x'))
    g = k64model.Gltf(a.src)
    m, canvas = k64model.load(g, 256, 256)
    alb = np.array(canvas.convert('RGB'), float) / 255.0
    lo, hi = m.pos.min(0), m.pos.max(0)
    c = np.array([(lo[0] + hi[0]) / 2, 0.0, (lo[2] + hi[2]) / 2])
    hw = float(np.sqrt(((m.pos[:, [0, 2]] - c[[0, 2]]) ** 2).sum(1)).max()) * (1 + 1.0 / W)
    y0, h = float(lo[1]), float(hi[1] - lo[1]) * (1 + 0.5 / H)
    light = np.array([float(v) for v in a.light.split(',')])
    light /= np.linalg.norm(light)
    cells, previews = [], []
    for k in range(a.views):
        rgb, cov = render_view(m, alb, 2 * math.pi * k / a.views, c, hw, y0, h, W, H, light, a.ambient)
        opaque = cov >= 0.5
        rgb = bleed(rgb, opaque)
        rgba = np.concatenate([np.clip(rgb * 255 + 0.5, 0, 255), (opaque * 255)[..., None]], -1).astype(np.uint8)
        im = Image.fromarray(rgba, 'RGBA')
        cells.append(tex_blob(im, a.format, '%s view %d' % (a.src, k)))
        previews.append(im)
    stride = max(len(b) for b in cells)
    stride += (-stride) & 7
    out = struct.pack('>4sHHIfffff', b'K6I1', a.views, 0, stride, hw, h, y0, float(c[0]), float(c[2]))
    for b in cells:
        out += b + bytes(stride - len(b))
    with open(a.dst, 'wb') as f:
        f.write(out)
    if a.preview:
        sheet = Image.new('RGBA', (W * a.views, H), (40, 40, 60, 255))
        for k, im in enumerate(previews):
            sheet.alpha_composite(im, (k * W, 0))
        sheet.resize((W * a.views * 4, H * 4), Image.NEAREST).save(a.preview)
    print('k64impostor: %d views of %dx%d %s, half_w %.1f height %.1f (%d tris), %d bytes'
          % (a.views, W, H, a.format, hw, h, len(m.tris), len(out)), file=sys.stderr)


def add_args(p):
    p.add_argument('--views', type=int, default=8)
    p.add_argument('--cell', default='32x64', help='WxH texels per view (TMEM: <= 4 KB)')
    p.add_argument('--format', default='rgba16', help='rgba16, ci8, ci4 (transparent = index 0)')
    p.add_argument('--light', default='0.4,0.8,0.45', help='model-space direction toward the light')
    p.add_argument('--ambient', type=float, default=0.45)
    p.add_argument('--preview', help='write a PNG of all views (development aid)')
