-- Menu smoke test for the native game (cvf): boots through the logo/intro
-- storyboards and the title menu, opens Versus character select, goes back,
-- opens Online > Host Nearby (and waits for an outside scanner, see
-- run_smoke.sh), then Online > Join Nearby, and exits with "SMOKE OK".
-- Any Lua error on the way ends the game before that line is printed.
--
-- Loaded as a module (save/config.ini: [Common] Modules = <this file>), so
-- main.lua has defined everything and is about to enter main.menu.loop().
-- Input is scripted by wrapping the Lua input functions; the engine is not
-- changed. Menu layout: CvF's options11.def (Arcade, Versus, Online, ...;
-- Online: Lobby, Host Nearby, Join Nearby, Host Game, Join Game, Back).

local frame = 0
local queue = {}     -- {wait, key} steps; key is pressed for exactly one frame
local pressed = nil  -- key pressed on this frame
local wait = 0
local phase = 'boot'
local function log(s) print(string.format('SMOKE [%5d] %s', frame, s)) end
local function push(...)
	for _, s in ipairs({...}) do table.insert(queue, s) end
end
local function fail(s)
	log('FAIL ' .. s)
	os.exit(3)
end

local function norm(k) return (tostring(k):gsub('^[/$>~]+', '')) end
local function hit(keys)
	if pressed == nil or pressed == 'esc' then return false end
	if type(keys) ~= 'table' then keys = {keys} end
	for _, k in ipairs(keys) do
		if norm(k) == pressed then return true end
	end
	return false
end

local rawGetInput, rawEsc, rawRefresh = getInput, esc, refresh
getInput = function(p, keys, ...)
	if hit(keys) then return true end
	return rawGetInput(p, keys, ...)
end
esc = function(...)
	if pressed == 'esc' then return true end
	return rawEsc(...)
end

-- per-frame driver
local selectCalls, lastSelectFrame = 0, -1
local hostFrames, joinCalls = 0, 0
refresh = function(...)
	frame = frame + 1
	pressed = nil
	if wait > 0 then
		wait = wait - 1
	elseif #queue > 0 then
		local s = table.remove(queue, 1)
		if type(s) == 'number' then
			wait = s
		else
			pressed = s
			log('press ' .. s)
		end
	end
	-- back on the main menu after character select: Online
	if phase == 'select' and selectCalls > 0 and frame - lastSelectFrame > 30 then
		phase = 'online'
		log('character select closed, opening Online')
		push(40, 'D', 30, 'a', 60, 'D', 30, 'a')  -- Versus -> Online; Lobby -> Host Nearby
	end
	if frame > 60 * 240 then fail('timeout in phase ' .. phase) end
	return rawRefresh(...)
end

-- storyboards play natively; the title menu comes right after them
local rawStoryboard = main.f_storyboard
main.f_storyboard = function(path, ...)
	log('storyboard ' .. tostring(path))
	local r = rawStoryboard(path, ...)
	if path == motif.files.intro.storyboard or motif.files.intro.storyboard == '' then
		phase = 'title'
		log('title menu')
		push(90, 'D', 30, 'a')  -- Arcade -> Versus
	end
	return r
end

hook.add('main.t_itemname', 'smoke', function(t, item)
	local name = t[item].itemname
	log('menu item ' .. tostring(name))
	if name == 'versus' then phase = 'select' end
	if name == 'nearbyhost' then phase = 'host' end
	if name == 'nearbyjoin' then phase = 'join' end
end)

-- character select polls cvfTouchTap every frame (start.lua)
local rawTap = cvfTouchTap
cvfTouchTap = function(...)
	selectCalls = selectCalls + 1
	lastSelectFrame = frame
	if selectCalls == 1 then log('character select running') end
	if selectCalls == 180 then
		log('character select ok (180 frames), leaving')
		push('esc')
	end
	return rawTap(...)
end

-- Host Nearby: the status screen is redrawn every frame
local rawHostStatus = cvfNearbyHostStatus
cvfNearbyHostStatus = function(...)
	local s = rawHostStatus(...)
	hostFrames = hostFrames + 1
	if hostFrames == 1 then
		log('host status: ' .. s:gsub('\n', ' | '))
		local f = io.open('smoke-hosting', 'w')
		if f then f:write('1') f:close() end
	end
	if hostFrames % 30 == 0 and hostFrames < 60 * 60 then
		local f = io.open('smoke-scanned', 'r')
		if f then
			f:close()
			log('outside scanner finished, leaving Host Nearby')
			hostFrames = 60 * 60
			push('esc', 60, 'D', 30, 'a')  -- Host Nearby -> Join Nearby
		end
	elseif hostFrames == 60 * 60 then
		log('no outside scanner, leaving Host Nearby')
		push('esc', 60, 'D', 30, 'a')
	end
	return s
end

-- Join Nearby: the host list is polled every frame
local rawHosts = cvfNearbyHosts
cvfNearbyHosts = function(...)
	local hosts, status = rawHosts(...)
	joinCalls = joinCalls + 1
	if joinCalls == 1 then log('join nearby running') end
	if joinCalls == 240 then
		log(string.format('join nearby: %d host(s), status "%s"', #hosts, status))
		push('esc')
		phase = 'done'
	end
	return hosts, status
end

-- leaving Join Nearby returns to the Online menu: done
local rawScanEnd = cvfNearbyScanEnd
cvfNearbyScanEnd = function(...)
	rawScanEnd(...)
	if phase == 'done' then
		log('SMOKE OK')
		os.exit(0)
	end
end

log('smoke module loaded')
