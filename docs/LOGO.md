# Kestrel64 boot logo

Every ROM built with `k64_add_rom` plays the Kestrel64 logo before `main()`: the 2D bird
silhouette on a night-blue background turns into a lit 3D kestrel, flies a loop around the
camera, lands, and the `KESTREL64 SDK` title fades in. It lasts 7.5 s; START, A or B skip it
(0.4 s fade out). When it returns, every memory zone is reset, so `main()` starts from the
same state as without it.

## Opting out

- one ROM: `k64_add_rom(NAME foo ... NO_LOGO)`
- the whole build: `cmake -DK64_LOGO=OFF`

crt0 calls `k64_logo_play` through a weak reference, so a ROM without the logo object just
skips it.

## Files

| File | What |
|------|------|
| `tools/k64logo.py` | single source: bird outline, mesh build, animation track, video renderer, C emitter |
| `src/logo/logo_data.h` | generated: mesh (877 vertices, 964 triangles), 450-frame track, baked font |
| `src/logo/logo.c` | player: kgfx (RSP) mesh with 3 lights, gradient background mesh, font title |
| `include/k64/logo.h` | `k64_logo_play()` |

The bird is split in body (inflated by a distance field), two flat wings hinged at the
shoulder, and the legs (tucked during flight). Each frame of the track holds the body
rotation (quaternion), position, scale, the morph factors (flat white -> colors, unlit ->
lit), the wing angle, the leg tuck and the fades.

## Regenerating

    python -I tools/k64logo.py c src/logo/logo_data.h

## YouTube videos

The same model and track render offline (numpy rasterizer, 2x supersampling, ffmpeg):

    python -I tools/k64logo.py video intro.mp4            # 7.5 s, 1920x1080, 60 fps
    python -I tools/k64logo.py video outro.mp4 --outro    # 9 s, "Support Kestrel64 on Patreon"
    python -I tools/k64logo.py still frame.png --t 4.0 [--outro]

`--size WxH`, `--fps N` and `--ffmpeg PATH` change the defaults. The videos have no audio
track.
