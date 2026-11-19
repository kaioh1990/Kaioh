package main

import (
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
)

type TransType int32

const (
	TT_none TransType = iota
	TT_add
	TT_sub
	TT_subadd
	TT_default
)

type Error string

func (e Error) Error() string { return string(e) }
func LogMessage(format string, a ...any) {}

type Sff struct{}
type PaletteList struct{}
type Sprite struct{}
type Tiling struct{}
type Rotation struct{}
type RemapPreset map[int]int

func NormalizeRect(rect [4]float32) [4]float32 {
	if rect[0] > rect[2] {
		rect[0], rect[2] = rect[2], rect[0]
	}
	if rect[1] > rect[3] {
		rect[1], rect[3] = rect[3], rect[1]
	}
	return rect
}

var _ = math.Pi

const IMax = int32(math.MaxInt32)
const IErr = ^IMax
func IsNumeric(s string) bool {
	_, err := strconv.ParseFloat(strings.TrimSpace(s), 64)
	return err == nil
}
func Atoi(str string) int32 {
	var n int64
	str = strings.TrimSpace(str)
	if len(str) > 0 {
		var a string
		if str[0] == '-' || str[0] == '+' {
			a = str[1:]
		} else {
			a = str
		}
		// Negative side allows one more (-2147483648 is a valid int32)
		limit := int64(math.MaxInt32)
		if str[0] == '-' {
			limit++
		}
		for i := range a {
			if a[i] < '0' || '9' < a[i] {
				break
			}
			n = n*10 + int64(a[i]-'0')
			if n > limit {
				
				LogMessage("Atoi conversion outside int32 range: %v", a[:i+1])
				if str[0] == '-' {
					return IErr
				}
				return IMax
			}
		}
		if str[0] == '-' {
			n *= -1
		}
	}
	return int32(n)
}
func Atof(str string) float64 {
	f := 0.0
	str = strings.TrimSpace(str)
	if len(str) > 0 {
		var a string
		if str[0] == '-' || str[0] == '+' {
			a = str[1:]
		} else {
			a = str
		}
		i, p := 0, 0
		for ; i < len(a); i++ {
			if a[i] == '.' {
				if p != 0 {
					break
				}
				p = i + 1
				continue
			}
			if a[i] < '0' || '9' < a[i] {
				break
			}
			f = f*10 + float64(a[i]-'0')
		}
		e := 0.0
		if i+1 < len(a) && (a[i] == 'e' || a[i] == 'E') {
			j := i + 1
			if a[j] == '-' || a[j] == '+' {
				j++
			}
			for ; j < len(a) && '0' <= a[j] && a[j] <= '9'; j++ {
				e = e*10 + float64(a[j]-'0')
			}
			if e != 0 {
				if str[i+1] == '-' {
					e *= -1
				}
				if p == 0 {
					p = i
				}
			}
		}
		if p > 0 {
			f *= math.Pow10(p - i + int(e))
		}
		if str[0] == '-' {
			f *= -1
		}
	}
	return f
}
func SplitAndTrim(str, sep string) (ss []string) {
	ss = strings.Split(str, sep)
	for i, s := range ss {
		ss[i] = strings.TrimSpace(s)
	}
	if false {
		// TinyGo's GC finds an object's head by walking back from an interior
		// pointer, so thousands of line substrings pointing into one multi-MB
		// file string made every collection quadratic (62% of character
		// compile time on the Vita). Give each line its own allocation.
		for i, s := range ss {
			if s != "" {
				ss[i] = strings.Clone(s)
			}
		}
	}
	return
}
func SectionName(sec string) (string, string) {
	if len(sec) == 0 || sec[0] != '[' {
		return "", ""
	}
	sec = strings.TrimSpace(strings.SplitN(sec, ";", 2)[0])
	if sec[len(sec)-1] != ']' {
		return "", ""
	}
	sec = sec[1:strings.Index(sec, "]")]
	var name string
	i := strings.Index(sec, " ")
	if i >= 0 {
		name = sec[:i+1]
		sec = sec[i+1:]
	} else {
		name = sec
		sec = ""
	}
	return strings.ToLower(name), sec
}
// AnimFrame holds frame data, used in animation tables.
type AnimFrame struct {
	Time      int32
	Group     int32
	Number    int32
	Xoffset   int32
	Yoffset   int32
	TransType TransType
	SrcAlpha  byte
	DstAlpha  byte
	Hscale    int8
	Vscale    int8
	Xscale    float32
	Yscale    float32
	Angle     float32
	Clsn1     [][4]float32
	Clsn2     [][4]float32
}

