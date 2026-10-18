/* -*- C++ -*-
 * 
 *  ONScripter.h - Execution block parser of ONScripter
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

#ifndef __ONSCRIPTER_H__
#define __ONSCRIPTER_H__

#include "ScriptParser.h"
#include "DirtyRect.h"
#include "ButtonLink.h"
#include <string>
#include <map>
#include <set>
#include <vector>

#if defined(ANDROID)
#include "SDL.h"
#include "SDL_image.h"
#include "SDL_ttf.h"
#include "SDL_mixer.h"
#else
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_mixer.h>
#endif

#if defined(USE_GLES)
#include "renderer/gles_renderer.h"
#endif

#if defined(USE_SMPEG)
#include <smpeg.h>
#endif

// CONscripter: the engine's clock and timers (ONScripter_trace.cpp). An
// --auto-play run under SDL_VIDEODRIVER=dummy has a virtual clock that moves
// only when the engine waits, so its times, and pictures, repeat run to run.
// (SDL_GetTicks)() and the like, in brackets, are SDL's own.
Uint32 consTicks();
SDL_TimerID consAddTimer( Uint32 ms, SDL_TimerCallback callback, void *param );
SDL_bool consRemoveTimer( SDL_TimerID id );
void consDelay( Uint32 ms );
void consIdle( bool idle );      // the main thread waits (true) or works again (false): ms_cpu
double consFrameWork();          // ms the main thread worked since the last call (wall time, without waits)
bool consClockWait( SDL_Event *event );
#define SDL_GetTicks() consTicks()
#define SDL_AddTimer( ms, callback, param ) consAddTimer( ms, callback, param )
#define SDL_RemoveTimer( id ) consRemoveTimer( id )
#define SDL_Delay( ms ) consDelay( ms )
int consSaveVersion();           // the CONscripter save block's version (ONScripter_cons.cpp)

#define DEFAULT_VIDEO_SURFACE_FLAG (SDL_SWSURFACE)

#define DEFAULT_BLIT_FLAG (0)
//#define DEFAULT_BLIT_FLAG (SDL_RLEACCEL)

// CONscripter: sprites 0-999 are NScripter's numbered ones; 1000 up are the
// handles spnew hands out (ONScripter_names.cpp), drawn by layer
#define NS_SPRITE_NUM 1000
#define SPRITE_HANDLE_NUM 1000
#define MAX_SPRITE_NUM (NS_SPRITE_NUM + SPRITE_HANDLE_NUM)
#define SPTRAIL_MAX 32 // CONscripter: sptrail's most copies (128 cost 29 ms a frame for a 1280x720 sprite natively)
#define MAX_SPRITE2_NUM 256
#define MAX_TEXTURE_NUM 16
#define MAX_PARAM_NUM 100
#define MAX_EFFECT_NUM 256

#define DEFAULT_VOLUME 100
#define ONS_MIX_CHANNELS 50
#define ONS_MIX_EXTRA_CHANNELS 4
#if defined(USE_CONS_OPUS) // CONscripter: Opus sounds, decoded as they play (ONScripter_cons.cpp)
SDL_RWops *opusWavRW(const unsigned char *data, unsigned long length); // as a WAV; NULL when it's no Opus
Mix_Chunk *opusChunk(const unsigned char *data, unsigned long length, const SDL_AudioSpec &spec, void **filling);
bool opusChunkStart(int channel, void *filling, unsigned char *data);
void opusChunkFinish(int channel);
bool opusPiece(const unsigned char *data, unsigned long length, double from, double count, bool loop,
               const SDL_AudioSpec &spec, std::vector<Sint16> *pcm, double *length_s);
#endif
// CONscripter: movies through the player's ffmpeg where nothing else plays them
#if !defined(_WIN32) && !defined(IOS) && !defined(ANDROID) && !defined(WEB) && !defined(__LIBRETRO__) && !defined(USE_SMPEG)
#define CONS_FFMPEG_MOVIES 1
#else
#define CONS_FFMPEG_MOVIES 0
#endif
#define ONS_MIX_FADE_CHANNELS 8 // CONscripter: after the others, for the fade of a cut sound
#define MIX_WAVE_CHANNEL (ONS_MIX_CHANNELS+0)
#define MIX_BGM_CHANNEL (ONS_MIX_CHANNELS+1)
#define MIX_LOOPBGM_CHANNEL0 (ONS_MIX_CHANNELS+2)
#define MIX_LOOPBGM_CHANNEL1 (ONS_MIX_CHANNELS+3)

#define DEFAULT_WM_TITLE "ONScripter"
#define DEFAULT_WM_ICON  "ONScripter"

struct lua_State; // CONscripter: ONScripter_screen.cpp
class ONScripter : public ScriptParser
{
public:
    typedef AnimationInfo::ONSBuf ONSBuf;
    
    struct ButtonState{
        unsigned int event_type;
        unsigned int event_button;
        int x, y, button;
        char str[16];
        bool down_flag;
    };
        
    ONScripter();
    ~ONScripter();

    // ----------------------------------------
    // start-up options
    void enableCDAudio();
    void setCDNumber(int cdrom_drive_number);
    void setFontFile(const char *filename);
    void setRegistryFile(const char *filename);
    void setDLLFile(const char *filename);
    void setArchivePath(const char *path);
    void setSaveDir(const char *path);
    void makeSaveDir(const char *path); // CONscripter
    void setFullscreenMode(int mode=0);
    void setVideoOff();
    void setWindowMode();
    void setVsyncOff();
    void setFontCache();
    void setDebugLevel(int debug);
    void enableButtonShortCut();
    void enableWheelDownAdvance();
    void disableRescale();
    void renderFontOutline();
    void enableEdit();
    void setKeyEXE(const char *path);
    void setTraceFile(const char *path); // CONscripter: --trace (ONScripter_cons.cpp)
    void setTraceIgnore(const char *spec); // CONscripter: --trace-ignore
    void setAutoPlay(int pages); // CONscripter: --auto-play, a run that plays itself
    void setAutoChoice(const char *policy);
    void setSeed(int seed);
    void setReplay(const char *path); // CONscripter: --replay, --taken
    void setTaken(const char *path);
    void setChoose(const char *spec); // CONscripter: --choose, --choices
    void setChoices(const char *path);
    void setKnownLabels(const char *path); // CONscripter: --known-labels
    void setStopKnown(int pages) { stop_known = pages; } // CONscripter: --stop-known
    void setUntil(const char *label); // CONscripter: --until, --max-pages
    void setMaxPages(int pages) { max_pages = pages; }
    void setTraceShots(const char *pages); // CONscripter: --shot-pages, --shot-every, --shot-dir
    void setTraceShotEvery(int pages);
    void setTraceShotDir(const char *dir);
    void setTraceShotAt(const char *places); // CONscripter: --shot-at, --shot-after
    void setTraceShotAfter(const char *times);
    void setTraceShotEffects(const char *percents); // CONscripter: --shot-effects
    void startupFailed(); // CONscripter: records the error just printed, exits with 3
    // CONscripter: calls from the web page, as ONS_WEB_EVENT events (ONScripter_cons.cpp)
#define ONS_WEB_EVENT (SDL_USEREVENT+7)
#define ONS_SHOT_EVENT (SDL_USEREVENT+8) // CONscripter: a --shot-after time came
// SDL_USEREVENT+10: kobo.h's sleep_event
#define ONS_FREQ_END_EVENT (SDL_USEREVENT+11) // CONscripter: a chfreq sound came to its end
    enum { WEB_SET_AUTO = 1, WEB_SET_SKIP = 2, WEB_SET_RUBY = 3, WEB_SET_TEXT_SPEED = 4, WEB_RESEND_TEXT = 5 };
    void setStartGoto(const char *label); // CONscripter: --goto, --vars, --dump-vars
    void setStartVars(const char *path);
    void setStartLoad(int slot); // CONscripter: --load, --checkpoint-every, --checkpoint-slot
    void setCheckpoints(int pages, int slot);
    void setCheckpointStart(); // CONscripter: --checkpoint-start
    void setAutoPlayReal(); // CONscripter: --real-time
    void setTraceVars(const char *spec); // CONscripter: --trace-vars
    bool setScreenFilter(const char *names); // CONscripter: --filter; false on an unknown name
    void setTraceLongWait(int ms); // CONscripter: --trace-long-wait
    void setAudioStats();          // CONscripter: --audio-stats
    void setTraceLayers();                  // CONscripter: --trace-layers
    void setTraceMaxImage(int mb); // CONscripter: --trace-max-image
    void setRoundtrip(int pages, int slot); // CONscripter: --roundtrip-every, --roundtrip-slot
    void setDumpFrames(const char *path); // CONscripter: --dump-frames
    void setTracePerf();                  // CONscripter: --trace-perf
    void setDumpVars(const char *path);
    const char* getArchivePath() { return archive_path; }
    void setWindowWidth(int width);
    void setWindowHeight(int height);
    void setSharpness(float sharpness);
    void setMaxScreenWidth(int w){ max_screen_width = w; }
    int  getWidth(){ return screen_width;};
    int  getHeight(){return screen_height;};
    ButtonState &getCurrentButtonState(){return current_button_state;};
    int  getSkip(){return automode_flag?2:((skip_mode&SKIP_NORMAL)?1:0);};
    AnimationInfo *getSMPEGInfo(){return smpeg_info;};
    
    int  openScript();
    int  init();

    // ----------------------------------------
    // Commands
    typedef int (ONScripter::*FuncList)();
    struct FuncLUT{
        char command[30];
        FuncList method;
    };
    struct FuncHash{
        FuncLUT *func;
        int num;
    } func_hash['z'-'a'+1];

    void makeFuncLUT();
    static void listCommands();

    int yesnoboxCommand();
    int wavestopCommand();
    int waveCommand();
    int waittimerCommand();
    int waitCommand();
    int vspCommand();
    int voicevolCommand();
    int vCommand();
    int trapCommand();
    int transbtnCommand();
    int textspeeddefaultCommand();
    int textspeedCommand();
    int textshowCommand();
    int textonCommand();
    int textoffCommand();
    int texthideCommand();
    int textcolorCommand();
    int textclearCommand();
    int texecCommand();
    int tateyokoCommand();
    int talCommand();
    int tablegotoCommand();
    int systemcallCommand();
    int strspCommand();
    int stopCommand();
    int sp_rgb_gradationCommand();
    int spstrCommand();
    int spreloadCommand();
    int splitCommand();
    int spclclkCommand();
    int spfilterCommand();
    int spzoomCommand();
    int spzCommand();
    int spclipCommand();
    int spmaskCommand();
    int spfadetoCommand();
    int sptrailCommand();
    int seldisableCommand();
    int spnewCommand(); // ONScripter_names.cpp
    int screenCommand(); // ONScripter_screen.cpp
    void setSkipScreens( const char *kinds );
    int spfreeCommand();
    int spwaveCommand();
    int spovertextCommand();
    int spblendCommand();
    int bitandCommand();
    int spgrabCommand();
    int spmotionCommand();
    int spcurveCommand();
    int effectcurveCommand();
    int effectsoftCommand();
    int clickskipCommand();
    int getclickskipCommand();
    int spmotionwaitCommand();
    int spmotionclockCommand();
    int screenflashCommand();
    int screenfilterCommand();
    int screenshakeCommand();
    int mpegholdCommand();
    int spmoviewaitCommand();
    int btnseCommand();
    int strspoutlineCommand();
    int screenrippleCommand();
    int screenpulseCommand();
    int screenzoomCommand();
    int windowtransCommand();
    int windowfadeCommand();
    int languageCommand(); // CONscripter: the text's language, for the web page
    int conssetCommand(); // CONscripter: player settings
    int consgetCommand();
    bool setSetting(const char *key, int v);
    bool getSetting(const char *key, int &v);
    int openurlCommand();
    int getkeyCommand();
    int padmapCommand(); // CONscripter: gamepad
    int getplaytimeCommand();
    int logrowsCommand();
    int getlogrowCommand();
    int logaddCommand();
    int logvoiceCommand();
    // CONscripter: the text history as rows (logrows, getlogrow)
    struct LogRow { std::string text, voice; int tag = -1; };
    std::vector<LogRow> log_rows;
    int setplaytimeCommand();
    int spbtnCommand();
    int skipoffCommand();
    int sevolCommand();
    int setwindow3Command();
    int setwindow2Command();
    int setwindowCommand();
    int setcursorCommand();
    int selectCommand();
    int savetimeCommand();
    int savepointCommand();
    int saveonCommand();
    int saveoffCommand();
    int savegameCommand();
    int savefileexistCommand();
    int savescreenshotCommand();
    int resettimerCommand();
    int resetCommand();
    int repaintCommand();
    int rndCommand();
    int rmodeCommand();
    int quakeCommand();
    int puttextCommand();
    int prnumclearCommand();
    int prnumCommand();
    int pushscreenCommand();
    int printCommand();
    int popscreenCommand();
    int playstopCommand();
    int playonceCommand();
    int playCommand();
    int okcancelboxCommand();
    int ofscopyCommand();
    int negaCommand();
    int nextcselCommand();
    int mspCommand();
    int mpegplayCommand();
    int mp3volCommand();
    int mp3stopCommand();
    int mp3fadeoutCommand();
    int mp3fadeinCommand();
    int mp3Command();
    int movieCommand();
    int movemousecursorCommand();
    int monocroCommand();
    int menu_windowCommand();
    int menu_fullCommand();
    int menu_click_pageCommand();
    int menu_click_defCommand();
    int menu_automodeCommand();
    int lsp2Command();
    int lspCommand();
    int loopbgmstopCommand();
    int loopbgmCommand();
    int lookbackflushCommand();
    int lookbackbuttonCommand();
    int logspCommand();
    int locateCommand();
    int loadgameCommand();
    int ldCommand();
    int layermessageCommand();
    int kinsokuCommand();
    int jumpfCommand();
    int jumpbCommand();
    int ispageCommand();
    int isfullCommand();
    int isskipCommand();
    int isdownCommand();
    int inputCommand();
    int indentCommand();
    int humanorderCommand();
    int getzxcCommand();
    int getvoicevolCommand();
    int getversionCommand();
    int gettimerCommand();
    int gettextCommand();
    int gettaglogCommand();
    int gettagCommand();
    int gettabCommand();
    int getspsizeCommand();
    int getspposCommand();
    int getspmodeCommand();
    int getsevolCommand();
    int getscreenshotCommand();
    int getsavestrCommand();
    int getretCommand();
    int getregCommand();
    int getpageupCommand();
    int getpageCommand();
    int getmp3volCommand();
    int getmouseposCommand();
    int getmouseoverCommand();
    int getmclickCommand();
    int getlogCommand();
    int getinsertCommand();
    int getfunctionCommand();
    int getenterCommand();
    int getcursorpos2Command();
    int getcursorposCommand();
    int getcursorCommand();
    int getcselstrCommand();
    int getcselnumCommand();
    int gameCommand();
    int flushoutCommand();
    int fileexistCommand();
    int exec_dllCommand();
    int exbtnCommand();
    int erasetextwindowCommand();
    int endCommand();
    int dwavestopCommand();
    int dwavefadeinCommand();
    int chvolfadeCommand();
    int chpanCommand();
    int rgosubCommand();
    int chfreqCommand();
    int bgmlaterCommand();
    int getbgmposCommand();
    int autooffCommand();
    int spcutCommand();
    int dwaveCommand();
    int dvCommand();
    int drawtextCommand();
    int drawsp3Command();
    int drawsp2Command();
    int drawspCommand();
    int drawfillCommand();
    int drawclearCommand();
    int drawbg2Command();
    int drawbgCommand();
    int drawCommand();
    int deletescreenshotCommand();
    int delsavegameCommand();
    int copysavegameCommand();
    int delayCommand();
    int defineresetCommand();
    int cspCommand();
    int cselgotoCommand();
    int cselbtnCommand();
    int clickCommand();
    int clCommand();
    int chvolCommand();
    int checkpageCommand();
    int checkkeyCommand();
    int cellCommand();
    int captionCommand();
    int btnwait2Command();
    int btnwaitCommand();
    int btntime2Command();
    int btntimeCommand();
    int btndownCommand();
    int btndefCommand();
    int btnCommand();
    int bspCommand();
    int brCommand();
    int bltCommand();
    int bgcopyCommand();
    int bgCommand();
    int bdownCommand();
    int barclearCommand();
    int barCommand();
    int aviCommand();
    int automode_timeCommand();
    int autoclickCommand();
    int allsp2resumeCommand();
    int allspresumeCommand();
    int allsp2hideCommand();
    int allsphideCommand();
    int amspCommand();

    void NSDCallCommand(int texnum, const char *str1, int proc, const char *str2);
    void NSDDeleteCommand(int texnum);
    void NSDLoadCommand(int texnum, const char *str);
    void NSDPresentRectCommand(int x1, int y1, int x2, int y2);
    void NSDSp2Command(int texnum, int dcx, int dcy, int sx, int sy, int w, int h,
                       int xs, int ys, int rot, int alpha);
    void NSDSetSpriteCommand(int spnum, int texnum, const char *tag);

    void stopSMPEG();
    void updateEffectDst();
    
private:
    // ----------------------------------------
    // global variables and methods
    enum { IDLE_EVENT_MODE  = 0,
           WAIT_RCLICK_MODE = 1, // for lrclick
           WAIT_BUTTON_MODE = 2, // For select, btnwait and rmenu.
           WAIT_INPUT_MODE  = (4|8),  // can be skipped by a click
           WAIT_TIMER_MODE  = 32,
           WAIT_VOICE_MODE  = 128,
           WAIT_TEXT_MODE   = 256 // clickwait, newpage, select
    };
    int  event_mode;

    bool is_script_read;
    char *wm_title_string;
    char *wm_icon_string;
    char wm_edit_string[256];
    bool fullscreen_mode;
    bool stretch_mode;
    bool window_mode;

    // start-up options
    bool cdaudio_flag;
    char *default_font;
    char *registry_file;
    char *dll_file;
    char *getret_str;
    int  getret_int;
    bool enable_wheeldown_advance_flag;
    bool disable_rescale_flag;
    bool edit_flag;
    char *key_exe_file;
    bool vsync;
    bool video;
    bool cacheFont;
    bool screen_dirty_flag;

    // variables relevant to button
    ButtonState current_button_state, last_mouse_state;

    ButtonLink root_button_link, *current_button_link, exbtn_d_button_link;
    bool is_exbtn_enabled;

    bool btntime2_flag;
    long btntime_value;
    long internal_button_timer;
    long btnwait_time;
    bool btndown_flag;
    bool transbtn_flag;

    int current_over_button;
    int shift_over_button;

    bool bexec_flag;
    bool getzxc_flag;
    bool gettab_flag;
    bool getpageup_flag;
    bool getpagedown_flag;
    bool getmclick_flag;
    bool getinsert_flag;
    bool getfunction_flag;
    bool getenter_flag;
    bool getcursor_flag;
    bool spclclk_flag;

    bool getmouseover_flag;
    bool getkey_flag; // CONscripter: getkey
    int  getmouseover_lower;
    int  getmouseover_upper;

    // variables relevant to selection
    enum { SELECT_GOTO_MODE  = 0, 
           SELECT_GOSUB_MODE = 1, 
           SELECT_NUM_MODE   = 2, 
           SELECT_CSEL_MODE  = 3 
    };
    struct SelectLink{
        SelectLink *next;
        char *text;
        char *label;
        bool disabled; // CONscripter: seldisable, shown greyed and not selectable

        SelectLink(){
            next = NULL;
            text = label = NULL;
            disabled = false;
        };

        ~SelectLink(){
            if ( text )  delete[] text;
            if ( label ) delete[] label;
        };
    } root_select_link;
    NestInfo select_label_info;
    int  shortcut_mouse_line;
    std::set<int> select_disabled; // CONscripter: seldisable's options for the next select

    void initSDL();
    void calcRenderRect();
    void openAudio(int freq=-1);
    void reset(); // called on definereset
    void resetSub(); // called on reset
    void resetSentenceFont();
    void flush( int refresh_mode, SDL_Rect *rect=NULL, bool clear_dirty_flag=true, bool direct_flag=false );
    void flushDirect( SDL_Rect &rect, int refresh_mode );
    void presentScreen();
    void presentPending();
    Uint32 present_time = 0;      // CONscripter: ticks the last present ended
    enum { SKIP_PRESENT_MS = 50 };  // while skipping, presents at most this often
    bool present_pending = false; // flushed to the texture, not shown yet
    bool blt_shown = false;       // CONscripter: blt's picture is over the screen
    SDL_Rect blt_shown_src, blt_shown_dst;
    #ifdef USE_SMPEG
    void flushDirectYUV(SDL_Overlay *overlay);
    #endif
    void mouseOverCheck( int x, int y );
    void consPressCell( bool down ); // CONscripter: spbtn's pressed cell
    void warpMouse(int x, int y);
    void setFullScreen(bool fullscreen);
public:
    void executeLabel();
    void runScript();
    AnimationInfo *getSpriteInfo(int no){ return &sprite_info[no]; };
    AnimationInfo *getSprite2Info(int no){ return &sprite2_info[no]; };
    Uint32 getTextureFormat() { return texture_format; };
private:
    int  parseLine();
    void deleteButtonLink();
    void refreshMouseOverButton();
    void deleteSelectLink();
    void clearCurrentPage();
    void shadowTextDisplay( SDL_Surface *surface, SDL_Rect &clip );
    void newPage();
    ButtonLink *getSelectableSentence( char *buffer, FontInfo *info, bool flush_flag = true, bool nofile_flag = false, bool disabled = false );
    void decodeExbtnControl( const char *ctl_str, SDL_Rect *check_src_rect=NULL, SDL_Rect *check_dst_rect=NULL );
    void saveAll();
    void loadEnvData();
    void saveEnvData();
    int  refreshMode();
    void quit();
    void disableGetButtonFlag();
    int  getNumberFromBuffer( const char **buf );

    // ----------------------------------------
    // variables and methods relevant to animation
    enum { ALPHA_BLEND_CONST          = 1,
           ALPHA_BLEND_MULTIPLE       = 2,
           ALPHA_BLEND_FADE_MASK      = 3,
           ALPHA_BLEND_CROSSFADE_MASK = 4
    };

    AnimationInfo btndef_info, bg_info, cursor_info[2];
    AnimationInfo tachi_info[3]; // 0 ... left, 1 ... center, 2 ... right
    AnimationInfo *sprite_info, *sprite2_info;
    AnimationInfo *texture_info;
    AnimationInfo *bar_info[MAX_PARAM_NUM], *prnum_info[MAX_PARAM_NUM];
    AnimationInfo lookback_info[4];
    AnimationInfo dialog_info;

    int  human_order[3];
    bool all_sprite_hide_flag;
    bool all_sprite2_hide_flag;
    bool show_dialog_flag;
    
    int  calcDurationToNextAnimation();
    void proceedAnimation(int current_time);
    void setupAnimationInfo(AnimationInfo *anim, FontInfo *info=NULL);
    void parseTaggedString(AnimationInfo *anim );
    void drawTaggedSurface(SDL_Surface *dst_surface, AnimationInfo *anim, SDL_Rect &clip);
    void drawZoomedSurface(SDL_Surface *dst_surface, AnimationInfo *anim, SDL_Rect &poly_rect, SDL_Rect &clip);
    SDL_Rect zoomedRect(const AnimationInfo &ai);
    SDL_Rect drawnRect(const AnimationInfo &ai);
    bool spriteZoomOrMotion( const SDL_Rect &clip );
    void stepSpriteMotion(AnimationInfo &ai, int current_time);
    void motionAt(const AnimationInfo &ai, int keys, int t, int v[7], int fine=1);
    bool trailCopyAt(const AnimationInfo &ai, int i, int v[7]);
    SDL_Rect trailCopies(AnimationInfo &ai, SDL_Surface *surface, SDL_Rect *clip);
    void drawWithTrail(SDL_Surface *surface, AnimationInfo &ai, SDL_Rect &clip);
    void featherBands(SDL_Surface *surface, const SDL_Rect &r, int f, int colour, const SDL_Rect &clip, std::vector<Uint32> &saved, bool apply);
    // spmask: the masks loaded (name, ARGB8888 surface), by AnimationInfo::screen_mask
    std::vector<std::pair<std::string, SDL_Surface*> > screen_masks;
    int screenMaskSlot( const char *name );
    void fadeToFrame( AnimationInfo &ai, int k );
    bool stepSpriteFade( AnimationInfo &ai, int current_time );
    void screenMaskBlend( SDL_Surface *surface, AnimationInfo *anim, const SDL_Rect &clip, std::vector<Uint32> &saved, bool apply );
    void endKeptMotion(AnimationInfo &ai);
    void applyMotion(AnimationInfo &ai, const int *v);
    int curveAt(int curve, int p);
    // spcurve's tables, curves CURVE_FIRST and on
    enum { CURVE_FIRST = 10, MAX_CURVES = 64, MAX_CURVE_VALUES = 1025 };
    std::vector<int> curve_table[MAX_CURVES];
    bool proceedSpriteMotions(int current_time, bool draw = true);
    int nextSpriteMotionTime();
    bool stepSpriteWave(AnimationInfo &ai, int current_time);
    int waveShift(const AnimationInfo &ai, int row, int drawn_h);
    // CONscripter: lsp sprites from hi down to lo in drawing order, with
    // the ones spz moves drawn right over their sprite
    void drawSpriteRange( SDL_Surface *surface, SDL_Rect &clip, int hi, int lo );
    // CONscripter: spovertext: z_over's value for a sprite drawn over the
    // text window; drawn after the text
    static const int Z_OVER_TEXT = MAX_SPRITE_NUM;
    void drawOverTextSprites( SDL_Surface *surface, SDL_Rect &clip );
    void stopAnimation(int click);
    void loadCursor(int no, const char *str, int x, int y, bool abs_flag = false);

    // ----------------------------------------
    // variables and methods relevant to effect
    DirtyRect dirty_rect; // only this region is updated
    int  effect_counter, effect_duration; // counter in each effect
    int  effect_curve = 0; // CONscripter: effectcurve, for the next effect
    int  effect_soft = 0, effect_soft_reverse = 0, effect_soft_form = 0; // CONscripter: effectsoft, for the next masked effect
    void softMaskBlend( SDL_Surface *mask, int counter, int duration, SDL_Rect *clip );
    bool click_skip_flag = false; // CONscripter: clickskip (define)
    bool click_skipping = false; // CONscripter: clickskip: a click ended a delay or effect, the next ones end at once
    bool waitInterrupted();
    int  effect_timer_resolution;
    int  effect_start_time;
    int  effect_start_time_old;
    volatile bool update_effect_dst;

    void generateEffectDst(int effect_no);
    bool setEffect( EffectLink *effect );
    bool doEffect( EffectLink *effect, bool clear_dirty_region=true );
    void drawEffect( SDL_Rect *dst_rect, SDL_Rect *src_rect, SDL_Surface *surface );
    bool drawEffectFrame( EffectLink *effect, int effect_no );
    void traceShotEffect( EffectLink *effect, int effect_no );
    void generateMosaic( SDL_Surface *src_surface, int level );
    
    struct BreakupCell {
        int cell_x, cell_y;
        int dir;
        int state;
        int radius;
        BreakupCell(): cell_x(0), cell_y(0),
                       dir(0), state(0), radius(0){}
    } *breakup_cells;
    bool *breakup_cellforms, *breakup_mask;
    void buildBreakupCellforms();
    void buildBreakupMask();
    void initBreakup( char *params );
    void effectBreakup( char *params, int duration );
    // CONscripter: effect 99 with "cons_blur.dll", "cons_rule.dll", ... (ONScripter_cons.cpp)
    bool effectRetired( const char *params, int duration );
    void effectBlur( const char *params, int duration );
    void effectRule( const char *params, int duration );
    void warpFrame( SDL_Surface *map, int cells, int c, float t ); // ONScripter_warp.cpp

#ifdef USE_BUILTIN_EFFECTS
    //cascade
    void effectCascade(char *params, int duration);

    //trig
    int *sin_table, *cos_table;
    void buildSinTable();
    void buildCosTable();
    void effectTrvswave(char *params, int duration);
    int *whirl_table;
    void buildWhirlTable();
    void effectWhirl(char *params, int duration);
#endif

    // ----------------------------------------
    // variables and methods relevant to event
    enum { NOT_EDIT_MODE            = 0,
           EDIT_SELECT_MODE         = 1,
           EDIT_VARIABLE_INDEX_MODE = 2,
           EDIT_VARIABLE_NUM_MODE   = 3,
           EDIT_MP3_VOLUME_MODE     = 4,
           EDIT_VOICE_VOLUME_MODE   = 5,
           EDIT_SE_VOLUME_MODE      = 6
    };
    
    int  next_time;
    int  variable_edit_mode;
    int  variable_edit_index;
    int  variable_edit_num;
    int  variable_edit_sign;
    int  shift_pressed_status;
    int  ctrl_pressed_status;
    int  num_fingers; // numbur of fingers touching on the screen
    
    void flushEventSub( SDL_Event &event );
    void flushEvent();
    void removeEvent(int type);
    void removeBGMFadeEvent();
public:
    void waitEventSub(int count);
private:
    bool waitEvent(int count);
    bool trapHandler();
    bool mouseMoveEvent( SDL_MouseMotionEvent *event );
    bool mousePressEvent( SDL_MouseButtonEvent *event );

    bool mouseWheelEvent(SDL_MouseWheelEvent *event);
    bool padButton( int button, bool down ); // CONscripter: gamepad
    bool padAction( SDL_Keycode action );
    bool padTick();

    void variableEditMode( SDL_KeyboardEvent *event );
    void shiftCursorOnButton( int diff );
    bool keyDownEvent( SDL_KeyboardEvent *event );
    void keyUpEvent( SDL_KeyboardEvent *event );
    bool keyPressEvent( SDL_KeyboardEvent *event );
    void timerEvent(bool init_flag);
#if (defined(IOS) || defined(ANDROID) || defined(WINRT))
    bool convTouchKey(SDL_TouchFingerEvent &finger);
#endif
    void runEventLoop();

    // ----------------------------------------
    // variables and methods relevant to file/file2
    void searchSaveFile( SaveFileInfo &info, int no );
    char *readSaveStrFromFile( int no );
    int  loadSaveFile( int no );
    void saveMagicNumber( bool output_flag );
    void storeSaveFile();
    int  writeSaveFile( int no=0, const char *savestr=NULL );

    int  loadSaveFile2( int file_version );
    void saveSaveFile2( bool output_flag );
    // CONscripter: extra state after the NScripter save data (ONScripter_cons.cpp)
    void loadSaveFileCons();
    void saveSaveFileCons( bool output_flag );
    // CONscripter: places in the script by label, line and the line's hash (ONScripter_savepoint.cpp)
    void loadScriptPosition();
    void saveSavePoints( bool output_flag );
    void loadSavePoints();
    void writeSavePoint( char *address, bool output_flag );
    char *readSavePoint( bool after, const char *what, int *label, int *line );
    // ONScripter_names.cpp: spnew's handles (sprite_handle_layer[h - NS_SPRITE_NUM]:
    // 0 free, else layer + 1; sprite_handle_var: the variable spnew wrote into)
    int sprite_handle_layer[SPRITE_HANDLE_NUM];
    int sprite_handle_var[SPRITE_HANDLE_NUM];
    void resetSpriteHandles();
    int newSpriteHandle( int layer );
    int spriteHandleOver( int no );
    void writeNamesSection( bool output_flag );
    void readNamesSection();
    void clearHandleSprites();
    void drawSpritesOver( SDL_Surface *surface, SDL_Rect &clip, int target, int depth ); // ONScripter_cons.cpp
    // CONscripter: screens the engine draws (ONScripter_screen.cpp); the open ones, the last on top
    std::vector<struct ConsScreen*> screens;
    std::map<std::string, int> screen_pages; // the save slots' page each screen showed last
    const char *screen_strsp_text = NULL; // strsp's text in place of its argument (a screen's, with line breaks)
    std::string skip_screens = ",save,load,settings,backlog,extras,"; // the kinds --auto-play closes at once
    bool screen_waiting = false; // a screen waits for the player (screenWait)
    bool screenPress( int x, int y ); // a press on a screen's slider knob: dragged from the press
#ifdef USE_LUA
public:
    int screenLuaVar( lua_State *L );
private:
    bool screenVariable( const char *ref, bool str, ScriptHandler::VariableInfo &vi );
    void screenExec( const std::string &cmd );
    void screenTrace( struct ConsScreen *s, const char *action, int slot, int value );
    time_t screenSaveTime( int no );
    int screenSprite( struct ConsScreen *s );
    int screenImage( struct ConsScreen *s, int x, int y, const char *file_override=NULL, int cells=0 );
    int screenText( struct ConsScreen *s, const std::string &text, int x, int y, const std::vector<std::string> *colors=NULL, int no=-1 );
    void screenTextButton( struct ConsScreen *s, const std::string &text, int x, int y, int no, bool off );
    int screenGet( const std::string &ref );
    void screenSet( const std::string &ref, int value );
    void screenClear( struct ConsScreen *s );
    bool screenEnabled( struct ConsScreen *s );
    bool screenWhen( struct lua_State *L );
    bool screenRowsList( struct ConsScreen *s, std::string &var, int &back_max );
    void screenRows( struct ConsScreen *s, int w );
    void screenDraw( struct ConsScreen *s );
    void screenSlots( struct ConsScreen *s, int w );
    ScriptParser::Page *screenLogPage( int k );
    bool screenScrollable( struct ConsScreen *s, int n );
    void screenList( struct ConsScreen *s, int w );
    void screenGallery( struct ConsScreen *s, int w );
    void screenViewer( struct ConsScreen *s );
    void screenItems( struct ConsScreen *s, int w );
    int screenConfirm( struct ConsScreen *s, int slot );
    void screenSave( struct ConsScreen *s, int slot );
    int screenActions( struct ConsScreen *s, int start, int *value );
    struct ConsScreen *screenLoad( const std::string &name, const std::string &arg );
    void screenShow( struct ConsScreen *s );
    void screenHide( struct ConsScreen *s );
    void screenCloseAll();
    int screenWait( struct ConsScreen *s );
    void screenKnob( struct ConsScreen *s, int w, int v );
    int screenMotions( struct ConsScreen *s, const char *key );
    void screenFade( int ms );
    void screenFill( struct ConsScreen *s, int w, int v );
    int screenKnobKey( struct ConsScreen *s, int button );
    bool screenDragWait();
    int screenDrag( struct ConsScreen *s, int button, int *value );
    int screenRun( struct ConsScreen *s, int *value );
    int screenOpen( const std::string &name, const std::string &arg, int *value );
    // the shot of a screen closed with hold, taken by the next screen
    // opened before the display changes (a tab of a menu opening another)
    SDL_Surface *screen_held_shot = NULL;
#endif
    // CONscripter: play time (getplaytime), counted from playtime_start on
    // top of playtime_base; the save data's offset of it (writeSaveFile
    // brings it up to date there).
    long long playtime_base_ms = 0;
    Uint32 playtime_start = 0;
    int playtime_offset = -1;
    int playtimeSeconds();
    void setPlaytime( int seconds );
    // CONscripter: where savescreenshot and delsavegame put a file
    std::string saveDirPath( const char *name );
    // CONscripter: decodes a WebP image where SDL_image can't (USE_WEBP)
    SDL_Surface *loadWebP( const unsigned char *data, size_t length );

    // CONscripter: screen filters (screenfilter), by slot; kind 0 = slot unused
    enum { MAX_SCREEN_FILTER = 4 };
    AnimationInfo::Filter screen_filter[MAX_SCREEN_FILTER];
    // the sprite each slot applies after (-1: after all the sprites under
    // the text window), and the slots one refresh has applied so far
    int screen_filter_z[MAX_SCREEN_FILTER];
    int screen_filters_applied;
    void clearScreenFilters();
    bool screenFilterBlurs();
    void applyScreenFilters( SDL_Surface *surface, SDL_Rect &clip, int z = -1 );
    // CONscripter: screenflash, a colour added over everything (0: none)
    int screen_flash_opacity = 0;
    uchar3 screen_flash_colour = {0xff, 0xff, 0xff};
    void applyScreenFlash( SDL_Surface *surface, SDL_Rect &clip );

    // CONscripter: animated effects on the same picture (screenshake,
    // screenripple, screenpulse), by slot, stepped like sprite animations
    enum { MAX_SCREEN_EFFECT = 8, SCREEN_EFFECT_PARAMS = 11 };
    enum { SCREEN_EFFECT_NONE = 0, SCREEN_EFFECT_SHAKE = 1, SCREEN_EFFECT_RIPPLE = 2, SCREEN_EFFECT_PULSE = 3, SCREEN_EFFECT_ZOOM = 4 };
    struct ScreenEffect {
        int kind;
        int param[SCREEN_EFFECT_PARAMS]; // the command's numbers after the slot (and map)
        char *map_name;                  // ripple: the displacement map
        unsigned char *map;              // its grey levels
        int map_w, map_h;
        int next_step;                   // ticks of the next step
        int elapsed;                     // ms, up to the effect's time
        int ran;                         // ms since it started (shake: its end)
        int range_x, range_y;            // shake: amplitude, 16.16 pixels
        unsigned int phase_x, phase_y;   // shake: wave phases (or path progress)
        int point;                       // shake: point on the path (mode 1)
        int opacity, amp, angle, phase;  // ripple: 0-256, 16.16 pixels, 0.1 degrees, 0-999
        ONSBuf *snap;                    // pulse: the tinted picture when it started
        int snap_w, snap_h;
        int drawn[4];                    // its look when last flushed (proceedScreenEffects)
    };
    ScreenEffect screen_effect[MAX_SCREEN_EFFECT];
    enum { FRAME_MS = 16 };              // effects draw at most once per this (60 Hz)
    int screen_effect_flush_time;        // ticks of the last redraw proceedScreenEffects made
    bool screen_effect_pending = false;  // text drawn that the next redraw shows
    int screenEffectCommand( int kind, const char *name );
    void startScreenEffect( ScreenEffect &e );
    void stepScreenEffect( ScreenEffect &e );
    void shakeOffset( ScreenEffect &e, int &dx, int &dy );
    void screenEffectLook( ScreenEffect &e, int look[4] );
    void clearScreenEffect( ScreenEffect &e );
    void clearScreenEffects();
    bool screenEffectActive();
    int nextScreenEffectTime();
    void proceedScreenEffects( int current_time );
    void applyScreenEffects( SDL_Surface *surface, SDL_Rect &clip );
    // CONscripter: the part of the full-screen text layer (text_info) that
    // holds text, so a redraw blends that part only (a screen effect redraws
    // the whole screen every frame)
    SDL_Rect text_area = {0, 0, 0, 0}, text_area_backup = {0, 0, 0, 0};
    void clearTextLayer();
    void blendTextLayer( SDL_Surface *surface, SDL_Rect &clip );

    // CONscripter: strsp's font settings, per sprite and in saved screens
    struct StrspFont { int num_xy[2], font_size_xy[2], pitch_xy[2]; bool bold, shadow, tate; int outline = -1; }; // tate: vstrsp; outline: strspoutline's
    // CONscripter: screen states saved by pushscreen, restored by popscreen
    struct ScreenSprite {
        char *image_name;
        char *str; // a strsp sprite: its text (no image_name), font and cell colours
        StrspFont font;
        int cells;
        uchar3 *colors;
        int x, y, cell, trans;
        bool visible;
        int num_of_filters;
        AnimationInfo::Filter filter[AnimationInfo::MAX_FILTER_NUM];
        int zoom_x, zoom_y;
        int zoom_rot;
        int z_over = -1; // spz
        int clip_rect[6] = {0, 0, 0, 0, 0, 0}; // spclip
    };
    struct ScreenState {
        char *bg_name;
        char *bgm_name; // NULL: no BGM
        bool bgm_loop;
        int first, last; // the sprites saved
        ScreenSprite *sprite;
        AnimationInfo::Filter screen_filter[MAX_SCREEN_FILTER];
        int screen_filter_z[MAX_SCREEN_FILTER];
    };
    enum { MAX_SCREEN_STACK = 32 };
    ScreenState screen_stack[MAX_SCREEN_STACK];
    int screen_stack_num;
    int window_trans; // windowtrans: the text window picture's opacity, 0-255
    // windowfade: the window and its text drawn at window_fade/256, going from
    // window_fade_from to window_fade_to over window_fade_ms after window_fade_delay
    bool window_fade_on = false, window_fade_redraw = false;
    int window_fade = 256, window_fade_from = 256, window_fade_to = 256, window_fade_ms = 0, window_fade_delay = 0;
    Uint32 window_fade_start = 0;
    int nextWindowFadeTime();
    void proceedWindowFade( int current_time, bool draw = true );
    void stopWindowFade();
    void clearScreenStack();
    void popScreenState();
    // CONscripter: text size tag "!zNNN" (percent of the window's font size)
    int text_size_percent;
    bool text_italic; // "!i1" ... "!i0"
    int text_outline; // "!o#RRGGBB" ... "!o0": 0xRRGGBB, -1 none
    void centreLine( const char *buf );
    FontInfo *textSizeFont( FontInfo *info, FontInfo &scaled );
    bool isProportional( FontInfo *info );
    int hankakuExtra( FontInfo *glyph_info, const char *c );
    bool breakAtSpace( const char *buf, FontInfo *info );
    bool pastPage( const char *text );

    // CONscripter: debug trace (--trace or CONS_TRACE), one JSON record per
    // line: the state at every text page, missing files and script errors
    struct TraceSprite {
        std::string image;           // what was shown, to tell when it changed
        int x, y, w, h, trans, cell;
        int over;                    // spz's sprite
        unsigned motion;             // the spmotion moving it (traceMotionId), 0 none
        int trail[4], clip[6], blend; // sptrail, spclip, spblend
        unsigned running = 0;        // the motion whose start was recorded and end not yet
        int since;                   // the page it last changed
    };
    enum { TRACE_SPRITES = MAX_SPRITE_NUM + MAX_SPRITE2_NUM + 3 }; // lsp, then lsp2, then ld
    FILE *trace_fp;
    int trace_page;
    TraceSprite *trace_sprite;       // by TRACE_SPRITES index
    bool *trace_ignore;              // sprites left out (--trace-ignore), by the same index
    char *trace_ignore_spec;
    std::string trace_bg, trace_bgm; // as last recorded
    bool trace_shown;                // something changed since the last print was recorded
    char *trace_cmd_start;           // the command being run
    std::vector<bool> trace_label_seen;
    std::vector<std::pair<int, int> > trace_shot_pages; // ranges of pages to take a picture of
    int trace_shot_every;
    std::string trace_shot_dir;
    void traceShot( std::string &out, bool dump=false );
    struct ShotAt { std::string at; int line; char *begin, *end; int visits; bool pending; unsigned int later; };
    std::vector<ShotAt> trace_shot_at; // --shot-at: the lines, found in traceStart
    struct ShotTime { unsigned int ms; std::string name; const char *why; };
    std::vector<ShotTime> trace_shot_after; // --shot-after, and --shot-at's '+ms': the times, in order
    size_t trace_shot_after_next = 0;
    std::vector<int> trace_shot_effects; // --shot-effects: the percents of each effect
    int trace_fx_seq = 0;            // effects filmed so far
    std::string trace_shot_name;     // the next dump's picture, when not dump<pg>-<ms>.png
    const char *trace_dump_why = "F8"; // why the next dump is written
    void traceShotAtSetup();
    void traceShotAt( char *line_start );
    void traceShotAfter();
    void traceShotDump( const char *why, const std::string &name );
    void traceLabel( char *address );
    char *trace_se_name[ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS]; // the sound each channel last played
    bool trace_se_loop[ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS];
    void traceStart();
    void traceStale();
    void traceWrite( std::string &record );
    void traceAt( std::string &out, char *address );
    void traceVia( std::string &out );
    AnimationInfo &traceSpriteInfo( int i, const char *&kind, int &no );
    SDL_Rect traceBox( AnimationInfo &ai, const char *kind );
    int traceSpriteUpdate( int i, int since );
    void traceSprite( std::string &out, int i );
    bool traceChanges( std::string &out );
    void traceMotion( AnimationInfo &ai, const char *ended );
    void traceShake( int slot, const char *ended, int ms, const char *quake=NULL );
    int traceCommand( const char *name, FuncList method );
    void traceSelect( int picked );
    void traceButtons( int result );
    void tracePage( bool dump=false );
    void traceSound( int channel, const char *name, bool loop );
    void traceWarn( const char *what, const char *file );
void traceUnknownCommand( const char *cmd );
    void traceError( const char *message, const char *kind="script" );
    void traceStack( std::string &out );
    void traceEnd();
    void traceMeta();
    bool trace_meta_written;
    static void traceCrash( int sig );
    void traceIgnoreSetup();
    const char *trace_end_reason;    // why a run that plays itself stopped
    std::string trace_effect;        // the effect the command being run used, as JSON
    void traceEffect( std::string &out, EffectLink *effect );
    bool isStrsp( int no );
    bool isLoopPlaying( int channel );
    void dropStoppedSoundEvents();
    // CONscripter: a sound cut off fades out instead of stopping mid-wave (a pop)
    void playChannel( int channel, Mix_Chunk *chunk, int loops );
    int channelPlayedMs( int channel ); // -1: nothing playing
    void endFreqChannel( int channel ); // CONscripter: chfreq's sound ended
    int dwave_fade_in_ms = 0;   // CONscripter: dwavefadein, for the next dwave
    int automode_char_time = 0; // CONscripter: automode_time's ms per character
    int automodeWait();
    int channel_fade_in_ms = 0; // the fade-in of the sound playChannel starts
    // CONscripter: chvolfade -1, the script's BGM volume (percent) on top of the setting
    int bgm_script_from = 100, bgm_script_to = 100, bgm_script_ms = 0, bgm_script_curve = 0;
    Uint32 bgm_script_start = 0;
    void resetScriptVolumes();
    int bgmScriptVolume();
    int musicVolume();
    void applyMusicVolume();
    int nextSoundFadeTime();
    void proceedSoundFades( int current_time );
    // CONscripter: bgmlater, a music to start at a time (name NULL: none)
    char *bgm_later_name = NULL;
    bool bgm_later_loop = true;
    Uint32 bgm_later_time = 0;
    void cancelLaterBGM();
    void cutChannel( int channel );
    void crossFadeOutBGM( int ms ); // CONscripter: bgm "file",ms
    std::string trace_sound;         // the sound the command being run plays, as JSON fields
    void traceMovie( bool script_click );

    // CONscripter: player settings a game's settings screen sets
    // (consset's keys, btnse); not saved, the game sets them
    // again at startup
    bool click_voice_stop = false;   // a click that ends a text wait stops the voice
    bool auto_voice_wait = true;     // auto mode waits for the voice to end (off: cuts it)
    bool select_keep_auto = false;   // auto mode goes on after a select
    bool select_keep_skip = false;   // skip goes on after a select
    bool click_auto_stop = true;     // a click or key ends auto mode (and does nothing else)
    bool click_skip_stop = true;     // a click or key ends skip
    int skip_button = 0;             // clickskipstop 0: a button clicked during skip, for the next textbtnwait
    int movie_stop = -1;             // a click stops a movie: 0 never, 1 always, -1 the script's flag
    bool movie_skipped = false;      // the last movie was stopped by a click (or auto-play)
    unsigned long long fresh_channels = 0; // dwave channels started since the last wait: a movie's own sounds
    void stopFreshChannels();
    // CONscripter: mpeghold, a movie that plays while the button is held
    struct MovieHold {
        int sprite = -1;             // drawn over the movie
        int channel = -1;            // its sound's, -1 none
        int fade_ms = 0;             // 0: the sound pauses with the movie; else restarts looped, fades on release
        bool held = false, started = false;
        bool mouse = false;          // what holds it: the left button,
        int keys = 0, fingers = 0;   // Enter, Space (bits), touches
    };
    MovieHold *movie_hold = NULL;
    int hold_release = 0;            // it ended while held: the releases that are no clicks (bit 0 the button, keys << 1, 16 a touch)
    bool holdRelease( const SDL_Event &event );
    void movieHoldSet( bool held );
    void movieHoldEvent( const SDL_Event &event );
    void movieHoldEnd();
#if defined(WEB)
    void playMovieHoldWeb( const char *path );
#endif
    int game_speed = 100;            // script times run at this percent of real speed
    bool effect_off = false;         // every print effect is drawn at once
#if defined(KOBO)
    // CONscripter: e-ink, where each frame of a fade is a refresh: print effects
    // and text show at once, like --auto-play
    static const bool still_screen = true;
#else
    static const bool still_screen = false;
#endif
    bool text_shadow = true;         // the message text has setwindow's shadow (off: none)
    int syse_volume = -1;            // sysevol: wave's channel (menu sounds), -1 the sound effects'
    char *btn_se_file = NULL;        // btnse: played when a btnwait ends on a button
    bool shadowed( const FontInfo *info ){ return info->is_shadow && (text_shadow || info != &sentence_font); }
    bool outlined( const FontInfo *info ){ return render_font_outline || info->outline >= 0; }
    int strsp_outline = -1;          // strspoutline: the outline later strsp sprites get (-1: none)
    int speedTime( int ms );         // ms at game_speed
    bool autoWaitsForVoice();
    void voiceAfterWait();

    // CONscripter: what the web page can change (ONS_WEB_EVENT, from its
    // consSet... calls) and what it is told (ONScripter_cons.cpp)
    bool ruby_hidden;                // ruby isn't drawn (the text hooks still have it)
    int text_speed_override;         // ms per character, -1 for the game's own
    int web_mode_sent;               // auto and skip as last sent to consMode
    int web_window_sent[5];          // the text window as last sent to consWindow
    std::vector<unsigned char> web_globals_written; // gloval.sav as persistHook last wrote it
    std::string text_language;       // `language`: the text's language, sent to consLanguage
    std::string web_language_sent;   // as last sent to consLanguage
    Uint32 web_persist_time, web_kidoku_time; // when persistHook last looked, last wrote kidoku.dat
    int web_logs_written[2];         // the label and file logs' counts as persistHook last wrote them
    bool persist_waiting;            // inside waitEvent's wait: persistNow may write at once
    bool persist_due;                // persistNow came outside a wait: the next wait writes everything
    bool webEvent( int code, int value );
    void webStateHook();
    SDL_Rect textWindowRect();
    void persistHook( bool force = false );
public:
    void persistNow();
private:
    void webPageHook();
    void soundCommand( int channel, const char *name, bool play );

    // CONscripter: --auto-play, a run that plays itself (ONScripter_cons.cpp)
    enum { AUTO_CHOICE_FIRST = 0, AUTO_CHOICE_CYCLE = 1, AUTO_CHOICE_RANDOM = 2, AUTO_CHOICE_UNTAKEN = 3 };
    bool auto_play_flag;
    bool auto_play_real;             // --real-time
    int auto_play_limit;             // pages, 0 for no limit
    int auto_play_pages;
    int auto_play_waits;             // since the last page
    int auto_play_choices;           // since the last page
    int auto_play_same;              // pages since one in a label not seen before
    std::vector<bool> auto_label_seen; // labels pages were shown in
    int auto_choice;
    int auto_seed;                   // -1 unless --seed
    std::map<char*, int> auto_choice_visits; // cycle: the times each choice was reached
    bool auto_choice_given;          // --auto-choice was given
    struct AutoAnswer { std::string at; int value; };
    std::vector<AutoAnswer> replay_answers; // --replay: the choices recorded, in order
    size_t replay_next;              // the next one to give
    size_t replay_warned;            // 1 + the answer a divergence was written for
    int replay_seed;                 // the recorded run's seed, -1 for none
    char *replay_path, *taken_path;
    std::map<std::string, int> auto_taken; // --taken: "at<TAB>option" and its count
    std::set<std::string> auto_taken_run; // "at<TAB>option" made in this run
    std::set<std::string> auto_taken_places; // the places --taken lists ("at")
    char *known_path = NULL;         // --known-labels: labels earlier runs reached
    std::set<std::string> auto_known_names;
    std::vector<bool> auto_known;    // by label index, made from auto_known_names
    int stop_known = 0;              // --stop-known: pages on known ground before stopping
    int auto_play_known = 0;         // pages since one in a label no run had reached, or a choice no run had made
    bool auto_play_diverged = false; // this run has made such a choice or reached such a label
    std::set<std::string> auto_avoid; // --taken: "at<TAB>option" marked avoid
    std::map<std::string, std::vector<int> > choose_answers; // --choose: "at" and its answers, by visit
    std::map<std::string, size_t> choose_visits;
    std::vector<std::string> choose_specs; // as given, for the meta record
    std::vector<AutoAnswer> choices_list; // --choices: the answers, in order
    size_t choices_next = 0;
    bool choices_ended = false;
    char *choices_path = NULL;
    bool autoPlayOffered( const std::vector<int> &nos, int value, const char *from );
    std::string traceAtText( char *address );
    void autoPlayChosen( int value );
    void autoPlayWarn( const char *what, const char *extra=NULL );
    bool autoPlayWait( int &count );
    int autoPlayButton();
    void autoPlayPage();
    void autoPlayStuck( const char *what );
    char *until_label = NULL;        // --until label (without '*')
    int until_index = -1;            // its label, once the script is read
    int max_pages = 0;               // --max-pages: pages before the run ends, 0 for no limit
    int run_pages = 0;               // pages shown, for --max-pages
    void untilSetup();
    void stopRun( const char *reason );
    void signalSetup();
    int quitReason();
    char *start_goto;                // --goto label (without '*')
    char *start_vars;                // --vars file
    char *dump_vars_path;            // --dump-vars file
    void startAt();
    bool start_goto_due = false;     // --goto: jump once *start's own lines have run
    void startGoto( char *line_start );
    int start_load;                  // --load slot, -1 for none
    int checkpoint_every, checkpoint_slot, checkpoint_count, checkpoint_pages;
    bool checkpoint_start;           // --checkpoint-start
    void writeCheckpoint();
    void autoPlayCheckpoint();
    bool loadGame( int no );
    void readVarsFile( const char *path );
    void writeVarsFile();
    void traceVars( std::string &out );
    char *trace_vars_spec;           // --trace-vars
    bool trace_vars_flag;            // watching variables
    std::vector<int> trace_num_vars, trace_str_vars; // the variables watched
    std::vector<int> trace_num_seen; // their values as last recorded
    std::vector<std::string> trace_str_seen;
    void traceVarsSetup( bool names=false ); // names: the second pass, once the define section ran
    void traceVarChanges( char *at );
    int trace_long_wait;             // --trace-long-wait ms
    bool trace_layers = false;       // --trace-layers: a "layers" record on each print or effect
    std::vector<AnimationInfo*> *layer_record = NULL; // while set, what refreshSurface draws, in order (NULL: the text)
    void traceLayers();
    bool trace_overflow;             // the page's text went past the window's last line
    std::set<std::string> trace_large_seen; // pictures warned of as large (--trace-max-image)
    static void traceLargeImage( AnimationInfo *ai, int w, int h );
    int roundtrip_every, roundtrip_slot;
    bool roundtrip_due;              // save and load at the next command
    bool roundtrip_pending;          // loaded: compare at the page's click wait
    bool roundtrip_skip_page;        // the page just recorded was the page shown again
    std::map<std::string, std::string> roundtrip_before; // the state at the page
    void roundtripState( std::map<std::string, std::string> &state );
    void roundtripPage();
    void traceSaveLeftOut( int version ); // a loaded save's CONscripter block not read (0: it has none)
    std::string roundtripExcerpt( const std::string &s, size_t from );
    void roundtripLoad();
    void roundtripClear();
    FILE *dump_frames_fp = NULL;     // --dump-frames: a line per present
    bool trace_perf = false;         // --trace-perf: a "perf" record per page
    unsigned presents = 0, presents_page = 0; // frames presented: in all, since the last page
    double perf_work = 0, perf_work_max = 0, perf_block_max = 0; // their ms since the last page
    void tracePresent( double work_ms, double block_ms );
    int trace_wait_ms = 0;           // the time the waits and effects since the last page asked for
    std::string traceBlocked( ButtonLink *bl );
    void traceLongWait( int ms );
    void traceChannel( int ch );     // chvolfade, chpan, chfreq: ,"ch" and ,"empty" in their ev
    void traceRamp( int kind, int ch, int to, int ms ); // chvolfade (0), chpan (1), chfreq (2) ramping to `to` over ms
    void traceSounds();              // the event loop: "sound" records of ramps that reached their value
    void traceSoundEnded( int ch );  // a sound played to its end (ch -1: the music)
    void audioStatsOpened();
    void traceAudioStats();          // "audio" records (ONScripter_cons.cpp)
    static void traceErrorHook( const char *message );
    static void traceExitHook();
    
    // ----------------------------------------
    // variables and methods relevant to image
    bool monocro_flag;
    uchar3 monocro_color;
    uchar3 monocro_color_lut[256];
    int  nega_mode;

    enum { REFRESH_NONE_MODE        = 0,
           REFRESH_NORMAL_MODE      = 1,
           REFRESH_SAYA_MODE        = 2,
           REFRESH_SHADOW_MODE      = 4,
           REFRESH_TEXT_MODE        = 8,
           REFRESH_CURSOR_MODE      = 16
    };
    int  refresh_shadow_text_mode;

    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_GameController *controller;

    void setCaption(const char *title, const char *iconstr = NULL);
    void setScreenDirty(bool screen_dirty);
    // format = SDL_PIXELFORMAT_ABGR8888 for OpenGL ES 1.x, OpenGL ES 2.x (Android, iOS)
    // format = SDL_PIXELFORMAT_ARGB8888 for OpenGL, Direct3D (Windows, Linux, MacOSX) or for any 32bit surface without SDL_Renderer
    // format = SDL_PIXELFORMAT_RGB565 for any 16bit surface without SDL_Renderer (Android, Zaurus)
    Uint32 texture_format;
    SDL_Surface *accumulation_surface; // Final image, i.e. picture_surface (+ shadow + text_surface)
    SDL_Surface *backup_surface; // Final image w/o (shadow + text_surface) used in leaveTextDisplayMode()
    SDL_Surface *screen_surface; // Text + Select_image + Tachi image + background
    SDL_Surface *effect_dst_surface; // Intermediate source buffer for effect
    SDL_Surface *effect_src_surface; // Intermediate destnation buffer for effect
    SDL_Surface *effect_tmp_surface; // Intermediate buffer for effect
    SDL_Surface *screenshot_surface; // Screenshot
    int screenshot_w, screenshot_h;
    SDL_Surface *image_surface; // Reference for loadImage()
    int max_texture_width, max_texture_height;
    SDL_Texture *blt_texture;
    SDL_Rect blt_texture_src_rect;
#if defined(USE_GLES)
    GlesRenderer *gles_renderer;
#endif

    unsigned char *tmp_image_buf;
    unsigned long tmp_image_buf_length;
    unsigned long mean_size_of_loaded_images;
    unsigned long num_loaded_images;

    unsigned char *resize_buffer;
    size_t resize_buffer_size;

    SDL_Surface *loadImage(char *filename, bool *has_alpha=NULL, int *location=NULL, unsigned char *alpha=NULL);
    SDL_Surface *createRectangleSurface(char *filename, bool *has_alpha, unsigned char *alpha=NULL);
    SDL_Surface *createSurfaceFromFile(char *filename,bool *has_alpha, int *location);

    int  resizeSurface( SDL_Surface *src, SDL_Surface *dst );
    void alphaBlend(SDL_Surface *mask_surface, int trans_mode, Uint32 mask_value = 255, SDL_Rect *clip = NULL,
        SDL_Surface *src1 = NULL, SDL_Surface *src2 = NULL, SDL_Surface *dst = NULL);
    void alphaBlendText( SDL_Surface *dst_surface, SDL_Rect dst_rect,
                         SDL_Surface *src_surface, SDL_Color &color, SDL_Rect *clip, bool rotate_flag );
    void makeNegaSurface( SDL_Surface *surface, SDL_Rect &clip );
    void makeMonochromeSurface( SDL_Surface *surface, SDL_Rect &clip );
    void refreshSurface( SDL_Surface *surface, SDL_Rect *clip_src, int refresh_mode = REFRESH_NORMAL_MODE );
    void refreshSprite( int sprite_no, bool active_flag, int cell_no, SDL_Rect *check_src_rect, SDL_Rect *check_dst_rect );
    void createBackground();

    // ----------------------------------------
    // variables and methods relevant to rmenu
    bool system_menu_enter_flag;
    int  system_menu_mode;

    int  shelter_event_mode;
    int  shelter_display_mode;
    bool shelter_draw_cursor_flag;
    Page *cached_page;
    ButtonLink *shelter_button_link;
    SelectLink *shelter_select_link;
    ButtonState shelter_mouse_state;
    
    void enterSystemCall();
    void leaveSystemCall( bool restore_flag = true );
    int  executeSystemCall();
    
    void executeSystemMenu();
    void executeSystemSkip();
    void executeSystemAutomode();
    bool executeSystemReset();
    void executeSystemEnd();
    void executeWindowErase();
    bool executeSystemLoad();
    void executeSystemSave();
    bool executeSystemYesNo( int caller, int file_no=0 );
    void setupLookbackButton();
    void executeSystemLookback();
    void buildDialog(bool yesno_flag, const char *mes1, const char *mes2);

    // ----------------------------------------
    // variables and methods relevant to sound
    enum{
        SOUND_NONE    =  0,
        SOUND_PRELOAD =  1,
        SOUND_CHUNK   =  2, // WAV, Ogg Vorbis
        SOUND_MUSIC   =  4, // WAV, MP3, Ogg Vorbis (streaming)
        SOUND_MIDI    =  8,
        SOUND_OTHER   = 16
    };
    int  cdrom_drive_number;
    char *default_cdrom_drive;
    bool cdaudio_on_flag; // false if mute
    bool volume_on_flag; // false if mute
    SDL_AudioSpec audio_format;
    bool audio_open_flag;
    
    bool wave_play_loop_flag;
    char *wave_file_name;
    
    bool midi_play_loop_flag;
    char *midi_file_name;
    Mix_Music *midi_info;

#ifdef USE_CDROM    
    SDL_CD *cdrom_info;
#endif
    int current_cd_track;
    bool cd_play_loop_flag;
    bool music_play_loop_flag;
    double music_loopback_offset;
    bool mp3save_flag;
    char *music_file_name;
    unsigned char *music_buffer; // for looped music
    long music_buffer_length;
    Uint32 mp3fade_start;
    Uint32 mp3fadeout_duration;
    Uint32 mp3fadein_duration;
    Uint32 mp3fadeout_duration_internal;
    Uint32 mp3fadein_duration_internal;
    char *fadeout_music_file_name;
    Mix_Music *music_info;
    char *loop_bgm_name[2];
    
    Mix_Chunk *wave_sample[ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS];

    char *midi_cmd;

    unsigned char *layer_smpeg_buffer;
    bool layer_smpeg_loop_flag;
    AnimationInfo *smpeg_info;
#if defined(USE_SMPEG)
    SMPEG* layer_smpeg_sample;
    SMPEG_Filter layer_smpeg_filter;
#endif
    
    int playSound(const char *filename, int format, bool loop_flag, int channel=0);
    void playCDAudio();
    int playWave(Mix_Chunk *chunk, int format, bool loop_flag, int channel);
    void setChunkLoop( const unsigned char *buf, unsigned long len ); // CONscripter: LOOPSTART/LOOPEND of a chunk
    int playMIDI(bool loop_flag);
    
    int playMPEG(const char *filename, bool click_flag, bool loop_flag=false);
#if CONS_FFMPEG_MOVIES
    int playMovieFFmpeg(const char *path, bool click_flag, bool loop_flag); // CONscripter: Linux, macOS
#endif
    // CONscripter: a movie as any picture (lsp n,":c;movie\a.mp4"), playing in
    // its place; mpegplay's frames, the text window drawn over them
    SDL_Surface *screen_movie = NULL;
    static bool isMovieFile( const char *name );
    std::vector<struct SpriteMovie *> sprite_movies;
    SDL_Surface *openSpriteMovie( AnimationInfo *anim );
    void closeSpriteMovie( size_t i );
    SpriteMovie *findSpriteMovie( AnimationInfo *anim );
    int nextSpriteMovieTime();
    bool proceedSpriteMovies( int current_time, bool draw = true );
    void setAnimImage( AnimationInfo *anim, SDL_Surface *surface, SDL_Surface *surface_m, bool has_alpha, int location );
#if defined(WEB)
    int playMovieLayered( const char *path, bool click_flag, bool loop_flag );
#endif
    int playAVI( const char *filename, bool click_flag );
    enum { WAVE_PLAY        = 0,
           WAVE_PRELOAD     = 1,
           WAVE_PLAY_LOADED = 2
    };
    void stopBGM( bool continue_flag );
    void stopAllDWAVE();
    void playClickVoice();
    
    // ----------------------------------------
    // variables and methods relevant to text
    enum { DISPLAY_MODE_NORMAL  = 0, 
           DISPLAY_MODE_TEXT    = 1
    };
    int  display_mode;

    enum { SKIP_NONE   = 0,
           SKIP_NORMAL = 1, // skip endlessly (press 's' button)
           SKIP_TO_EOL = 2, // skip to end of line
           SKIP_TO_EOP = 4  // skip to end of page (press 'o' button)
    };
    int  skip_mode;

    int effect_tmp; //tmp variable for use by effect routines

    enum { TRAP_NONE        = 0,
           TRAP_LEFT_CLICK  = 1,
           TRAP_RIGHT_CLICK = 2,
           TRAP_NEXT_SELECT = 4,
           TRAP_STOP        = 8,
           TRAP_CLICKED     = 16
    };
    int  trap_mode;
    char *trap_dist;

    long internal_timer;
    bool automode_flag;
    long automode_time;
    long autoclick_time;

    bool saveon_flag;
    bool internal_saveon_flag; // to saveoff at the head of text

    bool new_line_skip_flag;
    int  text_speed_no;
    bool is_kinsoku;
    AnimationInfo text_info;
    AnimationInfo sentence_font_info;
    char *font_file;
    void *font_cache = NULL;
    int erase_text_window_mode;
    bool text_on_flag; // suppress the effect of erase_text_window_mode
    bool draw_cursor_flag;
    int  indent_offset;

    int force_window_width, force_window_height;
    int max_screen_width = 0; // CONscripter: 0, or the widest the game is drawn (see init)
    bool scale_on_decode = false; // CONscripter: loadWebP decodes at screen_ratio...
    bool decoded_scaled = false;  // ...and says it did, so setAnimImage doesn't scale again
    int decoded_full_w, decoded_full_h; // the picture's own size, when decoded_scaled
    float sharpness = NAN;
    // CONscripter: --filter, how the screen is shown (ONScripter_cons.cpp)
    enum { FILTER_CRT, FILTER_PC98, FILTER_GAMEBOY, FILTER_SEPIA, FILTER_MONO, FILTER_VHS };
    std::vector<int> screen_filters;
    std::vector<ONSBuf> filter_buf;
    const unsigned char *filterScreen(SDL_Rect &rect, int &pitch);

    void setwindowCore();
    
    void shiftHalfPixelX(SDL_Surface *surface);
    void shiftHalfPixelY(SDL_Surface *surface);
    void drawGlyph( SDL_Surface *dst_surface, FontInfo *info, SDL_Color &color, char *text, int xy[2], AnimationInfo *cache_info, SDL_Rect *clip, SDL_Rect &dst_rect );
    void drawChar( char* text, FontInfo *info, bool flush_flag, bool lookback_flag, SDL_Surface *surface, AnimationInfo *cache_info, SDL_Rect *clip=NULL );
    void drawString( const char *str, uchar3 color, FontInfo *info, bool flush_flag, SDL_Surface *surface, SDL_Rect *rect = NULL, AnimationInfo *cache_info=NULL, bool pack_hankaku=true );
    void restoreTextBuffer(SDL_Surface *surface = NULL);
    void recordSpriteText(int no, FontInfo *fi);
    // CONscripter: strsp's font settings per sprite, for pushscreen and the save (CONS block 5)
    StrspFont strsp_font[MAX_SPRITE_NUM];
    void setStrsp( int no, const char *text, const StrspFont &sf, int cells, uchar3 *colors );
    void writeStrsp( char *text, const StrspFont &sf, int cells, uchar3 *colors, bool output_flag );
    void readStrsp( char **text, StrspFont &sf, int &cells, uchar3 *&colors );
    void sendSpriteTexts();
    void resendWebText();
    void enterTextDisplayMode(bool text_flag = true);
    void leaveTextDisplayMode(bool force_leave_flag = false);
    bool doClickEnd();
    // CONscripter: rgosub
    char *rgosub_label = NULL;
    char *rgosub_return = NULL; // the click wait waiting now, as script text
    bool rgosub_pending = false;
    bool rgosub_1byte = false; // the wait is in a ` string
    int rclickMenuMode();
    bool clickEndRgosub( bool ended );
    bool clickWait( char *out_text );
    bool clickNewPage( char *out_text, bool consume = true );
    void startRuby(const char *buf, FontInfo &info);
    void endRuby(bool flush_flag, bool lookback_flag, SDL_Surface *surface, AnimationInfo *cache_info);
    int  textCommand();
    bool checkLineBreak(const char *buf, FontInfo *fi);
    void processEOT();
    bool processText();
};

#endif // __ONSCRIPTER_H__
