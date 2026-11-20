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
#            NAME:DELAY:JITTER:LOSS
#                              through tests/netplay/udp_relay.py: delay and
#                              jitter in ms, loss in percent
#            default: direct lag:40:15:0 loss:20:10:8
#
# Environment:
#   HOST_BIN, GUEST_BIN  engines (default build/release/cvf_headless; the Go
#                        build cvf_headless_go works too, see DEVSETUP.md)
#   WORK                 work directory (default: a new temporary one)
#   PORT_BASE            TCP port; base+1..base+4 are UDP (default 7650)
#   SEED_HOST, SEED_GUEST  input scripts (CVF_NET_INPUT, default 11 / 22)
#   FPS                  CVF_HEADLESS_FPS for both (default 0: unpaced)
#   ARGS                 match arguments (default: Alucard vs Richter,
#                        ParanoiaBG, 1 round)
#   TIMEOUT              seconds per match (default 600)
#   GGPO_LOGS=1          GGPO's trace in each instance's save/logs
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
P=${PORT_BASE:-7650}
ARGS=${ARGS:--p1 Alucard -p2 Richter -s stages/ParanoiaBG.def -rounds 1}
TIMEOUT=${TIMEOUT:-600}
WORK=${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/cvf_netplay.XXXXXX")}
mkdir -p "$WORK"
md5() { if command -v md5sum >/dev/null; then md5sum < "$1" | cut -d' ' -f1; else md5 -q "$1"; fi; }

# instance directory: the game through symlinks, its own save/ and ports
mkinst() {  # dir listen_port rollback_port
    rm -rf "$1"
    mkdir -p "$1"
    for d in chars data stages font sound external; do
        [ -e "$G/$d" ] && ln -s "$G/$d" "$1/$d"
    done
    cp -R "$G/save" "$1/save"
    rm -rf "$1/save/logs" "$1/save/replays"
    mkdir -p "$1/save/logs" "$1/save/replays"
    sed -e "s/^ListenPort .*/ListenPort                     = $2/" \
        -e "s/^Rollback.Port .*/Rollback.Port                  = $3/" \
        -e "s/^ButtonAssist .*/ButtonAssist               = 0/" \
        -e "s/^Rollback.GgpoLogsEnabled .*/Rollback.GgpoLogsEnabled       = ${GGPO_LOGS:-0}/" \
        "$G/save/config.ini" > "$1/save/config.ini"
    : > "$1/vita_statedump.txt"   # write statedump.txt
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
for sc in $SCEN; do
    name=${sc%%:*}
    R="$WORK/$name"
    mkdir -p "$R"
    mkinst "$R/host" $P $((P + 1))
    mkinst "$R/guest" $P $((P + 2))
    hpeer="" gpeer="" relay=""
    if [ "$sc" != direct ]; then
        IFS=: read -r _ delay jitter loss <<< "$sc"
        python3 "$E/tests/netplay/udp_relay.py" $((P + 1)) $((P + 2)) $((P + 3)) $((P + 4)) \
            --delay "$delay" --jitter "$jitter" --loss "$loss" --time "$TIMEOUT" > "$R/relay.txt" 2>&1 &
        relay=$!
        hpeer="CVF_NET_PEER_UDP=$((P + 3))" gpeer="CVF_NET_PEER_UDP=$((P + 4))"
        sleep 0.5
    fi
    t0=$(date +%s)
    run "$R/host" CVF_NET_INPUT=${SEED_HOST:-11} CVF_HEADLESS_FPS=${FPS:-0} $hpeer "$HB" . $ARGS -ip ""
    hp=$!
    sleep 0.5
    run "$R/guest" CVF_NET_INPUT=${SEED_GUEST:-22} CVF_HEADLESS_FPS=${FPS:-0} $gpeer "$GB" . $ARGS -ip 127.0.0.1
    gp=$!
    wait_pid_timeout $gp "$TIMEOUT" || echo "$name: guest timed out"
    wait_pid_timeout $hp 30 || echo "$name: host timed out"
    t1=$(date +%s)
    if [ -n "$relay" ]; then
        kill $relay 2>/dev/null
        wait $relay 2>/dev/null
    fi
    hs=$(cat "$R/host/status" 2>/dev/null || echo -)
    gs=$(cat "$R/guest/status" 2>/dev/null || echo -)
    hm=$(md5 "$R/host/statedump.txt" 2>/dev/null)
    gm=$(md5 "$R/guest/statedump.txt" 2>/dev/null)
    hw=$(grep '^winner:' "$R/host/out.txt" | tail -1)
    gw=$(grep '^winner:' "$R/guest/out.txt" | tail -1)
    echo "== $name ($sc, $((t1 - t0)) s)"
    echo "  host : exit $hs, $hw, $(grep 'statedump:' "$R/host/out.txt" | sed 's/.*statedump: //'), md5 $hm"
    echo "  guest: exit $gs, $gw, $(grep 'statedump:' "$R/guest/out.txt" | sed 's/.*statedump: //'), md5 $gm"
    [ -n "$relay" ] && echo "  $(cat "$R/relay.txt")"
    grep -h "EventCodeDesync\|desync\|deadlock\|panic" "$R/host/out.txt" "$R/guest/out.txt" | sort | uniq -c | sed 's/^/  /'
    ok=1
    [ "$hs" = 0 ] && [ "$gs" = 0 ] || ok=0
    [ -n "$hm" ] && [ "$hm" = "$gm" ] || ok=0
    [ -s "$R/host/statedump.txt" ] || ok=0
    if [ $ok = 1 ] && [ -z "$ref" ]; then ref=$hm; fi
    if [ $ok = 1 ] && [ "$hm" != "$ref" ]; then
        echo "  differs from the first scenario's dump ($ref)"
        ok=0
    fi
    if [ $ok = 0 ]; then
        fail=1
        if [ -s "$R/host/statedump.txt" ] && [ -s "$R/guest/statedump.txt" ]; then
            cmp "$R/host/statedump.txt" "$R/guest/statedump.txt" | sed 's/^/  /'
        fi
    fi
done
echo "work: $WORK"
if [ $fail = 0 ]; then echo "netplay: PASS (dump $ref)"; else echo "netplay: FAIL"; exit 1; fi
