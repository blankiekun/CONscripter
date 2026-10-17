/* -*- C++ -*-
 *
 *  ONScripter_screen.cpp - CONscripter: screens the engine draws (save and
 *  load menus first), described by Lua tables in the archive
 *  (screens/<name>.lua) and opened with `screen "name"`. See "Screens" in
 *  COMMANDS.md for the table format.
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
#include <sys/stat.h>
#include <time.h>
#include <limits.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <math.h>
#if defined(WEB)
#include <emscripten.h>
#endif

void traceString( std::string &out, const char *text, int len=-1, bool json=true );
std::string fromUTF8( const char *text );
extern Coding2UTF16 *coding2utf16;

#ifdef USE_LUA
#include <lua.hpp>

// spnew's layers are 0-2 (ONScripter_names.cpp); screens draw over them all
static const int LAYER_SCREEN = 3;
// a slot's button is its widget's number (index in `widgets` times 100) plus the slot's place on the page
static const int WIDGET_BUTTONS = 100;
// a gallery's cells are its buttons 1-90, its viewer's previous, next, hide and back 91-94
static const int GALLERY_CELLS = 90, VIEWER_BUTTONS = 91;
// a slider's buttons: its widget's number plus 1 (minus), 2 (plus), 3 (a picture slider's knob)
static const int KNOB = 3;

struct ConsScreen {
    std::string name, kind, arg;
    lua_State *L = NULL;
    int table = LUA_NOREF;
    char *at = NULL;            // the screen command: where a gosub returns
    int page = 1, selected = 0; // the save slots' page, the slot selected (0 none)
    std::vector<int> sprites;   // handles drawn
    std::vector<int> hidden;    // sprites hidden while open
    bool window = false;        // the text window was hidden
    int filter_slot = 0;
    bool saveon = true;         // internal_saveon_flag at open
    SDL_Surface *shot = NULL;   // the screen as it was at open: the thumbnail
    int pending = LUA_NOREF, pending_next = 0; // the actions left after a gosub
    int scroll = 0;             // a list's first entry (log: pages back from the one on screen)
    std::vector<std::string> voices; // a list's voice per button (its place on screen)
    int view_w = 0, view_cell = 0, view_variant = 0; // a gallery's viewer: widget, cell (1-based), variant shown
    bool view_hidden = false;   // its panel hidden (a click shows it)
    std::map<int, int> knobs;   // a picture slider's knob sprite, by widget number
    std::map<int, int> fills, texts; // a slider's fill picture and text sprites, by widget number
    int drag = 0;               // the knob button being dragged (0 none)
    int grab_x = 0, grab_y = 0; // where it was taken from the knob's centre (0 from the track)
    std::vector<int> owners;    // the widget each of `sprites` belongs to (0 none)
    int cur_w = 0;              // the widget being drawn
    bool shown = false;         // drawn once: its open motions and fade done
    int fade_next = 0;          // a cross-fade (ms) for the next draw: a widget's state changed
};

enum { SCREEN_STAY, SCREEN_CLOSE, SCREEN_GOSUB, SCREEN_LOADED };

/* Lua helpers: fields of the table on top of the stack */
static int fieldInt( lua_State *L, const char *key, int def )
{
    lua_getfield( L, -1, key );
    int v = lua_isnumber( L, -1 ) ? (int)lua_tointeger( L, -1 ) : lua_isboolean( L, -1 ) ? lua_toboolean( L, -1 ) : def;
    lua_pop( L, 1 );
    return v;
}

static std::string fieldStr( lua_State *L, const char *key, const char *def="" )
{
    lua_getfield( L, -1, key );
    std::string v = lua_isstring( L, -1 ) ? lua_tostring( L, -1 ) : def;
    lua_pop( L, 1 );
    return v;
}

static int indexInt( lua_State *L, int i, int def )
{
    lua_rawgeti( L, -1, i );
    int v = lua_isnumber( L, -1 ) ? (int)lua_tointeger( L, -1 ) : def;
    lua_pop( L, 1 );
    return v;
}

static std::string indexStr( lua_State *L, int i )
{
    lua_rawgeti( L, -1, i );
    std::string v = lua_isstring( L, -1 ) ? lua_tostring( L, -1 ) : "";
    lua_pop( L, 1 );
    return v;
}

// pushes t[key] when it is a table and returns true; else pushes nothing
static bool fieldTable( lua_State *L, const char *key )
{
    lua_getfield( L, -1, key );
    if (lua_istable( L, -1 )) return true;
    lua_pop( L, 1 );
    return false;
}

static std::string quoted( const std::string &s )
{
    std::string r = "\"";
    for (char c : s) if (c != '"' && c != '\n') r += c;
    return r + "\"";
}

static std::string fileName( std::string f )
{
    for (char &c : f) if (c == '/') c = '\\';
    return f;
}

// "%12", "%name", "$3", "?name[4]", "name" (an integer): the variable through the script parser
bool ONScripter::screenVariable( const char *ref, bool str, ScriptHandler::VariableInfo &vi )
{
    std::string s = fromUTF8( ref );
    if (s.empty()) return false;
    if (s[0] != '%' && s[0] != '$' && s[0] != '?') s = (str ? "$" : "%") + s;
    std::vector<char> buf( s.begin(), s.end() );
    buf.push_back( 0 );
    script_h.enterExternalScript( buf.data() );
    script_h.readVariable();
    vi = script_h.current_variable;
    script_h.leaveExternalScript();
    return vi.type == ScriptHandler::VAR_INT || vi.type == ScriptHandler::VAR_STR || vi.type == ScriptHandler::VAR_ARRAY;
}

static int luaVar( lua_State *L )
{
    return ((ONScripter*)lua_touserdata( L, lua_upvalueindex(1) ))->screenLuaVar( L );
}

// var(n | "name"), str(n | "name"): a variable's value
int ONScripter::screenLuaVar( lua_State *L )
{
    bool str = lua_toboolean( L, lua_upvalueindex(2) );
    std::string ref = lua_isnumber( L, 1 ) ? std::string(str ? "$" : "%") + std::to_string( lua_tointeger( L, 1 ) ) : luaL_checkstring( L, 1 );
    ScriptHandler::VariableInfo vi;
    if (!screenVariable( ref.c_str(), str, vi )) return luaL_error( L, "no variable %s", ref.c_str() );
    if (vi.type == ScriptHandler::VAR_STR){
        const char *v = script_h.getVariableData( vi.var_no ).str;
        std::string utf8;
        traceString( utf8, v ? v : "", -1, false ); // in UTF-8, as the table is
        lua_pushstring( L, utf8.c_str() );
    }
    else lua_pushinteger( L, script_h.getIntVariable( &vi ) );
    return 1;
}

// One NScripter command, run as the script would (the trace sees it too).
void ONScripter::screenExec( const std::string &cmd )
{
    char *at = trace_cmd_start;
    std::vector<char> buf( cmd.begin(), cmd.end() );
    buf.push_back( 0 );
    script_h.enterExternalScript( buf.data() );
    runScript();
    script_h.leaveExternalScript();
    trace_cmd_start = at;
}

void ONScripter::screenTrace( ConsScreen *s, const char *action, int slot, int value )
{
    if (!trace_fp) return;
    char buf[96];
    snprintf( buf, sizeof(buf), "{\"t\":\"screen\",\"pg\":%d,\"at\":", trace_page );
    std::string r = buf;
    traceAt( r, s->at );
    r += ",\"screen\":";
    traceString( r, fromUTF8( s->name.c_str() ).c_str() );
    r += ",\"kind\":";
    traceString( r, fromUTF8( s->kind.c_str() ).c_str() );
    snprintf( buf, sizeof(buf), ",\"action\":\"%s\",\"page\":%d", action, s->page );
    r += buf;
    if (slot > 0){ snprintf( buf, sizeof(buf), ",\"slot\":%d", slot ); r += buf; }
    if (value != INT_MIN){ snprintf( buf, sizeof(buf), ",\"value\":%d", value ); r += buf; }
    r += "}";
    traceWrite( r );
}

static std::string savePath( const char *save_dir, const char *archive_path, int no )
{
    char name[32];
    snprintf( name, sizeof(name), "save%d.dat", no );
    return std::string( save_dir ? save_dir : archive_path ) + name;
}

static std::string thumbName( int no )
{
    return "savethumb" + std::to_string( no ) + ".bmp";
}

// The save's time (0: no save in the slot).
time_t ONScripter::screenSaveTime( int no )
{
    struct stat st;
    if (stat( savePath( save_dir, archive_path, no ).c_str(), &st ) != 0) return 0;
    return st.st_mtime ? st.st_mtime : 1;
}

/* A slot's text line: %N the slot in two digits, %n as is, %Y %y %m %d %H
 * %M %S the save's time, %T its text, %T1 to %T9 its parts cut at `split`
 * (%T-1 to %T-9 from the last),
 * %% a %. In the script's encoding. */
static std::string slotText( const std::string &tmpl, int no, time_t t, const char *text, const std::string &split )
{
    std::string in = fromUTF8( tmpl.c_str() ), out;
    struct tm *tm = t ? localtime( &t ) : NULL;
    char buf[16];
    for (size_t i=0 ; i<in.size() ; i++){
        if (in[i] != '%' || i+1 >= in.size()){ out += in[i]; continue; }
        char c = in[++i];
        buf[0] = 0;
        if (c == 'N') snprintf( buf, sizeof(buf), "%02d", no );
        else if (c == 'n') snprintf( buf, sizeof(buf), "%d", no );
        else if (c == 'T' && i+1 < in.size() && ((in[i+1] >= '1' && in[i+1] <= '9') ||
                 (in[i+1] == '-' && i+2 < in.size() && in[i+2] >= '1' && in[i+2] <= '9'))){
            bool back = in[i+1] == '-';
            i += back ? 2 : 1;
            std::vector<std::string> parts;
            std::string all = text ? text : "";
            size_t from = 0;
            for (size_t j=0 ; j<all.size() ; ){ // split only at a character's start (a Shift-JIS trail byte can be '|')
                if (!split.empty() && all.compare( j, split.size(), split ) == 0){
                    parts.push_back( all.substr( from, j - from ) );
                    from = j += split.size();
                }
                else j += (IS_TWO_BYTE( (unsigned char)all[j] ) && !coding2utf16->force_utf8 && j+1 < all.size()) ? 2 : 1;
            }
            parts.push_back( all.substr( from ) );
            int k = in[i] - '1';
            if (k < (int)parts.size()) out += parts[back ? parts.size() - 1 - k : k];
        }
        else if (c == 'T'){ if (text) out += text; }
        else if (c == '%') out += '%';
        else if (tm && strchr( "YymdHMS", c )){
            char f[3] = { '%', c, 0 };
            strftime( buf, sizeof(buf), f, tm );
        }
        out += buf;
    }
    return out;
}

