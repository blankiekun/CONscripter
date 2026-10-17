/* -*- C++ -*-
 * 
 *  ONScripter_event.cpp - Event handler of ONScripter
 *
 *  Copyright (c) 2001-2016 Ogapee. All rights reserved.
 *            (c) 2014-2016 jh10001 <jh10001@live.cn>
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
#if defined(KOBO)
#include "kobo.h"
#endif
#include "Utils.h"
#include <math.h>
#if defined(WEB)
#include <emscripten.h>
#endif
#if defined(LINUX)
#include <sys/types.h>
#include <sys/wait.h>
#endif

#define ONS_TIMER_EVENT   (SDL_USEREVENT)
#define ONS_MUSIC_EVENT   (SDL_USEREVENT+1)
#define ONS_CDAUDIO_EVENT (SDL_USEREVENT+2)
#define ONS_MIDI_EVENT    (SDL_USEREVENT+3)
#define ONS_CHUNK_EVENT   (SDL_USEREVENT+4)
#define ONS_BREAK_EVENT   (SDL_USEREVENT+5)
#define ONS_BGMFADE_EVENT (SDL_USEREVENT+6)

// This sets up the fade event flag for use in bgm fadeout and fadein.
#define BGM_FADEOUT 0
#define BGM_FADEIN  1

#define EDIT_MODE_PREFIX "[EDIT MODE]  "
#define EDIT_SELECT_STRING "MP3 vol (m)  SE vol (s)  Voice vol (v)  Numeric variable (n)"

static SDL_TimerID timer_id = 0;
#if defined(WEB)
static Uint32 timer_due = 0; // CONscripter: the ticks timer_id fires at
#endif
SDL_TimerID timer_cdaudio_id = 0;
SDL_TimerID timer_bgmfade_id = 0;

#define LONG_CLICK_TIME 1200 //ms
Uint32 g_lastpress_time = 0;

typedef SDL_Keycode ONS_Key;

bool ext_music_play_once_flag = false;

/* **************************************** *
 * Callback functions
 * **************************************** */
extern "C" void musicFinishCallback()
{
    SDL_Event event;
    event.type = ONS_MUSIC_EVENT;
    SDL_PushEvent(&event);
}

extern "C" Uint32 SDLCALL timerCallback( Uint32 interval, void *param )
{
    SDL_RemoveTimer( timer_id );
    timer_id = 0;

    SDL_Event event;
    event.type = ONS_TIMER_EVENT;
    SDL_PushEvent( &event );

    return 0;
}

extern "C" Uint32 cdaudioCallback( Uint32 interval, void *param )
{
    SDL_RemoveTimer( timer_cdaudio_id );
    timer_cdaudio_id = 0;

    SDL_Event event;
    event.type = ONS_CDAUDIO_EVENT;
    SDL_PushEvent( &event );

    return interval;
}

extern "C" Uint32 SDLCALL bgmfadeCallback( Uint32 interval, void *param )
{
    SDL_Event event;
    event.type = ONS_BGMFADE_EVENT;
    event.user.code = (param == NULL) ? BGM_FADEOUT : BGM_FADEIN;
    SDL_PushEvent( &event );

    return interval;
}

/* **************************************** *
 * OS Dependent Input Translation
 * **************************************** */

ONS_Key transKey(ONS_Key key)
{
#if defined(IPODLINUX)
    switch(key){
      case SDLK_m:      key = SDLK_UP;      break; /* Menu                   */
      case SDLK_d:      key = SDLK_DOWN;    break; /* Play/Pause             */
      case SDLK_f:      key = SDLK_RIGHT;   break; /* Fast forward           */
      case SDLK_w:      key = SDLK_LEFT;    break; /* Rewind                 */
      case SDLK_RETURN: key = SDLK_RETURN;  break; /* Action                 */
      case SDLK_h:      key = SDLK_ESCAPE;  break; /* Hold                   */
      case SDLK_r:      key = SDLK_UNKNOWN; break; /* Wheel clockwise        */
      case SDLK_l:      key = SDLK_UNKNOWN; break; /* Wheel counterclockwise */
      default: break;
    }
#endif
#if defined(WINCE)
    switch(key){
      case SDLK_UP:     key = SDLK_UP;      break; /* vkUP    */
      case SDLK_DOWN:   key = SDLK_DOWN;    break; /* vkDOWN  */
      case SDLK_LEFT:   key = SDLK_LCTRL;   break; /* vkLEFT  */
      case SDLK_RIGHT:  key = SDLK_RETURN;  break; /* vkRIGHT */
      case SDLK_KP0:    key = SDLK_q;       break; /* vkStart */
      case SDLK_KP1:    key = SDLK_RETURN;  break; /* vkA     */
      case SDLK_KP2:    key = SDLK_SPACE;   break; /* vkB     */
      case SDLK_KP3:    key = SDLK_ESCAPE;  break; /* vkC     */
      default: break;
    }
#endif
    return key;
}

// CONscripter: what each pad button does (padmap changes it): a key the
// engine already reads, or PAD_BACKLOG / PAD_HIDE for its own actions.
// LT and RT, axes in SDL, come after SDL's buttons as PAD_LT, PAD_RT.
enum { PAD_LT = SDL_CONTROLLER_BUTTON_MAX, PAD_RT, PAD_BUTTONS };
static const ONS_Key PAD_BACKLOG = -1, PAD_HIDE = -2;
static ONS_Key pad_map[PAD_BUTTONS] = {
    SDLK_RETURN,            /* A: advance, confirm */
    SDLK_ESCAPE,            /* B: cancel, the menu */
    SDLK_a,                 /* X: auto mode (a game's getkey A) */
    PAD_HIDE,               /* Y: hide the text window */
    SDLK_0,                 /* BACK: text speed */
    SDLK_F10,               /* GUIDE: full screen */
    SDLK_ESCAPE,            /* START: the menu */
    SDLK_UNKNOWN,           /* LEFTSTICK */
    SDLK_UNKNOWN,           /* RIGHTSTICK */
    PAD_BACKLOG,            /* LEFTSHOULDER: backlog, as the wheel */
    SDLK_RCTRL,             /* RIGHTSHOULDER: skip while held */
    SDLK_UP,                /* DPAD_UP */
    SDLK_DOWN,              /* DPAD_DOWN */
    SDLK_LEFT,              /* DPAD_LEFT */
    SDLK_RIGHT,             /* DPAD_RIGHT */
    SDLK_UNKNOWN,           /* MISC1 */
    SDLK_UNKNOWN,           /* PADDLE1 */
    SDLK_UNKNOWN,           /* PADDLE2 */
    SDLK_UNKNOWN,           /* PADDLE3 */
    SDLK_UNKNOWN,           /* PADDLE4 */
    SDLK_UNKNOWN,           /* TOUCHPAD */
    SDLK_RETURN,            /* PAD_LT: advance */
    SDLK_RETURN,            /* PAD_RT: advance */
};

#define ONS_PAD_EVENT (SDL_USEREVENT+12) // CONscripter: a held stick's tick
#define PAD_DEADZONE 12000   // of 32767
#define PAD_TRIGGER 16000    // LT/RT count as pressed past this
#define PAD_TICK_MS 16
#define PAD_REPEAT_FIRST 400 // ms the left stick waits before it repeats
#define PAD_REPEAT_NEXT 120
#define PAD_POINTER_SPEED 1200 // window pixels a second at full tilt

static SDL_TimerID pad_timer = 0;
static int pad_dir = -1;        // the left stick's d-pad button, -1 centred
static Uint32 pad_next = 0;     // when it repeats
static bool pad_trigger[2] = {false, false};
static float pad_px = -1, pad_py = -1; // the right stick's pointer, window pixels

static Uint32 padTimerCallback( Uint32 interval, void *param )
{
    SDL_Event event;
    event.type = ONS_PAD_EVENT;
    SDL_PushEvent( &event );
    return 0; // padTick starts the next one while a stick is held
}

// padmap "button","key": what a pad button does. Buttons by SDL's names
// (a b x y back guide start leftstick rightstick leftshoulder rightshoulder
// dpup dpdown dpleft dpright, and lefttrigger righttrigger); keys by
// SDL's names ("Return", "Escape", "A", "Right Ctrl", "Left", "F1"), or
// "backlog", "hide", "" for nothing.
int ONScripter::padmapCommand()
{
    const char *buf = script_h.readStr();
    int button = SDL_GameControllerGetButtonFromString( buf );
    if      (!strcmp( buf, "lefttrigger" ))  button = PAD_LT;
    else if (!strcmp( buf, "righttrigger" )) button = PAD_RT;
    const char *key = script_h.readStr();
    if (button < 0){
        utils::printError("padmap: no pad button \"%s\"\n", buf);
        return RET_CONTINUE;
    }
    if      (!strcmp( key, "backlog" )) pad_map[button] = PAD_BACKLOG;
    else if (!strcmp( key, "hide" ))    pad_map[button] = PAD_HIDE;
    else if (!key[0])                   pad_map[button] = SDLK_UNKNOWN;
    else{
        ONS_Key k = SDL_GetKeyFromName( key );
        if (k == SDLK_UNKNOWN) utils::printError("padmap: no key \"%s\"\n", key);
        else pad_map[button] = k;
    }
    return RET_CONTINUE;
}

