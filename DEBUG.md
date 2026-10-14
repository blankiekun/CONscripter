# Debugging a converted game

CONscripter can write a **trace** while a game runs: one JSON object per
line, recording what is on screen at every text page, the commands that
changed it, the choices made, every file the script asks for that can't
be loaded, and any script error. A trace is
small, easy to search with `grep` or `jq`, and readable by people and
tools, so you can find bugs such as a sprite that never goes away, a
picture placed a few pixels off, or a missing voice without taking
screenshots.

Tracing is off unless you turn it on, and adds no cost when off.

## Turning it on

```sh
conscripter --root . --trace trace.jsonl
CONS_TRACE=trace.jsonl conscripter --root .
```

`--trace` can also go in `ons_args`. A relative path is relative to the
folder the engine was started from, not the game folder. The file is
overwritten on every run and flushed after each record, so if the engine
crashes, every record before the crash is kept.

To leave a game's own interface out (its button bar, menus), list those
sprites with `--trace-ignore` or `CONS_TRACE_IGNORE`: comma-separated
numbers or ranges, `lsp` sprites by default, `lsp2:` or `ld:` before the
others. Ignored sprites are left out of every record, their commands
aren't logged, and their buttons aren't listed.

```sh
conscripter --root . --trace trace.jsonl --trace-ignore 22-48,lsp2:12-23
```

## Playing through by itself

`--auto-play pages` makes the game play itself, as fast as it can, so one
command plays through a game and leaves a trace to read:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy conscripter --root . --save-dir /tmp/saves \
    --trace trace.jsonl --trace-ignore 22-48,lsp2:12-23 --auto-play 0
```

- Every wait for a click (text, `click`, `delay`, `textbtnwait`) is
  answered at once.
- A `btnwait` or `select` takes a button, chosen by `--auto-choice` (below).
- Effects take 1 ms, as do timed waits (`wait`, `waittimer`) and BGM
  fades. Vsync is turned off, since otherwise each character drawn would
  wait for the screen.
- Movies are skipped, but a missing movie is still reported.
- Sounds still play, so the pages' `se` lists are real.

It stops after `pages` text pages, or when the script ends if `pages` is
0. It also stops when it seems to be going round in circles: after 200
choices or 20000 waits without a new page (a menu), or 3000 pages without
one in a label it hadn't shown a page in before (a loop of routes, the
same choice taken at each turn). It then writes a `warn` with `what`
`auto-play stuck` and where, then an `end` with the `reason`.

The trace isn't needed: `--auto-play` alone checks that a game runs to
the end without a script error. Give it a `--save-dir` of its own, since
a game that saves as it goes would otherwise write to the player's save
files. `--trace-ignore` matters here too: buttons on ignored sprites are
never pressed, so a button bar's save, load and skip buttons stay
untouched.

`--auto-choice` decides which button to take. The options are the
buttons numbered from 1 that aren't on ignored sprites, in order. A
`btnwait` also has a final option, 0: a click elsewhere, which usually
goes on past a hyperlink. Each place in the script counts its visits:

| `--auto-choice` | Takes |
|---|---|
| `first` (default) | the option matching the visit count, staying on the last: the first button the first time, the next one when the game comes back (a hyperlink, then on; a menu of topics, each in turn) |
| `cycle` | the same, but going round again after the last |
| `random` | any option; with `--seed n` the same run every time |

`--seed n` also fixes the numbers `rnd` gives, so a run that worked out a
route can be repeated exactly.

With conscripter-convert's smoke test instead (scripted clicks; it
passes its environment on to the engine):

```sh
CONS_TRACE=/tmp/trace.jsonl CONS_TRACE_IGNORE=22-48,lsp2:12-23 \
    python3 -I research/xuse/smoke.py out/<game>-cons /tmp/work 1,0,0,0,... --goto <label>
