/* -*- C++ -*-
 * 
 *  onscripter_main.cpp -- main function of ONScripter
 *
 *  Copyright (c) 2001-2018 Ogapee. All rights reserved.
 *            (c) 2014-2019 jh10001 <jh10001@live.cn>
 *            (c) 2022-2023 yurisizuku <https://github.com/YuriSizuku>
 *
 *  ogapee@aqua.dti2.ne.jp
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "ONScripter.h"
#include "Utils.h"
#include "gbk2utf16.h"
#include "sjis2utf16.h"
#include "version.h"
#include "stdlib.h"
#include <ctype.h>
#include <string>
#include <vector>

ONScripter ons;
Coding2UTF16 *coding2utf16 = NULL;
std::string g_stdoutpath = "stdout.txt";
std::string g_stderrpath = "stderr.txt";
static std::string g_bad_options; // CONscripter: unknown options and stray words, a startup failure
static bool g_options_file = false; // reading ons_args: an unknown option there is only reported

#if defined(IOS)
#import <Foundation/NSArray.h>
#import <UIKit/UIKit.h>
#import "DataCopier.h"
#import "DataDownloader.h"
#import "ScriptSelector.h"
#import "MoviePlayer.h"
#endif

#if defined(ANDROID)
#include <unistd.h>
#endif

#if defined(WINRT)
#include "ScriptSelector.h"
#endif

void optionHelp()
{
    printf( "Usage: conscripter [option ...]\n" );
    printf( "  -h, --help\t\tshow this help and exit\n");
    printf( "  -v, --version\t\tshow the version information and exit\n\n");
    
    printf( " load options: \n");
    printf( "  -f, --font file\tset a TTF font file\n");
    printf( "  -r, --root path\tset the root path to the archives\n");
    printf( "      --save-dir\tset save dir (made if it doesn't exist)\n");
    printf( "      --debug:1\t\tprint debug info\n");
    printf( "      --trace file\twrite a debug trace, one JSON line per text page (or CONS_TRACE=file)\n");
    printf( "      --trace-ignore 22-48,lsp2:12-23\tleave these sprites out of the trace (or CONS_TRACE_IGNORE)\n");
    printf( "      --trace-vars 300-899,$10-20\twith --trace, a record for each change of these variables\n");
    printf( "      --trace-layers\twith --trace, a \"layers\" record on each print or effect: what is drawn, front to back\n");
    printf( "      --trace-long-wait ms\twith --trace, warn of a wait, effect or fade asked to last longer (10000)\n");
    printf( "      --audio-stats\twith --trace, an \"audio\" record every 100 ms of sound, per channel and for the mix\n");
    printf( "      --trace-max-image mb\twith --trace, warn of a single picture taking more memory than this (64)\n");
    printf( "      --roundtrip-every n\twith --trace and --auto-play, save and load every n pages and compare (--roundtrip-slot, 999)\n");
    printf( "      --auto-play pages\tplay by itself, without waiting, for this many pages (0: to the end)\n");
    printf( "      --no-text-overlay\tweb: the page gets no text, choices or backlog (consText, consChoice, consSprites, consBacklog, consOverlay), so draws no text overlay\n");
    printf( "      --real-time\twith --auto-play, effects, waits and fades take their real time (sprite movement plays out)\n");
    printf( "      --auto-choice first|cycle|random|untaken\thow --auto-play picks a button or choice\n");
    printf( "      --skip-screens kinds\twith --auto-play, close screens of these kinds at once (save,load,settings,backlog,extras; none: play them all)\n");
    printf( "      --replay trace.jsonl\twith --auto-play, make the choices a trace recorded, then --auto-choice\n");
    printf( "      --known-labels file\twith --stop-known, the labels earlier runs reached (one name a line)\n");
    printf( "      --stop-known n\twith --auto-play, stop after n pages without a label or choice earlier runs hadn't reached\n");
    printf( "      --taken file\twith --auto-choice untaken, the choices made before (\"*label:line<TAB>option<TAB>count\" lines), to take least\n");
    printf( "      --choose '*label:line=N'\twith --auto-play, take option N at that choice (N,M,...: on each visit in turn); repeatable\n");
    printf( "      --choices file\twith --auto-play, take these choices in order (\"*label:line<TAB>option\" lines), then --replay, --auto-choice\n");
    printf( "      --seed n\t\tstart the random numbers (rnd, --auto-choice random) from n\n");
    printf( "      --shot-pages 412,980-990\twith --trace, save a picture of these pages (--shot-dir, default shots)\n");
    printf( "      --shot-every n\twith --trace, save a picture of every n-th page\n");
    printf( "      --shot-at '*label:line[+ms]'\twith --trace, a picture and a dump record each time that line has run, or ms later (comma-separated, repeatable)\n");
    printf( "      --shot-after ms\twith --trace, a picture and a dump record that many ms after the start (comma-separated, repeatable)\n");
    printf( "      --shot-effects pcts\twith --trace, each effect drawn at these percents of its time (comma-separated; only on --shot-pages/--shot-every pages when given)\n");
    printf( "      --goto label\tstart at this label, once the define section has run\n");
    printf( "      --vars file\tset these variables at the start (lines %%312=876, $5=text; UTF-8)\n");
    printf( "      --load n\t\tstart by loading save slot n\n");
    printf( "      --checkpoint-every n\twith --auto-play, save every n pages, to slots 900, 901, ... (--checkpoint-slot)\n");
    printf( "      --checkpoint-start\twith --goto, save at the label before any of it runs (--checkpoint-slot)\n");
    printf( "      --until label\tend the run when the script first reaches this label\n");
    printf( "      --max-pages n\tend the run after n text pages\n");
    printf( "      --dump-vars file\twrite the variables in that form at F8 and at exit\n");
    printf( "      --enc:[gbk|sjis|utf8]\tdefine the encoding of script\n");
    printf( "      (exit codes: 0 ok, 1 --auto-play stuck, 2 script error, 3 startup failure, 128+n signal n)\n\n");

    printf( " render options: \n");
    printf( "      --window\t\tstart in windowed mode\n");
    printf( "      --width 1280\tforce window width\n");
    printf( "      --height 720\tforce window height\n");
    printf( "      --fullscreen\tstart in fullscreen mode (alt+f or alt+enter)\n");
    printf( "      --fullscreen2\tstart in fullscreen mode with stretch (f10 toggle stretch)\n");
    printf( "      --sharpness 3.1 \t use gles to make image sharp\n");
    printf( "      --filter crt,pc98\tshow the screen through filters, in order: crt, pc98, gameboy, sepia, mono, vhs\n");
    printf( "      --no-video\tdo not decode video\n");
    printf( "      --no-vsync\tturn off vsync\n\n");
    
    printf( " other options: \n");
    printf( "      --cdaudio\t\tuse CD audio if available\n");
    printf( "      --cdnumber no\tchoose the CD-ROM drive number\n");
    printf( "      --registry file\tset a registry file\n");
    printf( "      --dll file\tset a dll file\n");
    printf( "      --enable-wheeldown-advance\tadvance the text on mouse wheel down\n");
    printf( "      --disable-rescale\tdo not rescale the images in the archives\n");
    printf( "      --force-button-shortcut\tignore useescspc and getenter command\n");
    printf( "      --render-font-outline\trender the outline of a text instead of casting a shadow\n");
    printf( "      --edit\t\tenable online modification of the volume and variables when 'z' is pressed\n");
    printf( "      --key-exe file\tset a file (*.EXE) that includes a key table\n");
    printf( "      --fontcache\tcache default font\n");
    exit(0);
}

void optionVersion()
{
    printf("CONscripter %s <https://github.com/blankiekun/CONscripter>\n", CONSCRIPTER_VERSION);
    printf("a fork of ONScripter-Yuri, based on ONScripter-Jh and ONScripter\n\n");
    printf("Written by Ogapee <ogapee@aqua.dti2.ne.jp>\n\n");
    printf("Copyright (c) 2001-2018 Ogapee.\n\
                (c) 2014-2018 jh10001<jh10001@live.cn>\n\
                (c) 2022-2023 yurisizuku <https://github.com/YuriSizuku>\n");
    printf("This is free software; see the source for copying conditions.\n");
    exit(0);
}

#if defined(ANDROID)
extern "C"
{
#include <jni.h>
#include <android/log.h>
#include <errno.h>
static JavaVM *jniVM = NULL;
static jobject JavaONScripter = NULL;
static jmethodID JavaPlayVideo = NULL;
static jmethodID JavaGetFD = NULL;
static jmethodID JavaMkdir = NULL;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    jniVM = vm;
    return JNI_VERSION_1_2;
};

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved)
{
    jniVM = vm;
};

#ifndef SDL_JAVA_PACKAGE_PATH
#error You have to define SDL_JAVA_PACKAGE_PATH to your package path with dots replaced with underscores, for example "com_example_SanAngeles"
#endif
#define JAVA_EXPORT_NAME2(name,package) Java_##package##_##name
#define JAVA_EXPORT_NAME1(name,package) JAVA_EXPORT_NAME2(name,package)
#define JAVA_EXPORT_NAME(name) JAVA_EXPORT_NAME1(name,SDL_JAVA_PACKAGE_PATH)

JNIEXPORT jint JNICALL JAVA_EXPORT_NAME(ONScripter_nativeInitJavaCallbacks) (JNIEnv * jniEnv, jobject thiz)
{
    JavaONScripter = jniEnv->NewGlobalRef(thiz);
    jclass JavaONScripterClass = jniEnv->GetObjectClass(JavaONScripter);
    JavaPlayVideo = jniEnv->GetMethodID(JavaONScripterClass, "playVideo", "([B)V");
    JavaGetFD = jniEnv->GetMethodID(JavaONScripterClass, "getFD", "([BI)I");
    JavaMkdir = jniEnv->GetMethodID(JavaONScripterClass, "mkdir", "([B)I");
    return 0;
}

JNIEXPORT jint JNICALL 
JAVA_EXPORT_NAME(ONScripter_nativeGetWidth) ( JNIEnv*  env, jobject thiz )
{
    return ons.getWidth();
}

JNIEXPORT jint JNICALL 
JAVA_EXPORT_NAME(ONScripter_nativeGetHeight) ( JNIEnv*  env, jobject thiz )
{
    return ons.getHeight();
}

void playVideoAndroid(const char *path)
{
    JNIEnv * jniEnv = NULL;
    jniVM->AttachCurrentThread(&jniEnv, NULL);

    if (!jniEnv){
        __android_log_print(ANDROID_LOG_ERROR, "ONS", "ONScripter::playVideoAndroid: Java VM AttachCurrentThread() failed");
        return;
    }

    jbyte *jb = new jbyte[strlen(path)];
    for (int i=0;i<strlen(path);i++) jb[i] = path[i];
    jbyteArray jba = jniEnv->NewByteArray(strlen(path));
    jniEnv->SetByteArrayRegion(jba, 0, strlen(path), jb);
    jniEnv->CallVoidMethod(JavaONScripter, JavaPlayVideo, jba);
    jniEnv->DeleteLocalRef(jba);
    delete[] jb;
}

#include <sys/stat.h>
int stat_ons(const char *path, struct stat * statbuf){
    int res = stat(path, statbuf);
    if(res!=-1 ) return res;

    JNIEnv * jniEnv = NULL;
    jniVM->AttachCurrentThread(&jniEnv, NULL);
    jbyte *jb = new jbyte[strlen(path)];
    for (int i=0;i<strlen(path);i++) jb[i] = path[i];
    jbyteArray jba = jniEnv->NewByteArray(strlen(path));
    jniEnv->SetByteArrayRegion(jba, 0, strlen(path), jb);
    int fd = jniEnv->CallIntMethod(JavaONScripter, JavaGetFD, jba, 0);
    jniEnv->DeleteLocalRef(jba);
    delete[] jb;
    res = fstat(fd, statbuf);
    close(fd);
    return res;
}

#undef fopen
FILE *fopen_ons(const char *path, const char *mode)
{
    int mode2 = 0;
    if (mode[0] == 'w') mode2 = 1;

    FILE *fp = fopen(path, mode);
    // __android_log_print(ANDROID_LOG_INFO, "## cons",  "fopen_ons %s, %s, %p", path, mode, fp);

    if (fp) return fp;
    
    JNIEnv * jniEnv = NULL;
    jniVM->AttachCurrentThread(&jniEnv, NULL);

    if (!jniEnv){
        __android_log_print(ANDROID_LOG_ERROR, "ONS", "ONScripter::getFD: Java VM AttachCurrentThread() failed");
        return NULL;
    }

    jbyte *jb = new jbyte[strlen(path)];
    for (int i=0;i<strlen(path);i++) jb[i] = path[i];
    jbyteArray jba = jniEnv->NewByteArray(strlen(path));
    jniEnv->SetByteArrayRegion(jba, 0, strlen(path), jb);
    int fd = jniEnv->CallIntMethod(JavaONScripter, JavaGetFD, jba, mode2 );
    jniEnv->DeleteLocalRef(jba);
    delete[] jb;

    return fdopen(fd, mode);
}

#undef mkdir
extern int mkdir(const char *pathname, mode_t mode);
int mkdir_ons(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0 || errno != EACCES) return 0;

    JNIEnv * jniEnv = NULL;
    jniVM->AttachCurrentThread(&jniEnv, NULL);

    if (!jniEnv){
        __android_log_print(ANDROID_LOG_ERROR, "ONS", "ONScripter::mkdir: Java VM AttachCurrentThread() failed");
        return -1;
    }

    jbyte *jb = new jbyte[strlen(path)];
    for (int i=0;i<strlen(path);i++) jb[i] = path[i];
    jbyteArray jba = jniEnv->NewByteArray(strlen(path));
    jniEnv->SetByteArrayRegion(jba, 0, strlen(path), jb);
    int ret = jniEnv->CallIntMethod(JavaONScripter, JavaMkdir, jba);
    jniEnv->DeleteLocalRef(jba);
    delete[] jb;

    return ret;
}
}
#endif

#if defined(IOS)
extern "C" void playVideoIOS(const char *filename, bool click_flag, bool loop_flag)
{
    NSString *str = [[NSString alloc] initWithUTF8String:filename];
    id obj = [MoviePlayer alloc];
    [[obj init] play:str click : click_flag loop : loop_flag];
    [obj release];
}
#endif

#if defined(WEB)
#include <emscripten.h>

#undef fopen

/*
* fetch the file from server beforce fopen for lazyload
* value: g_cons_module, g_cons_index, g_cons_filemap
* function: fetch_file
*/
FILE *fopen_ons(const char *path, const char *mode)
{
    // printf("## fopen_ons %s, ", path);
    static int use_lazyload = -1;
    if(use_lazyload==-1)
    {
        use_lazyload = EM_ASM_INT(
            // check enviroment
            if (!g_cons_module) return 0;
            if (!g_cons_index) return 0;
            if (!fetch_file) return 0;

            // check lazyload flag with filemap
            if (!g_cons_filemap) return 0;
            if (Object.keys(g_cons_filemap).length==0) return 0;
            return 1;
        );
    }
    
    FILE *fp = fopen(path, mode);
    if(!fp && use_lazyload==1 && (strcmp(mode, "r") || strcmp(mode, "rb")))
    {   
        int ret = 0;
        ret = EM_ASM_INT( // path is combined after --gamedir
            var path = g_cons_module.UTF8ToString($0);
            var key = path.toLowerCase(); // pay attention to the case sensitive of web
            if(!g_cons_filemap[key]) return 0;
            if(g_cons_filemap[key].loaded) return 2;
            try {
                fetch_file(g_cons_module.FS, key, g_cons_filemap);
                return 1;
            } 
            catch(e) { 
                console.log("## fopen_ons web lazyload ${path} error: ", e);
                return 0;
            }, path);
        switch (ret)
        {
            case 0: 
            {
                // printf(" not found in g_cons_filemap !\n");
                return fp; // use fopen to check for other file mount
                break;
            }
            case 1:
            {
                // printf(" waiting for fetch...\n");
                break;
            }
            case 2:
            {
                // printf(" already loaded!\n");
                return fp;
                break;
            }
        }

        while(!EM_ASM_INT(
            var path = g_cons_module.UTF8ToString($0);
            var key = path.toLowerCase();
            return g_cons_filemap[key].loaded;, path))
        {
            SDL_Delay(5); // wait for async function
        }
        fp = fopen(path, mode); // reload after fetch
    }
    return fp;
}

