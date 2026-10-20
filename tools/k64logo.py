#!/usr/bin/env python3
# kestrel64-sdk — the boot logo and the video intro/outro, all generated from one description.
#
#   k64logo.py c OUT.inc                         ROM data for src/logo/logo.c (the output is
#                                                committed: building the SDK does not run this)
#   k64logo.py video OUT.mp4 [--outro] [--size 1920x1080] [--fps 60] [--ffmpeg PATH]
#   k64logo.py still OUT.png --t SECONDS [--outro] [--size WxH]
#
# The bird is the Kestrel64 logo silhouette (SHAPE). Its 3D model is built from that outline,
# so seen from the side it is the logo: the body is the silhouette inflated by its distance
# field, the two wings are the wing outline hinged on the back ridge, the legs fold on a hip.
# The ROM and the videos play the same 60 Hz track, lit with kgfx's formula
# (rgb = color * min(ambient + sum(max(N.L, 0) * light), 2)).
#
# Needs numpy, Pillow, scipy and scikit-image; video also needs an ffmpeg binary.
import argparse, math, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(HERE)
FONT = os.path.join(SDK, 'assets', 'fonts', 'Lato.ttf')
PATREON = 'patreon.com/celgadis84'

# ---- the logo, in a 708 px square, y down (traced from the label art) ----
S = 708
SHAPE = [
    (520, 682.5), (510, 680.5), (503.5, 667), (499, 663.5), (493, 663.5), (470, 676.5),
    (461, 675.5), (457.5, 672), (457.5, 665), (476.5, 647), (476.5, 634), (466, 626.5),
    (427, 628.5), (416, 623.5), (408.5, 612), (408.5, 589), (405.5, 581), (401, 576.5),
    (384, 569.5), (365, 557.5), (353, 558.5), (346, 562.5), (276, 617.5), (243, 635.5),
    (211, 645.5), (182, 650.5), (162, 650.5), (114, 643.5), (78, 642.5), (65.5, 636), (64.5, 630),
    (71, 622.5), (154, 583.5), (289, 503.5), (327.5, 472), (349.5, 446), (353.5, 434),
    (349, 429.5), (340, 430.5), (321, 447.5), (313, 451.5), (302, 451.5), (296, 448.5),
    (284.5, 437), (275.5, 409), (265.5, 385), (259, 375.5), (248, 371.5), (226, 370.5),
    (187, 361.5), (145, 335.5), (134.5, 323), (134.5, 317), (140, 311.5), (149, 310.5),
    (170, 315.5), (188, 316.5), (210, 312.5), (215.5, 309), (213, 304.5), (200, 299.5),
    (166, 295.5), (109, 283.5), (89.5, 270), (81.5, 256), (66.5, 240), (62.5, 232), (63.5, 223),
    (70, 218.5), (83, 217.5), (137, 234.5), (175, 240.5), (193, 246.5), (208, 246.5), (215.5, 243),
    (213, 238.5), (194, 232.5), (176, 221.5), (137, 213.5), (118, 204.5), (92, 197.5), (58, 180.5),
    (42.5, 167), (37.5, 157), (35, 137.5), (57, 142.5), (79, 153.5), (99, 158.5), (124, 169.5),
    (169, 181.5), (182.5, 181), (159, 158.5), (123, 138.5), (94, 117.5), (65.5, 89), (56.5, 76),
    (50.5, 55), (41.5, 39), (42.5, 31), (46, 27.5), (62, 27.5), (103, 55.5), (165, 86.5),
    (281, 132.5), (302, 137.5), (324, 147.5), (358, 157.5), (409, 183.5), (429, 200.5),
    (439.5, 213), (452.5, 242), (463.5, 275), (471.5, 289), (488, 306.5), (498, 312.5),
    (514, 313.5), (536, 303.5), (561, 296.5), (596, 293.5), (614, 297.5), (626, 303.5),
    (647, 318.5), (660.5, 333), (674.5, 365), (677.5, 379), (676.5, 387), (673, 390.5),
    (667, 390.5), (642, 377.5), (626, 378.5), (611, 385.5), (587.5, 406), (576.5, 426),
    (553.5, 457), (499.5, 505), (490.5, 517), (476.5, 544), (474.5, 553), (476.5, 567),
    (520, 637.5), (525, 640.5), (535, 640.5), (550, 633.5), (564, 632.5), (572, 635.5),
    (577.5, 644), (572, 653.5), (543, 663.5), (527, 679.5),
]
SHOULDER = ((200.0, 430.0), (560.0, 300.0))   # hinge of the wings (back ridge), tail -> head
HIP = (455.0, 572.0)
HEAD = (612.0, 345.0)
TAIL_TIP = (68.0, 636.0)
EYE, EYE_R = (624.0, 333.0), 10.0
TILT = math.atan2(620 - 330, 620 - 100)          # the logo body rises this much tail -> head
PARTS = ('body', 'wing_near', 'wing_far', 'legs')
WING_DZ = 3.0                                     # near wing in front of the far one when folded

