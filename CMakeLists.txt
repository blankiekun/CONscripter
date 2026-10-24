cmake_minimum_required(VERSION 3.16.3)
project(conscripter)
include(CheckSymbolExists)

# CONscripter: the SIMD drawing paths wherever the compiler targets SSE2 or NEON
function(config_simd TARGET_NAME)
    check_symbol_exists(__SSE2__ "stdint.h" HAVE_SSE2)
    check_symbol_exists(__ARM_NEON "stdint.h" HAVE_NEON)
    if(HAVE_SSE2)
        target_compile_definitions(${TARGET_NAME} PRIVATE USE_SIMD USE_SIMD_X86_SSE2)
    elseif(HAVE_NEON)
        target_compile_definitions(${TARGET_NAME} PRIVATE USE_SIMD USE_SIMD_ARM_NEON)
    endif()
endfunction()

function(config_linux TARGET_NAME)
    add_executable(${TARGET_NAME} ${CONSCRIPTER_CODE})
    install(TARGETS ${TARGET_NAME} RUNTIME)
    message("Linux enviroment for ${TARGET_NAME}, type ${CMAKE_BUILD_TYPE}, compiler ${CMAKE_C_COMPILER}")
    target_compile_definitions(${TARGET_NAME} PRIVATE
        ${CONSCRIPTER_DEFINES}
        LINUX
        USE_PARALLEL
        USE_GLES
        USE_FILELOG
    )
    if(CMAKE_BUILD_TYPE MATCHES "MinSizeRel")
        target_compile_options(${TARGET_NAME} PRIVATE "-flto")
        target_link_options(${TARGET_NAME} PRIVATE "-s" "-flto")
    endif()
    if(KOBO) # CONscripter: Kobo e-readers (script/cross_kobo.sh)
        target_sources(${TARGET_NAME} PRIVATE src/cons/kobo.cpp)
        target_compile_definitions(${TARGET_NAME} PRIVATE KOBO)
        # all static: the Kobo's glibc is older than any toolchain's
        target_link_options(${TARGET_NAME} PRIVATE -static)
    endif()

    if(STATIC_PORT_ROOT)
        target_include_directories(${TARGET_NAME} PRIVATE
            ${STATIC_PORT_ROOT}/include
        )
        target_link_directories(${TARGET_NAME} PRIVATE
            ${STATIC_PORT_ROOT}/lib
        )
        target_link_options(${TARGET_NAME} PRIVATE 
            -static-libgcc
            -static-libstdc++
        )
        target_link_libraries(${TARGET_NAME} PRIVATE # static
            -Wl,-Bstatic
            lua
            jpeg
            bz2
            SDL2
            SDL2_ttf
            SDL2_image
            SDL2_mixer
            $<$<BOOL:${KOBO}>:fbink>
            $<$<NOT:$<BOOL:${KOBO}>>:-Wl,-Bdynamic>
            dl
            $<$<BOOL:${KOBO}>:-Wl,--whole-archive>
            pthread
            $<$<BOOL:${KOBO}>:-Wl,--no-whole-archive>
        )

    else()
        include(FindPkgConfig)
        find_package(SDL2 REQUIRED CONFIG REQUIRED COMPONENTS SDL2)
        pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)
        pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)
        pkg_check_modules(SDL2_MIXER REQUIRED SDL2_mixer)
        pkg_check_modules(JPEG REQUIRED libjpeg)
        find_package(Lua REQUIRED)
        target_include_directories(${TARGET_NAME} PRIVATE
            ${LUA_INCLUDE_DIR}
        )
        target_link_libraries(${TARGET_NAME} PRIVATE
            SDL2::SDL2
            jpeg
            bz2
            lua
            dl
            SDL2_ttf
            SDL2_image
            SDL2_mixer
            pthread
        )
    endif()

    if(CMAKE_SYSROOT) # cross compile
        message("CMAKE_SYSROOT ${CMAKE_SYSROOT}")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --sysroot=${CMAKE_SYSROOT}")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --sysroot=${CMAKE_SYSROOT}")
        target_include_directories(${TARGET_NAME} PRIVATE
            ${CMAKE_SYSROOT}/include
        )
        target_link_directories(${TARGET_NAME} PRIVATE
            ${CMAKE_SYSROOT}/lib
        )
    endif()

    config_simd(${TARGET_NAME})
endfunction()