// CONscripter: a host page that reads game files on demand (the site's player) is
// asked for the bytes before fread() needs them, and loads them asynchronously
// while the engine waits: its synchronous read would make Firefox suspend the
// AudioContext, a gap in the sound at each voice, sound effect or picture.
extern "C" void webPreload(FILE *fp, size_t offset, size_t length)
{
    while (!EM_ASM_INT(return !self.consPreload || self.consPreload($0, $1 >>> 0, $2 >>> 0) ? 1 : 0;,
                       fileno(fp), offset, length))
        SDL_Delay(1);
}

extern "C" bool playVideoWeb(const char *path, bool click_flag, bool loop_flag)
{
    EM_ASM(
        var path = g_cons_module.UTF8ToString($0);
        playVideo(path, $1, $2);
    ,path,click_flag,loop_flag);

    while(EM_ASM_INT(return g_cons_module.wait_video;))
    {
        SDL_Delay(5);
    }
    return EM_ASM_INT(return g_cons_module.video_skipped ? 1 : 0;); // CONscripter
}
#endif

// CONscripter: steps to an option's value; an option without one is
// reported (a startup failure, or only noted in ons_args) instead of read past the end
static bool nextArg(int &argc, char **&argv)
{
    if (argc <= 1){
        if (g_options_file) utils::printInfo(" option %s in ons_args needs a value\n", argv[0]);
        else g_bad_options += std::string(g_bad_options.empty() ? "" : ", ") + "option " + argv[0] + " needs a value";
        return false;
    }
    argc--;
    argv++;
    return true;
}

