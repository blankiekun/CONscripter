/* -*- C++ -*-
 *
 *  ONScripter_cons.cpp - CONscripter additions to ONScripter
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
#include "ONScripter_audio.h"
#include "Utils.h"
#include "coding2utf16.h"
#include "gbk2utf16.h"
#include "version.h"
#include "DirectReader.h"
#include <unordered_map>
#include <algorithm>
#include <climits>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif
#include <vector>
#if defined(WEB)
#include <emscripten.h>
#endif
#if defined(USE_WEBP)
#include <webp/decode.h>
#endif

// Marks the block saveSaveFileCons() appends to a save file. Saves
// written without it (stock ONScripter, older CONscripter) end before it,
// and readInt() returns 0 past the end of the data.
#define CONS_SAVE_MAGIC   0x534e4f43 // "CONS"
#define CONS_SAVE_VERSION 31 // the "Version n" comments below name the parts in the order they are
                            // written (version 26's first). A new part goes at the end; loading
                            // reads the parts an older save has and leaves the rest at their defaults
#define CONS_SAVE_OLDEST  18 // the oldest block loading reads: version 18 reshaped earlier parts
int consSaveVersion() { return CONS_SAVE_VERSION; }

extern SDL_TimerID timer_bgmfade_id; // bgmfadein, bgmfadeout

/* clickskip (define)
 *   A click (or key) that ends a delay or an effect early also ends every
 *   later delay and effect at once (an effect drawn as print 1), until the
 *   next text, wait for a click (click, btnwait, ...) or mpegplay. Xuse's
 *   skip_state_b (最果てのイマ). getclickskip %v: 1 while it does. */
int ONScripter::clickskipCommand()
{
    if ( current_mode != DEFINE_MODE )
        errorAndExit( "clickskip: not in the define section." );
    click_skip_flag = true;

    return RET_CONTINUE;
}

int ONScripter::getclickskipCommand()
{
    script_h.readInt();
    script_h.setInt( &script_h.current_variable, click_skipping ? 1 : 0 );

    return RET_CONTINUE;
}

// After a clickable wait (delay, an effect's frame): a click or key ended it,
// not its time (auto mode's timeouts count as time).
bool ONScripter::waitInterrupted()
{
    return !((automode_flag || autoclick_time > 0) ||
             (usewheel_flag  && current_button_state.button == -5) ||
             (!usewheel_flag && current_button_state.button == -2));
}

/* bitand %var,a,b / bitor %var,a,b / bitxor %var,a,b
 *   %var = a & b, a | b, a ^ b, as 32-bit integers: flags packed in one
 *   variable, which NScripter's arithmetic reaches only a bit at a time
 *   with div and mod. "bitand %0,%5,4:if %0 != 0 ..." tests bit 2. */
int ONScripter::bitandCommand()
{
    bool is_or = script_h.isName("bitor"), is_xor = script_h.isName("bitxor");
    script_h.readInt();
    script_h.pushVariable();
    int a = script_h.readInt();
    int b = script_h.readInt();
    script_h.setInt( &script_h.pushed_variable, is_or ? (a | b) : is_xor ? (a ^ b) : (a & b) );
    return RET_CONTINUE;
}

/* CONscripter
 * seldisable n[,n...]
 *   Options n (from 1) of the next select, selgosub or selnum are shown
 *   greyed (selectcolor's off colour at half alpha) and can't be chosen:
 *   the mouse doesn't light them up or click them, the cursor keys pass
 *   over them, auto-play doesn't take them. They keep their place and
 *   number. The next select uses the list up; put seldisable just before
 *   it (no save is made in between, so a load runs it again).
 *   seldisable 2:select "行く",*go,"待つ",*wait,"帰る",*home */
int ONScripter::seldisableCommand()
{
    do select_disabled.insert( script_h.readInt() );
    while (script_h.getEndStatus() & ScriptHandler::END_COMMA);
    return RET_CONTINUE;
}

/* screenfilter slot, kind[, blend[, strength[, param, param, param[, z]]]]
 *   A filter like spfilter's (same kinds, blends and parameters) on the
 *   whole picture: the background and the sprites behind the text window
 *   (those above the humanz number), where monocro applies. Not the text
 *   window or the sprites in front of it. Slots 1-4 apply in order; kind 0
 *   clears a slot and "screenfilter 0" clears them all. Like spfilter it
 *   doesn't draw: show the result with print (an effect fades it in). The
 *   filters are kept in save files and cleared by reset. With z (a sprite
 *   number) the slot applies right after sprite z (and those spz puts over
 *   it) is drawn: the sprites above z stay unfiltered, as an object over
 *   a filter in the scene; z among the sprites in front of the window
 *   filters the window too.
 *   "screenfilter 1,4,0,256,-200" darkens the scene for night;
 *   "screenfilter 1,6,0,256,4,4,0,999" blurs the background (and sprite
 *   999) only. */
int ONScripter::screenfilterCommand()
{
    leaveTextDisplayMode();

    int slot = script_h.readInt();
    AnimationInfo::Filter f;
    f.kind = AnimationInfo::FILTER_NONE;
    f.blend = AnimationInfo::FILTER_BLEND_COPY;
    f.strength = 256;
    f.param[0] = f.param[1] = f.param[2] = 0;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA)
        f.kind = script_h.readInt();
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA)
        f.blend = script_h.readInt();
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA)
        f.strength = script_h.readInt();
    for (int i=0 ; i<3 && (script_h.getEndStatus() & ScriptHandler::END_COMMA) ; i++)
        f.param[i] = script_h.readInt();
    int z = -1;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA)
        z = script_h.readInt();
    if (z >= MAX_SPRITE_NUM) z = -1;

    if (slot == 0){
        clearScreenFilters();
    }
    else if (slot < 0 || slot > MAX_SCREEN_FILTER){
        utils::printError("screenfilter: slot %d out of range\n", slot);
        return RET_CONTINUE;
    }
    else if (f.kind < AnimationInfo::FILTER_NONE || f.kind > AnimationInfo::FILTER_LAST){
        utils::printError("screenfilter: unknown kind %d\n", f.kind);
        return RET_CONTINUE;
    }
    else if (f.blend < AnimationInfo::FILTER_BLEND_COPY || f.blend > AnimationInfo::FILTER_BLEND_DODGE){
        utils::printError("screenfilter: unknown blend %d\n", f.blend);
        return RET_CONTINUE;
    }
    else{
        if (f.strength < 0)   f.strength = 0;
        if (f.strength > 256) f.strength = 256;
        screen_filter[slot-1] = f;
        screen_filter_z[slot-1] = z < 0 ? -1 : z;
    }
    dirty_rect.fill( screen_width, screen_height );

    return RET_CONTINUE;
}

void ONScripter::clearScreenFilters()
{
    for (int i=0 ; i<MAX_SCREEN_FILTER ; i++){
        screen_filter[i].kind = AnimationInfo::FILTER_NONE;
        screen_filter[i].blend = AnimationInfo::FILTER_BLEND_COPY;
        screen_filter[i].strength = 256;
        screen_filter[i].param[0] = screen_filter[i].param[1] = screen_filter[i].param[2] = 0;
        screen_filter_z[i] = -1;
    }
    screen_filters_applied = 0;
}

/* screenflash opacity[,"#RRGGBB"]
 *   Adds the colour (white when not given) at opacity/256 (0-256) to every
 *   pixel of the screen, over everything: the picture, the text window,
 *   the sprites in front of it and the buttons, as a flash of lightning
 *   does. It doesn't draw: follow it with print. "screenflash 0" takes it
 *   away, as reset does. Not kept in save files.
 *   "screenflash 128:print 1:delay 100:screenflash 0:print 1" */
int ONScripter::screenflashCommand()
{
    screen_flash_opacity = script_h.readInt();
    if (screen_flash_opacity < 0) screen_flash_opacity = 0;
    if (screen_flash_opacity > 256) screen_flash_opacity = 256;
    screen_flash_colour[0] = screen_flash_colour[1] = screen_flash_colour[2] = 0xff;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA)
        readColor( &screen_flash_colour, script_h.readStr() );
    dirty_rect.fill( screen_width, screen_height );

    return RET_CONTINUE;
}

void ONScripter::applyScreenFlash( SDL_Surface *surface, SDL_Rect &clip )
{
    if (screen_flash_opacity == 0 || surface == NULL || surface->format->BytesPerPixel != 4) return;
    SDL_PixelFormat *fmt = surface->format;
    Uint32 add[3], mask[3] = {fmt->Rmask, fmt->Gmask, fmt->Bmask};
    int shift[3] = {fmt->Rshift, fmt->Gshift, fmt->Bshift};
    for (int c=0 ; c<3 ; c++) add[c] = screen_flash_colour[c] * screen_flash_opacity / 256;
    SDL_LockSurface( surface );
    for (int y=clip.y ; y<clip.y+clip.h ; y++){
        Uint32 *p = (Uint32*)((Uint8*)surface->pixels + y * surface->pitch) + clip.x;
        for (int x=0 ; x<clip.w ; x++, p++){
            Uint32 out = *p & ~(mask[0] | mask[1] | mask[2]);
            for (int c=0 ; c<3 ; c++){
                Uint32 v = ((*p & mask[c]) >> shift[c]) + add[c];
                out |= (v > 0xff ? 0xff : v) << shift[c];
            }
            *p = out;
        }
    }
    SDL_UnlockSurface( surface );
}

bool ONScripter::screenFilterBlurs()
{
    for (int i=0 ; i<MAX_SCREEN_FILTER ; i++)
        if (screen_filter[i].kind == AnimationInfo::FILTER_BLUR) return true;
    return false;
}

// The slots whose z is z (drawSpriteRange, right after that sprite), or
// with z -1 those the refresh hasn't applied yet; -1 also starts the next
// refresh afresh.
void ONScripter::applyScreenFilters( SDL_Surface *surface, SDL_Rect &clip, int z )
{
    AnimationInfo::Filter f[MAX_SCREEN_FILTER];
    int num = 0;
    for (int i=0 ; i<MAX_SCREEN_FILTER ; i++){
        if (screen_filter[i].kind == AnimationInfo::FILTER_NONE) continue;
        if (z >= 0 ? screen_filter_z[i] != z : (screen_filters_applied & (1 << i)) != 0) continue;
        f[num++] = screen_filter[i];
        screen_filters_applied |= 1 << i;
    }
    if (z < 0) screen_filters_applied = 0;
    if (num == 0 || surface == NULL || surface->format->BytesPerPixel != 4) return;
    AnimationInfo::applyFilterList( surface, clip, f, num );
}

/* screenshake slot[, time, interval, mode, speed_x, speed_y, x0, y0, x1, y1[, end[, curve]]]
 *   Shakes the picture behind the text window (what screenfilter
 *   filters) until the slot is cleared, or with end (> 0) for end ms,
 *   after which the slot clears itself; without waiting: the script and
 *   the text go on while it shakes. Every interval ms the picture is drawn
 *   moved by an offset whose largest size grows from x0,y0 to x1,y1
 *   pixels over time ms, then stays. Mode 0 swings each axis on a sine
 *   wave that advances speed/65536 of a turn per ms (speed_x for x,
 *   speed_y for y); mode 1 jumps along a fixed path of 7 points, moving on
 *   when speed_x * ms reaches 65536; mode 2 jolts to and fro: each step
 *   moves it by the full size, the other way from the last (speeds
 *   unused). curve (an spmotion curve: 1-3 or an spcurve table; 0 or left
 *   out straight) eases the size from x0,y0 to x1,y1. Where the picture
 *   moved away, what was there stays (it is drawn over itself).
 *   "screenshake 1,1000,100,0,800,800,0,0,30,30" shakes harder and
 *   harder for a second; "screenshake 1" stops it.
 *
 * screenripple slot[, "map", time, interval, opacity0, opacity1, amp0,
 *              amp1, angle, angle_step, phase_step[, blend[, phase]]]
 *   Ripples the same picture through a displacement map (a greyscale
 *   image as large as the screen): each pixel is taken from amp *
 *   sin(2 pi (grey / 256 + phase / 1000)) pixels away in the direction
 *   angle (tenths of a degree, 0 = right, 900 = down). The rippled picture
 *   is laid over the plain one at opacity (0-256); with blend 1 the plain
 *   picture is multiplied by the rippled one instead (darker where both
 *   are dark), at that opacity. Over time ms, opacity
 *   goes from opacity0 to opacity1 and amp from amp0 to amp1; every
 *   interval ms the angle turns by angle_step and the phase moves by
 *   phase_step, from phase (0 if left out).
 *   "screenripple 1,"wave.png",3000,100,100,150,5,10,3150,20,30"
 *
 * Either command with only a slot (1-8) clears that slot, whatever it
 * holds; slot 0 clears them all. The effects apply after the screen
 * filters, in slot order. Neither command draws: the next print shows the
 * effect, which then moves by itself. Kept in save files (restarted when
 * loaded), cleared by reset. */
int ONScripter::screenshakeCommand()
{
    return screenEffectCommand( SCREEN_EFFECT_SHAKE, "screenshake" );
}

int ONScripter::screenrippleCommand()
{
    return screenEffectCommand( SCREEN_EFFECT_RIPPLE, "screenripple" );
}

/* windowtrans opacity
 *   Draws the text window's picture (setwindow's image) at opacity
 *   (0-255, 255 = as the picture is), on top of the picture's own alpha.
 *   Kept across setwindow and loading a save (not saved itself: a game
 *   sets it again from its settings at startup); reset puts it back to
 *   255. "windowtrans 128" halves the window's opacity. */
/* automode_time ms[,char_ms]
 *   With a second number, auto mode waits ms plus char_ms for each
 *   character of the page (as some games' "page" and "character" auto
 *   waits do). Without it, as in NScripter: a negative ms is a time per
 *   character, otherwise a time per page. */
int ONScripter::automodeWait()
{
    if (automode_time < 0)
        return -automode_time * num_chars_in_sentence;
    return automode_time + automode_char_time * num_chars_in_sentence;
}

/* rgosub *label
 *   A right click (or Esc) at a text click wait, '@' or '\\' (where rmode
 *   lets it open the right-click menu), gosubs to label instead; its return
 *   comes back to that click wait, waiting again, with the text as it was
 *   (nothing drawn again or skipped). The game's own menu in place of the
 *   system one: "rgosub *menu" ... "*menu:...:return". rgosub "" turns it
 *   off. Waits on a clickstr character, and button waits, keep the
 *   right-click menu. Not saved: set it at startup. */
int ONScripter::rgosubCommand()
{
    const char *label = script_h.readStr();
    setStr( &rgosub_label, label[0] == '*' ? label + 1 : (label[0] ? label : NULL) );

    return RET_CONTINUE;
}

// What a right click at a text wait opens: rgosub's label, the menu, or the window erased.
int ONScripter::rclickMenuMode()
{
    if (rgosub_label && rgosub_return) return SYSTEM_RGOSUB;
    return root_rmenu_link.next ? SYSTEM_MENU : SYSTEM_WINDOWERASE;
}

// After a click wait (ended: it stopped the text): goes to rgosub's label
// when a right click asked for it, to come back to the same wait.
bool ONScripter::clickEndRgosub( bool ended )
{
    char *back = rgosub_return;
    rgosub_return = NULL;
    if (!ended || !rgosub_pending) return ended;
    rgosub_pending = false;
    clickstr_state = CLICK_NONE;
    gosubReal( rgosub_label, back );
    if (rgosub_1byte) script_h.backquote_resume = back; // read on as the ` string it is in
    return true;
}

/* Player settings, for a game's settings screen. None is saved: the game
 * keeps its settings and sets them again at startup.
 *
 * consset "key",value / consget %var,"key"
 *   Sets or reads one setting:
 *   mutebgm, mutese, mutevoice  1 mutes the music, the sound effects
 *     (every dwave channel but 0), the voice (channel 0), whatever their
 *     volume; muted sounds go on playing unheard. Auto mode doesn't wait
 *     for a muted voice.
 *   clickvoicestop  1: a click that ends a text wait (a click wait in the
 *     text, textbtnwait) stops the voice (channel 0). 0 (default): it
 *     plays on.
 *   autovoicewait  1 (default): auto mode waits for the voice to end
 *     before reading on. 0: it reads on after its own time and stops it.
 *   selectkeep  1: auto mode and skip go on after a select's option is
 *     chosen. 0 (default): a select ends them, as in NScripter. Reads 1
 *     only when both of the next two are 1.
 *   selectkeepauto, selectkeepskip  The same for auto mode alone, skip
 *     alone (selectkeep sets both).
 *   clickautostop  1 (default): a click or key during auto mode ends it
 *     and does nothing else. 0: auto mode goes on and the click acts as
 *     without it (reads on, presses a button).
 *   clickskipstop  1 (default): a click or key (not a modifier or F key)
 *     ends skip. 0: skip goes on; the script ends it (skipoff): a
 *     button clicked during skip is what the next textbtnwait returns.
 *   moviestop  Whether a click stops a movie (mpegplay, movie): 1 always,
 *     0 never, -1 (default) as the script's click flag says.
 *   gamespeed  Script times run at this percent of real speed (100
 *     default, 10-10000): wait, delay, waittimer and gettimer, effects
 *     (print), spmotion and screenshake's time and end. Text speed,
 *     auto mode's wait and sounds aren't changed.
 *   sysevol  The volume (0-100) of wave's channel, where the menu sounds
 *     play: wave, waveloop, selectvoice's and menuselectvoice's sounds,
 *     btnse's. -1 (default): the sound effects' volume (sevol).
 *   effectoff  1: every print effect (and bg/ld/cl's) is drawn at once, as
 *     with effect 1; 0 (default): as the script asks.
 *   textshadow  0: the message text is drawn without a shadow, whatever
 *     setwindow says; 1 (default): with setwindow's.
 *   An unknown key is an error. "consset "gamespeed",200" plays script
 *   times twice as fast.
 * btnse "file"
 *   A sound played on wave's channel when a btnwait, btnwait2 or
 *   textbtnwait ends on a button (a number of 1 or more; not a click
 *   beside the buttons, a right click or a key), the menus' "confirm"
 *   sound. "" (default): none. */
bool ONScripter::setSetting( const char *key, int v )
{
    int mute = !strcmp( key, "mutebgm" ) ? SOUND_MUTE_BGM : !strcmp( key, "mutese" ) ? SOUND_MUTE_SE :
               !strcmp( key, "mutevoice" ) ? SOUND_MUTE_VOICE : 0;
    if (mute){
        sound_mute = v ? (sound_mute | mute) : (sound_mute & ~mute);
        applyMusicVolume();
    }
    else if (!strcmp( key, "clickvoicestop" )) click_voice_stop = v != 0;
    else if (!strcmp( key, "autovoicewait" )) auto_voice_wait = v != 0;
    else if (!strcmp( key, "selectkeep" )) select_keep_auto = select_keep_skip = v != 0;
    else if (!strcmp( key, "selectkeepauto" )) select_keep_auto = v != 0;
    else if (!strcmp( key, "selectkeepskip" )) select_keep_skip = v != 0;
    else if (!strcmp( key, "clickautostop" )) click_auto_stop = v != 0;
    else if (!strcmp( key, "clickskipstop" )) click_skip_stop = v != 0;
    else if (!strcmp( key, "moviestop" )) movie_stop = v < 0 ? -1 : v > 0 ? 1 : 0;
    else if (!strcmp( key, "gamespeed" )) game_speed = v < 10 ? 10 : v > 10000 ? 10000 : v;
    else if (!strcmp( key, "sysevol" )){
        syse_volume = v < 0 ? -1 : v > 100 ? 100 : v;
        if (wave_sample[MIX_WAVE_CHANNEL])
            Mix_Volume( MIX_WAVE_CHANNEL, (syse_volume >= 0 ? syse_volume : se_volume) * MIX_MAX_VOLUME / 100 );
    }
    else if (!strcmp( key, "effectoff" )) effect_off = v != 0;
    else if (!strcmp( key, "textshadow" )){
        if ((v != 0) != text_shadow){
            text_shadow = v != 0;
            dirty_rect.fill( screen_width, screen_height );
        }
    }
    else return false;
    return true;
}

bool ONScripter::getSetting( const char *key, int &v )
{
    if (!strcmp( key, "mutebgm" )) v = (sound_mute & SOUND_MUTE_BGM) != 0;
    else if (!strcmp( key, "mutese" )) v = (sound_mute & SOUND_MUTE_SE) != 0;
    else if (!strcmp( key, "mutevoice" )) v = (sound_mute & SOUND_MUTE_VOICE) != 0;
    else if (!strcmp( key, "clickvoicestop" )) v = click_voice_stop;
    else if (!strcmp( key, "autovoicewait" )) v = auto_voice_wait;
    else if (!strcmp( key, "selectkeep" )) v = select_keep_auto && select_keep_skip;
    else if (!strcmp( key, "selectkeepauto" )) v = select_keep_auto;
    else if (!strcmp( key, "selectkeepskip" )) v = select_keep_skip;
    else if (!strcmp( key, "clickautostop" )) v = click_auto_stop;
    else if (!strcmp( key, "clickskipstop" )) v = click_skip_stop;
    else if (!strcmp( key, "moviestop" )) v = movie_stop;
    else if (!strcmp( key, "gamespeed" )) v = game_speed;
    else if (!strcmp( key, "sysevol" )) v = syse_volume;
    else if (!strcmp( key, "effectoff" )) v = effect_off;
    else if (!strcmp( key, "textshadow" )) v = text_shadow;
    else return false;
    return true;
}

int ONScripter::conssetCommand()
{
    std::string key = script_h.readStr();
    int v = script_h.readInt();
    if (!setSetting( key.c_str(), v ))
        errorAndExit( ("consset: no setting " + key).c_str() );
    return RET_CONTINUE;
}

int ONScripter::consgetCommand()
{
    script_h.readInt();
    script_h.pushVariable();
    std::string key = script_h.readStr();
    int v = 0;
    if (!getSetting( key.c_str(), v ))
        errorAndExit( ("consget: no setting " + key).c_str() );
    script_h.setInt( &script_h.pushed_variable, v );
    return RET_CONTINUE;
}

/* strspoutline "#RRGGBB"
 *   strsp and vstrsp sprites made after it with their shadow on draw it
 *   as a 1 px outline of this colour (the text log's light rim around
 *   dark letters), kept with the sprite in saves. "" (default): a shadow,
 *   as before. */
int ONScripter::strspoutlineCommand()
{
    const char *buf = script_h.readStr();
    strsp_outline = -1;
    if (buf[0] == '#'){
        uchar3 c;
        readColor( &c, buf );
        strsp_outline = (c[0] << 16) | (c[1] << 8) | c[2];
    }
    return RET_CONTINUE;
}

int ONScripter::btnseCommand()
{
    const char *name = script_h.readStr();
    setStr( &btn_se_file, name[0] ? name : NULL );
    return RET_CONTINUE;
}

int ONScripter::speedTime( int ms )
{
    if (game_speed == 100 || ms <= 0) return ms;
    int t = (int)((long long)ms * 100 / game_speed);
    return t > 0 ? t : 1; // a time never becomes 0 (an effect divides by its duration)
}

bool ONScripter::autoWaitsForVoice()
{
    return auto_voice_wait && !(sound_mute & SOUND_MUTE_VOICE);
}

// After a text wait: a click stops the voice (clickvoicestop), auto mode
// reading on stops it when it doesn't wait for it (autovoicewait 0).
void ONScripter::voiceAfterWait()
{
    if (!wave_sample[0] || skip_mode & SKIP_NORMAL || ctrl_pressed_status) return;
    if (automode_flag ? auto_voice_wait : !click_voice_stop) return;
    cutChannel( 0 );
    Mix_FreeChunk( wave_sample[0] );
    wave_sample[0] = NULL;
}

int ONScripter::autooffCommand()
{
    automode_flag = false;
    webStateHook();
    return RET_CONTINUE;
}
int ONScripter::windowtransCommand()
{
    int n = script_h.readInt();
    window_trans = n < 0 ? 0 : n > 255 ? 255 : n;
    dirty_rect.add( textWindowRect() );
    return RET_CONTINUE;
}

