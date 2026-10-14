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
#define CONS_SAVE_VERSION 4 // 1: sprite filters; 2: + screen stack; 3: + screen filters; 4: + screen effects

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

/* screenshake slot[, time, interval, mode, speed_x, speed_y, x0, y0, x1, y1]
 *   Shakes the picture behind the text window (what screenfilter
 *   filters) until the slot is cleared, without waiting: the script and
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
int ONScripter::windowtransCommand()
{
    int n = script_h.readInt();
    window_trans = n < 0 ? 0 : n > 255 ? 255 : n;
    dirty_rect.add( sentence_font_info.pos );
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
        // Nine numbers, then optional ones (screenripple's blend).
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
                const int *a = shake_path[e.point], *b = shake_path[e.point + 1];
                ux = a[0] + ((long long)(b[0] - a[0]) * e.phase_x >> 16);
                uy = a[1] + ((long long)(b[1] - a[1]) * e.phase_x >> 16);
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
 *   visibility, cell, transparency, filters), the screen filters and the
 *   BGM on a stack.
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
        sp.x = ai->orig_pos.x;
        sp.y = ai->orig_pos.y;
        sp.visible = ai->visible;
        sp.cell = ai->current_cell;
        sp.trans = ai->trans;
        sp.num_of_filters = ai->num_of_filters;
        memcpy(sp.filter, ai->filter, sizeof(sp.filter));
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
        if (!same){
            if (sp.image_name == NULL && ai->image_name) root_button_link.removeSprite(i);
            ai->remove();
            if (sp.image_name){
                ai->setImageName(sp.image_name);
                ai->trans = sp.trans;
                parseTaggedString( ai );
                ai->num_of_filters = sp.num_of_filters;
                memcpy(ai->filter, sp.filter, sizeof(sp.filter));
                ai->filter_changed = true;
                setupAnimationInfo( ai );
            }
        }
        ai->orig_pos.x = sp.x;
        ai->orig_pos.y = sp.y;
        ai->scalePosXY( screen_ratio1, screen_ratio2 );
        ai->visible = sp.visible && sp.image_name;
        ai->trans = sp.trans;
        if (ai->num_of_cells > 0) ai->setCell(sp.cell);
        if (ai->image_surface && ai->visible) dirty_rect.add( ai->pos );
        delete[] sp.image_name;
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
        for (int i=st.first ; i<=st.last ; i++)
            delete[] st.sprite[i - st.first].image_name;
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
}

void ONScripter::loadSaveFileCons()
{
    clearScreenStack();
    clearScreenFilters();
    clearScreenEffects();
    dirty_rect.fill( screen_width, screen_height );
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
            sp.x = sp.y = sp.cell = 0;
            sp.trans = -1;
            sp.visible = false;
            sp.num_of_filters = 0;
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

/* Debug trace (--trace file, or CONS_TRACE=file): one JSON object per line,
 * flushed as it is written, for finding bugs in a converted game without
 * screenshots. DEBUG.md describes the records. "at" is "*label:line", the
 * line counted from 1 through the script files in order (with 0.txt alone,
 * its own line number). Text and file names are converted to UTF-8. */

extern Coding2UTF16 *coding2utf16;
static ONScripter *trace_ons = NULL;

// The last commands run, for the crash record: where each was and its name
// (the name is the engine's own string), formatted only after a crash.
enum { TRACE_RECENT = 50 };
static struct { char *at; const char *cmd; } trace_recent[TRACE_RECENT];
static int trace_recent_count = 0; // ever run; the next goes in count % TRACE_RECENT

// Script bytes (in the script's encoding) as a JSON string in UTF-8;
// two-byte characters are told apart as the engine draws them. With json
// false, the text alone, without quotes or escapes (a newline becomes a
// space).
static void traceString( std::string &out, const char *text, int len=-1, bool json=true )
{
    if (!text){
        if (json) out += "null";
        return;
    }
    if (len < 0) len = strlen(text);
    if (json) out += '"';
    for (int i=0 ; i<len ; i++){
        unsigned char c = text[i];
        uint16_t u = c;
        if (IS_TWO_BYTE(c) && !coding2utf16->force_utf8 && i+1 < len){
            u = coding2utf16->conv2UTF16( c << 8 | (unsigned char)text[++i] );
            if (u == 0) u = 0xfffd; // not in the table
        }
        else if (c >= 0x80){
            if (!coding2utf16->force_utf8) u = 0xfffd; // a stray byte
            else{ // part of a UTF-8 character
                out += c;
                continue;
            }
        }

        if (!json && u < 0x20) out += ' ';
        else if (!json && u < 0x80) out += (char)u;
        else if (u == '"' || u == '\\'){
            out += '\\';
            out += (char)u;
        }
        else if (u == '\n') out += "\\n";
        else if (u < 0x20){
            char buf[8];
            snprintf( buf, sizeof(buf), "\\u%04x", u );
            out += buf;
        }
        else if (u < 0x80) out += (char)u;
        else{
            unsigned char u8[4];
            out.append( (char*)u8, coding2utf16->convUTF16ToUTF8( u8, u ) );
        }
    }
    if (json) out += '"';
}

// UTF-8 text in the script's encoding (as a script would have it).
static std::string fromUTF8( const char *text )
{
    if (coding2utf16->force_utf8) return text;
    std::string out;
    while (*text){
        if (!(*text & 0x80)){
            out += *text++;
            continue;
        }
        uint16_t c = coding2utf16->convUTF162Coding( coding2utf16->convUTF8ToUTF16( &text ) );
        if (c >= 0x100) out += (char)(c >> 8);
        out += c ? (char)(c & 0xff) : '?'; // '?' for a character the encoding lacks
    }
    return out;
}

void ONScripter::setTraceFile( const char *path )
{
    static bool exit_hooked = false;

    if (trace_fp) fclose( trace_fp );
    trace_fp = ::fopen( path, "w" ); // not ScriptParser::fopen, which looks in the game folder
    if (!trace_fp){
        utils::printError( "can't open the trace file %s\n", path );
        return;
    }
    trace_ons = this;
    ScriptHandler::error_hook = traceErrorHook;
    if (!exit_hooked){
        atexit( traceExitHook );
#if !defined(WEB)
        // a crash writes a "crash" record, then crashes as before (a core dump)
        signal( SIGSEGV, traceCrash );
        signal( SIGABRT, traceCrash );
        signal( SIGFPE, traceCrash );
#if defined(SIGBUS)
        signal( SIGBUS, traceCrash );
#endif
#endif
    }
    exit_hooked = true;
}

// A crash: a "crash" record with where the script was, the gosub calls
// that led there and the last commands run, then the signal again with
// its default action. Not safe in a signal handler, but it is only a debug
// aid, and the trace is flushed up to here anyway.
void ONScripter::traceCrash( int sig )
{
    signal( sig, SIG_DFL ); // a crash in here ends it at once
    if (trace_ons && trace_ons->trace_fp){
        ONScripter *ons = trace_ons;
        const char *name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT" : sig == SIGFPE ? "SIGFPE" : "SIGBUS";
        char buf[96];
        snprintf( buf, sizeof(buf), "{\"t\":\"crash\",\"pg\":%d,\"signal\":\"%s\",\"at\":", ons->trace_page, name );
        std::string r = buf;
        ons->traceAt( r, ons->trace_sprite ? ons->script_h.getCurrent( true ) : NULL );
        r += ",\"stack\":[";
        ons->traceStack( r );
        r += "],\"recent\":[";
        int first = trace_recent_count > TRACE_RECENT ? trace_recent_count - TRACE_RECENT : 0;
        for (int i=first ; i<trace_recent_count ; i++){
            if (i > first) r += ',';
            r += "{\"at\":";
            ons->traceAt( r, trace_recent[i % TRACE_RECENT].at );
            r += ",\"cmd\":";
            traceString( r, trace_recent[i % TRACE_RECENT].cmd );
            r += '}';
        }
        r += "]}";
        ons->traceWrite( r );
        fclose( ons->trace_fp );
        ons->trace_fp = NULL; // no end record: it crashed
    }
    raise( sig );
}

