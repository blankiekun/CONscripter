/* -*- C++ -*-
 *
ONScripter_spriteeffects.cpp - CONscripter: sprite motion and looks (spmotion, spfilter, spblend, spmask, spcut, spgrab, ...)
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
 *   lightness inverted (a negative that keeps the hue); 9 levels: each
 *   param (r, g, b) is gamma*65536 + low*256 + high, gamma in 1/1000, and
 *   the channel v becomes low + (high-low) * (v/255)^(1000/gamma) (gamma
 *   0: low); 10 the same levels on the greyscale (54 r + 183 g + 19 b)/256;
 *   11 the grey of the weights param r,g,b (in 1/256, each term rounded
 *   down): "spfilter 10,11,0,256,77,150,29" is kind 2 to within a step.
 *   blend combines that colour with the pixel's: 0 copy (default),
 *   1 multiply, 2 add, 3 screen, 4 overlay, 5 colour dodge. strength 0-256 (default 256) is how much
 *   of the result replaces the pixel. So "spfilter 10,1,1,256,255,200,160"
 *   tints sprite 10 warm, and "spfilter 10,1,0,256,0,0,0" makes it a black
 *   silhouette.
 *   A sprite without an image ignores spfilter, like the original
 *   engines' filters, which belong to a loaded object. The filters stay
 *   when lsp changes the image and are cleared by csp. A new filter is
 *   applied to the picture as it is; clearing or a filter that can't be
 *   is applied when the sprite is next drawn, so "spfilter 10,0" before
 *   an lsp of a new picture costs nothing. */
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
    else if (f.blend < AnimationInfo::FILTER_BLEND_COPY || f.blend > AnimationInfo::FILTER_BLEND_DODGE){
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
        // A picture with its filters so far gets the new one on top, as a
        // reload would make it (filters go before the screen's scaling).
        if (!ai->filter_changed && ai->image_surface && ai->trans_mode != AnimationInfo::TRANS_STRING &&
#ifdef USE_BUILTIN_LAYER_EFFECTS
            ai->trans_mode != AnimationInfo::TRANS_LAYER &&
#endif
            screen_ratio1 == screen_ratio2 && ai->file_name && !isMovieFile( ai->file_name )){
            SDL_Rect all = {0, 0, ai->image_surface->w, ai->image_surface->h};
            AnimationInfo::applyFilterList( ai->image_surface, all, &f, 1 );
        }
        else
            ai->filter_changed = true;
    }

    // Otherwise the picture is made again when it is next drawn
    // (drawTaggedSurface): an lsp before that loads it only once.
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
 *   "spmotion num" stops it (back at its place); "spmotion num,2" stops it
 *   where it is now, as loop 2 leaves its last key. Up to 64 keys. The motion stays when
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
    bool has_mode = script_h.getEndStatus() & ScriptHandler::END_COMMA;
    int mode = has_mode ? script_h.readInt() : 0;
    bool has_keys = has_mode && (script_h.getEndStatus() & ScriptHandler::END_COMMA);
    if (mode == 2 && !has_keys && ai->motion_keys > 0){
        // stopped where it is now: its pose at this time applied to the sprite
        int last = ai->motion_key[ai->motion_keys-1][0], t = SDL_GetTicks() - ai->motion_start;
        if (ai->motion_loop && last > 0) t = ai->loopTime( t );
        else if (ai->motion_passes > 1 && last > 0 && t < last * ai->motion_passes) t %= last;
        else if (t > last) t = last;
        int v[7];
        motionAt( *ai, ai->motion_keys, t, v );
        applyMotion( *ai, v );
    }
    else endKeptMotion( *ai );
    traceMotion( *ai, has_keys ? "replaced" : "stopped" );
    ai->stopMotion();
    ai->trail_keys = 0;
    if (has_keys){
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
        if (ai->motion_clock >= 0) ai->motion_start -= ai->motion_clock; // spmotionclock
        traceMotion( *ai, NULL );
        ai->motion_clock = -1;
        stepSpriteMotion( *ai, ai->motion_next ); // now: spmotionclock may have started it part-way
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

/* spmotionclock num,src
 *   The next spmotion with keys on sprite num starts as far in as sprite
 *   src's motion has run now (from its start if src has none): a motion
 *   handed to another sprite goes on with its clock, src stopped after
 *   this or not. src negative (-ms): it starts ms into its keys (at
 *   gamespeed, as the keys), for a motion resumed part-way. csp on num
 *   drops it.
 *       spmotionclock 11,10:spmotion 10:lsp 11,"a.png",0,0:spmotion 11,1,...
 *       spmotionclock 10,-1500:spmotion 10,2,... */
int ONScripter::spmotionclockCommand()
{
    int no = script_h.readInt();
    int src = script_h.readInt();
    if (no < 0 || no >= MAX_SPRITE_NUM || src >= MAX_SPRITE_NUM){
        utils::printError("spmotionclock: sprite %d or %d out of range\n", no, src);
        return RET_CONTINUE;
    }
    if (src < 0){ // -ms: that far into its keys
        sprite_info[no].motion_clock = speedTime( -src );
        return RET_CONTINUE;
    }
    AnimationInfo &s = sprite_info[src];
    sprite_info[no].motion_clock = s.motion_keys > 0 ? (int)SDL_GetTicks() - s.motion_start : -1;

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

/* effectcurve curve
 *   Eases the next effect (print or an effect command with a time) by
 *   curve, as spmotion's (1-3 ease in, out, in and out; 10-73 an spcurve
 *   table): its frame at a part p of the time is drawn as at curve(p).
 *   Used once, by the effect that follows; 0 is straight. Not saved.
 *   "effectcurve 2:print 10,1000" (a cross-fade that slows down). */
int ONScripter::effectcurveCommand()
{
    effect_curve = script_h.readInt();

    return RET_CONTINUE;
}

/* effectsoft softness[,reverse[,form]]
 *   The next masked effect (15, 18) sweeps a threshold over the mask's grey
 *   levels instead of the 256-level ramp: at part t of the time (eased by
 *   effectcurve) the threshold is p = 1 + floor(255 t); the new screen shows
 *   fully where the mask is at most p and fades out over the levels above it,
 *   losing 255 x 255 / (softness x p) a level (a band about softness x p / 255
 *   levels wide). reverse 1: the threshold falls (p = 254 - floor(255 t)) and
 *   the new screen shows where the mask is above it: the old one leaves the
 *   bright parts first. Nitroplus's DrawTransition (Muramasa.exe 0044fe00,
 *   00457bf0). Used once, by the effect that follows; not saved.
 *   form 1: Full Metal Daemon Muramasa's (Mware's psUpdateTransitionRule):
 *   at part n = t (1 - t reversed) a mask level u = grey/255 shows at
 *   sat(k(n - u) + u n), k = 2000 / softness: a band about 255 / k levels
 *   wide all the way (reversed, the new screen shows where it doesn't).
 *   "effectsoft 100:print 18,1000,"mask.png"". */
int ONScripter::effectsoftCommand()
{
    effect_soft = script_h.readInt();
    if (effect_soft < 0) effect_soft = 0;
    effect_soft_reverse = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : 0;
    effect_soft_form = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : 0;

    return RET_CONTINUE;
}

/* The new screen over the old through the mask's threshold table
 * (effectsoft); src1/src2/dst as alphaBlend's. */
void ONScripter::softMaskBlend( SDL_Surface *mask, int counter, int duration, SDL_Rect *clip )
{
    SDL_Rect rect = screen_rect;
    if ( clip && AnimationInfo::doClipping( &rect, clip ) ) return;
    if ( mask->format->BytesPerPixel != 4 ) return;

    // The exe's table (00457bf0): 255 up to the threshold, then a ramp down
    // by k a level, truncated (fistp under a truncating control word).
    int n = duration > 0 ? (int)((long long)255 * counter / duration) : 255;
    if (n > 255) n = 255;
    int p = effect_soft_reverse ? 254 - n : n + 1;
    double k = 255.0 / (p > 0 ? p : 1) * (255.0 / effect_soft);
    Uint8 lut[256];
    for (int g = 0; effect_soft_form == 1 && g < 256; g++){
        // psUpdateTransitionRule: g_TransitionNow, 2 / g_TransitionRate
        double now = duration > 0 ? (double)counter / duration : 1.0;
        if (now > 1.0) now = 1.0;
        if (effect_soft_reverse) now = 1.0 - now;
        double u = g / 255.0, k = 2000.0 / effect_soft;
        double v = k * (now - u) + u * now;
        int a = v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255 + 0.5);
        lut[g] = effect_soft_reverse ? 255 - a : a;
    }
    for (int g = 0; effect_soft_form != 1 && g < 256; g++){
        int a = 255;
        if (g > p){
            double v = 255.0 - (g - p) * k;
            a = v < 0 ? 0 : (int)v;
        }
        if (p < 0) a = 0;
        lut[g] = effect_soft_reverse ? 255 - a : a;
    }

    SDL_Surface *src1 = effect_src_surface, *src2 = effect_dst_surface, *dst = accumulation_surface;
    SDL_LockSurface(src1); SDL_LockSurface(src2); SDL_LockSurface(dst); SDL_LockSurface(mask);
    for (int y = rect.y; y < rect.y + rect.h; y++){
        ONSBuf *s1 = (ONSBuf *)src1->pixels + src1->w * y;
        ONSBuf *s2 = (ONSBuf *)src2->pixels + src2->w * y;
        ONSBuf *d = (ONSBuf *)dst->pixels + dst->w * y;
        const Uint32 *m = (const Uint32 *)((Uint8 *)mask->pixels + mask->pitch * (y % mask->h));
        // the mask's column wraps by a counter: no divide per pixel (Vita/3DS CPUs have none)
        for (int x = rect.x, mx = rect.x % mask->w; x < rect.x + rect.w; x++, mx = mx + 1 == mask->w ? 0 : mx + 1){
            Uint32 a = lut[m[mx] & 0xFF];
            a += a >> 7; // 255 -> 256: the new screen exactly
            Uint32 c1 = s1[x], c2 = s2[x];
            Uint32 rb = ((((c2 & 0xff00ff) - (c1 & 0xff00ff)) * a >> 8) + (c1 & 0xff00ff)) & 0xff00ff;
            Uint32 g = ((((c2 & 0x00ff00) - (c1 & 0x00ff00)) * a >> 8) + (c1 & 0x00ff00)) & 0x00ff00;
            d[x] = rb | g;
        }
    }
    SDL_UnlockSurface(mask); SDL_UnlockSurface(dst); SDL_UnlockSurface(src2); SDL_UnlockSurface(src1);
}


// How far (0-1024) along from one key to the next a motion is at p (0-1024)
// of the time between them, by the next key's curve.
int ONScripter::curveAt( int curve, int p )
{
    return curveShare( curve_table, curve, p );
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


/* spgrab num[,src[,x,y,w,h]]
 *   Puts a copy of a picture into lsp sprite num, which is shown where the
 *   copy was taken: src -1 (default) the screen as the sprites and the
 *   background draw it now (without the text window, sprite num left
 *   out), src -2 the screen as last shown (as a print shows it: what a
 *   screen closed with hold left), src -3 the screen under sprite num (the
 *   background and the sprites drawn before num, which must be below 1000:
 *   a copy of what lies under a picture, to zoom or blend), src n lsp sprite n's picture (its cell now), each from (x, y)
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
    if (src == -2) // the last frame shown, as it is
        from = SDL_ConvertSurface( accumulation_surface, accumulation_surface->format, 0 );
    else if (src == -3){ // the screen under sprite num: the background and the sprites drawn before it
        from = SDL_ConvertSurface( accumulation_surface, accumulation_surface->format, 0 );
        SDL_Rect all = {0, 0, from->w, from->h};
        SDL_BlitSurface( bg_info.image_surface, &all, from, &all );
        if (!all_sprite_hide_flag && no + 1 < NS_SPRITE_NUM)
            drawSpriteRange( from, all, NS_SPRITE_NUM-1, no+1 );
    }
    else if (src < 0){
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

/* spclip's feather: the bands of the screen rectangle r within f pixels
 * of its edges, inside clip, without overlaps. Before a sprite is drawn
 * (apply false) their pixels are saved; after (apply true) each goes
 * back toward its saved value by 1 − m, m the fade-in at its place
 * (3t² − 2t³ of its distance from each edge over f, the two multiplied).
 * With a colour (bit 24 set, 0xRRGGBB below) it goes toward the colour
 * instead and nothing is saved. A 32-bit surface only. */
void ONScripter::featherBands( SDL_Surface *surface, const SDL_Rect &r, int f, int colour, const SDL_Rect &clip, std::vector<Uint32> &saved, bool apply )
{
    if (surface->format->BytesPerPixel != 4 || f <= 0) return;
    bool tinted = (colour & 0x1000000) != 0;
    if (tinted && !apply) return;
    Uint32 tint = tinted ? SDL_MapRGBA( surface->format, (colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff, 0xff ) : 0;
    int fy = f < r.h ? f : r.h, fx = f < r.w ? f : r.w;
    int top_end = r.y + fy, bottom = r.y + r.h - f > top_end ? r.y + r.h - f : top_end;
    int left_end = r.x + fx, right = r.x + r.w - f > left_end ? r.x + r.w - f : left_end;
    SDL_Rect bands[4] = { {r.x, r.y, r.w, top_end - r.y}, {r.x, bottom, r.w, r.y + r.h - bottom},
                          {r.x, top_end, left_end - r.x, bottom - top_end}, {right, top_end, r.x + r.w - right, bottom - top_end} };
    auto ramp = [f]( int d ){ // d: pixels from the edge, 0 at the edge's own pixel
        float t = (d + 0.5f) / f;
        if (t >= 1) return 1.0f;
        return t * t * (3 - 2 * t);
    };
    if (!apply) saved.clear();
    size_t at = 0;
    SDL_LockSurface( surface );
    for (SDL_Rect &b : bands){
        SDL_Rect in;
        if (b.w <= 0 || b.h <= 0 || !SDL_IntersectRect( &b, &clip, &in )) continue;
        for (int y=in.y ; y<in.y+in.h ; y++){
            Uint32 *p = (Uint32*)((Uint8*)surface->pixels + y * surface->pitch) + in.x;
            int dy = y - r.y < r.y + r.h - 1 - y ? y - r.y : r.y + r.h - 1 - y;
            float my = ramp( dy );
            for (int x=in.x ; x<in.x+in.w ; x++, p++){
                if (!apply){ saved.push_back( *p ); continue; }
                Uint32 old = tinted ? tint : saved[at++];
                int dx = x - r.x < r.x + r.w - 1 - x ? x - r.x : r.x + r.w - 1 - x;
                int m = (int)(ramp( dx ) * my * 256 + 0.5f);
                if (m >= 256) continue;
                Uint32 now = *p, out = 0;
                for (int sh=0 ; sh<32 ; sh+=8){
                    int o = (old >> sh) & 0xff, n = (now >> sh) & 0xff;
                    out |= (Uint32)(o + ((n - o) * m >> 8)) << sh;
                }
                *p = out;
            }
        }
    }
    SDL_UnlockSurface( surface );
}

/* spclip num,x,y,w,h[,feather[,colour]]
 *   Draws lsp sprite num only inside the screen's rectangle x, y, w, h
 *   (its picture is cut there wherever the sprite moves: a window its
 *   children show through); with a feather, it fades in from each edge
 *   over that many pixels (smoothly: 3t² − 2t³, a corner the product of
 *   its two edges'), from the colour ("#rrggbb") when one is given (a
 *   soft frame of that colour inside the rectangle), else from what is
 *   under it. "spclip num" draws it everywhere again. It
 *   stays when lsp changes the image, is removed by csp and is kept in
 *   save files and by pushscreen.
 *   lsp 20,"tape.png",262,-220:spclip 20,260,138,100,300 */
int ONScripter::spclipCommand()
{
    int no = script_h.readInt();
    int r[6] = {0, 0, 0, 0, 0, 0};
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA)
        for (int k=0 ; k<4 ; k++) r[k] = script_h.readInt();
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA) r[4] = script_h.readInt();
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA){
        uchar3 c;
        readColor( &c, script_h.readStr() );
        r[5] = 0x1000000 | (c[0] << 16) | (c[1] << 8) | c[2];
    }
    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spclip: sprite %d out of range\n", no);
        return RET_CONTINUE;
    }
    AnimationInfo &ai = sprite_info[no];
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );
    for (int k=0 ; k<6 ; k++) ai.clip_rect[k] = r[k];
    if (ai.clip_rect[2] <= 0 || ai.clip_rect[3] <= 0) ai.clip_rect[2] = ai.clip_rect[3] = 0;
    if (ai.clip_rect[2] == 0 || ai.clip_rect[4] <= 0) ai.clip_rect[4] = ai.clip_rect[5] = 0;
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );

    return RET_CONTINUE;
}