/* windowfade alpha,ms[,from[,delay]]
 *   Draws the text window (picture or colour) and its text at alpha/256
 *   (0-256), reached from `from` (default: where it is now), held for
 *   `delay` ms, over ms while the script goes on. Faded out to 0, the
 *   window is hidden as textoff, back at full for the next show. Not saved
 *   (a load and reset bring it back to full). Fade out over 500 ms:
 *       windowfade 0,500 */
int ONScripter::windowfadeCommand()
{
    int to = script_h.readInt();
    int ms = script_h.readInt();
    int from = window_fade, delay = 0;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA) from = script_h.readInt();
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA) delay = script_h.readInt();
    window_fade_to = to < 0 ? 0 : to > 256 ? 256 : to;
    window_fade = window_fade_from = from < 0 ? 0 : from > 256 ? 256 : from;
    window_fade_ms = ms > 0 ? ms : 0;
    window_fade_delay = delay > 0 ? delay : 0;
    window_fade_start = SDL_GetTicks();
    window_fade_on = true;
    window_fade_redraw = true; // by the next frame or print, so a texton next shows it at `from`
    dirty_rect.add( textWindowRect() );
    proceedWindowFade( window_fade_start, false ); // ms 0: at once
    return RET_CONTINUE;
}

void ONScripter::stopWindowFade()
{
    window_fade_on = false;
    window_fade = 256;
}

int ONScripter::nextWindowFadeTime()
{
    return window_fade_on ? SDL_GetTicks() + 16 : 0;
}

void ONScripter::proceedWindowFade( int current_time, bool draw )
{
    if (!window_fade_on) return;
    int t = current_time - (int)window_fade_start - window_fade_delay;
    int v = t >= window_fade_ms ? window_fade_to : t <= 0 ? window_fade_from :
        window_fade_from + (window_fade_to - window_fade_from) * t / window_fade_ms;
    if (t >= window_fade_ms){
        window_fade_on = false;
        if (v == 0){
            v = 256;
            if (display_mode & DISPLAY_MODE_TEXT){ // as textoff, without the window effect
                if (windowchip_sprite_no >= 0)
                    sprite_info[windowchip_sprite_no].visible = false;
                display_mode = DISPLAY_MODE_NORMAL;
                text_on_flag = false;
                window_fade = 0; // drawn below
#if defined(WEB)
                EM_ASM({ if (self.consOverlay && !Module.consNoTextOverlay) self.consOverlay('hide'); });
#endif
                webStateHook(); // consWindow now, not at the end of the wait
            }
        }
    }
    if (v == window_fade && !(draw && window_fade_redraw)) return;
    window_fade = v;
    if (draw) window_fade_redraw = false;
    // the window, its windowchip and the click cursor
    DirtyRect d;
    d.setDimension( screen_width, screen_height );
    d.add( textWindowRect() );
    if (windowchip_sprite_no >= 0) d.add( drawnRect( sprite_info[windowchip_sprite_no] ) );
    for (int i=0 ; i<2 ; i++){
        SDL_Rect c = cursor_info[i].pos;
        if (!cursor_info[i].abs_flag){
            c.x += sentence_font.x() * screen_ratio1 / screen_ratio2;
            c.y += sentence_font.y() * screen_ratio1 / screen_ratio2;
        }
        d.add( c );
    }
    SDL_Rect r = d.bounding_box;
    if (draw)
        flushDirect( r, refreshMode() | (draw_cursor_flag ? REFRESH_CURSOR_MODE : 0) );
    else
        dirty_rect.add( r );
}

/* openurl "url"
 *   Opens url in the browser (a new tab in the web build), leaving full
 *   screen first, as a game's link to its web page. Not while the game
 *   plays itself (--auto-play): stderr only. */
int ONScripter::openurlCommand()
{
    const char *url = script_h.readStr();
    if (auto_play_flag){
        utils::printInfo("openurl: %s (not opened in auto-play)\n", url);
        return RET_CONTINUE;
    }
    if (fullscreen_mode) setFullScreen(false);
    if (SDL_OpenURL( url ) != 0)
        utils::printError("openurl: %s: %s\n", url, SDL_GetError());
    return RET_CONTINUE;
}

/* screenpulse slot, time, interval, zoom_x, zoom_y, opacity, colour,
 *             tint_opacity, tint
 *   A pulse over the picture behind the text window: when it first
 *   shows, the picture is kept as it is and tinted (tint 0 none, 1
 *   multiplied by colour, 2 colour laid over it), with colour 0xRRGGBB
 *   at tint_opacity (0-256). Over time ms, that copy is laid over the
 *   live picture scaled by 1 + f * zoom / 1000 (x and y) around the
 *   centre at f * opacity (0-256), f going 0 -> 1 -> 0 as
 *   sin(pi * elapsed / time); every interval ms it takes a step. After
 *   time ms nothing shows until the slot is cleared.
 *   "screenpulse 3,700,10,150,150,150,16744703,256,1" pulses a
 *   reddened copy 15 % larger. */
int ONScripter::screenpulseCommand()
{
    return screenEffectCommand( SCREEN_EFFECT_PULSE, "screenpulse" );
}

/* screenzoom slot, time, interval, zoom, x, y, zoom_time, fade_in, ease, power
 *   A frozen copy of the picture behind the text window (taken when it
 *   first shows) laid over the live picture and zoomed: over zoom_time
 *   ms its scale goes from 1 to zoom/1000 and its centre from the
 *   screen's centre to (x, y), eased by ease (0 straight, 1 in: (t^2)^p,
 *   2 out, 3 in and out; p = power/1000, 1000 when 0); its opacity rises
 *   from 0 to 256 over fade_in ms (0: at once). It then stays as it is
 *   until the slot is cleared (clear it inside a transition to fade it
 *   out). time is the longer of the two; every interval ms it takes a
 *   step. "screenzoom 2,150,16,2000,480,400,150,150,0,0" punches in on
 *   the bottom half, twice as large. */
int ONScripter::screenzoomCommand()
{
    return screenEffectCommand( SCREEN_EFFECT_ZOOM, "screenzoom" );
}

// screenzoom's easing (the MAGES engine's): f(t) for t in 0..1.
static double screenZoomEase( int ease, double power, double t )
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    switch (ease){
    case 1: return pow( t * t, power );
    case 2: return 1 - pow( (1 - t) * (1 - t), power );
    case 3:
        if (t < 0.5) return pow( 4 * t * t, power ) * 0.5;
        return 1 - pow( 4 * (1 - t) * (1 - t), power ) * 0.5;
    default: return t;
    }
}

int ONScripter::screenEffectCommand( int kind, const char *name )
{
    leaveTextDisplayMode();

    int slot = script_h.readInt();
    bool set = (script_h.getEndStatus() & ScriptHandler::END_COMMA) != 0;
    // Read the whole command before checking it: arguments left unread
    // would be shown as text.
    char *map_name = NULL;
    int param[SCREEN_EFFECT_PARAMS] = {0};
    if (set){
        if (kind == SCREEN_EFFECT_RIPPLE) setStr( &map_name, script_h.readStr() );
        // Nine numbers, then optional ones (screenripple's blend and phase, screenshake's end).
        for (int i=0 ; i<SCREEN_EFFECT_PARAMS ; i++){
            if (i >= 9 && !(script_h.getEndStatus() & ScriptHandler::END_COMMA)) break;
            param[i] = script_h.readInt();
        }
    }
    if (slot < 0 || slot > MAX_SCREEN_EFFECT || (slot == 0 && set)){
        utils::printError("%s: slot %d out of range\n", name, slot);
        delete[] map_name;
        return RET_CONTINUE;
    }
    if (!set){
        for (int i=0 ; i<MAX_SCREEN_EFFECT ; i++)
            if ((slot == 0 || i == slot-1) && screen_effect[i].kind == SCREEN_EFFECT_SHAKE) traceShake( i+1, "stopped", screen_effect[i].ran );
        if (slot == 0) clearScreenEffects();
        else clearScreenEffect( screen_effect[slot-1] );
        dirty_rect.fill( screen_width, screen_height );
        return RET_CONTINUE;
    }

    ScreenEffect &e = screen_effect[slot-1];
    if (e.kind == SCREEN_EFFECT_SHAKE) traceShake( slot, "replaced", e.ran );
    clearScreenEffect( e );
    e.map_name = map_name;
    for (int i=0 ; i<SCREEN_EFFECT_PARAMS ; i++) e.param[i] = param[i];
    e.kind = kind;
    if (kind == SCREEN_EFFECT_SHAKE){ // gamespeed: its time and end
        e.param[0] = speedTime( e.param[0] );
        e.param[9] = speedTime( e.param[9] );
    }
    startScreenEffect( e );
    if (e.kind == SCREEN_EFFECT_SHAKE) traceShake( slot, NULL, e.param[9] );
    dirty_rect.fill( screen_width, screen_height );

    return RET_CONTINUE;
}

// Puts an effect in its first state (after its parameters are set).
void ONScripter::startScreenEffect( ScreenEffect &e )
{
    int *p = e.param;
    if (p[1] < 1) p[1] = 1; // interval
    if (e.kind == SCREEN_EFFECT_RIPPLE && e.map == NULL && e.map_name){
        bool has_alpha;
        SDL_Surface *map = loadImage( e.map_name, &has_alpha );
        if (map == NULL){
            utils::printError("screenripple: can't load %s\n", e.map_name);
            e.kind = SCREEN_EFFECT_NONE;
            return;
        }
        e.map_w = map->w;
        e.map_h = map->h;
        e.map = new unsigned char[map->w * map->h];
        SDL_LockSurface( map );
        for (int y=0 ; y<map->h ; y++){
            ONSBuf *row = (ONSBuf*)((unsigned char*)map->pixels + map->pitch * y);
            for (int x=0 ; x<map->w ; x++){
                Uint8 r, g, b;
                SDL_GetRGB( row[x], map->format, &r, &g, &b );
                e.map[y * map->w + x] = r;
            }
        }
        SDL_UnlockSurface( map );
        SDL_FreeSurface( map );
    }
    e.elapsed = 0;
    e.ran = 0;
    e.next_step = SDL_GetTicks() + p[1];
    e.range_x = e.range_y = 0;
    e.phase_x = e.phase_y = 0;
    e.point = 0;
    delete[] e.snap;
    e.snap = NULL; // pulse: taken when next drawn
    e.drawn[0] = INT_MIN; // its first step always draws
    if (e.kind == SCREEN_EFFECT_RIPPLE){
        e.opacity = p[2];
        e.amp = p[4] << 16;
        e.angle = p[6];
        e.phase = (p[10] % 1000 + 1000) % 1000;
    }
}

// One interval of an effect: its sizes move toward their end values and
// its waves on.
void ONScripter::stepScreenEffect( ScreenEffect &e )
{
    int *p = e.param;
    int time = p[0], interval = p[1];
    e.elapsed += interval;
    e.ran += interval;
    if (e.elapsed > time) e.elapsed = time;
    int t = (time > 0) ? (int)((long long)e.elapsed * 256 / time) : 256;

    if (e.kind == SCREEN_EFFECT_SHAKE){
        // param: time, interval, mode, speed x, y, x0, y0, x1, y1, end, curve
        if (p[10] && time > 0) t = curveAt( p[10], t * 4 ) / 4;
        e.range_x = (p[5] << 16) + (int)((((long long)(p[7] - p[5])) << 16) * t >> 8);
        e.range_y = (p[6] << 16) + (int)((((long long)(p[8] - p[6])) << 16) * t >> 8);
        e.phase_x += (unsigned int)(p[3] * interval);
        if (p[2] == 2) e.point++; // to and fro: its sign
        else if (p[2] == 1){
            if (e.phase_x >= 0x10000){
                e.phase_x = 0;
                e.point = (e.point + 1) % 7;
            }
        }
        else e.phase_y += (unsigned int)(p[4] * interval);
    }
    else if (e.kind == SCREEN_EFFECT_RIPPLE){
        // param: time, interval, opacity0, opacity1, amp0, amp1, angle, angle step, phase step
        e.opacity = p[2] + ((p[3] - p[2]) * t >> 8);
        e.amp = (p[4] << 16) + (int)((((long long)(p[5] - p[4])) << 16) * t >> 8);
        e.angle += p[7];
        e.phase = ((e.phase + p[8]) % 1000 + 1000) % 1000;
    }
}

void ONScripter::clearScreenEffect( ScreenEffect &e )
{
    delete[] e.map_name;
    delete[] e.map;
    delete[] e.snap;
    e.snap = NULL;
    e.snap_w = e.snap_h = 0;
    e.kind = SCREEN_EFFECT_NONE;
    e.map_name = NULL;
    e.map = NULL;
    e.map_w = e.map_h = 0;
    for (int i=0 ; i<SCREEN_EFFECT_PARAMS ; i++) e.param[i] = 0;
}

void ONScripter::clearScreenEffects()
{
    for (int i=0 ; i<MAX_SCREEN_EFFECT ; i++)
        clearScreenEffect( screen_effect[i] );
}

bool ONScripter::screenEffectActive()
{
    for (int i=0 ; i<MAX_SCREEN_EFFECT ; i++)
        if (screen_effect[i].kind != SCREEN_EFFECT_NONE) return true;
    return false;
}

// The ticks of the next step of any effect, 0 when none runs.
int ONScripter::nextScreenEffectTime()
{
    int min = 0;
    for (int i=0 ; i<MAX_SCREEN_EFFECT ; i++){
        ScreenEffect &e = screen_effect[i];
        if (e.kind == SCREEN_EFFECT_NONE) continue;
        if (min == 0 || min > e.next_step) min = e.next_step;
    }
    if (min && min - screen_effect_flush_time < FRAME_MS) min = screen_effect_flush_time + FRAME_MS;
    // text waiting to be shown goes up at the next frame, not the next step
    if (min && screen_effect_pending && min > screen_effect_flush_time + FRAME_MS) min = screen_effect_flush_time + FRAME_MS;
    return min;
}

void ONScripter::proceedScreenEffects( int current_time )
{
    // Steps due sooner than a frame after the last redraw wait for the next
    // one; they run then all the same, so only frames in between are dropped.
    if (current_time - screen_effect_flush_time < FRAME_MS) return;
    bool changed = false;
    for (int i=0 ; i<MAX_SCREEN_EFFECT ; i++){
        ScreenEffect &e = screen_effect[i];
        if (e.kind == SCREEN_EFFECT_NONE) continue;
        // After a long stop (a menu, a load) go on from now.
        if (current_time - e.next_step > 1000) e.next_step = current_time;
        bool stepped = false;
        while (e.next_step <= current_time){
            stepScreenEffect( e );
            e.next_step += e.param[1];
            stepped = true;
            // a shake given an end clears its slot when it's over
            if (e.kind == SCREEN_EFFECT_SHAKE && e.param[9] > 0 && e.ran >= e.param[9]){
                traceShake( i+1, "ran out", e.ran );
                clearScreenEffect( e );
                changed = true;
                break;
            }
        }
        // Redraw only when a step changed what it shows (a slow shake keeps
        // its offset for several steps, a finished zoom or pulse holds still).
        if (!stepped || e.kind == SCREEN_EFFECT_NONE) continue;
        int look[4];
        screenEffectLook( e, look );
        if (memcmp( look, e.drawn, sizeof(look) ) != 0){
            memcpy( e.drawn, look, sizeof(look) );
            changed = true;
        }
    }
    if (changed || screen_effect_pending){
        SDL_Rect rect = {0, 0, screen_width, screen_height};
        flushDirect( rect, refreshMode() | (draw_cursor_flag ? REFRESH_CURSOR_MODE : 0) );
        screen_effect_flush_time = current_time;
        screen_effect_pending = false;
    }
}

static const int shake_path[8][2] = { // 16.16, the original's table
    {-65536, 32768}, {65536, 49152}, {0, 32768}, {16384, 0},
    {-32768, -32768}, {16384, -16384}, {0, 0}, {-65536, 32768} };

// Where a shake moves the screen, in screen pixels.
void ONScripter::shakeOffset( ScreenEffect &e, int &dx, int &dy )
{
    long long ux, uy; // where on the wave or path, 16.16
    if (e.param[2] == 1){
        // jumps from point to point, as the original (filmed)
        ux = shake_path[e.point][0];
        uy = shake_path[e.point][1];
    }
    else if (e.param[2] == 2){
        // the full size, one way then the other (Nitroplus's Shake, the exe's
        // shake frame: the sign flips at each step, the first step +)
        ux = uy = (e.point & 1) ? 65536 : -65536;
    }
    else{
        ux = (long long)(sin( M_PI * 2 * ((e.phase_x >> 8) & 0xff) / 256 ) * 65536);
        uy = (long long)(sin( M_PI * 2 * ((e.phase_y >> 8) & 0xff) / 256 ) * 65536);
    }
    dx = (int)((ux * e.range_x) >> 32) * screen_ratio1 / screen_ratio2;
    dy = (int)((uy * e.range_y) >> 32) * screen_ratio1 / screen_ratio2;
}

// What of an effect's state applyScreenEffects draws: equal looks draw the same.
void ONScripter::screenEffectLook( ScreenEffect &e, int look[4] )
{
    look[0] = look[1] = look[2] = look[3] = 0;
    if (e.kind == SCREEN_EFFECT_SHAKE) shakeOffset( e, look[0], look[1] );
    else if (e.kind == SCREEN_EFFECT_RIPPLE){
        look[0] = e.opacity; look[1] = e.amp; look[2] = e.angle; look[3] = e.phase;
    }
    else look[0] = e.elapsed; // zoom, pulse
}

void ONScripter::applyScreenEffects( SDL_Surface *surface, SDL_Rect &clip )
{
    if (surface == NULL || surface->format->BytesPerPixel != 4) return;
    int w = surface->w, h = surface->h, pitch = surface->pitch / 4;
    ONSBuf *copy = NULL;

    for (int i=0 ; i<MAX_SCREEN_EFFECT ; i++){
        ScreenEffect &e = screen_effect[i];
        if (e.kind == SCREEN_EFFECT_NONE) continue;
        SDL_LockSurface( surface );
        ONSBuf *buf = (ONSBuf*)surface->pixels;
        if (copy == NULL) copy = new ONSBuf[w * h];
        for (int y=0 ; y<h ; y++)
            memcpy( copy + y * w, buf + y * pitch, w * sizeof(ONSBuf) );

        if (e.kind == SCREEN_EFFECT_SHAKE){
            int dx, dy;
            shakeOffset( e, dx, dy );
            for (int y=0 ; y<h ; y++){
                int sy = y - dy;
                if (sy < 0 || sy >= h) continue;
                int x0 = std::max(0, dx), x1 = std::min(w, w + dx);
                if (x1 > x0) memcpy( buf + y * pitch + x0, copy + sy * w + x0 - dx, (x1 - x0) * sizeof(ONSBuf) );
            }
        }
        else if (e.kind == SCREEN_EFFECT_ZOOM){
            // param: time, interval, zoom, x, y, zoom time, fade in, ease, power
            int *p = e.param;
            if (e.snap == NULL || e.snap_w != w || e.snap_h != h){
                delete[] e.snap;
                e.snap = new ONSBuf[w * h];
                e.snap_w = w;
                e.snap_h = h;
                memcpy( e.snap, copy, w * h * sizeof(ONSBuf) );
            }
            double power = p[8] > 0 ? p[8] / 1000.0 : 1.0;
            double f = screenZoomEase( p[7], power, p[5] > 0 ? (double)e.elapsed / p[5] : 1.0 );
            int opa = p[6] > 0 ? (int)((long long)e.elapsed * 256 / p[6]) : 256;
            if (opa > 256) opa = 256;
            double s = 1 + (p[2] / 1000.0 - 1) * f;
            if (s < 0.01) s = 0.01;
            double ratio = (double)screen_ratio1 / screen_ratio2;
            double cx = w / 2.0 + (p[3] * ratio - w / 2.0) * f, cy = h / 2.0 + (p[4] * ratio - h / 2.0) * f;
            if (opa > 0){
                for (int y=clip.y ; y<clip.y + clip.h ; y++){
                    int ty = (int)(h / 2.0 + (y + 0.5 - cy) / s);
                    if (ty < 0 || ty >= h) continue;
                    ONSBuf *dst = buf + y * pitch;
                    for (int x=clip.x ; x<clip.x + clip.w ; x++){
                        int tx = (int)(w / 2.0 + (x + 0.5 - cx) / s);
                        if (tx < 0 || tx >= w) continue;
                        ONSBuf src = e.snap[ty * w + tx], org = dst[x];
                        ONSBuf rb = (((org & 0xff00ff) * (256 - opa) + (src & 0xff00ff) * opa) >> 8) & 0xff00ff;
                        ONSBuf g  = (((org & 0x00ff00) * (256 - opa) + (src & 0x00ff00) * opa) >> 8) & 0x00ff00;
                        dst[x] = (org & 0xff000000) | rb | g;
                    }
                }
            }
        }
        else if (e.kind == SCREEN_EFFECT_PULSE){
            // param: time, interval, zoom x, y, opacity, colour, tint opacity, tint
            int *p = e.param;
            if (e.snap == NULL || e.snap_w != w || e.snap_h != h){
                delete[] e.snap;
                e.snap = new ONSBuf[w * h];
                e.snap_w = w;
                e.snap_h = h;
                int cr = (p[5] >> 16) & 0xff, cg = (p[5] >> 8) & 0xff, cb = p[5] & 0xff;
                int to = p[6] < 0 ? 0 : p[6] > 256 ? 256 : p[6];
                for (int k=0 ; k<w * h ; k++){
                    ONSBuf c = copy[k];
                    int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
                    int tr = r, tg = g, tb = b;
                    if (p[7] == 1){ tr = r * cr / 255; tg = g * cg / 255; tb = b * cb / 255; }
                    else if (p[7] == 2){ tr = cr; tg = cg; tb = cb; }
                    r += (tr - r) * to >> 8;
                    g += (tg - g) * to >> 8;
                    b += (tb - b) * to >> 8;
                    e.snap[k] = (c & 0xff000000) | (r << 16) | (g << 8) | b;
                }
            }
            double f = (p[0] > 0) ? sin( M_PI * e.elapsed / p[0] ) : 0;
            int opa = (int)(f * p[4]);
            if (opa > 256) opa = 256;
            if (opa > 0){
                double sx = 1 + f * p[2] / 1000, sy = 1 + f * p[3] / 1000;
                double cx = w / 2.0, cy = h / 2.0;
                for (int y=clip.y ; y<clip.y + clip.h ; y++){
                    int ty = (int)(cy + (y + 0.5 - cy) / sy);
                    if (ty < 0 || ty >= h) continue;
                    ONSBuf *dst = buf + y * pitch;
                    for (int x=clip.x ; x<clip.x + clip.w ; x++){
                        int tx = (int)(cx + (x + 0.5 - cx) / sx);
                        if (tx < 0 || tx >= w) continue;
                        ONSBuf src = e.snap[ty * w + tx], org = dst[x];
                        ONSBuf rb = (((org & 0xff00ff) * (256 - opa) + (src & 0xff00ff) * opa) >> 8) & 0xff00ff;
                        ONSBuf g  = (((org & 0x00ff00) * (256 - opa) + (src & 0x00ff00) * opa) >> 8) & 0x00ff00;
                        dst[x] = (org & 0xff000000) | rb | g;
                    }
                }
            }
        }
        else if (e.kind == SCREEN_EFFECT_RIPPLE){
            double amp = e.amp / 65536.0 * screen_ratio1 / screen_ratio2;
            double th = M_PI * 2 * e.angle / 3600;
            long long ax = (long long)(cos(th) * amp * 65536), ay = (long long)(sin(th) * amp * 65536);
            int phase = (int)(((long long)e.phase << 16) / 1000) >> 6;
            static int sine[1024]; // 16.16
            if (sine[256] == 0)
                for (int k=0 ; k<1024 ; k++) sine[k] = (int)lround(sin( M_PI * 2 * k / 1024 ) * 65536);
            int opa = e.opacity < 0 ? 0 : e.opacity > 256 ? 256 : e.opacity;
            bool multiply = e.param[9] == 1;
            // the map's column for each x, worked out once, not per pixel
            std::vector<int> map_x( e.map ? clip.w : 0 );
            for (int x=0 ; x<(int)map_x.size() ; x++) map_x[x] = (clip.x + x) * e.map_w / w;
            for (int y=clip.y ; y<clip.y + clip.h ; y++){
                ONSBuf *dst = buf + y * pitch;
                const unsigned char *map_row = e.map ? e.map + y * e.map_h / h * e.map_w : NULL;
                for (int x=clip.x ; x<clip.x + clip.w ; x++){
                    int m = map_row ? map_row[map_x[x - clip.x]] : 0;
                    int s = sine[(m * 4 + phase) & 1023];
                    int sx = x + (int)((ax * s) >> 32), sy = y + (int)((ay * s) >> 32);
                    if (sx < 0 || sx >= w) sx = x;
                    if (sy < 0 || sy >= h) sy = y;
                    ONSBuf src = copy[sy * w + sx];
                    if (multiply){
                        ONSBuf o = copy[y * w + x];
                        ONSBuf r = ((((o >> 16) & 0xff) * ((src >> 16) & 0xff)) / 255) << 16;
                        ONSBuf g = ((((o >> 8) & 0xff) * ((src >> 8) & 0xff)) / 255) << 8;
                        ONSBuf b = ((o & 0xff) * (src & 0xff)) / 255;
                        src = (o & 0xff000000) | r | g | b;
                    }
                    if (opa >= 256){
                        dst[x] = src;
                        continue;
                    }
                    ONSBuf org = copy[y * w + x];
                    ONSBuf rb = (((org & 0xff00ff) * (256 - opa) + (src & 0xff00ff) * opa) >> 8) & 0xff00ff;
                    ONSBuf g  = (((org & 0x00ff00) * (256 - opa) + (src & 0x00ff00) * opa) >> 8) & 0x00ff00;
                    dst[x] = (org & 0xff000000) | rb | g;
                }
            }
        }
        SDL_UnlockSurface( surface );
    }
    delete[] copy;
}

