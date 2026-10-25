"""--diff, motions, jumps and durations, stale archives, missing pictures and sound rules: python3 -I tools/test_cons_trace_check.py"""
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cons_trace_check  # noqa: E402


def sprite(**kw):
    s = {'n': 10, 'kind': 'lsp', 'x': 50, 'y': 50, 'w': 100, 'h': 100, 'alpha': 255, 'file': 'a.png'}
    s.update(kw)
    return s


def run_diff(old, new):
    paths = []
    for recs in (old, new):
        f = tempfile.NamedTemporaryFile('w', suffix='.jsonl', delete=False)
        f.write(''.join(json.dumps(r) + '\n' for r in recs))
        f.close()
        paths.append(f.name)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        cons_trace_check.diff(paths[0], paths[1], 10)
    for p in paths:
        os.unlink(p)
    return out.getvalue()


def page(*sp):
    return {'t': 'page', 'pg': 1, 'at': '*a:1', 'sp': list(sp)}


class Diff(unittest.TestCase):
    def test_older_traces_sampled_at_another_time(self):
        # before "mnow", the box moved with a motion's zoom
        out = run_diff([page(sprite(motion='loop'))], [page(sprite(x=18, y=18, w=164, h=164, motion='loop'))])
        self.assertIn('lsp 10: the same loop motion, sampled at another time', out)

    def test_older_against_newer_trace(self):
        out = run_diff([page(sprite(motion='loop'))], [page(sprite(motion='loop', mid='1234abcd', mnow=[5, 0, 0, 100, 100, 0, 256]))])
        self.assertIn('no differences', out)

    def test_running_in_one_over_in_the_other(self):
        keep = sprite(motion='keep', mid='1234abcd', mnow=[100, 10, 0, 100, 100, 0, 256])
        ended = {'t': 'motion', 'pg': 0, 'n': 10, 'kind': 'lsp', 'id': '1234abcd', 'ended': 'cut short'}
        out = run_diff([page(keep)], [ended, page(sprite(x=90))])
        self.assertIn('the same motion 1234abcd, running in one, over in the other: cut short', out)

    def test_other_motion(self):
        out = run_diff([page(sprite(motion='once', mid='1234abcd', mnow=[0] * 7))],
                       [page(sprite(motion='once', mid='99999999', mnow=[0] * 7))])
        self.assertIn('lsp 10: a.png at 50,50 100x100 alpha 255 motion once 1234abcd -> a.png at 50,50 100x100 alpha 255 motion once 99999999\n', out)


class Build(unittest.TestCase):
    def test_stale_archive_and_missing_picture(self):
        d = tempfile.mkdtemp()
        name = b'bg\\a.png'
        with open(os.path.join(d, 'arc.nsa'), 'wb') as f:  # one entry, as conv-cons writes it
            f.write((1).to_bytes(2, 'big') + (6 + len(name) + 14).to_bytes(4, 'big') + name + bytes(14))
        with open(os.path.join(d, '0.txt'), 'w', encoding='cp932') as f:
            f.write('*start\nlsp 1,":a;bg\\a.png",0,0\nlsp 2,"bg\\b.png",0,0\nlsp 3,"bg\\$1.png",0,0\n')
        trace = os.path.join(d, 't.jsonl')
        with open(trace, 'w') as f:
            f.write(json.dumps({'t': 'warn', 'pg': 0, 'what': 'stale archive', 'file': '0.txt',
                                'script': '1', 'archive': '2'}) + '\n')
        out = io.StringIO()
        argv = sys.argv
        sys.argv = ['x', trace, '--script', os.path.join(d, '0.txt'), '--skip', 'coverage']
        try:
            with contextlib.redirect_stdout(out):
                cons_trace_check.main()
        except SystemExit:
            pass
        finally:
            sys.argv = argv
        out = out.getvalue()
        self.assertIn('packed for another 0.txt', out)
        self.assertIn('bg\\b.png is named by the script', out)
        self.assertNotIn('a.png is named', out)
        self.assertNotIn('$1', out)


