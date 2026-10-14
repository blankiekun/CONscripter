#!/usr/bin/env python3
"""Explore a game's routes: play it again and again, each run taking choices
the earlier runs didn't, until a run reaches no new label.

usage: cons_explore.py GAME_DIR [--out DIR] [--runs N] [--pages N] [--seed N]
                       [--engine PATH] [--engine-args "ARGS"] [--script 0.txt]

Each run is an --auto-play run of the engine with --auto-choice untaken and
--taken FILE, the file listing every choice the earlier runs made and how
many times (built from their traces' choice records, as lines
"*label:line<TAB>option<TAB>count"), so each run takes the options taken
least. A run that ends in a script error marks the last choice it made
before the error (one not marked already) "avoid" in the file, so later
runs make another there; when there is none to mark, every run would end
at that error, so the exploration stops. It
stops when --patience runs in a row reach no label the earlier ones hadn't,
or after --runs runs, then lists the labels of the script no run reached and the traces it
wrote. Each run gets its own trace and save folder in --out, so the game's
own saves are never touched. Give the game's button bar to --engine-args as
--trace-ignore, so the runs don't press its buttons, and its title menu and
settings screens to --skip-choices, so each run starts the story.

  cons_explore.py out/game-cons --out /tmp/explore --engine-args "--trace-ignore 22-48,lsp2:12-23"
"""
import argparse
import collections
import os
import re
import shlex
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode = True  # no __pycache__ beside the checker
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cons_trace_check import records, script_lines, trace_labels, unreached  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(os.path.dirname(HERE), 'build_native', 'conscripter')


def choices(trace, skip=None):
    """(at, option) for each choice a trace made."""
    out = []
    for r in records(trace):
        if r.get('t') != 'choice' or not r.get('at'):
            continue
        opt = r.get('picked') if 'picked' in r else r.get('result')
        if isinstance(opt, int) and opt >= 0 and not (skip and skip.search(r['at'])):
            out.append((r['at'], opt))
    return out


