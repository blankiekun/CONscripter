#!/usr/bin/env python3
"""Vertical text across a load: `tateyoko 1` is in the save.

usage: python3 -I tests/tateload/check.py <conscripter binary> [any TTF]
Plays tests/tateload/0.txt with a checkpoint save at each page, then loads
the save of page 2 and plays on. Each loaded page must hash (`px`) as the
same page did in the first run: before the CONS block kept tateyoko, a load
laid the vertical window's text out horizontally.
Exit 0 when they match.
"""
import glob, json, os, shutil, subprocess, sys, tempfile


def run(binary, d, env, *args):
    subprocess.run(["timeout", "-k", "5", "30", binary, "-r", ".", "--save-dir", "sv", "--auto-play", "0", *args],
                   cwd=d, env=env, capture_output=True, text=True)


def pages(path):
    return [r for r in map(json.loads, open(path)) if r.get("t") == "page"]


def main():
    binary = os.path.abspath(sys.argv[1])
    font = sys.argv[2] if len(sys.argv) > 2 else next(iter(glob.glob("/usr/share/fonts/**/*.tt[fc]", recursive=True)), None)
    with tempfile.TemporaryDirectory() as d:
        shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), "0.txt"), d)
        shutil.copy(font, os.path.join(d, "default.ttf"))
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        run(binary, d, env, "--trace", "a.jsonl", "--checkpoint-every", "1")
        a = pages(os.path.join(d, "a.jsonl"))
        cp = [r for r in map(json.loads, open(os.path.join(d, "a.jsonl"))) if r.get("t") == "checkpoint" and r["pg"] == 2]
        if len(a) != 4 or not cp:
            print(f"FAIL first run: {len(a)} pages, checkpoint {cp}")
            return 1
        run(binary, d, env, "--trace", "b.jsonl", "--load", str(cp[0]["slot"]))
        b = pages(os.path.join(d, "b.jsonl"))
    want = [(p["text"], p["px"]) for p in a[1:]]
    got = [(p["text"], p["px"]) for p in b]
    if got != want:
        print(f"FAIL loaded pages {got}, played {want}")
        return 1
    print("ok: vertical text across a load")
    return 0


if __name__ == "__main__":
    sys.exit(main())
