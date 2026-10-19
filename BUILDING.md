# Building CONscripter

How to build the engine from source. To only play a game, download a release instead (see [README.md](README.md)).

The build scripts are in `script/`, Dockerfiles in `docker/`; GitHub Actions
builds every platform on each tag (`.github/workflows/`). Every build produces
`conscripter` (`conscripter.exe`, `conscripter.js` + `conscripter.wasm`,
`libconscripter.so` on Android).

## Windows (MSYS2)

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

## Linux

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

## Web (Emscripten)

Install [emsdk](https://github.com/emscripten-core/emsdk) and use `cross_web.sh` to build.
You need to add `$EMSDK_HOME` and `$MSYS2_HOME` (if windows) user variable at first.

``` shell
cd script
chmod +x *.sh
sh -c "BUILD_TYPE=Debug EMCSDK_HOME=/path/to/emsdk ./cross_web.sh"
```

## Linux, cross-compiled (arm, aarch64)

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

## Windows, cross-compiled (MinGW)

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

## Windows, cross-compiled (llvm-mingw)

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

## Android

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

## Kobo

`cross_kobo.sh` builds a static armhf `conscripter` with `KOBO` on
(`src/cons/kobo.cpp`), SDL2 without X11, GL or sound, FBInk and a small
ffmpeg (`thirdparty/build/arch_kobo/bin/ffmpeg`). It needs
`crossbuild-essential-armhf`, `autoconf`, `automake`, `libtool` and
`pkg-config`, or Docker: `docker/docker_kobo.sh`.

## macOS

`cd script && bash cross_darwin.sh` builds the ports (Lua, SDL2, SDL2_image,
SDL2_ttf, SDL2_mixer) as static libraries for arm64 and x86_64 and joins the
two engines into `build_darwin/conscripter` with `lipo`; it needs Xcode's
command line tools and `pkg-config`. `local_darwin.sh` is the quicker build
against Homebrew's libraries, for that Mac only.

## All platforms with Docker

You can easily build all supported platforms by docker, see `docker/docker_xxx.sh` in detail.
If you want to build for linux arm in x86 platform, install qemu at first.

```  shell
sudo apt-get install qemu-user-static binfmt-support
```