SIL = (244, 238, 228)                             # the flat 2D logo
RUFOUS, BUFF, SLATE = (178, 82, 40), (232, 202, 158), (118, 136, 168)
DARK, YELLOW, CREAM = (46, 42, 50), (226, 178, 60), (240, 228, 204)
BG_TOP, BG_BOT, GLOW = (8, 11, 22), (22, 30, 52), (34, 50, 88)
AMB = (58, 60, 76)
LIGHTS = [((-0.45, 0.75, 0.55), (255, 238, 214)),     # key, warm, upper left front
          ((0.85, -0.15, 0.35), (64, 84, 150)),       # fill, cold, right
          ((0.25, 0.35, -0.9), (150, 140, 128))]      # rim, from behind
TITLE, TITLE2 = 'KESTREL64', ' SDK'
TITLE_RGB, TITLE2_RGB = (246, 242, 234), (242, 178, 51)

# ---- camera: same vertical framing in 4:3 (ROM) and 16:9 (video) ----
FOVY, NEAR, FAR = math.radians(40), 0.5, 40.0
EYE_POS, AT = (0.0, 0.35, 6.5), (0.0, 0.35, 0.0)
SCALE = 1.9 / S                                  # model units -> world at rest
REST = (0.0, 0.78, 0.0)
OUTRO_REST = (-2.15, 0.28, 0.0)
TITLE_Y = 0.70                                    # title top, fraction of the screen height
FPS = 60


def smooth(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3 - 2 * t)


def lerp(a, b, t):
    return a + (b - a) * t


# ================================================================== model
def raster(poly):
    im = Image.new('L', (S, S), 0)
    ImageDraw.Draw(im).polygon([tuple(p) for p in poly], fill=255)
    return np.asarray(im) > 127


def largest(m):
    from scipy import ndimage
    lab, n = ndimage.label(m)
    if n <= 1:
        return m
    return lab == 1 + int(np.argmax(ndimage.sum(m, lab, range(1, n + 1))))


def outline(m, tol):
    from skimage import measure
    c = max(measure.find_contours(np.pad(m, 1).astype(float), 0.5), key=len)
    c = measure.approximate_polygon(c, tol)[:-1]
    return np.stack([c[:, 1] - 1, c[:, 0] - 1], 1)


def sample(f, P):
    x = np.clip(P[:, 0], 0, S - 1.001); y = np.clip(P[:, 1], 0, S - 1.001)
    xi, yi = x.astype(int), y.astype(int); fx, fy = x - xi, y - yi
    return (f[yi, xi] * (1 - fx) * (1 - fy) + f[yi, xi + 1] * fx * (1 - fy)
            + f[yi + 1, xi] * (1 - fx) * fy + f[yi + 1, xi + 1] * fx * fy)


