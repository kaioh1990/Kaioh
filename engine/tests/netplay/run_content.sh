#!/bin/bash
# Content fingerprint test (cvf_fingerprint.go): the guest plays from a
# modified copy of the game directory, and the session must stop with a
# "Game data mismatch" message on both sides when the change affects the
# match, or play when it does not.
#
#   tests/netplay/run_content.sh GAMEDIR [CASE...]
#
# Cases (default: all):
#   rules    SpecialCooldown 45 -> 30 in data/cvf_online.ini      mismatch
#   lobby    LobbyURL, WideView and a comment in cvf_online.ini   plays
#   common   data/common.const edited                            mismatch
#   char     chars/Richter/states.st edited (Richter plays)      mismatch
#   stage    stages/ParanoiaBG.def edited                        mismatch
#   unused   an extra, different character only on the guest     plays
#   lf       chars/Alucard text files with LF instead of CRLF    plays
#
# Environment: HOST_BIN, GUEST_BIN, PORT_BASE, WORK as run_prematch.sh;
# RUNS (default 1) runs per case. With OLD_BIN (a build without the
# fingerprint, which sends an empty one) every case is also run old host -
# new guest and new host - old guest, and must play (empty = unknown, allowed).
set -u
E=$(cd "$(dirname "$0")/../.." && pwd)
G=$(cd "${1:?usage: run_content.sh GAMEDIR [CASE...]}" && pwd)
shift
CASES=${*:-rules lobby common char stage unused lf}
WORK=${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/cvf_content.XXXXXX")}
RUNS=${RUNS:-1}
mkdir -p "$WORK"

# a game dir for the guest: GAMEDIR through symlinks, one part copied and changed
variant() {  # case
    local v=$WORK/game-$1
    rm -rf "$v"; mkdir -p "$v/chars" "$v/data" "$v/stages"
    cp -RL "$G/save" "$v/save"
    for d in font sound external; do [ -e "$G/$d" ] && ln -s "$G/$d" "$v/$d"; done
    for f in "$G"/data/* "$G"/stages/* "$G"/chars/*; do ln -s "$f" "$v/${f#$G/}"; done
    case $1 in
        rules)  rm "$v/data/cvf_online.ini"
                sed 's/^SpecialCooldown *= *45/SpecialCooldown = 30/' "$G/data/cvf_online.ini" > "$v/data/cvf_online.ini" ;;
        lobby)  rm "$v/data/cvf_online.ini"
                sed -e 's#^LobbyURL *=.*#LobbyURL = https://example.invalid#' -e 's/^WideView *= *1/WideView = 0/' \
                    -e 's/^;/;;/' "$G/data/cvf_online.ini" > "$v/data/cvf_online.ini" ;;
        common) rm "$v/data/common.const"
                { cat "$G/data/common.const"; echo "; edited"; } > "$v/data/common.const" ;;
        char)   rm "$v/chars/Richter"; cp -R "$G/chars/Richter" "$v/chars/Richter"
                { echo "; edited"; cat "$G/chars/Richter/states.st"; } > "$v/chars/Richter/states.st" ;;
        stage)  rm "$v/stages/ParanoiaBG.def"
                { cat "$G/stages/ParanoiaBG.def"; echo "; edited"; } > "$v/stages/ParanoiaBG.def" ;;
        unused) cp -R "$G/chars/Richter" "$v/chars/Extra"
                echo "; edited" >> "$v/chars/Extra/states.st" ;;
        lf)     rm "$v/chars/Alucard"; cp -R "$G/chars/Alucard" "$v/chars/Alucard"
                for f in "$v"/chars/Alucard/*.st "$v"/chars/Alucard/*.def; do sed -i.bak 's/\r$//' "$f"; rm -f "$f.bak"; done ;;
        *) echo "unknown case $1"; exit 2 ;;
    esac
    echo "$v"
}

expect() {
    case $1 in rules|common|char|stage) echo mismatch ;; *) echo ok ;; esac
}

fail=0
for c in $CASES; do
    v=$(variant "$c")
    pairs="new:new"
    [ -n "${OLD_BIN:-}" ] && pairs="$pairs old:new new:old"
    for pr in $pairs; do
        hb=${HOST_BIN:-$E/build/release/cvf_headless} gb=${GUEST_BIN:-$E/build/release/cvf_headless}
        [ "${pr%:*}" = old ] && hb=$OLD_BIN
        [ "${pr#*:}" = old ] && gb=$OLD_BIN
        want=$(expect "$c")
        [ "$pr" != new:new ] && want=ok   # an old side sends no fingerprint: allowed
        echo "== $c ($pr host:guest, expect $want)"
        HOST_BIN=$hb GUEST_BIN=$gb GUEST_GAME=$v EXPECT=$want WORK=$WORK/run-$c-${pr/:/-} \
            "$E/tests/netplay/run_prematch.sh" "$G" "$RUNS" | grep -v '^work:' | sed 's/^/  /'
        [ "${PIPESTATUS[0]}" = 0 ] || fail=1
    done
done
echo "work: $WORK"
if [ $fail = 0 ]; then echo "content: PASS"; else echo "content: FAIL"; exit 1; fi