// A pad button pressed (down) or let go: as its key, or its own action;
// true: the wait ends.
bool ONScripter::padButton( int button, bool down )
{
    if (button < 0 || button >= PAD_BUTTONS) return false;
    SDL_KeyboardEvent key;
    SDL_zero( key );
    key.keysym.sym = pad_map[button];
    if (key.keysym.sym == SDLK_UNKNOWN) return false;
    if (key.keysym.sym < 0) return !down && padAction( key.keysym.sym ); // on release
    key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    key.keysym.sym = transKey( key.keysym.sym );
    if (down){
        bool ret = keyDownEvent( &key );
        if (btndown_flag) ret |= keyPressEvent( &key );
        return ret;
    }
    keyUpEvent( &key );
    return keyPressEvent( &key );
}

// A pad button's own action (PAD_BACKLOG, PAD_HIDE), on release; true:
// the wait ends.
bool ONScripter::padAction( ONS_Key action )
{
    if (action == PAD_BACKLOG){
        SDL_MouseWheelEvent wheel;
        SDL_zero( wheel );
        wheel.type = SDL_MOUSEWHEEL;
        wheel.y = 1;
        // mouseWheelEvent takes the pointer from the wheel's x/y: keep it
        int x = current_button_state.x, y = current_button_state.y;
        bool ret = mouseWheelEvent( &wheel );
        current_button_state.x = x;
        current_button_state.y = y;
        return ret;
    }
    // PAD_HIDE: as a right click with no rmenu, in a text wait
    if (!(event_mode & WAIT_TEXT_MODE) || system_menu_mode != SYSTEM_NULL) return false;
    current_button_state.button = -1;
    sprintf(current_button_state.str, "RCLICK");
    system_menu_mode = SYSTEM_WINDOWERASE;
    return true;
}

// Sticks, polled every PAD_TICK_MS while one is off centre: the left one
// taps the d-pad (repeating while held), the right one moves the pointer.
bool ONScripter::padTick()
{
    if (!controller) return false;
    Sint16 lx = SDL_GameControllerGetAxis( controller, SDL_CONTROLLER_AXIS_LEFTX );
    Sint16 ly = SDL_GameControllerGetAxis( controller, SDL_CONTROLLER_AXIS_LEFTY );
    Sint16 rx = SDL_GameControllerGetAxis( controller, SDL_CONTROLLER_AXIS_RIGHTX );
    Sint16 ry = SDL_GameControllerGetAxis( controller, SDL_CONTROLLER_AXIS_RIGHTY );
    Uint32 now = (SDL_GetTicks)();

    int dir = -1;
    if (abs( lx ) > PAD_DEADZONE || abs( ly ) > PAD_DEADZONE){
        if (abs( lx ) > abs( ly )) dir = lx > 0 ? SDL_CONTROLLER_BUTTON_DPAD_RIGHT : SDL_CONTROLLER_BUTTON_DPAD_LEFT;
        else                       dir = ly > 0 ? SDL_CONTROLLER_BUTTON_DPAD_DOWN : SDL_CONTROLLER_BUTTON_DPAD_UP;
    }
    bool tap = dir >= 0 && (dir != pad_dir || (int)(now - pad_next) >= 0);
    if (dir >= 0 && dir != pad_dir) pad_next = now + PAD_REPEAT_FIRST;
    else if (tap) pad_next = now + PAD_REPEAT_NEXT;
    pad_dir = dir;
    bool right = abs( rx ) > PAD_DEADZONE || abs( ry ) > PAD_DEADZONE;
    pad_timer = dir >= 0 || right ? SDL_AddTimer( PAD_TICK_MS, padTimerCallback, NULL ) : 0;
    if (tap && (padButton( dir, true ) | padButton( dir, false ))) return true;

    if (right){
        int w, h, mx, my;
        SDL_GetWindowSize( window, &w, &h );
        SDL_GetMouseState( &mx, &my );
        if (pad_px < 0 || fabsf( pad_px - mx ) >= 1 || fabsf( pad_py - my ) >= 1){ pad_px = mx; pad_py = my; }
        float step = PAD_POINTER_SPEED * PAD_TICK_MS / 1000.0f / 32767;
        pad_px += (abs( rx ) > PAD_DEADZONE ? rx : 0) * step;
        pad_py += (abs( ry ) > PAD_DEADZONE ? ry : 0) * step;
        if (pad_px < 0) pad_px = 0; else if (pad_px > w-1) pad_px = w-1;
        if (pad_py < 0) pad_py = 0; else if (pad_py > h-1) pad_py = h-1;
#if defined(WEB)
        // the browser can't move its pointer (SDL_WarpMouse is unsupported):
        // the game sees the move, in its own pixels as the web build's are
        SDL_MouseMotionEvent motion;
        SDL_zero( motion );
        motion.type = SDL_MOUSEMOTION;
        motion.x = (int)pad_px * screen_width / w;
        motion.y = (int)pad_py * screen_height / h;
        if (mouseMoveEvent( &motion )) return true;
#else
        warpMouse( (int)pad_px, (int)pad_py );
#endif
    }
    else pad_px = -1;

    return false;
}

void ONScripter::flushEventSub( SDL_Event &event )
{
    if ( event.type == ONS_MUSIC_EVENT ){
        if ( music_play_loop_flag ||
             (cd_play_loop_flag && !cdaudio_flag ) ){
            stopBGM( true );
            if (music_file_name){
                playSound(music_file_name, SOUND_MUSIC, true);
                Mix_SetMusicPosition( music_loopback_offset );
            }
            else
                playCDAudio();
        }
        else{
            stopBGM( false );
        }
    }
    else if ((event.type == ONS_BGMFADE_EVENT) &&
             (event.user.code == BGM_FADEOUT)){
        Uint32 cur_fade_duration = mp3fadeout_duration_internal;
        if (skip_mode & (SKIP_NORMAL | SKIP_TO_EOP) ||
            ctrl_pressed_status || (auto_play_flag && !auto_play_real)) {
            cur_fade_duration = 0;
            Mix_VolumeMusic( 0 );
        }
        Uint32 tmp = SDL_GetTicks() - mp3fade_start;
        if ( tmp < cur_fade_duration ) {
            tmp = cur_fade_duration - tmp;
            tmp *= musicVolume(); // CONscripter: chvolfade -1
            tmp /= cur_fade_duration;

            Mix_VolumeMusic( tmp * MIX_MAX_VOLUME / 100 );
        } else {
            char *ext = NULL;
            if (fadeout_music_file_name) ext = strrchr(fadeout_music_file_name, '.');
            if (ext && (strcmp(ext+1, "OGG") && strcmp(ext+1, "ogg"))){
                // set break event to return to script processing when playing music other than ogg
                SDL_Event event;
                event.type = ONS_BREAK_EVENT;
                SDL_PushEvent( &event );
            }

            stopBGM( false );
        }
    }
    else if ((event.type == ONS_BGMFADE_EVENT) &&
             (event.user.code == BGM_FADEIN)){
        Uint32 cur_fade_duration = mp3fadein_duration_internal;
        if (skip_mode & (SKIP_NORMAL | SKIP_TO_EOP) ||
            ctrl_pressed_status || (auto_play_flag && !auto_play_real)) {
            cur_fade_duration = 0;
            Mix_VolumeMusic( musicVolume() * MIX_MAX_VOLUME / 100 ); // CONscripter: chvolfade -1
        }
        Uint32 tmp = SDL_GetTicks() - mp3fade_start;
        if ( tmp < cur_fade_duration ) {
            tmp *= musicVolume(); // CONscripter: chvolfade -1
            tmp /= cur_fade_duration;

            Mix_VolumeMusic( tmp * MIX_MAX_VOLUME / 100 );
        } else {
            if (timer_bgmfade_id) SDL_RemoveTimer( timer_bgmfade_id );
            timer_bgmfade_id = 0;
            mp3fadeout_duration_internal = 0;

            char *ext = NULL;
            if (music_file_name) ext = strrchr(music_file_name, '.');
            if (ext && (strcmp(ext+1, "OGG") && strcmp(ext+1, "ogg"))){
                // set break event to return to script processing when playing music other than ogg
                SDL_Event event;
                event.type = ONS_BREAK_EVENT;
                SDL_PushEvent( &event );
            }
        }
    }
    else if ( event.type == ONS_CDAUDIO_EVENT ){
        if ( cd_play_loop_flag ){
            stopBGM( true );
            playCDAudio();
        }
        else{
            stopBGM( false );
        }
    }
    else if ( event.type == ONS_MIDI_EVENT ){
        ext_music_play_once_flag = !midi_play_loop_flag;
        Mix_FreeMusic( midi_info );
        playMIDI(midi_play_loop_flag);
    }
    else if ( event.type == ONS_CHUNK_EVENT ){ // for processing btntim2 and automode correctly
        if ( wave_sample[event.user.code] ){
            if (event.user.code < ONS_MIX_CHANNELS) traceSoundEnded( event.user.code ); // CONscripter
            Mix_FreeChunk( wave_sample[event.user.code] );
            wave_sample[event.user.code] = NULL;
            if (event.user.code == MIX_LOOPBGM_CHANNEL0 && 
                loop_bgm_name[1] &&
                wave_sample[MIX_LOOPBGM_CHANNEL1])
                playChannel(MIX_LOOPBGM_CHANNEL1, 
                            wave_sample[MIX_LOOPBGM_CHANNEL1], -1);
        }
    }
}

