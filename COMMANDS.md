# CONscripter script reference

Every script command the engine accepts, plus text syntax, web hooks and the
engine behaviour CONscripter changes. Written for lookup: one `###` heading per
command (aliases on the same heading), so `grep -n "^### .*\blsp\b" COMMANDS.md`
finds an entry.

Conventions:

- **[cons]** marks what CONscripter adds or changes over ONScripter-Yuri. Every
  addition applies to every script; nothing is opt-in. Commands and tags take
  effect where a script uses them.
- **(define)** marks a command allowed only in the define section (before
  `game`); elsewhere it is a script error.
- `%var` an integer variable, `$var` a string variable, `*label` a label,
  `num` a sprite number (`lsp`), `ch` a `dwave` channel. `[...]` is optional.
- **Impl** names the C++ handler. Commands are looked up by name in
  `func_lut` in `src/cons/ONScripter_lut.cpp`; handlers live in
  `src/cons/ScriptParser_command.cpp` (flow, variables, define section),
  `src/cons/ONScripter_command.cpp` (everything else) and
  `src/cons/ONScripter_cons.cpp` ([cons] commands). Grep the handler name.
- With `--trace`, every command goes through `ONScripter::traceCommand`
  (`ONScripter_trace.cpp`); see [DEBUG.md](DEBUG.md).

