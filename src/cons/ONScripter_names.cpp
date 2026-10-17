/* -*- C++ -*-
 *
 *  ONScripter_names.cpp - CONscripter: named variables and sprite handles
 *
 *  defvar, defstr, defarr give a name its own slot from 10000 up (a
 *  numalias to it, so %name, $name and ?name[i] work wherever %N, $N and
 *  ?N do); saves and gloval.sav keep them by name, so the slots may move
 *  between builds. spnew hands out a sprite number from 1000 up, drawn by
 *  its layer rather than by its number; spfree gives it back.
 *
 *  This file is part of CONscripter, a fork of ONScripter-Yuri (GPLv2 or
 *  later; see COPYING).
 */

#include "ONScripter.h"
#include "Utils.h"
#include <algorithm>

#define CONS_GLOVAL_NAMES 0x4e4e4f43 // "CONN": the named globals after gloval.sav's numbered ones

enum { LAYER_GAME = 0, LAYER_UI = 1, LAYER_OVERLAY = 2, LAYER_SCREEN = 3 }; // screen: ONScripter_screen.cpp's, over the others

// ---------------------------------------------------------------------
// ScriptHandler

ScriptHandler::ArrayVariable *ScriptHandler::addArrayVariable( int no, int size )
{
    ArrayVariable *av = new ArrayVariable();
    if (current_array_variable) current_array_variable->next = av;
    else                        root_array_variable = av;
    current_array_variable = av;
    av->no = no;
    av->num_dim = 1;
    for (int i=0 ; i<20 ; i++) av->dim[i] = 0;
    av->dim[0] = size;
    av->data = new int[size];
    memset( av->data, 0, sizeof(int) * size );
    av->named = true;
    return av;
}

bool ScriptHandler::findNumAlias( const char *name, int *no )
{
    for (Alias *a = root_num_alias.next ; a ; a = a->next)
        if (!strcmp( a->alias, name )){
            *no = a->num;
            return true;
        }
    return false;
}

// ---------------------------------------------------------------------
// ScriptParser: the name table

// The next argument is the bare word w (global, ui, ...): read it.
static bool nextWordIs( ScriptHandler &sh, const char *w )
{
    const char *p = sh.getNext();
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strlen( w );
    if (strncasecmp( p, w, n )) return false;
    char c = p[n];
    if (isalnum( (unsigned char)c ) || c == '_') return false;
    sh.readLabel();
    return true;
}

static int *arrayData( ScriptHandler &sh, int no )
{
    for (ScriptHandler::ArrayVariable *av = sh.getRootArrayVariable() ; av ; av = av->next)
        if (av->no == no) return av->data;
    return NULL;
}

const ScriptParser::NamedVar *ScriptParser::findNamedVar( const char *name )
{
    for (size_t i=0 ; i<named_vars.size() ; i++)
        if (named_vars[i].name == name) return &named_vars[i];
    return NULL;
}

const ScriptParser::NamedVar *ScriptParser::namedVarAt( int no )
{
    if (no < namedVarBase()) return NULL;
    size_t i = no - namedVarBase(); // one slot per name, in order
    return i < named_vars.size() ? &named_vars[i] : NULL;
}

void ScriptParser::setNamedDefault( const NamedVar &v )
{
    ScriptHandler &sh = script_h;
    if (v.kind == '%'){
        sh.getVariableData( v.no ).num = v.def_num;
    }
    else if (v.kind == '$'){
        ScriptHandler::VariableData &vd = sh.getVariableData( v.no );
        delete[] vd.str;
        vd.str = NULL;
        if (!v.def_str.empty()){
            vd.str = new char[v.def_str.size() + 1];
            strcpy( vd.str, v.def_str.c_str() );
        }
    }
    else if (int *data = arrayData( sh, v.no ))
        memset( data, 0, sizeof(int) * v.size );
}