def triangulate(m, tol, spacing):
    """Outline ring (first nring points) + hex grid inside; Delaunay kept inside m.
    Returns points (x, y px), triangles counter-clockwise in y-up space, nring."""
    from scipy.spatial import Delaunay
    from scipy.ndimage import distance_transform_edt, binary_dilation
    ring = outline(m, tol)
    if spacing:                          # short boundary edges: Delaunay then keeps to the outline
        dense = []
        for a, b in zip(ring, np.roll(ring, -1, 0)):
            k = max(1, int(np.ceil(np.linalg.norm(b - a) / (spacing * 0.7))))
            dense += [a + (b - a) * i / k for i in range(k)]
        ring = np.array(dense)
    P = [ring]
    if spacing:
        d = distance_transform_edt(m)
        g = []
        for j, y in enumerate(np.arange(spacing / 2, S, spacing * 0.866)):
            for x in np.arange(spacing / 2 * (j & 1), S, spacing):
                if d[int(y), int(x)] > spacing * 0.55:
                    g.append((x, y))
        if g:
            P.append(np.array(g))
    P = np.concatenate(P)
    T = Delaunay(P).simplices
    md = m.astype(float)
    keep = sample(md, P[T].mean(1)) > 0.5
    T = T[keep]
    a, b, c = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    cross = (b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - (b[:, 1] - a[:, 1]) * (c[:, 0] - a[:, 0])
    flip = cross > 0                     # y down: clockwise here = counter-clockwise y-up
    T[flip] = T[flip][:, ::-1]
    return P, T, len(ring)


def seg_dist(P, a, b):
    """Signed distance of points from the line a->b (positive = right of it, y down = below)."""
    a, b = np.array(a), np.array(b)
    d = (b - a) / np.linalg.norm(b - a)
    r = P - a
    return r[:, 0] * d[1] * -1 + r[:, 1] * d[0], r @ d


def body_colors(P):
    belly, _ = seg_dist(P, (100, 620), HEAD)
    c = np.array(RUFOUS) + (np.array(BUFF) - RUFOUS) * np.clip((belly + 5) / 45, 0, 1)[:, None]
    rng = np.random.default_rng(64)
    c *= (0.86 + 0.14 * rng.random(len(P)))[:, None] ** (belly < 10)[:, None]   # mottled back
    hd = np.hypot(P[:, 0] - HEAD[0], P[:, 1] - HEAD[1])
    c = c + (np.array(SLATE) - c) * np.clip((74 - hd) / 18, 0, 1)[:, None]
    cheek = np.hypot(P[:, 0] - 620, P[:, 1] - 372)                      # pale cheek patch
    c = c + (np.array(CREAM) - c) * np.clip((30 - cheek) / 12, 0, 1)[:, None]
    beak = np.clip((P[:, 0] - 648) / 8, 0, 1) * (P[:, 1] > 326)
    c = c + (np.array(YELLOW) - c) * beak[:, None]
    c = c + (np.array(DARK) - c) * np.clip((P[:, 0] - 664) / 6, 0, 1)[:, None] * (P[:, 1] > 326)[:, None]
    tail = np.clip((338 - P[:, 0]) / 40, 0, 1) * np.clip((P[:, 1] - 470) / 30, 0, 1)
    tt = np.hypot(P[:, 0] - TAIL_TIP[0], P[:, 1] - TAIL_TIP[1])
    tc = np.array(SLATE) + (np.array(DARK) - SLATE) * np.clip(1 - abs(tt - 52) / 22, 0, 1)[:, None]
    tc = tc + (np.array(CREAM) - tc) * np.clip((24 - tt) / 10, 0, 1)[:, None]
    c = c + (tc - c) * tail[:, None]
    return np.clip(c, 0, 255)


def wing_colors(P, upper):
    dn, _ = seg_dist(P, *SHOULDER)
    dn = -dn                                                     # above the hinge = positive
    f = np.clip((dn - 150) / 150, 0, 1)
    if upper:
        c = np.array(RUFOUS) + (np.array(DARK) - RUFOUS) * f[:, None]
        rng = np.random.default_rng(7)
        c *= (0.85 + 0.15 * rng.random(len(P)))[:, None]
    else:
        c = np.array(BUFF) + (np.array((96, 90, 100)) - BUFF) * f[:, None]
    return np.clip(c, 0, 255)


def build():
    """Parts: dict name -> (V (n,3) model units, N (n,3), C (n,3) 0..255, T (m,3))."""
    from scipy.ndimage import distance_transform_edt, gaussian_filter
    from scipy.ndimage import binary_dilation
    m = raster(SHAPE)
    Y, X = np.mgrid[0:S, 0:S]
    (ax, ay), (bx, by) = SHOULDER
    wing = largest(m & (Y < ay + (X - ax) * (by - ay) / (bx - ax)) & (X < bx))
    legs = largest(m & ~wing & (Y > HIP[1]) & (X > 380))
    body = largest(m & ~wing & ~legs)
    to_model = lambda P: np.stack([P[:, 0] - S / 2, S / 2 - P[:, 1]], 1)
    parts = {}

    def solid(mask, tol, spacing, blur, k, colors, extra=None):
        P, T, nr = triangulate(mask, tol, spacing)
        h = gaussian_filter(distance_transform_edt(mask), blur) * k * mask
        z = sample(h, P)
        if extra is not None:
            z *= extra(P)
        n = len(P); inner = np.arange(nr, n)
        # front: all points; back: the inner points mirrored (ring shared, z = 0)
        V = np.concatenate([np.c_[to_model(P), np.where(np.arange(n) < nr, 0, z)],
                            np.c_[to_model(P[nr:]), -z[nr:]]])
        remap = np.arange(n); remap[nr:] = n + np.arange(n - nr)
        T2 = np.concatenate([T, remap[T][:, ::-1]])
        C = np.concatenate([colors(P), colors(P[nr:])])
        return V, T2, C

    tailflat = lambda P: 1 - 0.55 * np.clip((345 - P[:, 0]) / 60, 0, 1) * np.clip((P[:, 1] - 470) / 40, 0, 1)
    V, T, C = solid(body, 3.0, 34, 9, 0.95, body_colors, tailflat)
    # eyes: small dark discs on the head surface, both sides
    hb = gaussian_filter(distance_transform_edt(body), 9) * 0.95
    ez = float(sample(hb, np.array([EYE]))[0]) + 1.5
    for side in (1, -1):
        base = len(V)
        ring = [(EYE[0] + EYE_R * math.cos(a), EYE[1] + EYE_R * math.sin(a)) for a in np.linspace(0, 2 * math.pi, 9)[:-1]]
        pts = np.array([EYE] + ring)
        V = np.concatenate([V, np.c_[to_model(pts), np.full(len(pts), side * ez)]])
        C = np.concatenate([C, np.array([(20, 18, 22)] + [(36, 32, 30)] * 8, float)])
        tri = [(base, base + 1 + (i + 1) % 8, base + 1 + i) for i in range(8)]   # y down -> ccw y up
        if side < 0:
            tri = [t[::-1] for t in tri]
        T = np.concatenate([T, np.array(tri)])
    parts['body'] = (V, T, C)

    Pw, Tw, _ = triangulate(binary_dilation(wing, iterations=7) & m, 2.5, 52)
    for name, dz, front_upper in (('wing_near', WING_DZ, True), ('wing_far', -WING_DZ, False)):
        n = len(Pw)
        V = np.concatenate([np.c_[to_model(Pw), np.full(n, dz)]] * 2)
        T = np.concatenate([Tw, Tw[:, ::-1] + n])
        C = np.concatenate([wing_colors(Pw, front_upper), wing_colors(Pw, not front_upper)])
        parts[name] = (V, T, C, np.r_[np.tile([0, 0, 1.0], (n, 1)), np.tile([0, 0, -1.0], (n, 1))])

    legcol = lambda P: np.array(YELLOW) + (np.array(DARK) - YELLOW) * np.clip((P[:, 1] - 640) / 14, 0, 1)[:, None]
    parts['legs'] = solid(binary_dilation(legs, iterations=6) & m, 2.0, 28, 4, 0.8, legcol)

    out = {}
    for k, p in parts.items():
        V, T, C = p[:3]
        if len(p) == 4:
            N = p[3]
        else:
            N = np.zeros_like(V)
            fn = np.cross(V[T[:, 1]] - V[T[:, 0]], V[T[:, 2]] - V[T[:, 0]])
            for i in range(3):
                np.add.at(N, T[:, i], fn)
            N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-9)
        out[k] = (V.astype(float), N, C.astype(float), T.astype(int))
    return out


