# Castlevania Fighter Online — build scripts

Builds a patched [Ikemen GO](https://github.com/ikemen-engine/Ikemen-GO) engine for running
Castlevania Fighter v0.8.5b with rollback netcode.

- `patches/ikemen-cvf-rollback-fixes.patch` — two rollback desync fixes on top of Ikemen GO `6a09392`:
  1. `ScreenPos` read a camera value only refreshed while rendering (desynced every CvF character).
  2. Projectile animations only advanced while drawing (desynced projectile-heavy characters).
- `.github/workflows/build-macos.yml` — builds macOS app bundles (Intel for Rosetta / cross-play, and native Apple Silicon).

The game content itself is not in this repo. Drop the built `.app` into the `CvF Online` folder,
next to `chars/`, `data/`, `stages/`.