void ONScripter::flushEvent()
{
    SDL_Event event;
    while( SDL_PollEvent( &event ) )
        flushEventSub( event );
}

void ONScripter::removeEvent(int type)
{
    SDL_Event event;

    while(SDL_PeepEvents( &event, 1, SDL_GETEVENT, type, type) > 0);
}

void ONScripter::removeBGMFadeEvent()
{
    removeEvent(ONS_BGMFADE_EVENT);
}

void ONScripter::waitEventSub(int count)
{
    next_time = count;
    timerEvent(true);

    runEventLoop();
    removeEvent( ONS_BREAK_EVENT );
}

bool ONScripter::waitEvent( int count )
{
    if (count != 0) fresh_channels = 0; // CONscripter: time passes, the sounds started are no movie's
    if (trace_fp && count > 0){ // CONscripter: as asked, before auto-play shortens it
        traceLongWait( count );
        trace_wait_ms += count;
    }
    if (auto_play_flag && autoPlayWait( count )) return false;
    if (count == 0) presentPending(); // CONscripter: a wait that doesn't sleep (the event loop presents before it sleeps)
    if (count > 0) count += SDL_GetTicks();
    // chimimouryou: the reader may look up sprite text while the game waits for input.
    // shortcut: timed waits (btntime, automode) don't send it; add them if games need it.
    if (count == -1) sendSpriteTexts();
    webPersistHook(); // CONscripter: closing the tab is no quit (every wait: auto mode has no click wait)
    webStateHook(); // CONscripter: auto, skip and the text window, for the web page
    
    while(1){
        waitEventSub( count );
        webStateHook();
        if ( system_menu_mode == SYSTEM_NULL ) break;
        if ( system_menu_mode == SYSTEM_RGOSUB ){ // CONscripter: the click wait ends for rgosub's label
            system_menu_mode = SYSTEM_NULL;
            rgosub_pending = true;
            return true;
        }
        int ret = executeSystemCall();
        if      (ret == 1) return true;
        else if (ret == 2) return false;
    }

    return false;
}

void midiCallback( int sig )
{
#if defined(LINUX)
    int status;
    wait( &status );
#endif
    if ( !ext_music_play_once_flag ){
        SDL_Event event;
        event.type = ONS_MIDI_EVENT;
        SDL_PushEvent(&event);
    }
}

extern "C" void waveCallback( int channel )
{
    if (channel >= ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS) return; // CONscripter: a cut sound's fade
    if (!Mix_Playing(channel)) {
        SDL_Event event;
        event.type = ONS_CHUNK_EVENT;
        event.user.code = channel;
        SDL_PushEvent(&event);
    }
}

bool ONScripter::trapHandler()
{
    if (event_mode & WAIT_BUTTON_MODE || 
        event_mode & WAIT_TEXT_MODE) return false;

    if (trap_mode & TRAP_STOP){
        trap_mode |= TRAP_CLICKED;
        return false;
    }
    
    trap_mode = TRAP_NONE;
    stopAnimation( clickstr_state );
    setCurrentLabel( trap_dist );
    current_button_state.button = 0; // to escape from screen effect

    return true;
}

/* **************************************** *
 * Event handlers
 * **************************************** */
bool ONScripter::mouseMoveEvent( SDL_MouseMotionEvent *event )
{
    current_button_state.x = event->x;
    current_button_state.y = event->y;

    if ( event_mode & WAIT_BUTTON_MODE ){
        mouseOverCheck( current_button_state.x, current_button_state.y );
        if (getmouseover_flag &&
            current_over_button >= getmouseover_lower &&
            current_over_button <= getmouseover_upper){
            current_button_state.button = current_over_button;
            return true;
        }
    }

    return false;
}

bool ONScripter::mousePressEvent( SDL_MouseButtonEvent *event )
{
    // utils::printInfo("## mousePressEvent\n");
    if ( variable_edit_mode ) return false;
    
    if ( automode_flag ){
        automode_flag = false;
        return false;
    }

    if ( (event->button == SDL_BUTTON_RIGHT && trap_mode & TRAP_RIGHT_CLICK) ||
         (event->button == SDL_BUTTON_LEFT  && trap_mode & TRAP_LEFT_CLICK) ){
        if (trapHandler()) return true;
    }

    current_button_state.x = event->x;
    current_button_state.y = event->y;
    current_button_state.down_flag = false;
    skip_mode &= ~SKIP_NORMAL;
    if ( event->button == SDL_BUTTON_LEFT )
        consPressCell( event->type == SDL_MOUSEBUTTONDOWN ); // CONscripter

    bool longpress_flag = false;
    if(event->button == SDL_BUTTON_LEFT && event->type == SDL_MOUSEBUTTONUP)
    {
        // utils::printInfo("## mousePressEvent %u %u\n", event->timestamp, g_lastpress_time);
        if(event->timestamp - g_lastpress_time >= LONG_CLICK_TIME)
        {
            // utils::printInfo("## longpress_flag");
            longpress_flag = true;
        }
    }

    if ( ((event->button == SDL_BUTTON_RIGHT && event->type == SDL_MOUSEBUTTONUP) || longpress_flag) &&
         ((rmode_flag && event_mode & WAIT_TEXT_MODE) ||
          (event_mode & (WAIT_BUTTON_MODE | WAIT_RCLICK_MODE))) ){
        current_button_state.button = -1;
        sprintf(current_button_state.str, "RCLICK");
        if (event_mode & WAIT_TEXT_MODE){
            system_menu_mode = rclickMenuMode(); // CONscripter: rgosub
        }
    }
    else if ( event->button == SDL_BUTTON_LEFT &&
              ( event->type == SDL_MOUSEBUTTONUP || btndown_flag ) ){
        current_button_state.button = current_over_button;
        if (current_over_button == -1){
            if (!bexec_flag) current_button_state.button = 0;
            sprintf(current_button_state.str, "LCLICK");
        }
        else{
            sprintf(current_button_state.str, "S%d", current_over_button);
            if (bexec_flag && current_button_link){
                ButtonLink *cbl = current_button_link;
                if ( current_button_link->exbtn_ctl[2] ){
                    SDL_Rect check_src_rect = cbl->image_rect;
                    SDL_Rect check_dst_rect = {0, 0, 0, 0};
                    decodeExbtnControl( cbl->exbtn_ctl[2], &check_src_rect, &check_dst_rect );
                }
                else{
                    sprite_info[ cbl->sprite_no ].visible = true;
                    sprite_info[ cbl->sprite_no ].setCell(2);
                    dirty_rect.add( cbl->image_rect );
                }
                flush( refreshMode() );
            }
        }
            
        if ( event->type == SDL_MOUSEBUTTONDOWN )
            current_button_state.down_flag = true;
    }
    else if ( getmclick_flag && event->button == SDL_BUTTON_MIDDLE ){
        current_button_state.button = -70;
        sprintf(current_button_state.str, "MCLICK");
    }
    else return false;

    if ( event_mode & (WAIT_INPUT_MODE | WAIT_BUTTON_MODE) ){
        if (!(event_mode & (WAIT_TEXT_MODE)))
            skip_mode |= SKIP_TO_EOL;
        playClickVoice();
        stopAnimation( clickstr_state );

        return true;
    }

    return false;
}

