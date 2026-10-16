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
#define CONS_SAVE_VERSION 15 // 1: sprite filters; 2: + screen stack; 3: + screen filters; 4: + screen effects;
                            // 5: + strsp sprites, looping dwave sounds; 6: + strsp sprites in the screen stack;
                            // 7: + sprite zooms; 8: + script volumes (chvolfade, bgmvolfade);
                            // 9: + sprite motions; 10: + sprite orders (spz); 11: + play time;
                            // 12: + keyed sprite alphas (spalpha); 13: + motions' zooms, angles, keep (spmotion2)
                            // 14: + the music bgmlater will start; 15: + additive sprites (spblend)

extern SDL_TimerID timer_bgmfade_id; // bgmfadein, bgmfadeout

/* chvolfade: each dwave channel's script volume, a percentage of the
 * setting's, ramped linearly from `from` to `to` over `ms` from `start`.
 * The track effect below scales the samples by it (in 1/4096, from the
 * last mix's gain to this one's across the buffer, so a ramp has no
 * steps); SDL_mixer's channel volume, which the settings and dwavefadein
 * use, stays theirs. It stays with the channel, for the next sounds too,
 * so a script can set it before the sound starts; reset sets 100. */
static struct { volatile int from, to, ms; volatile Uint32 start; int last; } track_gain[ONS_MIX_CHANNELS];
static bool track_gain_s16 = false;

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

/* spzoom num, zoom_x[, zoom_y]
 *   Draws sprite num (an lsp sprite) zoomed by zoom_x and zoom_y percent
 *   (100 = its size; zoom_y defaults to zoom_x) about the centre of its
 *   place, which amsp and msp move as usual. Unlike lsp2, the sprite keeps
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

    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spzoom: sprite %d out of range\n", no);
        return RET_CONTINUE;
    }
    AnimationInfo *ai = &sprite_info[no];
    ai->zoom_x = zx;
    ai->zoom_y = zy;
    // Where the zoomed picture was isn't known after an amsp: redraw it all.
    if (ai->visible) dirty_rect.fill( screen_width, screen_height );

    return RET_CONTINUE;
}

/* spmotion num[,loop,t1,x1,y1,t2,x2,y2,...]
 *   Moves sprite num (an lsp sprite) by itself along offsets from its
 *   place: x,y (pixels) at t ms from now, straight from key to key,
 *   stepped about every 16 ms by the timer, during text and click waits
 *   too. loop 1 starts again after the last key (put the first key's
 *   offset there for a smooth loop); loop 0 ends at the last key and the
 *   sprite is back at its place. "spmotion num" stops it there. Up to 32
 *   keys. amsp, msp and spzoom act on the place as usual; the motion stays
 *   when lsp changes the image, is removed by csp and is kept in save
 *   files (restarted). "spmotion 10,1,0,0,0,250,0,-12,500,0,0" bobs
 *   sprite 10 up and down twice a second. loop 2 runs once and leaves the
 *   sprite at the last key: its offset is added to the sprite's place (as
 *   msp), its zoom to the spzoom and its alpha to the sprite's; the angle
 *   goes back to 0. Another spmotion or spmotion2 on the sprite (also one
 *   that stops it), lsp, msp or amsp first ends such a motion at its last
 *   key.
 *
 * spmotion2 num[,loop,t1,x1,y1,zoom1,angle1,alpha1,...]
 *   spmotion with a zoom (percent, 100 = its size, times spzoom's) and an
 *   angle (degrees, counterclockwise, as lsp2's), both about the centre of
 *   the sprite's place, and an alpha (0-256 of the sprite's) in each key:
 *   "spmotion2 10,1,0,0,0,100,0,256,1000,0,0,100,360,256" spins sprite 10
 *   once a second. */
int ONScripter::spmotionCommand()
{
    leaveTextDisplayMode();

    bool two = script_h.isName("spmotion2");
    int no = script_h.readInt();
    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spmotion: sprite %d out of range\n", no);
        while (script_h.getEndStatus() & ScriptHandler::END_COMMA) script_h.readInt();
        return RET_CONTINUE;
    }
    AnimationInfo *ai = &sprite_info[no];
    SDL_Rect before = drawnRect( *ai );
    endKeptMotion( *ai );
    ai->motion_keys = 0;
    ai->motion_dx = ai->motion_dy = 0;
    ai->motion_zoom = 100;
    ai->motion_rot = 0;
    ai->motion_alpha = 256;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA){
        int mode = script_h.readInt();
        ai->motion_loop = mode == 1;
        ai->motion_keep = mode == 2;
        while (script_h.getEndStatus() & ScriptHandler::END_COMMA){
            int t = script_h.readInt();
            int x = script_h.readInt();
            int y = script_h.readInt();
            int zoom = two ? script_h.readInt() : 100;
            int rot = two ? script_h.readInt() : 0;
            int alpha = two ? script_h.readInt() : 256;
            if (ai->motion_keys == AnimationInfo::MAX_MOTION_KEYS){
                utils::printError("spmotion: more than %d keys\n", AnimationInfo::MAX_MOTION_KEYS);
                continue;
            }
            int *k = ai->motion_key[ai->motion_keys++];
            k[0] = speedTime( t ); k[1] = x; k[2] = y; k[3] = zoom; k[4] = rot; k[5] = alpha; // gamespeed
        }
        ai->motion_start = ai->motion_next = SDL_GetTicks();
        stepSpriteMotion( *ai, ai->motion_start );
    }
    if (ai->visible){
        dirty_rect.add( before );
        dirty_rect.add( drawnRect( *ai ) );
    }

    return RET_CONTINUE;
}