/* Drawing */

int ONScripter::screenSprite( ConsScreen *s )
{
    // from the top down: a screen opened over another gets lower numbers, drawn on top
    for (int i=SPRITE_HANDLE_NUM-1 ; i>=0 ; i--)
        if (sprite_handle_layer[i] == 0 && !sprite_info[NS_SPRITE_NUM + i].image_name){
            sprite_handle_layer[i] = LAYER_SCREEN + 1;
            sprite_handle_var[i] = -1;
            s->sprites.push_back( NS_SPRITE_NUM + i );
            s->owners.push_back( s->cur_w );
            return NS_SPRITE_NUM + i;
        }
    errorAndExit( "screen: no sprite handle left" );
    return -1;
}

// An image (the table on top of the stack: file, x, y, cells, alpha, blend, scale, opaque) at x, y.
int ONScripter::screenImage( ConsScreen *s, int x, int y, const char *file_override )
{
    lua_State *L = s->L;
    std::string file = file_override ? file_override : fileName( fromUTF8( fieldStr( L, "file" ).c_str() ) );
    int cells = fieldInt( L, "cells", 1 );
    std::string tag = fieldInt( L, "opaque", 0 ) ? ":c" : ":a";
    if (cells > 1) tag += "/" + std::to_string( cells ) + ",0,3";
    int no = screenSprite( s );
    screenExec( "lsp " + std::to_string( no ) + "," + quoted( tag + ";" + file ) + "," + std::to_string( x ) + "," +
                std::to_string( y ) + "," + std::to_string( fieldInt( L, "alpha", 255 ) ) );
    std::string blend = fieldStr( L, "blend" );
    int mode = blend == "add" ? 1 : blend == "multiply" ? 2 : blend == "sub" ? 3 : blend == "screen" ? 4 : 0;
    if (mode) screenExec( "spblend " + std::to_string( no ) + "," + std::to_string( mode ) );
    int scale = fieldInt( L, "scale", 100 );
    if (scale != 100) screenExec( "spzoom " + std::to_string( no ) + "," + std::to_string( scale ) );
    return no;
}

// A text (the table on top: size, cols, rows, bold, shadow, color, align)
// at x, y; `colors` in place of `color`: one cell each (normal, hover).
int ONScripter::screenText( ConsScreen *s, const std::string &text, int x, int y, const std::vector<std::string> *colors, int no )
{
    lua_State *L = s->L;
    int size = fieldInt( L, "size", 20 );
    if (fieldStr( L, "align" ) == "centre" || fieldStr( L, "align" ) == "center")
        x -= (int)text.size() * size / 4; // half-width characters are half as wide, as the script measures
    std::string color = fieldStr( L, "color" ), color_args = color.empty() ? "" : "," + quoted( color );
    if (colors){
        color_args.clear();
        for (const std::string &c : *colors) color_args += "," + quoted( c );
    }
    if (no < 0) no = screenSprite( s );
    // a text with line breaks can't be in the command: strsp takes it from
    // screen_strsp_text (the command's own text, the breaks as spaces, shows in the trace)
    bool lines = text.find( 0x0a ) != std::string::npos;
    std::string shown = text;
    for (char &c : shown) if (c == 0x0a) c = ' ';
    if (lines) screen_strsp_text = text.c_str();
    screenExec( "strsp " + std::to_string( no ) + "," + quoted( shown ) + "," +
                std::to_string( x ) + "," + std::to_string( y ) + "," +
                std::to_string( fieldInt( L, "cols", 40 ) ) + "," + std::to_string( fieldInt( L, "rows", 1 ) ) + "," +
                std::to_string( size ) + "," + std::to_string( size ) + ",0," + std::to_string( fieldInt( L, "pitch", 0 ) ) + "," +
                std::to_string( fieldInt( L, "bold", 0 ) ) + "," + std::to_string( fieldInt( L, "shadow", 0 ) ) + color_args );
    screen_strsp_text = NULL;
    return no;
}

// A button drawn as text (the table on top: text, colors `color`, `hover`,
// `disabled_color`), clickable as button number `no` unless `off`.
void ONScripter::screenTextButton( ConsScreen *s, const std::string &text, int x, int y, int no, bool off )
{
    lua_State *L = s->L;
    std::vector<std::string> colors = { fieldStr( L, "color", "#FFFFFF" ) };
    if (off) colors[0] = fieldStr( L, "disabled_color", colors[0].c_str() );
    else colors.push_back( fieldStr( L, "hover", colors[0].c_str() ) );
    int sp = screenText( s, text, x, y, &colors );
    if (!off) screenExec( "spbtn " + std::to_string( sp ) + "," + std::to_string( no ) );
}

/* Bound values: a variable ("%12", "%name") */
int ONScripter::screenGet( const std::string &ref )
{
    ScriptHandler::VariableInfo vi;
    return screenVariable( ref.c_str(), false, vi ) && vi.type != ScriptHandler::VAR_STR ? script_h.getIntVariable( &vi ) : 0;
}

void ONScripter::screenSet( const std::string &ref, int value )
{
    ScriptHandler::VariableInfo vi;
    if (screenVariable( ref.c_str(), false, vi ) && vi.type != ScriptHandler::VAR_STR) script_h.setInt( &vi, value );
}

// The widget's (on top) `var`: a name, or a list of them set together (the first read).
static std::vector<std::string> widgetVars( lua_State *L )
{
    std::vector<std::string> vars;
    lua_getfield( L, -1, "var" );
    if (lua_istable( L, -1 )){
        int n = (int)lua_rawlen( L, -1 );
        for (int i=1 ; i<=n ; i++) vars.push_back( indexStr( L, i ) );
    }
    else if (lua_isstring( L, -1 )) vars.push_back( lua_tostring( L, -1 ) );
    lua_pop( L, 1 );
    return vars;
}

/* A slider's or toggle's text (UTF-8): %v the value, %V in full-width
 * digits, %B a bar (`marks`' first character per step up to the value, the
 * second after it), %C the mark for on or off, %% a %. */
static std::string boundText( const std::string &tmpl, int v, int min, int max, int step, const std::string &on, const std::string &off, bool state )
{
    static const char *wide[] = { "０", "１", "２", "３", "４", "５", "６", "７", "８", "９" };
    std::string out;
    for (size_t i=0 ; i<tmpl.size() ; i++){
        if (tmpl[i] != '%' || i+1 >= tmpl.size()){ out += tmpl[i]; continue; }
        char c = tmpl[++i];
        if (c == 'v') out += std::to_string( v );
        else if (c == 'V'){
            std::string d = std::to_string( v );
            for (char ch : d) out += (ch == '-') ? "－" : wide[ch - '0'];
        }
        else if (c == 'B') for (int k=min+step ; k<=max ; k+=step) out += k <= v ? on : off;
        else if (c == 'C') out += state ? on : off;
        else if (c == '%') out += '%';
        else{ out += '%'; out += c; }
    }
    return out;
}

// The marks of a slider's bar or a toggle: `marks = {"■", "□"}`.
static void widgetMarks( lua_State *L, std::string &on, std::string &off )
{
    on = "■"; off = "□";
    if (fieldTable( L, "marks" )){ on = indexStr( L, 1 ); off = indexStr( L, 2 ); lua_pop( L, 1 ); }
}

// The text of the slider on top at value v (UTF-8).
static std::string sliderText( lua_State *L, int v )
{
    std::string on, off;
    widgetMarks( L, on, off );
    return boundText( fieldStr( L, "text" ), v, fieldInt( L, "min", 0 ), fieldInt( L, "max", 10 ), fieldInt( L, "step", 1 ), on, off, v != 0 );
}

void ONScripter::screenClear( ConsScreen *s )
{
    for (int no : s->sprites){
        screenExec( "csp " + std::to_string( no ) );
        sprite_handle_layer[no - NS_SPRITE_NUM] = 0;
    }
    s->sprites.clear();
    s->owners.clear();
    s->knobs.clear();
    s->fills.clear();
    s->texts.clear();
    deleteButtonLink();
}

// The slot an action acts on: its own number, else the slot selected.
static int actionSlot( lua_State *L, int selected )
{
    return indexInt( L, 2, selected );
}

// The save slots' number of pages (1 without them).
static int slotPages( lua_State *L, int table )
{
    int pages = 1;
    lua_rawgeti( L, LUA_REGISTRYINDEX, table );
    int screen_pages = fieldInt( L, "pages", 0 ); // the screen's own pages (settings tabs)
    if (screen_pages > 0){ lua_pop( L, 1 ); return screen_pages; }
    lua_getfield( L, -1, "widgets" );
    int n = lua_istable( L, -1 ) ? (int)lua_rawlen( L, -1 ) : 0;
    for (int i=1 ; i<=n ; i++){
        lua_rawgeti( L, -1, i );
        bool slots = indexStr( L, 1 ) == "saveslots";
        if (slots) pages = fieldInt( L, "pages", 1 );
        lua_pop( L, 1 );
        if (slots) break;
    }
    lua_pop( L, 2 );
    return pages;
}

// The page a page action goes to ("+1", "-1": from the page shown), 0 if none.
static int pageTarget( const std::string &to, int page, int pages )
{
    int p = (to[0] == '+' || to[0] == '-') ? page + atoi( to.c_str() ) : atoi( to.c_str() );
    return p >= 1 && p <= pages ? p : 0;
}

// Whether the action (or list of them) on top of the stack can be taken
// now: its first save, load, delete or page can.
bool ONScripter::screenEnabled( ConsScreen *s )
{
    lua_State *L = s->L;
    lua_rawgeti( L, -1, 1 );
    bool list = lua_istable( L, -1 );
    lua_pop( L, 1 );
    int n = list ? (int)lua_rawlen( L, -1 ) : 1;
    for (int i=1 ; i<=n ; i++){
        if (list) lua_rawgeti( L, -1, i );
        else lua_pushvalue( L, -1 );
        std::string what = indexStr( L, 1 ), to = indexStr( L, 2 );
        int slot = actionSlot( L, s->selected );
        lua_pop( L, 1 );
        if (what == "save") return slot > 0;
        if (what == "load" || what == "delete") return slot > 0 && screenSaveTime( slot );
        if (what == "page"){
            int p = pageTarget( to, s->page, slotPages( L, s->table ) );
            return p > 0 && p != s->page;
        }
        if (what == "scroll") return screenScrollable( s, atoi( to.c_str() ) );
    }
    return true;
}

