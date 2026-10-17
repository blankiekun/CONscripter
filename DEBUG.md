# Debugging a converted game

CONscripter can write a **trace** while a game runs: one JSON object per
line, recording what is on screen at every text page, the commands that
changed it, the choices made, every file the script asks for that can't
be loaded, and any script error or crash. A trace is
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
Lists add up: the command line's (the option may be given more than
once), one in the game's `ons_args` (a converted game lists its button
bar there, so `--auto-play` never opens its menus from the bar) and
`CONS_TRACE_IGNORE`.

```sh
conscripter --root . --trace trace.jsonl --trace-ignore 22-48,lsp2:12-23
```

Other options add to what the trace holds:

| Option | Adds |
|---|---|
| `--trace-vars 300-899,$10-20` | a `var` record for each change of these variables: numbers alone are `%` variables, `$` before the strings; numbers and ranges, comma-separated (0 to the script's `numalias` range, 4096 by default); a `defvar` or `defstr` name (`%score`, `$who`) watches that one, recorded under its name |
| `--audio-stats` | an `audio` record for every 100 ms of sound mixed, for each dwave channel playing and for the whole mix (off by default: nothing is counted) |
| `--trace-long-wait ms` | a `long wait` warning for each wait, effect or fade the script asks to last longer than this (default 10000) |
| `--trace-max-image mb` | a `large image` warning for each picture that alone takes more memory than this, in MB (default 64, a 4096x4096 picture) |

## Playing through by itself

`--auto-play pages` makes the game play itself, as fast as it can, so one
command plays through a game and leaves a trace to read:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy conscripter --root . --save-dir /tmp/saves \
    --trace trace.jsonl --trace-ignore 22-48,lsp2:12-23 --auto-play 0
```

- Every wait for a click (text, `click`, `delay`, `textbtnwait`) is
  answered at once.
- A `btnwait` or `select` takes a button, chosen by `--auto-choice` (below),
  or by `--choose` and `--choices` ("Following a route").
- Effects take 1 ms, as do timed waits (`wait`, `waittimer`) and BGM
  fades. Vsync is turned off, since otherwise each character drawn would
  wait for the screen.
- Movies are skipped, but a missing movie is still reported.
- With `--real-time`, effects, timed waits, `spmotion` and BGM fades take
  their real time instead, so sprite movement and fades play out as a
  player sees them (for `--shot-after` mid-effect, or watching; `--shot-effects`
  films effects without it
  in a window); only clicks and choices are still made at once.
- Under `SDL_VIDEODRIVER=dummy` the run keeps a virtual clock: game time
  (`SDL_GetTicks`, the trace's `ms`, `gettimer`) moves only when the
  engine waits, straight to the next timer due, and not while it draws
  or loads. Two runs with the same options give the same times and so
  the same frames, mid-motion too (`--shot-after`, `--shot-at` with
  `+ms`); a `--real-time` run takes no wall-clock time for its waits.
  With a window the clock is the real one, to watch.
- Sounds still play, so the pages' `se` lists are real.

It stops after `pages` text pages, or when the script ends if `pages` is
0. It also stops when it seems to be going round in circles: after 200
choices or 20000 waits without a new page (a menu), or 3000 pages without
one in a label it hadn't shown a page in before (a loop of routes, the
same choice taken at each turn). It then writes a `warn` with `what`
`auto-play stuck` and where, then an `end` with the `reason`.

`--until label` ends a run when the script first reaches that label
(written with or without `*`), and `--max-pages n` after n text pages,
counted as the trace counts them. Both work without `--auto-play` too,
while playing by hand. The end `reason` is `until label` or `page
limit`, and the exit code 0. A label the script doesn't have is a
startup failure.

```sh
conscripter --root . --trace trace.jsonl --auto-play 0 --until s9_l36
```

The trace isn't needed: `--auto-play` alone checks that a game runs to
the end without a script error. Give it a `--save-dir` of its own, since
a game that saves as it goes would otherwise write to the player's save
files. The engine makes the folder if it doesn't exist (not in the web
build). `--trace-ignore` matters here too: buttons on ignored sprites are
never pressed, so a button bar's save, load and skip buttons stay
untouched.

Screens the engine draws (`screen`, COMMANDS.md "Screens") of the kinds
`save`, `load`, `settings`, `backlog` and `extras` are closed at once, with
a `screen` record whose action is `skipped`, so a run needs no
`--trace-ignore` for them. `--skip-screens kinds` (comma-separated) sets
the kinds closed so, `--skip-screens none` plays them all: their buttons
are then choices as a `btnwait`'s, at the `screen` command's place (see
the `screen` record for the numbers).

`--auto-choice` decides which button to take. The options are the
buttons numbered from 1 that aren't on ignored sprites, in order. A
`btnwait` also has a final option, 0: a click elsewhere, which usually
goes on past a hyperlink. Each place in the script counts its visits:

| `--auto-choice` | Takes |
|---|---|
| `first` (default) | the option matching the visit count, staying on the last: the first button the first time, the next one when the game comes back (a hyperlink, then on; a menu of topics, each in turn) |
| `cycle` | the same, but going round again after the last |
| `random` | any option; with `--seed n` the same run every time |
| `untaken` | of the options not taken there earlier in this run, the one `--taken FILE` counts the fewest times (one never taken while there is one), the first in order of those; `first` when this run has taken them all |

`--seed n` also fixes the numbers `rnd` gives, so a run that worked out a
route can be repeated exactly.

The `--taken` file has a line for each choice made before: the choice
record's `at`, a tab, the option (a `select`'s `picked`, a `btnwait`'s
`result`), a tab, and how many times it was made. A line without the
count (as older files have) counts once, and lines for the same choice
add up. Lines starting with `#` are left out. `tools/cons_explore.py`
(below) writes it from earlier runs.

```
*s4_l2:5467	1	3
*conv_link1_wait:5994	0	1
```

Ties go to the first option in order, so runs with the same `--taken`
file and `--seed` make the same choices. `avoid` in place of the count
marks a choice that led to a script error: `untaken` doesn't make it
while the place has another option that isn't marked.

```
*conv_link118_wait:232891	2	avoid
```

`--stop-known n` with `--known-labels FILE` (the labels earlier runs
reached, one name a line) ends a run that has nothing new left to show.
Once the run has gone its own way (a choice the `--taken` file doesn't
list, or a page in a label the file doesn't), it stops after `n` pages
in a row in labels the file lists without such a choice, with the end
`reason` `known ground` and exit code 0. Before that it may play `10n`
pages on known ground: every run shares the opening, but one that never
leaves it repeats an earlier run.

### Without a screen

`SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy` run the engine with no
window and no sound device, so no X server or Xvfb is needed. Several
runs can go at once (one per core), each with its own `--save-dir` and
trace, since a game's global variables and saves live in the save folder:

```sh
for s in 1 2 3 4; do
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy conscripter --root . --save-dir /tmp/saves$s \
      --trace /tmp/trace$s.jsonl --auto-play 0 --auto-choice random --seed $s &
done; wait
```

The exit code says how a run ended, so a script can check it without
reading the trace:

| Code | Meaning |
|---|---|
| 0 | the script ended, a page limit, `--until` or `--stop-known` stopped it, or the window was closed |
| 1 | `--auto-play` got stuck |
| 2 | a script error |
| 3 | it couldn't start: an unknown option on the command line (one in `ons_args` is only reported), no script or font, SDL failed, a `--goto` or `--until` label or `--vars` file it can't use, a save `--load` can't load |
| 128+n | ended by signal n: 143 `SIGTERM` (`timeout`, `kill`), 130 `SIGINT` (Ctrl+C) or 129 `SIGHUP`, with an `end` record; or a crash, 139 a segmentation fault, 134 an abort, with a `crash` record |

### Replaying a run

`--replay trace.jsonl` (with `--auto-play`) makes the choices a trace
recorded, in order, then goes on with `--auto-choice`. It takes the
trace's `--seed` and `--auto-choice` too, unless given. This repeats a
run on a newer build or a fixed script, even when it was played by hand.
If a choice comes up somewhere other than where the recorded one did (the
script changed), it writes a `replay diverged` warning and uses the
recorded answer if it is still one of the options; when the recorded
choices run out, a `replay ended` warning.

### Following a route

A heroine's route often needs its choices made in a set order, which
`--auto-choice` can't do. `--choose` and `--choices` (with `--auto-play`)
give the answers. An answer is the option as `--auto-choice` numbers it
and the choice record gives it: a `select`'s `picked`, counted from 1, or
a `btnwait`'s `result`, 0 for a click elsewhere.

`--choose '*label:line=N'` takes option N at every choice at that place
(the choice record's `at`; the `*` can be left out). `=N,M,K` gives the
answers for the first, second and third visits there, then stays on the
last. It can be given more than once, for more places.

```sh
conscripter --root . --trace t.jsonl --auto-play 0 --choose '*s4_l2:5467=2' --choose '*conv_link1_wait:5994=1,1,0'
```

`--choices FILE` gives a list of answers, made in order. Each line is a
place, a tab (or `=`) and the answer; `#` lines and blank lines are left
out, as is anything after a second tab, so a `--taken` file's lines can
be copied in. Each line is used up when a choice comes up at its place.
A choice somewhere else doesn't use one up: it is made as if there were
no list (a hyperlink or menu the list doesn't mention is left to
`--auto-choice`). Once every line is used up, the next choice writes a
`choices ended` warning, and `--replay` or `--auto-choice` goes on from there.

```
# Azusa's route
*s4_l2:5467	2
*s6_l1:9120	1
*s9_l36:62510	3
```

Each choice takes the first of these that has an answer for it:

1. `--choose` for that place;
2. the next `--choices` line, if it is for that place (used up even when
   `--choose` answered);
3. `--replay`'s next recorded choice;
4. `--auto-choice` (`untaken` with `--taken` as usual).

An answer that isn't one of the options offered (a button that isn't
there, a select with fewer options) writes a `choice not offered` warning
and goes on down the list. A choice made by `--choose` or `--choices`
still counts for `untaken` as made in this run.

### Exploring routes

`tools/cons_explore.py` plays a game again and again, each run taking the
choices the earlier ones took least (`--auto-choice untaken`, with a
`--taken` file counting the choices in all the earlier traces), until two runs in a row
reach no new label (`--patience`) or after `--runs`. It then lists the
script's labels no run reached, and the traces it wrote, to check each
with the checker:

```sh
python3 tools/cons_explore.py out/saiima-cons --out /tmp/explore --runs 6 --seed 1 \
    --engine-args "--trace-ignore 22-48,lsp2:12-23" --skip-choices '^\*conv_(title|sl_|settings|confirm|extras)'
```

```
run 1: 68739 pages, 360 choices (232 new), 1410 new labels, 1410 in all; stuck: 200 choices without a new page, exit 1, 834 s
run 2: 10000 pages, 45 choices (0 new), 0 new labels, 1410 in all; known ground, exit 0, 125 s
run 3: 10000 pages, 45 choices (0 new), 0 new labels, 1410 in all; known ground, exit 0, 125 s
2 runs in a row reached no new label: done

1410 of 3178 labels reached; 1768 never:
  *conv_title_exit:195, *s0_l0:203, *s0_l3:207, *s1_l0:278, *s1_l1:282 ...
```

Run 1 takes every hyperlink in turn and plays the whole route. Runs 2
and 3 find no choice it didn't make, so each stops after 10,000 pages on
known ground (`--stop-known`); without it each would play the whole
route again.

Once every option at a place has been taken, `untaken` takes the one
taken least, so later runs still go down the routes the earlier ones took
only once.

A run that ends in a script error never reaches the choices after it, so
the explorer marks the last choice the run made before the error `avoid`
in the `--taken` file, and later runs go another way there. If every
option at that place leads to the error, each one gets marked in turn,
and then the choice before it. When there is no choice left to mark (the
error comes before the first one, or all of them are marked), every run
would end there, so it stops. A run that marks a choice doesn't count
toward `--patience`, even when it reached no new label. The errors are listed at the end, each once,
with how many runs ended there and the choices marked for it:

```
script errors:
  *conv_link118_1:232962: pushscreen: too many saved screens (popscreen missing?): 1 run; avoided *conv_link118_wait:232891 2
```

Fix the error (in the converter or the game) and explore again to reach
what lies past it.

Each run has its own trace (`runNN.jsonl`) and save folder in `--out`.
A run ended by a signal (`timeout`, `kill`) didn't finish, so the
explorer stops there.
Each run after the first stops once it has played `--stop-known` pages
(1000) back on ground earlier runs reached, after going its own way, so
it doesn't play the rest of a route again; `--stop-known 0` plays every
run to its end. On saiima a run that found nothing new stopped after
10,000 pages (130 s) instead of playing its whole 68,700 (900 s).
`--skip-choices` names the places, as a regex on `*label:line`, whose
choices every run should make the same way: a title menu or a settings
screen, which would otherwise take a run off to load or quit. `--route FILE`
gives every run the engine's `--choices FILE` (see "Following a route"),
so the exploration starts down a route and explores what lies past it;
the choices the route makes are never marked `avoid`. `--pages`
limits each run, `--seed` makes the exploration repeatable, and
`--engine` and `--engine-args` choose the engine and add options.

## Starting anywhere

A bug far into a game can be reached directly instead of playing up to
it. `--goto label` starts at that label once the `*define` section and
`*start`'s own lines have run (the game's setup there: a converted
game's settings and their defaults, the text window), as soon as the
script reaches a line outside the two at no `gosub`'s depth (a title
screen's first label, or a `goto` out of `*start`). `--vars file` sets
variables first, before `*start` runs, so the scene runs as it would on
its route:

```sh
python3 tools/cons_trace_vars.py trace.jsonl --page 4120 -o vars.txt
conscripter --root . --goto s9_l36 --vars vars.txt --trace scene.jsonl
```

The file is UTF-8 (the engine converts it to the script's encoding), one
variable a line; `#` lines and blank lines are left out. A string runs to
the end of the line, so it can't hold a line break:

```
# CONscripter variables, page 4120
%312=876
$5=text to the end of the line
%score=3
?cg_seen[12]=1
```

`defvar`, `defstr` and `defarr` names are written and read by name
(`%score`, `$who`, `?cg_seen[12]` for an array's element), not by slot,
in these files and in a dump's `vars`.

A `--vars` run is a test run: it doesn't write `gloval.sav`, so global
variables in the file (an old dump's unlocks and settings) don't replace
the player's. Saves to slots are still written; add `--save-dir` to keep
them apart from the real ones.

`--dump-vars file` writes every variable that isn't 0 or empty in this
form when F8 is pressed and when the engine exits. `tools/cons_trace_vars.py`
writes one from a trace: the variables at the start of a page (`--page`),
when the run first reached a place (`--at "*label:line"` or `--at "*label"`),
or at the end. It takes them from the last F8 `dump` before then, brought
up to date with the `var` records after it, or, without one, plays back the
`var` records, so the trace needs `--trace-vars` for every variable the
game uses (`--trace-vars '0-4095,$0-4095'`). Global variables loaded from
the save folder at start have no `var` record; for a game that uses them,
press F8 early in the run, or take the file `--dump-vars` writes.

`--diff` compares the `var` records of two traces and prints the first
change that differs, for example the engine's against a converter's own
tracer, or two runs of a route that should have set the same flags.
`--only 300-899,$10-20` looks at those variables only:

```
$ python3 tools/cons_trace_vars.py --diff before.jsonl after.jsonl
change 3 differs:
  before.jsonl: %904=10 at *start:172 (page 0)
  after.jsonl: %905=7 at *start:172 (page 0)
  the change before, the same in both: %903=8 at *start:172
```

### Checkpoints and saves

`--checkpoint-every n` (with `--auto-play`) saves the game every n pages,
in save slots 900, 901, 902, ... (`--checkpoint-slot s` starts at
another), and writes a `checkpoint` record each time. `--load n` starts a
run by loading save slot n, so a long run can be picked up near a bug:

```sh
conscripter --root . --save-dir /tmp/saves --trace t.jsonl --auto-play 0 --checkpoint-every 1000
grep '"t":"checkpoint"' t.jsonl        # {"t":"checkpoint","pg":4000,"slot":903,"at":"*s9_l36:62510"}
conscripter --root . --save-dir /tmp/saves --trace from4000.jsonl --load 903
```

`--checkpoint-start` (with `--goto`) makes one checkpoint at the label
itself, after `*start`'s setup and before any of the label runs: loading it plays the scene from its first
line, effects and all, where a checkpoint at the first click wait would
start after them.

A checkpoint is the state at that page's click wait, as a player's save
there would be, so the loaded run shows that page's text again as its
page 1. Some games keep their own data in high save slots (saiima uses 900
and up): give such a game another `--checkpoint-slot`. Give the game's
button bar to `--trace-ignore` too, or once saves exist an auto-play run
may press its load button and go round in circles.

`--roundtrip-every n` (with `--trace` and `--auto-play`) checks saving
itself: every n pages it saves to a spare slot (`--roundtrip-slot`, 999),
clears what it compares (as a load from the title screen would find it;
the settings made once in `*define` stay), loads the save, and when the
same page shows again compares the state
with the first time: the text, the background, the sprites, the music and
looping sounds, the filters, the variables, ... Any difference is written
as a `save/load mismatch` warning, which points at something the save file
leaves out or the game's `loadgosub` gets wrong:

```json
{"t":"warn","pg":51,"what":"save/load mismatch","at":"*s4_l2:5387","diff":["sp lsp 49: \"忍\" 58,424 316x20 alpha 255 cell 0 filters 0 -> none"]}
```

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
playing, then look for `"t":"dump"` in the trace. The dump also holds the
variables, so the moment can be started again with `--goto` and `--vars`
(see "Starting anywhere"); with `--dump-vars file`, F8 writes them to that
file too.

## In the web build

The web build writes the same records, and passes each one to the page
as `self.consTrace(line)` when the page defines it. The engine's own
page (`src/cons_web/conscripter.html`) does this when its URL has
`?trace`. It keeps the records, and its menu (F9) gets a **trace** button
(**F7**) that downloads them as `trace.jsonl`. The other options can go
in the URL too:

```
conscripter.html?trace&trace-ignore=22-48,lsp2:12-23&auto-play=0&auto-choice=random&seed=3
```

It takes `trace-ignore`, `auto-play`, `auto-choice`, `seed`, `until` and
`max-pages`.

F8 works as above, without the picture (the web build has nowhere to
save it). A page of your own can do the same: add `--trace`, `"/trace.jsonl"`
to the arguments given to `callMain`, and define `self.consTrace`
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

A menu, a dialog or an effect between two lines has no click wait of
its own. Two more options take a picture at other moments, each with a
`dump` record of the state then (as F8 writes, with `why` saying which
option took it):

```sh
conscripter --root . --trace trace.jsonl --auto-play 0 --shot-at '*conv_menu:812,*s4_l2:5470+500' --shot-after 12000
```

- `--shot-at '*label:line'` takes one once the script has run that line
  and goes on to another: after every command on it, before the next
  line runs (or wherever a `goto` or `gosub` on it led). Each time the
  line runs, so a menu shown three times gives three pictures, saved as
  `<shot-dir>/at000412-conv_menu-812-1.png`: the page (`pg`), the label,
  the line and the visit, counted from 1. The `dump`'s `at` is where the
  script goes on, the line after it.
- `--shot-at '*label:line+ms'` takes it that many milliseconds of game
  time after the line has run, as `--shot-after` would (and wherever the
  script is then), saved as `at000412-s4_l2-5470-1+500.png`: a frame
  mid-motion or mid-fade (with `--real-time`), the same in every run
  under the dummy driver (the virtual clock, above).
- `--shot-effects 25,50,75` films every effect as it starts: its frame
  at each percent of its own time (an `--auto-play` run, which cuts
  effects to 1 ms, too), saved as `<shot-dir>/fx<pg>-<n>-<pct>.png` (`n`
  counts the effects filmed) with a dump record each; then the effect runs
  as it would. With `--shot-pages` or `--shot-every`, only effects on those
  pages.
- `--shot-after ms` takes one that many milliseconds after the engine
  started (the trace's `ms`), at the next point the engine gets to: the
  next line of the script, or, while it waits for a click or a button,
  the moment itself. It is saved as `<shot-dir>/ms0012000.png`.

Both take a comma-separated list and can be given more than once, and
both work in an `--auto-play` run and while playing by hand. Under
`--auto-play` time runs much faster than in play, so a time there
matches a different place than it would by hand; under the dummy video
driver it is game time, which repeats run to run. A picture is the screen
as shown at that moment. A command that changes a sprite but doesn't
show it (`lsp` without `print`) leaves the screen as it was, so a
picture after its line doesn't have the new sprite yet; the `dump`'s
`sp` already lists it. After a text line that ends with `\`, the page is
already cleared; take the page itself with `--shot-pages`. The web build
writes the `dump` records without the pictures.

## Records

Every record has `"t"`, its type. `"pg"` is the number of text pages
recorded so far, so a warning or error can be placed between two pages.
Pages, `ev` and `motion` records also have `"ms"`, the milliseconds since the
engine started, which shows where time goes (a long effect, a slow
load).

`"at"` is a place in the script, `"*label:line"`. The line is counted
from 1 through the script files in order, so for a game with only
`0.txt` it is that file's line number (`sed -n 1873p 0.txt`). It is
`null` outside the script, for example in a Lua callback.

Text and file names are converted to UTF-8 from the script's encoding.

### `meta`: the first record

```json
{"t":"meta","schema":2,"engine":"CONscripter 0.18.0","time":"2026-10-14T11:20:43","w":800,"h":600,"utf8":false,"dir":"./",
 "ignore":"22-48,lsp2:12-23","start":{"goto":"s9_l36","vars":"vars.txt"},"auto":{"pages":0,"choice":"first","seed":3},
 "stop":{"until":"s9_l40","max_pages":500}}
```

`schema` changes when a record's meaning changes (2 added the records and
fields marked "schema 2" below). `w`,`h` are the screen
size, `utf8` whether the script is UTF-8 (`--enc:utf8`), `dir` the game
folder (`--root`), `ignore` the `--trace-ignore` list (or `null`), `start`
how the run started when not from the beginning (`goto` and `vars`, or
`load` with the slot), and
`auto` the `--auto-play` settings (only with `--auto-play`; `seed` only
with `--seed`, `replay` and `taken` with those files, `choose` the
`--choose` options as given, in a list, `choices` the `--choices` file,
`stop_known` and `known` with `--stop-known`), and `stop` what ends the
run early (`until` the `--until` label, `max_pages` the `--max-pages`;
only with them). A run that can't
start still writes a `meta` record (`w`,`h` may be 0) before its `error`.

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
| `text` | The page's text so far, as shown; `\n` between lines. Ruby is written `(body/ruby)` and a text button `<n...>`; the tags the engine ran (`!w`, `#rrggbb`, `@`, ...) aren't there, so one that is was shown as text |
| `tag` | The `[...]` tag before the text, as written (`"あずさ\|voice/v300002.ogg"`), when it has one (schema 2) |
| `overflow` | `true` when the text ran past the text window's last line (its last column in vertical text), so some of it wasn't seen; it stays on for the page's later click waits (schema 2) |
| `mem` | The most image memory in use since the last page: `{"surf","bytes"}`, the number of pictures the engine held (sprites, backgrounds, the text window, buttons, cursors) and their bytes, without the screen's own buffers or sound (schema 2) |
| `bg` | The `bg` argument: a file, `black`, `white` or `#rrggbb` |
| `bgm` | The music last started with `bgm`/`mp3`/`ogg`/`play`/`midi` and not stopped (it may have finished if it doesn't loop); `null` for none |
| `loopbgm` | `[intro, loop]` when `loopbgm` is playing (omitted otherwise) |
| `bgmvol` | The music's volume as heard, in percent, when below 100: the player's setting, `consset "mutebgm"`, a fade, `bgmvol`, `chvolfade -1` |
| `se` | The sound channels still playing (`dwave`, `wave`, voices): channel, file, `loop` for a looping one, `vol`, its volume as heard in percent (the setting, `chvol`, `soundmute`, a channel fade), when below 100, and `freq`, its `chfreq` rate per mille, when not 1000 (omitted when none) |
| `wait` | The milliseconds the script asked to wait since the last page: text drawn a character at a time, `wait`, `delay`, effects. Auto-play skips them, so this is what a player would wait |
| `px` | A checksum of the screen as shown (FNV-1a of its pixels), for `--diff` |
| `window` | Whether the text window is shown |
| `windowtrans` | The text window picture's opacity (`windowtrans`), when not 255 |
| `monocro` | The colour of `monocro`, as `"#rrggbb"`, while it is on |
| `nega` | The `nega` mode, while it is on |
| `flash` | `[opacity, "#rrggbb"]` while a `screenflash` is on |
| `filters` | The `screenfilter` slots in use: `{"slot","kind","blend","strength","p":[p1,p2,p3]}`, with `"z"` when it has one (see COMMANDS.md) |
| `fx` | The `screenshake`/`screenripple`/`screenpulse` slots in use: `{"slot","kind":"shake"\|"ripple"\|"pulse"}`, with the ripple's `map` and a shake's `end` (ms) when it has one; a timed wait skipped by auto-play still counts toward that end |
| `shot` | The picture saved of this page (`--shot-pages`, `--shot-every`) |
| `sprites_hidden` | `true` while `allsphide` hides the `lsp` and `ld` sprites (they are still listed); `sprites2_hidden` likewise for `allsp2hide` and `lsp2` |
| `sp` | Every visible sprite, in the order: `lsp` 0-999, `lsp2` 0-255, `ld` (0 left, 1 centre, 2 right). A lower `lsp` number is drawn in front |

Each sprite in `sp`:

| Field | Meaning |
|---|---|
| `n`, `kind` | Its number and how it was made: `lsp`, `lsp2` or `ld` |
| `x`,`y`,`w`,`h` | Its box on the screen in pixels (the current cell; for `lsp2`, the bounding box after scaling and rotation; zoomed by `spzoom`, not by an `spmotion`: that is in `mnow`) |
| `alpha` | 0-255, as set by `lsp`'s opacity or `amsp` |
| `file` | The image file; `str` instead for a string sprite (its text), `tag` for a sprite without a file (a colour); `"(spgrab)"` for a copy of the screen made by `spgrab`, `"(spgrab n)"` for a copy of sprite n |
| `missing` | `true` if the image couldn't be loaded (no size then) |
| `cell`, `cells` | The cell shown, for an animated or multi-cell sprite |
| `edges` | Which screen edges the box touches or crosses: `T`op, `B`ottom, `L`eft, `R`ight (omitted when none) |
| `off` | `true` when the box is entirely off the screen |
| `filters` | The number of `spfilter` filters on it |
| `zoom` | `[x,y]`, its `spzoom` in percent (omitted at 100) |
| `rot` | its `spzoom` angle in degrees, counterclockwise (omitted at 0) |
| `anim` | `true` for a sprite that steps its own cells (its picture changes with time) |
| `motion` | `"loop"`, `"once"`, `"keep"` (loop mode 2) or `"leave"` (3: removed at its end) while an `spmotion` moves it (the box is its place, without the motion) |
| `mid` | With `motion`: the motion's id, as in its `motion` record (the same keys and mode give the same id, in any run) |
| `mnow` | With `motion`: `[t,x,y,zoom_x,zoom_y,angle,alpha]`, where the motion had it at its last step, `t` ms after it started: the offset in script pixels, zoom in percent, angle in degrees, alpha 0-256 (as its keys) |
| `clip` | `[x,y,w,h]`, its `spclip` rectangle |
| `blend` | `"add"`, `"sub"`, `"mul"` or `"screen"`: how `spblend` (or `lsp2add`, `lsp2sub`) draws it |
| `over` | The sprite `spz` draws it right over |
| `trail` | `[passes,alpha,lag]`, its `sptrail` (lag in ms at game speed); `[passes,alpha,lag,1]` when the copies are drawn over it |
| `shown` | The alpha it is drawn at, when `spmotion`'s keys make it differ from `alpha` |
| `since` | The page where it last appeared, or changed its image, position, size, opacity, cell, `spz`, `sptrail`, `spclip`, `spblend`, or an `spmotion` starting, ending or replaced (an animation stepping its own cells, or a fade or motion moving, doesn't count). A change to any of these alone writes the sprite in the command's `ev` record |

`since` makes lingering sprites easy to spot: a character still up with
a `since` far below the current `pg`, especially across a `bg` change.

### `dump`: the state when F8 was pressed, or at a picture

The same fields as a `page`, with `"t":"dump"`, `pg` the last page (a
dump isn't counted as one), `at` where the script is waiting, natively
a `shot`, and `vars`: every numbered variable that isn't 0 or empty
(schema 2), `{"%312":876,"$5":"text"}`. `why` says what wrote it: `F8`,
or `shot-at`, `shot-after` and `shot-effect` for a picture those options took (see
"Pictures of pages").

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
| `ch`, `file` | For `dwave`, `wave` and their variants: the channel and the file played, variables filled in (schema 2) |
| `cut` | The sound that channel was still playing, which this one stopped (schema 2). Under `--auto-play` every voice is cut by the next, since clicks come at once |
| `ch`, `empty` | For `chvolfade`, `chpan`, `chfreq`: the channel, and `empty` `true` when it plays nothing |
| `cross` | For `bgm "file",ms`: `"no music"` when there was none to cross-fade from |
| `skip` | A movie (`mpegplay`, `movie`): whether a click stops it; `moviestop` beside it when that setting overrides the script's flag; `window`: `true` when the text window is shown as it starts (drawn over the movie) |
| `ms_cpu` | On any command (listed below or not) that kept the main thread working for more than a frame (16 ms) without waiting: the longest such stretch, in wall-clock ms (a picture or sound decoded at once) |
| `via` | Inside a `gosub` (a `textgosub` routine, a helper): the line it returns to, which led there |

Which commands are written:

- **sprites and the background** (`lsp`, `lsp2` and their variants,
  `csp`, `vsp`, `msp`, `amsp`, `cell`, `spfilter`, `strsp`, `vstrsp`, `spz`, `ld`, `cl`,
  `bg`, `popscreen`, ...): only when they changed something, so a `csp`
  of a sprite that isn't there doesn't appear;
- **sound and movies** (`bgm`, `mp3`, `play`, `stop`, `dwave`, `wave`,
  `loopbgm`, their stops and fades, `bgmfadein`, `mp3fadein`, `chvolfade`,
  `dwavefadein`, `chvol`, `mpegplay`, `movie`, `avi`), the
  player settings a game's settings screen sets (`consset`, `btnse`) and
  **effect definitions** (`effect`): always;
- **what is laid over the picture** (`screenfilter`, `screenshake`,
  `screenripple`, `screenpulse`, `pushscreen`, `monocro`, `nega`,
  `quake`, `windowtrans`, `screenflash`, `allsphide`, ...): always;
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
elsewhere, negative for right-click, keys or a timeout. A button that
can't be seen has `blocked`: `off` (its area is off the screen), `hidden`
(its sprite isn't shown), `alpha 0`, or `under lsp n` (an opaque `lsp`
sprite in front covers all of it; a click still reaches it).

### `motion`: an `spmotion`, `screenshake` or `quake` starting or ending

```json
{"t":"motion","pg":3,"ms":604,"n":11,"kind":"lsp","id":"81731c79","mode":"once","keys":[[0,0,0,100,100,0,256,0],[500,50,50,100,100,0,256,0]],"end":1104,"at":"*start:11"}
{"t":"motion","pg":3,"ms":922,"n":11,"kind":"lsp","id":"81731c79","ended":"replaced","ran":318}
{"t":"motion","pg":3,"ms":604,"shake":1,"len":300}
{"t":"motion","pg":4,"ms":905,"shake":1,"ended":"ran out","ran":300}
{"t":"motion","pg":4,"ms":923,"quake":"quake","amp":4,"len":200}
```

A sprite's motion (`n`, `kind`, its `id` as the sprite records' `mid`)
starts with its `mode` (as the sprite records' `motion`), `passes` for
loop -n, its `keys` as `spmotion` wrote them (`[ms,x,y,zoom_x,zoom_y,angle,alpha,curve]`,
ms at game speed), the `end` it is due at (`ms`; none for a loop) and
`at`, the `spmotion`. It ends with `ended` and `ran`, the ms since it
started:

| `ended` | Why |
|---|---|
| `ran out` | its time is over (loop 3: the sprite is gone too) |
| `cut short` | it jumped to its end before its time: `spmotionwait` after a click or with skipping, `--auto-play` without `--real-time` (which ends a motion at once, `ran` 0), or a loop 2 motion when `spmotion`, `lsp`, `msp` or `amsp` sets the sprite anew |
| `stopped` | `spmotion n` |
| `replaced` | another `spmotion` on the sprite |
| `lsp` | `lsp` on a loop 3 motion's sprite keeps the sprite and ends the motion |
| `removed` | anything else, seen at the next record: `csp`, a load, a reset (no `ran`) |

A shake records its `screenshake` slot as `shake` and `len`, its end in
ms (none: it shakes until stopped); a `quake`, `quakex` or `quakey`
its command as `quake`, `amp` its amplitude and `len` its time. `ended`
is `ran out`, `cut short` (a quake shortened by `--auto-play` or a
click), `stopped` (`screenshake n`, `screenshake 0`) or `replaced` (a
new `screenshake` in the slot), `ran` how long it shook (for a shake,
with the time `--auto-play` skipped). A load or reset ending a shake
records nothing. Sprites left out with `--trace-ignore` have no motion
records.

### `sound`: a fade, pan or rate that got there, a sound that ended

```json
{"t":"sound","pg":3,"ms":971,"ch":3,"cmd":"chvolfade","to":30}
{"t":"sound","pg":3,"ms":1467,"ch":5,"ended":true,"file":"se\\shot.ogg"}
```

`cmd`, `to`: a `chvolfade`, `chpan` or `chfreq` ramp (`ms` over 0) that
reached `to` at `ms`; `ch` -1 is the music. Written when the engine next
looks (between events), so it can come after records with a later `ms`.
`ended`: the sound on `ch` (-1: the music) played to its end, not stopped.
Ramps run on the trace's clock, so under the virtual clock (`--auto-play`
with the dummy driver) a ramp longer than the waits after it never gets
there; sounds play in real time, so they end by the wall clock.

### `audio`: what was heard, every 100 ms (`--audio-stats`)

```json
{"t":"audio","pg":0,"ms":1270,"audio_ms":600,"mix":1,"rms":[2908,2298],"bal":-117,"hz":880,"step":553,"pos":0.600,"file":"bgm\\a.ogg"}
{"t":"audio","pg":0,"ms":1505,"audio_ms":723,"ch":3,"rms":[1743,871],"bal":-333,"hz":880,"step":366,"pos":0.700,"file":"se\\rain.ogg"}
```

One per 100 ms window of a dwave channel's sound (`ch`, after `chvolfade`,
`chpan`, `chfreq` and times its channel volume: as heard) or of the whole
mix (`mix`: the music with every channel, at the output). `rms`: left and
right RMS (0-32767); `bal`: (right - left) / (right + left), per mille;
`hz`: the main frequency, from the zero crossings (a mix of tones gives
the loudest high one); `step`: the largest jump between two samples (a
click, a skip in the sound: a 440 Hz tone at RMS 2000 steps about 180);
`pos`: where in its file the window ends, s (for `mix`, the music's; a
loop shows as `pos` going back). `audio_ms` is the mixer's own clock, the
window's end in sound mixed since the audio opened: it runs in real time,
also under the dummy audio driver. `ms` is the trace's clock when the
record is written (the engine writes them between events, so after a long
wait several come at once); under the virtual clock (`--auto-play` with
the dummy video driver) game time jumps through waits while sound plays
in real time, so a page hears only the few ms its run took: use
`--real-time`, or no `--auto-play`, to hear a scene as a player does.
`"lost":n`: windows dropped (100000 waiting to be written).

### `label`: the first time a label runs

```json
{"t":"label","pg":12,"name":"s4_l2"}
```

Written the first time the script runs a command or shows a page in a
label, so the labels a run reached can be compared with the script's.

### `var`: a variable changed (`--trace-vars`, schema 2)

```json
{"t":"var","pg":7,"at":"*start:172","n":"%902","v":8}
{"t":"var","pg":7,"at":"*s4_l2:5150","n":"$5","v":"text"}
```

Written each time the script sets one of the variables `--trace-vars`
lists, to a new value or the same one.

### `checkpoint`: a save made by `--checkpoint-every` (schema 2)

```json
{"t":"checkpoint","pg":200,"slot":901,"at":"*conv_link1_1:6147"}
```

Written after that page's record; `--load 901` starts there.

### `roundtrip`: a save and load made by `--roundtrip-every` (schema 2)

```json
{"t":"roundtrip","pg":50,"slot":999,"at":"*conv_wait:243429"}
```

Written when it saves and loads, at the next command after page `pg`.
The records that follow up to the next page come from the load and the
game's `loadgosub` (a scene set up again, a `missing image` warning
repeated); the checker leaves those warnings out. The page shown again is
not written, so `pg` goes on as before. A looping sound saved during its
`dwavefadein` restarts at full volume, so its volume isn't compared.

### `warn`: a file that can't be used, and other things to look at

```json
{"t":"warn","pg":0,"what":"missing image","file":"nothere.png","at":"*start:7"}
```

`what` is one of `missing image`, `broken image` (found but couldn't be
decoded), `image too large`, `force-loaded JPG`, `missing sound`,
`broken sound`, `broken music`, `sound too large`, `missing movie`, or
`screenshot not written` (`savescreenshot` couldn't write `file`, as
the path it tried: a folder that doesn't exist; with `--save-dir` the
picture goes into the save folder, without the name's folders).
The engine's own default files (`cursor0.bmp`, `uoncur.bmp`, ...), which
it tries whether or not a game has them, aren't reported.

Schema 2 adds these, with their own fields:

| `what` | Fields | Written when |
|---|---|---|
| `long wait` | `cmd`, `ms` | a command asks to wait longer than `--trace-long-wait` (10000 ms): an effect (`print 10,270000`), `wait`, `delay`, `waittimer` (the time left), a fade, a `quake`, or a `btnwait` with a long `btntime`. `ms` is what the script asked for, even when auto-play shortened it, at `speed` (the `gamespeed` percent, when not 100: `delay 8513` at 10 is 85130) |
| `large image` | `file`, `w`, `h`, `mb` | a picture, as kept after scaling to the screen, takes more memory than `--trace-max-image` (64 MB); once per file. `file` is the tag for a picture without a file (a colour), `null` if none. Every build, the web one too, decodes and draws pictures in software, so a picture's width and height aren't limited (a 24000x600 animation strip works in a browser), only the memory it takes |
| `save/load mismatch` | `diff` | `--roundtrip-every` found the state changed by saving and loading; `at` is the page compared, `pg` the page after the round trip. `diff` has up to 40 strings `"field: before -> after"` (`none` when absent; a long value is cut to 100 bytes around where the two differ, with `...`): `at`, `text`, `tag`, `bg`, `bgm`, `loopbgm` (` (stopped)` after the names when it no longer plays, or its intro plays with the loop gone), `se <ch>` (looping sounds, with ` vol n` when quieter than 100), `window`, `windowtrans`, `monocro`, `nega`, `flash`, `filter <slot>`, `fx <slot>`, `sprites_hidden`, `sprites2_hidden`, `pushscreen`, `sp <kind> <n>` (the image, then `x,y wxh alpha a cell c filters f`, and `over z` for `spz`, `motion loop` or `motion once` while `spmotion` runs), `%n`, `$n`; `["can't save or load"]` if that failed |
| `save place lost` | `place`, `label`, `line` | a save loaded from another build of `0.txt` whose line (by its hash) isn't in its label any more (COMMANDS.md, `savepoint`): `place` is `the save's place` or `a gosub's return place`, `label` and `line` (0 is the `*label` line) where it was, `at` where it resumes |
| `replay diverged` | `expected` | `--replay` reached a choice at `at`, where the recorded run's was at `expected` |
| `replay ended` | | `--replay` ran out of recorded choices |
| `choice not offered` | `option`, `from`, `options` | the answer `--choose` or `--choices` (`from`) gave isn't one of the `options` offered; the choice goes on to the next way of choosing ("Following a route") |
| `choices ended` | | every `--choices` line was used up; written at the next choice |
| `auto-play stuck` | | `--auto-play` stopped going round in circles (above) |
| `retired transition` | `file` (the plugin's name, `cons_slide.dll`) | an effect names a retired transition plugin (COMMANDS.md, "Transitions"), drawn as a cross-fade instead; once per name. The build predates its `cons_warp.dll` maps: rebuild it |
| `stale archive` | `file` (`0.txt`), `script`, `archive` | the archives were packed for another `0.txt`: the converter keeps the hash of the `0.txt` it built (FNV-1a 64, 16 hex digits) as `cons\script.hash` in `arc.nsa`, and `script`, the running `0.txt`'s, differs from `archive`, the one kept. Written once the define section has run (`game`). A build without the file (loose files, a dry run, a game not from the converter) or a script not in `0.txt` gets no warning |

### `error`: a script error, just before the engine exits

```json
{"t":"error","pg":3,"kind":"script","msg":"Label \"nosuchlabel\" is not found. [*nosuchlabel]","at":"*sub:19","stack":["*start:12"]}
```

`stack` is the chain of `gosub` calls that led there, innermost first,
each given as the place it returns to. `kind` (schema 2) is `script`, or
`startup` when the engine couldn't start (an unknown option, no script
or font, a bad `--goto` or `--until` label, `--vars` file or `--load` slot); `at` is then `null` and
`stack` empty if the script wasn't running yet. `lua` is a `cons_process`
that raised an error: the engine stops that process and goes on.

### `crash`: the engine crashed (schema 2)

```json
{"t":"crash","pg":5,"signal":"SIGSEGV","at":"*x:12","stack":["*a:3"],
 "recent":[{"at":"*x:10","cmd":"lsp"},{"at":"*x:12","cmd":"print"}]}
```

Written from the signal handler when the engine crashes (`SIGSEGV`,
`SIGABRT`, `SIGFPE`, ...), as the last record: `at` the command being
run, `stack` as for `error`, and `recent` the last 50 commands run
(any command, oldest first), which usually show what led to it. No `end`
follows, and the exit code is 128 plus the signal's number.

### `screen`: an action on a screen the engine draws

```json
{"t":"screen","pg":1,"at":"*conv_save:279362","screen":"save","kind":"save","action":"select","page":1,"slot":1}
{"t":"screen","pg":1,"at":"*conv_save:279362","screen":"confirm","kind":"confirm","action":"close","page":1,"value":1}
```

`action` is `open`, `select`, `page` (`value` the page), `save`, `load`,
`delete`, `no save` / `no load` / `no delete` / `no close` ... (the
question answered no), `set` (a slider, toggle or radio: `value` the new
value), `scroll` (`value` the list's first page back), `voice` (a list's
voice played: `value` its page back), `gosub`, `open` (another screen), `close` (`value` what it gave) or
`skipped` (`--auto-play` closed it); `slot` is the save slot acted on.
`at` is the `screen` command's place (for a screen opened over another,
the first one's). Each wait for the player also writes a `choice` record
of kind `btnwait`: a button's number is its widget's place in `widgets`
(from 1) times 100, plus a save slot's place on the page (from 1) for a
`saveslots`; `-1` is right-click. `--choose '*conv_save:279362=1401,2600'`
selects the first slot, then takes widget 26.

### `end`: the last record

```json
{"t":"end","pg":119,"reason":"script end"}
{"t":"end","pg":300,"reason":"auto-play page limit"}
```

Written when the engine exits, with `reason` saying why:

| `reason` | Why |
|---|---|
| `script end` | the script ran `end`, or ran out |
| `window closed` | the window was closed |
| `player quit` | the player quit from the game's menu, or with Shift+Q |
| `signal SIGTERM` | a signal: `SIGTERM` (`timeout`, `kill`), `SIGINT` (Ctrl+C) or `SIGHUP`. The engine ends as when the window is closed, and the exit code is 128 plus the signal's number |
| `script error`, `startup error` | after the `error` record |
| `until label`, `page limit` | `--until`, `--max-pages` |
| `auto-play page limit`, `known ground`, `stuck: ...` | `--auto-play` stopped it (above), `known ground` for `--stop-known` |
| `exit` | none of these (`--help` after `--trace`) |

If the run hangs, a second signal two seconds or more after the first
ends it at once, without an `end`. A trace that ends with neither `end`
nor `crash` was killed so, or with SIGKILL, or crashed too hard to
write; the last records show where.

## Checking a trace

`tools/cons_trace_check.py` (Python 3, nothing to install) reads a trace
and prints what looks wrong, ranked by kind, each at the script line to
look at:

```sh
python3 tools/cons_trace_check.py trace.jsonl --script 0.txt
```

```
trace.jsonl: 32268 pages, 59104 commands, 115 choices; stuck: 3000 pages without a new label
config cons_trace_check.json: 21 known findings left out (crowd 4, linger 16, sound 1)
image memory: 12 to 340 MB (the most at page 4120, *s9_l36:62672), 54 MB at the end

== sound (2)
  *s13_l147:194699         se\se088.ogg plays on two channels at once (2, 4), on 1 page
  ...
== linger (4)
  *s4_l2:5970              lsp 60 system\noise.png has been up since page 29311 (150 pages) (shown at *s4_l2:5239: lsp 60,$116,0,0,%116)
  ...
```

| Kind | What it finds |
|---|---|
| `error` | Script and startup errors (with the gosub chain), a crash (its signal and the last commands run), a trace without `end` or ended by a signal (killed), an auto-play run that got stuck, a `stale archive` (a build whose `0.txt` was rewritten after its archives were packed: convert the game again) |
| `missing` | Files that couldn't be loaded, grouped by file, and sprites shown without their image; with `--script`, every picture the script names (a string ending in `.png`, `.jpg`, `.bmp`, ...; names built from variables left out) that is neither in the `arc*.nsa` archives nor a loose file in the script's folder, reached or not |
| `text` | A page's text, or a `select`'s options, with garbled characters (U+FFFD, control or private-use characters, mojibake such as UTF-8 read as Shift-JIS, which mixes kanji and half-width katakana); text past the window (`overflow`, or a line longer than `--line-chars`); an empty page; a text tag shown as text (`!w100`, `!s`, `!d`, `#rrggbb`, `~...~`, a `\` or `@`); with `--script`, a `%`, `$` or `?` in a text line, which prints a variable or reads an array (a converter that left them in) |
| `sound` | The same sound started on two channels before one click wait, or a looping one doubled (in a trace without `--auto-play`, any overlap); a voice cut off by the next sound before a click wait, so it was never heard; a voice or the music playing at volume 0; `chfreq` (no effect) or a `chpan` ramp on a channel playing nothing; a `bgm "file",ms` cross-fade with no music playing |
| `movie` | Screen changes made after a movie and before the next `print`: the engine redraws the screen from before the movie as it ends, so they show a moment late (found under `--auto-play` too, which skips the movie); a movie the script lets a click stop that `moviestop 0` makes unskippable |
| `voice` | A spoken line with no voice: the speaker's (the `tag`'s name) other lines have one on at least `--voice-share` (0.8) of them and `--voice-min` (5); for lines without a tag, the `--voice-window` (3) spoken lines before and after it in the same label all have one. A line of only "……" or "！？" needs none. Also a voice file that belongs to another speaker: each speaker with `--voice-min` voiced lines gets the shortest file-name start that `--voice-share` of their voices have and no other speaker's do (`v3` for あずさ's `v300010.ogg`), and a line whose voice has another speaker's start is reported. Also one voice file played for lines that say different things (punctuation left out), a voice given to the wrong line |
| `linger` | A tall picture (a character or CG) still up after the background changed, or up for more than `--linger` pages |
| `float` | A picture at least `--float-h` of the screen tall whose bottom edge stops up to `--float-gap` pixels above the screen's |
| `offscreen` | A sprite entirely off the screen |
| `invisible` | A sprite drawn at alpha 0 (its `shown` alpha when it has one; not while it fades in or its keys loop) |
| `crowd` | More than `--crowd` tall pictures at once, or two at the same place; a picture covering the whole screen (a background or frame drawn as a sprite) isn't counted |
| `overlay` | A screen filter, effect, `screenflash`, `monocro` or `nega` still on after the background changed, and with `--cg REGEX` any filter or effect over a background whose name matches (an eye-close left over a CG), and a `screenshake` with no end shaking for more than `--shake` pages (default 30; a game drawing everything as sprites keeps one background, so only this catches a shake never cleared) |
| `loop` | A looping sound playing for more than `--loop` pages |
| `duration` | A wait or effect the script asks to last very long (the `long wait` warnings: `print 10,270000` is 4.5 minutes) |
| `slow` | Where the time went: the `--slow` (10) places with the most time, `--slow-ms` (1000) or more, between one record and the next, added up over every visit (a slow load, a long effect, a wait auto-play didn't shorten) |
| `save` | A `save/load mismatch` from `--roundtrip-every`, once for each set of fields that differed, with how many round trips gave the same. `--save-ignore REGEX` leaves fields out, such as a variable the game's `loadgosub` sets on every load (`'^%190$'`) |
| `replay` | Where a `--replay` run went another way, or ran out of choices; a `--choose` or `--choices` answer that wasn't offered; where the `--choices` list ran out |
| `memory` | Image memory (the pages' `mem`) that jumps by `--mem-jump` MB (64) from one page to the next, or grows steadily by `--mem-leak` MB (128) or more over the run (a leak); a single picture taking more than `--trace-max-image` (the `large image` warnings). The report's head gives the least and most, and where it grew |
| `button` | A `btnwait` button that can't be seen (its `blocked`) |
| `coverage` | With `--script`, the labels the run never reached (`--coverage-skip REGEX` leaves some out) |
| `unused` | With `--root`, the image and sound files in the game folder no trace loaded. Loose files only: files in `.nsa` or `.sar` archives aren't listed |

Voices are the sounds on the `--voice-ch` channels (0) or whose file
matches `--voice` (a `voice` folder); spoken lines are those with a tag
naming a speaker, or whose text matches `--dialogue` (starting with
「, 『 or “). `--script` takes the game's script file, or a folder whose
numbered `.txt` files are read in order. `--skip slow,coverage` leaves
rules out.

Each sprite finding ends with the command that showed it, so the bug can
be traced from the page back to the line that put the sprite up. A
finding is reported once (the first page it is seen), not on every page.

"Tall" means at least `--tall` (0.4) of the screen height, which leaves
out name plates and buttons. The other limits, with their defaults, are
in `--help`. `--max N` sets how many findings of each kind are shown, and
`--json` prints them all as JSON (with the counts, the `end` or `crash`
record, the memory summary and how many findings the config left out).

Several traces can be checked at once, such as the runs `cons_explore.py`
wrote. Each finding is listed once, and `coverage` and `unused` count what
any of the runs reached:

```sh
python3 tools/cons_trace_check.py /tmp/explore/run*.jsonl --script 0.txt --root .
```

The report's head gives `script waits`, the time the script asked to wait
in all (the pages' `wait`), which is about how long the run takes to play
by hand, and the page that asks for the most.

`--baseline old.json`, an earlier report made with `--json`, leaves out
the findings it already had (matched by rule, place and message, with
the numbers in it left out), so only new ones show after a change.

To check a fix, compare a trace from before it with one from after, made
the same way (the same `--auto-play`, `--auto-choice` and `--seed`):

```sh
python3 tools/cons_trace_check.py --diff before.jsonl after.jsonl
```

This prints each page whose background, music, screen state or sprites
differ (once for a run of pages with the same difference), and where the
two runs part if they took different routes. A page whose state is the same
but whose picture (`px`) differs is listed too, when nothing on it moves by
itself (an animated, fading or moving sprite, a shake): a drawing change
between two builds, such as a zoom or an effect drawn differently.

A sprite's `spmotion` is compared by its id (`mid`): the same motion
sampled at another time is no difference (the box leaves the motion out),
a different one or one only in one run is, and a sprite whose motion still
runs in one run and is over in the other (its `motion` record's `ended`)
says `(the same motion <id>, running in one, over in the other: <why>)`.
A trace from before motion ids, whose box moved with a motion's zoom and
angle, gets `the same <mode> motion, sampled at another time` instead.
`tools/test_cons_trace_check.py` tests this.

The report ends with the pages its findings are on, as a `--shot-pages`
option for taking their pictures.

### Settings for a game

A game's settings can go in `cons_trace_check.json`, beside the trace or
the script (or given with `--config FILE`), so each check needs no long
command line. Any option goes in by its name (`coverage_skip` or
`coverage-skip`), with these too:

| Key | Meaning |
|---|---|
| `ignore` | Sprites to leave out of every check, written as for `--trace-ignore`: the game's own interface, when the trace was made without it |
| `known` | Findings to leave out: each `{"rule": ..., "match": REGEX}` (a regex on the message), `{"rule", "file"}` (a file named in it) or `{"rule", "sprite": "lsp:61"}`, with an optional `"at"` regex on the place. `"rule": "*"` matches every rule |
| `skip` | Rules to leave out, a list |
| `script` | The script, relative to the config file |
| `_comment` | Anything; keys starting with `_` are left out |

```json
{
 "_comment": "saiima: lsp 61 is the game's frame; se068 is played twice on purpose",
 "script": "0.txt",
 "coverage_skip": "^conv_",
 "linger": 300,
 "known": [
  {"rule": "linger", "sprite": "lsp:61"},
  {"rule": "crowd", "file": "bg\\frame01.png"},
  {"rule": "sound", "match": "se068", "at": "^\\*s30_l1:"},
  {"rule": "duration", "match": "^btnwait"}
 ]
}
```

Options given on the command line win over the file. The report says
which config it used and how many findings it left out, by rule, so a
pattern that hides too much shows.

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
# the slowest stretches (the checker's "slow" adds them up by place)
jq -r 'select(.t=="page" or .t=="ev") | "\(.ms) \(.at)"' trace.jsonl | awk 'p {print $1-p, $2} {p=$1}' | sort -rn | head
# every change of variable %312 (--trace-vars)
jq -c 'select(.t=="var" and .n=="%312") | {pg, at, v}' trace.jsonl
# sprites up for more than 50 pages
jq -c 'select(.t=="page") | .pg as $p | .sp[] | select($p - .since > 50) | {pg:$p, n, file}' trace.jsonl
# pictures that end just above the bottom edge
jq -c 'select(.t=="page") | .at as $a | .sp[] | select(.h > 500 and (.y+.h) < 600 and (.y+.h) > 560) | {at:$a, n, file, y, h}' trace.jsonl
```