/* spfade num,ms[,from]
 *   Fades sprite num (an lsp sprite) in by itself: it is drawn at from/256
 *   (0-256, default 0) of its alpha, rising to its alpha over ms, stepped
 *   about every 16 ms by the timer while the script goes on (the text, a
 *   click wait). "spfade num,0" ends it at once. The fade stays when lsp
 *   changes the image and is removed by csp; a save keeps the sprite's
 *   own alpha, so a load shows it at the end of its fade.
 *   "lsp 60,":a;noise.png",0,0,200:spfade 60,2000:print 1" brings static
 *   up to 200 over two seconds. */
int ONScripter::spfadeCommand()
{
    int no = script_h.readInt();
    int ms = script_h.readInt();
    int from = 0;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA) from = script_h.readInt();
    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spfade: sprite %d out of range\n", no);
        return RET_CONTINUE;
    }
    AnimationInfo &ai = sprite_info[no];
    if (from < 0) from = 0;
    if (from > 256) from = 256;
    ai.fade_ms = ms > 0 ? speedTime( ms ) : 0; // gamespeed
    ai.fade_from = from;
    ai.fade_alpha = ai.fade_ms > 0 ? from : 256;
    ai.fade_start = ai.fade_next = SDL_GetTicks();
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );

    return RET_CONTINUE;
}

/* spalpha num[,loop,t1,a1,t2,a2,...]
 *   Draws sprite num (an lsp sprite) at a changing part of its alpha by
 *   itself: a/256 (0-256) at t ms from now, straight from key to key,
 *   stepped about every 16 ms by the timer while the script goes on (the
 *   text, a click wait). loop 1 starts again after the last key (put the
 *   first key's alpha there for a smooth loop); loop 0 stops at the last
 *   key and the sprite keeps that alpha; loop n (2 or more) goes through
 *   the keys n times, then stops as loop 0 does (a light flashing n
 *   times). "spalpha num" stops it and draws
 *   the sprite at its own alpha. Up to 32 keys. The keys stay when lsp
 *   changes the image, are removed by csp and are kept in save files
 *   (restarted). "spalpha 20,1,0,0,600,256,1100,256,1700,0,2200,0"
 *   flickers sprite 20 in and out (a fire's glow). */
int ONScripter::spalphaCommand()
{
    int no = script_h.readInt();
    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spalpha: sprite %d out of range\n", no);
        while (script_h.getEndStatus() & ScriptHandler::END_COMMA) script_h.readInt();
        return RET_CONTINUE;
    }
    AnimationInfo *ai = &sprite_info[no];
    ai->alpha_keys = 0;
    ai->key_alpha = 256;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA){
        ai->alpha_loop = script_h.readInt();
        if (ai->alpha_loop < 0) ai->alpha_loop = 0;
        while (script_h.getEndStatus() & ScriptHandler::END_COMMA){
            int t = script_h.readInt();
            int a = script_h.readInt();
            if (ai->alpha_keys == AnimationInfo::MAX_MOTION_KEYS){
                utils::printError("spalpha: more than %d keys\n", AnimationInfo::MAX_MOTION_KEYS);
                continue;
            }
            if (a < 0) a = 0;
            if (a > 256) a = 256;
            int *k = ai->alpha_key[ai->alpha_keys++];
            k[0] = speedTime( t ); k[1] = a; // gamespeed
        }
        ai->alpha_start = ai->alpha_next = SDL_GetTicks();
        stepSpriteAlpha( *ai, ai->alpha_start );
    }
    if (ai->visible) dirty_rect.add( drawnRect( *ai ) );

    return RET_CONTINUE;
}

/* spblend num,mode
 *   Draws sprite num (an lsp sprite) adding its colours to what is under
 *   it (mode 1), times its alpha, or blended as usual (mode 0): a white
 *   sprite at alpha a lightens the screen by a, as a flash of light. It
 *   keeps its place among the sprites (unlike lsp2add, drawn over all of
 *   them). It stays when lsp changes the image, is removed by csp and is
 *   kept in save files. lsp 2,":c;>800,600,#ffffff",0,0:spblend 2,1:
 *   spalpha 2,5,0,128,80,0 flashes the screen five times. */
int ONScripter::spblendCommand()
{
    int no = script_h.readInt();
    int mode = script_h.readInt();
    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spblend: sprite %d out of range\n", no);
        return RET_CONTINUE;
    }
    AnimationInfo *ai = &sprite_info[no];
    ai->blending_mode = (mode == 1) ? AnimationInfo::BLEND_ADD : AnimationInfo::BLEND_NORMAL;
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

