# must use after _fetch.sh from cross_darwin.sh, with ARCH (arm64 or x86_64) and PORTBUILD_PATH set
# Static libraries only, so the engine needs nothing from Homebrew; bz2, iconv and z come from macOS.

XCC="clang -arch $ARCH"
if [ "$ARCH" = arm64 ]; then HOST=aarch64-apple-darwin; else HOST=x86_64-apple-darwin; fi
export PKG_CONFIG_LIBDIR=$PORTBUILD_PATH/lib/pkgconfig # only our ports, never Homebrew's

function build_lua()
{
    echo "## LUA_SRC=$LUA_SRC"

    make -C $LUA_SRC/src clean
    make -C $LUA_SRC/src liblua.a CC="$XCC" MYCFLAGS="-DLUA_USE_MACOSX" -j$CORE_NUM
    mkdir -p $PORTBUILD_PATH/lib $PORTBUILD_PATH/include
    cp $LUA_SRC/src/liblua.a $PORTBUILD_PATH/lib
    (cd $LUA_SRC/src && cp lua.h luaconf.h lualib.h lauxlib.h lua.hpp $PORTBUILD_PATH/include)
}

# name src [configure options]
function build_autotools()
{
    local B="$2/build_darwin_$ARCH"
    echo "## $1 $2"
    rm -rf "$B" && mkdir -p "$B"
    pushd "$B"
    ../configure --host=$HOST CC="$XCC" CXX="clang++ -arch $ARCH" OBJC="$XCC" \
        --disable-shared --enable-static "${@:3}" \
        --prefix=$PORTBUILD_PATH
    make -j$CORE_NUM
    make install
    popd
}

function build_sdl2() { build_autotools SDL2 $SDL2_SRC --disable-video-x11; }
function build_sdl2_image() { build_autotools SDL2_image $SDL2_IMAGE_SRC --enable-stb_image \
    --disable-webp --disable-tif --disable-jxl --disable-avif; }
function build_sdl2_ttf() { build_autotools SDL2_ttf $SDL2_TTF_SRC --disable-harfbuzz; }
function build_sdl2_mixer() { build_autotools SDL2_mixer $SDL2_MIXER_SRC; }

# SDL2_image is built without WebP (and ImageIO has none): the engine decodes it with libwebp (USE_WEBP)
function build_webp()
{
    echo "## WEBP_SRC=$WEBP_SRC"
    rm -rf $WEBP_SRC/build_darwin_$ARCH
    cmake -S $WEBP_SRC -B $WEBP_SRC/build_darwin_$ARCH \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PORTBUILD_PATH -DCMAKE_OSX_ARCHITECTURES=$ARCH \
        -DBUILD_SHARED_LIBS=OFF -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
        -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
        -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF \
        -DWEBP_BUILD_LIBWEBPMUX=OFF
    make -C $WEBP_SRC/build_darwin_$ARCH install -j$CORE_NUM
}