void ScriptParser::applyNamedValue( const NamedVar &v, const NamedValue &val )
{
    ScriptHandler &sh = script_h;
    if (v.kind != val.kind) return;
    if (v.kind == '%'){
        if (!val.nums.empty()) sh.getVariableData( v.no ).num = val.nums[0];
    }
    else if (v.kind == '$'){
        ScriptHandler::VariableData &vd = sh.getVariableData( v.no );
        delete[] vd.str;
        vd.str = NULL;
        if (!val.str.empty()){
            vd.str = new char[val.str.size() + 1];
            strcpy( vd.str, val.str.c_str() );
        }
    }
    else if (int *data = arrayData( sh, v.no ))
        for (int i=0 ; i<v.size && i<(int)val.nums.size() ; i++) data[i] = val.nums[i];
}

/* defvar name[,default][,global]   a number, %name
 * defstr name[,"default"][,global] a string, $name
 * defarr name,size[,global]        size numbers, ?name[0] .. ?name[size-1] */
void ScriptParser::defineNamed( char kind )
{
    const char *cmd = kind == '%' ? "defvar" : kind == '$' ? "defstr" : "defarr";
    std::string name = script_h.readLabel();
    char msg[256];
    if (name.empty() || name[0] == '*' || isdigit( (unsigned char)name[0] )){
        snprintf( msg, sizeof(msg), "%s: a name (letters, digits, _) expected", cmd );
        errorAndExit( msg );
    }
    NamedVar v;
    v.name = name;
    v.kind = kind;
    v.size = 0;
    v.global = false;
    v.def_num = 0;
    if (kind == '?'){
        if (!(script_h.getEndStatus() & ScriptHandler::END_COMMA)){
            snprintf( msg, sizeof(msg), "%s: %s: no size", cmd, name.c_str() );
            errorAndExit( msg );
        }
        v.size = script_h.readInt();
        if (v.size <= 0){
            snprintf( msg, sizeof(msg), "%s: %s: size %d", cmd, name.c_str(), v.size );
            errorAndExit( msg );
        }
    }
    else if (script_h.getEndStatus() & ScriptHandler::END_COMMA){
        if (nextWordIs( script_h, "global" )) v.global = true;
        else if (kind == '%') v.def_num = script_h.readInt();
        else                  v.def_str = script_h.readStr();
    }
    if (!v.global && (script_h.getEndStatus() & ScriptHandler::END_COMMA)){
        if (!nextWordIs( script_h, "global" )){
            snprintf( msg, sizeof(msg), "%s: %s: \"global\" expected", cmd, name.c_str() );
            errorAndExit( msg );
        }
        v.global = true;
    }
    if (script_h.getEndStatus() & ScriptHandler::END_COMMA){
        snprintf( msg, sizeof(msg), "%s: %s: too many arguments", cmd, name.c_str() );
        errorAndExit( msg );
    }

    const NamedVar *old = findNamedVar( name.c_str() );
    if (old){ // declared again (the define section run again, a label run twice): kept as it is
        if (old->kind != kind || old->size != v.size || old->global != v.global){
            snprintf( msg, sizeof(msg), "%s: %s declared before as another kind", cmd, name.c_str() );
            errorAndExit( msg );
        }
        return;
    }
    int no;
    if (script_h.findNumAlias( name.c_str(), &no )){
        snprintf( msg, sizeof(msg), "%s: %s is a numalias already", cmd, name.c_str() );
        errorAndExit( msg );
    }
    v.no = namedVarBase() + (int)named_vars.size();
    named_vars.push_back( v );
    script_h.addNumAlias( name.c_str(), v.no );
    if (kind == '?') script_h.addArrayVariable( v.no, v.size );
    setNamedDefault( v );
    if (v.global) // gloval.sav's value, read before the define section ran
        for (size_t i=0 ; i<pending_globals.size() ; i++)
            if (pending_globals[i].name == name){
                applyNamedValue( v, pending_globals[i] );
                pending_globals.erase( pending_globals.begin() + i );
                break;
            }
}