void ONScripter::drawSpriteRange( SDL_Surface *surface, SDL_Rect &clip, int hi, int lo )
{
    bool moved = false;
    for (int i=0 ; i<MAX_SPRITE_NUM && !moved ; i++)
        if (sprite_info[i].z_over >= 0) moved = true;
    for (int i=hi ; i>=lo ; i--){
        AnimationInfo &ai = sprite_info[i];
        if (ai.image_surface && ai.visible && (!moved || ai.z_over < 0))
            drawTaggedSurface( surface, &ai, clip );
        if (!moved) continue;
        for (int k=MAX_SPRITE_NUM-1 ; k>=0 ; k--){
            AnimationInfo &over = sprite_info[k];
            if (over.z_over == i && over.image_surface && over.visible)
                drawTaggedSurface( surface, &over, clip );
        }
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
    if (ai.motion_keys == 0) return;
    int last = ai.motion_key[ai.motion_keys-1][0];
    int t = current_time - ai.motion_start;
    if (ai.motion_loop && last > 0) t %= last;
    else if (t >= last){
        if (ai.motion_keep){
            int *k = ai.motion_key[ai.motion_keys-1];
            ai.orig_pos.x += k[1];
            ai.orig_pos.y += k[2];
            ai.scalePosXY( screen_ratio1, screen_ratio2 );
            ai.zoom_x = ai.zoom_x * k[3] / 100;
            ai.zoom_y = ai.zoom_y * k[3] / 100;
            if (k[5] < 256) ai.trans = (ai.trans < 0 ? 255 : ai.trans) * k[5] / 256;
        }
        ai.motion_keys = 0;
        ai.motion_dx = ai.motion_dy = 0;
        ai.motion_zoom = 100;
        ai.motion_rot = 0;
        ai.motion_alpha = 256;
        return;
    }
    int i = 0;
    while (i + 1 < ai.motion_keys && ai.motion_key[i+1][0] <= t) i++;
    int *a = ai.motion_key[i];
    int v[6] = {0, a[1], a[2], a[3], a[4], a[5]};
    if (i + 1 < ai.motion_keys && t > a[0]){
        int *b = ai.motion_key[i+1];
        int span = b[0] - a[0];
        if (span > 0)
            for (int j=1 ; j<6 ; j++) v[j] += (b[j] - a[j]) * (t - a[0]) / span;
    }
    ai.motion_dx = v[1] * screen_ratio1 / screen_ratio2;
    ai.motion_dy = v[2] * screen_ratio1 / screen_ratio2;
    ai.motion_zoom = v[3];
    ai.motion_rot = v[4];
    ai.motion_alpha = v[5];
}

// Sets a keyed alpha (spalpha) for the time; stops one that ran once, at
// its last key's alpha.
void ONScripter::stepSpriteAlpha( AnimationInfo &ai, int current_time )
{
    if (ai.alpha_keys == 0) return;
    int last = ai.alpha_key[ai.alpha_keys-1][0];
    int t = current_time - ai.alpha_start;
    int times = ai.alpha_loop > 1 ? ai.alpha_loop : 1;
    if (ai.alpha_loop == 1 && last > 0) t %= last;
    else if (t >= last * times){
        ai.key_alpha = ai.alpha_key[ai.alpha_keys-1][1];
        ai.alpha_keys = 0;
        return;
    }
    else if (last > 0) t %= last; // loop n: the time into this round
    int i = 0;
    while (i + 1 < ai.alpha_keys && ai.alpha_key[i+1][0] <= t) i++;
    int *a = ai.alpha_key[i];
    int alpha = a[1];
    if (i + 1 < ai.alpha_keys && t > a[0]){
        int *b = ai.alpha_key[i+1];
        int span = b[0] - a[0];
        if (span > 0) alpha += (b[1] - a[1]) * (t - a[0]) / span;
    }
    ai.key_alpha = alpha;
}

// Steps the sprites' fades, keyed alphas and motions; draws what changed
// (draw false: only says whether something did, for an effect running).
bool ONScripter::proceedSpriteMotions( int current_time, bool draw )
{
    // Every sprite that changed is drawn in one flush: a flush per sprite
    // takes long enough, with the software renderer, that sprites fading
    // in together appeared one after another.
    DirtyRect changed;
    changed.setDimension(screen_width, screen_height);
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo &ai = sprite_info[i];
        // spfade
        if (ai.fade_ms > 0 && ai.fade_next <= current_time){
            if (current_time - ai.fade_next > 1000){
                ai.fade_start += current_time - ai.fade_next;
                ai.fade_next = current_time;
            }
            while (ai.fade_next <= current_time) ai.fade_next += 16;
            int alpha = ai.fade_alpha, t = current_time - ai.fade_start;
            if (t >= ai.fade_ms){
                ai.fade_ms = 0;
                ai.fade_alpha = 256;
            }
            else
                ai.fade_alpha = ai.fade_from + (256 - ai.fade_from) * t / ai.fade_ms;
            if (ai.visible && ai.image_surface && alpha != ai.fade_alpha)
                changed.add( drawnRect( ai ) );
        }
        // spalpha
        if (ai.alpha_keys > 0 && ai.alpha_next <= current_time){
            if (current_time - ai.alpha_next > 1000){
                ai.alpha_start += current_time - ai.alpha_next;
                ai.alpha_next = current_time;
            }
            while (ai.alpha_next <= current_time) ai.alpha_next += 16;
            int alpha = ai.key_alpha;
            stepSpriteAlpha( ai, current_time );
            if (ai.visible && ai.image_surface && alpha != ai.key_alpha)
                changed.add( drawnRect( ai ) );
        }
        if (ai.motion_keys == 0 || ai.motion_next > current_time) continue;
        // After a long stop (a menu, a load) go on from now.
        if (current_time - ai.motion_next > 1000){
            ai.motion_start += current_time - ai.motion_next;
            ai.motion_next = current_time;
        }
        while (ai.motion_next <= current_time) ai.motion_next += 16;
        int dx = ai.motion_dx, dy = ai.motion_dy, zoom = ai.motion_zoom, rot = ai.motion_rot, alpha = ai.motion_alpha;
        SDL_Rect r = drawnRect( ai );
        stepSpriteMotion( ai, current_time );
        if (!ai.visible || !ai.image_surface) continue;
        if (dx == ai.motion_dx && dy == ai.motion_dy && zoom == ai.motion_zoom && rot == ai.motion_rot && alpha == ai.motion_alpha && ai.motion_keys > 0) continue;
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
    return r;
}

// A visible sprite is zoomed or moving: it covers more than its place.
bool ONScripter::spriteZoomOrMotion()
{
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        const AnimationInfo &ai = sprite_info[i];
        if (ai.visible && ai.image_surface && (ai.motion_keys > 0 || ai.zoom_x != 100 || ai.zoom_y != 100)) return true;
    }
    return false;
}