def end_of(trace):
    meta, end = {}, None
    pages = 0
    for r in records(trace):
        t = r.get('t')
        if t == 'meta':
            meta = r
        elif t == 'page':
            pages = r['pg']
        elif t in ('crash', 'error') or (t == 'end' and end is None):
            end = r
    return meta, pages, end


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('game', help='the game folder (the engine runs there with --root .)')
    ap.add_argument('--out', default='explore', help='where the traces, save folders and the taken file go (default explore)')
    ap.add_argument('--runs', type=int, default=20, help='the most runs (default 20)')
    ap.add_argument('--pages', type=int, default=0, help='pages per run, 0 for to the end (default 0)')
    ap.add_argument('--patience', type=int, default=2, help='stop after this many runs in a row reach no new label (default 2)')
    ap.add_argument('--skip-choices', metavar='REGEX', help='places ("*label:line") whose choices are left out of the taken file, so every run makes them as the first did: a title menu, a settings screen')
    ap.add_argument('--seed', type=int, help='--seed for every run, so the exploration can be repeated')
    ap.add_argument('--engine', default=ENGINE, help='the engine (default build_native/conscripter)')
    ap.add_argument('--engine-args', default='', metavar='"ARGS"', help='more engine options, such as "--trace-ignore 22-48"')
    ap.add_argument('--script', help="the game's script, for the labels no run reached (default GAME/0.txt)")
    ap.add_argument('--coverage-skip', metavar='REGEX', help='labels to leave out of that list')
    ap.add_argument('--stop-known', type=int, default=1000, metavar='PAGES',
                    help='end a run that has gone its own way after this many pages back on ground earlier runs reached '
                         '(the engine\'s --stop-known; before that it may play ten times as many); 0 plays every run to its end (default 1000)')
    ap.add_argument('--timeout', type=int, default=1800, help='seconds a run may take (default 1800)')
    ap.add_argument('--max', type=int, default=60, help='unreached labels listed (default 60)')
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    engine = os.path.abspath(args.engine)
    taken = os.path.join(out, 'taken.txt')
    known = os.path.join(out, 'known.txt')
    env = dict(os.environ)
    env.setdefault('SDL_VIDEODRIVER', 'dummy')
    env.setdefault('SDL_AUDIODRIVER', 'dummy')

    reached, made, traces = set(), collections.Counter(), []
    avoid = set()  # (at, option) choices that led to a script error
    errors = {}  # (at, msg) -> [runs that ended there, choices avoided for it]
    skip = re.compile(args.skip_choices) if args.skip_choices else None
    idle = 0
    meta = {}
    def write_taken():
        with open(taken, 'w', encoding='utf-8') as f:
            f.write('# choices made by the earlier runs\n')
            for (at, opt), n in sorted(made.items()):
                f.write('%s\t%d\t%d\n' % (at, opt, n))
            for at, opt in sorted(avoid):
                f.write('%s\t%d\tavoid\n' % (at, opt))
        with open(known, 'w', encoding='utf-8') as f:
            f.write('# labels reached by the earlier runs\n')
            for label in sorted(reached):
                f.write(label + '\n')

    for run in range(1, args.runs + 1):
        write_taken()
        trace = os.path.join(out, 'run%02d.jsonl' % run)
        saves = os.path.join(out, 'saves%02d' % run)
        shutil.rmtree(saves, ignore_errors=True)
        os.makedirs(saves)
        cmd = [engine, '--root', '.', '--save-dir', saves, '--trace', trace,
               '--auto-play', str(args.pages), '--auto-choice', 'untaken', '--taken', taken]
        if args.seed is not None:
            cmd += ['--seed', str(args.seed)]
        if args.stop_known > 0:
            cmd += ['--known-labels', known, '--stop-known', str(args.stop_known)]
        cmd += shlex.split(args.engine_args)
        start = time.time()
        try:
            code = subprocess.run(cmd, cwd=args.game, env=env, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, timeout=args.timeout).returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
        if not os.path.exists(trace):
            sys.exit('run %d wrote no trace (exit %s): %s' % (run, code, ' '.join(shlex.quote(c) for c in cmd)))
        traces.append(trace)
        meta, pages, end = end_of(trace)
        labels = trace_labels(trace)
        new = labels - reached
        reached |= labels
        mine = choices(trace, skip)
        fresh = set(mine) - set(made)
        made.update(mine)
        end = end or {}
        how = end.get('reason') or {'crash': 'crashed (%s)' % end.get('signal'),
                                    'error': 'error at %s: %s' % (end.get('at'), end.get('msg'))}.get(end.get('t'), 'ended')
        stop = blame = None
        if str(end.get('reason', '')).startswith('signal '):
            # stopped from outside (timeout, pkill, ^C): not a finished run
            stop = 'run %d was killed (%s): stopping' % (run, end['reason'][7:])
        elif end.get('t') == 'error':
            # the last choice before the error not avoided already (an
            # avoided one is made only when its place has nothing else)
            err = errors.setdefault((end.get('at'), end.get('msg')), [0, []])
            err[0] += 1
            blame = next((c for c in reversed(mine) if c not in avoid), None)
            if blame:
                avoid.add(blame)
                err[1].append(blame)
                how += '; avoiding %s %d' % blame
            else:
                stop = 'no choice left to avoid before the error: every run would end there'
        print('run %d: %d pages, %d choices (%d new), %d new labels, %d in all; %s, exit %s, %.0f s'
              % (run, pages, len(mine), len(fresh), len(new), len(reached), how, code, time.time() - start))
        sys.stdout.flush()
        if stop:
            print(stop)
            break
        idle = 0 if new or blame else idle + 1
        if idle >= args.patience:
            print('%d run%s in a row reached no new label: done' % (idle, 's' if idle > 1 else ''))
            break
    else:
        print('stopped after %d runs (--runs)' % args.runs)
    write_taken()

    if errors:
        print('\nscript errors:')
        for (at, msg), (n, blamed) in sorted(errors.items(), key=lambda e: -e[1][0]):
            print('  %s: %s: %d run%s%s' % (at, msg, n, 's' if n > 1 else '',
                  '; avoided ' + ', '.join('%s %d' % c for c in blamed) if blamed else ''))

    script = args.script or os.path.join(args.game, '0.txt')
    if os.path.exists(script):
        lines = script_lines(script, meta.get('utf8'))
        left = unreached(lines, reached, args.coverage_skip)
        total = len(unreached(lines, set(), args.coverage_skip))
        print('\n%d of %d labels reached; %d never:' % (total - len(left), total, len(left)))
        if left:
            print('  ' + ', '.join('*%s:%d' % (l, n) for n, l in left[:args.max]) + (' ...' if len(left) > args.max else ''))
    print('\ntraces (check each with tools/cons_trace_check.py):')
    for t in traces:
        print('  ' + t)
    print('choices made: %s' % taken)


if __name__ == '__main__':
    main()