func newAnimFrame() *AnimFrame {
	return &AnimFrame{
		Time:      -1,
		Group:     -1,
		TransType: TT_none,
		SrcAlpha:  255,
		DstAlpha:  0,
		Hscale:    1, // These two are technically flags but are coded like scale for simplicity
		Vscale:    1,
		Xscale:    1,
		Yscale:    1,
		Angle:     0,
	}
}

func ReadAnimFrame(line string) (*AnimFrame, error) {
	// Valid lines must start with a number or '-'
	if len(line) == 0 || (line[0] < '0' || '9' < line[0]) && line[0] != '-' {
		return nil, nil
	}

	// Split by commas then trim spaces
	raw := strings.SplitN(line, ",", 10)
	ary := make([]string, len(raw))
	for i, s := range raw {
		ary[i] = strings.TrimSpace(s)
	}

	if len(ary) < 5 {
		return nil, Error("Invalid animation element definition")
	}

	var err error

	// Read required parameters
	af := newAnimFrame()
	af.Group = int32(Atoi(ary[0]))
	af.Number = int32(Atoi(ary[1]))
	af.Xoffset = int32(Atoi(ary[2]))
	af.Yoffset = int32(Atoi(ary[3]))
	af.Time = Atoi(ary[4])

	// Read H and V flags
	if len(ary) >= 6 && ary[5] != "" {
		for _, ch := range ary[5] {
			switch ch {
			case 'H', 'h':
				af.Hscale = -1
				af.Xoffset *= -1
			case 'V', 'v':
				af.Vscale = -1
				af.Yoffset *= -1
			default:
				err = Error(fmt.Sprintf("Invalid animation element flip flag: %c", ch))
			}
		}
	}

	// Read alpha
	if len(ary) >= 7 && ary[6] != "" {
		// Helper to read source and destination
		parseAlphaNumbers := func(a string, start int, defSrc, defDst byte) (src, dst byte) {
			src, dst = defSrc, defDst
			i, alp := start, 0
			// Parse source alpha
			for ; i < len(a) && a[i] >= '0' && a[i] <= '9'; i++ {
				alp = alp*10 + int(a[i]-'0')
			}
			alp &= 0x3fff // Kind of redundant with the next step, but harmless
			if alp > 255 {
				alp = 255
			}
			src = byte(alp)
			// Parse destination alpha
			if i < len(a) && a[i] == 'd' {
				i++
				if i < len(a) && a[i] >= '0' && a[i] <= '9' {
					alp = 0
					for ; i < len(a) && a[i] >= '0' && a[i] <= '9'; i++ {
						alp = alp*10 + int(a[i]-'0')
					}
					alp &= 0x3fff
					if alp > 255 {
						alp = 255
					}
					dst = byte(alp)
				}
			}
			return
		}

		// Helper for the repetitive error
		transErr := func(s string) {
			err = Error(fmt.Sprintf("Invalid animation element transparency: %v", s))
		}

		ia := strings.IndexAny(ary[6], "ASas")
		if ia >= 0 {
			ary[6] = ary[6][ia:]
		}
		a := strings.ToLower(ary[6])
		switch {
		case a == "a1":
			af.TransType = TT_add
			af.SrcAlpha = 255
			af.DstAlpha = 128

		case len(a) >= 3 && a[:3] == "sas": // SubAdd with alpha
			af.TransType = TT_subadd
			af.SrcAlpha, af.DstAlpha = parseAlphaNumbers(a, 3, 255, 255)

		case len(a) >= 2 && a[:2] == "sa": // Plain SubAdd
			af.TransType = TT_subadd
			af.SrcAlpha = 255
			af.DstAlpha = 255
			if len(a) > 2 {
				transErr(a)
			}

		case len(a) >= 2 && a[:2] == "as": // Add with alpha
			af.TransType = TT_add
			af.SrcAlpha, af.DstAlpha = parseAlphaNumbers(a, 2, 255, 0) // Mugen defaults to 0 here

		case len(a) >= 2 && a[:2] == "ss": // Sub with alpha
			af.TransType = TT_sub
			af.SrcAlpha, af.DstAlpha = parseAlphaNumbers(a, 2, 255, 255) // This is new so we can default properly

		case len(a) >= 1 && a[0] == 'a': // Plain Add
			af.TransType = TT_add
			af.SrcAlpha = 255
			af.DstAlpha = 255
			// The first letter is enough in Mugen, but we will warn to encourage proper syntax
			// https://github.com/ikemen-engine/Ikemen-GO/issues/3717
			if len(a) > 1 {
				transErr(a)
			}

		case len(a) >= 1 && a[0] == 's': // Plain Sub
			af.TransType = TT_sub
			af.SrcAlpha = 255
			af.DstAlpha = 255
			if len(a) > 1 {
				transErr(a)
			}

		default:
			transErr(a)
		}
	}

	// Read X scale
	// In Mugen 1.1 a blank parameter means 0
	// In Ikemen it means no change, like the other optional parameters
	if len(ary) >= 8 && ary[7] != "" {
		if IsNumeric(ary[7]) {
			af.Xscale = float32(Atof(ary[7]))
		} else {
			err = Error(fmt.Sprintf("Invalid animation element x-scale: %v", ary[7]))
		}
	}

	// Read Y scale
	if len(ary) >= 9 && ary[8] != "" {
		if IsNumeric(ary[8]) {
			af.Yscale = float32(Atof(ary[8]))
		} else {
			err = Error(fmt.Sprintf("Invalid animation element y-scale: %v", ary[8]))
		}
	}

	// Read angle
	if len(ary) >= 10 && ary[9] != "" {
		if IsNumeric(ary[9]) {
			af.Angle = float32(Atof(ary[9]))
		} else {
			err = Error(fmt.Sprintf("Invalid animation element angle: %v", ary[9]))
		}
	}

	return af, err
}
type Animation struct {
	sff                        *Sff
	palettedata                *PaletteList
	spr                        *Sprite
	frames                     []AnimFrame
	tile                       Tiling
	loopstart                  int32
	interpolate_offset         []int32
	interpolate_scale          []int32
	interpolate_angle          []int32
	interpolate_blend          []int32
	curtime                    int32
	curelem                    int32
	curelemtime                int32
	lastActionFrame            int32
	drawidx                    int32
	totaltime                  int32
	looptime                   int32
	prelooptime                int32
	mask                       int16
	transType                  TransType
	srcAlpha                   int16
	dstAlpha                   int16
	newframe                   bool
	loopend                    bool
	interpolate_offset_x       float32
	interpolate_offset_y       float32
	scale_x                    float32
	scale_y                    float32
	rot                        Rotation
	curtrans                   TransType
	interpolate_blend_srcalpha float32
	interpolate_blend_dstalpha float32
	remap                      RemapPreset
	start_scale                [2]float32
	isParallax                 bool
	parallaxRot                bool
	isVideo                    bool // Because videos are rendered through Animation.Draw()
	phantomPixel               bool
	copyAction                 int32
	warnMissing                bool
	loopcount                  int32
	filename                   string
	actionNumber               int32
}
func newAnimation(sff *Sff, pal *PaletteList) *Animation {
	return &Animation{
		sff:                        sff,
		palettedata:                pal,
		mask:                       -1,
		transType:                  TT_default,
		srcAlpha:                   255,
		dstAlpha:                   0,
		curtrans:                   TT_none,
		interpolate_blend_srcalpha: 255,
		interpolate_blend_dstalpha: 0,
		newframe:                   true,
		remap:                      make(RemapPreset),
		start_scale:                [...]float32{1, 1},
		lastActionFrame:            -1,
		copyAction:                 -1,
		warnMissing:                true,
	}
}
type AnimCompiler struct {
	sff       *Sff
	pal       *PaletteList
	at        AnimationTable
	seen      map[int32]bool // Actions found in the file being compiled, for duplicate checks
	lines     []string
	i         int // Line index
	filename  string
	mainFile  string // First file compiled, in case common animations are used
	animNo    int32
	log       bool
	finalized bool
}

