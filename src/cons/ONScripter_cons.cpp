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
#include "Utils.h"
#include "coding2utf16.h"
#include "gbk2utf16.h"
#include "version.h"
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
#define CONS_SAVE_VERSION 23 // only this version's block is read; the "Version n" comments below
                            // name the parts in the order they are written

extern SDL_TimerID timer_bgmfade_id; // bgmfadein, bgmfadeout

/* chvolfade: each dwave channel's script volume, a percentage of the
 * setting's, ramped linearly from `from` to `to` over `ms` from `start`.
 * The track effect below scales the samples by it (in 1/4096, from the
 * last mix's gain to this one's across the buffer, so a ramp has no
 * steps); SDL_mixer's channel volume, which the settings and dwavefadein
 * use, stays theirs. It stays with the channel, for the next sounds too,
 * so a script can set it before the sound starts; reset sets 100. */
static struct { volatile int from, to, ms; volatile Uint32 start; int last; } track_gain[ONS_MIX_CHANNELS];
static bool track_gain_s16 = false, track_stereo = false;
/* chpan: each dwave channel's pan, -1000 (left) .. 1000 (right), ramped
 * as track_gain; the track effect lowers the other side (stereo only). */
static struct { volatile int from, to, ms; volatile Uint32 start; int last; } track_pan[ONS_MIX_CHANNELS];

static int scriptVolumeAt( int from, int to, int ms, Uint32 start, Uint32 now )
{
    Uint32 t = now - start;
    if (ms <= 0 || t >= (Uint32)ms) return to;
    return from + (to - from) * (int)t / ms;
}

/* soundmute: the groups the player has muted (SOUND_MUTE_*). The track
 * effect below silences a muted channel's sound (ramped over a buffer, so
 * no click) and it goes on playing unheard; the music's volume is 0. */
#define SOUND_MUTE_BGM   1
#define SOUND_MUTE_SE    2
#define SOUND_MUTE_VOICE 4
static volatile int sound_mute = 0;

static bool channelMuted( int channel )
{
    int group = channel == 0 ? SOUND_MUTE_VOICE : channel == MIX_BGM_CHANNEL ? SOUND_MUTE_BGM : SOUND_MUTE_SE;
    return (sound_mute & group) != 0;
}

// Appends text (as JSON when json) to out (ONScripter_trace.cpp).
void traceString( std::string &out, const char *text, int len=-1, bool json=true );

// chpan's pan on channel now, -1000..1000.
static int trackPan( int channel )
{
    return scriptVolumeAt( track_pan[channel].from, track_pan[channel].to, track_pan[channel].ms,
                           track_pan[channel].start, SDL_GetTicks() );
}

int trackGain( int channel ) // 0-4096; the trace reads it too
{
    if (channel < 0 || channel >= ONS_MIX_CHANNELS) return 4096;
    if (channelMuted( channel )) return 0;
    return scriptVolumeAt( track_gain[channel].from, track_gain[channel].to, track_gain[channel].ms,
                           track_gain[channel].start, SDL_GetTicks() ) * 4096 / 100;
}

/* spfilter num, kind[, blend[, strength[, param...]]]
 *   kind 0 clears every filter on sprite num. Other kinds add a filter
 *   (up to AnimationInfo::MAX_FILTER_NUM, applied in order) that makes a
 *   colour for each pixel: 1 the colour param r,g,b; 2 the pixel in
 *   greyscale; 3 its negative; 4 the pixel plus param (-255..255);
 *   5 the pixel in greyscale plus the offsets r,g,b (-255..255), a
 *   monotone like sepia: "spfilter 10,5,0,256,28,-8,-38"; 6 a blur: the
 *   mean of the pixels up to param x,y pixels away; 7 the pixel with its
 *   hue turned by param/256 of a turn and its saturation moved by the
 *   second param (-256 grey .. 0 kept .. 256 full); 8 the pixel with its
 *   lightness inverted (a negative that keeps the hue).
 *   blend combines that colour with the pixel's: 0 copy (default),
 *   1 multiply, 2 add, 3 screen. strength 0-256 (default 256) is how much
 *   of the result replaces the pixel. So "spfilter 10,1,1,256,255,200,160"
 *   tints sprite 10 warm, and "spfilter 10,1,0,256,0,0,0" makes it a black
 *   silhouette.
 *   A sprite without an image ignores spfilter, like the original
 *   engines' filters, which belong to a loaded object. The filters stay
 *   when lsp changes the image and are cleared by csp. */
int ONScripter::spfilterCommand()
{
    leaveTextDisplayMode();

    int no = script_h.readInt();
    AnimationInfo::Filter f;
    f.kind = script_h.readInt();
    f.blend = AnimationInfo::FILTER_BLEND_COPY;
    f.strength = 256;
    f.param[0] = f.param[1] = f.param[2] = 0;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA)
        f.blend = script_h.readInt();
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA)
        f.strength = script_h.readInt();
    for (int i=0 ; i<3 && (script_h.getEndStatus() & ScriptHandler::END_COMMA) ; i++)
        f.param[i] = script_h.readInt();

    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spfilter: sprite %d out of range\n", no);
        return RET_CONTINUE;
    }
    AnimationInfo *ai = &sprite_info[no];
    if (ai->image_name == NULL) return RET_CONTINUE;

    if (f.kind == AnimationInfo::FILTER_NONE){
        if (ai->num_of_filters == 0) return RET_CONTINUE;
        ai->clearFilters();
    }
    else if (f.kind < AnimationInfo::FILTER_COLOUR || f.kind > AnimationInfo::FILTER_LAST){
        utils::printError("spfilter: unknown kind %d\n", f.kind);
        return RET_CONTINUE;
    }
    else if (f.blend < AnimationInfo::FILTER_BLEND_COPY || f.blend > AnimationInfo::FILTER_BLEND_SCREEN){
        utils::printError("spfilter: unknown blend %d\n", f.blend);
        return RET_CONTINUE;
    }
    else if (ai->num_of_filters >= AnimationInfo::MAX_FILTER_NUM){
        utils::printError("spfilter: sprite %d already has %d filters\n", no, ai->num_of_filters);
        return RET_CONTINUE;
    }
    else{
        if (f.strength < 0)   f.strength = 0;
        if (f.strength > 256) f.strength = 256;
        ai->filter[ai->num_of_filters++] = f;
        ai->filter_changed = true;
    }

    if (ai->visible) dirty_rect.add( ai->pos );
    setupAnimationInfo( ai );
    if (ai->visible) dirty_rect.add( ai->pos );

    return RET_CONTINUE;
}

/* spzoom num, zoom_x[, zoom_y[, angle]]
 *   Draws sprite num (an lsp sprite) zoomed by zoom_x and zoom_y percent
 *   (100 = its size; zoom_y defaults to zoom_x) and turned counterclockwise
 *   by angle degrees (as lsp2; left as it was when not given) about the
 *   centre of its place, which amsp and msp move as usual. Unlike lsp2, the sprite keeps
 *   its place among the other sprites. The zoom stays when lsp changes the
 *   image and is reset by csp. A zoomed sprite's buttons keep their
 *   unzoomed place. spzoom redraws the whole screen at the next print:
 *   call it again after moving a zoomed sprite, so that what its zoomed
 *   picture covered is redrawn too: "amsp 10,x,y:spzoom 10,150:print 1". */
int ONScripter::spzoomCommand()
{
    leaveTextDisplayMode();

    int no = script_h.readInt();
    int zx = script_h.readInt();
    int zy = zx;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA)
        zy = script_h.readInt();
    bool turn = script_h.getEndStatus() & ScriptHandler::END_COMMA;
    int deg = turn ? script_h.readInt() : 0;

    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spzoom: sprite %d out of range\n", no);
        return RET_CONTINUE;
    }
    AnimationInfo *ai = &sprite_info[no];
    ai->zoom_x = zx;
    ai->zoom_y = zy;
    if (turn) ai->zoom_rot = deg;
    // Where the zoomed picture was isn't known after an amsp: redraw it all.
    if (ai->visible) dirty_rect.fill( screen_width, screen_height );

    return RET_CONTINUE;
}

/* spmotion num[,loop,t1,x1,y1,zx1,zy1,angle1,alpha1,curve1,...]
 *   Moves sprite num (an lsp sprite) by itself along keys from its place:
 *   at t ms from now the offset x,y (pixels), the zoom zx,zy (percent,
 *   times spzoom's), the angle (degrees counterclockwise, added to
 *   spzoom's), both about the centre of its place, and the alpha (0-256 of
 *   the sprite's). curve is how a key is reached from the one before: 0
 *   straight, 1 ease in, 2 ease out, 3 ease in and out, 4 jump at the key,
 *   10-73 a table defined by spcurve. Stepped about every 16 ms by the
 *   timer, during text and click waits too. loop 1 starts again after the
 *   last key; loop 0 ends at the last key with the sprite back at its
 *   place; loop 2 runs once and leaves the sprite at the last key (its
 *   offset added to the place as msp, its zoom to spzoom, its angle to
 *   spzoom's angle, its alpha to the sprite's); loop -n runs n times, then does as
 *   loop 2; loop 3 runs once and then removes the sprite as csp does (lsp
 *   on it ends that and keeps the sprite; a save leaves it out); loop 4
 *   is followed by a key number k before the keys: it runs once, then
 *   repeats from key k (a lead-in, then a loop; as loop 1, it never ends
 *   by itself, under --auto-play too): a fade
 *   out is "spmotion num,3,0,0,0,100,100,0,256,0,ms,0,0,100,100,0,0,0", a
 *   fade in a loop 0 from alpha 0. Under --auto-play (without --real-time),
 *   which doesn't wait, a motion that ends (any loop but 1 and 4) ends at once.
 *   "spmotion num" stops it. Up to 64 keys. The motion stays when
 *   lsp changes the image, is removed by csp and is kept in save files
 *   (restarted). Another spmotion, lsp, msp or amsp first ends a loop 2
 *   or -n motion at its last key.
 *   "spmotion 10,1,0,0,0,100,100,0,256,0,1000,0,0,100,100,360,256,0"
 *   spins sprite 10 once a second. */
int ONScripter::spmotionCommand()
{
    leaveTextDisplayMode();

    int no = script_h.readInt();
    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spmotion: sprite %d out of range\n", no);
        while (script_h.getEndStatus() & ScriptHandler::END_COMMA) script_h.readInt();
        return RET_CONTINUE;
    }
    AnimationInfo *ai = &sprite_info[no];
    SDL_Rect before = drawnRect( *ai );
    bool shown = ai->visible; // (loop 3 may remove it at once)
    endKeptMotion( *ai );
    traceMotion( *ai, (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? "replaced" : "stopped" );
    ai->stopMotion();
    ai->trail_keys = 0;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA){
        int mode = script_h.readInt();
        ai->motion_loop = mode == 1 || mode == 4;
        ai->motion_keep = mode == 2 || mode < 0;
        ai->motion_leave = mode == 3;
        ai->motion_passes = mode < 0 ? -mode : 0;
        // loop 4: the key to go back to, then the keys
        ai->motion_loop_key = 0;
        int loop_key = mode == 4 && (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : 0;
        while (script_h.getEndStatus() & ScriptHandler::END_COMMA){
            int v[8];
            for (int j=0 ; j<8 ; j++) v[j] = script_h.readInt();
            if (ai->motion_keys == AnimationInfo::MAX_MOTION_KEYS){
                utils::printError("spmotion: more than %d keys\n", AnimationInfo::MAX_MOTION_KEYS);
                continue;
            }
            ai->allocMotionKeys();
            int *k = ai->motion_key[ai->motion_keys++];
            // stored as ms, x, y, zoom x, angle, alpha, zoom y, curve
            k[0] = speedTime( v[0] ); k[1] = v[1]; k[2] = v[2]; k[3] = v[3]; // gamespeed
            k[4] = v[5]; k[5] = v[6] < 0 ? 0 : v[6] > 256 ? 256 : v[6]; k[6] = v[4]; k[7] = v[7];
        }
        if (loop_key < 0 || (loop_key > 0 && loop_key >= ai->motion_keys - 1)){
            utils::printError("spmotion: loop key %d out of range\n", loop_key);
            loop_key = 0;
        }
        ai->motion_loop_key = loop_key;
        ai->motion_start = ai->motion_next = SDL_GetTicks();
        traceMotion( *ai, NULL );
        stepSpriteMotion( *ai, ai->motion_start );
        if (((auto_play_flag && !auto_play_real) || still_screen) && !ai->motion_loop && ai->motion_keys > 0){ // still_screen: e-ink shows only the end
            int end = ai->motion_start + ai->motion_key[ai->motion_keys-1][0] * (ai->motion_passes > 1 ? ai->motion_passes : 1);
            if (ai->visible) dirty_rect.add( drawnRect( *ai ) );
            stepSpriteMotion( *ai, end );
            ai->motion_end = end; // gettimer and shakes still count its time at spmotionwait
        }
    }
    if (shown) dirty_rect.add( before );
    if (ai->visible){
        dirty_rect.add( drawnRect( *ai ) );
    }

    return RET_CONTINUE;
}

/* spmotionwait num[,click]
 *   Waits until sprite num's motion (spmotion) has ended; num -1: every
 *   motion that ends (looping ones are left out). click 1: a click ends
 *   the wait. Whatever is left when it ends (a click, skipping, an
 *   --auto-play run, whose autoPlayWait cuts the wait to 1 ms) jumps to the motion's end, as if
 *   its time had passed, and the screen is drawn. For a move that must end
 *   before the script goes on, in place of a loop of waits:
 *       spmotion 10,2,0,0,0,100,100,0,256,0,500,200,0,100,100,0,256,3:spmotionwait 10 */
int ONScripter::spmotionwaitCommand()
{
    int no = script_h.readInt();
    bool click = (script_h.getEndStatus() & ScriptHandler::END_COMMA) && script_h.readInt() == 1;
    int now = SDL_GetTicks(), left = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo &ai = sprite_info[i];
        if (no >= 0 && i != no) continue;
        if (ai.motion_keys == 0 && ai.motion_end - now > left) left = ai.motion_end - now; // ended by --auto-play
        ai.motion_end = 0;
        if (ai.motion_keys == 0 || ai.motion_loop) continue;
        int end = ai.motion_start + ai.motion_key[ai.motion_keys-1][0] * (ai.motion_passes > 1 ? ai.motion_passes : 1);
        if (end - now > left) left = end - now;
    }
    if (left > 0 && !(skip_mode & SKIP_NORMAL || ctrl_pressed_status)){
        event_mode = WAIT_TIMER_MODE | (click ? WAIT_INPUT_MODE : 0);
        waitEvent( left );
    }
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo &ai = sprite_info[i];
        if ((no >= 0 && i != no) || ai.motion_keys == 0 || ai.motion_loop) continue;
        if (ai.visible) dirty_rect.add( drawnRect( ai ) );
        stepSpriteMotion( ai, ai.motion_start + ai.motion_key[ai.motion_keys-1][0] * (ai.motion_passes > 1 ? ai.motion_passes : 1) );
        if (ai.visible) dirty_rect.add( drawnRect( ai ) );
    }
    flush( refreshMode() );

    return RET_CONTINUE;
}

/* spcurve id,v0,v1,...,vn
 *   Defines curve id (10-73) for spmotion's keys: how far (0-1024) the
 *   way from one key to the next goes, sampled at n+1 even steps of the
 *   time, straight between samples. A curve read from a game's exe (its
 *   easing table, or values its function returns) goes here as it is.
 *   Kept in save files. "spcurve 10,0,40,300,1024" starts slowly. */
int ONScripter::spcurveCommand()
{
    int id = script_h.readInt();
    std::vector<int> v;
    while (script_h.getEndStatus() & ScriptHandler::END_COMMA) v.push_back( script_h.readInt() );
    if (id < CURVE_FIRST || id >= CURVE_FIRST + MAX_CURVES || v.size() < 2 || v.size() > MAX_CURVE_VALUES){
        utils::printError("spcurve: curve %d out of range, or fewer than 2 or more than %d values\n", id, MAX_CURVE_VALUES);
        return RET_CONTINUE;
    }
    curve_table[id - CURVE_FIRST] = v;

    return RET_CONTINUE;
}

