#!/usr/bin/env python3
"""Web range test: the game files read by HTTP Range (README, "Web") in each layout.

usage: python3 -I tests/web/range.py <web build dir>

A small game shows data\\a.png (512 x 512, 13 blocks of 64 KB) and checks its size
with getspsize; data\\pad.bin (2 MB) is never opened. Each layout is served, with or
without Range, and must show "Size ok" through the consText hook, with no page error
and no synchronous read (g_range_stats.sync). Read by range, the archive or zip
must cost less than its size less half of pad.bin; without Range, the whole file once.
  loose        lazyload files (the control)
  nsa          0.txt, default.ttf loose, arc.nsa read by range
  zip          game.zip (deflated, in a top folder) with every file
  zip-nsa      game.zip holding a stored arc.nsa
  nsa-whole    nsa, the server without Range
  zip-whole    zip, the server without Range
  zip-nsa-deflated  the nsa deflated in the zip: the page says to use zip -0
The browser is $CHROME, else the first of chromium, chromium-browser, google-chrome
on the PATH; the font is $FONT, else the first TTF under /usr/share/fonts.
Needs only the Python standard library. Exit 0 when every check passes.
"""
import collections, glob, http.server, json, os, random, shutil, struct, subprocess, sys, tempfile, threading, zipfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
from web_run import RangeHandler  # noqa: E402

GAME = """;mode320
*define
nsa
game
*start
lsp 1,"data\\a.png",0,0
getspsize 1,%0,%1
if %0 == 512 && %1 == 512 goto *ok
`Size wrong\\
end
*ok
`Size ok\\
end
"""

# Posts what the page saw to /result once "Size ok" shows, the page shows a
# cons_fail message, or after 60 s.
HOOK = r"""<script>
(() => {
  const texts = [], errors = [], t0 = performance.now();
  self.consText = (a) => texts.push(a instanceof Uint8Array ? String.fromCharCode(...a) : String(a));
  const ce = console.error.bind(console);
  console.error = (...a) => {
    const s = a.join(' ');
    // no cursor pictures in the test game, no archive in some layouts
    if (!/can't find file \[\w*cur\w*\.bmp\]|failed to open nsa/.test(s)) errors.push(s);
    ce(...a);
  };
  addEventListener('error', e => errors.push('error: ' + e.message));
  addEventListener('unhandledrejection', e => errors.push('rejection: ' + e.reason));
  const timer = setInterval(() => {
    const info = document.querySelector('#cons_webinfo p'), shown = info && info.innerText || '';
    const ok = texts.some(t => t.includes('Size'));
    if (!ok && !/compressed/.test(shown) && performance.now() - t0 < 60000) return;
    clearInterval(timer);
    fetch('/result', {method: 'POST', body: JSON.stringify({texts, errors, info: shown,
      ms: performance.now() - t0, stats: typeof g_range_stats == 'undefined' ? null : g_range_stats})});
  }, 50);
})();
</script>
"""


def png(w, h, seed=1):
    """An RGB PNG of noise, stored (not compressed): w * h * 3 bytes and more."""
    rnd = random.Random(seed)
    raw = b"".join(b"\0" + rnd.randbytes(w * 3) for _ in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 0)) + chunk(b"IEND", b""))


def nsa(entries):
    """An NSA archive of (name, data), stored: u16 count, u32 base (big-endian),
    then per entry name\\0, u8 0, u32 offset from base, u32 size, u32 size."""
    index, body = b"", b""
    for name, data in entries:
        index += name.encode() + b"\0" + struct.pack(">BIII", 0, len(body), len(data), len(data))
        body += data
    return struct.pack(">HI", len(entries), 6 + len(index)) + index + body


