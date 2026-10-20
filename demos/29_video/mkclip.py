#!/usr/bin/env python3
"""Synthetic test clip for demo 29: PNG frames + a mono WAV, all procedural.

    mkclip.py outdir [--frames 96] [--size 320x240] [--rate 22050]

Content chosen to exercise every macroblock mode: a static textured band (skip), a
scrolling gradient sky (inter), bouncing discs (inter/intra), a hard cut at frame 60
(intra in a P frame) and fine checker detail (high frequencies).
"""
import argparse, os, wave
import numpy as np
from PIL import Image

def frame(f, w, h):
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    if f < 60:
        sky = np.stack([40 + 60 * np.sin((xx + 3 * f) / 47.0) + yy * 0.3,
                        80 + 50 * np.sin((yy - 2 * f) / 31.0),
                        160 + 60 * np.cos((xx - yy + 4 * f) / 59.0)], -1)
    else:   # hard cut: a different scene
        sky = np.stack([200 - yy * 0.5, 120 + 80 * np.sin(xx / 13.0 + f * 0.2), 60 + yy * 0.3], -1)
    img = sky
    band = (yy >= h - 48)
    checker = (((xx // 4) + (yy // 4)) % 2) * 120 + 60
    img = np.where(band[..., None], np.stack([checker, checker * 0.8, 255 - checker], -1), img)
    for k in range(4):
        cx = (w / 2) + (w / 2 - 30) * np.sin(f * 0.07 * (k + 1) + k)
        cy = (h / 2 - 24) + (h / 2 - 60) * np.cos(f * 0.05 * (k + 2) + 2 * k)
        disc = (xx - cx) ** 2 + (yy - cy) ** 2 < (18 + 6 * k) ** 2
        col = np.array([[255, 60, 40], [40, 220, 90], [250, 230, 60], [240, 240, 255]][k], float)
        img = np.where(disc[..., None], col, img)
    return np.clip(img, 0, 255).astype(np.uint8)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('outdir')
    ap.add_argument('--frames', type=int, default=96)
    ap.add_argument('--size', default='320x240')
    ap.add_argument('--rate', type=int, default=22050)
    ap.add_argument('--fps', type=float, default=24.0)
    a = ap.parse_args()
    w, h = map(int, a.size.split('x'))
    fdir = os.path.join(a.outdir, 'frames')
    os.makedirs(fdir, exist_ok=True)
    for f in range(a.frames):
        Image.fromarray(frame(f, w, h)).save(os.path.join(fdir, 'f%04d.png' % f))
    # Audio: a note per half second (pentatonic), clicks at the cut.
    n = int(a.rate * a.frames / a.fps)
    t = np.arange(n) / a.rate
    notes = [261.6, 293.7, 329.6, 392.0, 440.0, 523.3]
    freq = np.array([notes[int(x * 2) % len(notes)] for x in t])
    env = 1 - (t * 2 % 1)
    s = 0.35 * np.sin(2 * np.pi * np.cumsum(freq) / a.rate) * env
    with wave.open(os.path.join(a.outdir, 'clip.wav'), 'wb') as wf:
        wf.setnchannels(1); wf.setsampwidth(2); wf.setframerate(a.rate)
        wf.writeframes((s * 32767).astype('<i2').tobytes())

if __name__ == '__main__':
    main()