static bool sameStr( const char *a, const char *b )
{
    if (a == NULL || b == NULL) return a == b;
    return strcmp(a, b) == 0;
}

static char *copyStr( const char *s )
{
    if (s == NULL) return NULL;
    char *c = new char[strlen(s) + 1];
    strcpy(c, s);
    return c;
}

/* pushscreen first,last
 *   Saves the background, sprites first..last (image, position,
 *   visibility, cell, transparency, filters; a strsp sprite's text, font
 *   and colours), the screen filters and the BGM on a stack.
 *   popscreen puts the newest saved state back and removes it from the
 *   stack. Neither draws anything: show the result with print. Sprites
 *   and the background that didn't change in between aren't reloaded, and
 *   a BGM that is still playing keeps playing. For a scene run as a
 *   subroutine that may change the whole screen:
 *       pushscreen 0,999:gosub *scene:popscreen:print 10
 *   The stack holds MAX_SCREEN_STACK states, is kept in save files and is
 *   emptied by reset. */
int ONScripter::pushscreenCommand()
{
    int first = script_h.readInt();
    int last = script_h.readInt();
    if (first < 0) first = 0;
    if (last >= MAX_SPRITE_NUM) last = MAX_SPRITE_NUM - 1;
    if (screen_stack_num >= MAX_SCREEN_STACK)
        errorAndExit( "pushscreen: too many saved screens (popscreen missing?)" );

    ScreenState &st = screen_stack[screen_stack_num++];
    st.bg_name = copyStr(bg_info.file_name);
    st.bgm_name = copyStr(music_file_name);
    st.bgm_loop = music_play_loop_flag;
    st.first = first;
    st.last = last;
    st.sprite = (last >= first) ? new ScreenSprite[last - first + 1] : NULL;
    for (int i=first ; i<=last ; i++){
        AnimationInfo *ai = &sprite_info[i];
        ScreenSprite &sp = st.sprite[i - first];
        sp.image_name = copyStr(ai->image_name);
        sp.str = NULL;
        sp.colors = NULL;
        sp.cells = 0;
        if (isStrsp( i )){
            sp.str = copyStr(ai->file_name);
            sp.font = strsp_font[i];
            sp.cells = ai->num_of_cells;
            sp.colors = new uchar3[sp.cells];
            for (int c=0 ; c<sp.cells ; c++)
                for (int k=0 ; k<3 ; k++) sp.colors[c][k] = ai->color_list ? ai->color_list[c][k] : 0xff;
        }
        sp.x = ai->orig_pos.x;
        sp.y = ai->orig_pos.y;
        sp.visible = ai->visible && !(ai->motion_keys > 0 && ai->motion_leave); // leaving: as a save
        sp.cell = ai->current_cell;
        sp.trans = ai->trans;
        sp.num_of_filters = ai->num_of_filters;
        memcpy(sp.filter, ai->filter, sizeof(sp.filter));
        sp.zoom_x = ai->zoom_x;
        sp.zoom_y = ai->zoom_y;
        sp.zoom_rot = ai->zoom_rot;
        sp.z_over = ai->z_over;
        memcpy(sp.clip_rect, ai->clip_rect, sizeof(sp.clip_rect));
    }
    memcpy(st.screen_filter, screen_filter, sizeof(st.screen_filter));
    memcpy(st.screen_filter_z, screen_filter_z, sizeof(st.screen_filter_z));

    return RET_CONTINUE;
}

int ONScripter::popscreenCommand()
{
    leaveTextDisplayMode();

    if (screen_stack_num == 0){
        utils::printError("popscreen: no saved screen\n");
        return RET_CONTINUE;
    }
    popScreenState();

    return RET_CONTINUE;
}

void ONScripter::popScreenState()
{
    ScreenState &st = screen_stack[--screen_stack_num];

    if (!sameStr(bg_info.file_name, st.bg_name)){
        bg_info.remove();
        setStr( &bg_info.file_name, st.bg_name ? st.bg_name : "black" );
        createBackground();
        dirty_rect.fill( screen_width, screen_height );
    }

    for (int i=st.first ; i<=st.last ; i++){
        AnimationInfo *ai = &sprite_info[i];
        ScreenSprite &sp = st.sprite[i - st.first];
        if (ai->image_surface && ai->visible) dirty_rect.add( ai->pos );

        bool same = sameStr(ai->image_name, sp.image_name) &&
            ai->num_of_filters == sp.num_of_filters &&
            memcmp(ai->filter, sp.filter, sizeof(AnimationInfo::Filter)*sp.num_of_filters) == 0;
        if (same && (sp.str || isStrsp( i ))) // the same strsp sprite: text, font and colours
            same = sp.str && isStrsp( i ) && sameStr(ai->file_name, sp.str) &&
                memcmp(&strsp_font[i], &sp.font, sizeof(StrspFont)) == 0 &&
                ai->num_of_cells == sp.cells && ai->color_list &&
                memcmp(ai->color_list, sp.colors, sizeof(uchar3)*sp.cells) == 0;
        if (!same){
            if (sp.image_name == NULL && sp.str == NULL && (ai->image_name || isStrsp( i ))) root_button_link.removeSprite(i);
            ai->remove();
            if (sp.image_name || sp.str){
                ai->trans = sp.trans;
                ai->num_of_filters = sp.num_of_filters;
                memcpy(ai->filter, sp.filter, sizeof(sp.filter));
                ai->filter_changed = true;
            }
            if (sp.image_name){
                ai->setImageName(sp.image_name);
                parseTaggedString( ai );
                setupAnimationInfo( ai );
            }
            else if (sp.str){
                setStrsp( i, sp.str, sp.font, sp.cells, sp.colors );
                sp.colors = NULL; // the sprite's now
            }
        }
        ai->orig_pos.x = sp.x;
        ai->orig_pos.y = sp.y;
        ai->scalePosXY( screen_ratio1, screen_ratio2 );
        ai->visible = sp.visible && (sp.image_name || sp.str);
        ai->trans = sp.trans;
        if (ai->num_of_cells > 0) ai->setCell(sp.cell);
        if (ai->image_surface && ai->visible) dirty_rect.add( zoomedRect( *ai ) ); // the old zoom
        ai->zoom_x = sp.zoom_x;
        ai->zoom_y = sp.zoom_y;
        ai->zoom_rot = sp.zoom_rot;
        ai->z_over = sp.z_over;
        memcpy(ai->clip_rect, sp.clip_rect, sizeof(ai->clip_rect));
        if (ai->image_surface && ai->visible) dirty_rect.add( zoomedRect( *ai ) );
        if (ai->image_surface && ai->visible) dirty_rect.add( ai->pos );
        delete[] sp.image_name;
        delete[] sp.str;
        delete[] sp.colors;
    }
    delete[] st.sprite;

    if (memcmp(screen_filter, st.screen_filter, sizeof(screen_filter)) != 0 ||
        memcmp(screen_filter_z, st.screen_filter_z, sizeof(screen_filter_z)) != 0){
        memcpy(screen_filter, st.screen_filter, sizeof(screen_filter));
        memcpy(screen_filter_z, st.screen_filter_z, sizeof(screen_filter_z));
        dirty_rect.fill( screen_width, screen_height );
    }

    if (!sameStr(music_file_name, st.bgm_name)){
        stopBGM( false );
        if (st.bgm_name){
            setStr( &music_file_name, st.bgm_name );
            music_play_loop_flag = st.bgm_loop;
            mp3save_flag = true;
            playSound( music_file_name, SOUND_MUSIC | SOUND_MIDI | SOUND_CHUNK,
                       music_play_loop_flag, MIX_BGM_CHANNEL );
        }
    }

    delete[] st.bg_name;
    delete[] st.bgm_name;
}

void ONScripter::clearScreenStack()
{
    while (screen_stack_num > 0){
        ScreenState &st = screen_stack[--screen_stack_num];
        for (int i=st.first ; i<=st.last ; i++){
            delete[] st.sprite[i - st.first].image_name;
            delete[] st.sprite[i - st.first].str;
            delete[] st.sprite[i - st.first].colors;
        }
        delete[] st.sprite;
        delete[] st.bg_name;
        delete[] st.bgm_name;
    }
}

void ONScripter::saveSaveFileCons( bool output_flag )
{
    writeInt( CONS_SAVE_MAGIC, output_flag );
    writeInt( CONS_SAVE_VERSION, output_flag );

    // Version 26: the named locals and spnew's handles with their pictures
    // (writeNamesSection, ONScripter_names.cpp), first: they set the
    // handles' numbers the later parts are read by.
    writeNamesSection( output_flag );

    // Version 1: sprite filters. Count, then per sprite its number, filter count
    // and each filter's kind, blend, strength and three parameters.
    int num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].num_of_filters > 0) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo *ai = &sprite_info[i];
        if (ai->num_of_filters == 0) continue;
        writeInt( i, output_flag );
        writeInt( ai->num_of_filters, output_flag );
        for (int j=0 ; j<ai->num_of_filters ; j++){
            writeInt( ai->filter[j].kind, output_flag );
            writeInt( ai->filter[j].blend, output_flag );
            writeInt( ai->filter[j].strength, output_flag );
            for (int k=0 ; k<3 ; k++)
                writeInt( ai->filter[j].param[k], output_flag );
        }
    }

    // Version 2: the pushscreen stack, oldest first. Only sprites with an
    // image are written (number, then their fields).
    writeInt( screen_stack_num, output_flag );
    for (int n=0 ; n<screen_stack_num ; n++){
        ScreenState &st = screen_stack[n];
        writeStr( st.bg_name, output_flag );
        writeStr( st.bgm_name, output_flag );
        writeInt( st.bgm_loop ? 1 : 0, output_flag );
        writeInt( st.first, output_flag );
        writeInt( st.last, output_flag );
        int num = 0;
        for (int i=st.first ; i<=st.last ; i++)
            if (st.sprite[i - st.first].image_name) num++;
        writeInt( num, output_flag );
        for (int i=st.first ; i<=st.last ; i++){
            ScreenSprite &sp = st.sprite[i - st.first];
            if (sp.image_name == NULL) continue;
            writeInt( i, output_flag );
            writeStr( sp.image_name, output_flag );
            writeInt( sp.x, output_flag );
            writeInt( sp.y, output_flag );
            writeInt( sp.visible ? 1 : 0, output_flag );
            writeInt( sp.cell, output_flag );
            writeInt( sp.trans, output_flag );
            writeInt( sp.num_of_filters, output_flag );
            for (int j=0 ; j<sp.num_of_filters ; j++){
                writeInt( sp.filter[j].kind, output_flag );
                writeInt( sp.filter[j].blend, output_flag );
                writeInt( sp.filter[j].strength, output_flag );
                for (int k=0 ; k<3 ; k++)
                    writeInt( sp.filter[j].param[k], output_flag );
            }
        }
    }

    // Version 3: the screen filter slots (kind, blend, strength, parameters),
    // the current ones, then those of each pushscreen state.
    writeInt( MAX_SCREEN_FILTER, output_flag );
    for (int n=-1 ; n<screen_stack_num ; n++){
        AnimationInfo::Filter *f = (n < 0) ? screen_filter : screen_stack[n].screen_filter;
        for (int i=0 ; i<MAX_SCREEN_FILTER ; i++){
            writeInt( f[i].kind, output_flag );
            writeInt( f[i].blend, output_flag );
            writeInt( f[i].strength, output_flag );
            for (int k=0 ; k<3 ; k++)
                writeInt( f[i].param[k], output_flag );
        }
    }

    // Version 4: the screen effect slots (kind, map, parameters); their
    // animation starts again when loaded.
    writeInt( MAX_SCREEN_EFFECT, output_flag );
    writeInt( SCREEN_EFFECT_PARAMS, output_flag );
    for (int i=0 ; i<MAX_SCREEN_EFFECT ; i++){
        ScreenEffect &e = screen_effect[i];
        writeInt( e.kind, output_flag );
        writeStr( e.map_name, output_flag );
        for (int k=0 ; k<SCREEN_EFFECT_PARAMS ; k++)
            writeInt( e.param[k], output_flag );
    }

    // Version 5: strsp sprites, which the NScripter data keeps only the
    // place of (number, text, columns and rows, font size, pitch, bold,
    // shadow, then the colour of each cell), and the dwave channels playing
    // a looping sound (channel, file).
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (isStrsp( i )) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        if (!isStrsp( i )) continue;
        AnimationInfo *ai = &sprite_info[i];
        writeInt( i, output_flag );
        writeStrsp( ai->file_name, strsp_font[i], ai->num_of_cells, ai->color_list, output_flag );
    }
    num = 0;
    for (int ch=0 ; ch<ONS_MIX_CHANNELS ; ch++)
        if (isLoopPlaying( ch )) num++;
    writeInt( num, output_flag );
    for (int ch=0 ; ch<ONS_MIX_CHANNELS ; ch++){
        if (!isLoopPlaying( ch )) continue;
        writeInt( ch, output_flag );
        writeStr( trace_se_name[ch], output_flag );
    }

    // Version 6: the strsp sprites of each pushscreen state, oldest first
    // (version 2 left them out): the number of states, then per state the
    // count and per sprite its number, place, visibility, cell,
    // transparency, filters and strsp data as in version 5.
    writeInt( screen_stack_num, output_flag );
    for (int n=0 ; n<screen_stack_num ; n++){
        ScreenState &st = screen_stack[n];
        num = 0;
        for (int i=st.first ; i<=st.last ; i++)
            if (st.sprite[i - st.first].str) num++;
        writeInt( num, output_flag );
        for (int i=st.first ; i<=st.last ; i++){
            ScreenSprite &sp = st.sprite[i - st.first];
            if (sp.str == NULL) continue;
            writeInt( i, output_flag );
            writeInt( sp.x, output_flag );
            writeInt( sp.y, output_flag );
            writeInt( sp.visible ? 1 : 0, output_flag );
            writeInt( sp.cell, output_flag );
            writeInt( sp.trans, output_flag );
            writeInt( sp.num_of_filters, output_flag );
            for (int j=0 ; j<sp.num_of_filters ; j++){
                writeInt( sp.filter[j].kind, output_flag );
                writeInt( sp.filter[j].blend, output_flag );
                writeInt( sp.filter[j].strength, output_flag );
                for (int k=0 ; k<3 ; k++)
                    writeInt( sp.filter[j].param[k], output_flag );
            }
            writeStrsp( sp.str, sp.font, sp.cells, sp.colors, output_flag );
        }
    }

    // Version 7: the sprite zooms (spzoom): the count, then per sprite its
    // number and zoom x and y; then the number of pushscreen states and per
    // state, oldest first, the same for the sprites it saved.
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].zoom_x != 100 || sprite_info[i].zoom_y != 100) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo *ai = &sprite_info[i];
        if (ai->zoom_x == 100 && ai->zoom_y == 100) continue;
        writeInt( i, output_flag );
        writeInt( ai->zoom_x, output_flag );
        writeInt( ai->zoom_y, output_flag );
    }
    writeInt( screen_stack_num, output_flag );
    for (int n=0 ; n<screen_stack_num ; n++){
        ScreenState &st = screen_stack[n];
        num = 0;
        for (int i=st.first ; i<=st.last ; i++)
            if (st.sprite[i - st.first].zoom_x != 100 || st.sprite[i - st.first].zoom_y != 100) num++;
        writeInt( num, output_flag );
        for (int i=st.first ; i<=st.last ; i++){
            ScreenSprite &sp = st.sprite[i - st.first];
            if (sp.zoom_x == 100 && sp.zoom_y == 100) continue;
            writeInt( i, output_flag );
            writeInt( sp.zoom_x, output_flag );
            writeInt( sp.zoom_y, output_flag );
        }
    }

    // Version 8: the script volumes, where their ramps go: the BGM's
    // (chvolfade -1), then the number of dwave channels and each one's
    // (chvolfade; it matters for the looping sounds version 5 restarts).
    writeInt( bgm_script_to, output_flag );
    writeInt( ONS_MIX_CHANNELS, output_flag );
    for (int i=0 ; i<ONS_MIX_CHANNELS ; i++)
        writeInt( track_gain[i].to, output_flag );

    // Version 10: the sprite orders (spz): the count, then per sprite its
    // number and the sprite it is drawn over; then the number of pushscreen
    // states and per state, oldest first, the same for its sprites.
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].z_over >= 0) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        if (sprite_info[i].z_over < 0) continue;
        writeInt( i, output_flag );
        writeInt( sprite_info[i].z_over, output_flag );
    }
    writeInt( screen_stack_num, output_flag );
    for (int n=0 ; n<screen_stack_num ; n++){
        ScreenState &st = screen_stack[n];
        num = 0;
        for (int i=st.first ; i<=st.last ; i++)
            if (st.sprite[i - st.first].z_over >= 0) num++;
        writeInt( num, output_flag );
        for (int i=st.first ; i<=st.last ; i++){
            if (st.sprite[i - st.first].z_over < 0) continue;
            writeInt( i, output_flag );
            writeInt( st.sprite[i - st.first].z_over, output_flag );
        }
    }

    // Version 11: the play time in seconds (getplaytime), last: writeSaveFile
    // puts the time of the save there, at playtime_offset.
    if (output_flag) playtime_offset = file_io_buf_ptr;
    writeInt( playtimeSeconds(), output_flag );

    // Version 14: bgmlater's music ("" for none), the time left and loop.
    writeStr( bgm_later_name, output_flag );
    Uint32 now = SDL_GetTicks();
    writeInt( bgm_later_name && bgm_later_time > now ? bgm_later_time - now : 0, output_flag );
    writeInt( bgm_later_loop ? 1 : 0, output_flag );

    // Version 15: the sprites spblend adds or multiplies: the count, then
    // per sprite its number and blending mode.
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].blending_mode != AnimationInfo::BLEND_NORMAL) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        if (sprite_info[i].blending_mode == AnimationInfo::BLEND_NORMAL) continue;
        writeInt( i, output_flag );
        writeInt( sprite_info[i].blending_mode, output_flag );
    }

    // Version 16: the sprite angles (spzoom's): the count, then per sprite its
    // number and angle; then the number of pushscreen states and per state,
    // oldest first, the same for the sprites it saved.
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].zoom_rot != 0) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        if (sprite_info[i].zoom_rot == 0) continue;
        writeInt( i, output_flag );
        writeInt( sprite_info[i].zoom_rot, output_flag );
    }
    writeInt( screen_stack_num, output_flag );
    for (int n=0 ; n<screen_stack_num ; n++){
        ScreenState &st = screen_stack[n];
        num = 0;
        for (int i=st.first ; i<=st.last ; i++)
            if (st.sprite[i - st.first].zoom_rot != 0) num++;
        writeInt( num, output_flag );
        for (int i=st.first ; i<=st.last ; i++){
            if (st.sprite[i - st.first].zoom_rot == 0) continue;
            writeInt( i, output_flag );
            writeInt( st.sprite[i - st.first].zoom_rot, output_flag );
        }
    }

    // Version 17: the sprite clips (spclip): the count, then per sprite its
    // number and rectangle; then the number of pushscreen states and per
    // state, oldest first, the same for the sprites it saved.
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].clip_rect[2] > 0) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        if (sprite_info[i].clip_rect[2] <= 0) continue;
        writeInt( i, output_flag );
        for (int k=0 ; k<4 ; k++) writeInt( sprite_info[i].clip_rect[k], output_flag );
    }
    writeInt( screen_stack_num, output_flag );
    for (int n=0 ; n<screen_stack_num ; n++){
        ScreenState &st = screen_stack[n];
        num = 0;
        for (int i=st.first ; i<=st.last ; i++)
            if (st.sprite[i - st.first].clip_rect[2] > 0) num++;
        writeInt( num, output_flag );
        for (int i=st.first ; i<=st.last ; i++){
            if (st.sprite[i - st.first].clip_rect[2] <= 0) continue;
            writeInt( i, output_flag );
            for (int k=0 ; k<4 ; k++) writeInt( st.sprite[i - st.first].clip_rect[k], output_flag );
        }
    }

    // Version 18: the sprite motions (spmotion): the count, then per sprite
    // its number, loop (1 + loop 4's key), keep, passes, key count and keys (8 values each,
    // as stored); then the spcurve tables: per curve its value count and
    // values. Motions restart on load.
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].motion_keys > 0 && !sprite_info[i].motion_leave) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo *ai = &sprite_info[i];
        if (ai->motion_keys == 0 || ai->motion_leave) continue; // leaving: left out with its sprite
        writeInt( i, output_flag );
        writeInt( ai->motion_loop ? 1 + ai->motion_loop_key : 0, output_flag ); // loop 4: 1 + its loop key
        writeInt( ai->motion_keep ? 1 : 0, output_flag );
        writeInt( ai->motion_passes, output_flag );
        writeInt( ai->motion_keys, output_flag );
        for (int k=0 ; k<ai->motion_keys ; k++)
            for (int j=0 ; j<8 ; j++) writeInt( ai->motion_key[k][j], output_flag );
    }
    for (int c=0 ; c<MAX_CURVES ; c++){
        writeInt( (int)curve_table[c].size(), output_flag );
        for (size_t j=0 ; j<curve_table[c].size() ; j++) writeInt( curve_table[c][j], output_flag );
    }

    // Version 19: the script's Lua globals and running processes, as a
    // Lua chunk (LUAHandler::saveState; "" without Lua).
#ifdef USE_LUA
    std::string lua = lua_handler.saveState();
    writeStr( (char*)lua.c_str(), output_flag );
#else
    writeStr( NULL, output_flag );
