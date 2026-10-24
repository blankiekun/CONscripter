#!/usr/bin/env python3
"""Voice wait test: `!vNNN` under --auto-play.

usage: python3 -I tests/voicewait/check.py <conscripter binary> [any TTF]
Runs tests/voicewait/0.txt with a 3 s v.wav on channel 0. Auto-play answers
the `!v2000` wait as a click, so the run goes on to the next page at once
(it used to loop on the wait until "20000 waits without a new page", exit 1).
Exit 0 when the run ends with both pages.
"""
import glob, json, os, shutil, subprocess, sys, tempfile, wave


def main():
    binary = os.path.abspath(sys.argv[1])
    font = sys.argv[2] if len(sys.argv) > 2 else next(iter(glob.glob("/usr/share/fonts/**/*.tt[fc]", recursive=True)), None)
    with tempfile.TemporaryDirectory() as d:
        shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), "0.txt"), d)
        shutil.copy(font, os.path.join(d, "default.ttf"))
        with wave.open(os.path.join(d, "v.wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(44100)
            w.writeframes(b"\0\0" * 44100 * 3)
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        r = subprocess.run(["timeout", "-k", "5", "30", binary, "-r", ".", "--save-dir", "sv", "--trace", "t.jsonl", "--auto-play", "0"],
                           cwd=d, env=env, capture_output=True, text=True)
        recs = [json.loads(l) for l in open(os.path.join(d, "t.jsonl"))]
    end = next((e for e in recs if e.get("t") == "end"), {})
    pages = [e["text"] for e in recs if e.get("t") == "page"]
    if r.returncode != 0 or len(pages) != 2:
        print(f"FAIL engine exit {r.returncode}, end {end.get('reason')!r}, pages {pages}")
        return 1
    print("ok: voice wait under auto-play")
    return 0


if __name__ == "__main__":
    sys.exit(main())