int ScriptParser::defvarCommand(){ defineNamed( '%' ); return RET_CONTINUE; }
int ScriptParser::defstrCommand(){ defineNamed( '$' ); return RET_CONTINUE; }
int ScriptParser::defarrCommand(){ defineNamed( '?' ); return RET_CONTINUE; }

void ScriptParser::clearNamedVars()
{
    named_vars.clear(); // their aliases and arrays go with script_h.reset()
    pending_globals.clear();
}

void ScriptParser::namedEntries( std::vector<NamedEntry> &out )
{
    for (size_t i=0 ; i<named_vars.size() ; i++){
        const NamedVar &v = named_vars[i];
        NamedEntry e;
        e.key = std::string( 1, v.kind ) + v.name;
        e.num = 0;
        e.str = NULL;
        if (v.kind == '%'){
            e.num = script_h.getVariableData( v.no ).num;
            if (e.num) out.push_back( e );
        }
        else if (v.kind == '$'){
            e.str = script_h.getVariableData( v.no ).str;
            if (e.str && e.str[0]) out.push_back( e );
        }
        else if (int *data = arrayData( script_h, v.no ))
            for (int k=0 ; k<v.size ; k++)
                if (data[k]){
                    e.key = "?" + v.name + "[" + std::to_string( k ) + "]";
                    e.num = data[k];
                    out.push_back( e );
                }
    }
}

/* "%name", "$name" or "?name[i]" (the leading character may be left out):
 * where its number is (% and ?; for $ the slot's num), NULL when no such
 * name; kind gets '%', '$' or '?', no the slot. */
int *ScriptParser::namedSlot( const char *key, char *kind, int *no )
{
    char k = (*key == '%' || *key == '$' || *key == '?') ? *key++ : 0;
    std::string name;
    while (isalnum( (unsigned char)*key ) || *key == '_') name += (char)tolower( (unsigned char)*key++ );
    const NamedVar *v = findNamedVar( name.c_str() );
    if (!v || (k && k != v->kind)) return NULL;
    *kind = v->kind;
    *no = v->no;
    if (v->kind != '?') return &script_h.getVariableData( v->no ).num;
    if (*key != '[') return NULL;
    int i = atoi( key+1 );
    int *data = arrayData( script_h, v->no );
    return (data && i >= 0 && i < v->size) ? data + i : NULL;
}

// "%12" or, for a name's slot, "%name"
std::string ScriptParser::varKey( char kind, int no )
{
    const NamedVar *v = namedVarAt( no );
    return std::string( 1, kind ) + (v ? v->name : std::to_string( no ));
}

void ScriptParser::resetNamedVars( bool global )
{
    for (size_t i=0 ; i<named_vars.size() ; i++)
        if (named_vars[i].global == global) setNamedDefault( named_vars[i] );
}

/* Count, then per name: the name, its kind ('%', '$', '?') and value
 * (a number; a string; the element count and the numbers). */
void ScriptParser::writeNamedVars( bool global, bool output_flag )
{
    int num = 0;
    for (size_t i=0 ; i<named_vars.size() ; i++)
        if (named_vars[i].global == global) num++;
    writeInt( num, output_flag );
    for (size_t i=0 ; i<named_vars.size() ; i++){
        const NamedVar &v = named_vars[i];
        if (v.global != global) continue;
        writeStr( (char*)v.name.c_str(), output_flag );
        writeInt( v.kind, output_flag );
        if (v.kind == '%')
            writeInt( script_h.getVariableData( v.no ).num, output_flag );
        else if (v.kind == '$')
            writeStr( script_h.getVariableData( v.no ).str, output_flag );
        else{
            int *data = arrayData( script_h, v.no );
            writeInt( v.size, output_flag );
            for (int k=0 ; k<v.size ; k++) writeInt( data ? data[k] : 0, output_flag );
        }
    }
}