```

## The state at any moment

With a trace on, **F8** writes a `dump` record: the state right now, in
the same form as a page, wherever the game is (a menu, a `btnwait`, the
middle of a scene). Natively it also saves a picture of the screen as
`<shot-dir>/dump<pg>-<ms>.png`. Press it when something looks wrong while
playing, then look for `"t":"dump"` in the trace.

## In the web build

The web build writes the same records, and passes each one to the page
as `self.onsyuriTrace(line)` when the page defines it. The engine's own
page (`src/onsyuri_web/conscripter.html`) does this when its URL has
`?trace`. It keeps the records, and its menu (F9) gets a **trace** button
(**F7**) that downloads them as `trace.jsonl`. The other options can go
in the URL too:

```
conscripter.html?trace&trace-ignore=22-48,lsp2:12-23&auto-play=0&auto-choice=random&seed=3
```

F8 works as above, without the picture (the web build has nowhere to
save it). A page of your own can do the same: add `--trace`, `"/trace.jsonl"`
to the arguments given to `callMain`, and define `self.onsyuriTrace`
before the game starts.

## Pictures of pages

A trace is meant to find a bug without looking. To see a page once the
trace or the checker points at it, run the game again the same way and
save pictures of just those pages:

```sh
conscripter --root . --trace trace.jsonl --auto-play 0 --shot-pages 412,980-990 --shot-dir shots
```

`--shot-pages` takes page numbers and ranges (the trace's `pg`), and
`--shot-every n` takes every n-th page. Each picture is the screen as
shown at that page's click wait, text window and all, saved as
`<shot-dir>/page000412.png` (`--shot-dir` defaults to `shots`, relative to
the folder the engine was started from). The page's record gives the
file as `shot`. Both need `--trace`, which counts the pages. An
auto-play run with the same options and `--seed` reaches the same pages.

## Records

Every record has `"t"`, its type. `"pg"` is the number of text pages
recorded so far, so a warning or error can be placed between two pages.
Pages and `ev` records also have `"ms"`, the milliseconds since the
engine started, which shows where time goes (a long effect, a slow
load).

`"at"` is a place in the script, `"*label:line"`. The line is counted
from 1 through the script files in order, so for a game with only
`0.txt` it is that file's line number (`sed -n 1873p 0.txt`). It is
`null` outside the script, for example in a Lua callback.

Text and file names are converted to UTF-8 from the script's encoding.

### `meta`: the first record

```json
{"t":"meta","schema":1,"engine":"CONscripter 0.18.0","time":"2026-10-14T11:20:43","w":800,"h":600,"utf8":false,"dir":"./",
 "ignore":"22-48,lsp2:12-23","auto":{"pages":0,"choice":"first","seed":3}}
```

`schema` changes when a record's meaning changes. `w`,`h` are the screen
size, `utf8` whether the script is UTF-8 (`--enc:utf8`), `dir` the game
folder (`--root`), `ignore` the `--trace-ignore` list (or `null`), and
`auto` the `--auto-play` settings (only with `--auto-play`; `seed` only
with `--seed`).

### `page`: each click wait and new page

Written when the text stops for a click (`@`, `\`, or a `clickstr`
character), including while skipping and when a `textgosub` routine
handles the wait.

```json
{"t":"page","pg":41,"at":"*s4_l2:5467","text":"「じゃ肉々しいところの匂いを送ってあげるよ」",
 "bg":"bg\\80all_0100.png","bgm":"bgm\\bgm16.ogg",
 "se":[{"ch":0,"file":"voice\\v300010.ogg"},{"ch":2,"file":"se\\se125.ogg","loop":true}],
 "sp":[{"n":64,"kind":"lsp","x":119,"y":-50,"w":563,"h":700,"alpha":255,
        "file":"sprite\\bu021203m.png","edges":"TB","since":1}]}