Adding a command: write `int ONScripter::fooCommand()` (declare it in
`ONScripter.h`), add `{"foo", &ONScripter::fooCommand}` to `func_lut`, keep
any new state in the `CONS` save block (see [Save files](#save-files)), and
add its entry here.

---

## Program flow

### goto
`goto *label` — jump. Impl `ScriptParser::gotoCommand`.

### gosub
`gosub *label[,args...]` — call; arguments are read by `getparam`.
Impl `ScriptParser::gosubCommand`.

### return
`return [*label]` — return from `gosub` (to `*label` if given).
Impl `ScriptParser::returnCommand`.

### if / notif
`if cond command[:command...]`, `notif cond ...` — conditions: `%a == 1`,
`$s == "x"`, `fchk n` (read mark), `lchk *label`, joined by `&&` or `||`
(not both). Impl `ScriptParser::ifCommand`.

### for / next / break
`for %i = from to to [step n]` … `next`; `break` leaves the loop,
`break *label` leaves and jumps. Impl `ScriptParser::forCommand`,
`nextCommand`, `breakCommand`.

### tablegoto
`tablegoto %n,*l0,*l1,...` — jump to the `%n`th label.
Impl `ONScripter::tablegotoCommand`.

### jumpf / jumpb
`jumpf` skips forward to the next `~`; `jumpb` back to the previous `~`.
Impl `ONScripter::jumpfCommand`, `jumpbCommand`.

### skip
`skip n` — continue `n` lines below (0 = 1). Impl `ScriptParser::skipCommand`.

### getparam / getparam2
`getparam %a,$b,i%c,s%d,...` — read the `gosub`/`defsub` arguments: a
variable takes the value, `i%`/`s%` take the variable number passed.
`getparam2` keeps unread arguments. Impl `ScriptParser::getparamCommand`.

### defsub
`defsub name` — a user command `name` that `gosub`s `*name`.
Impl `ScriptParser::defsubCommand`.

### textgosub (define)
`textgosub *label` — call `*label` at every `@` and `\` instead of waiting.
Impl `ScriptParser::textgosubCommand`.

### rgosub [cons]
`rgosub *label` — a right click (or Esc, where `rmode` allows the menu) at a
text click wait (`@`, `\`) gosubs to `*label` instead of the right-click menu;
its `return` comes back to the same wait, which waits again, the text as it
was. `rgosub ""` turns it off. A `clickstr` wait and button waits keep the
menu; with `textgosub` its routine does the waiting. Not saved: set it at
startup. `rgosub *menu` … `*menu:gosub *game_menu:return`.
Impl `ONScripter::rgosubCommand`, `clickEndRgosub`.

### pretextgosub (define)
`pretextgosub *label` — call `*label` before each text line (read the
`[tag]` with `gettag`). Impl `ScriptParser::pretextgosubCommand`.

### gettag
`gettag %a,$b,...` — in a `pretextgosub` routine, the `[a/b]` tag of the
line. Impl `ONScripter::gettagCommand`.

### pagetag (define)
`pagetag` — tags apply per page (for `gettaglog`). Impl `ScriptParser::pagetagCommand`.

### loadgosub (define)
`loadgosub *label` — called after a save is loaded. Impl `ScriptParser::loadgosubCommand`.

### texec
`texec` — in a `textgosub` routine, finish the click (new page or new line).
Impl `ONScripter::texecCommand`.

### trap / lr_trap
`trap *label` / `lr_trap *label` — a left (or left/right) click jumps to
`*label`; `trap off` cancels. Impl `ONScripter::trapCommand`.

### end
`end` — quit. Impl `ONScripter::endCommand`.

### reset
`reset` — back to the start of the script, variables cleared. [cons]: also
clears `screenfilter`, `screenflash`, screen effects, `chvolfade`
volumes, `chpan` pans, `windowtrans`, `!z`, play time and the `pushscreen` stack.
Impl `ONScripter::resetCommand`.

### definereset
`definereset` — rerun the define section. Impl `ONScripter::defineresetCommand`.

### game (define)
`game` — end the define section and start the game at `*start`.
Impl `ONScripter::gameCommand`.

### systemcall
`systemcall skip|reset|save|load|lookback|windowerase|rmenu|automode|end` —
run a system menu function. Impl `ONScripter::systemcallCommand`.

### luacall / luasub
`luacall *label` — call Lua function `NSCALL_label` from script events;
`luasub name` — a command implemented by Lua `NSCOM_name`.
Impl `ScriptParser::luacallCommand`, `luasubCommand`.
[cons] Lua's state is saved: the script's global data (numbers, strings,
booleans, tables of them; not functions, coroutines or a table inside
itself) goes into save files and a load puts it back, clearing data
globals the save didn't have. Globals made before `system.lua` (the
libraries, the NS functions) aren't saved.
[cons] Processes, for an engine's parallel processes:
`cons_process(name, "func", args...)` runs global function `func` as a
coroutine resumed once a frame (~16 ms) while the script goes on;
`cons_frame()` (= `coroutine.yield`) ends its frame, returning ends it;
the same name again restarts it. `cons_process_stop(name)`,
`cons_process_running(name)`. Saved by name, function and arguments; a load
(or `reset`, which stops them) can't keep a coroutine's place, so a load
starts each again from the top: keep progress in globals. A process waits
only with `cons_frame()`: a waiting command run through `NSExec` (`wait`,
`delay`, an effect) holds the whole game, script and processes, for its time.
A save's Lua state is read as data (text, no globals reachable); a save
holding anything else is refused whole, its globals left as they were.
`function blink(n) while true do t = (t or 0) + 1 ... cons_frame() end end`
then `cons_process("blink", "blink", 5)`. Impl `LUAHandler::saveState`,
`stepProcesses`.
[cons] `NSVarNo("name")`: a `numalias`'s number (a `defvar`, `defstr` or
`defarr` name's slot), nil if none: `NSSetIntValue(NSVarNo("score"), 5)`.

## Variables, strings, maths

### bitand / bitor / bitxor [cons]
`bitand %var,a,b` — `%var = a & b` (`bitor` |, `bitxor` ^), 32-bit: flags
packed in one variable. `bitand %0,%5,4:if %0 != 0 ...` tests bit 2.
Impl `ONScripter::bitandCommand`.

### mov / mov3 / mov4 / mov5 / mov6 / mov7 / mov8 / mov9 / mov10 / movl
`mov %a,1` / `mov $s,"x"`; `movN %a,v1,...,vN` sets N consecutive
variables; `movl ?arr,v1,...` fills an array. Impl `ScriptParser::movCommand`.

### add / sub / mul / div / mod
`add %a,n` (also `add $s,"x"` appends); `sub`, `mul`, `div`, `mod` likewise.
Impl `ScriptParser::addCommand` etc.

### inc / dec
`inc %a`, `dec %a`. Impl `ScriptParser::incCommand`, `decCommand`.

### rnd / rnd2
`rnd %a,n` → 0..n-1; `rnd2 %a,lo,hi` → lo..hi. Impl `ONScripter::rndCommand`.

### sin / cos / tan
`sin %a,deg` — ×1000. Impl `ScriptParser::sinCommand` etc.

### dim (define)
`dim ?arr[n][m]` — declare an array. Impl `ScriptParser::dimCommand`.

### intlimit (define)
`intlimit n,lo,hi` — clamp `%n`. Impl `ScriptParser::intlimitCommand`.

### numalias / stralias (define)
`numalias name,n`, `stralias name,"s"` — named constants.
Impl `ScriptParser::numaliasCommand`, `straliasCommand`.

### defvar / defstr / defarr [cons]
`defvar name[,default][,global]`, `defstr name[,"default"][,global]`,
`defarr name,size[,global]` — a variable with a name and its own slot (from
10000 up, above the numbered ones and a `;$V`/`;value` range): `%name`,
`$name`, `?name[i]` (0 to size-1) work wherever `%N`, `$N`, `?N` do, as a
`numalias` to the slot. Saves and `gloval.sav` keep them by name, so a new
build may add, drop or reorder them: a load sets the names it knows and
leaves the others at their defaults. `global` ones are in `gloval.sav` (after
the numbered globals, needs `globalon`) and survive `reset`/`definereset`;
the others go to their default at `reset`, a load and `definereset`. The same
declaration again (the define section run again) does nothing; a name
declared before as another kind, or a `numalias` already, is an error. Any
section; usually `*define`. `defvar seen_ending,global`, `defarr cg,300,global`,
`mov ?cg[12],1`. Impl `ScriptParser::defineNamed` (`ONScripter_names.cpp`).

### globalon (define)
`globalon` — `%200` and above are global, saved in `gloval.sav`.
Impl `ScriptParser::globalonCommand`.

### len / mid / cmp / atoi / itoa / itoa2
`len %a,$s`; `mid $d,$s,start,len` (bytes); `cmp %r,$a,$b` (-1/0/1);
`atoi %a,$s`; `itoa $s,%a` (`itoa2` full-width digits).
Impl `ScriptParser::lenCommand`, `midCommand`, `cmpCommand`, `atoiCommand`,
`itoaCommand`.

### split / splitstring
`split $s,",",%a,$b,...` — split by a one-character delimiter.
Impl `ONScripter::splitCommand`.

### date / time
`date %y,%m,%d` (year % 100); `time %h,%m,%s`.
Impl `ScriptParser::dateCommand`, `timeCommand`.

### getret
`getret %a` / `getret $s` — the result of `exec_dll`/`layermessage`.
Impl `ONScripter::getretCommand`.

### getreg
`getreg $s,"file.ini","section","key"` — read an ini value (`registry`).
Impl `ONScripter::getregCommand`.

### fileexist
`fileexist %a,"file"` — 1 if the file is in the archives or loose.
Impl `ONScripter::fileexistCommand`.

### input
`input $s,"title","default",maxlen,widechar[,w,h,boxw,boxh]` — sets the
default only (no input box). Impl `ONScripter::inputCommand`.

### getversion / versionstr
`getversion %a` — NScripter version number; `versionstr "s1","s2"` — the
version text. Impl `ONScripter::getversionCommand`, `ScriptParser::versionstrCommand`.

### getplaytime / setplaytime [cons]
`getplaytime %var` — seconds played since the game started; `setplaytime s`
sets it. Kept in each save (as at `savegame`), restored by `loadgame`;
`reset` and old saves set 0. Impl `ONScripter::getplaytimeCommand`,
`setplaytimeCommand`.

## Define section and settings

### caption
`caption "title"` — window title. Impl `ONScripter::captionCommand`.

### nsa / ns2 / ns3 / sar / arc / nsadir
`nsa`, `ns2`, `ns3` — use `.nsa`/`.ns2`/`.ns3` archives; `sar`/`arc
"file"` — a `.sar`; `nsadir "dir"` (define) — where the archives are.
Impl `ScriptParser::nsaCommand`, `arcCommand`, `nsadirCommand`.

### savedir (define)
`savedir "dir"` — where saves go. Impl `ScriptParser::savedirCommand`.

### savenumber
`savenumber n` — save slots in the menu. Impl `ScriptParser::savenumberCommand`.

### savename
`savename "save","load","slot"` — menu names. Impl `ScriptParser::savenameCommand`.

### filelog / labellog (define)
`filelog`, `labellog` — record files seen / labels read (`fchk`, `lchk`).
Impl `ScriptParser::filelogCommand`, `labellogCommand`.

### kidokuskip / kidokumode
`kidokuskip` — skip mode stops at unread text; `kidokumode 1` — skip only
read text. Impl `ScriptParser::kidokuskipCommand`, `kidokumodeCommand`.
CONscripter: `kidoku.dat` keeps the read marks by label, line in the
label and the line's hash (`ScriptHandler::writeKidokuCons`,
ONScripter_savepoint.cpp), so after a rebuild of `0.txt` a line read
before stays read wherever it moved inside its label; marks of a changed
or deleted line, or of a label gone, are dropped. A stock `kidoku.dat`
(a bitmap of byte offsets) is still read when its size fits the script.

### english (define)
`english` — English (half-width) mode. Impl `ScriptParser::englishCommand`.

### zenkakko (define)
`zenkakko` — full-width brackets. Impl `ScriptParser::zenkakkoCommand`.

### mode_ext / automode (define), mode_saya, mode_wave_demo
`mode_ext`/`automode` — enable auto mode; `mode_saya`, `mode_wave_demo` —
NScripter compatibility flags. Impl `ScriptParser::mode_extCommand`,
`mode_sayaCommand`, `mode_wave_demoCommand`.

### autosaveoff (define)
`autosaveoff` — no automatic save point at text. Impl `ScriptParser::autosaveoffCommand`.

### usewheel / useescspc (define)
`usewheel` — wheel up/down returns -2/-3 in `btnwait`; `useescspc` — Escape
-10, Space -11. Impl `ScriptParser::usewheelCommand`, `useescspcCommand`.

### maxkaisoupage (define)
`maxkaisoupage n` — backlog pages kept. Impl `ScriptParser::maxkaisoupageCommand`.

### defaultspeed (define)
`defaultspeed slow,normal,fast` — text ms per character.
Impl `ScriptParser::defaultspeedCommand`.

### defaultfont (define)
`defaultfont "name"`. Impl `ScriptParser::defaultfontCommand`.

### defmp3vol / defsevol / defvoicevol
Default volumes (0-100). Impl `ScriptParser::defmp3volCommand` etc.

### shadedistance
`shadedistance x,y` — text shadow offset. Impl `ScriptParser::shadedistanceCommand`.

### underline
`underline y` — tachi-e bottom line. Impl `ScriptParser::underlineCommand`.

### humanz
`humanz z` — sprites numbered above `z` are drawn behind the text window.
Impl `ScriptParser::humanzCommand`.

### transmode (define)
`transmode leftup|rightup|copy|alpha` — default transparency of images.
Impl `ScriptParser::transmodeCommand`.

### windowback (define)
`windowback` — the text window is drawn under sprites above `humanz`.
Impl `ScriptParser::windowbackCommand`.

### windowchip (define)
`windowchip num` — sprite drawn with the text window. Impl `ScriptParser::windowchipCommand`.

### effectblank (define)
`effectblank ms` — pause after an effect. Impl `ScriptParser::effectblankCommand`.

### effectcut (define)
`effectcut` — effects drawn at once while skipping. Impl `ScriptParser::effectcutCommand`.

### effectskip
`effectskip 0|1` — whether a click skips an effect. Impl `ScriptParser::effectskipCommand`.

### effect / windoweffect
`effect n,kind[,ms[,image]]` (define, n ≥ 2) — define effect `n`;
`windoweffect kind[,ms[,image]]` — the text window's effect. Kinds: 1 at
once, 2-9 wipes/fades, 10 cross-fade, 11-14 scroll, 15/18 mask fade, 99 a
plugin `"name.dll/args"`. [cons] plugins: see [Transitions](#transitions-effect-99).
Impl `ScriptParser::effectCommand`.

### setlayer (define)
`setlayer num,interval,"dll"` — a built-in layer effect (snow, oldmovie…);
`layermessage num,"msg"` talks to it (result in `getret`).
Impl `ScriptParser::setlayerCommand`, `ONScripter::layermessageCommand`.

### soundpressplgin / spi (define)
`soundpressplgin "dll|ext"` — compressed sound plugin (ignored).
Impl `ScriptParser::soundpressplginCommand`.

### exec_dll
`exec_dll "name/args"` — built-in DLL emulation (`getret` for the result).
Impl `ONScripter::exec_dllCommand`.

### kinsoku / setkinsoku / addkinsoku
`kinsoku on|off`; `setkinsoku "start","end"`, `addkinsoku "start","end"`
(define) — line-break rules. Impl `ONScripter::kinsokuCommand`,
`ScriptParser::setkinsokuCommand`, `addkinsokuCommand`.

### roff (define), rmenu, rmode
`rmenu "name",func,...` — the right-click menu items (functions as
`systemcall`); `roff` disables it; `rmode 0|1` — 0 = right click returns -1
to the script. Impl `ScriptParser::rmenuCommand`, `roffCommand`,
`ONScripter::rmodeCommand`.

### menusetwindow / menuselectcolor / menuselectvoice (define)
`menusetwindow fw,fh,px,py,bold,shadow,"#col"`; `menuselectcolor
"#on","#off","#nofile"`; `menuselectvoice "open","cancel","menu","over",
"click","warn","yes","no"`. Impl `ScriptParser::menusetwindowCommand` etc.

### lookbackbutton / lookbackcolor / lookbacksp / lookbackflush
Backlog arrows' images, the text colour, their sprites (define), and
`lookbackflush` — clear the backlog. Impl `ONScripter::lookbackbuttonCommand`,
`ScriptParser::lookbackcolorCommand`, `lookbackspCommand`,
`ONScripter::lookbackflushCommand`.

### clickvoice (define)
`clickvoice "click","newpage"` — sounds at `@` and `\`.
Impl `ScriptParser::clickvoiceCommand`.

### clickstr / linepage / linepage2 (define)
`clickstr "。」",lines` — wait after these characters; new page after
`lines` lines. `linepage` — every line end is `\`; `linepage2` — `@`.
Impl `ScriptParser::clickstrCommand`, `linepageCommand`.

### savepoint / saveon / saveoff
`savepoint` — the save records this place; `saveoff`/`saveon` stop/start
recording at text. Impl `ONScripter::savepointCommand` etc.
CONscripter: a save keeps where it resumes, and where each `gosub` and
`for` returns, as the label, the line in it (0 is the `*label` line), the
byte in the line and the line's hash (ONScripter_savepoint.cpp), so a save
still loads at its line after a rebuild of `0.txt` that moved it. On load
the line is found by its hash, nearest to its old number; a line no longer
there falls back to the line at that number in the label (a return place:
the line after it), then to the label's start, with one `load: ...` error
line and a `save place lost` trace warning; a label gone keeps the stock
place (the line number in the whole script).

## Text and text window

### setwindow / setwindow2 / setwindow3
`setwindow x,y,cols,rows,fw,fh,px,py,speed,bold,shadow,"#col"|"image",wx1,wy1[,wx2,wy2]`
— text area and window; `setwindow2 "#col"|"image"` — only the window
picture; `setwindow3` like `setwindow` without a new page.
[cons] `consset "textshadow"` and `windowtrans` override shadow and opacity.
Impl `ONScripter::setwindowCommand`, `setwindow2Command`, `setwindow3Command`.

### windowtrans [cons]
`windowtrans opacity` — draw the window picture at 0-255 (over its own alpha).
Kept across `setwindow` and loads, not saved; `reset` → 255.
Impl `ONScripter::windowtransCommand`.

### textspeed / textspeeddefault
`textspeed ms` per character; `textspeeddefault` back to the menu's.
Impl `ONScripter::textspeedCommand`, `textspeeddefaultCommand`.

### textcolor
`textcolor "#RRGGBB"`. Impl `ONScripter::textcolorCommand`.

### texton / textoff / textshow / texthide / textclear / erasetextwindow
`textoff`/`texton` — hide/show the window and text; `texthide`/`textshow` —
redraw without/with text; `textclear` — new page; `erasetextwindow 0|1` —
hide the window during effects. Impl `ONScripter::textonCommand` etc.

### br
`br` — line break. Impl `ONScripter::brCommand`.

### locate
`locate x,y` — text cursor (characters). Impl `ONScripter::locateCommand`.

### indent
`indent n` — indent following lines. Impl `ONScripter::indentCommand`.

### tateyoko
`tateyoko 0|1` — horizontal / vertical text. [cons] turned marks
(《》「」『』（）【】〈〉―～…) are centred in their cell, and ← → ↑ ↓ are turned
too. Impl `ONScripter::tateyokoCommand`.

### rubyon / rubyoff
`rubyon [w,h[,"font"]]` — draw ruby `(base/reading)`; `rubyoff`.
Impl `ScriptParser::rubyonCommand`, `rubyoffCommand`.

### puttext
`puttext "text"` — draw text as a script line would. Impl `ONScripter::puttextCommand`.

### gettext
`gettext $s` — the current page's text. Impl `ONScripter::gettextCommand`.

### getcursorpos / getcursorpos2
`getcursorpos %x,%y` — where the next character goes (`2`: the last one).
Impl `ONScripter::getcursorposCommand`, `getcursorpos2Command`.

### drawtext
`drawtext` — draw the text window's text onto the screen buffer.
Impl `ONScripter::drawtextCommand`.

### selectcolor
`selectcolor "#on","#off"` — `select` choice colours.
Impl `ScriptParser::selectcolorCommand`.

## Backlog

### getlog / getlogtext
`getlog $s,page` — backlog page text (0 = newest); `getlogtext` without
tags. Impl `ONScripter::getlogCommand`.

### gettaglog
`gettaglog $s,page` — a page's `pagetag` tag. Impl `ONScripter::gettaglogCommand`.

### logsp / logsp2
`logsp num,page,x,y[,"#col"...]` — a sprite of a backlog page's text
(`logsp2` with font size and pitch). Impl `ONScripter::logspCommand`.

### logrows / getlogrow [cons]
`logrows %n,columns[,width]` — lay out the whole history as rows of `columns`
full-width characters (half-width count half, or in a proportional
horizontal sentence font their own width over the font's size, as the
text draws them; a half-width word that doesn't fit goes to the next
row, ruby as its base; with `width`, rows `width` pixels wide at the
sentence font's size), oldest first; `%n` = row count. `getlogrow $text,$voice,row` — row text (0 =
oldest, "" past the end) and, on a page's first row, the channel-0 `dwave`
voice of that page. Backlogs scrolled a line at a time:
`logrows %1,26` then `strsp` each row; the voice icon plays `dwave 0,$voice`.
Impl `ONScripter::logrowsCommand`, `getlogrowCommand`.

### logadd [cons]
`logadd "text"[,"voice"]` — add a page with `text` to the history, before
the page being written (the screen is left as it is); `voice` is the voice
`getlogrow` gives for it. Lines a story puts in the backlog without showing
them. Impl `ONScripter::logaddCommand`.

## Pictures: background, standing pictures, effects

### bg
`bg "file"|white|black|"#RRGGBB",effect[,ms[,image]]` — background.
Impl `ONScripter::bgCommand`.

### ld / cl / tal / humanorder
`ld l|c|r,"file",effect` — standing picture; `cl l|c|r|a,effect` — clear;
`tal l|c|r,trans,effect` — its transparency; `humanorder "lcr",effect` —
draw order. Impl `ONScripter::ldCommand`, `clCommand`, `talCommand`,
`humanorderCommand`.

### print
`print effect[,ms[,image]]` — show pending changes with an effect.
[cons] sprite motions, fades and alpha keep moving during the effect.
Impl `ONScripter::printCommand`.

### quake / quakex / quakey
`quake n,ms` — shake. Impl `ONScripter::quakeCommand`.

### flushout
`flushout ms` — fade to white. Impl `ONScripter::flushoutCommand`.

### monocro
`monocro "#col"|off` — tint everything behind the window.
Impl `ONScripter::monocroCommand`.

### nega
`nega 0|1|2` — negative (1 under the window, 2 all). Impl `ONScripter::negaCommand`.

### bgcopy / bgcpy
`bgcopy` — the current screen becomes the background. Impl `ONScripter::bgcopyCommand`.

### blt
`blt dx,dy,dw,dh,sx,sy,sw,sh` — blit from the background (`bg`) to the screen.
Impl `ONScripter::bltCommand`.

### ofscopy / ofscpy
`ofscopy` — copy the rendered screen into the work buffer.
Impl `ONScripter::ofscopyCommand`.

### repaint
`repaint` — redraw everything. Impl `ONScripter::repaintCommand`.

### getscreenshot / savescreenshot / savescreenshot2 / deletescreenshot
`getscreenshot w,h` — grab the screen; `savescreenshot "file.bmp"` — write
and free it (`2` keeps it); `deletescreenshot` — free it. With a save
directory set (`--save-dir`) the file goes there, without its folders;
one it can't write is a trace `warn`, `screenshot not written`.
Impl `ONScripter::getscreenshotCommand`, `savescreenshotCommand`,
`deletescreenshotCommand`.

### draw / drawclear / drawfill / drawbg / drawbg2 / drawsp / drawsp2 / drawsp3
Off-screen drawing: `drawclear`, `drawfill r,g,b`, `drawbg`, `drawbg2
x,y,sx,sy,rot`, `drawsp num,cell,alpha,x,y`, `drawsp2 num,cell,alpha,x,y,sx,sy,rot`,
`drawsp3 num,cell,alpha,x,y,m00,m01,m10,m11`; `draw` shows the buffer.
Impl `ONScripter::drawCommand` etc.

### screenfilter [cons]
`screenfilter slot,kind[,blend[,strength[,p1,p2,p3[,z]]]]` — `spfilter`'s
filters on the whole picture (background and sprites behind the window,
where `monocro` applies). Slots 1-4 apply in order; kind 0 clears a slot,
`screenfilter 0` all. With `z` (a sprite number) the slot applies right
after sprite z (and what `spz` puts over it) is drawn, so the sprites
above z stay unfiltered; a z in front of the window filters the window
too. Doesn't draw: follow with `print`. Saved, kept by `pushscreen`,
cleared by `reset`. `screenfilter 1,4,0,256,-200:print 10` (night);
`screenfilter 1,6,0,256,4,4,0,999` blurs the background only. Impl
`ONScripter::screenfilterCommand`.

### screenflash [cons]
`screenflash opacity[,"#RRGGBB"]` — add the colour (white) at opacity/256 over
everything, window and buttons included (not under `lsp2add`). Follow with
`print`; `screenflash 0` removes; not saved.
`screenflash 128:print 1:delay 100:screenflash 0:print 1`.
Impl `ONScripter::screenflashCommand`.

### screenshake / screenripple / screenpulse / screenzoom [cons]
Self-running effects on the `screenfilter` picture (after the filters) while
the script goes on. Slots 1-8 shared; a command with only a slot clears it,
slot 0 clears all. Each steps every `interval` ms; sizes go from the first to
the second value over `time` ms, then stay. Follow with `print`. Saved
(restarted on load), cleared by `reset`; while one runs every update redraws
the whole screen. All go through `ONScripter::screenEffectCommand`.

- `screenshake slot,time,interval,mode,speed_x,speed_y,x0,y0,x1,y1[,end]` —
  move by up to `x`,`y` px. Mode 0: sine per axis advancing speed/65536 turn
  per ms; mode 1: a fixed 7-point path (next point when speed_x × ms reaches
  65536). `end` > 0 stops it after `end` ms.
  `screenshake 1,500,16,0,1300,1700,10,10,10,10,500`.
- `screenripple slot,"map",time,interval,opacity0,opacity1,amp0,amp1,angle,angle_step,phase_step[,blend[,phase]]`
  — each pixel taken from amp × sin(2π(grey/256 + phase/1000)) px away in
  direction `angle` (tenths of a degree, 0 right, 900 down), grey from the
  greyscale `map`; laid over at opacity, or `blend` 1 multiplies. `phase`
  (1/1000 turn, default 0) is where the phase starts.
- `screenpulse slot,time,interval,zoom_x,zoom_y,opacity,colour,tint_opacity,tint`
  — a frozen copy, tinted (`tint` 0 none, 1 multiply by `colour` 0xRRGGBB,
  2 lay it over at tint_opacity), scaled 1 + f × zoom/1000 at f × opacity,
  f = sin(π t/time); nothing after `time` until cleared.
- `screenzoom slot,time,interval,zoom,x,y,zoom_time,fade_in,ease,power` — a
  frozen copy zoomed from 1 to zoom/1000 and its centre moved to x,y over
  zoom_time (ease 0 linear, 1 in, 2 out, 3 in-out, power/1000, MAGES curves),
  faded in over fade_in, held until cleared. zoom 1000 = freeze frame.

## Sprites

### lsp / lsph
`lsp num,"tagged image",x,y[,alpha]` — load sprite (`lsph` hidden). Image
tags: `:a;file` alpha, `:c;` copy, `:l;` left-top transparent, `:s/w,h,0;text`
text, `:a/cells,ms,mode;file` animation, `#RRGGBB` colour.
Impl `ONScripter::lspCommand`.

### lsp2 / lsph2 / lsp2add / lsph2add / lsp2sub / lsph2sub
`lsp2 num,"image",cx,cy,sx%,sy%,rot[,alpha]` — sprites drawn over all `lsp`
ones, scaled/rotated about their centre; `add`/`sub` blend.
Impl `ONScripter::lsp2Command`.

### csp / csp2
`csp num` (-1 all) — clear. [cons] also drops `spfilter`, `spzoom`, motions,
fades, `spz`; `csp -1` clears `spnew`'s sprites too, which stay held (their
variables keep their numbers; `spfree` gives one back).
Impl `ONScripter::cspCommand`.

### spnew / spfree [cons]
`spnew %var[,layer]` — a free sprite number from 1000 up into `%var` (a
`defvar` name usually), for `lsp`, `msp`, `spz`, buttons and the rest; its
place among the sprites is its layer's, not its number's: `game` (the
default; over sprite humanz+1: over every numbered sprite under the standing
pictures, under them and the window), `ui` (over sprite humanz: with
`windowback` over the text window, under sprites humanz-1 to 0) or `overlay`
(over sprite 0: over every numbered sprite); `0`, `1`, `2` for the same.
Within a layer the lower number is on top; `spnew` gives the lowest free one.
`spnew` on a variable that holds its handle already keeps the number (and
sets the layer). `spfree num` clears the sprite (as `csp`) and gives the
number back. The handles stay held through `reset` and a load (the variable
holds its number again); `definereset` frees them. Saves keep each handle
with its variable's name and the handles' pictures; the stock part of a save
holds sprites 0-999 only. `spz` puts a handle elsewhere, as any sprite.
`defvar namebox:spnew %namebox,ui:lsp %namebox,"name.png",20,400`.
Impl `ONScripter::spnewCommand`, `spfreeCommand` (`ONScripter_names.cpp`).

### vsp / vsp2
`vsp num,0|1` (-1 all) — visibility. Impl `ONScripter::vspCommand`.

### msp / msp2 / amsp / amsp2
`msp num,dx,dy[,dalpha]` — move relative; `amsp num,x,y[,alpha]` absolute;
`2` versions take scale/rotation too. [cons] ends a `spmotion` loop-2 first.
Impl `ONScripter::mspCommand`, `amspCommand`.

### cell
`cell num,cell` — show a cell. Impl `ONScripter::cellCommand`.

### spstr
`spstr "ctl"` — run an `exbtn` control string. Impl `ONScripter::spstrCommand`.

### spreload
`spreload num` (-1 the window) — reload a sprite's image. Impl `ONScripter::spreloadCommand`.

### strsp / strsph / vstrsp [cons]
`strsp num,"text",x,y,cols,rows,fw,fh,px,py,bold,shadow[,"#col"...]` —
a text sprite (`strsph` hidden). [cons] `vstrsp` same args, text in
columns top to bottom, first at the right, brackets/dashes/long vowels turned.
Both kept in saves (text, font, colours) and by `pushscreen`. Spaces after
text are kept. Impl `ONScripter::strspCommand`.

### strspoutline [cons]
`strspoutline "#RRGGBB"` — later `strsp`/`vstrsp` with shadow on draw a 1 px
outline of this colour instead; `""` back to the shadow. Kept with the sprite.
Impl `ONScripter::strspoutlineCommand`.

### prnum / prnumclear
`prnum n,value,x,y,w,h,"#col"` — draw a number (n 0-99); `prnumclear`.
Impl `ONScripter::prnumCommand`, `prnumclearCommand`.

### bar / barclear
`bar n,value,x,y,w,h,max,"#col"` — a bar; `barclear`.
Impl `ONScripter::barCommand`, `barclearCommand`.

### sp_rgb_gradation
`sp_rgb_gradation num,ur,ug,ub,lr,lg,lb,kr,kg,kb,alpha` — vertical gradient
on a sprite, key colour kept. Impl `ONScripter::sp_rgb_gradationCommand`.

### allsphide / allspresume / allsp2hide / allsp2resume
Hide / bring back all `lsp` (`2`: `lsp2`) sprites.
Impl `ONScripter::allsphideCommand` etc.

### getspmode / getsppos / getspsize
`getspmode %v,num` (visible); `getsppos num,%x,%y`; `getspsize num,%w,%h[,%cells]`.
Impl `ONScripter::getspmodeCommand` etc.

### spfilter [cons]
`spfilter num,kind[,blend[,strength[,p1,p2,p3]]]` — colour filters applied
when the image loads (no per-frame cost). kind: 1 colour p1,p2,p3; 2 grey;
3 negative; 4 + p1 (-255..255); 5 grey + offsets (sepia `28,-8,-38`); 6 blur
radius p1,p2; 7 hue +p1/256 turn, saturation p2 (-256 grey..256); 8 lightness
inverted. blend: 0 copy, 1 multiply, 2 add, 3 screen. strength 0-256 (256).
Up to 4 stack; `spfilter num,0` removes. Stays through `lsp`, removed by
`csp`, saved. `spfilter 10,1,1,256,148,152,200` (night tint).
Impl `ONScripter::spfilterCommand`.

### spzoom [cons]
`spzoom num,zx[,zy[,angle]]` — zoom % (negative flips) and turn angle
degrees counterclockwise (as `lsp2`; kept when left out) about the centre of
its place, keeping its place among the sprites (unlike `lsp2`). Stays through
`lsp`, reset by `csp`, saved, kept by `pushscreen`; buttons keep the
unzoomed place. Redraws the whole screen at the next `print`; call again after
moving: `amsp 10,x,y:spzoom 10,150:print 1`; `spzoom 10,100,100,-15`
tilts it. An `spmotion` angle adds to it; loop 2 leaves its last angle here. Animated sprites cycle cells.
Impl `ONScripter::spzoomCommand`.

### spmotion [cons]
`spmotion num[,loop,t,x,y,zx,zy,angle,alpha,curve,...]` — keys from its
place: offset x,y, zoom zx,zy % (times `spzoom`), angle (degrees CCW, added
to `spzoom`'s), alpha 0-256, at `t` ms from now; `curve` is how the key is
reached from the one before: 0 straight, 1 ease in, 2 ease out, 3 ease in
and out, 4 jump at the key, 10-73 an `spcurve` table. Stepped every ~16 ms
by the timer (also during waits). loop 0: ends, back at its place; 1:
repeats; 2: once, then the last key's offset/zoom/angle/alpha are applied to
the sprite; -n: n times, then as 2; 3: once, then the sprite is removed (as
`csp`; `lsp` on it ends that and keeps it; saves and `pushscreen` leave it
out); 4: `spmotion num,4,k,keys...`, once, then repeats from key k on (a
lead-in, then a loop: the last key should match key k). Under `--auto-play` (not `--real-time`) every motion but loop 1 ends at
once. `spmotion num` stops. Up to 64 keys.
Stays through `lsp`, removed by `csp`, saved (restarted). Another
`spmotion`, `lsp`, `msp`, `amsp` first finishes a loop 2/-n motion at its
last key.
`spmotion 10,1,0,0,0,100,100,0,256,0,250,0,-12,100,100,0,256,3,500,0,0,100,100,0,256,3` (bob);
`spmotion 10,1,0,0,0,100,100,0,256,0,1000,0,0,100,100,360,256,0` (spin);
`spmotion 10,4,1,0,0,0,100,100,0,256,0,500,0,40,100,100,0,256,0,1000,0,30,100,100,0,256,3,1500,0,40,100,100,0,256,3` (drop in, then bob);
`spmotion 20,1,0,0,0,100,100,0,0,0,600,0,0,100,100,0,256,0,1100,0,0,100,100,0,256,0,1700,0,0,100,100,0,0,0` (glow);
`lsp 60,":a;noise.png",0,0,200:spmotion 60,0,0,0,0,100,100,0,0,0,2000,0,0,100,100,0,256,0` (fade in, was `spfade 60,2000`);
`spmotion 60,3,0,0,0,100,100,0,256,0,300,0,0,100,100,0,0,0` (fade out and remove).
Impl `ONScripter::spmotionCommand`.

### spmotionwait [cons]
`spmotionwait num[,click]` — wait until sprite num's `spmotion` ends (-1:
every motion that ends; looping ones are left out); click 1: a click ends
the wait. What is left (a click, skipping) jumps to the motion's end, and the
screen is drawn. `spmotion 10,2,...:spmotionwait 10`.
Impl `ONScripter::spmotionwaitCommand`.

### spcurve [cons]
`spcurve id,v0,v1,...,vn` — curve id (10-73) for `spmotion` keys: how far
(0-1024) from one key to the next at n+1 even steps (2 to 1025 values), straight
between them. For an easing table read from a game's exe, as it is. Saved.
`spcurve 10,0,40,300,1024`. Impl `ONScripter::spcurveCommand`.

### spcut [cons]
`spcut num,"mask",x,y` — cut a hole in sprite num's picture (every cell): the
mask, its top left at screen (x, y), takes opacity away where it is dark
(black: all of it). Lasts until the picture is loaded again; not saved.
`getmousepos %0,%1:spcut 52,"hole.png",%0,%1:print 1`.
Impl `ONScripter::spcutCommand`.

### spgrab [cons]
`spgrab num[,src[,x,y,w,h]]` — a copy into lsp sprite num, shown where it was
taken: src -1 (default) the screen as sprites and background draw it now
(no text window, sprite num left out), src n sprite n's picture (its cell
now; x, y in that picture); default all of it. The picture under it can
then change while the copy fades or moves away by itself: a cross-fade the
script doesn't wait for, `spgrab 20:bg "b.png",1:spmotion 20,3,0,0,0,100,100,0,256,0,800,0,0,100,100,0,0,0`. Not kept
in saves (a load leaves it empty) or by `pushscreen`; the trace names it
`(spgrab)`. Impl `ONScripter::spgrabCommand`.