// pending: gloval.sav, read before the define section declares the names
void ScriptParser::readNamedVars( bool global, bool pending )
{
    int num = readInt();
    for (int n=0 ; n<num ; n++){
        NamedValue val;
        char *s = NULL;
        readStr( &s );
        val.name = s ? s : "";
        delete[] s;
        val.kind = (char)readInt();
        if (val.kind == '%') val.nums.push_back( readInt() );
        else if (val.kind == '$'){
            s = NULL;
            readStr( &s );
            val.str = s ? s : "";
            delete[] s;
        }
        else if (val.kind == '?'){
            int size = readInt();
            if (size < 0 || file_io_buf_ptr + (size_t)size * 4 > file_io_buf_len) return; // damaged
            for (int k=0 ; k<size ; k++) val.nums.push_back( readInt() );
        }
        else return; // damaged
        if (pending){
            pending_globals.push_back( val );
            continue;
        }
        const NamedVar *v = findNamedVar( val.name.c_str() );
        if (v && v->global == global) applyNamedValue( *v, val );
        // a name this build doesn't declare is left out
    }
}

void ScriptParser::writeGlovalNames( bool output_flag )
{
    writeInt( CONS_GLOVAL_NAMES, output_flag );
    writeNamedVars( true, output_flag );
}

void ScriptParser::readGlovalNames()
{
    if (file_io_buf_ptr + 4 > file_io_buf_len || readInt() != CONS_GLOVAL_NAMES) return;
    readNamedVars( true, true );
}

// ---------------------------------------------------------------------
// ONScripter: sprite handles

void ONScripter::resetSpriteHandles()
{
    for (int i=0 ; i<SPRITE_HANDLE_NUM ; i++){
        sprite_handle_layer[i] = 0;
        sprite_handle_var[i] = -1;
    }
}

// The lowest free handle (held by no spnew, no picture in it), -1 if none.
int ONScripter::newSpriteHandle( int layer )
{
    for (int i=0 ; i<SPRITE_HANDLE_NUM ; i++)
        if (sprite_handle_layer[i] == 0 && !sprite_info[NS_SPRITE_NUM + i].image_name){
            sprite_handle_layer[i] = layer + 1;
            sprite_handle_var[i] = -1;
            return NS_SPRITE_NUM + i;
        }
    return -1;
}

/* Where a handle is drawn: right over a numbered sprite (as spz). The
 * numbered order is NScripter's, lower numbers on top: sprites
 * 999 .. z_order+1, the standing pictures, (windowback: lsp2, the text
 * window) z_order .. 0, (no windowback: lsp2, the text window).
 *   game    over sprite z_order+1: the top of the pictures under the text
 *           window, under the standing pictures and the text window
 *   ui      over sprite z_order: with windowback over the text window,
 *           under sprites z_order-1 .. 0
 *   overlay over sprite 0: over every numbered sprite
 * Among handles in one layer the lower number is on top; spnew gives the
 * lowest free one. A handle that spnew doesn't hold (lsp 1500 without
 * spnew) is in the game layer. z_order is humanz (499 by default). */
int ONScripter::spriteHandleOver( int no )
{
    int layer = sprite_handle_layer[no - NS_SPRITE_NUM] - 1;
    if (layer == LAYER_OVERLAY || layer == LAYER_SCREEN) return 0;
    if (layer == LAYER_UI) return z_order;
    return z_order + 1 < NS_SPRITE_NUM ? z_order + 1 : NS_SPRITE_NUM - 1;
}

static int readLayer( ScriptHandler &sh )
{
    if (!(sh.getEndStatus() & ScriptHandler::END_COMMA)) return LAYER_GAME;
    if (nextWordIs( sh, "game" )) return LAYER_GAME;
    if (nextWordIs( sh, "ui" )) return LAYER_UI;
    if (nextWordIs( sh, "overlay" )) return LAYER_OVERLAY;
    int l = sh.readInt();
    return l < LAYER_GAME || l > LAYER_OVERLAY ? LAYER_GAME : l;
}

