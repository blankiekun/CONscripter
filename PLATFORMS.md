# Installing CONscripter on each platform

Per-device steps. The summary and the quick start are in [README.md](README.md).

## Linux and macOS

Download a static binary from the
[releases](https://github.com/blankiekun/CONscripter/releases) or build from
source (see [BUILDING.md](BUILDING.md)).

![CONscripter on Linux](screenshot/onsyuri_mo2_linuxtest.jpg)

On macOS, download `conscripter_<version>_universal_darwin` (one binary for
Apple silicon and Intel Macs, macOS 10.13 or later). It is not signed, so
macOS refuses it at first: make it runnable and clear the quarantine flag
once, then start it in the game folder:

```shell
chmod +x conscripter_*_universal_darwin
xattr -d com.apple.quarantine conscripter_*_universal_darwin
```

A game wrapped in a `.app` (as conscripter-convert's GUI makes it) opens
with right-click > Open the first time (or the same `xattr -dr` on the
`.app`).

## Web

To play without hosting anything, use the [web player](https://blankiekun.github.io/?app=cons#cons). To host the engine yourself:

Serve these files from a web server. <kbd>F10</kbd> stretches the game to the
page, <kbd>F11</kbd> goes fullscreen, <kbd>F9</kbd>, right click or a long
click opens the menu.

``` bash
conscripter.html
conscripter.js
conscripter.wasm
cons_index.json
[your game files]
```

![CONscripter in a phone browser](screenshot/onsyuri_mo2_webtest3.jpg)

The page loads the game listed in `cons_index.json` (set by
`<meta cons_index="cons_index.json">` in `conscripter.html`):

``` json
{
  "title": "game1",
  "gamedir": "/cons/game1",
  "savedir": "/cons_save/game1",
  "args": [],
  "lazyload": "true",
  "files":[
    {"path": "0.txt" , "url":"http://localhost:5500/asset/test/0.txt"},
    {"path": "bgm/bgm.ogg" , "url":"http://localhost:5500/asset/test/bgm/bgm.ogg"},
    {"path": "default.ttf" , "url":"http://localhost:5500/asset/test/default.ttf"}
  ]
}
```

`conscripter_index.py` generates it:

``` sh
python conscripter_index.py -i /path/to/game --urlbase https://example.com/game1/ --lazyload
```

The JavaScript interface uses CONscripter's own names (pages written for
ONScripter-Yuri need them renamed): the module factory is `cons(module)`, the
globals are `g_cons_module`, `g_cons_index` and `g_cons_filemap`, the
index is `cons_index.json` and saves default to `/cons_save/<title>` in
IndexedDB.

`mpegplay` and `movie` play a loose file (not one inside an archive) in the
page's `<video>` element, over the game and at its size; MP4 with H.264 and
AAC plays in every browser. Browsers block autoplay with sound until the page
has been clicked; the video then plays muted instead of waiting.

## Android

Install the APK. A three-finger tap or a long touch opens the menu, four fingers
skip text. Put games in `/storage/emulated/0/Android/data/io.github.blankiekun.conscripter/files`
or pick any folder through SAF. The package id is
`io.github.blankiekun.conscripter`.

![CONscripter on Android](screenshot/onsyuri_ui_androidtest2.jpg)

## RetroArch

Build `src/cons_libretro` (see its `CMakeLists.txt`) and load
`conscripter_libretro` with a game's `0.txt`, `00.txt`, `nscript.dat` or folder.
Script encoding and mouse emulation are core options.

### Switch

On a Switch, copy the release's `conscripter_libretro_libnx.nro` to
`/retroarch/cores/` on the SD card (it is RetroArch with the core built
in, as RetroArch for Switch has every core) and start it from RetroArch's
"Load Core". Its build: `.github/workflows/build_libretro.yml`. A game
picture takes about twice its decoded size while it loads (a 4612x4518
picture: 167 MB); if a big game runs out of memory, start RetroArch in
title mode (hold R while opening a game) rather than from the album.

### PS Vita

On a PS Vita with RetroArch installed, copy the release's
`conscripter_libretro_vita.self` to `ux0:/app/RETROVITA/` as
`conscripter_libretro.self` (and `conscripter_libretro.info` to its
`info/`) and load it from RetroArch's "Load Core". RetroArch gives the
core all free memory (about 300 MB), which a game with pictures as big
as above only just fits.

### Miyoo Mini

On a Miyoo Mini or Mini+ (Onion OS), copy the release's
`conscripter_libretro_miyoo.so` to `RetroArch/.retroarch/cores/` on the
SD card as `conscripter_libretro.so`. The device has 128 MB of memory:
games with pictures much over 1280x720 may not fit.

### Wii U

On a Wii U with RetroArch (Aroma), copy the release's
`conscripter_libretro_wiiu.rpx` to `sd:/retroarch/cores/` as
`conscripter_libretro.rpx` and load it from RetroArch's "Load Core".

### 3DS

On a 3DS, copy the release's `conscripter_libretro.3dsx` to `/3ds/` on
the SD card and start it from the Homebrew Launcher. A game wider than
the top screen's 400 pixels is drawn 400 wide, pictures included, to fit
the 3DS's memory. A New 3DS (124 MB for RetroArch) is needed for a long
script: Tsukihime's is 27 MB on its own.

## Kobo (KOReader)

Unzip `conscripter_<version>_kobo_koreader.zip` at the root of the Kobo's
storage (it holds `.adds/conscripter/`: the engine, its ffmpeg and
`play.sh`), copy `play.sh` next to a game's archives, then in KOReader's file
browser long-press it and pick "Execute shell script". The game is drawn
sideways: turn the Kobo clockwise.

- Tap to click, long-press for the menu. The forward page key is Enter, the
  back one Escape; hold back for 2 s to quit to KOReader.
- The power button shows a sleep screen (the game, its title, battery, time
  and play time) and suspends the Kobo; power again resumes the game. Turn
  Wi-Fi off first: the game doesn't.
- Print effects and text show at once, as each frame is an e-ink refresh
  (motions still move). There's no sound.
- `KOBO_FLIP=1` in `play.sh` turns the game the other way round. The log is
  `conscripter.log` in the game's folder.

Made for the Libra 2; FBInk knows the screen and touch panel of the others.
Known limits: the sleep screen, suspend and movies are untested on the
device; Wi-Fi stays on in a suspend; the sleep cover doesn't suspend it; a
crash leaves KOReader's screen unrestored; a game taller than 4:3 runs off a
1680x1264 screen.