def check(recs):
    """The checker's report on a trace of recs."""
    f = tempfile.NamedTemporaryFile('w', suffix='.jsonl', delete=False)
    f.write(''.join(json.dumps(r) + '\n' for r in recs + [{'t': 'end', 'pg': 1, 'reason': 'script end'}]))
    f.close()
    out = io.StringIO()
    argv = sys.argv
    sys.argv = ['x', f.name, '--skip', 'coverage']
    try:
        with contextlib.redirect_stdout(out):
            cons_trace_check.main()
    except SystemExit:
        pass
    finally:
        sys.argv = argv
        os.unlink(f.name)
    return out.getvalue()


def ev(src, **kw):
    r = {'t': 'ev', 'pg': 1, 'ms': 0, 'cmd': src.split()[0], 'at': '*a:%d' % len(src), 'src': src}
    r.update(kw)
    return r


class Sound(unittest.TestCase):
    def test_empty_channel(self):
        out = check([ev('chfreq 3,1500', ch=3, empty=True), ev('chpan 3,500,1000', ch=3, empty=True),
                     ev('chpan 3,-200', ch=3, empty=True), ev('chfreq 4,900', ch=4)])
        self.assertIn('chfreq 3,1500 on channel 3, which plays nothing', out)
        self.assertIn('chpan 3,500,1000 on channel 3', out)
        self.assertNotIn('chpan 3,-200', out)  # set before a dwave
        self.assertNotIn('chfreq 4', out)

    def test_cross_fade_from_nothing(self):
        out = check([ev('bgm "a.ogg",1000', bgm='a.ogg', cross='no music'), ev('bgm "b.ogg",1000', bgm='b.ogg')])
        self.assertIn('bgm "a.ogg",1000 cross-fades from no music', out)
        self.assertNotIn('b.ogg', out)

    def test_long_wait_at_gamespeed(self):
        out = check([{'t': 'warn', 'pg': 1, 'what': 'long wait', 'cmd': 'delay', 'speed': 10, 'ms': 85130, 'at': '*a:2'}])
        self.assertIn('delay asks for 85130 ms (1.4 minutes), at gamespeed 10 %', out)


    def test_save_data_left_out_while_reloading(self):
        out = check([{'t': 'roundtrip', 'pg': 1, 'slot': 999, 'at': '*a:2'},
                     {'t': 'warn', 'pg': 1, 'what': 'save data left out', 'version': 29, 'reads': 30, 'at': '*a:2'}])
        self.assertIn('without its CONscripter data (version 29, this build reads 30)', out)


def samples(*xs):
    return [{'t': 'sample', 'pg': 1, 'ms': i * 64, 'f': i * 4, 'n': 10, 'x': x, 'y': 0, 'w': 100, 'h': 100, 'zoom': [100, 100],
             'angle': 0, 'alpha': 256} for i, x in enumerate(xs)]


START = {'t': 'motion', 'pg': 1, 'n': 10, 'kind': 'lsp', 'id': '1', 'mode': 'once', 'at': '*s:5'}


class TimingChecks(unittest.TestCase):
    def test_smooth_motion_is_quiet(self):
        out = check([START] + samples(0, 40, 80, 120) +
                    [{'t': 'effect', 'pg': 1, 'type': 10, 'len': 500, 'due': 500, 'ran': 510, 'at': '*s:6'}])
        self.assertNotIn('== jump', out)
        self.assertNotIn('== duration', out)

    def test_off_screen_and_back(self):
        out = check([START] + samples(0, 0, -32766, 10, 20))
        self.assertIn('lsp 10 goes 32766 px away (0,0 -> -32766,0) and back within 2 samples', out)
        self.assertIn('*s:5', out)

    def test_unseen_steps(self):
        out = check([START] + samples(-2000, -2000, -9000, -2000, -2000))  # off the screen all along
        self.assertNotIn('== jump', out)

    def test_late_effect_and_motion(self):
        out = check([START, {'t': 'effect', 'pg': 1, 'type': 10, 'len': 500, 'due': 500, 'ran': 900, 'at': '*s:6'},
                     {'t': 'motion', 'pg': 1, 'n': 10, 'kind': 'lsp', 'id': '1', 'ended': 'ran out', 'ran': 1300, 'len': 1000}])
        self.assertIn('effect 10 (500 ms as written) ran 900 ms, asked for 500', out)
        self.assertIn('motion of lsp 10 ran 1300 ms, asked for 1000', out)

    def test_diff_lists_new_findings(self):
        out = run_diff([START] + samples(0, 40, 80, 120, 160), [START] + samples(0, 40, 440, 480, 520))
        self.assertIn('new jump/duration findings', out)
        self.assertIn('lsp 10 steps 400 px between samples (40,0 -> 440,0', out)


