package main

import (
	"os"
	"strconv"
	"strings"
)

// Headless quick-vs match, the entry point of the native C engine's
// translated simulation (tools/gen/gotoc root "cvfHeadlessMain").
//
// It performs what a command-line quick match does in Ikemen GO, minus
// window, renderer, audio and Lua:
//   - main.go realMain: command line, LUTs, config
//   - System.init: the non-platform part
//   - external/script/main.lua main.f_commandLine (no -loadmotif)
//   - the Lua "game" function for one match
//
// Returns the winning side (1, 2), 0 for a draw, -1 on error.
// cvfHeadless: running without window/renderer (platform calls are skipped).
var cvfHeadless bool

// CVF_FRAMES=n stops after n frames (benchmarks).
var cvfFrames, cvfMaxFrames int

func cvfHeadlessMain(args []string) int32 {
	cvfHeadless = true
	if n, err := strconv.Atoi(os.Getenv("CVF_FRAMES")); err == nil {
		cvfMaxFrames = n
	}
	// ---- main.go
	sys.baseDir = "./"
	os.Args = append([]string{"ikemen-go"}, args...)
	processCommandLine()
	if sys.cmdFlags == nil {
		sys.cmdFlags = make(map[string]string)
	}
	initLUTs()
	configPath := "save/config.ini"
	if val, ok := sys.cmdFlags["-config"]; ok {
		configPath = val
	}
	cfg, err := loadConfig(configPath)
	if err != nil {
		LogMessage("loadConfig: %v", err)
		return -1
	}
	sys.cfg = *cfg

	// ---- System.init without window/renderer/audio/Lua
	sys.setGameSize(int32(sys.gameWidth), int32(sys.gameHeight))
	for i := range sys.cgi {
		sys.cgi[i].palInfo = make(map[int]PalInfo)
	}
	sys.resetRemapInput()
	for i := range sys.stringPool {
		sys.stringPool[i] = *NewStringPool()
	}

	// ---- main.lua: main.f_commandLine
	flags := sys.cmdFlags
	sys.gameMode = "quickvs"
	sys.credits = -1
	sys.cfg.SetValueUpdate("Config.BootLoadingMode", "0")
	fs, err := loadFightScreen("")
	if err != nil {
		LogMessage("loadFightScreen: %v", err)
		return -1
	}
	sys.fightScreen = *fs

	matchWinsDraw := [2]int32{int32(sys.cfg.Options.Match.MaxDrawGames), int32(sys.cfg.Options.Match.MaxDrawGames)}
	matchWinsSimul := [2]int32{int32(sys.cfg.Options.Simul.Match.Wins), int32(sys.cfg.Options.Simul.Match.Wins)}
	matchWinsSingle := [2]int32{int32(sys.cfg.Options.Match.Wins), int32(sys.cfg.Options.Match.Wins)}
	matchWinsTag := [2]int32{int32(sys.cfg.Options.Tag.Match.Wins), int32(sys.cfg.Options.Tag.Match.Wins)}
	roundTime := int32(sys.cfg.Options.Time)
	cvfSetFightScreenElements(sys.cfg.Options.GuardBreak, sys.cfg.Options.Dizzy, sys.cfg.Options.RedLife)
	frames := sys.fightScreen.time.framespercount

	type qvPlayer struct {
		character      string
		player, num    int
		pal            int
		ai             float32
		input          int
	}
	var players []qvPlayer
	teamMode := [2]int32{0, 0}
	numChars := [2]int32{0, 0}
	assignedPals := map[string]map[int]bool{}
	for num := 1; num <= MaxPlayerNo; num++ { // main.lua sorts players by -p<num>
		v, ok := flags["-p"+strconv.Itoa(num)]
		if !ok {
			continue
		}
		player := 2 - num%2
		numChars[player-1]++
		pal := 1
		if s, ok := flags["-p"+strconv.Itoa(num)+".color"]; ok {
			pal = int(Atoi(s))
		} else if s, ok := flags["-p"+strconv.Itoa(num)+".pal"]; ok {
			pal = int(Atoi(s))
		} else if ap, ok := assignedPals[v]; ok {
			for i := 1; i <= 12; i++ {
				if !ap[i] {
					pal = i
					break
				}
			}
		}
		if assignedPals[v] == nil {
			assignedPals[v] = map[int]bool{}
		}
		assignedPals[v][pal] = true
		ai := float32(0)
		if s, ok := flags["-p"+strconv.Itoa(num)+".ai"]; ok {
			ai = float32(Atof(s))
		}
		input := player
		if s, ok := flags["-p"+strconv.Itoa(num)+".input"]; ok {
			input = int(Atoi(s))
		}
		players = append(players, qvPlayer{character: v, player: player, num: num, pal: pal, ai: ai, input: input})
		cvfRefresh() // main.lua calls refresh() for every -p<n> flag
	}
	if s, ok := flags["-tmode1"]; ok {
		teamMode[0] = Atoi(s)
	}
	if s, ok := flags["-tmode2"]; ok {
		teamMode[1] = Atoi(s)
	}
	if s, ok := flags["-time"]; ok {
		roundTime = Atoi(s)
	}
	if s, ok := flags["-rounds"]; ok {
		for i := 0; i < 2; i++ {
			matchWinsSingle[i] = Atoi(s)
			matchWinsSimul[i] = Atoi(s)
			matchWinsTag[i] = Atoi(s)
		}
	}
	if s, ok := flags["-draws"]; ok {
		for i := 0; i < 2; i++ {
			matchWinsDraw[i] = Atoi(s)
		}
	}
	framesMul := [2]int32{1, 1}
	for side := 0; side < 2; side++ {
		if teamMode[side] == 0 && numChars[side] > 1 {
			teamMode[side] = 1
		}
		if teamMode[side] == 3 {
			framesMul[side] = numChars[side]
		}
	}
	for side := 0; side < 2; side++ {
		enemy := 1 - side
		switch teamMode[enemy] {
		case 1:
			sys.matchWins[side] = matchWinsSimul[enemy]
		case 2:
			sys.matchWins[side] = numChars[enemy]
		case 3:
			sys.matchWins[side] = matchWinsTag[enemy]
		default:
			sys.matchWins[side] = matchWinsSingle[enemy]
		}
		sys.maxDraws[side] = matchWinsDraw[side]
	}
	frames = frames * Max(framesMul[0], framesMul[1])
	sys.curFramesPerCount = frames
	t := Max(-1, roundTime*frames)
	if t > 0 && sys.cfg.Config.LegacyTime {
		t += sys.curFramesPerCount - 1
	}
	sys.maxRoundTime = t

	stage := sys.cfg.Debug.StartStage
	if s, ok := flags["-s"]; ok {
		for _, v := range []string{s, "stages/" + s, "stages/" + s + ".def"} {
			if FileExist(v) != "" {
				stage = v
				break
			}
		}
	}
	if _, err := sys.sel.AddStage(stage); err != nil {
		LogMessage("Unable to add stage: %v", stage)
		return -1
	}
	sys.sel.ClearSelected()
	sys.matchNo = 1
	sys.sel.SelectStage(1)
	for side := 0; side < 2; side++ {
		cvfSetTeamMode(side, TeamMode(teamMode[side]), numChars[side])
	}
	ref := 0
	charDef := map[string]int{}
	for _, v := range players {
		key := strings.ToLower(v.character)
		charRef, ok := charDef[key]
		if !ok {
			sys.sel.AddChar(v.character)
			charRef = ref
			charDef[key] = charRef
			ref++
		}
		sys.sel.AddSelectedChar(v.player-1, charRef, v.pal)
		sys.aiLevel[v.num-1] = Clamp(v.ai, 0, 8)
		sys.inputRemap[v.num-1] = v.input - 1
	}

	// loadStart("pausemenu=false")
	if sys.sel.gameParams == nil {
		sys.sel.gameParams = newGameParamsFromMotif(&sys.motif)
	}
	sys.sel.music = make(Music)
	sys.sel.gameParams.AppendParams(SplitAndTrim(StripComment("pausemenu=false"), ","))
	sys.sel.music.AppendParams(sys.sel.gameParams.MusicEntries())
	sys.loaderReset()

	return cvfGame()
}