def model_pivots():
    tm = lambda p: np.array([p[0] - S / 2, S / 2 - p[1], 0.0])
    a, b = tm(SHOULDER[0]), tm(SHOULDER[1])
    return a, (b - a) / np.linalg.norm(b - a), tm(HIP)


# ================================================================== track
def rot_axis(u, a):
    u = np.asarray(u, float); c, s = math.cos(a), math.sin(a)
    K = np.array([[0, -u[2], u[1]], [u[2], 0, -u[0]], [-u[1], u[0], 0]])
    return np.eye(3) + s * K + (1 - c) * K @ K


def euler(yaw, pitch, roll):
    return rot_axis((0, 1, 0), yaw) @ rot_axis((0, 0, 1), pitch) @ rot_axis((1, 0, 0), roll)


def catmull(pts, n):
    P = np.array(pts, float)
    P = np.vstack([2 * P[0] - P[1], P, 2 * P[-1] - P[-2]])
    out = []
    for i in range(1, len(P) - 2):
        p0, p1, p2, p3 = P[i - 1], P[i], P[i + 1], P[i + 2]
        t0 = 0; t1 = t0 + np.linalg.norm(p1 - p0) ** 0.5; t2 = t1 + np.linalg.norm(p2 - p1) ** 0.5
        t3 = t2 + np.linalg.norm(p3 - p2) ** 0.5
        for t in np.linspace(t1, t2, n, endpoint=False):
            a1 = (t1 - t) / (t1 - t0) * p0 + (t - t0) / (t1 - t0) * p1
            a2 = (t2 - t) / (t2 - t1) * p1 + (t - t1) / (t2 - t1) * p2
            a3 = (t3 - t) / (t3 - t2) * p2 + (t - t2) / (t3 - t2) * p3
            b1 = (t2 - t) / (t2 - t0) * a1 + (t - t0) / (t2 - t0) * a2
            b2 = (t3 - t) / (t3 - t1) * a2 + (t - t1) / (t3 - t1) * a3
            out.append((t2 - t) / (t2 - t1) * b1 + (t - t1) / (t2 - t1) * b2)
    out.append(P[-2])
    return np.array(out)


def flight(pts, t0, t1, ts):
    """Positions and flight orientation (yaw, climb, bank) at times ts along the spline,
    slow at both ends."""
    C = catmull(pts, 200)
    L = np.r_[0, np.cumsum(np.linalg.norm(np.diff(C, axis=0), axis=1))]
    u = np.clip((ts - t0) / (t1 - t0), 0, 1)
    s = (0.5 - 0.5 * np.cos(np.pi * u)) * L[-1]
    pos = np.stack([np.interp(s, L, C[:, k]) for k in range(3)], 1)
    ds = 1e-3 * L[-1]
    ahead = np.stack([np.interp(np.minimum(s + ds, L[-1]), L, C[:, k]) for k in range(3)], 1)
    behind = np.stack([np.interp(np.maximum(s - ds, 0), L, C[:, k]) for k in range(3)], 1)
    v = ahead - behind
    yaw = np.unwrap(np.arctan2(-v[:, 2], v[:, 0]))
    climb = np.arctan2(v[:, 1], np.hypot(v[:, 0], v[:, 2]))
    dyaw = np.gradient(yaw, ts)
    bank = np.clip(-0.22 * dyaw, -0.9, 0.9)
    k = np.exp(-0.5 * (np.arange(-12, 13) / 5.0) ** 2); k /= k.sum()
    bank = np.convolve(np.pad(bank, 12, mode='edge'), k, 'valid')
    climb = np.convolve(np.pad(climb, 12, mode='edge'), k, 'valid')
    return pos, yaw, climb, bank


