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

// "%12", "%name", "$3", "name" (an integer): the variable through the script parser
bool ONScripter::screenVariable( const char *ref, bool str, ScriptHandler::VariableInfo &vi )
{
    std::string s = fromUTF8( ref );
    if (s.empty()) return false;
    if (s[0] != '%' && s[0] != '$') s = (str ? "$" : "%") + s;
    std::vector<char> buf( s.begin(), s.end() );
    buf.push_back( 0 );
    script_h.enterExternalScript( buf.data() );
    script_h.readVariable();
    vi = script_h.current_variable;
    script_h.leaveExternalScript();
    return vi.type == ScriptHandler::VAR_INT || vi.type == ScriptHandler::VAR_STR;
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
    ScriptHandler::VariableData &d = script_h.getVariableData( vi.var_no );
    if (vi.type == ScriptHandler::VAR_STR){
        std::string utf8;
        traceString( utf8, d.str ? d.str : "", -1, false ); // in UTF-8, as the table is
        lua_pushstring( L, utf8.c_str() );
    }
    else lua_pushinteger( L, d.num );
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

// A text (the table on top: size, cols, rows, bold, shadow, color, align) at x, y.
void ONScripter::screenText( ConsScreen *s, const std::string &text, int x, int y )
{
    lua_State *L = s->L;
    int size = fieldInt( L, "size", 20 );
    if (fieldStr( L, "align" ) == "centre" || fieldStr( L, "align" ) == "center")
        x -= (int)text.size() * size / 4; // half-width characters are half as wide, as the script measures
    std::string color = fieldStr( L, "color" );
    int no = screenSprite( s );
    screenExec( "strsp " + std::to_string( no ) + "," + quoted( text ) + "," + std::to_string( x ) + "," + std::to_string( y ) + "," +
                std::to_string( fieldInt( L, "cols", 40 ) ) + "," + std::to_string( fieldInt( L, "rows", 1 ) ) + "," +
                std::to_string( size ) + "," + std::to_string( size ) + ",0,0," + std::to_string( fieldInt( L, "bold", 0 ) ) + "," +
                std::to_string( fieldInt( L, "shadow", 0 ) ) + (color.empty() ? "" : "," + quoted( color )) );
}

void ONScripter::screenClear( ConsScreen *s )
{
    for (int no : s->sprites){
        screenExec( "csp " + std::to_string( no ) );
        sprite_handle_layer[no - NS_SPRITE_NUM] = 0;
    }
    s->sprites.clear();
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
    }
    return true;
}

void ONScripter::screenDraw( ConsScreen *s )
{
    lua_State *L = s->L;
    screenClear( s );
    lua_rawgeti( L, LUA_REGISTRYINDEX, s->table );
    lua_getfield( L, -1, "widgets" );
    int n = lua_istable( L, -1 ) ? (int)lua_rawlen( L, -1 ) : 0;
    for (int i=1 ; i<=n ; i++){
        lua_rawgeti( L, -1, i );
        std::string type = indexStr( L, 1 );
        int x = fieldInt( L, "x", 0 ), y = fieldInt( L, "y", 0 );
        int page = fieldInt( L, "page", 0 );
        if (page > 0 && page != s->page){ lua_pop( L, 1 ); continue; }

        if (type == "image") screenImage( s, x, y );
        else if (type == "text") screenText( s, fromUTF8( fieldStr( L, "text" ).c_str() ), x, y );
        else if (type == "button"){
            int no = screenImage( s, x, y );
            bool on = true;
            if (fieldTable( L, "action" )){
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
        lua_pop( L, 1 );
    }
    lua_pop( L, 2 );
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
    std::string q = fieldStr( L, "confirm" );
    if (q.empty()) return 1;
    lua_rawgeti( L, LUA_REGISTRYINDEX, s->table );
    std::string dialog = fieldStr( L, "dialog", "confirm" );
    lua_pop( L, 1 );
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
                else if (vi.type == ScriptHandler::VAR_INT) text = std::to_string( d.num );
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

        if (what == "close"){
            *value = indexInt( L, 2, 0 );
            screenTrace( s, "close", 0, *value );
            ret = SCREEN_CLOSE;
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
                    script_h.setNumVariable( vi.var_no, to_slot ? s->selected : (int)lua_tointeger( L, -1 ) );
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
    // the save slots' page: the page variable's, else the one shown last
    lua_getfield( L, -1, "widgets" );
    int n = lua_istable( L, -1 ) ? (int)lua_rawlen( L, -1 ) : 0;
    for (int i=1 ; i<=n ; i++){
        lua_rawgeti( L, -1, i );
        if (indexStr( L, 1 ) == "saveslots"){
            int pages = fieldInt( L, "pages", 1 );
            std::string ref = fieldStr( L, "page_var" );
            ScriptHandler::VariableInfo vi;
            if (!ref.empty() && screenVariable( ref.c_str(), false, vi )) s->page = script_h.getVariableData( vi.var_no ).num;
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
    lua_getfield( L, -1, "widgets" );
    int n = lua_istable( L, -1 ) ? (int)lua_rawlen( L, -1 ) : 0;
    for (int i=1 ; i<=n ; i++){
        lua_rawgeti( L, -1, i );
        std::string ref = fieldStr( L, "page_var" );
        ScriptHandler::VariableInfo vi;
        if (indexStr( L, 1 ) == "saveslots" && !ref.empty() && screenVariable( ref.c_str(), false, vi ))
            script_h.setNumVariable( vi.var_no, s->page );
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
    waitEvent( -1 );
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
        int button = screenWait( s );

        lua_rawgeti( L, LUA_REGISTRYINDEX, s->table );
        if (button == -1){ // right-click, Esc
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
            if (k > 0){ // a save slot
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
        screens.pop_back();
        screenHide( s );
        flush( refreshMode() );
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

#endif