```

| Field | Meaning |
|---|---|
| `at` | The text line being shown |
| `text` | The page's text so far, as written in the script (tags and all); `\n` between lines |
| `bg` | The `bg` argument: a file, `black`, `white` or `#rrggbb` |
| `bgm` | The music last started with `bgm`/`mp3`/`ogg`/`play`/`midi` and not stopped (it may have finished if it doesn't loop); `null` for none |
| `loopbgm` | `[intro, loop]` when `loopbgm` is playing (omitted otherwise) |
| `se` | The sound channels still playing (`dwave`, `wave`, voices): channel, file, and `loop` for a looping one (omitted when none) |
| `window` | Whether the text window is shown |
| `windowtrans` | The text window picture's opacity (`windowtrans`), when not 255 |
| `monocro` | The colour of `monocro`, as `"#rrggbb"`, while it is on |
| `nega` | The `nega` mode, while it is on |
| `filters` | The `screenfilter` slots in use: `{"slot","kind","blend","strength","p":[p1,p2,p3]}` (see ADDITIONS.md) |
| `fx` | The `screenshake`/`screenripple`/`screenpulse` slots in use: `{"slot","kind":"shake"\|"ripple"\|"pulse"}`, with the ripple's `map` |
| `shot` | The picture saved of this page (`--shot-pages`, `--shot-every`) |
| `sprites_hidden` | `true` while `allsphide` hides the `lsp` and `ld` sprites (they are still listed); `sprites2_hidden` likewise for `allsp2hide` and `lsp2` |
| `sp` | Every visible sprite, in the order: `lsp` 0-999, `lsp2` 0-255, `ld` (0 left, 1 centre, 2 right). A lower `lsp` number is drawn in front |

Each sprite in `sp`:

| Field | Meaning |
|---|---|
| `n`, `kind` | Its number and how it was made: `lsp`, `lsp2` or `ld` |
| `x`,`y`,`w`,`h` | Its box on the screen in pixels (the current cell; for `lsp2`, the bounding box after scaling and rotation) |
| `alpha` | 0-255, as set by `lsp`'s opacity or `amsp` |
| `file` | The image file; `str` instead for a string sprite (its text), `tag` for a sprite without a file (a colour) |
| `missing` | `true` if the image couldn't be loaded (no size then) |
| `cell`, `cells` | The cell shown, for an animated or multi-cell sprite |
| `edges` | Which screen edges the box touches or crosses: `T`op, `B`ottom, `L`eft, `R`ight (omitted when none) |
| `off` | `true` when the box is entirely off the screen |
| `filters` | The number of `spfilter` filters on it |
| `since` | The page where it last appeared, or changed its image, position, size, opacity or cell (an animation stepping its own cells doesn't count) |

`since` makes lingering sprites easy to spot: a character still up with
a `since` far below the current `pg`, especially across a `bg` change.

### `dump`: the state when F8 was pressed

The same fields as a `page`, with `"t":"dump"`, `pg` the last page (a
dump isn't counted as one), `at` where the script is waiting, and natively
a `shot`.

### `ev`: a command that changed the screen or the sound

```json
{"t":"ev","pg":40,"cmd":"lsp","at":"*s4_l2:5464","src":"lsp 50,\":a;system\\nameplate.png\",42,412",
 "sp":[{"n":50,"kind":"lsp","x":42,"y":412,"w":284,"h":43,"alpha":255,"file":"system\\nameplate.png","since":41}]}
{"t":"ev","pg":41,"cmd":"cl","at":"*no:20","src":"cl c,1","sp":[{"n":1,"kind":"ld","gone":true}]}
{"t":"ev","pg":41,"cmd":"stop","at":"*no:22","src":"stop","bgm":null}
```

Written after the command runs, so `pg` is the page before it shows.
`src` is the command as written in the script (variables not filled
in); what it did follows, as the fields that changed since the last
record:

| Field | Meaning |
|---|---|
| `sp` | Each sprite shown or changed (as in a page's `sp`, its `since` the next page), or `{"n","kind","gone":true}` for one removed or hidden |
| `bg`, `bgm` | The new background or music (`bgm` `null` when stopped) |
| `effect` | The effect it showed with (`print`, or `bg`, `ld`, `cl` with an effect number): `{"no","type","ms"}`, with the `mask` image (types 15, 18) or `dll` (99); `no` only for an effect defined with `effect` |
| `def` | For `effect`: the effect as defined, in the same form |
| `via` | Inside a `gosub` (a `textgosub` routine, a helper): the line it returns to, which led there |

Which commands are written:

- **sprites and the background** (`lsp`, `lsp2` and their variants,
  `csp`, `vsp`, `msp`, `amsp`, `cell`, `spfilter`, `strsp`, `ld`, `cl`,
  `bg`, `popscreen`, ...): only when they changed something, so a `csp`
  of a sprite that isn't there doesn't appear;
- **sound and movies** (`bgm`, `mp3`, `play`, `stop`, `dwave`, `wave`,
  `loopbgm`, their stops and fades, `mpegplay`, `movie`, `avi`) and
  **effect definitions** (`effect`): always;
- **what is laid over the picture** (`screenfilter`, `screenshake`,
  `screenripple`, `screenpulse`, `pushscreen`, `monocro`, `nega`,
  `quake`, `windowtrans`, `allsphide`, ...): always;
- **`print`**: when something changed since the last `print` written
  (sprites, the background, or what is laid over the picture), so it
  marks when those changes appear, and with what effect.

A change no command explains (an animation, a command not in the list)
still shows in the next page's `sp`.

### `choice`: a choice the game offered

```json
{"t":"choice","pg":1,"kind":"select","at":"*start:15","options":[{"text":"はい","label":"yes"},{"text":"いいえ","label":"no"}],"picked":2}
{"t":"choice","pg":1,"kind":"btnwait","at":"*start:14","buttons":[{"no":1,"sp":10,"file":"ch.png"}],"result":1}
```

`select`, `selgosub` and `selnum` give the options (text and label) and
the one picked, counted from 1. `btnwait` (and `btnwait2`, not
`textbtnwait`, which waits on a text page) gives the buttons that were
offered (a sprite button's number, sprite and image or `str`; an area's
`x`,`y`,`w`,`h`) and the result: the button's number, 0 for a click
elsewhere, negative for right-click, keys or a timeout.

### `label`: the first time a label runs

```json
{"t":"label","pg":12,"name":"s4_l2"}
```

Written the first time the script runs a command or shows a page in a
label, so the labels a run reached can be compared with the script's.

### `warn`: a file that can't be used

```json
{"t":"warn","pg":0,"what":"missing image","file":"nothere.png","at":"*start:7"}
```

`what` is one of `missing image`, `broken image` (found but couldn't be
decoded), `image too large`, `force-loaded JPG`, `missing sound`,
`broken sound`, `broken music`, `sound too large`, or `missing movie`.
The engine's own default files (`cursor0.bmp`, `uoncur.bmp`, ...), which
it tries whether or not a game has them, aren't reported.

### `error`: a script error, just before the engine exits

```json
{"t":"error","pg":3,"msg":"Label \"nosuchlabel\" is not found. [*nosuchlabel]","at":"*sub:19","stack":["*start:12"]}
```

`stack` is the chain of `gosub` calls that led there, innermost first,
each given as the place it returns to.

### `end`: the last record

```json
{"t":"end","pg":119}
{"t":"end","pg":300,"reason":"auto-play page limit"}
```

Written when the engine exits: at `end`, after a script error, or when
the window is closed (`timeout`'s SIGTERM counts, as SDL turns it into a
quit). `reason` says why `--auto-play` stopped it: `auto-play page
limit`, or `stuck: ...`. A trace that doesn't end with it stopped in a crash or was killed
with SIGKILL; the last records show where.

## Checking a trace

`tools/cons_trace_check.py` (Python 3, nothing to install) reads a trace
and prints what looks wrong, ranked by kind, each at the script line to
look at:

```sh
python3 tools/cons_trace_check.py trace.jsonl --script 0.txt
```

```
trace.jsonl: 49302 pages, 97649 commands, 230 choices; ended

== linger (66)
  *conv_link37_1:54515     lsp 61 bg\frame01.png stayed up when the background changed bg\img_0064.png -> bg\bg01d.png (shown at *s62_l1:219303: lsp %180,$180,0,0)
  ...
```

| Kind | What it finds |
|---|---|
| `error` | Script errors (with the gosub chain), a trace without `end` (a crash), an auto-play run that got stuck |
| `missing` | Files that couldn't be loaded, grouped by file, and sprites shown without their image |
| `linger` | A tall picture (a character or CG) still up after the background changed, or up for more than `--linger` pages |
| `float` | A picture at least `--float-h` of the screen tall whose bottom edge stops up to `--float-gap` pixels above the screen's |
| `offscreen` | A sprite entirely off the screen |
| `invisible` | A sprite shown at alpha 0 |
| `crowd` | More than `--crowd` tall pictures at once, or two at the same place |
| `overlay` | A screen filter or effect still on after the background changed, and with `--cg REGEX` any filter or effect over a background whose name matches (an eye-close left over a CG) |
| `loop` | A looping sound playing for more than `--loop` pages |
| `coverage` | With `--script`, the labels the run never reached (`--coverage-skip REGEX` leaves some out) |

Each sprite finding ends with the command that showed it, so the bug can
be traced from the page back to the line that put the sprite up. A
finding is reported once (the first page it is seen), not on every page.

"Tall" means at least `--tall` (0.4) of the screen height, which leaves
out name plates and buttons. The other limits, with their defaults, are
in `--help`. `--max N` sets how many findings of each kind are shown, and
`--json` prints them all as JSON.

To check a fix, compare a trace from before it with one from after, made
the same way (the same `--auto-play`, `--auto-choice` and `--seed`):

```sh
python3 tools/cons_trace_check.py --diff before.jsonl after.jsonl
```

This prints each page whose background, music, screen state or sprites
differ (once for a run of pages with the same difference), and where the
two runs part if they took different routes.

The report ends with the pages its findings are on, as a `--shot-pages`
option for taking their pictures.

## Reading a trace

```sh
# every problem the engine noticed
grep -E '"t":"(warn|error)"' trace.jsonl
# a page's full state
grep '"t":"page","pg":41,' trace.jsonl | jq .
# everything that happened to sprite 64
jq -c 'select(.t=="ev") | select(any(.sp[]?; .n==64 and .kind=="lsp")) | {pg, at, src}' trace.jsonl
# the route taken
grep '"t":"choice"' trace.jsonl
# an effect or filter over a page (for example an eye-close left over a CG)
jq -c 'select(.t=="page" and ((.filters // []) + (.fx // []) | length) > 0) | {pg, at, bg, filters, fx}' trace.jsonl
# the slowest stretches
jq -r 'select(.ms) | "\(.ms) \(.at)"' trace.jsonl | awk 'p {print $1-p, $2} {p=$1}' | sort -rn | head
# sprites up for more than 50 pages
jq -c 'select(.t=="page") | .pg as $p | .sp[] | select($p - .since > 50) | {pg:$p, n, file}' trace.jsonl
# pictures that end just above the bottom edge
jq -c 'select(.t=="page") | .at as $a | .sp[] | select(.h > 500 and (.y+.h) < 600 and (.y+.h) > 560) | {at:$a, n, file, y, h}' trace.jsonl
```