// Text log page k back from the one on screen (0); NULL past the oldest or when empty.
ScriptParser::Page *ONScripter::screenLogPage( int k )
{
    Page *page = current_page;
    while (page && page != start_page && k > 0){
        k--;
        page = page->previous;
    }
    return (k > 0 || !page || page->text_count == 0) ? NULL : page;
}

// Whether a list can scroll by n ("+1" older, "-1" newer).
bool ONScripter::screenScrollable( ConsScreen *s, int n )
{
    int to = s->scroll + n;
    return to >= 0 && to != s->scroll && (n < 0 || screenLogPage( to ) != NULL);
}

/* list (on top), widget number w, source "log": the text log's pages from
 * s->scroll back, the newest at the bottom of the box x, y, h, each under
 * its speaker's name (the page's tag, "name|voice"); with a voice the
 * name is a button playing it. Stops at the first page that doesn't fit
 * (the first is shown anyway). */
void ONScripter::screenList( ConsScreen *s, int w )
{
    lua_State *L = s->L;
    s->voices.clear();
    if (fieldStr( L, "source", "log" ) != "log") return;
    int ax = fieldInt( L, "x", 0 ), ay = fieldInt( L, "y", 0 ), ah = fieldInt( L, "h", 0 );
    int size = fieldInt( L, "size", 20 ), lh = fieldInt( L, "line_height", size );
    int gap = lh - size;
    int lift = sentence_font.rubyon_flag ? gap : 0; // with rubyon strsp draws the lines a gap lower
    std::string name_color = fieldStr( L, "name_color", "#FFFFFF" ), hover = fieldStr( L, "hover", "#FFFFFF" );
    std::string mark = fromUTF8( fieldStr( L, "voice_mark", "　♪" ).c_str() );
    lua_pushinteger( L, gap );
    lua_setfield( L, -2, "pitch" );
    int y = ay + ah;
    for (int k=0 ; k<WIDGET_BUTTONS-1 ; k++){
        Page *p = screenLogPage( s->scroll + k );
        if (!p) break;
        std::string text( p->text, p->text_count ), name, voice;
        int lines = 1;
        for (char c : text) if (c == 0x0a) lines++;
        if (p->tag){
            std::string tag = p->tag;
            size_t bar = tag.find( '|' );
            name = tag.substr( 0, bar );
            if (bar != std::string::npos) voice = tag.substr( bar + 1 );
        }
        int named = (!name.empty() || !voice.empty()) ? 1 : 0;
        int top = y - (lines + named) * lh - lh / 2;
        if (top < ay && k > 0) break;
        if (top < ay) top = ay;
        lua_pushinteger( L, lines );
        lua_setfield( L, -2, "rows" );
        screenText( s, text, ax, top + named * lh - lift );
        lua_pushinteger( L, 1 );
        lua_setfield( L, -2, "rows" );
        if (named){
            std::vector<std::string> colors = { name_color };
            if (!voice.empty()) colors.push_back( hover );
            int sp = screenText( s, voice.empty() ? name : name + mark, ax, top - lift, &colors );
            if (!voice.empty()) screenExec( "spbtn " + std::to_string( sp ) + "," + std::to_string( w * WIDGET_BUTTONS + k + 1 ) );
        }
        s->voices.push_back( voice );
        y = top;
    }
}

// A gallery cell's (on top) CGs: their files and whether each was seen.
static void cellCgs( lua_State *L, std::vector<std::string> &files, std::vector<bool> &seen )
{
    files.clear(); seen.clear();
    if (!fieldTable( L, "cgs" )) return;
    int n = (int)lua_rawlen( L, -1 );
    for (int i=1 ; i<=n ; i++){
        lua_rawgeti( L, -1, i );
        files.push_back( fieldStr( L, "file" ) );
        seen.push_back( fieldInt( L, "seen", 0 ) != 0 );
        lua_pop( L, 1 );
    }
    lua_pop( L, 1 );
}

/* gallery (on top), widget number w: `cells` in a grid of `cols` at x, y
 * by dx, dy. A cell is open when one of its `cgs` was seen: its `thumb`
 * (two cells) at the place plus `thumb_dx`, `thumb_dy`, a button opening
 * the viewer; else `locked` at the place. */
void ONScripter::screenGallery( ConsScreen *s, int w )
{
    lua_State *L = s->L;
    int x0 = fieldInt( L, "x", 0 ), y0 = fieldInt( L, "y", 0 ), dx = fieldInt( L, "dx", 0 ), dy = fieldInt( L, "dy", 0 );
    int cols = std::max( 1, fieldInt( L, "cols", 1 ) ), tdx = fieldInt( L, "thumb_dx", 0 ), tdy = fieldInt( L, "thumb_dy", 0 );
    std::string locked = fieldStr( L, "locked" );
    if (!fieldTable( L, "cells" )) return;
    int n = std::min( (int)lua_rawlen( L, -1 ), GALLERY_CELLS );
    for (int k=1 ; k<=n ; k++){
        lua_rawgeti( L, -1, k );
        int x = x0 + ((k-1) % cols) * dx, y = y0 + ((k-1) / cols) * dy;
        std::vector<std::string> files;
        std::vector<bool> seen;
        cellCgs( L, files, seen );
        bool open = std::find( seen.begin(), seen.end(), true ) != seen.end();
        std::string thumb = fieldStr( L, "thumb" );
        int cells = fieldInt( L, "cells", 2 );
        lua_pop( L, 1 );
        lua_newtable( L ); // the picture's fields
        lua_pushinteger( L, open ? cells : 1 );
        lua_setfield( L, -2, "cells" );
        if (open){
            int no = screenImage( s, x + tdx, y + tdy, fileName( fromUTF8( thumb.c_str() ) ).c_str() );
            screenExec( "spbtn " + std::to_string( no ) + "," + std::to_string( w * WIDGET_BUTTONS + k ) );
        }
        else if (!locked.empty()) screenImage( s, x, y, fileName( fromUTF8( locked.c_str() ) ).c_str() );
        lua_pop( L, 1 );
    }
    lua_pop( L, 1 );
}

// The viewer's variant from v, stepping by step past those not seen (-1: none).
static int seenVariant( const std::vector<bool> &seen, int v, int step )
{
    int n = (int)seen.size();
    for (int i=0 ; i<n ; i++){
        int k = ((v + i * step) % n + n) % n;
        if (seen[k]) return k;
    }
    return -1;
}

/* The viewer of gallery (on top): the cell's CG over the whole screen, and
 * unless hidden its `viewer` panel: pictures, `name`, then `previous`,
 * `next`, `hide`, `back` buttons. */
void ONScripter::screenViewer( ConsScreen *s )
{
    lua_State *L = s->L;
    std::vector<std::string> files;
    std::vector<bool> seen;
    if (fieldTable( L, "cells" )){
        lua_rawgeti( L, -1, s->view_cell );
        if (lua_istable( L, -1 )) cellCgs( L, files, seen );
        lua_pop( L, 2 );
    }
    if (s->view_variant < 0 || s->view_variant >= (int)files.size()) return;
    lua_newtable( L );
    lua_pushboolean( L, 1 );
    lua_setfield( L, -2, "opaque" );
    screenImage( s, 0, 0, fileName( fromUTF8( files[s->view_variant].c_str() ) ).c_str() );
    lua_pop( L, 1 );
    if (s->view_hidden || !fieldTable( L, "viewer" )) return;
    if (fieldTable( L, "panel" )){
        int n = (int)lua_rawlen( L, -1 );
        for (int i=1 ; i<=n ; i++){
            lua_rawgeti( L, -1, i );
            screenImage( s, fieldInt( L, "x", 0 ), fieldInt( L, "y", 0 ) );
            lua_pop( L, 1 );
        }
        lua_pop( L, 1 );
    }
    if (fieldTable( L, "name" )){
        screenImage( s, fieldInt( L, "x", 0 ), fieldInt( L, "y", 0 ) );
        lua_pop( L, 1 );
    }
    const char *keys[] = { "previous", "next", "hide", "back" };
    for (int b=0 ; b<4 ; b++){
        if (!fieldTable( L, keys[b] )) continue;
        int no = screenImage( s, fieldInt( L, "x", 0 ), fieldInt( L, "y", 0 ) );
        screenExec( "spbtn " + std::to_string( no ) + "," + std::to_string( s->view_w * WIDGET_BUTTONS + VIEWER_BUTTONS + b ) );
        lua_pop( L, 1 );
    }
    lua_pop( L, 1 );
}

/* list (on top), widget number w, source "items": `items` in columns of
 * `rows` at x, y by dx (a column) and dy (a row), those with `shown =
 * false` left out (the rest close up): `selector` (two cells: nothing,
 * under the pointer) a button running the item's `action`, `new_mark`
 * when the item's `new` is true, its `text` (fields as `text`, the item's
 * `color` in place of the list's). Positions in the row are relative. */
void ONScripter::screenItems( ConsScreen *s, int w )
{
    lua_State *L = s->L;
    int x0 = fieldInt( L, "x", 0 ), y0 = fieldInt( L, "y", 0 ), dx = fieldInt( L, "dx", 0 ), dy = fieldInt( L, "dy", 0 );
    int rows = std::max( 1, fieldInt( L, "rows", 1 ) ), max = rows * std::max( 1, fieldInt( L, "cols", 1 ) );
    int tx = fieldInt( L, "text_x", 0 ), ty = fieldInt( L, "text_y", 0 );
    std::string color = fieldStr( L, "color", "#FFFFFF" );
    if (!fieldTable( L, "items" )) return;
    int n = std::min( (int)lua_rawlen( L, -1 ), WIDGET_BUTTONS - 1 ), shown = 0;
    for (int k=1 ; k<=n && shown<max ; k++){
        lua_rawgeti( L, -1, k );
        if (!fieldInt( L, "shown", 1 )){ lua_pop( L, 1 ); continue; }
        int x = x0 + (shown / rows) * dx, y = y0 + (shown % rows) * dy;
        bool is_new = fieldInt( L, "new", 0 ) != 0;
        std::vector<std::string> colors = { fieldStr( L, "color", color.c_str() ) };
        std::string text = fromUTF8( fieldStr( L, "text" ).c_str() );
        lua_pop( L, 1 );
        lua_pushvalue( L, -2 ); // the list's own fields for its parts
        if (fieldTable( L, "selector" )){
            int no = screenImage( s, x + fieldInt( L, "x", 0 ), y + fieldInt( L, "y", 0 ) );
            screenExec( "spbtn " + std::to_string( no ) + "," + std::to_string( w * WIDGET_BUTTONS + k ) );
            lua_pop( L, 1 );
        }
        if (is_new && fieldTable( L, "new_mark" )){
            screenImage( s, x + fieldInt( L, "x", 0 ), y + fieldInt( L, "y", 0 ) );
            lua_pop( L, 1 );
        }
        screenText( s, text, x + tx, y + ty, &colors );
        lua_pop( L, 1 );
        shown++;
    }
    lua_pop( L, 1 );
}