#endif

    // Version 20: the sprite trails (sptrail): the count, then per sprite
    // its number, passes, alpha and lag (ms at game speed).
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].trail_passes > 0) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo &ai = sprite_info[i];
        if (ai.trail_passes == 0) continue;
        writeInt( i, output_flag );
        writeInt( ai.trail_passes, output_flag );
        writeInt( ai.trail_alpha, output_flag );
        writeInt( ai.trail_lag, output_flag );
    }

    // Version 21: the dwave channels' pans (chpan), where their ramps go:
    // the number of channels, then each one's.
    writeInt( ONS_MIX_CHANNELS, output_flag );
    for (int i=0 ; i<ONS_MIX_CHANNELS ; i++)
        writeInt( track_pan[i].to, output_flag );

    // Version 22: the screen filter slots' z (screenfilter), the current
    // ones, then those of each pushscreen state, as in version 3.
    writeInt( MAX_SCREEN_FILTER, output_flag );
    for (int n=-1 ; n<screen_stack_num ; n++){
        int *z = (n < 0) ? screen_filter_z : screen_stack[n].screen_filter_z;
        for (int i=0 ; i<MAX_SCREEN_FILTER ; i++)
            writeInt( z[i], output_flag );
    }

    // Version 25: where the script resumes and where each gosub and for
    // returns, by label, line and the line's hash (ONScripter_savepoint.cpp).
    saveSavePoints( output_flag );

    // Version 27: the sprite waves (spwave): the count, then per sprite its
    // number, ms (at game speed), period, amps, curve and the ms gone.
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].wave_on()) num++;
    writeInt( num, output_flag );
    now = SDL_GetTicks();
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo &ai = sprite_info[i];
        if (!ai.wave_on()) continue;
        writeInt( i, output_flag );
        writeInt( ai.wave_ms, output_flag );
        writeInt( ai.wave_period, output_flag );
        writeInt( ai.wave_amp0, output_flag );
        writeInt( ai.wave_amp1, output_flag );
        writeInt( ai.wave_curve, output_flag );
        writeInt( now - ai.wave_start, output_flag );
    }

    // Version 28: the sprite trails drawn over their sprites (sptrail's
    // over): the count, then their numbers.
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].trail_passes > 0 && sprite_info[i].trail_over) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].trail_passes > 0 && sprite_info[i].trail_over) writeInt( i, output_flag );

    // Version 29: the sprite clips' feathers (spclip's sixth argument): the
    // count, then per sprite its number and feather (version 30: and its
    // colour, 0 for none); then per pushscreen state, oldest first, the
    // same for the sprites it saved.
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].clip_rect[4] > 0) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        if (sprite_info[i].clip_rect[4] <= 0) continue;
        writeInt( i, output_flag );
        writeInt( sprite_info[i].clip_rect[4], output_flag );
        writeInt( sprite_info[i].clip_rect[5], output_flag );
    }
    writeInt( screen_stack_num, output_flag );
    for (int n=0 ; n<screen_stack_num ; n++){
        ScreenState &st = screen_stack[n];
        num = 0;
        for (int i=st.first ; i<=st.last ; i++)
            if (st.sprite[i - st.first].clip_rect[4] > 0) num++;
        writeInt( num, output_flag );
        for (int i=st.first ; i<=st.last ; i++){
            if (st.sprite[i - st.first].clip_rect[4] <= 0) continue;
            writeInt( i, output_flag );
            writeInt( st.sprite[i - st.first].clip_rect[4], output_flag );
            writeInt( st.sprite[i - st.first].clip_rect[5], output_flag );
        }
    }

    // Version 31: the sprites spmask masks: the count, then per sprite its
    // number, the mask's name and its x, y.
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].screen_mask >= 0) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo &ai = sprite_info[i];
        if (ai.screen_mask < 0) continue;
        writeInt( i, output_flag );
        writeStr( (char*)screen_masks[ai.screen_mask].first.c_str(), output_flag );
        writeInt( ai.screen_mask_xy[0], output_flag );
        writeInt( ai.screen_mask_xy[1], output_flag );
    }
}

// strsp's text, font (columns and rows, font size, pitch, bold, shadow)
// and the colour of each cell, in the save.
void ONScripter::writeStrsp( char *text, const StrspFont &sf, int cells, uchar3 *colors, bool output_flag )
{
    writeStr( text, output_flag );
    for (int k=0 ; k<2 ; k++){
        writeInt( sf.num_xy[k], output_flag );
        writeInt( sf.font_size_xy[k], output_flag );
        writeInt( sf.pitch_xy[k], output_flag );
    }
    // vstrsp's columns ride in bold's second bit: the save format stays
    writeInt( (sf.bold ? 1 : 0) | (sf.tate ? 2 : 0), output_flag );
    // strspoutline's colour rides in shadow, above 0xffffff
    writeInt( !sf.shadow ? 0 : sf.outline >= 0 ? 0x1000000 | sf.outline : 1, output_flag );
    writeInt( cells, output_flag );
    for (int c=0 ; c<cells ; c++)
        for (int k=0 ; k<3 ; k++)
            writeInt( colors ? colors[c][k] : 0xff, output_flag );
}

void ONScripter::readStrsp( char **text, StrspFont &sf, int &cells, uchar3 *&colors )
{
    readStr( text );
    for (int k=0 ; k<2 ; k++){
        sf.num_xy[k] = readInt();
        sf.font_size_xy[k] = readInt();
        sf.pitch_xy[k] = readInt();
    }
    int bold = readInt();
    sf.bold = (bold & 1) != 0;
    sf.tate = (bold & 2) != 0;
    int shadow = readInt();
    sf.shadow = shadow == 1 || shadow >= 0x1000000;
    sf.outline = shadow >= 0x1000000 ? shadow & 0xffffff : -1;
    cells = readInt();
    colors = new uchar3[cells > 0 ? cells : 1];
    for (int c=0 ; c<cells ; c++)
        for (int k=0 ; k<3 ; k++) colors[c][k] = readInt();
}

// Makes sprite no a strsp sprite again, as strsp did, keeping its place,
// visibility, cell and opacity; colors becomes the sprite's.
void ONScripter::setStrsp( int no, const char *text, const StrspFont &sf, int cells, uchar3 *colors )
{
    AnimationInfo *ai = &sprite_info[no];
    strsp_font[no] = sf;
    FontInfo fi;
    fi.is_newline_accepted = true;
    for (int k=0 ; k<2 ; k++){
        fi.num_xy[k] = sf.num_xy[k];
        fi.font_size_xy[k] = sf.font_size_xy[k];
        fi.pitch_xy[k] = sf.pitch_xy[k];
    }
    fi.is_bold = sf.bold;
    fi.is_shadow = sf.shadow;
    fi.outline = sf.outline;
    if (sf.tate) fi.setTateyokoMode( FontInfo::TATE_MODE );
    int cell = ai->current_cell;
    ai->font_size_xy[0] = -1;
    ai->removeTag();
    setStr( &ai->file_name, text );
    ai->num_of_cells = cells;
    ai->color_list = colors;
    ai->trans_mode = AnimationInfo::TRANS_STRING;
    ai->is_single_line = false;
    ai->is_tight_region = false;
    ai->is_ruby_drawable = sentence_font.rubyon_flag;
    recordSpriteText( no, &fi );
    if (cell < cells) ai->current_cell = cell;
}

// A sprite strsp made (its text in file_name, no image tag).
bool ONScripter::isStrsp( int no )
{
    AnimationInfo &ai = sprite_info[no];
    return ai.trans_mode == AnimationInfo::TRANS_STRING && !ai.image_name && ai.file_name && ai.num_of_cells > 0;
}

void ONScripter::loadSaveFileCons()
{
    clearScreenStack();
    clearScreenFilters();
    clearScreenEffects();
    resetScriptVolumes(); // older saves have none: 100
    applyMusicVolume();
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        sprite_info[i].z_over = -1;
        memset(sprite_info[i].clip_rect, 0, sizeof(sprite_info[i].clip_rect)); // older saves have none
        sprite_info[i].blending_mode = AnimationInfo::BLEND_NORMAL; // older saves have none
        sprite_info[i].trail_passes = sprite_info[i].trail_keys = 0; // older saves have none
        sprite_info[i].trail_over = false;
        sprite_info[i].trail_rect.w = sprite_info[i].trail_rect.h = 0;
        sprite_info[i].screen_mask = -1; // older saves have none
    }
    dirty_rect.fill( screen_width, screen_height );
    setPlaytime( 0 ); // older saves have none
#ifdef USE_LUA
    lua_handler.loadState( NULL ); // a save without Lua data has no script globals
#endif
    clearHandleSprites(); // a save without the block: no handle pictures, named locals at their defaults
    if (readInt() != CONS_SAVE_MAGIC){
        if (trace_fp) traceSaveLeftOut( 0 );
        return;
    }
    int version = readInt();
    if (version < CONS_SAVE_OLDEST || version > CONS_SAVE_VERSION){
        utils::printError("save: CONscripter data version %d, this build reads %d to %d; left out\n", version, CONS_SAVE_OLDEST, CONS_SAVE_VERSION);
        if (trace_fp) traceSaveLeftOut( version );
        return;
    }
    if (version >= 26) readNamesSection();

    int num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt();
        int count = readInt();
        AnimationInfo dummy;
        AnimationInfo *ai = (no >= 0 && no < MAX_SPRITE_NUM) ? &sprite_info[no] : &dummy;
        ai->clearFilters();
        for (int j=0 ; j<count ; j++){
            AnimationInfo::Filter f;
            f.kind = readInt();
            f.blend = readInt();
            f.strength = readInt();
            for (int k=0 ; k<3 ; k++) f.param[k] = readInt();
            if (j < AnimationInfo::MAX_FILTER_NUM) ai->filter[ai->num_of_filters++] = f;
        }
        ai->filter_changed = true;
        if (ai != &dummy && ai->image_name) setupAnimationInfo( ai );
    }

    int depth = readInt();
    for (int n=0 ; n<depth ; n++){
        ScreenState st;
        st.bg_name = NULL;
        st.bgm_name = NULL;
        for (int i=0 ; i<MAX_SCREEN_FILTER ; i++){ // saves before version 3 have none
            st.screen_filter[i].kind = AnimationInfo::FILTER_NONE;
            st.screen_filter[i].blend = AnimationInfo::FILTER_BLEND_COPY;
            st.screen_filter[i].strength = 256;
            st.screen_filter[i].param[0] = st.screen_filter[i].param[1] = st.screen_filter[i].param[2] = 0;
            st.screen_filter_z[i] = -1;
        }
        readStr( &st.bg_name );
        readStr( &st.bgm_name );
        st.bgm_loop = readInt() == 1;
        st.first = readInt();
        st.last = readInt();
        if (st.first < 0) st.first = 0;
        if (st.last >= MAX_SPRITE_NUM) st.last = MAX_SPRITE_NUM - 1;
        st.sprite = (st.last >= st.first) ? new ScreenSprite[st.last - st.first + 1] : NULL;
        for (int i=st.first ; i<=st.last ; i++){
            ScreenSprite &sp = st.sprite[i - st.first];
            sp.image_name = NULL;
            sp.str = NULL;
            sp.colors = NULL;
            sp.cells = 0;
            sp.x = sp.y = sp.cell = 0;
            sp.trans = -1;
            sp.visible = false;
            sp.num_of_filters = 0;
            sp.zoom_x = sp.zoom_y = 100;
            sp.zoom_rot = 0;
        }
        int num = readInt();
        for (int m=0 ; m<num ; m++){
            int i = readInt();
            ScreenSprite dummy_sp;
            dummy_sp.image_name = NULL;
            ScreenSprite &sp = (i >= st.first && i <= st.last) ? st.sprite[i - st.first] : dummy_sp;
            readStr( &sp.image_name );
            sp.x = readInt();
            sp.y = readInt();
            sp.visible = readInt() == 1;
            sp.cell = readInt();
            sp.trans = readInt();
            int count = readInt();
            sp.num_of_filters = 0;
            for (int j=0 ; j<count ; j++){
                AnimationInfo::Filter f;
                f.kind = readInt();
                f.blend = readInt();
                f.strength = readInt();
                for (int k=0 ; k<3 ; k++) f.param[k] = readInt();
                if (j < AnimationInfo::MAX_FILTER_NUM) sp.filter[sp.num_of_filters++] = f;
            }
            if (&sp == &dummy_sp) delete[] dummy_sp.image_name;
        }
        if (n < MAX_SCREEN_STACK) screen_stack[screen_stack_num++] = st;
        else{
            for (int i=st.first ; i<=st.last ; i++) delete[] st.sprite[i - st.first].image_name;
            delete[] st.sprite;
            delete[] st.bg_name;
            delete[] st.bgm_name;
        }
    }

    int slots = readInt();
    for (int n=-1 ; n<screen_stack_num ; n++){
        AnimationInfo::Filter *dst = (n < 0) ? screen_filter : screen_stack[n].screen_filter;
        for (int i=0 ; i<slots ; i++){
            AnimationInfo::Filter f;
            f.kind = readInt();
            f.blend = readInt();
            f.strength = readInt();
            for (int k=0 ; k<3 ; k++) f.param[k] = readInt();
            if (i < MAX_SCREEN_FILTER) dst[i] = f;
        }
    }

    slots = readInt();
    int params = readInt();
    for (int i=0 ; i<slots ; i++){
        ScreenEffect dummy;
        memset( &dummy, 0, sizeof(dummy) );
        ScreenEffect &e = (i < MAX_SCREEN_EFFECT) ? screen_effect[i] : dummy;
        int kind = readInt();
        readStr( &e.map_name );
        for (int k=0 ; k<SCREEN_EFFECT_PARAMS ; k++) e.param[k] = 0; // a save with fewer
        for (int k=0 ; k<params ; k++){
            int n = readInt();
            if (k < SCREEN_EFFECT_PARAMS) e.param[k] = n;
        }
        e.kind = kind;
        if (kind != SCREEN_EFFECT_NONE) startScreenEffect( e );
        if (&e == &dummy) clearScreenEffect( dummy );
    }

    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt();
        char *text = NULL;
        StrspFont sf;
        int cells;
        uchar3 *colors;
        readStrsp( &text, sf, cells, colors );
        // as strsp makes it; the place, visibility, cell and opacity are
        // already loaded
        if (no >= 0 && no < MAX_SPRITE_NUM && cells > 0 && text)
            setStrsp( no, text, sf, cells, colors );
        else
            delete[] colors;
        delete[] text;
    }
    num = readInt();
    for (int n=0 ; n<num ; n++){
        int ch = readInt();
        char *name = NULL;
        readStr( &name );
        if (ch >= 0 && ch < ONS_MIX_CHANNELS && name)
            playSound( name, SOUND_CHUNK, true, ch );
        delete[] name;
    }

    depth = readInt();
    for (int n=0 ; n<depth ; n++){
        num = readInt();
        for (int m=0 ; m<num ; m++){
            int i = readInt();
            ScreenSprite sp;
            sp.image_name = NULL;
            sp.zoom_x = sp.zoom_y = 100;
            sp.zoom_rot = 0;
            sp.x = readInt();
            sp.y = readInt();
            sp.visible = readInt() == 1;
            sp.cell = readInt();
            sp.trans = readInt();
            int count = readInt();
            sp.num_of_filters = 0;
            for (int j=0 ; j<count ; j++){
                AnimationInfo::Filter f;
                f.kind = readInt();
                f.blend = readInt();
                f.strength = readInt();
                for (int k=0 ; k<3 ; k++) f.param[k] = readInt();
                if (j < AnimationInfo::MAX_FILTER_NUM) sp.filter[sp.num_of_filters++] = f;
            }
            sp.str = NULL;
            readStrsp( &sp.str, sp.font, sp.cells, sp.colors );
            ScreenState *st = (n < screen_stack_num) ? &screen_stack[n] : NULL;
            if (st && i >= st->first && i <= st->last && sp.str && sp.cells > 0){
                ScreenSprite &dst = st->sprite[i - st->first];
                delete[] dst.image_name;
                delete[] dst.str;
                delete[] dst.colors;
                dst = sp;
            }
            else{
                delete[] sp.str;
                delete[] sp.colors;
            }
        }
    }

    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt();
        int zx = readInt(), zy = readInt();
        if (no >= 0 && no < MAX_SPRITE_NUM){
            sprite_info[no].zoom_x = zx;
            sprite_info[no].zoom_y = zy;
        }
    }
    depth = readInt();
    for (int n=0 ; n<depth ; n++){
        num = readInt();
        for (int m=0 ; m<num ; m++){
            int i = readInt();
            int zx = readInt(), zy = readInt();
            ScreenState *st = (n < screen_stack_num) ? &screen_stack[n] : NULL;
            if (st && i >= st->first && i <= st->last){
                st->sprite[i - st->first].zoom_x = zx;
                st->sprite[i - st->first].zoom_y = zy;
            }
        }
    }

    bgm_script_from = bgm_script_to = readInt();
    bgm_script_ms = 0;
    applyMusicVolume();
    num = readInt();
    for (int i=0 ; i<num ; i++){
        int vol = readInt();
        if (i >= ONS_MIX_CHANNELS) continue;
        track_gain[i].ms = 0;
        track_gain[i].from = track_gain[i].to = vol;
    }

    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt(), over = readInt();
        if (no >= 0 && no < MAX_SPRITE_NUM) sprite_info[no].z_over = over;
    }
    depth = readInt();
    for (int n=0 ; n<depth ; n++){
        num = readInt();
        for (int m=0 ; m<num ; m++){
            int i = readInt(), over = readInt();
            ScreenState *st = (n < screen_stack_num) ? &screen_stack[n] : NULL;
            if (st && i >= st->first && i <= st->last) st->sprite[i - st->first].z_over = over;
        }
    }

    setPlaytime( readInt() );

    readStr( &bgm_later_name );
    bgm_later_time = SDL_GetTicks() + readInt();
    bgm_later_loop = readInt() != 0;

    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt(), mode = readInt();
        if (no >= 0 && no < MAX_SPRITE_NUM)
            sprite_info[no].blending_mode = (mode >= AnimationInfo::BLEND_NORMAL && mode <= AnimationInfo::BLEND_SCREEN) ? mode : AnimationInfo::BLEND_NORMAL;
    }

    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt(), rot = readInt();
        if (no >= 0 && no < MAX_SPRITE_NUM) sprite_info[no].zoom_rot = rot;
    }
    depth = readInt();
    for (int n=0 ; n<depth ; n++){
        num = readInt();
        for (int m=0 ; m<num ; m++){
            int i = readInt(), rot = readInt();
            ScreenState *st = (n < screen_stack_num) ? &screen_stack[n] : NULL;
            if (st && i >= st->first && i <= st->last) st->sprite[i - st->first].zoom_rot = rot;
        }
    }

    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt(), r[4];
        for (int k=0 ; k<4 ; k++) r[k] = readInt();
        if (no >= 0 && no < MAX_SPRITE_NUM) memcpy(sprite_info[no].clip_rect, r, sizeof(r));
    }
    depth = readInt();
    for (int n=0 ; n<depth ; n++){
        num = readInt();
        for (int m=0 ; m<num ; m++){
            int i = readInt(), r[4];
            for (int k=0 ; k<4 ; k++) r[k] = readInt();
            ScreenState *st = (n < screen_stack_num) ? &screen_stack[n] : NULL;
            if (st && i >= st->first && i <= st->last) memcpy(st->sprite[i - st->first].clip_rect, r, sizeof(r));
        }
    }

    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt();
        AnimationInfo dummy;
        AnimationInfo *ai = (no >= 0 && no < MAX_SPRITE_NUM) ? &sprite_info[no] : &dummy;
        int loop = readInt();
        ai->motion_loop = loop != 0;
        ai->motion_loop_key = loop > 1 ? loop - 1 : 0;
        ai->motion_keep = readInt() != 0;
        ai->motion_leave = false; // loop 3 isn't saved
        ai->motion_passes = readInt();
        int keys = readInt();
        ai->motion_keys = 0;
        for (int k=0 ; k<keys ; k++){
            int v[8];
            for (int j=0 ; j<8 ; j++) v[j] = readInt();
            if (k >= AnimationInfo::MAX_MOTION_KEYS) continue;
            ai->allocMotionKeys();
            for (int j=0 ; j<8 ; j++) ai->motion_key[k][j] = v[j];
            ai->motion_keys++;
        }
        if (ai->motion_loop_key >= ai->motion_keys - 1) ai->motion_loop_key = 0;
        ai->motion_start = ai->motion_next = SDL_GetTicks();
        stepSpriteMotion( *ai, ai->motion_start );
    }
    for (int c=0 ; c<MAX_CURVES ; c++){
        int n = readInt();
        if (n < 0 || n > MAX_CURVE_VALUES) n = 0; // a bad save
        curve_table[c].clear();
        for (int j=0 ; j<n ; j++) curve_table[c].push_back( readInt() );
    }

    if (version < 19) return; // an older save: the later parts keep their defaults
    char *lua = NULL;
    readStr( &lua );
#ifdef USE_LUA
    lua_handler.loadState( lua );
#endif
    delete[] lua;

    if (version < 20) return;
    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt(), passes = readInt(), alpha = readInt(), lag = readInt();
        if (no < 0 || no >= MAX_SPRITE_NUM || passes <= 0) continue;
        AnimationInfo &ai = sprite_info[no];
        ai.trail_passes = passes < SPTRAIL_MAX ? passes : SPTRAIL_MAX;
        ai.trail_alpha = alpha;
        ai.trail_lag = lag;
    }

    if (version < 21) return;
    num = readInt();
    for (int i=0 ; i<num ; i++){
        int pan = readInt();
        if (i >= ONS_MIX_CHANNELS) continue;
        track_pan[i].ms = 0;
        track_pan[i].from = track_pan[i].to = pan;
    }

    if (version < 22) return;
    num = readInt();
    for (int n=-1 ; n<screen_stack_num ; n++){
        int *z = (n < 0) ? screen_filter_z : screen_stack[n].screen_filter_z;
        for (int i=0 ; i<num ; i++){
            int v = readInt();
            if (i < MAX_SCREEN_FILTER) z[i] = v;
        }
    }

    if (version < 25) return; // the place is the NScripter data's line
    loadSavePoints();

    if (version < 27) return;
    num = readInt();
    int now = SDL_GetTicks();
    for (int n=0 ; n<num ; n++){
        int no = readInt(), ms = readInt(), period = readInt(), amp0 = readInt(), amp1 = readInt(), curve = readInt(), gone = readInt();
        if (no < 0 || no >= MAX_SPRITE_NUM || period <= 0) continue;
        AnimationInfo &ai = sprite_info[no];
        ai.wave_ms = ms;
        ai.wave_start = now - gone;
        ai.wave_period = period;
        ai.wave_amp0 = amp0;
        ai.wave_amp1 = amp1;
        ai.wave_curve = curve;
        ai.wave_tick = ai.wave_amp = 0;
        stepSpriteWave( ai, now );
    }

    if (version < 28) return;
    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt();
        if (no >= 0 && no < MAX_SPRITE_NUM && sprite_info[no].trail_passes > 0) sprite_info[no].trail_over = true;
    }

    if (version < 29) return;
    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt(), f = readInt(), colour = version >= 30 ? readInt() : 0;
        if (no >= 0 && no < MAX_SPRITE_NUM && sprite_info[no].clip_rect[2] > 0){
            sprite_info[no].clip_rect[4] = f;
            sprite_info[no].clip_rect[5] = colour;
        }
    }
    depth = readInt();
    for (int n=0 ; n<screen_stack_num ; n++)
        for (int i=screen_stack[n].first ; i<=screen_stack[n].last ; i++) screen_stack[n].sprite[i - screen_stack[n].first].clip_rect[4] = screen_stack[n].sprite[i - screen_stack[n].first].clip_rect[5] = 0;
    for (int n=0 ; n<depth ; n++){
        num = readInt();
        for (int m=0 ; m<num ; m++){
            int i = readInt(), f = readInt(), colour = version >= 30 ? readInt() : 0;
            ScreenState *st = (n < screen_stack_num) ? &screen_stack[n] : NULL;
            if (st && i >= st->first && i <= st->last){
                st->sprite[i - st->first].clip_rect[4] = f;
                st->sprite[i - st->first].clip_rect[5] = colour;
            }
        }
    }

    if (version < 31) return;
    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt();
        char *name = NULL;
        readStr( &name );
        int x = readInt(), y = readInt();
        if (no >= 0 && no < MAX_SPRITE_NUM && name){
            sprite_info[no].screen_mask = screenMaskSlot( name );
            sprite_info[no].screen_mask_xy[0] = x;
            sprite_info[no].screen_mask_xy[1] = y;
        }
        delete[] name;
    }
}