function(config_darwin TARGET_NAME)
    add_executable(${TARGET_NAME} ${CONSCRIPTER_CODE})
    message("Darwin enviroment for ${TARGET_NAME}, type ${CMAKE_BUILD_TYPE}, compiler ${CMAKE_C_COMPILER}")
    target_compile_definitions(${TARGET_NAME} PRIVATE
            ${CONSCRIPTER_DEFINES}
            MACOSX
            USE_PARALLEL
            USE_GLES
            USE_FILELOG
    )
    target_compile_options(${TARGET_NAME} PRIVATE
            -Wno-deprecated-declarations
            -Wno-invalid-source-encoding
    )

    if(CMAKE_BUILD_TYPE MATCHES "MinSizeRel")
        target_compile_options(${TARGET_NAME} PRIVATE "-flto")
        target_link_options(${TARGET_NAME} PRIVATE "-flto")
    endif()

    config_simd(${TARGET_NAME})
    include(FindPkgConfig)
    if(STATIC_PORT_ROOT) # script/cross_darwin.sh: static ports, PKG_CONFIG_LIBDIR points at them
        pkg_check_modules(SDL2_PORTS REQUIRED SDL2_ttf SDL2_image SDL2_mixer)
        target_include_directories(${TARGET_NAME} PRIVATE
            ${STATIC_PORT_ROOT}/include
            ${SDL2_PORTS_STATIC_INCLUDE_DIRS}
        )
        target_link_libraries(${TARGET_NAME} PRIVATE
            ${STATIC_PORT_ROOT}/lib/liblua.a
            ${STATIC_PORT_ROOT}/lib/libwebpdecoder.a
            bz2 # macOS's own
            ${SDL2_PORTS_STATIC_LDFLAGS}
        )
        target_compile_definitions(${TARGET_NAME} PRIVATE USE_WEBP)
        return()
    endif()
    pkg_check_modules(SDL2 REQUIRED sdl2)
    pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)
    pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)
    pkg_check_modules(SDL2_MIXER REQUIRED SDL2_mixer)
    pkg_check_modules(LUA REQUIRED lua)

    target_include_directories(${TARGET_NAME} PRIVATE
            ${LUA_INCLUDE_DIRS}
            ${SDL2_INCLUDE_DIRS}
            ${SDL2_TTF_INCLUDE_DIRS}
            ${SDL2_IMAGE_INCLUDE_DIRS}
            ${SDL2_MIXER_INCLUDE_DIRS}
    )
    target_link_libraries(${TARGET_NAME} PRIVATE
            bz2
            ${LUA_LINK_LIBRARIES}
            ${SDL2_LINK_LIBRARIES}
            ${SDL2_TTF_LINK_LIBRARIES}
            ${SDL2_IMAGE_LINK_LIBRARIES}
            ${SDL2_MIXER_LINK_LIBRARIES}
    )
endfunction()

function(config_windows_msys2 TARGET_NAME)
    add_executable(${TARGET_NAME} WIN32 
        ${CONSCRIPTER_CODE}
        ${PROJECT_SOURCE_DIR}/asset/icon1.rc)
    if(MINGW)
        message("MINGW enviroment for ${TARGET_NAME}, type ${CMAKE_BUILD_TYPE}")
        if(CMAKE_BUILD_TYPE MATCHES "Debug")
            message("generating debug configuration")
            target_compile_definitions(${TARGET_NAME} PRIVATE _DEBUG)
            target_compile_options(${TARGET_NAME} PRIVATE -g)
            target_link_options(${TARGET_NAME} PRIVATE "-Wl,-subsystem,console") 
        else()
            if(CMAKE_BUILD_TYPE MATCHES "MinSizeRel")
                target_compile_options(${TARGET_NAME} PRIVATE "-flto")
                target_link_options(${TARGET_NAME} PRIVATE "-flto")
            endif()
            target_link_options(${TARGET_NAME} PRIVATE "-Wl,-subsystem,windows")
        endif()

        target_compile_definitions(${TARGET_NAME} PRIVATE
            ${CONSCRIPTER_DEFINES}
            _WIN32
            USE_PARALLEL
            USE_SIMD
            USE_SIMD_X86_SSE2
            USE_GLES
            USE_FILELOG
        )
        target_link_options(${TARGET_NAME} PRIVATE 
            -static-libgcc   # static link mingw env
            -static-libstdc++
        ) 
        target_link_libraries(${TARGET_NAME}  # static links
            -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive 
            -Wl,-Bstatic 
            mingw32 
            SDL2main
            SDL2
            lua

            SDL2_image
            m
            png
            jxl
            hwy
            jpeg
            tiff
            zstd
            deflate
            webp
            sharpyuv
            lzma
            lerc
            jbig

            SDL2_ttf
            harfbuzz
            rpcrt4
            Usp10
            graphite2
            brotlidec
            brotlicommon
            freetype
            z
            bz2
        
            SDL2_mixer
            opusfile
            opus
            ogg
            mpg123
            shlwapi
        )

        target_link_libraries(${TARGET_NAME} 
            -Wl,-Bdynamic # dynamic links
            dinput8 shell32 setupapi advapi32 uuid version oleaut32 ole32 
            imm32 winmm gdi32 user32
        )

    else()
        message("Windows enviroment for ${TARGET_NAME} not supported!")
    endif()
