#!/bin/sh
set -eu

log="${FILMVIZ_OFX_LOG_PATH:-$HOME/Library/Logs/FilmViz/filmviz_ofx.log}"

if [ ! -f "$log" ]; then
    echo "FilmViz OFX log does not exist yet:"
    echo "  $log"
    exit 1
fi

if [ "$(uname -s)" = "Darwin" ]; then
    open "$log"
else
    ${PAGER:-less} "$log"
fi