/* --save-dir path: makes the folder, and the folders it is in, if they
 * don't exist yet, so the game's first save doesn't fail. Not in the web
 * build, whose save folder is the page's. */
void ONScripter::makeSaveDir( const char *path )
{
#if !defined(WEB)
    std::string dir( path );
    for (size_t i=1 ; i<=dir.size() ; i++){
        if (i < dir.size() && dir[i] != '/' && dir[i] != '\\') continue;
        std::string part = dir.substr( 0, i );
        struct stat st;
        if (stat( part.c_str(), &st ) == 0) continue;
#if defined(_WIN32)
        _mkdir( part.c_str() );
#else
        mkdir( part.c_str(), 0755 );
#endif
    }
#endif
}

/* Where savescreenshot writes name: in the save directory (without name's
 * folders) when one is set, else in the game folder. The save files are
 * in the same place, so thumbnails sit next to their saves. */
std::string ONScripter::saveDirPath( const char *name )
{
    std::string path(name);
    std::replace(path.begin(), path.end(), '\\', '/');

    if (save_dir){
        size_t idx = path.find_last_of('/');
        if (idx != std::string::npos) path = path.substr(idx + 1);
        return std::string(save_dir) + path;
    }
    if (archive_path) return std::string(archive_path) + path;
    return path;
}

/* delsavegame no[,file]
 *   Deletes save file no and, when given, file (the save's thumbnail, as
 *   named to savescreenshot), so savefileexist reports the slot empty.
 *   A missing file is not an error. In the web build the save directory
 *   is synced afterwards, as after savegame. */
int ONScripter::delsavegameCommand()
{
    int no = script_h.readInt();
    const char *thumb = NULL;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA)
        thumb = script_h.readStr();

    char filename[64];
    sprintf( filename, "save%d.dat", no );
    std::string path = std::string(save_dir ? save_dir : archive_path) + filename;
    if (::remove(path.c_str()) != 0 && errno != ENOENT)
        utils::printError("delsavegame: can't delete %s\n", path.c_str());
#if !defined(WEB)
    // writeSaveFile's second copy
    sprintf( filename, RELATIVEPATH "sav%csave%d.dat", DELIMITER, no );
    path = std::string(save_dir ? save_dir : archive_path) + filename;
    ::remove(path.c_str());
#endif

    if (thumb && thumb[0]){
        path = saveDirPath(thumb);
        if (::remove(path.c_str()) != 0 && errno != ENOENT)
            utils::printError("delsavegame: can't delete %s\n", path.c_str());
    }

#if defined(WEB)
    EM_ASM(
        if(window.flush_save) window.flush_save();
        else FS.syncfs(false, (err)=>{console.log(err);});
    );
#endif

    return RET_CONTINUE;
}

/* copysavegame from,to
 *   Copies save file from to save file to, as it is (its savegame2 text
 *   too); when from doesn't exist, to is left as it is. For a script that
 *   keeps saves of its own beside a slot (the pages of nested scenes).
 *   In the web build the save directory is synced afterwards. */
int ONScripter::copysavegameCommand()
{
    int from = script_h.readInt();
    int to = script_h.readInt();
    std::string dir(save_dir ? save_dir : archive_path);
    std::vector<std::string> names;
    char filename[64];
    sprintf( filename, "save%d.dat", from );
    names.push_back(filename);
    sprintf( filename, "save%d.dat", to );
    names.push_back(filename);
#if !defined(WEB)
    // writeSaveFile's second copy
    sprintf( filename, RELATIVEPATH "sav%csave%d.dat", DELIMITER, from );
    names.push_back(filename);
    sprintf( filename, RELATIVEPATH "sav%csave%d.dat", DELIMITER, to );
    names.push_back(filename);
#endif
    for (size_t i=0 ; i+1<names.size() ; i+=2){
        std::string src = dir + names[i], dst = dir + names[i+1];
        // ::fopen: dir already holds the folder, the member fopen would add archive_path again
        FILE *in = ::fopen(src.c_str(), "rb");
        if (!in) continue; // never delete a player's save because the source is missing
        std::vector<char> data;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) data.insert(data.end(), buf, buf + n);
        fclose(in);
        FILE *out = ScriptHandler::openReplacement(dst);
        if (!out || !ScriptHandler::commitReplacement(out, dst,
                fwrite(data.data(), 1, data.size(), out) == data.size()))
            utils::printError("copysavegame: can't write %s\n", dst.c_str());
    }

#if defined(WEB)
    EM_ASM(
        if(window.flush_save) window.flush_save();
        else FS.syncfs(false, (err)=>{console.log(err);});
    );
#endif

    return RET_CONTINUE;
}

/* "!zNNN" in text draws the following characters at NNN percent of the
 * text window's font size until the next "!z" ("!z100" or "!z" goes back;
 * reset clears it). A character takes its scaled width on the line and
 * sits on the line's baseline; the line pitch stays. The window's column
 * count still counts characters, so a script with larger text has to
 * break its lines itself. Only the text window's own text is scaled, not
 * the backlog. Returns the font to draw info's next character with: info
 * itself, or scaled set up at the tag's size. */
FontInfo *ONScripter::textSizeFont( FontInfo *info, FontInfo &scaled )
{
    if (info != &sentence_font || (text_size_percent == 100 && !text_italic && text_outline < 0)) return info;
    scaled = *info;
    scaled.is_italic = text_italic;
    if (text_outline >= 0){ // "!o": the shadow drawn as an outline of this colour
        scaled.is_shadow = true;
        scaled.outline = text_outline;
    }
    for (int i=0 ; i<2 ; i++)
        scaled.font_size_xy[i] = info->font_size_xy[i] * text_size_percent / 100;
    if (scaled.font_size_xy[0] < 1 || scaled.font_size_xy[1] < 1) return info;
    if (scaled.openFont( font_file, screen_ratio1, screen_ratio2 ) == NULL) return info;
    return &scaled;
}

/* Proportional text. In a proportional font (its 'i' and 'W' differ in
 * width), half-width characters on a horizontal line take their own width
 * plus half the window's character spacing, not half a cell, and are drawn
 * one at a time; lines break at spaces. A monospaced font (IPA Gothic)
 * keeps the grid. */
bool ONScripter::isProportional( FontInfo *info )
{
    if (info->getTateyokoMode() != FontInfo::YOKO_MODE || info->ttf_font[0] == NULL) return false;
    int a, w_i, w_w;
    TTF_GlyphMetrics((TTF_Font*)info->ttf_font[0], 'i', &a, &a, &a, &a, &w_i);
    TTF_GlyphMetrics((TTF_Font*)info->ttf_font[0], 'W', &a, &a, &a, &a, &w_w);
    return w_i != w_w;
}

// The width of the half-width character at c over half a cell, in script pixels.
int ONScripter::hankakuExtra( FontInfo *glyph_info, const char *c )
{
    if (!isProportional(glyph_info)) return 0;
    unsigned short unicode = (unsigned char)*c;
    if (coding2utf16->force_utf8){
        if (unicode >= 0x80) unicode = coding2utf16->convUTF8ToUTF16(&c);
    }
    else if ((*c & 0xe0) == 0xa0 || (*c & 0xe0) == 0xc0) unicode = unicode - 0xa0 + 0xff60; // half-width kana, as drawGlyph
    int a, adv;
    if (TTF_GlyphMetrics((TTF_Font*)FontInfo::glyphFont(glyph_info->ttf_font[0], unicode), unicode, &a, &a, &a, &a, &adv) != 0) return 0;
    return adv * screen_ratio2 / screen_ratio1 - glyph_info->font_size_xy[0] / 2;
}

/* True when the word after the space at buf (up to the next space, line
 * end, full-width character or tag) doesn't fit on info's line after the
 * space: the caller then starts a new line instead of drawing the space. */
bool ONScripter::breakAtSpace( const char *buf, FontInfo *info )
{
    if (buf[0] != ' ' || info->isLineEmpty()) return false;
    FontInfo scaled_font;
    FontInfo *glyph_info = textSizeFont(info, scaled_font);
    if (!isProportional(glyph_info)) return false;
    int w = 0;
    for (const char *p = buf; *p && *p != 0x0a && charCells(p) == 1 && (p == buf || *p != ' '); p += charBytes(p)){
        if (*p == '@' || *p == '\\' || *p == '#' || *p == '(' || (*p == '!' && strchr("swdzic", p[1]) && p[1])) break;
        w += info->pitch_xy[0]/2 + hankakuExtra(glyph_info, p);
    }
    return info->isEndOfLine(0, w - 1);
}

/* "!c" in text: the rest of the text line (up to its end, '@' or '\\') is
 * centred in the room left on the window's line, a horizontal one. Tags
 * take no room; ruby markup is counted as text. */
void ONScripter::centreLine( const char *buf )
{
    FontInfo *f = &sentence_font;
    if (f->getTateyokoMode() != FontInfo::YOKO_MODE) return;
    FontInfo scaled_font;
    FontInfo *glyph_info = textSizeFont(f, scaled_font);
    const char *p = buf;
    while (*p == ' ' && f->isLineEmpty()) p++; // leading spaces aren't drawn
    int w = 0;
    while (*p && *p != 0x0a && *p != '@' && *p != '\\'){
        if (*p == '!' && p[1] && strchr("swdzicv", p[1])){
            for (p += 2; *p >= '0' && *p <= '9'; p++);
            continue;
        }
        if (*p == '#' && strspn( p + 1, "0123456789abcdefABCDEF" ) >= 6){ // a colour
            p += 7;
            continue;
        }
        if (charCells(p) == 2){
            w += f->pitch_xy[0];
            p += charBytes(p);
            continue;
        }
        w += f->pitch_xy[0]/2 + hankakuExtra(glyph_info, p);
        p += charBytes(p);
    }
    int room = f->num_xy[0]*f->pitch_xy[0] - (f->xy[0]*f->pitch_xy[0]/2 + f->hankaku_dx) - w;
    if (room < 2) return;
    f->addLineOffset(room/2);
    f->hankaku_dx += room/2;
}

/* True when the character at text would be drawn past the window's last
 * line, or would start a line past it: the caller waits for a click and
 * goes on on a new page (the English Muramasa's paragraphs outgrow its
 * small window), instead of drawing outside the window. */
bool ONScripter::pastPage( const char *text )
{
    FontInfo *f = &sentence_font;
    if (f->num_xy[0] <= 0 || f->num_xy[1] <= 0) return false;
    bool yoko = f->getTateyokoMode() == FontInfo::YOKO_MODE;
    if (yoko ? f->xy[1]/2 >= f->num_xy[1] : f->xy[0] < 0) return true;
    if (yoko ? f->xy[1]/2 < f->num_xy[1] - 1 : f->xy[0] >= 2) return false;
    // on the last line: drawChar's own line break
    FontInfo scaled_font;
    FontInfo *glyph_info = textSizeFont(f, scaled_font);
    if (charCells(text) == 2) return f->isEndOfLine();
    return f->isEndOfLine() || (!f->isLineEmpty() && isProportional(glyph_info) &&
                                f->isEndOfLine(1, hankakuExtra(glyph_info, text) - 1));
}

// The numbers after the '/' of an effect's dll name ("cons_zoom.dll/0,5100").
static int readDllParams( const char *name, int *param, int max )
{
    const char *p = strchr(name, '/');
    int n = 0;
    while (p && n < max){
        param[n++] = atoi(p + 1);
        p = strchr(p + 1, ',');
    }
    return n;
}

/* effect num,99,duration,"cons_slide.dll/..." (and cons_zoom, cons_flip,
 *   cons_cube, cons_melt, cons_wave)
 *   Retired plugins: their transitions are cons_warp.dll maps now. A
 *   script from an older build still naming one gets a plain cross-fade
 *   over the same time, with one warning per name (and a trace warning).
 *   Returns false for any other name. */
bool ONScripter::effectRetired( const char *params, int duration )
{
    static const char *retired[] = { "cons_slide.dll", "cons_zoom.dll", "cons_flip.dll",
                                     "cons_cube.dll", "cons_melt.dll", "cons_wave.dll" };
    static bool warned[sizeof(retired) / sizeof(retired[0])];
    for (size_t i=0 ; i<sizeof(retired) / sizeof(retired[0]) ; i++){
        if (strncmp(params, retired[i], strlen(retired[i]))) continue;
        if (!warned[i]){
            warned[i] = true;
            utils::printError("%s is retired (a cons_warp.dll map now): a cross-fade instead; rebuild the game to get the transition back\n", retired[i]);
            if (trace_fp) traceWarn("retired transition", retired[i]);
        }
        alphaBlend( NULL, ALPHA_BLEND_CONST, 256 * effect_counter / (duration > 0 ? duration : 1), &dirty_rect.bounding_box );
        return true;
    }
    return false;
}

// src blurred sideways by a box of 2 radius + 1 pixels (edges repeated)
// into the rows of dst.
static void blurRows( SDL_Surface *src, SDL_Surface *dst, int radius )
{
    int w = src->w, h = src->h;
    if (radius <= 0){
        SDL_BlitSurface(src, NULL, dst, NULL);
        return;
    }
    int n = 2 * radius + 1;
    SDL_LockSurface(src);
    SDL_LockSurface(dst);
    for (int y=0 ; y<h ; y++){
        const Uint8 *row = (const Uint8*)src->pixels + y * src->pitch;
        Uint8 *out = (Uint8*)dst->pixels + y * dst->pitch;
        int sum[4] = {0, 0, 0, 0};
        for (int k=-radius ; k<=radius ; k++){
            int sx = k < 0 ? 0 : k >= w ? w - 1 : k;
            for (int c=0 ; c<4 ; c++) sum[c] += row[sx * 4 + c];
        }
        for (int x=0 ; x<w ; x++){
            for (int c=0 ; c<4 ; c++) out[x * 4 + c] = (Uint8)(sum[c] / n);
            int add = x + radius + 1, sub = x - radius;
            add = add >= w ? w - 1 : add;
            sub = sub < 0 ? 0 : sub;
            for (int c=0 ; c<4 ; c++) sum[c] += row[add * 4 + c] - row[sub * 4 + c];
        }
    }
    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

/* effect num,99,duration,"cons_blur.dll/radius,new_only"
 *   A cross-fade with the pictures blurred sideways, by a box of up to
 *   radius pixels either side halfway (none at the ends); new_only 1
 *   blurs only the new picture. For the MAGES engine's InOut_Act 6. */
void ONScripter::effectBlur( const char *params, int duration )
{
#if defined(BPP16)
    alphaBlend( NULL, ALPHA_BLEND_CONST, 256 * effect_counter / duration, &dirty_rect.bounding_box );
#else
    int param[2] = {24, 0};
    readDllParams(params, param, 2);
    float p = duration > 0 ? (float)effect_counter / duration : 1.0f;
    if (p > 1.0f) p = 1.0f;
    int radius = (int)lroundf(param[0] * (1.0f - fabsf(2 * p - 1)));
    int alpha = (int)(256 * p);
    static SDL_Surface *tmp = NULL;
    if (tmp && (tmp->w != accumulation_surface->w || tmp->h != accumulation_surface->h)){
        SDL_FreeSurface(tmp);
        tmp = NULL;
    }
    if (!tmp) tmp = SDL_ConvertSurface(accumulation_surface, accumulation_surface->format, 0);
    // The old picture (blurred unless new_only) into the accumulation,
    // the new one blurred into tmp, then tmp over it at the progress.
    blurRows(effect_src_surface, accumulation_surface, param[1] ? 0 : radius);
    blurRows(effect_dst_surface, tmp, radius);
    int w = accumulation_surface->w, h = accumulation_surface->h;
    SDL_LockSurface(tmp);
    SDL_LockSurface(accumulation_surface);
    for (int y=0 ; y<h ; y++){
        Uint8 *out = (Uint8*)accumulation_surface->pixels + y * accumulation_surface->pitch;
        const Uint8 *b = (const Uint8*)tmp->pixels + y * tmp->pitch;
        for (int x=0 ; x<w * 4 ; x++) out[x] = (Uint8)((out[x] * (256 - alpha) + b[x] * alpha) >> 8);
    }
    SDL_UnlockSurface(accumulation_surface);
    SDL_UnlockSurface(tmp);
#endif
}

/* effect num,99,duration,"cons_rule.dll/file,cells[,curve]"
 *   A transition drawn from data: file holds cells grey masks side by
 *   side (each stretched to the screen), the first at the start, the last
 *   at the end, evenly in time; between two cells their masks mix. Where
 *   the mask is white the new picture shows, black the old one, grey a mix.
 *   For a game's own transition: run its kernel (the exe's function in an
 *   emulator, or a port) on a black old and a white new picture and keep
 *   the frames as cells, so the engine needs no code for it. Covers what
 *   mixes the two pictures in place (wipes, dissolves, blinds, soft
 *   edges). curve (an spmotion curve: 1-3 or an spcurve table) eases the
 *   time between the first and the last cell.
 * effect num,99,duration,"cons_warp.dll/file,cells[,curve]"
 *   The same with warp cells, which also move pixels: each cell holds,
 *   at reduced size, where every pixel of the screen reads the old and
 *   the new picture from, and the mask (format in ONScripter_warp.cpp).
 *   Slides, zooms, flips, melts and waves are such maps.
 *   Fewer than 2 cells or a missing file: a plain cross-fade. */
void ONScripter::effectRule( const char *params, int duration )
{
    static char *rule_name = NULL;
    static SDL_Surface *rule = NULL;
    bool warp = !strncmp(params, "cons_warp.dll", 13);
    // file,cells or file,cells,curve, read from the right: the file name
    // may hold commas, as before the curve was added
    const char *p = strchr(params, '/');
    const char *comma = p ? strrchr(p, ',') : NULL;
    int cells = comma ? atoi(comma + 1) : 0, curve = 0;
    if (comma){
        const char *before = comma - 1;
        while (before > p && isdigit((unsigned char)*before)) before--;
        if (before > p && *before == ',' && before + 1 < comma && comma[1] && strspn(comma + 1, "0123456789") == strlen(comma + 1)){
            curve = cells;
            cells = atoi(before + 1);
            comma = before;
        }
    }
#if defined(BPP16)
    cells = 0; // 16-bit surfaces: a plain cross-fade, the rule never loaded
#endif
    if (cells >= 2){
        std::string name(p + 1, comma - p - 1);
        if (!rule_name || name != rule_name){
            if (rule) SDL_FreeSurface(rule);
            delete[] rule_name;
            rule_name = new char[name.size() + 1];
            strcpy(rule_name, name.c_str());
            rule = loadImage(rule_name);
            if (!rule) utils::printError("%.13s: can't load %s\n", params, rule_name);
        }
    }
    if (cells < 2 || !rule || rule->format->BytesPerPixel != 4 || rule->w < cells * (warp ? 2 : 1) ||
        (warp && (rule->h % 4 || rule->h < 8))){
        alphaBlend( NULL, ALPHA_BLEND_CONST, 256 * effect_counter / (duration > 0 ? duration : 1), &dirty_rect.bounding_box );
        return;
    }
    float pr = duration > 0 ? (float)effect_counter / duration : 1.0f;
    if (pr > 1.0f) pr = 1.0f;
    if (curve) pr = curveAt(curve, (int)(pr * 1024)) / 1024.0f;
    float f = pr * (cells - 1);
    int c = (int)f;
    if (c > cells - 2) c = cells - 2;
    if (warp){
        warpFrame(rule, cells, c, f - c);
        return;
    }
    int frac = (int)((f - c) * 256);
    int w = accumulation_surface->w, h = accumulation_surface->h, cw = rule->w / cells;
    int rshift = rule->format->Rshift;
    SDL_LockSurface(rule);
    SDL_LockSurface(effect_src_surface);
    SDL_LockSurface(effect_dst_surface);
    SDL_LockSurface(accumulation_surface);
    for (int y=0 ; y<h ; y++){
        const Uint32 *m = (const Uint32*)((const Uint8*)rule->pixels + (y * rule->h / h) * rule->pitch);
        const Uint8 *s = (const Uint8*)effect_src_surface->pixels + y * effect_src_surface->pitch;
        const Uint8 *d = (const Uint8*)effect_dst_surface->pixels + y * effect_dst_surface->pitch;
        Uint8 *out = (Uint8*)accumulation_surface->pixels + y * accumulation_surface->pitch;
        for (int x=0 ; x<w ; x++){
            int rx = x * cw / w;
            int m0 = (m[c * cw + rx] >> rshift) & 0xff, m1 = (m[(c + 1) * cw + rx] >> rshift) & 0xff;
            int a = m0 + (m1 - m0) * frac / 256;
            for (int k=0 ; k<4 ; k++, s++, d++, out++) *out = (Uint8)((*s * (255 - a) + *d * a) / 255);
        }
    }
    SDL_UnlockSurface(accumulation_surface);
    SDL_UnlockSurface(effect_dst_surface);
    SDL_UnlockSurface(effect_src_surface);
    SDL_UnlockSurface(rule);
}

/* WebP images, for builds whose SDL_image has no WebP decoder (the
 * emscripten port). Returns NULL for anything that isn't WebP, or when
 * built without USE_WEBP, so the caller falls back to IMG_Load_RW. */
SDL_Surface *ONScripter::loadWebP(const unsigned char *data, size_t length)
{
#if defined(USE_WEBP)
    if (length < 12 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WEBP", 4)) return NULL;
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config) || WebPGetFeatures(data, length, &config.input) != VP8_STATUS_OK) return NULL;
    int w = config.input.width, h = config.input.height;
    if (scale_on_decode){
        // drawn smaller (screen_ratio): libwebp scales while decoding, so the
        // full-size picture is never in memory; setAnimImage then keeps it
        if ((w = w * screen_ratio1 / screen_ratio2) == 0) w = 1;
        if ((h = h * screen_ratio1 / screen_ratio2) == 0) h = 1;
        config.options.use_scaling = 1;
        config.options.scaled_width = w;
        config.options.scaled_height = h;
        decoded_scaled = true;
        decoded_full_w = config.input.width;
        decoded_full_h = config.input.height;
    }
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) return NULL;
    SDL_LockSurface(s);
    config.output.colorspace = MODE_RGBA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = (uint8_t *)s->pixels;
    config.output.u.RGBA.stride = s->pitch;
    config.output.u.RGBA.size = (size_t)s->pitch * h;
    bool ok = WebPDecode(data, length, &config) == VP8_STATUS_OK;
    SDL_UnlockSurface(s);
    if (!ok){
        SDL_FreeSurface(s);
        return NULL;
    }
    return s;
#else
    (void)data;
    (void)length;
    return NULL;
#endif
}

/* Web page controls (chimimouryou): the page calls these on the module
 * (module._consSetAuto(1), ...); each becomes an event the engine
 * handles while it waits, as a key press would be. */
#if defined(WEB)
static void pushWebEvent( int code, int value )
{
    SDL_Event event;
    SDL_zero( event );
    event.type = ONS_WEB_EVENT;
    event.user.code = code;
    event.user.data1 = (void*)(intptr_t)value;
    SDL_PushEvent( &event );
}

