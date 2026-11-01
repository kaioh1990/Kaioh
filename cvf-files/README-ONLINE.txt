CASTLEVANIA FIGHTER ONLINE (Ikemen GO + rollback netcode)
=========================================================

This folder is Castlevania Fighter v0.8.5b running on a patched Ikemen GO
engine with rollback netcode. The SAME folder works on every device:
copy the whole folder to each machine.

  Windows desktop ........ double-click  Ikemen_GO.exe
  Steam Deck / Z13 ....... run           play-linux.sh   (see below)
  Mac .................... double-click  "CvF Online (Apple Silicon).app"
  iPhone / iPad .......... install the CvF Online app (section 5)

Your original "Castlevania Fighter (v0.8.5b)" folder is untouched.


1. FIRST LAUNCH
---------------
- The game starts at the CvF title screen. The main menu now has
  ARCADE / VERSUS / ONLINE.
- Keyboard defaults (change in Options > Key Config):
  arrows = move, Z X C / A S D = buttons, Enter = start.
- In menus press Enter or Z (or the A / bottom face button) to choose.
- Command list: pause a match (Esc) > Command List.
- Controllers work (Options > Joystick Config).
- Alt+Enter toggles fullscreen.


2. PLAYING ONLINE
-----------------
Online menu: LOBBY / HOST GAME / JOIN GAME

  Host Game  You wait for an opponent. The screen shows your Internet
             address and port, tries to open the port on your router
             automatically, and lists you in the lobby.
  Lobby      Shows everyone who is hosting right now. Pick a name to join.
             No IP typing needed. (The first time, you choose a player name.)
  Join Game  Join by typing the host's address yourself.

If nobody can join you: the host screen will say the router could not be
set up automatically. Then either forward TCP *and* UDP port 7500 on your
router to your machine, or have both players use a VPN like Tailscale /
ZeroTier / Radmin and join by the VPN address.

Both players must use the same build of this folder.

Rollback settings are in save/config.ini under [Netplay]:
  Rollback.FrameDelay = 2   (raise to 3-4 on bad connections)
Lobby server address: data/cvf_online.ini (LobbyURL).


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

  "CvF Online (Apple Silicon).app"  <- native, use this on M1/M2/M3/M4 Macs.
  "CvF Online (Intel - for online vs PC).app"  <- for Intel Macs (it also
     runs on Apple Silicon through Rosetta 2).

Both can play online against Windows / Steam Deck / Z13 / iPad: the engine
now uses its own copies of the few math functions that used to give
slightly different answers on Apple chips vs Intel/AMD, so every device
computes exactly the same thing (which is what rollback needs).

If macOS says the app "can't be opened": right-click it > Open > Open
(the apps are not notarized by Apple).

The apps are built by GitHub Actions in github.com/kaioh1990/Kaioh.


5. IPHONE / IPAD (iOS / iPadOS 17 or newer)
-------------------------------------------
Downloads: github.com/kaioh1990/Kaioh/releases/tag/cvf-online-ios

a) Install the app - pick ONE:
   - Xcode (easiest if you have it): unzip cvf-online-ios-xcode.zip, open
     CvF-Online-iOS/I.K.E.M.E.N-Go.xcodeproj, click the project > target
     "I.K.E.M.E.N-Go" > Signing & Capabilities > Team = your Apple ID.
     Plug in / pair the iPad, select it at the top, press Run (Cmd+R).
     First time: on the iPad, Settings > General > VPN & Device
     Management > trust your developer certificate, and turn on
     Settings > Privacy & Security > Developer Mode.
   - Sideloadly / AltStore / SideStore: install cvf-online-ios.ipa.
   With a free Apple ID the app has to be re-installed (re-signed) every
   7 days; a paid developer account lasts a year.

b) Copy the game onto the device: connect it to the Mac, open Finder >
   your iPad > Files, and drag this whole "CvF Online" folder onto
   "CvF Online" (the app). You can skip the .exe/.dll files, the Mac
   apps and lib/ to save space.

c) Start the app. When it asks for the game folder, pick
   On My iPad > CvF Online > CvF Online (the folder that contains chars,
   data, stages). It only asks once.

Controls: on-screen pad and buttons, or any MFi / Xbox / PlayStation
controller paired in Bluetooth settings before starting the app.
Online play works the same as on the other devices. Hosting from an
iPad needs the port forwarded on your router (iOS doesn't let apps open
router ports automatically), so it's easiest to let the PC/Mac host and
join from the iPad via the Lobby.


6. WHAT TO TEST AND REPORT
--------------------------
Please try, in this order:
  a) Windows vs Steam Deck (both Intel/AMD) - a few full matches.
  b) Mac or iPad vs Windows / Steam Deck.
If something goes wrong (desync message, freeze, crash), send back:
  - the newest files in save/logs/
  - which two devices, which characters, which stage.
To capture detailed rollback logs, set on BOTH machines in
save/config.ini:   Rollback.StateLogsEnabled = 1


Known issues
------------
- The Stella & Loretta boss is hidden from character select (her intro
  freezes when picked as player 1). She is still the arcade boss.
- Hellsing Alucard (HAlucard) was removed: he broke matches.
- DChick and Galamoth reference stages that are missing from CvF 0.8.5b.
- Linux build: error pop-up dialogs are not shown (errors go to the
  terminal and save/logs instead).

Engine changes: patches/ikemen-cvf-online.patch in github.com/kaioh1990/Kaioh
(rollback desync fixes, cross-CPU math, online lobby and CvF rules on top
of Ikemen GO commit 6a09392).
