-- Menu drive for cvf_vitasim with Ikemen GO's own scripts and screenpack
-- (VITA_PORT.md section 10, "title -> select -> match"): title menu ->
-- ARCADE -> SINGLE MODE -> character select (P1 confirms the first cell)
-- -> the arcade fights against the CPU. Input is scripted by wrapping the
-- Lua input functions, as tests/smoke/smoke.lua does; the engine is not
-- changed. Loaded as a module (save/config.ini: [Common] Modules = <this>).
-- Prints "DRIVE ..." lines; cvf_vitasim's CVF_SIM_FRAMES ends the run.

local frame = 0
local queue = {}
local pressed = nil
local wait = 0
local function log(s) print(string.format('DRIVE [%5d] %s', frame, s)) end
local function push(...)
	for _, s in ipairs({...}) do table.insert(queue, s) end
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

local rawGetInput, rawEsc, rawRefresh, rawCmd = getInput, esc, refresh, commandGetState
getInput = function(p, keys, ...)
	if hit(keys) then return true end
	return rawGetInput(p, keys, ...)
end
-- the character select reads command states (main.f_btnPalNo)
commandGetState = function(c, k, ...)
	if pressed ~= nil and pressed ~= 'esc' and norm(k) == pressed then return true end
	return rawCmd(c, k, ...)
end
esc = function(...)
	if pressed == 'esc' then return true end
	return rawEsc(...)
end

-- scripted presses come from no controller: report player 1's
getLastInputController = function() return 1 end

local titled = false
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
	return rawRefresh(...)
end

local rawStoryboard = main.f_storyboard
main.f_storyboard = function(path, ...)
	log('storyboard ' .. tostring(path))
	return rawStoryboard(path, ...)
end

hook.add('main.t_itemname', 'drive', function(t, item)
	log('menu item ' .. tostring(t[item].itemname))
end)

-- the menu loop runs this hook every frame; start the key sequence there
hook.add('main.menu.loop', 'drive', function()
	if not titled then
		titled = true
		log('title menu')
		-- ARCADE, open it; SINGLE MODE; character select: confirm P1's
		-- cell (repeated: palette / order screens take a confirm too)
		push(150, 'a', 60, 'a', 180, 'a', 90, 'a', 90, 'a', 90, 'a')
	end
end)

local rawGame = game
game = function(...)
	log('fight starts')
	local r = rawGame(...)
	log('fight returned ' .. tostring(r))
	return r
end