### spblend [cons]
`spblend num,mode` — mode 1 adds the sprite's colours (times its alpha) to
what is under it, mode 2 multiplies what is under it by them (white leaves
it, black darkens it, times its alpha), mode 3 subtracts its colours, mode 4
screens (lightens, never past white), keeping its place among the lsp sprites; 0 normal. Stays
through `lsp`, removed by `csp`, saved.
`lsp 2,":c;>800,600,#ffffff",0,0:spblend 2,1:spmotion 2,-5,0,0,0,100,100,0,128,0,80,0,0,100,100,0,0,0` (five flashes).
Impl `ONScripter::spblendCommand`.

### spclip [cons]
`spclip num,x,y,w,h` — draw sprite num only inside the screen's rectangle
(x, y, w, h), wherever the sprite moves (a window its children show
through); `spclip num` draws it everywhere again. Stays through `lsp`,
removed by `csp`, saved, kept by `pushscreen`.
`lsp 20,"tape.png",262,-220:spclip 20,260,138,100,300`.
Impl `ONScripter::spclipCommand`.

### sptrail [cons]
`sptrail num,passes,alpha,lag` — draw `passes` (1-32) copies of sprite num
under it, each at `alpha` per mille (1-1000) of its alpha; copy i is where
its `spmotion` had it `lag` × i ms before (a motion trail).
Before the motion starts the copies sit on its first key; after it ends
they go on to its end and reach the sprite. A sprite that doesn't move
shows no copies, and `msp`/`amsp` moves aren't followed. `sptrail num`
ends it. Stays through `lsp`, removed by `csp`, saved (not by
`pushscreen`, like the motions).
`lsp 10,"car.png",0,200:sptrail 10,3,500,40:spmotion 10,0,0,0,0,100,100,0,256,0,1000,800,0,100,100,0,256,0`.
Impl `ONScripter::sptrailCommand`.

