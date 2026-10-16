"""--diff and motions: python3 -I tools/test_cons_trace_check.py"""
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


if __name__ == '__main__':
    unittest.main()
