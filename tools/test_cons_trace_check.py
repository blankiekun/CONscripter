"""--diff, motions, stale archives, missing pictures and sound rules: python3 -I tools/test_cons_trace_check.py"""
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


if __name__ == '__main__':
    unittest.main()
