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
#include <algorithm>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(WEB)
#include <emscripten.h>
#endif

// Marks the block saveSaveFileCons() appends to a save file. Saves
// written without it (stock ONScripter, older CONscripter) end before it,
// and readInt() returns 0 past the end of the data.
#define CONS_SAVE_MAGIC   0x534e4f43 // "CONS"
#define CONS_SAVE_VERSION 2 // 1: sprite filters; 2: + screen stack

/* spfilter num, kind[, blend[, strength[, param...]]]
 *   kind 0 clears every filter on sprite num. Other kinds add a filter
 *   (up to AnimationInfo::MAX_FILTER_NUM, applied in order) that makes a
 *   colour for each pixel: 1 the colour param r,g,b; 2 the pixel in
 *   greyscale; 3 its negative; 4 the pixel plus param (-255..255).
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
    else if (f.kind < AnimationInfo::FILTER_COLOUR || f.kind > AnimationInfo::FILTER_BRIGHTNESS){
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
 *   visibility, cell, transparency, filters) and the BGM on a stack.
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
}

void ONScripter::loadSaveFileCons()
{
    clearScreenStack();
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
