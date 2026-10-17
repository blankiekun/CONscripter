#!/bin/bash
# Headless check of screens (ONScripter_screen.cpp): a save screen and a load
# screen with a confirm dialog, played by --auto-play --choose.
# usage: tools/test_screens.sh <conscripter binary> <a Japanese TTF>   (needs iconv, magick)
set -u
BIN=$(realpath "$1"); FONT=$(realpath "$2")
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
cd "$D" && cp "$FONT" default.ttf && mkdir -p screens sv
fail=0
check(){ if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; fail=1; fi; }

# widgets: 1 slots (101, 102), 2 next page (200), 3 delete (300), 4 close (400);
# the confirm dialog: 2 yes (200), 3 no (300)
for kind in save load; do cat > screens/$kind.lua <<LUA
return {
  kind = "$kind",
  widgets = {
    {"saveslots", x=10, y=10, rows=2, dy=60, pages=2, select=true, save_text="\$10", split="|",
      action = {{"set", "%20", slot=true}, {"$kind", confirm="No.%N でいいですか？"}},
      button = {file=">200,50,#808080", opaque=true}, thumb={x=0, y=0, w=40, h=30},
      lines = {{x=50, y=0, size=16, text="%N %T1", empty="%N 空き"}, {x=50, y=20, size=16, text="[%T2][%T-1]", empty=""}}},
    {"button", file=">60,20,#00ff00", opaque=true, x=300, y=10, action={"page", "+1"}},
    {"button", file=">60,20,#ff0000", opaque=true, x=300, y=40, action={{"delete", confirm="No.%N を消しますか？"}, {"gosub", "*deleted"}}},
    {"button", file=">60,20,#0000ff", opaque=true, x=300, y=70, action={"close", 7}},
  },
}
LUA
done
cat > screens/confirm.lua <<'LUA'
return { kind = "confirm", widgets = {
  {"text", text=arg, x=320, y=200, size=16, align="centre"},
  {"button", file=">60,20,#ffffff", opaque=true, x=200, y=240, action={"close", 1}},
  {"button", file=">60,20,#000000", opaque=true, x=300, y=240, action={"close", 0}},
}}
LUA
printf '%s' '*define
loadgosub *loaded
game
*start
mov $10,"一章ポ|節"
あ\
screen "save",%5
lsp 999,"X:save"+str(%5)+"/"+str(%20),0,0
screen "load",%5
end
*deleted
mov %21,1
return
*loaded
return
' | sed 's/str(\(%[0-9]*\))/"\1"/g' | iconv -f utf-8 -t cp932 > 0.txt
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -k 5 30 "$BIN" -r . --enc:sjis --save-dir sv --trace t.jsonl \
  --auto-play 0 --skip-screens none --until loaded \
  --choose '*start:7=101,101,102,102,101,101,300,200,400' --choose '*start:9=102,300,200,101,101,200' > log.txt 2>&1