bool ONScripter::mouseWheelEvent(SDL_MouseWheelEvent *event)
{
    if (variable_edit_mode) return false;

    if (automode_flag) {
        automode_flag = false;
        return false;
    }

    current_button_state.x = event->x;
    current_button_state.y = event->y;
    current_button_state.down_flag = false;
    skip_mode &= ~SKIP_NORMAL;

    if (event->y > 0 &&
        (bexec_flag ||
        (event_mode & WAIT_TEXT_MODE) ||
        (usewheel_flag && event_mode & WAIT_BUTTON_MODE) ||
        system_menu_mode == SYSTEM_LOOKBACK)) {
        current_button_state.button = -2;
        sprintf(current_button_state.str, "WHEELUP");
        if (event_mode & WAIT_TEXT_MODE) system_menu_mode = SYSTEM_LOOKBACK;
    } else if (event->y < 0 &&
        (bexec_flag ||
        (enable_wheeldown_advance_flag && event_mode & WAIT_TEXT_MODE) ||
        (usewheel_flag && event_mode & WAIT_BUTTON_MODE) ||
        system_menu_mode == SYSTEM_LOOKBACK)) {
        if (event_mode & WAIT_TEXT_MODE)
            current_button_state.button = 0;
        else
            current_button_state.button = -3;
        sprintf(current_button_state.str, "WHEELDOWN");
    }
    else return false;

    if (event_mode & (WAIT_INPUT_MODE | WAIT_BUTTON_MODE)) {
        if (!(event_mode & (WAIT_TEXT_MODE)))
            skip_mode |= SKIP_TO_EOL;
        playClickVoice();
        stopAnimation(clickstr_state);

        return true;
    }

    return false;
}

void ONScripter::variableEditMode( SDL_KeyboardEvent *event )
{
    int  i;
    const char *var_name;
    char var_index[12];

    switch ( event->keysym.sym ) {
      case SDLK_m:
        if ( variable_edit_mode != EDIT_SELECT_MODE ) return;
        variable_edit_mode = EDIT_MP3_VOLUME_MODE;
        variable_edit_num = music_volume;
        break;

      case SDLK_s:
        if ( variable_edit_mode != EDIT_SELECT_MODE ) return;
        variable_edit_mode = EDIT_SE_VOLUME_MODE;
        variable_edit_num = se_volume;
        break;

      case SDLK_v:
        if ( variable_edit_mode != EDIT_SELECT_MODE ) return;
        variable_edit_mode = EDIT_VOICE_VOLUME_MODE;
        variable_edit_num = voice_volume;
        break;

      case SDLK_n:
        if ( variable_edit_mode != EDIT_SELECT_MODE ) return;
        variable_edit_mode = EDIT_VARIABLE_INDEX_MODE;
        variable_edit_num = 0;
        break;

        enum {
            ONS_KP_0 = SDLK_KP_0,
            ONS_KP_1 = SDLK_KP_1,
            ONS_KP_2 = SDLK_KP_2,
            ONS_KP_3 = SDLK_KP_3,
            ONS_KP_4 = SDLK_KP_4,
            ONS_KP_5 = SDLK_KP_5,
            ONS_KP_6 = SDLK_KP_6,
            ONS_KP_7 = SDLK_KP_7,
            ONS_KP_8 = SDLK_KP_8,
            ONS_KP_9 = SDLK_KP_9
        };

      case SDLK_9: case ONS_KP_9: variable_edit_num = variable_edit_num * 10 + 9; break;
      case SDLK_8: case ONS_KP_8: variable_edit_num = variable_edit_num * 10 + 8; break;
      case SDLK_7: case ONS_KP_7: variable_edit_num = variable_edit_num * 10 + 7; break;
      case SDLK_6: case ONS_KP_6: variable_edit_num = variable_edit_num * 10 + 6; break;
      case SDLK_5: case ONS_KP_5: variable_edit_num = variable_edit_num * 10 + 5; break;
      case SDLK_4: case ONS_KP_4: variable_edit_num = variable_edit_num * 10 + 4; break;
      case SDLK_3: case ONS_KP_3: variable_edit_num = variable_edit_num * 10 + 3; break;
      case SDLK_2: case ONS_KP_2: variable_edit_num = variable_edit_num * 10 + 2; break;
      case SDLK_1: case ONS_KP_1: variable_edit_num = variable_edit_num * 10 + 1; break;
      case SDLK_0: case ONS_KP_0: variable_edit_num = variable_edit_num * 10 + 0; break;

      case SDLK_MINUS: case SDLK_KP_MINUS:
        if ( variable_edit_mode == EDIT_VARIABLE_NUM_MODE && variable_edit_num == 0 ) variable_edit_sign = -1;
        break;

      case SDLK_BACKSPACE:
        if ( variable_edit_num ) variable_edit_num /= 10;
        else if ( variable_edit_sign == -1 ) variable_edit_sign = 1;
        break;

      case SDLK_RETURN: case SDLK_KP_ENTER:
        switch( variable_edit_mode ){

          case EDIT_VARIABLE_INDEX_MODE:
            variable_edit_index = variable_edit_num;
            variable_edit_num = script_h.getVariableData(variable_edit_index).num;
            if ( variable_edit_num < 0 ){
                variable_edit_num = -variable_edit_num;
                variable_edit_sign = -1;
            }
            else{
                variable_edit_sign = 1;
            }
            break;

          case EDIT_VARIABLE_NUM_MODE:
            script_h.setNumVariable( variable_edit_index, variable_edit_sign * variable_edit_num );
            break;

          case EDIT_MP3_VOLUME_MODE:
            music_volume = variable_edit_num;
            Mix_VolumeMusic( musicVolume() * MIX_MAX_VOLUME / 100 ); // CONscripter: chvolfade -1
            break;

          case EDIT_SE_VOLUME_MODE:
            se_volume = variable_edit_num;
            for ( i=1 ; i<ONS_MIX_CHANNELS ; i++ )
                if ( wave_sample[i] ) Mix_Volume( i, se_volume * MIX_MAX_VOLUME / 100 );
            if ( wave_sample[MIX_LOOPBGM_CHANNEL0] ) Mix_Volume( MIX_LOOPBGM_CHANNEL0, se_volume * MIX_MAX_VOLUME / 100 );
            if ( wave_sample[MIX_LOOPBGM_CHANNEL1] ) Mix_Volume( MIX_LOOPBGM_CHANNEL1, se_volume * MIX_MAX_VOLUME / 100 );
            break;

          case EDIT_VOICE_VOLUME_MODE:
            voice_volume = variable_edit_num;
            if ( wave_sample[0] ) Mix_Volume( 0, se_volume * MIX_MAX_VOLUME / 100 );

          default:
            break;
        }
        if ( variable_edit_mode == EDIT_VARIABLE_INDEX_MODE )
            variable_edit_mode = EDIT_VARIABLE_NUM_MODE;
        else
            variable_edit_mode = EDIT_SELECT_MODE;
        break;

      case SDLK_ESCAPE:
        if ( variable_edit_mode == EDIT_SELECT_MODE ){
            variable_edit_mode = NOT_EDIT_MODE;
            setCaption( DEFAULT_WM_TITLE, DEFAULT_WM_ICON );
            SDL_Delay( 100 );
            setCaption( wm_title_string, wm_icon_string );
            return;
        }
        variable_edit_mode = EDIT_SELECT_MODE;

      default:
        break;
    }

    if ( variable_edit_mode == EDIT_SELECT_MODE ){
        sprintf( wm_edit_string, "%s%s", EDIT_MODE_PREFIX, EDIT_SELECT_STRING );
    }
    else if ( variable_edit_mode == EDIT_VARIABLE_INDEX_MODE ) {
        sprintf( wm_edit_string, "%s%s%d", EDIT_MODE_PREFIX, "Variable Index?  %", variable_edit_sign * variable_edit_num );
    }
    else if ( variable_edit_mode >= EDIT_VARIABLE_NUM_MODE ){
        int p=0;
        
        switch( variable_edit_mode ){

          case EDIT_VARIABLE_NUM_MODE:
            sprintf( var_index, "%%%d", variable_edit_index );
            var_name = var_index; p = script_h.getVariableData(variable_edit_index).num; break;

          case EDIT_MP3_VOLUME_MODE:
            var_name = "MP3 Volume"; p = music_volume; break;

          case EDIT_VOICE_VOLUME_MODE:
            var_name = "Voice Volume"; p = voice_volume; break;

          case EDIT_SE_VOLUME_MODE:
            var_name = "Sound effect Volume"; p = se_volume; break;

          default:
            var_name = "";
        }
        sprintf( wm_edit_string, "%sCurrent %s=%d  New value? %s%d",
                 EDIT_MODE_PREFIX, var_name, p, (variable_edit_sign==1)?"":"-", variable_edit_num );
    }

    setCaption( wm_edit_string, wm_icon_string );
}

