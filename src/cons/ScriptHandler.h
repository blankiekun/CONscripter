/* -*- C++ -*-
 * 
 *  ScriptHandler.h - Script manipulation class
 *
 *  Copyright (c) 2001-2018 Ogapee. All rights reserved.
 *            (c) 2016-2018 jh10001 <jh10001@live.cn>
 *            (c) 2022-2023 yurisizuku <https://github.com/YuriSizuku>
 *
 *  ogapee@aqua.dti2.ne.jp
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

#ifndef __SCRIPT_HANDLER_H__
#define __SCRIPT_HANDLER_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "BaseReader.h"
#include "coding2utf16.h"

// CONscripter: in Shift_JIS, 0xa0-0xdf are half-width katakana (cons_sjis)
extern bool cons_sjis;
extern Coding2UTF16 *coding2utf16;
#define IS_TWO_BYTE(x) \
        ( ((unsigned char)(x) > (unsigned char)0x80) && ((unsigned char)(x) !=(unsigned char) 0xff) && \
          !(cons_sjis && (unsigned char)(x) >= (unsigned char)0xa0 && (unsigned char)(x) <= (unsigned char)0xdf) )

#define UTF8_N_BYTE(x) ( \
    (((unsigned char)(x) & 0x80) == 0x00) ? 1 : ( \
    (((unsigned char)(x) & 0xE0) == 0xC0) ? 2 : ( \
    (((unsigned char)(x) & 0xF0) == 0xE0) ? 3 : ( \
    (((unsigned char)(x) & 0xF8) == 0xF0) ? 4 : 0))))

// CONscripter: the bytes of the character at p in the script's encoding
// (1 for a stray byte).
inline int charBytes( const char *p )
{
    if (coding2utf16->force_utf8){
        int n = UTF8_N_BYTE(*p);
        for (int i=1 ; i<n ; i++) if (((unsigned char)p[i] & 0xc0) != 0x80) return i; // cut short
        return n ? n : 1;
    }
    return IS_TWO_BYTE(*p) && p[1] && p[1] != 0x0a ? 2 : 1;
}

// CONscripter: the half-width cells the character at p takes, as its
// Shift_JIS bytes would: 1 for ASCII and half-width katakana, else 2; in
// UTF-8 also 1 for the Latin letters Shift_JIS lacks (U+00C0-024F but × ÷).
inline int charCells( const char *p )
{
    const unsigned char *u = (const unsigned char*)p;
    if (!coding2utf16->force_utf8) return IS_TWO_BYTE(*p) ? 2 : 1;
    if (u[0] < 0x80) return 1;
    if (u[0] == 0xef && ((u[1] == 0xbd && u[2] >= 0xa1) || (u[1] == 0xbe && u[2] <= 0x9f))) return 1; // U+FF61-FF9F
    if (u[0] == 0xc3) return (u[1] == 0x97 || u[1] == 0xb7) ? 2 : 1;
    return (u[0] >= 0xc4 && u[0] <= 0xc9) ? 1 : 2;
}

typedef unsigned char uchar3[3];

class ScriptHandler
{
public:
    enum { END_NONE       = 0,
           END_COMMA      = 1,
           END_1BYTE_CHAR = 2,
           END_COMMA_READ = 4, // for LUA
           END_NO_NEW_LINE = 8 // CONscripter: a 1-byte string ended "`/"
    };
    struct LabelInfo{
        char *name;
        char *label_header;
        char *start_address;
        int  start_line;
        int  num_of_lines;
    };

    struct ArrayVariable{
        struct ArrayVariable* next;
        int no;
        int num_dim;
        int dim[20];
        int *data;
        bool named; // CONscripter: defarr's; saved by name, not in the numbered list
        ArrayVariable(){
            next = NULL;
            data = NULL;
            named = false;
        };
        ~ArrayVariable(){
            if (data) delete[] data;
        };
        ArrayVariable& operator=(const ArrayVariable& av){
            no = av.no;
            num_dim = av.num_dim;

            int total_dim = 1;
            for (int i=0 ; i<20 ; i++){
                dim[i] = av.dim[i];
                total_dim *= dim[i];
            }

            if (data) delete[] data;
            data = NULL;
            if (av.data){
                data = new int[total_dim];
                memcpy(data, av.data, sizeof(int)*total_dim);
            }

            return *this;
        };
    };

    enum { VAR_NONE  = 0,
           VAR_INT   = 1,  // integer
           VAR_ARRAY = 2,  // array
           VAR_STR   = 4,  // string
           VAR_CONST = 8,  // direct value or alias, not variable
           VAR_PTR   = 16  // pointer to a variable, e.g. i%0, s%0
    };
    struct VariableInfo{
        int type;
        int var_no;   // for integer(%), array(?), string($) variable
        ArrayVariable array; // for array(?)
    };

    ScriptHandler();
    ~ScriptHandler();

    void reset();
    void setSaveDir(const char *path);
    FILE *fopen( const char *path, const char *mode, bool use_save_dir=false );
    std::string fullPath( const char *path, bool use_save_dir=false );
    // CONscripter: a file is written as path.tmp and renamed over path only
    // once it was written whole, so a failed write never destroys the old one
    static FILE *openReplacement( const std::string &path );
    static bool commitReplacement( FILE *fp, const std::string &path, bool written=true );
    void setKeyTable( const unsigned char *key_table );

    // basic parser function
    const char *readToken();
    const char *readLabel();
    const char *readStr();
    int  readInt();
    void skipToken();
    int  parseInt( char **buf );
    int  parseIntExpression( char **buf );
    void readVariable( bool reread_flag=false );

    // function for string access
    inline char *getStringBuffer(){ return string_buffer; };
    char *saveStringBuffer();
    void addStringBuffer( char ch );
    
    // function for direct manipulation of script address 
    inline char *getCurrent(bool use_script=false){ return (use_script && !is_internal_script)?last_script_context->current_script:current_script; };
    inline char *getNext(){ return next_script; };
    inline char *getWait(){ return wait_script?wait_script:next_script; };
    char *getWaitAt( int offset, bool *in_1byte ); // CONscripter: the wait ending at that string buffer offset, NULL if none
    void markWait( char *at, bool in_1byte );
    char *backquote_resume = NULL; // CONscripter: text read from here on is a ` string's rest
    void setCurrent(char *pos);
    void pushCurrent( char *pos );
    void popCurrent();

    void enterExternalScript(char *pos); // LUA
    void leaveExternalScript();
    bool isExternalScript();

    int  getOffset( char *pos );
    char *getAddress( int offset );
    int  getLineByAddress( char *address );
    char *getAddressByLine( int line );
    LabelInfo getLabelByAddress( char *address );
    LabelInfo getLabelByLine( int line );

    bool isName( const char *name );
    bool isText();
    bool compareString( const char *buf );
    void setEndStatus(int val){ end_status |= val; };
    inline int getEndStatus(){ return end_status; };
    void skipLine( int no=1 );
    void setLinepage( bool val );
    void setEnglishMode( bool val ){ english_mode = val; };

    // function for kidoku history
    bool isKidoku();
    void markAsKidoku( char *address=NULL );
    void setKidokuskip( bool kidokuskip_flag );
    void saveKidokuData();
    void loadKidokuData();

    void addStrVariable(char **buf);
    void addIntVariable(char **buf);
    void declareDim();

    void setClickstr( const char *list );
    int  checkClickstr(const char *buf, bool recursive_flag=false);

    void setInt( VariableInfo *var_info, int val, int offset=0 );
    void setNumVariable( int no, int val );
    // CONscripter (ONScripter_names.cpp): defarr's array, and a numalias by name
    ArrayVariable *addArrayVariable( int no, int size );
    bool findNumAlias( const char *name, int *no );
    void pushVariable();
    int  getIntVariable( VariableInfo *var_info=NULL );

    int  getStringFromInteger( char *buffer, int no, int num_column, bool is_zero_inserted=false );

    int  openScript( char *path );

    LabelInfo lookupLabel( const char* label );
    LabelInfo lookupLabelNext( const char* label );
    void errorAndExit( const char *str );
    // CONscripter: called with the message before a script error exits
    // (the debug trace records it)
    static void (*error_hook)( const char *message );
    // CONscripter: the label an address is in, by binary search (-1 outside the script)
    int getLabelIndex( const char *address );
    const char *getLabelName( int index ){ return label_info[index].name; };
    int getNumLabels(){ return num_of_labels; };
    // CONscripter: a label's index by name, any case (the first of a duplicated name), -1 if none
    int findLabelIndex( const char *name );
    bool inScript( const char *address ){ return script_buffer && address >= script_buffer && address < script_buffer + script_buffer_length; };
    // CONscripter: for the web page's consPosition
    int getScriptLength(){ return script_buffer_length; };
    int getOffset( const char *address ){ return inScript( address ) ? (int)(address - script_buffer) : -1; };
    int getKidokuState(){ return kidokuskip_flag ? (skip_enabled ? 1 : 0) : -1; }; // the text read before, -1 unknown
    // CONscripter (ONScripter_savepoint.cpp): kidoku.dat by label, line in it and the line's hash
    LabelInfo getLabelInfo( int index ){ return label_info[index]; };
    void writeKidokuCons( FILE *fp );
    void readKidokuCons( FILE *fp );
    // CONscripter (ONScripter_strtab.cpp): text tables. A line `^id` of
    // 0.txt, and a string "^id" in a command, take the text of id in
    // text.<language>.txt; lineHash is then the 0.txt line's, so saves and
    // read marks keep their places in any language.
    bool hasTextRefs(){ return text_refs; }
    const std::string &getTextTable(){ return text_table; }
    bool textTableExists( const char *lang );
    unsigned int lineHash( const char *line );
    unsigned int textHash( const char *line ); // of the line as it is
    bool isTableLine( const char *line ){ return inScript( line ) && table_line_hash.count( (int)(line - script_buffer) ) > 0; }
    // Reads 0.txt again with the table of lang; each pointer into the old
    // script is moved to the same place in the new one.
    void reloadScript( const char *lang, std::vector<char**> &pointers );
    void scriptPointers( std::vector<char**> &pointers );
    // ONScripter_savepoint.cpp: a script place that survives a rebuild
    struct Place{ int label, line, offset, waits; unsigned int hash; };
    bool placeOf( char *address, Place &place );
    char *addressOf( const Place &place );

    ArrayVariable *getRootArrayVariable();
    void loadArrayVariable( FILE *fp );
    
    void addNumAlias( const char *str, int no );
    void addStrAlias( const char *str1, const char *str2 );

    enum { LABEL_LOG = 0,
           FILE_LOG = 1
    };
    struct LogLink{
        LogLink *next;
        char *name;

        LogLink(){
            next = NULL;
            name = NULL;
        };
        ~LogLink(){
            if ( name ) delete[] name;
        };
    };
    struct LogInfo{
        LogLink root_log;
        LogLink *current_log;
        int num_logs;
        const char *filename;
        std::unordered_map<std::string, LogLink*> index; // CONscripter: name -> its link, for the lookup
    } log_info[2];
    LogLink *findAndAddLog( LogInfo &info, const char *name, bool add_flag );
    void resetLog( LogInfo &info );
    
    /* ---------------------------------------- */
    /* Variable */
    struct VariableData{
        int num;
        bool num_limit_flag;
        int num_limit_upper;
        int num_limit_lower;
        char *str;

        VariableData(){
            str = NULL;
            reset(true);
        };
        void reset(bool limit_reset_flag){
            num = 0;
            if (limit_reset_flag)
                num_limit_flag = false;
            if (str){
                delete[] str;
                str = NULL;
            }
        };
    };
    VariableData &getVariableData(int no);
    
    VariableInfo current_variable, pushed_variable;
    
    int screen_width;
    int screen_height;
    int variable_range;
    int global_variable_border;

    BaseReader *cBR;
    
