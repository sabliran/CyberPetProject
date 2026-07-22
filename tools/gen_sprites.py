#!/usr/bin/env python3
"""Regenerates CyberPet/sprites.c — pet body sprites as LVGL-v8 C arrays.

Since July 2026 the pet is SM01 "Smol" (the little TV-headed robot from
Unsignal). Art lives in tools/art/*.aseprite (32x40 canvas) and is parsed
directly by this script — no Aseprite install needed. The egg stage keeps
the original procedural pixel egg so hatching stays a reveal.

All three legged stages use the SAME Smol art at integer scales
(4x / 5x / 6x) inside the stageSize() canvases (165 / 205 / 245):
evolution = Smol grows. Frames:
  base  — smol.aseprite frame 4, the BLACK-SCREEN idle. The firmware's
          animated eyes render on the dark TV screen (ui.cpp places them
          at eyeOffY = -sz/10, which is where this script pins the screen
          center), so blinks / squints / droopy moods play on the CRT.
  walk1/walk2 — walkDown.aseprite frames 1 and 3 (right arm swings down /
          up), with their static-filled screens painted black to match.
  back1/back2 — back.aseprite frames 1 and 3, shown when a roam glide
          shrinks the depth zoom (walking away from the viewer); the
          firmware hides the eyes while these are up. No mirrored variants:
          the back silhouette is near-symmetric and the flash budget goes
          to the side view instead.
  side1/side2 — walk.aseprite frames 1 and 3 (profile gait), shown for
          mostly-horizontal glides. The art faces RIGHT natively, so the
          r variants are the originals and the plain ones are mirrored —
          matching the walk/back convention that r = moving right. Eyes
          hidden here too (the screen isn't visible in profile).

Frames are aligned to each other by the TV rim so the screen (and the
eyes on it) stays put while the feet/cape bob during the walk cycle.

Usage: python3 tools/gen_sprites.py   (from the repo root; needs Pillow)
Then copy sprites.c to CyberPet_1_75B/ and rebuild.
"""
import math, os, struct, zlib
from PIL import Image

ART = os.path.join(os.path.dirname(__file__), 'art')

# ---- minimal .aseprite reader (32-bit RGBA files only) --------------------

def load_ase_frames(path):
    """Returns the file's frames as flattened RGBA images (visible layers,
    top-down composite). Frame 0 of these files is empty — callers index
    from the aseprite timeline as-is."""
    d = open(path, 'rb').read()
    _, magic, nframes, w, h, depth = struct.unpack_from('<IHHHHH', d, 0)
    assert magic == 0xA5E0, f'not an aseprite file: {path}'
    assert depth == 32, f'{path}: only RGBA aseprite supported (depth={depth})'
    off, layers, frames = 128, [], []
    for _ in range(nframes):
        fbytes, fmagic, old_nchunks, _dur = struct.unpack_from('<IHHH', d, off)
        assert fmagic == 0xF1FA
        nchunks = struct.unpack_from('<I', d, off + 12)[0] or old_nchunks
        coff, cels = off + 16, []
        for _ in range(nchunks):
            csize, ctype = struct.unpack_from('<IH', d, coff)
            body = d[coff + 6 : coff + csize]
            if ctype == 0x2004:  # layer: keep flags for visibility
                layers.append(struct.unpack_from('<H', body, 0)[0])
            elif ctype == 0x2005:  # cel
                li, cx, cy, _op, celtype = struct.unpack_from('<HhhBH', body, 0)
                if celtype in (0, 2):
                    cw, ch = struct.unpack_from('<HH', body, 16)
                    raw = body[20:]
                    if celtype == 2:
                        raw = zlib.decompress(raw)
                    cels.append((li, cx, cy, Image.frombytes('RGBA', (cw, ch), raw)))
                elif celtype == 1:  # linked cel: reuse an earlier frame's image
                    src = struct.unpack_from('<H', body, 16)[0]
                    cels += [c for c in frames[src][1] if c[0] == li]
            coff += csize
        frames.append((None, cels))
        off += fbytes
    out = []
    for _, cels in frames:
        canvas = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        for li, cx, cy, im in sorted(cels, key=lambda c: c[0]):
            if li < len(layers) and not (layers[li] & 1):
                continue  # hidden layer
            canvas.alpha_composite(im, (cx, cy))
        out.append(canvas)
    return out

# ---- Smol frame prep ------------------------------------------------------

