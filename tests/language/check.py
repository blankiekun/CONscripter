#!/usr/bin/env python3
"""Language test: the remembered language (language.txt) and a language's own font.

usage: python3 -I tests/language/check.py <conscripter binary> [font] [another font]
Runs tests/language/0.txt with text.ja.txt and text.en.txt twice: the first
run starts in the script's first `language` (ja) and its `language "en"`
leaves language.txt saying en; the second starts in English. Then, with
default.en.ttf beside default.ttf, the text after `language "en"` is drawn
in it. Exit 0 when every check passes.
"""
import glob, json, os, shutil, subprocess, sys, tempfile

failed = []


def check(name, got, want):
    print(("ok  " if got == want else "FAIL") + f" {name}: got {got!r} want {want!r}")
    if got != want:
        failed.append(name)


def run(binary, d):
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    r = subprocess.run(["timeout", "-k", "5", "30", binary, "-r", ".", "--save-dir", "sv", "--trace", "t.jsonl", "--auto-play", "0", "--shot-every", "1", "--shot-dir", "shots"],
                       cwd=d, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL engine exit {r.returncode}\n{r.stderr[-2000:]}")
        sys.exit(1)
    return [e["text"] for e in map(json.loads, open(os.path.join(d, "t.jsonl"))) if e.get("t") == "page"]


def main():
    binary = os.path.abspath(sys.argv[1])
    fonts = sys.argv[2:] or sorted(glob.glob("/usr/share/fonts/**/*.ttf", recursive=True))
    with tempfile.TemporaryDirectory() as d:
        shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), "0.txt"), d)
        shutil.copy(fonts[0], os.path.join(d, "default.ttf"))
        os.mkdir(os.path.join(d, "sv"))
        for lang in ("ja", "en"):
            with open(os.path.join(d, f"text.{lang}.txt"), "w") as f:
                f.write(f"t1\t`{lang} one@\nt2\t`Same text@\n")
        check("first run: the first language's table, then en", run(binary, d), ["ja one", "ja one\nSame text"])
        remembered = os.path.join(d, "sv", "language.txt")
        check("language.txt", open(remembered).read() if os.path.exists(remembered) else None, "en\n")
        check("second run: starts in the language remembered", run(binary, d)[:1], ["en one"])
        # the page after `language "en"`: drawn in default.ttf, then in default.en.ttf
        other = next((f for f in fonts[1:] if open(f, "rb").read() != open(fonts[0], "rb").read()), None)
        if other is None:
            print("skip the font check: one font only")
        else:
            if os.path.exists(remembered):
                os.remove(remembered)
            run(binary, d)
            same = open(os.path.join(d, "shots", "page000002.png"), "rb").read()
            shutil.copy(other, os.path.join(d, "default.en.ttf"))
            if os.path.exists(remembered):
                os.remove(remembered)
            run(binary, d)
            check("default.en.ttf draws the English text", open(os.path.join(d, "shots", "page000002.png"), "rb").read() != same, True)
    print("language test: " + ("all passed" if not failed else f"{len(failed)} failed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