extern "C" {
// auto mode on (1) or off (0)
EMSCRIPTEN_KEEPALIVE void consSetAuto( int on ){ pushWebEvent( ONScripter::WEB_SET_AUTO, on ); }
// skip mode on (1) or off (0)
EMSCRIPTEN_KEEPALIVE void consSetSkip( int on ){ pushWebEvent( ONScripter::WEB_SET_SKIP, on ); }
// ruby drawn (1) or not (0)
EMSCRIPTEN_KEEPALIVE void consSetRuby( int on ){ pushWebEvent( ONScripter::WEB_SET_RUBY, on ); }
// ms per character, 0 at once, -1 the game's own speed
EMSCRIPTEN_KEEPALIVE void consSetTextSpeed( int ms ){ pushWebEvent( ONScripter::WEB_SET_TEXT_SPEED, ms ); }
// the text hooks again for what is on the screen (after consNoTextOverlay is cleared)
EMSCRIPTEN_KEEPALIVE void consResendText(){ pushWebEvent( ONScripter::WEB_RESEND_TEXT, 0 ); }
// gloval.sav, kidoku.dat, the logs and envdata written and synced now (the page hidden)
extern ONScripter ons;
EMSCRIPTEN_KEEPALIVE void consPersist(){ ons.persistNow(); }
}
#endif

// A call from the web page. Returns true when it ends the wait, as the
// 'a' and 's' keys do at a click wait.
bool ONScripter::webEvent( int code, int value )
{
    if (code == WEB_SET_AUTO && value && !automode_flag){
        automode_flag = true;
        skip_mode &= ~SKIP_NORMAL;
        webStateHook();
        if (event_mode & WAIT_INPUT_MODE){
            current_button_state.button = 0;
            stopAnimation( clickstr_state );
            return true;
        }
    }
    else if (code == WEB_SET_AUTO && !value && automode_flag){
        automode_flag = false;
        // a click wait timed for auto mode now waits for a click
        if ((event_mode & WAIT_TEXT_MODE) && (event_mode & WAIT_INPUT_MODE)){
            event_mode &= ~WAIT_VOICE_MODE;
            next_time = -1;
            timerEvent( false );
        }
        webStateHook();
    }
    else if (code == WEB_SET_SKIP && value && !(skip_mode & SKIP_NORMAL)){
        automode_flag = false;
        skip_mode |= SKIP_NORMAL;
        webStateHook();
        if (event_mode & WAIT_INPUT_MODE){
            current_button_state.button = 0;
            stopAnimation( clickstr_state );
            return true;
        }
        if (event_mode == WAIT_TIMER_MODE) return true; // a wait ends, as Ctrl ends it
    }
    else if (code == WEB_SET_SKIP && !value && (skip_mode & SKIP_NORMAL)){
        skip_mode &= ~SKIP_NORMAL;
        webStateHook();
    }
    else if (code == WEB_SET_RUBY) ruby_hidden = !value; // from the next text drawn
    else if (code == WEB_SET_TEXT_SPEED) text_speed_override = value >= 0 ? value : -1;
    // shortcut: not in the system menu or backlog (their text comes with the next page)
    else if (code == WEB_RESEND_TEXT && system_menu_mode == SYSTEM_NULL) resendWebText();
    return false;
}

/* language "code" — the language of the text the script shows from now
 * on (an ISO 639-1 code: "ja", "en"), which the web page is told as
 * self.consLanguage(code) at the next wait when it changed. Not saved:
 * a script that switches sets it again after a load.
 *       language "en" */
int ONScripter::languageCommand()
{
    text_language = script_h.readStr();
    return RET_CONTINUE;
}

// Tells the web page when auto or skip mode changed, as
// self.consMode(auto, skip), and when the text window moved, was resized,
// shown or hidden, as self.consWindow(x, y, w, h, shown) in screen
// pixels. Called as the engine waits.
void ONScripter::webStateHook()
{
#if defined(WEB)
    int mode = (automode_flag ? 1 : 0) | ((skip_mode & SKIP_NORMAL) ? 2 : 0);
    if (mode != web_mode_sent){
        web_mode_sent = mode;
        EM_ASM({ if (self.consMode) self.consMode($0, $1); }, mode & 1, mode >> 1);
    }
    SDL_Rect &r = sentence_font_info.pos;
    int window[5] = { r.x, r.y, r.w, r.h, (display_mode & DISPLAY_MODE_TEXT) ? 1 : 0 };
    if (memcmp( window, web_window_sent, sizeof(window) )){
        memcpy( web_window_sent, window, sizeof(window) );
        EM_ASM({ if (self.consWindow) self.consWindow($0, $1, $2, $3, $4); },
               window[0], window[1], window[2], window[3], window[4]);
    }
    if (text_language != web_language_sent){
        web_language_sent = text_language;
        EM_ASM({ if (self.consLanguage) self.consLanguage(UTF8ToString($0)); }, text_language.c_str());
    }
#endif
}

// Closing a tab, Android killing an app in the background, a Kobo whose
// battery runs out asleep and RetroArch closed from the Vita's LiveArea
// don't quit the engine, so what it writes on quit (gloval.sav, kidoku.dat,
// the file and label logs, envdata) would be lost: the settings, the read
// marks, the unlocks. At any wait (auto mode reads on with no click wait),
// at most every 2 seconds, the global variables are written when they
// changed, and the read marks and the logs (when they grew) every 30
// seconds (or with the globals); the web build then syncs the save
// directory to IndexedDB. force (persistNow, a Kobo going to sleep):
// everything at once, envdata too.
void ONScripter::persistHook( bool force )
{
    Uint32 now = SDL_GetTicks();
    force = force || persist_due;
    persist_due = false;
    if (!force && web_persist_time && now - web_persist_time < 2000) return;
    web_persist_time = now;
    bool changed = false;
    if (force){
        saveEnvData();
        changed = true;
    }
    if (globalon_flag){
        file_io_buf_ptr = 0;
        writeVariables( script_h.global_variable_border, script_h.variable_range, false );
        writeGlovalNames( false );
        allocFileIOBuf();
        writeVariables( script_h.global_variable_border, script_h.variable_range, true );
        writeGlovalNames( true );
        if (web_globals_written.size() != file_io_buf_ptr ||
            memcmp( web_globals_written.data(), file_io_buf, file_io_buf_ptr )){
            if (saveFileIOBuf( "gloval.sav" ) == 0){
                web_globals_written.assign( file_io_buf, file_io_buf + file_io_buf_ptr );
                changed = true;
            }
        }
    }
    if (changed || now - web_kidoku_time >= 30000){
        web_kidoku_time = now;
        if (kidokuskip_flag){
            script_h.saveKidokuData();
            changed = true;
        }
        // filelog/labellog (NScrflog.dat, NScrllog.dat): the pictures seen
        // and labels read that `fchk`/`lchk` test, a CG gallery's unlocks
        for (int k=0 ; k<2 ; k++){
            ScriptHandler::LogInfo &info = script_h.log_info[k];
            if (!(k == ScriptHandler::FILE_LOG ? filelog_flag : labellog_flag) || info.num_logs == web_logs_written[k]) continue;
            writeLog( info );
            web_logs_written[k] = info.num_logs;
            changed = true;
        }
    }
#if defined(WEB)
    if (changed) EM_ASM(
        if(window.flush_save) window.flush_save();
        else FS.syncfs(false, (err)=>{console.log(err);});
    );
#endif
}

// persistHook's everything now: the page hidden (_consPersist), the app
// going to the background or ending (persistWatch). At once when the
// engine is in a wait, whose state is whole; else (mid-command, as a
// load fetching its pictures, file_io_buf in use) at its next wait.
// shortcut: on Android persistWatch runs on SDL's Java thread, beside a
// main thread that is in a wait; a lock would close that small gap.
void ONScripter::persistNow()
{
    if (persist_waiting) persistHook( true );
    else persist_due = true;
}

// At each click wait and new page, before consText: where the text is
// in the script, as self.consPosition(offset, total, label, read): the
// byte offset of the text's line in the script, the script's length, the
// label it is in (UTF-8), and 1 if it was read before, 0 if not, -1 when
// the game doesn't keep track (kidokuskip off).
void ONScripter::webPageHook()
{
    webStateHook();
#if defined(WEB)
    char *address = script_h.getCurrent( true );
    int label = script_h.getLabelIndex( address );
    std::string name;
    if (label >= 0) traceString( name, script_h.getLabelName( label ), -1, false );
    EM_ASM({ if (self.consPosition) self.consPosition($0, $1, UTF8ToString($2), $3); },
           script_h.getOffset( address ), script_h.getScriptLength(), name.c_str(), script_h.getKidokuState());
#endif
}

// --trace-long-wait ms: a "long wait" warn when a wait, effect or BGM fade
// asks for more than this, whether or not auto-play shortens it (a
// "print 10,270000" that makes the player wait four minutes).
void ONScripter::consPressCell( bool down )
{
    if ( current_over_button == -1 || !current_button_link ) return;
    ButtonLink *bl = current_button_link;
    if ( bl->button_type != ButtonLink::SPRITE_BUTTON || bl->press_cell < 0 ) return;
    AnimationInfo &si = sprite_info[ bl->sprite_no ];
    if ( bl->press_cell >= si.num_of_cells ) return;
    si.setCell( down ? bl->press_cell : 1 );
    dirty_rect.add( bl->image_rect );
    flush( refreshMode() );
}

// getkey: the next btnwait or textbtnwait also ends on a letter key, with
// -1000 minus the letter's upper-case code (A = -1065, M = -1077), before
// the key's own meaning (skip, auto, cursor keys). Like getfunction, it
// lasts for one wait.
int ONScripter::getkeyCommand()
{
    getkey_flag = true;
    return RET_CONTINUE;
}

// Play time: the seconds the game has run since it started (or was reset,
// or setplaytime set it), kept in saves and put back by loads.
// getplaytime %var / setplaytime seconds.
int ONScripter::playtimeSeconds()
{
    return (int)((playtime_base_ms + (SDL_GetTicks() - playtime_start)) / 1000);
}

void ONScripter::setPlaytime( int seconds )
{
    playtime_base_ms = (long long)(seconds < 0 ? 0 : seconds) * 1000;
    playtime_start = SDL_GetTicks();
}

int ONScripter::getplaytimeCommand()
{
    script_h.readInt();
    script_h.setInt( &script_h.current_variable, playtimeSeconds() );
    return RET_CONTINUE;
}

int ONScripter::setplaytimeCommand()
{
    setPlaytime( script_h.readInt() );
    return RET_CONTINUE;
}

/* Movies on Linux and macOS (mpegplay, movie): no movie decoder is built
 * in there, so the player's ffmpeg decodes the file. One ffmpeg process
 * scales the video to the game's screen (letterboxed) and writes it as
 * frames in the screen's pixel format to its stdout (shown as the game's
 * screen, the text window drawn over them when shown), and the sound in
 * the mixer's format to a second pipe (fd 3). Two threads read both into buffers, so ffmpeg
 * never blocks on one pipe while the other is waited on; the sound plays
 * through Mix_HookMusic at the BGM volume, and its clock times the
 * frames (the wall clock when the movie has no sound). A click, Return,
 * Space or Escape skips it when the script allows; closing the window
 * stops it. Without ffmpeg the movie is skipped with a message. */
#if CONS_FFMPEG_MOVIES
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

namespace {

struct MovieStream {
    // frames: a ring of `slots` buffers of frame_size bytes
    SDL_mutex *lock = nullptr;
    SDL_cond *cond = nullptr;
    std::vector<unsigned char> frames;
    size_t frame_size = 0;
    int slots = 6, first = 0, count = 0;
    bool video_eof = false;
    // sound: a ring of bytes, and how many the mixer has played
    std::vector<unsigned char> sound;
    size_t sound_first = 0, sound_count = 0;
    Uint64 played = 0;
    bool sound_eof = false;
    bool stop = false;
    bool paused = false; // mpeghold, not held: the sound waits
    int volume = 100; // percent
    bool float_samples = false;
    int video_fd = -1, sound_fd = -1;
};

/* mpeghold's sprite, drawn over the movie with its cells going round. */
// mpeghold's sprite: the cell it shows (refreshSurface draws it over the frames)
struct MovieOverlay {
    bool shown = false;
    std::vector<int> durations; // ms each cell shows
    int total = 0;
    void load(AnimationInfo *ai)
    {
        if (!ai->visible || !ai->image_surface) return;
        shown = true;
        if (ai->is_animatable && ai->duration_list)
            for (int i = 0; i < ai->num_of_cells; i++) durations.push_back(std::max(ai->duration_list[i], 1));
        for (int d : durations) total += d;
    }
    int cell(Uint32 now) const
    {
        if (!shown) return -1;
        if (total == 0) return 0;
        int t = now % total, c = 0;
        while (t >= durations[c]) t -= durations[c++];
        return c;
    }
};

/* The ffmpeg to run: CONS_FFMPEG, else ffmpeg from PATH, else Homebrew's
 * (a macOS app started from the Finder has a short PATH). */
const char *ffmpegPath()
{
    const char *env = getenv("CONS_FFMPEG");
    if (env && *env) return access(env, X_OK) == 0 ? env : nullptr;
    const char *path = getenv("PATH");
    std::string dirs = path ? path : "";
    dirs += ":/opt/homebrew/bin:/usr/local/bin:/usr/bin";
    static std::string found;
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos) end = dirs.size();
        std::string candidate = dirs.substr(start, end - start) + "/ffmpeg";
        if (end > start && access(candidate.c_str(), X_OK) == 0) {
            found = candidate;
            return found.c_str();
        }
        start = end + 1;
    }
    return nullptr;
}

/* Starts `argv` with its stdout on *out_fd and, when sound_fd is not
 * null, a second output pipe as its fd 3; stderr is discarded unless
 * err_fd asks for it (then stdout is not kept). Returns the pid, or -1. */
pid_t spawnPipes(char *const argv[], int *out_fd, int *sound_fd, int *err_fd)
{
    int out[2], snd[2] = {-1, -1};
    if (pipe(out) != 0) return -1;
    if (sound_fd && pipe(snd) != 0) { close(out[0]); close(out[1]); return -1; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (err_fd) {
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_adddup2(&fa, out[1], 2);
    } else {
        posix_spawn_file_actions_adddup2(&fa, out[1], 1);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    }
    if (sound_fd) posix_spawn_file_actions_adddup2(&fa, snd[1], 3);
    // The pipes' other ends must not stay open in the child, or the
    // readers never see the end of the data.
    posix_spawn_file_actions_addclose(&fa, out[0]);
    posix_spawn_file_actions_addclose(&fa, out[1]);
    if (sound_fd) {
        posix_spawn_file_actions_addclose(&fa, snd[0]);
        posix_spawn_file_actions_addclose(&fa, snd[1]);
    }
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]);
    if (sound_fd) close(snd[1]);
    if (rc != 0) {
        close(out[0]);
        if (sound_fd) close(snd[0]);
        return -1;
    }
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
    if (err_fd) *err_fd = out[0]; else *out_fd = out[0];
    if (sound_fd) { fcntl(snd[0], F_SETFD, FD_CLOEXEC); *sound_fd = snd[0]; }
    return pid;
}

/* What `ffmpeg -i` says about the file: the frame rate (0 if it has no
 * video) and whether it has sound. */
void probeMovie(const char *ffmpeg, const char *path, double *fps, bool *has_sound, int *w = NULL, int *h = NULL)
{
    *fps = 0;
    *has_sound = false;
    if (w) *w = *h = 0;
    const char *argv[] = {ffmpeg, "-nostdin", "-hide_banner", "-i", path, NULL};
    int fd;
    pid_t pid = spawnPipes((char *const *)argv, NULL, NULL, &fd);
    if (pid < 0) return;
    std::string text;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) text.append(buf, n);
    close(fd);
    waitpid(pid, NULL, 0);
    size_t at = 0;
    while ((at = text.find("Stream #", at)) != std::string::npos) {
        size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (line.find("Audio:") != std::string::npos) *has_sound = true;
        if (line.find("Video:") != std::string::npos && *fps == 0) {
            // "..., 1024x576 [SAR ...", "..., 1024x576, ..."
            for (size_t x = line.find('x'); w && x != std::string::npos; x = line.find('x', x + 1)) {
                size_t s = x;
                while (s > 0 && isdigit((unsigned char)line[s - 1])) s--;
                if (s == x || s == 0 || line[s - 1] != ' ' || !isdigit((unsigned char)line[x + 1])) continue;
                *w = atoi(line.c_str() + s); // (not "0x31637661", a codec tag)
                *h = atoi(line.c_str() + x + 1);
                if (*w > 0 && *h > 0) break;
                *w = *h = 0;
            }
            // "..., 29.97 fps, ..." (or only "30 tbr")
            for (const char *unit : {" fps", " tbr"}) {
                size_t u = line.find(unit);
                if (u == std::string::npos) continue;
                size_t s = line.rfind(' ', u - 1);
                double v = atof(line.c_str() + s + 1);
                if (v > 0) { *fps = v; break; }
            }
            if (*fps == 0) *fps = 30;
        }
        at = end == std::string::npos ? text.size() : end;
    }
}

// ffmpeg's name for the screen's pixel format, byte by byte
const char *moviePixelFormat(Uint32 texture_format)
{
    bool abgr = texture_format == SDL_PIXELFORMAT_ABGR8888;
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
    return abgr ? "rgba" : "bgra";
#else
    return abgr ? "abgr" : "argb";
#endif
}

int readVideo(void *data)
{
    MovieStream *m = (MovieStream *)data;
    std::vector<unsigned char> frame(m->frame_size);
    for (;;) {
        size_t got = 0;
        while (got < m->frame_size) {
            ssize_t n = read(m->video_fd, frame.data() + got, m->frame_size - got);
            if (n <= 0) break;
            got += n;
        }
        SDL_LockMutex(m->lock);
        if (got < m->frame_size) {
            m->video_eof = true;
            SDL_CondBroadcast(m->cond);
            SDL_UnlockMutex(m->lock);
            return 0;
        }
        while (m->count == m->slots && !m->stop) SDL_CondWait(m->cond, m->lock);
        if (m->stop) { SDL_UnlockMutex(m->lock); return 0; }
        int slot = (m->first + m->count) % m->slots;
        memcpy(m->frames.data() + slot * m->frame_size, frame.data(), m->frame_size);
        m->count++;
        SDL_CondBroadcast(m->cond);
        SDL_UnlockMutex(m->lock);
    }
}

int readSound(void *data)
{
    MovieStream *m = (MovieStream *)data;
    unsigned char buf[8192];
    for (;;) {
        ssize_t n = read(m->sound_fd, buf, sizeof(buf));
        SDL_LockMutex(m->lock);
        if (n <= 0) {
            m->sound_eof = true;
            SDL_CondBroadcast(m->cond);
            SDL_UnlockMutex(m->lock);
            return 0;
        }
        for (ssize_t done = 0; done < n;) {
            while (m->sound_count == m->sound.size() && !m->stop) SDL_CondWait(m->cond, m->lock);
            if (m->stop) { SDL_UnlockMutex(m->lock); return 0; }
            size_t tail = (m->sound_first + m->sound_count) % m->sound.size();
            size_t room = std::min(m->sound.size() - m->sound_count, m->sound.size() - tail);
            size_t take = std::min(room, (size_t)(n - done));
            memcpy(m->sound.data() + tail, buf + done, take);
            m->sound_count += take;
            done += take;
        }
        SDL_UnlockMutex(m->lock);
    }
}

void mixMovieSound(void *data, Uint8 *stream, int len)
{
    MovieStream *m = (MovieStream *)data;
    SDL_LockMutex(m->lock);
    if (m->paused) {
        SDL_UnlockMutex(m->lock);
        memset(stream, 0, len);
        return;
    }
    size_t take = std::min((size_t)len, m->sound_count);
    for (size_t done = 0; done < take;) {
        size_t chunk = std::min(take - done, m->sound.size() - m->sound_first);
        memcpy(stream + done, m->sound.data() + m->sound_first, chunk);
        m->sound_first = (m->sound_first + chunk) % m->sound.size();
        m->sound_count -= chunk;
        done += chunk;
    }
    m->played += take;
    int volume = m->volume;
    bool float_samples = m->float_samples;
    SDL_CondBroadcast(m->cond);
    SDL_UnlockMutex(m->lock);
    memset(stream + take, 0, len - take); // the hook writes the whole buffer
    if (volume >= 100) return;
    if (float_samples) {
        float *s = (float *)stream;
        for (size_t i = 0; i < take / 4; i++) s[i] *= volume / 100.0f;
    } else {
        Sint16 *s = (Sint16 *)stream;
        for (size_t i = 0; i < take / 2; i++) s[i] = (Sint16)(s[i] * volume / 100);
    }
}

} // namespace