def tv_rim_anchor(im):
    """(center_x, top_y) of the TV rim: first row with a solid run of
    near-black pixels (the antenna is black too but only 1-2 px wide) and
    that run's center. Center-based so frames whose rim is drawn a pixel
    wider (the back view) still align to the same head position."""
    px = im.load()
    for y in range(im.height):
        xs = [x for x in range(im.width)
              if px[x, y][3] > 128 and max(px[x, y][:3]) < 50]
        if len(xs) >= 10:
            return (min(xs) + max(xs)) / 2.0, y
    raise SystemExit('no TV rim found in frame')

def head_bottom(im, from_y):
    """Last row of the contiguous wide-black band starting at the rim top —
    i.e. the bottom edge of the TV head (screen rows count too: they're
    black in the frames we use). Body rows never have a >=10 px black run."""
    px = im.load()
    y = from_y
    while y + 1 < im.height and sum(
            1 for x in range(im.width)
            if px[x, y + 1][3] > 128 and max(px[x, y + 1][:3]) < 50) >= 10:
        y += 1
    return y

def smol_frames():
    """Returns ({name: frame}, screen_center) in 32x40 sprite coords.
    Frames: base, walk1/walk2 (front), back1/back2 (walking away),
    side1/side2 (profile), with body-mirrored r variants of walk and side
    (r = moving right; back has no r — near-symmetric from behind)."""
    idle = load_ase_frames(os.path.join(ART, 'smol.aseprite'))
    walk = load_ase_frames(os.path.join(ART, 'walkDown.aseprite'))
    back = load_ase_frames(os.path.join(ART, 'back.aseprite'))
    side = load_ase_frames(os.path.join(ART, 'walk.aseprite'))
    base, white = idle[4], idle[5]  # black-screen blink frame / white-screen flash

    # Screen interior from the white-screen frame — the one frame where the
    # screen is a single unambiguous color (static shares colors with the cape).
    wp = white.load()
    pts = [(x, y) for y in range(white.height) for x in range(white.width)
           if wp[x, y][3] > 128 and min(wp[x, y][:3]) > 180]
    sx0, sy0 = min(p[0] for p in pts), min(p[1] for p in pts)
    sx1, sy1 = max(p[0] for p in pts), max(p[1] for p in pts)
    screen_center = ((sx0 + sx1 + 1) / 2.0, (sy0 + sy1 + 1) / 2.0)

    # Animation frames draw their own head (taller screen, antenna wiggle,
    # bob) — the pet's head must NOT change within a view, so each pose
    # keeps only its body and gets a fixed head (antenna + TV, everything
    # down to the rim's bottom edge) pasted on: the idle frame's head for
    # front poses, the first back frame's head for back poses.
    #
    # The body sway/cape trail in the art is directional (trails right =
    # correct when gliding LEFT). Every pose is also emitted body-mirrored
    # (suffix r, for rightward glides). Only the body flips: the head —
    # with its off-center antenna — stays identical in every frame.
    ax, ay = tv_rim_anchor(base)
    base_hb = head_bottom(base, ay)

    def align(f):
        """Shift a frame so its TV rim sits exactly on the base frame's."""
        fx, fy = tv_rim_anchor(f)
        aligned = Image.new('RGBA', f.size, (0, 0, 0, 0))
        aligned.paste(f, (round(ax - fx), ay - fy), f)
        return aligned

    def poses(frame_a, frame_b, head_src, black_only_erase=False):
        """(a, b, a_mirrored, b_mirrored) with the same head on all four.
        black_only_erase: clear just the near-black (head) pixels in the
        head rows instead of whole rows — the side view's cape rises up
        beside the head and must survive the head swap."""
        head_hb = head_bottom(head_src, ay)
        head = head_src.crop((0, 0, head_src.width, head_hb + 1))
        out = []
        for f in (frame_a, frame_b):
            body = align(f)
            erase_to = max(head_hb, head_bottom(body, ay))
            bp = body.load()
            for y in range(0, erase_to + 1):     # drop the frame's own head
                for x in range(body.width):
                    if black_only_erase and not (
                            bp[x, y][3] > 128 and max(bp[x, y][:3]) < 50):
                        continue
                    bp[x, y] = (0, 0, 0, 0)
            # Mirror about the screen's vertical axis so the flipped body
            # stays centered under the (unchanged) head. Continuous
            # reflection about x=scx maps column x to (2*scx - 1 - x);
            # FLIP_LEFT_RIGHT gives (W - 1 - x), so shift by 2*scx - W.
            mdx = round(2 * screen_center[0] - body.width)
            flipped = body.transpose(Image.FLIP_LEFT_RIGHT)
            mirrored = Image.new('RGBA', body.size, (0, 0, 0, 0))
            mirrored.paste(flipped, (mdx, 0), flipped)
            for b in (body, mirrored):
                b.alpha_composite(head, (0, 0))  # identical head on every frame
            out += [body, mirrored]
        return out[0], out[2], out[1], out[3]

    w1, w2, w1r, w2r = poses(walk[1], walk[3], base)          # right arm down / up
    b1, b2, _, _ = poses(back[1], back[3], align(back[1]))    # most distinct back poses
    # Side art faces right natively: the mirrored outputs are the LEFTWARD
    # frames and the originals become the r (rightward) variants.
    s1r, s2r, s1, s2 = poses(side[1], side[3], align(side[1]),
                             black_only_erase=True)  # opposite strides
    return {'base': base, 'walk1': w1, 'walk2': w2, 'walk1r': w1r, 'walk2r': w2r,
            'back1': b1, 'back2': b2,
            'side1': s1, 'side2': s2, 'side1r': s1r, 'side2r': s2r}, screen_center