// How far (0-1024) along from one key to the next a motion is at p (0-1024)
// of the time between them, by the next key's curve.
int ONScripter::curveAt( int curve, int p )
{
    if (p <= 0) return 0;
    if (p >= 1024) return 1024;
    switch (curve){
      case 1: return p * p / 1024;
      case 2: return 1024 - (1024 - p) * (1024 - p) / 1024;
      case 3: return (int)((long long)p * p * (3 * 1024 - 2 * p) / (1024 * 1024));
      case 4: return 0;
    }
    if (curve < CURVE_FIRST || curve >= CURVE_FIRST + MAX_CURVES) return p;
    const std::vector<int> &v = curve_table[curve - CURVE_FIRST];
    if (v.size() < 2) return p;
    int n = (int)v.size() - 1, i = p * n / 1024, r = p * n % 1024;
    return v[i] + (int)((long long)(v[i+1] - v[i]) * r / 1024);
}

/* spcut num,"mask",x,y
 *   Cuts a hole in sprite num's picture: the mask picture, its top left at
 *   screen (x, y), takes opacity away where it is dark (black: none left,
 *   white or transparent: unchanged), in every cell. It lasts until the
 *   sprite's picture is loaded again (not kept in saves). A bullet hole
 *   where the player clicked: getmousepos %0,%1:sub %0,47:sub %1,41:
 *   spcut 52,"hole.png",%0,%1:print 1. */
int ONScripter::spcutCommand()
{
    int no = script_h.readInt();
    char *name = NULL;
    setStr( &name, script_h.readStr() ); // readInt reuses the buffer
    int x = script_h.readInt();
    int y = script_h.readInt();
    AnimationInfo *ai = (no >= 0 && no < MAX_SPRITE_NUM) ? &sprite_info[no] : NULL;
    SDL_Surface *s = ai ? ai->image_surface : NULL;
    SDL_Surface *loaded = (s && s->format->BitsPerPixel == 32 && s->format->Amask) ? loadImage( name ) : NULL;
    SDL_Surface *m = loaded ? SDL_ConvertSurfaceFormat( loaded, SDL_PIXELFORMAT_ARGB8888, 0 ) : NULL;
    if (!m) utils::printError("spcut: sprite %d or mask %s can't be used\n", no, name);
    delete[] name;
    if (loaded) SDL_FreeSurface( loaded );
    if (!m) return RET_CONTINUE;

    int cell_w = s->w / (ai->num_of_cells > 0 ? ai->num_of_cells : 1);
    int ox = x - ai->orig_pos.x, oy = y - ai->orig_pos.y;
    Uint32 amask = s->format->Amask;
    int ashift = s->format->Ashift;
    SDL_LockSurface( s );
    for (int my = 0; my < m->h; my++){
        int sy = oy + my;
        if (sy < 0 || sy >= s->h) continue;
        Uint32 *mrow = (Uint32*)((Uint8*)m->pixels + my * m->pitch);
        Uint32 *srow = (Uint32*)((Uint8*)s->pixels + sy * s->pitch);
        for (int mx = 0; mx < m->w; mx++){
            if (ox + mx < 0 || ox + mx >= cell_w) continue;
            Uint32 p = mrow[mx];
            int ma = p >> 24, lum = (((p >> 16) & 0xff) + ((p >> 8) & 0xff) + (p & 0xff)) / 3;
            int keep = 255 - ma * (255 - lum) / 255; // opacity kept, 0-255
            for (int c = 0; c < ai->num_of_cells || c == 0; c++){
                Uint32 &d = srow[c * cell_w + ox + mx];
                Uint32 a = ((d & amask) >> ashift) * keep / 255;
                d = (d & ~amask) | ((a << ashift) & amask);
            }
        }
    }
    SDL_UnlockSurface( s );
    SDL_FreeSurface( m );
    if (ai->visible) dirty_rect.add( drawnRect( *ai ) );

    return RET_CONTINUE;
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

/* spgrab num[,src[,x,y,w,h]]
 *   Puts a copy of a picture into lsp sprite num, which is shown where the
 *   copy was taken: src -1 (default) the screen as the sprites and the
 *   background draw it now (without the text window, sprite num left
 *   out), src n lsp sprite n's picture (its cell now), each from (x, y)
 *   for w x h (default: all of it; for a sprite, x, y are in its picture).
 *   Then the picture under it can change while the copy covers it, and the
 *   copy can fade or move away by itself (spmotion), a cross-fade the
 *   script doesn't wait for:
 *       spgrab 20:bg "b.png",1:spmotion 20,2,0,0,0,100,100,0,256,0,800,0,0,100,100,0,0,0
 *   or stand for an object an engine draws into a texture. The copy isn't
 *   kept in save files (a load leaves the sprite empty), nor by pushscreen. */
int ONScripter::spgrabCommand()
{
    leaveTextDisplayMode();

    int no = script_h.readInt();
    int src = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : -1;
    int x = 0, y = 0, w = -1, h = -1;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA){
        x = script_h.readInt(); y = script_h.readInt();
        w = script_h.readInt(); h = script_h.readInt();
    }
    if (no < 0 || no >= MAX_SPRITE_NUM || src >= MAX_SPRITE_NUM || (src >= 0 && !sprite_info[src].image_surface)){
        utils::printError("spgrab: sprite %d or source %d can't be used\n", no, src);
        return RET_CONTINUE;
    }
    AnimationInfo *ai = &sprite_info[no];

    // The source picture, and where its (0, 0) is on the screen
    SDL_Surface *from;
    int ox = 0, oy = 0;
    if (src < 0){
        from = SDL_ConvertSurface( accumulation_surface, accumulation_surface->format, 0 );
        bool visible = ai->visible;
        ai->visible = false;
        SDL_Rect all = {0, 0, screen_width, screen_height};
        refreshSurface( from, &all, REFRESH_NORMAL_MODE );
        ai->visible = visible;
    }
    else{
        AnimationInfo *s = &sprite_info[src];
        int cw = s->image_surface->w / (s->num_of_cells > 0 ? s->num_of_cells : 1);
        SDL_Rect cell = {cw * s->current_cell, 0, cw, s->image_surface->h};
        from = SDL_CreateRGBSurfaceWithFormat( 0, cw, s->image_surface->h, 32, s->image_surface->format->format );
        if (from){
            SDL_SetSurfaceBlendMode( s->image_surface, SDL_BLENDMODE_NONE );
            SDL_BlitSurface( s->image_surface, &cell, from, NULL );
        }
        ox = s->orig_pos.x;
        oy = s->orig_pos.y;
    }
    if (!from) return RET_CONTINUE;
    // x, y, w, h are script coordinates, the pictures screen pixels; kept inside the picture
    int fw = from->w * screen_ratio2 / screen_ratio1, fh = from->h * screen_ratio2 / screen_ratio1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (w < 0 || w > fw - x) w = fw - x;
    if (h < 0 || h > fh - y) h = fh - y;
    if (w <= 0 || h <= 0){
        SDL_FreeSurface( from );
        utils::printError("spgrab: nothing to copy\n");
        return RET_CONTINUE;
    }

    if (ai->visible) dirty_rect.add( drawnRect( *ai ) );
    char name[64];
    snprintf( name, sizeof(name), ":c;>%d,%d,#000000", w, h );
    ai->visible = true;
    ai->setImageName( name );
    ai->orig_pos.x = ox + x;
    ai->orig_pos.y = oy + y;
    ai->scalePosXY( screen_ratio1, screen_ratio2 );
    ai->trans = -1;
    parseTaggedString( ai );
    setupAnimationInfo( ai );
    if (ai->image_surface){
        SDL_Rect r = {x * screen_ratio1 / screen_ratio2, y * screen_ratio1 / screen_ratio2,
                      ai->image_surface->w, ai->image_surface->h};
        SDL_SetSurfaceBlendMode( from, SDL_BLENDMODE_NONE );
        SDL_BlitSurface( from, &r, ai->image_surface, NULL );
        if (src < 0){ // the screen has no alpha of its own: opaque
            SDL_LockSurface( ai->image_surface );
            Uint32 amask = ai->image_surface->format->Amask;
            for (int j=0 ; j<ai->image_surface->h ; j++){
                Uint32 *row = (Uint32*)((Uint8*)ai->image_surface->pixels + j * ai->image_surface->pitch);
                for (int i=0 ; i<ai->image_surface->w ; i++) row[i] |= amask;
            }
            SDL_UnlockSurface( ai->image_surface );
        }
    }
    SDL_FreeSurface( from );
    // no file to load it from again: saves leave it out, and a later lsp of
    // the same rectangle mustn't take this surface for its own
    ai->deleteImageName();
    delete[] ai->surface_name;
    ai->surface_name = NULL;
    snprintf( name, sizeof(name), src < 0 ? "(spgrab)" : "(spgrab %d)", src ); // as the trace names it: the screen, or the sprite copied
    setStr( &ai->file_name, name );
    dirty_rect.add( drawnRect( *ai ) );

    return RET_CONTINUE;
}

/* spblend num,mode
 *   Draws sprite num (an lsp sprite) adding its colours to what is under
 *   it (mode 1), times its alpha, multiplying it by them (mode 2: white
 *   leaves it, black darkens it, times its alpha), subtracting its
 *   colours (mode 3), screening (mode 4: lightens, never past white) or
 *   blended as usual (mode 0): a white additive sprite at alpha a lightens the screen by a,
 *   as a flash of light. It
 *   keeps its place among the sprites (unlike lsp2add, drawn over all of
 *   them). It stays when lsp changes the image, is removed by csp and is
 *   kept in save files. lsp 2,":c;>800,600,#ffffff",0,0:spblend 2,1:
 *   spmotion 2,-5,0,0,0,100,100,0,128,0,80,0,0,100,100,0,0,0 flashes the
 *   screen five times. */
int ONScripter::spblendCommand()
{
    int no = script_h.readInt();
    int mode = script_h.readInt();
    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spblend: sprite %d out of range\n", no);
        return RET_CONTINUE;
    }
    AnimationInfo *ai = &sprite_info[no];
    static const int modes[] = {AnimationInfo::BLEND_NORMAL, AnimationInfo::BLEND_ADD, AnimationInfo::BLEND_MUL,
                                 AnimationInfo::BLEND_SUB, AnimationInfo::BLEND_SCREEN};
    ai->blending_mode = (mode >= 0 && mode <= 4) ? modes[mode] : AnimationInfo::BLEND_NORMAL;
    if (ai->visible) dirty_rect.add( drawnRect( *ai ) );

    return RET_CONTINUE;
}

/* spz num[,over]
 *   Draws lsp sprite num right over sprite over (just as if its number
 *   were between over and over - 1) instead of at its own number; "spz
 *   num" puts it back. For a picture that goes in front of some of the
 *   sprites numbered below it: "spz 70,59" draws sprite 70 over sprites
 *   59-69 and under 58. It stays when lsp changes the image, is removed by
 *   csp and is kept in save files and by pushscreen. */
int ONScripter::spzCommand()
{
    int no = script_h.readInt();
    int over = -1;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA) over = script_h.readInt();
    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spz: sprite %d out of range\n", no);
        return RET_CONTINUE;
    }
    if (over < 0 || over >= MAX_SPRITE_NUM || over == no) over = -1;
    sprite_info[no].z_over = over;
    if (sprite_info[no].visible) dirty_rect.add( drawnRect( sprite_info[no] ) );

    return RET_CONTINUE;
}

/* spclip num,x,y,w,h
 *   Draws lsp sprite num only inside the screen's rectangle x, y, w, h
 *   (its picture is cut there wherever the sprite moves: a window its
 *   children show through); "spclip num" draws it everywhere again. It
 *   stays when lsp changes the image, is removed by csp and is kept in
 *   save files and by pushscreen.
 *   lsp 20,"tape.png",262,-220:spclip 20,260,138,100,300 */
int ONScripter::spclipCommand()
{
    int no = script_h.readInt();
    int r[4] = {0, 0, 0, 0};
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA)
        for (int k=0 ; k<4 ; k++) r[k] = script_h.readInt();
    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spclip: sprite %d out of range\n", no);
        return RET_CONTINUE;
    }
    AnimationInfo &ai = sprite_info[no];
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );
    for (int k=0 ; k<4 ; k++) ai.clip_rect[k] = r[k];
    if (ai.clip_rect[2] <= 0 || ai.clip_rect[3] <= 0) ai.clip_rect[2] = ai.clip_rect[3] = 0;
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );

    return RET_CONTINUE;
}

/* CONscripter
 * sptrail num,passes,alpha,lag
 *   Draws passes (1-32, SPTRAIL_MAX) copies of lsp sprite num under it, each at
 *   alpha per mille (1-1000) of its alpha; copy i shows where the
 *   sprite's motion (spmotion) had it lag * i ms before, and
 *   once the motion ends the copies catch up with the sprite. A sprite
 *   that doesn't move shows no copies. "sptrail num" ends it. It stays
 *   when lsp changes the image, is removed by csp and is kept in save
 *   files (not by pushscreen, like the motions).
 *   lsp 10,"car.png",0,200:sptrail 10,3,500,40:spmotion 10,0,0,0,0,100,100,0,256,0,1000,800,0,100,100,0,256,0 */
int ONScripter::sptrailCommand()
{
    int no = script_h.readInt();
    int v[3] = {0, 0, 0};
    for (int k=0 ; k<3 && (script_h.getEndStatus() & ScriptHandler::END_COMMA) ; k++) v[k] = script_h.readInt();
    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("sptrail: sprite %d out of range\n", no);
        return RET_CONTINUE;
    }
    AnimationInfo &ai = sprite_info[no];
    dirty_rect.add( ai.trail_rect );
    ai.trail_rect.w = ai.trail_rect.h = 0;
    bool on = v[0] > 0 && v[1] > 0;
    ai.trail_passes = on ? (v[0] < SPTRAIL_MAX ? v[0] : SPTRAIL_MAX) : 0;
    ai.trail_alpha = on ? (v[1] < 1000 ? v[1] : 1000) : 0;
    ai.trail_lag = on && v[2] > 0 ? speedTime( v[2] ) : 0; // gamespeed
    if (!on) ai.trail_keys = 0;
    if (ai.visible && ai.image_surface && on){
        ai.trail_time = SDL_GetTicks();
        ai.trail_rect = trailCopies( ai, NULL, NULL );
        dirty_rect.add( ai.trail_rect );
    }

    return RET_CONTINUE;
}

/* CONscripter
 * spwave num,ms,period,amp0,amp1[,curve]
 *   Waves lsp sprite num for ms ms: each row of its picture is drawn
 *   trunc(sin(2 pi phase / 256) * amp) px to the right, phase being
 *   row * period / height (in 1/256 turns; period 256: one wave over the
 *   picture's height) plus one every 33 ms; amp goes from amp0 to amp1 px
 *   over the ms by curve (as spmotion's: 0 straight, 1-3 ease, spcurve's).
 *   Then it ends; "spwave num" ends it at once. Rows of a zoomed picture
 *   take the phase of the picture's row they show. Only rows are moved, no
 *   pixel is changed: a wave of amp 0 costs nothing. It stays when lsp
 *   changes the image, is removed by csp and is kept in save files (not by
 *   pushscreen). Nitroplus's DrawEffect waves (LowWave period 64,
 *   MiddleWave 512, HighWave 1024, SuperWave 4096).
 *   lsp 10,"bg.png",0,0:spwave 10,3000,512,0,20:print 1 */
int ONScripter::spwaveCommand()
{
    int no = script_h.readInt();
    int v[5] = {0, 0, 0, 0, 0};
    for (int k=0 ; k<5 && (script_h.getEndStatus() & ScriptHandler::END_COMMA) ; k++) v[k] = script_h.readInt();
    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spwave: sprite %d out of range\n", no);
        return RET_CONTINUE;
    }
    AnimationInfo &ai = sprite_info[no];
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );
    ai.wave_ms = v[0] > 0 ? speedTime( v[0] ) : 0; // gamespeed
    ai.wave_start = SDL_GetTicks();
    ai.wave_period = v[1];
    ai.wave_amp0 = v[2];
    ai.wave_amp1 = v[3];
    ai.wave_curve = v[4];
    ai.wave_tick = 0;
    ai.wave_amp = ai.wave_ms > 0 ? v[2] : 0;
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );

    return RET_CONTINUE;
}

// spwave: steps sprite ai's wave to current_time; true when what it draws changed.
bool ONScripter::stepSpriteWave( AnimationInfo &ai, int current_time )
{
    if (ai.wave_ms <= 0) return false;
    int t = current_time - ai.wave_start;
    if (t >= ai.wave_ms){
        bool shown = ai.wave_amp != 0;
        ai.wave_ms = ai.wave_amp = ai.wave_tick = 0;
        return shown;
    }
    if (t < 0) t = 0;
    int amp = ai.wave_amp0 + (int)((long long)(ai.wave_amp1 - ai.wave_amp0) * curveAt( ai.wave_curve, (int)((long long)t * 1024 / ai.wave_ms) ) / 1024);
    int step = speedTime( 33 );
    int tick = t / (step > 0 ? step : 1);
    if (amp == ai.wave_amp && (tick == ai.wave_tick || amp == 0)) return false;
    ai.wave_amp = amp;
    ai.wave_tick = tick;
    return true;
}

