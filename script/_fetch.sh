# prepare dirs
if ! [ -d $CMAKELISTS_PATH/thirdparty/port ]; then mkdir -p $CMAKELISTS_PATH/thirdparty/port; fi
if ! [ -d $CMAKELISTS_PATH/thirdparty/build/arch_mingw32 ]; then mkdir -p $CMAKELISTS_PATH/thirdparty/build/arch_mingw32; fi
if ! [ -d $CMAKELISTS_PATH/thirdparty/build/arch_mingw64 ]; then mkdir -p $CMAKELISTS_PATH/thirdparty/build/arch_mingw64; fi
if ! [ -d $CMAKELISTS_PATH/thirdparty/build/arch_linux32 ]; then mkdir -p $CMAKELISTS_PATH/thirdparty/build/arch_linux32; fi
if ! [ -d $CMAKELISTS_PATH/thirdparty/build/arch_linux64 ]; then mkdir -p $CMAKELISTS_PATH/thirdparty/build/arch_linux64; fi
if ! [ -d $CMAKELISTS_PATH/thirdparty/build/arch_linuxa64 ]; then mkdir -p $CMAKELISTS_PATH/thirdparty/build/arch_linuxa64; fi
if ! [ -d $CMAKELISTS_PATH/thirdparty/build/arch_wasm ]; then mkdir -p $CMAKELISTS_PATH/thirdparty/build/arch_wasm; fi

# sha256 of a file; sha256sum is not on every macOS
function fetch_sha256()
{
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# urlbase, name, sha256, optional full fallback url
# Checks the hash and exits the calling shell on failure.
function fetch_port()
{
    local dir="$CMAKELISTS_PATH/thirdparty/port"
    local file="$dir/$2.tar.gz"
    if [ -d "$dir/$2" ]; then return; fi
    if ! { [ -f "$file" ] && [ "$(fetch_sha256 "$file")" = "$3" ]; }; then
        echo "## fetch_port $1 $2"
        rm -f "$file"
        curl -fsSL --retry 2 --connect-timeout 20 "$1/$2.tar.gz" -o "$file" \
            || { [ -n "$4" ] && curl -fsSL --retry 2 --connect-timeout 20 "$4" -o "$file"; }
        if ! [ -f "$file" ] || [ "$(fetch_sha256 "$file")" != "$3" ]; then
            echo "## fetch_port: $2 failed or has the wrong checksum"
            rm -f "$file"
            exit 1
        fi
    fi
    tar zxf "$file" -C "$dir"
}

# fetch by curl
function fetch_lua()
{
    LUA_NAME=lua-5.4.4
    LUA_SRC=$CMAKELISTS_PATH/thirdparty/port/$LUA_NAME
    fetch_port https://www.lua.org/ftp $LUA_NAME 164c7849653b80ae67bec4b7473b884bf5cc8d2dca05653475ec2ed27b9ebf61 https://deb.debian.org/debian/pool/main/l/lua5.4/lua5.4_5.4.4.orig.tar.gz
}

function fetch_webp()
{
    WEBP_NAME=libwebp-1.5.0
    WEBP_SRC=$CMAKELISTS_PATH/thirdparty/port/$WEBP_NAME
    fetch_port https://storage.googleapis.com/downloads.webmproject.org/releases/webp $WEBP_NAME 7d6fab70cf844bf6769077bd5d7a74893f8ffd4dfb42861745750c63c2a5c92c
}

function fetch_jpeg()
{
    JPEG_NAME=jpeg-9
    JPEG_SRC=$CMAKELISTS_PATH/thirdparty/port/$JPEG_NAME
    fetch_port https://www.ijg.org/files jpegsrc.v9 6e004d72f9108014caccf434095514c139478c72b6b1873a3e3807ea5bec5c59
}

function fetch_bz2()
{  
    BZ2_NAME=bzip2-1.0.8
    BZ2_SRC=$CMAKELISTS_PATH/thirdparty/port/$BZ2_NAME
    fetch_port https://sourceware.org/pub/bzip2 $BZ2_NAME ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
}

function fetch_sdl2() 
{
    SDL2_NAME=SDL2-2.26.3
    SDL2_SRC=$CMAKELISTS_PATH/thirdparty/port/$SDL2_NAME
    fetch_port https://github.com/libsdl-org/SDL/releases/download/release-2.26.3  $SDL2_NAME c661205a553b7d252425f4b751ff13209e5e020b876bbfa1598494af61790057
}

function fetch_sdl2_image()
{
    SDL2_IMAGE_NAME=SDL2_image-2.6.3
    SDL2_IMAGE_SRC=$CMAKELISTS_PATH/thirdparty/port/$SDL2_IMAGE_NAME
    fetch_port https://github.com/libsdl-org/SDL_image/releases/download/release-2.6.3 $SDL2_IMAGE_NAME 931c9be5bf1d7c8fae9b7dc157828b7eee874e23c7f24b44ba7eff6b4836312c
}

function fetch_sdl2_ttf()
{
    SDL2_TTF_NAME=SDL2_ttf-2.20.2
    SDL2_TTF_SRC=$CMAKELISTS_PATH/thirdparty/port/$SDL2_TTF_NAME
    fetch_port https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.20.2 $SDL2_TTF_NAME 9dc71ed93487521b107a2c4a9ca6bf43fb62f6bddd5c26b055e6b91418a22053
}

function fetch_sdl2_mixer()
{
    SDL2_MIXER_NAME=SDL2_mixer-2.6.3
    SDL2_MIXER_SRC=$CMAKELISTS_PATH/thirdparty/port/$SDL2_MIXER_NAME
    fetch_port https://github.com/libsdl-org/SDL_mixer/releases/download/release-2.6.3 $SDL2_MIXER_NAME 7a6ba86a478648ce617e3a5e9277181bc67f7ce9876605eea6affd4a0d6eea8f
}

# fetch unused port
function fetch_stb()
{
    STB_NAME=stb
    STB_SRC=$CMAKELISTS_PATH/thirdparty/port/$STB_NAME
    git clone https://github.com/nothings/stb.git $STB_SRC
}