// Startup failed (no script, no font, SDL couldn't start): an error record
// of kind "startup" with the message just printed, then exit code 3.
void ONScripter::startupFailed()
{
    if (trace_fp){
        traceError( utils::lastError(), "startup" );
        traceEnd();
    }
    exit(3);
}

void ONScripter::traceErrorHook( const char *message )
{
    if (trace_ons && trace_ons->trace_fp){
        trace_ons->traceError( message );
        trace_ons->traceEnd(); // the engine exits next, and the web build runs no atexit handlers
    }
}

void ONScripter::traceExitHook()
{
    if (!trace_ons) return;
    if (trace_ons->dump_vars_path) trace_ons->writeVarsFile();
    if (trace_ons->trace_fp) trace_ons->traceEnd();
}

void ONScripter::traceWrite( std::string &record )
{
    if (!trace_meta_written) traceMeta(); // a startup error comes before traceStart
#if defined(WEB)
    // the web page keeps the records, to download them
    EM_ASM({ if (self.onsyuriTrace) self.onsyuriTrace(UTF8ToString($0)); }, record.c_str());
#endif
    record += '\n';
    fputs( record.c_str(), trace_fp );
    fflush( trace_fp ); // so a crash keeps everything before it
}

// "22-48,lsp2:12-23,ld:1": sprites left out of the trace (a game's own
// menus and button bar), applied when the trace starts.
void ONScripter::setTraceIgnore( const char *spec )
{
    setStr( &trace_ignore_spec, spec );
}

// The sprites --trace-ignore leaves out, for the trace and --auto-play.
void ONScripter::traceIgnoreSetup()
{
    trace_ignore = new bool[TRACE_SPRITES];
    for (int i=0 ; i<TRACE_SPRITES ; i++) trace_ignore[i] = false;
    for (const char *p = trace_ignore_spec ; p && *p ; ){
        int base = 0, num = MAX_SPRITE_NUM;
        if (!strncmp( p, "lsp2:", 5 )){
            base = MAX_SPRITE_NUM;
            num = MAX_SPRITE2_NUM;
            p += 5;
        }
        else if (!strncmp( p, "ld:", 3 )){
            base = MAX_SPRITE_NUM + MAX_SPRITE2_NUM;
            num = 3;
            p += 3;
        }
        else if (!strncmp( p, "lsp:", 4 )) p += 4;
        char *end;
        int first = strtol( p, &end, 10 ), last = first;
        if (*end == '-') last = strtol( end+1, &end, 10 );
        if (end == p) utils::printError( "--trace-ignore: can't read [%s]\n", p );
        for (int i=first ; i<=last ; i++)
            if (i >= 0 && i < num) trace_ignore[base + i] = true;
        p = strchr( end, ',' );
        if (p) p++;
    }
}

// Called once the script is read and the screen is set up.
void ONScripter::traceStart()
{
    trace_sprite = new TraceSprite[TRACE_SPRITES];
    trace_label_seen.assign( script_h.getNumLabels(), false );
    if (!trace_meta_written) traceMeta();
}

// The first record: the engine, the game and the options.
void ONScripter::traceMeta()
{
    trace_meta_written = true;
    char buf[256];
    time_t now = time( NULL );
    char date[32];
    strftime( date, sizeof(date), "%Y-%m-%dT%H:%M:%S", localtime( &now ) );
    snprintf( buf, sizeof(buf),
              "{\"t\":\"meta\",\"schema\":2,\"engine\":\"CONscripter %s\",\"time\":\"%s\",\"w\":%d,\"h\":%d,\"utf8\":%s,\"dir\":",
              CONSCRIPTER_VERSION, date, screen_width, screen_height,
              coding2utf16->force_utf8 ? "true" : "false" );
    std::string r = buf;
    traceString( r, archive_path );
    r += ",\"ignore\":";
    traceString( r, trace_ignore_spec );
    if (start_load >= 0){
        snprintf( buf, sizeof(buf), ",\"start\":{\"load\":%d}", start_load );
        r += buf;
    }
    else if (start_goto || start_vars){
        r += ",\"start\":{\"goto\":";
        traceString( r, start_goto );
        r += ",\"vars\":";
        traceString( r, start_vars );
        r += '}';
    }
    if (auto_play_flag){
        static const char *policy[] = { "first", "cycle", "random", "untaken" };
        snprintf( buf, sizeof(buf), ",\"auto\":{\"pages\":%d,\"choice\":\"%s\"", auto_play_limit, policy[auto_choice] );
        r += buf;
        if (auto_seed >= 0){
            snprintf( buf, sizeof(buf), ",\"seed\":%d", auto_seed );
            r += buf;
        }
        if (replay_path){
            r += ",\"replay\":";
            traceString( r, replay_path );
        }
        if (taken_path){
            r += ",\"taken\":";
            traceString( r, taken_path );
        }
        r += '}';
    }
    r += '}';
    traceWrite( r );
}

// The first time the script runs a command in a label: a "label" record,
// for the labels a run reached.
void ONScripter::traceLabel( char *address )
{
    int i = script_h.getLabelIndex( address );
    if (i < 0 || i >= (int)trace_label_seen.size() || trace_label_seen[i]) return;
    trace_label_seen[i] = true;
    char buf[48];
    snprintf( buf, sizeof(buf), "{\"t\":\"label\",\"pg\":%d,\"name\":", trace_page );
    std::string r = buf;
    traceString( r, script_h.getLabelName( i ) );
    r += '}';
    traceWrite( r );
}

// Appends "*label:line" for a place in the script, or null outside it (or
// before the script is labelled).
void ONScripter::traceAt( std::string &out, char *address )
{
    if (!trace_sprite || !script_h.inScript( address )){
        out += "null";
        return;
    }
    ScriptHandler::LabelInfo label = script_h.getLabelByAddress( address );
    std::string at = "*";
    at += label.name;
    char buf[16];
    snprintf( buf, sizeof(buf), ":%d", label.start_line + script_h.getLineByAddress( address ) + 1 );
    at += buf;
    traceString( out, at.c_str() );
}

// "*label:line" in UTF-8 without quotes (as in a choice record's "at"), or
// "" outside the script.
std::string ONScripter::traceAtText( char *address )
{
    if (!script_h.inScript( address ) || script_h.getNumLabels() == 0) return "";
    ScriptHandler::LabelInfo label = script_h.getLabelByAddress( address );
    std::string at = "*";
    at += label.name;
    char buf[16];
    snprintf( buf, sizeof(buf), ":%d", label.start_line + script_h.getLineByAddress( address ) + 1 );
    at += buf;
    std::string text;
    traceString( text, at.c_str(), -1, false );
    return text;
}

// Inside a gosub (a textgosub routine, a helper), appends ,"via": where it
// returns to, the line that led there.
void ONScripter::traceVia( std::string &out )
{
    for (NestInfo *info = last_nest_info ; info && info != &root_nest_info ; info = info->previous){
        if (info->nest_mode != NestInfo::LABEL) continue;
        out += ",\"via\":";
        traceAt( out, info->next_script );
        return;
    }
}

AnimationInfo &ONScripter::traceSpriteInfo( int i, const char *&kind, int &no )
{
    if (i < MAX_SPRITE_NUM){
        kind = "lsp";
        no = i;
        return sprite_info[i];
    }
    if (i < MAX_SPRITE_NUM + MAX_SPRITE2_NUM){
        kind = "lsp2";
        no = i - MAX_SPRITE_NUM;
        return sprite2_info[no];
    }
    kind = "ld"; // 0 left, 1 centre, 2 right
    no = i - MAX_SPRITE_NUM - MAX_SPRITE2_NUM;
    return tachi_info[no];
}

