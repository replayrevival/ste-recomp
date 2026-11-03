# Launcher mockup (approved direction, TESTER 2026-10-30)

Files:
- Setup.mockup.html: first start, "Add a game" (choose/drag file, search drives, found + verified, plain errors).
- Main.mockup.html: per-game launcher. Header, PLAY, presets, tabs Overview/Video/Enhancements/Mods/Controllers, controller hint bar.
  NOTE: the mockup still shows a multi-game library sidebar. TESTER decided: ONE launcher per game for now. Drop the library sidebar; show the game title up front and "made with [BRAND]" as a small maker mark.

These are design-canvas component files (x-dc markup + a JS logic class), not standalone pages; read them for layout, copy, colours and states.

Style tokens:
- Background #0A1A33 with a 32 px blueprint grid (lines rgba(90,160,230,0.08)); panel #0D2142; selected fill #12396A; dividers #1E3A63; idle outline #2F5C93.
- Accent cyan #3FD4FF (outline + glow), light cyan #7FE3FF, yellow #FFD43B (selection arrows, PLAY, warnings).
- Text #D6ECFF / #EAF5FF, secondary #A9C6E6, muted #7C9CC4.
- Buttons: chamfered corners (clip-path octagon, 8–12 px), 2 px outline; selected = cyan outline + glow + yellow arrow marker.
- Fonts: Pixelify Sans (titles only), Chakra Petch (everything else). Both OFL.
- Controller hint bar at the bottom: A select, B back, LB/RB tabs, START play (button labels follow the connected controller type).
