/* -*- C++ -*-
*
*  layer_particle.cpp
*    - CONscripter: setlayer num,interval,"particle", a pool of particles
*      with live random motion; every number comes from layermessage
*      "s|key=value,..." (COMMANDS.md "setlayer")
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

#ifdef USE_BUILTIN_LAYER_EFFECTS
#include "../builtin_layer.h"
#include "../ONScripter.h"
#include "../Utils.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#if defined(ANDROID)
#include "SDL_image.h"
#else
#include <SDL2/SDL_image.h>
#endif

extern ONScripter ons;

// uniform in [0,1) from the engine's RNG (rand(), as rnd and quake use)
static inline float U() { return (float)(rand() / (RAND_MAX + 1.0)); }

ParticleLayer::ParticleLayer(int w, int h, BaseReader *br)
{
  width = w;
  height = h;
  reader = br;
  sprite_info = sprite = NULL;
  cols = rows = 1;
  running = false;
  time = 0;
  last_ticks = 0;
  acc_ms = 0;
  tex_surf = NULL;
  tex_dirty = false;
}

ParticleLayer::~ParticleLayer()
{
  clearSheets();
  if (tex_surf) SDL_FreeSurface(tex_surf);
}

// Draws cell (sx0, 0, sw, sh) of src centred at (cx, cy) on dst inside clip,
// zoomed zx, zy (negative flips) and turned deg counterclockwise (as lsp2),
// nearest texel, at alpha 0-255; copy: the texel (alpha scaled) replaces
// what is under it, else it is blended over it. 32-bit surfaces, alpha in
// the format's Amask.
static void affineBlit(SDL_Surface *src, int sx0, int sw, int sh, SDL_Surface *dst, const SDL_Rect &clip,
                       float cx, float cy, float zx, float zy, float deg, int alpha, bool copy)
{
  if (!src || !dst || zx == 0 || zy == 0 || alpha <= 0 || sw <= 0 || sh <= 0) return;
  const float r = deg * (float)M_PI / 180, c = cosf(r), s = sinf(r);
  // forward: X = cx + zx*u*c + zy*v*s, Y = cy - zx*u*s + zy*v*c (u, v from the cell's centre)
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (int k = 0; k < 4; k++) {
    const float u = (k & 1 ? sw : -sw) * 0.5f * zx, v = (k & 2 ? sh : -sh) * 0.5f * zy;
    const float X = cx + u * c + v * s, Y = cy - u * s + v * c;
    if (X < x0) x0 = X;
    if (X > x1) x1 = X;
    if (Y < y0) y0 = Y;
    if (Y > y1) y1 = Y;
  }
  int bx0 = (int)floorf(x0), by0 = (int)floorf(y0), bx1 = (int)ceilf(x1), by1 = (int)ceilf(y1);
  if (bx0 < clip.x) bx0 = clip.x;
  if (by0 < clip.y) by0 = clip.y;
  if (bx1 > clip.x + clip.w) bx1 = clip.x + clip.w;
  if (by1 > clip.y + clip.h) by1 = clip.y + clip.h;
  if (bx0 < 0) bx0 = 0;
  if (by0 < 0) by0 = 0;
  if (bx1 > dst->w) bx1 = dst->w;
  if (by1 > dst->h) by1 = dst->h;
  if (bx0 >= bx1 || by0 >= by1) return;
  if (sx0 + sw > src->w) sw = src->w - sx0;
  if (sh > src->h) sh = src->h;
  const Uint32 sa = src->format->Amask, sas = src->format->Ashift, da = dst->format->Amask, das = dst->format->Ashift;
  const float du = c / zx, dv = s / zy; // per pixel along X
  SDL_LockSurface(src);
  SDL_LockSurface(dst);
  for (int Y = by0; Y < by1; Y++) {
    const float dy = Y + 0.5f - cy, dx = bx0 + 0.5f - cx;
    float u = (c * dx - s * dy) / zx + sw * 0.5f, v = (s * dx + c * dy) / zy + sh * 0.5f;
    Uint32 *d = (Uint32 *)((Uint8 *)dst->pixels + Y * dst->pitch) + bx0;
    for (int X = bx0; X < bx1; X++, d++, u += du, v += dv) {
      if (u < 0 || v < 0) continue;
      const int iu = (int)u, iv = (int)v;
      if (iu >= sw || iv >= sh) continue;
      const Uint32 p = ((const Uint32 *)((const Uint8 *)src->pixels + iv * src->pitch))[sx0 + iu];
      const Uint32 a = ((p & sa) >> sas) * alpha / 255;
      if (a == 0) continue;
      if (copy) { *d = (p & ~sa) | (a << das & da); continue; }
      const Uint32 m2 = a + (a >> 7), m1 = 256 - m2;
      const Uint32 rb = (((*d & 0xff00ff) * m1 + (p & 0xff00ff) * m2 + 0x800080) >> 8) & 0xff00ff;
      const Uint32 g = (((*d & 0x00ff00) * m1 + (p & 0x00ff00) * m2 + 0x008000) >> 8) & 0x00ff00;
      *d = rb | g | da;
    }
  }
  SDL_UnlockSurface(dst);
  SDL_UnlockSurface(src);
}

void ParticleLayer::clearSheets()
{
  for (AnimationInfo *a : sheets) delete a;
  sheets.clear();
}

void ParticleLayer::loadSheets(const char *file)
{
  clearSheets();
  unsigned long length = reader->getFileLength(file);
  if (length == 0) {
    utils::printError("particle: can't find [%s]\n", file);
    return;
  }
  unsigned char *buffer = new unsigned char[length];
  int location;
  reader->getFile(file, buffer, &location);
  SDL_Surface *tmp = IMG_Load_RW(SDL_RWFromMem(buffer, length), 1);
  delete[] buffer;
  if (!tmp) {
    utils::printError("particle: can't load [%s]\n", file);
    return;
  }
  SDL_Surface *ref = AnimationInfo::alloc32bitSurface(1, 1, ons.getTextureFormat());
  SDL_Surface *src = SDL_ConvertSurface(tmp, ref->format, SDL_SWSURFACE);
  SDL_FreeSurface(tmp);
  SDL_FreeSurface(ref);
  if (!src) return;

  cols = src->w / prm.cw;
  rows = src->h / prm.ch;
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  const int cells = cols * rows;
  std::vector<Uint32> tints = prm.tints;
  if (tints.empty()) tints.push_back(0xffffff);

  SDL_PixelFormat *fmt = src->format;
  SDL_LockSurface(src);
  for (Uint32 tint : tints) {
    const Uint32 tr = (tint >> 16) & 0xff, tg = (tint >> 8) & 0xff, tb = tint & 0xff;
    // the grid's cells re-packed in one row: cell row*cols+col
    SDL_Surface *dst = SDL_CreateRGBSurface(SDL_SWSURFACE, prm.cw * cells, prm.ch,
      32, fmt->Rmask, fmt->Gmask, fmt->Bmask, fmt->Amask);
    if (!dst) break;
    SDL_LockSurface(dst);
    for (int r = 0; r < rows; r++)
      for (int c = 0; c < cols; c++)
        for (int y = 0; y < prm.ch; y++) {
          const Uint32 *s = (const Uint32 *)((const Uint8 *)src->pixels + (r * prm.ch + y) * src->pitch) + c * prm.cw;
          Uint32 *d = (Uint32 *)((Uint8 *)dst->pixels + y * dst->pitch) + (r * cols + c) * prm.cw;
          for (int x = 0; x < prm.cw; x++) {
            const Uint32 p = s[x];
            const Uint32 pr = ((p & fmt->Rmask) >> fmt->Rshift) * tr / 255;
            const Uint32 pg = ((p & fmt->Gmask) >> fmt->Gshift) * tg / 255;
            const Uint32 pb = ((p & fmt->Bmask) >> fmt->Bshift) * tb / 255;
            d[x] = (p & fmt->Amask) | pr << fmt->Rshift | pg << fmt->Gshift | pb << fmt->Bshift;
          }
        }
    SDL_UnlockSurface(dst);
    AnimationInfo *anim = new AnimationInfo();
    anim->num_of_cells = cells;
    anim->duration_list = new int[cells];
    for (int j = 0; j < cells; j++) anim->duration_list[j] = 0;
    anim->loop_mode = 3; // not animatable
    anim->trans_mode = AnimationInfo::TRANS_ALPHA;
    anim->setImage(anim->setupImageAlpha(dst, NULL, true), ons.getTextureFormat());
    anim->visible = true;
    sheets.push_back(anim);
  }
  SDL_UnlockSurface(src);
  SDL_FreeSurface(src);
}

// "a:b:c" into out[0..max-1]; returns how many were read
static int readFloats(const std::string &v, float *out, int max)
{
  int n = 0;
  const char *p = v.c_str();
  while (n < max && *p) {
    char *end;
    out[n++] = strtof(p, &end);
    if (*end != ':') break;
    p = end + 1;
  }
  return n;
}

static std::vector<std::string> split(const std::string &s, char sep)
{
  std::vector<std::string> out;
  size_t a = 0;
  while (true) {
    size_t b = s.find(sep, a);
    out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

char *ParticleLayer::message(const char *message, int &ret_int)
{
  ret_int = 0;
  if (!strncmp(message, "s|", 2)) {
    prm = Params();
    std::string img;
    for (const std::string &item : split(message + 2, ',')) {
      size_t eq = item.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
      float f[5] = { 0, 0, 0, 0, 0 };
      if (k == "img") img = v;
      else if (k == "cw") prm.cw = atoi(v.c_str());
      else if (k == "ch") prm.ch = atoi(v.c_str());
      else if (k == "n") prm.n = atoi(v.c_str());
      else if (k == "hz") prm.hz = atoi(v.c_str());
      else if (k == "steps") prm.steps = atoi(v.c_str());
      else if (k == "warm") prm.warm = atoi(v.c_str());
      else if (k == "spawn") prm.spawn = strtof(v.c_str(), NULL);
      else if (k == "scale") prm.scale = strtof(v.c_str(), NULL);
      else if (k == "ox") prm.ox = strtof(v.c_str(), NULL);
      else if (k == "oy") prm.oy = strtof(v.c_str(), NULL);
      else if (k == "cls") readFloats(v, prm.cls, 3);
      else if (k == "vy") {
        for (const std::string &s : split(v, ':')) prm.vy.push_back(strtof(s.c_str(), NULL));
      }
      else if (k == "x") { readFloats(v, f, 2); prm.x0 = f[0]; prm.xw = f[1]; }
      else if (k == "y") { readFloats(v, f, 3); prm.y0 = f[0]; prm.yh = f[1]; prm.yh2 = f[2]; }
      else if (k == "dx") { readFloats(v, prm.dx, 2); prm.path = true; }
      else if (k == "ey") prm.ey = strtof(v.c_str(), NULL);
      else if (k == "swing") prm.swing = strtof(v.c_str(), NULL);
      else if (k == "cscale") {
        for (const std::string &s : split(v, ':')) prm.cscale.push_back(strtof(s.c_str(), NULL));
      }
      else if (k == "spin") readFloats(v, prm.spin, 4);
      else if (k == "tex") { readFloats(v, prm.tex, 4); prm.has_tex = prm.tex[0] >= 1 && prm.tex[1] >= 1; }
      else if (k == "side") readFloats(v, prm.side, 5);
      else if (k == "walk") prm.walk_y = (v == "y");
      else if (k == "wv") readFloats(v, prm.wv, 2);
      else if (k == "wa") readFloats(v, prm.wa, 3);
      else if (k == "end") { readFloats(v, prm.end, 3); prm.has_end = true; }
      else if (k == "fade") readFloats(v, prm.fade, 2);
      else if (k == "box") { readFloats(v, prm.box, 4); prm.has_box = true; }
      else if (k == "drift") readFloats(v, prm.drift, 4);
      else if (k == "flip") readFloats(v, prm.flip, 4);
      else if (k == "tint") {
        for (const std::string &s : split(v, ':')) prm.tints.push_back((Uint32)strtoul(s.c_str(), NULL, 16) & 0xffffff);
      }
      else if (k == "twinkle") {
        readFloats(v, f, 2);
        prm.twinkle_n = (int)f[0];
        prm.twinkle_step = f[1];
      }
      else utils::printError("particle: unknown key '%s'\n", k.c_str());
    }
    if (prm.n < 0) prm.n = 0;
    if (prm.n > 10000) prm.n = 10000;
    if (prm.cw < 1) prm.cw = 1;
    if (prm.ch < 1) prm.ch = 1;
    if (prm.hz < 1) prm.hz = 1;
    if (prm.steps < 1) prm.steps = 1;
    if (prm.twinkle_n < 0) prm.twinkle_n = 0;
    if (prm.flip[0] < 1) prm.flip[0] = 1;
    if (img.empty()) clearSheets();
    else loadSheets(img.c_str());
    if (tex_surf) SDL_FreeSurface(tex_surf);
    tex_surf = prm.has_tex ? AnimationInfo::alloc32bitSurface((int)prm.tex[0], (int)prm.tex[1], ons.getTextureFormat()) : NULL;
    tex_dirty = true;

    const int ntint = prm.tints.empty() ? 1 : (int)prm.tints.size();
    pts.assign(prm.n, Pt());
    for (Pt &p : pts) {
      const float ca = U() * prm.cls[0];
      p.c = ca + (U() > prm.cls[2] ? prm.cls[1] : 0);
      p.x = p.y = 0;
      p.px = p.py = p.pdx = p.pt = 0;
      p.ax = p.ay = p.az = 0;
      p.wv = 0;
      p.wa = U() * prm.wa[2];
      const float d = prm.drift[0] * p.c + prm.drift[1] + prm.drift[2] * U();
      p.drift = U() < 0.5f ? d : -d;
      p.alpha = 255;
      p.ramp = 1;
      p.fdir = 1;
      p.frame = 0;
      p.last = 0;
      p.tint = (int)(U() * ntint);
      if (p.tint >= ntint) p.tint = ntint - 1;
      p.active = false;
    }
    tw_val.assign(prm.twinkle_n, 0);
    tw_dir.assign(prm.twinkle_n, 0);
    for (int i = 0; i < prm.twinkle_n; i++) {
      tw_val[i] = U();
      tw_dir[i] = U() < 0.5f ? 1 : -1;
    }
    time = 0;
    for (int i = 0; i < prm.warm; i++)
      for (Pt &p : pts) step(p);
    running = true;
    acc_ms = 0;
    last_ticks = SDL_GetTicks();
  } else if (!strcmp(message, "x")) {
    running = false;
    pts.clear();
  } else if (!strcmp(message, "n")) {
    for (const Pt &p : pts)
      if (p.active) ret_int++;
  } else if (!strcmp(message, "t")) {
    ret_int = time;
  } else if (!strcmp(message, "p")) {
    // every active particle as "x:y:c:alpha;" (checks against the original)
    std::string dump;
    char buf[96];
    for (const Pt &p : pts) {
      if (!p.active) continue;
      snprintf(buf, sizeof(buf), "%.3f:%.3f:%.3f:%.1f;", p.x, p.y, p.c, p.alpha);
      dump += buf;
      ret_int++;
    }
    // a fresh allocation: the caller owns it (getret_str), as FuruLayer's
    char *ret = new char[dump.size() + 1];
    memcpy(ret, dump.c_str(), dump.size() + 1);
    return ret;
  }
  return NULL;
}

void ParticleLayer::spawn(Pt &p)
{
  if (prm.side[0] > 0 && U() < prm.side[0]) {
    p.x = U() < 0.5f ? prm.side[1] : prm.side[2];
    p.y = prm.side[3] + U() * (prm.side[4] - prm.side[3]);
  } else {
    p.x = prm.x0 + U() * prm.xw;
    p.y = prm.y0 + U() * prm.yh;
    if (prm.yh2 != 0) p.y += U() * prm.yh2;
  }
  if (prm.path) {
    p.px = p.x;
    p.py = p.y;
    p.pdx = prm.dx[0] + U() * (prm.dx[1] - prm.dx[0]);
    p.pt = 0;
  }
  p.alpha = 255;
  p.active = true;
}

// v's entry for p's class, the list spread over the classes [0,a+b)
float ParticleLayer::byClass(const std::vector<float> &v, const Pt &p) const
{
  if (v.empty()) return 0;
  const float span = prm.cls[0] + prm.cls[1];
  int i = span > 0 ? (int)(p.c / span * v.size()) : 0;
  if (i < 0) i = 0;
  if (i >= (int)v.size()) i = (int)v.size() - 1;
  return v[i];
}

// the point pt along p's path: a line from its start to (start x + dx, ey),
// or with swing a cubic Bezier whose control points sit at y = ey/4 and
// 3ey/4 (whole numbers, measured from 0, not from the start), swing left of
// both ends at the first and swing right of them at the second (one S
// sway), mirrored for every other particle (Muramasa.exe 00434200 calling
// 00414260: ey>>2, (ey>>1)+(ey>>2); the side by its per-drop seed's parity)
void ParticleLayer::pathPos(Pt &p)
{
  const float len = prm.ey - p.py, u = len > 0 ? p.pt / len : 0;
  const float xe = p.px + p.pdx;
  if (prm.swing == 0) {
    p.x = p.px + p.pdx * u;
    p.y = p.py + p.pt;
    return;
  }
  const float lo = (p.px < xe ? p.px : xe) - prm.swing, hi = (p.px > xe ? p.px : xe) + prm.swing;
  const bool flip = ((&p - pts.data()) & 1) == 0;
  const float x1 = flip ? hi : lo, x2 = flip ? lo : hi;
  const int ey = (int)prm.ey;
  const float y1 = (float)(ey >> 2), y2 = (float)((ey >> 1) + (ey >> 2));
  const float a = 1 - u, b0 = a * a * a, b1 = 3 * a * a * u, b2 = 3 * a * u * u, b3 = u * u * u;
  p.x = b0 * p.px + b1 * x1 + b2 * x2 + b3 * xe;
  p.y = b0 * p.py + b1 * y1 + b2 * y2 + b3 * prm.ey;
}

void ParticleLayer::step(Pt &p)
{
  if (!p.active) {
    if (U() < prm.spawn) spawn(p);
    return;
  }
  if (prm.spin[0] > 0 || prm.spin[1] > 0 || prm.spin[2] > 0) {
    // particle i turns (i mod m) + 1 units a step about each axis with a modulus m
    const int i = (int)(&p - pts.data());
    float *ang[3] = { &p.ax, &p.ay, &p.az };
    for (int k = 0; k < 3; k++) {
      const int m = (int)prm.spin[k];
      if (m > 0) *ang[k] = fmodf(*ang[k] + (i % m + 1) * prm.spin[3], 360);
    }
  }
  if (prm.path) {
    // along its own path, back to its own start at the end
    p.pt += byClass(prm.vy, p);
    const float len = prm.ey - p.py;
    if (len <= 0) p.pt = 0;
    else {
      while (p.pt >= len) p.pt -= len;
      while (p.pt < 0) p.pt += len;
    }
    pathPos(p);
    return;
  }
  p.x += p.fdir * p.drift * prm.drift[3] * p.ramp;
  p.ramp = (p.ramp + prm.flip[2]) * prm.flip[3];
  if (p.ramp > 1) p.ramp = 1;

  float &w = prm.walk_y ? p.y : p.x;
  w += p.wv;
  p.wv += p.wa;
  p.wa += (U() - 0.5f) * prm.wa[1];
  if (p.wv < prm.wv[0]) p.wv = prm.wv[0];
  if (p.wv > prm.wv[1]) p.wv = prm.wv[1];
  if (p.wa < -prm.wa[0]) p.wa = -prm.wa[0];
  if (p.wa > prm.wa[0]) p.wa = prm.wa[0];

  if (!prm.vy.empty()) p.y += byClass(prm.vy, p);

  if (prm.has_end) {
    const float line = prm.end[0] + prm.end[1] * p.c;
    if (prm.end[2] >= 0 ? p.y >= line : p.y <= line) {
      if (p.x < prm.x0 || p.x >= prm.x0 + prm.xw) { spawn(p); return; }
      if (p.alpha - prm.fade[1] <= 0) { spawn(p); return; }
      p.alpha -= prm.fade[0];
    }
  }
  if (prm.has_box &&
      (p.x < prm.box[0] || p.y < prm.box[1] || p.x >= prm.box[2] || p.y >= prm.box[3])) {
    spawn(p);
    return;
  }
  if (prm.flip[1] > 0 && time - p.last >= prm.flip[1]) {
    p.last = time;
    const int frames = (int)prm.flip[0];
    if (frames > 1) {
      p.frame += p.fdir > 0 ? 1 : -1;
      if (p.frame <= 0 || p.frame >= frames - 1) {
        p.fdir = -p.fdir;
        p.ramp = 0;
      }
    }
  }
}

void ParticleLayer::tick()
{
  for (int s = 0; s < prm.steps; s++)
    for (Pt &p : pts) step(p);
  for (int i = 0; i < prm.twinkle_n; i++) {
    tw_val[i] += tw_dir[i] * prm.twinkle_step;
    if (tw_val[i] >= 1) { tw_val[i] = 1; tw_dir[i] = -1; }
    if (tw_val[i] <= 0) { tw_val[i] = 0; tw_dir[i] = 1; }
  }
  time++;
  tex_dirty = true;
}

void ParticleLayer::update()
{
  const Uint32 now = SDL_GetTicks();
  if (!running) { last_ticks = now; return; }
  acc_ms += (float)(now - last_ticks) * ons.getGameSpeed() / 100;
  last_ticks = now;
  const float tick_ms = 1000.0f / prm.hz;
  for (int i = 0; i < 8 && acc_ms >= tick_ms; i++) {
    tick();
    acc_ms -= tick_ms;
  }
  if (acc_ms >= tick_ms) acc_ms = 0; // after a long stop: drop the backlog
}

void ParticleLayer::drawParticle(const Pt &p, int i, SDL_Surface *dst, SDL_Rect &clip, float ox, float oy, bool copy)
{
  AnimationInfo *a = sheets[p.tint < (int)sheets.size() ? p.tint : 0];
  int row = (int)p.c, col = p.frame;
  if (row < 0) row = 0;
  if (row >= rows) row = rows - 1;
  if (col < 0) col = 0;
  if (col >= cols) col = cols - 1;
  a->current_cell = row * cols + col;
  float alpha = p.alpha;
  if (prm.twinkle_n > 0) alpha *= tw_val[i % prm.twinkle_n];
  int ia = (int)(alpha + 0.5f);
  if (ia <= 0) return;
  if (ia > 255) ia = 255;
  const float cs = prm.cscale.empty() ? 1 : byClass(prm.cscale, p) / 1000;
  const float x = p.x * prm.scale + ox, y = p.y * prm.scale + oy;
  if (!copy && cs == 1 && p.ax == 0 && p.ay == 0 && p.az == 0) {
    a->blendOnSurface(dst, (int)x, (int)y, clip, ia);
    return;
  }
  // ours: a turn about the x or y axis squashes the height or width by its
  // cosine; the original's 3D perspective is left out
  const float d2r = (float)M_PI / 180;
  const float zx = cs * fabsf(cosf(p.ay * d2r)), zy = cs * fabsf(cosf(p.ax * d2r));
  const int cw = a->pos.w, ch = a->pos.h;
  affineBlit(a->image_surface, a->current_cell * cw, cw, ch, dst, clip,
             x + cw * 0.5f, y + ch * 0.5f, zx, zy, p.az, ia, copy);
}

void ParticleLayer::refresh(SDL_Surface *surface, SDL_Rect &clip)
{
  if (!running || sheets.empty()) return;
  if (tex_surf) {
    // tex: the particles go on their own picture (each texel replacing
    // what is under it), which is drawn as the sprite "*n" is placed:
    // its offset, spzoom and spmotion zoom and angle about the picture's
    // centre, and its alpha
    if (tex_dirty) {
      SDL_FillRect(tex_surf, NULL, 0);
      SDL_Rect all = { 0, 0, tex_surf->w, tex_surf->h };
      for (size_t i = 0; i < pts.size(); i++)
        if (pts[i].active) drawParticle(pts[i], (int)i, tex_surf, all, prm.ox, prm.oy, true);
      tex_dirty = false;
    }
    float zx = 1, zy = 1, rot = 0, offx = 0, offy = 0;
    int alpha = 255;
    if (sprite) {
      const float F = AnimationInfo::MOTION_FINE;
      zx = sprite->zoom_x / 100.0f * sprite->motion_zoom / (100 * F);
      zy = sprite->zoom_y / 100.0f * sprite->motion_zoom_y / (100 * F);
      rot = sprite->zoom_rot + sprite->motion_rot / F;
      offx = (float)(sprite->pos.x + sprite->motion_dx);
      offy = (float)(sprite->pos.y + sprite->motion_dy);
      alpha = (sprite->trans < 0 ? 255 : sprite->trans) * sprite->motion_alpha / 256;
    }
    affineBlit(tex_surf, 0, tex_surf->w, tex_surf->h, surface, clip,
               offx + prm.tex[2] + prm.tex[0] * 0.5f, offy + prm.tex[3] + prm.tex[1] * 0.5f,
               zx, zy, rot, alpha, false);
    return;
  }
  for (size_t i = 0; i < pts.size(); i++)
    if (pts[i].active) drawParticle(pts[i], (int)i, surface, clip, prm.ox, prm.oy, false);
}
#endif