/* spnew %var[,layer]: a free sprite number (1000 up) into %var, drawn in
 * layer game (default), ui or overlay (or 0, 1, 2). The variable names the
 * handle: reset and load leave it holding its handle, saves keep the
 * handle under the variable's name. spnew on a variable that holds a
 * handle already keeps it (its layer as given). */
int ONScripter::spnewCommand()
{
    script_h.readVariable();
    if (script_h.current_variable.type != ScriptHandler::VAR_INT)
        errorAndExit( "spnew: a % variable expected" );
    int var = script_h.current_variable.var_no;
    int layer = readLayer( script_h );

    int no = script_h.getVariableData( var ).num;
    if (no >= NS_SPRITE_NUM && no < MAX_SPRITE_NUM &&
        sprite_handle_layer[no - NS_SPRITE_NUM] && sprite_handle_var[no - NS_SPRITE_NUM] == var){
        sprite_handle_layer[no - NS_SPRITE_NUM] = layer + 1;
    }
    else{
        no = newSpriteHandle( layer );
        if (no < 0) errorAndExit( "spnew: no free sprite handle" );
        sprite_handle_var[no - NS_SPRITE_NUM] = var;
        script_h.setNumVariable( var, no );
    }
    dirty_rect.add( drawnRect( sprite_info[no] ) );
    return RET_CONTINUE;
}

// spfree num: removes the sprite (as csp) and gives its handle back.
int ONScripter::spfreeCommand()
{
    int no = script_h.readInt();
    if (no < NS_SPRITE_NUM || no >= MAX_SPRITE_NUM) return RET_CONTINUE;
    AnimationInfo &si = sprite_info[no];
    if (si.visible) dirty_rect.add( drawnRect( si ) );
    root_button_link.removeSprite( no );
    si.blending_mode = AnimationInfo::BLEND_NORMAL;
    si.remove();
    sprite_handle_layer[no - NS_SPRITE_NUM] = 0;
    sprite_handle_var[no - NS_SPRITE_NUM] = -1;
    return RET_CONTINUE;
}

// After the named locals were reset: the handles' variables hold them again.
static void rebindHandleVars( ScriptHandler &sh, int *layer, int *var )
{
    for (int i=0 ; i<SPRITE_HANDLE_NUM ; i++)
        if (layer[i] && var[i] >= 0) sh.setNumVariable( var[i], NS_SPRITE_NUM + i );
}

// A load or reset: the handles' pictures go (the numbered ones are read
// over), the named locals to their defaults; handles stay held.
void ONScripter::clearHandleSprites()
{
    for (int i=NS_SPRITE_NUM ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo &si = sprite_info[i];
        if (!si.image_name && !si.image_surface) continue;
        root_button_link.removeSprite( i );
        si.blending_mode = AnimationInfo::BLEND_NORMAL;
        si.remove();
    }
    resetNamedVars( false );
    rebindHandleVars( script_h, sprite_handle_layer, sprite_handle_var );
}

/* Part of the CONscripter save block (version 26): the named locals
 * (writeNamedVars); the held handles: count, then each one's number, layer
 * and the name of the variable holding it ("%n" unnamed, "" none); the
 * handles' pictures as the numbered ones' (count, then number, image,
 * x, y, visible, cell, alpha). */