def compose(frame, k, crop):
    """Crop to the shared union bbox and nearest-neighbor scale by k — no
    transparent padding: sprite bytes cost flash whether painted or not,
    so the canvas is exactly the art. The firmware reads each stage's
    canvas size and eye anchor from the generated sprite_layout.h."""
    cx0, cy0, cx1, cy1 = crop
    im = frame.crop((cx0, cy0, cx1 + 1, cy1 + 1))
    return im.resize((im.width * k, im.height * k), Image.NEAREST)

# ---- procedural egg (unchanged original) ----------------------------------

OUT, SH, HI, LN = 1, 2, 3, 4
PAL = {1: (150, 97, 62, 255), 2: (66, 120, 235, 255), 3: (150, 97, 62, 255),
       4: (92, 56, 36, 255), 5: (255, 208, 96, 255)}

def outline_and_shade(g, shade_from=0.7):
    h, w = len(g), len(g[0])
    o = [row[:] for row in g]
    ys = [y for y in range(h) if any(g[y])]
    y0, y1 = min(ys), max(ys)
    for y in range(h):
        for x in range(w):
            if g[y][x]:
                edge = any(not (0 <= y + dy < h and 0 <= x + dx < w and g[y + dy][x + dx])
                           for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)))
                if edge: o[y][x] = LN
                elif (y - y0) / (y1 - y0 + 1e-9) > shade_from: o[y][x] = SH
    return o

def render(g, px):
    """Paint the palette grid and scale by px, trimmed to the art bbox —
    like the Smol frames, no transparent padding."""
    h, w = len(g), len(g[0])
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    for y in range(h):
        for x in range(w):
            if g[y][x]: im.putpixel((x, y), PAL[g[y][x]])
    im = im.crop(im.getbbox())
    return im.resize((im.width * px, im.height * px), Image.NEAREST)

def put(g, pts, v=OUT):
    for x, y in pts:
        if 0 <= y < len(g) and 0 <= x < len(g[0]): g[y][x] = v

def egg_frame():
    egg = [[0] * 16 for _ in range(20)]
    for y in range(20):
        t = y / 19.0
        half = 6.8 * math.sin(math.pi * (0.18 + 0.82 * t) / 1.18) * (0.72 + 0.28 * t)
        for x in range(16):
            if abs(x - 7.5) <= half: egg[y][x] = OUT
    egg = outline_and_shade(egg, 0.75)
    put(egg, [(5, 6), (6, 7), (7, 6), (8, 7), (9, 6), (10, 7), (6, 8), (8, 8)], LN)
    return render(egg, 4)

# ---- assembly -------------------------------------------------------------

STAGES = (('blob', 4), ('creature', 5), ('evolved', 6))  # name, art scale

