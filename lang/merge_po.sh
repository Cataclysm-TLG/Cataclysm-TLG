#!/usr/bin/env bash

set -e

if [ ! -d lang/po ]; then
    if [ -d ../lang/po ]; then
        cd ..
    else
        echo "Error: Could not find lang/po subdirectory."
        exit 1
    fi
fi

merge_lang() {
    local n="$1"
    local o="lang/po/${n}.po"

    if [ ! -f "$o" ]; then
        echo "skip missing $o"
        return
    fi

    echo "updating $o"

    tmp=$(mktemp)

    # merge against template (preserves fuzzy flags as-is)
    msgmerge --sort-by-file "$o" lang/po/cataclysm-tlg.pot > "$tmp"

    # overwrite safely
    mv "$tmp" "$o"
}

# specific languages
if [ $# -gt 0 ]; then
    for n in "$@"; do
        merge_lang "$n"
    done

# all languages
elif [ -d lang/po ]; then
    shopt -s nullglob
    for f in lang/po/*.po; do
        n=$(basename "$f" .po)
        merge_lang "$n"
    done
fi