/* spmask num[,"mask"[,x,y]]
 *   Draws lsp sprite num times the mask picture laid on the screen with
 *   its top left at (x, y) (default 0, 0): at each screen pixel the
 *   sprite counts for the mask's alpha × its grey (white: all of it,
 *   black or clear: none; outside the mask: none), whatever the sprite
 *   does under it (spmotion, spzoom, spwave), and whatever its blend.
 *   "spmask num" (or "") draws it whole again. It stays when lsp changes
 *   the image, is removed by csp and is kept in save files (not by
 *   pushscreen). Nitroplus's partial DrawTransition, read at the screen
 *   pixel while its picture moves:
 *   lsp 30,"rain.png",880,32:spmask 30,"veil.png":spmotion 30,1,... */
int ONScripter::spmaskCommand()
{
    int no = script_h.readInt();
    std::string name;
    int x = 0, y = 0;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA) name = script_h.readStr();
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA){
        x = script_h.readInt();
        y = script_h.readInt();
    }
    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spmask: sprite %d out of range\n", no);
        return RET_CONTINUE;
    }
    AnimationInfo &ai = sprite_info[no];
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );
    ai.fade_on = false; // (ends spfadeto's)
    ai.fade_frame = -1;
    ai.screen_mask = -1; // (frees its slot for the one loaded now)
    ai.screen_mask = name.empty() ? -1 : screenMaskSlot( name.c_str() );
    ai.screen_mask_xy[0] = x;
    ai.screen_mask_xy[1] = y;
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );

    return RET_CONTINUE;
}