### spz [cons]
`spz num[,over]` — draw sprite `num` just over sprite `over` (between `over`
and `over-1`); `spz num` resets. `spz 70,61` draws 70 over 61-69, under 60.
Saved, kept by `pushscreen`, removed by `csp`. Impl `ONScripter::spzCommand`.

### spovertext [cons]
`spovertext num[,on]` — draw sprite `num` over the text window and its text
(and the bars), under the sprites `windowback`/`humanz` put over the window;
several in their numbers' order. `spovertext num,0` (or `spz num`) puts it
back. Saved, kept by `pushscreen`, removed by `csp` (as `spz`). Subtitles
over a text box: `lsp 400,"sub.png",0,480:spovertext 400:print 1`.
Impl `ONScripter::spovertextCommand`.

### pushscreen / popscreen [cons]
`pushscreen first,last` — push background, sprites first..last (image, pos,
visibility, cell, filters, strsp text), screen filters and BGM (32 deep);
`popscreen` restores the newest. Neither draws: follow with `print`.
Saved; emptied by `reset`. `pushscreen 0,999:gosub *scene:popscreen:print 10`.
Impl `ONScripter::pushscreenCommand`, `popscreenCommand`.

## Buttons and input

### btndef / bclear
`btndef "image"|clear` — the picture `btn` cuts from; `bclear` — clear buttons.
Impl `ONScripter::btndefCommand`.

