## What's new in 0.9.2-beta

A small fix release for three things players reported:

- **Karabiner 98k now hits through the scope too.** 0.9.1 fixed hip fire; with the scope up the bullet still left the
  barrel off the crosshair at high frame rates. Scoped shots now follow the camera exactly, like hip fire.
- **Mouse wheel zooms the sniper scope.** While a scope is up, wheel up/down zooms in/out through the game's own zoom
  steps, the same as the arrow keys and the pad's D-pad (those still work).
- **Automatic fire speed** (new row in Recomp Settings and in the launcher): at 60 fps and above the MP40 and other
  automatic weapons fired about 1.6 times faster than the game was played at 30 fps. Choose **Fast** (the game's own
  speed at your frame rate), **Normal** (the 30 fps pace, the new default), **Slow** or **Slowest**. It only touches
  automatic weapons, never the Karabiner or pistols. Known limit: the game fires its bullets in pairs, so the pace is
  per pair.