// spmask's mask name loaded into a slot of screen_masks (one no sprite
// uses is reused); -1 when it can't be loaded.
int ONScripter::screenMaskSlot( const char *name )
{
    int free_slot = -1;
    for (size_t k=0 ; k<screen_masks.size() ; k++){
        if (screen_masks[k].first == name) return (int)k;
        bool used = false;
        for (int i=0 ; i<MAX_SPRITE_NUM && !used ; i++) used = sprite_info[i].screen_mask == (int)k;
        if (!used && free_slot < 0) free_slot = (int)k;
    }
    SDL_Surface *loaded = loadImage( (char*)name );
    SDL_Surface *m = loaded ? SDL_ConvertSurfaceFormat( loaded, SDL_PIXELFORMAT_ARGB8888, 0 ) : NULL;
    if (loaded) SDL_FreeSurface( loaded );
    if (!m){
        utils::printError("spmask: mask %s can't be loaded\n", name);
        return -1;
    }
    if (free_slot < 0){
        free_slot = (int)screen_masks.size();
        screen_masks.push_back( std::make_pair( std::string(), (SDL_Surface*)NULL ) );
    }
    if (screen_masks[free_slot].second) SDL_FreeSurface( screen_masks[free_slot].second );
    screen_masks[free_slot] = std::make_pair( std::string( name ), m );
    return free_slot;
}

