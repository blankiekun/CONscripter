# must use after _fetch.sh and _linuxa32.sh from cross_kobo.sh
# The Kobo is armhf like linuxa32, but has no X11, GL or sound card:
# SDL only draws into memory (dummy video), kobo.cpp shows it with FBInk.

function fetch_fbink()
{
    # master: the touch quirks in FBInkState are newer than v1.25.0
    FBINK_REV=886f25f13368859ad8a899b88d04c26e19cda32e
    FBINK_NAME=FBInk-${FBINK_REV:0:7}
    FBINK_SRC=$CMAKELISTS_PATH/thirdparty/port/$FBINK_NAME
    if ! [ -d "$FBINK_SRC" ]; then
        git init -q $FBINK_SRC
        git -C $FBINK_SRC fetch -q --depth 1 https://github.com/NiLuJe/FBInk.git $FBINK_REV
        git -C $FBINK_SRC checkout -q FETCH_HEAD
        git -C $FBINK_SRC submodule -q update --init --depth 1
    fi
}

function build_fbink()
{
    echo "## FBINK_SRC=$FBINK_SRC"
    make -C $FBINK_SRC clean
    make -C $FBINK_SRC staticlib KOBO=true MINIMAL=true IMAGE=true CROSS_TC=arm-linux-gnueabihf -j$CORE_NUM
    mkdir -p $PORTBUILD_PATH/include $PORTBUILD_PATH/lib
    cp $FBINK_SRC/fbink.h $PORTBUILD_PATH/include
    cp $FBINK_SRC/Release/libfbink.a $PORTBUILD_PATH/lib
}

# _linuxa32.sh's, without "make all"'s test, which runs the ARM bzip2 on the host
function build_bz2()
{
    echo "## BZ2_SRC=$BZ2_SRC"

    make -C $BZ2_SRC clean
    make -C $BZ2_SRC libbz2.a bzip2 bzip2recover CC=arm-linux-gnueabihf-gcc AR=arm-linux-gnueabihf-ar RANLIB=arm-linux-gnueabihf-ranlib -j$CORE_NUM
    make -C $BZ2_SRC install PREFIX=$PORTBUILD_PATH
}

function build_sdl2()
{
    if ! [ -d "${SDL2_SRC}/build_${PLATFORM}" ]; then mkdir -p "${SDL2_SRC}/build_${PLATFORM}"; fi
    echo "## SDL2_SRC=$SDL2_SRC"

    export CFLAGS="-Os"
    export CXXFLAGS="-Os"
    pushd "${SDL2_SRC}/build_${PLATFORM}"
    ../configure --host=arm-linux-gnueabihf \
        --enable-static --disable-shared \
        --disable-video-x11 --disable-video-wayland --disable-video-kmsdrm \
        --disable-video-opengl --disable-video-opengles --disable-video-vulkan \
        --disable-alsa --disable-pulseaudio --disable-pipewire --disable-jack --disable-sndio \
        --disable-libudev --disable-dbus --disable-ime --disable-hidapi \
        --enable-arm-simd --enable-arm-neon \
        --prefix=$PORTBUILD_PATH
    make -j$CORE_NUM && make install
    popd
}

# ffmpeg for movies (the engine runs it, c.f. CONS_FFMPEG in play.sh):
# only what movies in games use, decoding to raw frames, no sound
function fetch_ffmpeg()
{
    FFMPEG_NAME=ffmpeg-7.1.1
    FFMPEG_SRC=$CMAKELISTS_PATH/thirdparty/port/$FFMPEG_NAME
    if ! [ -d "$FFMPEG_SRC" ]; then
        curl -fsSL https://ffmpeg.org/releases/$FFMPEG_NAME.tar.xz -o $FFMPEG_SRC.tar.xz
        tar xJf $FFMPEG_SRC.tar.xz -C $CMAKELISTS_PATH/thirdparty/port
    fi
}

function build_ffmpeg()
{
    if ! [ -d "${FFMPEG_SRC}/build_${PLATFORM}" ]; then mkdir -p "${FFMPEG_SRC}/build_${PLATFORM}"; fi
    echo "## FFMPEG_SRC=$FFMPEG_SRC"

    pushd "${FFMPEG_SRC}/build_${PLATFORM}"
    ../configure --enable-cross-compile --arch=arm --cpu=cortex-a9 --target-os=linux \
        --cross-prefix=arm-linux-gnueabihf- --extra-ldflags=-static \
        --disable-everything --disable-autodetect --disable-doc --disable-network \
        --disable-ffplay --disable-ffprobe --disable-debug --enable-small \
        --enable-protocol=file,pipe \
        --enable-demuxer=mov,matroska,avi,asf,mpegps,mpegts,mpegvideo,ogg \
        --enable-decoder=h264,hevc,mpeg1video,mpeg2video,mpeg4,msmpeg4v3,wmv1,wmv2,wmv3,vp8,vp9,theora \
        --enable-parser=h264,hevc,mpegvideo,mpeg4video,vp8,vp9,vc1 \
        --enable-filter=scale,pad,format,fps,null \
        --enable-encoder=rawvideo --enable-muxer=rawvideo \
        --prefix=$PORTBUILD_PATH
    make -j$CORE_NUM && make install
    popd
}