// Compares sprite i with how it was last recorded: 0 the same, 1 shown or
// changed (its "since" becomes since), 2 gone.
int ONScripter::traceSpriteUpdate( int i, int since )
{
    const char *kind;
    int no;
    AnimationInfo &ai = traceSpriteInfo( i, kind, no );
    TraceSprite &ts = trace_sprite[i];
    const char *image = ai.image_name;
    if (!image && ai.trans_mode == AnimationInfo::TRANS_STRING) image = ai.file_name; // strsp: its text
    if (!ai.visible || !image){
        if (ts.image.empty()) return 0;
        ts.image.clear();
        return 2;
    }
    SDL_Rect r = (kind[3] == '2') ? ai.bounding_rect : ai.pos; // lsp2: its box after scaling and turning
    // an animation steps its cells by itself
    int cell = ai.is_animatable ? -1 : ai.current_cell;
    if (ts.image == image && ts.x == r.x && ts.y == r.y && ts.w == r.w && ts.h == r.h &&
        ts.trans == ai.trans && ts.cell == cell) return 0;
    ts.image = image;
    ts.x = r.x; ts.y = r.y; ts.w = r.w; ts.h = r.h;
    ts.trans = ai.trans;
    ts.cell = cell;
    ts.since = since;
    return 1;
}

// Appends visible sprite i to a list.
void ONScripter::traceSprite( std::string &out, int i )
{
    const char *kind;
    int no;
    AnimationInfo &ai = traceSpriteInfo( i, kind, no );
    TraceSprite &ts = trace_sprite[i];

    char buf[160];
    if (out[out.size()-1] != '[') out += ',';
    snprintf( buf, sizeof(buf), "{\"n\":%d,\"kind\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"alpha\":%d",
              no, kind, ts.x, ts.y, ts.w, ts.h, ai.trans < 0 ? 255 : ai.trans );
    out += buf;
    bool missing = false;
    if (ai.trans_mode == AnimationInfo::TRANS_STRING){
        out += ",\"str\":";
        traceString( out, ai.file_name );
    }
    else if (ai.file_name){
        out += ",\"file\":";
        traceString( out, ai.file_name );
        missing = !ai.image_surface;
        if (missing) out += ",\"missing\":true";
    }
    else{
        out += ",\"tag\":"; // a colour or other sprite without a file
        traceString( out, ai.image_name );
    }
    if (ai.num_of_cells > 1){
        snprintf( buf, sizeof(buf), ",\"cell\":%d,\"cells\":%d", ai.current_cell, ai.num_of_cells );
        out += buf;
    }
    if (missing) ; // no size to place
    else if (ts.x >= screen_width || ts.y >= screen_height || ts.x + ts.w <= 0 || ts.y + ts.h <= 0)
        out += ",\"off\":true";
    else{
        std::string edges;
        if (ts.y <= 0) edges += 'T';
        if (ts.y + ts.h >= screen_height) edges += 'B';
        if (ts.x <= 0) edges += 'L';
        if (ts.x + ts.w >= screen_width) edges += 'R';
        if (!edges.empty()) out += ",\"edges\":\"" + edges + "\"";
    }
    if (ai.num_of_filters > 0){
        snprintf( buf, sizeof(buf), ",\"filters\":%d", ai.num_of_filters );
        out += buf;
    }
    snprintf( buf, sizeof(buf), ",\"since\":%d}", ts.since );
    out += buf;
}

// Appends what changed since it was last recorded: ,"sp":[...] (a sprite
// shown or changed, or {"n","kind","gone":true}), ,"bg" and ,"bgm".
// Returns false when nothing did.
bool ONScripter::traceChanges( std::string &out )
{
    size_t length = out.size();
    std::string sp;
    for (int i=0 ; i<TRACE_SPRITES ; i++){
        if (trace_ignore[i]) continue;
        int change = traceSpriteUpdate( i, trace_page + 1 ); // seen first on the next page
        if (change == 0) continue;
        if (sp.empty()) sp = ",\"sp\":[";
        if (change == 1){
            traceSprite( sp, i );
            continue;
        }
        const char *kind;
        int no;
        traceSpriteInfo( i, kind, no );
        char buf[64];
        if (sp[sp.size()-1] != '[') sp += ',';
        snprintf( buf, sizeof(buf), "{\"n\":%d,\"kind\":\"%s\",\"gone\":true}", no, kind );
        sp += buf;
    }
    if (!sp.empty()) out += sp + "]";

    const char *bgm = music_file_name ? music_file_name : midi_file_name;
    if (trace_bg != (bg_info.file_name ? bg_info.file_name : "")){
        trace_bg = bg_info.file_name ? bg_info.file_name : "";
        out += ",\"bg\":";
        traceString( out, bg_info.file_name );
    }
    if (trace_bgm != (bgm ? bgm : "")){
        trace_bgm = bgm ? bgm : "";
        out += ",\"bgm\":";
        traceString( out, bgm );
    }
    return out.size() > length;
}

// Commands recorded as "ev": 1 when they change a sprite or the
// background, 2 always (sounds, movies), 3 print, when something changed
// since the last one, 4 an effect definition, 5 always, and the next print
// too (what is laid over the picture).
static const struct { const char *name; int kind; } trace_commands[] = {
    {"lsp",1}, {"lsph",1}, {"lsp2",1}, {"lsph2",1}, {"lsp2add",1}, {"lsph2add",1},
    {"lsp2sub",1}, {"lsph2sub",1}, {"csp",1}, {"csp2",1}, {"vsp",1}, {"vsp2",1},
    {"msp",1}, {"msp2",1}, {"amsp",1}, {"amsp2",1}, {"cell",1}, {"spfilter",1},
    {"strsp",1}, {"strsph",1}, {"spstr",1}, {"spreload",1}, {"ld",1}, {"cl",1},
    {"bg",1}, {"bgcopy",1}, {"bgcpy",1}, {"popscreen",1},
    {"bgm",2}, {"bgmonce",2}, {"mp3",2}, {"mp3loop",2}, {"mp3save",2}, {"play",2},
    {"playonce",2}, {"playstop",2}, {"stop",2}, {"mp3stop",2}, {"bgmstop",2},
    {"mp3fadeout",2}, {"bgmfadeout",2}, {"loopbgm",2}, {"loopbgmstop",2},
    {"wave",2}, {"waveloop",2}, {"wavestop",2}, {"dwave",2}, {"dwaveloop",2},
    {"dwavestop",2}, {"dwaveplay",2}, {"dwaveplayloop",2},
    {"mpegplay",2}, {"movie",2}, {"avi",2},
    {"screenfilter",5}, {"screenshake",5}, {"screenripple",5}, {"screenpulse",5},
    {"pushscreen",5}, {"monocro",5}, {"nega",5}, {"quake",5}, {"quakex",5}, {"quakey",5},
    {"allsphide",5}, {"allspresume",5}, {"allsp2hide",5}, {"allsp2resume",5}, {"windowtrans",5},
    {"print",3},
    {"effect",4},
};

