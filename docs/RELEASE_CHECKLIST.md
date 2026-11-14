<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Release checklist

Never publish or push: Miko publishes.

1. Candidate promoted (PROMOTED note for the launcher session), validator 0 FAIL, SHA-256 recorded.
2. Soak clean (mixed run, dumps, [DISPATCH] stalls, fps against the previous release).
3. **Showcase screenshots refreshed (standing rule, Miko 2026-11-13).**
   - Capture with the game's own capture path (backbuffer, not the desktop). Clean frames: no debug overlays or probes; HUD on unless the shot is a comparison.
   - Set: at least 1080p 16:9 and 21:9 ultrawide; Unlimited draw distance (wide city skyline); a big fight with effects (beams, explosions, hit flash); each feature new in this release; button prompts in Xbox and PlayStation styles; from beta on, HD art vs original side by side.
   - Files: `docs/screenshots/<version>_<what>.png` (e.g. `0.1.4_ultrawide_tokyo.png`), PNG, at most ~2560 px wide, compressed.
   - Move the previous release's set to `docs/screenshots/archive/<version>/` (do not delete).
   - Update the README "Screenshots" section: one short caption per image; keep Miko's own shots if she provides them.
   - Hand the set to the launcher session.
4. Docs: RELEASE_ALPHA.md known issues, LAUNCHER_INTERFACE.md, credits.
5. Publish repo synced locally (author "Replay Revival", no push).