/* spfadeto num,"mask",ms,from,to,softness[,form[,curve]]
 *   Fades lsp sprite num through the mask's grey levels, from level from
 *   to level to (0-1000, 1000 whole), over ms while the script goes on:
 *   the mask (its green), stretched over the screen and fixed there, is
 *   read through a table that shows the sprite where the grey is at most
 *   the level × 0.255 and fades it out over the levels above it, losing
 *   255 × 255 ÷ (softness × level) a grey (softness 0: a hard edge).
 *   The table is made at 60 frames a second as Muramasa.exe makes it
 *   (transition_start 0044fe00, each frame 00457bf0), so a fade that
 *   falls keeps what the start's ramp left above from. form 1: Full
 *   Metal Daemon Muramasa's (Mware's psUpdateTransitionRule): grey u
 *   (0-1) shows sat(k(n − u) + u·n), k = 2000 ÷ softness, n the level
 *   ÷ 1000. curve as spmotion's. Ended at 1000 the sprite is drawn whole
 *   (as "spmask num"); at another level it stays masked. Drawn by spmask's
 *   path: spmask and csp end it; it stays through lsp and is saved
 *   (going on after a load). Nitroplus's DrawTransition:
 *   lsp 20,"a.png",0,0:spfadeto 20,"wipe.png",800,0,1000,100:print 1 */