void ONScripter::screenDraw( ConsScreen *s )
{
    lua_State *L = s->L;
    screenClear( s );
    lua_rawgeti( L, LUA_REGISTRYINDEX, s->table );
    lua_getfield( L, -1, "widgets" );
    int n = lua_istable( L, -1 ) ? (int)lua_rawlen( L, -1 ) : 0;
    if (s->view_w > 0){ // a gallery's viewer covers the screen
        lua_rawgeti( L, -1, s->view_w );
        s->cur_w = s->view_w;
        screenViewer( s );
        s->cur_w = 0;
        lua_pop( L, 3 );
        return;
    }
    for (int i=1 ; i<=n ; i++){
        lua_rawgeti( L, -1, i );
        s->cur_w = i;
        std::string type = indexStr( L, 1 );
        int x = fieldInt( L, "x", 0 ), y = fieldInt( L, "y", 0 );
        int page = fieldInt( L, "page", 0 );
        if (page > 0 && page != s->page){ lua_pop( L, 1 ); continue; }
        if (fieldTable( L, "pages" )){ // shown on pages a to b
            bool in = s->page >= indexInt( L, 1, 1 ) && s->page <= indexInt( L, 2, 1 );
            lua_pop( L, 1 );
            if (!in){ lua_pop( L, 1 ); continue; }
        }

        if (type == "image") screenImage( s, x, y );
        else if (type == "text") screenText( s, fromUTF8( fieldStr( L, "text" ).c_str() ), x, y );
        else if (type == "button" && fieldStr( L, "file" ).empty()){
            bool on = fieldInt( L, "enabled", 1 ) != 0;
            if (on && fieldTable( L, "action" )){
                on = screenEnabled( s );
                lua_pop( L, 1 );
            }
            screenTextButton( s, fromUTF8( fieldStr( L, "text" ).c_str() ), x, y, i * WIDGET_BUTTONS, !on );
        }
        else if (type == "slider"){
            std::vector<std::string> vars = widgetVars( L );
            int v = vars.empty() ? 0 : screenGet( vars[0] );
            std::string text = sliderText( L, v );
            if (!text.empty()) s->texts[i] = screenText( s, fromUTF8( text.c_str() ), x, y );
            screenKnob( s, i, v );
            const char *keys[] = { "minus", "plus" };
            for (int k=0 ; k<2 ; k++){
                if (!fieldTable( L, keys[k] )) continue;
                int bx = fieldInt( L, "x", x ), by = fieldInt( L, "y", y );
                if (fieldStr( L, "file" ).empty())
                    screenTextButton( s, fromUTF8( fieldStr( L, "text" ).c_str() ), bx, by, i * WIDGET_BUTTONS + k + 1, false );
                else
                    screenExec( "spbtn " + std::to_string( screenImage( s, bx, by ) ) + "," + std::to_string( i * WIDGET_BUTTONS + k + 1 ) );
                lua_pop( L, 1 );
            }
        }
        else if ((type == "toggle" || type == "radio") && !fieldStr( L, "file" ).empty()){ // pictures: `file`, `on_file` while on
            std::vector<std::string> vars = widgetVars( L );
            int v = vars.empty() ? 0 : screenGet( vars[0] );
            bool state = type == "radio" ? v == fieldInt( L, "value", 1 ) : v == fieldInt( L, "on", 1 );
            std::string on_file = fieldStr( L, "on_file" );
            int no = screenImage( s, x, y, state && !on_file.empty() ? fileName( fromUTF8( on_file.c_str() ) ).c_str() : NULL );
            screenExec( "spbtn " + std::to_string( no ) + "," + std::to_string( i * WIDGET_BUTTONS ) +
                        (fieldInt( L, "pressed", -1 ) >= 0 ? "," + std::to_string( fieldInt( L, "pressed", -1 ) ) : "") );
        }
        else if (type == "toggle" || type == "radio"){
            std::vector<std::string> vars = widgetVars( L );
            int v = vars.empty() ? 0 : screenGet( vars[0] );
            bool state = type == "radio" ? v == fieldInt( L, "value", 1 ) : v == fieldInt( L, "on", 1 );
            std::string on, off;
            widgetMarks( L, on, off );
            std::string text = boundText( fieldStr( L, "text", "%C" ), v, 0, 0, 1, on, off, state );
            screenTextButton( s, fromUTF8( text.c_str() ), x, y, i * WIDGET_BUTTONS, false );
        }
        else if (type == "button" && fieldTable( L, "current" )){ // its page range shown: `selected` in its place, not a button
            bool in = s->page >= indexInt( L, 1, 1 ) && s->page <= indexInt( L, 2, 1 );
            lua_pop( L, 1 );
            std::string sel = fieldStr( L, "selected" );
            if (in && !sel.empty()){ // one picture, not the button's cells
                lua_newtable( L );
                screenImage( s, x, y, fileName( fromUTF8( sel.c_str() ) ).c_str() );
                lua_pop( L, 1 );
            }
            else if (!in) screenExec( "spbtn " + std::to_string( screenImage( s, x, y ) ) + "," + std::to_string( i * WIDGET_BUTTONS ) );
        }
        else if (type == "button"){
            int no = screenImage( s, x, y );
            bool on = fieldInt( L, "enabled", 1 ) != 0;
            if (on && fieldTable( L, "action" )){
                on = screenEnabled( s );
                lua_pop( L, 1 );
            }
            if (on) screenExec( "spbtn " + std::to_string( no ) + "," + std::to_string( i * WIDGET_BUTTONS ) +
                                (fieldInt( L, "pressed", -1 ) >= 0 ? "," + std::to_string( fieldInt( L, "pressed", -1 ) ) : "") );
            else{
                int cell = fieldInt( L, "disabled", -1 );
                if (cell >= 0) screenExec( "cell " + std::to_string( no ) + "," + std::to_string( cell ) );
            }
        }
        else if (type == "saveslots") screenSlots( s, i );
        else if (type == "list" && fieldStr( L, "source", "log" ) == "items") screenItems( s, i );
        else if (type == "list") screenList( s, i );
        else if (type == "gallery") screenGallery( s, i );
        lua_pop( L, 1 );
    }
    s->cur_w = 0;
    lua_pop( L, 2 );
}

// A cross-fade (print 10) over ms to what is drawn now, the text window left as it is.
void ONScripter::screenFade( int ms )
{
    tmp_effect.effect = 10;
    tmp_effect.duration = ms;
    if (!setEffect( &tmp_effect )) while (doEffect( &tmp_effect ));
}

/* The screen's `open` or `close` motion (key) on each sprite drawn: the
 * widget's own (false: none), else the screen's. {ms, zoom_x, zoom_y
 * (percent), alpha (0-256), x, y (the point the zoom keeps in place;
 * default the sprite's centre), curve (spmotion's)}: open from there to
 * the sprite as drawn, close from the sprite to there. Returns the
 * longest one's ms. */
int ONScripter::screenMotions( ConsScreen *s, const char *key )
{
    lua_State *L = s->L;
    bool open = !strcmp( key, "open" );
    int longest = 0;
    lua_rawgeti( L, LUA_REGISTRYINDEX, s->table );
    lua_getfield( L, -1, "widgets" );
    for (size_t k=0 ; k<s->sprites.size() ; k++){
        AnimationInfo &ai = sprite_info[s->sprites[k]];
        if (s->owners[k] > 0 && lua_istable( L, -1 )) lua_rawgeti( L, -1, s->owners[k] ); else lua_pushnil( L );
        if (lua_istable( L, -1 )) lua_getfield( L, -1, key ); else lua_pushnil( L );
        if (lua_isnil( L, -1 )){ lua_pop( L, 1 ); lua_getfield( L, -3, key ); }
        int ms = lua_istable( L, -1 ) ? fieldInt( L, "ms", 0 ) : 0;
        if (ms > 0 && ai.visible){
            int w = ai.pos.w * screen_ratio2 / screen_ratio1, h = ai.pos.h * screen_ratio2 / screen_ratio1;
            int cx = ai.orig_pos.x + w / 2, cy = ai.orig_pos.y + h / 2;
            int zx = fieldInt( L, "zoom_x", 100 ), zy = fieldInt( L, "zoom_y", 100 );
            // the offset that keeps (x, y) in place while the sprite zooms about its centre
            int ox = (fieldInt( L, "x", cx ) - cx) * (100 - zx) / 100, oy = (fieldInt( L, "y", cy ) - cy) * (100 - zy) / 100;
            std::string there = std::to_string( ox ) + "," + std::to_string( oy ) + "," + std::to_string( zx ) + "," +
                                std::to_string( zy ) + ",0," + std::to_string( fieldInt( L, "alpha", 256 ) );
            std::string rest = "0,0,100,100,0,256", curve = std::to_string( fieldInt( L, "curve", 0 ) );
            screenExec( "spmotion " + std::to_string( s->sprites[k] ) + (open ? ",0" : ",2") + ",0," + (open ? there : rest) +
                        ",0," + std::to_string( ms ) + "," + (open ? rest : there) + "," + curve );
            longest = std::max( longest, ms );
        }
        lua_pop( L, 2 );
    }
    lua_pop( L, 2 );
    return longest;
}

/* saveslots (on top of the stack), widget number w: the page's slots, each
 * at x + col*dx, y + row*dy: the selected look, the thumbnail, the "new"
 * mark, the button (its hit area and hover look) and the text lines. */
