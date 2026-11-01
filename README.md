# Castlevania Fighter Online

Build scripts, engine patch and lobby server for running Castlevania Fighter v0.8.5b on a
patched [Ikemen GO](https://github.com/ikemen-engine/Ikemen-GO) with rollback netcode.

| Path | What it is |
|---|---|
| `patches/ikemen-cvf-online.patch` | Changes on top of Ikemen GO `6a09392` (see below) |
| `cvf-files/` | Edited CvF files: menu (`options11.def`), roster, command lists, online settings |
| `lobby/` | Lobby server (Go, no dependencies) |
| `render.yaml` | One-click deploy of the lobby to Render's free tier |
| `.github/workflows/` | Builds Windows + macOS (Intel and Apple Silicon) and publishes them as releases |

## Engine changes

1. **Rollback fix:** `ScreenPos` read a camera value that was only refreshed while rendering.
2. **Rollback fix:** projectile animations only advanced while drawing.
3. **Online helpers:** the Host screen shows your Internet/LAN address and port, opens the router port
   automatically via UPnP, and lists you in the lobby. New **Online > Lobby** screen lists players
   who are hosting; pick one to join without typing an IP.

## Deploying the lobby (free)

1. Sign in to [render.com](https://render.com) with GitHub.
2. **New > Blueprint**, pick this repository, then **Apply**. Render reads `render.yaml`.
3. Copy the service URL (e.g. `https://cvf-lobby.onrender.com`) into `cvf-files/data/cvf_online.ini`
   as `LobbyURL = ...` (and into `data/cvf_online.ini` in your game folder).

Free Render services sleep after 15 minutes without traffic; the first visit after that takes
about a minute while it wakes up. The game shows "Connecting to the lobby..." meanwhile.
The lobby only stores who is hosting; game traffic goes directly between players.