// The Lua setFightScreenElements function for main.f_commandLine's table
// {guardbar, stunbar, redlifebar}.
func cvfSetFightScreenElements(guardbar, stunbar, redlifebar bool) {
	sys.fightScreen.guardbar = guardbar
	sys.fightScreen.stunbar = stunbar
	sys.fightScreen.redlifebar = redlifebar
	if _, ok := sys.fightScreen.match.enabled[sys.gameMode]; ok {
		sys.fightScreen.match.active = sys.fightScreen.match.enabled[sys.gameMode]
	}
	for _, v := range sys.fightScreen.aiLevels {
		if _, ok := v.enabled[sys.gameMode]; ok {
			v.active = v.enabled[sys.gameMode]
		}
	}
	for _, v := range sys.fightScreen.scores {
		if _, ok := v.enabled[sys.gameMode]; ok {
			v.active = v.enabled[sys.gameMode]
		}
	}
	for _, v := range sys.fightScreen.winCounts {
		if _, ok := v.enabled[sys.gameMode]; ok {
			v.active = v.enabled[sys.gameMode]
		}
	}
}

// The Lua setTeamMode function.
func cvfSetTeamMode(side int, tm TeamMode, nt int32) {
	sys.sel.selected[side] = nil
	sys.tmode[side] = tm
	if tm == TM_Turns {
		sys.numSimul[side] = 1
	} else {
		sys.numSimul[side] = nt
	}
	sys.numTurns[side] = nt
	if (tm == TM_Simul || tm == TM_Tag) && nt == 1 {
		sys.tmode[side] = TM_Single
	}
}