void ONScripter::shiftCursorOnButton( int diff )
{
    int num;
    ButtonLink *button = root_button_link.next;
    for (num=0 ; button ; num++) 
        button = button->next;

    shortcut_mouse_line += diff;
    if      (shortcut_mouse_line < 0)    shortcut_mouse_line = num-1;
    else if (shortcut_mouse_line >= num) shortcut_mouse_line = 0;

    button = root_button_link.next;
    for (int i=0 ; i<shortcut_mouse_line ; i++) 
        button  = button->next;
    
    if (button){
        int x = button->select_rect.x + button->select_rect.w/2;
        int y = button->select_rect.y + button->select_rect.h/2;
        if      (x < 0)             x = 0;
        else if (x >= screen_width) x = screen_width-1;
        if      (y < 0)              y = 0;
        else if (y >= screen_height) y = screen_height-1;

        x = x * screen_device_width / screen_width + render_view_rect.x;
        y = y * screen_device_height / screen_height + render_view_rect.y;

        shift_over_button = button->no;
        warpMouse(x, y);
    }
}

bool ONScripter::keyDownEvent( SDL_KeyboardEvent *event )
{
    if (event->keysym.sym == SDLK_ESCAPE){
        current_button_state.event_type = SDL_MOUSEBUTTONDOWN;
        current_button_state.event_button = SDL_BUTTON_RIGHT;
    }
    else if (event->keysym.sym == SDLK_KP_ENTER){
        current_button_state.event_type = SDL_MOUSEBUTTONDOWN;
        current_button_state.event_button = SDL_BUTTON_LEFT;
    }
    else if (event->keysym.sym == SDLK_LEFT){
        current_button_state.event_type = SDL_MOUSEBUTTONDOWN;
        current_button_state.event_button = (unsigned char)SDL_MOUSEWHEEL;
        current_button_state.y = 1;
    }
    else if (event->keysym.sym == SDLK_RIGHT){
        current_button_state.event_type = SDL_MOUSEBUTTONDOWN;
        current_button_state.event_button = (unsigned char)SDL_MOUSEWHEEL;
        current_button_state.y = -1;
    }

    switch ( event->keysym.sym ) {
      case SDLK_RCTRL:
        ctrl_pressed_status  |= 0x01;
        sprintf(current_button_state.str, "CTRL");
        goto ctrl_pressed;
      case SDLK_LCTRL:
        ctrl_pressed_status  |= 0x02;
        sprintf(current_button_state.str, "CTRL");
        goto ctrl_pressed;
      case SDLK_RSHIFT:
        shift_pressed_status |= 0x01;
        break;
      case SDLK_LSHIFT:
        shift_pressed_status |= 0x02;
        break;
      default:
        break;
    }

    return false;

  ctrl_pressed:
    current_button_state.button  = 0;
    playClickVoice();
    stopAnimation( clickstr_state );

    return true;
}

void ONScripter::keyUpEvent( SDL_KeyboardEvent *event )
{
    if (event->keysym.sym == SDLK_ESCAPE){
        current_button_state.event_type = SDL_MOUSEBUTTONUP;
        current_button_state.event_button = SDL_BUTTON_RIGHT;
    }
    else if (event->keysym.sym == SDLK_KP_ENTER){
        current_button_state.event_type = SDL_MOUSEBUTTONUP;
        current_button_state.event_button = SDL_BUTTON_LEFT;
    }
    else if (event->keysym.sym == SDLK_LEFT){
        current_button_state.event_type = SDL_MOUSEBUTTONUP;
        current_button_state.event_button = (unsigned char)SDL_MOUSEWHEEL;
        current_button_state.y = 1;
    }
    else if (event->keysym.sym == SDLK_RIGHT){
        current_button_state.event_type = SDL_MOUSEBUTTONUP;
        current_button_state.event_button =  (unsigned char)SDL_MOUSEWHEEL;
        current_button_state.y = -1;
    }

    switch ( event->keysym.sym ) {
      case SDLK_RCTRL:
        ctrl_pressed_status  &= ~0x01;
        break;
      case SDLK_LCTRL:
        ctrl_pressed_status  &= ~0x02;
        break;
      case SDLK_RSHIFT:
        shift_pressed_status &= ~0x01;
        break;
      case SDLK_LSHIFT:
        shift_pressed_status &= ~0x02;
        break;
      default:
        break;
    }
}

