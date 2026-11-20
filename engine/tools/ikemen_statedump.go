package main

import (
	"bufio"
	"math"
	"os"
	"sort"
	"strconv"
)

// Reference state dump (ux0:data/CvF/vita_statedump.txt, with
// vita_statehash.txt for the fixed seed): one block per game tick with the
// full observable state of every character, written to statedump.txt. These
// dumps are the golden references the native C engine is checked against.
// Floats are written as their IEEE bit patterns so comparisons are exact.

var stateDumpOn = -1
var stateDumpW *bufio.Writer
var stateDumpF *os.File
var stateDumpTick int
var stateDumpBuf []byte

func stateDumpEnabled() bool {
	if stateDumpOn < 0 {
		stateDumpOn = 0
		if FileExist("vita_statedump.txt") != "" {
			if f, err := os.Create("statedump.txt"); err == nil {
				stateDumpF = f
				stateDumpW = bufio.NewWriterSize(f, 1<<16)
				stateDumpOn = 1
			}
		}
	}
	return stateDumpOn == 1
}

func sdF(b []byte, f float32) []byte {
	return strconv.AppendUint(b, uint64(math.Float32bits(f)), 16)
}

func sdI(b []byte, v int64) []byte { return strconv.AppendInt(b, v, 10) }

func stateDumpTickFn() {
	if !stateDumpEnabled() {
		return
	}
	stateDumpTick++
	// Online (rollback) match: frames are simulated again after a rollback,
	// so keep the last simulation of each match frame and write them in
	// order when the match ends (stateDumpFinish).
	net := sys.rollback.session != nil && sys.rollback.netConnection != nil
	b := stateDumpBuf[:0]
	b = append(b, "T "...)
	if net {
		b = sdI(b, int64(sys.matchTime))
	} else {
		b = sdI(b, int64(stateDumpTick))
	}
	b = append(b, " rs "...)
	b = sdI(b, int64(sys.roundState()))
	b = append(b, " gt "...)
	b = sdI(b, int64(sys.gameTime()))
	b = append(b, " seed "...)
	b = sdI(b, int64(sys.randseed))
	b = append(b, '\n')
	for pn := 0; pn < len(sys.chars); pn++ {
		for _, c := range sys.chars[pn] {
			if c == nil {
				continue
			}
			b = append(b, "C "...)
			b = sdI(b, int64(pn))
			b = append(b, ' ')
			b = sdI(b, int64(c.helperIndex))
			b = append(b, " id "...)
			b = sdI(b, int64(c.id))
			b = append(b, " hid "...)
			b = sdI(b, int64(c.helperId))
			b = append(b, " st "...)
			b = sdI(b, int64(c.ss.no))
			b = append(b, ' ')
			b = sdI(b, int64(c.ss.prevno))
			b = append(b, ' ')
			b = sdI(b, int64(c.ss.time))
			b = append(b, " t "...)
			b = sdI(b, int64(c.ss.stateType))
			b = append(b, ',')
			b = sdI(b, int64(c.ss.moveType))
			b = append(b, ',')
			b = sdI(b, int64(c.ss.physics))
			b = append(b, " an "...)
			b = sdI(b, int64(c.animNo))
			b = append(b, ' ')
			b = sdI(b, int64(c.animElemNo(0).ToI()))
			b = append(b, ' ')
			b = sdI(b, int64(c.animElemTime(c.animElemNo(0).ToI()).ToI()))
			b = append(b, " p "...)
			for i := 0; i < 3; i++ {
				b = sdF(b, c.pos[i])
				b = append(b, ' ')
			}
			b = append(b, "v "...)
			for i := 0; i < 3; i++ {
				b = sdF(b, c.vel[i])
				b = append(b, ' ')
			}
			b = append(b, "f "...)
			b = sdI(b, int64(c.facing))
			b = append(b, " l "...)
			b = sdI(b, int64(c.life))
			b = append(b, " pw "...)
			b = sdI(b, int64(c.getPower()))
			b = append(b, " c "...)
			if c.ctrl() {
				b = append(b, '1')
			} else {
				b = append(b, '0')
			}
			b = stateDumpVarsI(b, " var", c.cnsvar)
			b = stateDumpVarsF(b, " fv", c.cnsfvar)
			b = stateDumpVarsI(b, " sv", c.cnssysvar)
			b = stateDumpVarsF(b, " sf", c.cnssysfvar)
			b = append(b, '\n')
		}
		if pn < len(sys.explods) && len(sys.explods[pn]) > 0 {
			b = append(b, "E "...)
			b = sdI(b, int64(pn))
			for _, e := range sys.explods[pn] {
				b = append(b, ' ')
				b = sdI(b, int64(e.id))
				b = append(b, ':')
				b = sdI(b, int64(e.animNo))
				b = append(b, ':')
				b = sdF(b, e.pos[0])
				b = append(b, ',')
				b = sdF(b, e.pos[1])
			}
			b = append(b, '\n')
		}
		if pn < len(sys.projs) && len(sys.projs[pn]) > 0 {
			b = append(b, "P "...)
			b = sdI(b, int64(pn))
			for _, p := range sys.projs[pn] {
				b = append(b, ' ')
				b = sdI(b, int64(p.id))
				b = append(b, ':')
				b = sdF(b, p.pos[0])
				b = append(b, ',')
				b = sdF(b, p.pos[1])
			}
			b = append(b, '\n')
		}
	}
	stateDumpBuf = b
	if net {
		if stateDumpNet == nil {
			stateDumpNet = make(map[int32][]byte)
		}
		if sys.rollback.session.inRollback {
			stateDumpNetResim++
		}
		stateDumpNet[sys.matchTime] = append([]byte(nil), b...)
		return
	}
	stateDumpW.Write(b)
	if stateDumpTick%60 == 0 {
		stateDumpW.Flush()
	}
}

