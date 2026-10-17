#!/usr/bin/env python3
"""Warp maps for CONscripter's cons_warp.dll transitions.

The cell format is in src/cons/ONScripter_warp.cpp and COMMANDS.md
("Transitions"). This tool writes maps that redo the retired cons_slide, cons_zoom,
cons_flip, cons_cube, cons_melt and cons_wave plugins, and renders a map
the slow way (floating point) to check the engine's output against.

  cons_warp.py make KIND PARAMS --size 800x600 [--grid 8] [--cells N] -o map.png
      KIND and PARAMS as the plugin's dll name: slide 0,-1,0; zoom 3,220,5;
      flip 0,0; cube 1,0; melt 71,7 (a seed is needed); wave 0,80,15.
      Prints the dll name to use: cons_warp.dll/map.png,N
  cons_warp.py render map.png CELLS PERCENT old.png new.png -o out.png
  cons_warp.py diff a.png b.png      largest channel difference, pixels over 1
  cons_warp.py test                  self-check

Needs numpy and Pillow.
"""
import argparse
import math
import sys

import numpy as np
from PIL import Image

OLD_X, OLD_Y, MASK, NEW_X, NEW_Y = range(5)
SMOOTH, BLACK = 128, 64  # a block's flags: bilinear sampling, black outside the picture


# --- transitions: each returns, at progress p for grid points at pixel
# indices X, Y (arrays), the source positions (pixel indices) in the old
# and the new picture, the mask, its sharpness (0: a level 0-255; 8: a
# signed distance in pixels, positive where the new picture shows) and
# the block flags (SMOOTH, BLACK; True is SMOOTH).

def edge_dist(X, Y, rx, ry, W, H, sx, sy):
    """Signed distance of pixel centres to a screen-sized rectangle at rx, ry
    moved in direction sx, sy from the screen: only its edges inside the
    screen count (so the distance stays linear for interpolation)."""
    ex = (X + 0.5 - rx) if sx > 0 else (rx + W - X - 0.5)
    ey = (Y + 0.5 - ry) if sy > 0 else (ry + H - Y - 0.5)
    return ex if not sy else ey if not sx else np.minimum(ex, ey)


def slide(p, X, Y, W, H, layer=0, dx=-1, dy=0):
    # a diagonal push leaves a black corner: outside the pictures reads black
    ox, oy = W * p, H * p
    ident = (X, Y)
    if layer == 0:
        rx, ry = -dx * (W - ox), -dy * (H - oy)
        return ident, (X - rx, Y - ry), edge_dist(X, Y, rx, ry, W, H, -dx, -dy), 8, False
    rx, ry = dx * ox, dy * oy
    old = (X - rx, Y - ry)
    if layer == 1:
        return old, ident, -edge_dist(X, Y, rx, ry, W, H, dx, dy), 8, False
    new = (X - rx + dx * W, Y - ry + dy * H)
    if not (dx and dy):
        return old, new, -edge_dist(X, Y, rx, ry, W, H, dx, dy), 8, False
    # diagonal: split by x alone, each picture's part off the other's rows
    # reads black (the plugin's black corners), so the mask stays straight
    return old, new, -edge_dist(X, Y, rx, ry, W, H, dx, 0), 8, BLACK


def zoom(p, X, Y, W, H, mode=0, scale=200, curve=1):
    rng = scale / 100 - 1
    curve = curve if curve > 0 else 1

    def z(s):
        k = 1 + rng * s
        return ((X + 0.5 - W / 2) / k + W / 2 - 0.5, (Y + 0.5 - H / 2) / k + H / 2 - 0.5)
    if mode == 1:
        s = p ** curve
        return z(s), (X, Y), np.full_like(X, 255 * s), 0, True
    if mode == 2:
        if p < 0.5:
            return z((2 * p) ** curve), (X, Y), np.zeros_like(X), 0, True
        return (X, Y), z((2 - 2 * p) ** curve), np.full_like(X, 255.0), 0, True
    s = (1 - p) ** curve
    old = z(p) if mode == 3 else (X, Y)
    return old, z(s), np.full_like(X, 255 * (1 - s)), 0, True


def squeeze(A, length, a, b):
    """Source along one axis for the whole picture drawn on a..b, and the
    signed distance to that span (positive inside)."""
    span = max(b - a, 1e-3)
    return (A + 0.5 - a) * length / span - 0.5, np.minimum(A + 0.5 - a, b - A - 0.5)


