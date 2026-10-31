# The story of this port

This page is the long version: how *Medal of Honor: European Assault* went from an Xbox disc to a native
Windows game, told roughly in the order it happened. It was built with [Claude Code](https://claude.com/claude-code)
doing the reverse-engineering and coding, and the author testing every change in the real game. If you just
want the feature list, see the [README](README.md).

## Getting it to boot at all

The port is built with **static recompilation**: the original Xbox program (`DEFAULT.XBE`) is translated,
function by function, into plain C code, which is then compiled into a normal Windows `.exe`. There is no
emulated CPU running in a loop — the game's own logic runs as native code. What *is* emulated is the Xbox's
hardware: its sound chip, its graphics chip, memory layout, and so on, enough for the translated game code
to believe it's still running on the original console.

Getting from "it compiles" to "the first level is playable" meant chasing down a long list of individual
bugs: the game froze right after the intro; cutscene movies needed to be skippable because they first
played with no sound; big, warped shapes appeared on screen because of a vertex-math edge case; menu
navigation worked, then stopped working after an optimization. Each of these was found by comparing what
the game expected against what the emulated hardware was actually doing, and fixed one at a time.

## From a slow software renderer to real GPU rendering

The very first playable build drew every pixel on the CPU — about 1.3 frames per second. Getting this into
"feels like a real game" territory took two big steps and a long tail of smaller ones:

1. **Software optimization first**: caching decoded textures, compiling the graphics chip's pixel-shader
   programs instead of interpreting them every frame, running vertex math on multiple CPU threads at once.
   This alone took it from ~1.3 to ~25-30 fps.
2. **A real GPU renderer**: instead of emulating the Xbox's graphics chip in software, its drawing commands
   are now translated into Direct3D 11 calls that run on your actual graphics card. This is what makes
   high resolutions, 4K, and frame rates far above the Xbox's original 30 fps possible at all.

After that, performance work shifted to finding where time was actually going: how long the game's own
logic took per frame, how long the graphics translation took, and removing avoidable waiting between the
two. Dozens of smaller fixes (batching draw calls, enlarging internal buffers the game was contending on,
skipping synchronization points that turned out to be unnecessary) added up to the game running comfortably
above 100 fps in most scenes on modern hardware, with only occasional dips in very busy fights.

## Controls that feel native

The original game expects an Xbox controller. Supporting mouse and keyboard properly meant finding the
exact piece of game code that turns a controller stick's movement into a camera turn, and replacing it with
one that reads the mouse instead — so looking around has the same immediacy as in a native PC game, instead
of the "stick acceleration" feel of a converted analog input. On top of that: full key rebinding, a
gamepad path with its own remapping (so a modern pad's layout and extra buttons line up with the Xbox
original's "Sharp Shooter" control scheme), and on-screen button prompts that automatically switch between
keyboard keys, Xbox glyphs and PlayStation glyphs depending on what you last touched — drawn by finding and
decoding the game's own prompt-icon art inside its UI archives and its in-mission HUD sheet.

## Seeing further, aiming your way

Two early requests shaped a lot of later work: a wider field of view, and a longer draw distance. Both
turned out to be more involved than "change one number" — widening the field of view alone made background
objects start popping in and out of existence, because the game's own visibility and level-of-detail checks
read the *same* angle value used for rendering. The real fix widens the camera at its source so every system
that depends on it (rendering, visibility culling, level-of-detail) agrees, with a smooth ease instead of a
jarring snap when the aiming camera zooms in and out. Draw distance needed its own separate per-object cull
fixed the same way. Both are live-adjustable while playing.

A related, much-requested option is **"Super Accuracy"**: the original game has every player weapon spread
its shots and climb with recoil. The two numeric fields responsible for that (found by instrumenting the
game's own weapon-loading code and matching the results against the actual Xbox behavior) can now be zeroed
out, for players who want a steadier, more precise feel.

## Weapon and mod support

Building on the weapon research above, a simple **text-file modding system** lets you tweak a weapon's
magazine size (confirmed working), and experimentally its max ammo, starting ammo, fire rate, damage and
range — just by editing a file named after the weapon in `mods\weapons`. The same idea extends to
**textures** (replace any decoded texture by dropping a PNG with the right filename into `mods\textures`,
after dumping the original textures from the game with a built-in menu option) and to **sound effects**
(same idea, with WAV files, in `mods\sounds`). None of this requires programming — just text files and
images, and none of it ships any of the original game's extracted art or audio; every player builds their
own from their own copy.

## Chasing sound bugs

Audio turned out to hide three separate, unrelated bugs:

- **A metallic whistle in the music.** This one took real detective work: comparing the port's decoded
  audio against a reference decode of the same files bar by bar, byte for byte, which pointed to a single
  wrong instruction translation in the audio codec — an 8-bit multiply that was translated as a wider one,
  which silently corrupted an internal lookup-table index. Once identified, it was a small, precise fix with
  a big, audible result: the whistle is completely gone.
- **Center and low-frequency channels only coming out of one ear.** The game mixes its sound effects across
  six speaker channels (front-left/right/center, rear-left/right, subwoofer) even though most players only
  have stereo speakers; the code that folds six channels down to two had a channel-pairing bug. Fixed with a
  proper six-to-two mix.
- **Sound occasionally vanishing for the rest of a session.** The hardest of the three: all background
  music, dialogue and ambience could silently stop after a long play session, while gunfire and other short
  effects kept working. Tracking it down meant live-attaching diagnostic tools to the running game to watch
  its internal audio state change in real time, which eventually showed a shared internal "ring buffer"
  occasionally getting its position frozen by what looks like two game threads' sound commands colliding. A
  guard now detects this and automatically restarts the shared buffer without interrupting the level — it
  has not reproduced since, but the guard stays in as a safety net, and is the one issue still asked to be
  reported if anyone sees it.

## More blood, screen effects, rain, sharper text

A set of visual extras were added entirely by finding the right data inside the game's own files and
exposing it as options, rather than drawing anything new into the 3D scene:

- **Blood** is a particle effect the game already has; its amount, size, color, lifetime and gravity are
  now tunable (Normal/More/Lots/Extreme), and a long-standing quirk where blood only appeared on a random
  fraction of hits was tracked down and fixed so it (optionally) shows on every hit.
- **Screen blood, screen dirt and rain drops** are a new idea on top of that: a transparent overlay window
  drawn directly on your screen, independent of the 3D scene, that reacts to nearby hits, nearby explosions,
  and the game's own weather effects (found by locating the particle definitions St. Nazaire's rain levels
  use).
- **Sharper fonts**: every one of the game's 8 built-in fonts (menus, HUD, subtitles) is redrawn at a much
  higher resolution from a clean source and dropped in through the same texture-replacement mechanism used
  for mods, keeping the original glyph layout and spacing so nothing in the UI shifts.

## Letting the movies play, and skipping them if you want

Movies (the pre-rendered cutscenes) used to be skipped by default, because they originally played back with
no sound. With that fixed, movies now play normally, and a settings option lets you choose whether a button
press skips them, whether the game's own skip rule applies, or whether to never show them at all.

## A full Arabic language pack

This is one of the larger single efforts in the project: decoding the game's own text, font and archive
formats well enough to replace them wholesale rather than patch around them. That meant writing a small
toolchain that can shape Arabic text into its correct joined letterforms, reverse it into the visual
left-to-right order the game's text renderer expects, redraw every font's glyph atlas with the added Arabic
letters (while making room by dropping accented Latin letters the pack doesn't need), and rebuild the game's
own archive and movie-subtitle formats byte-for-byte. The result is a mostly-translated Arabic pack —
menus, HUD, objectives, most dialogue — with right-to-left, mirrored menus, including a fully Arabic,
mirrored version of this project's own in-game settings screen. Some longer subtitle lines in narrow text
boxes still don't wrap perfectly, which is the one honestly-unsolved piece of it.

## The launcher

Rather than ask players to understand environment variables or edit an ini file by hand, a small standalone
launcher (English and Arabic) was built from scratch: it checks your game files and Windows components, has
ready-made performance presets, lets you rebind keys without touching the in-game menu, backs up your save
files, checks GitHub for a newer release, and has a one-click "report a problem" button that opens an issue
here with your system information attached.

## Turning it into something you can actually download

The last stretch of work wasn't about the game itself: making sure dozens of development-only debug hotkeys
and tools are disabled in the version you download, writing the install instructions, known-issues list and
credits, and building the actual installable package (this repository's releases) from the working project
folder, as a repeatable, one-command step.

## Things that were tried and didn't make the cut

Not everything attempted shipped. Letting killed soldiers' bodies stay on the ground longer than the
original few seconds looked simple but kept breaking something else every time one cause was fixed — other
soldiers freezing, bodies turning into strange spiky shapes, or going see-through — and was eventually
dropped rather than ship something half-working. A generic "edit any setting from a text file" mod system
was built and then deliberately removed once it became clear every one of those settings already had a
proper menu option in the launcher or the in-game settings screen; the file-based mod system that *did* ship
was narrowed to only the two things that genuinely have no menu equivalent: weapon stats and, going
forward, enemy AI behavior (an early, promising lead there — plain-text tuning values for vehicle gunner AI
— is still unexplored for regular infantry).

## What's next

Enemy AI tuning (continuing the lead above), the rest of the Arabic translation (subtitles and credits),
and whatever the first wave of player reports on [Issues](../../issues) turns up.

---

# ملخص بالعربية

هذا المشروع هو نسخة لعبة *Medal of Honor: European Assault* الأصلية من إكس بوكس وتحويلها إلى لعبة ويندوز
أصلية، عبر **إعادة ترجمة** برنامج اللعبة نفسه (لا محاكاة) إلى كود يعمل مباشرة على المعالج. بدأ المشروع
بإقلاع اللعبة ومعالجة عشرات الأخطاء الصغيرة حتى أصبح المستوى الأول قابلاً للعب، ثم تحوّل الرسم من معالج
برمجي بطيء (أقل من 2 إطار/ثانية) إلى رسم حقيقي على كرت الشاشة (Direct3D 11) يتجاوز 100 إطار/ثانية في أغلب
المشاهد.

بعدها أضيفت ميزات التحكم (ماوس وكيبورد بتصويب أصلي، تخصيص المفاتيح، أيقونات أزرار تتبدل تلقائياً)، مجال رؤية
ومسافة رسم أكبر (مع حل مشكلة اختفاء الأجسام البعيدة التي ظهرت أول مرة)، خيار "دقة عالية" بلا ارتداد أو
تشتت للرصاص، ونظام تعديلات بملفات نصية وصور (أسلحة، خامات، أصوات).

في الصوت تم حل ثلاث مشاكل مختلفة تماماً: صفير معدني في الموسيقى (سببه خطأ ترجمة عملية ضرب صغيرة في كود فك
الصوت)، صوت المنتصف والمؤثرات الخلفية يخرج من جهة واحدة فقط (مشكلة في دمج القنوات الست إلى اثنتين)، واختفاء
الصوت بالكامل بعد جلسات طويلة (أصعب مشكلة، حُلّت بآلية حماية تعيد تشغيل الصوت تلقائياً دون مقاطعة اللعب).

أضيفت أيضاً: دم أكبر وأوضح وعلى كل إصابة، تأثيرات دم وغبار ومطر على الشاشة نفسها، خطوط أوضح لكل نصوص اللعبة،
خيار لتخطي الفيديوهات، **حزمة لغة عربية كاملة تجريبياً** بقوائم من اليمين لليسار حتى في قائمة الإعدادات
الخاصة بنا، ومشغّل (لانشر) عربي/إنجليزي يسهّل كل هذا على اللاعب. بعض الأفكار جُرّبت ولم تنجح (إبقاء جثث
القتلى لفترة أطول) وأُزيلت بصراحة بدل نشرها ناقصة.