void ONScripter::screenSlots( ConsScreen *s, int w )
{
    lua_State *L = s->L;
    int rows = fieldInt( L, "rows", 1 ), cols = fieldInt( L, "cols", 1 ), per = rows * cols;
    int x0 = fieldInt( L, "x", 0 ), y0 = fieldInt( L, "y", 0 ), dx = fieldInt( L, "dx", 0 ), dy = fieldInt( L, "dy", 0 );
    int first = fieldInt( L, "first", 1 ) + (s->page - 1) * per;
    int all_first = fieldInt( L, "first", 1 ), all_num = per * fieldInt( L, "pages", 1 );
    int shot_w = 0, shot_h = 0;
    if (fieldTable( L, "shot" )){ shot_w = indexInt( L, 1, 0 ); shot_h = indexInt( L, 2, 0 ); lua_pop( L, 1 ); }

    // the newest save among the widget's slots
    int newest = 0;
    time_t newest_t = 0;
    for (int k=0 ; k<all_num ; k++){
        time_t t = screenSaveTime( all_first + k );
        if (t > newest_t){ newest_t = t; newest = all_first + k; }
    }

    // a thumbnail (the shot, centred in the box and scaled to it)
    auto thumb = [&]( const char *key, int bx, int by, int slot ){
        if (!fieldTable( L, key )) return;
        int tx = bx + fieldInt( L, "x", 0 ), ty = by + fieldInt( L, "y", 0 ), tw = fieldInt( L, "w", 0 ), th = fieldInt( L, "h", 0 );
        int sw = shot_w ? shot_w : tw, sh = shot_h ? shot_h : th;
        lua_pushinteger( L, sw ? tw * 100 / sw : 100 );
        lua_setfield( L, -2, "scale" );
        lua_pushboolean( L, 1 );
        lua_setfield( L, -2, "opaque" );
        screenImage( s, tx + tw / 2 - sw / 2, ty + th / 2 - sh / 2, thumbName( slot ).c_str() );
        lua_pop( L, 1 );
    };
    auto image = [&]( const char *key, int bx, int by ){
        if (!fieldTable( L, key )) return;
        screenImage( s, bx + fieldInt( L, "x", 0 ), by + fieldInt( L, "y", 0 ) );
        lua_pop( L, 1 );
    };

    for (int k=0 ; k<per ; k++){
        int slot = first + k;
        int bx = x0 + (k % cols) * dx, by = y0 + (k / cols) * dy;
        time_t t = screenSaveTime( slot );
        if (slot == s->selected) image( "selected", bx, by );
        if (t) thumb( "thumb", bx, by, slot );
        if (t && slot == newest) image( "new", bx, by );
        if (fieldTable( L, "button" )){
            int no = screenImage( s, bx + fieldInt( L, "x", 0 ), by + fieldInt( L, "y", 0 ) );
            screenExec( "spbtn " + std::to_string( no ) + "," + std::to_string( w * WIDGET_BUTTONS + k + 1 ) );
            lua_pop( L, 1 );
        }
        char *text = t ? readSaveStrFromFile( slot ) : NULL;
        std::string split = fromUTF8( fieldStr( L, "split" ).c_str() );
        if (fieldTable( L, "lines" )){
            int n = (int)lua_rawlen( L, -1 );
            for (int j=1 ; j<=n ; j++){
                lua_rawgeti( L, -1, j );
                std::string line = slotText( fieldStr( L, t ? "text" : "empty" ), slot, t, text, split );
                if (!line.empty()) screenText( s, line, bx + fieldInt( L, "x", 0 ), by + fieldInt( L, "y", 0 ) );
                lua_pop( L, 1 );
            }
            lua_pop( L, 1 );
        }
        if (text) delete[] text;
    }
    if (s->selected && screenSaveTime( s->selected )) thumb( "preview", 0, 0, s->selected );
}

/* Actions */

// Asks the action's question (on top of the stack) on the dialog screen:
// 1 yes (or no question), 0 no, -1 the screens are gone (a load).
int ONScripter::screenConfirm( ConsScreen *s, int slot )
{
    lua_State *L = s->L;
    std::string q = fieldStr( L, "confirm" ), dialog = fieldStr( L, "dialog" ); // the action's own dialog first
    if (q.empty() && dialog.empty()) return 1;
    if (dialog.empty()){
        lua_rawgeti( L, LUA_REGISTRYINDEX, s->table );
        dialog = fieldStr( L, "dialog", "confirm" );
        lua_pop( L, 1 );
    }
    char no[16];
    snprintf( no, sizeof(no), "%02d", slot );
    for (size_t i ; (i = q.find( "%N" )) != std::string::npos ; ) q.replace( i, 2, no );
    int value = 0;
    if (screenOpen( dialog, q, &value ) == SCREEN_LOADED) return -1;
    return value != 0;
}

void ONScripter::screenSave( ConsScreen *s, int slot )
{
    lua_State *L = s->L;
    // the widget's shot size and text: the first saveslots
    int shot_w = 0, shot_h = 0;
    std::string text;
    lua_rawgeti( L, LUA_REGISTRYINDEX, s->table );
    lua_getfield( L, -1, "widgets" );
    int n = lua_istable( L, -1 ) ? (int)lua_rawlen( L, -1 ) : 0;
    for (int i=1 ; i<=n ; i++){
        lua_rawgeti( L, -1, i );
        if (indexStr( L, 1 ) == "saveslots"){
            if (fieldTable( L, "shot" )){ shot_w = indexInt( L, 1, 0 ); shot_h = indexInt( L, 2, 0 ); lua_pop( L, 1 ); }
            else if (fieldTable( L, "thumb" )){ shot_w = fieldInt( L, "w", 0 ); shot_h = fieldInt( L, "h", 0 ); lua_pop( L, 1 ); }
            std::string ref = fieldStr( L, "save_text" );
            ScriptHandler::VariableInfo vi;
            if (!ref.empty() && screenVariable( ref.c_str(), true, vi )){
                ScriptHandler::VariableData &d = script_h.getVariableData( vi.var_no );
                if (vi.type == ScriptHandler::VAR_STR && d.str) text = d.str;
                else if (vi.type != ScriptHandler::VAR_STR) text = std::to_string( script_h.getIntVariable( &vi ) );
            }
            lua_pop( L, 1 );
            break;
        }
        lua_pop( L, 1 );
    }
    lua_pop( L, 2 );

    if (s->shot && shot_w > 0 && shot_h > 0){
        SDL_Surface *surface = AnimationInfo::alloc32bitSurface( shot_w, shot_h, texture_format );
        resizeSurface( s->shot, surface );
        std::string path = saveDirPath( thumbName( slot ).c_str() );
#if defined(ANDROID) || defined(WEB)
        FILE *fp = ::fopen( path.c_str(), "wb" );
        SDL_RWops *rwops = fp ? SDL_RWFromFP( fp, SDL_TRUE ) : NULL;
#else
        SDL_RWops *rwops = SDL_RWFromFile( path.c_str(), "wb" );
#endif
        if (rwops == NULL || SDL_SaveBMP_RW( surface, rwops, 1 ) != 0){
            utils::printError( "screen: can't write %s: %s\n", path.c_str(), SDL_GetError() );
            if (trace_fp) traceWarn( "screenshot not written", path.c_str() );
        }
        SDL_FreeSurface( surface );
    }
    writeSaveFile( slot, text.c_str() ); // the game as it was when the screen opened
}

/* Runs the action (a table: {"what", ...}) or the list of them on top of
 * the stack, from the start-th. */
int ONScripter::screenActions( ConsScreen *s, int start, int *value )
{
    lua_State *L = s->L;
    lua_rawgeti( L, -1, 1 );
    bool list = lua_istable( L, -1 );
    lua_pop( L, 1 );
    int n = list ? (int)lua_rawlen( L, -1 ) : 1;
    for (int i=start ; i<n ; i++){
        if (list) lua_rawgeti( L, -1, i+1 );
        else lua_pushvalue( L, -1 );
        std::string what = indexStr( L, 1 );
        int slot = actionSlot( L, s->selected );
        int ret = SCREEN_STAY;
        bool stop = false; // a confirm answered no, or the action can't be taken: the rest is left out

        // a question before any other action (save, load and delete ask their own way, below)
        if (what != "save" && what != "load" && what != "delete" && (!fieldStr( L, "confirm" ).empty() || !fieldStr( L, "dialog" ).empty())){
            int yes = screenConfirm( s, slot );
            if (yes < 0) return SCREEN_LOADED;
            if (yes == 0){
                screenTrace( s, ("no " + what).c_str(), 0, INT_MIN );
                lua_pop( L, 1 );
                return SCREEN_STAY;
            }
        }

        if (what == "close"){
            *value = indexInt( L, 2, 0 );
            screenTrace( s, "close", 0, *value );
            ret = SCREEN_CLOSE;
        }
        else if (what == "scroll"){
            int n = atoi( indexStr( L, 2 ).c_str() );
            if (screenScrollable( s, n )){
                s->scroll += n;
                screenTrace( s, "scroll", 0, s->scroll );
            }
            else if (fieldTable( L, "at_end" )){ // can't: this instead (close at the newest page)
                ret = screenActions( s, 0, value );
                if (ret == SCREEN_LOADED) return ret;
                lua_pop( L, 1 );
            }
        }
        else if (what == "bgm"){ // plays the music from its start, looping
            screenExec( "bgmfadein 0" );
            screenExec( "bgm " + quoted( fileName( fromUTF8( indexStr( L, 2 ).c_str() ) ) ) );
            screenTrace( s, "bgm", 0, INT_MIN );
        }
        else if (what == "page"){
            int page = pageTarget( indexStr( L, 2 ), s->page, slotPages( L, s->table ) );
            if (page > 0){ s->page = page; s->selected = 0; }
            screenTrace( s, "page", 0, page );
        }
        else if (what == "save" || what == "load" || what == "delete"){
            bool has = slot > 0 && screenSaveTime( slot );
            int yes = 1;
            if (slot <= 0 || (what != "save" && !has)) stop = true;
            else if ((what != "save" || has) && (yes = screenConfirm( s, slot )) <= 0){
                if (yes < 0) return SCREEN_LOADED;
                screenTrace( s, ("no " + what).c_str(), slot, INT_MIN );
                stop = true;
            }
            else if (what == "save"){
                screenSave( s, slot );
                screenTrace( s, "save", slot, INT_MIN );
                s->selected = 0;
            }
            else if (what == "delete"){
                screenExec( "delsavegame " + std::to_string( slot ) + "," + quoted( thumbName( slot ) ) );
                screenTrace( s, "delete", slot, INT_MIN );
                s->selected = 0;
            }
            else{
                screenTrace( s, "load", slot, INT_MIN );
                lua_pop( L, 1 );
                screenCloseAll();
                loadGame( slot ); // if it can't, the script goes on after the screen command
                return SCREEN_LOADED; // the screens are gone either way
            }
        }
        else if (what == "set"){
            ScriptHandler::VariableInfo vi;
            std::string ref = indexStr( L, 2 );
            bool to_slot = fieldInt( L, "slot", 0 );
            if (screenVariable( ref.c_str(), false, vi )){
                lua_rawgeti( L, -1, 3 );
                if (vi.type == ScriptHandler::VAR_STR)
                    setStr( &script_h.getVariableData( vi.var_no ).str, fromUTF8( lua_isstring( L, -1 ) ? lua_tostring( L, -1 ) : "" ).c_str() );
                else
                    script_h.setInt( &vi, to_slot ? s->selected : (int)lua_tointeger( L, -1 ) );
                lua_pop( L, 1 );
            }
            screenTrace( s, "set", slot, INT_MIN );
        }
        else if (what == "gosub"){
            std::string label = fromUTF8( indexStr( L, 2 ).c_str() );
            if (label[0] == '*') label.erase( 0, 1 );
            screenTrace( s, "gosub", slot, INT_MIN );
            if (i + 1 < n){
                lua_pushvalue( L, list ? -2 : -1 );
                s->pending = luaL_ref( L, LUA_REGISTRYINDEX );
                s->pending_next = i + 1;
            }
            gosubReal( label.c_str(), s->at ); // the screen command again: it goes on with the rest
            ret = SCREEN_GOSUB;
        }
        else if (what == "open"){
            std::string name = indexStr( L, 2 ), arg = indexStr( L, 3 );
            int v = 0;
            screenTrace( s, "open", 0, INT_MIN );
            lua_pop( L, 1 );
            if (screenOpen( name, arg, &v ) == SCREEN_LOADED) return SCREEN_LOADED;
            continue;
        }
        else
            utils::printError( "screen %s: unknown action %s\n", s->name.c_str(), what.c_str() );
        lua_pop( L, 1 );
        if (ret != SCREEN_STAY || stop) return ret;
    }
    return SCREEN_STAY;
}

