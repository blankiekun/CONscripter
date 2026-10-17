/* -*- C++ -*-
 *
 *  ONScripter_warp.cpp - CONscripter: transitions drawn from warp maps
 *  ("cons_warp.dll", read by ONScripter::effectRule)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "ONScripter.h"
#include <algorithm>
#include <vector>

/* A warp cell: four planes of gw x gh points stacked top to bottom, each
 * 16-bit value v split into its high byte (planes 0 and 2) and low byte
 * (planes 1 and 3, the same channel):
 *   planes 0/1: R old picture x, G old picture y, B mask;
 *   planes 2/3: R new picture x, G new picture y; B (plane 2 only) 255:
 *   bilinear sampling in the block right of and below that point, 0
 *   nearest; B of plane 3 alone (a byte): that block's mask sharpness s.
 * Point (i, j) sits on screen pixel (i * w / (gw - 1), j * h / (gh - 1)).
 * x and y: the pixel is read from (pixel + (v - 32768) / 16) of that
 * picture (edges repeated). Mask: 128 + (v - 32768) * 2^s / 16, clamped
 * to 0-255, how much of the new picture shows: s 0 holds a level (and
 * a plain level L is v = 30720 + 16 L), s 8 a signed distance to an
 * edge, 1/16 pixel a step, so the edge stays sharp between points and
 * cells. Everything is interpolated bilinearly between points and
 * linearly between cells; the smooth flag and s come from the nearer
 * cell in time. */

namespace {

enum { OLD_X, OLD_Y, MASK, NEW_X, NEW_Y, VALUES };

// A picture's pixels, copied out of its surface so that writing the output
// doesn't make the compiler read the surface's fields again.
struct Picture {
    const Uint8 *pixels;
    int pitch, w, h;
    explicit Picture( const SDL_Surface *s ) : pixels( (const Uint8*)s->pixels ), pitch( s->pitch ), w( s->w ), h( s->h ){}
};

inline Uint32 pixelAt( const Picture &s, int x, int y )
{
    x = x < 0 ? 0 : x >= s.w ? s.w - 1 : x;
    y = y < 0 ? 0 : y >= s.h ? s.h - 1 : y;
    return ((const Uint32*)(s.pixels + y * s.pitch))[x];
}

// x16, y16 in 1/16 pixel; two channels at once in 16-bit lanes.
inline Uint32 bilinearAt( const Picture &s, int x16, int y16 )
{
    int x = x16 >> 4, y = y16 >> 4, fx = x16 & 15, fy = y16 & 15;
    if (x < 0){ x = 0; fx = 0; } else if (x >= s.w - 1){ x = s.w - 1; fx = 0; }
    if (y < 0){ y = 0; fy = 0; } else if (y >= s.h - 1){ y = s.h - 1; fy = 0; }
    const Uint32 *r0 = (const Uint32*)(s.pixels + y * s.pitch) + x;
    const Uint32 *r1 = fy ? (const Uint32*)((const Uint8*)r0 + s.pitch) : r0;
    int nx = fx ? 1 : 0;
    Uint32 a = r0[0], b = r0[nx], c = r1[0], d = r1[nx], out = 0;
    for (int shift=0 ; shift<=8 ; shift+=8){
        Uint32 top = ((a >> shift) & 0xff00ff) * (16 - fx) + ((b >> shift) & 0xff00ff) * fx;
        Uint32 bottom = ((c >> shift) & 0xff00ff) * (16 - fx) + ((d >> shift) & 0xff00ff) * fx;
        out |= (((top * (16 - fy) + bottom * fy + 0x800080) >> 8) & 0xff00ff) << shift;
    }
    return out;
}

// (a * (255 - m) + b * m) / 255 per channel, rounded down as cons_rule does.
inline Uint32 mix( Uint32 a, Uint32 b, int m )
{
    Uint32 out = 0;
    for (int shift=0 ; shift<=8 ; shift+=8){
        Uint32 v = ((a >> shift) & 0xff00ff) * (255 - m) + ((b >> shift) & 0xff00ff) * m;
        out |= (((v + 0x10001 + ((v >> 8) & 0xff00ff)) >> 8) & 0xff00ff) << shift;
    }
    return out;
}

// One picture's pixel at x, y moved by vx, vy (1/65536 pixel).
template <bool BILINEAR> inline Uint32 fetch( const Picture &s, int x, int y, int vx, int vy )
{
    if (BILINEAR) return bilinearAt( s, x * 16 + ((vx + 2048) >> 12), y * 16 + ((vy + 2048) >> 12) );
    return pixelAt( s, x + ((vx + 32768) >> 16), y + ((vy + 32768) >> 16) );
}

enum { ONLY_OLD, ONLY_NEW, BOTH };

// Pixels x0 to x1 of a row from values vin stepping by d; ms the mask
// shift. (Everything in locals: out may alias ints, so the compiler would
// read them again after each pixel written.)
template <bool BILINEAR, int SHOWN>
void drawSpan( Uint32 *out, int x0, int x1, int y, const int *vin, const int *d, int ms, const Picture src, const Picture dst )
{
    int v[VALUES];
    for (int n=0 ; n<VALUES ; n++) v[n] = vin[n];
    for (int x=x0 ; x<x1 ; x++){
        if (SHOWN == ONLY_OLD){
            out[x] = fetch<BILINEAR>( src, x, y, v[OLD_X], v[OLD_Y] );
            v[OLD_X] += d[OLD_X]; v[OLD_Y] += d[OLD_Y];
        }
        else if (SHOWN == ONLY_NEW){
            out[x] = fetch<BILINEAR>( dst, x, y, v[NEW_X], v[NEW_Y] );
            v[NEW_X] += d[NEW_X]; v[NEW_Y] += d[NEW_Y];
        }
        else{
            int m = 128 + ((v[MASK] + (1 << (ms - 1))) >> ms);
            if (m <= 0) out[x] = fetch<BILINEAR>( src, x, y, v[OLD_X], v[OLD_Y] );
            else if (m >= 255) out[x] = fetch<BILINEAR>( dst, x, y, v[NEW_X], v[NEW_Y] );
            else out[x] = mix( fetch<BILINEAR>( src, x, y, v[OLD_X], v[OLD_Y] ), fetch<BILINEAR>( dst, x, y, v[NEW_X], v[NEW_Y] ), m );
            for (int n=0 ; n<VALUES ; n++) v[n] += d[n];
        }
    }
}

}