### btn
`btn n,x,y,w,h,sx,sy` — a button from the `btndef` picture. Impl `ONScripter::btnCommand`.

### spbtn / cellcheckspbtn / existspbtn [cons]
`spbtn num,n[,pressed_cell]` — sprite `num` is button `n` (cell 1 on hover).
[cons] `pressed_cell` shows while the left button is held on it.
`cellcheckspbtn` only if the sprite has 2+ cells.
Impl `ONScripter::spbtnCommand`.

### exbtn / exbtn_d / cellcheckexbtn / bdef
`exbtn num,n,"ctl"` — button that runs control string `"ctl"` (`C` clear,
`P num,cell`, `S`…) on hover; `exbtn_d "ctl"` — when nothing is hovered.
Impl `ONScripter::exbtnCommand`.

### bsp
`bsp num[,"hover","none"]` — sprite button with ctl strings. Impl `ONScripter::bspCommand`.

### btnwait / btnwait2 / textbtnwait / selectbtnwait / bexec
`btnwait %v` — wait for a button; `btnwait2` keeps buttons; `textbtnwait`
waits for text and buttons; `bexec $s[,%x,%y]` — key string result.
[cons] `btnwait`/`btnwait2` end auto mode and skip (`textbtnwait` neither).
Returns: n button, 0 click off buttons, -1 right click, -2/-3 wheel up/down
(`usewheel`), -10 Esc / -11 Space (`useescspc`), -12/-13 PgUp/PgDn
(`getpage`), -19 Enter (`getenter`), -20 Tab (`gettab`), -21..-32 F1-F12
(`getfunction`), -40..-43 ↑→↓← (`getcursor`), -50 Insert (`getinsert`),
-51/-52/-53 Z/X/C (`getzxc`), -70 middle click (`getmclick`), -2 or -5
timeout (`btntime`). [cons] `getkey` letter codes; `btnse` plays on a button;
`select` ignores non-option `spbtn` buttons.
Impl `ONScripter::btnwaitCommand`.

### btntime / btntime2 / btime
`btntime ms` — the next `btnwait` times out; `btntime2` also during voice.
Impl `ONScripter::btntimeCommand`.

### getbtntimer / gettimer / resettimer / waittimer
`gettimer %v` — ms since `resettimer`; `getbtntimer %v` — ms the last
`btnwait` took; `waittimer ms` — wait until the timer reaches ms.
[cons] scaled by `consset "gamespeed"`. Impl `ONScripter::gettimerCommand`,
`resettimerCommand`, `waittimerCommand`.

### btndown / bdown
`btndown 0|1` — buttons react on press. Impl `ONScripter::btndownCommand`.

### transbtn / btrans
`transbtn` — transparent pixels aren't part of a button. Impl `ONScripter::transbtnCommand`.

### getpage / getpageup / getenter / gettab / getcursor / bcursor / getinsert / getzxc / getfunction / getmclick
Enable the `btnwait` codes above for one wait.
Impl `ONScripter::getpageCommand` etc.

### getkey [cons]
`getkey` — the next `btnwait`/`textbtnwait` also ends on a letter key,
returning -1000 - upper-case code (A -1065, M -1077, Z -1090), before the
key's own meaning. One wait. Impl `ONScripter::getkeyCommand`.

### checkkey
`checkkey %v,"KEY"` — 1 if the last button string matches.
Impl `ONScripter::checkkeyCommand`.

### spclclk
`spclclk` — Space acts as a click. Impl `ONScripter::spclclkCommand`.

### getmousepos / getmouseover / movemousecursor
`getmousepos %x,%y`; `getmouseover lo,hi` — hover returns button numbers in
range; `movemousecursor x,y`. Impl `ONScripter::getmouseposCommand` etc.

### setcursor / abssetcursor
`setcursor n,"image",x,y` — click-wait cursor (n 0 line, 1 page); `abs` in
screen coordinates. Impl `ONScripter::setcursorCommand`.

### isdown / isfull / isskip / ispage
`isdown %v` (mouse held), `isfull %v` (full screen), `isskip %v` (1 skip,
2 auto), `ispage %v` (at a page wait). Impl `ONScripter::isdownCommand` etc.

### click / lrclick
`click` — wait for a click; `lrclick` — left or right. Impl `ONScripter::clickCommand`.

### autoclick
`autoclick ms` — click waits end by themselves. Impl `ONScripter::autoclickCommand`.

### automode_time [cons]
`automode_time ms[,char_ms]` — auto wait; [cons] second number adds
`char_ms` per character of the page. One number: NScripter's (negative =
per character). Only `ms` kept in `envdata`.
Impl `ONScripter::automode_timeCommand`.

### usewheel, useescspc
See define section.

## Choices

### select / selgosub / selnum / csel
`select "text",*label,...` — choose and jump; `selgosub` — gosub; `selnum
%v,"a","b",...` — index. `csel` — custom select (`cselbtn`, `cselgoto`,
`getcselnum`, `getcselstr`). [cons] `consset "selectkeep",1` keeps auto/skip.
Impl `ONScripter::selectCommand`.

### cselbtn / cselgoto / getcselnum / getcselstr
`cselbtn n,btn,x,y`; `cselgoto n`; `getcselnum %v`; `getcselstr $s,n`.
Impl `ONScripter::cselbtnCommand` etc.

### nextcsel
`nextcsel %v` — whether a `csel` follows. Impl `ONScripter::nextcselCommand`.

### selectvoice [cons]
`selectvoice "open","over","select"` — sounds; [cons] allowed anywhere, `""`
is no sound. Impl `ScriptParser::selectvoiceCommand`.