/* Opening, waiting, closing */

ConsScreen *ONScripter::screenLoad( const std::string &name, const std::string &arg )
{
    std::string file = "screens/" + name + ".lua";
    unsigned long length = script_h.cBR->getFileLength( file.c_str() );
    if (length == 0) errorAndExit( ("screen: can't find " + file).c_str() );
    std::vector<char> buf( length + 1 );
    int location;
    script_h.cBR->getFile( file.c_str(), (unsigned char*)buf.data(), &location );

    ConsScreen *s = new ConsScreen;
    s->name = name;
    s->arg = arg;
    s->L = luaL_newstate();
    lua_State *L = s->L;
    luaL_requiref( L, "_G", luaopen_base, 1 );
    luaL_requiref( L, LUA_STRLIBNAME, luaopen_string, 1 );
    luaL_requiref( L, LUA_TABLIBNAME, luaopen_table, 1 );
    luaL_requiref( L, LUA_MATHLIBNAME, luaopen_math, 1 );
    lua_pop( L, 4 );
    for (int str=0 ; str<2 ; str++){
        lua_pushlightuserdata( L, this );
        lua_pushboolean( L, str );
        lua_pushcclosure( L, luaVar, 2 );
        lua_setglobal( L, str ? "str" : "var" );
    }
    lua_pushstring( L, arg.c_str() );
    lua_setglobal( L, "arg" );
    if (luaL_loadbuffer( L, buf.data(), length, file.c_str() ) || lua_pcall( L, 0, 1, 0 ))
        errorAndExit( ("screen: " + std::string( lua_tostring( L, -1 ) )).c_str() );
    if (!lua_istable( L, -1 )) errorAndExit( ("screen: " + file + " returns no table").c_str() );
    s->kind = fieldStr( L, "kind", "other" );
    s->table = luaL_ref( L, LUA_REGISTRYINDEX );
    return s;
}

void ONScripter::screenShow( ConsScreen *s )
{
    lua_State *L = s->L;
    // the game as it is now, for a save made from here (as savegame does)
    s->saveon = internal_saveon_flag;
    if (saveon_flag && internal_saveon_flag) storeSaveFile();
    internal_saveon_flag = false;
    if (!s->shot) s->shot = AnimationInfo::alloc32bitSurface( screen_width, screen_height, texture_format );
    if (s->shot) SDL_BlitSurface( accumulation_surface, NULL, s->shot, NULL );

    lua_rawgeti( L, LUA_REGISTRYINDEX, s->table );
    if (fieldTable( L, "hide" )){
        if (fieldInt( L, "window", 0 ) && (display_mode & DISPLAY_MODE_TEXT)){
            s->window = true;
            screenExec( "textoff" );
        }
        if (fieldTable( L, "sprites" )){
            int n = (int)lua_rawlen( L, -1 );
            for (int i=1 ; i<=n ; i++){
                int no = indexInt( L, i, -1 );
                if (no >= 0 && no < MAX_SPRITE_NUM && sprite_info[no].visible){
                    s->hidden.push_back( no );
                    screenExec( "vsp " + std::to_string( no ) + ",0" );
                }
            }
            lua_pop( L, 1 );
        }
        lua_pop( L, 1 );
    }
    if (fieldTable( L, "filter" )){
        std::string cmd = "screenfilter ";
        int n = (int)lua_rawlen( L, -1 );
        for (int i=1 ; i<=n ; i++) cmd += (i > 1 ? "," : "") + std::to_string( indexInt( L, i, 0 ) );
        s->filter_slot = indexInt( L, 1, 0 );
        if (s->filter_slot > 0) screenExec( cmd );
        lua_pop( L, 1 );
    }
    // the screen's own pages: from its page_var (and written back at close)
    {
        std::string ref = fieldStr( L, "page_var" );
        int pages = fieldInt( L, "pages", 0 );
        if (pages > 0 && !ref.empty()){
            int p = screenGet( ref );
            s->page = p >= 1 && p <= pages ? p : 1;
        }
    }
    // the save slots' page: the page variable's, else the one shown last
    lua_getfield( L, -1, "widgets" );
    int n = lua_istable( L, -1 ) ? (int)lua_rawlen( L, -1 ) : 0;
    for (int i=1 ; i<=n ; i++){
        lua_rawgeti( L, -1, i );
        if (indexStr( L, 1 ) == "saveslots"){
            int pages = fieldInt( L, "pages", 1 );
            std::string ref = fieldStr( L, "page_var" );
            if (!ref.empty()) s->page = screenGet( ref );
            else s->page = screen_pages[s->name];
            if (s->page < 1 || s->page > pages) s->page = 1;
        }
        lua_pop( L, 1 );
    }
    lua_pop( L, 2 );
    screenTrace( s, "open", 0, INT_MIN );
}

void ONScripter::screenHide( ConsScreen *s )
{
    lua_State *L = s->L;
    screenClear( s );
    // the page for next time
    screen_pages[s->name] = s->page;
    lua_rawgeti( L, LUA_REGISTRYINDEX, s->table );
    if (fieldInt( L, "pages", 0 ) > 0 && !fieldStr( L, "page_var" ).empty()) screenSet( fieldStr( L, "page_var" ), s->page );
    lua_getfield( L, -1, "widgets" );
    int n = lua_istable( L, -1 ) ? (int)lua_rawlen( L, -1 ) : 0;
    for (int i=1 ; i<=n ; i++){
        lua_rawgeti( L, -1, i );
        std::string ref = fieldStr( L, "page_var" );
        if (indexStr( L, 1 ) == "saveslots" && !ref.empty()) screenSet( ref, s->page );
        lua_pop( L, 1 );
    }
    lua_pop( L, 2 );
    if (s->window) screenExec( "texton" ); // first: it drops the pending updates but the window's
    for (int no : s->hidden) screenExec( "vsp " + std::to_string( no ) + ",1" );
    if (s->filter_slot > 0) screenExec( "screenfilter " + std::to_string( s->filter_slot ) + ",0" );
    internal_saveon_flag = s->saveon;
    if (s->shot) SDL_FreeSurface( s->shot );
    if (s->pending != LUA_NOREF) luaL_unref( L, LUA_REGISTRYINDEX, s->pending );
    lua_close( L );
    delete s;
}

void ONScripter::screenCloseAll()
{
    while (!screens.empty()){
        ConsScreen *s = screens.back();
        screens.pop_back();
        screenHide( s );
    }
}

// The button the player took (0: none of the screen's).
int ONScripter::screenWait( ConsScreen *s )
{
    for (ButtonLink *bl = root_button_link.next ; bl ; bl = bl->next){
        bl->show_flag = 0;
        if (bl->button_type == ButtonLink::SPRITE_BUTTON){
            sprite_info[bl->sprite_no].visible = true;
            sprite_info[bl->sprite_no].setCell( 0 );
        }
        dirty_rect.add( bl->image_rect );
    }
    skip_mode &= ~SKIP_NORMAL;
    automode_flag = false;
    webStateHook();
    flush( refreshMode() );
    trace_cmd_start = s->at; // --auto-play's choices are made at the screen command
    event_mode = WAIT_BUTTON_MODE | WAIT_TIMER_MODE;
    refreshMouseOverButton();
    lua_rawgeti( s->L, LUA_REGISTRYINDEX, s->table );
    bool wheel = fieldTable( s->L, "wheel_up" ) ? (lua_pop( s->L, 1 ), true) : false;
    if (fieldTable( s->L, "wheel_down" )){ lua_pop( s->L, 1 ); wheel = true; }
    if (!s->knobs.empty()) wheel = true; // the wheel and Left/Right step a knob
    lua_pop( s->L, 1 );
    bool usewheel = usewheel_flag;
    if (wheel) usewheel_flag = true; // the wheel gives -2 (up) and -3 (down)
    screen_waiting = true;
    waitEvent( -1 );
    screen_waiting = false;
    usewheel_flag = usewheel;
    int button = current_button_state.button;
    if (trace_fp) traceButtons( button );
    if (auto_play_flag) autoPlayChosen( button );
    event_mode = IDLE_EVENT_MODE;
    disableGetButtonFlag();
    return button;
}

/* Runs screen s (already shown, on top of `screens`) until it closes;
 * *value: what close gave. */
