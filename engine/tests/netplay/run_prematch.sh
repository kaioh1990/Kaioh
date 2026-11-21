#!/bin/bash
# Repeats the TCP part of an online match start many times: connect, sync
# settings and content fingerprint, seed and pre-match time, the delay-netplay
# frames before the match, the character loading and the second Synchronize()
# at match start. Each run stops once both sides print "Rollback startup"
# (GGPO is not exercised: run_netplay.sh does that).
#
#   tests/netplay/run_prematch.sh GAMEDIR RUNS
#
# It found the "Synchronization error" race (GAPS.md): a peer's last input
# frames and its stop marker arriving during one System.await left the other
# side a frame behind, and the next time check failed. Unfixed builds fail in
# a few percent of runs with LOAD=3 on a two-core machine.
#
# Environment: HOST_BIN, GUEST_BIN, PORT_BASE (default 7680; base+1, +2 UDP,
# +5 the TCP relay), WORK, FPS, ARGS as run_netplay.sh, and
#   LOAD=N       N busy loops beside the engines
#   TCP=D/J      the TCP connection through tcp_relay.py, D +- J ms (seed =
#                run number)
#   GUEST_GAME   the guest's game directory (content fingerprint tests)
#   EXPECT       "ok" (default): every run must reach the match; or
#                "mismatch": every run must stop with "Game data mismatch"
#                on both sides (and never reach the match)
set -u
E=$(cd "$(dirname "$0")/../.." && pwd)
G=$(cd "${1:?usage: run_prematch.sh GAMEDIR RUNS}" && pwd)
N=${2:?usage: run_prematch.sh GAMEDIR RUNS}
GG=$(cd "${GUEST_GAME:-$G}" && pwd)
HB=${HOST_BIN:-$E/build/release/cvf_headless}
GB=${GUEST_BIN:-$E/build/release/cvf_headless}
P=${PORT_BASE:-7680}
ARGS=${ARGS:--p1 Alucard -p2 Richter -s stages/ParanoiaBG.def -rounds 1}
EXPECT=${EXPECT:-ok}
WORK=${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/cvf_prematch.XXXXXX")}
mkdir -p "$WORK"

hogs=""
if [ "${LOAD:-0}" -gt 0 ]; then
    for i in $(seq 1 "$LOAD"); do ( while :; do :; done ) & hogs="$hogs $!"; done
fi
cleanup() { [ -n "$hogs" ] && kill $hogs 2>/dev/null; }
trap cleanup EXIT

mkinst() {  # dir listen_port rollback_port gamedir
    rm -rf "$1"; mkdir -p "$1"
    for d in chars data stages font sound external; do
        [ -e "$4/$d" ] && ln -s "$4/$d" "$1/$d"
    done
    cp -RL "$4/save" "$1/save"
    rm -rf "$1/save/logs" "$1/save/replays"; mkdir -p "$1/save/logs" "$1/save/replays"
    sed -e "s/^ListenPort .*/ListenPort                     = $2/" \
        -e "s/^Rollback.Port .*/Rollback.Port                  = $3/" \
        -e "s/^ButtonAssist .*/ButtonAssist               = 0/" "$4/save/config.ini" > "$1/save/config.ini"
    : > "$1/vita_statedump.txt"; : > "$1/vita_statehash.txt"
}
GP=$P
[ -n "${TCP:-}" ] && GP=$((P + 5))
mkinst "$WORK/host" $P $((P + 1)) "$G"
mkinst "$WORK/guest" $GP $((P + 2)) "$GG"

END="Rollback startup\|Synchronization error\|^winner\|Game data mismatch\|Content/build mismatch"
ok=0 bad=0 mism=0 other=0
for i in $(seq 1 "$N"); do
    relay=""
    if [ -n "${TCP:-}" ]; then
        python3 "$E/tests/netplay/tcp_relay.py" $((P + 5)) $P --delay "${TCP%/*}" --jitter "${TCP#*/}" \
            --seed "$i" --time 120 > "$WORK/tcp_relay.txt" 2>&1 &
        relay=$!
        sleep 0.2
    fi
    rm -f "$WORK/host/out.txt" "$WORK/guest/out.txt"
    (cd "$WORK/host" && exec env CVF_NET_INPUT=11 CVF_HEADLESS_FPS=${FPS:-0} "$HB" . $ARGS -ip "" > out.txt 2>&1) &
    hp=$!
    sleep 0.3
    (cd "$WORK/guest" && exec env CVF_NET_INPUT=22 CVF_HEADLESS_FPS=${FPS:-0} "$GB" . $ARGS -ip 127.0.0.1 > out.txt 2>&1) &
    gp=$!
    for t in $(seq 1 600); do
        sleep 0.1
        grep -q "$END" "$WORK/host/out.txt" 2>/dev/null && grep -q "$END" "$WORK/guest/out.txt" 2>/dev/null && break
        kill -0 $hp 2>/dev/null || kill -0 $gp 2>/dev/null || break
    done
    kill $hp $gp 2>/dev/null
    wait $hp $gp 2>/dev/null
    if [ -n "$relay" ]; then kill $relay 2>/dev/null; wait $relay 2>/dev/null; fi
    h=$WORK/host/out.txt g=$WORK/guest/out.txt
    if grep -q "Game data mismatch\|Content/build mismatch" "$h" && grep -q "Game data mismatch\|Content/build mismatch" "$g"; then
        mism=$((mism + 1))
        [ $i = 1 ] && echo "  host : $(grep -m1 -o 'Game data mismatch.*\|Content/build mismatch.*' "$h")" &&
            echo "  guest: $(grep -m1 -o 'Game data mismatch.*\|Content/build mismatch.*' "$g")"
        r=mismatch
    elif grep -q "Synchronization error" "$h" "$g"; then
        bad=$((bad + 1)); r="SYNC ERROR"
    elif grep -q "Rollback startup" "$h" && grep -q "Rollback startup" "$g"; then
        ok=$((ok + 1)); r=ok
    else
        other=$((other + 1)); r=other
    fi
    want=ok
    [ "$EXPECT" = mismatch ] && want=mismatch
    if [ "$r" != "$want" ]; then
        mkdir -p "$WORK/run$i"
        cp "$h" "$WORK/run$i/host.txt"; cp "$g" "$WORK/run$i/guest.txt"
        echo "run $i: $r (logs in $WORK/run$i)"
    fi
done
echo "work: $WORK"
echo "prematch: $N runs: ok $ok, sync error $bad, mismatch $mism, other $other"
if [ "$EXPECT" = mismatch ]; then [ $mism = "$N" ]; else [ $ok = "$N" ]; fi && echo "prematch: PASS" || { echo "prematch: FAIL"; exit 1; }