func newAnimCompiler(sff *Sff, pal *PaletteList, log bool) *AnimCompiler {
	return &AnimCompiler{
		sff:  sff,
		pal:  pal,
		at:   NewAnimationTable(),
		seen: make(map[int32]bool),
		log:  log,
	}
}
func (ac *AnimCompiler) warnf(format string, args ...interface{}) {
	if !ac.log {
		return
	}
	LogMessage("WARNING: %v, action %v: %v", ac.filename, ac.animNo, fmt.Sprintf(format, args...))
}

func (ac *AnimCompiler) errorf(line int, format string, args ...interface{}) error {
	return Error(fmt.Sprintf("%v, action %v, line %v: %v",
		ac.filename, ac.animNo, line, fmt.Sprintf(format, args...)))
}
func (ac *AnimCompiler) compileText(filename, text string) {
	// The resolved table shares its map with the caller, so compiling again would mutate it
	if ac.finalized {
		LogMessage("WARNING: %v compiled after the table was resolved (ignored)", filename)
		return
	}
	ac.filename = filename
	if ac.mainFile == "" {
		ac.mainFile = filename
	}
	ac.seen = make(map[int32]bool)
	ac.lines, ac.i = SplitAndTrim(text, "\n"), 0
	for ac.compileAction() != nil {
	}
}