var sdKeys []int32

var stateDumpNet map[int32][]byte
var stateDumpNetResim int

// At the end of the run: writes the frames of an online match in
// match-time order (the last simulation of each one) and flushes the dump.
func stateDumpFinish() {
	if stateDumpOn != 1 {
		return
	}
	if stateDumpNet == nil {
		stateDumpW.Flush()
		return
	}
	keys := make([]int32, 0, len(stateDumpNet))
	for k := range stateDumpNet {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })
	LogMessage("statedump: %d match frames, %d frames simulated again in rollbacks", len(keys), stateDumpNetResim)
	for _, k := range keys {
		stateDumpW.Write(stateDumpNet[k])
	}
	stateDumpNet = nil
	stateDumpW.Flush()
}

func stateDumpVarsI(b []byte, tag string, m map[int32]int32) []byte {
	sdKeys = sdKeys[:0]
	for k, v := range m {
		if v != 0 {
			sdKeys = append(sdKeys, k)
		}
	}
	if len(sdKeys) == 0 {
		return b
	}
	sort.Slice(sdKeys, func(i, j int) bool { return sdKeys[i] < sdKeys[j] })
	b = append(b, tag...)
	for _, k := range sdKeys {
		b = append(b, ' ')
		b = sdI(b, int64(k))
		b = append(b, '=')
		b = sdI(b, int64(m[k]))
	}
	return b
}

func stateDumpVarsF(b []byte, tag string, m map[int32]float32) []byte {
	sdKeys = sdKeys[:0]
	for k, v := range m {
		if v != 0 {
			sdKeys = append(sdKeys, k)
		}
	}
	if len(sdKeys) == 0 {
		return b
	}
	sort.Slice(sdKeys, func(i, j int) bool { return sdKeys[i] < sdKeys[j] })
	b = append(b, tag...)
	for _, k := range sdKeys {
		b = append(b, ' ')
		b = sdI(b, int64(k))
		b = append(b, '=')
		b = sdF(b, m[k])
	}
	return b
}