// spwave: how far right (px) row (0 at the top, of drawn_h drawn rows) of
// sprite ai is drawn, as Nitroplus's wave (装甲悪鬼村正's FUN_00430de0):
// the picture's row phase plus the step, through a 256-entry sine table.
int ONScripter::waveShift( const AnimationInfo &ai, int row, int drawn_h )
{
    static float sine[256];
    static bool made = false;
    if (!made){
        for (int i=0 ; i<256 ; i++) sine[i] = (float)sin( i * M_PI / 128 );
        made = true;
    }
    int h = ai.pos.h > 0 ? ai.pos.h : 1;
    unsigned src = drawn_h > 0 ? (unsigned)((long long)row * h / drawn_h) : (unsigned)row;
    unsigned base = (unsigned)(((unsigned long long)src * (((unsigned)ai.wave_period << 16) / (unsigned)h)) >> 16);
    return (int)((double)sine[(base + ai.wave_tick) & 255] * ai.wave_amp);
}

/* spovertext num[,on]
 *   Draws lsp sprite num over the text window and its text (and the bars),
 *   under the sprites windowback and humanz put over the window; "spovertext
 *   num,0" (or spz num) puts it back at its own number. Several are drawn
 *   in their numbers' order. Like spz it stays when lsp changes the image,
 *   is removed by csp and is kept in save files and by pushscreen. */
int ONScripter::spovertextCommand()
{
    int no = script_h.readInt();
    int on = 1;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA) on = script_h.readInt();
    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spovertext: sprite %d out of range\n", no);
        return RET_CONTINUE;
    }
    sprite_info[no].z_over = on ? Z_OVER_TEXT : -1;
    if (sprite_info[no].visible) dirty_rect.add( drawnRect( sprite_info[no] ) );

    return RET_CONTINUE;
}

void ONScripter::drawOverTextSprites( SDL_Surface *surface, SDL_Rect &clip )
{
    if (all_sprite_hide_flag) return;
    for (int i=MAX_SPRITE_NUM-1 ; i>=0 ; i--){
        AnimationInfo &ai = sprite_info[i];
        if (ai.z_over == Z_OVER_TEXT && ai.image_surface && ai.visible){
            if (ai.trail_passes > 0) trailCopies( ai, surface, &clip );
            drawTaggedSurface( surface, &ai, clip );
        }
    }
}

void ONScripter::drawSpriteRange( SDL_Surface *surface, SDL_Rect &clip, int hi, int lo )
{
    bool moved = false;
    for (int i=0 ; i<MAX_SPRITE_NUM && !moved ; i++)
        if (sprite_info[i].z_over >= 0) moved = true;
    for (int i=hi ; i>=lo ; i--){
        AnimationInfo &ai = sprite_info[i];
        if (ai.image_surface && ai.visible && (!moved || ai.z_over < 0)){
            if (ai.trail_passes > 0) trailCopies( ai, surface, &clip );
            drawTaggedSurface( surface, &ai, clip );
        }
        if (moved){
            for (int k=MAX_SPRITE_NUM-1 ; k>=0 ; k--){
                AnimationInfo &over = sprite_info[k];
                if (over.z_over == i && over.image_surface && over.visible){
                    if (over.trail_passes > 0) trailCopies( over, surface, &clip );
                    drawTaggedSurface( surface, &over, clip );
                }
            }
        }
        // screenfilter's z: the sprites drawn after i aren't filtered
        applyScreenFilters( surface, clip, i );
    }
}

// A motion that keeps its last key (loop 2) and still runs ends there now:
// before spmotion, lsp, msp or amsp sets the sprite anew.
void ONScripter::endKeptMotion( AnimationInfo &ai )
{
    if (ai.motion_keys == 0 || !ai.motion_keep || ai.motion_loop) return;
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );
    stepSpriteMotion( ai, ai.motion_start + ai.motion_key[ai.motion_keys-1][0] );
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );
}

// Sets a motion's offset, zoom, angle and alpha for the time; ends a motion
// that ran once (one that keeps its last key moves the sprite there).
void ONScripter::stepSpriteMotion( AnimationInfo &ai, int current_time )
{
    ai.trail_time = current_time;
    if (ai.motion_keys == 0) return;
    int last = ai.motion_key[ai.motion_keys-1][0];
    int t = current_time - ai.motion_start;
    if (ai.motion_loop && last > 0) t = ai.loopTime( t );
    else if (ai.motion_passes > 1 && last > 0 && t < last * ai.motion_passes) t %= last;
    else if (t >= last){
        if (ai.motion_keep){
            int *k = ai.motion_key[ai.motion_keys-1];
            ai.orig_pos.x += k[1];
            ai.orig_pos.y += k[2];
            ai.scalePosXY( screen_ratio1, screen_ratio2 );
            ai.zoom_x = ai.zoom_x * k[3] / 100;
            ai.zoom_y = ai.zoom_y * k[6] / 100;
            ai.zoom_rot += k[4];
            if (k[5] < 256) ai.trans = (ai.trans < 0 ? 255 : ai.trans) * k[5] / 256;
        }
        // jumped to its end before its time: spmotionwait cut short, --auto-play, a kept motion set anew
        traceMotion( ai, current_time > (int)SDL_GetTicks() ? "cut short" : "ran out" );
        // (sptrail: the copies follow it on until they reach the sprite)
        ai.trail_keys = ai.trail_passes > 0 ? ai.motion_keys : 0;
        if (ai.motion_leave){ // loop 3: gone, as csp
            ai.remove();
            ai.visible = false;
        }
        ai.stopMotion();
        return;
    }
    int v[7];
    motionAt( ai, ai.motion_keys, t, v );
    ai.motion_dx = v[1] * screen_ratio1 / screen_ratio2;
    ai.motion_dy = v[2] * screen_ratio1 / screen_ratio2;
    ai.motion_zoom = v[3];
    ai.motion_zoom_y = v[6];
    ai.motion_rot = v[4];
    ai.motion_alpha = v[5];
}

// Steps the sprites' fades and motions; draws what changed
// (draw false: only says whether something did, for an effect running).
// The values (0, x, y, zoom x, angle, alpha, zoom y) of the first `keys`
// motion keys at t ms from the start, each key reached along its curve.
void ONScripter::motionAt( const AnimationInfo &ai, int keys, int t, int v[7] )
{
    int i = 0;
    while (i + 1 < keys && ai.motion_key[i+1][0] <= t) i++;
    const int *a = ai.motion_key[i];
    v[0] = 0;
    for (int j=1 ; j<7 ; j++) v[j] = a[j];
    if (i + 1 < keys && t > a[0]){
        const int *b = ai.motion_key[i+1];
        int span = b[0] - a[0];
        if (span > 0){
            int p = curveAt( b[7], (int)((long long)(t - a[0]) * 1024 / span) );
            for (int j=1 ; j<7 ; j++) v[j] += (int)((long long)(b[j] - a[j]) * p / 1024);
        }
    }
}

// sptrail: copy i's motion values (as motion_dx, motion_dy, motion_zoom,
// motion_rot, motion_alpha, motion_zoom_y in v[1..6]): the sprite's
// i * lag ms before the last step. False when it has none (no motion, no
// trail).
bool ONScripter::trailCopyAt( const AnimationInfo &ai, int i, int v[7] )
{
    int keys = ai.motion_keys > 0 ? ai.motion_keys : ai.trail_keys;
    if (ai.trail_passes == 0 || keys == 0) return false;
    const int *k = ai.motion_key[keys-1];
    int last = k[0];
    int t = ai.trail_time - ai.motion_start - i * ai.trail_lag;
    if (t < 0) t = 0; // before the start: on the first key
    if (ai.motion_loop && last > 0) t = ai.loopTime( t );
    else if (ai.motion_passes > 1 && last > 0 && t < last * ai.motion_passes) t %= last;
    else if (t >= last){
        // past the end: where the sprite is now
        v[1] = v[2] = v[4] = 0;
        v[3] = v[6] = 100;
        v[5] = 256;
        return true;
    }
    motionAt( ai, keys, t, v );
    if (ai.motion_keys == 0 && ai.motion_keep){
        // the ended motion's last key is now the sprite's own place, zoom,
        // angle and alpha (stepSpriteMotion): relative to it
        v[1] -= k[1];
        v[2] -= k[2];
        if (k[3] > 0) v[3] = v[3] * 100 / k[3];
        if (k[6] > 0) v[6] = v[6] * 100 / k[6];
        v[4] -= k[4];
        if (k[5] > 0 && k[5] < 256) v[5] = v[5] * 256 / k[5];
        if (v[5] > 256) v[5] = 256; // (can't be drawn more opaque than the sprite)
    }
    v[1] = v[1] * screen_ratio1 / screen_ratio2;
    v[2] = v[2] * screen_ratio1 / screen_ratio2;
    return true;
}

// sptrail: when the copies of an ended motion have all reached the sprite
// (ticks from the motion's start).
static int trailEnd( const AnimationInfo &ai )
{
    int last = ai.motion_key[ai.trail_keys-1][0];
    return last * (ai.motion_passes > 1 ? ai.motion_passes : 1) + ai.trail_passes * ai.trail_lag;
}

// sptrail: draws the copies of a sprite that differ from it on surface
// (none when surface is NULL), farthest first; returns the area they
// cover.
SDL_Rect ONScripter::trailCopies( AnimationInfo &ai, SDL_Surface *surface, SDL_Rect *clip )
{
    DirtyRect area;
    area.setDimension( screen_width, screen_height );
    int saved[7] = {0, ai.motion_dx, ai.motion_dy, ai.motion_zoom, ai.motion_rot, ai.motion_alpha, ai.motion_zoom_y};
    for (int i=ai.trail_passes ; i>=1 ; i--){
        int v[7];
        if (!trailCopyAt( ai, i, v )) break;
        if (memcmp( v + 1, saved + 1, 6 * sizeof(int) ) == 0) continue;
        ai.motion_dx = v[1];
        ai.motion_dy = v[2];
        ai.motion_zoom = v[3];
        ai.motion_rot = v[4];
        ai.motion_alpha = v[5] * ai.trail_alpha / 1000;
        ai.motion_zoom_y = v[6];
        SDL_Rect r = zoomedRect( ai );
        r.x += ai.motion_dx;
        r.y += ai.motion_dy;
        area.add( r );
        if (surface) drawTaggedSurface( surface, &ai, *clip );
    }
    ai.motion_dx = saved[1];
    ai.motion_dy = saved[2];
    ai.motion_zoom = saved[3];
    ai.motion_rot = saved[4];
    ai.motion_alpha = saved[5];
    ai.motion_zoom_y = saved[6];
    return area.bounding_box;
}

bool ONScripter::proceedSpriteMotions( int current_time, bool draw )
{
    // Every sprite that changed is drawn in one flush: a flush per sprite
    // takes long enough, with the software renderer, that sprites fading
    // in together appeared one after another.
    DirtyRect changed;
    changed.setDimension(screen_width, screen_height);
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo &ai = sprite_info[i];
        if (ai.wave_ms > 0){ // spwave
            SDL_Rect r = drawnRect( ai );
            if (stepSpriteWave( ai, current_time ) && ai.visible && ai.image_surface){
                changed.add( r );
                changed.add( drawnRect( ai ) );
            }
        }
        if ((ai.motion_keys == 0 && ai.trail_keys == 0) || ai.motion_next > current_time) continue;
        // After a long stop (a menu, a load) go on from now.
        if (current_time - ai.motion_next > 1000){
            ai.motion_start += current_time - ai.motion_next;
            ai.motion_next = current_time;
        }
        while (ai.motion_next <= current_time) ai.motion_next += 16;
        int dx = ai.motion_dx, dy = ai.motion_dy, zoom = ai.motion_zoom, zoom_y = ai.motion_zoom_y, rot = ai.motion_rot, alpha = ai.motion_alpha;
        SDL_Rect r = drawnRect( ai );
        bool shown = ai.visible && ai.image_surface;
        stepSpriteMotion( ai, current_time );
        if (ai.trail_passes > 0){
            // sptrail: the copies, until they reach the sprite
            if (ai.motion_keys == 0 && ai.trail_keys > 0 && current_time - ai.motion_start >= trailEnd( ai ))
                ai.trail_keys = 0;
            SDL_Rect t = {0, 0, 0, 0};
            if (ai.visible && ai.image_surface) t = trailCopies( ai, NULL, NULL );
            if (ai.visible){
                changed.add( ai.trail_rect );
                changed.add( t );
            }
            ai.trail_rect = t;
        }
        if (!ai.visible || !ai.image_surface){
            if (shown) changed.add( r ); // loop 3 removed it
            continue;
        }
        if (dx == ai.motion_dx && dy == ai.motion_dy && zoom == ai.motion_zoom && zoom_y == ai.motion_zoom_y && rot == ai.motion_rot && alpha == ai.motion_alpha && ai.motion_keys > 0) continue;
        SDL_Rect r2 = drawnRect( ai );
        int x1 = (r.x < r2.x) ? r.x : r2.x, y1 = (r.y < r2.y) ? r.y : r2.y;
        int x2 = (r.x + r.w > r2.x + r2.w) ? r.x + r.w : r2.x + r2.w;
        int y2 = (r.y + r.h > r2.y + r2.h) ? r.y + r.h : r2.y + r2.h;
        SDL_Rect both = {x1, y1, x2 - x1, y2 - y1};
        changed.add( both );
    }
    if (changed.bounding_box.w <= 0 || changed.bounding_box.h <= 0) return false;
    if (draw)
        flushDirect( changed.bounding_box, refreshMode() | (draw_cursor_flag ? REFRESH_CURSOR_MODE : 0) );
    return true;
}

// Where an lsp sprite's picture is drawn: zoomed (spzoom), moved (spmotion).
SDL_Rect ONScripter::drawnRect( const AnimationInfo &ai )
{
    SDL_Rect r = zoomedRect( ai );
    r.x += ai.motion_dx;
    r.y += ai.motion_dy;
    if (ai.trail_rect.w > 0 && ai.trail_rect.h > 0){ // and its sptrail copies
        int x2 = r.x + r.w > ai.trail_rect.x + ai.trail_rect.w ? r.x + r.w : ai.trail_rect.x + ai.trail_rect.w;
        int y2 = r.y + r.h > ai.trail_rect.y + ai.trail_rect.h ? r.y + r.h : ai.trail_rect.y + ai.trail_rect.h;
        if (ai.trail_rect.x < r.x) r.x = ai.trail_rect.x;
        if (ai.trail_rect.y < r.y) r.y = ai.trail_rect.y;
        r.w = x2 - r.x;
        r.h = y2 - r.y;
    }
    if (ai.wave_ms > 0){ // and its spwave rows, as far as they go
        int a = abs(ai.wave_amp0) > abs(ai.wave_amp1) ? abs(ai.wave_amp0) : abs(ai.wave_amp1);
        r.x -= a + 1;
        r.w += 2 * a + 2;
    }
    return r;
}

// A visible sprite is zoomed or moving: it covers more than its place.
bool ONScripter::spriteZoomOrMotion()
{
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        const AnimationInfo &ai = sprite_info[i];
        if (ai.visible && ai.image_surface && (ai.motion_keys > 0 || ai.trail_keys > 0 || ai.wave_ms > 0 || ai.zoom_x != 100 || ai.zoom_y != 100 || ai.zoom_rot != 0)) return true;
    }
    return false;
}

// The ticks of the next motion step, 0 when no sprite moves.
int ONScripter::nextSpriteMotionTime()
{
    int min = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo &ai = sprite_info[i];
        if (ai.wave_ms > 0 && ai.visible){ // spwave: its next step, or its end
            int step = speedTime( 33 );
            int next = ai.wave_start + (ai.wave_tick + 1) * (step > 0 ? step : 1);
            if (next > ai.wave_start + ai.wave_ms) next = ai.wave_start + ai.wave_ms;
            if (min == 0 || min > next) min = next;
        }
        if (ai.motion_keys == 0 && ai.trail_keys == 0) continue;
        if (min == 0 || min > ai.motion_next) min = ai.motion_next;
    }
    return min;
}

