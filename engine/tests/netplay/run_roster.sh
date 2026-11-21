#!/bin/bash
# Roster desync sweep: online sessions (run_netplay.sh with MATCHES) over
# character pairs, so that every character plays at least once, both sides
# compare every frame of every match, in every network scenario.
#
#   tests/netplay/run_roster.sh GAMEDIR [SCENARIO...]
#
# Characters: CHARS="Name ..." (as -p1 takes them: a folder under chars/
# with Name.def, or "folder/file.def"), default every chars/X/X.def of GAMEDIR
# except the folders in EXCLUDE (default "Intro", CvF's intro storyboard).
# Pairs: each character against the next one in
# the list (wrapping round), then the reverse pair, so each character plays
# on both sides; PAIRS="A:B ..." replaces them. The pairs are split into
# sessions of PER_SESSION matches (default 6); each session is one
# run_netplay.sh call (the first pair through ARGS, the others through
# MATCHES), with the scenarios given (default: direct loss:20:10:8).
# SEEDS="11:22 ..." repeats everything with other input scripts.
#
# Environment: as run_netplay.sh (HOST_BIN, GUEST_BIN, PORT_BASE, FPS, LOAD,
# TIMEOUT, RELAY_SEED), plus CHARS, EXCLUDE, PAIRS, PER_SESSION, SEEDS, STAGE
# (default stages/ParanoiaBG.def), ROUNDS (default 1), MATCH_ARGS (more
# match arguments, e.g. "-time 30" for short rounds) and WORK. A pair may
# give palettes: A:B:PAL1:PAL2.
# Prints one line per session and "roster: PASS" if every session passed.
set -u
E=$(cd "$(dirname "$0")/../.." && pwd)
G=$(cd "${1:?usage: run_roster.sh GAMEDIR [SCENARIO...]}" && pwd)
shift
SCEN=${*:-direct loss:20:10:8}
WORK=${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/cvf_roster.XXXXXX")}
mkdir -p "$WORK"
STAGE=${STAGE:-stages/ParanoiaBG.def}
PER=${PER_SESSION:-6}
if [ -z "${CHARS:-}" ]; then
    CHARS=""
    for d in "$G"/chars/*/; do
        n=$(basename "$d")
        case " ${EXCLUDE-Intro} " in *" $n "*) continue ;; esac
        [ -f "$d/$n.def" ] && CHARS="$CHARS $n"
    done
fi
set -- $CHARS
nc=$#
echo "characters ($nc):$CHARS"
if [ -z "${PAIRS:-}" ]; then
    arr=($CHARS)
    PAIRS=""
    for ((i = 0; i < nc; i++)); do
        a=${arr[$i]} b=${arr[$(((i + 1) % nc))]}
        PAIRS="$PAIRS $a:$b"
    done
    for ((i = 0; i < nc; i++)); do
        a=${arr[$i]} b=${arr[$(((i + 1) % nc))]}
        PAIRS="$PAIRS $b:$a"
    done
fi
set -- $PAIRS
echo "pairs ($#):$PAIRS"
fail=0
s=0
for seeds in ${SEEDS:-11:22}; do
    set -- $PAIRS
    while [ $# -gt 0 ]; do
        s=$((s + 1))
        first=$1; shift
        rest=""
        for ((k = 1; k < PER && $# > 0; k++)); do rest="$rest $1"; shift; done
        out=$WORK/session$s.txt
        IFS=: read -r c1 c2 pal1 pal2 <<< "$first"
        ARGS="-p1 $c1 -p2 $c2 ${pal1:+-p1.pal $pal1} ${pal2:+-p2.pal $pal2} -s $STAGE -rounds ${ROUNDS:-1} ${MATCH_ARGS:-}" MATCHES="${rest# }" \
            SEED_HOST=${seeds%:*} SEED_GUEST=${seeds#*:} WORK=$WORK/session$s \
            "$E/tests/netplay/run_netplay.sh" "$G" $SCEN > "$out" 2>&1
        st=$?
        [ $st = 0 ] || fail=1
        echo "session $s (seeds $seeds): $first$rest: $(tail -1 "$out") ($out)"
    done
done
echo "work: $WORK"
if [ $fail = 0 ]; then echo "roster: PASS"; else echo "roster: FAIL"; exit 1; fi
