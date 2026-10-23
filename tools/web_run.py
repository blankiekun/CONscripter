#!/usr/bin/env python3
"""Runs a game in the web build under headless Chromium and saves its trace.

usage: python3 -I tools/web_run.py <web build dir> <game dir> [-o trace.jsonl]
           [--timeout s] [--query 'auto-play=0&seed=3'] [--no-range]

The build dir holds conscripter.html/.js/.wasm (cross_web.sh's build_web).
The game dir is the game as loose files (DEBUG.md, "In the web build"); its
cons_index.json is used when it has one, else one is made with every file
fetched when the engine opens it (lazyload), or, when the game dir holds
game.zip, {"zip": "game.zip"} (README, "Web"). The server answers HTTP Range
requests (--no-range: whole files only, as a plain server). The page is opened as
conscripter.html?trace&<query>, served on 127.0.0.1 at a free port, and each
trace record is written to the output as it comes (default trace.jsonl), so a
run that hangs or crashes still leaves its records. It stops at the "end" or
"crash" record, or after --timeout seconds (exit 1). The page's errors go to
stderr. At the end it prints the seconds to the first page record and the bytes
sent, per file. The browser is $CHROME, else the first of chromium, chromium-browser,
google-chrome on the PATH. Needs only the Python standard library.
"""
import argparse, collections, http.server, io, json, os, re, shutil, subprocess, sys, tempfile, threading, time

# Sends the page's new trace records to /trace twice a second, and its errors to /error.
HOOK = """<script>
(() => {
  let sent = 0;
  const post = (path, body) => fetch(path, {method: 'POST', body});
  const ce = console.error.bind(console);
  console.error = (...a) => { post('/error', a.join(' ')); ce(...a); };
  addEventListener('error', e => post('/error', 'error: ' + e.message));
  setInterval(() => {
    if (typeof g_trace_lines === 'undefined' || g_trace_lines.length <= sent) return;
    const lines = g_trace_lines.slice(sent);
    sent += lines.length;
    post('/trace', lines.join('\\n') + '\\n');
  }, 500);
})();
</script>
"""


class RangeHandler(http.server.SimpleHTTPRequestHandler):
    """Serves a directory, with Range (bytes=a-b, bytes=a-, bytes=-n) unless no_range,
    and counts the bytes of file bodies it sends per path in sent (a Counter).
    Subclass it with directory, no_range and sent set."""
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm"}
    directory, no_range, sent = None, False, None

    def __init__(self, *a, **k):
        super().__init__(*a, directory=self.directory, **k)

    def send_head(self):
        m = re.fullmatch(r"bytes=(\d*)-(\d*)", self.headers.get("Range", "").strip())
        path = self.translate_path(self.path)
        if self.no_range or not m or m.group() == "bytes=-" or not os.path.isfile(path):
            f = super().send_head()
            if isinstance(f, io.BufferedReader):
                return Counted(f, self.sent, self.path.split("?")[0], os.fstat(f.fileno()).st_size)
            return f
        f = open(path, "rb")
        size = os.fstat(f.fileno()).st_size
        a, b = m.groups()
        start, end = (max(0, size - int(b)), size) if a == "" else (int(a), min(size, int(b) + 1) if b else size)
        if start >= end:
            f.close()
            self.send_error(416)
            return None
        f.seek(start)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end - 1}/{size}")
        self.send_header("Content-Length", str(end - start))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        return Counted(f, self.sent, self.path.split("?")[0], end - start)

    def log_message(self, *a):
        pass


