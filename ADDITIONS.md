# CONscripter additions

When a game converted by
[conscripter-convert](https://github.com/blankiekun/conscripter-convert)
needs something the NScripter command set can't do, it is added here.
Nothing here is opt-in. A new behaviour (a button's pressed picture,
a setting) applies to every script, with no command or flag to turn it
on, and so do fixes to behaviour that is wrong (text drawn in the
wrong place, characters dropped, a click in the sound: half-width
characters, cut sounds, spaces, vertical marks, music volume). New
commands and tags take effect where a script uses them. The code
is in `src/onsyuri/ONScripter_cons.cpp` where possible.

- **`spfilter num,kind[,blend[,strength[,p1,p2,p3]]]`**: colour filters
  on an `lsp` sprite, applied when its image is loaded (no cost per
  frame). `kind` makes a colour for each pixel: 1 the colour `p1,p2,p3`
  (r, g, b); 2 the pixel in greyscale; 3 its negative; 4 the pixel
  plus `p1` (-255..255); 5 the pixel in greyscale plus the offsets
  `p1,p2,p3` (a monotone such as sepia: `28,-8,-38`); 6 a blur, the
  mean of the pixels up to `p1`,`p2` pixels away (x, y); 7 the pixel with
  its hue turned by `p1`/256 of a turn and its saturation moved by `p2`
  (-256 grey, 0 kept, 256 full); 8 the pixel with its lightness inverted
  (a negative that keeps the hue). `blend` combines it with the pixel: 0 copy
  (default), 1 multiply, 2 add, 3 screen. `strength` 0-256 (default 256)
  is how much of the result replaces the pixel. Up to 4 filters stack in
  order; `spfilter num,0` removes them. The filters stay when `lsp`
  changes the sprite's image, are removed by `csp`, are ignored by a
  sprite without an image, and are kept in save files.
  `spfilter 10,1,1,256,148,152,200` tints sprite 10 for a night scene;
  `spfilter 10,1,0,256,0,0,0` makes it a black silhouette.
- **`spzoom num,zoom_x[,zoom_y]`**: draws `lsp` sprite `num` zoomed by
  `zoom_x` and `zoom_y` percent (100 = its size; `zoom_y` defaults to
  `zoom_x`; negative flips it) about the centre of its place, which `amsp`
  and `msp` move as usual. Unlike `lsp2`, the sprite keeps its place among
  the other sprites (`lsp2` sprites are all drawn over them). The zoom
  stays when `lsp` changes the image, is reset by `csp` and is kept in
  save files and by `pushscreen`. Its buttons keep their unzoomed place.
  `spzoom` redraws the whole screen at the next `print`, so call it again
  after moving a zoomed sprite: `amsp 10,x,y:spzoom 10,150:print 1` (a
  camera zoom on a background, step by step). An animated sprite
  (`:a/n,…`) zoomed this way shows each of its cells in turn (it showed
  the first cell, the others clipped away) and redraws where it is drawn
  as they change, also when that is partly off the screen (it used to
  redraw only its unzoomed place, and a screen update of an area partly
  off the screen was dropped).
- **`spmotion num[,loop,t1,x1,y1,t2,x2,y2,...]`**: moves `lsp` sprite
  `num` by itself along offsets from its place: `x,y` pixels at `t` ms
  from now, straight from key to key, stepped about every 16 ms by the
  timer (during text and click waits too, like the sprite animations).
  `loop` 1 starts again after the last key (give the last key the first
  one's offset for a smooth loop); `loop` 0 ends at the last key and the
  sprite is back at its place. `spmotion num` stops it there. Up to 32
  keys. `amsp`, `msp` and `spzoom` act on the place as usual; the motion
  stays when `lsp` changes the image, is removed by `csp` and is kept in
  save files (restarted on load). `spmotion 10,1,0,0,0,250,0,-12,500,0,0`
  bobs sprite 10 twice a second; a one-shot list of shrinking offsets
  shakes it. While a sprite is zoomed or moving, each refresh redraws the
  whole screen, and `csp` redraws where it was drawn. `loop` 2 runs once
  and leaves the sprite at the last key: its offset is added to the
  sprite's place (as `msp` would), its zoom to the `spzoom` and its
  alpha to the sprite's (the angle goes back to 0). Another `spmotion`
  or `spmotion2` on the sprite (also one that stops it), `lsp`, `msp` or
  `amsp` first ends such a motion at its last key, so a skipped scene
  puts the sprite where it would be.
- **`spmotion2 num[,loop,t1,x1,y1,zoom1,angle1,alpha1,...]`**: `spmotion`
  with a zoom (percent, 100 = its size, on top of `spzoom`'s) and an
  angle (degrees, counterclockwise, as `lsp2`'s), both about the centre
  of the sprite's place, and an alpha (0-256 of the sprite's own) in each
  key, straight from key to key like the offsets.
  `spmotion2 10,1,0,0,0,100,0,256,1000,0,0,100,360,256` spins sprite 10
  once a second; `spmotion2 10,2,0,0,0,100,0,256,500,300,0,100,0,0`
  slides it 300 pixels right as it fades out, and it stays gone (the
  next `lsp` on it shows it again).
- **`spfade num,ms[,from]`**: fades `lsp` sprite `num` in by itself: it
  is drawn at `from`/256 (0-256, default 0) of its alpha, rising to its
  alpha over `ms`, stepped about every 16 ms by the timer while the script
  goes on (text and click waits too). `spfade num,0` ends it at once. The
  fade stays when `lsp` changes the image and is removed by `csp`; a save
  keeps the sprite's own alpha, so a load shows it at the end of its fade.
  `lsp 60,":a;noise.png",0,0,200:spfade 60,2000:print 1` brings static up
  to 200 over two seconds. A sprite loaded without an alpha fades too
  (it was drawn at nothing until the fade's end: fixed), and the sprites
  that fade or move (`spmotion`) at one step are drawn in one screen
  update, so a bar's buttons faded together come up together.
- **`spalpha num[,loop,t1,a1,t2,a2,...]`**: draws `lsp` sprite `num` at
  a changing part of its alpha by itself: `a`/256 (0-256) at `t` ms from
  now, straight from key to key, stepped about every 16 ms by the timer
  (text and click waits too). `loop` 1 starts again after the last key;
  `loop` 0 stops there and the sprite keeps that alpha. `spalpha num`
  stops it and draws the sprite at its own alpha. Up to 32 keys. The keys
  stay when `lsp` changes the image, are removed by `csp` and are kept in
  save files (restarted). `spalpha 20,1,0,0,600,256,1100,256,1700,0,2200,0`
  makes sprite 20 glow in and out, as a fire does.
- **`spz num[,over]`**: draws `lsp` sprite `num` right over sprite
  `over`, as if its number were between `over` and `over - 1`, instead of
  at its own number; `spz num` puts it back. For a picture that goes in
  front of some of the sprites numbered below it: `spz 70,61` draws
  sprite 70 over 61-69 and under 60. It stays when `lsp` changes the
  image, is removed by `csp` and is kept in save files and by
  `pushscreen`.
- **`screenfilter slot,kind[,blend[,strength[,p1,p2,p3]]]`**: the same
  filters on the whole picture: the background and the sprites behind the
  text window (numbers above `humanz`), where `monocro` applies; not the
  text window or the sprites in front of it. Slots 1-4 apply in order;
  kind 0 clears a slot and `screenfilter 0` clears them all. It doesn't
  draw: follow it with `print` (an effect fades it in). Kept in save
  files and by `pushscreen`, cleared by `reset`.
  `screenfilter 1,4,0,256,-200:print 10` darkens the scene for night.
- **`screenflash opacity[,"#RRGGBB"]`**: adds the colour (white when
  not given) at `opacity`/256 to every pixel of the screen, over
  everything: the picture, the text window, the sprites in front of it
  and the buttons (`lsp2add` stays under the text window). It doesn't
  draw: follow it with `print`. `screenflash 0` takes it away, as `reset`
  does; not kept in save files.
  `screenflash 128:print 1:delay 100:screenflash 0:print 1` (lightning).
- **`screenshake slot,time,interval,mode,speed_x,speed_y,x0,y0,x1,y1[,end]`**
  and **`screenripple slot,"map",time,interval,opacity0,opacity1,amp0,amp1,angle,angle_step,phase_step[,blend]`**:
  effects that move by themselves on the same picture as `screenfilter`
  (applied after the filters), while the script and the text go on. Every
  `interval` ms they take a step; over `time` ms their sizes go from the
  first to the second value, then stay. `screenshake` moves the picture by
  up to `x`,`y` pixels: mode 0 swings each axis on a sine wave advancing
  `speed`/65536 of a turn per ms, mode 1 jumps along a fixed 7-point path
  (the next point when `speed_x` × ms reaches 65536). `screenripple` takes
  each pixel from `amp` × sin(2π(grey/256 + phase/1000)) pixels away in
  direction `angle` (tenths of a degree, 0 = right, 900 = down), `grey`
  being the pixel's level in the greyscale image `map`, and lays the
  result over the picture at `opacity` (0-256), or with a 10th number
  `blend` 1 multiplies the picture by it at that opacity; each step turns the angle
  by `angle_step` and moves the phase by `phase_step`.
  **`screenpulse slot,time,interval,zoom_x,zoom_y,opacity,colour,tint_opacity,tint`**
  keeps a copy of the picture when it first shows, tinted (`tint` 0 none,
  1 multiplied by `colour` 0xRRGGBB, 2 `colour` laid over it, at
  `tint_opacity` 0-256), and lays it over the live picture scaled by
  1 + f × `zoom`/1000 around the centre at f × `opacity` (0-256), f going
  0 → 1 → 0 as sin(π × elapsed/`time`); after `time` ms nothing shows
  until the slot is cleared. Slots 1-8 are shared;
  each command with only a slot clears it, slot 0 clears them all. Like
  `screenfilter` they don't draw: follow them with `print`. Kept in save
  files (restarted when loaded), cleared by `reset`. While one runs, every
  update redraws the whole screen.
  `screenshake 1,1000,100,0,800,800,0,0,30,30` shakes harder and harder for
  a second, then keeps shaking until `screenshake 1`.
  With an 11th number `end` (> 0) a shake stops by itself after `end` ms
  (its slot clears): `screenshake 1,500,16,0,1300,1700,10,10,10,10,500`
  shakes for half a second, as a game's timed shake does.
  **`screenzoom slot,time,interval,zoom,x,y,zoom_time,fade_in,ease,power`**
  (same slots): a frozen copy of that picture, taken when it first shows,
  laid over the live one and zoomed: over `zoom_time` ms its scale goes
  from 1 to `zoom`/1000 and its centre from the screen's centre to `x,y`,
  eased by `ease` (0 straight, 1 in, 2 out, 3 in and out, with `power`/1000,
  1000 when 0: the MAGES engine's curves); its opacity rises from 0 to 256
  over `fade_in` ms. It then stays until the slot is cleared (clear it
  inside a transition to fade it out); `time` is the longer of the two
  times. With `zoom` 1000 it is a freeze frame. For the MAGES
  画面キャプチャースプライト (an impact zooming in on part of the picture).
- **`windowtrans opacity`**: draws the text window's picture (setwindow's
  image) at `opacity` (0-255), on top of the picture's own alpha, for a
  "window opacity" setting. Kept across `setwindow` and loading a save
  (not saved: set it again from the game's settings at startup); `reset`
  puts it back to 255. `windowtrans 128`.
- **`dwavefadein ms`** / **`dwavefadeout ch,ms`**: fade a `dwave`
  channel in or out. `dwavefadein` makes the next `dwave`, `dwaveloop`,
  `dwaveplay` or `dwaveplayloop` fade its sound in over `ms`
  milliseconds (once; 0 = at once). `dwavefadeout` fades channel `ch`'s
  sound out over `ms`, then stops it, while the script goes on; a looping
  sound fading out isn't kept in a save, and 0 ms (or a sound not heard
  yet, at the start of a fade-in) stops it at once, as `dwavestop`.
  `dwavefadein 2000:dwaveloop 1,"se\rain.ogg"` … `dwavefadeout 1,1500`.
- **`automode_time ms,char_ms`**: with a second number, auto mode waits
  `ms` plus `char_ms` for each character of the page, as games with a
  "per page" and a "per character" auto wait do. `automode_time 1500,30`
  waits 1.8 s after a page of 10 characters. With one number it works
  as in NScripter (a negative `ms` is a time per character) and the time
  per character is 0. Only the first number is kept in `envdata`.
- **`chvolfade ch,vol[,ms]`** / **`bgmvolfade vol[,ms]`**: the script's
  own volume for `dwave` channel `ch`'s sound, or for the music (`bgm`,
  `mp3`), in percent of the player's setting (sound effects or voice
  for a channel, music for the BGM), changed over `ms` milliseconds
  (default 0, at once) while the script goes on, linearly from where it
  is. `chvol` and `bgmvol` set the setting itself; these go on top of
  it, so the setting still applies. The sound keeps playing at 0. The
  volume stays with the channel, or the music, for the sounds played
  next, so setting it before `dwave` or `bgm` starts a sound quieter;
  `reset` puts it back to 100. `bgmfadein` and `bgmfadeout` fade to and
  from the script's volume, and `dwavefadein` and `dwavefadeout` work
  with it. Kept in save files (where a ramp goes).
  `chvolfade 3,30:dwaveloop 3,"se\rain.ogg"` … `chvolfade 3,100,2000`;
  `bgmvolfade 20,500` ducks the music, `bgmvolfade 100,1000` brings it
  back.
- **`vstrsp num,"text",x,y,columns,rows,w,h,pitch_x,pitch_y,bold,shadow[,colours]`**:
  `strsp` with the text in columns, top to bottom, the first at the right
  (as `tateyoko 1` lays out the text window): `columns` columns of `rows`
  characters, `pitch_x` pixels between columns and `pitch_y` between
  characters; brackets, dashes and long vowel marks are turned. Kept in
  saves and by `pushscreen` like `strsp`. A save screen's chapter title in
  its side panel: `vstrsp 20,"―　プロローグ　―",955,41,1,20,20,20,0,0,0,0,"#000000"`.
- **`getkey`**: the next `btnwait` or `textbtnwait` also ends when a
  letter key is pressed, returning -1000 minus the letter's upper-case
  code (A is -1065, M -1077, Z -1090), before the key's own meaning
  (S skip, A auto, H/J/K/L cursor). It lasts for one wait, like
  `getfunction`. A game whose keys open its menus
  (`getkey:textbtnwait %9` … `if %9 == -1077 gosub *menu`).
- **`getplaytime %var`** / **`setplaytime seconds`**: the play time, the
  seconds the game has run since it started, kept in each save (as it
  stands when `savegame` writes it, not when its page began) and put
  back by `loadgame`; it goes on counting from there. `reset` sets it to
  0, as does loading a save made before it was kept. A save screen
  showing each save's play time (`getplaytime %1` before `savegame2`,
  into the label); a new game after the title starts at
  `setplaytime 0`.
- **`logrows %n,columns`** / **`getlogrow $text,$voice,row`**: the text
  history as rows, for a backlog scrolled a line at a time. `logrows`
  lays out every page kept, oldest first: each of its lines, cut every
  `columns` full-width characters (half-width ones count half), ruby
  shown as its base text, and puts the number of rows in `%n`.
  `getlogrow` gives row `row` (0 = the oldest; "" past the last) and in
  `$voice` the voice its page played on channel 0, as the `dwave` named
  it, on the page's first row ("" on the others). `logrows %1,26`, then
  a `strsp` for each row shown; clicking the voice icon plays
  `dwave 0,$voice`.
- **Player settings** a game's settings screen sets, kept in memory only
  (the game keeps its settings in global variables and sets them again
  at startup); without them the engine works as before:
  - **`soundmute bgm,se,voice`**: 1 mutes the music, the sound effects
    (every `dwave` channel but 0), the voice (channel 0), whatever their
    volume; 0 lets them be heard. Muted sounds play on unheard, so music
    comes back where it has got to. Auto mode doesn't wait for a muted
    voice.
  - **`clickvoicestop on`**: 1 = a click that ends a text wait (`\`, `@`,
    `textbtnwait`) stops the voice; 0 (default) = it plays on.
  - **`autovoicewait on`**: 1 (default) = auto mode waits for the voice
    to end; 0 = it reads on after its own time and stops the voice.
  - **`selectkeep on`**: 1 = auto mode and skip go on after a `select`'s
    option is chosen; 0 (default) = a `select` ends them.
  - **`moviestop mode`**: whether a click stops a movie (`mpegplay`,
    `movie`): 1 always, 0 never, -1 (default) as the script's flag says.
  - **`gamespeed percent`** (10-10000, default 100): script times run at
    that percent of real speed: `wait`, `delay`, `waittimer`, `gettimer`,
    effects (`print`), `spfade`, `spmotion`, and `screenshake`'s time and
    end. Text speed, auto mode's wait and sounds keep theirs.
    `gamespeed 200` plays a 1 s fade in 0.5 s (Nitroplus's ゲーム全般の処理速度;
    a game's own menus set it back to 100 while they are open).
  - **`effectoff on`**: 1 = every `print` effect (and those of `bg`,
    `ld`, `cl`) is drawn at once, as effect 1 (a game's "use screen
    effects" switch); 0 (default) = as the script asks.
  - **`textshadow on`**: 0 = the message text has no shadow, whatever
    `setwindow` says (a "text decoration" switch); 1 (default) =
    `setwindow`'s. Other text (`strsp`, menus) keeps its own.
- **`spbtn num,no,pressed_cell`**: a third argument to `spbtn` names the
  cell sprite `num` shows while the left mouse button is held on it (the
  hover cell is 1, as before). It goes back to the hover cell when the
  button is let go, to cell 0 when the pointer leaves, and shows the
  pressed cell again if the pointer comes back with the button still
  held. Without the argument, buttons work as before. (Nitroplus menus
  draw a pressed picture for every button.)
- **`mpegplay "file",click,1`**: a third argument 1 leaves the music
  playing through the movie (`mpegplay` stops it otherwise). For a movie
  with no sound of its own cut into a scene, such as a battle cut whose
  sound effect the script plays with `dwave` beside it. The native
  builds play a movie's sound in place of the music, so give it a movie
  without sound. (Nitroplus's `CreateMovie` cuts play over the scene's
  music.)
- **`selectvoice open,over,select` anywhere**: it was refused outside the
  define section; now a script can set the sounds for one screen (the
  `over` sound plays when the pointer enters any button) and clear them
  after it with `selectvoice "","",""`. An empty name is no sound (it
  used to try to play a file named "").
- **`pushscreen first,last`** / **`popscreen`**: save the background,
  sprites `first`..`last` (image, position, visibility, cell, filters;
  a `strsp` sprite's text, font and colours),
  the screen filters and the BGM on a stack (32 deep), and put the newest back. Neither
  draws: follow `popscreen` with `print`. Unchanged sprites aren't
  reloaded and a BGM that is still playing keeps playing. For a scene run
  as a subroutine that may change the whole screen:
  `pushscreen 0,999:gosub *scene:popscreen:print 10`. The stack is kept
  in save files (so a game saved inside the scene still returns
  correctly) and emptied by `reset`.
- **`delsavegame no[,file]`**: deletes save file `no` and, when given,
  `file`, its thumbnail as named to `savescreenshot`, from the folder
  `savescreenshot` writes to (the save directory, or the game folder when
  none is set, the same folder as the saves). `savefileexist` then reports
  the slot empty. In the web build the save directory is synced
  afterwards. `delsavegame 3,"savethumb3.bmp"`.
- **`copysavegame from,to`**: copies save file `from` to save file `to`
  as it is (with its `savegame2` text); when `from` doesn't exist,
  deletes `to` instead. For saves a script keeps beside a slot, such as
  the pages of nested scenes that a save from the menu takes along.
  The web build syncs the save directory afterwards.
  `copysavegame 901,1031` … `copysavegame 1031,901`.
- **Slide and zoom transitions**, as effect 99 plugins (stock builds
  draw an unknown plugin as a cross-fade):
  - `effect num,99,ms,"cons_slide.dll/layer,dx,dy"`: a picture moves one
    screen width or height in direction `dx`,`dy` (each -1, 0 or 1).
    `layer` 0: the new picture slides in over the old one; 1: the old
    picture slides out, uncovering the new one; 2: both move together
    (like effects 11-14, also diagonally).
  - `effect num,99,ms,"cons_zoom.dll/mode,scale[,curve]"`: zooms about the
    screen centre, up to `scale` percent (5100 = 51 times), the zoom
    eased by the progress to the power `curve` (default 1). `mode` 0: the
    new picture shrinks into place while the old one fades out over it;
    1: the old picture grows and fades out over the new one; 2: zooms
    into the old picture for the first half, then out of the new one;
    3: as 0, while the old picture also grows as it fades.
  - `effect num,99,ms,"cons_flip.dll/axis,layer"`: a picture turns like a
    card seen edge-on about the screen centre (axis 0 squeezes the width,
    1 the height): layer 0, the new picture grows from a line over the old
    one (its size sin(p · 90°)); layer 1, the old picture shrinks to a line
    over the new one (cos(p · 90°)).
  - `effect num,99,ms,"cons_cube.dll/dx,dy"`: the pictures are two faces
    of a cube as wide (or high) as the screen turning a quarter turn,
    seen straight on: the new face comes in moving in direction `dx`,`dy`
    (one of them -1 or 1), each picture squeezed to its face's width
    (sin and cos of the angle), the parts beyond the screen cut off.
  - `effect num,99,ms,"cons_melt.dll/stretch[,seed]"`: the old picture
    melts down off the screen, uncovering the new one. Its top edge is a
    smooth random curve; each column starts falling after 0.2-0.3 of the
    time and falls with constant acceleration, the last one leaving at
    the end. A falling column is stretched by 1 + `stretch`/100 × the
    fraction of the height it has fallen. `seed` (non-zero) fixes the
    curve; by default it is new each time.
  - `effect num,99,ms,"cons_wave.dll/direction,amplitude,waves"`: a
    cross-fade with the pictures' rows waving sideways (`direction` 0 or
    2; 1 or 3 the columns up and down; 2 and 3 the other way): each row
    shifted by `amplitude`/1000 of the width × sin(π p) × sin(2π(`waves`/10
    × y / height + p)), p the progress. For the MAGES engine's InOut_Act 1.
  - `effect num,99,ms,"cons_blur.dll/radius[,new_only]"`: a cross-fade
    with the pictures blurred sideways by a box of up to `radius` pixels
    either side, the most halfway and none at the ends; `new_only` 1
    blurs only the new picture. For the MAGES engine's InOut_Act 6.
  `effect 20,99,800,"cons_slide.dll/0,-1,0"` slides the new picture in from
  the right.
- **Text size tag `!zNNN`**: the text that follows is drawn at `NNN`
  percent of the text window's font size, until the next `!z` (`!z100` or
  `!z` goes back; `reset` clears it). Each character takes its scaled
  width on the line and sits on the line's baseline; the line pitch stays.
  The window's column count still counts characters, so a script with
  larger text breaks its lines itself. The backlog shows the text at the
  normal size. `!z150「ぎゃーっ！！」!z100` (other engines print the tag).
- **Half-width characters in text** are still drawn two to a cell, but
  not paired with a following full-width character, `/`, `@`, `\` or the
  start of a tag (`#` colour, `!` wait or size, `(` ruby; after a kept
  space, `Ghost #FFFFFF` showed the colour tag).
  Stock ONScripter paired them, so after an odd number of half-width
  characters a `\` or `@` was shown instead of waiting, `あ1/2い\`
  ran on instead of giving the "'new line' must follow '/'" script error,
  ruby on a half-width body (`(a/えい)`) was shown as text, and the
  Shift-JIS bytes after it came out garbled. A single half-width character
  at the end of a line is no longer dropped. Sprite text (`strsp`,
  `select` choices) and the page redrawn for the backlog follow the same
  rule: `1あ` no longer comes out garbled there.
- **Spaces inside text are kept.** ONScripter draws half-width
  characters two at a time and skipped the spaces before each pair, so
  English lost every space that started a pair: “Tell me the tales” came
  out “Tellmethe tales”. A space that follows other text on the line is
  now drawn; spaces before the text itself are still skipped. The same
  holds for sprite text (`strsp`), which dropped every space unless the
  text started with a backquote.
- **Marks turned for vertical text are centred in their cell.** In
  `tateyoko 1`, ONScripter turns brackets, dashes and similar marks
  (《》「」『』（）【】〈〉 ― ～ …) a quarter turn, but placed the turned
  glyph as if it weren't turned: 《 sat high and left of the column, 「
  left, ― right. The glyph now turns about the centre of its cell, so ︽
  sits on the text it opens and ﹁ at the top right, as in print.
- **Cut sounds fade out instead of popping.** A sound stopped while it
  plays (a voice cut by the next one, `dwavestop`, `wavestop`, `stop`,
  `loopbgmstop`, a load) used to stop mid-wave, a step in the output
  heard as a click. Its next 10 ms now fade out on a channel of their own
  (8 are kept for this, after the game's), so the next sound still starts
  at once. Music played with `bgm`/`mp3` as a stream still stops at once:
  SDL_mixer gives no way to read it ahead.
- **Music played with `bgm` or `mp3` starts at the music setting.**
  The setting (0-100) was given to SDL_mixer as is, on its 0-128 scale,
  so music at 100 % played at 78 % (−2 dB) until something changed its
  volume (a `bgmfadein`, `bgmvol`); MIDI music too.
- **A `bgmstop` fading out during a `bgmfadein` no longer hangs the next
  `bgm`.** The fade-out took the fade-in's timer slot without stopping
  it, and the fade-in, ending, removed the fade-out's timer: the music
  played on, its name already cleared, and the next `bgm` or `bgmstop`
  started a fade-out it waited on forever (a name it didn't know counted
  as not Ogg, and only such a fade's end breaks the wait). A fade-out
  now stops a fade-in first, and music whose name is gone is not waited
  for. Seen with 俺たちに翼はない's `bgmfadein 1000` … `bgmfadeout 16:bgmstop`
  a few clicks apart.
- **WebP images in the web build**: emscripten's SDL_image port has no
  WebP decoder, so the web build links libwebp (`script/cross_web.sh`
  builds it) and decodes WebP itself, lossy and lossless, with alpha. The
  other builds read WebP through SDL_image when it was built with
  libwebp (vcpkg's Windows build and most Linux packages are).
- **`ons_args` in a lazily loaded web game**: the web build read it with
  a plain `fopen`, which finds nothing until the page has fetched the
  file, so a game packed with `--lazyload` lost its options: without
  `--enc:sjis` its Shift-JIS script was read as GBK (the text garbled).
  It is now fetched like the game's other files.
- **Global variables and read marks kept when the web page closes**:
  ONScripter writes `gloval.sav` and `kidoku.dat` when the game quits or
  saves, but closing or reloading the tab is no quit, so the settings,
  the read marks and the unlocks of a web game were lost unless the
  player saved afterwards. The web build now writes the global variables
  at a click wait when they changed (at most every 2 seconds), and the
  read marks every 30 seconds or with them, then syncs the save directory.
- **Opus sounds in every build**: SDL_mixer plays Opus only when it was
  built with opusfile, and none of the builds' SDL_mixer was (the
  prebuilt Linux and Windows ports, Android's, emscripten's). The engine
  now decodes Ogg Opus itself, for `bgm`, `dwave` and every other sound
  command, and hands SDL_mixer the sound as WAV (48 kHz, 16-bit stereo).
  The file is recognised by its header, so it can keep an `.ogg` name. A
  `LOOPSTART` and `LOOPEND` (or `LOOPLENGTH`) tag, in 48 kHz samples as in
  SDL_mixer, loops music as it does Vorbis. `cmake/cons_opus.cmake`
  downloads libogg 1.3.5, libopus 1.5.2 and opusfile 0.12 when the build
  is configured and compiles them in (`-DCONS_OPUS=OFF` leaves them out).
  Music is decoded whole (about 11 MB a minute), so long music is better
  left as Vorbis. libopus is compiled without its stack protector and
  `_FORTIFY_SOURCE` in the MinGW builds: with them 0.21.0's Windows
  `.exe` needed `libssp-0.dll` and didn't start without it.
- **Movies on Linux and macOS**: `mpegplay` and `movie` printed
  "mpegplay command is disabled" there. They now play in the game window
  through the player's ffmpeg (`CONS_FFMPEG`, else `ffmpeg` on `PATH`,
  `/opt/homebrew/bin`, `/usr/local/bin`): any format ffmpeg reads, scaled
  to the screen and letterboxed, the sound at the music setting and the
  pictures timed by it. A click, Return, Space or Escape skips the movie
  when the script allows (`mpegplay "f",1`, `movie "f",click`); `movie`'s
  `loop` repeats it. Without ffmpeg the movie is skipped with a message
  (and a trace warning). The file must be loose, as on the other builds.
- **`select` ignores sprite buttons that aren't its options**: a button
  the script left defined with `spbtn` (a menu bar's) could be clicked
  during a `select`, whose wait took its number as the option: no label
  was found and the script stopped with an error. Such clicks are now
  ignored.
- **A new page erases the last one's text where the window picture
  doesn't reach**: the screen was refreshed only over the window picture,
  so with a picture smaller than the text area (a 1×1 transparent one,
  text over the scene) the old page stayed under the new one. The window
  now counts as its picture and the area its text can take.
- **Loose files with Japanese names are found** on Linux, macOS and in
  the web build: the script's Shift-JIS name is looked for as UTF-8, the
  file systems' encoding, before the old ways (EUC-JP on Linux, the raw
  bytes on macOS). A loose game no longer needs ASCII file names.
- **Text hooks (web build)**: the engine calls these functions on the
  page's global scope (`self`) when they are defined, so the page can lay
  selectable text over the game:
  - `onsyuriText(text, skipped, boxes)` at each click wait and new page:
    `text` is the page shown so far as raw script bytes (a `Uint8Array`
    in the script's encoding), `skipped` is 1 while the reader skips
    text, and `boxes` is an `Int32Array` of `offset, x, y, size` per
    character drawn (byte offset in `text`; position and font size in
    screen pixels).
  - `onsyuriChoice(text, x, y, size, pitch)` for each `select` choice as
    it is drawn: its raw bytes, position, font size and character pitch
    in screen pixels.
  - `onsyuriOverlay(event)` when drawn text leaves the screen: `'page'` (a
    new page), `'hide'` and `'show'` (the text window hidden and shown
    again with its page), `'choices'` (the buttons cleared), `'enter'` (the
    system menu or backlog opened), `'leave'` (back to the page) or
    `'reset'` (a game loaded from the menu).
  - `onsyuriBacklog(text, boxes, voice)` for each backlog page shown, like
    `onsyuriText` (the current page's boxes are kept for when it returns),
    with the last voice `dwave` played on channel 0 for that page (as
    for `onsyuriVoice`), or `null`.
  - `onsyuriSprites(text, boxes)` whenever the game waits for input: the
    visible `strsp` sprites' text and character boxes, like `onsyuriText`
    (all sprites in one `text`), for menus and backlogs a script draws itself.
  - `onsyuriVoice(channel, file)` when `dwave`, `dwaveloop`, `dwaveplay`,
    `wave` or `waveloop` starts a sound: the channel (0 is the usual
    voice channel) and the file as the script names it, as a UTF-8
    string (`voice\v300002.ogg`), so the page can keep the voice of the
    page being read. It is called before the next `onsyuriText`.
  - `onsyuriPosition(offset, total, label, read)` at each click wait and
    new page, just before `onsyuriText`: the byte offset in the script of
    the line the text is on, the script's length in bytes (for a
    percentage), the label it is in (UTF-8, without `*`), and `read`: 1 if
    the text was read before, 0 if not, -1 when the game doesn't keep track
    (it hasn't turned on `kidokuskip`). Several click waits on one line
    share an offset.
  - `onsyuriMode(auto, skip)` when auto mode or skip mode starts or stops
    (each 0 or 1), whatever started it.
  - `onsyuriWindow(x, y, w, h, shown)` when the text window moves, changes
    size, or is shown or hidden: its rectangle in screen pixels (the window
    picture or colour, as `setwindow` places it) and 1 or 0.

  The page can also call these on the module (the object `onsyuri()` was
  given), for example `module._onsyuriSetAuto(0)`. The engine acts on them
  the next time it waits (at once while it waits for a click):
  - `_onsyuriSetAuto(on)`, `_onsyuriSetSkip(on)`: start (1) or stop (0)
    auto or skip mode, as the `a` and `s` keys do; starting one at a click
    wait goes on at once, as the keys do, and stopping auto mode at a click
    wait makes it wait for a click.
  - `_onsyuriSetRuby(on)`: 0 stops drawing ruby (furigana) from the next
    text drawn, leaving the space for it; the text hooks still have the
    `(base/reading)` text. 1 draws it again.
  - `_onsyuriSetTextSpeed(ms)`: the milliseconds per character, 0 for the
    whole text at once, -1 for the game's own speed again.
- **Save files** end with an extra block (`CONS`, a version, then the
  state above) after the NScripter data. Saves without it load as before.
  The block also keeps what the NScripter data loses: `strsp` sprites
  (their text, font and colours; the NScripter data has only their place),
  also those in the `pushscreen` stack,
  and sounds looping on `dwave` channels (`dwaveloop`, `dwaveplayloop`),
  which a load starts again.
  A load no longer cuts the looping sounds it starts again (`waveloop`,
  `loopbgm`, looping `dwave`): stopping the old ones used to queue events
  that freed the new ones at once.