int ONScripter::spfadetoCommand()
{
    int no = script_h.readInt();
    std::string name = script_h.readStr();
    int v[6] = {0, 0, 0, 0, 0, 0};
    for (int k=0 ; k<6 && (script_h.getEndStatus() & ScriptHandler::END_COMMA) ; k++) v[k] = script_h.readInt();
    if (no < 0 || no >= MAX_SPRITE_NUM){
        utils::printError("spfadeto: sprite %d out of range\n", no);
        return RET_CONTINUE;
    }
    AnimationInfo &ai = sprite_info[no];
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );
    ai.fade_on = false;
    ai.fade_frame = -1;
    ai.screen_mask = -1; // (frees its slot for the one loaded now)
    ai.screen_mask = screenMaskSlot( name.c_str() );
    ai.screen_mask_xy[0] = ai.screen_mask_xy[1] = 0;
    if (ai.screen_mask < 0) return RET_CONTINUE; // (the exe draws no transition without its mask)
    ai.fade_on = true;
    ai.fade_from = v[1] < 0 ? 0 : v[1] > 1000 ? 1000 : v[1];
    ai.fade_to = v[2] < 0 ? 0 : v[2] > 1000 ? 1000 : v[2];
    ai.fade_soft = v[3] < 0 ? 0 : v[3];
    ai.fade_form = v[4];
    ai.fade_curve = v[5];
    ai.fade_frames = v[0] * 60 / 1000 > 1 ? v[0] * 60 / 1000 : 1;
    ai.fade_ms = v[0] > 0 ? speedTime( v[0] ) : 0; // gamespeed
    ai.fade_start = SDL_GetTicks();
    fadeToFrame( ai, 0 );
    if ((auto_play_flag && !auto_play_real) || still_screen) ai.fade_start -= ai.fade_ms;
    stepSpriteFade( ai, SDL_GetTicks() );
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );

    return RET_CONTINUE;
}

// spfadeto's table made up to frame k of its fade (0: the start's).
void ONScripter::fadeToFrame( AnimationInfo &ai, int k )
{
    unsigned char *t = ai.fade_table;
    int c = curveAt( ai.fade_curve, (int)((long long)k * 1024 / ai.fade_frames) );
    if (ai.fade_form == 1){
        double now = (ai.fade_from + (ai.fade_to - ai.fade_from) * c / 1024.0) / 1000.0;
        for (int g=0 ; g<256 ; g++){
            double u = g / 255.0;
            double x = ai.fade_soft > 0 ? 2000.0 / ai.fade_soft * (now - u) + u * now : (u <= now ? 1 : 0);
            t[g] = x <= 0 ? 0 : x >= 1 ? 255 : (int)(x * 255 + 0.5);
        }
        ai.fade_frame = k;
        return;
    }
    // Muramasa.exe: levels a, b = from, to × 0.255; per = 255 ÷ softness.
    int a = (int)(ai.fade_from * 0.255), b = (int)(ai.fade_to * 0.255);
    double per = ai.fade_soft > 0 ? 255.0 / ai.fade_soft : 1e9;
    // 255 from i on, less by step a grey, until below 0; returns where it stopped.
    auto ramp = [t]( int i, double step ){
        for (double v=255 ; i<=255 ; i++, v-=step){
            t[i] = (unsigned char)v;
            if (v - step < 0){ i++; break; }
        }
        return i;
    };
    for (int f=ai.fade_frame < 0 || k == 0 ? 0 : ai.fade_frame + 1 ; f<=k ; f++){
        if (f == 0){
            for (int i=0 ; i<256 ; i++)
                t[i] = a < b ? (i < a ? 255 : 0) : (b < a && a < i ? 0 : 255);
            if (a != 0) ramp( a, per * 255 / a );
            continue;
        }
        int d = (int)((long long)abs( a - b ) * curveAt( ai.fade_curve, (int)((long long)f * 1024 / ai.fade_frames) ) / 1024);
        int i = a, p;
        if (a < b){
            for ( ; i < d + a + 1 && i <= 255 ; i++) t[i] = 255;
            p = d + a + 1;
        }
        else {
            for ( ; i > a - d - 1 && i >= 0 ; i--) t[i] = 0;
            p = a - d - 1 == 0 ? 1 : a - d - 1;
        }
        if (i < 0) continue;
        i = ramp( i, 255.0 / p * per );
        for ( ; i <= 255 && t[i] != 0 ; i++) t[i] = 0;
    }
    ai.fade_frame = k;
}

