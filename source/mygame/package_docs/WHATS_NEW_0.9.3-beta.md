## What's new in 0.9.3-beta

The biggest release so far: a **map editor**, a new **weapon lighting** system, blood stains, an ultrawide view, a free
camera, many gameplay options and tools to find crashes.

> ⚠️ **The Map editor is EXPERIMENTAL.** It is off by default and still in testing. It may show bugs, and in rare cases it
> can crash or freeze the game, especially with many placed soldiers or when changing levels. Your edits are saved per level
> in `mods\maps\`; if a level misbehaves, turn the editor off or delete that level's file. Please report problems on
> GitHub Issues with `play_log.txt` and `crash.txt`.

### Map editor (experimental)

Turn it on in Recomp Settings (F1) > Mods and tools > Map editor, press **Insert** for the free camera, then **Numpad 0** for
the editor window.

- **Characters:** drag any German or Allied soldier type into the world. It lands where the mouse points, on the floor or on top
  of a crate. A translucent stand-in shape follows the mouse while you drag.
- **Props:** drag the pickups the level knows (medkit, ammo, documents, the level's weapons). They glow and can be picked up.
- **Zones:** your own spawn zones. Pick a soldier type, set soldiers per wave, radius, waves and wake distance, then drag "New
  zone" into the world. Soldiers are born inside the ring and walk around in it; the next wave comes after you leave and come back.
- **Delete:** a short right click removes the soldier, prop or zone under the pointer (hold the button to look around).
- Placed soldiers stay where you drop them and still fight, take cover and throw grenades. Up to 16 living editor soldiers.
- Everything is saved per mission in `mods\maps\<level>.txt` and comes back at mission start and after a restart. The
  **Edits: On/Off** button starts a level as the game made it without deleting your file.

*Known limits:* 16 living soldiers at most; no prop rotation; no weapons from other levels; zones cannot be edited after
placing; rings show through walls; soldiers can be born inside crates; the drag preview is a shape, not the real model; the
level's own pickups cannot be removed; window text is English only; not in the launcher yet.

### Weapon lighting

Recomp Settings (F1) > Effects > **Weapon lighting**: Off / Low / Normal / High / Extreme. The gun and hands get directional
light and reflections from the scene (a lamp on your right lights the right side of the gun), plus dust after explosions,
blood splatters when you are hurt and animated rain drops. No fps cost: only the weapon's own pixels are touched.

### Blood stains

Hits leave blood on walls and floors, deaths leave a pool. Drawn from your own blood pictures, tinted by the Blood colour
setting, in every level. F1 > Effects > **Blood stains** (Off / Low / Normal / High) and **Stain limit**.

### Weapon feel

The weapon has weight: when you turn the camera it lags behind and catches up with a small swing.

**Sprint pose:** while sprinting the weapon is carried across the body and swings with your steps. Firing or aiming brings it
back at once.

**Scope sway:** under a quarter of your health, the aim in a scope moves in a slow wave.

### Hit marker and explosions

### Free camera

Press **Insert** in a mission to leave the soldier and fly anywhere (W A S D, mouse, Space / C up and down, Shift faster,
Ctrl slower). The world stands still by default, or keeps running if you choose.

### More new things

- **Enemy grenades:** enemies who shoot at you now throw grenades (Gameplay > Enemy grenades: Low / Medium / High). The original
  game never made enemies throw them in the levels we tried.
- **Ultrawide:** F1 > Video > **Aspect ratio** (4:3 / 16:9 / 21:9 / Auto, restart). The view grows sideways, the HUD goes to
  the screen edges, menus stay 16:9.
- **Damage mode** (Normal / Low damage): pistols and machine guns need 7 hits to kill, heavy rifles one head shot or two body shots.
- **Cheats tab** (launcher and F1): unlimited ammo, invincible, suicide, unlock all missions (your save is not changed), super
  accuracy, squad mates (Normal / Tough / Weak).
- **Auto switch weapon:** Off / Empty magazine / Out of ammo.
- **Grenade macro:** hold the grenade control to take it out and pull the pin, release to throw; your gun comes back after.
- **Mouse wheel = change weapon** (in a scope it still zooms).
- **Pain sounds and breathing** with your own WAV files in `mods\sounds\`, no fps cost.
- **HUD:** a vertical stamina bar, a fourth RUN icon, sharper stance icons, 5 gamepad icon styles (Xbox, Xbox 360, PS2/PS3,
  PS4, PS5), and 17+ HUD elements you can hide one by one.
- **Stereo sound** (Audio): Original or Mono. *If a sound seems to fade or vanish when you turn to one side, switch it to Mono.*
- **Heavy rifle aiming fix** (Gewehr, M1 Garand, scoped rifles and others) at high fps.
- **Texture mods:** BC7, BC4 and BC5 DDS files now work.
- **F1 menu** reorganised into categories, movable and resizable; the mouse wheel scrolls it.
- **Menus with the mouse:** the campaign and mission map screens now follow the pointer; the wheel zooms the first scope of a level.
- **Fixed:** stutter on entering a level with very large texture packs; a crash when entering a new level; dead bodies flung in
  the air at high fps (optional **Corpse fix (60 Hz physics)**, off by default, it can freeze the game).

### Help us find crashes

- If the game crashes, open a GitHub issue and attach `crash.txt`, `crash.dmp` and `play_log.txt` from the game folder. Say how
  long you had been playing and whether you use overlays (RTSS, Discord, Steam, ReShade).
- The launcher's **Report a problem** button now builds `MoH_report.zip` for you (logs, ini, system info; your Windows user name
  is removed).
- If parts of the ground turn pure black, press **F2** while it is black and attach `play_log.txt` and the screenshot.

### Ideas for later versions (not promised)

Things we want to try. None has a date and some may turn out impossible:

- Weapons from other levels placed by the editor (the BAR of the first levels is the hardest case).
- More editor objects (crates, barrels, vehicles, mounted guns), prop rotation, editing a zone after placing it.
- Soldiers that really use the game's cover.
- Higher-detail character models.
- The editor in the launcher and in Arabic.

Multiplayer reminder: local split-screen works. There is no online or LAN play, no bots and no campaign co-op: the original game
has no network code.

The full README with videos and pictures is on the GitHub page.
