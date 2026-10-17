/* -*- C++ -*-
 *
 *  ONScripter_savepoint.cpp - CONscripter: saves and kidoku.dat keep their
 *  places in the script by label, line in the label and the line's hash,
 *  so they survive a rebuild of 0.txt that keeps the label and the line
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
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

void traceString( std::string &out, const char *text, int len=-1, bool json=true );

// A line is one physical line of the script as read (CRs dropped): a line
// of `:`-joined commands is one line, and so is each line of a text or of a
// command continued after a comma. Line 0 of a label is its `*label` line.

static unsigned int lineHash( const char *p )
{
    unsigned int h = 2166136261u; // FNV-1a 32
    for ( ; *p != 0x0a ; p++ ){
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    return h;
}

static char *labelEnd( ScriptHandler &sh, int i )
{
    return (i+1 < sh.getNumLabels()) ? sh.getLabelInfo( i+1 ).label_header
                                     : sh.getAddress( sh.getScriptLength() );
}

struct LabelLines{
    std::vector<char*> lines; // where each line of the label starts
    std::unordered_map<unsigned int, std::vector<int> > by_hash; // filled when first needed
};

static void labelLines( ScriptHandler &sh, int i, LabelLines &ll )
{
    char *end = labelEnd( sh, i );
    for (char *p = sh.getLabelInfo( i ).label_header ; p < end ; p++){
        ll.lines.push_back( p );
        while (*p != 0x0a) p++;
    }
}

// The line with that hash: the one at `line` if it still has it, else the
// nearest one that has; -1 if none.
static int nearestLine( LabelLines &ll, int line, unsigned int hash )
{
    int n = ll.lines.size();
    if (line >= 0 && line < n && lineHash( ll.lines[line] ) == hash) return line;
    if (ll.by_hash.empty())
        for (int k=0 ; k<n ; k++) ll.by_hash[lineHash( ll.lines[k] )].push_back( k );
    std::unordered_map<unsigned int, std::vector<int> >::iterator it = ll.by_hash.find( hash );
    if (it == ll.by_hash.end()) return -1;
    int best = -1;
    for (size_t j=0 ; j<it->second.size() ; j++){
        int k = it->second[j];
        if (best < 0 || abs( k - line ) < abs( best - line )) best = k;
    }
    return best;
}

static int labelByName( ScriptHandler &sh, const char *name )
{
    for (int i=0 ; i<sh.getNumLabels() ; i++)
        if (!strcmp( sh.getLabelName( i ), name )) return i;
    return -1;
}

// The stock place (the line in the whole script and the colons before the
// command), kept within the script for a save from another build;
// loadSavePoints then puts it where the label and line are now.
void ONScripter::loadScriptPosition()
{
    int i = readInt();
    int j = readInt();
    current_label_info = script_h.getLabelByLine( i );
    current_line = i - current_label_info.start_line;
    if (current_line < 0 || current_line >= current_label_info.num_of_lines) current_line = 0;
    char *line = script_h.getAddressByLine( current_label_info.start_line + current_line );
    char *end = script_h.getAddress( script_h.getScriptLength() );
    char *buf = line;
    for ( ; j>0 && buf ; j--){
        while (buf < end && *buf != ':') buf++;
        buf = (buf < end) ? buf+1 : NULL;
    }
    script_h.setCurrent( buf ? buf : line );
}

// A place as the label's name, the line in the label, the byte in the line
// and the line's hash; "" outside the script.
void ONScripter::writeSavePoint( char *address, bool output_flag )
{
    int i = script_h.getLabelIndex( address );
    if (i < 0){
        writeStr( NULL, output_flag );
        return;
    }
    ScriptHandler::LabelInfo label = script_h.getLabelInfo( i );
    char *line = label.label_header;
    int n = 0;
    for (char *p = line ; p < address ; p++)
        if (*p == 0x0a){
            line = p+1;
            n++;
        }
    writeStr( label.name, output_flag );
    writeInt( n, output_flag );
    writeInt( address - line, output_flag );
    writeInt( (int)lineHash( line ), output_flag );
}

// Where a place written by writeSavePoint is in this script: its line, found
// by its hash (nearest to where it was first). A line no longer there falls
// back to the label's line at that number (its start; with `after`, a return
// place, the next line's), then to the label's start, with one error line
// and a trace warning. NULL keeps the stock place: none was written, or the
// label is gone.
char *ONScripter::readSavePoint( bool after, const char *what, int *label, int *line )
{
    char *name = NULL;
    readStr( &name );
    if (!name) return NULL;
    int n = readInt();
    int offset = readInt();
    unsigned int hash = (unsigned int)readInt();

    const char *to = NULL;
    char *address = NULL;
    int i = labelByName( script_h, name );
    int k = 0;
    if (i < 0) to = "the place by line number in the script";
    else{
        LabelLines ll;
        labelLines( script_h, i, ll );
        k = nearestLine( ll, n, hash );
        if (k >= 0){
            int len = 0;
            while (ll.lines[k][len] != 0x0a) len++;
            address = ll.lines[k] + (offset >= 0 && offset <= len ? offset : 0);
        }
        else if (n >= 0 && n < (int)ll.lines.size()){
            k = n;
            if (after && k+1 < (int)ll.lines.size()) k++;
            address = ll.lines[k];
            to = "the line by number in the label";
        }
        else{
            k = 0;
            address = ll.lines[0];
            to = "the label's start";
        }
    }
    if (to){
        utils::printError( "load: %s, line %d of *%s, isn't in this script; resumed at %s\n", what, n, name, to );
        if (trace_fp){
            char buf[96];
            snprintf( buf, sizeof(buf), "{\"t\":\"warn\",\"pg\":%d,\"what\":\"save place lost\",\"place\":\"%s\",\"label\":", trace_page, what );
            std::string r = buf;
            traceString( r, name );
            snprintf( buf, sizeof(buf), ",\"line\":%d,\"at\":", n );
            r += buf;
            traceAt( r, address ? address : script_h.getCurrent( true ) );
            r += '}';
            traceWrite( r );
        }
    }
    delete[] name;
    if (label) *label = i;
    if (line) *line = k;
    return address;
}

// Version 25 of the save data: where the script resumes (as the stock save
// does: the line's start in a text, else after the last `:` before the
// command), then the count and each gosub and for return place, in nest order.
void ONScripter::saveSavePoints( bool output_flag )
{
    char *line = script_h.getAddressByLine( current_label_info.start_line + current_line );
    char *address = line;
    if (!script_h.isText())
        for (char *p = line ; p < script_h.getCurrent( true ) ; p++)
            if (*p == ':') address = p+1;
    writeSavePoint( address, output_flag );

    int num = 0;
    for (NestInfo *info = root_nest_info.next ; info ; info = info->next) num++;
    writeInt( num, output_flag );
    for (NestInfo *info = root_nest_info.next ; info ; info = info->next)
        writeSavePoint( info->next_script, output_flag );
}

void ONScripter::loadSavePoints()
{
    int label, line;
    char *address = readSavePoint( false, "the save's place", &label, &line );
    if (address){
        script_h.setCurrent( address );
        current_label_info = script_h.getLabelInfo( label );
        current_line = line;
    }

    int num = readInt();
    NestInfo *info = root_nest_info.next;
    for (int n=0 ; n<num ; n++){
        address = readSavePoint( true, "a gosub's return place", NULL, NULL );
        if (!info) continue;
        if (address) info->next_script = address;
        info = info->next;
    }
    pretext_buf = last_nest_info->next_script;
}

// kidoku.dat: "CONSKDK1", then for each label with read marks its name (with
// a 0), the count of its lines with marks and per line its number in the
// label, its hash, its length (with the newline) and one bit per byte, set
// where the script read there; numbers are 4 bytes, little-endian. Marks
// whose line is gone (by hash, as a save's place) are dropped.
static const char KIDOKU_MAGIC[8] = { 'C','O','N','S','K','D','K','1' };

static void put32( std::string &s, unsigned int v )
{
    for (int i=0 ; i<4 ; i++) s += (char)((v >> (i*8)) & 0xff);
}

void ScriptHandler::writeKidokuCons( FILE *fp )
{
    std::string out( KIDOKU_MAGIC, sizeof(KIDOKU_MAGIC) );
    for (int i=0 ; i<num_of_labels ; i++){
        char *start = label_info[i].label_header;
        char *end = labelEnd( *this, i );
        int b0 = (start - script_buffer) / 8, b1 = (end - script_buffer + 7) / 8;
        while (b0 < b1 && !kidoku_buffer[b0]) b0++;
        if (b0 == b1) continue; // no marks in the label

        std::string lines;
        int count = 0, n = 0;
        for (char *p = start ; p < end ; n++){
            char *q = p;
            while (*q != 0x0a) q++;
            int len = q - p + 1;
            std::string mask( (len+7)/8, 0 );
            bool marked = false;
            for (int k=0 ; k<len ; k++){
                int o = p - script_buffer + k;
                if (kidoku_buffer[o/8] & (1 << (o%8))){
                    mask[k/8] |= (char)(1 << (k%8));
                    marked = true;
                }
            }
            if (marked){
                put32( lines, n );
                put32( lines, lineHash( p ) );
                put32( lines, len );
                lines += mask;
                count++;
            }
            p = q+1;
        }
        out += label_info[i].name;
        out += '\0';
        put32( out, count );
        out += lines;
    }
    fwrite( out.data(), 1, out.size(), fp );
}

void ScriptHandler::readKidokuCons( FILE *fp )
{
    std::vector<unsigned char> d;
    unsigned char buf[65536];
    size_t got;
    while ((got = fread( buf, 1, sizeof(buf), fp )) > 0) d.insert( d.end(), buf, buf+got );

    if (d.size() < sizeof(KIDOKU_MAGIC) || memcmp( &d[0], KIDOKU_MAGIC, sizeof(KIDOKU_MAGIC) )){
        if (d.size() == (size_t)script_buffer_length/8) // the stock format, from this same script
            memcpy( kidoku_buffer, &d[0], d.size() );
        else if (!d.empty())
            utils::printInfo( "kidoku.dat: the old format, for another script; the read marks start over\n" );
        return;
    }

    std::unordered_map<std::string, int> labels;
    for (int i=0 ; i<num_of_labels ; i++) labels.insert( std::make_pair( std::string( label_info[i].name ), i ) );

    size_t pos = sizeof(KIDOKU_MAGIC);
    bool bad = false;
    int lost = 0;
    #define GET32(v) do{ if (pos+4 > d.size()){ bad = true; break; } \
        v = d[pos] | (d[pos+1] << 8) | (d[pos+2] << 16) | ((unsigned int)d[pos+3] << 24); pos += 4; }while(0)
    while (!bad && pos < d.size()){
        size_t z = pos;
        while (z < d.size() && d[z]) z++;
        if (z == d.size()){ bad = true; break; }
        std::unordered_map<std::string, int>::iterator it = labels.find( std::string( (char*)&d[pos], z-pos ) );
        pos = z+1;
        LabelLines ll;
        if (it != labels.end()) labelLines( *this, it->second, ll );
        unsigned int count;
        GET32( count );
        for (unsigned int c=0 ; !bad && c<count ; c++){
            unsigned int n, hash, len;
            GET32( n ); if (bad) break;
            GET32( hash ); if (bad) break;
            GET32( len ); if (bad) break;
            size_t bytes = ((size_t)len + 7) / 8;
            if (pos + bytes > d.size()){ bad = true; break; }
            int k = ll.lines.empty() ? -1 : nearestLine( ll, (int)n, hash );
            unsigned int now_len = 0;
            if (k >= 0) while (ll.lines[k][now_len++] != 0x0a) ;
            if (k >= 0 && now_len == len){
                int o0 = ll.lines[k] - script_buffer;
                for (unsigned int b=0 ; b<len ; b++)
                    if (d[pos + b/8] & (1 << (b%8))){
                        int o = o0 + b;
                        kidoku_buffer[o/8] |= (char)(1 << (o%8));
                    }
            }
            else lost++;
            pos += bytes;
        }
    }
    #undef GET32
    if (bad) utils::printError( "kidoku.dat: cut short or broken; read up to byte %d\n", (int)pos );
    if (lost) utils::printInfo( "kidoku.dat: %d read lines are no longer in the script\n", lost );
}
