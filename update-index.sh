#!/bin/bash
INDEX="$HOME/fonteOS/index"
echo "# name|version|url|sha256|description" > "$INDEX"

for pkg in "$@"; do
    echo -n "fetching $pkg from repology... "
    data=$(curl -s -A "fonteOS-fpackage/0.5 (https://github.com/mkead1232/fonteOS)" \
        "https://repology.org/api/v1/project/$pkg" 2>/dev/null)

    if [ -z "$data" ] || [ "$data" = "[]" ]; then
        echo "SKIP (not found)"
        continue
    fi

    ver=$(echo "$data" | python3 -c "
import sys,json
try:
    data=json.load(sys.stdin)
    newest=[p for p in data if p.get('status')=='newest']
    print(newest[0]['version'] if newest else '')
except: print('')
" 2>/dev/null)

    [ -z "$ver" ] && echo "SKIP (no version)" && continue

    url=$(echo "$data" | python3 -c "
import sys,json
try:
    data=json.load(sys.stdin)
    for p in data:
        if p.get('status')=='newest' and p.get('downloads'):
            print(p['downloads'][0]); sys.exit()
    print('')
except: print('')
" 2>/dev/null)

    [ -z "$url" ] && url="https://ftp.gnu.org/gnu/$pkg/$pkg-$ver.tar.gz"

    desc=$(echo "$data" | python3 -c "
import sys,json
try:
    data=json.load(sys.stdin)
    for p in data:
        if p.get('summary'): print(p['summary'][:80]); sys.exit()
    print('No description')
except: print('No description')
" 2>/dev/null)

    echo "$pkg|$ver|$url||$desc" >> "$INDEX"
    echo "OK ($ver)"
    sleep 1
done

echo ""
echo "done — $(grep -c '^[^#]' $INDEX) package(s) in index"
