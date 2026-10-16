#!/usr/bin/env python3
"""Check a CONscripter debug trace (--trace) for suspicious states.

usage: cons_trace_check.py TRACE... [--script 0.txt] [--root DIR] [--config FILE] [--baseline OLD.json] [--max N] [--json]
       cons_trace_check.py --diff OLD NEW [--max N]

Reads the trace (DEBUG.md describes it) and prints what looks wrong, ranked
by kind, each finding at the script line ("*label:line") to look at:

  error        script and startup errors; a crash (its signal and last
               commands); a run that ended without "end", was killed by a
               signal or got stuck
  missing      files that couldn't be loaded, grouped by file
  text         garbled characters (in pages and select options), text past
               the window, empty pages, text tags shown as text; with
               --script, %, $ or ? in a text line (they print a variable)
  sound        the same sound on two channels at once; a voice cut off by
               the next one before a click wait; a voice or music at volume 0
  voice        a line with no voice where the same speaker (or the lines
               around it) has one; a voice file another speaker's lines use;
               one voice file for lines that say different things
  linger       a sprite still up after the background changed, or for many pages
  float        a tall picture whose bottom edge stops just above the screen's
  offscreen    a sprite entirely off the screen
  invisible    a sprite shown at alpha 0
  crowd        too many tall pictures at once, or two at the same place
               (pictures covering the whole screen left out)
  overlay      a screen filter, effect, monocro or nega still on after the
               background changed,
               or (--cg) any filter or effect over a background matching REGEX,
               or a screenshake with no end shaking for many pages (--shake)
  loop         a looping sound left playing for many pages
  duration     a wait or effect the script asks to last very long
  slow         where the time went: the slowest stretches between records
  save         a state that changed after a save and load (--roundtrip-every)
  replay       a --replay run that went another way, or ran out of choices;
               a --choose or --choices answer not offered, a --choices list run out
  memory       image memory that grows steadily or jumps; a single picture
               taking a lot of it (--trace-max-image)
  movie        a screen change made after a movie but before the next print
               (the screen from before the movie shows in between); a movie
               the script lets a click stop that moviestop 0 makes unskippable
  button       a btnwait button that can't be seen: off the screen, hidden,
               at alpha 0 or under an opaque sprite
  coverage     (--script) labels the run never reached
  unused       (--root) image and sound files no trace loaded

Several traces (the runs of cons_explore.py) are read as one: coverage and
unused count what any of them reached, and each finding is listed once.
--baseline OLD.json (an earlier --json report) leaves out the findings it
already had, so only new ones show.

Settings for a game can go in a JSON file, cons_trace_check.json beside the
trace or the script, or --config FILE: any option below by name, "ignore"
(sprites to leave out, as --trace-ignore), and "known" (findings to leave
out). Options given on the command line win.

--diff compares two traces of the same route page by page (before and after
a fix) and prints the pages whose state differs, and pages whose picture
differs while their state is the same (nothing animating).
"""
import argparse
import json
import os
import re
import sys
from collections import OrderedDict, defaultdict

FILTER_KINDS = {1: 'colour', 2: 'greyscale', 3: 'negative', 4: 'brightness', 5: 'monotone',
                6: 'blur', 7: 'hue/saturation', 8: 'lightness invert'}

RULES = ['error', 'missing', 'text', 'sound', 'voice', 'linger', 'float', 'offscreen',
         'invisible', 'crowd', 'overlay', 'loop', 'duration', 'slow', 'save', 'replay',
         'memory', 'movie', 'button', 'coverage', 'unused']

# Options a config file may set, with their defaults.
DEFAULTS = {
    'script': None, 'root': None, 'cg': None, 'coverage_skip': None, 'ignore': None, 'skip': None,
    'max': 20, 'tall': 0.4, 'linger': 150, 'float_h': 0.75, 'float_gap': 40, 'crowd': 4,
    'loop': 100, 'shake': 30, 'voice_ch': '0', 'voice': r'(^|[\\/])voices?[\\/]',
    'dialogue': '^[\\s\u3000]*[\u300c\u300e\u201c]', 'voice_window': 3, 'voice_share': 0.8,
    'voice_min': 5, 'line_chars': None, 'save_ignore': None, 'slow': 10, 'slow_ms': 1000, 'mem_jump': 64, 'mem_leak': 128,
}

SOUND_CMDS = ('dwave', 'dwaveloop', 'wave', 'waveloop', 'dwaveplay', 'dwaveplayloop')

# Characters that shouldn't be in a page's text.
GARBLED = [
    (re.compile('\ufffd'), 'U+FFFD (a character the conversion lost)'),
    (re.compile('[\x00-\x08\x0b-\x1f\x7f-\x9f]'), 'a control character'),
    (re.compile('[\ue000-\uf8ff]'), 'a private-use character'),
    (re.compile('[\u7e3a\u7e67\u7e5d].{0,3}[\u7e3a\u7e67\u7e5d]'), 'UTF-8 read as Shift-JIS (mojibake)'),
    (re.compile('\u00c3[\u0080-\u00bf]|\u00e2\u20ac|\u00c2[\u00a0-\u00bf]'), 'UTF-8 read as Latin-1 (mojibake)'),
]
# UTF-8 read as Shift-JIS: kanji and half-width katakana in turn (a run of katakana alone is fine)
HALF_KANA = re.compile('(?:[\u3000-\u30ff\u4e00-\u9fff][\uff61-\uff9f]){3,}')
# Text tags the engine runs; in a page's text they were shown instead.
TAGS = re.compile(r'![wd]\d+|!s(?:\d+|d)|#[0-9A-Fa-f]{6}|~[^~\n]*~|\\|@')
# In a text line of the script: these print a variable or read an array.
RESERVED = re.compile(r'[%$](?:\d+|[A-Za-z_]\w*)|\?')
RUBY = re.compile(r'\(([^/()\n]*)/[^()\n]*\)')