private:
    enum { OP_INVALID = 0, // 000
           OP_PLUS    = 2, // 010
           OP_MINUS   = 3, // 011
           OP_MULT    = 4, // 100
           OP_DIV     = 5, // 101
           OP_MOD     = 6  // 110
    };
    
    struct Alias{
        struct Alias *next;
        char *alias;
        int  num;
        char *str;

        Alias(){
            next = NULL;
            alias = NULL;
            str = NULL;
        };
        Alias( const char *name, int num ){
            next = NULL;
            alias = new char[ strlen(name) + 1];
            strcpy( alias, name );
            str = NULL;
            this->num = num;
        };
        Alias( const char *name, const char *str ){
            next = NULL;
            alias = new char[ strlen(name) + 1];
            strcpy( alias, name );
            this->str = new char[ strlen(str) + 1];
            strcpy( this->str, str );
        };
        ~Alias(){
            if (alias) delete[] alias;
            if (str)   delete[] str;
        };
    };
    
    struct ScriptContext{
        ScriptContext *prev, *next;
        char *current_script;
        char *next_script;
        int end_status;
        VariableInfo current_variable, pushed_variable;
        ScriptContext(){
            prev = next = NULL;
            current_script = next_script = NULL;
        }
    };

    int  readScript(char *path);
    int  readScriptSub(FILE *fp, char **buf, int encrypt_mode);
    void readConfiguration();
    int  labelScript();

    int findLabel( const char* label );

    char *checkComma( char *buf );
    void parseStr( char **buf );
    void readNextOp( char **buf, int *op, int *num );
    int  calcArithmetic( int num1, int op, int num2 );
    int  parseArray( char **buf, ArrayVariable &array );
    int  *getArrayPtr( int no, ArrayVariable &array, int offset );

    /* ---------------------------------------- */
    /* Variable */
    struct VariableData *variable_data;
    struct ExtendedVariableData{
        int no;
        VariableData vd;
    } *extended_variable_data;
    int num_extended_variable_data;
    int max_extended_variable_data;

    Alias root_num_alias, *last_num_alias;
    Alias root_str_alias, *last_str_alias;
    
    ArrayVariable *root_array_variable, *current_array_variable;

    char *archive_path;
    char *save_dir;
    int  script_buffer_length;
    char *script_buffer;
    unsigned char *tmp_script_buf;
    
    char *string_buffer; // update only be readToken
    int  string_counter;
    char *saved_string_buffer; // updated only by saveStringBuffer
    char *str_string_buffer; // updated only by readStr

    LabelInfo *label_info;
    int num_of_labels;
    std::unordered_map<std::string, int> label_index; // CONscripter: lowercased name -> index
    // CONscripter: text tables (ONScripter_strtab.cpp)
    bool text_refs = false;
    std::string text_table;
    std::unordered_map<int, unsigned int> table_line_hash; // offset of a line a table filled -> its 0.txt line's hash
    void expandTextTable( const char *lang );

    bool skip_enabled;
    bool kidokuskip_flag;
    char *kidoku_buffer;

    bool text_flag; // true if the current token is text
    int  end_status;
    bool linepage_flag;
    char *clickstr_list;
    bool english_mode;

    char *current_script;
    char *next_script;
    char *wait_script; // address where '@' or '//' appears
    // CONscripter: each '@' and '\\' of the text token: the string buffer's
    // length after it, and the script address of the wait itself (rgosub)
    enum { MAX_WAIT_MARKS = 64 };
    int wait_mark_offset[MAX_WAIT_MARKS];
    char *wait_mark_script[MAX_WAIT_MARKS];
    bool wait_mark_1byte[MAX_WAIT_MARKS]; // in a ` string
    int wait_marks;

    char *pushed_current_script;
    char *pushed_next_script;

    bool is_internal_script;
    ScriptContext root_script_context, *last_script_context;

    unsigned char key_table[256];
    bool key_table_flag;
};

#endif // __SCRIPT_HANDLER_H__