### yesnobox / okcancelbox
`yesnobox %v,"message","title"` — 1 yes, 0 no. Impl `ONScripter::yesnoboxCommand`.

## Sound

### bgm / mp3 / mp3loop / bgmonce / mp3save
`bgm "file"` — loop music; `bgmonce`/`mp3` once; `mp3save` saved with the
game. [cons] starts at the music setting (was 78 %). [cons] `bgm "file",ms`
cross-fades: the music playing fades out over ms while the new one fades in
(not for `.wav`/MIDI music, nor while skipping or under `--auto-play`, where
the old one stops at once). Impl `ONScripter::mp3Command`, `crossFadeOutBGM`.

### bgmstop / mp3stop
Stop music (with `bgmfadeout` fade). [cons] a fade-out during `bgmfadein`
no longer hangs. Impl `ONScripter::mp3stopCommand`.

### bgmfadein / mp3fadein / bgmfadeout / mp3fadeout
`bgmfadein ms`, `bgmfadeout ms` — fades for the next start/stop.
[cons] fade to/from the `chvolfade -1` volume; skipping (and fast `--auto-play`), music starts at full volume. Impl `ONScripter::mp3fadeinCommand`,
`mp3fadeoutCommand`.

### bgmvol / mp3vol / getbgmvol / getmp3vol
Music volume 0-100. Impl `ONScripter::mp3volCommand`, `getmp3volCommand`.

### bgmlater [cons]
`bgmlater ms,"file"[,loop]` — the music starts ms from now while the script
goes on (through waits, text and click waits), as `bgm`, or as `bgmonce` with
loop 0. A music command before then (`bgm`, `bgmonce`, `mp3`…, `play`,
`bgmstop`, `playstop`), `reset` and a load cancel it; saved (the time left).
`bgmonce "intro.ogg":bgmlater 7720,"loop.ogg"` starts a loop 7.7 s after its
intro. Impl `ONScripter::bgmlaterCommand`.

### getbgmpos [cons]
`getbgmpos %v` — ms the music playing now is through its current pass (a
looped piece counts from its start again each time round); 0 with no music or
music played as a sound (.wav); while a `bgmlater` music is still to start,
minus the ms until it starts. `getbgmpos %1:mov %2,252000-%1:bgmlater
%2,"next.ogg",0` plays next.ogg when a 252 s song ends. Impl
`ONScripter::getbgmposCommand`.

### loopbgm / loopbgmstop
`loopbgm "intro","loop"`; `loopbgmstop`. Impl `ONScripter::loopbgmCommand`,
`loopbgmstopCommand`.

### play / playonce / playstop
MIDI/CD: `play "file"`. Impl `ONScripter::playCommand`, `playstopCommand`.

### wave / waveloop / wavestop
`wave "file"` — on the system channel. [cons] its volume is `consset "sysevol"`.
Impl `ONScripter::waveCommand`, `wavestopCommand`.

### dwave / dwaveloop / dwaveload / dwaveplay / dwaveplayloop / dwavestop
`dwave ch,"file"`; `dwaveload` loads, `dwaveplay` plays it; `dwavestop ch`.
Channel 0 is the voice. [cons] looping channels kept in saves; cut sounds
fade 10 ms. [cons] `dwaveloop` loops inside the file when it has a loop:
`LOOPSTART`/`LOOPEND` (or `LOOPLENGTH`) Ogg Vorbis comments, or a WAV's
`smpl` loop (as `bgm` does through SDL_mixer); not after `dwaveload` or a
load. Impl `ONScripter::dwaveCommand`, `dwavestopCommand`, `setChunkLoop`.

### dwavefadein [cons]
`dwavefadein ms` — the next `dwave*` fades in (once). Fading out:
`chvolfade ch,0,ms,1`. Impl `ONScripter::dwavefadeinCommand`.

### chvol / chvolfade [cons]
`chvol ch,vol` — the setting; [cons] `chvolfade ch,vol[,ms[,stop]]` — the
script's own % on top, ramped, stays with the channel, saved, `reset` → 100.
ch -1 is the music (`bgmfadein`/`bgmfadeout` fade to and from it; `chvolfade
-1,20,500` ducks). stop 1: the dwave channel's sound fades out over ms and
stops (its volume stays for the next sounds; a looping one fading out isn't
saved). `chvolfade 3,30:dwaveloop 3,"se\rain.ogg"`; `chvolfade 3,0,1500,1`.
Impl `ONScripter::chvolCommand`, `chvolfadeCommand`.

### chpan [cons]
`chpan ch,pan[,ms]` — dwave channel ch's pan, -1000 (left only) .. 0 (centre)
.. 1000 (right only), lowering the other side; over ms (default 0) while the
script goes on. Stays with the channel, saved, `reset` → 0; stereo output
only; not the music. `chpan 2,-1000:dwave 2,"se\shot.ogg"`; `chpan 2,1000,1500`.
Impl `ONScripter::chpanCommand`.

### chfreq [cons]
`chfreq ch,rate[,ms]` — the sound playing on dwave channel ch at rate/1000 of
its speed and pitch (100-4000), ramped from its rate now over ms (gamespeed
applies). A looping sound stays at it; one that doesn't ends sooner or later.
The next sound on the channel plays at its own rate; not saved. No sound: no
effect. `dwave 10,"se\shot.ogg":chfreq 10,1100,1000`. Impl
`ONScripter::chfreqCommand`.

### sevol / voicevol / getsevol / getvoicevol
Volumes 0-100. Impl `ONScripter::sevolCommand` etc.

### stop
`stop` — all sound. Impl `ONScripter::stopCommand`.

### v / dv
`v123` / `dv123` — play `voice\123.wav` (prefix commands).
Impl `ONScripter::vCommand`, `dvCommand`.

### consset / consget [cons]
`consset "key",value` / `consget %var,"key"` — player settings (memory
only; the game sets them again at startup). Keys: `mutebgm`, `mutese`,
`mutevoice` (1 mutes; sounds play on unheard; auto mode doesn't wait for a
muted voice); `clickvoicestop` (1: a click ending a text wait stops the
voice); `autovoicewait` (0: auto mode doesn't wait for the voice; default
1); `selectkeep` (1: auto and skip continue after a `select`); `moviestop`
(1 a click always stops a movie, 0 never, -1 as the script says);
`gamespeed` (10-10000 %, 100: script times — `wait`, `delay`, timers,
effects, `spmotion`, `screenshake` — run at that speed; text
speed, auto wait and sounds unaffected); `sysevol` (volume of `wave`'s
channel: menu sounds, `selectvoice`, `btnse`; -1 = `sevol`); `effectoff`
(1: every `print`/`bg`/`ld`/`cl` effect drawn at once); `textshadow` (0: no
message text shadow whatever `setwindow` says; default 1). An unknown key
is an error. `consset "gamespeed",200`.
Impl `ONScripter::conssetCommand`, `consgetCommand`.

### btnse [cons]
`btnse "file"` — played on `wave`'s channel when a button wait ends on a
button (≥ 1). `""` none. Not saved. Impl `ONScripter::btnseCommand`.

## Movies

### mpegplay [cons]
`mpegplay "file",click[,1]` — play a movie; click 1 = clickable. [cons] a
third argument 1 keeps the music playing. [cons] A click that stops it
also stops the `dwave` sounds started since the last wait (the movie's own).
Linux/macOS play through ffmpeg
(`CONS_FFMPEG`, else `PATH`). [cons] The text window, when shown, is drawn
over the movie (nothing else). Impl `ONScripter::mpegplayCommand`.

### Movies as pictures [cons]
A movie file (`.mp4`, `.m4v`, `.webm`, `.mpg`, `.mpeg`, `.ogv`, `.mov`,
`.mkv`, `.avi`, `.wmv`, a loose file, not in an archive) given to `lsp`,
`lsp2`, `bg` or `ld` plays in the picture's place, without its sound, while
the game waits: the sprite order, the tag's transparency (`:a` takes the
right half as the alpha), `spmotion`, `spzoom` and the text window over it
work as for a picture. `":c;movie\cut.mp4"` plays once and stays on its last
frame, `":c/1,0,0;movie\loop.mp4"` loops. `csp` (or another picture) stops
it; a save keeps the name, so a load starts it again. Under `--auto-play`
only its first frame shows. Linux/macOS decode through ffmpeg, the web build
through a hidden `<video>`. Impl `ONScripter::openSpriteMovie`,
`proceedSpriteMovies`.

### spmoviewait [cons]
`spmoviewait n[,click]` — waits until the movie in sprite n has ended; with
click 1 a click, skip or auto-play ends the wait (`consset "moviestop"` overrides the
flag), the movie stopping on the frame shown and the `dwave` sounds started
since the last wait stopping with it. A looping movie, or none, returns at
once. Impl `ONScripter::spmoviewaitCommand`.

### mpeghold [cons]
`mpeghold "file"[,sp[,ch,"sound",fade_ms]]` — a movie that plays only while
the left button, a touch, Enter or Space is held down and stops on its
frame when let go; its first frame shows until the first press; it can't be
skipped and the music plays on. Sprite sp (-1 none), if shown, is drawn
over it with its cells going round (a "hold the button" prompt). "sound"
plays on dwave channel ch: fade_ms 0 pauses and resumes it with the movie
(it plays on after the movie); otherwise each press starts it again,
looped, and it fades out over fade_ms when let go and when the movie ends.
Linux/macOS (ffmpeg) and the web build. Impl `ONScripter::mpegholdCommand`.

### movie
`movie "file"[,pos,x,y,w,h][,click][,loop][,async]` / `movie stop`.
Impl `ONScripter::movieCommand`.

### avi
`avi "file",click`. Impl `ONScripter::aviCommand`.

## Waiting and timing

