/* -*- C++ -*-
 *
 *  ONScripter_audio.h - CONscripter: what ONScripter_audio.cpp shares with
 *  the other files in src/cons (script volumes and pans, mutes)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef __ONSCRIPTER_AUDIO_H__
#define __ONSCRIPTER_AUDIO_H__

#include "ONScripter.h"
#include <string>
#include <vector>

struct TrackGain { volatile int from, to, ms, curve; volatile Uint32 start; int last; };
struct TrackPan { volatile int from, to, ms; volatile Uint32 start; int last; };
extern TrackGain track_gain[ONS_MIX_CHANNELS];
extern TrackPan track_pan[ONS_MIX_CHANNELS];

#define SOUND_MUTE_BGM   1
#define SOUND_MUTE_SE    2
#define SOUND_MUTE_VOICE 4
extern volatile int sound_mute;

// Appends text (as JSON when json) to out (ONScripter_trace.cpp).
void traceString( std::string &out, const char *text, int len=-1, bool json=true );
int curveShare( const std::vector<int> *tables, int curve, int p );

#endif
