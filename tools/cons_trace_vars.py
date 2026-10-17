#!/usr/bin/env python3
"""Variables from a CONscripter debug trace, for starting a game anywhere.

usage: cons_trace_vars.py TRACE [--page N | --at PLACE] [-o FILE]
       cons_trace_vars.py --diff OLD NEW [--only LIST]

The first form writes a --vars file (lines "%312=876" and "$5=text") with
the variables at a moment of the run, for the engine's --goto LABEL --vars
FILE. It takes them from the last "dump" record (F8) at or before that
moment if there is one, otherwise it plays back the "var" records
(--trace-vars) up to it: the start of page N, or the first time the run
reached PLACE ("*label:line", or "*label" for its first line run). With
neither, the end of the run.

--diff compares the "var" records of two traces (two runs, or the engine's
against a converter's tracer) and prints the first change that differs.
"""
import argparse
import json
import sys


def records(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            try:
                yield json.loads(line)
            except ValueError:
                pass


def reached(r, at):
    """Whether record r is at PLACE: "*label:line", or "*label" (any line in it)."""
    a = r.get('at')
    if not isinstance(a, str):
        return False
    return a == at if ':' in at else a.split(':')[0] == at


def collect(path, page=None, at=None):
    """The variables at the moment asked for: (vars, where, how)."""
    vars_, dump, where = {}, None, 'the end of the run'
    for r in records(path):
        t = r.get('t')
        if page is not None and t == 'page' and r['pg'] >= page:
            where = 'the start of page %d (%s)' % (page, r.get('at'))
            break
        if at is not None and (reached(r, at) or (t == 'label' and '*' + str(r.get('name')) == at.lower())):
            where = '%s (page %d)' % (r.get('at') or at, r.get('pg', 0))
            break
        if t == 'var':
            vars_[r['n']] = r['v']
        elif t == 'dump' and 'vars' in r:
            dump = r
    else:
        if page is not None or at is not None:
            sys.exit('the trace never reached %s' % ('page %d' % page if page is not None else at))
    if dump is not None:
        # a dump has every variable then; var records after it bring it up to date
        out = dict(dump['vars'])
        seen = False
        for r in records(path):
            if r is dump or (r.get('t') == 'dump' and r.get('pg') == dump.get('pg') and r.get('at') == dump.get('at')):
                seen = True
                continue
            if seen and r.get('t') == 'var':
                if page is not None and r.get('pg', 0) >= page:
                    break
                out[r['n']] = r['v']
            if seen and at is not None and reached(r, at):
                break
        return out, where, 'the F8 dump at page %d (%s), with the var records after it' % (dump.get('pg', 0), dump.get('at'))
    return vars_, where, 'the var records'


def key(n):
    return (n[0] != '%', int(n[1:]) if n[1:].isdigit() else 0, n)


def write(vars_, out, header):
    lines = ['# ' + header]
    for n in sorted(vars_, key=key):
        v = vars_[n]
        if n[0] in '%?' and v != 0:  # ?name[i]: a defarr element
            lines.append('%s=%d' % (n, v))
        elif n.startswith('$') and v != '':
            lines.append('%s=%s' % (n, str(v).replace('\r', '').replace('\n', ' ')))
    text = '\n'.join(lines) + '\n'
    if out:
        with open(out, 'w', encoding='utf-8') as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return len(lines) - 1


def parse_only(spec):
    """'300-899,$10-20' -> a test for a variable's name."""
    if not spec:
        return lambda n: True
    ranges = []
    for tok in spec.split(','):
        tok = tok.strip()
        kind = '$' if tok.startswith('$') else '%'
        a, _, b = tok.lstrip('$%').partition('-')
        ranges.append((kind, int(a), int(b or a)))
    return lambda n: any(n[0] == k and a <= int(n[1:]) <= b for k, a, b in ranges if n[1:].isdigit())


def diff(old, new, only):
    a = [r for r in records(old) if r.get('t') == 'var' and only(r['n'])]
    b = [r for r in records(new) if r.get('t') == 'var' and only(r['n'])]
    for i, (ra, rb) in enumerate(zip(a, b)):
        if (ra['n'], ra['v']) != (rb['n'], rb['v']):
            print('change %d differs:' % (i + 1))
            for path, r in ((old, ra), (new, rb)):
                print('  %s: %s=%s at %s (page %s)' % (path, r['n'], json.dumps(r['v'], ensure_ascii=False), r.get('at'), r.get('pg')))
            if i:
                p = a[i - 1]
                print('  the change before, the same in both: %s=%s at %s' % (p['n'], json.dumps(p['v'], ensure_ascii=False), p.get('at')))
            return 1
    if len(a) != len(b):
        longer, r = (old, a[len(b)]) if len(a) > len(b) else (new, b[len(a)])
        print('the first %d changes are the same; %s goes on with %s=%s at %s'
              % (min(len(a), len(b)), longer, r['n'], json.dumps(r['v'], ensure_ascii=False), r.get('at')))
        return 1
    print('the same %d changes in both' % len(a))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('trace', nargs='?')
    ap.add_argument('--page', type=int, help='the variables at the start of this page (the trace\'s pg)')
    ap.add_argument('--at', metavar='PLACE', help='the variables when the run first reached *label:line (or *label)')
    ap.add_argument('-o', '--out', metavar='FILE', help='write the --vars file here (default: print it)')
    ap.add_argument('--diff', nargs=2, metavar=('OLD', 'NEW'), help='the first var record that differs between two traces')
    ap.add_argument('--only', metavar='LIST', help='with --diff, only these variables: 300-899,$10-20')
    args = ap.parse_args()
    if args.diff:
        sys.exit(diff(args.diff[0], args.diff[1], parse_only(args.only)))
    if not args.trace:
        ap.error('give a trace, or --diff OLD NEW')
    if args.page is not None and args.at:
        ap.error('give --page or --at, not both')
    vars_, where, how = collect(args.trace, args.page, args.at)
    n = write(vars_, args.out, 'variables at %s of %s, from %s' % (where, args.trace, how))
    if args.out:
        print('%s: %d variables at %s' % (args.out, n, where), file=sys.stderr)


if __name__ == '__main__':
    main()