bool ONScripter::keyPressEvent( SDL_KeyboardEvent *event )
{
    current_button_state.button = 0;
    current_button_state.down_flag = false;
    if ( automode_flag ){
        automode_flag = false;
        return false;
    }
    
    if ( event->type == SDL_KEYUP ){
        if ((trace_fp || dump_vars_path) && event->keysym.sym == SDLK_F8){ // CONscripter: the state now, for the trace
            if (trace_fp) tracePage( true );
            if (dump_vars_path) writeVarsFile();
            return false;
        }
#if !defined(WINRT) && !defined(WEB) && (defined(WIN32) || defined(_WIN32) || defined(LINUX) || defined(MACOSX))
      if ((event->keysym.mod & KMOD_ALT) && event->keysym.sym == SDLK_RETURN) {
        setFullScreen(!fullscreen_mode);
        return true;
      }
      if (event->keysym.sym == SDLK_F10) {
        stretch_mode = !fullscreen_mode; 
        setFullScreen(!fullscreen_mode);
        return true;
      }
#endif
        if ( variable_edit_mode ){
            variableEditMode( event );
            return false;
        }

        if ( edit_flag && event->keysym.sym == SDLK_z ){
            variable_edit_mode = EDIT_SELECT_MODE;
            variable_edit_sign = 1;
            variable_edit_num = 0;
            sprintf( wm_edit_string, "%s%s", EDIT_MODE_PREFIX, EDIT_SELECT_STRING );
            setCaption( wm_edit_string, wm_icon_string );
        }
    }
    
    // a key ends skip mode, as a click does; CONscripter: but not a
    // modifier or function key (Shift held for a dictionary popup, Alt+Tab,
    // F8 for the trace, F10 for full screen), which isn't meant for the game
    if (event->type == SDL_KEYUP &&
        !(event->keysym.sym >= SDLK_LCTRL && event->keysym.sym <= SDLK_RGUI) &&
        !(event->keysym.sym >= SDLK_F1 && event->keysym.sym <= SDLK_F12) &&
        event->keysym.sym != SDLK_CAPSLOCK)
        skip_mode &= ~SKIP_NORMAL;
    
    if ( shift_pressed_status && event->keysym.sym == SDLK_q && current_mode == NORMAL_MODE ){
        trace_end_reason = "player quit"; // CONscripter
        endCommand();
    }

    if ( (trap_mode & TRAP_LEFT_CLICK) && 
         (event->keysym.sym == SDLK_RETURN ||
          event->keysym.sym == SDLK_KP_ENTER ||
          event->keysym.sym == SDLK_SPACE ) ){
        if (trapHandler()) return true;
    }
    else if ( (trap_mode & TRAP_RIGHT_CLICK) && 
              (event->keysym.sym == SDLK_ESCAPE) ){
        if (trapHandler()) return true;
    }
    
    if ( event_mode & WAIT_BUTTON_MODE &&
         (((event->type == SDL_KEYUP || btndown_flag) &&
           ((!getenter_flag && event->keysym.sym == SDLK_RETURN) ||
            (!getenter_flag && event->keysym.sym == SDLK_KP_ENTER))) ||
          ((spclclk_flag || !useescspc_flag) && event->keysym.sym == SDLK_SPACE)) ){
        if ( event->keysym.sym == SDLK_RETURN ||
             event->keysym.sym == SDLK_KP_ENTER ||
             (spclclk_flag && event->keysym.sym == SDLK_SPACE) ){
            current_button_state.button = current_over_button;
            if (current_over_button == -1){
                if (!bexec_flag) current_button_state.button = 0;
                sprintf(current_button_state.str, "RETURN");
            }
            else{
                sprintf(current_button_state.str, "S%d", current_over_button);
                if (bexec_flag && current_button_link){
                    ButtonLink *cbl = current_button_link;
                    if ( current_button_link->exbtn_ctl[2] ){
                        SDL_Rect check_src_rect = cbl->image_rect;
                        SDL_Rect check_dst_rect = {0, 0, 0, 0};
                        decodeExbtnControl( cbl->exbtn_ctl[2], &check_src_rect, &check_dst_rect );
                    }
                    else{
                        sprite_info[ cbl->sprite_no ].visible = true;
                        sprite_info[ cbl->sprite_no ].setCell(2);
                        dirty_rect.add( cbl->image_rect );
                    }
                    flush( refreshMode() );
                }
            }
            if ( event->type == SDL_KEYDOWN )
                current_button_state.down_flag = true;
        }
        else{
            current_button_state.button = -1;
            if (!bexec_flag) current_button_state.button = 0;
            sprintf(current_button_state.str, "SPACE");
        }
        playClickVoice();
        stopAnimation( clickstr_state );

        return true;
    }

    if ( event->type == SDL_KEYDOWN ) return false;

    // CONscripter: getkey: a letter key ends the button wait with
    // -1000 minus its upper-case code, before the keys' own meanings
    if ( getkey_flag && event_mode & WAIT_BUTTON_MODE &&
         event->keysym.sym >= SDLK_a && event->keysym.sym <= SDLK_z ){
        current_button_state.button = -1000 - ('A' + event->keysym.sym - SDLK_a);
        sprintf(current_button_state.str, "%c", 'A' + event->keysym.sym - SDLK_a);
        stopAnimation( clickstr_state );
        return true;
    }
    
    if ( ( event_mode & (WAIT_INPUT_MODE | WAIT_BUTTON_MODE) ) &&
         ( autoclick_time == 0 || (event_mode & WAIT_BUTTON_MODE)) ){
        if ( !useescspc_flag && event->keysym.sym == SDLK_ESCAPE){
            current_button_state.button  = -1;
            sprintf(current_button_state.str, "RCLICK");
            if (rmode_flag && event_mode & WAIT_TEXT_MODE){
                system_menu_mode = rclickMenuMode(); // CONscripter: rgosub
            }
        }
        else if ( useescspc_flag && event->keysym.sym == SDLK_ESCAPE ){
            current_button_state.button  = -10;
        }
        else if ( !spclclk_flag && useescspc_flag && event->keysym.sym == SDLK_SPACE ){
            current_button_state.button  = -11;
        }
        else if (((!getcursor_flag && event->keysym.sym == SDLK_LEFT) ||
                  event->keysym.sym == SDLK_h) &&
                 (event_mode & WAIT_TEXT_MODE ||
                  (usewheel_flag && !getcursor_flag && event_mode & WAIT_BUTTON_MODE) || 
                  system_menu_mode == SYSTEM_LOOKBACK)){
            current_button_state.button = -2;
            sprintf(current_button_state.str, "WHEELUP");
            if (event_mode & WAIT_TEXT_MODE) system_menu_mode = SYSTEM_LOOKBACK;
        }
        else if (((!getcursor_flag && event->keysym.sym == SDLK_RIGHT) ||
                  event->keysym.sym == SDLK_l) &&
                 ((enable_wheeldown_advance_flag && event_mode & WAIT_TEXT_MODE) ||
                  (usewheel_flag && event_mode & WAIT_BUTTON_MODE) || 
                  system_menu_mode == SYSTEM_LOOKBACK)){
            if (event_mode & WAIT_TEXT_MODE)
                current_button_state.button = 0;
            else
                current_button_state.button = -3;
            sprintf(current_button_state.str, "WHEELDOWN");
        }
        else if (((!getcursor_flag && event->keysym.sym == SDLK_UP) ||
                  event->keysym.sym == SDLK_k ||
                  event->keysym.sym == SDLK_p) &&
                 event_mode & WAIT_BUTTON_MODE){
            shiftCursorOnButton(1);
            return false;
        }
        else if (((!getcursor_flag && event->keysym.sym == SDLK_DOWN) ||
                  event->keysym.sym == SDLK_j ||
                  event->keysym.sym == SDLK_n) &&
                 event_mode & WAIT_BUTTON_MODE){
            shiftCursorOnButton(-1);
            return false;
        }
        else if ( getpageup_flag && event->keysym.sym == SDLK_PAGEUP ){
            current_button_state.button  = -12;
            sprintf(current_button_state.str, "PAGEUP");
        }
        else if ( getpagedown_flag && event->keysym.sym == SDLK_PAGEDOWN ){
            current_button_state.button  = -13;
            sprintf(current_button_state.str, "PAGEDOWN");
        }
        else if ( (getenter_flag && event->keysym.sym == SDLK_RETURN) ||
                  (getenter_flag && event->keysym.sym == SDLK_KP_ENTER) ){
            current_button_state.button  = -19;
        }
        else if ( gettab_flag && event->keysym.sym == SDLK_TAB ){
            current_button_state.button  = -20;
        }
        else if ( getcursor_flag && event->keysym.sym == SDLK_UP ){
            current_button_state.button  = -40;
            sprintf(current_button_state.str, "UP");
        }
        else if ( getcursor_flag && event->keysym.sym == SDLK_RIGHT ){
            current_button_state.button  = -41;
            sprintf(current_button_state.str, "RIGHT");
        }
        else if ( getcursor_flag && event->keysym.sym == SDLK_DOWN ){
            current_button_state.button  = -42;
            sprintf(current_button_state.str, "DOWN");
        }
        else if ( getcursor_flag && event->keysym.sym == SDLK_LEFT ){
            current_button_state.button  = -43;
            sprintf(current_button_state.str, "LEFT");
        }
        else if ( getinsert_flag && event->keysym.sym == SDLK_INSERT ){
            current_button_state.button  = -50;
        }
        else if ( getzxc_flag && event->keysym.sym == SDLK_z ){
            current_button_state.button  = -51;
        }
        else if ( getzxc_flag && event->keysym.sym == SDLK_x ){
            current_button_state.button  = -52;
        }
        else if ( getzxc_flag && event->keysym.sym == SDLK_c ){
            current_button_state.button  = -53;
        }
        else if ( getfunction_flag && 
                  event->keysym.sym >= SDLK_F1 && event->keysym.sym <= SDLK_F12 ){
            current_button_state.button = -21-(event->keysym.sym - SDLK_F1);
            sprintf(current_button_state.str, "F%d", event->keysym.sym - SDLK_F1+1);
        }
        else if ( bexec_flag && 
                  event->keysym.sym >= SDLK_0 && event->keysym.sym <= SDLK_9 ){
            current_button_state.button = -1; // dummy
            sprintf(current_button_state.str, "%d", event->keysym.sym - SDLK_0);
        }
        else if ( bexec_flag && 
                  event->keysym.sym >= SDLK_a && event->keysym.sym <= SDLK_z ){
            current_button_state.button = -1; // dummy
            sprintf(current_button_state.str, "%c", 'A' + event->keysym.sym - SDLK_a);
        }
        else if ( bexec_flag && 
                  (event->keysym.sym == SDLK_RSHIFT || event->keysym.sym == SDLK_LSHIFT) ){
            current_button_state.button = -1; // dummy
            sprintf(current_button_state.str, "SHIFT");
        }
        
        if ( current_button_state.button != 0 ){
            stopAnimation( clickstr_state );

            return true;
        }
    }

    if ( event_mode & WAIT_INPUT_MODE &&
         ( autoclick_time == 0 || (event_mode & WAIT_BUTTON_MODE)) ){
        if (event->keysym.sym == SDLK_RETURN || 
            event->keysym.sym == SDLK_KP_ENTER ||
            event->keysym.sym == SDLK_SPACE ){
            if (!(event_mode & WAIT_TEXT_MODE))
                skip_mode |= SKIP_TO_EOL;
            playClickVoice();
            stopAnimation( clickstr_state );

            return true;
        }
    }
    
    if ( event_mode & WAIT_INPUT_MODE ){
        if (event->keysym.sym == SDLK_s && !automode_flag ){
            skip_mode |= SKIP_NORMAL;
            utils::printInfo("toggle skip to true\n");
            stopAnimation( clickstr_state );

            return true;
        }
        else if (event->keysym.sym == SDLK_o){
            if (skip_mode & SKIP_TO_EOP)
                skip_mode &= ~SKIP_TO_EOP;
            else
                skip_mode |= SKIP_TO_EOP;
            utils::printInfo("toggle draw one page flag to %s\n", (skip_mode & SKIP_TO_EOP?"true":"false") );
            if ( skip_mode & SKIP_TO_EOP ){
                stopAnimation( clickstr_state );

                return true;
            }
        }
        else if ( event->keysym.sym == SDLK_a && !automode_flag ){
            automode_flag = true;
            skip_mode &= ~SKIP_NORMAL;
            utils::printInfo("change to automode\n");
            stopAnimation( clickstr_state );

            return true;
        }
        else if ( event->keysym.sym == SDLK_0 ){
            if (++text_speed_no > 2) text_speed_no = 0;
            sentence_font.wait_time = -1;
        }
        else if ( event->keysym.sym == SDLK_1 ){
            text_speed_no = 0;
            sentence_font.wait_time = -1;
        }
        else if ( event->keysym.sym == SDLK_2 ){
            text_speed_no = 1;
            sentence_font.wait_time = -1;
        }
        else if ( event->keysym.sym == SDLK_3 ){
            text_speed_no = 2;
            sentence_font.wait_time = -1;
        }
    }

    if ( event_mode & ( WAIT_INPUT_MODE | WAIT_BUTTON_MODE ) ){
        if ( event->keysym.sym == SDLK_f ){ // alt+f for fullscreen
            if ( fullscreen_mode ) menu_windowCommand();
            else                   menu_fullCommand();
        }
    }

    return false;
}