// The ticks of the next motion step, 0 when no sprite moves.
int ONScripter::nextSpriteMotionTime()
{
    int min = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo &ai = sprite_info[i];
        if (ai.fade_ms > 0 && (min == 0 || min > ai.fade_next)) min = ai.fade_next;
        if (ai.alpha_keys > 0 && (min == 0 || min > ai.alpha_next)) min = ai.alpha_next;
        if (ai.motion_keys == 0) continue;
        if (min == 0 || min > ai.motion_next) min = ai.motion_next;
    }
    return min;
}

// The box a sprite's picture covers, zoomed (spzoom, a motion's zoom) and
// turned (a motion's angle) about its centre.
SDL_Rect ONScripter::zoomedRect( const AnimationInfo &ai )
{
    SDL_Rect r = ai.pos;
    if (ai.zoom_x == 100 && ai.zoom_y == 100 && ai.motion_zoom == 100 && ai.motion_rot == 0) return r;
    int w = ai.pos.w * abs(ai.zoom_x) / 100 * abs(ai.motion_zoom) / 100;
    int h = ai.pos.h * abs(ai.zoom_y) / 100 * abs(ai.motion_zoom) / 100;
    if (ai.motion_rot % 360 != 0) w = h = (int)ceil( sqrt( (double)w * w + (double)h * h ) );
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
    if (anim->zoom_x == 0 || anim->zoom_y == 0 || anim->motion_zoom == 0) return;
    SDL_Rect pos = anim->pos, affine_pos = anim->affine_pos, bounding_rect = anim->bounding_rect;
    int scale_x = anim->scale_x, scale_y = anim->scale_y, rot = anim->rot;

    anim->pos.x = poly_rect.x + pos.w/2;
    anim->pos.y = poly_rect.y + pos.h/2;
    anim->affine_pos.x = 0; // blendOnSurface2 adds the current cell's offset
    anim->affine_pos.y = 0;
    anim->affine_pos.w = pos.w;
    anim->affine_pos.h = pos.h;
    anim->scale_x = anim->zoom_x * anim->motion_zoom / 100;
    anim->scale_y = anim->zoom_y * anim->motion_zoom / 100;
    anim->rot = anim->motion_rot;
    anim->calcAffineMatrix();
    anim->blendOnSurface2( dst_surface, anim->pos.x, anim->pos.y, clip, anim->trans );

    anim->pos = pos;
    anim->affine_pos = affine_pos;
    anim->bounding_rect = bounding_rect;
    anim->scale_x = scale_x;
    anim->scale_y = scale_y;
    anim->rot = rot;
}

/* screenfilter slot, kind[, blend[, strength[, param...]]]
 *   A filter like spfilter's (same kinds, blends and parameters) on the
 *   whole picture: the background and the sprites behind the text window
 *   (those above the humanz number), where monocro applies. Not the text
 *   window or the sprites in front of it. Slots 1-4 apply in order; kind 0
 *   clears a slot and "screenfilter 0" clears them all. Like spfilter it
 *   doesn't draw: show the result with print (an effect fades it in). The
 *   filters are kept in save files and cleared by reset.
 *   "screenfilter 1,4,0,256,-200" darkens the scene for night. */
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
    }
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

void ONScripter::applyScreenFilters( SDL_Surface *surface, SDL_Rect &clip )
{
    AnimationInfo::Filter f[MAX_SCREEN_FILTER];
    int num = 0;
    for (int i=0 ; i<MAX_SCREEN_FILTER ; i++)
        if (screen_filter[i].kind != AnimationInfo::FILTER_NONE) f[num++] = screen_filter[i];
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
 *              amp1, angle, angle_step, phase_step[, blend]]
 *   Ripples the same picture through a displacement map (a greyscale
 *   image as large as the screen): each pixel is taken from amp *
 *   sin(2 pi (grey / 256 + phase / 1000)) pixels away in the direction
 *   angle (tenths of a degree, 0 = right, 900 = down). The rippled picture
 *   is laid over the plain one at opacity (0-256); with blend 1 the plain
 *   picture is multiplied by the rippled one instead (darker where both
 *   are dark), at that opacity. Over time ms, opacity
 *   goes from opacity0 to opacity1 and amp from amp0 to amp1; every
 *   interval ms the angle turns by angle_step and the phase moves by
 *   phase_step.
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
 * dwavefadeout ch,ms
 *   Fades the sound of dwave channel ch out over ms milliseconds, then
 *   stops it; the script goes on at once. A looping sound fading out isn't
 *   kept in a save. 0 ms stops it as dwavestop does. */
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

