ReShade add-on for the MoH: European Assault PC port (optional)
================================================================

ReShade adds post-processing filters (sharpening, colour, contrast and many more) on top of the game picture.

INSTALL
  1. Extract this zip INTO the port's main folder, the one that contains MOHEuropeanAssault.exe and Launcher.exe
     (so dxgi.dll ends up next to the exe, and reshade-shaders\ next to it).
  2. Start the game from Launcher.exe. A "ReShade (add-on)" switch now appears on the Game tab.

USE
  Home    open / close the ReShade menu in the game (pick and tune the filters there)
  Insert  turn all filters on / off
  Print Screen  ReShade screenshot (the port's own F12 screenshot also works)
  The default preset enables AdaptiveSharpen, Colourfulness, MagicHDR and Unsharp. Turn off the ones you do not want
  (MagicHDR is the heaviest); your changes are saved to ReShadePreset.ini automatically.

TURN OFF / UNINSTALL
  Switch ReShade off in the launcher (it renames dxgi.dll to dxgi.dll.off), or delete dxgi.dll,
  dxgi.dll.off, ReShade.ini, ReShadePreset.ini and the reshade-shaders folder.

NOTES
  * Heavy filters (depth of field, bloom, ambient occlusion, SMAA at high settings) cost frames.
  * Effects that need the depth buffer (depth of field, ambient occlusion) may not work correctly.
  * If the game does not start after installing, switch ReShade off first and report it on the issues page.
  * Do not use ReShade with online services; this port has none.

CREDITS AND LICENSES
  ReShade by crosire (Patrick Mours), BSD 3-Clause, see LICENSES\ReShade-BSD-3-Clause.txt. https://reshade.me
  The shaders in reshade-shaders\ are SweetFX (CeeJay.dk and others) and FXShaders (luluco250); each .fx file
  carries its own license header, which stays with it.