// Runs a command and records it if it is one of trace_commands: where it
// is, its source text (arguments as written), and what it changed.
int ONScripter::traceCommand( const char *name, FuncList method )
{
    char *start = trace_cmd_start = script_h.getCurrent();
    trace_recent[trace_recent_count % TRACE_RECENT].at = start;
    trace_recent[trace_recent_count % TRACE_RECENT].cmd = name;
    trace_recent_count++;
    trace_effect.clear();
    trace_sound.clear();
    traceLabel( start );
    int kind = 0;
    for (size_t i=0 ; i<sizeof(trace_commands)/sizeof(trace_commands[0]) ; i++)
        if (!strcmp( name, trace_commands[i].name )){
            kind = trace_commands[i].kind;
            break;
        }

    int ret = (this->*method)();
    if (kind == 0 || !trace_sprite) return ret;

    char buf[64];
    snprintf( buf, sizeof(buf), "{\"t\":\"ev\",\"pg\":%d,\"ms\":%u,\"cmd\":\"%s\",\"at\":", trace_page, SDL_GetTicks(), name );
    std::string r = buf;
    traceAt( r, start );
    traceVia( r );
    r += ",\"src\":";
    // to the end of the command: ':' or ';' outside a string, or the line's end
    int len = 0;
    bool quoted = false;
    while (len < 300 && start[len] && start[len] != '\n' &&
           (quoted || (start[len] != ':' && start[len] != ';'))){
        if (start[len] == '"') quoted = !quoted;
        if (IS_TWO_BYTE(start[len]) && start[len+1] && start[len+1] != '\n') len++;
        len++;
    }
    while (len > 0 && (start[len-1] == ' ' || start[len-1] == '\t')) len--;
    traceString( r, start, len );

    bool changed = traceChanges( r );
    if (kind == 4){ // the effect as defined
        r += ",\"def\":";
        traceEffect( r, last_effect_link );
    }
    else if (!trace_effect.empty())
        r += ",\"effect\":" + trace_effect;
    r += trace_sound;
    if (kind == 1 && !changed) return ret;
    if (kind == 3){
        if (!trace_shown) return ret;
        trace_shown = false;
    }
    else if (changed || kind == 5) trace_shown = true;
    r += '}';
    traceWrite( r );
    return ret;
}

// An effect as {"no","type","ms"} with its "mask" (types 15, 18) or
// "dll" (99); "no" for one defined with the effect command.
void ONScripter::traceEffect( std::string &out, EffectLink *effect )
{
    char buf[96];
    if (effect != &tmp_effect && effect != &window_effect && effect != &root_effect_link)
        snprintf( buf, sizeof(buf), "{\"no\":%d,\"type\":%d,\"ms\":%d", effect->no, effect->effect, effect->duration );
    else
        snprintf( buf, sizeof(buf), "{\"type\":%d,\"ms\":%d", effect->effect, effect->duration );
    out += buf;
    if (effect->anim.image_name && (effect->effect == 15 || effect->effect == 18 || effect->effect == 99)){
        out += effect->effect == 99 ? ",\"dll\":" : ",\"mask\":";
        traceString( out, effect->anim.image_name );
    }
    out += '}';
}

// A select, selgosub or selnum choice: the options and the one taken
// (counted from 1).
void ONScripter::traceSelect( int picked )
{
    char buf[64];
    snprintf( buf, sizeof(buf), "{\"t\":\"choice\",\"pg\":%d,\"kind\":\"select\",\"at\":", trace_page );
    std::string r = buf;
    traceAt( r, trace_cmd_start );
    r += ",\"options\":[";
    for (SelectLink *link = root_select_link.next ; link ; link = link->next){
        if (r[r.size()-1] != '[') r += ',';
        r += "{\"text\":";
        traceString( r, link->text );
        r += ",\"label\":";
        traceString( r, link->label );
        r += '}';
    }
    snprintf( buf, sizeof(buf), "],\"picked\":%d}", picked );
    r += buf;
    traceWrite( r );
}

// A btnwait: the buttons offered (with their sprites) and the result.
void ONScripter::traceButtons( int result )
{
    char buf[96];
    snprintf( buf, sizeof(buf), "{\"t\":\"choice\",\"pg\":%d,\"kind\":\"btnwait\",\"at\":", trace_page );
    std::string r = buf;
    traceAt( r, trace_cmd_start );
    traceVia( r );
    r += ",\"buttons\":[";
    for (ButtonLink *bl = root_button_link.next ; bl ; bl = bl->next){
        bool sprite = bl->button_type == ButtonLink::SPRITE_BUTTON || bl->button_type == ButtonLink::TMP_SPRITE_BUTTON;
        if (sprite && bl->sprite_no >= 0 && bl->sprite_no < MAX_SPRITE_NUM && trace_ignore[bl->sprite_no]) continue;
        if (r[r.size()-1] != '[') r += ',';
        snprintf( buf, sizeof(buf), "{\"no\":%d", bl->no );
        r += buf;
        if (sprite && bl->sprite_no >= 0 && bl->sprite_no < MAX_SPRITE_NUM){
            AnimationInfo &ai = sprite_info[bl->sprite_no];
            snprintf( buf, sizeof(buf), ",\"sp\":%d", bl->sprite_no );
            r += buf;
            if (ai.file_name){
                r += (ai.trans_mode == AnimationInfo::TRANS_STRING) ? ",\"str\":" : ",\"file\":";
                traceString( r, ai.file_name );
            }
        }
        else{
            snprintf( buf, sizeof(buf), ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d",
                      bl->select_rect.x, bl->select_rect.y, bl->select_rect.w, bl->select_rect.h );
            r += buf;
        }
        r += '}';
    }
    snprintf( buf, sizeof(buf), "],\"result\":%d}", result );
    r += buf;
    traceWrite( r );
}

// The state at a click wait or a new page: where the script is, the text,
// the background, the music and sounds, and every visible sprite. With
// dump (F8), the same state at any moment, as a "dump" record that isn't
// counted as a page (a change it shows counts from the next page).
void ONScripter::tracePage( bool dump )
{
    if (!trace_sprite) return;
    if (!dump) trace_page++;
    int since = dump ? trace_page + 1 : trace_page;

    char buf[64];
    traceLabel( script_h.getCurrent( true ) );
    snprintf( buf, sizeof(buf), "{\"t\":\"%s\",\"pg\":%d,\"ms\":%u,\"at\":",
              dump ? "dump" : "page", trace_page, SDL_GetTicks() );
    std::string r = buf;
    traceAt( r, script_h.getCurrent( true ) );
    r += ",\"text\":";
    traceString( r, current_page->text, current_page->text_count );
    if (current_page->tag){ // the text's [...] tag (a speaker's name, a voice)
        r += ",\"tag\":";
        traceString( r, current_page->tag );
    }
    r += ",\"bg\":";
    traceString( r, bg_info.file_name );
    r += ",\"bgm\":";
    traceString( r, music_file_name ? music_file_name : midi_file_name );
    if (loop_bgm_name[0]){
        r += ",\"loopbgm\":[";
        traceString( r, loop_bgm_name[0] );
        r += ',';
        traceString( r, loop_bgm_name[1] );
        r += ']';
    }

    bool se = false;
    for (int ch=0 ; audio_open_flag && ch<=MIX_WAVE_CHANNEL ; ch++){
        if (!trace_se_name[ch] || !Mix_Playing( ch )) continue;
        r += se ? "," : ",\"se\":[";
        se = true;
        snprintf( buf, sizeof(buf), "{\"ch\":%d,\"file\":", ch );
        r += buf;
        traceString( r, trace_se_name[ch] );
        r += trace_se_loop[ch] ? ",\"loop\":true}" : "}";
    }
    if (se) r += ']';

    // what is laid over the picture
    r += (display_mode & DISPLAY_MODE_TEXT) ? ",\"window\":true" : ",\"window\":false";
    if (window_trans != 255){
        snprintf( buf, sizeof(buf), ",\"windowtrans\":%d", window_trans );
        r += buf;
    }
    if (monocro_flag){
        snprintf( buf, sizeof(buf), ",\"monocro\":\"#%02x%02x%02x\"", monocro_color[0], monocro_color[1], monocro_color[2] );
        r += buf;
    }
    if (nega_mode){
        snprintf( buf, sizeof(buf), ",\"nega\":%d", nega_mode );
        r += buf;
    }
    bool listed = false;
    for (int i=0 ; i<MAX_SCREEN_FILTER ; i++){
        AnimationInfo::Filter &f = screen_filter[i];
        if (f.kind == AnimationInfo::FILTER_NONE) continue;
        r += listed ? "," : ",\"filters\":[";
        listed = true;
        char fb[128];
        snprintf( fb, sizeof(fb), "{\"slot\":%d,\"kind\":%d,\"blend\":%d,\"strength\":%d,\"p\":[%d,%d,%d]}",
                  i+1, f.kind, f.blend, f.strength, f.param[0], f.param[1], f.param[2] );
        r += fb;
    }
    if (listed) r += ']';
    listed = false;
    for (int i=0 ; i<MAX_SCREEN_EFFECT ; i++){
        ScreenEffect &e = screen_effect[i];
        if (e.kind == SCREEN_EFFECT_NONE) continue;
        static const char *names[] = { "", "shake", "ripple", "pulse" };
        r += listed ? "," : ",\"fx\":[";
        listed = true;
        snprintf( buf, sizeof(buf), "{\"slot\":%d,\"kind\":\"%s\"", i+1, names[e.kind] );
        r += buf;
        if (e.map_name){
            r += ",\"map\":";
            traceString( r, e.map_name );
        }
        r += '}';
    }
    if (listed) r += ']';

    if (trace_overflow) r += ",\"overflow\":true";
    if (dump) traceVars( r );
    if (all_sprite_hide_flag) r += ",\"sprites_hidden\":true";
    if (all_sprite2_hide_flag) r += ",\"sprites2_hidden\":true";
    traceShot( r, dump );
    r += ",\"sp\":[";
    for (int i=0 ; i<TRACE_SPRITES ; i++){
        if (trace_ignore[i]) continue;
        traceSpriteUpdate( i, since ); // a change no command recorded
        if (!trace_sprite[i].image.empty()) traceSprite( r, i );
    }
    r += "]}";
    traceWrite( r );
}