endfunction()

function(config_windows_mingw TARGET_NAME)
    add_executable(${TARGET_NAME} WIN32 
        ${CONSCRIPTER_CODE}
        ${PROJECT_SOURCE_DIR}/asset/icon1.rc)
    message("Cross MINGW enviroment for ${TARGET_NAME}, type ${CMAKE_BUILD_TYPE}, compiler ${CMAKE_C_COMPILER}")
   
    # config build type and arch
    if(CMAKE_BUILD_TYPE MATCHES "Debug")
        message("generating debug configuration")
        target_compile_definitions(${TARGET_NAME} PRIVATE _DEBUG)
        target_compile_options(${TARGET_NAME} PRIVATE -g)
        target_link_options(${TARGET_NAME} PRIVATE "-Wl,-subsystem,console") 
    else()
        if(CMAKE_BUILD_TYPE MATCHES "MinSizeRel")
            # target_compile_options flto might failed, so disable that
            # target_compile_options(${TARGET_NAME} PRIVATE "-flto")
            target_link_options(${TARGET_NAME} PRIVATE "-s" "-flto")
        endif()
        target_link_options(${TARGET_NAME} PRIVATE "-Wl,-subsystem,windows")
    endif()

    # config compile 
    set(CMAKE_C_VISIBILITY_PRESET hidden)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    target_compile_options(${TARGET_NAME} PRIVATE
        -Wno-invalid-source-encoding
    )
    target_compile_definitions(${TARGET_NAME} PRIVATE
        ${CONSCRIPTER_DEFINES}
        -D_WIN32_WINNT=0X0400 # support win xp
        _WIN32
        XMD_H # fix jpeg IN32 problem in jmorecfg.h
        USE_PARALLEL
        USE_GLES
        USE_FILELOG
    )
    target_include_directories(${TARGET_NAME} PRIVATE
        ${STATIC_PORT_ROOT}/include
    )
    target_link_directories(${TARGET_NAME} PRIVATE
        ${STATIC_PORT_ROOT}/lib
    )
    target_link_options(${TARGET_NAME} PRIVATE 
        -Wl,--subsystem,windows:4.0
        -static-libgcc
        -static-libstdc++
    )

    target_link_libraries(${TARGET_NAME} PRIVATE # static
        -Wl,-Bstatic
        lua
        bz2
        
        mingw32
        SDL2
        SDL2main
        SDL2_image
        SDL2_ttf
        SDL2_mixer
        # ssp should be used for linux mingw
        ssp
        
        -Wl,-Bdynamic
        dinput8 shell32 setupapi advapi32 uuid version oleaut32 ole32 
        imm32 winmm gdi32 user32 rpcrt4
    )
    if (EXTRA_SDL_LIB) # cmake sdl2_ttf will need freetype
        target_link_libraries(${TARGET_NAME} PRIVATE # static
            -Wl,-Bstatic
            freetype
        )
    endif()
endfunction()