def union_crop(frames, scx):
    """Union bbox of all frames, symmetrized about the screen's vertical
    axis (x = scx) so mirrored frames share the crop and the screen center
    lands exactly at canvas W/2."""
    bx0 = min(f.getbbox()[0] for f in frames.values())
    by0 = min(f.getbbox()[1] for f in frames.values())
    bx1 = max(f.getbbox()[2] for f in frames.values()) - 1  # PIL bbox: exclusive
    by1 = max(f.getbbox()[3] for f in frames.values()) - 1
    ax = int(2 * scx) - 1  # continuous reflection: column x -> ax - x
    return min(bx0, ax - bx1), by0, max(bx1, ax - bx0), by1

def make_sprites():
    """Returns ([(name, image)], layouts) matching sprites.h / sprite_layout.h."""
    frames, (scx, scy) = smol_frames()
    crop = union_crop(frames, scx)
    egg = egg_frame()
    # Layout rows: w, h, eyeCx, eyeCy, grid, eyeW, eyeOffX, drift — indexed
    # by STAGE_*. Egg values reproduce the pre-trim eye behavior.
    layouts = [('egg', (egg.width, egg.height, egg.width // 2,
                        egg.height // 2 - egg.height // 10, 4, 5, 4, 2))]
    out = [('egg', egg)]
    for name, k in STAGES:
        base = compose(frames['base'], k, crop)
        eye_cx = round(k * (scx - crop[0]))
        assert eye_cx * 2 == base.width, 'screen axis must be the canvas center'
        layouts.append((name, (base.width, base.height, eye_cx,
                               round(k * (scy - crop[1])), k,
                               (5 * k) // 2, 2 * k, k)))
        out.append((name, base))
        for pose in ('walk1', 'walk2', 'walk1r', 'walk2r', 'back1', 'back2',
                     'side1', 'side2', 'side1r', 'side2r'):
            out.append((f'{name}_{pose}', compose(frames[pose], k, crop)))
    return out, layouts

def to_layout_h(layouts, hpath):
    rows = ',\n'.join(f'  {{ {", ".join(str(v) for v in vals)} }},  // {name}'
                      for name, vals in layouts)
    open(hpath, 'w').write(f'''\
// Auto-generated by tools/gen_sprites.py — do not hand-edit.
// Per-stage sprite geometry, indexed by STAGE_* (egg, blob, creature,
// evolved). Canvases are trimmed to the art (no transparent padding), so
// the widget size and the eye anchor (TV-screen center, px from the
// canvas top-left) come from here. grid = art pixel scale (eye snap),
// eyeW/eyeOffX/drift = eye sizing tuned to the screen at that scale.
#pragma once
typedef struct {{ int w, h, eyeCx, eyeCy, grid, eyeW, eyeOffX, drift; }} SpriteLayout;
static const SpriteLayout SPRITE_LAYOUT[4] = {{
{rows}
}};
''')

def to_c(sprites, cpath):
    out = ['// Auto-generated by tools/gen_sprites.py — do not hand-edit.',
           '// Format: LV_IMG_CF_TRUE_COLOR_ALPHA, 16-bit color, LV_COLOR_16_SWAP=1.',
           '#include "sprites.h"', '']
    for n, im in sprites:
        w, h = im.size
        b = bytearray()
        for y in range(h):
            for x in range(w):
                r, g, bl, a = im.getpixel((x, y))
                c = ((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3)
                b += bytes(((c >> 8) & 0xFF, c & 0xFF, a))
        out.append(f'static const uint8_t sprite_{n}_map[] = {{')
        for i in range(0, len(b), 24):
            out.append('  ' + ','.join(f'0x{v:02x}' for v in b[i:i + 24]) + ',')
        out.append('};')
        out.append(f'''const lv_img_dsc_t sprite_{n} = {{
  .header = {{ .cf = LV_IMG_CF_TRUE_COLOR_ALPHA, .always_zero = 0, .reserved = 0, .w = {w}, .h = {h} }},
  .data_size = sizeof(sprite_{n}_map),
  .data = sprite_{n}_map,
}};
''')
    open(cpath, 'w').write('\n'.join(out))

if __name__ == '__main__':
    root = os.path.join(os.path.dirname(__file__), '..')
    sprites, layouts = make_sprites()
    to_c(sprites, os.path.join(root, 'CyberPet', 'sprites.c'))
    to_layout_h(layouts, os.path.join(root, 'CyberPet', 'sprite_layout.h'))
    for name, (w, h, *_rest) in layouts:
        print(f'  {name}: {w}x{h}')
    print('wrote CyberPet/sprites.c + sprite_layout.h (sprites.h is hand-maintained)')