// "412,980-990": the pages --shot-pages takes a picture of.
void ONScripter::setTraceShots( const char *pages )
{
    for (const char *p = pages ; p && *p ; ){
        char *end;
        int first = strtol( p, &end, 10 ), last = first;
        if (*end == '-') last = strtol( end+1, &end, 10 );
        if (end == p){
            utils::printError( "--shot-pages: can't read [%s]\n", p );
            break;
        }
        trace_shot_pages.push_back( std::make_pair( first, last ) );
        p = strchr( end, ',' );
        if (p) p++;
    }
}

void ONScripter::setTraceShotEvery( int pages )
{
    trace_shot_every = pages;
}

void ONScripter::setTraceShotDir( const char *dir )
{
    trace_shot_dir = dir;
}

// On a page --shot-pages or --shot-every asks for, saves the screen as
// <shot dir>/page<pg>.png and appends ,"shot":"<file>"; on a dump, always,
// as dump<pg>-<ms>.png (not in the web build, which has nowhere to put it).
void ONScripter::traceShot( std::string &out, bool dump )
{
    bool wanted = dump || (trace_shot_every > 0 && trace_page % trace_shot_every == 0);
    for (size_t i=0 ; i<trace_shot_pages.size() && !wanted ; i++)
        wanted = trace_page >= trace_shot_pages[i].first && trace_page <= trace_shot_pages[i].second;
#if defined(WEB)
    if (dump) wanted = false;
#endif
    if (!wanted) return;

#if defined(_WIN32)
    _mkdir( trace_shot_dir.c_str() );
#else
    mkdir( trace_shot_dir.c_str(), 0755 );
#endif
    char name[48];
    if (dump) snprintf( name, sizeof(name), "/dump%06d-%u.png", trace_page, SDL_GetTicks() );
    else      snprintf( name, sizeof(name), "/page%06d.png", trace_page );
    std::string path = trace_shot_dir + name;
    // the screen as shown, without the alpha channel the picture doesn't use
    SDL_Surface *rgb = SDL_ConvertSurfaceFormat( accumulation_surface, SDL_PIXELFORMAT_RGB24, 0 );
    if (!rgb || IMG_SavePNG( rgb, path.c_str() ) != 0){
        utils::printError( "can't save the picture %s: %s\n", path.c_str(), SDL_GetError() );
        if (rgb) SDL_FreeSurface( rgb );
        return;
    }
    SDL_FreeSurface( rgb );
    out += ",\"shot\":";
    traceString( out, path.c_str() );
}

// dwave, wave and the like, before they play a sound (name, or NULL for
// the one dwaveload loaded) on a channel: the trace's ev gets
// ,"ch":n,"file":"..." and ,"cut":"..." when it stops another sound still
// playing there, and the web page's self.onsyuriVoice(channel, file) is
// called when it plays.
void ONScripter::soundCommand( int channel, const char *name, bool play )
{
    if (channel < 0 || channel >= ONS_MIX_CHANNELS + ONS_MIX_EXTRA_CHANNELS) return;
    const char *before = trace_se_name[channel];
    bool playing = audio_open_flag && before && Mix_Playing( channel );
    if (!name) name = before; // dwaveplay: what dwaveload loaded
    if (trace_fp){
        char buf[32];
        snprintf( buf, sizeof(buf), ",\"ch\":%d,\"file\":", channel );
        trace_sound = buf;
        traceString( trace_sound, name );
        if (playing && name != before){
            trace_sound += ",\"cut\":";
            traceString( trace_sound, before );
        }
    }
    if (play && name){
        std::string file;
        traceString( file, name, -1, false ); // UTF-8
        if (channel == 0 && current_page) setStr( &current_page->voice, file.c_str() ); // for onsyuriBacklog
#if defined(WEB)
        EM_ASM({ if (self.onsyuriVoice) self.onsyuriVoice($0, UTF8ToString($1)); }, channel, file.c_str());
#endif
    }
}

// Notes the sound a channel plays, for the pages' "se" lists.
void ONScripter::traceSound( int channel, const char *name, bool loop )
{
    if (channel < 0 || channel >= ONS_MIX_CHANNELS + ONS_MIX_EXTRA_CHANNELS) return;
    setStr( &trace_se_name[channel], name );
    trace_se_loop[channel] = loop;
}

// A file the script asked for that is missing or can't be read.
void ONScripter::traceWarn( const char *what, const char *file )
{
    // the engine's own defaults, tried whether or not the game has them
    static const char *defaults[] = { DEFAULT_LOOKBACK_NAME0, DEFAULT_LOOKBACK_NAME1, DEFAULT_LOOKBACK_NAME2,
                                      DEFAULT_LOOKBACK_NAME3, "cursor0.bmp", "cursor1.bmp" };
    for (size_t i=0 ; i<sizeof(defaults)/sizeof(defaults[0]) ; i++)
        if (!strcmp( file, defaults[i] )) return;

    char buf[64];
    snprintf( buf, sizeof(buf), "{\"t\":\"warn\",\"pg\":%d,\"what\":\"%s\",\"file\":", trace_page, what );
    std::string r = buf;
    traceString( r, file );
    r += ",\"at\":";
    traceAt( r, script_h.getCurrent( true ) );
    r += '}';
    traceWrite( r );
}

// A script error, just before the engine exits, with the gosub calls that
// led to it (innermost first: where each returns to).
void ONScripter::traceError( const char *message, const char *kind )
{
    char buf[96];
    snprintf( buf, sizeof(buf), "{\"t\":\"error\",\"pg\":%d,\"kind\":\"%s\",\"msg\":", trace_page, kind );
    std::string r = buf;
    // without the line's end and the frame printError puts round it
    std::string msg = message;
    while (!msg.empty() && (msg[msg.size()-1] == '\n' || msg[msg.size()-1] == ' ')) msg.erase( msg.size()-1 );
    traceString( r, msg.c_str() );
    r += ",\"at\":";
    traceAt( r, trace_sprite ? script_h.getCurrent( true ) : NULL ); // none before the script runs
    r += ",\"stack\":[";
    traceStack( r );
    r += "]}";
    traceWrite( r );
}

// The gosub calls that led here, innermost first: where each returns to.
void ONScripter::traceStack( std::string &out )
{
    bool first = true;
    for (NestInfo *info = last_nest_info ; info && info != &root_nest_info ; info = info->previous){
        if (info->nest_mode != NestInfo::LABEL) continue;
        if (!first) out += ',';
        first = false;
        traceAt( out, info->next_script );
    }
}

