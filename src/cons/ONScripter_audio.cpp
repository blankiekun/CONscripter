/* -*- C++ -*-
 *
ONScripter_audio.cpp - CONscripter: channel and music volume, fades, loops and tracks (ch*, bgm*, dwave fades, audio stats)
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

extern SDL_TimerID timer_bgmfade_id; // bgmfadein, bgmfadeout


/* chvolfade: each dwave channel's script volume, a percentage of the
 * setting's, ramped linearly from `from` to `to` over `ms` from `start`.
 * The track effect below scales the samples by it (in 1/4096, from the
 * last mix's gain to this one's across the buffer, so a ramp has no
 * steps); SDL_mixer's channel volume, which the settings and dwavefadein
 * use, stays theirs. It stays with the channel, for the next sounds too,
 * so a script can set it before the sound starts; reset sets 100. */
TrackGain track_gain[ONS_MIX_CHANNELS];
/* chvolfade's stop with a curve: the sound fades out along it over ms from
 * start (on top of track_gain), then SDL_mixer halts it (Mix_ExpireChannel).
 * A new sound on the channel plays without it. */
static struct { volatile int ms, curve; volatile Uint32 start; } track_out[ONS_MIX_CHANNELS];
static bool track_gain_s16 = false, track_stereo = false;
/* chpan: each dwave channel's pan, -1000 (left) .. 1000 (right), ramped
 * as track_gain; the track effect lowers the other side (stereo only). */
TrackPan track_pan[ONS_MIX_CHANNELS];

// spcurve's tables, for the ramps' curves (the mixer reads them).
static const std::vector<int> *ramp_curves = NULL;

// How far (0-1024) along curve (spmotion's: 0 straight, 1-3, 4 jump at the
// end, tables) a ramp is at p (0-1024) of its time; tables: CURVE_FIRST on.
int curveShare( const std::vector<int> *tables, int curve, int p )
{
    if (p <= 0) return 0;
    if (p >= 1024) return 1024;
    switch (curve){
      case 1: return p * p / 1024;
      case 2: return 1024 - (1024 - p) * (1024 - p) / 1024;
      case 3: return (int)((long long)p * p * (3 * 1024 - 2 * p) / (1024 * 1024));
      case 4: return 0;
    }
    enum { FIRST = 10, COUNT = 64 }; // ONScripter's CURVE_FIRST, MAX_CURVES
    if (!tables || curve < FIRST || curve >= FIRST + COUNT) return p;
    const std::vector<int> &v = tables[curve - FIRST];
    if (v.size() < 2) return p;
    int n = (int)v.size() - 1, i = p * n / 1024, r = p * n % 1024;
    return v[i] + (int)((long long)(v[i+1] - v[i]) * r / 1024);
}

static int scriptVolumeAt( int from, int to, int ms, Uint32 start, Uint32 now, int curve=0 )
{
    Uint32 t = now - start;
    if (ms <= 0 || t >= (Uint32)ms) return to;
    if (curve) return from + (int)((long long)(to - from) * curveShare( ramp_curves, curve, (int)((long long)t * 1024 / ms) ) / 1024);
    return from + (to - from) * (int)t / ms;
}

/* soundmute: the groups the player has muted (SOUND_MUTE_*). The track
 * effect below silences a muted channel's sound (ramped over a buffer, so
 * no click) and it goes on playing unheard; the music's volume is 0. */
volatile int sound_mute = 0;

static bool channelMuted( int channel )
{
    int group = channel == 0 ? SOUND_MUTE_VOICE : channel == MIX_BGM_CHANNEL ? SOUND_MUTE_BGM : SOUND_MUTE_SE;
    return (sound_mute & group) != 0;
}

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
    Uint32 now = SDL_GetTicks();
    int gain = scriptVolumeAt( track_gain[channel].from, track_gain[channel].to, track_gain[channel].ms,
                               track_gain[channel].start, now, track_gain[channel].curve ) * 4096 / 100;
    if (track_out[channel].ms > 0) // fading out to its stop
        gain = gain * (100 - scriptVolumeAt( 0, 100, track_out[channel].ms, track_out[channel].start, now, track_out[channel].curve )) / 100;
    return gain;
}


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


