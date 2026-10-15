# CONscripter

![GitHub release](https://img.shields.io/github/v/release/blankiekun/CONscripter?color=green&label=conscripter&style=flat-square)
![web](https://img.shields.io/github/actions/workflow/status/blankiekun/CONscripter/build_web.yml?label=web(wasm)&logo=firefox&style=flat-square)
![android](https://img.shields.io/github/actions/workflow/status/blankiekun/CONscripter/build_android.yml?label=android&logo=android&style=flat-square)
![windows](https://img.shields.io/github/actions/workflow/status/blankiekun/CONscripter/build_win.yml?label=windows&logo=mingww64&style=flat-square)
![linux](https://img.shields.io/github/actions/workflow/status/blankiekun/CONscripter/build_linux.yml?label=linux&logo=linux&style=flat-square)
![mac](https://img.shields.io/github/actions/workflow/status/blankiekun/CONscripter/build_darwin.yml?label=mac&logo=apple&style=flat-square)

An [ONScripter](https://onscripter.osdn.jp/onscripter.html) engine for playing
NScripter visual novels on the web, Windows, Linux, macOS, Android and
RetroArch: a fork of [ONScripter-Yuri](https://github.com/YuriSizuku/OnscripterYuri)
(itself based on [ONScripter-Jh](https://github.com/jh10001/ONScripter-Jh)),
built on SDL2.

CONscripter is the engine behind two companion projects:

- [chimimouryou](https://github.com/blankiekun/chimimouryou), a self-hosted
  Japanese e-book reader, plays visual novels in the browser with the
  WebAssembly build of CONscripter. The web build hands each page's text and
  where each character was drawn to the web page (see the text hooks in
  [ADDITIONS.md](ADDITIONS.md)), which lays it over the game for Yomitan and
  counts it in reading statistics.
- [conscripter-convert](https://github.com/blankiekun/conscripter-convert)
  (`vn2ons`) converts games made with other engines (Xuse, for now) into a
  `0.txt` script and `.nsa` archives that CONscripter plays without engine
  patches.

![CONscripter running a game in the browser](screenshot/onsyuri_mo2_webtest.png)

## Features

- **Scripts**: Shift-JIS (`--enc:sjis`), GBK (`--enc:gbk`) and UTF-8
  (`--enc:utf8`) scripts; Lua scripts and animation; English half-width text;
  `nt2`/`nt3` encrypted scripts; the `effectskip` command.
- **Archives**: `arc.sar`, `arc.nsa` and friends, including games that mix
  `.sar` and `.nsa` archives.
- **Rendering**: fullscreen (`--fullscreen`, <kbd>Alt</kbd>+<kbd>Enter</kbd>)
  and stretched fullscreen (`--fullscreen2`, <kbd>F10</kbd>), any resolution
  (`--width`, `--height`), GLES2 sharpening (`--sharpness 1.0`).
- **Input**: long click or touch opens the menu; the backlog works with UTF-8
  scripts.
- **Platforms**:

| Platform | Builds | Notes |
|---|---|---|
| Web | Emscripten (WebAssembly) | Saves in IndexedDB, zip export/import of saves, lazy loading of game files, touch and a web menu on phones, WebP images, video in a `<video>` element |
| Windows | x86, x64 (MinGW, static); x86, x64, ARM64 (MSVC + vcpkg) | Video through the system player |
| Linux | x86, x64, arm, aarch64 | Static or dynamic link; video in the window through the player's ffmpeg |
| macOS | x64, arm64 | Video in the window through the player's ffmpeg |
| Android | API 22+ (Android 5.1) | SD cards through SAF, scoped storage, non-English paths, video through the system player, 16 KB pages |
| RetroArch | libretro core `conscripter_libretro` | |

## CONscripter additions

Commands, text tags, web hooks and save data that CONscripter adds to
ONScripter-Yuri for games converted by
[conscripter-convert](https://github.com/blankiekun/conscripter-convert).
None is opt-in: new behaviour applies to every script. See
[ADDITIONS.md](ADDITIONS.md).

To find bugs in a converted game, `--trace file` writes what is on screen at
every text page, the commands that changed it, the choices made, missing
files and script errors as JSON lines, and `--auto-play` plays a game through
by itself. See [DEBUG.md](DEBUG.md).

## 1. Usage

### (1) Command line

Options can also go in an `ons_args` file in the game folder.

``` bash
./conscripter --help
./conscripter --root /path/to/game --save-dir /path/to/save --font /path/default.ttf --enc:sjis
./conscripter --width 1280 --height 720 --sharpness=3.1
./conscripter --fullscreen2 # fullscreen1 alt+f, fullscreen2 f10 (toggle stretch)
```

```
Usage: conscripter [option ...]
  -h, --help            show this help and exit
  -v, --version         show the version information and exit

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

Japanese games need a font that covers Japanese, passed with `--font` or put in
the game folder as `default.ttf`. Use a fixed-width font such as IPA Gothic:
the engine draws each character in a full-width cell, so a proportional font
leaves gaps after narrow kana.

❗ Force-quitting a game can damage its save; if a game won't start afterwards,
remove `envdata` from the save folder.

### (2) Games from other engines

[conscripter-convert](https://github.com/blankiekun/conscripter-convert)
turns a game you own into one CONscripter can play:

``` sh
vn2ons convert-game /path/to/game -o out/mygame --font /path/to/ipag.ttf --title "My Game"
./conscripter --root out/mygame
```

See its README for the supported engines and what converts.

### (3) Linux

Download a static binary from the
[releases](https://github.com/blankiekun/CONscripter/releases) or build from
source (see [Build](#2-build)).

![CONscripter on Linux](screenshot/onsyuri_mo2_linuxtest.jpg)

### (4) Web

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

### (5) Android

Install the APK. A three-finger tap or a long touch opens the menu, four fingers
skip text. Put games in `/storage/emulated/0/Android/data/io.github.blankiekun.conscripter/files`
or pick any folder through SAF. The package id is
`io.github.blankiekun.conscripter`.

![CONscripter on Android](screenshot/onsyuri_ui_androidtest2.jpg)

### (6) RetroArch

Build `src/cons_libretro` (see its `CMakeLists.txt`) and load
`conscripter_libretro` with a game's `0.txt`, `00.txt`, `nscript.dat` or folder.
Script encoding and mouse emulation are core options.

## 2. Build

The build scripts are in `script/`, Dockerfiles in `docker/`; GitHub Actions
builds every platform on each tag (`.github/workflows/`). Every build produces
`conscripter` (`conscripter.exe`, `conscripter.js` + `conscripter.wasm`,
`libconscripter.so` on Android).

### (1) local windows

Install the dependency in msys2,

``` sh
pacman -Syu --noconfirm
pacman -S --noconfirm make tar vim curl # util tools
pacman -S --noconfirm mingw-w64-x86_64-binutils mingw-w64-x86_64-gcc mingw-w64-x86_64-gdb # mingw64 compile tool
pacman -S --noconfirm mingw-w64-i686-binutils mingw-w64-i686-gcc mingw-w64-i686-gdb # mingw32 compile tool

pacman -S --noconfirm mingw-w64-i686-SDL2 mingw-w64-x86_64-SDL2
pacman -S --noconfirm mingw-w64-i686-SDL2_image mingw-w64-x86_64-SDL2_image
pacman -S --noconfirm mingw-w64-i686-SDL2_ttf mingw-w64-x86_64-SDL2_ttf
pacman -S --noconfirm mingw-w64-i686-SDL2_mixer mingw-w64-x86_64-SDL2_mixer

pacman -S --noconfirm mingw-w64-i686-brotli mingw-w64-x86_64-brotli
pacman -S --noconfirm mingw-w64-i686-mesa mingw-w64-x86_64-mesa
pacman -S --noconfirm mingw-w64-i686-lua mingw-w64-x86_64-lua
```

and then use these `local_msys2mingw32.sh` or `local_msys2mingw64.sh` to build.

``` sh
cd script
chmod +x *.sh
sh -c "export BUILD_TYPE=Debug && export MSYS2_HOME=/path/to/msys2 && ./local_msys2mingw32.sh"
```

### (2) local linux

Install the dependency

``` bash
# linux64
sudo apt-get update
sudo apt-get -y install gcc gdb make cmake git curl
sudo apt-get -y install libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libsdl2-mixer-dev
sudo apt-get -y install libbz2-dev libjpeg-dev libpng-dev
sudo apt-get -y install liblua5.3-dev libgl1-mesa-dev

# linux32
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get -y install gcc-multilib g++-multilib
sudo apt-get -y install libsdl2-dev:i386 libsdl2-ttf-dev:i386 libsdl2-image-dev:i386 libsdl2-mixer-dev:i386
sudo apt-get -y install libbz2-dev:i386 libjpeg-dev:i386 libpng-dev:i386
sudo apt-get -y install liblua5.3-dev:i386 libgl1-mesa-dev:i386
```

``` sh
cd script
chmod +x *.sh
sh -c "export BUILD_TYPE=Debug && ./local_linux32.sh"
```

and then use `local_linux32.sh` or `local_linux64.sh` to build.

### (3) cross web

Install [emsdk](https://github.com/emscripten-core/emsdk) and use `cross_web.sh` to build.
You need to add `$EMSDK_HOME` and `$MSYS2_HOME` (if windows) user variable at first.

``` shell
cd script
chmod +x *.sh
sh -c "BUILD_TYPE=Debug EMCSDK_HOME=/path/to/emsdk ./cross_web.sh"
```

### (4) cross linux

Install the dependency for cross compiler,

``` shell
# install in the target machine, aarch64
sudo apt-get -y install libx11-dev libxext-dev libasound2-dev
sudo apt-get -y install libgl1-mesa-dev mesa-utils

# install in the target machine, armhf (if you want to run armhf in aarch64)
sudo dpkg --add-architecture armhf && sudo apt-get update
sudo apt-get -y install libc6:armhf
sudo apt-get -y install libx11-dev:armhf libxext-dev:armhf libasound2-dev:armhf
sudo apt-get -y install libgl1-mesa-dev:armhf mesa-utils:armhf

# install in the local machine
sudo apt-get -y install tar make cmake curl git
sudo apt-get -y install crossbuild-essential-i386
sudo apt-get -y install crossbuild-essential-x86_64
sudo apt-get -y install crossbuild-essential-armhf
sudo apt-get -y install crossbuild-essential-arm64
```

then use `cross_linuxa64.sh` or `cross_linuxa32.sh`, `cross_linux32.sh`, `cross_linux64.sh` to compile.

![CONscripter on a Raspberry Pi](screenshot/onsyuri_mo2_linuxtest2.png)

For raspberrypi or the other arm64 devices cross compiling.
As there are many system bindings in SDL2,
just build libraries in the target machine, and use these build cache to link.
Or you can use `docker_linuxarm64.sh` for cross compile.

```shell
# at first build sdl2 in raspberry pi
sh -c "export SYSROOT=/ && ./local_linux64.sh"

# copy prebuild of dependency to local
cp -rf /path/to/rpi/CONscripter/thirdparty/build/arch_aarch64 thirdparty/build/arch_aarch64

# use SKIP_PORTS to skip thirdpart builds
cd script
chmod +x *.sh
sh -c "export BUILD_TYPE=Debug && export SKIP_PORTS=yes && ./local_linux64.sh"
```

### (5) cross mingw

Install mingw cross compiler and tools,

in `debian` or `wsl2`,

``` shell
sudo apt-get -y install tar make cmake curl git
# use pkg-config is to find sdl2 by compiling sdl2_image, do not install mingw-w64-tools, this pkg-config is broken
sudo apt-get -y install mingw-w64 zstd pkg-config
```

in `msys2`,

```shell
pacman -Syu --noconfirm
pacman -S --noconfirm make tar vim curl # util tools
pacman -S --noconfirm mingw-w64-x86_64-gcc mingw-w64-x86_64-gdb
pacman -S --noconfirm mingw-w64-x86_64-binutils mingw-w64-x86_64-pkg-config
pacman -S --noconfirm mingw-w64-i686-gcc mingw-w64-i686-gdb
pacman -S --noconfirm mingw-w64-i686-binutils mingw-w64-i686-pkg-config
```

then use `cross_mingw32.sh` or `cross_mingw64.sh` to compile.

If you want to make compatible for `winxp`, should use gcc below 12.

### (6) cross llvmmingw

Download [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases/tag/20240619) and add `${LLVMMINGW_HOME}/bin` to path,

then use `cross_llvmmingw32.sh` or `cross_llvmmingw64.sh` to compile (either `bash (git bash)` or `msys2 shell`).

If you want to build compatible for `winxp`, you can use [i686-11.2.0-release-win32-dwarf-rt_v9-rev1](https://github.com/niXman/mingw-builds-binaries/releases/download/11.2.0-rt_v9-rev1/i686-11.2.0-release-win32-dwarf-rt_v9-rev1.7z). Higher version like `gcc 14` is not available.

Do not mix `mingw` and `llvm-mingw` libraries, which will cause `ld.lld: error: undefined symbol: ___chkstk_ms` problem !

```sh
export PATH=/path/to/mingw32/bin:$PATH
export CC=i686-w64-mingw32-gcc
export CXX=i686-w64-mingw32-g++
export RC=i686-w64-mingw32-windres

echo CC=$(which $CC)
echo CXX=$(which $CXX)
echo RC=$(which $RC)

bash -c "./cross_llvmmingw32.sh"
```

### (7) cross android

Install android sdk and ndk, then

``` sh
# prepare port dependencies
cd script
sh ./cross_android.sh
cd -

# use ANDROID_HOME or local.properties for sdk
cd src/cons_android/
chmod +x ./gradlew && ./gradlew assembleDebug
```

### (8) cross by docker

You can easily build all supported platforms by docker, see `docker/docker_xxx.sh` in detail.
If you want to build for linux arm in x86 platform, install qemu at first.

```  shell
sudo apt-get install qemu-user-static binfmt-support
```

## 3. Known issues

- Windows: blurry fullscreen comes from high-DPI scaling. Change the
  executable's high-DPI setting, or:

  ```bat
  set app_name=conscripter.exe
  reg add "HKCU\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" /t reg_sz /v "%~dp0%app_name%" /d "~ HIGHDPIAWARE" /f
  ```

- Web: SDL2 `Mix_LoadMUS` can't decode MP3 music.
- Release binaries are large because every library is linked statically, for
  compatibility.

## 4. Credits and license

CONscripter is [GPL-2.0-or-later](LICENSE), like the projects it derives from:

- [ONScripter](https://onscripter.osdn.jp/onscripter.html) by Ogapee (Studio O.G.A.)
- [ONScripter-Jh](https://github.com/jh10001/ONScripter-Jh) by jh10001
- [ONScripter-Yuri](https://github.com/YuriSizuku/OnscripterYuri) by YuriSizuku
  and its contributors (MSVC builds by ryank231231, the libretro core by iyzsong,
  macOS builds by yujincheng08, and others)

CONscripter is not affiliated with or endorsed by these projects; bug reports
about CONscripter belong in [this repository](https://github.com/blankiekun/CONscripter/issues).