def track(outro=False):
    """One dict per 60 Hz frame: pos, R (3x3), scale, zs, theta, tuck, cm, lm, fade, ta, ty."""
    if not outro:
        dur, f0, f1 = 7.5, 1.8, 5.3
        pts = [REST, (0.8, 1.05, -0.8), (1.8, 1.25, -3.2), (0.0, 1.5, -5.5), (-2.4, 0.85, -3.0),
               (-1.8, 0.25, 0.8), (0.25, 0.3, 3.9), (1.9, 0.9, 1.6), (1.4, 1.4, -1.6),
               (-1.2, 1.1, -1.2), (-0.95, 0.8, -0.2), REST]
    else:
        dur, f0, f1 = 9.0, 0.0, 1.9
        pts = [(-6.5, 2.4, -3.5), (-3.6, 1.3, -1.2), (-2.6, 0.55, -0.25), OUTRO_REST]
    ts = np.arange(int(round(dur * FPS))) / FPS
    pos, yaw, climb, bank = flight(pts, f0, f1, ts)
    k = round(yaw[-1] / (2 * math.pi)); yaw_end = yaw - 2 * math.pi * k   # land facing right
    frames = []
    for i, t in enumerate(ts):
        if outro:
            w = 1 - smooth((t - (f1 - 0.55)) / 0.55)
            pre = 0.0
            cm = lm = zs = 1.0
            fade = smooth(t / 0.4) * (1 - smooth((t - (dur - 0.5)) / 0.5))
            sc = 1.0
        else:
            w = smooth((t - f0) / 0.4) * (1 - smooth((t - (f1 - 0.55)) / 0.55))
            pre = -0.45 * smooth((t - 1.0) / 0.8)
            cm = lm = smooth((t - 1.0) / 0.8)
            zs = smooth((t - 1.0) / 0.7)
            fade = smooth(t / 0.5) * (1 - smooth((t - (dur - 0.5)) / 0.5))
            sc = 0.94 + 0.06 * smooth(t / 0.6)
        y = yaw_end[i] if t > (f0 + f1) / 2 else yaw[i]
        rest_yaw = 0.0 if t > (f0 + f1) / 2 or outro else pre
        yy = lerp(rest_yaw, y, w)
        pitch = w * (climb[i] - TILT * 0.85)
        R = euler(yy, pitch, w * bank[i])
        p = pos[i] if (f0 <= t <= f1) else (np.array(REST if not outro else OUTRO_REST) if t > f1 else pos[0])
        # wings: logo pose (0) at rest, beating in flight, gliding when descending
        ph = 2 * math.pi * 3.1 * (t - f0)
        desc = np.clip(-climb[i] * 3, 0, 1)
        beat = (1.35 + lerp(0.75, 0.15, desc) * math.sin(ph))
        flap = smooth((t - (f0 - 0.25)) / 0.35) * (1 - smooth((t - (f1 - 0.45)) / 0.45))
        theta = flap * beat
        if outro and t > f1:                                    # perched: a slow wing stretch
            theta = 0.18 * smooth((t - 4.2) / 0.6) * (1 - smooth((t - 5.6) / 0.6))
        tuck = -1.15 * w
        if outro:
            ta = smooth((t - 1.6) / 0.5)
        else:
            ta = smooth((t - 5.25) / 0.5)
        frames.append(dict(t=t, pos=np.asarray(p, float), R=R, scale=sc * SCALE, zs=max(zs, 0.0),
                           theta=theta, tuck=tuck, cm=cm, lm=lm, fade=fade, ta=ta, ty=1 - ta))
    return frames


def part_matrices(fr):
    """4x4 world matrix (column vectors) of each part, and its rotation (no scale) for lights."""
    a, u, hip = model_pivots()
    W = np.eye(4); W[:3, :3] = fr['R'] @ np.diag([fr['scale'], fr['scale'], fr['scale'] * fr['zs']])
    W[:3, 3] = fr['pos']
    out = {}
    for name in PARTS:
        L = np.eye(4); Rl = np.eye(3)
        if name.startswith('wing'):
            Rl = rot_axis(u, fr['theta'] if name == 'wing_near' else -fr['theta'])
            L[:3, :3] = Rl; L[:3, 3] = a - Rl @ a
        elif name == 'legs':
            Rl = rot_axis((0, 0, 1), fr['tuck'])
            L[:3, :3] = Rl; L[:3, 3] = hip - Rl @ hip
        out[name] = (W @ L, fr['R'] @ Rl)
    return out


def camera(aspect):
    f = 1 / math.tan(FOVY / 2)
    P = np.array([[f / aspect, 0, 0, 0], [0, f, 0, 0],
                  [0, 0, (FAR + NEAR) / (NEAR - FAR), 2 * FAR * NEAR / (NEAR - FAR)], [0, 0, -1, 0]])
    e, at = np.array(EYE_POS), np.array(AT)
    fw = (at - e) / np.linalg.norm(at - e)
    s = np.cross(fw, (0, 1, 0)); s /= np.linalg.norm(s)
    up = np.cross(s, fw)
    V = np.eye(4); V[0, :3], V[1, :3], V[2, :3] = s, up, -fw
    V[:3, 3] = -V[:3, :3] @ e
    return P @ V


def light_terms(fr):
    lm, fade = fr['lm'], fr['fade']
    amb = [lerp(255, a, lm) * fade for a in AMB]
    lights = [(d, [c * lm * fade for c in col]) for d, col in LIGHTS]
    return amb, lights