int ONScripter::screenRun( ConsScreen *s, int *value )
{
    lua_State *L = s->L;
    for (;;){
        int ret = SCREEN_STAY;
        if (s->pending != LUA_NOREF){
            lua_rawgeti( L, LUA_REGISTRYINDEX, s->pending );
            luaL_unref( L, LUA_REGISTRYINDEX, s->pending );
            s->pending = LUA_NOREF;
            ret = screenActions( s, s->pending_next, value );
            if (ret != SCREEN_LOADED) lua_pop( L, 1 );
            if (ret != SCREEN_STAY) return ret;
        }
        screenDraw( s );
        int fade = s->fade_next;
        s->fade_next = 0;
        if (!s->shown){ // opening: its motions, its fade
            s->shown = true;
            screenMotions( s, "open" );
            lua_rawgeti( L, LUA_REGISTRYINDEX, s->table );
            fade = fieldInt( L, "fade", 0 );
            lua_pop( L, 1 );
        }
        if (fade > 0) screenFade( fade );
        int button = s->drag ? s->drag : screenWait( s ); // back from a gosub mid-drag: the drag goes on

        lua_rawgeti( L, LUA_REGISTRYINDEX, s->table );
        if (button == -2 || button == -3) button = screenKnobKey( s, button );
        if (s->view_w > 0 && (button == -1 || button == 0)){ // the viewer: back, or a click shows the panel
            if (button == -1) s->view_w = 0;
            else s->view_hidden = false;
        }
        else if ((button == -2 || button == -3) && fieldTable( L, button == -2 ? "wheel_up" : "wheel_down" )){
            ret = screenActions( s, 0, value );
            if (ret != SCREEN_LOADED) lua_pop( L, 1 );
        }
        else if (button == -1){ // right-click, Esc
            if (fieldTable( L, "cancel" )){
                ret = screenActions( s, 0, value );
                if (ret != SCREEN_LOADED) lua_pop( L, 1 );
            }
            else{
                *value = 0;
                screenTrace( s, "close", 0, 0 );
                ret = SCREEN_CLOSE;
            }
        }
        else if (button >= WIDGET_BUTTONS){
            lua_getfield( L, -1, "widgets" );
            lua_rawgeti( L, -1, button / WIDGET_BUTTONS );
            int k = button % WIDGET_BUTTONS;
            std::string type = indexStr( L, 1 );
            bool changed = false;
            if (type == "slider" && k == KNOB) ret = screenDrag( s, button, value );
            else if (type == "slider" || type == "toggle" || type == "radio"){
                std::vector<std::string> vars = widgetVars( L );
                int v = vars.empty() ? 0 : screenGet( vars[0] ), to = v;
                if (type == "slider"){
                    int step = fieldInt( L, "step", 1 );
                    to = std::max( fieldInt( L, "min", 0 ), std::min( fieldInt( L, "max", 10 ), v + (k == 1 ? -step : step) ) );
                }
                else if (type == "radio") to = fieldInt( L, "value", 1 );
                else to = v == fieldInt( L, "on", 1 ) ? fieldInt( L, "off", 0 ) : fieldInt( L, "on", 1 );
                if (to != v){
                    for (const std::string &r : vars) screenSet( r, to );
                    screenTrace( s, "set", 0, to );
                    changed = true;
                    s->fade_next = fieldInt( L, "fade", 0 );
                }
            }
            if (type == "gallery"){
                std::vector<std::string> files;
                std::vector<bool> seen;
                int cell = k >= VIEWER_BUTTONS ? s->view_cell : k;
                if (fieldTable( L, "cells" )){
                    lua_rawgeti( L, -1, cell );
                    if (lua_istable( L, -1 )) cellCgs( L, files, seen );
                    lua_pop( L, 2 );
                }
                if (k < VIEWER_BUTTONS){ // a cell: the viewer on its first CG seen
                    int v = seenVariant( seen, 0, 1 );
                    if (v >= 0){
                        s->view_w = button / WIDGET_BUTTONS; s->view_cell = k; s->view_variant = v; s->view_hidden = false;
                        screenTrace( s, "view", 0, k );
                    }
                }
                else if (k == VIEWER_BUTTONS || k == VIEWER_BUTTONS + 1){
                    int step = k == VIEWER_BUTTONS ? -1 : 1;
                    int v = seenVariant( seen, s->view_variant + step, step );
                    if (v >= 0) s->view_variant = v;
                    screenTrace( s, "variant", 0, s->view_variant );
                }
                else if (k == VIEWER_BUTTONS + 2) s->view_hidden = true;
                else s->view_w = 0;
            }
            else if (type == "list" && fieldStr( L, "source", "log" ) == "items"){
                if (fieldTable( L, "items" )){
                    lua_rawgeti( L, -1, k );
                    if (lua_istable( L, -1 ) && fieldTable( L, "action" )){
                        ret = screenActions( s, 0, value );
                        if (ret == SCREEN_LOADED) return ret;
                        lua_pop( L, 1 );
                    }
                    lua_pop( L, 2 );
                }
            }
            else if (type == "list"){ // a name: its page's voice
                if (k >= 1 && k <= (int)s->voices.size() && !s->voices[k-1].empty()){
                    screenExec( "dwave " + std::to_string( fieldInt( L, "voice_channel", 0 ) ) + "," + quoted( s->voices[k-1] ) );
                    screenTrace( s, "voice", 0, s->scroll + k - 1 );
                }
            }
            else if (type == "slider" || type == "toggle" || type == "radio"){
                if (changed && fieldTable( L, "action" )){ // after the change: apply it
                    ret = screenActions( s, 0, value );
                    if (ret != SCREEN_LOADED) lua_pop( L, 1 );
                }
            }
            else if (k > 0){ // a save slot
                int per = fieldInt( L, "rows", 1 ) * fieldInt( L, "cols", 1 );
                int slot = fieldInt( L, "first", 1 ) + (s->page - 1) * per + k - 1;
                if (fieldInt( L, "select", 0 ) && slot != s->selected){
                    s->selected = slot;
                    screenTrace( s, "select", slot, INT_MIN );
                }
                else{
                    s->selected = slot;
                    if (fieldTable( L, "action" )){
                        ret = screenActions( s, 0, value );
                        if (ret != SCREEN_LOADED) lua_pop( L, 1 );
                    }
                }
            }
            else if (fieldTable( L, "action" )){
                ret = screenActions( s, 0, value );
                if (ret != SCREEN_LOADED) lua_pop( L, 1 );
            }
            if (ret != SCREEN_LOADED) lua_pop( L, 2 );
        }
        if (ret == SCREEN_LOADED) return ret; // the Lua state is gone
        lua_pop( L, 1 );
        if (ret != SCREEN_STAY) return ret;
    }
}

// Opens screen `name` over the others and runs it until it closes.
int ONScripter::screenOpen( const std::string &name, const std::string &arg, int *value )
{
    ConsScreen *s = screenLoad( name, arg );
    s->at = screens.empty() ? trace_cmd_start : screens.back()->at;
    *value = 0;
    if (auto_play_flag && skip_screens.find( "," + s->kind + "," ) != std::string::npos){
        screenTrace( s, "skipped", 0, 0 );
        lua_close( s->L );
        delete s;
        return SCREEN_CLOSE;
    }
    screens.push_back( s );
    screenShow( s );
    int ret = screenRun( s, value );
    if (ret == SCREEN_GOSUB) return ret;
    if (ret != SCREEN_LOADED){
        int ms = screenMotions( s, "close" );
        if (ms > 0) screenExec( "wait " + std::to_string( ms ) );
        lua_rawgeti( s->L, LUA_REGISTRYINDEX, s->table );
        int fade = fieldInt( s->L, "fade", 0 );
        lua_pop( s->L, 1 );
        screens.pop_back();
        screenHide( s );
        if (fade > 0) screenFade( fade );
        else flush( refreshMode() );
    }
    return ret;
}

/* Picture sliders: a slider with `knob` (an image, placed where it is at
 * `min`) moved `travel` pixels from min to max (down with `vertical`;
 * negative: left or up), over `track` (an image, or a box {x, y, w, h};
 * default the knob's sweep). The knob is button widget*100+3 with the
 * track as its hit area, so the pointer over the track lights the knob
 * (cell 1). A press there drags it: `var` follows the pointer by `step`
 * and `action` runs at each change. The wheel and Left/Right over it step
 * it, moving the knob up or left for the wheel up and Left. */

struct Knob { int x, y, travel, min, max, step; bool vertical; };

// The picture slider on top of the stack.
static Knob knobOf( lua_State *L )
{
    Knob k = { 0, 0, fieldInt( L, "travel", 0 ), fieldInt( L, "min", 0 ), fieldInt( L, "max", 10 ),
               std::max( 1, fieldInt( L, "step", 1 ) ), fieldInt( L, "vertical", 0 ) != 0 };
    if (fieldTable( L, "knob" )){ k.x = fieldInt( L, "x", 0 ); k.y = fieldInt( L, "y", 0 ); lua_pop( L, 1 ); }
    return k;
}

// The knob's place at value v.
static void knobAt( const Knob &k, int v, int &x, int &y )
{
    int d = k.max > k.min ? (int)lround( (double)(v - k.min) * k.travel / (k.max - k.min) ) : 0;
    x = k.x + (k.vertical ? 0 : d);
    y = k.y + (k.vertical ? d : 0);
}

// Draws the track and the knob of slider w (on top of the stack) at value v.
void ONScripter::screenKnob( ConsScreen *s, int w, int v )
{
    lua_State *L = s->L;
    if (!fieldTable( L, "knob" )) return;
    lua_pop( L, 1 );
    Knob k = knobOf( L );
    SDL_Rect area = { 0, 0, 0, 0 };
    if (fieldTable( L, "track" )){
        area = { fieldInt( L, "x", 0 ), fieldInt( L, "y", 0 ), fieldInt( L, "w", 0 ), fieldInt( L, "h", 0 ) };
        if (!fieldStr( L, "file" ).empty()){
            AnimationInfo &t = sprite_info[screenImage( s, area.x, area.y )];
            area.w = t.pos.w * screen_ratio2 / screen_ratio1; // a cell's size, in script pixels
            area.h = t.pos.h * screen_ratio2 / screen_ratio1;
        }
        lua_pop( L, 1 );
    }
    if (fieldTable( L, "fill" )){ // under the knob: drawn first
        s->fills[w] = screenImage( s, fieldInt( L, "x", 0 ), fieldInt( L, "y", 0 ) );
        lua_pop( L, 1 );
    }
    int x, y;
    knobAt( k, v, x, y );
    fieldTable( L, "knob" );
    int no = screenImage( s, x, y );
    lua_pop( L, 1 );
    s->knobs[w] = no;
    screenFill( s, w, v );
    AnimationInfo &ai = sprite_info[no];
    if (area.w <= 0 || area.h <= 0){ // the knob's sweep
        int x1, y1;
        knobAt( k, k.max, x1, y1 );
        area = { std::min( k.x, x1 ), std::min( k.y, y1 ), abs( x1 - k.x ) + ai.pos.w * screen_ratio2 / screen_ratio1,
                 abs( y1 - k.y ) + ai.pos.h * screen_ratio2 / screen_ratio1 };
    }
    screenExec( "spbtn " + std::to_string( no ) + "," + std::to_string( w * WIDGET_BUTTONS + KNOB ) );
    for (ButtonLink *bl = root_button_link.next ; bl ; bl = bl->next)
        if (bl->no == w * WIDGET_BUTTONS + KNOB)
            bl->select_rect = { area.x * screen_ratio1 / screen_ratio2, area.y * screen_ratio1 / screen_ratio2,
                                area.w * screen_ratio1 / screen_ratio2, area.h * screen_ratio1 / screen_ratio2 };
    if (s->drag == w * WIDGET_BUTTONS + KNOB && ai.num_of_cells > 1) ai.setCell( 1 );
}

