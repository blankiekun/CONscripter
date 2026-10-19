#!/usr/bin/env python3
"""Label variants test: runs tests/labels/0.txt and checks the labels its jumps reach.

usage: python3 -I tests/labels/check.py <conscripter binary> [any TTF]
Exit 0 when the labels come in the expected order.
"""
import glob, json, os, shutil, subprocess, sys, tempfile

# a jump to *name takes *name@<language> when there is one; `language` changes only later jumps
EXPECTED = ["start", "a", "b@en", "c", "d@en", "swap@ja", "e@en", "f@en", "fin@en"]


def main():
    binary = os.path.abspath(sys.argv[1])
    font = sys.argv[2] if len(sys.argv) > 2 else next(iter(glob.glob("/usr/share/fonts/**/*.tt[fc]", recursive=True)), None)
    with tempfile.TemporaryDirectory() as d:
        shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), "0.txt"), d)
        shutil.copy(font, os.path.join(d, "default.ttf"))
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        r = subprocess.run(["timeout", "-k", "5", "30", binary, "-r", ".", "--save-dir", "sv", "--trace", "t.jsonl", "--auto-play", "0"],
                           cwd=d, env=env, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"FAIL engine exit {r.returncode}\n{r.stderr[-2000:]}")
            return 1
        names = [e["name"] for e in map(json.loads, open(os.path.join(d, "t.jsonl"))) if e.get("t") == "label" and e["name"] != "define"]
    if names != EXPECTED:
        print(f"FAIL labels {names}\n  expected {EXPECTED}")
        return 1
    print("ok: label variants")
    return 0


if __name__ == "__main__":
    sys.exit(main())