// The last record, written as the engine exits normally or after a script
// error; a trace without it ended in a crash.
void ONScripter::traceEnd()
{
    char buf[64];
    snprintf( buf, sizeof(buf), "{\"t\":\"end\",\"pg\":%d", trace_page );
    std::string r = buf;
    if (trace_end_reason){
        r += ",\"reason\":";
        traceString( r, trace_end_reason );
    }
    r += '}';
    traceWrite( r );
    fclose( trace_fp );
    trace_fp = NULL;
}

/* --auto-play pages: the game plays itself, for finding bugs with the
 * trace. Every wait for a click is answered at once, a btnwait or select
 * picks a button (--auto-choice), effects take 1 ms, timed waits and BGM
 * fades 1 ms, vsync is off and movies are skipped; sounds still play. It stops after the given number of pages
 * (0: when the script ends), or when it goes round in circles: 20000 waits
 * or 200 choices without a new page, or 3000 pages without a new label. */
void ONScripter::setAutoPlay( int pages )
{
    auto_play_flag = true;
    auto_play_limit = pages > 0 ? pages : 0;
    setVsyncOff(); // each drawn character would wait for the screen
}

void ONScripter::setAutoChoice( const char *policy )
{
    if      (!strcmp( policy, "first" ))  auto_choice = AUTO_CHOICE_FIRST;
    else if (!strcmp( policy, "cycle" ))  auto_choice = AUTO_CHOICE_CYCLE;
    else if (!strcmp( policy, "random" )) auto_choice = AUTO_CHOICE_RANDOM;
    else if (!strcmp( policy, "untaken" )) auto_choice = AUTO_CHOICE_UNTAKEN;
    else{
        utils::printError( "--auto-choice: first, cycle, random or untaken, not %s\n", policy );
        return;
    }
    auto_choice_given = true;
}

// A string or number field of a trace record, found by its name; enough
// for the records the engine writes. Returns false when it isn't there.
static bool traceField( const std::string &line, const char *name, std::string *text, int *num, bool last=false )
{
    std::string key = std::string( "\"" ) + name + "\":";
    size_t i = last ? line.rfind( key ) : line.find( key );
    if (i == std::string::npos) return false;
    i += key.size();
    if (text){
        if (line[i] != '"') return false;
        text->clear();
        for (i++ ; i < line.size() && line[i] != '"' ; i++){
            if (line[i] == '\\' && i+1 < line.size()) i++;
            *text += line[i];
        }
        return true;
    }
    if (line[i] != '-' && (line[i] < '0' || line[i] > '9')) return false;
    *num = atoi( line.c_str() + i );
    return true;
}

// --replay trace.jsonl: --auto-play makes the choices that trace recorded,
// in order, and takes its seed and --auto-choice unless they are given.
void ONScripter::setReplay( const char *path )
{
    setStr( &replay_path, path );
    FILE *fp = ::fopen( path, "rb" );
    if (!fp){
        utils::printError( "--replay: can't open %s\n", path );
        startupFailed();
    }
    std::string line;
    char buf[4096];
    while (fgets( buf, sizeof(buf), fp )){
        line += buf;
        if (line[line.size()-1] != '\n' && !feof( fp )) continue; // a long line
        std::string t;
        if (traceField( line, "t", &t, NULL )){
            AutoAnswer a;
            if (t == "meta"){
                traceField( line, "seed", NULL, &replay_seed );
                std::string policy;
                if (!auto_choice_given && traceField( line, "choice", &policy, NULL )){
                    setAutoChoice( policy.c_str() );
                    auto_choice_given = false;
                }
            }
            else if (t == "choice" && traceField( line, "at", &a.at, NULL ) &&
                     (traceField( line, "picked", NULL, &a.value, true ) || traceField( line, "result", NULL, &a.value, true )))
                replay_answers.push_back( a );
        }
        line.clear();
    }
    fclose( fp );
}

// --taken file: lines "*label:line<TAB>option", the choices --auto-choice
// untaken avoids (option as in the choice record: picked or result).
void ONScripter::setTaken( const char *path )
{
    setStr( &taken_path, path );
    FILE *fp = ::fopen( path, "rb" );
    if (!fp){
        utils::printError( "--taken: can't open %s\n", path );
        startupFailed();
    }
    char buf[1024];
    while (fgets( buf, sizeof(buf), fp )){
        size_t len = strlen( buf );
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = 0;
        char *tab = strchr( buf, '\t' );
        if (buf[0] == '#' || !tab) continue;
        *tab = 0;
        char key[1100];
        snprintf( key, sizeof(key), "%s\t%d", buf, atoi( tab+1 ) );
        auto_taken.insert( key );
    }
    fclose( fp );
}

// A warn record about the choices --auto-play makes, at the command being
// run; extra is more fields, starting with a comma.
void ONScripter::autoPlayWarn( const char *what, const char *extra )
{
    if (!trace_fp) return;
    char buf[96];
    snprintf( buf, sizeof(buf), "{\"t\":\"warn\",\"pg\":%d,\"what\":\"%s\",\"at\":", trace_page, what );
    std::string r = buf;
    traceAt( r, trace_cmd_start );
    if (extra) r += extra;
    r += '}';
    traceWrite( r );
}

// After a choice was made: the next recorded one is due, and the choice
// counts as taken for "untaken".
void ONScripter::autoPlayChosen( int value )
{
    if (replay_next < replay_answers.size()) replay_next++;
    char buf[16];
    snprintf( buf, sizeof(buf), "\t%d", value );
    auto_taken.insert( traceAtText( trace_cmd_start ) + buf );
}

// The same seed gives the same rnd numbers, and the same random choices.
void ONScripter::setSeed( int seed )
{
    auto_seed = seed;
    srand( seed );
}

// Called by waitEvent: true when the wait is answered (a click, or the
// button pressed); otherwise a timed wait is shortened.
bool ONScripter::autoPlayWait( int &count )
{
    if (++auto_play_waits > 20000) autoPlayStuck( "20000 waits without a new page" );
    if (!(event_mode & (WAIT_INPUT_MODE | WAIT_BUTTON_MODE))){
        if (count > 1) count = 1;
        return false;
    }

    // let the events waiting be handled (timers, sounds that ended), as a
    // skipped click wait does
    int mode = event_mode;
    event_mode = IDLE_EVENT_MODE;
    waitEventSub( 0 );
    event_mode = mode;

    // textbtnwait waits on a text page: a click
    int button = (event_mode & WAIT_BUTTON_MODE && !(event_mode & WAIT_INPUT_MODE)) ? autoPlayButton() : 0;
    current_button_state.button = button;
    if (button > 0) sprintf( current_button_state.str, "S%d", button );
    else            sprintf( current_button_state.str, "LCLICK" );
    current_button_state.down_flag = false;
    return true;
}

