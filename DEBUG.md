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

Headless, with conscripter-convert's smoke test (it passes its
environment on to the engine; the ranges are its button bar):

```sh
CONS_TRACE=/tmp/trace.jsonl CONS_TRACE_IGNORE=22-48,lsp2:12-23 \
    python3 -I research/xuse/smoke.py out/<game>-cons /tmp/work 1,0,0,0,... --goto <label>
```

## Records

Every record has `"t"`, its type. `"pg"` is the number of text pages
recorded so far, so a warning or error can be placed between two pages.

`"at"` is a place in the script, `"*label:line"`. The line is counted
from 1 through the script files in order, so for a game with only
`0.txt` it is that file's line number (`sed -n 1873p 0.txt`). It is
`null` outside the script, for example in a Lua callback.

Text and file names are converted to UTF-8 from the script's encoding.

### `meta`: the first record

```json
{"t":"meta","schema":1,"engine":"CONscripter 0.18.0","time":"2026-10-14T11:20:43","w":800,"h":600,"utf8":false,"dir":"./"}
```

`schema` changes when a record's meaning changes. `w`,`h` are the screen
size, `utf8` whether the script is UTF-8 (`--enc:utf8`), `dir` the game
folder (`--root`), and `ignore` the `--trace-ignore` list (or `null`).

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
| `via` | Inside a `gosub` (a `textgosub` routine, a helper): the line it returns to, which led there |

Which commands are written:

- **sprites and the background** (`lsp`, `lsp2` and their variants,
  `csp`, `vsp`, `msp`, `amsp`, `cell`, `spfilter`, `strsp`, `ld`, `cl`,
  `bg`, `popscreen`, ...): only when they changed something, so a `csp`
  of a sprite that isn't there doesn't appear;
- **sound, movies and screen effects** (`bgm`, `mp3`, `play`, `stop`,
  `dwave`, `wave`, `loopbgm`, their stops and fades, `mpegplay`,
  `movie`, `avi`, `screenfilter`, `screenshake`, `screenripple`,
  `screenpulse`, `pushscreen`, `monocro`, `nega`, `quake`, `allsphide`,
  ...): always;
- **`print`**: when something changed since the last `print` written,
  so it marks when the changes above appear.

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
```

Written when the engine exits: at `end`, after a script error, or when
the window is closed (`timeout`'s SIGTERM counts, as SDL turns it into a
quit). A trace that doesn't end with it stopped in a crash or was killed
with SIGKILL; the last records show where.

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
# sprites up for more than 50 pages
jq -c 'select(.t=="page") | .pg as $p | .sp[] | select($p - .since > 50) | {pg:$p, n, file}' trace.jsonl
# pictures that end just above the bottom edge
jq -c 'select(.t=="page") | .at as $a | .sp[] | select(.h > 500 and (.y+.h) < 600 and (.y+.h) > 560) | {at:$a, n, file, y, h}' trace.jsonl
```
