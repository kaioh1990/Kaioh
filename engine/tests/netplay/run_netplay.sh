#!/bin/bash
# Online match test: two headless engines play a full rollback match over
# loopback through the real netplay path (main.lua's -ip: enterNetPlay, TCP
# handshake, synchronize, GGPO rollback over UDP), with scripted inputs, and
# their per-frame state dumps are compared.
#
#   tests/netplay/run_netplay.sh GAMEDIR [SCENARIO...]
#
# GAMEDIR    reference game directory (cvf-refdata.tgz + data/Xsound.snd, see
#            docs/DEVSETUP.md); used read-only, through symlinks
# SCENARIO   direct            no relay
#            NAME:DELAY:JITTER:LOSS[:OPTION...]
#                              through tests/netplay/udp_relay.py: delay and
#                              jitter in ms, loss in percent. Options:
#                sync          the loss also hits GGPO's sync handshake
#                              (spared otherwise). Builds without the GGPO
#                              handshake fix (third/ggpo, GAPS.md) can abort a
#                              match when sync packets are lost: the
#                              synchronized peer dropped the other after 3 s,
#                              which retried only every 2 s. Fixed builds pass
#                              ":sync" cases, also against unfixed ones
#                tcp=D/J       the netplay TCP connection (sync settings, seed,
#                              delay-netplay input, stop markers) also goes
#                              through tests/netplay/tcp_relay.py with D +- J ms
#            default: direct lag:40:15:0 loss:20:10:8
#
# Environment:
#   HOST_BIN, GUEST_BIN  engines (default build/release/cvf_headless; the Go
#                        build cvf_headless_go works too, see DEVSETUP.md)
#   WORK                 work directory (default: a new temporary one)
#   PORT_BASE            TCP port; base+1..base+5 are UDP and the TCP relay
#                        (default 7650)
#   SEED_HOST, SEED_GUEST  input scripts (CVF_NET_INPUT, default 11 / 22)
#   FPS                  CVF_HEADLESS_FPS for both (default 0: unpaced)
#   ARGS                 match arguments (default: Alucard vs Richter,
#                        ParanoiaBG, 1 round)
#   MATCHES              more matches in the same online session after the
#                        first, "P1:P2[:PAL1:PAL2] ..." (CVF_NET_MATCHES of
#                        cvfheadless.go: back to the character select for
#                        SELECT_FRAMES delay-netplay frames, default 60, then
#                        the next pair; the same pair again is a rematch).
#                        Every match's dump is compared (statedump.K.txt),
#                        and the memory after each match is printed
#   LOAD=N               N busy loops beside the engines (a loaded machine)
#   GUEST_GAME           the guest's game directory (default GAMEDIR)
#   TIMEOUT              seconds per scenario (default 600, x matches)
#   GGPO_LOGS=1          GGPO's trace in each instance's save/logs
#   RELAY_LOG=1          the relay's packet log (SCENARIO/packets.txt)
#   RELAY_SEED           the relays' random seed (default 1)
#   GC_TRACE=1           CVF_GC_TRACE for native engines (collector lines in
#                        out.txt)
#   CLEAN=1              delete the dumps once compared (long sessions write
#                        tens of MB per match and side)
#   NODUMP=1             no state dumps (memory figures without the dump
#                        buffer, which holds a whole match): only the exit
#                        status and the winners are compared
#
# Every scenario must end with both dumps identical, and all scenarios must
# give the same dump (the inputs depend on the simulated time only, so the
# network conditions must not change the match). Prints PASS or FAIL.
set -u
E=$(cd "$(dirname "$0")/../.." && pwd)
G=${1:?usage: run_netplay.sh GAMEDIR [SCENARIO...]}
G=$(cd "$G" && pwd)
shift
SCEN=${*:-direct lag:40:15:0 loss:20:10:8}
HB=${HOST_BIN:-$E/build/release/cvf_headless}
GB=${GUEST_BIN:-$E/build/release/cvf_headless}
GG=$(cd "${GUEST_GAME:-$G}" && pwd)
P=${PORT_BASE:-7650}
ARGS=${ARGS:--p1 Alucard -p2 Richter -s stages/ParanoiaBG.def -rounds 1}
MATCHES=${MATCHES:-}
NM=$((1 + $(echo $MATCHES | wc -w)))
TIMEOUT=${TIMEOUT:-$((600 * NM))}
WORK=${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/cvf_netplay.XXXXXX")}
mkdir -p "$WORK"
md5() { if command -v md5sum >/dev/null; then md5sum < "$1" | cut -d' ' -f1; else md5 -q "$1"; fi; }
dumpname() { [ "$1" = 1 ] && echo statedump.txt || echo "statedump.$1.txt"; }
# The last frames of a match can be simulated with predicted remote inputs
# that are never corrected: the rollback loop ends with the match, so GGPO
# does not roll back for inputs that arrive later (up to its 8 prediction
# frames). Those frames may differ between the sides (seen with lag: a
# command recognized in the win pose on one side only). TAIL (default 8)
# frames at the end are compared separately: a difference there is reported
# but does not fail the run; everything before must be identical.
TAIL=${TAIL:-8}
frames() { grep -c '^T ' "$1"; }
core() {  # dump [frames] -> md5 of its first FRAMES-TAIL frames (FRAMES: its own count)
    local n=${2:-$(frames "$1")}
    awk -v n=$((n - TAIL)) '/^T /{if (++k > n) exit} {print}' "$1" > "$1.core"
    md5 "$1.core"
    rm -f "$1.core"
}

hogs=""
if [ "${LOAD:-0}" -gt 0 ]; then
    for i in $(seq 1 "$LOAD"); do ( while :; do :; done ) & hogs="$hogs $!"; done
    trap 'kill $hogs 2>/dev/null' EXIT
fi

# instance directory: the game through symlinks, its own save/ and ports
mkinst() {  # dir listen_port rollback_port gamedir
    local g=$4
    rm -rf "$1"
    mkdir -p "$1"
    for d in chars data stages font sound external; do
        [ -e "$g/$d" ] && ln -s "$g/$d" "$1/$d"
    done
    cp -RL "$g/save" "$1/save"
    rm -rf "$1/save/logs" "$1/save/replays"
    mkdir -p "$1/save/logs" "$1/save/replays"
    sed -e "s/^ListenPort .*/ListenPort                     = $2/" \
        -e "s/^Rollback.Port .*/Rollback.Port                  = $3/" \
        -e "s/^ButtonAssist .*/ButtonAssist               = 0/" \
        -e "s/^Rollback.GgpoLogsEnabled .*/Rollback.GgpoLogsEnabled       = ${GGPO_LOGS:-0}/" \
        "$g/save/config.ini" > "$1/save/config.ini"
    [ "${NODUMP:-0}" = 1 ] || : > "$1/vita_statedump.txt"   # write statedump.txt
    : > "$1/vita_statehash.txt"   # fixed seed
    # ButtonAssist (a local setting) keeps state per input poll, not per
    # frame, so with it the scripted inputs would depend on the network
}

# run CMD in DIR with a watchdog; exit status in DIR/status
run() {  # dir env... -- cmd...
    local d=$1; shift
    ( cd "$d" && env "$@" > out.txt 2>&1; echo $? > status ) &
}

wait_pid_timeout() {  # pid seconds
    local i=0
    while kill -0 "$1" 2>/dev/null; do
        sleep 1
        i=$((i + 1))
        if [ $i -ge "$2" ]; then
            pkill -P "$1" 2>/dev/null
            kill "$1" 2>/dev/null
            return 1
        fi
    done
}

fail=0
ref=""
cref=""
NSCEN=$(echo $SCEN | wc -w)
for sc in $SCEN; do
    name=${sc%%:*}
    R="$WORK/$name"
    mkdir -p "$R"
    mkinst "$R/host" $P $((P + 1)) "$G"
    hpeer="" gpeer="" relay="" trelay="" dropsync="" tcplag=""
    if [ "$sc" != direct ]; then
        IFS=: read -r _ delay jitter loss opts <<< "$sc"
        for o in ${opts//:/ }; do
            case $o in
                sync) dropsync=sync ;;
                tcp=*) tcplag=${o#tcp=} ;;
                *) echo "unknown scenario option $o"; exit 2 ;;
            esac
        done
        python3 "$E/tests/netplay/udp_relay.py" $((P + 1)) $((P + 2)) $((P + 3)) $((P + 4)) \
            --delay "$delay" --jitter "$jitter" --loss "$loss" --time "$TIMEOUT" --seed "${RELAY_SEED:-1}" \
            $([ "$dropsync" = sync ] || echo --keep-sync) \
            ${RELAY_LOG:+--log "$R/packets.txt"} > "$R/relay.txt" 2>&1 &
        relay=$!
        hpeer="CVF_NET_PEER_UDP=$((P + 3))" gpeer="CVF_NET_PEER_UDP=$((P + 4))"
    fi
    if [ -n "$tcplag" ]; then
        # the guest connects to the TCP relay, which forwards to the host
        mkinst "$R/guest" $((P + 5)) $((P + 2)) "$GG"
        python3 "$E/tests/netplay/tcp_relay.py" $((P + 5)) $P --delay "${tcplag%/*}" --jitter "${tcplag#*/}" \
            --time "$TIMEOUT" --seed "${RELAY_SEED:-1}" > "$R/tcp_relay.txt" 2>&1 &
        trelay=$!
    else
        mkinst "$R/guest" $P $((P + 2)) "$GG"
    fi
    [ -n "$relay$trelay" ] && sleep 0.5
    gc=""
    [ "${GC_TRACE:-0}" = 1 ] && gc="CVF_GC_TRACE=1"
    t0=$(date +%s)
    run "$R/host" CVF_NET_INPUT=${SEED_HOST:-11} CVF_HEADLESS_FPS=${FPS:-0} CVF_NET_MATCHES="$MATCHES" $gc $hpeer "$HB" . $ARGS -ip ""
    hp=$!
    sleep 0.5
    run "$R/guest" CVF_NET_INPUT=${SEED_GUEST:-22} CVF_HEADLESS_FPS=${FPS:-0} CVF_NET_MATCHES="$MATCHES" $gc $gpeer "$GB" . $ARGS -ip 127.0.0.1
    gp=$!
    wait_pid_timeout $gp "$TIMEOUT" || echo "$name: guest timed out"
    wait_pid_timeout $hp 30 || echo "$name: host timed out"
    t1=$(date +%s)
    for r in $relay $trelay; do
        kill $r 2>/dev/null
        wait $r 2>/dev/null
    done
    hs=$(cat "$R/host/status" 2>/dev/null || echo -)
    gs=$(cat "$R/guest/status" 2>/dev/null || echo -)
    echo "== $name ($sc, $((t1 - t0)) s)"
    ok=1
    [ "$hs" = 0 ] && [ "$gs" = 0 ] || ok=0
    sums="" csums=""
    for k in $(seq 1 $NM); do
        f=$(dumpname $k)
        hm=$(md5 "$R/host/$f" 2>/dev/null)
        gm=$(md5 "$R/guest/$f" 2>/dev/null)
        if [ $NM = 1 ]; then
            hw=$(grep '^winner:' "$R/host/out.txt" | tail -1)
            gw=$(grep '^winner:' "$R/guest/out.txt" | tail -1)
            echo "  host : exit $hs, $hw, $(grep 'statedump:' "$R/host/out.txt" | sed 's/.*statedump: //'), md5 $hm"
            echo "  guest: exit $gs, $gw, $(grep 'statedump:' "$R/guest/out.txt" | sed 's/.*statedump: //'), md5 $gm"
        else
            pair=$(echo "$(echo " $ARGS " | sed -n 's/.* -p1 \([^ ]*\).* -p2 \([^ ]*\) .*/\1:\2/p') $MATCHES" | awk -v k=$k '{print $k}')
            for s in host guest; do
                [ $s = host ] && m=$hm || m=$gm
                echo "  $k $pair $s: $(grep 'statedump:' "$R/$s/out.txt" | sed -n "${k}p" | sed 's/.*statedump: //'), md5 $m; $(grep "session: match $k done" "$R/$s/out.txt" | sed 's/.*done: //')"
            done
        fi
        if [ "${NODUMP:-0}" = 1 ]; then
            pat="session: match $k done"
            [ $NM = 1 ] && pat="^winner:"
            hm=$(grep "$pat" "$R/host/out.txt" | tail -1 | grep -o 'winner:* [-0-9]*')
            gm=$(grep "$pat" "$R/guest/out.txt" | tail -1 | grep -o 'winner:* [-0-9]*')
            hm=${hm//[: ]/} gm=${gm//[: ]/}
        fi
        cm=$hm
        if [ "${NODUMP:-0}" != 1 ] && [ -n "$hm" ] && { [ "$hm" != "$gm" ] || [ $NSCEN -gt 1 ]; }; then
            nf=$(frames "$R/host/$f")
            ng=$(frames "$R/guest/$f")
            [ "$ng" -lt "$nf" ] && nf=$ng   # one side may have simulated a frame more at the end
            cm=$(core "$R/host/$f" "$nf")
            if [ -n "$gm" ] && [ "$hm" != "$gm" ] && [ "$cm" = "$(core "$R/guest/$f" "$nf")" ]; then
                echo "  match $k: differs only in its last $TAIL frames (predicted inputs at match end, never rolled back); identical before"
                hm="tail" gm="tail"
            fi
        fi
        csums="$csums $cm"
        [ -n "$hm" ] && [ "$hm" = "$gm" ] || ok=0
        [ "${NODUMP:-0}" = 1 ] || [ -s "$R/host/$f" ] || ok=0
        sums="$sums $hm"
        if [ $ok = 0 ] && [ -s "$R/host/$f" ] && [ -s "$R/guest/$f" ]; then
            cmp "$R/host/$f" "$R/guest/$f" | sed "s/^/  match $k: /"
        fi
    done
    [ -n "$relay" ] && echo "  $(cat "$R/relay.txt")"
    [ -n "$trelay" ] && echo "  $(cat "$R/tcp_relay.txt")"
    grep -h "EventCodeDesync\|desync\|deadlock\|panic\|Synchronization error\|mismatch" "$R/host/out.txt" "$R/guest/out.txt" | sort | uniq -c | sed 's/^/  /'
    if [ $ok = 1 ] && [ -z "$ref" ]; then ref=$sums; fi
    # across scenarios: the dumps without their tails must agree
    [ -z "$cref" ] && [ $ok = 1 ] && cref=$csums
    if [ $ok = 1 ] && [ "$csums" != "$cref" ]; then
        echo "  differs from the first scenario's dumps ($ref)"
        ok=0
    fi
    [ $ok = 0 ] && fail=1
    [ "${CLEAN:-0}" = 1 ] && rm -f "$R"/host/statedump*.txt "$R"/guest/statedump*.txt
done
echo "work: $WORK"
if [ $fail = 0 ]; then echo "netplay: PASS (dump$ref)"; else echo "netplay: FAIL"; exit 1; fi