class Counted:
    """A file whose first n bytes copyfile sends; they are added to sent[name]."""
    def __init__(self, f, sent, name, n):
        self.f, self.sent, self.name, self.left = f, sent, name, n

    def read(self, size=-1):
        data = self.f.read(self.left if size < 0 else min(size, self.left))
        self.left -= len(data)
        if self.sent is not None:
            self.sent[self.name] += len(data)
        return data

    def close(self):
        self.f.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build")
    ap.add_argument("game")
    ap.add_argument("-o", "--out", default="trace.jsonl")
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--query", default="auto-play=0", help="more URL options (DEBUG.md); default auto-play=0")
    ap.add_argument("--no-range", action="store_true", help="answer Range requests with the whole file")
    args = ap.parse_args()
    chrome = os.environ.get("CHROME") or next(
        filter(None, map(shutil.which, ["chromium", "chromium-browser", "google-chrome"])), None)
    if not chrome:
        sys.exit("no Chromium found (set CHROME)")

    game = os.path.abspath(args.game)
    site = tempfile.mkdtemp(prefix="cons-web-run-")
    engine_files = {"conscripter.html", "conscripter.js", "conscripter.wasm", "cons_index.json"}
    for name in os.listdir(game):  # the game's files as they are, linked
        if name not in engine_files:
            os.symlink(os.path.join(game, name), os.path.join(site, name))
    for name in ["conscripter.js", "conscripter.wasm"]:
        os.symlink(os.path.abspath(os.path.join(args.build, name)), os.path.join(site, name))
    html = open(os.path.join(args.build, "conscripter.html"), encoding="utf-8", newline="").read()
    assert "</body>" in html, "conscripter.html has no </body>"
    open(os.path.join(site, "conscripter.html"), "w", encoding="utf-8", newline="").write(
        html.replace("</body>", HOOK + "</body>", 1))
    index = os.path.join(game, "cons_index.json")
    if os.path.exists(index):
        shutil.copy(index, site)
    elif os.path.exists(os.path.join(game, "game.zip")):
        title = os.path.basename(game.rstrip("/"))
        json.dump({"title": title, "gamedir": "/cons/" + title, "savedir": "/cons_save/" + title,
                   "args": [], "zip": "game.zip", "files": []}, open(os.path.join(site, "cons_index.json"), "w"))
    else:
        files = []
        for root, _dirs, names in os.walk(game, followlinks=True):
            for n in names:
                rel = os.path.relpath(os.path.join(root, n), game).replace(os.sep, "/")
                if n not in engine_files:
                    files.append({"path": rel})
        title = os.path.basename(game.rstrip("/"))
        json.dump({"title": title, "gamedir": "/cons/" + title, "savedir": "/cons_save/" + title,
                   "args": [], "lazyload": True, "files": files}, open(os.path.join(site, "cons_index.json"), "w"))

    out = open(args.out, "w", encoding="utf-8")
    done = threading.Event()
    count = [0]
    sent = collections.Counter()
    first_page = []

    class Handler(RangeHandler):
        directory, no_range = site, args.no_range

        def do_POST(self):
            body = self.rfile.read(int(self.headers["Content-Length"])).decode("utf-8", "replace")
            if self.path == "/trace":
                out.write(body)
                out.flush()
                for line in body.splitlines():
                    count[0] += 1
                    if not first_page and line.startswith('{"t":"page"'):
                        first_page.append(time.monotonic() - started)
                    if line.startswith(('{"t":"end"', '{"t":"crash"')):
                        done.set()
            else:
                print("page:", body[:300], file=sys.stderr)
            self.send_response(204)
            self.end_headers()

    Handler.sent = sent
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    profile = tempfile.mkdtemp(prefix="cons-web-run-profile-")
    url = f"http://127.0.0.1:{server.server_port}/conscripter.html?trace&{args.query}"
    started = time.monotonic()
    browser = subprocess.Popen(
        [chrome, "--headless=new", "--no-sandbox", "--no-first-run", f"--user-data-dir={profile}",
         "--autoplay-policy=no-user-gesture-required", "--mute-audio", "--use-gl=swiftshader", "--enable-unsafe-swiftshader", url],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        finished = done.wait(args.timeout)
    finally:
        browser.kill()
        browser.wait()
        server.shutdown()
        out.close()
        shutil.rmtree(profile, ignore_errors=True)
        shutil.rmtree(site, ignore_errors=True)
    print("first page record: " + (f"{first_page[0]:.1f} s" if first_page else "none"))
    engine = {"/conscripter.html", "/conscripter.js", "/conscripter.wasm", "/cons_index.json"}
    print(f"bytes sent: {sum(sent.values())}, game files {sum(n for p, n in sent.items() if p not in engine)}")
    for path, n in sent.most_common(10):
        print(f"  {n:>12} {path}")
    print(f"{count[0]} records in {args.out}" + ("" if finished else f"; no end record in {args.timeout:g} s"))
    sys.exit(0 if finished else 1)


if __name__ == "__main__":
    main()