### wait / delay
`wait ms` (not clickable), `delay ms` (clickable). [cons] scaled by `consset "gamespeed"`.
Impl `ONScripter::waitCommand`, `delayCommand`.

### skipoff
`skipoff` — leave skip mode. Impl `ONScripter::skipoffCommand`.

### autooff [cons]
`autooff` — leave auto mode (as a click would). Impl `ONScripter::autooffCommand`.

## Saves

### savegame / savegame2
`savegame n` / `savegame2 n,"text"`. Impl `ONScripter::savegameCommand`.

### loadgame
`loadgame n`. Impl `ONScripter::loadgameCommand`.

### savefileexist / savetime / getsavestr
`savefileexist %v,n`; `savetime n,%month,%day,%hour,%min`; `getsavestr $s,n`.
Impl `ONScripter::savefileexistCommand` etc.

### delsavegame [cons]
`delsavegame no[,"thumb"]` — delete the save and its `savescreenshot`
thumbnail; web build syncs. Impl `ONScripter::delsavegameCommand`.

### copysavegame [cons]
`copysavegame from,to` — copy as is; missing `from` deletes `to`.
Impl `ONScripter::copysavegameCommand`.

### checkpage
`checkpage %v,page` — whether backlog page exists. Impl `ONScripter::checkpageCommand`.

## Screens [cons]

Screens the engine draws: the save and load menus so far (settings,
backlog, extras and the title to follow, in the same format). A screen is
a Lua file in the archive (or loose), `screens/<name>.lua`, that returns a
table describing it; the engine draws it with sprite handles over
everything else (over `spnew`'s overlay layer; a screen opened over
another is on top), runs it until it closes, and handles the mouse, touch,
keyboard (Up/Down move between the buttons, Enter presses, Esc is
right-click) and gamepad as `btnwait` does. A `--trace` gets a `screen`
record per action; `--auto-play` closes save, load, settings, backlog and
extras screens at once (`--skip-screens`, [DEBUG.md](DEBUG.md)).

### screen [cons]
`screen "name"[,%var[,"arg"]]` — open `screens/name.lua` and run it until
it closes; `%var` gets the value its `close` action gave (0 by default,
and for right-click). A `gosub` action runs its label with the screen still
up and comes back to the `screen` command, which goes on with the screen
(so the command must be reached again: a `return`). A `load` action loads
and doesn't come back. Opening a screen clears the button definitions (as
`btnwait` does). Impl `ONScripter::screenCommand` (`ONScripter_screen.cpp`).

`screen "save"`, `screen "confirm",%ok,"上書きしますか？"`.

### The table

