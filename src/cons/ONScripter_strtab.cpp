/* -*- C++ -*-
 *
 *  ONScripter_strtab.cpp - CONscripter: text tables. The script names its
 *  text by ID; each language's text is a table beside 0.txt, and
 *  `language` switches table while the game runs
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

// A line of 0.txt that is `^` and an ID (letters and digits) is the text
// of that ID in text.<language>.txt, and so is a string "^ID" in a command
// line. A table line is the ID, a tab, and the text as a 0.txt line (tags
// and all, one line); lines starting with ';' and empty lines are left out.
// The script is filled in as it is read, so text works as before (waits,
// textgosub, read marks); a filled line's hash for saves and kidoku.dat is
// that of its `^ID` line, the same in every language.

#include "ONScripter.h"
#include "Utils.h"
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

static unsigned int fnvLine( const char *p )
{
    unsigned int h = 2166136261u; // FNV-1a 32
    for ( ; *p != 0x0a ; p++ ){
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    return h;
}

unsigned int ScriptHandler::textHash( const char *line )
{
    return fnvLine( line );
}

unsigned int ScriptHandler::lineHash( const char *line )
{
    if (!table_line_hash.empty() && inScript( line )){
        std::unordered_map<int, unsigned int>::const_iterator it = table_line_hash.find( (int)(line - script_buffer) );
        if (it != table_line_hash.end()) return it->second;
    }
    return fnvLine( line );
}

static std::string tableName( const char *lang )
{
    return std::string( "text." ) + lang + ".txt";
}

bool ScriptHandler::textTableExists( const char *lang )
{
    FILE *fp = fopen( tableName( lang ).c_str(), "rb" );
    if (fp) fclose( fp );
    return fp != NULL;
}

// memmem, which mingw lacks
static const char *findBytes( const char *p, const char *end, const char *key, size_t n )
{
    const char *r = std::search( p, end, key, key + n );
    return r == end ? NULL : r;
}

static bool isIdChar( char c )
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// `^ID` alone on the line (spaces around it): the ID's length, else 0
static int refLine( const char *p, const char *end )
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || *p != '^') return 0;
    const char *q = ++p;
    while (q < end && isIdChar( *q )) q++;
    const char *r = q;
    while (r < end && (*r == ' ' || *r == '\t')) r++;
    return (q > p && (r == end || *r == 0x0a)) ? (int)(q - p) : 0;
}

// The language of the script's first `language "code"` (at a line's start
// or after a ':'), the table read first.
static std::string firstLanguage( const char *buf, int len )
{
    const char *key = "language \"";
    for (const char *p = buf ; (p = findBytes( p, buf + len, key, 10 )) != NULL ; p += 10){
        const char *b = p;
        while (b > buf && (b[-1] == ' ' || b[-1] == '\t')) b--;
        if (b != buf && b[-1] != 0x0a && b[-1] != ':') continue;
        const char *q = p + 10, *e = q;
        while (e < buf + len && *e != '"' && *e != 0x0a) e++;
        if (*e == '"' && e > q) return std::string( q, e - q );
    }
    return "";
}

// language.txt in the save folder: the language `language` set last, one
// line, so a start reads the script once with its table.
std::string ScriptHandler::rememberedLanguage()
{
    std::string code;
    FILE *fp = fopen( "language.txt", "rb", true );
    if (!fp) return code;
    char buf[64];
    if (fgets( buf, sizeof(buf), fp )) code = buf;
    fclose( fp );
    while (!code.empty() && (code.back() == '\n' || code.back() == '\r' || code.back() == ' ')) code.pop_back();
    return code;
}

void ScriptHandler::rememberLanguage( const char *lang )
{
    if (!*lang || rememberedLanguage() == lang) return;
    FILE *fp = fopen( "language.txt", "wb", true );
    if (!fp) return;
    fprintf( fp, "%s\n", lang );
    fclose( fp );
}

void ScriptHandler::expandTextTable( const char *lang )
{
    table_line_hash.clear();
    const char *end = script_buffer + script_buffer_length;
    text_refs = false;
    for (const char *p = script_buffer ; p < end && !text_refs ; ){
        const char *e = (const char*)memchr( p, 0x0a, end - p );
        if (!e) e = end;
        text_refs = refLine( p, e ) > 0;
        p = e + 1;
    }
    if (!text_refs) return;

    // the language last chosen (language.txt) when it has a table, else the first `language`'s
    std::string code = lang ? std::string( lang ) : rememberedLanguage();
    if (!lang && (code.empty() || !textTableExists( code.c_str() ))) code = firstLanguage( script_buffer, script_buffer_length );
    FILE *fp = code.empty() ? NULL : fopen( tableName( code.c_str() ).c_str(), "rb" );
    if (!fp){
        utils::printError( "text table: the script names its text by ID, but there is no %s\n",
                           code.empty() ? "`language` command to choose a table" : tableName( code.c_str() ).c_str() );
        return;
    }
    std::string data;
    std::vector<char> chunk( 65536 ); // on the heap: the web build's whole stack is 64 KB
    for (size_t n ; (n = ::fread( chunk.data(), 1, chunk.size(), fp )) > 0 ; ) data.append( chunk.data(), n );
    fclose( fp );

    std::unordered_map<std::string, std::string> table;
    for (size_t p = 0 ; p < data.size() ; ){
        size_t e = data.find( '\n', p );
        if (e == std::string::npos) e = data.size();
        size_t le = (e > p && data[e-1] == '\r') ? e-1 : e;
        if (le > p && data[p] != ';'){
            size_t tab = data.find( '\t', p );
            if (tab == std::string::npos || tab > le) tab = le;
            table[data.substr( p, tab - p )] = (tab < le) ? data.substr( tab + 1, le - tab - 1 ) : std::string();
        }
        p = e + 1;
    }

    // the filled script: each `^ID` line and "^ID" string replaced
    std::string out;
    out.reserve( script_buffer_length + script_buffer_length / 4 );
    std::vector<std::pair<int, unsigned int> > filled;
    int missing = 0;
    std::string first_missing;
    for (const char *p = script_buffer ; p < end ; ){
        const char *e = (const char*)memchr( p, 0x0a, end - p );
        if (!e) e = end;
        int id_len = refLine( p, e );
        const char *s = p;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        bool command = s < e && ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_');
        if (id_len > 0 || (command && findBytes( s, e, "\"^", 2 ))){
            int at = out.size();
            if (id_len > 0){
                const char *id = (const char*)memchr( p, '^', e - p ) + 1;
                std::unordered_map<std::string, std::string>::const_iterator it = table.find( std::string( id, id_len ) );
                if (it != table.end()) out += it->second;
                else if (missing++ == 0) first_missing.assign( id, id_len );
            }
            else{
                for (const char *q = p ; q < e ; ){
                    const char *r = q;
                    if (q[0] == '"' && q+1 < e && q[1] == '^'){
                        r = q + 2;
                        while (r < e && isIdChar( *r )) r++;
                        std::unordered_map<std::string, std::string>::const_iterator it;
                        if (r < e && *r == '"' && r > q+2 &&
                            (it = table.find( std::string( q+2, r - q - 2 ) )) != table.end()){
                            out += '"';
                            out += it->second;
                            out += '"';
                            q = r + 1;
                            continue;
                        }
                    }
                    out += *q++;
                }
            }
            filled.push_back( std::make_pair( at, fnvLine( p ) ) );
        }
        else out.append( p, e - p );
        out += 0x0a;
        p = e + 1;
    }
    if (missing)
        utils::printError( "text table %s: %d IDs of the script aren't in it (the first: %s); those lines are empty\n",
                           tableName( code.c_str() ).c_str(), missing, first_missing.c_str() );

    delete[] script_buffer;
    script_buffer = new char[out.size() + 1];
    memcpy( script_buffer, out.data(), out.size() );
    script_buffer[out.size()] = 0;
    script_buffer_length = out.size();
    current_script = next_script = script_buffer;
    // labels a table line holds count too (a line per language: the code
    // two editions differ in); as labelScript finds them
    num_of_labels = 0;
    for (const char *p = script_buffer, *end = script_buffer + script_buffer_length ; p < end ; ){
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p < end && *p == '*') num_of_labels++;
        const char *e = (const char*)memchr( p, 0x0a, end - p );
        p = e ? e + 1 : end;
    }
    for (size_t i=0 ; i<filled.size() ; i++) table_line_hash[filled[i].first] = filled[i].second;
    text_table = code;
}

void ScriptHandler::scriptPointers( std::vector<char**> &p )
{
    p.push_back( &current_script );
    p.push_back( &next_script );
    p.push_back( &wait_script );
    p.push_back( &pushed_current_script );
    p.push_back( &pushed_next_script );
    p.push_back( &backquote_resume );
    for (int i=0 ; i<wait_marks ; i++) p.push_back( &wait_mark_script[i] );
    for (ScriptContext *sc = root_script_context.next ; sc ; sc = sc->next){
        p.push_back( &sc->current_script );
        p.push_back( &sc->next_script );
    }
}

void ScriptHandler::reloadScript( const char *lang, std::vector<char**> &pointers )
{
    std::vector<Place> places( pointers.size() );
    std::vector<int> offset( pointers.size(), -1 ); // in the old script, for a place outside the labels
    std::vector<bool> placed( pointers.size() );
    for (size_t i=0 ; i<pointers.size() ; i++){
        offset[i] = getOffset( *pointers[i] );
        placed[i] = offset[i] >= 0 && placeOf( *pointers[i], places[i] );
    }

    // the read marks go over through kidoku.dat's format, by line hash
    FILE *kf = kidoku_buffer ? tmpfile() : NULL;
    if (kf) writeKidokuCons( kf );

    for (int i=0 ; i<num_of_labels ; i++) delete[] label_info[i].name;
    delete[] label_info;
    num_of_labels = 0;
    char *path = archive_path;
    text_table = lang;
    readScript( path );
    delete[] path;
    labelScript();

    if (kidoku_buffer){
        delete[] kidoku_buffer;
        kidoku_buffer = new char[script_buffer_length/8 + 1];
        memset( kidoku_buffer, 0, script_buffer_length/8 + 1 );
        if (kf){
            rewind( kf );
            readKidokuCons( kf );
        }
    }
    if (kf) fclose( kf );

    for (size_t i=0 ; i<pointers.size() ; i++){
        if (offset[i] < 0) continue; // not in the script (NULL, or an external script's)
        char *a = placed[i] ? addressOf( places[i] ) : NULL;
        *pointers[i] = a ? a : script_buffer + (offset[i] < script_buffer_length ? offset[i] : 0);
    }
}

// `language` with a table for the language: the script is read again with
// that table; the game goes on where it is, with the next text in the new
// language (what is on the screen and in the backlog stays as it was).
void ONScripter::switchTextTable( const char *lang )
{
    std::vector<char**> p;
    script_h.scriptPointers( p );
    for (NestInfo *n = root_nest_info.next ; n ; n = n->next) p.push_back( &n->next_script );
    p.push_back( &last_tilde.next_script );
    p.push_back( &select_label_info.next_script );
    p.push_back( &pretext_buf );
    p.push_back( &trace_cmd_start );
    for (size_t i=0 ; i<trace_shot_at.size() ; i++){ // --shot-at's lines
        p.push_back( &trace_shot_at[i].begin );
        p.push_back( &trace_shot_at[i].end );
    }
    int label = script_h.getLabelIndex( current_label_info.label_header );
    script_h.reloadScript( lang, p );
    // a table's line is one line in every language: the line numbers stay
    if (label >= 0) current_label_info = script_h.getLabelInfo( label );
    auto_choice_visits.clear();

    // a textgosub or rgosub that comes back into the middle of a text line
    // reads on as a ` string when the new language's line is one there
    script_h.backquote_resume = NULL;
    for (NestInfo *n = root_nest_info.next ; n ; n = n->next){
        char *a = n->next_script;
        int o = script_h.getOffset( a );
        if (o <= 0 || a[-1] == 0x0a) continue;
        char *line = a;
        while (line > a - o && line[-1] != 0x0a) line--;
        if (!script_h.isTableLine( line )) continue;
        int quotes = 0;
        for (char *q = line ; q < a ; q++) if (*q == '`') quotes++;
        if (quotes % 2) script_h.backquote_resume = a;
    }
}

/* Asset layers: a picture, sound or other file the script names is read from
 * layer\<name>\<path> when an active layer has one, else from <path>. The
 * active layers are those `assetlayer` turned on (the last turned on first),
 * then the text language's. So a build carries its variants side by side
 * (the English pictures in layer\en\, uncensored ones in layer\uncensored\)
 * and the script names only the base path. */