def write_zip(path, files, stored=(), top=""):
    with zipfile.ZipFile(path, "w") as z:
        for name, data in files.items():
            method = zipfile.ZIP_STORED if name in stored else zipfile.ZIP_DEFLATED
            z.writestr(top + name, data, compress_type=method)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    build = os.path.abspath(sys.argv[1])
    chrome = os.environ.get("CHROME") or next(
        filter(None, map(shutil.which, ["chromium", "chromium-browser", "google-chrome"])), None)
    font = os.environ.get("FONT") or next(iter(sorted(glob.glob("/usr/share/fonts/**/*.ttf", recursive=True))), None)
    if not chrome or not font:
        sys.exit("no Chromium or no TTF found (set CHROME / FONT)")

    files = {"0.txt": GAME.encode(), "default.ttf": open(font, "rb").read(),
             "data/a.png": png(512, 512), "data/pad.bin": random.Random(2).randbytes(2 << 20)}
    pad = len(files["data/pad.bin"])
    arc = nsa([("data\\a.png", files["data/a.png"]), ("data\\pad.bin", files["data/pad.bin"])])
    loose = {"0.txt": files["0.txt"], "default.ttf": files["default.ttf"]}
    html = open(os.path.join(build, "conscripter.html"), encoding="utf-8", newline="").read()
    assert "<head>" in html, "conscripter.html has no <head>"
    html = html.replace("<head>", "<head>" + HOOK, 1)

    site = tempfile.mkdtemp(prefix="cons-web-range-")
    # layout -> (Range served, the file read by range, expected failure)
    layouts = {"loose": (True, None, None), "nsa": (True, "arc.nsa", None), "zip": (True, "game.zip", None),
               "zip-nsa": (True, "game.zip", None), "nsa-whole": (False, "arc.nsa", None),
               "zip-whole": (False, "game.zip", None), "zip-nsa-deflated": (True, "game.zip", "zip -0")}
    for name in layouts:
        d = os.path.join(site, name)
        os.mkdir(d)
        for f in ["conscripter.js", "conscripter.wasm"]:
            os.symlink(os.path.join(build, f), os.path.join(d, f))
        open(os.path.join(d, "index.html"), "w", encoding="utf-8", newline="").write(html)
        index = {"title": name, "gamedir": "/cons/range", "savedir": "/cons_save/" + name, "args": [],
                 "lazyload": True, "files": []}
        kind = name.replace("-whole", "")
        if kind == "loose":
            for p, data in files.items():
                os.makedirs(os.path.dirname(os.path.join(d, p)), exist_ok=True)
                open(os.path.join(d, p), "wb").write(data)
            index["files"] = [{"path": p} for p in files]
        elif kind == "nsa":
            for p, data in {**loose, "arc.nsa": arc}.items():
                open(os.path.join(d, p), "wb").write(data)
            index["files"] = [{"path": p} for p in [*loose, "arc.nsa"]]
        else:
            index["zip"] = "game.zip"
            if kind == "zip":
                write_zip(os.path.join(d, "game.zip"), files, top="game/")
            else:
                write_zip(os.path.join(d, "game.zip"), {**loose, "arc.nsa": arc},
                          stored=() if kind == "zip-nsa-deflated" else ("arc.nsa",))
        json.dump(index, open(os.path.join(d, "cons_index.json"), "w"))

    result, done = {}, threading.Event()
    sent = collections.Counter()

    class Handler(RangeHandler):
        directory = site

        def do_POST(self):
            result.update(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
            self.send_response(204)
            self.end_headers()
            done.set()

    Handler.sent = sent
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    failed = 0
    try:
        for name, (ranged, big, fail) in layouts.items():
            result.clear()
            done.clear()
            sent.clear()
            Handler.no_range = not ranged
            profile = tempfile.mkdtemp(prefix="cons-web-range-profile-")
            browser = subprocess.Popen(
                [chrome, "--headless=new", "--no-sandbox", "--no-first-run", f"--user-data-dir={profile}",
                 "--autoplay-policy=no-user-gesture-required", "--mute-audio", "--use-gl=swiftshader",
                 "--enable-unsafe-swiftshader", f"http://127.0.0.1:{server.server_port}/{name}/index.html"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                got = done.wait(90)
            finally:
                browser.kill()
                browser.wait()
                shutil.rmtree(profile, ignore_errors=True)
            size = os.path.getsize(os.path.join(site, name, big)) if big else 0
            game_bytes = sum(n for p, n in sent.items() if not p.endswith((".js", ".wasm", ".html", ".json")))
            big_bytes = sent.get(f"/{name}/{big}", 0)
            stats = result.get("stats") or {}
            if fail:
                checks = [("the page says " + fail, fail in result.get("info", ""))]
            else:
                checks = [("Size ok", any("Size ok" in t for t in result.get("texts", []))),
                          ("no page errors", not result.get("errors")),
                          ("no synchronous read", not stats.get("sync"))]
                if big and ranged:
                    checks.append((f"{big}: {big_bytes} bytes < {size - pad // 2}", big_bytes < size - pad // 2))
                elif big:
                    checks.append((f"{big}: {big_bytes} bytes == {size}", big_bytes == size))
            print(f"{name}: {result.get('ms', 0) / 1000:.2f} s, game bytes {game_bytes}, "
                  f"range requests {stats.get('requests')}, sync {stats.get('sync')}" + ("" if got else ", NO RESULT"))
            for check, ok in checks:
                print(("  ok    " if ok else "  FAIL  ") + check)
                failed += not ok
            if not all(ok for _, ok in checks):
                for e in result.get("errors", [])[:5]:
                    print("        " + e[:300])
                print("        texts:", result.get("texts"), "info:", result.get("info", "")[:200])
    finally:
        server.shutdown()
        shutil.rmtree(site, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