# ================================================================== software renderer (videos)
def background(W, H, glow_xy):
    y = np.linspace(0, 1, H)[:, None]
    bg = np.array(BG_TOP) + (np.array(BG_BOT) - BG_TOP) * y[..., None]
    gx, gy = glow_xy
    X, Y = np.meshgrid(np.arange(W), np.arange(H))
    r = np.hypot(X - gx, Y - gy) / (0.62 * H)
    prof = np.interp(r, [0, 0.45, 1], [1, 0.42, 0])
    return bg + np.array(GLOW) * prof[..., None]


def screen_of(world, W, H, VP):
    c = VP @ np.r_[world, 1.0]
    return ((c[0] / c[3]) * 0.5 + 0.5) * W, (0.5 - 0.5 * (c[1] / c[3])) * H


def raster_tris(img, zb, X, Y, Z, C):
    """Gouraud + Z (smaller = nearer), counter-clockwise on screen y-up = front."""
    H, W = zb.shape
    for i in range(len(X)):
        x, y, z, c = X[i], Y[i], Z[i], C[i]
        area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])
        if area >= -1e-6:                 # y down: front faces have negative area
            continue
        x0, x1 = max(int(min(x)), 0), min(int(max(x)) + 1, W)
        y0, y1 = max(int(min(y)), 0), min(int(max(y)) + 1, H)
        if x0 >= x1 or y0 >= y1:
            continue
        py, px = np.mgrid[y0:y1, x0:x1] + 0.5
        w0 = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area
        w1 = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area
        w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue
        zz = w0 * z[0] + w1 * z[1] + w2 * z[2]
        sub = zb[y0:y1, x0:x1]
        ok = inside & (zz < sub)
        sub[ok] = zz[ok]
        col = w0[..., None] * c[0] + w1[..., None] * c[1] + w2[..., None] * c[2]
        img[y0:y1, x0:x1][ok] = col[ok]


def render(model, fr, W, H, ss=2, bg=None):
    w, h = W * ss, H * ss
    VP = camera(W / H)
    img = (bg.copy() if bg is not None else np.zeros((h, w, 3)))
    img *= fr['fade']
    zb = np.full((h, w), np.inf)
    amb, lights = light_terms(fr)
    mats = part_matrices(fr)
    for name in PARTS:
        V, N, C, T = model[name]
        M, R = mats[name]
        clip = (VP @ M @ np.c_[V, np.ones(len(V))].T).T
        sx = (clip[:, 0] / clip[:, 3] * 0.5 + 0.5) * w
        sy = (0.5 - 0.5 * clip[:, 1] / clip[:, 3]) * h
        sz = clip[:, 2] / clip[:, 3]
        col = SIL + (C - SIL) * fr['cm']
        Nw = N @ R.T
        k = np.array(amb, float)[None, :].repeat(len(V), 0)
        for d, lc in lights:
            d = np.array(d) / np.linalg.norm(d)
            k += np.maximum(Nw @ d, 0)[:, None] * np.array(lc)
        rgb = np.clip(col * np.minimum(k / 255.0, 2.0), 0, 255)
        raster_tris(img, zb, sx[T], sy[T], sz[T], rgb[T])
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    return im.resize((W, H), Image.LANCZOS) if ss > 1 else im


