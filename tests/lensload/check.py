#!/usr/bin/env python3
"""A lens's offset list across a load: the offset it stood at is in the save.

usage: python3 -I tests/lensload/check.py <conscripter binary> [any TTF]
Plays tests/lensload/0.txt in real time with a checkpoint save at each
page: its screenlens list ("0,0 200,0", a step every 2000 ms, no loop) has
ended at its last offset by page 1. Loading that save must show page 1 as
the first run did (`px`), the lens at offset 1 (the trace's `point`): before
the CONS block kept the offset, a load started the list again from the
first, the disc 200 px to the left.
Exit 0 when they match.
"""
import glob, json, os, shutil, subprocess, sys, tempfile


def run(binary, d, env, *args):
    subprocess.run(["timeout", "-k", "5", "30", binary, "-r", ".", "--save-dir", "sv", "--auto-play", "0", "--real-time", *args],
                   cwd=d, env=env, capture_output=True, text=True)


def pages(path):
    return [r for r in map(json.loads, open(path)) if r.get("t") == "page"]


def lens(p):
    return [(f.get("point"), p["px"]) for f in p.get("fx", []) if f["kind"] == "lens"]


def main():
    binary = os.path.abspath(sys.argv[1])
    font = sys.argv[2] if len(sys.argv) > 2 else next(iter(glob.glob("/usr/share/fonts/**/*.tt[fc]", recursive=True)), None)
    with tempfile.TemporaryDirectory() as d:
        shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), "0.txt"), d)
        shutil.copy(font, os.path.join(d, "default.ttf"))
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        run(binary, d, env, "--trace", "a.jsonl", "--checkpoint-every", "1")
        a = pages(os.path.join(d, "a.jsonl"))
        cp = [r for r in map(json.loads, open(os.path.join(d, "a.jsonl"))) if r.get("t") == "checkpoint" and r["pg"] == 1]
        if len(a) != 2 or not cp or [x[0] for x in lens(a[0])] != [1]:
            print(f"FAIL first run: {len(a)} pages, checkpoint {cp}, lens {lens(a[0]) if a else None}")
            return 1
        run(binary, d, env, "--trace", "b.jsonl", "--load", str(cp[0]["slot"]), "--max-pages", "1")
        b = pages(os.path.join(d, "b.jsonl"))
    if not b or lens(b[0]) != lens(a[0]):
        print(f"FAIL loaded page 1 lens {lens(b[0]) if b else None}, played {lens(a[0])}")
        return 1
    print("ok: a lens's offset across a load")
    return 0


if __name__ == "__main__":
    sys.exit(main())