// Finalizes the table, resolving Copy Action across every compiled file
// The compiler is spent afterwards and must not compile anything else
func (ac *AnimCompiler) finalize() AnimationTable {
	ac.at.resolveCopyAction()
	ac.at.filename = ac.mainFile
	ac.finalized = true
	return ac.at
}
func (ac *AnimCompiler) readAnimation() (*Animation, error) {
	a := newAnimation(ac.sff, ac.pal)
	a.filename = ac.filename
	a.actionNumber = ac.animNo

	a.mask = 0
	ols := int32(0)
	var err error
	var clsn1, clsn1d, clsn2, clsn2d [][4]float32
	def1, def2 := true, true

	for ; ac.i < len(ac.lines); ac.i++ {
		if len(ac.lines[ac.i]) > 0 && ac.lines[ac.i][0] == '[' {
			break
		}
		line := strings.ToLower(strings.TrimSpace(
			strings.SplitN(ac.lines[ac.i], ";", 2)[0]))

		// Copy Action
		if len(line) >= 12 && line[:12] == "copy action " { // Trailing space here
			numStr := strings.TrimSpace(line[12:])
			id, err := strconv.ParseInt(numStr, 10, 32)
			if err != nil || numStr == "" {
				// Mark for deletion if number is invalid
				a.copyAction = -1
			} else {
				a.copyAction = int32(id)
			}
			return a, nil
		}

		var af *AnimFrame
		af, lerr := ReadAnimFrame(line)

		// Use a local error to prevent overwriting the outer one with nil.
		// Tag with file/action/line so callers don't have to.
		if lerr != nil {
			err = ac.errorf(ac.i+1, "%v", lerr.Error())
		}

		switch {
		case af != nil:
			ols = a.loopstart
			if def1 {
				clsn1 = clsn1d
			}
			if def2 {
				clsn2 = clsn2d
			}
			af.Clsn1 = clsn1
			af.Clsn2 = clsn2

			a.frames = append(a.frames, *af)
			def1, def2 = true, true
		case len(line) >= 9 && line[:9] == "loopstart":
			a.loopstart = int32(len(a.frames))
		case len(line) >= 18 && line[:18] == "interpolate offset":
			a.interpolate_offset = append(a.interpolate_offset, int32(len(a.frames)))
		case len(line) >= 17 && line[:17] == "interpolate scale":
			a.interpolate_scale = append(a.interpolate_scale, int32(len(a.frames)))
		case len(line) >= 17 && line[:17] == "interpolate angle":
			a.interpolate_angle = append(a.interpolate_angle, int32(len(a.frames)))
		case len(line) >= 17 && line[:17] == "interpolate blend":
			a.interpolate_blend = append(a.interpolate_blend, int32(len(a.frames)))
		case len(line) >= 5 && line[:4] == "clsn":
			// Read Clsn boxes
			ii := strings.Index(line, ":")
			if ii < 0 {
				break
			}
			size := Atoi(line[ii+1:]) // Number of expected boxes
			if size < 0 {
				break
			}
			clsnType := line[4]
			isDefault := len(line) >= 12 && line[5:12] == "default"
			switch clsnType {
			case '1':
				clsn1, lerr = ac.readClsn(clsnType, size)
				if lerr != nil {
					err = lerr
				}
				if isDefault {
					clsn1d = clsn1
				}
				def1 = false
			case '2':
				clsn2, lerr = ac.readClsn(clsnType, size)
				if lerr != nil {
					err = lerr
				}
				if isDefault {
					clsn2d = clsn2
				}
				def2 = false
			}
		}
	}

	if int(a.loopstart) >= len(a.frames) {
		a.loopstart = ols
	}
	if len(a.frames) == 0 {
	} else if a.frames[len(a.frames)-1].Time == -1 {
		a.totaltime = -1
	} else {
		tmp := int32(0)
		for i, f := range a.frames {
			if f.Time == -1 {
				a.totaltime = 0
				a.looptime = -tmp
				a.prelooptime = 0
			}
			a.totaltime += f.Time
			if i < int(a.loopstart) {
				a.prelooptime += f.Time
				tmp += f.Time
			} else {
				a.looptime += f.Time
			}
		}
		if a.totaltime == -1 {
			a.prelooptime = 0
		}
	}
	return a, err
}

