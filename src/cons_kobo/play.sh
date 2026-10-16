#!/bin/sh
# CONscripter on a Kobo: copy this next to a game's archives and run it from
# KOReader's file browser (long-press it, "Execute shell script").
# The engine is /mnt/onboard/.adds/conscripter/conscripter, with its ffmpeg for
# movies beside it (script/cross_kobo.sh).
# Tap to click, long-press for the menu; the page keys are Enter and Escape.
# Hold the back page key for 2 seconds to quit back to KOReader.
# KOBO_FLIP=1 turns the game the other way round.
cd "$(dirname "$0")" || exit 1
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy CONS_FFMPEG=/mnt/onboard/.adds/conscripter/ffmpeg \
    /mnt/onboard/.adds/conscripter/conscripter --root . "$@" > conscripter.log 2>&1
