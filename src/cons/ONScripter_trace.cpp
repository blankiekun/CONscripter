/* -*- C++ -*-
 *
 *  ONScripter_trace.cpp - CONscripter's debug trace, auto-play, starts anywhere
 *  and save/load round trips (engine/DEBUG.md)
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

// A channel's volume as heard (ONScripter_cons.cpp), 0-4096.
int trackGain( int channel );

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
void traceString( std::string &out, const char *text, int len=-1, bool json=true )
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
        else if (cons_sjis && c >= 0xa1 && c <= 0xdf)
            u = c - 0xa0 + 0xff60; // Shift_JIS half-width katakana, as drawGlyph draws it
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
    // a bad option or --replay file fails while the options are read,
    // before main sets the encoding the trace's strings are converted with
    if (!coding2utf16) coding2utf16 = new GBK2UTF16();
    trace_end_reason = "startup error";
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
        trace_ons->trace_end_reason = "script error";
        trace_ons->traceEnd(); // the engine exits next, and the web build runs no atexit handlers
    }
}

void ONScripter::traceExitHook()
{
    if (!trace_ons) return;
    if (!coding2utf16) coding2utf16 = new GBK2UTF16(); // -h with --trace before it: an exit before main sets it
    if (trace_ons->dump_vars_path) trace_ons->writeVarsFile();
    if (trace_ons->trace_fp) trace_ons->traceEnd();
}

void ONScripter::traceWrite( std::string &record )
{
    if (!trace_meta_written) traceMeta(); // a startup error comes before traceStart
#if defined(WEB)
    // the web page keeps the records, to download them
    EM_ASM({ if (self.consTrace) self.consTrace(UTF8ToString($0)); }, record.c_str());
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
    AnimationInfo::large_hook = traceLargeImage;
    if (!trace_meta_written) traceMeta();
    traceShotAtSetup();
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
              coding2utf16 && coding2utf16->force_utf8 ? "true" : "false" ); // none yet while the options are read
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
        for (size_t i=0 ; i<choose_specs.size() ; i++){
            r += i ? "," : ",\"choose\":[";
            traceString( r, coding2utf16 ? fromUTF8( choose_specs[i].c_str() ).c_str() : choose_specs[i].c_str() );
            if (i+1 == choose_specs.size()) r += ']';
        }
        if (choices_path){
            r += ",\"choices\":";
            traceString( r, choices_path );
        }
        if (known_path){
            snprintf( buf, sizeof(buf), ",\"stop_known\":%d,\"known\":", stop_known );
            r += buf;
            traceString( r, known_path );
        }
        r += '}';
    }
    if (until_label || max_pages > 0){
        r += ",\"stop\":{";
        if (until_label){
            r += "\"until\":";
            traceString( r, until_label );
        }
        if (max_pages > 0){
            snprintf( buf, sizeof(buf), "%s\"max_pages\":%d", until_label ? "," : "", max_pages );
            r += buf;
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
    SDL_Rect r = (kind[3] == '2') ? ai.bounding_rect : zoomedRect( ai ); // lsp2: its box after scaling and turning
    // an animation steps its cells by itself
    int cell = ai.is_animatable ? -1 : ai.current_cell;
    int keyed = ai.alpha_keys > 0 ? (ai.alpha_loop == 1 ? 2 : 1) : 0;
    if (ts.image == image && ts.x == r.x && ts.y == r.y && ts.w == r.w && ts.h == r.h &&
        ts.trans == ai.trans && ts.cell == cell && ts.over == ai.z_over && ts.keyed == keyed) return 0;
    ts.image = image;
    ts.x = r.x; ts.y = r.y; ts.w = r.w; ts.h = r.h;
    ts.trans = ai.trans;
    ts.cell = cell;
    ts.over = ai.z_over;
    ts.keyed = keyed;
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
    if (ai.zoom_x != 100 || ai.zoom_y != 100){
        snprintf( buf, sizeof(buf), ",\"zoom\":[%d,%d]", ai.zoom_x, ai.zoom_y );
        out += buf;
    }
    if (ai.motion_keys > 0){
        snprintf( buf, sizeof(buf), ",\"motion\":%s", ai.motion_loop ? "\"loop\"" : ai.motion_keep ? "\"keep\"" : "\"once\"" );
        out += buf;
    }
    if (ai.is_animatable) out += ",\"anim\":true";
    if (ai.z_over >= 0){
        snprintf( buf, sizeof(buf), ",\"over\":%d", ai.z_over );
        out += buf;
    }
    if (ai.fade_ms > 0 && ai.fade_alpha < 256) out += ",\"fade\":true";
    if (ai.alpha_keys > 0) out += ai.alpha_loop == 1 ? ",\"keyed\":\"loop\"" : ",\"keyed\":\"once\"";
    // the alpha it is drawn at, after spfade, spalpha and spmotion2's keys
    int factor = (ai.fade_ms > 0 ? ai.fade_alpha : 256) * ai.key_alpha / 256 * ai.motion_alpha / 256;
    if (factor != 256){
        snprintf( buf, sizeof(buf), ",\"shown\":%d", (ai.trans < 0 ? 255 : ai.trans) * factor / 256 );
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
    {"msp",1}, {"msp2",1}, {"amsp",1}, {"amsp2",1}, {"cell",1}, {"spfilter",1}, {"spzoom",1}, {"spmotion",1}, {"spmotion2",1},
    {"strsp",1}, {"strsph",1}, {"vstrsp",1}, {"spz",1}, {"spblend",1}, {"spstr",1}, {"spreload",1}, {"ld",1}, {"cl",1},
    {"bg",1}, {"bgcopy",1}, {"bgcpy",1}, {"popscreen",1},
    {"bgm",2}, {"bgmonce",2}, {"mp3",2}, {"mp3loop",2}, {"mp3save",2}, {"play",2},
    {"playonce",2}, {"playstop",2}, {"stop",2}, {"mp3stop",2}, {"bgmstop",2},
    {"mp3fadeout",2}, {"bgmfadeout",2}, {"loopbgm",2}, {"loopbgmstop",2},
    {"wave",2}, {"waveloop",2}, {"wavestop",2}, {"dwave",2}, {"dwaveloop",2},
    {"dwavestop",2}, {"dwaveplay",2}, {"dwaveplayloop",2}, {"dwavefadeout",2},
    {"mpegplay",2}, {"movie",2}, {"avi",2}, {"spfade",2}, {"spalpha",2},
    {"mp3fadein",2}, {"bgmfadein",2}, {"bgmvolfade",2}, {"bgmlater",2}, {"dwavefadein",2}, {"chvol",2},
    {"soundmute",2}, {"clickvoicestop",2}, {"autovoicewait",2}, {"selectkeep",2}, {"moviestop",2},
    {"gamespeed",2}, {"effectoff",2}, {"textshadow",2}, {"sysevol",2}, {"btnse",2},
    {"screenfilter",5}, {"screenshake",5}, {"screenripple",5}, {"screenpulse",5}, {"screenzoom",5},
    {"pushscreen",5}, {"monocro",5}, {"nega",5}, {"quake",5}, {"quakex",5}, {"quakey",5},
    {"allsphide",5}, {"allspresume",5}, {"allsp2hide",5}, {"allsp2resume",5}, {"windowtrans",5}, {"screenflash",5},
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

// A movie's "skip": whether a click stops it, with "moviestop" when that
// overrides the script's flag.
void ONScripter::traceMovie( bool script_click )
{
    if (!trace_fp) return;
    bool click = movie_stop >= 0 ? movie_stop == 1 : script_click;
    trace_sound += click ? ",\"skip\":true" : ",\"skip\":false";
    if (movie_stop >= 0 && click != script_click)
        trace_sound += ",\"moviestop\":" + std::to_string( movie_stop );
    // The movie covers the whole screen, the text window too
    if (display_mode & DISPLAY_MODE_TEXT) trace_sound += ",\"window\":true";
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
        std::string blocked = traceBlocked( bl );
        if (!blocked.empty()) r += ",\"blocked\":\"" + blocked + "\"";
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

// Why a btnwait's button can't be seen: "off" the screen, its sprite
// "hidden" or drawn at "alpha 0", or "under lsp n", an opaque sprite in
// front covering it all (a click still reaches it). Empty when it can.
std::string ONScripter::traceBlocked( ButtonLink *bl )
{
    SDL_Rect &r = bl->select_rect;
    if (r.w <= 0 || r.h <= 0 || r.x >= screen_width || r.y >= screen_height || r.x + r.w <= 0 || r.y + r.h <= 0)
        return "off";
    if (bl->button_type != ButtonLink::SPRITE_BUTTON && bl->button_type != ButtonLink::TMP_SPRITE_BUTTON) return "";
    int no = bl->sprite_no;
    if (no < 0 || no >= MAX_SPRITE_NUM) return "";
    AnimationInfo &ai = sprite_info[no];
    if (!ai.visible || all_sprite_hide_flag) return "hidden";
    int factor = (ai.fade_ms > 0 ? ai.fade_alpha : 256) * ai.key_alpha / 256 * ai.motion_alpha / 256;
    if (ai.trans == 0 || (ai.fade_ms == 0 && ai.alpha_keys == 0 && factor == 0)) return "alpha 0";
    // shortcut: lsp in front only, without spz; lsp2 and ld aren't looked at
    if (ai.z_over >= 0) return "";
    for (int i=0 ; i<no ; i++){
        AnimationInfo &f = sprite_info[i];
        if (!f.visible || !f.image_surface || f.z_over >= 0 || f.trans_mode != AnimationInfo::TRANS_COPY ||
            (f.trans >= 0 && f.trans < 255) || f.fade_ms > 0 || f.key_alpha != 256 || f.motion_alpha != 256) continue;
        SDL_Rect c = zoomedRect( f );
        if (c.x <= r.x && c.y <= r.y && c.x + c.w >= r.x + r.w && c.y + c.h >= r.y + r.h)
            return "under lsp " + std::to_string( i );
    }
    return "";
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
    if (dump){
        r += ",\"why\":\"";
        r += trace_dump_why;
        r += '"';
    }
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

    if (audio_open_flag && (music_file_name || midi_file_name)){
        // the music as heard: the setting, soundmute, a fade, bgmvolfade
        int vol = (Mix_PlayingMusic() ? Mix_VolumeMusic( -1 ) : Mix_Volume( MIX_BGM_CHANNEL, -1 )) * 100 / MIX_MAX_VOLUME;
        if (vol < 100){
            snprintf( buf, sizeof(buf), ",\"bgmvol\":%d", vol );
            r += buf;
        }
    }
    if (trace_wait_ms > 0){
        snprintf( buf, sizeof(buf), ",\"wait\":%d", trace_wait_ms );
        r += buf;
    }
    if (!dump) trace_wait_ms = 0;

    bool se = false;
    for (int ch=0 ; audio_open_flag && ch<=MIX_WAVE_CHANNEL ; ch++){
        if (!trace_se_name[ch] || !Mix_Playing( ch )) continue;
        r += se ? "," : ",\"se\":[";
        se = true;
        snprintf( buf, sizeof(buf), "{\"ch\":%d,\"file\":", ch );
        r += buf;
        traceString( r, trace_se_name[ch] );
        if (trace_se_loop[ch]) r += ",\"loop\":true";
        int vol = Mix_Volume( ch, -1 ) * 100 / MIX_MAX_VOLUME * trackGain( ch ) / 4096; // as heard: soundmute, chvolfade
        if (vol < 100){
            snprintf( buf, sizeof(buf), ",\"vol\":%d", vol );
            r += buf;
        }
        r += '}';
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
    if (screen_flash_opacity){
        snprintf( buf, sizeof(buf), ",\"flash\":[%d,\"#%02x%02x%02x\"]", screen_flash_opacity,
                  screen_flash_colour[0], screen_flash_colour[1], screen_flash_colour[2] );
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
        static const char *names[] = { "", "shake", "ripple", "pulse", "zoom" };
        r += listed ? "," : ",\"fx\":[";
        listed = true;
        snprintf( buf, sizeof(buf), "{\"slot\":%d,\"kind\":\"%s\"", i+1, names[e.kind] );
        r += buf;
        if (e.kind == SCREEN_EFFECT_SHAKE && e.param[9] > 0){
            snprintf( buf, sizeof(buf), ",\"end\":%d", e.param[9] );
            r += buf;
        }
        if (e.map_name){
            r += ",\"map\":";
            traceString( r, e.map_name );
        }
        r += '}';
    }
    if (listed) r += ']';

    if (trace_overflow) r += ",\"overflow\":true";
    // the most picture surfaces alive at once since the last page
    snprintf( buf, sizeof(buf), ",\"mem\":{\"surf\":%d,\"bytes\":%lld}",
              AnimationInfo::peak_surfaces, AnimationInfo::peak_bytes );
    r += buf;
    if (!dump){
        AnimationInfo::peak_surfaces = AnimationInfo::live_surfaces;
        AnimationInfo::peak_bytes = AnimationInfo::live_bytes;
    }
    if (dump) traceVars( r );
    if (all_sprite_hide_flag) r += ",\"sprites_hidden\":true";
    if (all_sprite2_hide_flag) r += ",\"sprites2_hidden\":true";
    if (accumulation_surface && accumulation_surface->format->BytesPerPixel == 4){
        // FNV-1a of the screen as shown, without its alpha, for --diff
        SDL_Surface *s = accumulation_surface;
        Uint32 mask = s->format->Rmask | s->format->Gmask | s->format->Bmask, h = 2166136261u;
        SDL_LockSurface( s );
        for (int y=0 ; y<s->h ; y++){
            Uint32 *p = (Uint32*)((Uint8*)s->pixels + y * s->pitch);
            for (int x=0 ; x<s->w ; x++) h = (h ^ (p[x] & mask)) * 16777619u;
        }
        SDL_UnlockSurface( s );
        snprintf( buf, sizeof(buf), ",\"px\":\"%08x\"", h );
        r += buf;
    }
    traceShot( r, dump );
    r += ",\"sp\":[";
    for (int i=0 ; i<TRACE_SPRITES ; i++){
        if (trace_ignore[i]) continue;
        traceSpriteUpdate( i, since ); // a change no command recorded
        if (!trace_sprite[i].image.empty()) traceSprite( r, i );
    }
    r += "]}";
    if (roundtrip_pending && !dump){ // the page shown again after a round trip: compared, not recorded
        roundtripPage();
        if (roundtrip_skip_page){
            trace_page--;
            return;
        }
    }
    traceWrite( r );
    if (!dump && roundtrip_every > 0 && auto_play_flag && trace_page % roundtrip_every == 0){
        roundtripState( roundtrip_before );
        roundtrip_due = true;
    }
}

// "label:line" or "*label:line" as a choice record's "at": with the "*",
// in lower case, as the script's labels are kept.
static std::string choicePlace( const char *text, size_t len )
{
    while (len > 0 && (*text == ' ' || *text == '\t')){ text++; len--; }
    while (len > 0 && (text[len-1] == ' ' || text[len-1] == '\t')) len--;
    std::string at( text, len );
    for (size_t i=0 ; i<at.size() ; i++) if (at[i] >= 'A' && at[i] <= 'Z') at[i] += 'a' - 'A';
    if (!at.empty() && at[0] != '*') at = "*" + at;
    return at;
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

// "*s4_l2:5467,*conv_wait:120": the lines --shot-at takes a picture after.
void ONScripter::setTraceShotAt( const char *places )
{
    for (const char *p = places ; p && *p ; ){
        const char *comma = strchr( p, ',' );
        size_t len = comma ? (size_t)(comma - p) : strlen( p );
        ShotAt s;
        s.at = choicePlace( p, len );
        size_t colon = s.at.rfind( ':' );
        s.line = colon == std::string::npos ? 0 : atoi( s.at.c_str() + colon + 1 );
        if (s.line < 1){
            utils::printError( "--shot-at: give '*label:line', not [%s]\n", s.at.c_str() );
            startupFailed();
        }
        s.begin = s.end = NULL;
        s.visits = 0;
        s.pending = false;
        trace_shot_at.push_back( s );
        p = comma ? comma+1 : NULL;
    }
}

// "5000,12000": the times (ms since the engine started) --shot-after
// takes a picture at.
void ONScripter::setTraceShotAfter( const char *times )
{
    for (const char *p = times ; p && *p ; ){
        char *end;
        long ms = strtol( p, &end, 10 );
        if (end == p || ms < 0){
            utils::printError( "--shot-after: can't read [%s]\n", p );
            startupFailed();
        }
        trace_shot_after.push_back( (unsigned int)ms );
        p = *end == ',' ? end+1 : NULL;
    }
    std::sort( trace_shot_after.begin(), trace_shot_after.end() );
}

// Wakes the event loop when a --shot-after time comes, so a game waiting
// for a click (a menu, a btnwait) is shot then.
static Uint32 shotTimerCallback( Uint32 interval, void *param )
{
    SDL_Event event;
    SDL_zero( event );
    event.type = ONS_SHOT_EVENT;
    SDL_PushEvent( &event );
    return 0;
}

// Finds the --shot-at lines in the script, and starts the --shot-after
// timers.
void ONScripter::traceShotAtSetup()
{
    int lines = 0; // the script's last line, counted from 1
    for (int i=0 ; i<script_h.getScriptLength() ; i++)
        if (*script_h.getAddress( i ) == 0x0a) lines++;
    for (size_t i=0 ; i<trace_shot_at.size() ; ){
        ShotAt &s = trace_shot_at[i];
        char *addr = s.line <= lines ? script_h.getAddressByLine( s.line - 1 ) : NULL;
        if (!addr || traceAtText( addr ) != s.at){
            utils::printError( "--shot-at: no line %s in the script%s%s\n", s.at.c_str(),
                               addr ? "; that line is " : "", addr ? traceAtText( addr ).c_str() : "" );
            trace_shot_at.erase( trace_shot_at.begin() + i );
            continue;
        }
        s.begin = s.end = addr;
        while (script_h.inScript( s.end + 1 ) && *s.end != 0x0a) s.end++;
        i++;
    }
    Uint32 now = SDL_GetTicks();
    for (size_t i=0 ; i<trace_shot_after.size() ; i++)
        SDL_AddTimer( trace_shot_after[i] > now ? trace_shot_after[i] - now : 1, shotTimerCallback, NULL );
}

// Called before each line or command is run (executeLabel): once the
// script has left a --shot-at line, every command on it having run, a
// picture and a dump record, at<pg>-<label>-<line>-<visit>.png.
void ONScripter::traceShotAt( char *line_start )
{
    for (size_t i=0 ; i<trace_shot_at.size() ; i++){
        ShotAt &s = trace_shot_at[i];
        bool inside = line_start >= s.begin && line_start <= s.end;
        if (s.pending && !inside){
            s.pending = false;
            std::string label;
            for (size_t c=1 ; c<s.at.size() && s.at[c] != ':' ; c++){
                char ch = s.at[c];
                label += (isalnum( (unsigned char)ch ) || ch == '_') ? ch : '_';
            }
            char name[64];
            snprintf( name, sizeof(name), "-%d-%d.png", s.line, ++s.visits );
            char pg[16];
            snprintf( pg, sizeof(pg), "at%06d-", trace_page );
            traceShotDump( "shot-at", pg + label + name );
        }
        if (inside) s.pending = true;
    }
}

// At a --shot-after time, or as soon after as the engine gets to it: a
// picture and a dump record, ms<time>.png. Not in the define section,
// before there is a screen to take.
void ONScripter::traceShotAfter()
{
    if (current_mode == DEFINE_MODE) return;
    while (trace_shot_after_next < trace_shot_after.size() &&
           SDL_GetTicks() >= trace_shot_after[trace_shot_after_next]){
        char name[32];
        snprintf( name, sizeof(name), "ms%07u.png", trace_shot_after[trace_shot_after_next++] );
        traceShotDump( "shot-after", name );
    }
}

// A dump record, as F8 writes, with its "why" and its picture's name.
void ONScripter::traceShotDump( const char *why, const std::string &name )
{
    if (!trace_sprite || current_mode == DEFINE_MODE) return;
    trace_dump_why = why;
    trace_shot_name = name;
    tracePage( true );
    trace_dump_why = "F8";
    trace_shot_name.clear();
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
    char name[300];
    if (dump && !trace_shot_name.empty()) snprintf( name, sizeof(name), "/%s", trace_shot_name.c_str() );
    else if (dump) snprintf( name, sizeof(name), "/dump%06d-%u.png", trace_page, SDL_GetTicks() );
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
// playing there, and the web page's self.consVoice(channel, file) is
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
        if (channel == 0 && current_page){
            setStr( &current_page->voice, file.c_str() ); // for consBacklog
            setStr( &current_page->voice_file, name );    // for getlogrow
        }
#if defined(WEB)
        EM_ASM({ if (self.consVoice) self.consVoice($0, UTF8ToString($1)); }, channel, file.c_str());
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
// error, with why it ended; a trace without it ended in a crash.
void ONScripter::traceEnd()
{
    char buf[64];
    snprintf( buf, sizeof(buf), "{\"t\":\"end\",\"pg\":%d,\"reason\":", trace_page );
    std::string r = buf;
    traceString( r, trace_end_reason ? trace_end_reason : "exit" ); // "exit": none of the others (-h)
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

// --taken file: lines "*label:line<TAB>option<TAB>count", the choices
// --auto-choice untaken avoids (option as in the choice record: picked or
// result) and the times earlier runs made them. A line without a count
// counts 1; lines for the same choice add up. "avoid" for the count marks
// a choice never to make while the place has another (one that led to a
// script error).
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
        auto_taken_places.insert( buf );
        char *count = strchr( tab+1, '\t' );
        char key[1100];
        snprintf( key, sizeof(key), "%s\t%d", buf, atoi( tab+1 ) );
        if (count && !strcmp( count+1, "avoid" )) auto_avoid.insert( key );
        else auto_taken[key] += count ? atoi( count+1 ) : 1;
    }
    fclose( fp );
}

// --choose '*label:line=N': --auto-play takes option N (as in the choice
// record: picked or result) at every choice there; "=N,M,K" gives the
// answers for the visits in turn, staying on the last. It comes before
// --choices, --replay and --auto-choice.
void ONScripter::setChoose( const char *spec )
{
    const char *eq = strrchr( spec, '=' );
    std::string at = eq ? choicePlace( spec, eq - spec ) : "";
    std::vector<int> answers;
    for (const char *p = eq ? eq+1 : NULL ; p && *p ; ){
        char *end;
        int n = strtol( p, &end, 10 );
        if (end == p || n < 0){ answers.clear(); break; }
        answers.push_back( n );
        p = *end == ',' ? end+1 : NULL;
    }
    if (at.size() < 4 || at.find( ':' ) == std::string::npos || answers.empty()){
        utils::printError( "--choose: give '*label:line=N' (or =N,M,...), not [%s]\n", spec );
        startupFailed();
    }
    choose_answers[at] = answers;
    choose_specs.push_back( spec );
}

// --choices file: lines "*label:line<TAB>option" (or "=option"), "#"
// comments and blank lines left out; a second tab and what follows (a
// --taken file's count) is left out too. --auto-play makes them in order:
// each is used up when a choice comes up at its place; a choice somewhere
// else is left to --replay and --auto-choice, without using one up.
void ONScripter::setChoices( const char *path )
{
    setStr( &choices_path, path );
    FILE *fp = ::fopen( path, "rb" );
    if (!fp){
        utils::printError( "--choices: can't open %s\n", path );
        startupFailed();
    }
    char buf[1024];
    int line = 0;
    while (fgets( buf, sizeof(buf), fp )){
        line++;
        size_t len = strlen( buf );
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' ')) buf[--len] = 0;
        const char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        const char *sep = strchr( p, '\t' );
        if (!sep) sep = strrchr( p, '=' );
        char *end = NULL;
        AutoAnswer a;
        if (sep){
            a.at = choicePlace( p, sep - p );
            a.value = strtol( sep+1, &end, 10 );
        }
        if (!sep || end == sep+1 || a.value < 0 || a.at.find( ':' ) == std::string::npos){
            utils::printError( "--choices: %s line %d: give \"*label:line<TAB>option\", not [%s]\n", path, line, p );
            startupFailed();
        }
        choices_list.push_back( a );
    }
    fclose( fp );
}

// Whether an answer --choose or --choices gives is one of the options;
// if not, a "choice not offered" warning.
bool ONScripter::autoPlayOffered( const std::vector<int> &nos, int value, const char *from )
{
    if (std::find( nos.begin(), nos.end(), value ) != nos.end()) return true;
    std::string extra = ",\"option\":" + std::to_string( value ) + ",\"from\":\"" + from + "\",\"options\":[";
    for (size_t i=0 ; i<nos.size() ; i++){
        if (i) extra += ',';
        extra += std::to_string( nos[i] );
    }
    extra += ']';
    autoPlayWarn( "choice not offered", extra.c_str() );
    return false;
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
// counts as made in this run for "untaken".
void ONScripter::autoPlayChosen( int value )
{
    if (replay_next < replay_answers.size()) replay_next++;
    char buf[16];
    snprintf( buf, sizeof(buf), "\t%d", value );
    std::string at = traceAtText( trace_cmd_start );
    std::string key = at + buf;
    auto_taken_run.insert( key );
    // a choice no run had made, at a place they reached (not one --taken
    // leaves out, such as a title menu): new ground may follow
    if (auto_taken_places.count( at ) && !auto_taken.count( key )){
        auto_play_known = 0;
        auto_play_diverged = true;
    }
}

/* --known-labels file: the labels earlier runs reached, one name a line
 * (without "*"; "#" lines left out). With --stop-known n, a run that has
 * gone where no earlier run went (a choice not in --taken, or a label not
 * in the file) stops (exit 0, end reason "known ground") once it has
 * played n pages in a row in labels the file lists, without such a choice, so an exploration's runs don't
 * play through what earlier runs already showed. Before it has gone its
 * own way it may play 10n pages on known ground: the opening every run
 * shares comes first, but a run that never leaves it is a repeat. */