/* The fill picture of slider w (on top of the stack) at value v: cut
 * (spclip) from its min end (the side the knob is at min) to the knob's
 * centre. */
void ONScripter::screenFill( ConsScreen *s, int w, int v )
{
    auto f = s->fills.find( w );
    if (f == s->fills.end()) return;
    lua_State *L = s->L;
    Knob k = knobOf( L );
    SDL_Rect &kp = sprite_info[s->knobs[w]].pos;
    int kw = kp.w, kh = kp.h;
    int x, y;
    knobAt( k, v, x, y );
    AnimationInfo &ai = sprite_info[f->second];
    int fx = ai.orig_pos.x, fy = ai.orig_pos.y;
    int fw = ai.pos.w * screen_ratio2 / screen_ratio1, fh = ai.pos.h * screen_ratio2 / screen_ratio1;
    int c = k.vertical ? y + kh * screen_ratio2 / screen_ratio1 / 2 : x + kw * screen_ratio2 / screen_ratio1 / 2;
    int a = k.vertical ? fy : fx, b = a + (k.vertical ? fh : fw); // the fill's extent along the travel
    if (k.travel >= 0) b = std::min( b, c ); else a = std::max( a, c );
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );
    ai.visible = b > a;
    // spclip's rectangle (an empty one would clip nothing)
    ai.clip_rect[0] = k.vertical ? fx : a; ai.clip_rect[1] = k.vertical ? a : fy;
    ai.clip_rect[2] = k.vertical ? fw : b - a; ai.clip_rect[3] = k.vertical ? b - a : fh;
    if (!ai.visible) ai.clip_rect[2] = ai.clip_rect[3] = 0;
    if (ai.visible) dirty_rect.add( drawnRect( ai ) );
}

// The wheel or Left/Right (button -2, -3) over a knob: its slider's minus or plus button.
int ONScripter::screenKnobKey( ConsScreen *s, int button )
{
    int b = current_over_button;
    if (b < WIDGET_BUTTONS || b % WIDGET_BUTTONS != KNOB || !s->knobs.count( b / WIDGET_BUTTONS )) return button;
    lua_State *L = s->L;
    lua_getfield( L, -1, "widgets" );
    lua_rawgeti( L, -1, b / WIDGET_BUTTONS );
    bool up_lowers = (fieldInt( L, "travel", 0 ) > 0) != (button == -3);
    lua_pop( L, 2 );
    return b - KNOB + (up_lowers ? 1 : 2);
}

/* A press on a knob or its track (while a screen waits): it is taken at
 * once, not at the release, and dragged. x, y: the event's. */
bool ONScripter::screenPress( int x, int y )
{
    if (!screen_waiting || screens.empty()) return false;
#if !defined(WEB)
    x = (x - render_view_rect.x) * screen_scale_ratio1;
    y = (y - render_view_rect.y) * screen_scale_ratio2;
#endif
    current_button_state.x = x;
    current_button_state.y = y;
    mouseOverCheck( x, y );
    int b = current_over_button;
    if (b < WIDGET_BUTTONS || b % WIDGET_BUTTONS != KNOB || !screens.back()->knobs.count( b / WIDGET_BUTTONS )) return false;
    ConsScreen *s = screens.back();
    SDL_Rect &k = sprite_info[s->knobs[b / WIDGET_BUTTONS]].pos;
    bool on = x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h; // on the knob: it keeps its place under the pointer
    s->grab_x = on ? (x - k.x - k.w / 2) * screen_ratio2 / screen_ratio1 : 0;
    s->grab_y = on ? (y - k.y - k.h / 2) * screen_ratio2 / screen_ratio1 : 0;
    current_button_state.button = b;
    current_button_state.down_flag = true;
    return true;
}

// While a knob is dragged: waits for the pointer to move; false when the button is let go.
bool ONScripter::screenDragWait()
{
    ButtonLink *links = root_button_link.next;
    root_button_link.next = NULL; // no button lights up on the way
    int over = current_over_button;
    current_over_button = -1;
    bool flag = getmouseover_flag;
    int lower = getmouseover_lower, upper = getmouseover_upper;
    getmouseover_flag = true; // every move ends the wait
    getmouseover_lower = INT_MIN;
    getmouseover_upper = INT_MAX;
    current_button_state.event_type = 0;
    event_mode = WAIT_BUTTON_MODE;
    waitEventSub( -1 );
    event_mode = IDLE_EVENT_MODE;
    getmouseover_flag = flag;
    getmouseover_lower = lower;
    getmouseover_upper = upper;
    root_button_link.next = links;
    current_over_button = over;
    return !(current_button_state.event_type == SDL_MOUSEBUTTONUP && current_button_state.event_button == SDL_BUTTON_LEFT);
}

/* The knob `button` (its slider on top of the stack) pressed: follows the
 * pointer until the button is let go, setting the value and running the
 * action at each change. A gosub action leaves s->drag set: screenRun
 * comes back here when it returns. */
int ONScripter::screenDrag( ConsScreen *s, int button, int *value )
{
    lua_State *L = s->L;
    auto knob = s->knobs.find( button / WIDGET_BUTTONS );
    std::vector<std::string> vars = widgetVars( L );
    if ((!s->drag && !current_button_state.down_flag) || knob == s->knobs.end() || vars.empty())
        return SCREEN_STAY; // a key or --auto-play on the track: nothing to drag
    s->drag = button;
    Knob k = knobOf( L );
    AnimationInfo &ai = sprite_info[knob->second];
    if (ai.num_of_cells > 1){ ai.setCell( 1 ); dirty_rect.add( ai.pos ); }
    int ret = SCREEN_STAY;
    for (bool held = true ;;){
        // the knob's centre under the pointer
        int p = (k.vertical ? current_button_state.y : current_button_state.x) * screen_ratio2 / screen_ratio1 -
                (k.vertical ? s->grab_y : s->grab_x) -
                (k.vertical ? k.y + ai.pos.h * screen_ratio2 / screen_ratio1 / 2 : k.x + ai.pos.w * screen_ratio2 / screen_ratio1 / 2);
        int v = screenGet( vars[0] ), to = v;
        if (k.travel && k.max > k.min){
            to = k.min + (int)lround( (double)p * (k.max - k.min) / k.travel / k.step ) * k.step;
            to = std::max( k.min, std::min( k.max, to ) );
        }
        if (to != v){
            for (const std::string &r : vars) screenSet( r, to );
            screenTrace( s, "set", 0, to );
            dirty_rect.add( ai.pos );
            knobAt( k, to, ai.orig_pos.x, ai.orig_pos.y );
            ai.scalePosXY( screen_ratio1, screen_ratio2 );
            dirty_rect.add( ai.pos );
            for (ButtonLink *bl = root_button_link.next ; bl ; bl = bl->next)
                if (bl->no == button) bl->image_rect = ai.pos;
            screenFill( s, button / WIDGET_BUTTONS, to );
            auto text = s->texts.find( button / WIDGET_BUTTONS );
            if (text != s->texts.end()) // redrawn in its sprite: the value as it moves
                screenText( s, fromUTF8( sliderText( L, to ).c_str() ), fieldInt( L, "x", 0 ), fieldInt( L, "y", 0 ), NULL, text->second );
            flush( refreshMode() );
            if (fieldTable( L, "action" )){
                ret = screenActions( s, 0, value );
                if (ret == SCREEN_LOADED) return ret; // s is gone
                lua_pop( L, 1 );
                if (ret != SCREEN_STAY) break;
            }
        }
        else flush( refreshMode() );
        if (!held) break;
        // let go while a gosub action ran (its script may have taken the release)?
        held = (SDL_GetMouseState( NULL, NULL ) & SDL_BUTTON_LMASK) && screenDragWait();
    }
    if (ret != SCREEN_GOSUB){
        s->drag = 0;
        current_button_state.down_flag = false;
    }
    return ret;
}

void ONScripter::setSkipScreens( const char *kinds )
{
    skip_screens = std::string( "," ) + (strcmp( kinds, "none" ) ? kinds : "") + ",";
}

/* screen "name"[,%var[,"arg"]]: opens screens/name.lua and runs it until it
 * closes; %var gets the value its close action gave (0 by default). A
 * gosub action runs its label with the screen still open, then comes back
 * here: the same screen goes on. */
int ONScripter::screenCommand()
{
    char *at = script_h.getCurrent();
    std::string name = script_h.readStr();
    ScriptHandler::VariableInfo result;
    result.type = ScriptHandler::VAR_NONE;
    std::string arg;
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA){
        script_h.readVariable();
        result = script_h.current_variable;
        if (script_h.getEndStatus() & ScriptHandler::END_COMMA) arg = script_h.readStr();
    }

    int value = 0, ret;
    if (!screens.empty() && screens.back()->at == at && screens.back()->name == name){
        ConsScreen *s = screens.back(); // back from a gosub action
        trace_cmd_start = at;
        ret = screenRun( s, &value );
        if (ret == SCREEN_CLOSE){
            screens.pop_back();
            screenHide( s );
            flush( refreshMode() );
        }
    }
    else{
        trace_cmd_start = at;
        ret = screenOpen( name, arg, &value );
    }
    if (ret == SCREEN_CLOSE && result.type == ScriptHandler::VAR_INT) script_h.setInt( &result, value );
    return RET_CONTINUE;
}

#else // no Lua: no screens

int ONScripter::screenCommand()
{
    errorAndExit( "screen: this build has no Lua" );
    return RET_CONTINUE;
}

void ONScripter::setSkipScreens( const char * ) {}

bool ONScripter::screenPress( int, int ) { return false; }

#endif