/* Draws the frame t (0-1) of the way from warp cell c to c + 1 of map
 * (cells side by side, checked by the caller) into accumulation_surface. */
void ONScripter::warpFrame( SDL_Surface *map, int cells, int c, float t )
{
    const int w = accumulation_surface->w, h = accumulation_surface->h;
    const int gw = map->w / cells, gh = map->h / 4;
    // The two cells mixed in time, centred and times 4096 (1/65536 pixel).
    static std::vector<int> grid;
    static std::vector<Uint8> smooth, sharp;
    grid.resize( gw * gh * VALUES );
    smooth.resize( gw * gh );
    sharp.resize( gw * gh );
    const SDL_PixelFormat *f = map->format;
    const int shift[3] = { f->Rshift, f->Gshift, f->Bshift };
    SDL_LockSurface( map );
    for (int j=0 ; j<gh ; j++)
        for (int i=0 ; i<gw ; i++){
            float v[2][VALUES];
            for (int k=0 ; k<2 ; k++){
                int x = (c + k) * gw + i;
                for (int plane=0 ; plane<4 ; plane+=2){
                    Uint32 hi = ((const Uint32*)((const Uint8*)map->pixels + (plane * gh + j) * map->pitch))[x];
                    Uint32 lo = ((const Uint32*)((const Uint8*)map->pixels + ((plane + 1) * gh + j) * map->pitch))[x];
                    for (int ch=0 ; ch<(plane ? 2 : 3) ; ch++)
                        v[k][plane ? NEW_X + ch : ch] = (float)(((int)((hi >> shift[ch]) & 0xff) << 8 | (int)((lo >> shift[ch]) & 0xff)) - 32768);
                    if (plane && k == (t < 0.5f ? 0 : 1)){
                        smooth[j * gw + i] = (hi >> shift[2]) & 0x80;
                        int s = (lo >> shift[2]) & 0xff;
                        sharp[j * gw + i] = s > 8 ? 8 : s;
                    }
                }
            }
            int *g = &grid[(j * gw + i) * VALUES];
            for (int n=0 ; n<VALUES ; n++){
                float x = (v[0][n] + (v[1][n] - v[0][n]) * t) * 4096;
                g[n] = (int)(x < 0 ? x - 0.5f : x + 0.5f); // (lroundf is a slow call in the web build)
            }
        }
    SDL_UnlockSurface( map );

    SDL_LockSurface( effect_src_surface );
    SDL_LockSurface( effect_dst_surface );
    SDL_LockSurface( accumulation_surface );
    const Picture src( effect_src_surface ), dst( effect_dst_surface );
    // Fractions in 1/2^20. The blocks' first and end columns, and how far
    // the first is past its point.
    const Sint64 step = ((Sint64)(gw - 1) << 20) / w;
    std::vector<int> bx( gw );
    std::vector<Sint64> bfx( gw );
    for (int i=0 ; i<gw ; i++){
        bx[i] = std::min( (i * w + gw - 2) / (gw - 1), w );
        bfx[i] = ((Sint64)(bx[i] * (gw - 1) - i * w) << 20) / w;
    }
    // Per block of a row: the values at its first pixel and their steps
    // per pixel, both straight down a grid row, so added to row by row
    // (computing them on every row took as long as drawing a slide).
    const int nb = (gw - 1) * VALUES;
    std::vector<int> row( gw * VALUES ), sv( nb ), sd( nb ), sv1( nb ), sd1( nb );
    for (int j=0 ; j<gh - 1 ; j++){
        int y0 = (j * h + gh - 2) / (gh - 1), y1 = std::min( ((j + 1) * h + gh - 2) / (gh - 1), h );
        if (y0 >= y1) continue;
        for (int k=0 ; k<2 ; k++){
            int *bv = k ? &sv1[0] : &sv[0], *bd = k ? &sd1[0] : &sd[0];
            const int *g0 = &grid[j * gw * VALUES], *g1 = g0 + gw * VALUES;
            Sint64 fy = ((Sint64)((k ? y1 : y0) * (gh - 1) - j * h) << 20) / h;
            for (int n=0 ; n<gw * VALUES ; n++) row[n] = g0[n] + (int)(((Sint64)(g1[n] - g0[n]) * fy) >> 20);
            for (int n=0 ; n<nb ; n++){
                int i = n / VALUES, a = row[n], b = row[n + VALUES];
                bv[n] = a + (int)(((Sint64)(b - a) * bfx[i]) >> 20);
                bd[n] = (int)(((Sint64)(b - a) * step) >> 20);
            }
        }
        for (int n=0 ; n<nb ; n++){
            sv1[n] = (sv1[n] - sv[n]) / (y1 - y0);
            sd1[n] = (sd1[n] - sd[n]) / (y1 - y0);
        }
        for (int y=y0 ; y<y1 ; y++){
            Uint32 *out = (Uint32*)((Uint8*)accumulation_surface->pixels + y * accumulation_surface->pitch);
            for (int i=0 ; i<gw - 1 ; i++){
                int x0 = bx[i], x1 = bx[i + 1];
                if (x0 >= x1) continue;
                const int *v = &sv[i * VALUES], *d = &sd[i * VALUES];
                bool bilinear = smooth[j * gw + i] != 0;
                int ms = 16 - sharp[j * gw + i]; // the mask is in 1/65536 pixel, times 2^s
                // The mask is straight along the block: one picture alone
                // when both ends show only it.
                int m0 = 128 + ((v[MASK] + (1 << (ms - 1))) >> ms);
                int m1 = 128 + ((v[MASK] + d[MASK] * (x1 - x0 - 1) + (1 << (ms - 1))) >> ms);
                int shown = m0 <= 0 && m1 <= 0 ? ONLY_OLD : m0 >= 255 && m1 >= 255 ? ONLY_NEW : BOTH;
                if (bilinear){
                    if (shown == ONLY_OLD) drawSpan<true, ONLY_OLD>( out, x0, x1, y, v, d, ms, src, dst );
                    else if (shown == ONLY_NEW) drawSpan<true, ONLY_NEW>( out, x0, x1, y, v, d, ms, src, dst );
                    else drawSpan<true, BOTH>( out, x0, x1, y, v, d, ms, src, dst );
                }
                else{
                    if (shown == ONLY_OLD) drawSpan<false, ONLY_OLD>( out, x0, x1, y, v, d, ms, src, dst );
                    else if (shown == ONLY_NEW) drawSpan<false, ONLY_NEW>( out, x0, x1, y, v, d, ms, src, dst );
                    else drawSpan<false, BOTH>( out, x0, x1, y, v, d, ms, src, dst );
                }
            }
            for (int n=0 ; n<nb ; n++){
                sv[n] += sv1[n];
                sd[n] += sd1[n];
            }
        }
    }
    SDL_UnlockSurface( accumulation_surface );
    SDL_UnlockSurface( effect_dst_surface );
    SDL_UnlockSurface( effect_src_surface );
}