function(config_windows_msvc TARGET_NAME)
    add_executable(${TARGET_NAME} WIN32 
        ${CONSCRIPTER_CODE}
        ${PROJECT_SOURCE_DIR}/asset/icon1.rc)
    if(MSVC)
        # get package with cpp package manager like vcpkg
        find_package(Lua REQUIRED)
        find_package(SDL2 CONFIG REQUIRED)
        find_package(SDL2_image CONFIG REQUIRED)
        find_package(SDL2_ttf CONFIG REQUIRED)
        find_package(SDL2_mixer CONFIG REQUIRED)
        find_package(BZip2 REQUIRED)
        find_package(JPEG REQUIRED)
        find_package(Ogg CONFIG REQUIRED)
        find_package(Vorbis CONFIG REQUIRED)

        message("MSVC enviroment for ${TARGET_NAME}, type ${CMAKE_BUILD_TYPE}")
        if(CMAKE_BUILD_TYPE MATCHES "Debug")
            message("generating debug configuration")
            target_compile_definitions(${TARGET_NAME} PRIVATE _DEBUG)
            target_compile_options(${TARGET_NAME} PRIVATE -g)
            target_link_options(${TARGET_NAME} PRIVATE "/SUBSYSTEM:CONSOLE") 
        else()
            target_link_options(${TARGET_NAME} PRIVATE "/SUBSYSTEM:WINDOWS")
        endif()

        target_compile_definitions(${TARGET_NAME} PRIVATE 
            ${CONSCRIPTER_DEFINES}
            _WIN32
            USE_PARALLEL
            USE_GLES
            USE_FILELOG
        )

        # currently (Windows 11), Windows only supports IA32 (x86) / AMD64 / ARM64
        if(${CMAKE_VS_PLATFORM_NAME} STREQUAL "ARM64")
            # ARM64
            message("ARM NEON vector extension is enabled")
            target_compile_definitions(${TARGET_NAME} PRIVATE 
                USE_SIMD
                USE_SIMD_ARM_NEON
            )
        else()
            # x86(-64)
            message("x86 SSE2 vector extension is enabled")
            target_compile_definitions(${TARGET_NAME} PRIVATE 
                USE_SIMD
                USE_SIMD_X86_SSE2
            )
        endif()
        # include
        target_include_directories(${TARGET_NAME} PRIVATE ${LUA_INCLUDE_DIR})
        target_include_directories(${TARGET_NAME} PRIVATE ${JPEG_INCLUDE_DIR})
        # link
        target_link_libraries(${TARGET_NAME} PRIVATE ${JPEG_LIBRARIES})
        target_link_libraries(${TARGET_NAME} PRIVATE Vorbis::vorbis)
        target_link_libraries(${TARGET_NAME} PRIVATE Vorbis::vorbisfile)
        target_link_libraries(${TARGET_NAME} PRIVATE Vorbis::vorbisenc)
        target_link_libraries(${TARGET_NAME} PRIVATE Ogg::ogg)
        target_link_libraries(${TARGET_NAME} PRIVATE ${LUA_LIBRARIES})
        target_link_libraries(${TARGET_NAME}
            PRIVATE
            $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
            $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
        )
        target_link_libraries(${TARGET_NAME}
            PRIVATE
            $<IF:$<TARGET_EXISTS:SDL2_image::SDL2_image>,SDL2_image::SDL2_image,SDL2_image::SDL2_image-static>
        )
        target_link_libraries(${TARGET_NAME}
            PRIVATE
            $<IF:$<TARGET_EXISTS:SDL2_ttf::SDL2_ttf>,SDL2_ttf::SDL2_ttf,SDL2_ttf::SDL2_ttf-static>
        )
        target_link_libraries(${TARGET_NAME}
            PRIVATE
            $<IF:$<TARGET_EXISTS:SDL2_mixer::SDL2_mixer>,SDL2_mixer::SDL2_mixer,SDL2_mixer::SDL2_mixer-static>
        )
        target_link_libraries(${TARGET_NAME} PRIVATE BZip2::BZip2)
        
        # use /MT
        set_property(TARGET ${TARGET_NAME} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        message("Windows enviroment for ${TARGET_NAME} not supported!")
    endif()
endfunction()

function(config_web TARGET_NAME)
    add_executable(${TARGET_NAME} ${CONSCRIPTER_CODE})
    message("Web enviroment for "  ${TARGET_NAME})
    target_link_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/thirdparty/build/arch_wasm/lib
    )
    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/thirdparty/build/arch_wasm/include
    )

    if(CMAKE_BUILD_TYPE MATCHES "MinSizeRel")
        target_compile_options(${TARGET_NAME} PRIVATE "-flto")
        # CONscripter: no -s (strip): it drops the names STACK_OVERFLOW_CHECK=2's pass looks up
        target_link_options(${TARGET_NAME} PRIVATE "-flto")
    endif()

    # pass flags for emcc
    set(EMCC_FLAGS "-o ${TARGET_NAME}.js -sASYNCIFY -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB") # this two is very important! 
    # CONscripter: the C stack can't grow (emscripten's default is 64 KB, and an
    # overflow silently overwrote the globals below it: two start-up crashes);
    # every stack pointer move is checked, so an overflow aborts with "stack
    # overflow". ASYNCIFY_STACK_SIZE holds the wasm locals of the call chain
    # unwound at each sleep (default 4 KB; too small traps "unreachable").
    set(EMCC_FLAGS "${EMCC_FLAGS} -sSTACK_SIZE=1MB -sSTACK_OVERFLOW_CHECK=2 -sASYNCIFY_STACK_SIZE=65536")
    # frames over CONS_MAX_FRAME (cons_frame_guard) come from wasm-ld under LTO: make them errors
    set(EMCC_FLAGS "${EMCC_FLAGS} -Wframe-larger-than=${CONS_MAX_FRAME} -Wl,--fatal-warnings")
    set(EMCC_FLAGS "${EMCC_FLAGS} -sMODULARIZE -sINVOKE_RUN=0 -sEXPORT_NAME=cons") # web players call cons(module)
    set(EMCC_FLAGS "${EMCC_FLAGS} -sEXPORTED_RUNTIME_METHODS=callMain,UTF8ToString,FS,PATH,IDBFS") # make as module
    set(EMCC_FLAGS "${EMCC_FLAGS} -sUSE_SDL=2 -sUSE_SDL_TTF=2 -sUSE_BZIP2=1")
    set(EMCC_FLAGS "${EMCC_FLAGS} -sUSE_SDL_IMAGE=2 -sSDL2_IMAGE_FORMATS=bmp,png,jpg")
    set(EMCC_FLAGS "${EMCC_FLAGS} -sUSE_SDL_MIXER=2 -sSDL2_MIXER_FORMATS=ogg,mp3,mid -sUSE_OGG=1 -sUSE_VORBIS=1 -sUSE_MPG123=1")
    set(EMCC_FLAGS "${EMCC_FLAGS} -sFORCE_FILESYSTEM -lidbfs.js --use-preload-plugins --pre-js ${PROJECT_SOURCE_DIR}/src/cons_web/audio_pre.js")
    set_target_properties(${TARGET_NAME} PROPERTIES LINK_FLAGS ${EMCC_FLAGS})
    target_compile_options(${TARGET_NAME} PRIVATE 
        -Wno-invalid-source-encoding
    )
    target_compile_definitions(${TARGET_NAME} PRIVATE
        ${CONSCRIPTER_DEFINES}
        WEB
        LINUX
        USE_PARALLEL
        USE_WEBP
    )
    target_link_libraries(${TARGET_NAME} PRIVATE
        lua
        webpdecoder
    )
