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
}

ParticleLayer::~ParticleLayer()
{
  clearSheets();
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
      else if (k == "y") { readFloats(v, f, 2); prm.y0 = f[0]; prm.yh = f[1]; }
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

    const int ntint = prm.tints.empty() ? 1 : (int)prm.tints.size();
    pts.assign(prm.n, Pt());
    for (Pt &p : pts) {
      const float ca = U() * prm.cls[0];
      p.c = ca + (U() > prm.cls[2] ? prm.cls[1] : 0);
      p.x = p.y = 0;
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
  }
  p.alpha = 255;
  p.active = true;
}

void ParticleLayer::step(Pt &p)
{
  if (!p.active) {
    if (U() < prm.spawn) spawn(p);
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

  if (!prm.vy.empty()) {
    const float span = prm.cls[0] + prm.cls[1];
    int i = span > 0 ? (int)(p.c / span * prm.vy.size()) : 0;
    if (i < 0) i = 0;
    if (i >= (int)prm.vy.size()) i = (int)prm.vy.size() - 1;
    p.y += prm.vy[i];
  }

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

void ParticleLayer::refresh(SDL_Surface *surface, SDL_Rect &clip)
{
  if (!running || sheets.empty()) return;
  for (size_t i = 0; i < pts.size(); i++) {
    const Pt &p = pts[i];
    if (!p.active) continue;
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
    if (ia <= 0) continue;
    if (ia > 255) ia = 255;
    a->blendOnSurface(surface, (int)(p.x * prm.scale + prm.ox), (int)(p.y * prm.scale + prm.oy), clip, ia);
  }
}
#endif
