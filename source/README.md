# Source code

Everything in this folder is what the port is made of, **except the game itself**. Nothing here is a game file.

| Folder | What it is |
|---|---|
| `mygame/src/` | The port: `main.c`, `game_patches.c` (the fixes and features hooked into the game), `recomp_settings.c` (the in-game Recomp Settings screen), `recomp_manual.c` (hand-lifted functions and overrides), `ai_tuning.c` / `ai_aim.c` (enemy count, difficulty tables, AI tuning files, aim measurement), `rc_env.*` |
| `mygame/launcher/` | The launcher (`launcher.cpp`, `options.txt` = the option table, `lang/en.txt` and `lang/ar.txt` = its texts) |
| `mygame/languages/arabic/recomp_menu.txt` | The Arabic text of the Recomp Settings screen |
| `mygame/mods/` | The mod file formats (README files and examples): `weapons\`, `ai\` |
| `mygame/tools/` | The scripts used to find things in the game (CRC32 field names, fonts, icons, Arabic pack builder, screen effects) |
| `mygame/make_package.py` | Builds the release folder and zip |
| `xboxrecomp-0.16.0/` | The [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit (MIT) **with this port's changes**: the Direct3D 8 to 11 layer, the NV2A executor, kernel and audio glue, and the xemu-derived APU / NV2A files (LGPL 2.1+) |
| `gen_hand_edits.txt` | Every hand edit made inside the generated game code (see below) |

## What is NOT here, and why

`mygame/src/recomp/gen/` — the **recompiled game code** (about 86 MB of C generated from `DEFAULT.XBE`) — is not in
this repository. It is machine-translated Electronic Arts code, so it stays with whoever owns the game. Every
hand edit made inside it is listed in `gen_hand_edits.txt` (file, line, function, the edited line) so it can be
re-applied to a freshly generated copy. Only `recomp_types.h` (the register model, not game code) is included.

To build you need your own copy of the game (USA / NTSC-U), the recompiler from the toolkit
(`xboxrecomp-0.16.0/tools/recomp`, see its `docs/GETTING_STARTED.md`) and Visual Studio 2022 or later.
This port's `gen` was produced by that recompiler and then post-processed (registers kept in locals, a few
functions replaced by the hand-written versions in `recomp_manual.c` / `game_patches.c`); that post-processing
is not automated in this repository yet, so a first build will be slower than the released one until it is.
Questions and help are welcome in the issues.

## Building (once you have a `gen`)

```
mygame\build.bat
```
or

```
cmake -S mygame -B mygame\build
cmake --build mygame\build --config Release --target your_game_recomp
```

The developer diagnostics (AI probes, shot and projectile logs, aim measurement) are compiled out of the normal
build. A diagnostic build uses its own build folder: `cmake -S mygame -B mygame\build_diag -DRECOMP_DIAG=ON`.

## Licence of the port's own code

The port's own code (everything in `mygame\` and the changes to the toolkit) is under the MIT licence, see
`LICENSE-PORT.txt`. The toolkit keeps its own MIT licence, and the xemu-derived files stay under the
LGPL 2.1 or later (see `xboxrecomp-0.16.0/LICENSES/`). The game, its name, art, music and code belong to
Electronic Arts; nothing here gives any right to them.