void ONScripter::timerEvent(bool init_flag)
{
    int current_time = SDL_GetTicks();
    int remaining_time = next_time;
    if (next_time > 0){
        remaining_time -= current_time;
        if (remaining_time < 0) remaining_time = 0;
    }

    //trick to increase frame rate
    if (remaining_time == 0 && !init_flag){
        SDL_Event event;
        event.type = ONS_BREAK_EVENT;
        SDL_PushEvent(&event);
        return;
    }
    
    int duration = 0;
    if (event_mode & WAIT_TIMER_MODE){
        proceedAnimation(current_time);
        // CONscripter: from the clock now (drawing took time); a step due
        // already is due in 1 ms, not dropped until the wait ends
        int next = calcDurationToNextAnimation();
        duration = next - (int)SDL_GetTicks();
        if (duration <= 0) duration = next > current_time ? 1 : 0;
    }

    // trick to increase frame rate
    if (remaining_time == 0 && init_flag) {
        SDL_Event event;
        event.type = ONS_BREAK_EVENT;
        SDL_PushEvent(&event);
        return;
    }

    if (duration > 0){
        if (duration > remaining_time && remaining_time > 0)
            duration = remaining_time;

        if (timer_id) SDL_RemoveTimer(timer_id);
        timer_id = SDL_AddTimer(duration, timerCallback, NULL);
#if defined(WEB)
        timer_due = SDL_GetTicks() + duration;
#endif
    }
    else if (remaining_time > 0){
        if (timer_id) SDL_RemoveTimer(timer_id);
        timer_id = SDL_AddTimer(remaining_time, timerCallback, NULL);
#if defined(WEB)
        timer_due = SDL_GetTicks() + remaining_time;
#endif
    }
}

#if (defined(IOS) || defined(ANDROID) || defined(WINRT))
SDL_MouseWheelEvent transTouchKey(SDL_TouchFingerEvent &finger) {
    static struct FingerPoint {
        float x, y;
    } finger_start;

    static Sint32 old_key = 0;
    SDL_MouseWheelEvent mw;
    if (finger.type == SDL_FINGERDOWN) {
        finger_start.x = finger.x;
        finger_start.y = finger.y;
    } else if (finger.type == SDL_FINGERMOTION) {
        float dtfinger = finger.y - finger_start.y;
        Sint32 key = 0;
        if (dtfinger > 0.5) key = -1;
        else if (dtfinger < -0.5) key = 1;
        if (old_key != key) {
            mw.y = key;
            old_key = key;
            return mw;
        }
    }
    mw.y = 0;
    return mw;
}

bool ONScripter::convTouchKey(SDL_TouchFingerEvent &finger) {
    SDL_MouseWheelEvent mw = transTouchKey(finger);
    if (mw.y != 0) return mouseWheelEvent(&mw);
    return false;
}
#endif

#if defined(WEB)
// CONscripter: SDL_WaitEvent in the browser polls with SDL_Delay(1): an
// ASYNCIFY unwind and rewind of the whole stack each browser timer (4 ms),
// ~250 a second while a game waits for a click. Sleep until the engine's
// timer is due instead, at most a 60 Hz frame (input waits no longer).
static int waitNextEvent( SDL_Event *event )
{
    while (!SDL_PollEvent( event )){
        int wait = 16;
        if (timer_id){
            int left = (int)(timer_due - SDL_GetTicks());
            // due: its callback runs any moment (long past: a timer that won't)
            if (left < wait && left > -100) wait = left > 0 ? left : 1;
        }
        emscripten_sleep( wait );
    }
    return 1;
}
#else
#define waitNextEvent SDL_WaitEvent
#endif