func (ac *AnimCompiler) readClsn(clsnType byte, size int32) ([][4]float32, error) {
	clsn := make([][4]float32, size)
	if size == 0 {
		return clsn, nil
	}
	filled := make([]bool, size)

	// 1-based line number of the "Clsn1:" / "Clsn2:" header
	lineNo := ac.i + 1

	// Move past the "Clsn:" line to the first rectangle line
	ac.i++

	// Read exactly 'size' number rectangle lines
	count := int32(0)
	for count < size && ac.i < len(ac.lines) {
		rline := strings.ToLower(strings.TrimSpace(
			strings.SplitN(ac.lines[ac.i], ";", 2)[0]))
		if len(rline) == 0 {
			ac.i++
			continue // Skip blank lines
		}
		// Stop if we encounter a line that doesn't start with "clsn"
		if len(rline) < 4 || rline[:4] != "clsn" {
			break
		}
		// Find the coordinate assignment
		eqIdx := strings.Index(rline, "=")
		if eqIdx < 0 {
			break
		}
		// Optional [n] between the type and "="
		boxIdx := int32(-1)
		if brStart := strings.Index(rline, "["); brStart >= 0 && brStart < eqIdx {
			if brEnd := strings.Index(rline[brStart:], "]"); brEnd > 0 {
				idxStr := strings.TrimSpace(rline[brStart+1 : brStart+brEnd])
				valid := idxStr != ""
				for k := 0; valid && k < len(idxStr); k++ {
					if idxStr[k] < '0' || idxStr[k] > '9' {
						valid = false
					}
				}
				if valid {
					boxIdx = int32(Atoi(idxStr))
				}
			}
		}
		// If the index couldn't be honored (absent, out-of-range, or repeated),
		// fall back to the first free slot so the box isn't silently lost.
		if boxIdx < 0 || boxIdx >= size || filled[boxIdx] {
			requested := boxIdx
			boxIdx = -1
			for k := int32(0); k < size; k++ {
				if !filled[k] {
					boxIdx = k
					break
				}
			}
			if boxIdx < 0 {
				// Impossible to reach, but kept as a safety net
				ac.warnf("line %v: Clsn%v no free slot (size %v); box dropped",
					lineNo, string(clsnType), size)
				ac.i++
				continue
			}
			if requested >= 0 {
				ac.warnf("line %v: Clsn%v[%v] index is duplicate or out of range; placed in slot [%v]",
					lineNo, string(clsnType), requested, boxIdx)
			}
		}
		// Extract the coordinates after "="
		ary := strings.Split(rline[eqIdx+1:], ",")
		if len(ary) < 4 {
			break
		}
		clsn[boxIdx] = NormalizeRect([4]float32{
			float32(Atoi(ary[0])),
			float32(Atoi(ary[1])),
			float32(Atoi(ary[2])),
			float32(Atoi(ary[3])),
		})
		filled[boxIdx] = true
		count++
		ac.i++
	}

	// Back up one step to avoid skipping the line after the last rectangle
	ac.i--

	// Warn if size doesn't match found boxes
	// Mugen crashes here
	if count != size {
		return clsn, ac.errorf(lineNo, "Clsn%v: declared %v boxes but only %v found",
			string(clsnType), size, count)
	}

	return clsn, nil
}