endfunction()

function(config_android TARGET_NAME)
    add_library(${TARGET_NAME} SHARED ${CONSCRIPTER_CODE})
    message("Android enviroment for ${TARGET_NAME}, type ${CMAKE_BUILD_TYPE}, compiler ${CMAKE_C_COMPILER}")
    if(CMAKE_BUILD_TYPE MATCHES "Debug")
        message("generating debug configuration")
        target_compile_definitions(${TARGET_NAME} PRIVATE _DEBUG)
        target_compile_options(${TARGET_NAME} PRIVATE -g)
    endif()

    if(CMAKE_BUILD_TYPE MATCHES "MinSizeRel")
        target_compile_options(${TARGET_NAME} PRIVATE "-flto")
        target_link_options(${TARGET_NAME} PRIVATE "-s" "-flto")
    endif()

    target_compile_definitions(${TARGET_NAME} PRIVATE
        ${CONSCRIPTER_DEFINES} 
        ANDROID
        USE_PARALLEL
        USE_GLES
        USE_FILELOG
        UTF8_FILESYSTEM
        USE_WEBP
        SDL_JAVA_PACKAGE_PATH=io_github_blankiekun_conscripter
    )
    target_compile_options(${TARGET_NAME} PRIVATE 
        -Wno-invalid-source-encoding
    )
    target_link_libraries(${TARGET_NAME} PRIVATE
        lua
        jpeg
        bz2
        SDL2
        SDL2_ttf
        SDL2_image
        SDL2_mixer
        webpdecoder
        GLESv2
        log
    )
    config_simd(${TARGET_NAME})
endfunction()