def flip(p, X, Y, W, H, axis=0, layer=0):
    length, A = (W, X) if axis == 0 else (H, Y)
    f = math.sin(p * math.pi / 2) if layer == 0 else math.cos(p * math.pi / 2)
    span = int(length * f + 0.5)  # as the plugin rounds it
    start = (length - span) // 2
    src, dist = squeeze(A, length, start, start + span)
    moving = (src, Y) if axis == 0 else (X, src)
    if layer == 0:
        return (X, Y), moving, dist, 8, False
    return moving, (X, Y), -dist, 8, False


def cube(p, X, Y, W, H, dx=1, dy=0):
    axis = 0 if dx != 0 else 1
    d = dx if axis == 0 else dy
    length, A = (W, X) if axis == 0 else (H, Y)
    t = p * math.pi / 2
    s, c = math.sin(t), math.cos(t)
    half = length / 2
    outer, edge, far = half * (1 - s - c), half * (1 + s - c), half * (1 + s + c)
    if d > 0:
        new, _ = squeeze(A, length, outer, edge)
        old, _ = squeeze(A, length, edge, far)
        dist = edge - (A + 0.5)
    else:
        new, _ = squeeze(A, length, length - edge, length - outer)
        old, _ = squeeze(A, length, length - far, length - edge)
        dist = (A + 0.5) - (length - edge)
    pos = (lambda v: (v, Y)) if axis == 0 else (lambda v: (X, v))
    return pos(old), pos(new), dist, 8, False


def melt_delay(W, seed):
    """The plugin's per-column delay (float32 as it computes it)."""
    r = seed & 0xffffffff
    pt = np.zeros(13, np.float32)
    for i in range(11):
        r = (r * 1103515245 + 12345) & 0xffffffff
        pt[i + 1] = np.float32(((r >> 16) & 0x7fff) / 32768.0)
    pt[0], pt[12] = pt[1], pt[11]
    x = np.arange(W, dtype=np.float32)
    u = x * np.float32(10) / np.float32(W)
    i = u.astype(np.int32)
    t = u - i.astype(np.float32)  # (float32 as the plugin: int32 would make it float64)
    q0, q1, q2, q3 = pt[i], pt[i + 1], pt[i + 2], pt[i + 3]
    v = 0.5 * (2 * q1 + (q2 - q0) * t + (2 * q0 - 5 * q1 + 4 * q2 - q3) * t * t
               + (3 * q1 - q0 - 3 * q2 + q3) * t * t * t)
    lo, hi = v.min(), v.max()
    n = (v - lo) / (hi - lo) if hi > lo else np.zeros_like(v)
    return 0.2 + 0.1 * np.maximum(0, (n - 0.4) / 0.6)


def melt(p, X, Y, W, H, stretch=0, seed=0):
    if not seed:
        sys.exit('melt: give a seed (the plugin picks a random curve without one)')
    delay = melt_delay(W, seed)[np.clip(X.astype(int), 0, W - 1)].astype(np.float64)
    f = np.where(p > delay, (p - delay) / (1 - delay), 0.0)
    fallen = H * f * f
    scale = 1 + stretch / 100 * fallen / H
    # old shows from row floor(fallen) on, read from floor((y - fallen) / scale)
    return (X, (Y - fallen) / scale - 0.5), (X, Y), fallen - Y - 0.5, 8, False


def wave(p, X, Y, W, H, direction=0, amplitude=80, waves=15):
    vertical = direction & 1
    sign = -1 if direction >= 2 else 1
    amp = sign * amplitude / 1000 * (H if vertical else W) * math.sin(math.pi * p)
    length = W if vertical else H
    if vertical:
        d = amp * np.sin(2 * math.pi * (waves / 10 * X / length + p))
        pos = (X, Y - d)
    else:
        d = amp * np.sin(2 * math.pi * (waves / 10 * Y / length + p))
        pos = (X - d, Y)
    return pos, pos, np.full_like(X, round(255 * p)), 0, False


KINDS = {'slide': slide, 'zoom': zoom, 'flip': flip, 'cube': cube, 'melt': melt, 'wave': wave}
DEFAULT_CELLS = {'slide': 2, 'zoom': 65, 'flip': 33, 'cube': 33, 'melt': 33, 'wave': 65}