// The Lua game function for a single match (no netplay, no retries).
func cvfGame() int32 {
	sys.keyString = ""
	sys.keyInput = KeyUnknown
	sys.gameRunning = true
	sys.escPending = false
	sys.endMatch = false

	winp := int32(0)
	sys.debugRef = [2]int{}
	sys.roundsExisted = [2]int32{}
	sys.scoreRounds = [][2]float32{}
	for i := range sys.fightScreen.winIcons {
		sys.fightScreen.winIcons[i].clear()
	}
	sys.draws = 0
	sys.statsLog.startMatch()

	for {
		if sys.roundNo == 1 {
			sys.charList.clear()
		}
		// synchronous load (the Lua path runs it on a goroutine and waits)
		if sys.loader.state == LS_NotYet {
			sys.loader.state = LS_Loading
			sys.loader.load()
		}
		if sys.loader.state != LS_Complete {
			LogMessage("load failed: %v", sys.loader.err)
			return -1
		}
		sys.setGameAspect()
		sys.initPlayerID()
		for i := range sys.chars {
			c := sys.chars[i]
			if len(c) > 0 {
				if sys.roundNo == 1 {
					sys.charList.add(c[0])
				} else if c[0].roundsExisted() == 0 {
					sys.removePlayerFromCharList(i)
					sys.charList.add(c[0])
				}
				if c[0].roundsExisted() == 0 {
					c[0].loadPalettes()
				}
				for j := range sys.chars {
					cj := sys.chars[j]
					if i != j && len(cj) > 0 {
						if len(cj[0].cmd) == 0 {
							cj[0].cmd = make([]CommandList, len(sys.chars))
						}
						cj[0].cmd[i].CopyList(c[0].cmd[i])
					}
				}
			}
		}
		sys.charList.audit()
		if sys.roundNo == 1 {
			sys.endMatch = false
			sys.teamLeader = [2]int{0, 1}
			sys.stage.reset()
		}
		if sys.runMatch() {
			continue // match restart: reload and run again
		}
		if sys.esc {
			winp = -1
		} else {
			w1 := sys.wins[0] >= sys.matchWins[0]
			w2 := sys.wins[1] >= sys.matchWins[1]
			if w1 != w2 {
				winp = Btoi(w1) + Btoi(w2)*2
			}
		}
		break
	}
	sys.gameRunning = false
	return winp
}

// The Lua refresh function (one UI frame).
func cvfRefresh() {
	sys.luaDiscardDrawQueue()
	if sys.motif.fadeOut.isActive() {
		sys.motif.fadeOut.step()
	} else if sys.motif.fadeIn.isActive() {
		sys.motif.fadeIn.step()
	}
	sys.stepCommandLists()
	sys.uiFrameCounter++
	sys.update()
}