def text_layer(im, fr, outro):
    W, H = im.size
    a = fr['ta'] * fr['fade']
    if a <= 0:
        return im
    over = Image.new('RGBA', im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    if not outro:
        size = int(H * 0.115)
        f = ImageFont.truetype(FONT, size)
        w1, w2 = d.textlength(TITLE, font=f), d.textlength(TITLE2, font=f)
        x = (W - w1 - w2) / 2
        y = H * TITLE_Y + fr['ty'] * H * 0.03
        sw = max(1, size // 40)
        d.text((x, y), TITLE, font=f, fill=TITLE_RGB + (255,), stroke_width=sw, stroke_fill=TITLE_RGB + (255,))
        d.text((x + w1, y), TITLE2, font=f, fill=TITLE2_RGB + (255,), stroke_width=sw, stroke_fill=TITLE2_RGB + (255,))
    else:
        lines = [('Thanks for watching', 0.062, TITLE_RGB, 0.0),
                 ('Support Kestrel64 on Patreon', 0.085, TITLE2_RGB, 0.35),
                 (PATREON, 0.07, TITLE_RGB, 0.7)]
        x = W * 0.43
        y = H * 0.30
        # shrink the block so its widest line ends 6% short of the right edge
        k = min(1.0, W * 0.51 / max(d.textlength(s, font=ImageFont.truetype(FONT, int(H * sz)))
                                    for s, sz, _, _ in lines))
        lines = [(s, sz * k, col, delay) for s, sz, col, delay in lines]
        y += H * 0.085 * (1 - k) * 1.5
        for s, sz, col, delay in lines:
            t = smooth((fr['t'] - 1.6 - delay) / 0.5) * fr['fade']
            f = ImageFont.truetype(FONT, int(H * sz))
            sw = max(1, int(H * sz) // 40)
            d.text((x + (1 - t) * H * 0.04, y), s, font=f, fill=col + (int(255 * t),),
                   stroke_width=sw, stroke_fill=col + (int(255 * t),))
            y += H * sz * 1.55
        a = 1.0
    if not outro:
        al = over.getchannel('A').point(lambda v: int(v * a))
        over.putalpha(al)
    base = im.convert('RGBA')
    base.alpha_composite(over)
    return base.convert('RGB')


def frame_image(model, fr, W, H, outro, bg):
    im = render(model, fr, W, H, 2, bg)
    return text_layer(im, fr, outro)


def glow_center(W, H, outro):
    VP = camera(W / H)
    rest = np.array(OUTRO_REST if outro else REST)
    return screen_of(rest, W * 2, H * 2, VP)


def find_ffmpeg(path):
    if path:
        return path
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        pass
    p = os.path.expanduser(r'~\AppData\Roaming\Python\Python311\site-packages\imageio_ffmpeg\binaries')
    if os.path.isdir(p):
        for f in os.listdir(p):
            if f.startswith('ffmpeg'):
                return os.path.join(p, f)
    return 'ffmpeg'


# ================================================================== ROM data
def pack_normal(n):
    t = lambda v, s, lo, hi: max(lo, min(hi, int(v * s)))
    x, y, z = t(n[0], 15.5, -16, 15), t(n[1], 31.5, -32, 31), t(n[2], 15.5, -16, 15)
    return ((x & 31) << 11) | ((y & 63) << 5) | (z & 31)


def chunk(T, maxv=32):
    order = np.argsort(T.min(1))
    chunks, cur, tris = [], {}, []
    for i in order:
        t = T[i]
        new = [v for v in dict.fromkeys(t.tolist()) if v not in cur]
        if len(cur) + len(new) > maxv:
            chunks.append((list(cur), tris)); cur, tris = {}, []
            new = list(dict.fromkeys(t.tolist()))
        for v in new:
            cur[v] = len(cur)
        tris.append([cur[v] for v in t])
    if tris:
        chunks.append((list(cur), tris))
    return chunks


def emit_c(path):
    model = build()
    vtx, rgb, tri, ch = [], [], [], []
    for pi, name in enumerate(PARTS):
        V, N, C, T = model[name]
        for vs, ts in chunk(T):
            ch.append((pi, len(vtx), len(vs), len(tri)))
            for v in vs:
                p = np.round(V[v]).astype(int)
                vtx.append((p[0], p[1], p[2], pack_normal(N[v])))
                rgb.append(tuple(int(round(c)) for c in C[v]))
            tri += [tuple(t) for t in ts]
    frames = track(False)
    a, u, hip = model_pivots()
    import tempfile
    sys.path.insert(0, HERE)
    import k64font
    fd, tmp = tempfile.mkstemp(suffix='.k64f'); os.close(fd)
    k64font.bake(FONT, tmp, 28, ','.join(str(ord(c)) for c in sorted(set(TITLE + TITLE2 + '?'))))
    font = open(tmp, 'rb').read(); os.unlink(tmp)
    L = []
    w = L.append
    w('// Generated by tools/k64logo.py — do not edit.')
    w('#define LOGO_NVTX %d' % len(vtx))
    w('#define LOGO_NTRI %d' % len(tri))
    w('#define LOGO_NCHUNK %d' % len(ch))
    w('#define LOGO_NFRAMES %d' % len(frames))
    w('#define LOGO_SCALE %.9ff' % SCALE)
    w('#define LOGO_TITLE_Y %.4ff' % TITLE_Y)
    w('#define LOGO_REST_Y %.4ff' % REST[1])
    for nm, v in (('LOGO_HINGE', a), ('LOGO_AXIS', u), ('LOGO_HIP', hip)):
        w('static const float %s[3] = { %.6ff, %.6ff, %.6ff };' % (nm, v[0], v[1], v[2]))
    w('static const float LOGO_EYE[3] = { %.4ff, %.4ff, %.4ff }, LOGO_AT[3] = { %.4ff, %.4ff, %.4ff };' % (EYE_POS + AT))
    w('static const float LOGO_FOVY = %.6ff, LOGO_NEAR = %.3ff, LOGO_FAR = %.3ff;' % (FOVY, NEAR, FAR))
    w('static const u8 LOGO_SIL[3] = { %d, %d, %d }, LOGO_AMB[3] = { %d, %d, %d };' % (SIL + AMB))
    w('static const u8 LOGO_BG[3][3] = { { %d, %d, %d }, { %d, %d, %d }, { %d, %d, %d } };' % (BG_TOP + BG_BOT + GLOW))
    w('static const float LOGO_LDIR[%d][3] = {' % len(LIGHTS))
    for d, _ in LIGHTS:
        w('  { %.4ff, %.4ff, %.4ff },' % d)
    w('};')
    w('static const u8 LOGO_LCOL[%d][3] = {' % len(LIGHTS))
    for _, c in LIGHTS:
        w('  { %d, %d, %d },' % c)
    w('};')
    w('static const u32 LOGO_TITLE_RGBA = 0x%02X%02X%02XFF, LOGO_TITLE2_RGBA = 0x%02X%02X%02XFF;' % (TITLE_RGB + TITLE2_RGB))
    w('static const char LOGO_TITLE[] = "%s\\x02%s";' % (TITLE, TITLE2))
    w('// part, first vertex, vertex count, first triangle (count = next chunk\'s first - this)')
    w('static const u16 logo_chunk[LOGO_NCHUNK + 1][4] = {')
    for c in ch:
        w('  { %d, %d, %d, %d },' % c)
    w('  { 0, %d, 0, %d },' % (len(vtx), len(tri)))
    w('};')
    w('static k64_vtx_t logo_vtx[LOGO_NVTX] K64_ALIGN(16) = {')
    for (x, y, z, n) in vtx:
        w('  { %d, %d, %d, 0x%04X, %d, %d, %d, 255, 0, 0 },' % (x, y, z, n, *SIL))
    w('};')
    w('static const u8 logo_rgb[LOGO_NVTX][3] = {')
    for i in range(0, len(rgb), 8):
        w('  ' + ' '.join('{ %d, %d, %d },' % c for c in rgb[i:i + 8]))
    w('};')
    w('static const u8 logo_tri[LOGO_NTRI][3] = {')
    for i in range(0, len(tri), 10):
        w('  ' + ' '.join('{ %d, %d, %d },' % t for t in tri[i:i + 10]))
    w('};')
    w('// per 60 Hz frame: quaternion (w, x, y, z) * 16384, position * 4096, scale * 16384,')
    w('// z scale, wing angle, leg angle (rad * 4096), color mix, light mix, fade, title (* 255)')
    w('static const s16 logo_track[LOGO_NFRAMES][15] = {')
    for fr in frames:
        q = quat(fr['R'])
        vals = [round(v * 16384) for v in q] + [round(v * 4096) for v in fr['pos']]
        vals += [round(fr['scale'] / SCALE * 16384), round(fr['zs'] * 16384), round(fr['theta'] * 4096),
                 round(fr['tuck'] * 4096), round(fr['cm'] * 255), round(fr['lm'] * 255),
                 round(fr['fade'] * 255), round(fr['ta'] * 255)]
        w('  { ' + ', '.join(str(int(v)) for v in vals) + ' },')
    w('};')
    w('static const u8 logo_font[%d] K64_ALIGN(8) = {' % len(font))
    for i in range(0, len(font), 24):
        w('  ' + ' '.join('0x%02X,' % b for b in font[i:i + 24]))
    w('};')
    open(path, 'w', newline='\n').write('\n'.join(L) + '\n')
    print('k64logo: %d vertices, %d triangles, %d chunks, %d frames, font %d B -> %s'
          % (len(vtx), len(tri), len(ch), len(frames), len(font), path))


def quat(R):
    w = math.sqrt(max(0.0, 1 + R[0, 0] + R[1, 1] + R[2, 2])) / 2
    x = math.copysign(math.sqrt(max(0.0, 1 + R[0, 0] - R[1, 1] - R[2, 2])) / 2, R[2, 1] - R[1, 2])
    y = math.copysign(math.sqrt(max(0.0, 1 - R[0, 0] + R[1, 1] - R[2, 2])) / 2, R[0, 2] - R[2, 0])
    z = math.copysign(math.sqrt(max(0.0, 1 - R[0, 0] - R[1, 1] + R[2, 2])) / 2, R[1, 0] - R[0, 1])
    q = np.array([w, x, y, z]); return q / np.linalg.norm(q)


# ================================================================== CLI
def main():
    ap = argparse.ArgumentParser(description='kestrel64-sdk logo generator')
    sp = ap.add_subparsers(dest='cmd', required=True)
    c = sp.add_parser('c'); c.add_argument('out')
    for name in ('video', 'still'):
        v = sp.add_parser(name); v.add_argument('out')
        v.add_argument('--outro', action='store_true')
        v.add_argument('--size', default='1920x1080')
        v.add_argument('--t', type=float, default=0.0)
        v.add_argument('--fps', type=int, default=FPS)
        v.add_argument('--ffmpeg')
    a = ap.parse_args()
    if a.cmd == 'c':
        return emit_c(a.out)
    W, H = map(int, a.size.lower().split('x'))
    model = build()
    frames = track(a.outro)
    bg = background(W * 2, H * 2, glow_center(W, H, a.outro))
    if a.cmd == 'still':
        fr = frames[min(int(round(a.t * FPS)), len(frames) - 1)]
        frame_image(model, fr, W, H, a.outro, bg).save(a.out)
        return
    step = FPS / a.fps
    cmd = [find_ffmpeg(a.ffmpeg), '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
           '-s', '%dx%d' % (W, H), '-r', str(a.fps), '-i', '-', '-c:v', 'libx264', '-preset', 'slow',
           '-crf', '14', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', a.out]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    n = int(len(frames) / step)
    for i in range(n):
        p.stdin.write(frame_image(model, frames[int(i * step)], W, H, a.outro, bg).tobytes())
        if i % 60 == 0:
            print('k64logo: frame %d/%d' % (i, n), flush=True)
    p.stdin.close()
    sys.exit(p.wait())


if __name__ == '__main__':
    main()
