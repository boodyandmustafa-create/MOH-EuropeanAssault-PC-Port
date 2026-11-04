# Tester checklist (clean PC, 0.9.0-beta) / قائمة فحص للمجرّب

Send the tester the zip and its SHA-256. Ask them to follow this list and send back `play_log.txt` and a note of anything odd.

1. Unzip to a folder **without** spaces/special characters in its path (e.g. `C:\Games\MoH`). Check the SHA-256 of the zip.
2. Copy `DEFAULT.XBE`, `DATA\` and `EAGLRM.O` of the *USA* game into `game\`.
3. Start `Launcher.exe`:
   - [ ] the window opens (if it says system files are missing, note which and install what it links)
   - [ ] the Play tab shows every file "Found" and "USA (NTSC-U)"
   - [ ] Game tab: resolution is the monitor's size (max 2560x1440), 60 fps, draw distance 2
4. Press PLAY:
   - [ ] the first start takes a while (shaders) – note how long
   - [ ] the game reaches the title and the main menu; sound and music play
   - [ ] **F1** opens Recomp Settings; F2–F10, Pause, End, Insert, numpad do nothing special
   - [ ] **F12** shows "Screenshot saved" and a PNG appears in `screenshots\`
   - [ ] F11 / Alt+Enter toggle fullscreen
5. Play the first mission for 10–15 minutes:
   - [ ] mouse aim, WASD, shooting; gamepad (if available) incl. rumble
   - [ ] fps – note the GPU and how steady it is at 60 (try the "High performance" preset after)
   - [ ] sound stays (note the minute it vanishes, if it does)
6. Launcher again: change a key binding, press PLAY, check it works. Try **Report a problem**: the browser should open the
   "new issue" page, Explorer should show `play_log.txt`, and the clipboard should hold the system info.
7. Close the game and send back `play_log.txt` (and `play_log_prev.txt`).

Notes for the tester: the unsigned `.exe` files may trigger Windows SmartScreen ("More info" → "Run anyway").