// Steps spfadeto's table to the time; at its end drawn whole (level 1000)
// or none (0). Says whether the table changed.
bool ONScripter::stepSpriteFade( AnimationInfo &ai, int current_time )
{
    if (!ai.fade_on || ai.fade_frame < 0) return false;
    int e = current_time - ai.fade_start;
    int k = ai.fade_ms <= 0 || e >= ai.fade_ms ? ai.fade_frames : (int)((long long)e * ai.fade_frames / ai.fade_ms);
    if (k <= ai.fade_frame && k < ai.fade_frames) return false;
    fadeToFrame( ai, k );
    if (k >= ai.fade_frames){
        ai.fade_frame = -1;
        if (ai.fade_to >= 1000){ // (the exe drops the table, 00457c58)
            ai.fade_on = false;
            ai.screen_mask = -1;
        }
        else if (ai.fade_to * 255 / 1000 == 0 && ai.fade_form != 1) memset( ai.fade_table, 0, 256 );
    }
    return true;
}

// spmask's draw: before the sprite is drawn (apply false) the pixels of
// clip are saved; after it (apply true) each goes back from what was
// drawn towards what was there by the mask at that screen pixel.
void ONScripter::screenMaskBlend( SDL_Surface *surface, AnimationInfo *anim, const SDL_Rect &clip, std::vector<Uint32> &saved, bool apply )
{
    if (surface->format->BytesPerPixel != 4 || anim->screen_mask >= (int)screen_masks.size()) return;
    SDL_Surface *m = screen_masks[anim->screen_mask].second;
    SDL_Rect in = clip, all = { 0, 0, surface->w, surface->h };
    if (!SDL_IntersectRect( &clip, &all, &in )) return;
    if (!apply) saved.resize( (size_t)in.w * in.h );
    size_t at = 0;
    SDL_LockSurface( surface );
    for (int y=in.y ; y<in.y+in.h ; y++){
        Uint32 *p = (Uint32*)((Uint8*)surface->pixels + y * surface->pitch) + in.x;
        int my = y * screen_ratio2 / screen_ratio1 - anim->screen_mask_xy[1];
        Uint32 *mrow = (my >= 0 && my < m->h) ? (Uint32*)((Uint8*)m->pixels + my * m->pitch) : NULL;
        for (int x=in.x ; x<in.x+in.w ; x++, p++, at++){
            if (!apply){ saved[at] = *p; continue; }
            int mx = x * screen_ratio2 / screen_ratio1 - anim->screen_mask_xy[0];
            int k = 0; // 0-255: how much of the drawn pixel stays
            if (anim->fade_on){ // spfadeto: stretched over the screen, its green through the table, clear black
                Uint32 q = ((Uint32*)((Uint8*)m->pixels + (y * m->h / screen_height) * m->pitch))[x * m->w / screen_width];
                k = (q >> 24) ? anim->fade_table[(q >> 8) & 0xff] : anim->fade_table[0];
            }
            else if (mrow && mx >= 0 && mx < m->w){
                Uint32 q = mrow[mx];
                k = (q >> 24) * ((((q >> 16) & 0xff) + ((q >> 8) & 0xff) + (q & 0xff)) / 3) / 255;
            }
            if (k == 255) continue;
            Uint32 old = saved[at], now = *p, out = 0;
            for (int sh=0 ; sh<32 ; sh+=8){
                int o = (old >> sh) & 0xff, n = (now >> sh) & 0xff;
                out |= (Uint32)(o + (n - o) * k / 255) << sh;
            }
            *p = out;
        }
    }
    SDL_UnlockSurface( surface );
}