func (ac *AnimCompiler) readAction() (no int32, a *Animation, err error) {
	var name, subname string
	for ; ac.i < len(ac.lines); ac.i++ {
		name, subname = SectionName(ac.lines[ac.i])
		if len(name) > 0 {
			break
		}
	}
	if name != "begin " {
		return
	}
	spi := strings.Index(subname, " ")
	if spi < 0 {
		return
	}
	if strings.ToLower(subname[:spi+1]) != "action " {
		return
	}
	ac.i++

	no = Atoi(subname[spi+1:])
	ac.animNo = no
	a, err = ac.readAnimation()

	return
}
type AnimationTable struct {
	anims    map[int32]*Animation
	filename string
}

func NewAnimationTable() AnimationTable {
	return AnimationTable{
		anims: make(map[int32]*Animation),
	}
}

func (ac *AnimCompiler) compileAction() *Animation {
	for ac.i < len(ac.lines) {
		outerNo := ac.animNo
		no, a, err := ac.readAction()
		ac.animNo = outerNo
		// Animation errors do not crash Mugen. But we can log them
		if ac.log && err != nil {
			LogMessage("WARNING: %v", err.Error())
		}
		if a != nil {
			// In case of duplicate action numbers, just use the first one
			// Even if first one is "Copy Action"
			if existing := ac.at.anims[no]; existing != nil {
				if ac.log && ac.seen[no] {
					LogMessage("WARNING: Duplicate action key in %v: %v (ignored)", ac.filename, no)
				}
				ac.seen[no] = true
				return existing
			}
			// Recursive logic until we find a non-empty animation
			// If the current action is empty, we attempt to copy the very next action found in the file
			logged := false
			for len(a.frames) == 0 && ac.i < len(ac.lines) && a.copyAction < 0 {
				if !logged && ac.log {
					LogMessage("WARNING: %v, action %v: no valid frames", ac.filename, no)
					logged = true
				}
				if a2 := ac.compileAction(); a2 != nil {
					*a = *a2
					break
				}
				ac.i++
			}
			// Store only now that the animation is fully resolved or confirmed empty
			ac.at.anims[no] = a
			ac.seen[no] = true
			return a
		} else {
			// No action found on this line, advance to the next one
			ac.i++
		}
	}
	return nil
}