std::vector<std::string> LayerReader::layers;

std::string LayerReader::layered( const char *file_name )
{
    for (size_t i=0 ; i<layers.size() ; i++){
        std::string name = "layer\\" + layers[i] + "\\" + file_name;
        if (base->getFileLength( name.c_str() ) > 0) return name;
    }
    return file_name;
}

// shortcut: one look-up per layer on each read (a hash look-up in an archive); cache it if a trace shows the cost
size_t LayerReader::getFileLength( const char *file_name )
{
    return layers.empty() ? base->getFileLength( file_name ) : base->getFileLength( layered( file_name ).c_str() );
}

size_t LayerReader::getFile( const char *file_name, unsigned char *buffer, int *location )
{
    return layers.empty() ? base->getFile( file_name, buffer, location ) : base->getFile( layered( file_name ).c_str(), buffer, location );
}

static std::vector<std::string> asset_layers; // assetlayer's, the last turned on first
static std::string language_layer;

// The layers changed: pictures already loaded are read again when next set up
// (setupAnimationInfo keeps a picture of the same name otherwise).
void ONScripter::setLayers()
{
    LayerReader::layers = asset_layers;
    if (!language_layer.empty()) LayerReader::layers.push_back( language_layer );
    for (int i=0 ; i<MAX_SPRITE_NUM ; i++) sprite_info[i].filter_changed = true;
    for (int i=0 ; i<MAX_SPRITE2_NUM ; i++) sprite2_info[i].filter_changed = true;
    for (int i=0 ; i<3 ; i++) tachi_info[i].filter_changed = true;
    bg_info.filter_changed = true;
}