int ONScripter::playMovieFFmpeg(const char *path, bool click_flag, bool loop_flag)
{
    const char *ffmpeg = ffmpegPath();
    if (!ffmpeg) {
        utils::printError("can't play the movie [%s]: ffmpeg is not installed (or set CONS_FFMPEG to it)\n", path);
        if (trace_fp) traceWarn("no ffmpeg for the movie", path);
        return 0;
    }
    double fps;
    bool has_sound;
    probeMovie(ffmpeg, path, &fps, &has_sound);
    if (fps <= 0) {
        utils::printError("can't play the movie [%s]: ffmpeg finds no video in it\n", path);
        if (trace_fp) traceWarn("broken movie", path);
        return 0;
    }
    int freq = 0, channels = 0;
    Uint16 format = 0;
    if (has_sound && !(audio_open_flag && Mix_QuerySpec(&freq, &format, &channels) &&
                       (format == AUDIO_S16SYS || format == AUDIO_F32SYS)))
        has_sound = false;

    int w = screen_width, h = screen_height;
    char vf[256], rate[16], chans[16];
    snprintf(vf, sizeof(vf), "scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:-1:-1:black,format=%s", w, h, w, h, moviePixelFormat(texture_format));
    snprintf(rate, sizeof(rate), "%d", freq);
    snprintf(chans, sizeof(chans), "%d", channels);
    const char *sample_format = format == AUDIO_F32SYS ? (SDL_BYTEORDER == SDL_LIL_ENDIAN ? "f32le" : "f32be")
                                                       : (SDL_BYTEORDER == SDL_LIL_ENDIAN ? "s16le" : "s16be");
    std::vector<const char *> argv = {ffmpeg, "-nostdin", "-loglevel", "error", "-i", path,
                                      "-map", "0:v:0", "-vf", vf, "-f", "rawvideo", "pipe:1"};
    if (has_sound) {
        for (const char *a : {"-map", "0:a:0", "-ac", (const char *)chans, "-ar", (const char *)rate,
                              "-f", sample_format, "pipe:3"})
            argv.push_back(a);
    }
    argv.push_back(NULL);

    // The frames are the game's screen while it plays: the text window, when
    // shown, is drawn over them (refreshSurface)
    screen_movie = AnimationInfo::allocSurface(w, h, texture_format);
    SDL_Rect whole = {0, 0, w, h};
    MovieOverlay overlay;
    AnimationInfo *hold_sprite = movie_hold && movie_hold->sprite >= 0 && movie_hold->sprite < MAX_SPRITE_NUM
        ? &sprite_info[movie_hold->sprite] : NULL;
    if (hold_sprite) overlay.load(hold_sprite);
    int overlay_cell = -1, hold_cell = hold_sprite ? hold_sprite->current_cell : 0;
    int ret = 0;
    bool done = false;
    while (!done) {
        MovieStream m;
        m.lock = SDL_CreateMutex();
        m.cond = SDL_CreateCond();
        m.frame_size = (size_t)w * h * 4;
        m.frames.resize(m.frame_size * m.slots);
        m.sound.resize(has_sound ? (size_t)freq * channels * (format == AUDIO_F32SYS ? 4 : 2) * 4 : 1); // 4 s
        m.volume = musicVolume();
        m.float_samples = format == AUDIO_F32SYS;
        pid_t pid = spawnPipes((char *const *)argv.data(), &m.video_fd, has_sound ? &m.sound_fd : NULL, NULL);
        if (pid < 0) {
            utils::printError("can't play the movie [%s]: can't start %s\n", path, ffmpeg);
            SDL_DestroyCond(m.cond);
            SDL_DestroyMutex(m.lock);
            break;
        }
        SDL_Thread *video_thread = SDL_CreateThread(readVideo, "movie video", &m);
        SDL_Thread *sound_thread = has_sound ? SDL_CreateThread(readSound, "movie sound", &m) : NULL;
        if (has_sound) Mix_HookMusic(mixMovieSound, &m);
        Uint32 last = SDL_GetTicks(), played_ms = 0; // the time played (not counting mpeghold's pauses)
        int shown = 0;
        bool finished = false;
        while (!finished && !done) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                bool skip = false;
                if (event.type == SDL_QUIT) { ret = 1; done = true; }
                else if (movie_hold) { movieHoldEvent(event); continue; }
                else if (event.type == SDL_MOUSEBUTTONUP || event.type == SDL_FINGERUP) skip = true;
                else if (event.type == SDL_KEYUP) {
                    SDL_Keycode k = event.key.keysym.sym;
                    skip = k == SDLK_RETURN || k == SDLK_SPACE || k == SDLK_ESCAPE || k == SDLK_KP_ENTER;
                    if (k == SDLK_RCTRL) ctrl_pressed_status &= ~0x01;
                    if (k == SDLK_LCTRL) ctrl_pressed_status &= ~0x02;
                }
                if (skip && click_flag) done = movie_skipped = true;
            }
            if (done) break;
            bool running = !movie_hold || movie_hold->held;
            Uint32 now = SDL_GetTicks();
            if (running) played_ms += now - last;
            last = now;
            SDL_LockMutex(m.lock);
            m.paused = !running;
            double clock = has_sound && !m.sound_eof
                ? m.played / (double)(freq * channels * (m.float_samples ? 4 : 2))
                : played_ms / 1000.0;
            if (has_sound && m.sound_eof && m.sound_count == 0 && running) clock = 1e9; // the sound ended: show what is left
            bool due = m.count > 0 && clock >= shown / fps;
            if (due) {
                // Late frames are dropped, but not the last one queued.
                while (m.count > 1 && clock >= (shown + 1) / fps) {
                    m.first = (m.first + 1) % m.slots;
                    m.count--;
                    shown++;
                }
                const unsigned char *f = m.frames.data() + m.first * m.frame_size;
                for (int row = 0; row < h; row++)
                    memcpy((unsigned char *)screen_movie->pixels + row * screen_movie->pitch, f + (size_t)row * w * 4, w * 4);
                m.first = (m.first + 1) % m.slots;
                m.count--;
                shown++;
                SDL_CondBroadcast(m.cond);
            }
            finished = m.video_eof && m.count == 0 && (!has_sound || m.sound_eof);
            SDL_UnlockMutex(m.lock);
            int cell = overlay.cell(now);
            if (cell != overlay_cell) { overlay_cell = cell; due = shown > 0; }
            if (due) {
                if (overlay_cell >= 0) hold_sprite->setCell(overlay_cell);
                flushDirect(whole, refreshMode());
                presentScreen(); // CONscripter: this loop doesn't go back to the event loop
            } else {
                SDL_Delay(2);
            }
        }
        if (has_sound) Mix_HookMusic(NULL, NULL);
        kill(pid, SIGKILL);
        SDL_LockMutex(m.lock);
        m.stop = true;
        SDL_CondBroadcast(m.cond);
        SDL_UnlockMutex(m.lock);
        SDL_WaitThread(video_thread, NULL);
        if (sound_thread) SDL_WaitThread(sound_thread, NULL);
        close(m.video_fd);
        if (has_sound) close(m.sound_fd);
        waitpid(pid, NULL, 0);
        SDL_DestroyCond(m.cond);
        SDL_DestroyMutex(m.lock);
        if (!loop_flag) break;
    }
    SDL_FreeSurface(screen_movie);
    screen_movie = NULL;
    if (hold_sprite) hold_sprite->setCell(hold_cell);
    // The game's screen again.
    dirty_rect.fill(screen_width, screen_height);
    flush(refreshMode());
    return ret;
}
#endif // CONS_FFMPEG_MOVIES

/* mpeghold "file"[,sp[,ch,"sound",fade_ms]]
 *   Plays a movie that goes on only while the left button, a touch,
 *   Enter or Space is held down, and stops on its frame when they are
 *   let go; it shows its first frame until the first press, can't be
 *   skipped, and returns when it ends. The music plays on. Sprite sp
 *   (-1: none), if shown, is drawn over the movie with its cells going
 *   round (a "hold the button" prompt). "sound" plays on dwave channel
 *   ch while the movie does: with fade_ms 0 it is paused and resumed
 *   with the movie and plays on after it ends; otherwise each press
 *   starts it again from its beginning, looped, and it fades out over
 *   fade_ms when let go and when the movie ends.
 *       mpeghold "movie\hold.mp4",6,3,"se\fire.ogg",500
 *   続・殺戮のジャンゴ's 400-vs-3 battle, where the player holds the
 *   trigger down. Linux, macOS (ffmpeg) and the web build; elsewhere
 *   it plays as mpegplay "file",0,1. */
int ONScripter::mpegholdCommand()
{
    std::string movie = script_h.readStr();
    MovieHold hold;
    std::string sound;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA)
        hold.sprite = script_h.readInt();
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA){
        hold.channel = script_h.readInt();
        sound = script_h.readStr();
        hold.fade_ms = script_h.readInt();
    }
    if (hold.channel < 0 || hold.channel >= ONS_MIX_CHANNELS || sound.empty() ||
        playSound( sound.c_str(), SOUND_CHUNK | SOUND_PRELOAD, hold.fade_ms > 0, hold.channel ) != SOUND_CHUNK)
        hold.channel = -1;

    traceMovie( false );
    movie_hold = &hold;
    int quit = playMPEG( movie.c_str(), false );
    movieHoldEnd();
    movie_hold = NULL;
    if (quit){
        trace_end_reason = "window closed";
        endCommand();
    }
    repaintCommand();
    return RET_CONTINUE;
}

// mpeghold's movie held down (or let go): its sound with it.
void ONScripter::movieHoldSet( bool held )
{
    MovieHold *h = movie_hold;
    if (!h || held == h->held) return;
    h->held = held;
    int ch = h->channel;
    if (ch < 0 || !wave_sample[ch]) return;
    if (held){
        if (h->fade_ms > 0){
            Mix_HaltChannel( ch );
            playChannel( ch, wave_sample[ch], -1 );
        }
        else if (!h->started)
            playChannel( ch, wave_sample[ch], 0 );
        else
            Mix_Resume( ch );
        h->started = true;
    }
    else if (h->fade_ms > 0)
        Mix_FadeOutChannel( ch, h->fade_ms );
    else
        Mix_Pause( ch );
}

// An event while mpeghold's movie plays: what holds it down.
void ONScripter::movieHoldEvent( const SDL_Event &event )
{
    MovieHold *h = movie_hold;
    switch (event.type){
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP:
        if (event.button.button == SDL_BUTTON_LEFT) h->mouse = event.type == SDL_MOUSEBUTTONDOWN;
        break;
      case SDL_FINGERDOWN:
        h->fingers++;
        break;
      case SDL_FINGERUP:
        if (h->fingers > 0) h->fingers--;
        break;
      case SDL_KEYDOWN:
      case SDL_KEYUP: {
        SDL_Keycode k = event.key.keysym.sym;
        int bit = k == SDLK_RETURN ? 1 : k == SDLK_KP_ENTER ? 2 : k == SDLK_SPACE ? 4 : 0;
        if (event.type == SDL_KEYDOWN) h->keys |= bit;
        else h->keys &= ~bit;
        if (event.type == SDL_KEYUP && k == SDLK_RCTRL) ctrl_pressed_status &= ~0x01;
        if (event.type == SDL_KEYUP && k == SDLK_LCTRL) ctrl_pressed_status &= ~0x02;
        break;
      }
      case SDL_WINDOWEVENT:
        if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST){
            h->mouse = false;
            h->keys = h->fingers = 0;
        }
        break;
      default:
        break;
    }
    movieHoldSet( h->mouse || h->keys || h->fingers > 0 );
}

// mpeghold's movie ended: a looped sound fades out, a paused one plays
// on; what still holds it, let go later, is no click (a long press would
// open the menu).
void ONScripter::movieHoldEnd()
{
    MovieHold *h = movie_hold;
    hold_release = (h->mouse ? 1 : 0) | h->keys << 1 | (h->fingers > 0 ? 16 : 0);
    if (h->channel < 0 || !h->started) return;
    if (h->fade_ms == 0) Mix_Resume( h->channel );
    else if (h->held) Mix_FadeOutChannel( h->channel, h->fade_ms );
}

// An event after mpeghold's movie ended while held: true for the release
// of what held it, which is dropped.
bool ONScripter::holdRelease( const SDL_Event &event )
{
    int bit = 0;
    bool up = true;
    switch (event.type){
      case SDL_MOUSEBUTTONDOWN:
        up = false; // fall through
      case SDL_MOUSEBUTTONUP:
        if (event.button.button == SDL_BUTTON_LEFT) bit = 1;
        break;
      case SDL_FINGERDOWN:
        up = false; // fall through
      case SDL_FINGERUP:
        bit = 16;
        break;
      case SDL_KEYDOWN:
        up = false; // fall through
      case SDL_KEYUP: {
        SDL_Keycode k = event.key.keysym.sym;
        bit = k == SDLK_RETURN ? 2 : k == SDLK_KP_ENTER ? 4 : k == SDLK_SPACE ? 8 : 0;
        if (!up && event.key.repeat) return bit & hold_release; // still held
        break;
      }
      default:
        return false;
    }
    bool held = bit & hold_release;
    hold_release &= ~bit;
    return held && up;
}

#if defined(WEB)
/* mpeghold in the web build: the page plays the video while it is held
 * (conscripter.html's playVideoHold, which draws the sprite over it) and
 * says so in g_cons_module.video_held. */
void ONScripter::playMovieHoldWeb( const char *path )
{
    AnimationInfo *ai = movie_hold->sprite >= 0 && movie_hold->sprite < MAX_SPRITE_NUM ? &sprite_info[movie_hold->sprite] : NULL;
    SDL_Surface *rgba = ai && ai->visible && ai->image_surface ?
        SDL_ConvertSurfaceFormat( ai->image_surface, SDL_PIXELFORMAT_ABGR8888, 0 ) : NULL; // RGBA in memory
    int ms = rgba && ai->is_animatable && ai->duration_list ? ai->duration_list[0] : 0;
    EM_ASM({
        playVideoHold(g_cons_module.UTF8ToString($0), $1 ? HEAPU8.slice($1 >>> 0, ($1 >>> 0) + $3 * $4) : null, $2, $4, $3 >> 2,
                      $5, $6, $7, $8, $9, $10);
    }, path, rgba ? rgba->pixels : NULL, rgba ? rgba->w : 0, rgba ? rgba->pitch : 0, rgba ? rgba->h : 0,
       rgba ? ai->num_of_cells : 1, ms, rgba ? ai->pos.x : 0, rgba ? ai->pos.y : 0, screen_width, screen_height);
    if (rgba) SDL_FreeSurface( rgba );
    while (EM_ASM_INT( return g_cons_module.wait_video; )){
        movieHoldSet( EM_ASM_INT( return g_cons_module.video_held ? 1 : 0; ) );
        SDL_Delay( 5 );
    }
    // What held it at the end (the page's bits: 1 the pointer, 2 Enter, 8
    // Space); key events meanwhile went to the page.
    int end = EM_ASM_INT( return g_cons_module.video_hold_end|0; );
    movie_hold->mouse = end & 1;
    movie_hold->keys = end >> 1 & 7;
    SDL_FlushEvents( SDL_KEYDOWN, SDL_KEYUP );
}
#endif

/* Movies as pictures: a movie file (.mp4, .webm, .mpg, .mpeg, .ogv, .mov,
 * .mkv, .avi, .wmv) given to lsp, lsp2, bg or ld plays in the picture's
 * place, without its sound, while the game waits (and during an effect): the sprite's order, the
 * tag's transparency (":a" takes the right half as the alpha), spmotion,
 * spzoom and the text window over it work as for a picture.
 *     lsp 10,":c;movie\cut.mp4",0,0         plays once, stays on its last frame
 *     lsp 10,":c/1,0,0;movie\loop.mp4",0,0  loops
 * csp (or another picture) stops it; a save keeps the picture's name, so a
 * load starts it again. Linux and macOS decode it with ffmpeg (as
 * mpegplay), the web build with a hidden <video>; auto-play shows its first
 * frame only. */
bool ONScripter::isMovieFile( const char *name )
{
    const char *ext = name ? strrchr( name, '.' ) : NULL;
    if (!ext) return false;
    for (const char *e : {".mp4", ".m4v", ".webm", ".mpg", ".mpeg", ".ogv", ".mov", ".mkv", ".avi", ".wmv"})
        if (!strcasecmp( ext, e )) return true;
    return false;
}

struct SpriteMovie {
    AnimationInfo *anim = nullptr;
    std::string name;                // the picture it is (anim's surface_name)
    int w = 0, h = 0;
    double fps = 30;
    bool loop = false, ended = false;
    Uint32 start = 0;                // when frame 0 showed
    int shown = 0;                   // frames shown (or skipped)
    std::vector<unsigned char> frame; // the last frame, w*h in the screen's format
#if CONS_FFMPEG_MOVIES
    MovieStream m;
    pid_t pid = -1;
    SDL_Thread *thread = nullptr;
#elif defined(WEB)
    int id = 0;
#endif
};

namespace {

#if CONS_FFMPEG_MOVIES
bool movieStart(SpriteMovie *s, const char *path, Uint32 texture_format)
{
    const char *ffmpeg = ffmpegPath();
    if (!ffmpeg) return false;
    bool has_sound;
    probeMovie(ffmpeg, path, &s->fps, &has_sound, &s->w, &s->h);
    if (s->fps <= 0 || s->w <= 0 || s->h <= 0) return false;
    std::vector<const char *> argv = {ffmpeg, "-nostdin", "-loglevel", "error"};
    if (s->loop) argv.insert(argv.end(), {"-stream_loop", "-1"});
    argv.insert(argv.end(), {"-i", path, "-map", "0:v:0", "-pix_fmt", moviePixelFormat(texture_format),
                             "-f", "rawvideo", "pipe:1", (const char *)NULL});
    s->m.frame_size = (size_t)s->w * s->h * 4;
    s->m.frames.resize(s->m.frame_size * s->m.slots);
    s->pid = spawnPipes((char *const *)argv.data(), &s->m.video_fd, NULL, NULL);
    if (s->pid < 0) return false;
    s->m.lock = SDL_CreateMutex();
    s->m.cond = SDL_CreateCond();
    s->thread = SDL_CreateThread(readVideo, "sprite movie", &s->m);
    return true;
}

// Frame `due` (or the newest before it) into s->frame; false when none is new.
bool movieFrame(SpriteMovie *s, int due)
{
    bool got = false;
    SDL_LockMutex(s->m.lock);
    while (s->m.count > 1 && s->shown < due) { // late: drop
        s->m.first = (s->m.first + 1) % s->m.slots;
        s->m.count--;
        s->shown++;
    }
    if (s->m.count > 0 && s->shown <= due) {
        memcpy(s->frame.data(), s->m.frames.data() + s->m.first * s->m.frame_size, s->m.frame_size);
        s->m.first = (s->m.first + 1) % s->m.slots;
        s->m.count--;
        s->shown++;
        got = true;
        SDL_CondBroadcast(s->m.cond);
    }
    else if (s->m.count == 0 && s->m.video_eof)
        s->ended = true;
    SDL_UnlockMutex(s->m.lock);
    return got;
}

// Waits for the first frame (up to 5 s).
bool movieFirstFrame(SpriteMovie *s)
{
    Uint32 until = SDL_GetTicks() + 5000;
    SDL_LockMutex(s->m.lock);
    while (s->m.count == 0 && !s->m.video_eof && SDL_GetTicks() < until)
        SDL_CondWaitTimeout(s->m.cond, s->m.lock, 50);
    SDL_UnlockMutex(s->m.lock);
    return movieFrame(s, 0);
}

void movieStop(SpriteMovie *s)
{
    if (s->pid < 0) return;
    kill(s->pid, SIGKILL);
    SDL_LockMutex(s->m.lock);
    s->m.stop = true;
    SDL_CondBroadcast(s->m.cond);
    SDL_UnlockMutex(s->m.lock);
    SDL_WaitThread(s->thread, NULL);
    close(s->m.video_fd);
    waitpid(s->pid, NULL, 0);
    SDL_DestroyCond(s->m.cond);
    SDL_DestroyMutex(s->m.lock);
    s->pid = -1;
}
#elif defined(WEB)
bool movieStart(SpriteMovie *s, const char *path, Uint32)
{
    s->id = EM_ASM_INT(return consMovieOpen(UTF8ToString($0), $1, 0, 0, 0);, path, s->loop);
    if (s->id <= 0) return false;
    int state;
    while ((state = EM_ASM_INT(return consMovieState($0);, s->id)) == 0) SDL_Delay(5);
    if (state < 0) { EM_ASM(consMovieClose($0);, s->id); return false; }
    s->w = EM_ASM_INT(return g_cons_movies[$0].video.videoWidth;, s->id);
    s->h = EM_ASM_INT(return g_cons_movies[$0].video.videoHeight;, s->id);
    return s->w > 0 && s->h > 0;
}

bool movieFrame(SpriteMovie *s, int)
{
    bool got = EM_ASM_INT({
        var d = consMovieFrame($0, $2, $3);
        if (!d) return 0;
        HEAPU8.set(d, $1 >>> 0);
        return 1;
    }, s->id, s->frame.data(), s->w, s->h);
    if (!got && EM_ASM_INT(return consMovieEnded($0);, s->id)) s->ended = true;
    return got;
}

bool movieFirstFrame(SpriteMovie *s)
{
    for (int i = 0; i < 1000 && !s->ended; i++) {
        if (movieFrame(s, 0)) return true;
        SDL_Delay(5);
    }
    return false;
}

void movieStop(SpriteMovie *s)
{
    if (s->id > 0) EM_ASM(consMovieClose($0);, s->id);
    s->id = 0;
}
#else
bool movieStart(SpriteMovie *, const char *, Uint32) { return false; }
bool movieFrame(SpriteMovie *, int) { return false; }
bool movieFirstFrame(SpriteMovie *) { return false; }
void movieStop(SpriteMovie *) {}
#endif

// The canvas gives RGBA bytes; the screen may be BGRA.
void movieToScreenFormat(SpriteMovie *s, Uint32 texture_format)
{
#if defined(WEB)
    if (texture_format == SDL_PIXELFORMAT_ABGR8888) return;
    unsigned char *p = s->frame.data();
    for (size_t i = 0; i < s->frame.size(); i += 4) std::swap(p[i], p[i + 2]);
#else
    (void)s; (void)texture_format;
#endif
}

SDL_Surface *movieSurface(SpriteMovie *s, Uint32 texture_format)
{
    SDL_Surface *surface = AnimationInfo::allocSurface(s->w, s->h, texture_format);
    for (int row = 0; row < s->h; row++)
        memcpy((unsigned char *)surface->pixels + row * surface->pitch, s->frame.data() + (size_t)row * s->w * 4, s->w * 4);
    return surface;
}

} // namespace

#if defined(WEB)
/* mpegplay on the web with the text window shown: the movie (with its
 * sound) plays in a hidden <video>, its frames drawn as the game's screen
 * under the window. */
int ONScripter::playMovieLayered( const char *path, bool click_flag, bool loop_flag )
{
    SpriteMovie s;
    s.loop = loop_flag;
    s.id = EM_ASM_INT(return consMovieOpen(UTF8ToString($0), $1, 1, $2, $3);, path, loop_flag, screen_width, screen_height);
    int state = 0;
    while (s.id > 0 && (state = EM_ASM_INT(return consMovieState($0);, s.id)) == 0) SDL_Delay(5);
    if (s.id <= 0 || state < 0){
        movieStop( &s );
        return 0;
    }
    s.w = screen_width;
    s.h = screen_height;
    s.frame.assign( (size_t)s.w * s.h * 4, 0 );
    screen_movie = AnimationInfo::allocSurface( s.w, s.h, texture_format );
    SDL_Rect whole = {0, 0, s.w, s.h};
    int ret = 0;
    bool done = false;
    while (!done && !s.ended){
        SDL_Event event;
        while (SDL_PollEvent( &event )){
            bool skip = false;
            if (event.type == SDL_QUIT){ ret = 1; done = true; }
            else if (event.type == SDL_MOUSEBUTTONUP || event.type == SDL_FINGERUP) skip = true;
            else if (event.type == SDL_KEYUP){
                SDL_Keycode k = event.key.keysym.sym;
                skip = k == SDLK_RETURN || k == SDLK_SPACE || k == SDLK_ESCAPE || k == SDLK_KP_ENTER;
            }
            if (skip && click_flag) done = movie_skipped = true;
        }
        if (!done && movieFrame( &s, 0 )){
            movieToScreenFormat( &s, texture_format );
            for (int row = 0; row < s.h; row++)
                memcpy( (unsigned char *)screen_movie->pixels + row * screen_movie->pitch, s.frame.data() + (size_t)row * s.w * 4, s.w * 4 );
            flushDirect( whole, refreshMode() );
            presentScreen();
        }
        SDL_Delay( 5 );
    }
    movieStop( &s );
    SDL_FreeSurface( screen_movie );
    screen_movie = NULL;
    dirty_rect.fill( screen_width, screen_height );
    flush( refreshMode() );
    return ret;
}
#endif

SpriteMovie *ONScripter::findSpriteMovie( AnimationInfo *anim )
{
    for (SpriteMovie *s : sprite_movies)
        if (s->anim == anim) return s;
    return NULL;
}

void ONScripter::closeSpriteMovie( size_t i )
{
    movieStop( sprite_movies[i] );
    delete sprite_movies[i];
    sprite_movies.erase( sprite_movies.begin() + i );
}

// The movie anim's file names, as its first frame; NULL when it can't play.
SDL_Surface *ONScripter::openSpriteMovie( AnimationInfo *anim )
{
    for (size_t i = 0; i < sprite_movies.size(); i++)
        if (sprite_movies[i]->anim == anim){ closeSpriteMovie( i ); break; }

    SpriteMovie *s = new SpriteMovie;
    s->anim = anim;
    s->name = anim->file_name;
    s->loop = anim->duration_list && anim->loop_mode == 0; // ":c/1,0,0;"
    anim->is_animatable = false; // the frames come from here, not the cells

    std::string path = std::string( archive_path ) + anim->file_name;
    for (char &c : path) if (c == '/' || c == '\\') c = DELIMITER;
    // The global fopen: the member adds archive_path again. (The web build
    // fetches the file here.)
    FILE *fp = ::fopen( path.c_str(), "rb" );
    if (fp) fclose( fp );
    else{
        utils::printError( " *** can't find file [%s] ***\n", anim->file_name );
        if (trace_fp) traceWarn( "missing movie", anim->file_name );
        delete s;
        return NULL;
    }
    if (!movieStart( s, path.c_str(), texture_format )){
        utils::printError( "can't play the movie [%s] as a picture\n", anim->file_name );
        if (trace_fp) traceWarn( "broken movie", anim->file_name );
        movieStop( s );
        delete s;
        return NULL;
    }
    s->frame.assign( (size_t)s->w * s->h * 4, 0 );
    s->ended = !movieFirstFrame( s );
    movieToScreenFormat( s, texture_format );
    s->start = SDL_GetTicks();
    SDL_Surface *surface = movieSurface( s, texture_format );
    if (s->ended || auto_play_flag){ // the first frame stays
        movieStop( s );
        delete s;
    }
    else
        sprite_movies.push_back( s );
    return surface;
}

int ONScripter::nextSpriteMovieTime()
{
    int min = 0, now = SDL_GetTicks();
    for (SpriteMovie *s : sprite_movies){
        int t = s->start + (int)(s->shown * 1000 / s->fps);
        if (t <= now) t = now + 1; // behind (after an mpegplay): catch up
        if (min == 0 || t < min) min = t;
    }
    return min;
}