def records(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except ValueError:
                # the last line of a trace cut off by a crash
                yield {'t': 'broken', 'line': n}


def label_of(at):
    return at.split(':')[0][1:] if at else None


def line_of(at):
    try:
        return int(at.rsplit(':', 1)[1])
    except (AttributeError, IndexError, ValueError):
        return None


def parse_ignore(spec):
    """'22-48,lsp2:12-23,ld:1' -> set of (kind, n), as --trace-ignore."""
    out = set()
    for tok in (spec or '').split(','):
        tok = tok.strip()
        if not tok:
            continue
        kind = 'lsp'
        for k in ('lsp2', 'ld', 'lsp'):
            if tok.startswith(k + ':'):
                kind, tok = k, tok[len(k) + 1:]
                break
        a, _, b = tok.partition('-')
        for n in range(int(a), int(b or a) + 1):
            out.add((kind, n))
    return out


def script_lines(path, utf8):
    """The script's lines, decoded, counted from 1 as the trace's "at" counts them.
    PATH is a script file, or a folder whose numbered .txt files are read in order."""
    if os.path.isdir(path):
        names = sorted((f for f in os.listdir(path) if re.match(r'^\d+\.txt$', f)),
                       key=lambda f: (int(f[:-4]), f))
        files = [os.path.join(path, f) for f in names]
    else:
        files = [path]
    lines = []
    for fn in files:
        with open(fn, encoding='utf-8' if utf8 else 'cp932', errors='replace') as f:
            lines.extend(f.read().split('\n'))
    return lines


def script_labels(lines):
    """[(line number, label)] for each label in the script, lower-cased as the engine does."""
    out = []
    for n, line in enumerate(lines, 1):
        m = re.match(r'\s*\*+([^\s:;/]+)', line)
        if m:
            out.append((n, m.group(1).lower()))
    return out


def trace_labels(path):
    """The labels a trace reached."""
    labels = set()
    for r in records(path):
        if r.get('t') == 'label':
            labels.add(str(r.get('name')).lower())
        for k in ('at', 'via'):
            if isinstance(r.get(k), str):
                labels.add(label_of(r[k]).lower())
    return labels


def unreached(lines, labels, skip=None):
    skip = re.compile(skip) if skip else None
    return [(n, l) for n, l in script_labels(lines) if l not in labels and not (skip and skip.search(l))]


def snippet(text, pos=0, width=30):
    s = text[max(0, pos - 10):pos + width].replace('\n', ' / ')
    return '\u00ab%s\u00bb' % s


def width_of(line):
    return sum(0.5 if ord(c) < 0x100 or 0xff61 <= ord(c) <= 0xff9f else 1 for c in line)


class Checker:
    def __init__(self, args):
        self.args = args
        self.w, self.h = 800, 600
        self.findings = defaultdict(list)  # rule -> [(at, message, page)]
        self.suppressed = defaultdict(int)  # rule -> findings left out by "known"
        self.skip = set((args.skip or '').replace(' ', '').split(',')) - {''}
        self.known = [k for k in getattr(args, 'known', []) or []]
        self.ignore = parse_ignore(args.ignore)
        self.pg = 0                        # the page being read
        self.seen = set()                  # keys already reported
        self.set_by = {}                   # (kind, n) -> (at, src) of the ev that last showed it
        self.prev = None                   # the previous page
        self.loops = {}                    # file -> first page it was seen looping
        self.shakes = {}                   # slot -> (page, at) where its endless shake was set
        self.labels = set()                # labels reached
        self.missing = OrderedDict()       # (what, file) -> [count, first at]
        self.counts = defaultdict(int)
        self.end = None
        self.crash = None
        self.meta = {}
        self.lines = None                  # the script's lines (--script)
        self.voice_ch = {int(c) for c in str(args.voice_ch).split(',') if c.strip()}
        self.voice_re = re.compile(args.voice, re.I)
        self.save_ignore = re.compile(args.save_ignore) if args.save_ignore else None
        self.dialogue_re = re.compile(args.dialogue)
        self.started = {}                  # channel -> page when its sound started
        self.voiced = False                # a voice started since the last page
        self.voice_file = None             # the voice a sound command started since the last page
        self.voice_lines = {}              # voice file -> {its line's text, punctuation left out: (pg, at, text)}
        self.total = defaultdict(int)      # counts over every trace
        self.baseline = set()              # (rule, at, message) of --baseline's findings
        self.in_baseline = 0
        self.lines_seen = []               # (pg, at, speaker, dialogue, voiced) for the voice rule
        self.last_ms = None                # the last record with "ms"
        self.stretches = {}                # (from at, to at) -> [total ms, count, max, pg, desc]
        self.mem = []                      # (pg, at, bytes, surfaces)
        self.starts = []                   # (channel, file) started since the last page
        self.reloading = False             # between a "roundtrip" record and the next page
        self.mismatch = OrderedDict()      # the fields that differed -> [round trips, first at, page, diff]
        self.twice = OrderedDict()         # file -> [first at, page, channels, pages] played on two channels
        self.used = set()                  # files the traces loaded, normalised (--root)
        self.runs = []                     # (trace, pages, commands, choices, how it ended)
        self.wait_ms = 0                   # the time the script's waits, effects and text asked for
        self.wait_most = None              # (ms, page, at) of the page that asked for the most
        self.after_movie = None            # [the movie's ev, the screen changes since]

    def start_trace(self):
        """Forget what belongs to one run, before the next trace."""
        self.prev = self.end = self.crash = self.last_ms = None
        self.pg = 0
        self.loops, self.shakes, self.started = {}, {}, {}
        self.voiced, self.reloading, self.starts, self.mem = False, False, [], []
        self.after_movie = None
        self.counts = defaultdict(int)

    def add(self, rule, key, at, message):
        if rule in self.skip:
            return
        if key is not None:
            if (rule, key) in self.seen:
                return
            self.seen.add((rule, key))
        for k in self.known:
            if k.get('rule', '*') in ('*', rule) and k['re'].search(message) \
                    and (not k.get('at') or re.search(k['at'], at or '')):
                self.suppressed[rule] += 1
                return
        self.findings[rule].append((at or '?', message, self.pg))

    def tall(self, s):
        return s.get('h', 0) >= self.h * self.args.tall

    def is_voice(self, ch, f):
        return (ch is not None and ch in self.voice_ch) or bool(f and self.voice_re.search(f))

    def use(self, f):
        if f and isinstance(f, str):
            self.used.add(norm_file(f))

    def run(self, path):
        self.start_trace()
        for r in records(path):
            t = r.get('t')
            self.counts[t] += 1
            if t == 'page':
                self.pg = r['pg']
                self.reloading = False
            for k in ('at', 'via'):
                if isinstance(r.get(k), str):
                    self.labels.add(label_of(r[k]).lower())
            if 'ms' in r and t in ('page', 'ev'):
                self.timing(r)
            for k in ('file', 'bg', 'bgm', 'cut'):
                self.use(r.get(k))
            for s in (r.get('sp') or []) + (r.get('se') or []) + (r.get('buttons') or []):
                self.use(s.get('file'))
            for f in r.get('loopbgm') or []:
                self.use(f)
            if t == 'label':
                self.labels.add(str(r.get('name')).lower())
            elif t == 'meta':
                self.meta = r
                self.w, self.h = r.get('w') or self.w, r.get('h') or self.h
                if self.args.script:
                    self.lines = script_lines(self.args.script, r.get('utf8'))
            elif t == 'ev':
                m = re.match(r'screenshake\s+(\d+)', r.get('src') or '')
                if m:
                    self.shakes.pop(int(m.group(1)), None)  # set again (or cleared): counted from here
                self.ev(r)
            elif t in ('page', 'dump'):
                if self.ignore:
                    r['sp'] = [s for s in r.get('sp', []) if (s['kind'], s['n']) not in self.ignore]
                if t == 'page':
                    self.page(r)
            elif t == 'warn':
                self.warn(r)
            elif t == 'choice' and r.get('kind') == 'btnwait':
                self.buttons(r)
            elif t == 'choice':
                self.options(r)
            elif t == 'roundtrip':
                self.reloading = True
            elif t == 'error':
                stack = ' <- '.join(r.get('stack') or [])
                self.add('error', None, r.get('at'), '%s error: %s%s' % (
                    r.get('kind', 'script'), r.get('msg'), ' (called from %s)' % stack if stack else ''))
            elif t == 'crash':
                self.crash = r
                stack = ' <- '.join(r.get('stack') or [])
                recent = r.get('recent') or []
                last = ', '.join('%s %s' % (c.get('cmd'), c.get('at')) for c in recent[-8:])
                self.add('error', None, r.get('at'), 'crashed (%s)%s; the last commands: %s' % (
                    r.get('signal'), ' (called from %s)' % stack if stack else '', last or 'none'))
            elif t == 'end':
                self.end = r
            elif t == 'broken':
                self.add('error', None, None, 'line %d of the trace is cut off' % r['line'])

        if self.end is None and self.crash is None:
            last = self.prev.get('at') if self.prev else None
            self.add('error', None, last, 'the trace has no "end": the engine crashed or was killed after this page')
        elif self.end and str(self.end.get('reason', '')).startswith('signal '):
            last = self.prev.get('at') if self.prev else None
            self.add('error', None, last, 'the engine was killed (%s) after this page' % self.end['reason'][7:])
        elif self.end and str(self.end.get('reason', '')).startswith('stuck') and not self.findings['error']:
            self.add('error', None, self.prev.get('at') if self.prev else None,
                     'auto-play stopped: %s' % self.end['reason'])
        self.memory()
        end = self.end.get('reason', 'ended') if self.end else 'crashed' if self.crash else 'no end record'
        self.runs.append((path, self.counts['page'], self.counts['ev'], self.counts['choice'], end))
        for k, v in self.counts.items():
            self.total[k] += v

    def finish(self):
        """The checks over every trace read."""
        for (what, f), (n, at) in self.missing.items():
            self.add('missing', None, at, '%s %s%s' % (what, f, ' (%d times)' % n if n > 1 else ''))
        if self.args.script and self.lines is None:
            self.lines = script_lines(self.args.script, False)
        for (n, at, pg, diff) in self.mismatch.values():
            self.pg = pg
            self.add('save', None, at, 'after a save and load: %s%s' % (
                '; '.join(diff), ' (and %d more round trips with the same fields)' % (n - 1) if n > 1 else ''))
        for f, (at, pg, chs, n) in self.twice.items():
            self.pg = pg
            self.add('sound', None, at, '%s plays on two channels at once (%s), on %d page%s' % (
                f, ', '.join(str(c) for c in sorted(chs)), n, 's' if n > 1 else ''))
        self.voices()
        self.speakers()
        self.slow()
        if self.lines is not None:
            self.reserved()
            self.coverage()
        if self.args.root:
            self.unused()
        if self.baseline:
            for rule in RULES:
                keep = [f for f in self.findings[rule] if (rule, f[0], finding_key(f[1])) not in self.baseline]
                self.in_baseline += len(self.findings[rule]) - len(keep)
                self.findings[rule] = keep

    def buttons(self, r):
        for b in r.get('buttons') or []:
            if b.get('blocked'):
                name = 'button %s%s' % (b.get('no'), ' (lsp %s %s)' % (b['sp'], b.get('file') or b.get('str') or '')
                                         if 'sp' in b else '')
                self.add('button', (r.get('at'), b.get('no'), b['blocked']), r.get('at'),
                         '%s can\'t be seen: %s' % (name, b['blocked'] if b['blocked'] != 'off' else 'off the screen'))

    def speakers(self):
        """A voice file whose name another speaker's voices share: the wrong voice, or the wrong name."""
        names = defaultdict(list)
        for pg, at, speaker, dialogue, voiced, vf in self.lines_seen:
            if speaker and vf:
                names[speaker].append(os.path.basename(norm_file(vf)))
        # each speaker's prefix of each length that most (--voice-share) of their voices start with
        major = {}
        for who, files in names.items():
            if len(files) < self.args.voice_min:
                continue
            major[who] = {}
            for n in range(1, min(len(f) for f in files) + 1):
                counts = defaultdict(int)
                for f in files:
                    counts[f[:n]] += 1
                p, c = max(counts.items(), key=lambda kv: kv[1])
                if c < len(files) * self.args.voice_share:
                    break
                major[who][n] = p
        # the shortest that no other speaker's voices share (v3 for v300..., v4 for v410...)
        prefix = {}
        for who, ps in major.items():
            for n in sorted(ps):
                if all(m.get(n) != ps[n] for o, m in major.items() if o != who):
                    prefix[who] = ps[n]
                    break
        # only prefixes that tell the speakers apart, and none inside another
        good = {w: p for w, p in prefix.items()
                if not any(o != w and (q.startswith(p) or p.startswith(q)) for o, q in prefix.items())}
        for pg, at, speaker, dialogue, voiced, vf in self.lines_seen:
            if speaker not in good or not vf:
                continue
            f = os.path.basename(norm_file(vf))
            if f.startswith(good[speaker]):
                continue
            other = [w for w, p in good.items() if f.startswith(p)]
            if other:
                self.pg = pg
                self.add('voice', (at, f), at, '%s speaks with %s, a voice of %s (%s\u2026), not their own (%s\u2026)'
                         % (speaker, vf, other[0], good[other[0]], good[speaker]))

    def unused(self):
        root = self.args.root
        exts = ('.png', '.jpg', '.jpeg', '.bmp', '.gif', '.webp', '.ogg', '.opus', '.wav', '.mp3', '.mid', '.midi', '.flac')
        # shortcut: loose files only; a game in .nsa/.sar archives isn't listed, until the archives are read
        files = []
        for d, _, fs in os.walk(root):
            for f in fs:
                if f.lower().endswith(exts):
                    rel = os.path.relpath(os.path.join(d, f), root)
                    if norm_file(rel) not in self.used:
                        files.append(rel)
        if files:
            files.sort()
            by_dir = defaultdict(int)
            for f in files:
                by_dir[os.path.dirname(f) or '.'] += 1
            self.pg = 0
            self.add('unused', None, None, '%d files no trace loaded (%s): %s%s' % (
                len(files), ', '.join('%s %d' % kv for kv in sorted(by_dir.items(), key=lambda kv: -kv[1])[:8]),
                ', '.join(files[:self.args.max * 3]), ' ...' if len(files) > self.args.max * 3 else ''))

    def warn(self, r):
        what, at = r.get('what'), r.get('at')
        if what == 'auto-play stuck':
            self.add('error', None, at, 'auto-play stuck here (going round in circles?)')
        elif what == 'long wait':
            ms = r.get('ms', 0)
            self.add('duration', at, at, '%s asks for %d ms (%s)' % (
                r.get('cmd'), ms, '%.1f minutes' % (ms / 60000) if ms >= 60000 else '%.1f s' % (ms / 1000)))
        elif what == 'large image':
            self.add('memory', r.get('file') or at, at, '%s is %sx%s, %s MB as one picture' % (
                r.get('file') or 'a scaled picture', r.get('w'), r.get('h'), r.get('mb')))
        elif what == 'image too large for the web': # traces before 0.19.0: a px limit the engine doesn't have
            pass
        elif what == 'save/load mismatch':
            diff = [d for d in r.get('diff') or []
                    if not (self.save_ignore and self.save_ignore.search(d.split(':')[0]))]
            if not diff:
                return
            fields = tuple(sorted({d.split(':')[0] for d in diff}))
            s = self.mismatch.setdefault(fields, [0, at, self.pg, diff])
            s[0] += 1
        elif what == 'replay diverged':
            self.add('replay', None, at, 'the run reached a choice here, the recorded run at %s' % r.get('expected'))
        elif what == 'replay ended':
            self.add('replay', None, at, 'the recorded choices ran out here; --auto-choice picks from here on')
        elif what == 'choice not offered':
            self.add('replay', None, at, '--%s asks for option %s here, but the options are %s' % (
                r.get('from'), r.get('option'), ', '.join(str(o) for o in r.get('options') or [])))
        elif what == 'choices ended':
            self.add('replay', None, at, 'the --choices list ran out here; --replay or --auto-choice picks from here on')
        elif self.reloading:
            pass  # the load of a --roundtrip-every repeats the scene's warnings
        else:
            key = (what, r.get('file'))
            if key not in self.missing:
                self.missing[key] = [0, at]
            self.missing[key][0] += 1

    def movie(self, r):
        """A movie, or what follows one: the engine redraws the screen it
        had as the movie ends, so what the script changes before its next
        print shows a frame late, after the screen from before the movie."""
        cmd = r.get('cmd', '')
        if cmd in ('mpegplay', 'movie', 'avi'):
            self.movie_end()
            if r.get('skip') is False and 'moviestop' in r:
                self.add('movie', ('skip', r.get('at')), r.get('at'),
                         'a click can\'t stop the movie: moviestop %s overrides the script\'s flag' % r['moviestop'])
            self.after_movie = [r, []]
        elif self.after_movie:
            if cmd == 'print' or r.get('effect') or r.get('t') == 'page':
                self.movie_end(r)
            # (spmotion starts from what is drawn)
            elif (r.get('sp') is not None and cmd != 'spmotion') or cmd in ('bg', 'bgcopy', 'bgcpy', 'popscreen'):
                self.after_movie[1].append(r)

    def movie_end(self, by=None):
        if self.after_movie and self.after_movie[1]:
            m, changes = self.after_movie
            what = ', '.join('%s %s' % (c.get('cmd'), c.get('at')) for c in changes[:3])
            self.add('movie', ('flash', m.get('at')), m.get('at'),
                     'the screen from before the movie shows as it ends, then %d change%s made after it '
                     '(%s%s) at %s: make them before the movie' % (len(changes), 's' if len(changes) > 1 else '',
                     what, ', ...' if len(changes) > 3 else '', by.get('at') if by else 'the run\'s end'))
        self.after_movie = None

    def ev(self, r):
        sp = r.get('sp')
        if sp and self.ignore:
            r['sp'] = sp = [s for s in sp if (s['kind'], s['n']) not in self.ignore]
        self.movie(r)
        for s in sp or []:
            if not s.get('gone'):
                self.set_by[(s['kind'], s['n'])] = (r.get('at'), r.get('src', ''))
        cmd = r.get('cmd', '')
        if cmd not in SOUND_CMDS:
            return
        ch, f = r.get('ch'), r.get('file')
        m = re.match(r'\w+\s+(\d+)\s*,\s*"([^"]*)"', r.get('src', ''))
        if ch is None and m:
            ch = int(m.group(1))
        if f is None and m:
            f = m.group(2)
        if r.get('cut') and self.is_voice(ch, r['cut']) and self.started.get(ch) == r.get('pg'):
            self.add('sound', (ch, r['cut'], r.get('at')), r.get('at'),
                     'voice %s on channel %s cut off by %s before a click wait' % (r['cut'], ch, f or r.get('src')))
        if ch is not None:
            self.started[ch] = r.get('pg')
            if f:
                self.starts.append((ch, f))
        if self.is_voice(ch, f):
            self.voiced = True
            self.voice_file = f

    def timing(self, r):
        prev = self.last_ms
        self.last_ms = r
        if prev is None:
            return
        d = r['ms'] - prev['ms']
        if d <= 0:
            return
        key = (prev.get('at'), r.get('at'))
        s = self.stretches.get(key)
        if s is None:
            desc = lambda x: x.get('cmd') or 'page %d' % x.get('pg', 0)
            s = self.stretches[key] = [0, 0, 0, self.pg, '%s -> %s' % (desc(prev), desc(r))]
        s[0] += d
        s[1] += 1
        if d > s[2]:
            s[2], s[3] = d, self.pg

    def page(self, p):
        self.movie(p)
        a = self.args
        at, pg = p.get('at'), p['pg']
        sp = p.get('sp', [])
        prev = self.prev
        bg_changed = prev is not None and prev.get('bg') != p.get('bg')

        for s in sp:
            key = (s['kind'], s['n'], s.get('file') or s.get('str') or s.get('tag'))
            name = '%s %d %s' % (s['kind'], s['n'], key[2])
            origin = self.set_by.get((s['kind'], s['n']))
            where = ' (shown at %s: %s)' % origin if origin else ''
            since = s.get('since', pg)

            if s.get('missing'):
                self.add('missing', key, at, '%s is shown but its image is missing' % name)
                continue
            if bg_changed and since < pg and self.tall(s):
                self.add('linger', key + (since,), at, '%s stayed up when the background changed %s -> %s%s'
                         % (name, prev.get('bg'), p.get('bg'), where))
            elif pg - since >= a.linger and self.tall(s):
                self.add('linger', key + (since,), at, '%s has been up since page %d (%d pages)%s'
                         % (name, since, pg - since, where))
            gap = self.h - (s['y'] + s['h'])
            if s['h'] >= self.h * a.float_h and 0 < gap <= a.float_gap:
                self.add('float', key + (s['y'],), at, '%s ends %d px above the bottom of the screen (y %d, h %d)%s'
                         % (name, gap, s['y'], s['h'], where))
            if s.get('off'):
                self.add('offscreen', key + (s['x'], s['y']), at, '%s is off the screen at %d,%d (%dx%d)%s'
                         % (name, s['x'], s['y'], s['w'], s['h'], where))
            # the alpha it is drawn at (spmotion); one fading in or out, or moving in a loop, passes 0 on its way
            if s.get('shown', s.get('alpha')) == 0 and not s.get('motion'):
                self.add('invisible', key, at, '%s is shown at alpha 0%s' % (name, where))

        # a picture covering the whole screen is a background drawn as a sprite, not a character
        tall = [s for s in sp if self.tall(s) and not s.get('off') and not s.get('missing')
                and not (s['x'] <= 0 and s['y'] <= 0 and s['x'] + s['w'] >= self.w and s['y'] + s['h'] >= self.h)]
        if len(tall) > a.crowd:
            self.add('crowd', tuple(sorted((s['kind'], s['n']) for s in tall)), at,
                     '%d tall pictures at once: %s' % (len(tall), ', '.join(
                         '%s %d %s' % (s['kind'], s['n'], s.get('file')) for s in tall)))
        for i, s1 in enumerate(tall):
            for s2 in tall[i + 1:]:
                if abs(s1['x'] - s2['x']) <= 8 and abs(s1['y'] - s2['y']) <= 8:
                    self.add('crowd', ('same', s1['kind'], s1['n'], s2['kind'], s2['n'], s1.get('file'), s2.get('file')), at,
                             '%s %d %s and %s %d %s are at the same place (%d,%d)'
                             % (s1['kind'], s1['n'], s1.get('file'), s2['kind'], s2['n'], s2.get('file'), s1['x'], s1['y']))

        overlay = overlays(p)
        if overlay:
            desc = ', '.join(o['name'] if 'name' in o else
                             'screenfilter %d (%s)' % (o['slot'], FILTER_KINDS.get(o['kind'], o['kind']))
                             if isinstance(o['kind'], int) else 'screen%s %d' % (o['kind'], o['slot'])
                             for o in overlay)
            if bg_changed and overlays(prev) == overlay:
                self.add('overlay', (p.get('bg'), desc), at, '%s still on when the background changed %s -> %s'
                         % (desc, prev.get('bg'), p.get('bg')))
            if a.cg and re.search(a.cg, p.get('bg') or ''):
                self.add('overlay', ('cg', p.get('bg'), desc), at, '%s over the CG %s' % (desc, p.get('bg')))

        # a shake left on: a timed shake given no end shakes until cleared
        shaking = {o['slot']: o for o in (p.get('fx') or []) if o['kind'] == 'shake' and not o.get('end')}
        for slot in list(self.shakes):
            if slot not in shaking:
                del self.shakes[slot]
        for slot in shaking:
            first = self.shakes.setdefault(slot, (pg, at))
            if pg - first[0] >= a.shake:
                self.add('overlay', ('shake', slot, first[1]), first[1],
                         'screenshake %d has shaken for %d pages (set with no end, never cleared)' % (slot, pg - first[0]))

        se = p.get('se', [])
        looping = {s['file'] for s in se if s.get('loop')}
        for f in list(self.loops):
            if f not in looping:
                del self.loops[f]
        for f in looping:
            first = self.loops.setdefault(f, (pg, at))
            if pg - first[0] >= a.loop:
                self.add('loop', (f, first[0]), first[1], 'looping sound %s has played for %d pages' % (f, pg - first[0]))

        # the same sound on two channels: in an auto-play run, sounds outlast many pages, so there
        # only those started before the same click wait, and looping ones
        chans = defaultdict(set)
        auto = 'auto' in self.meta
        for s in se:
            if not auto or s.get('loop'):
                chans[s['file']].add(s['ch'])
        for ch, f in self.starts:
            chans[f].add(ch)
        self.starts = []
        for f, chs in chans.items():
            if len(chs) > 1:
                d = self.twice.setdefault(f, [at, pg, set(), 0])
                d[2].update(chs)
                d[3] += 1

        if 'wait' in p:
            self.wait_ms += p['wait']
            if not self.wait_most or p['wait'] > self.wait_most[0]:
                self.wait_most = (p['wait'], pg, at)
        for s in se:
            if s.get('vol') == 0 and self.is_voice(s['ch'], s['file']):
                self.add('sound', ('vol0', s['ch']), at, 'voice %s on channel %d plays at volume 0 (soundmute, chvol?)'
                         % (s['file'], s['ch']))
        if p.get('bgmvol') == 0 and p.get('bgm'):
            self.add('sound', ('vol0', 'bgm'), at, 'music %s plays at volume 0 (soundmute, bgmvol?)' % p['bgm'])

        self.text(p)
        # a voice that started for this page: a sound command, or one in "se" the last page didn't have
        old = {(s['ch'], s['file']) for s in (prev or {}).get('se', [])}
        new = [s['file'] for s in se if self.is_voice(s['ch'], s['file']) and (s['ch'], s['file']) not in old]
        vf = self.voice_file or (new[0] if new else None)
        voiced = self.voiced or bool(new)
        self.voiced, self.voice_file = False, None
        tag = p.get('tag')
        if tag is None and self.lines is not None:
            # the text line's own [name|voice] tag
            n = line_of(at)
            m = re.match(r'\s*\[([^\]]*)\]', self.lines[n - 1]) if n and n <= len(self.lines) else None
            tag = m.group(1) if m else None
        speaker = tag.split('|')[0].strip() if tag else None
        # a line of only "……", "っ！" or "！？" (in its quotes, if it has them) needs no voice
        said = re.findall('[\u300c\u300e]([^\u300d\u300f]*)', p.get('text', '')) or [p.get('text', '')]
        dialogue = (bool(speaker) or bool(self.dialogue_re.search(p.get('text', '')))) \
            and bool(re.sub('[\\W_\u3063\u30c3\u30fc]', '', ''.join(said)))
        self.lines_seen.append((pg, at, speaker, dialogue, voiced, vf))
        if vf:
            # the text this click wait added: a page's later waits repeat the earlier text
            text, before = p.get('text') or '', (prev or {}).get('text') or ''
            own = text[len(before):] if before and text.startswith(before) else text
            norm = re.sub(r'[\W_]', '', own)
            if norm:
                self.voice_lines.setdefault(vf, {}).setdefault(norm, (pg, at, own.strip()))
        if 'mem' in p and isinstance(p['mem'], dict):
            self.mem.append((pg, at, p['mem'].get('bytes', 0), p['mem'].get('surf', 0)))
        self.prev = p

    def garbled(self, text):
        """What is garbled in the text, with where, or None."""
        for rx, what in GARBLED:
            m = rx.search(text)
            if m:
                return '%s: %s' % (what, snippet(text, m.start()))
        m = HALF_KANA.search(text)
        if m and not self.meta.get('utf8'):
            return 'kanji and half-width katakana in turn (mojibake?): %s' % snippet(text, m.start())

    def options(self, r):
        """A select's options: garbled, or a text tag shown as text."""
        at = r.get('at')
        for i, o in enumerate(r.get('options') or [], 1):
            text = o.get('text') or ''
            bad = self.garbled(text)
            if bad:
                self.add('text', ('option', at, i), at, 'option %d of the %s has %s' % (i, r.get('kind'), bad))
            m = TAGS.search(text)
            if m:
                self.add('text', ('option tag', at, i), at, 'option %d of the %s shows a text tag, %s: %s'
                         % (i, r.get('kind'), m.group(0), snippet(text, m.start())))

    def text(self, p):
        at, text = p.get('at'), p.get('text', '')
        shown = RUBY.sub(r'\1', text)
        shown = re.sub(r'<\d+|>', '', shown)
        bad = self.garbled(text)
        if bad:
            self.add('text', ('garbled', at), at, 'the text has %s' % bad)
        if not shown.strip(' \t\n\u3000'):
            self.add('text', ('empty', at), at, 'an empty page (a click wait with no text)')
        m = TAGS.search(shown)
        if m:
            self.add('text', ('tag', at), at, 'a text tag shown as text, %s: %s' % (m.group(0), snippet(shown, m.start())))
        prev = self.prev or {}
        # "overflow" stays on for the later click waits of the same page: report it once
        if p.get('overflow') and not (prev.get('overflow') and text.startswith(prev.get('text', '\0'))):
            self.add('text', ('overflow', at), at, 'the text runs past the window: %s' % snippet(shown, len(shown) - 20))
        elif self.args.line_chars and not p.get('overflow'):
            for line in shown.split('\n'):
                if width_of(line) > self.args.line_chars:
                    self.add('text', ('long', at), at, 'a line of %g characters (--line-chars %d): %s'
                             % (width_of(line), self.args.line_chars, snippet(line, len(line) - 20)))
                    break

    def reserved(self):
        """%, $ and ? in the script's text lines, reached or not."""
        label = None
        for i, line in enumerate(self.lines, 1):
            line = line.lstrip(' \t')
            m = re.match(r'\*+([^\s:;/]+)', line)
            if m:
                label = m.group(1).lower()
            if not line or re.match(r'[A-Za-z_*;~:{}]', line):
                continue
            body = re.sub(r'^\[[^\]]*\]', '', line)
            m = RESERVED.search(body)
            if m:
                at = '*%s:%d' % (label, i)
                self.pg = 0
                self.add('text', ('reserved', i), at, '%s in a text line prints a variable or reads an array: %s'
                         % (m.group(0), snippet(body, m.start())))

    def voices(self):
        a = self.args
        stats = defaultdict(lambda: [0, 0])  # speaker -> [voiced, lines]
        for pg, at, speaker, dialogue, voiced, vf in self.lines_seen:
            if speaker:
                stats[speaker][0] += voiced
                stats[speaker][1] += 1
        dl = [x for x in self.lines_seen if x[3]]
        w = a.voice_window
        for i, (pg, at, speaker, dialogue, voiced, vf) in enumerate(dl):
            if voiced:
                continue
            self.pg = pg
            if speaker:
                v, n = stats[speaker]
                if v >= a.voice_min and v >= n * a.voice_share:
                    self.add('voice', None, at, 'no voice for %s, who has one on %d of %d lines' % (speaker, v, n))
            elif w > 0 and i >= w and i + w < len(dl):
                around = dl[i - w:i] + dl[i + 1:i + 1 + w]
                if all(x[4] and x[2] is None and label_of(x[1]) == label_of(at) for x in around):
                    self.add('voice', None, at, 'no voice, but the %d lines before and after have one' % w)
        # one voice file for lines that say different things: a voice given to the wrong line
        for f, said in self.voice_lines.items():
            if len(said) > 1:
                (_, at1, t1), (self.pg, at2, t2) = list(said.values())[:2]
                self.add('voice', None, at2, '%s plays for different lines: %s here, %s at %s'
                         % (f, snippet(t2), snippet(t1), at1))

    def slow(self):
        items = sorted(self.stretches.items(), key=lambda kv: -kv[1][0])
        for (fr, to), (total, n, mx, pg, desc) in items[:self.args.slow]:
            if total < self.args.slow_ms:
                break
            self.pg = pg
            self.add('slow', None, to, '%d ms from %s to here (%s)%s' % (
                total, fr, desc, ', %d times, at most %d ms' % (n, mx) if n > 1 else ''))

    def memory(self):
        if not self.mem:
            return
        MB = 1 << 20
        a = self.args
        prev = None
        for pg, at, b, surf in self.mem:
            if prev is not None and b - prev >= a.mem_jump * MB:
                self.pg = pg
                self.add('memory', at, at, 'image memory jumped from %d to %d MB (%d pictures)' % (prev // MB, b // MB, surf))
            prev = b
        # what stays: the least over the next 100 pages
        bs = [m[2] for m in self.mem]
        floor = [min(bs[i:i + 100]) for i in range(len(bs))]
        segs = [min(floor[len(floor) * k // 10:max(len(floor) * (k + 1) // 10, len(floor) * k // 10 + 1)])
                for k in range(10)]
        grew = defaultdict(int)
        for i in range(1, len(floor)):
            if floor[i] > floor[i - 1]:
                grew[label_of(self.mem[i][1])] += floor[i] - floor[i - 1]
        top = sorted(grew.items(), key=lambda kv: -kv[1])[:3]
        hi = max(self.mem, key=lambda m: m[2])
        old = getattr(self, 'mem_summary', None)
        if old is None or hi[2] // MB >= old['max_mb']:
            self.mem_summary = {'min_mb': min(bs) // MB, 'max_mb': hi[2] // MB, 'max_page': hi[0], 'max_at': hi[1],
                                'end_mb': bs[-1] // MB, 'grew': {'*%s' % l: v // MB for l, v in top}}
        rises = sum(1 for x, y in zip(segs, segs[1:]) if y >= x)
        if len(bs) >= 20 and rises >= 8 and segs[-1] - segs[0] >= a.mem_leak * MB:
            self.pg = self.mem[-1][0]
            self.add('memory', None, self.mem[-1][1], 'image memory grows steadily, from %d MB to %d MB (a leak?); most in %s'
                     % (segs[0] // MB, segs[-1] // MB, ', '.join('*%s +%d MB' % (l, v // MB) for l, v in top)))

    def coverage(self):
        left = unreached(self.lines, self.labels, self.args.coverage_skip)
        if left:
            shown = ', '.join('*%s:%d' % (l, n) for n, l in left[:self.args.max * 3])
            more = ' ...' if len(left) > self.args.max * 3 else ''
            self.pg = 0
            self.add('coverage', None, None, '%d labels never reached: %s%s' % (len(left), shown, more))

    def report(self):
        mem = getattr(self, 'mem_summary', None)
        if self.args.json:
            json.dump({'counts': dict(self.total), 'end': self.end, 'crash': self.crash, 'memory': mem,
                       'runs': [{'trace': r[0], 'pages': r[1], 'end': r[4]} for r in self.runs],
                       'suppressed': dict(self.suppressed), 'in_baseline': self.in_baseline,
                       'script_wait_ms': self.wait_ms,
                       'findings': {r: [{'at': at, 'page': pg, 'message': m} for at, m, pg in self.findings[r]]
                                    for r in RULES if self.findings[r]}},
                      sys.stdout, ensure_ascii=False, indent=1)
            print()
            return
        for run in self.runs:
            print('%s: %d pages, %d commands, %d choices; %s' % run)
        if self.wait_ms:
            ms, pg, at = self.wait_most
            total = '%.1f minutes' % (self.wait_ms / 60000) if self.wait_ms >= 60000 else '%.1f s' % (self.wait_ms / 1000)
            print('script waits: %s in all, played by hand (text, waits, effects); the most, %.1f s, at page %d, %s'
                  % (total, ms / 1000, pg, at))
        if self.args.baseline:
            print('baseline %s: %d findings it had left out' % (self.args.baseline, self.in_baseline))
        if self.args.config:
            n = sum(self.suppressed.values())
            print('config %s%s' % (self.args.config, ': %d known findings left out (%s)' % (n, ', '.join(
                '%s %d' % kv for kv in sorted(self.suppressed.items()))) if n else ''))
        if mem:
            print('image memory: %d to %d MB (the most at page %d, %s), %d MB at the end%s' % (
                mem['min_mb'], mem['max_mb'], mem['max_page'], mem['max_at'], mem['end_mb'],
                '; it grew most in %s' % ', '.join('%s +%d MB' % kv for kv in mem['grew'].items()) if mem['grew'] else ''))
        total = 0
        pages = set()
        for rule in RULES:
            items = self.findings[rule]
            if not items:
                continue
            total += len(items)
            print('\n== %s (%d)' % (rule, len(items)))
            for at, m, pg in items[:self.args.max]:
                print('  %-24s %s' % (at, m))
                if rule not in ('coverage', 'missing', 'slow', 'unused') and pg > 0:
                    pages.add(pg)
            if len(items) > self.args.max:
                print('  ... %d more (--max)' % (len(items) - self.args.max))
        if not total:
            print('nothing suspicious')
        elif pages and len(self.runs) == 1:
            print('\nTo see these pages, run the game again the same way with:\n  --shot-pages %s'
                  % ','.join(str(p) for p in sorted(pages)))


def norm_file(f):
    """A file name as the engine finds it: / for \\, lower case, no ./ in front."""
    f = f.replace('\\', '/').lower()
    return f[2:] if f.startswith('./') else f


def finding_key(message):
    """A finding's message with its numbers left out (pages, counts), to match one across runs."""
    return re.sub(r'\d+', '#', message)


def overlays(p):
    # a screenflash as 'screenflash <opacity>', beside the filter and effect slots
    flash = [{'kind': 'flash', 'slot': p['flash'][0]}] if p.get('flash') else []
    # monocro and nega as 'monocro #rrggbb', 'nega 1'
    tints = [{'kind': k, 'slot': p[k], 'name': '%s %s' % (k, p[k])} for k in ('monocro', 'nega') if p.get(k)]
    return (p.get('filters') or []) + (p.get('fx') or []) + flash + tints


def page_state(p):
    sp = {(s['kind'], s['n']): (s.get('file') or s.get('str') or s.get('tag'), s.get('x'), s.get('y'),
                                 s.get('w'), s.get('h'), s.get('alpha'), s.get('cell'))
          for s in p.get('sp', [])}
    rest = {k: p.get(k) for k in ('bg', 'bgm', 'window', 'windowtrans', 'monocro', 'nega', 'filters', 'fx', 'flash')}
    return sp, rest


def moving(p):
    """Something on the page that changes with time, so its picture can differ between two runs."""
    return bool(p.get('fx')) or any(s.get(k) for s in p.get('sp', []) for k in ('anim', 'motion'))


def diff(old, new, limit):
    a = [r for r in records(old) if r.get('t') == 'page']
    b = [r for r in records(new) if r.get('t') == 'page']
    groups = []  # [first page, last page, at, lines]: the same difference on consecutive pages
    parted = None
    for pa, pb in zip(a, b):
        if pa.get('at') != pb.get('at'):
            parted = 'the runs part at page %d: %s in %s, %s in %s' % (pa['pg'], pa.get('at'), old, pb.get('at'), new)
            break
        spa, ra = page_state(pa)
        spb, rb = page_state(pb)
        lines = []
        for k in sorted(set(ra) | set(rb)):
            if ra.get(k) != rb.get(k):
                lines.append('%s: %s -> %s' % (k, json.dumps(ra.get(k), ensure_ascii=False), json.dumps(rb.get(k), ensure_ascii=False)))
        for k in sorted(set(spa) | set(spb)):
            if spa.get(k) != spb.get(k):
                fmt = lambda v: 'none' if v is None else '%s at %s,%s %sx%s alpha %s' % v[:6]
                lines.append('%s %d: %s -> %s' % (k[0], k[1], fmt(spa.get(k)), fmt(spb.get(k))))
        if not lines and pa.get('px') and pb.get('px') and pa['px'] != pb['px'] and not (moving(pa) or moving(pb)):
            lines.append('the picture differs, the state the same (px %s -> %s)' % (pa['px'], pb['px']))
        if not lines:
            continue
        if groups and groups[-1][3] == lines and groups[-1][1] == pa['pg'] - 1:
            groups[-1][1] = pa['pg']
        else:
            groups.append([pa['pg'], pa['pg'], pa.get('at'), lines])
    for first, last, at, lines in groups[:limit]:
        print('page %d %s%s' % (first, at, ' (the same to page %d)' % last if last > first else ''))
        for line in lines:
            print('  ' + line)
    if len(groups) > limit:
        print('... %d more (--max)' % (len(groups) - limit))
    if parted:
        print(parted)
    elif len(a) != len(b):
        print('%s has %d pages, %s %d' % (old, len(a), new, len(b)))
    if not groups:
        print('no differences in the pages both runs reached')


def find_config(args):
    """--config, or cons_trace_check.json beside the trace or the script."""
    if args.config:
        return args.config
    for path in (args.trace[0], args.script):
        if path:
            d = path if os.path.isdir(path) else os.path.dirname(os.path.abspath(path))
            f = os.path.join(d, 'cons_trace_check.json')
            if os.path.exists(f):
                return f
    return None


def apply_config(ap, args):
    """Fill the options not given on the command line from the config, then the defaults."""
    conf = {}
    args.config = find_config(args)
    if args.config:
        try:
            with open(args.config, encoding='utf-8') as f:
                conf = json.load(f)
        except (OSError, ValueError) as e:
            ap.error('can\'t read the config %s: %s' % (args.config, e))
    known = []
    for k, v in conf.items():
        key = k.replace('-', '_')
        if key.startswith('_') or key == 'comment':
            continue
        if key == 'known':
            for item in v:
                rx = item.get('match')
                if rx is None and 'file' in item:
                    rx = re.escape(item['file'])
                elif rx is None and 'sprite' in item:
                    kind, _, n = str(item['sprite']).rpartition(':')
                    rx = r'\b%s %s\b' % (kind or 'lsp', n)
                if rx is None:
                    ap.error('%s: a "known" entry needs "match", "file" or "sprite": %s' % (args.config, item))
                known.append(dict(item, re=re.compile(rx)))
            continue
        if key not in DEFAULTS:
            ap.error('%s: unknown setting "%s"' % (args.config, k))
        if getattr(args, key) is None:
            if key == 'script' and v:
                v = os.path.join(os.path.dirname(os.path.abspath(args.config)), v)
            elif key == 'skip' and isinstance(v, list):
                v = ','.join(v)
            setattr(args, key, v)
    for key, v in DEFAULTS.items():
        if getattr(args, key) is None:
            setattr(args, key, v)
    args.known = known


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('trace', nargs='*')
    ap.add_argument('--diff', nargs=2, metavar=('OLD', 'NEW'))
    ap.add_argument('--config', metavar='FILE', help='the settings for a game (default: cons_trace_check.json beside the trace or the script)')
    ap.add_argument('--root', metavar='DIR', help='the game folder, to list image and sound files no trace loaded')
    ap.add_argument('--baseline', metavar='OLD.json', help='an earlier --json report: leave out the findings it had')
    ap.add_argument('--script', help="the game's 0.txt (or its folder), to list labels never reached and check text lines")
    ap.add_argument('--ignore', metavar='LIST', help='sprites to leave out, as --trace-ignore (22-48,lsp2:12-23)')
    ap.add_argument('--skip', metavar='RULES', help='rules to leave out, comma-separated (slow,coverage)')
    ap.add_argument('--coverage-skip', metavar='REGEX', help='labels to leave out of the coverage list')
    ap.add_argument('--cg', metavar='REGEX', help='backgrounds that are CGs (no filter or effect expected over them)')
    ap.add_argument('--max', type=int, help='findings shown per kind (default 20)')
    ap.add_argument('--json', action='store_true', help='print the findings as JSON')
    ap.add_argument('--tall', type=float, help='a picture at least this part of the screen height counts as a character or CG (default 0.4)')
    ap.add_argument('--linger', type=int, help='pages a tall picture may stay up (default 150)')
    ap.add_argument('--float-h', type=float, help='height, as part of the screen, of pictures checked for floating (default 0.75)')
    ap.add_argument('--float-gap', type=int, help='pixels above the bottom that count as floating (default 40)')
    ap.add_argument('--crowd', type=int, help='tall pictures allowed at once (default 4)')
    ap.add_argument('--loop', type=int, help='pages a looping sound may play (default 100)')
    ap.add_argument('--shake', type=int, help='pages a screenshake with no end may shake (default 30)')
    ap.add_argument('--voice-ch', metavar='LIST', help='channels that play voices (default 0)')
    ap.add_argument('--voice', metavar='REGEX', help='sound files that are voices (default: in a "voice" folder)')
    ap.add_argument('--dialogue', metavar='REGEX', help='page text that is a spoken line (default: starts with \u300c, \u300e or \u201c)')
    ap.add_argument('--voice-window', type=int, help='lines each side that must have a voice for a line without one to be reported, when the speaker is unknown (default 3; 0 = off)')
    ap.add_argument('--voice-share', type=float, help='part of a speaker\'s lines with a voice for one without to be reported (default 0.8)')
    ap.add_argument('--voice-min', type=int, help='lines with a voice a speaker needs first (default 5)')
    ap.add_argument('--line-chars', type=int, help='full-width characters a line may hold, for traces without "overflow" (default: off)')
    ap.add_argument('--save-ignore', metavar='REGEX', help='save/load mismatch fields to leave out, such as variables the game\'s loadgosub sets (^%%190$)')
    ap.add_argument('--slow', type=int, help='slowest stretches shown (default 10)')
    ap.add_argument('--slow-ms', type=int, help='least total ms for a stretch to be shown (default 1000)')
    ap.add_argument('--mem-jump', type=int, help='MB image memory may grow from one page to the next (default 64)')
    ap.add_argument('--mem-leak', type=int, help='MB of steady growth reported as a leak (default 128)')
    args = ap.parse_args()
    if args.diff:
        diff(args.diff[0], args.diff[1], args.max or DEFAULTS['max'])
        return
    if not args.trace:
        ap.error('give a trace, or --diff OLD NEW')
    apply_config(ap, args)
    c = Checker(args)
    if args.baseline:
        try:
            with open(args.baseline, encoding='utf-8') as f:
                old = json.load(f)
        except (OSError, ValueError) as e:
            ap.error("can't read the baseline %s: %s" % (args.baseline, e))
        c.baseline = {(rule, x['at'], finding_key(x['message']))
                      for rule, xs in old.get('findings', {}).items() for x in xs}
    for t in args.trace:
        c.run(t)
    c.finish()
    c.report()


if __name__ == '__main__':
    main()
