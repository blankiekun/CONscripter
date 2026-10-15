# CONscripter: Opus sounds. SDL_mixer reads Opus only when it was built
# with opusfile, and none of the SDL_mixers our builds use was (the
# prebuilt Linux and Windows ports, Android's, emscripten's). So the
# engine decodes Opus itself (ONScripter_cons.cpp, decodeOpus) with
# libogg, libopus and opusfile, compiled from source here into every
# build, and gives SDL_mixer the sound as WAV.
option(CONS_OPUS "Play Opus sounds (downloads libogg, libopus and opusfile when configuring)" ON)

function(cons_opus TARGET_NAME)
    if(NOT CONS_OPUS)
        return()
    endif()
    include(FetchContent)
    # Extracted files get the extraction's time (DOWNLOAD_EXTRACT_TIMESTAMP
    # is newer than Android's CMake 3.22)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    # SOURCE_SUBDIR names no CMakeLists.txt: only download libogg and
    # opusfile, whose few files are compiled below.
    FetchContent_Declare(cons_libogg
        URL https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.gz
        URL_HASH SHA256=0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664
        SOURCE_SUBDIR cons-none)
    FetchContent_Declare(cons_opusfile
        URL https://downloads.xiph.org/releases/opus/opusfile-0.12.tar.gz
        URL_HASH SHA256=118d8601c12dd6a44f52423e68ca9083cc9f2bfe72da7a8c1acb22a80ae3550b
        SOURCE_SUBDIR cons-none)
    FetchContent_Declare(cons_libopus
        URL https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
        URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1)
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    if(CMAKE_SYSTEM_NAME MATCHES "Emscripten")
        set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)
    endif()
    # libopus builds with -fstack-protector-strong and _FORTIFY_SOURCE by
    # default: with MinGW both need libssp-0.dll, which a lone .exe doesn't
    # have (0.21.0's Windows builds didn't start without it), and the web
    # build has neither
    if(CMAKE_SYSTEM_NAME MATCHES "Emscripten" OR MINGW)
        set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
        set(OPUS_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
    endif()
    FetchContent_MakeAvailable(cons_libogg cons_opusfile cons_libopus)

    # libogg's config_types.h, which its own build generates
    set(OGG_GEN ${CMAKE_CURRENT_BINARY_DIR}/cons_ogg_include)
    file(WRITE ${OGG_GEN}/ogg/config_types.h
        "#ifndef __CONFIG_TYPES_H__\n#define __CONFIG_TYPES_H__\n#include <stdint.h>\n"
        "typedef int16_t ogg_int16_t;\ntypedef uint16_t ogg_uint16_t;\n"
        "typedef int32_t ogg_int32_t;\ntypedef uint32_t ogg_uint32_t;\n"
        "typedef int64_t ogg_int64_t;\ntypedef uint64_t ogg_uint64_t;\n#endif\n")
    set(OGG_DIR ${cons_libogg_SOURCE_DIR})
    set(OPUSFILE_DIR ${cons_opusfile_SOURCE_DIR})
    add_library(cons_opusfile STATIC
        ${OGG_DIR}/src/bitwise.c ${OGG_DIR}/src/framing.c
        ${OPUSFILE_DIR}/src/info.c ${OPUSFILE_DIR}/src/internal.c
        ${OPUSFILE_DIR}/src/opusfile.c ${OPUSFILE_DIR}/src/stream.c)
    target_include_directories(cons_opusfile PUBLIC
        ${OGG_GEN} ${OGG_DIR}/include ${OPUSFILE_DIR}/include)
    target_compile_definitions(cons_opusfile PRIVATE OP_DISABLE_HTTP OP_DISABLE_EXAMPLES)
    target_link_libraries(cons_opusfile PUBLIC opus)
    if(CMAKE_SYSTEM_NAME MATCHES "Android")
        target_link_options(cons_opusfile PRIVATE "-Wl,-z,max-page-size=16384")
        # Bionic before API 24 has no fseeko/ftello for 64-bit offsets on
        # 32-bit ARM; the engine reads Opus from memory, never from files
        target_compile_definitions(cons_opusfile PRIVATE _FILE_OFFSET_BITS=32)
    endif()
    # Not target_link_libraries: the platform functions use its plain
    # signature on some platforms and the keyword one on others, and the
    # two can't be mixed. A target here still brings its include folders.
    set_property(TARGET ${TARGET_NAME} APPEND PROPERTY LINK_LIBRARIES cons_opusfile)
    target_compile_definitions(${TARGET_NAME} PRIVATE USE_CONS_OPUS)
endfunction()