func (at AnimationTable) resolveCopyAction() {
	// Track actions that fail to resolve
	var toDelete []int32

	for no, a := range at.anims {
		// Limit the chain depth to 8 to prevent infinite loops from circular references
		for loops := 0; a.copyAction >= 0 && loops < 8; loops++ {
			target := at.anims[a.copyAction]

			// If the target is missing or it's a self-reference, break the link to stop
			if target == nil || target == a {
				break
			}

			// If the target is also shared, jump to the next link
			if target.copyAction >= 0 {
				a.copyAction = target.copyAction
				continue
			}

			// Perform shallow copy
			*a = *target

			// Mark this action as resolved so we don't process it again
			a.copyAction = -1
			break
		}

		// If Copy Action could not be resolved, do not keep the action at all
		if a.copyAction >= 0 {
			toDelete = append(toDelete, no)
		}
	}

	// Purge the ghost animations from the table
	for _, no := range toDelete {
		delete(at.anims, no)
	}
}

func main() {
	h := uint64(1469598103934665603)
	mix := func(v uint64) { h = (h ^ v) * 1099511628211 }
	nAct := 0
	for _, fn := range os.Args[1:] {
		b, err := os.ReadFile(fn)
		if err != nil {
			continue
		}
		if len(b) >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf {
			b = b[3:]
		}
		ac := newAnimCompiler(nil, nil, false)
		ac.compileText(fn, string(b))
		at := ac.finalize()
		var keys []int
		for k := range at.anims {
			keys = append(keys, int(k))
		}
		sort.Ints(keys)
		for _, k := range keys {
			a := at.anims[int32(k)]
			nAct++
			mix(uint64(uint32(k)))
			mix(uint64(len(a.frames)))
			mix(uint64(uint32(a.loopstart)))
			mix(uint64(uint32(a.totaltime)))
			mix(uint64(uint32(a.looptime)))
			mix(uint64(uint32(a.prelooptime)))
			mix(uint64(uint32(a.actionNumber)))
			for _, l := range [][]int32{a.interpolate_offset, a.interpolate_scale, a.interpolate_angle, a.interpolate_blend} {
				mix(uint64(len(l)))
				for _, x := range l {
					mix(uint64(uint32(x)))
				}
			}
			for _, f := range a.frames {
				mix(uint64(uint32(f.Time)) | uint64(uint32(f.Group))<<32)
				mix(uint64(uint32(f.Number)) | uint64(uint32(f.Xoffset))<<32)
				mix(uint64(uint32(f.Yoffset)) | uint64(uint32(f.TransType))<<32)
				mix(uint64(f.SrcAlpha) | uint64(f.DstAlpha)<<8 | uint64(uint8(f.Hscale))<<16 | uint64(uint8(f.Vscale))<<24)
				mix(uint64(math.Float32bits(f.Xscale)) | uint64(math.Float32bits(f.Yscale))<<32)
				mix(uint64(math.Float32bits(f.Angle)))
				for _, c := range [][][4]float32{f.Clsn1, f.Clsn2} {
					mix(uint64(len(c)))
					for _, r := range c {
						for _, x := range r {
							mix(uint64(math.Float32bits(x)))
						}
					}
				}
			}
		}
	}
	fmt.Printf("%d 0x%016x\n", nAct, h)
	_ = strconv.Itoa
	_ = strings.Index
}
