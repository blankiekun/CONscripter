# bash -c "BUILD_TYPE=Debug SKIP_PORTS=yes ./cross_darwin.sh"
# A universal (arm64 + x86_64) binary with static ports, for macOS 10.13 and later:
# build_darwin/conscripter
CMAKELISTS_PATH=$(pwd)/..
CORE_NUM=$(sysctl -n hw.ncpu)
TARGETS=$@
export MACOSX_DEPLOYMENT_TARGET=10.13 # the oldest Xcode 14+ targets; arm64 starts at 11.0 anyway

if [ -z "$BUILD_TYPE" ]; then BUILD_TYPE=MinSizeRel; fi
if [ -z "$TARGETS" ]; then TARGETS=all; fi
echo "BUILD_TYPE=$BUILD_TYPE"

set -e
for ARCH in arm64 x86_64; do
    PORTBUILD_PATH=$CMAKELISTS_PATH/thirdparty/build/arch_darwin_$ARCH
    BUILD_PATH=./../build_darwin_$ARCH
    echo "## ARCH=$ARCH PORTBUILD_PATH=$PORTBUILD_PATH"
    source _darwin.sh
    if [ -z "$SKIP_PORTS" ]; then
        source _fetch.sh
        fetch_lua ; build_lua
        fetch_sdl2 ; build_sdl2
        fetch_sdl2_image ; build_sdl2_image
        fetch_sdl2_ttf ; build_sdl2_ttf
        fetch_sdl2_mixer ; build_sdl2_mixer
        fetch_webp ; build_webp
    fi
    cmake -B $BUILD_PATH -S $CMAKELISTS_PATH \
        -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
        -DCMAKE_OSX_ARCHITECTURES=$ARCH \
        -DSTATIC_PORT_ROOT=$PORTBUILD_PATH
    make -C $BUILD_PATH $TARGETS -j$CORE_NUM
done

mkdir -p ./../build_darwin
lipo -create ./../build_darwin_arm64/conscripter ./../build_darwin_x86_64/conscripter \
    -output ./../build_darwin/conscripter
