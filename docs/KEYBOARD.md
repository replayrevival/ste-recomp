# Keyboard (c134)

The keyboard is a pad on port 0. It is on by default; with a real pad connected it is merged on top (a pad keeps working).
Switch it off with `ste_video.ini` `[input] keyboard=0` or the environment variable `RECOMP_KEYBOARD=0` (the variable overrides the ini; `RECOMP_KEYBOARD=1` forces it on).
Keys reach the game from the "Godzilla STE - Kelvin" window only while it has focus; losing focus (Alt-Tab, another window activated) releases every key.
Alt+F4 / Alt+Space still go to Windows.

| Key | Pad |
|---|---|
| Arrow keys | D-pad |
| Enter | Start |
| Backspace | Back |
| Z / X / C / V | A / B / X / Y |
| Q / E | White / Black |
| 1 / 3 | Left / right trigger |
| W A S D (or keypad 8 4 2 6) | Left stick |
| I J K L | Right stick |
| Shift / Ctrl | Left / right stick click |

Code: the key table and map are upstream xbox_input keyboard.c (map unchanged). c134 changes: (1) keyboard.c default-on + ini key, (2) the Kelvin window's WndProc (ste_pb_hook.c) forwards WM_KEYDOWN/KEYUP/SYSKEYDOWN/SYSKEYUP to xbox_InputKeySet() and clears on WM_KILLFOCUS / inactive WM_ACTIVATE.
Before c134 the Kelvin window never forwarded keys, so RECOMP_KEYBOARD=1 did nothing there (only the old GDI framebuffer window did).

## Testing note (harnesses)
Test harnesses that set STE_VIRTUAL_PAD=1 (tests\fightshots.ps1, tests\soak.ps1) bypass the host pad AND the keyboard completely: they inject the scripted pad directly. To test keyboard input set STE_VIRTUAL_PAD=0 (e.g. fightshots.ps1 -Env @{STE_VIRTUAL_PAD='0'}). Real-keyboard testing over RDP is still pending (to be done by the project owner after promotion of c138); STE_KEY_TRACE=1 logs every key press as [KEY] lines in stderr for that.
