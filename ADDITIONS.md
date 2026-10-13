# CONscripter additions

When a game converted by
[conscripter-convert](https://github.com/blankiekun/conscripter-convert)
needs something the NScripter command set can't do, it is added here.
Every addition is opt-in: scripts that don't use it behave exactly as in
ONScripter-Yuri. The code is in `src/onsyuri/ONScripter_cons.cpp` where
possible.

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
- **`screenfilter slot,kind[,blend[,strength[,p1,p2,p3]]]`**: the same
  filters on the whole picture: the background and the sprites behind the
  text window (numbers above `humanz`), where `monocro` applies; not the
  text window or the sprites in front of it. Slots 1-4 apply in order;
  kind 0 clears a slot and `screenfilter 0` clears them all. It doesn't
  draw: follow it with `print` (an effect fades it in). Kept in save
  files and by `pushscreen`, cleared by `reset`.
  `screenfilter 1,4,0,256,-200:print 10` darkens the scene for night.
- **`screenshake slot,time,interval,mode,speed_x,speed_y,x0,y0,x1,y1`**
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
- **`pushscreen first,last`** / **`popscreen`**: save the background,
  sprites `first`..`last` (image, position, visibility, cell, filters),
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
  `effect 20,99,800,"cons_slide.dll/0,-1,0"` slides the new picture in from
  the right.
- **Text size tag `!zNNN`**: the text that follows is drawn at `NNN`
  percent of the text window's font size, until the next `!z` (`!z100` or
  `!z` goes back; `reset` clears it). Each character takes its scaled
  width on the line and sits on the line's baseline; the line pitch stays.
  The window's column count still counts characters, so a script with
  larger text breaks its lines itself. The backlog shows the text at the
  normal size. `!z150「ぎゃーっ！！」!z100` (other engines print the tag).
- **WebP images in the web build**: emscripten's SDL_image port has no
  WebP decoder, so the web build links libwebp (`script/cross_web.sh`
  builds it) and decodes WebP itself, lossy and lossless, with alpha. The
  other builds read WebP through SDL_image when it was built with
  libwebp (vcpkg's Windows build and most Linux packages are).
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
  - `onsyuriBacklog(text, boxes)` for each backlog page shown, like
    `onsyuriText` (the current page's boxes are kept for when it returns).
  - `onsyuriSprites(text, boxes)` whenever the game waits for input: the
    visible `strsp` sprites' text and character boxes, like `onsyuriText`
    (all sprites in one `text`), for menus and backlogs a script draws itself.
- **Save files** end with an extra block (`CONS`, a version, then the
  state above) after the NScripter data. Saves without it load as before.