// The button a btnwait or select takes. The options are the buttons
// numbered from 1 whose sprites --trace-ignore doesn't leave out, in order,
// then for a btnwait 0 (a click elsewhere, which often goes on past a
// hyperlink). Each place counts its visits: "first" takes the option of
// that number, staying on the last (a hyperlink, then on), "cycle" goes
// round, "random" takes any.
int ONScripter::autoPlayButton()
{
    if (++auto_play_choices > 200) autoPlayStuck( "200 choices without a new page" );

    std::vector<int> nos;
    for (ButtonLink *bl = root_button_link.next ; bl ; bl = bl->next){
        if (bl->no < 1) continue;
        if ((bl->button_type == ButtonLink::SPRITE_BUTTON || bl->button_type == ButtonLink::TMP_SPRITE_BUTTON) &&
            bl->sprite_no >= 0 && bl->sprite_no < MAX_SPRITE_NUM && trace_ignore && trace_ignore[bl->sprite_no]) continue;
        nos.push_back( bl->no );
    }
    std::sort( nos.begin(), nos.end() );
    nos.erase( std::unique( nos.begin(), nos.end() ), nos.end() );
    if (!(event_mode & WAIT_TEXT_MODE)) nos.push_back( 0 ); // a btnwait, not a select
    if (nos.empty()) return 0;

    // random takes a number each time, even when replaying, so that rnd
    // gives the numbers it gave in the run recorded
    int random = auto_choice == AUTO_CHOICE_RANDOM ? rand() : 0;

    if (replay_next < replay_answers.size()){
        const AutoAnswer &a = replay_answers[replay_next];
        std::string at = traceAtText( trace_cmd_start );
        if (a.at != at && replay_warned != replay_next + 1){
            replay_warned = replay_next + 1;
            std::string extra = ",\"expected\":";
            traceString( extra, a.at.c_str() ); // already UTF-8
            autoPlayWarn( "replay diverged", extra.c_str() );
        }
        if (std::find( nos.begin(), nos.end(), a.value ) != nos.end()) return a.value;
    }
    else if (!replay_answers.empty() && replay_warned != (size_t)-1){
        replay_warned = (size_t)-1;
        autoPlayWarn( "replay ended" );
    }

    if (auto_choice == AUTO_CHOICE_RANDOM) return nos[ random % nos.size() ];
    if (auto_choice == AUTO_CHOICE_UNTAKEN){
        std::string at = traceAtText( trace_cmd_start ) + "\t";
        for (size_t i=0 ; i<nos.size() ; i++)
            if (!auto_taken.count( at + std::to_string( nos[i] ) )) return nos[i];
    }
    size_t visit = auto_choice_visits[ script_h.getCurrent() ]++;
    if (auto_choice == AUTO_CHOICE_CYCLE) return nos[ visit % nos.size() ];
    return nos[ std::min( visit, nos.size() - 1 ) ];
}

// Stops a run going round in circles.
void ONScripter::autoPlayStuck( const char *what )
{
    utils::printError( "auto-play: %s, stopping\n", what );
    static char reason[64];
    snprintf( reason, sizeof(reason), "stuck: %s", what );
    trace_end_reason = reason;
    if (trace_fp){
        std::string r = "{\"t\":\"warn\",\"pg\":";
        r += std::to_string( trace_page );
        r += ",\"what\":\"auto-play stuck\",\"at\":";
        traceAt( r, script_h.getCurrent( true ) );
        traceVia( r );
        r += '}';
        traceWrite( r );
        traceEnd();
    }
    exit(1);
}

// After each page: stop at the page limit, or when 3000 pages have passed
// without one in a label not seen before (the game going round a loop of
// routes, the same choice taken at each turn).
void ONScripter::autoPlayPage()
{
    auto_play_waits = 0;
    auto_play_choices = 0;
    int label = script_h.getLabelIndex( script_h.getCurrent( true ) );
    if (auto_label_seen.empty()) auto_label_seen.assign( script_h.getNumLabels(), false );
    if (label >= 0 && !auto_label_seen[label]){
        auto_label_seen[label] = true;
        auto_play_same = 0;
    }
    else if (++auto_play_same >= 3000) autoPlayStuck( "3000 pages without a new label" );
    if (checkpoint_every > 0) autoPlayCheckpoint();
    if (auto_play_limit > 0 && ++auto_play_pages >= auto_play_limit){
        utils::printInfo( "auto-play: stopped after %d pages\n", auto_play_pages );
        trace_end_reason = "auto-play page limit";
        if (trace_fp) traceEnd();
        exit(0);
    }
}

/* --goto label and --vars file: a run starts at a label, with variables
 * set, as if "goto *label" were the first line of *start (the define
 * section has run). The vars file is UTF-8: lines "%312=876" and
 * "$5=text to the end of the line", "#" comments. --dump-vars file writes
 * one, at F8 and when the engine exits. */
void ONScripter::setStartGoto( const char *label )
{
    if (label[0] == '*') label++;
    setStr( &start_goto, label );
    for (char *p = start_goto ; *p ; p++) // labels are kept in lower case
        if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
}

void ONScripter::setStartVars( const char *path )
{
    setStr( &start_vars, path );
}

void ONScripter::setDumpVars( const char *path )
{
    static bool exit_hooked = false;
    setStr( &dump_vars_path, path );
    if (!exit_hooked){
        trace_ons = this;
        atexit( traceExitHook );
    }
    exit_hooked = true;
}

void ONScripter::setStartLoad( int slot )
{
    start_load = slot;
}

// --checkpoint-every pages (with --auto-play): save to a new slot every so
// many pages, counting up from --checkpoint-slot (900), so a run can be
// started again from there with --load. -1 leaves a setting as it is.
void ONScripter::setCheckpoints( int pages, int slot )
{
    if (pages >= 0) checkpoint_every = pages;
    if (slot >= 0) checkpoint_slot = slot;
}

// After each page with --checkpoint-every: the save a player would make
// at this click wait, and a "checkpoint" record.
void ONScripter::autoPlayCheckpoint()
{
    if (++checkpoint_pages % checkpoint_every) return;
    int slot = checkpoint_slot + checkpoint_count++;
    if (saveon_flag && internal_saveon_flag) storeSaveFile();
    if (writeSaveFile( slot ) != 0 || !trace_fp) return;
    char buf[80];
    snprintf( buf, sizeof(buf), "{\"t\":\"checkpoint\",\"pg\":%d,\"slot\":%d,\"at\":", trace_page, slot );
    std::string r = buf;
    traceAt( r, script_h.getCurrent( true ) );
    r += '}';
    traceWrite( r );
}

// Called by game, after *start's variables are cleared.
void ONScripter::startAt()
{
    if (start_load >= 0){ // the saved game's variables and place
        if (!loadGame( start_load )){
            utils::printError( "--load: can't load save slot %d\n", start_load );
            startupFailed();
        }
        return;
    }
    if (start_vars) readVarsFile( start_vars );
    if (!start_goto) return;
    bool found = false;
    for (int i=0 ; i<script_h.getNumLabels() && !found ; i++)
        found = !strcmp( script_h.getLabelName( i ), start_goto );
    if (!found){
        utils::printError( "--goto: no label *%s\n", start_goto );
        startupFailed();
    }
    setCurrentLabel( start_goto );
}

void ONScripter::readVarsFile( const char *path )
{
    FILE *fp = ::fopen( path, "rb" ); // not ScriptParser::fopen, which looks in the game folder
    if (!fp){
        utils::printError( "--vars: can't open %s\n", path );
        startupFailed();
    }
    char line[4096];
    int line_no = 0;
    while (fgets( line, sizeof(line), fp )){
        line_no++;
        size_t len = strlen( line );
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        char *p = line;
        if (line_no == 1 && !strncmp( p, "\xef\xbb\xbf", 3 )) p += 3; // a BOM
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == '#') continue;
        char *end;
        long no = (*p == '%' || *p == '$') ? strtol( p+1, &end, 10 ) : -1;
        if (no < 0 || end == p+1 || *end != '='){
            utils::printError( "--vars: %s:%d: not %%n=number or $n=text: %s\n", path, line_no, line );
            fclose( fp );
            startupFailed();
        }
        if (*p == '%') script_h.setNumVariable( no, atoi( end+1 ) );
        else           setStr( &script_h.getVariableData( no ).str, fromUTF8( end+1 ).c_str() );
    }
    fclose( fp );
}

