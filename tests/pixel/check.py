#!/usr/bin/env python3
"""Pixel test: draws known cases with tests/pixel/0.txt and checks exact pixels.

usage: python3 -I tests/pixel/check.py <conscripter binary> [any TTF]
The engine needs a default.ttf to start (no text is drawn): without one given,
the first TTF under /usr/share/fonts.
Needs only the Python standard library. Exit 0 when every check passes.
"""
import glob, os, shutil, struct, subprocess, sys, tempfile, zlib

SOLID = (200, 100, 50)
RED, BLUE, BLACK = (255, 0, 0), (0, 0, 255), (0, 0, 0)


def write_png(path, w, h, pixel, level=-1):
    """RGBA PNG; pixel(x, y) gives (r, g, b, a)."""
    raw = b"".join(b"\0" + bytes(c for x in range(w) for c in pixel(x, y)) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, level)) + chunk(b"IEND", b""))


def write_npa(path, files, inv, pack=None):
    """An encrypted Nitroplus NPA of files {name: data}; inv is the decryption
    table cons_archives.txt gives (conscripter-convert's npa.rs). pack(name, data)
    gives each entry's zlib stream for a compressed archive."""
    table = [0] * 256
    for i, b in enumerate(inv):
        table[b] = i
    key1, key2 = 0x1234567, 0x89ABCDE
    key12 = key1 * key2 & 0xFFFFFFFF
    bsum = lambda v: sum(v.to_bytes(4, "little"))
    index, data = b"", b""
    for i, (name, body) in enumerate(files.items()):
        raw = name.encode()
        stored = bytes((c - (0xFC * x - bsum(key12) - bsum(i))) & 255 for x, c in enumerate(raw))
        size = len(body)
        k = ((0x87654321 - sum(raw)) * len(raw) + key12) * size & 255
        if pack:
            body = pack(name, body)
        body = bytes(table[(c + k + x) & 255] if x < 0x1000 + len(raw) else c for x, c in enumerate(body))
        index += struct.pack("<I", len(raw)) + stored + struct.pack("<BIIII", 2, i, len(data), len(body), size)
        data += body
    head = b"NPA" + struct.pack("<IIIBBIIIIII", 1, key1, key2, 1 if pack else 0, 1, len(files), 0, len(files), 0, 0, len(index))
    with open(path, "wb") as f:
        f.write(head + index + data)


def read_bmp(path):
    d = open(path, "rb").read()
    off, = struct.unpack_from("<I", d, 10)
    w, h, _, bpp = struct.unpack_from("<iiHH", d, 18)
    n, stride = bpp // 8, (w * bpp // 8 + 3) & ~3
    def px(x, y):
        row = (h - 1 - y) if h > 0 else y
        b, g, r = d[off + row * stride + x * n: off + row * stride + x * n + 3]
        return (r, g, b)
    return w, abs(h), px


def read_png(path):
    d = open(path, "rb").read()
    pos, idat = 8, b""
    while pos < len(d):
        ln, t = struct.unpack_from(">I4s", d, pos)
        if t == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack_from(">IIBBBBB", d, pos + 8)
            assert depth == 8 and ctype in (2, 6) and not inter, "unexpected PNG format"
        elif t == b"IDAT":
            idat += d[pos + 8:pos + 8 + ln]
        pos += 12 + ln
    n = 3 if ctype == 2 else 4
    raw, stride, rows, prev = zlib.decompress(idat), w * n, [], bytearray(w * n)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - n] if i >= n else 0
            b, c = prev[i], prev[i - n] if i >= n else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, lambda x, y: tuple(rows[y][x * n:x * n + 3])


fails = 0


def check(name, got, want, tol=0):
    global fails
    ok = all(abs(g - w) <= tol for g, w in zip(got, want)) if tol else got == want
    if not ok:
        fails += 1
    print(("ok   " if ok else "FAIL ") + f"{name}: got {got} want {want}" + (f" (±{tol})" if tol else ""))


def bbox(px, x0, y0, x1, y1):
    """Bounding box (left, top, right, bottom, inclusive) of non-black pixels in the region."""
    pts = [(x, y) for y in range(y0, y1) for x in range(x0, x1) if px(x, y) != BLACK]
    if not pts:
        return None
    return (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts))


def border_ok(px, box, width):
    """The outer `width` pixels of box are red and the rest blue."""
    l, t, r, b = box
    bad = [(x, y, px(x, y)) for y in range(t, b + 1) for x in range(l, r + 1)
           if px(x, y) != (RED if min(x - l, r - x, y - t, b - y) < width else BLUE)]
    return bad[:3]