// Each movie's frame due now, drawn (unless !draw: an effect shows it);
// a movie whose picture is gone stops. True when a shown picture changed.
bool ONScripter::proceedSpriteMovies( int current_time, bool draw )
{
    bool changed = false;
    for (size_t i = 0; i < sprite_movies.size(); ){
        SpriteMovie *s = sprite_movies[i];
        AnimationInfo *anim = s->anim;
        if (!anim->image_surface || !anim->surface_name || s->name != anim->surface_name){
            closeSpriteMovie( i );
            continue;
        }
        int due = (int)((Uint32)current_time - s->start) * s->fps / 1000;
        if (movieFrame( s, due )){
            movieToScreenFormat( s, texture_format );
            anim->deleteSurface( false );
            setAnimImage( anim, movieSurface( s, texture_format ), NULL, false, BaseReader::ARCHIVE_TYPE_NONE );
            if (anim->visible) changed = true;
            if (anim->visible && draw){
                SDL_Rect rect = anim->pos;
                for (int n = 0; n < MAX_SPRITE_NUM; n++)
                    if (anim == &sprite_info[n]){ rect = drawnRect( *anim ); break; }
                if (anim == &bg_info) rect = screen_rect;
                flushDirect( rect, refreshMode() | (draw_cursor_flag ? REFRESH_CURSOR_MODE : 0) );
            }
        }
        if (s->ended){ // on its last frame
            closeSpriteMovie( i );
            continue;
        }
        i++;
    }
    return changed;
}

/* spmoviewait n[,click]
 *   Waits until the movie in sprite n (lsp of a movie file) has ended; with
 *   click 1 a click, skip or auto-play (moviestop overrides the flag) ends
 *   the wait, the movie stopping on the frame shown and the sounds started
 *   since the last wait (its own) with it. A looping movie, or none,
 *   returns at once. */
int ONScripter::spmoviewaitCommand()
{
    int no = script_h.readInt();
    bool click = false;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA)
        click = script_h.readInt() == 1;
    if (movie_stop >= 0) click = movie_stop == 1;
    if (no < 0 || no >= MAX_SPRITE_NUM) return RET_CONTINUE;
    AnimationInfo *anim = &sprite_info[no];
    SpriteMovie *s = findSpriteMovie( anim );
    if (!s && auto_play_flag && click && isMovieFile( anim->surface_name ) &&
        !(anim->duration_list && anim->loop_mode == 0)){ // shown as its first frame: skipped
        stopFreshChannels();
        return RET_CONTINUE;
    }
    if (!s || s->loop) return RET_CONTINUE;
    unsigned long long fresh = fresh_channels; // the waits below clear it
    for (;;){
        bool skipped = click && (auto_play_flag || skip_mode & (SKIP_NORMAL | SKIP_TO_EOL) || ctrl_pressed_status);
        if (skipped){ // on the frame shown
            for (size_t i = 0; i < sprite_movies.size(); i++)
                if (sprite_movies[i]->anim == anim){ closeSpriteMovie( i ); break; }
            fresh_channels = fresh;
            stopFreshChannels();
            break;
        }
        if (!findSpriteMovie( anim )) break;
        int next = nextSpriteMovieTime() - (int)SDL_GetTicks();
        event_mode = WAIT_TIMER_MODE | (click ? WAIT_INPUT_MODE : 0);
        if (waitEvent( next < 1 ? 1 : next > 100 ? 100 : next )) return RET_CONTINUE;
    }
    event_mode = IDLE_EVENT_MODE;
    return RET_CONTINUE;
}

/* logrows %n,columns[,width]
 *   Lays the text history out as rows, oldest first, and puts how many
 *   there are in %n: each page's lines, cut at columns full-width
 *   characters (a half-width one takes half, or in a proportional
 *   sentence font its own width over the font's size; a row of
 *   half-width text is cut at the space before the word that doesn't
 *   fit), with ruby shown as its base text. A page with no text has no
 *   rows. With width, a row is width pixels wide at the sentence font's
 *   size instead (proportional text drawn in a field of its own width).
 * getlogrow $text,$voice,row
 *   The text of row row (0 = the oldest) as logrows laid it out, "" past
 *   the last; $voice is the voice the row's page played on channel 0
 *   (as dwave named it) on the page's first row, "" on the others.
 *       logrows %1,26:getlogrow $1,$2,%1-1   (the newest row)
 *   For a backlog scrolled a line at a time, a row per strsp. */
int ONScripter::logrowsCommand()
{
    script_h.readInt();
    script_h.pushVariable();
    int columns = script_h.readInt();
    if (columns < 1) columns = 1;
    int row_px = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : 0;

    log_rows.clear();
    bool utf8 = coding2utf16->force_utf8;
    // Widths in thousandths of a full-width cell: a half-width letter of
    // a proportional font takes its own advance (as the text draws it).
    FontInfo scaled_font;
    FontInfo *glyph_info = textSizeFont(&sentence_font, scaled_font);
    // No TTF opened yet (or a bitmap font): count cells, as before.
    bool proportional = glyph_info->ttf_font[0] != NULL && isProportional(glyph_info);
    int em = glyph_info->font_size_xy[0] > 0 ? glyph_info->font_size_xy[0] : 1;
    auto width = [&](const char *c, int n) -> int {
        if (n > 1) return 1000;
        if (!proportional) return 500;
        int a, adv;
        if (TTF_GlyphMetrics((TTF_Font*)glyph_info->ttf_font[0], (unsigned char)*c, &a, &a, &a, &a, &adv) != 0) return 500;
        return adv * 1000 / em;
    };
    const int limit = row_px > 0 ? row_px * 1000 / em : columns * 1000;
    Page *page = start_page;
    while (1){
        const char *p = page->text, *end = page->text + page->text_count;
        std::string row;
        int units = 0;
        bool first = true, any = false;
        auto put = [&](){
            LogRow r;
            r.text = row;
            if (first && page->voice_file){ r.voice = page->voice_file; r.tag = page->voice_tag; }
            log_rows.push_back( r );
            first = false;
            row.clear();
            units = 0;
        };
        while (p && p < end){
            if (*p == 0x0a){
                put();
                any = false;
                p++;
                continue;
            }
            // ruby, "(base/ruby)": the base
            if (*p == '('){
                const char *slash = (const char*)memchr( p, '/', end - p );
                const char *close = slash ? (const char*)memchr( slash, ')', end - slash ) : NULL;
                if (close){
                    p++;
                    const char *base_end = slash;
                    while (p < base_end){
                        int n = utf8 ? UTF8_N_BYTE(*p) : (IS_TWO_BYTE(*p) ? 2 : 1);
                        int w = width(p, n);
                        if (units + w > limit) put();
                        row.append( p, n );
                        units += w;
                        p += n;
                    }
                    p = close + 1;
                    any = true;
                    continue;
                }
            }
            int n = utf8 ? UTF8_N_BYTE(*p) : (IS_TWO_BYTE(*p) ? 2 : 1);
            if (n < 1 || p + n > end) n = 1;
            int w = width(p, n);
            if (units + w > limit){
                // A word of half-width letters goes whole to the next row,
                // which starts after the space.
                size_t sp = row.rfind( ' ' );
                if (n == 1 && *p != ' ' && sp != std::string::npos && sp > 0){
                    std::string rest = row.substr( sp + 1 );
                    row.erase( sp );
                    put();
                    row = rest;
                    for (size_t i = 0; i < rest.size(); ){
                        int m = utf8 ? UTF8_N_BYTE(rest[i]) : (IS_TWO_BYTE(rest[i]) ? 2 : 1);
                        if (m < 1) m = 1;
                        units += width(&rest[i], m);
                        i += m;
                    }
                }
                else
                    put();
                if (*p == ' '){
                    p++;
                    continue;
                }
            }
            row.append( p, n );
            units += w;
            p += n;
            any = true;
        }
        if (any) put();
        if (page == current_page) break;
        page = page->next;
    }
    script_h.setInt( &script_h.pushed_variable, (int)log_rows.size() );
    return RET_CONTINUE;
}

int ONScripter::getlogrowCommand()
{
    script_h.readVariable();
    int text_var = script_h.current_variable.var_no;
    script_h.readVariable();
    int voice_var = script_h.current_variable.var_no;
    int row = script_h.readInt();
    bool in = row >= 0 && row < (int)log_rows.size();
    setStr( &script_h.getVariableData( text_var ).str, in ? log_rows[row].text.c_str() : NULL );
    setStr( &script_h.getVariableData( voice_var ).str, in && !log_rows[row].voice.empty() ? log_rows[row].voice.c_str() : NULL );
    return RET_CONTINUE;
}

/* logadd "text"[,"voice"[,tag]]
 *   Adds a page with text to the text history, before the page being
 *   written (so what is on screen stays as it is); voice is the voice
 *   getlogrow gives for it, tag its tag as logvoice's. Lines the story
 *   puts in the backlog without showing them. */
int ONScripter::logaddCommand()
{
    std::string text = script_h.readStr();
    const char *voice = NULL;
    std::string v;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA){
        v = script_h.readStr();
        if (!v.empty()) voice = v.c_str();
    }
    int tag = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : -1;
    if (text.empty()) return RET_CONTINUE;

    // The oldest page (or a free one) becomes the page being written,
    // which takes over its contents; the old place gets the new text.
    Page *next = current_page->next;
    if (start_page == next) start_page = start_page->next;
    std::swap( next->text, current_page->text );
    std::swap( next->max_text, current_page->max_text );
    std::swap( next->text_count, current_page->text_count );
    std::swap( next->tag, current_page->tag );
    std::swap( next->voice, current_page->voice );
    std::swap( next->voice_file, current_page->voice_file );
    std::swap( next->voice_tag, current_page->voice_tag );
    Page *added = current_page;
    current_page = next;
    if (cached_page == added) cached_page = current_page;

    if (added->text) delete[] added->text;
    added->max_text = (int)text.size() + 1;
    added->text = new char[added->max_text];
    memcpy( added->text, text.data(), text.size() );
    added->text_count = (int)text.size();
    setStr( &added->tag, NULL );
    std::string file;
    if (voice) traceString( file, voice, -1, false ); // UTF-8, as dwave keeps it
    setStr( &added->voice, voice ? file.c_str() : NULL );
    setStr( &added->voice_file, voice );
    added->voice_tag = voice ? tag : -1;
    return RET_CONTINUE;
}

/* logvoice "voice",tag
 *   The page being written gets voice as its history voice, as if dwave
 *   had played it on channel 0, without playing it, and tag (a number,
 *   such as the speaker's) with it: a voice the player switched off is
 *   still in the log, and a screen's rows list can leave its button out
 *   while the switch is off (`voice_off`). A dwave on channel 0 after it
 *   keeps the tag. */
int ONScripter::logvoiceCommand()
{
    std::string v = script_h.readStr();
    int tag = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : -1;
    if (!current_page) return RET_CONTINUE;
    std::string file;
    if (!v.empty()) traceString( file, v.c_str(), -1, false ); // UTF-8, as dwave keeps it
    setStr( &current_page->voice, v.empty() ? NULL : file.c_str() );
    setStr( &current_page->voice_file, v.empty() ? NULL : v.c_str() );
    current_page->voice_tag = v.empty() ? -1 : tag;
    return RET_CONTINUE;
}

// The text window as the screen sees it: the window picture and the area
// its text can take, which may lie outside a small picture (a fully
// transparent 1x1 picture: the text on the background). Refreshing only
// the picture left the last page's text under the next one.
SDL_Rect ONScripter::textWindowRect()
{
    SDL_Rect r = sentence_font_info.pos;
    FontInfo &f = sentence_font;
    int n = f.num_xy[0] > f.num_xy[1] ? f.num_xy[0] : f.num_xy[1];
    int pad = f.font_size_xy[0] > f.font_size_xy[1] ? f.font_size_xy[0] : f.font_size_xy[1];
    SDL_Rect t;
    t.x = f.top_xy[0] - pad;
    t.y = f.top_xy[1] - pad;
    t.w = f.pitch_xy[0] * n + pad * 2;
    t.h = f.pitch_xy[1] * n + pad * 2;
    if (r.w <= 0 || r.h <= 0) return t;
    int x0 = r.x < t.x ? r.x : t.x, y0 = r.y < t.y ? r.y : t.y;
    int x1 = r.x + r.w > t.x + t.w ? r.x + r.w : t.x + t.w;
    int y1 = r.y + r.h > t.y + t.h ? r.y + r.h : t.y + t.h;
    r.x = x0; r.y = y0; r.w = x1 - x0; r.h = y1 - y0;
    return r;
}

// CONscripter: --filter crt,pc98,...: the screen as shown goes through these
// filters, in the order given. Only what goes to the window changes: the
// picture the game draws (saves, screenshots, the trace's shots) stays as it is.
bool ONScripter::setScreenFilter(const char *names)
{
    static const char *list[] = { "crt", "pc98", "gameboy", "sepia", "mono", "vhs" }; // FILTER_* order
    screen_filters.clear();
    std::string all(names);
    for (size_t start = 0; start <= all.size(); ) {
        size_t end = all.find(',', start);
        if (end == std::string::npos) end = all.size();
        std::string name = all.substr(start, end - start);
        int i = 0;
        while (i < 6 && name != list[i]) ++i;
        if (i == 6) return false;
        screen_filters.push_back(i);
        start = end + 1;
    }
    return true;
}

// Filters rect of accumulation_surface into filter_buf and returns it, with
// its pitch. Patterns (scanlines, dither) follow screen coordinates, so a
// part of the screen updated alone lines up with the rest; vhs widens rect,
// as its colours spread 2 pixels sideways.
const unsigned char *ONScripter::filterScreen(SDL_Rect &rect, int &pitch)
{
    static const int bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    static const Uint8 gb[4][3] = { { 15, 56, 15 }, { 48, 98, 48 }, { 139, 172, 15 }, { 155, 188, 15 } };
    const SDL_PixelFormat *f = accumulation_surface->format;
    const Uint32 keep = ~(f->Rmask | f->Gmask | f->Bmask); // alpha
    bool vhs = std::find(screen_filters.begin(), screen_filters.end(), (int)FILTER_VHS) != screen_filters.end();
    if (vhs) {
        int x0 = rect.x - 2 < 0 ? 0 : rect.x - 2;
        int x1 = rect.x + rect.w + 2 > screen_width ? screen_width : rect.x + rect.w + 2;
        rect.x = x0; rect.w = x1 - x0;
    }
    filter_buf.resize((size_t)rect.w * rect.h);
    const int src_pitch = accumulation_surface->pitch / sizeof(ONSBuf);
    const ONSBuf *src = (const ONSBuf*)accumulation_surface->pixels;
    for (int y = 0; y < rect.h; ++y) {
        const int sy = rect.y + y;
        const ONSBuf *row = src + sy * src_pitch;
        ONSBuf *out = filter_buf.data() + (size_t)y * rect.w;
        for (int x = 0; x < rect.w; ++x) {
            const int sx = rect.x + x;
            ONSBuf p = row[sx];
            int r = (p >> f->Rshift) & 0xff, g = (p >> f->Gshift) & 0xff, b = (p >> f->Bshift) & 0xff;
            if (vhs) { // red from 2 pixels right, blue from 2 left
                r = (row[sx + 2 < screen_width ? sx + 2 : screen_width - 1] >> f->Rshift) & 0xff;
                b = (row[sx - 2 >= 0 ? sx - 2 : 0] >> f->Bshift) & 0xff;
            }
            const int d = bayer[(sy & 3) * 4 + (sx & 3)]; // 0..15
            for (int k : screen_filters) {
                switch (k) {
                case FILTER_CRT: { // darker odd lines, a faint RGB stripe mask
                    int line = (sy & 1) ? 160 : 256;
                    int m[3] = { 220, 220, 220 };
                    m[sx % 3] = 256;
                    r = r * line * m[0] >> 16; g = g * line * m[1] >> 16; b = b * line * m[2] >> 16;
                    break;
                }
                case FILTER_PC98: { // 64 colours (4 levels a channel), ordered dither
                    int c[3] = { r, g, b };
                    for (int &v : c) {
                        int q = (v * 3 + d * 255 / 16) / 255; // the dither rounds
                        q = q < 0 ? 0 : q > 3 ? 3 : q;
                        v = q * 85;
                    }
                    r = c[0]; g = c[1]; b = c[2];
                    break;
                }
                case FILTER_GAMEBOY: { // 4 greens, ordered dither
                    int l = (77 * r + 150 * g + 29 * b) >> 8;
                    int q = (l * 3 + d * 255 / 16) / 255;
                    q = q < 0 ? 0 : q > 3 ? 3 : q;
                    r = gb[q][0]; g = gb[q][1]; b = gb[q][2];
                    break;
                }
                case FILTER_SEPIA: {
                    int nr = (101 * r + 197 * g + 48 * b) >> 8;
                    int ng = (89 * r + 176 * g + 43 * b) >> 8;
                    int nb = (70 * r + 137 * g + 34 * b) >> 8;
                    r = nr > 255 ? 255 : nr; g = ng > 255 ? 255 : ng; b = nb;
                    break;
                }
                case FILTER_MONO:
                    r = g = b = (77 * r + 150 * g + 29 * b) >> 8;
                    break;
                }
            }
            out[x] = (p & keep) | ((Uint32)r << f->Rshift) | ((Uint32)g << f->Gshift) | ((Uint32)b << f->Bshift);
        }
    }
    pitch = rect.w * sizeof(ONSBuf);
    return (const unsigned char*)filter_buf.data();
}

// CONscripter: the original game's archives, read in place, so a converted game
// needs only its script and the player's own files. cons_archives.txt in the
// game folder names them, one per line:
//   npa <folder> <file> <512 hex digits>
// the script's <folder>/x is entry x of the Nitroplus NPA <file> (relative to
// the game folder); the digits are its 256-byte decryption table. Loose files
// come first, then these archives, then arc.nsa.
namespace {
struct OriginalEntry { FILE *fp; long long offset; size_t stored, size, encrypted; unsigned char k; const unsigned char *table; };
struct OriginalArchive { FILE *fp; unsigned char table[256]; };
std::vector<OriginalArchive*> original_archives;
std::unordered_map<std::string, OriginalEntry> original_files;

// The lookup form of a name: '/' separators, lower case (ASCII and full-width
// Latin: the scripts write ｂ where the archive has Ｂ).
// shortcut: other letters (Greek, Cyrillic) keep their case; fold them when a game needs it
std::string originalKey( const char *name )
{
    std::string s( name );
    for (size_t i=0 ; i<s.size() ; i++){
        unsigned char c = s[i];
        if (c == '\\') s[i] = '/';
        else if (c >= 'A' && c <= 'Z') s[i] = c + 'a' - 'A';
        else if (c == 0xef && i+2 < s.size() && (unsigned char)s[i+1] == 0xbc && (unsigned char)s[i+2] >= 0xa1 && (unsigned char)s[i+2] <= 0xba){
            s[i+1] = (char)0xbd; // U+FF21-FF3A to U+FF41-FF5A
            s[i+2] = (char)((unsigned char)s[i+2] - 0x20);
            i += 2;
        }
    }
    return s;
}

int originalSeek( FILE *fp, long long offset )
{
#if defined(_WIN32)
    return _fseeki64( fp, offset, SEEK_SET );
#else
    return fseeko( fp, (off_t)offset, SEEK_SET );
#endif
}

unsigned int le32( const unsigned char *p ){ return p[0] | p[1] << 8 | p[2] << 16 | (unsigned int)p[3] << 24; }
unsigned int byteSum( unsigned int v ){ return (v & 0xff) + (v >> 8 & 0xff) + (v >> 16 & 0xff) + (v >> 24); }

// Reads an NPA's index (the layout: conscripter-convert's npa.rs) and adds
// its files under folder/.
bool readNpa( OriginalArchive *a, const std::string &folder, const char *path )
{
    unsigned char h[0x29];
    webPreload( a->fp, 0, sizeof(h) );
    if (fread( h, 1, sizeof(h), a->fp ) != sizeof(h) || memcmp( h, "NPA", 3 )) return false;
    if (h[15]){
        // shortcut: zlib-compressed archives (scripts, system.npa) are refused; add inflate when a game needs one at run time
        utils::printError( "cons_archives: %s is compressed, not read\n", path );
        return false;
    }
    unsigned int key12 = le32( h+7 ) * le32( h+11 ), total = le32( h+17 ), index_len = le32( h+37 );
    std::vector<unsigned char> index( index_len );
    webPreload( a->fp, sizeof(h), index_len );
    if (fread( index.data(), 1, index_len, a->fp ) != index_len) return false;
    bool encrypted = h[16] != 0;
    size_t p = 0;
    for (unsigned int i=0 ; i<total ; i++){
        if (p + 4 > index_len) return false;
        unsigned int n = le32( &index[p] );
        if (p + 4 + n + 17 > index_len) return false;
        std::string raw( (const char*)&index[p+4], n );
        unsigned int sum = 0;
        for (unsigned int x=0 ; x<n ; x++){
            raw[x] = (char)((unsigned char)raw[x] + 0xfc * x - byteSum( key12 ) - byteSum( i ));
            sum += (unsigned char)raw[x];
        }
        const unsigned char *e = &index[p + 4 + n];
        p += 4 + n + 17;
        if (e[0] != 2 || le32( e+9 ) != le32( e+13 )) continue; // a folder (stored = size: not compressed)
        OriginalEntry f;
        f.fp = a->fp;
        f.offset = 0x29LL + index_len + le32( e+5 );
        f.stored = le32( e+9 );
        f.size = le32( e+13 );
        f.encrypted = encrypted ? 0x1000 + n : 0;
        f.k = (unsigned char)(((0x87654321u - sum) * n + key12) * (unsigned int)f.size);
        f.table = a->table;
        original_files[originalKey( (folder + "/" + sjisToUTF8( raw.c_str() )).c_str() )] = f;
    }
    return true;
}

void loadOriginalArchives( const char *archive_path )
{
    std::string dir( archive_path ? archive_path : "" );
    FILE *fp = fopen( (dir + "cons_archives.txt").c_str(), "r" );
    if (!fp) return;
    char line[1024], kind[16], folder[256], file[256], hex[600];
    while (fgets( line, sizeof(line), fp )){
        if (sscanf( line, "%15s %255s %255s %599s", kind, folder, file, hex ) != 4 || strcmp( kind, "npa" ) || strlen( hex ) != 512) continue;
        OriginalArchive *a = new OriginalArchive();
        for (int i=0 ; i<256 ; i++){
            unsigned int b;
            sscanf( hex + i*2, "%2x", &b );
            a->table[i] = (unsigned char)b;
        }
        std::string path = dir + file;
        a->fp = fopen( path.c_str(), "rb" );
        if (!a->fp || !readNpa( a, originalKey( folder ), path.c_str() )){
            utils::printError( "cons_archives: can't read %s\n", path.c_str() );
            if (a->fp) fclose( a->fp );
            delete a;
            continue;
        }
        original_archives.push_back( a );
    }
    fclose( fp );
}
}

size_t consOriginalFile( const char *archive_path, const char *file_name, unsigned char *buffer )
{
    static bool loaded = false;
    if (!loaded){
        loaded = true;
        loadOriginalArchives( archive_path );
    }
    if (original_files.empty()) return 0;
    std::unordered_map<std::string, OriginalEntry>::const_iterator it = original_files.find( originalKey( file_name ) );
    if (it == original_files.end()) return 0;
    const OriginalEntry &f = it->second;
    if (!buffer) return f.size;
    webPreload( f.fp, (size_t)f.offset, f.stored );
    if (originalSeek( f.fp, f.offset ) || fread( buffer, 1, f.stored, f.fp ) != f.stored) return 0;
    for (size_t i=0 ; i<f.stored && i<f.encrypted ; i++)
        buffer[i] = (unsigned char)(f.table[buffer[i]] - f.k - i);
    return f.size;
}
