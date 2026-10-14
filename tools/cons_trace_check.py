#!/usr/bin/env python3
"""Check a CONscripter debug trace (--trace) for suspicious states.

usage: cons_trace_check.py TRACE [--script 0.txt] [--cg REGEX] [--max N] [--json]
       cons_trace_check.py --diff OLD NEW [--max N]

Reads the trace (DEBUG.md describes it) and prints what looks wrong, ranked
by kind, each finding at the script line ("*label:line") to look at:

  error        script errors; a run that crashed (no "end") or got stuck
  missing      files that couldn't be loaded, grouped by file
  linger       a sprite still up after the background changed, or for many pages
  float        a tall picture whose bottom edge stops just above the screen's
  offscreen    a sprite entirely off the screen
  invisible    a sprite shown at alpha 0
  crowd        too many tall pictures at once, or two at the same place
  overlay      a screen filter or effect still on after the background changed,
               or (--cg) any filter or effect over a background matching REGEX
  loop         a looping sound left playing for many pages
  coverage     (--script) labels the run never reached

--diff compares two traces of the same route page by page (before and after
a fix) and prints the pages whose state differs.
"""
import argparse
import json
import re
import sys
from collections import OrderedDict, defaultdict

FILTER_KINDS = {1: 'colour', 2: 'greyscale', 3: 'negative', 4: 'brightness', 5: 'monotone',
                6: 'blur', 7: 'hue/saturation', 8: 'lightness invert'}

RULES = ['error', 'missing', 'linger', 'float', 'offscreen', 'invisible',
         'crowd', 'overlay', 'loop', 'coverage']


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