class EngineVars(unittest.TestCase):
    def test_listed_variable_set_to_a_value_that_matters(self):
        d = tempfile.mkdtemp()
        conf = os.path.join(d, 'c.json')
        with open(conf, 'w') as f:
            json.dump({'engine_vars': [
                {'var': '%400', 'name': 'F1104', 'when': '!=0', 'note': 'no window background'},
                {'var': 401, 'name': 'F1105', 'when': '3-32', 'note': 'bubble window'},
                {'name': 'F022', 'note': 'copied into each save'}]}, f)
        recs = [{'t': 'var', 'pg': 1, 'at': '*a:1', 'n': '%400', 'v': 0},
                {'t': 'var', 'pg': 1, 'at': '*a:2', 'n': '%400', 'v': 1},
                {'t': 'var', 'pg': 2, 'at': '*a:2', 'n': '%400', 'v': 1},
                {'t': 'var', 'pg': 2, 'at': '*a:3', 'n': '%401', 'v': 2},
                {'t': 'var', 'pg': 2, 'at': '*a:4', 'n': '%401', 'v': 5},
                {'t': 'var', 'pg': 2, 'at': '*a:5', 'n': '%402', 'v': 5}]
        f = os.path.join(d, 't.jsonl')
        with open(f, 'w') as t:
            t.write(''.join(json.dumps(r) + '\n' for r in recs + [{'t': 'end', 'pg': 2, 'reason': 'script end'}]))
        out = io.StringIO()
        argv, sys.argv = sys.argv, ['x', f, '--skip', 'coverage', '--config', conf]
        try:
            with contextlib.redirect_stdout(out):
                cons_trace_check.main()
        finally:
            sys.argv = argv
        out = out.getvalue()
        self.assertIn('== engine-var (2)', out)
        self.assertIn('*a:2                     F1104 (%400) = 1: no window background', out)
        self.assertIn('F1105 (%401) = 5: bubble window', out)
        self.assertIn('engine vars: F1104 2 sets at 1 places; F1105 1 sets at 1 places', out)
        self.assertIn('not in 0.txt (the converter drops their writes): F022', out)
        self.assertEqual(cons_trace_check.trace_vars_of(conf), '400,401')

    def test_numalias_name_matches_its_number(self):
        d = tempfile.mkdtemp()
        conf = os.path.join(d, 'c.json')
        with open(conf, 'w') as f:
            json.dump({'engine_vars': [{'var': '%f1104', 'when': '!=0', 'note': 'no window background'}]}, f)
        script = os.path.join(d, '0.txt')
        with open(script, 'w') as f:
            f.write('*define\nnumalias f1103,236:numalias F1104,237\ngame\n*a\nmov %f1104,1\n')
        f = os.path.join(d, 't.jsonl')
        with open(f, 'w') as t:
            t.write(''.join(json.dumps(r) + '\n' for r in [
                {'t': 'var', 'pg': 1, 'at': '*a:1', 'n': '%237', 'v': 1},
                {'t': 'var', 'pg': 1, 'at': '*a:1', 'n': '%236', 'v': 1},
                {'t': 'end', 'pg': 1, 'reason': 'script end'}]))
        out = io.StringIO()
        argv, sys.argv = sys.argv, ['x', f, '--skip', 'coverage', '--config', conf, '--script', script]
        try:
            with contextlib.redirect_stdout(out):
                cons_trace_check.main()
        finally:
            sys.argv = argv
        out = out.getvalue()
        self.assertIn('%f1104 (%237) = 1: no window background', out)
        self.assertIn('engine vars: %f1104 1 sets at 1 places', out)
        self.assertEqual(cons_trace_check.trace_vars_of(conf), '%f1104')


if __name__ == '__main__':
    unittest.main()