/* chvolfade ch,vol[,ms[,stop[,curve]]]
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
 *   it as dwavestop does. "chvolfade 2,0,1500,1" fades the rain out.
 *   curve (an spmotion curve: 1-3, 10-73 from spcurve) is how the volume
 *   goes there, straight without one: "chvolfade 2,0,1500,1,2" fades out
 *   slowing down. */
int ONScripter::chvolfadeCommand()
{
    int ch = script_h.readInt();
    int vol = script_h.readInt();
    int ms = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : 0;
    bool stop = (script_h.getEndStatus() & ScriptHandler::END_COMMA) && script_h.readInt() == 1;
    int curve = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : 0;
    ramp_curves = curve_table;
    traceChannel( ch );
    if (ch == -1){ // the music
        if (vol < 0) vol = 0; else if (vol > 100) vol = 100;
        bgm_script_from = bgmScriptVolume();
        bgm_script_start = SDL_GetTicks();
        bgm_script_to = vol;
        bgm_script_curve = curve;
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
        // Along a curve: faded by the track effect, then halted.
        if (curve && ms > 0 && audio_open_flag && !Mix_Paused( ch ) && Mix_Playing( ch )){
            track_out[ch].ms = 0; // the mixer reads these as they change
            track_out[ch].curve = curve;
            track_out[ch].start = SDL_GetTicks();
            track_out[ch].ms = ms;
            Mix_ExpireChannel( ch, ms );
            return RET_CONTINUE;
        }
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
    int from = scriptVolumeAt( track_gain[ch].from, track_gain[ch].to, track_gain[ch].ms, track_gain[ch].start, now, track_gain[ch].curve );
    // The mixer reads these as they change: no ramp until the last one
    track_gain[ch].ms = 0;
    track_gain[ch].from = from;
    track_gain[ch].start = now;
    track_gain[ch].to = vol;
    track_gain[ch].curve = curve;
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


// chvolfade back to 100, at once (reset, startup).
void ONScripter::resetScriptVolumes()
{
    for (int i=0 ; i<ONS_MIX_CHANNELS ; i++){
        track_gain[i].ms = 0;
        track_gain[i].from = track_gain[i].to = 100;
        track_gain[i].curve = 0;
        track_gain[i].last = 4096;
        track_out[i].ms = 0;
        track_pan[i].ms = 0;
        track_pan[i].from = track_pan[i].to = track_pan[i].last = 0;
    }
    bgm_script_from = bgm_script_to = 100;
    bgm_script_ms = 0;
}

// chvolfade -1's volume now, percent.
int ONScripter::bgmScriptVolume()
{
    return scriptVolumeAt( bgm_script_from, bgm_script_to, bgm_script_ms, bgm_script_start, SDL_GetTicks(), bgm_script_curve );
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
        music_play_loop_flag = bgm_later_loop;
        mp3save_flag = true; // once too, as mp3save: a load plays it again
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
 * and LOOPEND (or LOOPLENGTH) of an Ogg Vorbis or Opus file's comments,
 * or the first loop of a WAV's smpl chunk.
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
    bool opus = len >= 36 && !memcmp( buf + 28, "OpusHead", 8 ); // tags in 48 kHz samples, as SDL_mixer reads them
    for (unsigned long i = 0; i + 8 < len && i < 65536 && !cm; i++){
        if (!memcmp( buf + i, "\x01vorbis", 7 )) id = buf + i;
        if (!memcmp( buf + i, "\x03vorbis", 7 )) cm = buf + i;
        if (opus && !memcmp( buf + i, "OpusTags", 8 )) id = buf, cm = buf + i + 1; // (its magic is a byte longer)
    }
    if (!id || !cm) return false;
    *rate = opus ? 48000 : (int)le32(id - buf + 12);
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
static struct { volatile bool on; volatile int from, to, ms, curve; volatile Uint32 start; int last; double pos; bool ended; } track_freq[ONS_MIX_CHANNELS];
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
    int rate = scriptVolumeAt( track_freq[channel].from, track_freq[channel].to, track_freq[channel].ms, track_freq[channel].start, SDL_GetTicks(), track_freq[channel].curve );
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
        track_out[channel].ms = 0;
        track_gain[channel].last = trackGain( channel );
        track_pan[channel].last = trackPan( channel );
        track_freq[channel].on = false;
    }
    track_chans = audio_format.channels;
    track_gain_s16 = audio_format.format == AUDIO_S16SYS;
    track_stereo = audio_format.channels == 2;
    Mix_RegisterEffect( channel, cutTrackEffect, NULL, NULL ); // reset when a sound ends
}

/* chfreq ch,rate[,ms[,curve]]
 *   Plays the sound now on dwave channel ch at rate/1000 of its speed,
 *   which raises or lowers its pitch with it (100-4000; 1000 its own),
 *   reached over ms (default 0, at once; gamespeed applies) while the
 *   script goes on, from the rate it has now. A looping sound stays at it,
 *   one that doesn't ends sooner or later by as much. The next sound on the
 *   channel plays at its own rate; not kept in saves (a load starts looping
 *   sounds at their own rate). With no sound playing it does nothing.
 *       dwave 10,"se\shot.ogg":chfreq 10,1100,1000   (up a tenth in a second)
 *   curve (an spmotion curve: 1-3, 10-73 from spcurve) is how the rate goes
 *   there, straight without one. */
int ONScripter::chfreqCommand()
{
    int ch = script_h.readInt();
    int rate = script_h.readInt();
    int ms = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : 0;
    int curve = (script_h.getEndStatus() & ScriptHandler::END_COMMA) ? script_h.readInt() : 0;
    ramp_curves = curve_table;
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
    int from = track_freq[ch].on ? scriptVolumeAt( track_freq[ch].from, track_freq[ch].to, track_freq[ch].ms, track_freq[ch].start, now, track_freq[ch].curve ) : 1000;
    if (!track_freq[ch].on){
        track_freq[ch].pos = cut_track[ch].pos / (2 * audio_format.channels);
        track_freq[ch].ended = false;
        track_freq[ch].last = from;
        if (!cut_track[ch].loop){ // looped underneath, so that a slower sound isn't cut short
#if defined(USE_CONS_OPUS)
            opusChunkFinish( ch ); // playing it again ends its filling
#endif
            Mix_PlayChannel( ch, cut_track[ch].chunk, -1 ); // (the playing sound's end isn't signalled)
            Mix_RegisterEffect( ch, cutTrackEffect, NULL, NULL );
        }
        track_freq[ch].on = true;
    }
    track_freq[ch].from = from;
    track_freq[ch].start = now;
    track_freq[ch].to = rate;
    track_freq[ch].curve = curve;
    track_freq[ch].ms = ms > 0 ? speedTime( ms ) : 0;
    SDL_UnlockAudio();
    traceRamp( 2, ch, rate, track_freq[ch].ms );
    return RET_CONTINUE;
}

int trackFreq( int channel ) // per mille; the trace reads it
{
    if (channel < 0 || channel >= ONS_MIX_CHANNELS || !track_freq[channel].on) return 1000;
    return scriptVolumeAt( track_freq[channel].from, track_freq[channel].to, track_freq[channel].ms, track_freq[channel].start, SDL_GetTicks(), track_freq[channel].curve );
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
#if defined(USE_CONS_OPUS)
    else if (opusPiece( mb, music_buffer_length, s0 > 0 ? s0 : 0, (ms + ahead) / 1000.0, music_play_loop_flag, audio_format, &src, &length )){}
#endif
    else{ // shortcut: not Ogg Vorbis or Opus (WAV: quick; MP3: decoded whole)
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



/* Opus sounds: SDL_mixer plays Opus only when it was built with
 * opusfile, which none of our builds' is, so the engine decodes them
 * itself (cmake/cons_opus.cmake builds opusfile into every build) and
 * hands SDL_mixer a WAV: 48 kHz 16-bit stereo, with a LOOPSTART /
 * LOOPEND (or LOOPLENGTH) tag, in 48 kHz samples as SDL_mixer reads them
 * from Opus, as the WAV's loop (smpl). Any file name works: the data is
 * recognised by its header. The WAV is decoded as it is read
 * (opusWavRW), so a music streams from it on the mixer's thread, and a
 * sound is decoded ahead of where it plays (opusChunk): decoding either
 * whole stalled the main thread (300 ms for a 3-minute music, 30-50 ms
 * for a looping sound), and a picture moving then hitched. */
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

struct OpusWav {
    OggOpusFile *of;
    std::vector<unsigned char> head; // the WAV's header
    Sint64 frames, at;               // the WAV's data frames; where it is read (bytes)
    Sint64 first;                    // pcm holds frames [first, first+n); the decoder is at first+n
    int n;
    opus_int16 pcm[11520];           // 120 ms of 48 kHz stereo, opusfile's largest
};

static Sint64 SDLCALL opusWavSize(SDL_RWops *rw)
{
    OpusWav *w = (OpusWav *)rw->hidden.unknown.data1;
    return (Sint64)w->head.size() + w->frames * 4;
}

static Sint64 SDLCALL opusWavSeek(SDL_RWops *rw, Sint64 offset, int whence)
{
    OpusWav *w = (OpusWav *)rw->hidden.unknown.data1;
    Sint64 at = whence == RW_SEEK_SET ? offset : whence == RW_SEEK_CUR ? w->at + offset : opusWavSize(rw) + offset;
    if (at < 0) return SDL_SetError("Opus: seek before the start");
    return w->at = at;
}

static size_t SDLCALL opusWavRead(SDL_RWops *rw, void *ptr, size_t size, size_t num)
{
    OpusWav *w = (OpusWav *)rw->hidden.unknown.data1;
    unsigned char *out = (unsigned char *)ptr;
    Sint64 head = w->head.size(), end = opusWavSize(rw);
    size_t want = size * num, done = 0;
    while (done < want && w->at < end) {
        if (w->at < head) {
            size_t k = std::min(want - done, (size_t)(head - w->at));
            memcpy(out + done, w->head.data() + w->at, k);
            w->at += k;
            done += k;
            continue;
        }
        Sint64 f = (w->at - head) / 4;
        if (f < w->first || f >= w->first + w->n) {
            // Anywhere but where the decoder is (a loop, a position) is a seek
            bool ok = f == w->first + w->n || op_pcm_seek(w->of, f) == 0;
            int n = 0;
            if (ok) do n = op_read_stereo(w->of, w->pcm, sizeof(w->pcm) / sizeof(w->pcm[0])); while (n == OP_HOLE); // a gap: go on
            w->first = n > 0 ? f : -1;
            w->n = n > 0 ? n : 0;
            if (n <= 0) break;
        }
        Sint64 from = w->at - head - w->first * 4;
        size_t k = std::min(want - done, (size_t)(w->n * 4 - from));
        for (size_t i = 0; i < k; i++) { // WAV is little-endian
            opus_int16 s = w->pcm[(from + i) / 2];
            out[done + i] = (from + i) & 1 ? (s >> 8) & 0xff : s & 0xff;
        }
        w->at += k;
        done += k;
    }
    return size ? done / size : 0;
}

static size_t SDLCALL opusWavWrite(SDL_RWops *, const void *, size_t, size_t)
{
    return 0;
}

static int SDLCALL opusWavClose(SDL_RWops *rw)
{
    OpusWav *w = (OpusWav *)rw->hidden.unknown.data1;
    op_free(w->of);
    delete w;
    SDL_FreeRW(rw);
    return 0;
}

SDL_RWops *opusWavRW(const unsigned char *data, unsigned long length)
{
    if (length < 36 || memcmp(data, "OggS", 4) != 0 || memcmp(data + 28, "OpusHead", 8) != 0)
        return NULL;
    int err = 0;
    OggOpusFile *of = op_open_memory(data, length, &err);
    if (!of) return NULL;
    Sint64 frames = op_pcm_total(of, -1);
    const OpusTags *tags = op_tags(of, -1);
    Sint64 loop_start = tags ? opusTagSamples(tags, "LOOPSTART") : -1;
    Sint64 loop_end = tags ? opusTagSamples(tags, "LOOPEND") : -1;
    Sint64 loop_length = tags ? opusTagSamples(tags, "LOOPLENGTH") : -1;
    SDL_RWops *rw = frames > 0 ? SDL_AllocRW() : NULL;
    if (!rw) {
        op_free(of);
        return NULL;
    }
    if (loop_start >= 0 && loop_end < 0 && loop_length > 0) loop_end = loop_start + loop_length;
    bool loop = loop_start >= 0 && loop_end > loop_start && loop_end <= frames;

    OpusWav *w = new OpusWav;
    w->of = of;
    w->frames = frames;
    w->at = 0;
    w->first = -1;
    w->n = 0;
    std::vector<unsigned char> &head = w->head;
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

    rw->type = SDL_RWOPS_UNKNOWN;
    rw->size = opusWavSize;
    rw->seek = opusWavSeek;
    rw->read = opusWavRead;
    rw->write = opusWavWrite;
    rw->close = opusWavClose;
    rw->hidden.unknown.data1 = w;
    return rw;
}

/* An Opus sound played as a chunk: SDL_mixer keeps a chunk whole, in the
 * device's format, so it is made at its full length, silent, and filled
 * from the WAV through an audio stream (to the device's format): its first
 * OPUS_CHUNK_AHEAD_MS at once, the rest by an effect on its channel, 8
 * times as much as each mix takes, ahead of where it plays (chfreq reads
 * it at most 4 times as fast). The effect ends with its sound (done): a
 * sound stopped early keeps silence after what was filled; chfreq, which
 * plays it again, fills it first (opusChunkFinish). */
#define OPUS_CHUNK_AHEAD_MS 500
struct OpusChunk { SDL_RWops *rw; SDL_AudioStream *cvt; Mix_Chunk *chunk; Uint32 filled, frame; unsigned char *data; };
static OpusChunk *opus_chunk[ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS]; // filling, on each channel

static void opusFill(OpusChunk *z, Uint32 upto)
{
    Uint8 in[16384];
    if (upto > z->chunk->alen) upto = z->chunk->alen;
    upto -= upto % z->frame;
    while (z->filled < upto) {
        int got = SDL_AudioStreamGet(z->cvt, z->chunk->abuf + z->filled, upto - z->filled);
        if (got > 0) {
            z->filled += got;
            continue;
        }
        if (got < 0 || !z->rw) { // all there is: the rest stays silent
            z->filled = z->chunk->alen;
            break;
        }
        size_t n = SDL_RWread(z->rw, in, 1, sizeof(in));
        if (n > 0) SDL_AudioStreamPut(z->cvt, in, (int)n);
        else {
            SDL_RWclose(z->rw);
            z->rw = NULL;
            SDL_AudioStreamFlush(z->cvt);
        }
    }
}

static void opusChunkFree(OpusChunk *z)
{
    if (z->rw) SDL_RWclose(z->rw);
    SDL_FreeAudioStream(z->cvt);
    delete[] z->data;
    delete z;
}

static void SDLCALL opusChunkEffect(int, void *, int len, void *udata)
{
    OpusChunk *z = (OpusChunk *)udata;
    opusFill(z, z->filled + (Uint32)len * 8);
}

static void SDLCALL opusChunkDone(int channel, void *udata)
{
    if (channel >= 0 && channel < ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS && opus_chunk[channel] == udata)
        opus_chunk[channel] = NULL;
    opusChunkFree((OpusChunk *)udata);
}

/* The chunk of an Opus sound (NULL when data isn't one), its first part
 * filled; *filling, unless that was all of it, goes to opusChunkStart once
 * the chunk plays. data must last until then. */
Mix_Chunk *opusChunk(const unsigned char *data, unsigned long length, const SDL_AudioSpec &spec, void **filling)
{
    *filling = NULL;
    SDL_RWops *rw = opusWavRW(data, length);
    if (!rw) return NULL;
    OpusWav *w = (OpusWav *)rw->hidden.unknown.data1;
    Uint32 frame = spec.channels * (SDL_AUDIO_BITSIZE(spec.format) / 8);
    Uint64 bytes = (Uint64)w->frames * spec.freq / 48000 * frame;
    SDL_AudioStream *cvt = SDL_NewAudioStream(AUDIO_S16LSB, 2, 48000, spec.format, spec.channels, spec.freq);
    Mix_Chunk *chunk = cvt && bytes > 0 && bytes < 0x7fffffff ? (Mix_Chunk *)SDL_calloc(1, sizeof(Mix_Chunk)) : NULL;
    Uint8 *abuf = chunk ? (Uint8 *)SDL_calloc(1, (size_t)bytes) : NULL;
    if (!abuf || SDL_RWseek(rw, w->head.size(), RW_SEEK_SET) < 0) {
        SDL_free(chunk);
        if (cvt) SDL_FreeAudioStream(cvt);
        SDL_RWclose(rw);
        return NULL;
    }
    chunk->allocated = 1; // Mix_FreeChunk frees abuf
    chunk->abuf = abuf;
    chunk->alen = (Uint32)bytes;
    chunk->volume = MIX_MAX_VOLUME;
    OpusChunk *z = new OpusChunk{ rw, cvt, chunk, 0, frame, NULL };
    opusFill(z, (Uint32)((Uint64)spec.freq * OPUS_CHUNK_AHEAD_MS / 1000 * frame));
    if (z->filled < chunk->alen) *filling = z;
    else opusChunkFree(z);
    return chunk;
}

/* The chunk opusChunk made plays on channel: the rest of it is filled as
 * it plays. Takes data (new[]) over and returns true, or fills it all now
 * (no effect could be added) and returns false. */
bool opusChunkStart(int channel, void *filling, unsigned char *data)
{
    OpusChunk *z = (OpusChunk *)filling;
    if (!Mix_RegisterEffect(channel, opusChunkEffect, opusChunkDone, z)) {
        opusFill(z, z->chunk->alen);
        opusChunkFree(z);
        return false;
    }
    z->data = data;
    if (channel < ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS) opus_chunk[channel] = z;
    return true;
}

/* An Opus sound's seconds [from, from+count) in the device's format
 * (16-bit), going round when loop, in *pcm, and its length in *length (s);
 * false when it isn't Opus. */
bool opusPiece(const unsigned char *data, unsigned long length, double from, double count, bool loop,
               const SDL_AudioSpec &spec, std::vector<Sint16> *pcm, double *length_s)
{
    SDL_RWops *rw = opusWavRW(data, length);
    if (!rw) return false;
    OpusWav *w = (OpusWav *)rw->hidden.unknown.data1;
    SDL_AudioStream *cvt = SDL_NewAudioStream(AUDIO_S16LSB, 2, 48000, AUDIO_S16SYS, spec.channels, spec.freq);
    Sint64 at = (Sint64)(from * 48000), left = (Sint64)(count * 48000);
    if (loop) at %= w->frames;
    std::vector<Uint8> in;
    while (cvt && left > 0 && at < w->frames) {
        Sint64 n = std::min(left, w->frames - at);
        in.resize((size_t)n * 4);
        SDL_RWseek(rw, w->head.size() + at * 4, RW_SEEK_SET);
        size_t got = SDL_RWread(rw, in.data(), 1, in.size());
        if (got == 0) break;
        SDL_AudioStreamPut(cvt, in.data(), (int)got);
        left -= n;
        if (!loop) break;
        at = 0; // a looping music goes round
    }
    *length_s = w->frames / 48000.0;
    SDL_RWclose(rw);
    if (!cvt) return false;
    SDL_AudioStreamFlush(cvt);
    int bytes = SDL_AudioStreamAvailable(cvt);
    pcm->resize(bytes / 2);
    if (bytes > 0) SDL_AudioStreamGet(cvt, pcm->data(), bytes);
    SDL_FreeAudioStream(cvt);
    return true;
}

// The sound on channel filled to its end now (called under the audio lock).
void opusChunkFinish(int channel)
{
    if (channel >= 0 && channel < ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS && opus_chunk[channel])
        opusFill(opus_chunk[channel], opus_chunk[channel]->chunk->alen);
}
#endif // USE_CONS_OPUS
