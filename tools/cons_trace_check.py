#!/usr/bin/env python3
"""Check a CONscripter debug trace (--trace) for suspicious states.

usage: cons_trace_check.py TRACE [--script 0.txt] [--config FILE] [--cg REGEX] [--max N] [--json]
       cons_trace_check.py --diff OLD NEW [--max N]

Reads the trace (DEBUG.md describes it) and prints what looks wrong, ranked
by kind, each finding at the script line ("*label:line") to look at:

  error        script and startup errors; a crash (its signal and last
               commands); a run that ended without "end" or got stuck
  missing      files that couldn't be loaded, grouped by file
  text         garbled characters, text past the window, empty pages,
               text tags shown as text; with --script, %, $ or ? in a text
               line (they print a variable)
  sound        the same sound on two channels at once; a voice cut off by
               the next one before a click wait
  voice        a line with no voice where the same speaker (or the lines
               around it) has one
  linger       a sprite still up after the background changed, or for many pages
  float        a tall picture whose bottom edge stops just above the screen's
  offscreen    a sprite entirely off the screen
  invisible    a sprite shown at alpha 0
  crowd        too many tall pictures at once, or two at the same place
  overlay      a screen filter or effect still on after the background changed,
               or (--cg) any filter or effect over a background matching REGEX
  loop         a looping sound left playing for many pages
  duration     a wait or effect the script asks to last very long
  slow         where the time went: the slowest stretches between records
  save         a state that changed after a save and load (--roundtrip-every)
  replay       a --replay run that went another way, or ran out of choices
  memory       image memory that grows steadily or jumps; a single picture
               taking a lot of it (--trace-max-image)
  coverage     (--script) labels the run never reached

Settings for a game can go in a JSON file, cons_trace_check.json beside the
trace or the script, or --config FILE: any option below by name, "ignore"
(sprites to leave out, as --trace-ignore), and "known" (findings to leave
out). Options given on the command line win.

--diff compares two traces of the same route page by page (before and after
a fix) and prints the pages whose state differs.
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
         'memory', 'coverage']

# Options a config file may set, with their defaults.
DEFAULTS = {
    'script': None, 'cg': None, 'coverage_skip': None, 'ignore': None, 'skip': None,
    'max': 20, 'tall': 0.4, 'linger': 150, 'float_h': 0.75, 'float_gap': 40, 'crowd': 4,
    'loop': 100, 'voice_ch': '0', 'voice': r'(^|[\\/])voices?[\\/]',
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
        self.lines_seen = []               # (pg, at, speaker, dialogue, voiced) for the voice rule
        self.last_ms = None                # the last record with "ms"
        self.stretches = {}                # (from at, to at) -> [total ms, count, max, pg, desc]
        self.mem = []                      # (pg, at, bytes, surfaces)
        self.starts = []                   # (channel, file) started since the last page
        self.reloading = False             # between a "roundtrip" record and the next page
        self.mismatch = OrderedDict()      # the fields that differed -> [round trips, first at, page, diff]
        self.twice = OrderedDict()         # file -> [first at, page, channels, pages] played on two channels

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

    def run(self, path):
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
            if t == 'label':
                self.labels.add(str(r.get('name')).lower())
            elif t == 'meta':
                self.meta = r
                self.w, self.h = r.get('w') or self.w, r.get('h') or self.h
                if self.args.script:
                    self.lines = script_lines(self.args.script, r.get('utf8'))
            elif t == 'ev':
                self.ev(r)
            elif t in ('page', 'dump'):
                if self.ignore:
                    r['sp'] = [s for s in r.get('sp', []) if (s['kind'], s['n']) not in self.ignore]
                if t == 'page':
                    self.page(r)
            elif t == 'warn':
                self.warn(r)
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
        elif self.end and str(self.end.get('reason', '')).startswith('stuck') and not self.findings['error']:
            self.add('error', None, self.prev.get('at') if self.prev else None,
                     'auto-play stopped: %s' % self.end['reason'])
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
        self.slow()
        self.memory()
        if self.lines is not None:
            self.reserved()
            self.coverage()

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
        elif self.reloading:
            pass  # the load of a --roundtrip-every repeats the scene's warnings
        else:
            key = (what, r.get('file'))
            if key not in self.missing:
                self.missing[key] = [0, at]
            self.missing[key][0] += 1

    def ev(self, r):
        sp = r.get('sp')
        if sp and self.ignore:
            r['sp'] = sp = [s for s in sp if (s['kind'], s['n']) not in self.ignore]
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
            if s.get('alpha') == 0:
                self.add('invisible', key, at, '%s is shown at alpha 0%s' % (name, where))

        tall = [s for s in sp if self.tall(s) and not s.get('off') and not s.get('missing')]
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

        overlay = (p.get('filters') or []) + (p.get('fx') or [])
        if overlay:
            desc = ', '.join('screenfilter %d (%s)' % (o['slot'], FILTER_KINDS.get(o['kind'], o['kind']))
                             if isinstance(o['kind'], int) else 'screen%s %d' % (o['kind'], o['slot'])
                             for o in overlay)
            if bg_changed and ((prev.get('filters') or []) + (prev.get('fx') or [])) == overlay:
                self.add('overlay', (p.get('bg'), desc), at, '%s still on when the background changed %s -> %s'
                         % (desc, prev.get('bg'), p.get('bg')))
            if a.cg and re.search(a.cg, p.get('bg') or ''):
                self.add('overlay', ('cg', p.get('bg'), desc), at, '%s over the CG %s' % (desc, p.get('bg')))

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

        self.text(p)
        # a voice that started for this page: a sound command, or one in "se" the last page didn't have
        old = {(s['ch'], s['file']) for s in (prev or {}).get('se', [])}
        voiced = self.voiced or any(self.is_voice(s['ch'], s['file']) and (s['ch'], s['file']) not in old for s in se)
        self.voiced = False
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
        self.lines_seen.append((pg, at, speaker, dialogue, voiced))
        if 'mem' in p and isinstance(p['mem'], dict):
            self.mem.append((pg, at, p['mem'].get('bytes', 0), p['mem'].get('surf', 0)))
        self.prev = p

    def text(self, p):
        at, text = p.get('at'), p.get('text', '')
        shown = RUBY.sub(r'\1', text)
        shown = re.sub(r'<\d+|>', '', shown)
        for rx, what in GARBLED:
            m = rx.search(text)
            if m:
                self.add('text', ('garbled', at), at, 'the text has %s: %s' % (what, snippet(text, m.start())))
                break
        else:
            m = HALF_KANA.search(text)
            if m and not self.meta.get('utf8'):
                self.add('text', ('garbled', at), at, 'the text has kanji and half-width katakana in turn (mojibake?): %s'
                         % snippet(text, m.start()))
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
        for pg, at, speaker, dialogue, voiced in self.lines_seen:
            if speaker:
                stats[speaker][0] += voiced
                stats[speaker][1] += 1
        dl = [x for x in self.lines_seen if x[3]]
        w = a.voice_window
        for i, (pg, at, speaker, dialogue, voiced) in enumerate(dl):
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
            json.dump({'counts': dict(self.counts), 'end': self.end, 'crash': self.crash, 'memory': mem,
                       'suppressed': dict(self.suppressed),
                       'findings': {r: [{'at': at, 'page': pg, 'message': m} for at, m, pg in self.findings[r]]
                                    for r in RULES if self.findings[r]}},
                      sys.stdout, ensure_ascii=False, indent=1)
            print()
            return
        end = self.end.get('reason', 'ended') if self.end else 'crashed' if self.crash else 'no end record'
        print('%s: %d pages, %d commands, %d choices; %s' % (
            self.args.trace, self.counts['page'], self.counts['ev'], self.counts['choice'], end))
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
                if rule not in ('coverage', 'missing', 'slow') and pg > 0:
                    pages.add(pg)
            if len(items) > self.args.max:
                print('  ... %d more (--max)' % (len(items) - self.args.max))
        if not total:
            print('nothing suspicious')
        elif pages:
            print('\nTo see these pages, run the game again the same way with:\n  --shot-pages %s'
                  % ','.join(str(p) for p in sorted(pages)))


def page_state(p):
    sp = {(s['kind'], s['n']): (s.get('file') or s.get('str') or s.get('tag'), s.get('x'), s.get('y'),
                                 s.get('w'), s.get('h'), s.get('alpha'), s.get('cell'))
          for s in p.get('sp', [])}
    rest = {k: p.get(k) for k in ('bg', 'bgm', 'window', 'windowtrans', 'monocro', 'nega', 'filters', 'fx')}
    return sp, rest


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
    for path in (args.trace, args.script):
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
    ap.add_argument('trace', nargs='?')
    ap.add_argument('--diff', nargs=2, metavar=('OLD', 'NEW'))
    ap.add_argument('--config', metavar='FILE', help='the settings for a game (default: cons_trace_check.json beside the trace or the script)')
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
    c.run(args.trace)
    c.report()


if __name__ == '__main__':
    main()