// The numbered variables that aren't 0 or empty, as "%n=number" and
// "$n=text" lines (UTF-8) in the --dump-vars file.
void ONScripter::writeVarsFile()
{
    FILE *fp = ::fopen( dump_vars_path, "wb" );
    if (!fp){
        utils::printError( "--dump-vars: can't write %s\n", dump_vars_path );
        return;
    }
    if (trace_fp) fprintf( fp, "# CONscripter variables, page %d\n", trace_page );
    else          fprintf( fp, "# CONscripter variables\n" );
    for (int i=0 ; i<script_h.variable_range ; i++){
        ScriptHandler::VariableData &vd = script_h.getVariableData( i );
        if (vd.num) fprintf( fp, "%%%d=%d\n", i, vd.num );
    }
    for (int i=0 ; i<script_h.variable_range ; i++){
        ScriptHandler::VariableData &vd = script_h.getVariableData( i );
        if (!vd.str || !vd.str[0]) continue;
        std::string text;
        traceString( text, vd.str, -1, false );
        fprintf( fp, "$%d=%s\n", i, text.c_str() );
    }
    fclose( fp );
}

// Appends ,"vars":{"%1":5,"$2":"text"}: the numbered variables that
// aren't 0 or empty.
void ONScripter::traceVars( std::string &out )
{
    out += ",\"vars\":{";
    bool first = true;
    char buf[32];
    for (int i=0 ; i<script_h.variable_range ; i++){
        ScriptHandler::VariableData &vd = script_h.getVariableData( i );
        if (!vd.num) continue;
        snprintf( buf, sizeof(buf), "%s\"%%%d\":%d", first ? "" : ",", i, vd.num );
        out += buf;
        first = false;
    }
    for (int i=0 ; i<script_h.variable_range ; i++){
        ScriptHandler::VariableData &vd = script_h.getVariableData( i );
        if (!vd.str || !vd.str[0]) continue;
        snprintf( buf, sizeof(buf), "%s\"$%d\":", first ? "" : ",", i );
        out += buf;
        traceString( out, vd.str );
        first = false;
    }
    out += '}';
}

/* --trace-vars "300-899,$10-20": a "var" record each time one of these
 * variables changes (numbers alone are %, $ before a range for strings),
 * found by comparing them after each command, so nothing is done when the
 * option isn't given. */
void ONScripter::setTraceVars( const char *spec )
{
    setStr( &trace_vars_spec, spec );
}

void ONScripter::traceVarsSetup()
{
    for (const char *p = trace_vars_spec ; p && *p ; ){
        bool str = (*p == '$');
        if (*p == '$' || *p == '%') p++;
        char *end;
        int first = strtol( p, &end, 10 ), last = first;
        if (*end == '-') last = strtol( end+1, &end, 10 );
        if (end == p){
            utils::printError( "--trace-vars: can't read [%s]\n", p );
            break;
        }
        if (first < 0 || last >= script_h.variable_range)
            utils::printError( "--trace-vars: only variables 0-%d can be watched\n", script_h.variable_range-1 );
        for (int i=std::max( first, 0 ) ; i<=last && i<script_h.variable_range ; i++){
            if (str) trace_str_vars.push_back( i );
            else     trace_num_vars.push_back( i );
        }
        p = strchr( end, ',' );
        if (p) p++;
    }
    trace_num_seen.assign( trace_num_vars.size(), 0 );
    trace_str_seen.assign( trace_str_vars.size(), "" );
    trace_vars_flag = true;
    traceVarChanges( NULL ); // the values set before the script starts
}

// After a command: a "var" record for each watched variable that changed,
// at the command that changed it.
void ONScripter::traceVarChanges( char *at )
{
    char buf[80];
    for (size_t i=0 ; i<trace_num_vars.size() ; i++){
        int v = script_h.getVariableData( trace_num_vars[i] ).num;
        if (v == trace_num_seen[i]) continue;
        trace_num_seen[i] = v;
        snprintf( buf, sizeof(buf), "{\"t\":\"var\",\"pg\":%d,\"at\":", trace_page );
        std::string r = buf;
        traceAt( r, at );
        snprintf( buf, sizeof(buf), ",\"n\":\"%%%d\",\"v\":%d}", trace_num_vars[i], v );
        r += buf;
        traceWrite( r );
    }
    for (size_t i=0 ; i<trace_str_vars.size() ; i++){
        const char *v = script_h.getVariableData( trace_str_vars[i] ).str;
        if (!v) v = "";
        if (trace_str_seen[i] == v) continue;
        trace_str_seen[i] = v;
        snprintf( buf, sizeof(buf), "{\"t\":\"var\",\"pg\":%d,\"at\":", trace_page );
        std::string r = buf;
        traceAt( r, at );
        snprintf( buf, sizeof(buf), ",\"n\":\"$%d\",\"v\":", trace_str_vars[i] );
        r += buf;
        traceString( r, v );
        r += '}';
        traceWrite( r );
    }
}

/* Web page controls (chimimouryou): the page calls these on the module
 * (module._onsyuriSetAuto(1), ...); each becomes an event the engine
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
EMSCRIPTEN_KEEPALIVE void onsyuriSetAuto( int on ){ pushWebEvent( ONScripter::WEB_SET_AUTO, on ); }
// skip mode on (1) or off (0)
EMSCRIPTEN_KEEPALIVE void onsyuriSetSkip( int on ){ pushWebEvent( ONScripter::WEB_SET_SKIP, on ); }
// ruby drawn (1) or not (0)
EMSCRIPTEN_KEEPALIVE void onsyuriSetRuby( int on ){ pushWebEvent( ONScripter::WEB_SET_RUBY, on ); }
// ms per character, 0 at once, -1 the game's own speed
EMSCRIPTEN_KEEPALIVE void onsyuriSetTextSpeed( int ms ){ pushWebEvent( ONScripter::WEB_SET_TEXT_SPEED, ms ); }
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
// self.onsyuriMode(auto, skip), and when the text window moved, was resized,
// shown or hidden, as self.onsyuriWindow(x, y, w, h, shown) in screen
// pixels. Called as the engine waits.
void ONScripter::webStateHook()
{
#if defined(WEB)
    int mode = (automode_flag ? 1 : 0) | ((skip_mode & SKIP_NORMAL) ? 2 : 0);
    if (mode != web_mode_sent){
        web_mode_sent = mode;
        EM_ASM({ if (self.onsyuriMode) self.onsyuriMode($0, $1); }, mode & 1, mode >> 1);
    }
    SDL_Rect &r = sentence_font_info.pos;
    int window[5] = { r.x, r.y, r.w, r.h, (display_mode & DISPLAY_MODE_TEXT) ? 1 : 0 };
    if (memcmp( window, web_window_sent, sizeof(window) )){
        memcpy( web_window_sent, window, sizeof(window) );
        EM_ASM({ if (self.onsyuriWindow) self.onsyuriWindow($0, $1, $2, $3, $4); },
               window[0], window[1], window[2], window[3], window[4]);
    }
#endif
}

// At each click wait and new page, before onsyuriText: where the text is
// in the script, as self.onsyuriPosition(offset, total, label, read): the
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
    EM_ASM({ if (self.onsyuriPosition) self.onsyuriPosition($0, $1, UTF8ToString($2), $3); },
           script_h.getOffset( address ), script_h.getScriptLength(), name.c_str(), script_h.getKidokuState());
#endif
}

// --trace-long-wait ms: a "long wait" warn when a wait, effect or BGM fade
// asks for more than this, whether or not auto-play shortens it (a
// "print 10,270000" that makes the player wait four minutes).
void ONScripter::setTraceLongWait( int ms )
{
    trace_long_wait = ms;
}

void ONScripter::traceLongWait( int ms )
{
    if (ms <= trace_long_wait) return;
    const char *cmd = trace_recent_count ? trace_recent[(trace_recent_count-1) % TRACE_RECENT].cmd : NULL;
    char buf[96];
    snprintf( buf, sizeof(buf), "{\"t\":\"warn\",\"pg\":%d,\"what\":\"long wait\",\"cmd\":", trace_page );
    std::string r = buf;
    traceString( r, cmd );
    snprintf( buf, sizeof(buf), ",\"ms\":%d,\"at\":", ms );
    r += buf;
    traceAt( r, trace_cmd_start );
    traceVia( r );
    r += '}';
    traceWrite( r );
}