/* CONscripter
 * sptrail num,passes,alpha,lag[,over]
 *   Draws passes (1-32, SPTRAIL_MAX) copies of lsp sprite num under it (over
 *   it when over is 1, as Nitroplus's SetBlur(…, false) does), each at
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
    int v[4] = {0, 0, 0, 0};
    for (int k=0 ; k<4 && (script_h.getEndStatus() & ScriptHandler::END_COMMA) ; k++) v[k] = script_h.readInt();
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
    ai.trail_over = on && v[3] != 0;
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
 *   Waves lsp sprite num: each row of its picture is drawn
 *   trunc(sin(2 pi phase / 256) * amp) px to the right, phase being
 *   row * period / height (in 1/256 turns; period 256: one wave over the
 *   picture's height) plus one every 33 ms; amp goes from amp0 to amp1 px
 *   over ms ms by curve (as spmotion's: 0 straight, 1-3 ease, spcurve's),
 *   then stays at amp1; the wave ends there when amp1 is 0 (as Nitroplus's
 *   cb_00431230 ends one that reached 0). "spwave num" (or period 0) ends
 *   it at once. Rows of a zoomed picture
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
    ai.wave_period = v[1] > 0 && (v[2] != 0 || v[3] != 0) ? v[1] : 0;
    ai.wave_amp0 = v[2];
    ai.wave_amp1 = v[3];
    ai.wave_curve = v[4];
    ai.wave_tick = 0;
    ai.wave_amp = !ai.wave_on() ? 0 : ai.wave_ms > 0 ? v[2] : v[3];
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );

    return RET_CONTINUE;
}

// spwave: steps sprite ai's wave to current_time; true when what it draws changed.
bool ONScripter::stepSpriteWave( AnimationInfo &ai, int current_time )
{
    if (!ai.wave_on()) return false;
    int t = current_time - ai.wave_start;
    if (t >= ai.wave_ms && ai.wave_amp1 == 0){ // ramped to nothing: over
        bool shown = ai.wave_amp != 0;
        ai.wave_period = ai.wave_amp = ai.wave_tick = 0;
        return shown;
    }
    if (t < 0) t = 0;
    int amp = t >= ai.wave_ms ? ai.wave_amp1 :
        ai.wave_amp0 + (int)((long long)(ai.wave_amp1 - ai.wave_amp0) * curveAt( ai.wave_curve, (int)((long long)t * 1024 / ai.wave_ms) ) / 1024);
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
            drawWithTrail( surface, ai, clip );
        }
    }
}

// The sprites spz put over sprite `target`, the higher number first; over a
// handle, those over it next (a handle among them has its own).
void ONScripter::drawSpritesOver( SDL_Surface *surface, SDL_Rect &clip, int target, int depth )
{
    for (int k=MAX_SPRITE_NUM-1 ; k>=0 ; k--){
        AnimationInfo &over = sprite_info[k];
        if (over.z_over != target) continue;
        if (over.image_surface && over.visible){
            drawWithTrail( surface, over, clip );
        }
        if (k >= NS_SPRITE_NUM && depth < 8) drawSpritesOver( surface, clip, k, depth+1 ); // 8: a loop of spz is cut
    }
}

void ONScripter::drawSpriteRange( SDL_Surface *surface, SDL_Rect &clip, int hi, int lo )
{
    bool moved = false;
    for (int i=0 ; i<MAX_SPRITE_NUM && !moved ; i++)
        if (sprite_info[i].z_over >= 0) moved = true;
    // CONscripter: spnew's handles, each drawn right over its layer's numbered sprite
    int handles[SPRITE_HANDLE_NUM], num_handles = 0;
    for (int screen=0 ; screen<2 ; screen++) // the screens' last: over the other overlay handles
        for (int i=MAX_SPRITE_NUM-1 ; i>=NS_SPRITE_NUM ; i--){
            AnimationInfo &ai = sprite_info[i];
            if ((sprite_handle_layer[i - NS_SPRITE_NUM] == 4) != (screen == 1)) continue; // LAYER_SCREEN + 1
            if (ai.z_over < 0 && (ai.image_surface || moved)) handles[num_handles++] = i;
        }
    for (int i=hi ; i>=lo ; i--){
        AnimationInfo &ai = sprite_info[i];
        if (ai.image_surface && ai.visible && (!moved || ai.z_over < 0)){
            drawWithTrail( surface, ai, clip );
        }
        if (moved) drawSpritesOver( surface, clip, i, 0 );
        for (int k=0 ; k<num_handles ; k++){ // high to low: the lower handle on top
            AnimationInfo &h = sprite_info[handles[k]];
            if (spriteHandleOver( handles[k] ) != i) continue;
            if (h.image_surface && h.visible){
                drawWithTrail( surface, h, clip );
            }
            if (moved) drawSpritesOver( surface, clip, handles[k], 0 );
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

// Moves the sprite by a motion's values v (as a key's: ms, x, y, zoom x,
// angle, alpha, zoom y): its place, spzoom and alpha keep them.
void ONScripter::applyMotion( AnimationInfo &ai, const int *v )
{
    ai.orig_pos.x += v[1];
    ai.orig_pos.y += v[2];
    ai.scalePosXY( screen_ratio1, screen_ratio2 );
    ai.zoom_x = ai.zoom_x * v[3] / 100;
    ai.zoom_y = ai.zoom_y * v[6] / 100;
    ai.zoom_rot += v[4];
    if (v[5] < 256) ai.trans = (ai.trans < 0 ? 255 : ai.trans) * v[5] / 256;
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
        if (ai.motion_keep) applyMotion( ai, ai.motion_key[ai.motion_keys-1] );
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
    motionAt( ai, ai.motion_keys, t, v, AnimationInfo::MOTION_FINE );
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
// motion keys at t ms from the start, each key reached along its curve;
// the zooms and the angle in `fine` parts of the keys' units.
void ONScripter::motionAt( const AnimationInfo &ai, int keys, int t, int v[7], int fine )
{
    int i = 0;
    while (i + 1 < keys && ai.motion_key[i+1][0] <= t) i++;
    const int *a = ai.motion_key[i];
    v[0] = 0;
    for (int j=1 ; j<7 ; j++) v[j] = a[j] * (j == 3 || j == 4 || j == 6 ? fine : 1);
    if (i + 1 < keys && t > a[0]){
        const int *b = ai.motion_key[i+1];
        int span = b[0] - a[0];
        if (span > 0){
            int p = curveAt( b[7], (int)((long long)(t - a[0]) * 1024 / span) );
            for (int j=1 ; j<7 ; j++) v[j] += (int)((long long)(b[j] - a[j]) * (j == 3 || j == 4 || j == 6 ? fine : 1) * p / 1024);
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
        v[3] = v[6] = 100 * AnimationInfo::MOTION_FINE;
        v[5] = 256;
        return true;
    }
    motionAt( ai, keys, t, v, AnimationInfo::MOTION_FINE );
    if (ai.motion_keys == 0 && ai.motion_keep){
        // the ended motion's last key is now the sprite's own place, zoom,
        // angle and alpha (stepSpriteMotion): relative to it
        v[1] -= k[1];
        v[2] -= k[2];
        if (k[3] > 0) v[3] = v[3] * 100 / k[3];
        if (k[6] > 0) v[6] = v[6] * 100 / k[6];
        v[4] -= k[4] * AnimationInfo::MOTION_FINE;
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

// A sprite and its sptrail copies, under it or over it.
void ONScripter::drawWithTrail( SDL_Surface *surface, AnimationInfo &ai, SDL_Rect &clip )
{
    bool copies = ai.trail_passes > 0;
    if (copies && !ai.trail_over) trailCopies( ai, surface, &clip );
    drawTaggedSurface( surface, &ai, clip );
    if (copies && ai.trail_over) trailCopies( ai, surface, &clip );
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
        if (ai.wave_on()){ // spwave
            SDL_Rect r = drawnRect( ai );
            if (stepSpriteWave( ai, current_time ) && ai.visible && ai.image_surface){
                changed.add( r );
                changed.add( drawnRect( ai ) );
            }
        }
        if (stepSpriteFade( ai, current_time ) && ai.visible && ai.image_surface) // spfadeto
            changed.add( drawnRect( ai ) );
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
    if (ai.wave_on()){ // and its spwave rows, as far as they go
        int a = abs(ai.wave_amp0) > abs(ai.wave_amp1) ? abs(ai.wave_amp0) : abs(ai.wave_amp1);
        r.x -= a + 1;
        r.w += 2 * a + 2;
    }
    return r;
}

// A visible sprite drawn outside its place (zoomed up, turned, moved,
// trailed, waving) whose place the rect reaches: the commands mark a
// sprite's place as changed, which misses where it is drawn. A sprite drawn
// inside its place (zoomed down: a 2x picture at 50%) needs no more than
// the rect; redrawing the whole screen for it made each text character and
// motion step a full-screen redraw.
bool ONScripter::spriteZoomOrMotion( const SDL_Rect &clip )
{
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo &ai = sprite_info[i];
        if (!ai.visible || !ai.image_surface) continue;
        if (ai.wave_on()) return true;
        if (ai.motion_keys == 0 && ai.trail_keys == 0 && ai.zoom_x == 100 && ai.zoom_y == 100 && ai.zoom_rot == 0) continue;
        SDL_Rect d = drawnRect( ai ), &p = ai.pos;
        if (d.x >= p.x && d.y >= p.y && d.x + d.w <= p.x + p.w && d.y + d.h <= p.y + p.h) continue; // drawn inside its place
        if (SDL_HasIntersection( &clip, &ai.pos )) return true;
    }
    return false;
}

// The ticks of the next motion step, 0 when no sprite moves.
int ONScripter::nextSpriteMotionTime()
{
    int min = 0;
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo &ai = sprite_info[i];
        if (ai.wave_on() && ai.visible){ // spwave: its next step, or its ramp's end
            int step = speedTime( 33 );
            int next = ai.wave_start + (ai.wave_tick + 1) * (step > 0 ? step : 1);
            int end = ai.wave_start + ai.wave_ms;
            if (end > (int)SDL_GetTicks() && next > end) next = end;
            if (min == 0 || min > next) min = next;
        }
        if (ai.fade_on && ai.fade_frame >= 0){ // spfadeto: its next frame
            int next = ai.fade_start + (int)(((long long)(ai.fade_frame + 1) * ai.fade_ms + ai.fade_frames - 1) / ai.fade_frames); // (rounded up: k reached then)
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
    const int F = AnimationInfo::MOTION_FINE;
    if (ai.zoom_x == 100 && ai.zoom_y == 100 && ai.motion_zoom == 100 * F && ai.motion_zoom_y == 100 * F && ai.motion_rot == 0 && ai.zoom_rot == 0) return r;
    int w = (int)((long long)ai.pos.w * abs(ai.zoom_x) * abs(ai.motion_zoom) / (10000 * F));
    int h = (int)((long long)ai.pos.h * abs(ai.zoom_y) * abs(ai.motion_zoom_y) / (10000 * F));
    if ((ai.zoom_rot * F + ai.motion_rot) % (360 * F) != 0) w = h = (int)ceil( sqrt( (double)w * w + (double)h * h ) );
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
    // in 1/100 percent and degrees: a slow zoom or turn moves on each frame, not in whole steps
    const int F = AnimationInfo::MOTION_FINE;
    anim->scale_x = anim->zoom_x * anim->motion_zoom / 100;
    anim->scale_y = anim->zoom_y * anim->motion_zoom_y / 100;
    anim->rot = anim->zoom_rot * F + anim->motion_rot;
    anim->calcAffineMatrix( 100 * F, F );
    anim->blendOnSurface2( dst_surface, anim->pos.x, anim->pos.y, clip, anim->trans );

    anim->pos = pos;
    anim->affine_pos = affine_pos;
    anim->bounding_rect = bounding_rect;
    anim->scale_x = scale_x;
    anim->scale_y = scale_y;
    anim->rot = rot;
}