// The box a sprite's picture covers, zoomed (spzoom, a motion's zoom) and
// turned (spzoom's angle, a motion's) about its centre.
SDL_Rect ONScripter::zoomedRect( const AnimationInfo &ai )
{
    SDL_Rect r = ai.pos;
    if (ai.zoom_x == 100 && ai.zoom_y == 100 && ai.motion_zoom == 100 && ai.motion_zoom_y == 100 && ai.motion_rot == 0 && ai.zoom_rot == 0) return r;
    int w = ai.pos.w * abs(ai.zoom_x) / 100 * abs(ai.motion_zoom) / 100;
    int h = ai.pos.h * abs(ai.zoom_y) / 100 * abs(ai.motion_zoom_y) / 100;
    if ((ai.zoom_rot + ai.motion_rot) % 360 != 0) w = h = (int)ceil( sqrt( (double)w * w + (double)h * h ) );
    r.x = ai.pos.x + ai.pos.w/2 - w/2 - 1;
    r.y = ai.pos.y + ai.pos.h/2 - h/2 - 1;
    r.w = w + 2;
    r.h = h + 2;
    return r;
}

// Draws a zoomed lsp sprite with lsp2's scaling, about the centre of its
// place (poly_rect, its top left on the screen).
void ONScripter::drawZoomedSurface( SDL_Surface *dst_surface, AnimationInfo *anim, SDL_Rect &poly_rect, SDL_Rect &clip )
{
    if (anim->zoom_x == 0 || anim->zoom_y == 0 || anim->motion_zoom == 0 || anim->motion_zoom_y == 0) return;
    SDL_Rect pos = anim->pos, affine_pos = anim->affine_pos, bounding_rect = anim->bounding_rect;
    int scale_x = anim->scale_x, scale_y = anim->scale_y, rot = anim->rot;

    anim->pos.x = poly_rect.x + pos.w/2;
    anim->pos.y = poly_rect.y + pos.h/2;
    anim->affine_pos.x = 0; // blendOnSurface2 adds the current cell's offset
    anim->affine_pos.y = 0;
    anim->affine_pos.w = pos.w;
    anim->affine_pos.h = pos.h;
    anim->scale_x = anim->zoom_x * anim->motion_zoom / 100;
    anim->scale_y = anim->zoom_y * anim->motion_zoom_y / 100;
    anim->rot = anim->zoom_rot + anim->motion_rot;
    anim->calcAffineMatrix();
    anim->blendOnSurface2( dst_surface, anim->pos.x, anim->pos.y, clip, anim->trans );

    anim->pos = pos;
    anim->affine_pos = affine_pos;
    anim->bounding_rect = bounding_rect;
    anim->scale_x = scale_x;
    anim->scale_y = scale_y;
    anim->rot = rot;
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
    else if (f.blend < AnimationInfo::FILTER_BLEND_COPY || f.blend > AnimationInfo::FILTER_BLEND_SCREEN){
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

/* screenshake slot[, time, interval, mode, speed_x, speed_y, x0, y0, x1, y1[, end]]
 *   Shakes the picture behind the text window (what screenfilter
 *   filters) until the slot is cleared, or with end (> 0) for end ms,
 *   after which the slot clears itself; without waiting: the script and
 *   the text go on while it shakes. Every interval ms the picture is drawn
 *   moved by an offset whose largest size grows from x0,y0 to x1,y1
 *   pixels over time ms, then stays. Mode 0 swings each axis on a sine
 *   wave that advances speed/65536 of a turn per ms (speed_x for x,
 *   speed_y for y); mode 1 jumps along a fixed path of 7 points, moving on
 *   when speed_x * ms reaches 65536. Where the picture moved away, what
 *   was there stays (it is drawn over itself).
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
/* dwavefadein ms
 *   The next dwave, dwaveloop, dwaveplay or dwaveplayloop fades its sound
 *   in over ms milliseconds (0: at once, as before).
 *       dwavefadein 2000:dwaveloop 1,"se\rain.ogg"
 *   (Fading out is chvolfade's stop.) */
int ONScripter::dwavefadeinCommand()
{
    dwave_fade_in_ms = script_h.readInt();
    if (dwave_fade_in_ms < 0) dwave_fade_in_ms = 0;
    return RET_CONTINUE;
}

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

// A skipped movie stops the sounds started with it (since the last wait):
// a sound cut into the movie would play on over what follows.
void ONScripter::stopFreshChannels()
{
    for (int ch = 0; ch < ONS_MIX_CHANNELS; ch++){
        if (!(fresh_channels >> ch & 1) || !wave_sample[ch]) continue;
        trace_se_loop[ch] = false;
        cutChannel( ch );
        Mix_FreeChunk( wave_sample[ch] );
        wave_sample[ch] = NULL;
    }
    fresh_channels = 0;
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

/* chvolfade ch,vol[,ms[,stop]]
 *   Sets the volume of dwave channel ch's sound to vol percent (0-100)
 *   of the sound effect or voice setting, over ms milliseconds (default
 *   0, at once) while the script goes on: a fade from where it is now.
 *   The sound keeps playing at 0. The volume stays with the channel for
 *   the sounds played on it next, so set it before dwave to start a sound
 *   quieter; reset puts every channel back to 100. Kept in a save.
 *       chvolfade 3,30:dwaveloop 3,"se\rain.ogg"   (at 30 %)
 *       chvolfade 3,100,2000                       (back up in 2 s)
 *   ch -1 is the music (bgm, mp3): vol percent of the music setting, for
 *   the next bgm too; bgmfadein and bgmfadeout fade to and from it.
 *   stop 1 (dwave channels): the sound fades out over ms and stops, and
 *   the channel's volume stays as it was for its next sounds (vol is
 *   ignored); a looping sound fading out isn't kept in a save; 0 ms stops
 *   it as dwavestop does. "chvolfade 2,0,1500,1" fades the rain out. */
int ONScripter::chvolfadeCommand()
{
    int ch = script_h.readInt();
    int vol = script_h.readInt();
    int ms = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : 0;
    bool stop = (script_h.getEndStatus() & ScriptHandler::END_COMMA) && script_h.readInt() == 1;
    traceChannel( ch );
    if (ch == -1){ // the music
        if (vol < 0) vol = 0; else if (vol > 100) vol = 100;
        bgm_script_from = bgmScriptVolume();
        bgm_script_start = SDL_GetTicks();
        bgm_script_to = vol;
        bgm_script_ms = ms > 0 ? ms : 0;
        traceRamp( 0, -1, vol, bgm_script_ms );
        applyMusicVolume();
        return RET_CONTINUE;
    }
    if      (ch < 0) ch = 0;
    else if (ch >= ONS_MIX_CHANNELS) ch = ONS_MIX_CHANNELS-1;
    if (stop){
        if (!wave_sample[ch]) return RET_CONTINUE;
        trace_se_loop[ch] = false; // stopping: not for the save
        // SDL_mixer won't fade a sound at volume 0 (a fade-in just started):
        // nothing is heard yet, so stop it as dwavestop does
        if (ms <= 0 || !audio_open_flag || Mix_Paused( ch ) || Mix_FadeOutChannel( ch, ms ) == 0){
            cutChannel( ch );
            Mix_FreeChunk( wave_sample[ch] );
            wave_sample[ch] = NULL;
        }
        return RET_CONTINUE;
    }
    if (vol < 0) vol = 0; else if (vol > 100) vol = 100;
    Uint32 now = SDL_GetTicks();
    int from = scriptVolumeAt( track_gain[ch].from, track_gain[ch].to, track_gain[ch].ms, track_gain[ch].start, now );
    // The mixer reads these as they change: no ramp until the last one
    track_gain[ch].ms = 0;
    track_gain[ch].from = from;
    track_gain[ch].start = now;
    track_gain[ch].to = vol;
    track_gain[ch].ms = ms > 0 ? ms : 0;
    traceRamp( 0, ch, vol, track_gain[ch].ms );
    return RET_CONTINUE;
}

/* chpan ch,pan[,ms]
 *   Pans dwave channel ch's sound: pan -1000 (only the left speaker)
 *   .. 0 (centre, default) .. 1000 (only the right), lowering the other
 *   side; over ms milliseconds (default 0, at once) while the script goes
 *   on, from where it is now. Like chvolfade's volume it stays with the
 *   channel for its next sounds, so set it before dwave; reset centres
 *   every channel. Kept in a save. Stereo output only.
 *       chpan 2,-1000:dwave 2,"se\shot.ogg"   (from the left)
 *       chpan 2,1000,1500                     (to the right in 1.5 s)
 *   Only dwave channels: SDL_mixer can't pan its music stream alone. */
int ONScripter::chpanCommand()
{
    int ch = script_h.readInt();
    int pan = script_h.readInt();
    int ms = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : 0;
    traceChannel( ch );
    if (ch < 0 || ch >= ONS_MIX_CHANNELS){
        utils::printError("chpan: channel %d out of range\n", ch);
        return RET_CONTINUE;
    }
    if (pan < -1000) pan = -1000; else if (pan > 1000) pan = 1000;
    int from = trackPan( ch );
    track_pan[ch].ms = 0; // the mixer reads these as they change
    track_pan[ch].from = from;
    track_pan[ch].start = SDL_GetTicks();
    track_pan[ch].to = pan;
    track_pan[ch].ms = ms > 0 ? ms : 0;
    traceRamp( 1, ch, pan, track_pan[ch].ms );
    return RET_CONTINUE;
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
 *     chosen. 0 (default): a select ends them, as in NScripter.
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
    else if (!strcmp( key, "selectkeep" )) select_keep = v != 0;
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
    else if (!strcmp( key, "selectkeep" )) v = select_keep;
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
    return (int)((long long)ms * 100 / game_speed);
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

// chvolfade back to 100, at once (reset, startup).
void ONScripter::resetScriptVolumes()
{
    for (int i=0 ; i<ONS_MIX_CHANNELS ; i++){
        track_gain[i].ms = 0;
        track_gain[i].from = track_gain[i].to = 100;
        track_gain[i].last = 4096;
        track_pan[i].ms = 0;
        track_pan[i].from = track_pan[i].to = track_pan[i].last = 0;
    }
    bgm_script_from = bgm_script_to = 100;
    bgm_script_ms = 0;
}

// chvolfade -1's volume now, percent.
int ONScripter::bgmScriptVolume()
{
    return scriptVolumeAt( bgm_script_from, bgm_script_to, bgm_script_ms, bgm_script_start, SDL_GetTicks() );
}

// The music's volume, percent: the setting times chvolfade -1's.
int ONScripter::musicVolume()
{
    if (sound_mute & SOUND_MUTE_BGM) return 0;
    return music_volume * bgmScriptVolume() / 100;
}

// Sets the music's volume, unless bgmfadein or bgmfadeout is fading it
// (their steps use musicVolume()).
void ONScripter::applyMusicVolume()
{
    if (timer_bgmfade_id) return;
    Mix_VolumeMusic( musicVolume() * MIX_MAX_VOLUME / 100 );
    if (wave_sample[MIX_BGM_CHANNEL]) Mix_Volume( MIX_BGM_CHANNEL, musicVolume() * MIX_MAX_VOLUME / 100 );
}

/* bgmlater ms,"file"[,loop]
 *   Plays file as the music ms milliseconds from now, while the script
 *   goes on (through waits, text and click waits): as bgm, or as bgmonce
 *   with loop 0. A music command before then (bgm, bgmonce, mp3..., play,
 *   bgmstop, playstop), reset and a load cancel it. Kept in a save (the
 *   time left).
 *   A piece whose loop starts a set time after its intro, or music that
 *   comes in some seconds after a line:
 *       bgmonce "bgm\intro.ogg":bgmlater 7720,"bgm\loop.ogg" */
int ONScripter::autooffCommand()
{
    automode_flag = false;
    webStateHook();
    return RET_CONTINUE;
}

int ONScripter::bgmlaterCommand()
{
    int ms = script_h.readInt();
    const char *buf = script_h.readStr();
    setStr( &bgm_later_name, buf[0] ? buf : NULL ); // before readInt reuses buf
    bool loop = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() != 0 : true;
    bgm_later_loop = loop;
    bgm_later_time = SDL_GetTicks() + (ms > 0 ? ms : 0);
    return RET_CONTINUE;
}

/* getbgmpos %v
 *   Sets %v to how far (ms) the music playing now is through its current
 *   pass (a looped piece counts again from its start each time round); 0
 *   with no music, or music played as a sound (a .wav). While a bgmlater
 *   music is still to start, minus the ms until it starts (what plays
 *   now, fading out or not, is about to go).
 *   The rest of a song for a roll timed to its end:
 *       getbgmpos %1:mov %2,252000-%1:bgmlater %2,"bgm\next.ogg",0 */
int ONScripter::getbgmposCommand()
{
    script_h.readInt();
    int ms = 0;
#ifdef SDL_MIXER_VERSION_ATLEAST
#if SDL_MIXER_VERSION_ATLEAST(2,6,0) // (older mixers: 0)
    if (music_info && Mix_PlayingMusic()){
        double s = Mix_GetMusicPosition( music_info );
        if (s > 0) ms = (int)(s * 1000);
    }
#endif
#endif
    if (bgm_later_name){
        int left = (int)(bgm_later_time - SDL_GetTicks());
        ms = left > 0 ? -left : -1;
    }
    script_h.setInt( &script_h.current_variable, ms );
    return RET_CONTINUE;
}

void ONScripter::cancelLaterBGM()
{
    setStr( &bgm_later_name, NULL );
}

// The ticks of the next step of chvolfade -1's ramp or of bgmlater's start,
// 0 when there is none. (chvolfade's ramps are stepped by the mixer.)
int ONScripter::nextSoundFadeTime()
{
    int t = 0;
    if (bgm_script_ms > 0 && SDL_GetTicks() - bgm_script_start < (Uint32)bgm_script_ms + 20)
        t = SDL_GetTicks() + 20;
    if (bgm_later_name && (t == 0 || (int)bgm_later_time < t))
        t = bgm_later_time > SDL_GetTicks() ? bgm_later_time : SDL_GetTicks() + 1;
    return t;
}

void ONScripter::proceedSoundFades( int current_time )
{
    if (bgm_later_name && SDL_GetTicks() >= bgm_later_time){
        // as mp3Command plays it
        char *name = bgm_later_name;
        bgm_later_name = NULL;
        stopBGM( false );
        mp3save_flag = music_play_loop_flag = bgm_later_loop;
        music_loopback_offset = 0.0;
        music_file_name = name;
        playSound( music_file_name, SOUND_MUSIC | SOUND_MIDI | SOUND_CHUNK,
                   music_play_loop_flag, MIX_BGM_CHANNEL );
    }
    if (bgm_script_ms <= 0) return;
    applyMusicVolume();
    if (SDL_GetTicks() - bgm_script_start >= (Uint32)bgm_script_ms){
        bgm_script_from = bgm_script_to;
        bgm_script_ms = 0;
    }
}

int ONScripter::windowtransCommand()
{
    int n = script_h.readInt();
    window_trans = n < 0 ? 0 : n > 255 ? 255 : n;
    dirty_rect.add( textWindowRect() );
    return RET_CONTINUE;
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
        // param: time, interval, mode, speed x, y, x0, y0, x1, y1
        e.range_x = (p[5] << 16) + (int)((((long long)(p[7] - p[5])) << 16) * t >> 8);
        e.range_y = (p[6] << 16) + (int)((((long long)(p[8] - p[6])) << 16) * t >> 8);
        e.phase_x += (unsigned int)(p[3] * interval);
        if (p[2] == 1){
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

    // Version 23: the sprite waves (spwave): the count, then per sprite its
    // number, ms (at game speed), period, amps, curve and the ms gone.
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].wave_ms > 0) num++;
    writeInt( num, output_flag );
    now = SDL_GetTicks();
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo &ai = sprite_info[i];
        if (ai.wave_ms <= 0) continue;
        writeInt( i, output_flag );
        writeInt( ai.wave_ms, output_flag );
        writeInt( ai.wave_period, output_flag );
        writeInt( ai.wave_amp0, output_flag );
        writeInt( ai.wave_amp1, output_flag );
        writeInt( ai.wave_curve, output_flag );
        writeInt( now - ai.wave_start, output_flag );
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

// Stopping a sound queues an event saying its channel finished
// (ONS_CHUNK_EVENT), which frees whatever that channel plays when the event
// comes. A load stops every sound and starts the saved ones again at once
// (waveloop, loopbgm, looping dwave), so those events would cut the new
// ones: drop them.
void ONScripter::dropStoppedSoundEvents()
{
    removeEvent( SDL_USEREVENT+4 ); // ONS_CHUNK_EVENT
}

/* The loop inside a sound file, in sample frames at its rate: LOOPSTART
 * and LOOPEND (or LOOPLENGTH) of an Ogg Vorbis file's comments, or the
 * first loop of a WAV's smpl chunk (decodeOpus writes Opus tags there).
 * SDL_mixer loops music by these itself, but a chunk (dwaveloop) only
 * whole: the next playChannel loops inside it (setChunkLoop). */
// The loop of the next sound playChannel starts (bytes; end 0: the whole).
static Uint32 next_loop_start = 0, next_loop_end = 0;
static bool soundLoop( const unsigned char *buf, unsigned long len, Sint64 *start, Sint64 *end, int *rate )
{
    auto le32 = [&](unsigned long at) -> Uint32 {
        return at + 4 <= len ? buf[at] | buf[at+1] << 8 | buf[at+2] << 16 | (Uint32)buf[at+3] << 24 : 0;
    };
    *start = *end = -1;
    if (len > 12 && !memcmp( buf, "RIFF", 4 ) && !memcmp( buf + 8, "WAVE", 4 )){
        for (unsigned long at = 12; at + 8 <= len; at += 8 + ((le32(at+4) + 1) & ~1u)){
            if (!memcmp( buf + at, "fmt ", 4 )) *rate = (int)le32(at + 12);
            if (!memcmp( buf + at, "smpl", 4 ) && le32(at + 8 + 28) > 0){
                *start = le32(at + 8 + 36 + 8);
                *end = (Sint64)le32(at + 8 + 36 + 12) + 1;
            }
        }
        return *start >= 0 && *end > *start && *rate > 0;
    }
    if (len < 4 || memcmp( buf, "OggS", 4 )) return false;
    const unsigned char *id = NULL, *cm = NULL;
    for (unsigned long i = 0; i + 7 < len && i < 65536 && !cm; i++){
        if (!memcmp( buf + i, "\x01vorbis", 7 )) id = buf + i;
        if (!memcmp( buf + i, "\x03vorbis", 7 )) cm = buf + i;
    }
    if (!id || !cm) return false;
    *rate = (int)le32(id - buf + 12);
    // (A comment header split over pages isn't read: tags sit early.)
    unsigned long at = cm - buf + 7;
    at += 4 + le32(at);
    Uint32 count = le32(at);
    at += 4;
    Sint64 length = -1;
    for (Uint32 i = 0; i < count && at + 4 <= len; i++){
        Uint32 n = le32(at);
        if (at + 4 + n > len) break;
        std::string tag( (const char*)buf + at + 4, n );
        for (auto &c : tag) if (c == '=') break; else c = toupper(c);
        if (!tag.compare( 0, 10, "LOOPSTART=" )) *start = atoll( tag.c_str() + 10 );
        if (!tag.compare( 0, 8, "LOOPEND=" )) *end = atoll( tag.c_str() + 8 );
        if (!tag.compare( 0, 11, "LOOPLENGTH=" )) length = atoll( tag.c_str() + 11 );
        at += 4 + n;
    }
    if (*start >= 0 && *end < 0 && length > 0) *end = *start + length;
    return *start >= 0 && *end > *start && *rate > 0;
}

void ONScripter::setChunkLoop( const unsigned char *buf, unsigned long len )
{
    Sint64 start, end;
    int rate = 0;
    next_loop_start = next_loop_end = 0;
    if (!soundLoop( buf, len, &start, &end, &rate )) return;
    int frame = audio_format.channels * (SDL_AUDIO_BITSIZE( audio_format.format ) / 8);
    next_loop_start = (Uint32)(start * audio_format.freq / rate) * frame;
    next_loop_end = (Uint32)(end * audio_format.freq / rate) * frame;
}

/* A sound cut off (a voice by the next one, dwavestop, stop, a load) used
 * to stop at once, mid-wave: a step in the output, heard as a pop. Instead,
 * the next CUT_FADE_MS of it fade out on a channel of their own, starting
 * where it was cut, while its channel is free at once for the next sound.
 * Where a channel is in its sound is counted by an effect that sees what
 * each mix takes from it (SDL_mixer's own fades change the volume once a
 * buffer, ~90 ms, too coarse to help). Music (Mix_Music) can't be read
 * ahead, so it still stops at once. */
#define CUT_FADE_MS 10
static struct { Mix_Chunk *chunk; Uint32 pos; bool loop; Uint32 loop_start, loop_end; } cut_track[ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS];
static struct { Mix_Chunk *chunk; Uint8 *buf; } cut_fade[ONS_MIX_FADE_CHANNELS];
static int cut_fade_next = 0;
static void playFadeTail( Uint8 *raw, Uint32 bytes, int volume );
extern bool ext_music_play_once_flag; // ONScripter_event.cpp

/* chfreq: a dwave channel's sound played at rate/1000 of its speed (and
 * pitch), ramped linearly from `from` to `to` over `ms` from `start` as
 * chvolfade's volume is. While `on`, the track effect makes the channel's
 * output itself from the sound at `pos` (sample frames, between samples
 * read straight between them), going on by the rate each frame; SDL_mixer
 * plays the sound looped underneath (so a slowed sound isn't cut at its
 * old length) and only its position is ignored. A sound that doesn't loop
 * ends with ONS_FREQ_END_EVENT, which halts the channel. A new sound on the
 * channel plays at its own rate again. */
static struct { volatile bool on; volatile int from, to, ms; volatile Uint32 start; int last; double pos; bool ended; } track_freq[ONS_MIX_CHANNELS];
static int track_chans = 2;

static void freqTrackEffect( int channel, Sint16 *out, int len )
{
    Mix_Chunk *chunk = cut_track[channel].chunk;
    int chans = track_chans, frame = 2 * chans, frames = len / frame;
    Uint32 total = chunk ? chunk->alen / frame : 0;
    bool inner = cut_track[channel].loop && cut_track[channel].loop_end > cut_track[channel].loop_start;
    double lstart = inner ? cut_track[channel].loop_start / frame : 0;
    double lend = inner ? cut_track[channel].loop_end / frame : total;
    int last = track_freq[channel].last;
    int rate = scriptVolumeAt( track_freq[channel].from, track_freq[channel].to, track_freq[channel].ms, track_freq[channel].start, SDL_GetTicks() );
    track_freq[channel].last = rate;
    const Sint16 *src = chunk ? (const Sint16*)chunk->abuf : NULL;
    double pos = track_freq[channel].pos;
    for (int f=0 ; f<frames ; f++){
        if (src && pos >= lend && (cut_track[channel].loop || inner)) pos -= lend - lstart;
        if (!src || track_freq[channel].ended || pos >= lend || pos < 0){
            for (int c=0 ; c<chans ; c++) out[f*chans + c] = 0;
            if (src && !track_freq[channel].ended){
                track_freq[channel].ended = true;
                SDL_Event event;
                SDL_zero( event );
                event.type = ONS_FREQ_END_EVENT;
                event.user.code = channel;
                SDL_PushEvent( &event );
            }
            continue;
        }
        Uint32 a = (Uint32)pos, b = a + 1;
        if (b >= (Uint32)lend) b = (cut_track[channel].loop || inner) ? (Uint32)lstart : a;
        double t = pos - a;
        for (int c=0 ; c<chans ; c++)
            out[f*chans + c] = (Sint16)(src[a*chans + c] + (src[b*chans + c] - src[a*chans + c]) * t);
        pos += (last + (double)(rate - last) * f / frames) / 1000.0;
    }
    track_freq[channel].pos = pos;
    cut_track[channel].pos = (Uint32)pos * frame; // for cutChannel and channelPlayedMs
}

/* --audio-stats: what is heard, in windows of AUDIO_STATS_MS of mixed
 * sound, for each dwave channel (as the track effect leaves it, times its
 * channel volume) and for the whole mix (the music and every channel),
 * counted on the mixer's thread and written by the trace from the main
 * one. Off: nothing is counted. */
#define AUDIO_STATS_MS 100
static bool audio_stats = false;
static int stats_rate = 44100, stats_chans = 2;
static Uint64 stats_frames = 0; // mixed so far, counted at the post mix
static struct { double sq[2]; Uint32 n; int step, cross, last[2]; bool neg; } stats_acc[ONS_MIX_CHANNELS+1]; // last: the mix
struct AudioStat { int ch; Uint32 audio_ms; int rms[2], hz, step; double pos; };
static std::vector<AudioStat> stats_done; // under the audio lock
static int stats_lost = 0;

// len bytes of S16 samples heard on ch (ONS_MIX_CHANNELS: the mix) at
// volume/MIX_MAX_VOLUME; pos: where the sound is after them, s.
static void audioStats( int ch, const Sint16 *s, int len, int volume, double pos )
{
    int frames = len / 2 / stats_chans, window = stats_rate * AUDIO_STATS_MS / 1000;
    for (int f=0 ; f<frames ; f++){
        int l = s[f*stats_chans], r = s[f*stats_chans + stats_chans - 1];
        int d = abs( l - stats_acc[ch].last[0] ), e = abs( r - stats_acc[ch].last[1] );
        if (d > stats_acc[ch].step) stats_acc[ch].step = d;
        if (e > stats_acc[ch].step) stats_acc[ch].step = e;
        stats_acc[ch].last[0] = l; stats_acc[ch].last[1] = r;
        stats_acc[ch].sq[0] += (double)l * l; stats_acc[ch].sq[1] += (double)r * r;
        bool neg = l + r < 0;
        if (neg != stats_acc[ch].neg) stats_acc[ch].cross++;
        stats_acc[ch].neg = neg;
        // shortcut: a sound's last part window waits for the channel's next sound
        if (++stats_acc[ch].n < (Uint32)window) continue;
        AudioStat a = { ch, (Uint32)((stats_frames + f + 1) * 1000 / stats_rate), {0, 0},
                        (int)((Sint64)stats_acc[ch].cross * stats_rate / (2 * stats_acc[ch].n)),
                        stats_acc[ch].step * volume / MIX_MAX_VOLUME, pos < 0 ? -1 : pos - (double)(frames - f - 1) / stats_rate };
        for (int i=0 ; i<2 ; i++) a.rms[i] = (int)(sqrt( stats_acc[ch].sq[i] / stats_acc[ch].n ) * volume / MIX_MAX_VOLUME);
        if (stats_done.size() < 100000) stats_done.push_back( a ); else stats_lost++;
        stats_acc[ch].sq[0] = stats_acc[ch].sq[1] = 0;
        stats_acc[ch].n = 0; stats_acc[ch].step = 0; stats_acc[ch].cross = 0;
    }
}

static void statsPostMix( void *udata, Uint8 *stream, int len )
{
    double pos = -1; // the music's, as this buffer ends (NULL: the one playing, read under the audio lock)
#if defined(SDL_MIXER_VERSION_ATLEAST) && SDL_MIXER_VERSION_ATLEAST(2,6,0)
    if (Mix_PlayingMusic()) pos = Mix_GetMusicPosition( NULL );
#endif
    audioStats( ONS_MIX_CHANNELS, (Sint16*)stream, len, MIX_MAX_VOLUME, pos );
    stats_frames += len / 2 / stats_chans;
}

void ONScripter::setAudioStats()
{
    audio_stats = true;
}

// After Mix_OpenAudio: the post mix counts the whole mix.
void ONScripter::audioStatsOpened()
{
    if (!audio_stats || audio_format.format != AUDIO_S16SYS) return;
    stats_rate = audio_format.freq;
    stats_chans = audio_format.channels;
    Mix_SetPostMix( statsPostMix, NULL );
}

// From the event loop: the windows heard since, as "audio" records.
void ONScripter::traceAudioStats()
{
    if (!audio_stats || !audio_open_flag) return;
    std::vector<AudioStat> done;
    SDL_LockAudio();
    done.swap( stats_done );
    int lost = stats_lost;
    stats_lost = 0;
    SDL_UnlockAudio();
    for (size_t i=0 ; i<done.size() ; i++){
        AudioStat &a = done[i];
        bool mix = a.ch == ONS_MIX_CHANNELS;
        char buf[256];
        int sum = a.rms[0] + a.rms[1];
        snprintf( buf, sizeof(buf), "{\"t\":\"audio\",\"pg\":%d,\"ms\":%u,\"audio_ms\":%u,\"%s\":%d,\"rms\":[%d,%d],\"bal\":%d,\"hz\":%d,\"step\":%d",
                  trace_page, SDL_GetTicks(), a.audio_ms, mix ? "mix" : "ch", mix ? 1 : a.ch, a.rms[0], a.rms[1],
                  sum ? (a.rms[1] - a.rms[0]) * 1000 / sum : 0, a.hz, a.step );
        std::string r = buf;
        snprintf( buf, sizeof(buf), ",\"pos\":%.3f", a.pos );
        if (a.pos >= 0) r += buf;
        const char *name = mix ? (music_file_name ? music_file_name : midi_file_name) : trace_se_name[a.ch];
        if (name){
            r += ",\"file\":";
            traceString( r, name );
        }
        r += '}';
        traceWrite( r );
    }
    if (lost){
        std::string r = "{\"t\":\"audio\",\"pg\":" + std::to_string( trace_page ) + ",\"lost\":" + std::to_string( lost ) + "}";
        traceWrite( r );
    }
}

static void cutTrackMix( int channel, void *stream, int len );
static void cutTrackEffect( int channel, void *stream, int len, void *udata )
{
    cutTrackMix( channel, stream, len );
    if (audio_stats && channel >= 0 && channel < ONS_MIX_CHANNELS && track_gain_s16){
        double pos = track_freq[channel].on ? track_freq[channel].pos / stats_rate :
            (double)cut_track[channel].pos / (2 * stats_chans) / stats_rate;
        audioStats( channel, (const Sint16*)stream, len, Mix_Volume( channel, -1 ), pos );
    }
}

static void cutTrackMix( int channel, void *stream, int len )
{
    if (channel < 0 || channel >= ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS) return;
    // A loop inside the sound (LOOPSTART/LOOPEND): what SDL_mixer took
    // from the whole is replaced from where the loop has got to.
    bool freq = channel < ONS_MIX_CHANNELS && track_freq[channel].on && track_gain_s16;
    if (freq) freqTrackEffect( channel, (Sint16*)stream, len );
    Mix_Chunk *looped = cut_track[channel].chunk;
    bool inner = !freq && looped && cut_track[channel].loop && cut_track[channel].loop_end > cut_track[channel].loop_start;
    if (inner){
        Uint32 end = cut_track[channel].loop_end, p = cut_track[channel].pos;
        Uint8 *s = (Uint8*)stream;
        for (int done = 0; done < len; ){
            if (p >= end) p = cut_track[channel].loop_start;
            Uint32 n = end - p;
            if (n > (Uint32)(len - done)) n = len - done;
            memcpy( s + done, looped->abuf + p, n );
            done += n;
            p += n;
        }
        cut_track[channel].pos = p;
    }
    if (channel < ONS_MIX_CHANNELS && track_gain_s16){
        int last = track_gain[channel].last, gain = trackGain( channel );
        track_gain[channel].last = gain;
        if (last != 4096 || gain != 4096){
            Sint16 *s = (Sint16*)stream;
            int n = len / 2;
            for (int i=0 ; i<n ; i++){
                int g = last + (int)((long long)(gain - last) * i / n);
                s[i] = (Sint16)(s[i] * g / 4096);
            }
        }
        // chpan: the other side lowered, ramped across the buffer
        int plast = track_pan[channel].last, pan = trackPan( channel );
        track_pan[channel].last = pan;
        if ((plast != 0 || pan != 0) && track_stereo){
            Sint16 *s = (Sint16*)stream;
            int n = len / 4;
            for (int i=0 ; i<n ; i++){
                int p = plast + (int)((long long)(pan - plast) * i / n);
                if (p > 0) s[2*i] = (Sint16)(s[2*i] * (1000 - p) / 1000);
                else if (p < 0) s[2*i+1] = (Sint16)(s[2*i+1] * (1000 + p) / 1000);
            }
        }
    }
    Mix_Chunk *chunk = cut_track[channel].chunk;
    if (!chunk || chunk->alen == 0 || inner || freq) return;
    Uint32 pos = cut_track[channel].pos + len;
    if (pos >= chunk->alen) pos = cut_track[channel].loop ? pos % chunk->alen : chunk->alen;
    cut_track[channel].pos = pos;
}

void ONScripter::playChannel( int channel, Mix_Chunk *chunk, int loops )
{
    if (channel_fade_in_ms > 0)
        Mix_FadeInChannel( channel, chunk, loops, channel_fade_in_ms );
    else
        Mix_PlayChannel( channel, chunk, loops );
    if (channel < 0 || channel >= ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS) return;
    cut_track[channel].chunk = chunk;
    cut_track[channel].pos = 0;
    cut_track[channel].loop = loops != 0;
    cut_track[channel].loop_start = next_loop_start;
    cut_track[channel].loop_end = next_loop_end <= chunk->alen ? next_loop_end : 0;
    next_loop_start = next_loop_end = 0;
    if (channel < ONS_MIX_CHANNELS){ // the ramps from where they are; the sound's own rate
        track_gain[channel].last = trackGain( channel );
        track_pan[channel].last = trackPan( channel );
        track_freq[channel].on = false;
    }
    track_chans = audio_format.channels;
    track_gain_s16 = audio_format.format == AUDIO_S16SYS;
    track_stereo = audio_format.channels == 2;
    Mix_RegisterEffect( channel, cutTrackEffect, NULL, NULL ); // reset when a sound ends
}

/* chfreq ch,rate[,ms]
 *   Plays the sound now on dwave channel ch at rate/1000 of its speed,
 *   which raises or lowers its pitch with it (100-4000; 1000 its own),
 *   reached over ms (default 0, at once; gamespeed applies) while the
 *   script goes on, from the rate it has now. A looping sound stays at it,
 *   one that doesn't ends sooner or later by as much. The next sound on the
 *   channel plays at its own rate; not kept in saves (a load starts looping
 *   sounds at their own rate). With no sound playing it does nothing.
 *       dwave 10,"se\shot.ogg":chfreq 10,1100,1000   (up a tenth in a second) */
int ONScripter::chfreqCommand()
{
    int ch = script_h.readInt();
    int rate = script_h.readInt();
    int ms = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : 0;
    if (rate < 100) rate = 100; else if (rate > 4000) rate = 4000;
    traceChannel( ch );
    if (ch < 0 || ch >= ONS_MIX_CHANNELS || !audio_open_flag || !wave_sample[ch] || !Mix_Playing( ch ) ||
        cut_track[ch].chunk != Mix_GetChunk( ch )) return RET_CONTINUE;
    if (audio_format.format != AUDIO_S16SYS){
        utils::printError("chfreq: needs 16-bit audio\n");
        return RET_CONTINUE;
    }
    Uint32 now = SDL_GetTicks();
    SDL_LockAudio();
    int from = track_freq[ch].on ? scriptVolumeAt( track_freq[ch].from, track_freq[ch].to, track_freq[ch].ms, track_freq[ch].start, now ) : 1000;
    if (!track_freq[ch].on){
        track_freq[ch].pos = cut_track[ch].pos / (2 * audio_format.channels);
        track_freq[ch].ended = false;
        track_freq[ch].last = from;
        if (!cut_track[ch].loop){ // looped underneath, so that a slower sound isn't cut short
            Mix_PlayChannel( ch, cut_track[ch].chunk, -1 ); // (the playing sound's end isn't signalled)
            Mix_RegisterEffect( ch, cutTrackEffect, NULL, NULL );
        }
        track_freq[ch].on = true;
    }
    track_freq[ch].from = from;
    track_freq[ch].start = now;
    track_freq[ch].to = rate;
    track_freq[ch].ms = ms > 0 ? speedTime( ms ) : 0;
    SDL_UnlockAudio();
    traceRamp( 2, ch, rate, track_freq[ch].ms );
    return RET_CONTINUE;
}

int trackFreq( int channel ) // per mille; the trace reads it
{
    if (channel < 0 || channel >= ONS_MIX_CHANNELS || !track_freq[channel].on) return 1000;
    return scriptVolumeAt( track_freq[channel].from, track_freq[channel].to, track_freq[channel].ms, track_freq[channel].start, SDL_GetTicks() );
}

// chfreq's sound (not looping) played to its end: stopped as it would have.
void ONScripter::endFreqChannel( int channel )
{
    if (channel < 0 || channel >= ONS_MIX_CHANNELS || !track_freq[channel].on || !track_freq[channel].ended) return;
    track_freq[channel].on = false;
    Mix_HaltChannel( channel ); // runs waveCallback, as when SDL_mixer ends it: frees the sound, ends a voice wait
}

// How long the sound on channel has played, in ms; -1 when none plays.
int ONScripter::channelPlayedMs( int channel )
{
    if (!audio_open_flag || channel < 0 || channel >= ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS ||
        !Mix_Playing( channel ) || !cut_track[channel].chunk) return -1;
    int bytes_ms = audio_format.freq * audio_format.channels * (SDL_AUDIO_BITSIZE(audio_format.format) / 8) / 1000;
    return bytes_ms > 0 ? (int)(cut_track[channel].pos / bytes_ms) : -1;
}

// Stops channel as Mix_Pause did, fading out what it was playing.
void ONScripter::cutChannel( int channel )
{
    bool playing = audio_open_flag && Mix_Playing( channel ) && !Mix_Paused( channel );
    Mix_Pause( channel ); // after this no mix takes more from it, so pos is final
    if (!playing || channel < 0 || channel >= ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS) return;
    Mix_Chunk *chunk = cut_track[channel].chunk;
    if (!chunk || chunk != Mix_GetChunk( channel ) || audio_format.format != AUDIO_S16SYS) return;

    int chans = audio_format.channels, frame = 2 * chans;
    Uint32 frames = (Uint32)audio_format.freq * CUT_FADE_MS / 1000;
    Uint32 pos = cut_track[channel].pos / frame * frame;
    Uint32 left = (chunk->alen - pos) / frame;
    if (!cut_track[channel].loop && left < frames) frames = left;
    if (frames < 2) return;

    Uint8 *raw = new Uint8[frames * frame];
    Sint16 *buf = (Sint16*)raw;
    const Sint16 *src = (const Sint16*)chunk->abuf;
    Uint32 total = chunk->alen / frame;
    Sint32 gain = trackGain( channel ); // chvolfade's, at the cut
    for (Uint32 f=0, at=pos/frame ; f<frames ; f++, at++){
        if (at >= total) at = 0; // a looping sound goes round
        for (int c=0 ; c<chans ; c++)
            buf[f*chans + c] = (Sint16)((Sint32)src[at*chans + c] * gain / 4096 * (Sint32)(frames - f) / (Sint32)frames);
    }

    playFadeTail( raw, frames * frame, Mix_Volume( channel, -1 ) );
}

// Plays raw (device format, faded already; taken over) once on one of the
// channels kept for fades, at volume (0-MIX_MAX_VOLUME).
static void playFadeTail( Uint8 *raw, Uint32 bytes, int volume )
{
    int n = cut_fade_next++ % ONS_MIX_FADE_CHANNELS;
    int fade_channel = ONS_MIX_CHANNELS + ONS_MIX_EXTRA_CHANNELS + n;
    Mix_HaltChannel( fade_channel );
    if (cut_fade[n].chunk) Mix_FreeChunk( cut_fade[n].chunk );
    delete[] cut_fade[n].buf;
    cut_fade[n].buf = raw;
    cut_fade[n].chunk = Mix_QuickLoad_RAW( cut_fade[n].buf, bytes );
    if (!cut_fade[n].chunk) return;
    Mix_Volume( fade_channel, volume );
    Mix_PlayChannel( fade_channel, cut_fade[n].chunk, 0 );
}

/* A piece of an Ogg Vorbis file, its frames [from, from+count) at the file's
 * rate, decoded without the rest: SDL_mixer decodes a sound whole (a
 * 4-minute track is 40 MB and 190 ms native, 290 ms in the web build, on
 * the main thread). The file's header pages and the audio pages around the
 * piece are spliced into a short Ogg file (pages renumbered, their CRCs
 * redone) for Mix_LoadWAV_RW, whichever Vorbis decoder it has. Decoders
 * give a page's samples up to its granule position, so the piece's last
 * frame is the last page's granule (marked the end of the stream, a whole
 * packet ending it): that places its first. Returns the
 * chunk (device format) and in *skip the frames before `from`; NULL when
 * the file isn't one Ogg Vorbis stream. */
static Mix_Chunk *oggPiece( const unsigned char *buf, unsigned long len, Sint64 from, Sint64 count,
                            int rate, int dev_rate, Uint32 frame, Uint32 *skip )
{
    struct Page { unsigned long at, size; Sint64 granule; };
    std::vector<Page> pages;
    for (unsigned long at = 0; at + 27 <= len && !memcmp( buf + at, "OggS", 4 ); ){
        int segs = buf[at+26];
        unsigned long size = 27 + segs;
        for (int i = 0; i < segs && at + 27 + i < len; i++) size += buf[at+27+i];
        if (at + size > len || memcmp( buf + at + 14, buf + 14, 4 )) break; // cut short, or a second stream
        Sint64 g = 0;
        for (int i = 7; i >= 0; i--) g = g << 8 | buf[at+6+i];
        pages.push_back( { at, size, g } );
        at += size;
    }
    size_t h = 0; // the first audio page: headers have granule 0
    while (h < pages.size() && pages[h].granule == 0) h++;
    if (h == 0 || h >= pages.size()) return NULL;
    // From a page before the one `from` is in (the first packet of a piece
    // gives no samples), and at a page that starts a packet
    size_t p = h, q;
    for (size_t i = h; i < pages.size(); i++) if (pages[i].granule > 0 && pages[i].granule <= from) p = i;
    p = p > h ? p - 1 : h;
    while (p > h && buf[pages[p].at+5] & 1) p--;
    for (q = p; q + 1 < pages.size() && (pages[q].granule < from + count || pages[q].granule <= 0 || buf[pages[q+1].at+5] & 1); q++);
    std::vector<unsigned char> ogg( buf, buf + pages[h].at );
    Uint32 seq = buf[pages[h-1].at+18] | buf[pages[h-1].at+19] << 8 | buf[pages[h-1].at+20] << 16 | (Uint32)buf[pages[h-1].at+21] << 24;
    for (size_t i = p; i <= q; i++){
        size_t at = ogg.size();
        ogg.insert( ogg.end(), buf + pages[i].at, buf + pages[i].at + pages[i].size );
        // The last page ends the stream: decoders then stop at its granule
        // (otherwise some give the last packet's overlap too)
        if (++seq != (Uint32)(ogg[at+18] | ogg[at+19] << 8 | ogg[at+20] << 16 | (Uint32)ogg[at+21] << 24) || (i == q && !(ogg[at+5] & 4))){
            ogg[at+5] |= i == q ? 4 : 0;
            for (int b = 0; b < 4; b++) ogg[at+18+b] = (seq >> 8*b) & 0xff, ogg[at+22+b] = 0;
            Uint32 crc = 0; // Ogg's CRC-32 (0x04c11db7, not reflected), over the page with its CRC zero
            for (size_t k = at; k < ogg.size(); k++){
                crc ^= (Uint32)ogg[k] << 24;
                for (int b = 0; b < 8; b++) crc = crc & 0x80000000 ? crc << 1 ^ 0x04c11db7 : crc << 1;
            }
            for (int b = 0; b < 4; b++) ogg[at+22+b] = (crc >> 8*b) & 0xff;
        }
    }
    Mix_Chunk *chunk = Mix_LoadWAV_RW( SDL_RWFromConstMem( ogg.data(), (int)ogg.size() ), 1 );
    if (!chunk) return NULL;
    Sint64 got = (Sint64)(chunk->alen / frame) * rate / dev_rate; // at the file's rate
    Sint64 first = pages[q].granule - got;
    *skip = from > first ? (Uint32)((from - first) * dev_rate / rate) : 0;
    return chunk;
}

// An Ogg file's length in frames at its rate: its last page's granule position.
static Sint64 oggLength( const unsigned char *buf, unsigned long len )
{
    for (unsigned long at = len >= 27 ? len - 27 : 0; at > 0; at--)
        if (!memcmp( buf + at, "OggS", 4 ) && buf[at+5] & 4){ // the end of stream page
            Sint64 g = 0;
            for (int i = 7; i >= 0; i--) g = g << 8 | buf[at+6+i];
            return g;
        }
    return 0;
}

/* bgm "file",ms: the music playing goes on fading out over ms while the
 * next fades in. SDL_mixer plays one music at a time, so the old one's
 * next ms are decoded again from its file (kept in music_buffer), from
 * where its decoder is (oggPiece: only those), faded and played on a fade
 * channel; the music is halted in the same lock, so the two join without a
 * gap or overlap. Not for music played as a sound (.wav on the BGM
 * channel) or a MIDI. */
void ONScripter::crossFadeOutBGM( int ms )
{
#if defined(SDL_MIXER_VERSION_ATLEAST) && SDL_MIXER_VERSION_ATLEAST(2,6,0)
    if (!audio_open_flag || ms <= 0 || !music_info || !music_buffer || Mix_PlayingMusic() != 1 || Mix_PausedMusic() ||
        timer_bgmfade_id || audio_format.format != AUDIO_S16SYS) return;
    int volume = Mix_VolumeMusic( -1 );
    if (volume <= 0) return;
    int chans = audio_format.channels, frame = 2 * chans, dev_rate = audio_format.freq;
    Uint32 frames = (Uint32)((long long)dev_rate * ms / 1000);
    // The decoder goes on while the piece is decoded (~5 ms): take more
    const int ahead = 250;
    double s0 = Mix_GetMusicPosition( music_info );
    std::vector<Sint16> src; // device frames from s0 on
    const unsigned char *mb = music_buffer;
    bool vorbis = music_buffer_length >= 44 && !memcmp( mb, "OggS", 4 ) && !memcmp( mb + 28, "\x01vorbis", 7 );
    int rate = vorbis ? mb[40] | mb[41] << 8 | mb[42] << 16 | mb[43] << 24 : 0;
    Sint64 total = vorbis ? oggLength( mb, music_buffer_length ) : 0;
    double length = total > 0 ? (double)total / rate : 0; // s
    Uint32 skip = 0;
    Mix_Chunk *piece = NULL;
    if (rate > 0 && total > 0){
        Sint64 from = s0 > 0 ? (Sint64)(s0 * rate) : 0, count = (Sint64)rate * (ms + ahead) / 1000;
        if (music_play_loop_flag) from %= total;
        while (count > 0 && from < total && (piece = oggPiece( mb, music_buffer_length, from, count, rate, dev_rate, frame, &skip ))){
            Uint32 n = piece->alen / frame;
            if (skip < n) src.insert( src.end(), (Sint16*)piece->abuf + skip * chans, (Sint16*)piece->abuf + n * chans );
            Mix_FreeChunk( piece );
            count -= total - from;
            if (!music_play_loop_flag) break;
            from = 0; // a looping music goes round
        }
    }
    else{ // shortcut: not Ogg Vorbis (WAV, Opus as WAV: quick; MP3: decoded whole)
        Mix_Chunk *whole = Mix_LoadWAV_RW( SDL_RWFromMem( music_buffer, music_buffer_length ), 1 );
        if (!whole) return;
        Uint32 n = whole->alen / frame, at = s0 > 0 ? (Uint32)(s0 * dev_rate) : 0;
        length = (double)n / dev_rate;
        if (music_play_loop_flag && n > 0) at %= n;
        for (Uint32 f = 0; f < frames + (Uint32)dev_rate * ahead / 1000 && n > 0; f++, at++){
            if (at >= n){ if (!music_play_loop_flag) break; at = 0; }
            src.insert( src.end(), (Sint16*)whole->abuf + at * chans, (Sint16*)whole->abuf + (at + 1) * chans );
        }
        Mix_FreeChunk( whole );
    }
    SDL_LockAudio();
    double s = Mix_GetMusicPosition( music_info );
    Uint32 total_src = src.size() / chans;
    if (s < s0 && music_play_loop_flag) s += length; // it went round meanwhile
    Uint32 at = s > s0 ? (Uint32)((s - s0) * dev_rate) : 0;
    if (at > total_src) at = total_src;
    if (frames > total_src - at) frames = total_src - at;
    if (frames >= 2){
        Uint8 *raw = new Uint8[frames * frame];
        Sint16 *buf = (Sint16*)raw;
        for (Uint32 f=0 ; f<frames ; f++)
            for (int c=0 ; c<chans ; c++)
                buf[f*chans + c] = (Sint16)((Sint32)src[(at+f)*chans + c] * (Sint32)(frames - f) / (Sint32)frames);
        playFadeTail( raw, frames * frame, volume );
        Mix_HookMusicFinished( NULL ); // as mp3stop's fade does
        ext_music_play_once_flag = true;
        Mix_HaltMusic();
    }
    SDL_UnlockAudio();
#else
    (void)ms;
#endif
}

// A dwave channel playing a sound with dwaveloop or dwaveplayloop.
bool ONScripter::isLoopPlaying( int channel )
{
    return audio_open_flag && trace_se_name[channel] && trace_se_loop[channel] && Mix_Playing( channel );
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
        sprite_info[i].trail_rect.w = sprite_info[i].trail_rect.h = 0;
    }
    dirty_rect.fill( screen_width, screen_height );
    setPlaytime( 0 ); // older saves have none
#ifdef USE_LUA
    lua_handler.loadState( NULL ); // a save without Lua data has no script globals
#endif
    if (readInt() != CONS_SAVE_MAGIC) return;
    int version = readInt();
    if (version != CONS_SAVE_VERSION){ // older layouts aren't read
        utils::printError("save: CONscripter data version %d, this build reads %d; left out\n", version, CONS_SAVE_VERSION);
        return;
    }

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

    char *lua = NULL;
    readStr( &lua );
#ifdef USE_LUA
    lua_handler.loadState( lua );
#endif
    delete[] lua;

    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt(), passes = readInt(), alpha = readInt(), lag = readInt();
        if (no < 0 || no >= MAX_SPRITE_NUM || passes <= 0) continue;
        AnimationInfo &ai = sprite_info[no];
        ai.trail_passes = passes < SPTRAIL_MAX ? passes : SPTRAIL_MAX;
        ai.trail_alpha = alpha;
        ai.trail_lag = lag;
    }

    num = readInt();
    for (int i=0 ; i<num ; i++){
        int pan = readInt();
        if (i >= ONS_MIX_CHANNELS) continue;
        track_pan[i].ms = 0;
        track_pan[i].from = track_pan[i].to = pan;
    }

    num = readInt();
    for (int n=-1 ; n<screen_stack_num ; n++){
        int *z = (n < 0) ? screen_filter_z : screen_stack[n].screen_filter_z;
        for (int i=0 ; i<num ; i++){
            int v = readInt();
            if (i < MAX_SCREEN_FILTER) z[i] = v;
        }
    }

    num = readInt();
    int now = SDL_GetTicks();
    for (int n=0 ; n<num ; n++){
        int no = readInt(), ms = readInt(), period = readInt(), amp0 = readInt(), amp1 = readInt(), curve = readInt(), gone = readInt();
        if (no < 0 || no >= MAX_SPRITE_NUM || ms <= 0) continue;
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
 *   too); when from doesn't exist, deletes to instead. For a script that
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
        FILE *in = fopen(src.c_str(), "rb");
        if (!in){
            if (::remove(dst.c_str()) != 0 && errno != ENOENT)
                utils::printError("copysavegame: can't delete %s\n", dst.c_str());
            continue;
        }
        std::vector<char> data;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) data.insert(data.end(), buf, buf + n);
        fclose(in);
        FILE *out = fopen(dst.c_str(), "wb");
        if (!out || fwrite(data.data(), 1, data.size(), out) != data.size())
            utils::printError("copysavegame: can't write %s\n", dst.c_str());
        if (out) fclose(out);
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

// The width of half-width character c over half a cell, in script pixels.
int ONScripter::hankakuExtra( FontInfo *glyph_info, char c )
{
    if (!isProportional(glyph_info)) return 0;
    unsigned short unicode = (unsigned char)c;
    if ((c & 0xe0) == 0xa0 || (c & 0xe0) == 0xc0) unicode = unicode - 0xa0 + 0xff60; // half-width kana, as drawGlyph
    int a, adv;
    if (TTF_GlyphMetrics((TTF_Font*)glyph_info->ttf_font[0], unicode, &a, &a, &a, &a, &adv) != 0) return 0;
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
    for (const char *p = buf; *p && *p != 0x0a && !IS_TWO_BYTE(*p) && (p == buf || *p != ' '); p++){
        if (*p == '@' || *p == '\\' || *p == '#' || *p == '(' || (*p == '!' && strchr("swdzic", p[1]) && p[1])) break;
        w += info->pitch_xy[0]/2 + hankakuExtra(glyph_info, *p);
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
        if (IS_TWO_BYTE(*p)){
            w += f->pitch_xy[0];
            p += coding2utf16->force_utf8 ? UTF8_N_BYTE(*p) : 2;
            continue;
        }
        w += f->pitch_xy[0]/2 + hankakuExtra(glyph_info, *p);
        p++;
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
    if (IS_TWO_BYTE(text[0])) return f->isEndOfLine();
    return f->isEndOfLine() || (!f->isLineEmpty() && isProportional(glyph_info) &&
                                f->isEndOfLine(1, hankakuExtra(glyph_info, text[0]) - 1));
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

/* effect num,99,duration,"cons_slide.dll/layer,dx,dy"
 *   A picture slides across the screen, moving in direction dx,dy (each
 *   -1, 0 or 1, so 8 directions) by one screen width or height:
 *   layer 0 the new picture slides in over the old one, 1 the old picture
 *   slides out and uncovers the new one, 2 both move together (a push,
 *   like effects 11-14 but also diagonal). Without the dll name's
 *   parameters it is 0,-1,0 (the new picture comes in from the right). */
void ONScripter::effectSlide( const char *params, int duration )
{
    int param[3] = {0, -1, 0};
    readDllParams(params, param, 3);
    int layer = param[0], dx = param[1], dy = param[2];

    int ox = duration > 0 ? screen_width  * effect_counter / duration : screen_width;
    int oy = duration > 0 ? screen_height * effect_counter / duration : screen_height;
    SDL_Rect old_rect = {0, 0, screen_width, screen_height};
    SDL_Rect new_rect = old_rect;
    if (layer == 0){
        new_rect.x = -dx * (screen_width  - ox);
        new_rect.y = -dy * (screen_height - oy);
    }
    else{
        old_rect.x = dx * ox;
        old_rect.y = dy * oy;
        if (layer == 2){
            new_rect.x = old_rect.x - dx * screen_width;
            new_rect.y = old_rect.y - dy * screen_height;
        }
    }

    // The picture below first, then the moving one over it (SDL_BlitSurface
    // clips the rect it is given, hence the copies).
    SDL_Rect below = layer == 0 ? old_rect : new_rect, above = layer == 0 ? new_rect : old_rect;
    if (layer == 2)
        SDL_FillRect( accumulation_surface, NULL, SDL_MapRGBA( accumulation_surface->format, 0, 0, 0, 0xff ) );
    SDL_BlitSurface(layer == 0 ? effect_src_surface : effect_dst_surface, NULL, accumulation_surface, &below);
    SDL_BlitSurface(layer == 0 ? effect_dst_surface : effect_src_surface, NULL, accumulation_surface, &above);
}

// src magnified by scale about the screen centre (bilinear), drawn over
// dst with opacity alpha (0-256).
static void drawZoomed( SDL_Surface *src, SDL_Surface *dst, float scale, int alpha )
{
    if (alpha <= 0) return;
    if (alpha > 256) alpha = 256;
    if (scale < 1.0f) scale = 1.0f;
    int w = dst->w, h = dst->h;
    // Source position of a pixel centre, in 16.16: c + (x + 0.5 - c) / scale - 0.5
    Sint64 step = (Sint64)(65536.0f / scale);
    Sint64 x0 = (Sint64)((w * 0.5f - w * 0.5f / scale - 0.5f + 0.5f / scale) * 65536.0f);
    Sint64 y0 = (Sint64)((h * 0.5f - h * 0.5f / scale - 0.5f + 0.5f / scale) * 65536.0f);

    SDL_LockSurface(src);
    SDL_LockSurface(dst);
    for (int y=0 ; y<h ; y++){
        Sint64 fy = y0 + step * y;
        if (fy < 0) fy = 0;
        int sy = (int)(fy >> 16), wy = (int)((fy >> 8) & 0xff);
        if (sy >= h - 1){ sy = h - 1; wy = 0; }
        const Uint8 *row0 = (const Uint8*)src->pixels + sy * src->pitch;
        const Uint8 *row1 = wy ? row0 + src->pitch : row0;
        Uint8 *out = (Uint8*)dst->pixels + y * dst->pitch;
        for (int x=0 ; x<w ; x++, out+=4){
            Sint64 fx = x0 + step * x;
            if (fx < 0) fx = 0;
            int sx = (int)(fx >> 16), wx = (int)((fx >> 8) & 0xff);
            if (sx >= w - 1){ sx = w - 1; wx = 0; }
            const Uint8 *a = row0 + sx * 4, *b = row1 + sx * 4;
            int nx = wx ? 4 : 0;
            for (int c=0 ; c<4 ; c++){
                int top = a[c] * (256 - wx) + a[c + nx] * wx;
                int bottom = b[c] * (256 - wx) + b[c + nx] * wx;
                int v = (top * (256 - wy) + bottom * wy) >> 16;
                out[c] = alpha == 256 ? v : (v * alpha + out[c] * (256 - alpha)) >> 8;
            }
        }
    }
    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

/* effect num,99,duration,"cons_zoom.dll/mode,scale[,curve]"
 *   Zooms about the screen centre. scale is the largest magnification in
 *   percent (5100 = 51 times); curve (default 1) is the power of the
 *   eased progress, so higher values move faster at the magnified end.
 *   With progress p (0 to 1) and z(s) = 1 + (scale/100 - 1) * s:
 *   mode 0: the new picture shrinks into place from z(1), the old one
 *     fading out over it: s = (1-p)^curve, old opacity s;
 *   mode 1: the old picture grows to z(1) and fades out over the new one:
 *     s = p^curve, old opacity 1-s;
 *   mode 2: zooms into the old picture for the first half, then out of
 *     the new one: s = (2p)^curve, then (2-2p)^curve;
 *   mode 3: as mode 0, while the old picture also grows to z(1) as it
 *     fades (linearly in p). */
void ONScripter::effectZoom( const char *params, int duration )
{
#if defined(BPP16)
    alphaBlend( NULL, ALPHA_BLEND_CONST, 256 * effect_counter / duration, &dirty_rect.bounding_box );
#else
    int param[3] = {0, 200, 1};
    readDllParams(params, param, 3);
    int mode = param[0];
    float range = param[1] / 100.0f - 1.0f;
    float curve = param[2] > 0 ? (float)param[2] : 1.0f;
    float p = duration > 0 ? (float)effect_counter / duration : 1.0f;
    if (p > 1.0f) p = 1.0f;

    float s;
    switch (mode){
      case 1:
        s = powf(p, curve);
        SDL_BlitSurface(effect_dst_surface, NULL, accumulation_surface, NULL);
        drawZoomed(effect_src_surface, accumulation_surface, 1.0f + range * s, (int)(256 * (1.0f - s)));
        break;
      case 2:
        if (p < 0.5f){
            s = powf(2.0f * p, curve);
            drawZoomed(effect_src_surface, accumulation_surface, 1.0f + range * s, 256);
        }
        else{
            s = powf(2.0f - 2.0f * p, curve);
            drawZoomed(effect_dst_surface, accumulation_surface, 1.0f + range * s, 256);
        }
        break;
      case 3:
        s = powf(1.0f - p, curve);
        drawZoomed(effect_dst_surface, accumulation_surface, 1.0f + range * s, 256);
        drawZoomed(effect_src_surface, accumulation_surface, 1.0f + range * p, (int)(256 * s));
        break;
      default:
        s = powf(1.0f - p, curve);
        drawZoomed(effect_dst_surface, accumulation_surface, 1.0f + range * s, 256);
        drawZoomed(effect_src_surface, accumulation_surface, 1.0f, (int)(256 * s));
        break;
    }
#endif
}

// src squeezed to factor f (0-1) of its width (axis 0) or height (axis 1)
// about the screen centre, drawn opaque over dst (nearest pixel).
static void drawSqueezed( SDL_Surface *src, SDL_Surface *dst, int axis, float f )
{
    int w = dst->w, h = dst->h;
    int len = axis == 0 ? w : h;
    int span = (int)(len * f + 0.5f);
    if (span <= 0) return;
    int start = (len - span) / 2;
    SDL_LockSurface(src);
    SDL_LockSurface(dst);
    for (int i=0 ; i<span ; i++){
        int s = (int)((i + 0.5f) * len / span);
        if (s >= len) s = len - 1;
        int d = start + i;
        if (axis == 0){
            for (int y=0 ; y<h ; y++)
                *((Uint32*)((Uint8*)dst->pixels + y * dst->pitch) + d) = *((const Uint32*)((const Uint8*)src->pixels + y * src->pitch) + s);
        }
        else
            memcpy((Uint8*)dst->pixels + d * dst->pitch, (const Uint8*)src->pixels + s * src->pitch, w * 4);
    }
    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

/* effect num,99,duration,"cons_flip.dll/axis,layer"
 *   A picture turns like a card seen edge-on, about the screen centre:
 *   axis 0 squeezes the width, 1 the height. layer 0: the new picture
 *   grows from a line over the old one, its size sin(p * 90 degrees);
 *   1: the old picture shrinks to a line over the new one, cos(p * 90
 *   degrees). p is the progress, 0 to 1. */
void ONScripter::effectFlip( const char *params, int duration )
{
#if defined(BPP16)
    alphaBlend( NULL, ALPHA_BLEND_CONST, 256 * effect_counter / duration, &dirty_rect.bounding_box );
#else
    int param[2] = {0, 0};
    readDllParams(params, param, 2);
    int axis = param[0] ? 1 : 0, layer = param[1];
    float p = duration > 0 ? (float)effect_counter / duration : 1.0f;
    if (p > 1.0f) p = 1.0f;
    const float quarter = 1.57079632679f;
    if (layer == 0){
        SDL_BlitSurface(effect_src_surface, NULL, accumulation_surface, NULL);
        drawSqueezed(effect_dst_surface, accumulation_surface, axis, sinf(p * quarter));
    }
    else{
        SDL_BlitSurface(effect_dst_surface, NULL, accumulation_surface, NULL);
        drawSqueezed(effect_src_surface, accumulation_surface, axis, cosf(p * quarter));
    }
#endif
}

// src's whole width (axis 0) or height (axis 1) mapped linearly onto the
// span a-b of dst along that axis (nearest pixel), clipped to dst.
static void drawSpan( SDL_Surface *src, SDL_Surface *dst, int axis, float a, float b )
{
    int w = dst->w, h = dst->h;
    int len = axis == 0 ? w : h;
    if (b - a < 0.5f) return;
    int d0 = a < 0 ? 0 : (int)a, d1 = b > len ? len : (int)(b + 0.5f);
    SDL_LockSurface(src);
    SDL_LockSurface(dst);
    for (int d=d0 ; d<d1 ; d++){
        int s = (int)((d + 0.5f - a) * len / (b - a));
        if (s < 0) s = 0;
        if (s >= len) s = len - 1;
        if (axis == 0){
            for (int y=0 ; y<h ; y++)
                *((Uint32*)((Uint8*)dst->pixels + y * dst->pitch) + d) = *((const Uint32*)((const Uint8*)src->pixels + y * src->pitch) + s);
        }
        else
            memcpy((Uint8*)dst->pixels + d * dst->pitch, (const Uint8*)src->pixels + s * src->pitch, w * 4);
    }
    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

/* effect num,99,duration,"cons_cube.dll/dx,dy"
 *   The pictures are two faces of a cube as wide (or high) as the screen,
 *   turning a quarter turn by p * 90 degrees, seen straight on (Xuse's
 *   BoxRolling): the new face comes in moving in direction dx,dy (one of
 *   them -1 or 1), each face squeezed to its projected width, sin and cos
 *   of the angle, and the parts beyond the screen cut off. The edge
 *   between them is at half the width times 1 + sin - cos from the side
 *   the new face comes from. */
void ONScripter::effectCube( const char *params, int duration )
{
#if defined(BPP16)
    alphaBlend( NULL, ALPHA_BLEND_CONST, 256 * effect_counter / duration, &dirty_rect.bounding_box );
#else
    int param[2] = {1, 0};
    readDllParams(params, param, 2);
    int axis = param[0] != 0 ? 0 : 1;
    int dir = axis == 0 ? param[0] : param[1];
    float p = duration > 0 ? (float)effect_counter / duration : 1.0f;
    if (p > 1.0f) p = 1.0f;
    float t = p * 1.57079632679f, s = sinf(t), c = cosf(t);
    float half = (axis == 0 ? screen_width : screen_height) * 0.5f;
    // Measured from the side the new face comes from.
    float outer = half * (1.0f - s - c), edge = half * (1.0f + s - c), far = half * (1.0f + s + c);
    float len = 2.0f * half;
    if (dir > 0){
        drawSpan(effect_dst_surface, accumulation_surface, axis, outer, edge);
        drawSpan(effect_src_surface, accumulation_surface, axis, edge, far);
    }
    else{
        drawSpan(effect_dst_surface, accumulation_surface, axis, len - edge, len - outer);
        drawSpan(effect_src_surface, accumulation_surface, axis, len - far, len - edge);
    }
#endif
}

/* effect num,99,duration,"cons_melt.dll/stretch[,seed]"
 *   The old picture melts down off the screen, uncovering the new one
 *   (Xuse's MeltDown). Its top edge is a smooth random curve (a
 *   Catmull-Rom spline through 11 random points across the width, its
 *   lowest 40 % cut off flat); each column starts to fall at a delay of
 *   0.2-0.3 of the duration (0.2 where flat) and
 *   falls with constant acceleration, h * ((p - delay) / (1 - delay))^2,
 *   so the last column leaves the screen at the end. A falling column is
 *   stretched by 1 + stretch / 100 * fallen / h. seed picks the curve
 *   (default 0: a new one each time). */
void ONScripter::effectMelt( const char *params, int duration )
{
#if defined(BPP16)
    alphaBlend( NULL, ALPHA_BLEND_CONST, 256 * effect_counter / duration, &dirty_rect.bounding_box );
#else
    int param[2] = {0, 0};
    readDllParams(params, param, 2);
    int w = accumulation_surface->w, h = accumulation_surface->h;
    static float *delay = NULL;
    static int delay_w = 0, last_counter = 0;
    if (!delay || delay_w != w || effect_counter < last_counter){
        unsigned int r = param[1] ? (unsigned int)param[1] : (unsigned int)SDL_GetTicks() * 2654435761u + 1;
        float pt[13];
        for (int i=0 ; i<11 ; i++){
            r = r * 1103515245u + 12345u;
            pt[i + 1] = (float)((r >> 16) & 0x7fff) / 32768.0f;
        }
        pt[0] = pt[1];
        pt[12] = pt[11];
        delete[] delay;
        delay = new float[w];
        delay_w = w;
        float lo = 1.0f, hi = 0.0f;
        for (int x=0 ; x<w ; x++){
            float u = (float)x * 10 / w;
            int i = (int)u;
            float t = u - i;
            const float *q = pt + i;
            float v = 0.5f * (2 * q[1] + (q[2] - q[0]) * t
                              + (2 * q[0] - 5 * q[1] + 4 * q[2] - q[3]) * t * t
                              + (3 * q[1] - q[0] - 3 * q[2] + q[3]) * t * t * t);
            delay[x] = v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        for (int x=0 ; x<w ; x++)
        {
            // the lowest 40 % of the curve is cut off: flat stretches
            float n = hi > lo ? (delay[x] - lo) / (hi - lo) : 0.0f;
            delay[x] = 0.2f + 0.1f * std::max(0.0f, (n - 0.4f) / 0.6f);
        }
    }
    last_counter = effect_counter;

    float p = duration > 0 ? (float)effect_counter / duration : 1.0f;
    if (p > 1.0f) p = 1.0f;
    SDL_BlitSurface(effect_dst_surface, NULL, accumulation_surface, NULL);
    SDL_LockSurface(effect_src_surface);
    SDL_LockSurface(accumulation_surface);
    for (int x=0 ; x<w ; x++){
        float f = p > delay[x] ? (p - delay[x]) / (1.0f - delay[x]) : 0.0f;
        float fallen = h * f * f;
        if (fallen >= h) continue;
        float scale = 1.0f + param[0] / 100.0f * fallen / h;
        for (int y=(int)fallen ; y<h ; y++){
            int s = (int)((y - fallen) / scale);
            if (s >= h) break;
            *((Uint32*)((Uint8*)accumulation_surface->pixels + y * accumulation_surface->pitch) + x) =
                *((const Uint32*)((const Uint8*)effect_src_surface->pixels + s * effect_src_surface->pitch) + x);
        }
    }
    SDL_UnlockSurface(accumulation_surface);
    SDL_UnlockSurface(effect_src_surface);
#endif
}

// A row (y) of src into out, shifted right by dx pixels (edges repeated),
// or with columns (vertical) shifted down by dx: one pixel of out.
static inline const Uint8 *shiftedPixel( SDL_Surface *src, int x, int y, int dx, bool vertical )
{
    int w = src->w, h = src->h;
    if (vertical){
        int sy = y - dx;
        sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy;
        return (const Uint8*)src->pixels + sy * src->pitch + x * 4;
    }
    int sx = x - dx;
    sx = sx < 0 ? 0 : sx >= w ? w - 1 : sx;
    return (const Uint8*)src->pixels + y * src->pitch + sx * 4;
}

/* effect num,99,duration,"cons_wave.dll/direction,amplitude,waves"
 *   A cross-fade with the pictures' rows waving sideways (direction 0 or
 *   2; 1 or 3: the columns up and down; 2 and 3 the other way): each row
 *   is shifted by amplitude/1000 of the screen's width times
 *   sin(pi p) (none at the ends, the most halfway) times
 *   sin(2 pi (waves/10 * y / height + p)), p the progress 0 to 1. For the
 *   MAGES engine's InOut_Act 1. */
void ONScripter::effectWave( const char *params, int duration )
{
#if defined(BPP16)
    alphaBlend( NULL, ALPHA_BLEND_CONST, 256 * effect_counter / duration, &dirty_rect.bounding_box );
#else
    int param[3] = {0, 80, 15};
    readDllParams(params, param, 3);
    bool vertical = param[0] & 1;
    float sign = param[0] >= 2 ? -1.0f : 1.0f;
    float p = duration > 0 ? (float)effect_counter / duration : 1.0f;
    if (p > 1.0f) p = 1.0f;
    int alpha = (int)(256 * p);
    int w = accumulation_surface->w, h = accumulation_surface->h;
    float amp = sign * param[1] / 1000.0f * (vertical ? h : w) * sinf(3.14159265f * p);
    int len = vertical ? w : h;
    SDL_LockSurface(effect_src_surface);
    SDL_LockSurface(effect_dst_surface);
    SDL_LockSurface(accumulation_surface);
    for (int y=0 ; y<h ; y++){
        Uint8 *out = (Uint8*)accumulation_surface->pixels + y * accumulation_surface->pitch;
        int row_dx = vertical ? 0 : (int)lroundf(amp * sinf(6.2831853f * (param[2] / 10.0f * y / len + p)));
        for (int x=0 ; x<w ; x++, out+=4){
            int dx = vertical ? (int)lroundf(amp * sinf(6.2831853f * (param[2] / 10.0f * x / len + p))) : row_dx;
            const Uint8 *a = shiftedPixel(effect_src_surface, x, y, dx, vertical);
            const Uint8 *b = shiftedPixel(effect_dst_surface, x, y, dx, vertical);
            for (int c=0 ; c<4 ; c++) out[c] = (Uint8)((a[c] * (256 - alpha) + b[c] * alpha) >> 8);
        }
    }
    SDL_UnlockSurface(accumulation_surface);
    SDL_UnlockSurface(effect_dst_surface);
    SDL_UnlockSurface(effect_src_surface);
#endif
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

/* effect num,99,duration,"cons_rule.dll/file,cells"
 *   A transition drawn from data: file holds cells grey masks side by
 *   side (each stretched to the screen), the first at the start, the last
 *   at the end, evenly in time; between two cells their masks mix. Where
 *   the mask is white the new picture shows, black the old one, grey a mix.
 *   For a game's own transition: run its kernel (the exe's function in an
 *   emulator, or a port) on a black old and a white new picture and keep
 *   the frames as cells, so the engine needs no code for it. Covers what
 *   mixes the two pictures in place (wipes, dissolves, blinds, soft
 *   edges); transitions that move pixels stay plugins (cons_slide, ...).
 *   Fewer than 2 cells or a missing file: a plain cross-fade. */
void ONScripter::effectRule( const char *params, int duration )
{
    static char *rule_name = NULL;
    static SDL_Surface *rule = NULL;
    const char *p = strchr(params, '/');
    const char *comma = p ? strrchr(p, ',') : NULL;
    int cells = comma ? atoi(comma + 1) : 0;
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
            if (!rule) utils::printError("cons_rule.dll: can't load %s\n", rule_name);
        }
    }
    if (cells < 2 || !rule || rule->format->BytesPerPixel != 4 || rule->w < cells){
        alphaBlend( NULL, ALPHA_BLEND_CONST, 256 * effect_counter / (duration > 0 ? duration : 1), &dirty_rect.bounding_box );
        return;
    }
    float pr = duration > 0 ? (float)effect_counter / duration : 1.0f;
    if (pr > 1.0f) pr = 1.0f;
    float f = pr * (cells - 1);
    int c = (int)f;
    if (c > cells - 2) c = cells - 2;
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
    int w, h;
    if (!WebPGetInfo(data, length, &w, &h)) return NULL;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) return NULL;
    SDL_LockSurface(s);
    uint8_t *ok = WebPDecodeRGBAInto(data, length, (uint8_t *)s->pixels, (size_t)s->pitch * h, s->pitch);
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
    }
    else if (code == WEB_SET_SKIP && !value && (skip_mode & SKIP_NORMAL)){
        skip_mode &= ~SKIP_NORMAL;
        webStateHook();
    }
    else if (code == WEB_SET_RUBY) ruby_hidden = !value; // from the next text drawn
    else if (code == WEB_SET_TEXT_SPEED) text_speed_override = value >= 0 ? value : -1;
    return false;
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
#endif
}

// The web build: closing or reloading the tab doesn't quit the engine, so
// what it writes on quit (gloval.sav, kidoku.dat) would be lost: the
// settings, the read marks, the unlocks. At any wait (auto mode reads on
// with no click wait), at most every 2 seconds, the global variables are written when they changed, and the
// read marks every 30 seconds (or with the globals), then the save
// directory is synced to IndexedDB.
void ONScripter::webPersistHook()
{
#if defined(WEB)
    Uint32 now = SDL_GetTicks();
    if (web_persist_time && now - web_persist_time < 2000) return;
    web_persist_time = now;
    bool changed = false;
    if (globalon_flag){
        file_io_buf_ptr = 0;
        writeVariables( script_h.global_variable_border, script_h.variable_range, false );
        allocFileIOBuf();
        writeVariables( script_h.global_variable_border, script_h.variable_range, true );
        if (web_globals_written.size() != file_io_buf_ptr ||
            memcmp( web_globals_written.data(), file_io_buf, file_io_buf_ptr )){
            if (saveFileIOBuf( "gloval.sav" ) == 0){
                web_globals_written.assign( file_io_buf, file_io_buf + file_io_buf_ptr );
                changed = true;
            }
        }
    }
    if (kidokuskip_flag && (changed || now - web_kidoku_time >= 30000)){
        script_h.saveKidokuData();
        web_kidoku_time = now;
        changed = true;
    }
    if (changed) EM_ASM(
        if(window.flush_save) window.flush_save();
        else FS.syncfs(false, (err)=>{console.log(err);});
    );
#endif
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
        playVideoHold(g_cons_module.UTF8ToString($0), $1 ? HEAPU8.slice($1, $1 + $3 * $4) : null, $2, $4, $3 >> 2,
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
 * place, without its sound, while the game waits: the sprite's order, the
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
        HEAPU8.set(d, $1);
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

// Each movie's frame due now, drawn; a movie whose picture is gone stops.
void ONScripter::proceedSpriteMovies( int current_time )
{
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
            if (anim->visible){
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
            if (first && page->voice_file) r.voice = page->voice_file;
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

/* logadd "text"[,"voice"]
 *   Adds a page with text to the text history, before the page being
 *   written (so what is on screen stays as it is); voice is the voice
 *   getlogrow gives for it. Lines the story puts in the backlog without
 *   showing them. */
int ONScripter::logaddCommand()
{
    std::string text = script_h.readStr();
    const char *voice = NULL;
    std::string v;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA){
        v = script_h.readStr();
        if (!v.empty()) voice = v.c_str();
    }
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
    return RET_CONTINUE;
}

/* Opus sounds: SDL_mixer plays Opus only when it was built with
 * opusfile, which none of our builds' is, so the engine decodes them
 * itself (cmake/cons_opus.cmake builds opusfile into every build) and
 * hands SDL_mixer a WAV: 48 kHz 16-bit stereo, with a LOOPSTART /
 * LOOPEND (or LOOPLENGTH) tag, in 48 kHz samples as SDL_mixer reads them
 * from Opus, as the WAV's loop (smpl). Any file name works: the data is
 * recognised by its header. */
#if defined(USE_CONS_OPUS)
#include <opusfile.h>

static void putLE32(std::vector<unsigned char> &v, Uint32 x)
{
    for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xff);
}

static Sint64 opusTagSamples(const OpusTags *tags, const char *name)
{
    const char *v = opus_tags_query(tags, name, 0);
    if (!v || !*v) return -1;
    if (!strchr(v, ':')) return strtoll(v, NULL, 10);
    // h:m:s.frac, as SDL_mixer also accepts
    double t = 0;
    for (const char *p = v; *p;) {
        char *end;
        double x = strtod(p, &end);
        if (end == p) return -1;
        t = *end == ':' ? (t + x) * 60 : t + x;
        p = *end == ':' ? end + 1 : end;
        if (*p && *p != ':' && end == p) return -1;
    }
    return (Sint64)(t * 48000);
}

unsigned char *decodeOpus(const unsigned char *data, unsigned long length, unsigned long *wav_length)
{
    if (length < 36 || memcmp(data, "OggS", 4) != 0 || memcmp(data + 28, "OpusHead", 8) != 0)
        return NULL;
    int err = 0;
    OggOpusFile *of = op_open_memory(data, length, &err);
    if (!of) return NULL;
    std::vector<opus_int16> pcm;
    ogg_int64_t total = op_pcm_total(of, -1);
    if (total > 0) pcm.reserve((size_t)total * 2);
    opus_int16 buf[11520]; // 120 ms of 48 kHz stereo, opusfile's largest
    for (;;) {
        int n = op_read_stereo(of, buf, sizeof(buf) / sizeof(buf[0]));
        if (n == OP_HOLE) continue; // a gap in the data: keep going
        if (n <= 0) break;
        pcm.insert(pcm.end(), buf, buf + n * 2);
    }
    Sint64 frames = pcm.size() / 2;
    const OpusTags *tags = op_tags(of, -1);
    Sint64 loop_start = tags ? opusTagSamples(tags, "LOOPSTART") : -1;
    Sint64 loop_end = tags ? opusTagSamples(tags, "LOOPEND") : -1;
    Sint64 loop_length = tags ? opusTagSamples(tags, "LOOPLENGTH") : -1;
    op_free(of);
    if (frames == 0) return NULL;
    if (loop_start >= 0 && loop_end < 0 && loop_length > 0) loop_end = loop_start + loop_length;
    bool loop = loop_start >= 0 && loop_end > loop_start && loop_end <= frames;

    std::vector<unsigned char> head;
    Uint32 data_bytes = (Uint32)(frames * 4);
    Uint32 smpl_bytes = loop ? 36 + 24 : 0;
    head.insert(head.end(), {'R', 'I', 'F', 'F'});
    putLE32(head, 4 + (8 + 16) + (smpl_bytes ? 8 + smpl_bytes : 0) + 8 + data_bytes);
    head.insert(head.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    putLE32(head, 16);
    putLE32(head, 1 | (2 << 16)); // PCM, stereo
    putLE32(head, 48000);
    putLE32(head, 48000 * 4);
    putLE32(head, 4 | (16 << 16)); // block align, bits
    if (loop) {
        head.insert(head.end(), {'s', 'm', 'p', 'l'});
        putLE32(head, smpl_bytes);
        for (int i = 0; i < 7; i++) putLE32(head, 0); // manufacturer ... SMPTE offset
        putLE32(head, 1); // one loop
        putLE32(head, 0); // no sampler data
        putLE32(head, 0); // loop id
        putLE32(head, 0); // forward
        putLE32(head, (Uint32)loop_start);
        putLE32(head, (Uint32)(loop_end - 1)); // the smpl end is the loop's last sample
        putLE32(head, 0); // fraction
        putLE32(head, 0); // play forever
    }
    head.insert(head.end(), {'d', 'a', 't', 'a'});
    putLE32(head, data_bytes);
    unsigned char *wav = new (std::nothrow) unsigned char[head.size() + data_bytes];
    if (!wav) return NULL;
    memcpy(wav, head.data(), head.size());
    // WAV is little-endian
    unsigned char *out = wav + head.size();
    for (size_t i = 0; i < pcm.size(); i++) {
        out[2 * i] = pcm[i] & 0xff;
        out[2 * i + 1] = (pcm[i] >> 8) & 0xff;
    }
    *wav_length = head.size() + data_bytes;
    return wav;
}
#endif // USE_CONS_OPUS

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