void ONScripter::runEventLoop()
{
    SDL_Event event, tmp_event;

    while (1) {
        if (!SDL_PollEvent(&event)){
            // CONscripter: what was drawn goes up once, before the engine
            // sleeps: not while events wait (SDL_PollEvent also returns 0
            // at SDL's poll sentinel), for at most SKIP_PRESENT_MS. A wait
            // shorter than a frame leaves it to the next if one went up
            // within a frame (text faster than vsync), and while skipping
            // goes up at most every SKIP_PRESENT_MS.
            Uint32 now = SDL_GetTicks();
            bool busy = SDL_HasEvents( SDL_FIRSTEVENT, SDL_LASTEVENT ) && now - present_time < SKIP_PRESENT_MS;
            bool sleeps = next_time <= 0 || next_time - (int)now >= FRAME_MS;
            if (!busy && sleeps && present_pending) presentScreen();
            else if (!busy && !sleeps && now - present_time >= FRAME_MS) presentPending();
            if (trace_fp) traceSounds(); // CONscripter
            consIdle( true );
            bool got = consClockWait(&event) || waitNextEvent(&event); // CONscripter: the virtual clock's timers first
            consIdle( false );
            if (!got) break;
        }
        tmp_event = event; // fix android long click problem
#if defined(USE_SMPEG)
        // required to repeat the movie
        if (layer_smpeg_sample)
            SMPEG_status(layer_smpeg_sample);
#endif    
        bool ret = false;
        // ignore continous SDL_MOUSEMOTION
        while (event.type == SDL_MOUSEMOTION || event.type == SDL_FINGERMOTION) {
            if ( SDL_PeepEvents( &tmp_event, 1, SDL_PEEKEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT ) == 0 ) break;
            if (tmp_event.type != SDL_MOUSEMOTION && tmp_event.type != SDL_FINGERMOTION) break;
            SDL_PeepEvents( &tmp_event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT );

            event = tmp_event;
        }

        if (hold_release && holdRelease( event )) continue; // CONscripter: mpeghold
#if defined(KOBO)
        if (event.type == kobo::sleep_event){ kobo::sleep(); continue; }
#endif

        switch (event.type) {
#if defined(IOS) || defined(ANDROID) || defined(WINRT)
        case SDL_FINGERMOTION:
        {
            if (!btndown_flag && convTouchKey(event.tfinger)) return;
            tmp_event.motion.x = (device_width * event.tfinger.x - render_view_rect.x) * screen_scale_ratio1;
            tmp_event.motion.y = (device_height * event.tfinger.y - render_view_rect.y) * screen_scale_ratio2;
            if (mouseMoveEvent( &tmp_event.motion )) return;
            if (btndown_flag){
                event.button.type = SDL_MOUSEBUTTONDOWN;
                event.button.button = SDL_BUTTON_LEFT;
                if (SDL_GetNumTouchFingers(event.tfinger.touchId) >= 3)
                    event.button.button = SDL_BUTTON_RIGHT;
                event.button.x = tmp_event.motion.x;
                event.button.y = tmp_event.motion.y;
                ret = mousePressEvent( &event.button );
                if (ret) return;
            }
        }
            break;
        case SDL_FINGERDOWN:
        {
            g_lastpress_time = event.button.timestamp; // fix long press on touch
            convTouchKey(event.tfinger);
            tmp_event.motion.x = (device_width * event.tfinger.x - render_view_rect.x) * screen_scale_ratio1;
            tmp_event.motion.y = (device_height * event.tfinger.y - render_view_rect.y) * screen_scale_ratio2;
            if (mouseMoveEvent( &tmp_event.motion )) return;
            if ( btndown_flag ){
                tmp_event.button.type = SDL_MOUSEBUTTONDOWN;
                tmp_event.button.button = SDL_BUTTON_LEFT;
                if (SDL_GetNumTouchFingers(event.tfinger.touchId) >= 3)
                    tmp_event.button.button = SDL_BUTTON_RIGHT;
                tmp_event.button.x = (device_width * event.tfinger.x - render_view_rect.x) * screen_scale_ratio1;
                tmp_event.button.y = (device_height * event.tfinger.y - render_view_rect.y) * screen_scale_ratio2;
                ret = mousePressEvent( &tmp_event.button );
            }
            {
                num_fingers = SDL_GetNumTouchFingers(event.tfinger.touchId);
                if (num_fingers >= 4){
                    tmp_event.key.keysym.sym = SDLK_LCTRL;
                    ret |= keyDownEvent( &tmp_event.key );
                }
            }
            if (ret) return;
        }
            break;
        case SDL_FINGERUP:
        {
            if (num_fingers == 0) break;
            {
                tmp_event.button.type = SDL_MOUSEBUTTONUP;
                tmp_event.button.button = SDL_BUTTON_LEFT;
                if (num_fingers == 3)
                    tmp_event.button.button = SDL_BUTTON_RIGHT;
                tmp_event.button.x = (device_width * event.tfinger.x - render_view_rect.x) * screen_scale_ratio1;
                tmp_event.button.y = (device_height * event.tfinger.y - render_view_rect.y) * screen_scale_ratio2;
                ret = mousePressEvent( &tmp_event.button );
            }
            tmp_event.key.keysym.sym = SDLK_LCTRL;
            keyUpEvent( &tmp_event.key );
            num_fingers = 0;
            if (ret) return;
        }
            break;
#endif
#if !defined(ANDROID) && !defined(IOS) && !defined(WINRT)
          case SDL_MOUSEMOTION:
            // printf("## SDL_MOUSEMOTION (%d, %d) ", event.button.x, event.button.y);
#if !defined(WEB)
            event.button.x = (event.button.x - render_view_rect.x) * screen_scale_ratio1;
            event.button.y = (event.button.y - render_view_rect.y) * screen_scale_ratio2;
#endif
            // printf("-> (%d, %d)\n", event.button.x, event.button.y);
            if (mouseMoveEvent( &event.motion )) return;
            if (btndown_flag){
                if (event.motion.state & SDL_BUTTON(SDL_BUTTON_LEFT))
                    tmp_event.button.button = SDL_BUTTON_LEFT;
                else if (event.motion.state & SDL_BUTTON(SDL_BUTTON_RIGHT))
                    tmp_event.button.button = SDL_BUTTON_RIGHT;
                else
                    break;
                tmp_event.button.type = SDL_MOUSEBUTTONDOWN;
                ret = mousePressEvent( &tmp_event.button );
                if (ret) return;
            }
            break;
            
          case SDL_MOUSEBUTTONDOWN:
            if(event.button.button == SDL_BUTTON_LEFT) {
                g_lastpress_time = event.button.timestamp;
            }
            current_button_state.event_type = event.type;
            current_button_state.event_button = event.button.button;
            if ( !btndown_flag ){
                if ( event.button.button == SDL_BUTTON_LEFT ){
                    consPressCell( true ); // CONscripter
                    if (screenPress( event.button.x, event.button.y )) return; // CONscripter: a screen slider's knob
                }
                break;
            }
          case SDL_MOUSEBUTTONUP:
            current_button_state.event_type = event.type;
            current_button_state.event_button = event.button.button;
#if !defined(WEB)
            // printf("## SDL_MOUSEBUTTONUP (%d, %d) ", event.button.x, event.button.y);
            event.button.x = (event.button.x - render_view_rect.x) * screen_scale_ratio1;
            event.button.y = (event.button.y - render_view_rect.y) * screen_scale_ratio2;
            // printf("-> (%d, %d)\n", event.button.x, event.button.y);
#endif

            ret = mousePressEvent( &event.button );
            if (ret) return;
            break;
          case SDL_MOUSEWHEEL:
            ret = mouseWheelEvent(&event.wheel);
            if (ret) return;
            break;
#endif

          case SDL_CONTROLLERDEVICEADDED:
            utils::printInfo("Open GameController %d\n", event.cdevice.which);
            if (controller != NULL) {
                SDL_GameControllerClose(controller);
                controller = NULL;
            }
            controller = SDL_GameControllerOpen(event.cdevice.which);
            break;

          case SDL_CONTROLLERDEVICEREMOVED:
            utils::printInfo("Close GameController %d\n", event.cdevice.which);
            if (controller != NULL) {
                SDL_GameControllerClose(controller);
                controller = NULL;
            }
            break;

          case SDL_CONTROLLERAXISMOTION: // CONscripter: triggers as buttons, sticks polled
            if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT){
                int t = event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
                bool down = event.caxis.value > PAD_TRIGGER;
                if (down != pad_trigger[t]){
                    pad_trigger[t] = down;
                    if (padButton( PAD_LT + t, down )) return;
                }
            }
            else if (!pad_timer && abs( event.caxis.value ) > PAD_DEADZONE){
                if (padTick()) return;
            }
            break;

          case ONS_PAD_EVENT:
            if (padTick()) return;
            break;

          case SDL_CONTROLLERBUTTONDOWN:
          case SDL_CONTROLLERBUTTONUP:
            if (padButton( event.cbutton.button, event.type == SDL_CONTROLLERBUTTONDOWN )) return;
            break;

          case SDL_KEYDOWN:
            event.key.keysym.sym = transKey(event.key.keysym.sym);
            ret = keyDownEvent( &event.key );
            if ( btndown_flag )
                ret |= keyPressEvent( &event.key );
            if (ret) return;
            break;

          case SDL_KEYUP:
            event.key.keysym.sym = transKey(event.key.keysym.sym);
            keyUpEvent( &event.key );
            ret = keyPressEvent( &event.key );
            if (ret) return;
            break;

          case ONS_TIMER_EVENT:
            timerEvent(false);
            break;

          case ONS_WEB_EVENT:
            if (webEvent( event.user.code, (int)(intptr_t)event.user.data1 )) return;
            break;

          case ONS_SHOT_EVENT: // CONscripter: a --shot-after time came
            traceShotAfter();
            break;

          case ONS_FREQ_END_EVENT: // CONscripter: chfreq's sound played out
            endFreqChannel( event.user.code );
            break;

          case ONS_MUSIC_EVENT:
          case ONS_BGMFADE_EVENT:
          case ONS_CDAUDIO_EVENT:
          case ONS_MIDI_EVENT:
            flushEventSub( event );
            break;

          case ONS_CHUNK_EVENT:
            flushEventSub( event );
            //utils::printInfo("ONS_CHUNK_EVENT %d: %x %d %x\n", event.user.code, wave_sample[0], automode_flag, event_mode);
            if ( event.user.code != 0 ||
                 !(event_mode & WAIT_VOICE_MODE) ) break;

            event_mode &= ~WAIT_VOICE_MODE;

          case ONS_BREAK_EVENT:
            if (event_mode & WAIT_VOICE_MODE && wave_sample[0]){
                next_time = -1;
                timerEvent(false);
                break;
            }

            if (automode_flag || autoclick_time > 0)
                current_button_state.button = 0;
            else if ( usewheel_flag ){
                current_button_state.button = -5;
                sprintf(current_button_state.str, "TIMEOUT");
            }
            else{
                current_button_state.button = -2;
                sprintf(current_button_state.str, "TIMEOUT");
            }

            if (event_mode & (WAIT_INPUT_MODE | WAIT_BUTTON_MODE) && 
                ( clickstr_state == CLICK_WAIT || 
                  clickstr_state == CLICK_NEWPAGE ) ){
                playClickVoice(); 
                stopAnimation( clickstr_state ); 
            }

            return;
          case SDL_WINDOWEVENT:  
              switch (event.window.event) {
              case SDL_WINDOWEVENT_EXPOSED:
                  repaintCommand();
                  break;
              case SDL_WINDOWEVENT_FOCUS_LOST:
#if defined(_DEBUG) // fix background music
                  Mix_Pause(-1);
                  Mix_PauseMusic();
#endif
                  // the mouse cursor leaves the window
                  SDL_MouseMotionEvent mevent;
                  mevent.x = screen_device_width;
                  mevent.y = screen_device_height;
                  mouseMoveEvent(&mevent);
                  break;
              case SDL_WINDOWEVENT_FOCUS_GAINED:
                  Mix_ResumeMusic();
                  Mix_Resume(-1);
                  break;
              case SDL_WINDOWEVENT_RESIZED:
                  calcRenderRect();
                  repaintCommand();
                  break;
              }
              break;
          case SDL_APP_WILLENTERBACKGROUND:
#if defined(USE_GLES)
                if(gles_renderer) gles_renderer->pause();
#endif
              break;
          case SDL_APP_DIDENTERFOREGROUND:
#if defined(ANDROID)
                utils::printInfo("## SDL_APP_DIDENTERFOREGROUND");
                SDL_Delay(200); // prevents not recognize the proper size of window
#endif
#if defined(USE_GLES)
              if(gles_renderer) gles_renderer->resume();
#endif
              repaintCommand();
              break;
          case SDL_QUIT:
            trace_end_reason = "window closed"; // CONscripter: or a signal (quitReason)
            endCommand();
            break;
            
          default:
            break;
        }
    }
}