// A language's own font: `<name>.<lang><ext>` beside the font file (default.en.ttf),
// used while the language is on; the text drawn after the switch is in it.
void ONScripter::setLanguageFont( const std::string &lang )
{
    if (base_font_file.empty()) base_font_file = font_file;
    std::string want = base_font_file;
    if (!lang.empty()){
        size_t dot = base_font_file.rfind( '.' ), sep = base_font_file.find_last_of( "/\\" );
        std::string own = (dot != std::string::npos && (sep == std::string::npos || dot > sep))
            ? base_font_file.substr( 0, dot ) + "." + lang + base_font_file.substr( dot )
            : base_font_file + "." + lang;
        FILE *fp = ::fopen( own.c_str(), "rb" );
        if (fp){
            fclose( fp );
            want = own;
        }
    }
    if (want == font_file) return;
    delete[] font_file;
    font_file = new char[ want.size() + 1 ];
    strcpy( font_file, want.c_str() );
    sentence_font.ttf_font[0] = sentence_font.ttf_font[1] = NULL; // opened again at the next character
}

void ONScripter::setLanguageLayer( const std::string &old_lang, const std::string &lang )
{
    if (old_lang == lang) return;
    language_layer = lang;
    setLayers();
}

// `assetlayer "name",on`: read files from layer\name\ where it has them (1)
// or not (0); a game's pictures setting (censored or not), say.
int ONScripter::assetlayerCommand()
{
    std::string name = script_h.readStr();
    bool on = script_h.readInt() != 0;
    for (size_t i=0 ; i<asset_layers.size() ; i++)
        if (asset_layers[i] == name){
            asset_layers.erase( asset_layers.begin() + i );
            break;
        }
    if (on) asset_layers.insert( asset_layers.begin(), name );
    setLayers();
    return RET_CONTINUE;
}