int ONScripter::dwavefadeoutCommand()
{
    int ch = script_h.readInt();
    int ms = script_h.readInt();
    if      (ch < 0) ch = 0;
    else if (ch >= ONS_MIX_CHANNELS) ch = ONS_MIX_CHANNELS-1;
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

/* chvolfade ch,vol[,ms]
 *   Sets the volume of dwave channel ch's sound to vol percent (0-100)
 *   of the sound effect or voice setting, over ms milliseconds (default
 *   0, at once) while the script goes on: a fade from where it is now.
 *   The sound keeps playing at 0. The volume stays with the channel for
 *   the sounds played on it next, so set it before dwave to start a sound
 *   quieter; reset puts every channel back to 100. Kept in a save.
 *       chvolfade 3,30:dwaveloop 3,"se\rain.ogg"   (at 30 %)
 *       chvolfade 3,100,2000                       (back up in 2 s)
 * bgmvolfade vol[,ms]
 *   The same for the music (bgm, mp3): vol percent of the music setting,
 *   for the next bgm too. bgmfadein and bgmfadeout fade to and from it.
 *   Kept in a save. */
int ONScripter::chvolfadeCommand()
{
    int ch = script_h.readInt();
    int vol = script_h.readInt();
    int ms = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : 0;
    if      (ch < 0) ch = 0;
    else if (ch >= ONS_MIX_CHANNELS) ch = ONS_MIX_CHANNELS-1;
    if (vol < 0) vol = 0; else if (vol > 100) vol = 100;
    Uint32 now = SDL_GetTicks();
    int from = scriptVolumeAt( track_gain[ch].from, track_gain[ch].to, track_gain[ch].ms, track_gain[ch].start, now );
    // The mixer reads these as they change: no ramp until the last one
    track_gain[ch].ms = 0;
    track_gain[ch].from = from;
    track_gain[ch].start = now;
    track_gain[ch].to = vol;
    track_gain[ch].ms = ms > 0 ? ms : 0;
    return RET_CONTINUE;
}

int ONScripter::bgmvolfadeCommand()
{
    int vol = script_h.readInt();
    int ms = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : 0;
    if (vol < 0) vol = 0; else if (vol > 100) vol = 100;
    bgm_script_from = bgmScriptVolume();
    bgm_script_start = SDL_GetTicks();
    bgm_script_to = vol;
    bgm_script_ms = ms > 0 ? ms : 0;
    applyMusicVolume();
    return RET_CONTINUE;
}

/* Player settings, for a game's settings screen. None is saved: the game
 * keeps its settings and sets them again at startup.
 *
 * soundmute bgm,se,voice
 *   1 mutes the music, the sound effects (every dwave channel but 0), the
 *   voice (channel 0), whatever their volume; 0 lets them be heard again.
 *   Muted sounds go on playing unheard, so music unmuted mid-piece goes on
 *   from where it is. Auto mode doesn't wait for a muted voice.
 * clickvoicestop on
 *   1: a click that ends a text wait (a click wait in the text, textbtnwait)
 *   stops the voice (channel 0). 0 (default): it plays on.
 * autovoicewait on
 *   1 (default): auto mode waits for the voice to end before reading on.
 *   0: it reads on after its own time and stops the voice.
 * selectkeep on
 *   1: auto mode and skip go on after a select's option is chosen.
 *   0 (default): a select ends them, as in NScripter.
 * moviestop mode
 *   Whether a click stops a movie (mpegplay, movie): 1 always, 0 never,
 *   -1 (default) as the script's click flag says.
 * gamespeed percent
 *   Script times run at percent of real speed (100 default, 10-10000):
 *   wait, delay, waittimer and gettimer, effects (print), spfade, spmotion
 *   and screenshake's time and end. 200 plays them in half the time. Text
 *   speed, auto mode's wait and sounds aren't changed.
 * sysevol vol
 *   The volume (0-100) of wave's channel, where the menu sounds play:
 *   wave, waveloop, selectvoice's and menuselectvoice's sounds, btnse's.
 *   -1 (default): the sound effects' volume (sevol), as before. A game's
 *   "system volume".
 * btnse "file"
 *   A sound played on wave's channel when a btnwait, btnwait2 or
 *   textbtnwait ends on a button (a number of 1 or more; not a click
 *   beside the buttons, a right click or a key), the menus' "confirm"
 *   sound. "" (default): none. */
int ONScripter::soundmuteCommand()
{
    int m = 0;
    if (script_h.readInt()) m |= SOUND_MUTE_BGM;
    if (script_h.readInt()) m |= SOUND_MUTE_SE;
    if (script_h.readInt()) m |= SOUND_MUTE_VOICE;
    sound_mute = m;
    applyMusicVolume();
    return RET_CONTINUE;
}

int ONScripter::clickvoicestopCommand()
{
    click_voice_stop = script_h.readInt() != 0;
    return RET_CONTINUE;
}

int ONScripter::autovoicewaitCommand()
{
    auto_voice_wait = script_h.readInt() != 0;
    return RET_CONTINUE;
}

int ONScripter::selectkeepCommand()
{
    select_keep = script_h.readInt() != 0;
    return RET_CONTINUE;
}

/* effectoff on
 *   1 = every print effect (and bg/ld/cl's) is drawn at once, as with
 *   effect 1; 0 (default) = as the script asks. */
int ONScripter::effectoffCommand()
{
    effect_off = script_h.readInt() != 0;
    return RET_CONTINUE;
}

/* textshadow on
 *   0 = the message text is drawn without a shadow, whatever setwindow
 *   says; 1 (default) = with setwindow's. */
int ONScripter::textshadowCommand()
{
    bool on = script_h.readInt() != 0;
    if (on != text_shadow){
        text_shadow = on;
        dirty_rect.fill( screen_width, screen_height );
    }
    return RET_CONTINUE;
}

int ONScripter::moviestopCommand()
{
    int m = script_h.readInt();
    movie_stop = m < 0 ? -1 : m > 0 ? 1 : 0;
    return RET_CONTINUE;
}

int ONScripter::sysevolCommand()
{
    int v = script_h.readInt();
    syse_volume = v < 0 ? -1 : v > 100 ? 100 : v;
    if (wave_sample[MIX_WAVE_CHANNEL])
        Mix_Volume( MIX_WAVE_CHANNEL, (syse_volume >= 0 ? syse_volume : se_volume) * MIX_MAX_VOLUME / 100 );
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

int ONScripter::gamespeedCommand()
{
    int p = script_h.readInt();
    game_speed = p < 10 ? 10 : p > 10000 ? 10000 : p;
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

// chvolfade and bgmvolfade back to 100, at once (reset, startup).
void ONScripter::resetScriptVolumes()
{
    for (int i=0 ; i<ONS_MIX_CHANNELS ; i++){
        track_gain[i].ms = 0;
        track_gain[i].from = track_gain[i].to = 100;
        track_gain[i].last = 4096;
    }
    bgm_script_from = bgm_script_to = 100;
    bgm_script_ms = 0;
}

// bgmvolfade's volume now, percent.
int ONScripter::bgmScriptVolume()
{
    return scriptVolumeAt( bgm_script_from, bgm_script_to, bgm_script_ms, bgm_script_start, SDL_GetTicks() );
}

// The music's volume, percent: the setting times bgmvolfade's.
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

void ONScripter::cancelLaterBGM()
{
    setStr( &bgm_later_name, NULL );
}

// The ticks of the next step of bgmvolfade's ramp or of bgmlater's start,
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
        // Nine numbers, then optional ones (screenripple's blend, screenshake's end).
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
        if (slot == 0) clearScreenEffects();
        else clearScreenEffect( screen_effect[slot-1] );
        dirty_rect.fill( screen_width, screen_height );
        return RET_CONTINUE;
    }

    ScreenEffect &e = screen_effect[slot-1];
    clearScreenEffect( e );
    e.map_name = map_name;
    for (int i=0 ; i<SCREEN_EFFECT_PARAMS ; i++) e.param[i] = param[i];
    e.kind = kind;
    if (kind == SCREEN_EFFECT_SHAKE){ // gamespeed: its time and end
        e.param[0] = speedTime( e.param[0] );
        e.param[9] = speedTime( e.param[9] );
    }
    startScreenEffect( e );
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
    if (e.kind == SCREEN_EFFECT_RIPPLE){
        e.opacity = p[2];
        e.amp = p[4] << 16;
        e.angle = p[6];
        e.phase = 0;
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
    return min;
}

void ONScripter::proceedScreenEffects( int current_time )
{
    bool changed = false;
    for (int i=0 ; i<MAX_SCREEN_EFFECT ; i++){
        ScreenEffect &e = screen_effect[i];
        if (e.kind == SCREEN_EFFECT_NONE) continue;
        // After a long stop (a menu, a load) go on from now.
        if (current_time - e.next_step > 1000) e.next_step = current_time;
        while (e.next_step <= current_time){
            stepScreenEffect( e );
            e.next_step += e.param[1];
            changed = true;
            // a shake given an end clears its slot when it's over
            if (e.kind == SCREEN_EFFECT_SHAKE && e.param[9] > 0 && e.ran >= e.param[9]){
                clearScreenEffect( e );
                break;
            }
        }
    }
    if (changed){
        SDL_Rect rect = {0, 0, screen_width, screen_height};
        flushDirect( rect, refreshMode() | (draw_cursor_flag ? REFRESH_CURSOR_MODE : 0) );
    }
}

static const int shake_path[8][2] = { // 16.16, the original's table
    {-65536, 32768}, {65536, 49152}, {0, 32768}, {16384, 0},
    {-32768, -32768}, {16384, -16384}, {0, 0}, {-65536, 32768} };

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
            int dx = (int)((ux * e.range_x) >> 32) * screen_ratio1 / screen_ratio2;
            int dy = (int)((uy * e.range_y) >> 32) * screen_ratio1 / screen_ratio2;
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
            for (int y=clip.y ; y<clip.y + clip.h ; y++){
                ONSBuf *dst = buf + y * pitch;
                for (int x=clip.x ; x<clip.x + clip.w ; x++){
                    int m = 0;
                    if (e.map){
                        int mx = x * e.map_w / w, my = y * e.map_h / h;
                        m = e.map[my * e.map_w + mx];
                    }
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
        sp.visible = ai->visible;
        sp.cell = ai->current_cell;
        sp.trans = ai->trans;
        sp.num_of_filters = ai->num_of_filters;
        memcpy(sp.filter, ai->filter, sizeof(sp.filter));
        sp.zoom_x = ai->zoom_x;
        sp.zoom_y = ai->zoom_y;
        sp.z_over = ai->z_over;
    }
    memcpy(st.screen_filter, screen_filter, sizeof(st.screen_filter));

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
        ai->z_over = sp.z_over;
        if (ai->image_surface && ai->visible) dirty_rect.add( zoomedRect( *ai ) );
        if (ai->image_surface && ai->visible) dirty_rect.add( ai->pos );
        delete[] sp.image_name;
        delete[] sp.str;
        delete[] sp.colors;
    }
    delete[] st.sprite;

    if (memcmp(screen_filter, st.screen_filter, sizeof(screen_filter)) != 0){
        memcpy(screen_filter, st.screen_filter, sizeof(screen_filter));
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
    // (bgmvolfade), then the number of dwave channels and each one's
    // (chvolfade; it matters for the looping sounds version 5 restarts).
    writeInt( bgm_script_to, output_flag );
    writeInt( ONS_MIX_CHANNELS, output_flag );
    for (int i=0 ; i<ONS_MIX_CHANNELS ; i++)
        writeInt( track_gain[i].to, output_flag );

    // Version 9: the sprite motions (spmotion): the count, then per sprite
    // its number, loop, key count and keys (ms, x, y). They restart on load.
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].motion_keys > 0) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo *ai = &sprite_info[i];
        if (ai->motion_keys == 0) continue;
        writeInt( i, output_flag );
        writeInt( ai->motion_loop ? 1 : 0, output_flag );
        writeInt( ai->motion_keys, output_flag );
        for (int k=0 ; k<ai->motion_keys ; k++)
            for (int j=0 ; j<3 ; j++) writeInt( ai->motion_key[k][j], output_flag );
    }

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

    // Version 12: the keyed alphas (spalpha): the count, then per sprite
    // its number, alpha now, loop, key count and keys (ms, alpha). Running
    // keys restart on load.
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].alpha_keys > 0 || sprite_info[i].key_alpha != 256) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo *ai = &sprite_info[i];
        if (ai->alpha_keys == 0 && ai->key_alpha == 256) continue;
        writeInt( i, output_flag );
        writeInt( ai->key_alpha, output_flag );
        writeInt( ai->alpha_loop, output_flag );
        writeInt( ai->alpha_keys, output_flag );
        for (int k=0 ; k<ai->alpha_keys ; k++)
            for (int j=0 ; j<2 ; j++) writeInt( ai->alpha_key[k][j], output_flag );
    }

    // Version 13: the motions' (version 9's, in the same order) keep flag
    // and each key's zoom, angle and alpha (spmotion2).
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo *ai = &sprite_info[i];
        if (ai->motion_keys == 0) continue;
        writeInt( ai->motion_keep ? 1 : 0, output_flag );
        for (int k=0 ; k<ai->motion_keys ; k++){
            writeInt( ai->motion_key[k][3], output_flag );
            writeInt( ai->motion_key[k][4], output_flag );
            writeInt( ai->motion_key[k][5], output_flag );
        }
    }

    // Version 14: bgmlater's music ("" for none), the time left and loop.
    writeStr( bgm_later_name, output_flag );
    Uint32 now = SDL_GetTicks();
    writeInt( bgm_later_name && bgm_later_time > now ? bgm_later_time - now : 0, output_flag );
    writeInt( bgm_later_loop ? 1 : 0, output_flag );

    // Version 15: the additive sprites (spblend): the count, then their
    // numbers.
    num = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].blending_mode == AnimationInfo::BLEND_ADD) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++)
        if (sprite_info[i].blending_mode == AnimationInfo::BLEND_ADD) writeInt( i, output_flag );
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