void ONScripter::setKnownLabels( const char *path )
{
    setStr( &known_path, path );
    FILE *fp = ::fopen( path, "rb" );
    if (!fp){
        utils::printError( "--known-labels: can't open %s\n", path );
        startupFailed();
    }
    char buf[1024];
    while (fgets( buf, sizeof(buf), fp )){
        size_t len = strlen( buf );
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' ')) buf[--len] = 0;
        char *name = buf[0] == '*' ? buf+1 : buf;
        if (name[0] && name[0] != '#') auto_known_names.insert( name );
    }
    fclose( fp );
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
        if (count > 1){
            // the time skipped still counts toward a shake's end
            for (int i=0 ; i<MAX_SCREEN_EFFECT ; i++){
                ScreenEffect &e = screen_effect[i];
                if (e.kind != SCREEN_EFFECT_SHAKE || e.param[9] <= 0) continue;
                e.ran += count - 1;
                if (e.ran >= e.param[9]){
                    clearScreenEffect( e );
                    dirty_rect.fill( screen_width, screen_height );
                }
            }
            count = 1;
        }
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
// round, "random" takes any, "untaken" one not made yet in this run that
// --taken counts least, leaving out those it says to avoid.
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

    // --choose for this place, then the next --choices line if it is for
    // this place (used up even when --choose answers), then --replay; an
    // answer that isn't offered goes on to the next
    if (!choose_answers.empty() || !choices_list.empty()){
        std::string at = traceAtText( trace_cmd_start );
        int listed = -1;
        if (choices_next < choices_list.size()){
            if (choices_list[choices_next].at == at) listed = choices_list[choices_next++].value;
        }
        else if (!choices_list.empty() && !choices_ended){
            choices_ended = true;
            autoPlayWarn( "choices ended" );
        }
        auto ch = choose_answers.find( at );
        if (ch != choose_answers.end()){
            size_t visit = choose_visits[at]++;
            int value = ch->second[ std::min( visit, ch->second.size() - 1 ) ];
            if (autoPlayOffered( nos, value, "choose" )) return value;
        }
        if (listed >= 0 && autoPlayOffered( nos, listed, "choices" )) return listed;
    }

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
        // leave out the options to avoid, unless all are; of those not
        // made yet in this run, the one --taken counts least, the first of
        // those; when all were made, as "first"
        std::string at = traceAtText( trace_cmd_start ) + "\t";
        std::vector<int> ok;
        for (size_t i=0 ; i<nos.size() ; i++)
            if (!auto_avoid.count( at + std::to_string( nos[i] ) )) ok.push_back( nos[i] );
        if (ok.empty()) ok = nos;
        int best = -1, fewest = 0;
        for (size_t i=0 ; i<ok.size() ; i++){
            std::string key = at + std::to_string( ok[i] );
            if (auto_taken_run.count( key )) continue;
            auto it = auto_taken.find( key );
            int n = it == auto_taken.end() ? 0 : it->second;
            if (best < 0 || n < fewest){ best = ok[i]; fewest = n; }
        }
        if (best >= 0) return best;
        size_t visit = auto_choice_visits[ script_h.getCurrent() ]++;
        return ok[ std::min( visit, ok.size() - 1 ) ];
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
// routes, the same choice taken at each turn). Also called without
// --auto-play for --max-pages.
void ONScripter::autoPlayPage()
{
    if (roundtrip_skip_page){ // the page shown again after a round trip
        roundtrip_skip_page = false;
        return;
    }
    if (max_pages > 0 && ++run_pages >= max_pages) stopRun( "page limit" );
    if (!auto_play_flag) return;
    auto_play_waits = 0;
    auto_play_choices = 0;
    int label = script_h.getLabelIndex( script_h.getCurrent( true ) );
    if (auto_label_seen.empty()) auto_label_seen.assign( script_h.getNumLabels(), false );
    if (label >= 0 && !auto_label_seen[label]){
        auto_label_seen[label] = true;
        auto_play_same = 0;
    }
    else if (++auto_play_same >= 3000) autoPlayStuck( "3000 pages without a new label" );
    if (stop_known > 0){
        if (auto_known.size() != (size_t)script_h.getNumLabels()){
            auto_known.assign( script_h.getNumLabels(), false );
            for (int i=0 ; i<script_h.getNumLabels() ; i++)
                auto_known[i] = auto_known_names.count( script_h.getLabelName( i ) ) > 0;
        }
        if (label >= 0 && !auto_known[label]){ // every page of a label no earlier run reached
            auto_play_known = 0;
            auto_play_diverged = true;
        }
        else if (++auto_play_known >= (auto_play_diverged ? stop_known : 10 * stop_known)){
            utils::printInfo( "auto-play: stopped after %d pages on known ground\n", auto_play_known );
            trace_end_reason = "known ground";
            if (trace_fp) traceEnd();
            exit(0);
        }
    }
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

/* --until label and --max-pages n: a run, played by hand or by itself,
 * ends when the script first reaches that label, or after n text pages
 * (counted as the trace counts them), with exit code 0. */
void ONScripter::setUntil( const char *label )
{
    if (label[0] == '*') label++;
    setStr( &until_label, label );
    for (char *p = until_label ; *p ; p++) // labels are kept in lower case
        if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
}

// Called once the script is read: the label --until waits for.
void ONScripter::untilSetup()
{
    for (int i=0 ; i<script_h.getNumLabels() && until_index < 0 ; i++)
        if (!strcmp( script_h.getLabelName( i ), until_label )) until_index = i;
    if (until_index < 0){
        utils::printError( "--until: no label *%s\n", until_label );
        startupFailed();
    }
}

void ONScripter::stopRun( const char *reason )
{
    utils::printInfo( "stopped: %s\n", reason );
    trace_end_reason = reason;
    if (trace_fp) traceEnd();
    exit(0);
}

/* SIGTERM, SIGINT and SIGHUP end the run as closing the window does, but
 * the end record says which signal it was and the exit code is 128+n.
 * SDL's own handlers, which only send a quit, are turned off; this one
 * notes the signal, and a timer sends the quit, since SDL_PushEvent isn't
 * safe in a signal handler. Another signal two seconds or more later kills
 * at once (a hung run); one sooner is the same signal sent twice, as
 * timeout sends it to the engine and to its process group. */
static volatile sig_atomic_t quit_signal = 0;
static volatile time_t quit_signal_time = 0;

static void quitSignal( int sig )
{
    if (quit_signal){
        if (time( NULL ) - quit_signal_time < 2) return;
        signal( sig, SIG_DFL );
        raise( sig );
        return;
    }
    quit_signal_time = time( NULL );
    quit_signal = sig;
}

static Uint32 quitSignalTimer( Uint32 interval, void *param )
{
    if (quit_signal && SDL_PeepEvents( NULL, 0, SDL_PEEKEVENT, SDL_QUIT, SDL_QUIT ) == 0){
        SDL_Event event;
        SDL_zero( event );
        event.type = SDL_QUIT;
        SDL_PushEvent( &event ); // again if something else (a movie) takes it
    }
    return interval;
}

// Called before SDL_Init.
void ONScripter::signalSetup()
{
#if !defined(WEB)
    SDL_SetHint( SDL_HINT_NO_SIGNAL_HANDLERS, "1" );
    signal( SIGTERM, quitSignal );
    signal( SIGINT, quitSignal );
#if defined(SIGHUP)
    signal( SIGHUP, quitSignal );
#endif
    if (SDL_InitSubSystem( SDL_INIT_TIMER ) == 0) SDL_AddTimer( 100, quitSignalTimer, NULL );
#endif
}

// Why the run ends, as the engine quits (endCommand): a signal, or the
// reason given before (the window closed, the player quit), or else the
// script ended. Returns the exit code.
int ONScripter::quitReason()
{
    if (quit_signal){
        static char reason[32];
        int sig = quit_signal;
        snprintf( reason, sizeof(reason), "signal %s", sig == SIGTERM ? "SIGTERM" : sig == SIGINT ? "SIGINT" : "SIGHUP" );
        trace_end_reason = reason;
        return 128 + sig;
    }
    if (!trace_end_reason) trace_end_reason = "script end";
    return 0;
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

// --trace-max-image mb: a warn when a single picture takes more memory
// than this (64 MB by default, a 4096x4096 picture), once for each file.
// Pictures are decoded and drawn in software in every build, the web one
// too, so their size is limited by memory only.
void ONScripter::setTraceMaxImage( int mb )
{
    AnimationInfo::large_bytes = (long long)mb << 20;
}

void ONScripter::traceLargeImage( AnimationInfo *ai, int w, int h )
{
    ONScripter *ons = trace_ons;
    if (!ons || !ons->trace_fp) return;
    const char *file = ai->file_name ? ai->file_name : ai->image_name;
    if (!ons->trace_large_seen.insert( file ? file : "" ).second) return;
    char buf[96];
    snprintf( buf, sizeof(buf), "{\"t\":\"warn\",\"pg\":%d,\"what\":\"large image\",\"file\":", ons->trace_page );
    std::string r = buf;
    traceString( r, file );
    long long bytes = ai->image_surface ? (long long)ai->image_surface->pitch * ai->image_surface->h : (long long)w * h * 4;
    snprintf( buf, sizeof(buf), ",\"w\":%d,\"h\":%d,\"mb\":%lld,\"at\":", w, h, (bytes + (1 << 19)) >> 20 );
    r += buf;
    ons->traceAt( r, ons->script_h.getCurrent( true ) );
    r += '}';
    ons->traceWrite( r );
}

/* --roundtrip-every n (with --trace and --auto-play): every n pages, the
 * game is saved to --roundtrip-slot (999) and loaded again, as a player
 * saving and loading at that click wait would. The load goes back to where
 * the save was made (the start of the page's line) and the game's
 * loadgosub runs; when the same page is shown again, its state is compared
 * with the one before, and a difference is a "save/load mismatch" warn.
 * The page shown again isn't recorded, so the run goes on as without it. */
void ONScripter::setRoundtrip( int pages, int slot )
{
    if (pages >= 0) roundtrip_every = pages;
    if (slot >= 0) roundtrip_slot = slot;
}

// What a page shows, field by field, for comparing: what the page record
// has (without the counts and times), the sprites without "since", the
// looping sounds, the pushscreen stack and the variables.
void ONScripter::roundtripState( std::map<std::string, std::string> &state )
{
    state.clear();
    char buf[160];
    traceAt( state["at"], script_h.getCurrent( true ) );
    traceString( state["text"], current_page->text, current_page->text_count );
    if (current_page->tag) traceString( state["tag"], current_page->tag );
    traceString( state["bg"], bg_info.file_name );
    traceString( state["bgm"], music_file_name ? music_file_name : midi_file_name );
    if (loop_bgm_name[0]){
        traceString( state["loopbgm"], loop_bgm_name[0] );
        traceString( state["loopbgm"], loop_bgm_name[1] );
        // it plays until stopped: the intro with the loop ready to follow,
        // or the loop, so anything else is a sound the load lost
        bool playing = Mix_Playing( MIX_LOOPBGM_CHANNEL1 ) ||
            (Mix_Playing( MIX_LOOPBGM_CHANNEL0 ) && (!loop_bgm_name[1] || wave_sample[MIX_LOOPBGM_CHANNEL1]));
        if (audio_open_flag && !playing) state["loopbgm"] += " (stopped)";
    }
    for (int ch=0 ; audio_open_flag && ch<=MIX_WAVE_CHANNEL ; ch++){
        if (!trace_se_name[ch] || !trace_se_loop[ch] || !Mix_Playing( ch )) continue; // a voice isn't saved
        snprintf( buf, sizeof(buf), "se %d", ch );
        traceString( state[buf], trace_se_name[ch] );
        int vol = Mix_Volume( ch, -1 ) * 100 / MIX_MAX_VOLUME * trackGain( ch ) / 4096;
        if (vol < 100) state[buf] += " vol " + std::to_string( vol );
    }
    state["window"] = (display_mode & DISPLAY_MODE_TEXT) ? "true" : "false";
    if (window_trans != 255) state["windowtrans"] = std::to_string( window_trans );
    if (monocro_flag){
        snprintf( buf, sizeof(buf), "#%02x%02x%02x", monocro_color[0], monocro_color[1], monocro_color[2] );
        state["monocro"] = buf;
    }
    if (nega_mode) state["nega"] = std::to_string( nega_mode );
    if (screen_flash_opacity){
        snprintf( buf, sizeof(buf), "%d #%02x%02x%02x", screen_flash_opacity,
                  screen_flash_colour[0], screen_flash_colour[1], screen_flash_colour[2] );
        state["flash"] = buf;
    }
    for (int i=0 ; i<MAX_SCREEN_FILTER ; i++){
        AnimationInfo::Filter &f = screen_filter[i];
        if (f.kind == AnimationInfo::FILTER_NONE) continue;
        snprintf( buf, sizeof(buf), "kind %d blend %d strength %d p %d,%d,%d", f.kind, f.blend, f.strength, f.param[0], f.param[1], f.param[2] );
        state["filter " + std::to_string( i+1 )] = buf;
    }
    for (int i=0 ; i<MAX_SCREEN_EFFECT ; i++){
        ScreenEffect &e = screen_effect[i];
        if (e.kind == SCREEN_EFFECT_NONE) continue;
        std::string &v = state["fx " + std::to_string( i+1 )];
        v = std::to_string( e.kind );
        if (e.map_name) traceString( v, e.map_name );
    }
    if (all_sprite_hide_flag) state["sprites_hidden"] = "true";
    if (all_sprite2_hide_flag) state["sprites2_hidden"] = "true";
    if (screen_stack_num) state["pushscreen"] = std::to_string( screen_stack_num );
    for (int i=0 ; i<TRACE_SPRITES ; i++){
        if (trace_ignore[i]) continue;
        const char *kind;
        int no;
        AnimationInfo &ai = traceSpriteInfo( i, kind, no );
        const char *image = ai.image_name;
        if (!image && ai.trans_mode == AnimationInfo::TRANS_STRING) image = ai.file_name;
        if (!ai.visible || !image) continue;
        SDL_Rect r = (kind[3] == '2') ? ai.bounding_rect : zoomedRect( ai );
        snprintf( buf, sizeof(buf), "sp %s %d", kind, no );
        std::string &v = state[buf];
        traceString( v, image );
        snprintf( buf, sizeof(buf), " %d,%d %dx%d alpha %d cell %d filters %d", r.x, r.y, r.w, r.h,
                  ai.trans < 0 ? 255 : ai.trans, ai.current_cell, ai.num_of_filters );
        v += buf;
        if (ai.z_over >= 0) v += " over " + std::to_string( ai.z_over );
        if (ai.alpha_keys > 0) v += ai.alpha_loop == 1 ? " keyed loop" : " keyed once"; // its alpha moves with the time
        else if (ai.key_alpha != 256) v += " keyed " + std::to_string( ai.key_alpha );
    }
    for (int i=0 ; i<script_h.variable_range ; i++){
        ScriptHandler::VariableData &vd = script_h.getVariableData( i );
        if (vd.num) state["%" + std::to_string( i )] = std::to_string( vd.num );
        if (vd.str && vd.str[0]) traceString( state["$" + std::to_string( i )], vd.str );
    }
}

// Saves and loads, as savegame and loadgame would (the save holds the
// state at the start of the page's line).
void ONScripter::roundtripLoad()
{
    roundtrip_due = false;
    if (saveon_flag && internal_saveon_flag) storeSaveFile();
    char buf[96];
    snprintf( buf, sizeof(buf), "{\"t\":\"roundtrip\",\"pg\":%d,\"slot\":%d,\"at\":", trace_page, roundtrip_slot );
    std::string r = buf;
    traceAt( r, script_h.getCurrent( true ) );
    r += '}';
    traceWrite( r );
    bool saved = writeSaveFile( roundtrip_slot ) == 0;
    if (saved) roundtripClear(); // what the save leaves out is then missing after the load
    if (!saved || !loadGame( roundtrip_slot )){
        r = "{\"t\":\"warn\",\"pg\":" + std::to_string( trace_page ) + ",\"what\":\"save/load mismatch\",\"diff\":[\"can't save or load\"]}";
        traceWrite( r );
        return;
    }
    roundtrip_pending = true;
}

// Clears the state roundtripState compares, as a load from the title
// screen would find it, so a field the save leaves out shows as changed
// instead of carrying over. Settings made once in *define (cursors,
// kinsoku, the window) are kept, as a real load keeps them.
void ONScripter::roundtripClear()
{
    for (int i=0 ; i<script_h.global_variable_border ; i++)
        script_h.getVariableData( i ).reset( false );
    monocro_flag = false;
    nega_mode = 0;
    window_trans = 255;
    all_sprite_hide_flag = all_sprite2_hide_flag = false;
    clearScreenStack();
    clearScreenFilters();
    clearScreenEffects();
    screen_flash_opacity = 0;
    stopCommand();
    loopbgmstopCommand();
    stopAllDWAVE();
    bg_info.reset();
    setStr( &bg_info.file_name, "black" );
    createBackground();
    for (int i=0 ; i<3 ; i++) tachi_info[i].reset();
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++) sprite_info[i].reset();
    for (int i=0 ; i<MAX_SPRITE2_NUM ; i++) sprite2_info[i].reset();
}

// A page after the load: the page saved, shown again, is compared and not
// recorded (an earlier page on the same line, shown again on the way, isn't
// recorded either); a page elsewhere means the load went somewhere else.
// Up to 100 bytes of a value from byte from, with "..." where cut.
std::string ONScripter::roundtripExcerpt( const std::string &s, size_t from )
{
    if (s.size() <= 100) return s;
    while (from > 0 && (s[from] & 0xc0) == 0x80) from--;
    size_t to = from + 100;
    if (to >= s.size()) to = s.size();
    else while (to > from && (s[to] & 0xc0) == 0x80) to--;
    return (from ? "..." : "") + s.substr( from, to - from ) + (to < s.size() ? "..." : "");
}

void ONScripter::roundtripPage()
{
    std::map<std::string, std::string> after;
    roundtripState( after );
    std::map<std::string, std::string> &before = roundtrip_before;
    bool same_place = after["at"] == before["at"];
    // the page's text so far, at its line or at an earlier click wait in
    // the same label (a page over several lines): on the way back to it
    const std::string &at_a = after["at"], &at_b = before["at"];
    bool same_label = at_a.compare( 0, at_a.rfind( ':' ), at_b, 0, at_b.rfind( ':' ) ) == 0;
    const std::string &text_a = after["text"], &text_b = before["text"];
    if ((same_place || (same_label && text_a.size() > 2)) && text_a != text_b &&
        text_b.compare( 0, text_a.size() - 1, text_a, 0, text_a.size() - 1 ) == 0){
        roundtrip_skip_page = true;
        return;
    }
    roundtrip_pending = false;
    roundtrip_skip_page = same_place;

    std::vector<std::string> diff;
    std::map<std::string, std::string>::iterator i = before.begin(), j = after.begin();
    while (i != before.end() || j != after.end()){
        std::string key, a = "none", b = "none";
        if (j == after.end() || (i != before.end() && i->first < j->first)){
            key = i->first; a = i->second; i++;
        }
        else if (i == before.end() || j->first < i->first){
            key = j->first; b = j->second; j++;
        }
        else{
            key = i->first; a = i->second; b = j->second; i++; j++;
        }
        if (a == b) continue;
        // a page continued past a click wait: the save holds its line only,
        // so the load shows that line's text alone (as every load does)
        if (key == "text" && same_place && a.size() > b.size() + 2 &&
            a.compare( a.size() - b.size() + 1, b.size() - 1, b, 1, b.size() - 1 ) == 0 &&
            a.compare( a.size() - b.size() - 1, 2, "\\n" ) == 0) continue;
        // long values: 100 bytes from a little before where they differ,
        // cut between UTF-8 characters
        size_t from = 0;
        while (from < a.size() && from < b.size() && a[from] == b[from]) from++;
        from = (from > 40) ? from - 40 : 0;
        diff.push_back( key + ": " + roundtripExcerpt( a, from ) + " -> " + roundtripExcerpt( b, from ) );
    }
    if (diff.empty()) return;

    char buf[64];
    snprintf( buf, sizeof(buf), "{\"t\":\"warn\",\"pg\":%d,\"what\":\"save/load mismatch\",\"at\":", trace_page );
    std::string r = buf;
    r += before["at"];
    r += ",\"diff\":[";
    for (size_t k=0 ; k<diff.size() && k<40 ; k++){
        if (k) r += ',';
        // the values are already JSON strings in UTF-8; escape them as text
        r += '"';
        for (size_t c=0 ; c<diff[k].size() ; c++){
            if (diff[k][c] == '"' || diff[k][c] == '\\') r += '\\';
            r += diff[k][c];
        }
        r += '"';
    }
    r += "]}";
    traceWrite( r );
}

// spbtn num,no,pressed_cell: the button under the pointer shows its pressed
// cell while the left mouse button is held, and its hover cell again when
// it is let go.