class Checker:
    def __init__(self, args):
        self.args = args
        self.w, self.h = 800, 600
        self.findings = defaultdict(list)  # rule -> [(at, message, page)]
        self.pg = 0                        # the page being read
        self.seen = set()                  # keys already reported
        self.set_by = {}                   # (kind, n) -> (at, src) of the ev that last showed it
        self.prev = None                   # the previous page
        self.loops = {}                    # file -> first page it was seen looping
        self.labels = set()                # labels reached
        self.missing = OrderedDict()       # (what, file) -> [count, first at]
        self.counts = defaultdict(int)
        self.end = None
        self.meta = {}

    def add(self, rule, key, at, message):
        if key is not None:
            if (rule, key) in self.seen:
                return
            self.seen.add((rule, key))
        self.findings[rule].append((at or '?', message, self.pg))

    def tall(self, s):
        return s.get('h', 0) >= self.h * self.args.tall

    def run(self, path):
        for r in records(path):
            t = r.get('t')
            self.counts[t] += 1
            if t == 'page':
                self.pg = r['pg']
            for k in ('at', 'via'):
                if r.get(k):
                    self.labels.add(label_of(r[k]))
            if t == 'label':
                self.labels.add(r.get('name'))
            elif t == 'meta':
                self.meta = r
                self.w, self.h = r.get('w', self.w), r.get('h', self.h)
            elif t == 'ev':
                for s in r.get('sp', []):
                    if not s.get('gone'):
                        self.set_by[(s['kind'], s['n'])] = (r.get('at'), r.get('src', ''))
            elif t == 'page':
                self.page(r)
            elif t == 'warn':
                if r.get('what') == 'auto-play stuck':
                    self.add('error', None, r.get('at'), 'auto-play stuck here (going round in circles?)')
                    continue
                key = (r.get('what'), r.get('file'))
                if key not in self.missing:
                    self.missing[key] = [0, r.get('at')]
                self.missing[key][0] += 1
            elif t == 'error':
                stack = ' <- '.join(r.get('stack', []))
                self.add('error', None, r.get('at'),
                         'script error: %s%s' % (r.get('msg'), ' (called from %s)' % stack if stack else ''))
            elif t == 'end':
                self.end = r
            elif t == 'broken':
                self.add('error', None, None, 'line %d of the trace is cut off' % r['line'])

        if self.end is None:
            last = self.prev.get('at') if self.prev else None
            self.add('error', None, last, 'the trace has no "end": the engine crashed or was killed after this page')
        elif str(self.end.get('reason', '')).startswith('stuck') and not self.findings['error']:
            self.add('error', None, self.prev.get('at') if self.prev else None,
                     'auto-play stopped: %s' % self.end['reason'])
        for (what, f), (n, at) in self.missing.items():
            self.add('missing', None, at, '%s %s%s' % (what, f, ' (%d times)' % n if n > 1 else ''))
        if self.args.script:
            self.coverage()

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

        looping = {s['file'] for s in p.get('se', []) if s.get('loop')}
        for f in list(self.loops):
            if f not in looping:
                del self.loops[f]
        for f in looping:
            first = self.loops.setdefault(f, (pg, at))
            if pg - first[0] >= a.loop:
                self.add('loop', (f, first[0]), first[1], 'looping sound %s has played for %d pages' % (f, pg - first[0]))

        self.prev = p

    def coverage(self):
        enc = 'utf-8' if self.meta.get('utf8') else 'cp932'
        unreached = []
        skip = re.compile(self.args.coverage_skip) if self.args.coverage_skip else None
        with open(self.args.script, encoding=enc, errors='replace') as f:
            for n, line in enumerate(f, 1):
                m = re.match(r'\s*\*+([^\s:;/]+)', line)
                if m and m.group(1) not in self.labels and not (skip and skip.search(m.group(1))):
                    unreached.append((n, m.group(1)))
        if unreached:
            shown = ', '.join('*%s:%d' % (l, n) for n, l in unreached[:self.args.max * 3])
            more = ' ...' if len(unreached) > self.args.max * 3 else ''
            self.add('coverage', None, None, '%d labels never reached: %s%s' % (len(unreached), shown, more))

    def report(self):
        if self.args.json:
            json.dump({'counts': dict(self.counts), 'end': self.end,
                       'findings': {r: [{'at': at, 'page': pg, 'message': m} for at, m, pg in self.findings[r]]
                                    for r in RULES if self.findings[r]}},
                      sys.stdout, ensure_ascii=False, indent=1)
            print()
            return
        end = self.end.get('reason', 'ended') if self.end else 'no end record'
        print('%s: %d pages, %d commands, %d choices; %s' % (
            self.args.trace, self.counts['page'], self.counts['ev'], self.counts['choice'], end))
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
                if rule not in ('coverage', 'missing') and pg > 0:
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('trace', nargs='?')
    ap.add_argument('--diff', nargs=2, metavar=('OLD', 'NEW'))
    ap.add_argument('--script', help="the game's 0.txt, to list labels never reached")
    ap.add_argument('--coverage-skip', metavar='REGEX', help='labels to leave out of the coverage list')
    ap.add_argument('--cg', metavar='REGEX', help='backgrounds that are CGs (no filter or effect expected over them)')
    ap.add_argument('--max', type=int, default=20, help='findings shown per kind (default 20)')
    ap.add_argument('--json', action='store_true', help='print the findings as JSON')
    ap.add_argument('--tall', type=float, default=0.4, help='a picture at least this part of the screen height counts as a character or CG (default 0.4)')
    ap.add_argument('--linger', type=int, default=150, help='pages a tall picture may stay up (default 150)')
    ap.add_argument('--float-h', type=float, default=0.75, help='height, as part of the screen, of pictures checked for floating (default 0.75)')
    ap.add_argument('--float-gap', type=int, default=40, help='pixels above the bottom that count as floating (default 40)')
    ap.add_argument('--crowd', type=int, default=4, help='tall pictures allowed at once (default 4)')
    ap.add_argument('--loop', type=int, default=100, help='pages a looping sound may play (default 100)')
    args = ap.parse_args()
    if args.diff:
        diff(args.diff[0], args.diff[1], args.max)
        return
    if not args.trace:
        ap.error('give a trace, or --diff OLD NEW')
    c = Checker(args)
    c.run(args.trace)
    c.report()


if __name__ == '__main__':
    main()