/* A sound cut off (a voice by the next one, dwavestop, stop, a load) used
 * to stop at once, mid-wave: a step in the output, heard as a pop. Instead,
 * the next CUT_FADE_MS of it fade out on a channel of their own, starting
 * where it was cut, while its channel is free at once for the next sound.
 * Where a channel is in its sound is counted by an effect that sees what
 * each mix takes from it (SDL_mixer's own fades change the volume once a
 * buffer, ~90 ms, too coarse to help). Music (Mix_Music) can't be read
 * ahead, so it still stops at once. */
#define CUT_FADE_MS 10
static struct { Mix_Chunk *chunk; Uint32 pos; bool loop; } cut_track[ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS];
static struct { Mix_Chunk *chunk; Uint8 *buf; } cut_fade[ONS_MIX_FADE_CHANNELS];
static int cut_fade_next = 0;

static void cutTrackEffect( int channel, void *stream, int len, void *udata )
{
    if (channel < 0 || channel >= ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS) return;
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
    }
    Mix_Chunk *chunk = cut_track[channel].chunk;
    if (!chunk || chunk->alen == 0) return;
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
    if (channel < ONS_MIX_CHANNELS) // the ramp from where it is
        track_gain[channel].last = trackGain( channel );
    track_gain_s16 = audio_format.format == AUDIO_S16SYS;
    Mix_RegisterEffect( channel, cutTrackEffect, NULL, NULL ); // reset when a sound ends
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

    int n = cut_fade_next++ % ONS_MIX_FADE_CHANNELS;
    int fade_channel = ONS_MIX_CHANNELS + ONS_MIX_EXTRA_CHANNELS + n;
    Mix_HaltChannel( fade_channel );
    if (cut_fade[n].chunk) Mix_FreeChunk( cut_fade[n].chunk );
    delete[] cut_fade[n].buf;
    cut_fade[n].buf = raw;
    cut_fade[n].chunk = Mix_QuickLoad_RAW( cut_fade[n].buf, frames * frame );
    if (!cut_fade[n].chunk) return;
    Mix_Volume( fade_channel, Mix_Volume( channel, -1 ) );
    Mix_PlayChannel( fade_channel, cut_fade[n].chunk, 0 );
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
        sprite_info[i].blending_mode = AnimationInfo::BLEND_NORMAL; // older saves have none
    }
    dirty_rect.fill( screen_width, screen_height );
    setPlaytime( 0 ); // older saves have none
    if (readInt() != CONS_SAVE_MAGIC) return;
    int version = readInt();
    if (version < 1) return;

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

    if (version < 2) return;
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

    if (version < 3) return;
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

    if (version < 4) return;
    slots = readInt();
    int params = readInt();
    for (int i=0 ; i<slots ; i++){
        ScreenEffect dummy;
        memset( &dummy, 0, sizeof(dummy) );
        ScreenEffect &e = (i < MAX_SCREEN_EFFECT) ? screen_effect[i] : dummy;
        int kind = readInt();
        readStr( &e.map_name );
        for (int k=0 ; k<params ; k++){
            int n = readInt();
            if (k < SCREEN_EFFECT_PARAMS) e.param[k] = n;
        }
        e.kind = kind;
        if (kind != SCREEN_EFFECT_NONE) startScreenEffect( e );
        if (&e == &dummy) clearScreenEffect( dummy );
    }

    if (version < 5) return;
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

    if (version < 6) return;
    depth = readInt();
    for (int n=0 ; n<depth ; n++){
        num = readInt();
        for (int m=0 ; m<num ; m++){
            int i = readInt();
            ScreenSprite sp;
            sp.image_name = NULL;
            sp.zoom_x = sp.zoom_y = 100;
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

    if (version < 7) return;
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

    if (version < 8) return;
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

    if (version < 9) return;
    std::vector< std::pair<AnimationInfo*, int> > motions; // version 13 adds to them
    AnimationInfo motion_dummy;
    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt();
        bool loop = readInt() != 0;
        int keys = readInt();
        AnimationInfo *ai = (no >= 0 && no < MAX_SPRITE_NUM) ? &sprite_info[no] : &motion_dummy;
        ai->motion_keys = 0;
        ai->motion_loop = loop;
        ai->motion_keep = false;
        for (int k=0 ; k<keys ; k++){
            int t = readInt(), x = readInt(), y = readInt();
            if (k >= AnimationInfo::MAX_MOTION_KEYS) continue;
            ai->motion_key[k][0] = t; ai->motion_key[k][1] = x; ai->motion_key[k][2] = y;
            ai->motion_key[k][3] = 100; ai->motion_key[k][4] = 0; ai->motion_key[k][5] = 256;
            ai->motion_keys++;
        }
        ai->motion_start = ai->motion_next = SDL_GetTicks();
        stepSpriteMotion( *ai, ai->motion_start );
        motions.push_back( std::make_pair( ai, keys ) );
    }

    if (version < 10) return;
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

    if (version < 11) return;
    setPlaytime( readInt() );

    if (version < 12) return;
    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt();
        int alpha = readInt();
        int loop = readInt();
        int keys = readInt();
        AnimationInfo dummy;
        AnimationInfo *ai = (no >= 0 && no < MAX_SPRITE_NUM) ? &sprite_info[no] : &dummy;
        ai->key_alpha = alpha;
        ai->alpha_keys = 0;
        ai->alpha_loop = loop;
        for (int k=0 ; k<keys ; k++){
            int t = readInt(), a = readInt();
            if (k >= AnimationInfo::MAX_MOTION_KEYS) continue;
            ai->alpha_key[k][0] = t; ai->alpha_key[k][1] = a;
            ai->alpha_keys++;
        }
        ai->alpha_start = ai->alpha_next = SDL_GetTicks();
        stepSpriteAlpha( *ai, ai->alpha_start );
    }

    if (version < 13) return;
    for (size_t n=0 ; n<motions.size() ; n++){
        AnimationInfo *ai = motions[n].first;
        ai->motion_keep = readInt() != 0;
        for (int k=0 ; k<motions[n].second ; k++){
            int zoom = readInt(), rot = readInt(), alpha = readInt();
            if (k >= AnimationInfo::MAX_MOTION_KEYS) continue;
            ai->motion_key[k][3] = zoom; ai->motion_key[k][4] = rot; ai->motion_key[k][5] = alpha;
        }
        stepSpriteMotion( *ai, ai->motion_start );
    }

    if (version < 14) return;
    readStr( &bgm_later_name );
    bgm_later_time = SDL_GetTicks() + readInt();
    bgm_later_loop = readInt() != 0;

    if (version < 15) return;
    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt();
        if (no >= 0 && no < MAX_SPRITE_NUM) sprite_info[no].blending_mode = AnimationInfo::BLEND_ADD;
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
    if (info != &sentence_font || text_size_percent == 100) return info;
    scaled = *info;
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
        if (*p == '@' || *p == '\\' || *p == '#' || *p == '(' || (*p == '!' && strchr("swdz", p[1]) && p[1])) break;
        w += info->pitch_xy[0]/2 + hankakuExtra(glyph_info, *p);
    }
    return info->isEndOfLine(0, w - 1);
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
 * tag's transparency (":a" takes the right half as the alpha), spalpha,
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
    FILE *fp = fopen( path.c_str(), "rb" ); // (the web build fetches it here)
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

/* logrows %n,columns
 *   Lays the text history out as rows, oldest first, and puts how many
 *   there are in %n: each page's lines, cut at columns full-width
 *   characters (a half-width one takes half), with ruby shown as its
 *   base text. A page with no text has no rows.
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

    log_rows.clear();
    bool utf8 = coding2utf16->force_utf8;
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
                        int w = (n > 1) ? 2 : 1;
                        if (units + w > columns * 2) put();
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
            int w = (n > 1) ? 2 : 1;
            if (units + w > columns * 2) put();
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
