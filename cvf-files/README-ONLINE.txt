CASTLEVANIA FIGHTER ONLINE (Ikemen GO + rollback netcode)
=========================================================

This folder is Castlevania Fighter v0.8.5b running on a patched Ikemen GO
engine with rollback netcode. The SAME folder works on every device:
copy the whole folder to each machine.

  Windows desktop ........ double-click  Ikemen_GO.exe
  Steam Deck / Z13 ....... run           play-linux.sh   (see below)
  Mac .................... double-click  "CvF Online (Intel - for online vs PC).app"

Your original "Castlevania Fighter (v0.8.5b)" folder is untouched.


1. FIRST LAUNCH
---------------
- The game starts at the CvF title screen. The main menu now has
  ARCADE / VERSUS / ONLINE.
- Keyboard defaults (change in Options > Key Config):
  arrows = move, Z X C / A S D = buttons, Enter = start.
- IMPORTANT: in menus, press Z (or the A / bottom face button on a
  controller) to choose an item. Enter does NOT select menu items.
- Controllers work (Options > Joystick Config).
- Alt+Enter toggles fullscreen.


2. PLAYING ONLINE
-----------------
One player HOSTS, the other JOINS.

  Host:  ONLINE > Host Game.  The host needs TCP *and* UDP port 7500
         forwarded on their router to their machine (or use a VPN
         like Tailscale / ZeroTier / Radmin and skip port forwarding).
  Join:  ONLINE > Join Game > New Address > type the host's IP.
  Then:  pick Versus, choose characters, fight.

Both players must use this same folder (same engine build, same
characters). Mismatched files will desync.

Rollback settings are in save/config.ini under [Netplay]:
  Rollback.FrameDelay = 2   (raise to 3-4 on bad connections)


3. STEAM DECK / ROG Z13 (SteamOS)
--------------------------------
- Copy this folder to the device (e.g. ~/Games/CvF Online).
- In Desktop Mode, right-click play-linux.sh > Properties > Permissions >
  "Is executable", then double-click it to test.
- To play from Game Mode: Steam > Add a Non-Steam Game > browse to
  play-linux.sh. Use a controller layout with the buttons mapped as you
  like (Steam Input), or configure the pad in Options.


4. MAC
------
Two Mac apps are included. Both must stay in this folder (next to chars/,
data/, stages/).

  "CvF Online (Intel - for online vs PC).app"  <- use this for online play
     against Windows / Steam Deck / Z13. On an Apple Silicon Mac it runs
     through Rosetta 2 (macOS offers to install Rosetta the first time),
     so the Mac does exactly the same decimal math as the PCs, which is
     what rollback needs.

  "CvF Online (Apple Silicon).app"  <- native and faster; use it for
     Mac-vs-Mac or offline play. Apple chips do some decimal math slightly
     differently from Intel/AMD, so it may desync against PCs.

If macOS says the app "can't be opened": right-click it > Open > Open
(the apps are not notarized by Apple).

Fallback: Ikemen_GO.exe also runs in CrossOver.

The apps are built by GitHub Actions in github.com/kaioh1990/Kaioh.


5. WHAT TO TEST AND REPORT
--------------------------
Please try, in this order:
  a) Windows vs Steam Deck (both Intel/AMD) - a few full matches.
  b) Mac (CrossOver) vs Windows.
If something goes wrong (desync message, freeze, crash), send back:
  - the newest files in save/logs/
  - which two devices, which characters, which stage.
To capture detailed rollback logs, set on BOTH machines in
save/config.ini:   Rollback.StateLogsEnabled = 1


Known issues
------------
- The Stella & Loretta boss is hidden from character select (her intro
  freezes when picked as player 1). She is still the arcade boss.
- HAlucard vs CPU matches can drag on: the round timer doesn't run out
  under CvF's lifebars, so rounds end only on a KO.
- DChick and Galamoth reference stages that are missing from CvF 0.8.5b.
- Linux build: error pop-up dialogs are not shown (errors go to the
  terminal and save/logs instead).

Engine changes: patches/ikemen-cvf-rollback-fixes.patch
(two rollback desync fixes on top of Ikemen GO commit 6a09392).