void parseOption(int argc, char *argv[]) {
    while (argc > 0) {
        if ( argv[0][0] == '-' ){
            // version, help
            if ( !strcmp( argv[0]+1, "h" ) || !strcmp( argv[0]+1, "-help" ) ){
                optionHelp();
            }
            else if ( !strcmp( argv[0]+1, "v" ) || !strcmp( argv[0]+1, "-version" ) ){
                optionVersion();
            }

            // load options
            else if ( !strcmp( argv[0]+1, "f" ) || !strcmp( argv[0]+1, "-font" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setFontFile(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "r" ) || !strcmp( argv[0]+1, "-root" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setArchivePath(argv[0]);
            }
            else if ( !strcmp(argv[0]+1, "-save-dir") ){
                if (!nextArg(argc, argv)) break;
                ons.makeSaveDir(argv[0]); // CONscripter
                ons.setSaveDir(argv[0]);
            }
            else if (!strcmp(argv[0]+1, "-debug:1")){
                ons.setDebugLevel(1);
            }
            else if ( !strcmp( argv[0]+1, "-trace" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setTraceFile(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-trace-ignore" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setTraceIgnore(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-auto-play" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setAutoPlay(atoi(argv[0]));
            }
            else if ( !strcmp( argv[0]+1, "-real-time" ) ){
                ons.setAutoPlayReal();
            }
            else if ( !strcmp( argv[0]+1, "-no-text-overlay" ) ){
#if defined(WEB)
                // the text hooks check it; a value the page set (its viewer's choice) wins
                EM_ASM({ if (Module.consNoTextOverlay === undefined) Module.consNoTextOverlay = 1; });
#endif
            }
            else if ( !strcmp( argv[0]+1, "-skip-screens" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setSkipScreens(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-auto-choice" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setAutoChoice(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-shot-pages" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setTraceShots(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-shot-every" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setTraceShotEvery(atoi(argv[0]));
            }
            else if ( !strcmp( argv[0]+1, "-shot-dir" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setTraceShotDir(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-shot-at" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setTraceShotAt(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-shot-after" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setTraceShotAfter(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-shot-effects" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setTraceShotEffects(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-replay" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setReplay(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-taken" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setTaken(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-choose" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setChoose(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-choices" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setChoices(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-known-labels" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setKnownLabels(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-stop-known" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setStopKnown(atoi(argv[0]));
            }
            else if ( !strcmp( argv[0]+1, "-load" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setStartLoad(atoi(argv[0]));
            }
            else if ( !strcmp( argv[0]+1, "-checkpoint-every" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setCheckpoints(atoi(argv[0]), -1);
            }
            else if ( !strcmp( argv[0]+1, "-checkpoint-start" ) ){
                ons.setCheckpointStart();
            }
            else if ( !strcmp( argv[0]+1, "-checkpoint-slot" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setCheckpoints(-1, atoi(argv[0]));
            }
            else if ( !strcmp( argv[0]+1, "-audio-stats" ) ){
                ons.setAudioStats();
            }
            else if ( !strcmp( argv[0]+1, "-trace-layers" ) ){
                ons.setTraceLayers();
            }
            else if ( !strcmp( argv[0]+1, "-trace-long-wait" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setTraceLongWait(atoi(argv[0]));
            }
            else if ( !strcmp( argv[0]+1, "-trace-max-image" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setTraceMaxImage(atoi(argv[0]));
            }
            else if ( !strcmp( argv[0]+1, "-roundtrip-every" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setRoundtrip(atoi(argv[0]), -1);
            }
            else if ( !strcmp( argv[0]+1, "-roundtrip-slot" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setRoundtrip(-1, atoi(argv[0]));
            }
            else if ( !strcmp( argv[0]+1, "-trace-vars" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setTraceVars(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-goto" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setStartGoto(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-vars" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setStartVars(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-dump-vars" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setDumpVars(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-until" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setUntil(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-max-pages" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setMaxPages(atoi(argv[0]));
            }
            else if ( !strcmp( argv[0]+1, "-seed" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setSeed(atoi(argv[0]));
            }
            else if (!strcmp(argv[0]+1, "-enc:sjis")){
                if(!coding2utf16) coding2utf16 = new SJIS2UTF16();
            }
            else if (!strcmp(argv[0]+1, "-enc:gbk")){
                if(!coding2utf16) coding2utf16 = new GBK2UTF16();
            }
            else if (!strcmp(argv[0]+1, "-enc:utf8")){
                if(!coding2utf16) coding2utf16 = new GBK2UTF16();
                coding2utf16->force_utf8 = true;
            }

            // render options
            else if ( !strcmp( argv[0]+1, "-window" ) ){
                ons.setWindowMode();
            }
            else if ( !strcmp( argv[0]+1, "-width" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setWindowWidth(atoi(argv[0]));
            }
            else if ( !strcmp( argv[0]+1, "-height" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setWindowHeight(atoi(argv[0]));
            }
            else if ( !strcmp( argv[0]+1, "-fullscreen" ) ){
                ons.setFullscreenMode(1);
            }
            else if ( !strcmp( argv[0]+1, "-fullscreen2" ) ){
                ons.setFullscreenMode(2);
            }
            else if ( !strcmp( argv[0]+1, "-filter" ) ){
                if (!nextArg(argc, argv)) break;
                if (!ons.setScreenFilter(argv[0]))
                    g_bad_options += std::string(g_bad_options.empty() ? "" : ", ") + "unknown --filter " + argv[0] + " (crt, pc98, gameboy, sepia, mono, vhs)";
            }
            else if ( !strcmp( argv[0]+1, "-sharpness" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setSharpness(atof(argv[0]));
            }
            else if (!strcmp(argv[0]+1, "-no-video")){
			    ons.setVideoOff();
			}
            else if (!strcmp(argv[0]+1, "-no-vsync")){
			    ons.setVsyncOff();
			}

            // other options
            else if ( !strcmp( argv[0]+1, "-cdaudio" ) ){
                ons.enableCDAudio();
            }
            else if ( !strcmp( argv[0]+1, "-cdnumber" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setCDNumber(atoi(argv[0]));
            }
            else if ( !strcmp( argv[0]+1, "-registry" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setRegistryFile(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-dll" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setDLLFile(argv[0]);
            }
            else if ( !strcmp( argv[0]+1, "-force-button-shortcut" ) ){
                ons.enableButtonShortCut();
            }
            else if ( !strcmp( argv[0]+1, "-enable-wheeldown-advance" ) ){
                ons.enableWheelDownAdvance();
            }
            else if ( !strcmp( argv[0]+1, "-disable-rescale" ) ){
                ons.disableRescale();
            }
            else if ( !strcmp( argv[0]+1, "-render-font-outline" ) ){
                ons.renderFontOutline();
            }
            else if ( !strcmp( argv[0]+1, "-edit" ) ){
                ons.enableEdit();
            }
            else if ( !strcmp( argv[0]+1, "-key-exe" ) ){
                if (!nextArg(argc, argv)) break;
                ons.setKeyEXE(argv[0]);
            }
            else if (!strcmp(argv[0]+1, "-fontcache")){
                ons.setFontCache();
            }
            else if (g_options_file){
                utils::printInfo(" unknown option %s in ons_args\n", argv[0]);
            }
            else{
                g_bad_options += std::string(g_bad_options.empty() ? "" : ", ") + "unknown option " + argv[0];
            }
        }
        else if (g_options_file){
            utils::printInfo(" not an option in ons_args: %s\n", argv[0]);
        }
        else{
            g_bad_options += std::string(g_bad_options.empty() ? "" : ", ") + "not an option: " + argv[0];
        }
        argc--;
        argv++;
    }
}

int main(int argc, char *argv[])
{
#if defined(PSP)
    ons.disableRescale();
    ons.enableButtonShortCut();
    SetupCallbacks();
#elif defined(WINRT)
    {
        ScriptSelector ss;
        ons.setArchivePath(ss.selectedPath.c_str());
    }
    ons.disableRescale();
#elif defined(ANDROID)
    ons.enableButtonShortCut();
#endif

#if defined(IOS)
#if defined(HAVE_CONTENTS)
    if ([[[DataCopier alloc] init] copy]) exit(-1);
#endif

    // scripts and archives are stored under /Library/Caches
    NSArray* cpaths = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    NSString* cpath = [[cpaths objectAtIndex : 0] stringByAppendingPathComponent:@"ONS"];
    char filename[256];
    strcpy(filename, [cpath UTF8String]);
    ons.setArchivePath(filename);

    // output files are stored under /Documents
    NSArray* dpaths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString* dpath = [[dpaths objectAtIndex : 0] stringByAppendingPathComponent:@"ONS"];
    strcpy(filename, [dpath UTF8String]);
    ons.setSaveDir(filename);

#if defined(ZIP_URL)
    if ([[[DataDownloader alloc] init] download]) exit(-1);
#endif

#if defined(USE_SELECTOR)
    // scripts and archives are stored under /Library/Caches
    cpath = [[[ScriptSelector alloc] initWithStyle:UITableViewStylePlain] select];
    strcpy(filename, [cpath UTF8String]);
    ons.setArchivePath(filename);

    // output files are stored under /Documents
    dpath = [[dpaths objectAtIndex : 0] stringByAppendingPathComponent:[cpath lastPathComponent]];
    NSFileManager *fm = [NSFileManager defaultManager];
    [fm createDirectoryAtPath : dpath withIntermediateDirectories : YES attributes : nil error : nil];
    strcpy(filename, [dpath UTF8String]);
    ons.setSaveDir(filename);
#endif

#if defined(RENDER_FONT_OUTLINE)
    ons.renderFontOutline();
#endif
#endif

    // ----------------------------------------
    // Parse options
    argv++;
    parseOption(argc - 1, argv);

    utils::printInfo("CONscripter %s, (Jh%s,Ons%s,NSC%d.%02d)\n", 
        CONSCRIPTER_VERSION, ONS_JH_VERSION, 
        ONS_VERSION, NSC_VERSION / 100, NSC_VERSION % 100);

    const char *argfilename = "ons_args";
#if defined(WEB)
    // CONscripter: through the lazy loader, as the game's files (a plain
    // fopen finds nothing until the page has fetched the file)
    FILE *(*open_args)(const char *, const char *) = fopen_ons;
#else
    FILE *(*open_args)(const char *, const char *) = fopen;
#endif
    FILE *fp = NULL;
    if (ons.getArchivePath()) {
        size_t len = strlen(ons.getArchivePath()) + strlen(argfilename) + 1;
        char *full_path = new char[len];
        sprintf(full_path, "%s%s", ons.getArchivePath(), argfilename);
        fp = open_args(full_path, "r");
        delete[] full_path;
    }
    else fp = open_args(argfilename, "r");
    if (fp) {
        // CONscripter: tokens of any length and number (was 16 of up to 63 chars)
        std::vector<std::string> tokens(1);
        for (int c ; (c = fgetc(fp)) != EOF ; ){
            if (!isspace(c)) tokens.back() += (char)c;
            else if (!tokens.back().empty()) tokens.emplace_back();
        }
        fclose(fp);
        if (tokens.back().empty()) tokens.pop_back();
        std::vector<char*> args;
        for (std::string &t : tokens) args.push_back(&t[0]);
        args.push_back(NULL);
        g_options_file = true;
        parseOption((int)tokens.size(), args.data());
        g_options_file = false;
    }

    if (coding2utf16 == NULL) coding2utf16 = new GBK2UTF16();
    if (!g_bad_options.empty()){ // CONscripter: once the options are read, so --trace after them counts
        utils::printError("%s (--help lists the options)\n", g_bad_options.c_str());
        ons.startupFailed(); // exits with 3
    }

    // ----------------------------------------
    // Run ONScripter
    if (ons.openScript()) ons.startupFailed(); // exits with 3
    if (ons.init()) ons.startupFailed();
#if defined(WEB)
    EM_ASM(
        self.postMessage("onsinit");
    );
#endif
    ons.executeLabel();
    exit(0);
}
