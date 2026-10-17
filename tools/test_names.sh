#!/bin/bash
# Headless check of defvar/defstr/defarr and spnew/spfree (ONScripter_names.cpp).
# usage: tools/test_names.sh <conscripter binary> <a Japanese TTF>   (needs iconv, magick)
set -u
BIN=$(realpath "$1"); FONT=$(realpath "$2")
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
cd "$D" && cp "$FONT" default.ttf
fail=0
check(){ if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; fail=1; fi; }
# run <script (UTF-8)> <save dir> [options]: the values *show printed ("X:..." lines)
run(){ printf '%s' "$1" | iconv -f utf-8 -t cp932 > 0.txt; mkdir -p "$2"
       SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -k 5 20 "$BIN" -r . --save-dir "$2" "${@:3}" 2>&1 |
       iconv -f cp932 -t utf-8 -c | grep -ao 'X:[^]]*\|Parse error[^]]*]' | tr '\n' ' '; }
px(){ magick "$1" -format "%[pixel:p{$2}]" info:; }

TAIL='*start
gosub *show
mov %score,11
add %seen,1
mov ?cg[3],?cg[3]+1
mov $who,"い"
spnew %h1
spnew %h2,ui
spnew %h3,overlay
spnew %h4,overlay
bg white,1
lsp 600,":c;>20,20,#ff0000",0,0
lsp 400,":c;>20,20,#0000ff",40,0
lsp %h1,":c;>200,20,#00ff00",0,0
lsp 499,":c;>20,20,#ff0000",0,40
lsp 498,":c;>20,20,#0000ff",40,40
lsp %h2,":c;>200,20,#ffff00",0,40
lsp 0,":c;>20,20,#ff0000",120,80
lsp %h3,":c;>100,20,#ff00ff",0,80
lsp %h4,":c;>100,20,#00ffff",50,80
print 1
savegame 1
goto *after
*after
gosub *show
print 1
getscreenshot 640,480
savescreenshot "shot.bmp"
end
*show
itoa $90,%score
itoa $91,%seen
itoa $92,?cg[3]
itoa $93,%h1
itoa $94,%h4
lsp 999,"X:"+$90+","+$91+","+$92+","+$who+","+$93+","+$94,0,0
csp 999
return
'
DEF1='*define
globalon
defvar score,7
defvar seen,global
defstr who,"あ"
defarr cg,20,global
defvar gone,3
defvar h1
defvar h2
defvar h3
defvar h4
defvar score,7
game
'
# the same names in another order, one more: the slots move
DEF2='*define
globalon
defvar extra,5
defvar h4
defarr cg,20,global
defvar h3
defstr who,"あ"
defvar h2
defvar seen,global
defvar h1
defvar score,7
game
'
check "defaults, set, spnew" "$(run "$DEF1$TAIL" s)" "X:7,0,0,あ,0,0 X:11,1,1,い,1000,1003 "
# game over 600, under 400; ui over 499, under 498; overlay over 0, the lower handle on top
check "layers" "$(for p in 10,10 50,10 10,50 50,50 75,90 125,90; do px s/shot.bmp $p; echo -n ' '; done)" \
  "srgba(1,253,0,1) srgba(0,1,253,1) srgba(255,253,0,1) srgba(1,1,253,1) srgba(253,1,255,1) srgba(1,253,253,1) "
cp s/shot.bmp shot1.bmp
check "load, slots moved" "$(run "$DEF2$TAIL" s --load 1)" "X:11,1,1,い,1000,1003 "
check "load: same picture" "$(compare -metric AE shot1.bmp s/shot.bmp null: 2>&1 | cut -d' ' -f1)" "0"

# a save from before version 26 (25: #65's): its NScripter part loads, no handle pictures
python3 -I -c '
import sys; d=bytearray(open(sys.argv[1],"rb").read()); i=d.find(b"CONS"); d[i+4]=25; open(sys.argv[2],"wb").write(d)' s/save1.dat old.dat
mkdir -p o && cp old.dat o/save1.dat
check "old save" "$(run "$DEF1$TAIL" o --load 1)" "X:7,0,0,あ,0,0 "
check "old save: numbered sprites" "$(px o/shot.bmp 10,10) $(px o/shot.bmp 75,90)" "srgba(255,1,1,1) srgba(255,255,255,1)"

# gloval.sav, definereset, reset (%200: a numbered global, the stage)
S3='*define
globalon
defvar seen,global
defstr gs,global
defvar loc,4
defvar h1
game
*start
itoa $90,%seen
itoa $91,%loc
itoa $92,%h1
lsp 999,"X:"+$90+","+$gs+","+$91+","+$92,0,0
csp 999
if %200==2 goto *fin
if %200==1 goto *two
mov %200,1
add %seen,1
mov $gs,"g"
mov %loc,9
spnew %h1,ui
definereset
*two
mov %200,2
mov %loc,8
spnew %h1
reset
*fin
end
'
check "gloval, definereset, reset" "$(run "$S3" g)" "X:0,,4,0 X:1,g,4,0 X:1,g,4,1000 "
check "gloval next run" "$(run "$S3" g)" "X:1,g,4,0 "

# spz from a handle onto another handle; csp -1 clears the handles' pictures, the handles stay held
S5='*define
defvar a
defvar b
defvar c
game
*start
bg white,1
spnew %a,overlay
spnew %b
lsp %a,":c;>20,20,#ff0000",0,0
lsp %b,":c;>20,20,#00ff00",0,0
lsp 0,":c;>20,20,#0000ff",0,0
spz %b,%a
print 1
getscreenshot 640,480
savescreenshot "z.bmp"
csp -1
spnew %c
itoa $1,%a
itoa $2,%b
itoa $3,%c
lsp 999,"X:"+$1+","+$2+","+$3,0,0
csp 999
print 1
getscreenshot 640,480
savescreenshot "c.bmp"
end
'
check "csp -1 keeps handles" "$(run "$S5" z)" "X:1000,1001,1002 "
check "spz onto a handle; csp -1" "$(px z/z.bmp 10,10) $(px z/c.bmp 10,10)" "srgba(1,253,0,1) srgba(255,255,255,1)"

# ;value / ;$V above 10000: the names go above them
check ";value20000" "$(run ';value20000
*define
defvar a,4
game
*start
itoa $1,%a
lsp 999,"X:"+$1,0,0
end
' v)" "X:4 "
check "another kind" "$(run '*define
defvar a
defstr a
game
' e)" "Parse error at define:2 [defstr: a declared before as another kind] "
check "extra argument" "$(run '*define
defvar a,global,3
game
' e)" "Parse error at define:1 [defvar: a: too many arguments] "
exit $fail
