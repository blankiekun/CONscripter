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
exit $fail