def default_cells(kind, params):
    # a diagonal slide in or out has a corner, whose distance isn't straight in time
    if kind == 'slide' and len(params) == 3 and params[0] != 2 and params[1] and params[2]:
        return 33
    return DEFAULT_CELLS[kind]


# --- the file

def u16(a):
    return np.clip(np.rint(a), 0, 65535).astype(np.uint16)


def make_cell(fn, p, W, H, gw, gh, params):
    X, Y = np.meshgrid(np.arange(gw) * W / (gw - 1), np.arange(gh) * H / (gh - 1))
    old, new, mask, sharp, smooth = fn(p, X, Y, W, H, *params)
    # sharpness 8: level 128 + 256 * distance; 0: the level itself
    v_mask = 32768 + (mask * 16 if sharp else (mask - 128) * 16)
    hi_lo = [u16(32768 + (old[0] - X) * 16), u16(32768 + (old[1] - Y) * 16), u16(v_mask),
             u16(32768 + (new[0] - X) * 16), u16(32768 + (new[1] - Y) * 16)]
    cell = np.zeros((4 * gh, gw, 3), np.uint8)
    for plane, vals in ((0, hi_lo[0:3]), (2, hi_lo[3:5])):
        for ch, v in enumerate(vals):
            cell[plane * gh:(plane + 1) * gh, :, ch] = v >> 8
            cell[(plane + 1) * gh:(plane + 2) * gh, :, ch] = v & 255
    cell[2 * gh:3 * gh, :, 2] = SMOOTH if smooth is True else smooth
    cell[3 * gh:4 * gh, :, 2] = 8 if sharp else 0
    return cell


def make(kind, params, W, H, grid, cells):
    gw, gh = W // grid + 1, H // grid + 1
    fn = KINDS[kind]
    return np.concatenate([make_cell(fn, k / (cells - 1), W, H, gw, gh, params) for k in range(cells)], axis=1)


def decode(m, cells):
    """Per cell: the five values (centred), the block flags and sharpness."""
    gh, gw = m.shape[0] // 4, m.shape[1] // cells
    m = m.astype(np.int64)
    out = []
    for c in range(cells):
        cm = m[:, c * gw:(c + 1) * gw]
        v = lambda plane, ch: (cm[plane * gh:(plane + 1) * gh, :, ch] << 8 | cm[(plane + 1) * gh:(plane + 2) * gh, :, ch]) - 32768
        out.append((np.stack([v(0, 0), v(0, 1), v(0, 2), v(2, 0), v(2, 1)]).astype(np.float64),
                    cm[2 * gh:3 * gh, :, 2], np.minimum(cm[3 * gh:4 * gh, :, 2], 8)))
    return out


