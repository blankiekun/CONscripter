#!/usr/bin/env python3
"""Web smoke test: plays tests/web/0.txt in a web build under headless Chromium
and checks the page hooks (consText, consChoice, consSprites, consNoTextOverlay,
_consResendText) and that the page logs no errors. Then starts the page again in
the same browser profile, over the first run's saves (kidoku.dat in IndexedDB):
a second start must play the same.

usage: python3 -I tests/web/smoke.py <web build dir> [engine args...]
The build dir holds conscripter.html/.js/.wasm (cross_web.sh's build_web, or a
release's conscripter_web). Engine args go into cons_index.json as a game's
ons_args would (`--no-text-overlay` must make it fail). 0.txt (UTF-8 here) is
served in Shift-JIS with --enc:sjis, or in UTF-8 with --enc:utf8. The browser is $CHROME,
else the first of chromium, chromium-browser, google-chrome on the PATH; the
font is $FONT, else the first TTF under /usr/share/fonts.
Needs only the Python standard library. Exit 0 when every check passes.
"""
import glob, http.server, json, os, shutil, subprocess, sys, tempfile, threading

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    build, engine_args = sys.argv[1], sys.argv[2:]
    chrome = os.environ.get("CHROME") or next(
        filter(None, map(shutil.which, ["chromium", "chromium-browser", "google-chrome"])), None)
    font = os.environ.get("FONT") or next(iter(sorted(glob.glob("/usr/share/fonts/**/*.ttf", recursive=True))), None)
    if not chrome or not font:
        sys.exit("no Chromium or no TTF found (set CHROME / FONT)")

    site = tempfile.mkdtemp(prefix="cons-web-smoke-")
    for f in ["conscripter.js", "conscripter.wasm"]:
        shutil.copy(os.path.join(build, f), site)
    shutil.copy(os.path.join(HERE, "smoke.js"), site)
    utf8 = "--enc:utf8" in engine_args
    if not utf8:
        engine_args = ["--enc:sjis"] + engine_args
    enc = "utf-8" if utf8 else "cp932"
    script = open(os.path.join(HERE, "0.txt"), encoding="utf-8").read()
    open(os.path.join(site, "0.txt"), "wb").write(script.encode(enc))
    shutil.copy(font, os.path.join(site, "default.ttf"))
    # the shipped page, with smoke.js run before anything else
    html = open(os.path.join(build, "conscripter.html"), encoding="utf-8", newline="").read()
    assert "<head>" in html, "conscripter.html has no <head>"
    html = html.replace("<head>", '<head><script src="smoke.js"></script>', 1)
    open(os.path.join(site, "index.html"), "w", encoding="utf-8", newline="").write(html)
    json.dump({"title": "smoke", "gamedir": "/cons/smoke", "savedir": "/cons_save/smoke", "args": engine_args,
               "lazyload": False, "files": [{"path": "0.txt"}, {"path": "default.ttf"}]},
              open(os.path.join(site, "cons_index.json"), "w"))

    result, missing, done = {}, [], threading.Event()
    results = []

    class Handler(http.server.SimpleHTTPRequestHandler):
        extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm"}

        def __init__(self, *a, **k):
            super().__init__(*a, directory=site, **k)

        def send_error(self, code, *a, **k):
            if self.path != "/favicon.ico":  # the browser's own request
                missing.append(f"{code} {self.path}")
            super().send_error(code, *a, **k)

        def do_POST(self):
            result.update(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
            self.send_response(204)
            self.end_headers()
            done.set()

        def log_message(self, *a):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    profile = tempfile.mkdtemp(prefix="cons-web-smoke-profile-")
    try:
        for run in ("first", "second"):  # a crash only on the second start (kidoku.dat read, #438) passed one start
            result.clear()
            done.clear()
            browser = subprocess.Popen(
                [chrome, "--headless=new", "--no-sandbox", "--no-first-run", f"--user-data-dir={profile}",
                 "--autoplay-policy=no-user-gesture-required", "--use-gl=swiftshader", "--enable-unsafe-swiftshader",
                 f"http://127.0.0.1:{server.server_port}/index.html"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                if not done.wait(120):
                    sys.exit(f"FAIL: no result from the page in 120 s ({run} start)")
            finally:
                browser.terminate()  # a clean exit, so IndexedDB is on disk for the second start
                try:
                    browser.wait(10)
                except subprocess.TimeoutExpired:
                    browser.kill()
                    browser.wait()
            results.append(dict(result))
    finally:
        server.shutdown()
        shutil.rmtree(profile, ignore_errors=True)
        shutil.rmtree(site, ignore_errors=True)

    first, second = results
    failed = check(first, missing, enc, [("no envdata before _consPersist on the first start", persisted(first)[:1] == ["false"])])
    print("second start, same profile:")
    failed += check(second, [], enc, [("kidoku.dat from the first start is there", second.get("kidoku"))])
    sys.exit(1 if failed else 0)


def persisted(result):
    return [r["text"] for r in result["log"] if r["phase"] == "persist"]


def check(result, missing, enc, extra=()):
    log = result["log"]

    def calls(phase, hook, text=None):
        return [r for r in log if r["phase"] == phase and r["hook"] == hook and (text is None or text in r["text"])]

    checks = [
        ("the game starts: consText with 'Page one.'", calls("start", "consText", "Page one.")),
        ("consSprites with the strsp text 'SPRITE'", calls("start", "consSprites", "SPRITE")),
        (f"consSprites with '日本語ｶﾅ' in {enc}, its boxes after SPRITE's",  # 3 for SPRITE (in pairs), 1 per kanji at least
         [r for r in calls("start", "consSprites") if "日本語ｶﾅ" in r["text"].encode("latin-1").decode(enc, "replace")
          and r["boxes"] >= 3 + 3]),
        ("consNoTextOverlay=1 stops every hook", not [r for r in log if r["phase"] == "off"]),
        ("_consResendText sends 'Page two.' as skipped", [r for r in calls("resend", "consText", "Page two.") if r["arg"] == 1]),
        ("_consResendText sends the string sprite", calls("resend", "consSprites", "SPRITE")),
        ("consChoice for both choices", calls("choice", "consChoice", "Left") and calls("choice", "consChoice", "Right")),
        ("consChoice says the seldisable option is disabled, only it",
         [r for r in calls("choice", "consChoice", "Gone") if r["off"] == 1]
         and all(r["off"] == 0 for r in calls("choice", "consChoice", "Left") + calls("choice", "consChoice", "Right"))),
        ("a click on the disabled option goes nowhere", not [r for r in log if r["hook"] == "consText" and "Page gone." in r["text"]]
         and not calls("disabled", "consText")),
        ("the choice is picked: consText with 'Page three.'", calls("picked", "consText", "Page three.")),
        ("_consPersist writes envdata at once", persisted(result)[-1:] == ["true"]),
        ("no page errors", not result["errors"]),
        ("no missing files", not missing),
        *extra,
    ]
    failed = 0
    for name, ok in checks:
        print(("ok    " if ok else "FAIL  ") + name)
        failed += not ok
    for e in result["errors"] + missing:
        print("      " + e[:300])
    if failed:
        print("hook calls:", json.dumps([(r["phase"], r["hook"], r["text"][:40]) for r in log]))
    return failed


if __name__ == "__main__":
    main()
