# Castlevania Fighter Online

Build scripts, engine patch and lobby server for running Castlevania Fighter v0.8.5b on a
patched [Ikemen GO](https://github.com/ikemen-engine/Ikemen-GO) with rollback netcode.

| Path | What it is |
|---|---|
| `patches/ikemen-cvf-online.patch` | Changes on top of Ikemen GO `6a09392` (see below) |
| `cvf-files/` | Edited CvF files: menu (`options11.def`), roster, command lists, online settings |
| `lobby/` | Lobby server (Go, no dependencies) |
| `render.yaml` | One-click deploy of the lobby to Render's free tier |
| `ios/customize.sh` | Turns the [ikemen-ios](https://github.com/Jesuszilla/ikemen-ios) wrapper into the CvF Online iPhone/iPad app |
| `android/customize.sh` | Turns the [ikemen-droid](https://github.com/Jesuszilla/ikemen-droid) wrapper into the CvF Online Android app (`cvf-online.patch`: game folder auto-detect, menu gestures, settings never overwritten; `cvf-online.keystore`: fixed signing key so updates install over the top) |
| `tools/detmath-check/` | CI check that Intel/AMD and Apple chips compute identical math |
| `.github/workflows/` | Builds Windows, macOS (Intel and Apple Silicon), iOS/iPadOS and Android, and publishes them as releases |

## Engine changes

1. **Rollback fix:** `ScreenPos` read a camera value that was only refreshed while rendering.
2. **Rollback fix:** projectile animations only advanced while drawing.
3. **Online helpers:** the Host screen shows your Internet/LAN address and port, opens the router port
   automatically via UPnP, and lists you in the lobby. New **Online > Lobby** screen lists players
   who are hosting; pick one to join without typing an IP.
4. **Cross-CPU determinism:** gameplay code uses pure-Go copies of `Exp`/`Log`/`Pow`/`Hypot`/`Log2`
   (Go's assembly versions differ between amd64 and arm64), and Apple builds disable FMA fusion,
   so PCs, Macs, iPads and Android phones can play each other without desyncs.
5. **Nearby play:** Online > Host Nearby / Join Nearby finds a player on the same Wi-Fi or phone
   hotspot with a small UDP probe on port 7520 (unicast sweep of the local subnet, so it works on
   iOS without the multicast entitlement). No Internet, lobby or IP typing needed.
   iPhones/iPads also find and join each other directly over Apple's peer-to-peer Wi-Fi
   (Bonjour `_cvfnearby._tcp/_udp`, `ios/bundled-game.patch` CvfP2P.m), with no network at all;
   the app relays the normal netplay traffic through 127.0.0.1 so the netcode is unchanged.
6. **CvF rules** (`data/cvf_online.ini`): attack invincibility cap and slower MP gain.

## Deploying the lobby (free)

1. Sign in to [render.com](https://render.com) with GitHub.
2. **New > Blueprint**, pick this repository, then **Apply**. Render reads `render.yaml`.
3. The service URL (currently `https://cvf-lobby.onrender.com`) goes into `cvf-files/data/cvf_online.ini`
   as `LobbyURL = ...` (and into `data/cvf_online.ini` in your game folder).

Free Render services sleep after 15 minutes without traffic; the first visit after that takes
about a minute while it wakes up. The game shows "Connecting to the lobby..." meanwhile.
The lobby only stores who is hosting; game traffic goes directly between players.