function(config_platform TARGET_NAME)
    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        config_linux(${TARGET_NAME})
    elseif(CMAKE_SYSTEM_NAME MATCHES "Windows")
        if(MSVC)
            config_windows_msvc(${TARGET_NAME})
        else()
            if (DEFINED STATIC_PORT_ROOT) 
                config_windows_mingw(${TARGET_NAME})
            else() 
                config_windows_msys2(${TARGET_NAME}) 
            endif()
        endif()
    elseif(CMAKE_SYSTEM_NAME MATCHES "Emscripten")
        config_web(${TARGET_NAME})
        add_custom_command(TARGET ${TARGET_NAME} PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy 
                ${CMAKE_SOURCE_DIR}/src/cons_web/*.html ${CMAKE_BINARY_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy 
                ${CMAKE_SOURCE_DIR}/src/cons_web/*.py ${CMAKE_BINARY_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy 
                ${CMAKE_SOURCE_DIR}/asset/icon1.ico ${CMAKE_BINARY_DIR}/favicon.ico
        )
    elseif(CMAKE_SYSTEM_NAME MATCHES "Android")
        config_android(${TARGET_NAME})
    elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
        config_darwin(${TARGET_NAME})
    else()
        message("${CMAKE_SYSTEM_NAME} enviroment for ${TARGET_NAME} not supported!")
    endif()
endfunction()

# CONscripter: no function may keep more than CONS_MAX_FRAME bytes on the stack
# (big buffers go on the heap): the web build's stack is 1 MB in all and two
# 64 KB buffers on it crashed it at start-up. An error for gcc and clang builds.
set(CONS_MAX_FRAME 16384)
function(cons_frame_guard TARGET_NAME)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${TARGET_NAME} PRIVATE -Werror=frame-larger-than=${CONS_MAX_FRAME})
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE -Wframe-larger-than=${CONS_MAX_FRAME} -Werror=frame-larger-than)
    endif()
endfunction()

set(CMAKE_CXX_STANDARD 11)
set(CONSCRIPTER_CODE
    src/cons/AnimationInfo.cpp
    src/cons/coding2utf16.cpp
    src/cons/DirectReader.cpp
    src/cons/DirtyRect.cpp
    src/cons/FontInfo.cpp
    src/cons/gbk2utf16.cpp
    src/cons/LUAHandler.cpp
    src/cons/NsaReader.cpp
    src/cons/ONScripter.cpp
    src/cons/ONScripter_animation.cpp
    src/cons/ONScripter_command.cpp
    src/cons/ONScripter_cons.cpp
    src/cons/ONScripter_spriteeffects.cpp
    src/cons/ONScripter_audio.cpp
    src/cons/ONScripter_trace.cpp
    src/cons/ONScripter_savepoint.cpp
    src/cons/ONScripter_strtab.cpp
    src/cons/ONScripter_names.cpp
    src/cons/ONScripter_warp.cpp
    src/cons/ONScripter_screen.cpp
    src/cons/ONScripter_effect.cpp
    src/cons/ONScripter_effect_breakup.cpp
    src/cons/ONScripter_event.cpp
    src/cons/ONScripter_file.cpp
    src/cons/ONScripter_file2.cpp
    src/cons/ONScripter_image.cpp
    src/cons/ONScripter_lut.cpp
    src/cons/onscripter_main.cpp
    src/cons/ONScripter_rmenu.cpp
    src/cons/ONScripter_sound.cpp
    src/cons/ONScripter_text.cpp
    src/cons/Parallel.cpp
    src/cons/resize_image.cpp
    src/cons/SarReader.cpp
    src/cons/ScriptHandler.cpp
    src/cons/ScriptParser.cpp
    src/cons/ScriptParser_command.cpp
    src/cons/sjis2utf16.cpp
    src/cons/renderer/gles_renderer.cpp
    src/cons/builtin_dll/layer_oldmovie.cpp
    src/cons/builtin_dll/layer_snow.cpp
    src/cons/builtin_dll/layer_particle.cpp
    src/cons/builtin_dll/ONScripter_effect_cascade.cpp
    src/cons/builtin_dll/ONScripter_effect_trig.cpp
)
set(CONSCRIPTER_DEFINES
    USE_LUA
    USE_BUILTIN_LAYER_EFFECTS
    ENABLE_1BYTE_CHAR
)
config_platform(${PROJECT_NAME})
cons_frame_guard(${PROJECT_NAME})
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/cons_opus.cmake) # CONscripter: Opus sounds
cons_opus(${PROJECT_NAME})