def main():
    binary = os.path.abspath(sys.argv[1])
    here = os.path.dirname(os.path.abspath(__file__))
    d = tempfile.mkdtemp(prefix="cons-pixel-")
    try:
        shutil.copy(os.path.join(here, "0.txt"), d)
        font = sys.argv[2] if len(sys.argv) > 2 else sorted(glob.glob("/usr/share/fonts/**/*.ttf", recursive=True))[0]
        shutil.copy(font, os.path.join(d, "default.ttf"))
        write_png(os.path.join(d, "solid.png"), 16, 16, lambda x, y: SOLID + (255,))
        write_png(os.path.join(d, "ramp.png"), 16, 4, lambda x, y: (255, 255, 255, x * 17))
        write_png(os.path.join(d, "border.png"), 8, 6,
                  lambda x, y: (RED if x in (0, 7) or y in (0, 5) else BLUE) + (255,))
        # 40x40 stored (level 0): more than the 4 KB the NPA encrypts.
        write_png(os.path.join(d, "big.png"), 40, 40, lambda x, y: SOLID + (255,), level=0)
        inv = [(i * 7 + 3) & 255 for i in range(256)]
        big = open(os.path.join(d, "big.png"), "rb").read()
        write_npa(os.path.join(d, "orig.npa"), {"BG\\Solid.PNG": big}, inv)
        # Compressed: each kind of deflate block (stored, fixed and dynamic Huffman).
        def pack(name, data):
            level, strategy = {"stored.png": (0, zlib.Z_DEFAULT_STRATEGY), "fixed.png": (9, zlib.Z_FIXED)}.get(name, (9, zlib.Z_DEFAULT_STRATEGY))
            z = zlib.compressobj(level, zlib.DEFLATED, 15, 9, strategy)
            return z.compress(data) + z.flush()
        # Pixel noise (never black), so zlib chooses a dynamic Huffman block.
        write_png(os.path.join(d, "noise.png"), 40, 40, lambda x, y: ((x * 37 + y * 11) % 192 + 64, (x * y) % 192 + 64, 128, 255), level=0)
        noise = open(os.path.join(d, "noise.png"), "rb").read()
        os.remove(os.path.join(d, "noise.png"))
        assert (pack("dynamic.png", noise)[2] >> 1) & 3 == 2, "not a dynamic block"
        write_npa(os.path.join(d, "origz.npa"), {"stored.png": big, "fixed.png": big, "dynamic.png": noise}, inv, pack)
        os.remove(os.path.join(d, "big.png"))
        with open(os.path.join(d, "cons_archives.txt"), "w") as f:
            f.write("npa orig orig.npa " + bytes(inv).hex() + "\n")
            f.write("npa z origz.npa " + bytes(inv).hex() + "\n")
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        r = subprocess.run(["timeout", "-k", "5", "30", binary, "-r", ".", "--save-dir", "sv",
                            "--trace", "t.jsonl", "--auto-play", "0", "--shot-effects", "50",
                            "--shot-dir", "shots"], cwd=d, env=env, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"FAIL engine exit {r.returncode}\n{r.stderr[-2000:]}")
            return 1

        _, _, px = read_bmp(os.path.join(d, "sv", "lsp.bmp"))
        # An opaque sprite copies exactly (engine #109: it came out ~1 % dark).
        check("lsp solid box", bbox(px, 0, 0, 36, 36), (10, 10, 25, 25))
        check("lsp solid pixels", sorted({px(x, y) for y in range(10, 26) for x in range(10, 26)}), [SOLID])
        # White at alpha x*17 over black: 0 and 255 are exact; between, the 0-256 weight
        # scale and the rounded mix may land one step either side of x*17.
        check("lsp ramp alpha 0", px(40, 11), BLACK)
        check("lsp ramp alpha 255", px(55, 11), (255, 255, 255))
        for x in range(1, 15):
            check(f"lsp ramp alpha {x * 17}", px(40 + x, 11), (x * 17,) * 3, tol=1)
        # spfilter 2: (77r + 150g + 29b) >> 8; 11: sum of (c * p) >> 8 per channel.
        g2 = (SOLID[0] * 77 + SOLID[1] * 150 + SOLID[2] * 29) >> 8
        check("spfilter 2 grey", px(77, 17), (g2,) * 3)
        g11 = sum(c * p >> 8 for c, p in zip(SOLID, (100, 50, 30)))
        check("spfilter 11 weights", px(107, 17), (g11,) * 3)
        # orig/bg/solid.png: BG\Solid.PNG in orig.npa, decrypted (case and separator folded).
        check("npa picture box", bbox(px, 126, 0, 172, 60), (130, 10, 169, 49))
        check("npa picture pixels", sorted({px(x, y) for y in range(10, 50) for x in range(130, 170)}), [SOLID])
        for kind, x in (("stored", 175), ("fixed", 220), ("dynamic", 265)):
            check(f"npa zlib {kind} box", bbox(px, x - 2, 0, x + 44, 60), (x, 10, x + 39, 49))

        _, _, px = read_bmp(os.path.join(d, "sv", "lsp2.bmp"))
        # lsp2 places the picture's centre at (cx, cy): 8x6 at (40,40) covers 36..43, 37..42.
        # Turned 90 the source's x -4..3 about its centre becomes y 3..-4, hence 37..44.
        # Every edge has its border (engine #110 dropped the last column and row).
        for name, region, want, width in (("100%", (20, 20, 60, 60), (36, 37, 43, 42), 1),
                                          ("200%", (80, 20, 120, 60), (92, 34, 107, 45), 2),
                                          ("rot90", (140, 20, 180, 60), (157, 37, 162, 44), 1)):
            box = bbox(px, *region)
            check(f"lsp2 {name} box", box, want)
            if box:
                check(f"lsp2 {name} border", border_ok(px, box, width), [])

        _, _, px = read_png(os.path.join(d, "shots", "fx000000-1-050.png"))
        # Crossfade black to SOLID at half its time: half of each channel, ±1 for the
        # fade's 0-256 alpha step rounding.
        check("crossfade 50%", px(160, 120), tuple(c // 2 for c in SOLID), tol=1)

        _, _, px = read_bmp(os.path.join(d, "sv", "window.bmp"))
        # The #808080 window that setwindow set in *define, rows 150..239, darkens SOLID
        # to about half; above it SOLID is untouched (game used to put back the
        # default window, the whole screen).
        check("define setwindow: above the window", px(160, 100), SOLID)
        check("define setwindow: in the window", px(300, 230), tuple(c * 128 // 255 for c in SOLID), tol=1)
    finally:
        shutil.rmtree(d)
    print("pixel test: " + ("all passed" if not fails else f"{fails} failed"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
