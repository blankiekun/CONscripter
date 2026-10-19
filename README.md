# CONscripter

![GitHub release](https://img.shields.io/github/v/release/blankiekun/CONscripter?color=green&label=conscripter&style=flat-square)
![web](https://img.shields.io/github/actions/workflow/status/blankiekun/CONscripter/build_web.yml?label=web(wasm)&logo=firefox&style=flat-square)
![android](https://img.shields.io/github/actions/workflow/status/blankiekun/CONscripter/build_android.yml?label=android&logo=android&style=flat-square)
![windows](https://img.shields.io/github/actions/workflow/status/blankiekun/CONscripter/build_win.yml?label=windows&logo=mingww64&style=flat-square)
![linux](https://img.shields.io/github/actions/workflow/status/blankiekun/CONscripter/build_linux.yml?label=linux&logo=linux&style=flat-square)
![mac](https://img.shields.io/github/actions/workflow/status/blankiekun/CONscripter/build_darwin.yml?label=mac&logo=apple&style=flat-square)
![kobo](https://img.shields.io/github/actions/workflow/status/blankiekun/CONscripter/build_kobo.yml?label=kobo(koreader)&logo=rakuten&style=flat-square)

An [ONScripter](https://onscripter.osdn.jp/onscripter.html) engine for playing
NScripter visual novels on the web, Windows, Linux, macOS, Android,
RetroArch and Kobo e-readers (through KOReader): a fork of [ONScripter-Yuri](https://github.com/YuriSizuku/OnscripterYuri)
(itself based on [ONScripter-Jh](https://github.com/jh10001/ONScripter-Jh)),
built on SDL2. To try it without installing anything, open the
[web player](https://blankiekun.github.io/?app=cons#cons).

Used by: [chimimouryou](https://github.com/blankiekun/chimimouryou), a
self-hosted Japanese e-book reader that plays visual novels in the browser
with the web build (see the text hooks in [COMMANDS.md](COMMANDS.md#web-hooks)),
and [conscripter-convert](https://github.com/blankiekun/conscripter-convert),
which converts games from other engines into scripts and archives that
CONscripter plays without engine patches.

![CONscripter running a game in the browser](screenshot/onsyuri_mo2_webtest.png)

## Features

- **Scripts**: Shift-JIS (`--enc:sjis`), GBK (`--enc:gbk`) and UTF-8
  (`--enc:utf8`) scripts; Lua scripts and animation; English half-width text;
  `nt2`/`nt3` encrypted scripts; the `effectskip` command.
- **Archives**: `arc.sar`, `arc.nsa` and friends, including games that mix
  `.sar` and `.nsa` archives.
- **Rendering**: fullscreen (`--fullscreen`, <kbd>Alt</kbd>+<kbd>Enter</kbd>)
  and stretched fullscreen (`--fullscreen2`, <kbd>F10</kbd>), any resolution
  (`--width`, `--height`), GLES2 sharpening (`--sharpness 1.0`), display
  filters (`--filter crt,pc98`: crt, pc98, gameboy, sepia, mono, vhs).
- **Input**: long click or touch opens the menu; the backlog works with UTF-8
  scripts.
- **Platforms**: see the table below.

## Platforms

| Platform | Builds | Notes | Install |
|---|---|---|---|
| Web | Emscripten (WebAssembly) | Saves in IndexedDB, zip export/import of saves, lazy loading of game files, touch and a web menu on phones, WebP images, video in a `<video>` element | [steps](PLATFORMS.md#web) |
| Windows | x86, x64 (MinGW, static); x86, x64, ARM64 (MSVC + vcpkg) | Video through the system player | [steps](#quick-start) |
| Linux | x86, x64, arm, aarch64 | Static or dynamic link; video in the window through the player's ffmpeg | [steps](PLATFORMS.md#linux-and-macos) |
| macOS | universal (Apple silicon and Intel), macOS 10.13+ (11.0+ on Apple silicon), static: no Homebrew needed | Video in the window through the player's ffmpeg; unsigned | [steps](PLATFORMS.md#linux-and-macos) |
| Android | API 22+ (Android 5.1) | SD cards through SAF, scoped storage, non-English paths, video through the system player, 16 KB pages | [steps](PLATFORMS.md#android) |
| RetroArch | libretro core `conscripter_libretro`; Switch (`conscripter_libretro_libnx.nro`), PS Vita (`conscripter_libretro_vita.self`), Miyoo Mini (`conscripter_libretro_miyoo.so`), Wii U (`conscripter_libretro_wiiu.rpx`), 3DS (`conscripter_libretro.3dsx`) |  | [steps](PLATFORMS.md#retroarch) |
| Kobo (KOReader) | armhf (static) | E-ink through FBInk: effects and text shown at once; touch and page keys; a sleep screen on the power button; video through its own ffmpeg, no sound | [steps](PLATFORMS.md#kobo-koreader) |

## Quick start

Download the build for your platform from the
[releases](https://github.com/blankiekun/CONscripter/releases) (per-device
steps: [PLATFORMS.md](PLATFORMS.md)), then run it with the game folder:

``` bash
./conscripter --root /path/to/game --save-dir /path/to/save --font /path/default.ttf --enc:sjis
```

Options can also go in an `ons_args` file in the game folder.

Japanese games need a font that covers Japanese, passed with `--font` or put in
the game folder as `default.ttf`. Use a fixed-width font such as IPA Gothic:
the engine draws each character in a full-width cell, so a proportional font
leaves gaps after narrow kana.

Note: force-quitting a game can damage its save; if a game won't start afterwards,
remove `envdata` from the save folder.

## Command-line options

``` bash
./conscripter --help
./conscripter --width 1280 --height 720 --sharpness=3.1
./conscripter --fullscreen2 # fullscreen1 alt+f, fullscreen2 f10 (toggle stretch)

```
Usage: conscripter [option ...]
  -h, --help            show this help and exit
  -v, --version         show the version information and exit
      --list-commands   print every command name the engine knows, one per line, and exit

 load options:
  -f, --font file       set a TTF font file
  -r, --root path       set the root path to the archives
      --save-dir        set save dir (made if it doesn't exist)
      --debug:1         print debug info
      --enc:[gbk|sjis|utf8]  define the encoding of script

 render options:
      --window          start in windowed mode
      --width 1280      force window width
      --height 720      force window height
      --fullscreen      start in fullscreen mode (alt+f or alt+enter)
      --fullscreen2     start in fullscreen mode with stretch (f10 to toggle stretch)
      --sharpness 3.1   use gles to make image sharp
      --filter crt,pc98 show the screen through filters, in order: crt, pc98, gameboy, sepia, mono, vhs
      --no-video        do not decode video
      --no-vsync        turn off vsync

 other options:
      --cdaudio         use CD audio if available
      --cdnumber no     choose the CD-ROM drive number
      --registry file   set a registry file
      --dll file        set a dll file
      --enable-wheeldown-advance        advance the text on mouse wheel down
      --disable-rescale do not rescale the images in the archives
      --force-button-shortcut   ignore useescspc and getenter command
      --render-font-outline     render the outline of a text instead of casting a shadow
      --edit            enable online modification of the volume and variables when 'z' is pressed
      --key-exe file    set a file (*.EXE) that includes a key table
      --fontcache       cache default font
```

## Games from other engines

[conscripter-convert](https://github.com/blankiekun/conscripter-convert)
turns a game you own (Xuse, Nitroplus, MAGES and TYPE-MOON games; the list is in
its README) into one CONscripter can play:

``` sh
conscripter-convert build /path/to/game -o out/mygame
./conscripter --root out/mygame
```

See its README for the supported games and what converts.

## CONscripter additions

Commands, text tags, web hooks and save data that CONscripter adds to
ONScripter-Yuri for games converted by conscripter-convert.
None is opt-in: new behaviour applies to every script. See
[COMMANDS.md](COMMANDS.md), the reference for every script command (ONScripter's and CONscripter's).

To find bugs in a converted game, `--trace file` writes what is on screen at
every text page, the commands that changed it, the choices made, missing
files and script errors as JSON lines, and `--auto-play` plays a game through
by itself. See [DEBUG.md](DEBUG.md).

## Building

On Linux, with SDL2, SDL2_ttf, SDL2_image, SDL2_mixer, libjpeg, bzip2, Lua and CMake installed:

``` sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo && make -C build -j"$(nproc)"
```

Every other platform (Windows, macOS, web, Android, Kobo, Docker): [BUILDING.md](BUILDING.md).

## Known issues

- Windows: blurry fullscreen comes from high-DPI scaling. Change the
  executable's high-DPI setting, or:

  ```bat
  set app_name=conscripter.exe
  reg add "HKCU\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" /t reg_sz /v "%~dp0%app_name%" /d "~ HIGHDPIAWARE" /f
  ```

- Web: SDL2 `Mix_LoadMUS` can't decode MP3 music.
- Release binaries are large because every library is linked statically, for
  compatibility.

## Credits and license

CONscripter is [GPL-2.0-or-later](LICENSE), like the projects it derives from:

- [ONScripter](https://onscripter.osdn.jp/onscripter.html) by Ogapee (Studio O.G.A.)
- [ONScripter-Jh](https://github.com/jh10001/ONScripter-Jh) by jh10001
- [ONScripter-Yuri](https://github.com/YuriSizuku/OnscripterYuri) by YuriSizuku
  and its contributors (MSVC builds by ryank231231, the libretro core by iyzsong,
  macOS builds by yujincheng08, and others)

CONscripter is not affiliated with or endorsed by these projects; bug reports
about CONscripter belong in [this repository](https://github.com/blankiekun/CONscripter/issues).