def render(m, cells, pct, old, new):
    """The frame at pct % in floating point, as the engine defines it."""
    H, W = old.shape[:2]
    gh, gw = m.shape[0] // 4, m.shape[1] // cells
    keys = decode(m, cells)
    f = pct / 100 * (cells - 1)
    c = min(int(f), cells - 2)
    t = f - c
    vals = keys[c][0] + (keys[c + 1][0] - keys[c][0]) * t
    k = keys[c if t < 0.5 else c + 1]
    x, y = np.arange(W), np.arange(H)
    j, fy = (y * (gh - 1)) // H, ((y * (gh - 1)) % H) / H
    i, fx = (x * (gw - 1)) // W, ((x * (gw - 1)) % W) / W
    J, I = np.meshgrid(j, i, indexing='ij')
    FY, FX = np.meshgrid(fy, fx, indexing='ij')
    v = (vals[:, J, I] * (1 - FX) + vals[:, J, I + 1] * FX) * (1 - FY) + (vals[:, J + 1, I] * (1 - FX) + vals[:, J + 1, I + 1] * FX) * FY
    flags, sharp = k[1][J, I], k[2][J, I]
    smooth, black = (flags & SMOOTH) != 0, (flags & BLACK) != 0
    level = np.clip(128 + np.floor(v[MASK] * 2.0 ** sharp / 16 + 0.5), 0, 255).astype(np.int64)
    YY, XX = np.meshgrid(y, x, indexing='ij')

    def sample(img, vx, vy):
        nx, ny = XX + np.floor(vx / 16 + 0.5).astype(int), YY + np.floor(vy / 16 + 0.5).astype(int)
        near = img[np.clip(ny, 0, H - 1), np.clip(nx, 0, W - 1)]
        x16, y16 = XX * 16 + np.floor(vx + 0.5).astype(int), YY * 16 + np.floor(vy + 0.5).astype(int)
        bx, by = np.where(smooth, (x16 + 8) >> 4, nx), np.where(smooth, (y16 + 8) >> 4, ny)
        outside = black & ((bx < 0) | (bx >= W) | (by < 0) | (by >= H))
        sx, sy, ax, ay = x16 >> 4, y16 >> 4, x16 & 15, y16 & 15
        ax = np.where((sx < 0) | (sx >= W - 1), 0, ax)
        ay = np.where((sy < 0) | (sy >= H - 1), 0, ay)
        sx, sy = np.clip(sx, 0, W - 1), np.clip(sy, 0, H - 1)
        sx1, sy1 = np.minimum(sx + 1, W - 1), np.minimum(sy + 1, H - 1)
        ax, ay = ax[..., None], ay[..., None]
        top = img[sy, sx] * (16 - ax) + img[sy, sx1] * ax
        bottom = img[sy1, sx] * (16 - ax) + img[sy1, sx1] * ax
        bil = (top * (16 - ay) + bottom * ay + 128) >> 8
        return np.where(outside[..., None], 0, np.where(smooth[..., None], bil, near))
    a = sample(old.astype(np.int64), v[OLD_X], v[OLD_Y])
    b = sample(new.astype(np.int64), v[NEW_X], v[NEW_Y])
    L = level[..., None]
    return ((a * (255 - L) + b * L) // 255).astype(np.uint8)


def load(path):
    return np.asarray(Image.open(path).convert('RGB'))


def diff(a, b):
    d = np.abs(a.astype(int) - b.astype(int)).max(axis=2)
    return int(d.max()), int((d > 1).sum())


def test():
    W, H = 160, 96
    rng = np.random.default_rng(1)
    old, new = rng.integers(0, 256, (H, W, 3), np.uint8), rng.integers(0, 256, (H, W, 3), np.uint8)
    # slide: the frame at 25 % is the old picture with the new one 3/4 of
    # the width to the right of its place
    m = make('slide', (0, -1, 0), W, H, 8, 2)
    out = render(m, 2, 25, old, new)
    assert (out[:, :120] == old[:, :120]).all() and (out[:, 120:] == new[:, :40]).all()
    # an empty map (no move, mask level 0..255 from a plain grey) is a cross-fade
    out = render(make('wave', (0, 0, 15), W, H, 8, 2), 2, 50, old, new)
    want = (old.astype(int) * (255 - 128) + new.astype(int) * 128) // 255
    assert (out == want).all()
    # a diagonal push: the corners neither picture covers are black
    out = render(make('slide', (2, -1, -1), W, H, 8, 2), 2, 50, old, new)
    assert (out[:48, 80:] == 0).all() and (out[48:, :80] == 0).all()
    assert (out[:48, :80] == old[48:, 80:]).all() and (out[48:, 80:] == new[:48, :80]).all()
    print('ok')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    mk = sub.add_parser('make')
    mk.add_argument('kind', choices=KINDS)
    mk.add_argument('params')
    mk.add_argument('--size', default='1280x720')
    mk.add_argument('--grid', type=int, default=8)
    mk.add_argument('--cells', type=int)
    mk.add_argument('-o', required=True)
    rd = sub.add_parser('render')
    rd.add_argument('map')
    rd.add_argument('cells', type=int)
    rd.add_argument('pct', type=float)
    rd.add_argument('old')
    rd.add_argument('new')
    rd.add_argument('-o', required=True)
    df = sub.add_parser('diff')
    df.add_argument('a')
    df.add_argument('b')
    sub.add_parser('test')
    a = ap.parse_args()
    if a.cmd == 'make':
        W, H = map(int, a.size.split('x'))
        params = [int(v) for v in a.params.split(',')]
        cells = a.cells or default_cells(a.kind, params)
        Image.fromarray(make(a.kind, params, W, H, a.grid, cells)).save(a.o, optimize=True)
        print(f'cons_warp.dll/{a.o},{cells}')
    elif a.cmd == 'render':
        Image.fromarray(render(load(a.map), a.cells, a.pct, load(a.old), load(a.new))).save(a.o)
    elif a.cmd == 'diff':
        print('max %d, over 1: %d pixels' % diff(load(a.a), load(a.b)))
    else:
        test()


if __name__ == '__main__':
    main()