The file is UTF-8 Lua (base, string, table and math libraries), run each
time the screen opens, so it can compute its table from the game's state:
`var(n | "name")` gives an integer variable (`%n` or a `defvar` name),
`str(n | "name")` a string variable (in UTF-8), and `arg` is the `screen`
command's third argument (a confirm dialog's question). Text is converted
to the script's encoding. Coordinates are screen pixels; file names as in
`lsp` (`/` or `\`).

```lua
return {
  kind = "save",          -- save, load, settings, backlog, extras, confirm, title, other (default)
  hide = { window = true, sprites = {34, 35} }, -- hidden while open (only those shown), shown again after
  filter = {4, 6, 0, 256, 1, 1, 0}, -- a screenfilter while open (slot first), cleared after
  dialog = "confirm",     -- the screen that asks actions' questions (default "confirm")
  cancel = {"close"},     -- what right-click and Esc do (default: close with 0)
  widgets = { ... },      -- drawn in order, the first at the bottom
}
```

Every widget is a table whose first element is its type; `page = n` on
any widget shows it only while the save slots show page n.

| Widget | Fields |
|---|---|
| `{"image", ...}` | `file`, `x`, `y`; `cells` (a strip of n cells), `alpha` (0-255), `opaque` (`:c`, no alpha), `blend` (`add`, `multiply`, `sub`, `screen`: `spblend`), `scale` (percent, about its centre: `spzoom`) |
| `{"text", ...}` | `text`, `x`, `y`, `size` (20), `cols` (40), `rows` (1), `bold`, `shadow`, `color` (`"#rrggbb"`), `align = "centre"` (x is the centre, measured as the script measures: half-width characters half as wide). Drawn as `strsp` |
| `{"button", ...}` | an image (fields as above, cell 1 under the pointer) and `action`; `disabled` (the cell shown when the action can't be taken: no slot selected, an empty slot to load or delete, the page shown; without it the normal cell), `pressed` (the cell while held, `spbtn`'s third argument) |
| `{"saveslots", ...}` | below |

**saveslots**: `rows` × `cols` save slots a page, at `x + col*dx`,
`y + row*dy`; `first` (1) is the first page's first slot, `pages` (1) the
number of pages; `page_var` (`"%900"`) holds the page shown (read at open,
written at close; without it the engine keeps the page per screen while it
runs). Positions inside a slot are relative to its corner:

- `button = {file, cells, x, y}`: the slot's hit area and hover look (cell 1);
- `select = true`: a click selects the slot, a second click (or a button's
  action without a slot number) acts on it; without it a click acts;
- `action`: what acting on a slot does (`{"save"}`, `{"load"}`, a list);
- `selected = {image}`: drawn on the selected slot (under the rest);
- `thumb = {x, y, w, h}`: the save's thumbnail scaled into the box;
  `preview = {x, y, w, h}` (absolute): the selected slot's;
  `shot = {w, h}`: the thumbnail's size, written as `savethumb<n>.bmp` in
  the save folder with each save (default the `thumb` box): the screen
  as it was when the menu opened;
- `new = {image}`: on the slot with the newest save among the widget's;
- `save_text = "$150"`: a variable whose value is saved as the save's
  text (`savegame2`'s);
- `lines = { {x, y, size, ..., text = "...", empty = "..."} }`: text lines
  (fields as `text`); `text` for a slot with a save, `empty` without one.
  `%N` is the slot in two digits, `%n` as is, `%Y %y %m %d %H %M %S` the
  save's time (its file's), `%T` its text, `%%` a `%`. With `split = "|"`
  on the widget, `%T1` to `%T9` are the text's parts cut at it, `%T-1` to
  `%T-9` counted from the last (`%T1` and `%T-1` the whole text without
  one; a part it lacks is empty): a game that saves more than one field in
  the text, or reads saves of an older build ("date|chapter" and "chapter"
  both show the chapter as `%T-1`).

**Actions**: a table `{"what", ...}`, or a list of them run in order. A
`save`, `load` or `delete` that can't be taken (no slot, an empty slot to
load or delete) ends the list; so does a question answered no.

| Action | Does |
|---|---|
| `{"close"[, value]}` | closes the screen; `screen`'s `%var` gets value |
| `{"open", "name"[, "arg"]}` | opens another screen over this one, then comes back to it (it can't `gosub`) |
| `{"page", n}` | shows page n of the save slots (`"+1"`, `"-1"`: the next, the previous) |
| `{"save"[, n]}`, `{"load"[, n]}`, `{"delete"[, n]}` | slot n, else the slot acted on. `save` writes the game as it was when the screen opened (as `savegame2` with `save_text`) and the thumbnail; `load` closes every screen and loads (`loadgosub` runs); `delete` removes the save and its thumbnail |
| `{"set", "%var", value}` | sets a variable (`slot = true` in place of the value: the slot acted on) |
| `{"gosub", "*label"}` | runs the label with the screen up; the actions after it run when it returns |

`confirm = "question"` on `save`, `load` or `delete` asks first on the
`dialog` screen (opened with the question as `arg`, `%N` replaced by the
slot): a close value other than 0 is yes. A `save` asks only over an
existing save. To be added in the same format: `slider`, `toggle`,
`radio` (bound to a setting or variable), `list`, `gallery`, sounds, and
the `voice` action.

A confirm dialog:

```lua
return {
  kind = "confirm",
  widgets = {
    {"image", file = "system/dialog_base.png", x = 140, y = 254},
    {"text", text = arg, x = 400, y = 270, size = 20, align = "centre", shadow = true},
    {"button", file = "system/dialog_button.png", cells = 3, x = 242, y = 305, action = {"close", 1}},
    {"button", file = "system/dialog_button.png", cells = 3, x = 416, y = 305, action = {"close", 0}},
  },
}
```

A save menu's slots, five a page on ten pages, selected then saved, with
the game's routine run after each save:

```lua
{"saveslots", x = 0, y = 44, rows = 5, dy = 92, pages = 10, page_var = "%900", select = true,
  action = {{"set", "%102", slot = true}, {"save", confirm = "No.%N に上書きしますか？"}, {"gosub", "*keep"}},
  button = {file = "system/row.png", cells = 2},
  thumb = {x = 14, y = 10, w = 100, h = 75}, shot = {400, 300},
  save_text = "$150",
  lines = {{x = 124, y = 11, size = 14, text = "No.%N %Y/%m/%d %H:%M", empty = "No.%N"}, {x = 124, y = 26, size = 14, text = "%T", empty = ""}}}
```

## Window and system

### menu_full / menu_window
Full screen / window. Impl `ONScripter::menu_fullCommand`, `menu_windowCommand`.

### menu_automode / menu_click_def / menu_click_page
Auto mode on; click to end line / page. Impl `ONScripter::menu_automodeCommand` etc.

### openurl [cons]
`openurl "url"` — open in the browser (new tab on web), leaving full screen.
With `--auto-play` only printed. Impl `ONScripter::openurlCommand`.

---

## Text syntax

In script text lines:

- `@` click wait, `\` click and new page, `/` (at line end) no line break,
  `_` the next character ignores kinsoku, `~` a `jumpf`/`jumpb` mark.
- `!w ms` wait, `!d ms` clickable wait, `!s ms` text speed (`!sd` default).
- `#RRGGBB` text colour.
- `(base/reading)` ruby with `rubyon`.
- `` ` `` starts half-width (English) text.
- **`!zNNN` [cons]** — text at NNN % of the window font size until the next
  `!z` (`!z`/`!z100` resets; `reset` clears). Sits on the baseline; line
  pitch unchanged; the backlog shows normal size.
- **`!i1` … `!i0` [cons]** — italic text (the font's synthetic italic)
  until `!i0` (or `!i`); `reset` clears it; the backlog shows it upright.
- **`!o#RRGGBB` … `!o0` [cons]** — the following text is drawn with a 1 px
  outline of that colour in place of its shadow (as `strspoutline` does for
  `strsp`), until `!o0`; `reset` clears it; the backlog shows it plain.
  `!o` is a tag only before `#` or a digit, in any text.
- **`!c` [cons]** — the rest of the text line (to its end, `@` or `\`) is
  centred in the room left on the window's line (horizontal text).
- **`!vNNN` [cons]** — waits until the voice (channel 0) has played NNN
  ms, or has ended, or a click (spaces after the number are skipped,
  as after `!w`).
- `!i` and `!c` are also read in `` ` `` half-width text; there `!w`, `!d`,
  `!z`, `!v` and `!s` are tags only before a number (`!sd` too) and `#`
  only before six hex digits; other `!` and `#` are text.

Text handling [cons]:

- Half-width characters are paired two to a cell but never with a following
  full-width character, `/`, `@`, `\` or a tag start (`#`, `!`, `(`). A lone
  half-width character at line end is kept. Same in `strsp`, `select`, backlog.
- Spaces after text on a line are drawn (English lost them before); leading
  spaces skipped.
- In a proportional font, half-width characters on a horizontal line take
  their own width (plus half the character spacing), are drawn one at a time,
  and lines break at spaces; a monospaced font keeps the grid.
- A `!` that starts no tag is drawn as text ("Hey! You!").
- A new page erases the old text over the window's text area even when the
  window picture is smaller.
- Text that would be drawn past the window's last line waits for a click
  and goes on on a new page (built in: `textgosub` isn't called there).

## Transitions (effect 99)

[cons] plugins; stock builds draw them as cross-fades.
Code: `src/cons/ONScripter_effect.cpp`, `ONScripter_cons.cpp` (grep `cons_blur` etc.) and `ONScripter_warp.cpp`.

- Retired: `cons_slide`, `cons_zoom`, `cons_flip`, `cons_cube`, `cons_melt`,
  `cons_wave` (`.dll/params`): they are `cons_warp.dll` maps now
  (`tools/cons_warp.py make slide 0,-1,0`, ...). A script still naming one
  (a build made before the maps) gets a plain cross-fade over the same
  time, with one warning per name on stderr and a `retired transition`
  trace warning; rebuild it to get the transition back.
- `"cons_blur.dll/radius[,new_only]"` — cross-fade with horizontal blur.
  MAGES InOut_Act 6.
- `"cons_rule.dll/file,cells[,curve]"` — a transition from data: `file` holds
  `cells` grey masks side by side (each stretched to the screen), evenly
  in time, mixed between cells; white shows the new picture, black the old.
  Bake a game's own wipe or dissolve into it (its kernel run on a black old
  and white new picture, a frame per cell) instead of writing a plugin.
  `curve` (an `spmotion` curve: 1-3, or 10-73 from `spcurve`) eases the time.
- `"cons_warp.dll/file,cells[,curve]"` — the same with warp cells, which
  also move pixels: each cell says, on a grid of points (1/8 of the screen
  is enough: 161x91 at 1280x720), where each pixel reads the old and the
  new picture and how much of the new one shows. Code:
  `src/cons/ONScripter_warp.cpp`; `tools/cons_warp.py` writes cells for
  slides, zooms, flips, cube turns, melts and waves. A cell is
  `gw` x `4 gh` pixels of an 8-bit RGB PNG (lossless: never re-encode it),
  four planes of `gw` x `gh` stacked top to bottom; each value `v` is 16
  bits, its high byte in planes 0 and 2, its low byte in planes 1 and 3:
  - planes 0/1: R, G the old picture's x, y; B the mask;
  - planes 2/3: R, G the new picture's x, y; plane 2's B (a byte) flags
    for the block right of and below that point: 128 reads the pictures
    bilinearly (zooms; else nearest, faster), 64 makes a position outside
    a picture read black (a diagonal push's corners; else the edge
    repeats); plane 3's B (a byte) the block's mask sharpness `s`, 0-8.
  Point `(i, j)` lies on pixel `(i w / (gw-1), j h / (gh-1))`. A pixel
  reads its picture at itself plus `(v - 32768) / 16` pixels (edges
  repeated). It shows `128 + (v - 32768) * 2^s / 16` (0-255, clamped)
  of the new picture: with `s` 0 a level (`v = 30720 + 16 * level`), with
  `s` 8 the signed distance in 1/16 pixel to an edge (positive on the new
  side), which keeps an edge sharp between points and cells. Values are
  interpolated bilinearly between points, straight between cells; `s`
  and the flags come from the nearer cell. About 2 ms a frame at
  1280x720 (4 ms on the web), zooms 12 (15).

`effect 20,99,800,"cons_warp.dll/warp\slide_0_-1_0.png,2"` slides the new picture in from the right (the map from `tools/cons_warp.py make slide 0,-1,0`).

## Web hooks

Code: `ONScripter::webPageHook`, `webStateHook`, `webPersistHook` and the other `EM_ASM` calls in `src/cons/ONScripter_cons.cpp`.
The engine calls these on `self` when defined:

| Hook | When | Arguments |
|---|---|---|
| `consText(text, skipped, boxes)` | each click wait and new page | `text` raw script bytes (`Uint8Array`); `skipped` 1 while skipping; `boxes` `Int32Array` of `offset,x,y,size` per character |
| `consChoice(text, x, y, size, pitch)` | each `select` choice drawn | raw bytes, position, font size, pitch |
| `consOverlay(event)` | drawn text leaves | `'page'`, `'hide'`, `'show'`, `'choices'`, `'enter'`, `'leave'`, `'reset'` |
| `consBacklog(text, boxes, voice)` | each backlog page shown | as `consText`, plus that page's channel-0 voice or `null` |
| `consSprites(text, boxes)` | any input wait | visible `strsp` text and boxes |
| `consVoice(channel, file)` | `dwave*`/`wave*` starts | channel, file name as UTF-8 (`voice\v300002.ogg`) |
| `consPosition(offset, total, label, read)` | before `consText` | script byte offset of the line, script length, label (no `*`), read 1/0/-1 (no `kidokuskip`) |
| `consMode(auto, skip)` | auto/skip start or stop | 0/1 each |
| `consWindow(x, y, w, h, shown)` | text window moves, resizes, shows, hides | rectangle, 1/0 |

`--no-text-overlay` (on the command line or in `ons_args`), or `consNoTextOverlay: 1`
on the module object the page passes to `cons()`, stops `consText`, `consChoice`,
`consSprites`, `consBacklog` and `consOverlay`: the page draws no text overlay.

The page calls these on the module (`cons()`'s object); acted on at the next
wait (at once at a click wait):

- `_consSetAuto(on)`, `_consSetSkip(on)` — as the `a`/`s` keys.
- `_consSetRuby(on)` — 0 stops drawing ruby (space kept; hooks still get
  `(base/reading)`).
- `_consSetTextSpeed(ms)` — ms per character, 0 instant, -1 the game's.

## Save files

[cons] saves end with a `CONS` block (tag, version, then state) after the
NScripter data; saves without it load as before. It holds every [cons] state
marked "saved" above, plus what NScripter data loses: `strsp` sprites (text,
font, colours, also in the `pushscreen` stack) and looping `dwave` channels
(restarted on load). Its first part (version 26) holds the named variables
(`defvar`, `defstr`, `defarr`; by name) and the `spnew` handles with their
pictures; the NScripter data holds sprites 0-999. Only the current version's
block is read: an older save loads its NScripter data alone.
Code: `CONS_SAVE_MAGIC` in `src/cons/ONScripter_cons.cpp`.

## Engine behaviour [cons]

No command involved:

- **Cut sounds** fade 10 ms on a spare channel (8 reserved) instead of
  clicking. Streamed `bgm`/`mp3` music still stops at once.
- **Load** no longer cuts the looping sounds it restarts.
- **Opus** decoded by the engine in every build (`.ogg` names fine,
  `LOOPSTART`/`LOOPEND`/`LOOPLENGTH` in 48 kHz samples). `cmake/cons_opus.cmake`;
  `-DCONS_OPUS=OFF` drops it. Music decoded whole (~11 MB/min).
- **WebP** in the web build via libwebp (`script/cross_web.sh`), elsewhere
  through SDL_image. An opaque WebP (no alpha channel) shown with `:a` is
  opaque, as a PNG is, not split into picture and right-half mask.
- **Movies on Linux/macOS** through ffmpeg; skipped with a message without it.
  Files must be loose.
- **Japanese loose file names** found on Linux, macOS and web (Shift-JIS
  names looked up as UTF-8 first).
- **Web**: `ons_args` fetched in lazily loaded games; `gloval.sav` written
  at waits when changed (≤ every 2 s), `kidoku.dat` every 30 s, then synced.
- **Screen updates** go up once before the engine waits (a timer tick, a
  wait, a click wait), once per effect frame, and at `blt`/`draw`, not at
  each drawing: with vsync a timer tick with several animations no longer
  takes a refresh each. While skipping or in `--auto-play` without
  `--real-time`, at most every 50 ms.
- **Player settings** (`consset`) live in memory only: the game sets them again at startup.