check "exit" "$?" "0"
acts=$(grep -a '"t":"screen"' t.jsonl | sed 's/.*"screen":"\([a-z]*\)".*"action":"\([a-z ]*\)".*/\1:\2/' | tr '\n' ' ')
check "actions" "$acts" "save:open save:select save:set save:save save:select save:set save:save save:select save:set confirm:open confirm:close save:no save save:page save:close load:open load:select confirm:open confirm:close load:delete load:gosub load:select load:set confirm:open confirm:close load:load "
check "save 1 kept" "$(ls sv | grep -c '^save1.dat$')" "1"
check "save 2 and its thumbnail deleted" "$(ls sv | grep -c '^save2.dat$\|^savethumb2.bmp$')" "0"
check "thumbnail size" "$(magick identify -format '%wx%h' sv/savethumb1.bmp 2>&1)" "40x30"
check "slot text" "$(grep -a -c '"src":"strsp [0-9]*,\\"01 一章ポ\\"' t.jsonl | sed 's/^[1-9][0-9]*$/drawn/')" "drawn"
check "split part 2" "$(grep -a -c '"src":"strsp [0-9]*,\\"\[節\]\[節\]\\"' t.jsonl | sed 's/^[1-9][0-9]*$/drawn/')" "drawn"
check "end" "$(tail -1 t.jsonl | grep -o '"reason":"[^"]*"')" '"reason":"until label"'

# settings: widgets 1 slider %30 (minus 101, plus 102), 2 toggle %31 (on
# 0, 200), 3 radio %32=2 (300), 4 tab to page 2 (400), 5 text (page 2),
# 6 toggle %33 (page 2, 600), 7 close 1 asked first (700)
cat > screens/settings.lua <<'LUA'
return { kind = "settings", pages = 2, widgets = {
  {"slider", var="%30", min=0, max=10, text="音量%B%V", x=10, y=10, size=16, cols=20,
    minus={text="－", x=300, y=10, size=16, cols=2}, plus={text="＋", x=340, y=10, size=16, cols=2}, action={"gosub", "*applied"}},
  {"toggle", var="%31", on=0, off=1, text="%C　確認", x=10, y=40, size=16, cols=10},
  {"radio", var="%32", value=2, text="%C　二", x=10, y=70, size=16, cols=10},
  {"button", text="次", x=400, y=10, size=16, cols=3, disabled_color="#F0C040", action={"page", 2}},
  {"text", text="二頁", x=10, y=100, size=16, page=2},
  {"toggle", var={"%33", "%35"}, text="%C　声", x=10, y=130, size=16, cols=10, page=2},
  {"button", text="閉じる", x=400, y=300, size=16, cols=5, action={"close", 1, confirm="閉じますか？"}},
}}
LUA
printf '%s' '*define
game
*start
mov %30,5
screen "settings",%5
if %30 == 6 && %31 == 1 && %32 == 2 && %33 == 1 && %35 == 1 && %5 == 1 && %34 == 1 lsp 999,":c;X:settings_ok",0,0
end
*applied
add %34,1
return
' | iconv -f utf-8 -t cp932 > 0.txt
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -k 5 30 "$BIN" -r . --enc:sjis --save-dir sv --trace t2.jsonl \
  --auto-play 0 --skip-screens none --choose '*start:5=102,200,300,400,600,700,300,700,200' > log2.txt 2>&1
check "settings exit" "$?" "0"
check "settings values" "$(grep -ao 'X:settings_ok' log2.txt | head -1)" "X:settings_ok"
acts=$(grep -a '"t":"screen"' t2.jsonl | sed 's/.*"screen":"\([a-z]*\)".*"action":"\([a-z ]*\)".*/\1:\2/' | tr '\n' ' ')
check "settings actions" "$acts" "settings:open settings:set settings:gosub settings:set settings:set settings:page settings:set confirm:open confirm:close settings:no close confirm:open confirm:close settings:close "
check "tab shown as current on page 2" "$(grep -a -c '"src":"strsp [0-9]*,\\"次\\",400,10,3,1,16,16,0,0,0,0,\\"#F0C040\\"' t2.jsonl | sed 's/^[1-9][0-9]*$/drawn/')" "drawn"

# backlog: widgets 1 list (names with a voice: 101..), 2 older (200), 3 newer (300), 4 close (400)
cat > screens/backlog.lua <<'LUA'
return { kind = "backlog", wheel_up = {"scroll", "+1"}, wheel_down = {"scroll", "-1", at_end = {"close"}}, widgets = {
  {"list", source="log", x=20, y=20, h=200, size=16, line_height=20, cols=30, name_color="#D8C088", hover="#FFFFFF"},
  {"button", text="上", x=500, y=20, size=16, cols=2, disabled_color="#808080", action={"scroll", "+1"}},
  {"button", text="下", x=500, y=60, size=16, cols=2, disabled_color="#808080", action={"scroll", "-1"}},
  {"button", text="閉", x=500, y=100, size=16, cols=2, action={"close"}},
}}
LUA
printf '%s' '*define
pretextgosub *pretext
game
*start
[太郎|v1.wav]一頁目。\
[花子|]二頁目の一行目、@
二頁目の二行目。\
三頁目。\
screen "backlog"
end
*pretext
saveon
return
' | iconv -f utf-8 -t cp932 > 0.txt
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -k 5 30 "$BIN" -r . --enc:sjis --save-dir sv --trace t3.jsonl \
  --auto-play 0 --skip-screens none --choose '*start:9=200,200,101,300,300,400' > log3.txt 2>&1
check "backlog exit" "$?" "0"
acts=$(grep -a '"t":"screen"' t3.jsonl | sed 's/.*"action":"\([a-z ]*\)".*"value":\(-*[0-9]*\).*/\1=\2/;s/.*"action":"\([a-z ]*\)".*/\1/' | tr '\n' ' ')
check "backlog actions" "$acts" "open scroll=1 scroll=2 voice=3 scroll=1 scroll=0 close=0 "
check "backlog name with its voice" "$(grep -a -c '"src":"strsp [0-9]*,\\"太郎　♪\\"' t3.jsonl | sed 's/^[1-9][0-9]*$/drawn/')" "drawn"
check "backlog voice played" "$(grep -a -c '"cmd":"dwave".*v1.wav' t3.jsonl | sed 's/^[1-9][0-9]*$/played/')" "played"
# picture sliders with a fill at 300 of 1000: widget 1 left to right
# (knob x 157, fill lit 100-162), widget 2 reversed (travel -190: knob x
# 233, fill lit from the left end too, 100-238), 3 close (300)
cat > screens/settings.lua <<'LUA'
return { kind = "settings", widgets = {
  {"slider", var="%30", min=0, max=1000, travel=190, knob={file=">10,20,#ff0000", x=100, y=95},
    track={x=100, y=95, w=200, h=20}, fill={file=">200,10,#00ff00", x=100, y=100}},
  {"slider", var="%31", min=0, max=1000, travel=-190, knob={file=">10,20,#ff0000", x=290, y=195},
    track={x=100, y=195, w=200, h=20}, fill={file=">200,10,#00ff00", x=100, y=200}},
  {"button", text="閉", x=500, y=300, size=16, cols=2, action={"close"}},
}}
LUA
printf '%s' '*define
game
*start
mov %30,300:mov %31,300
screen "settings"
end
' | iconv -f utf-8 -t cp932 > 0.txt
# no auto-play (it would close the screen at once): the frame at 1.5 s, then killed
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -k 2 5 "$BIN" -r . --enc:sjis --save-dir sv --trace t4.jsonl \
  --shot-after 1500 --shot-dir sh > log4.txt 2>&1
# each pixel as g (the fill), r (a knob) or k (black)
px(){ for p in "$@"; do magick sh/ms0001500.png -format "%[fx:p{$p}.g>0.5?1:p{$p}.r>0.5?2:0]" info: 2>&1; done | tr 012 kgr; }
check "fill from the left end to the knob" "$(px 101,105 150,105 165,105 200,105 298,105)" "ggrkk"
check "reversed: from the left end too" "$(px 101,205 150,205 220,205 240,205 260,205 298,205)" "gggrkk"
exit $fail