void ONScripter::writeNamesSection( bool output_flag )
{
    writeNamedVars( false, output_flag );

    int num = 0;
    for (int i=0 ; i<SPRITE_HANDLE_NUM ; i++) if (sprite_handle_layer[i]) num++;
    writeInt( num, output_flag );
    for (int i=0 ; i<SPRITE_HANDLE_NUM ; i++){
        if (!sprite_handle_layer[i]) continue;
        writeInt( NS_SPRITE_NUM + i, output_flag );
        writeInt( sprite_handle_layer[i] - 1, output_flag );
        std::string name;
        int var = sprite_handle_var[i];
        const NamedVar *v = namedVarAt( var );
        if (v) name = v->name;
        else if (var >= 0) name = "%" + std::to_string( var );
        writeStr( (char*)name.c_str(), output_flag );
    }

    num = 0;
    for (int i=NS_SPRITE_NUM ; i<MAX_SPRITE_NUM ; i++) if (sprite_info[i].image_name) num++;
    writeInt( num, output_flag );
    for (int i=NS_SPRITE_NUM ; i<MAX_SPRITE_NUM ; i++){
        AnimationInfo *ai = &sprite_info[i];
        if (!ai->image_name) continue;
        writeInt( i, output_flag );
        writeStr( ai->image_name, output_flag );
        writeInt( ai->orig_pos.x, output_flag );
        writeInt( ai->orig_pos.y, output_flag );
        writeInt( ai->visible && !(ai->motion_keys > 0 && ai->motion_leave) ? 1 : 0, output_flag );
        writeInt( ai->current_cell, output_flag );
        writeInt( ai->trans, output_flag );
    }
}

/* The handles are the save's: its variables hold the numbers they held
 * when saved, so its sprites' data (read by number) is theirs. A handle
 * this build holds for a variable the save doesn't know keeps its number
 * when free, else gets a new one. */
void ONScripter::readNamesSection()
{
    readNamedVars( false, false );

    int cur_layer[SPRITE_HANDLE_NUM], cur_var[SPRITE_HANDLE_NUM];
    memcpy( cur_layer, sprite_handle_layer, sizeof(cur_layer) );
    memcpy( cur_var, sprite_handle_var, sizeof(cur_var) );
    resetSpriteHandles();
    std::vector<int> rebound;
    int num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt(), layer = readInt();
        char *s = NULL;
        readStr( &s );
        int var = -1;
        if (s && s[0] == '%') var = atoi( s+1 );
        else if (s && s[0]){
            const NamedVar *v = findNamedVar( s );
            if (v && v->kind == '%') var = v->no;
        }
        delete[] s;
        if (no < NS_SPRITE_NUM || no >= MAX_SPRITE_NUM) continue;
        sprite_handle_layer[no - NS_SPRITE_NUM] = (layer < LAYER_GAME || layer > LAYER_OVERLAY ? LAYER_GAME : layer) + 1;
        if (var < 0) continue;
        sprite_handle_var[no - NS_SPRITE_NUM] = var;
        script_h.setNumVariable( var, no );
        rebound.push_back( var );
    }
    for (int i=0 ; i<SPRITE_HANDLE_NUM ; i++){
        if (!cur_layer[i] || cur_var[i] < 0) continue;
        if (std::find( rebound.begin(), rebound.end(), cur_var[i] ) != rebound.end()) continue;
        int no = NS_SPRITE_NUM + i;
        if (sprite_handle_layer[i] == 0) sprite_handle_layer[i] = cur_layer[i];
        else if ((no = newSpriteHandle( cur_layer[i] - 1 )) < 0) continue;
        sprite_handle_var[no - NS_SPRITE_NUM] = cur_var[i];
        script_h.setNumVariable( cur_var[i], no );
    }

    num = readInt();
    for (int n=0 ; n<num ; n++){
        int no = readInt();
        AnimationInfo dummy, *ai = (no >= NS_SPRITE_NUM && no < MAX_SPRITE_NUM) ? &sprite_info[no] : &dummy;
        ai->remove();
        readStr( &ai->image_name );
        if (ai->image_name && ai != &dummy){
            parseTaggedString( ai );
            setupAnimationInfo( ai );
        }
        ai->orig_pos.x = readInt();
        ai->orig_pos.y = readInt();
        ai->scalePosXY( screen_ratio1, screen_ratio2 );
        ai->visible = readInt() == 1;
        ai->current_cell = readInt();
        ai->trans = readInt();
    }
}
