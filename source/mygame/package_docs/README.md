# Medal of Honor: European Assault — Remastered (PC port)

**Version 0.9.0-beta** · Windows 10/11 64-bit · unofficial fan project

A native Windows port of *Medal of Honor: European Assault* (Xbox, USA / NTSC-U), made by statically
recompiling the game's own program (`DEFAULT.XBE`) to a PC executable. It is **not an emulator**.
**No game data is included** — you need your own copy of the game.

> Not affiliated with, endorsed by or sponsored by Electronic Arts. *Medal of Honor* and *European Assault*
> are trademarks of Electronic Arts Inc. See [NOTICE.txt](NOTICE.txt).

## What you get

- Native 16:9 / widescreen rendering (Direct3D 11), any resolution up to 4K, FOV and draw distance options
- 30 – 240 fps (only **60 fps is stable**; 120 / 144 / 165 / 240 are marked *unstable* in the launcher)
- Mouse and keyboard with full key binding (and gamepad support), native mouse aim
- Sharper menu / HUD / subtitle fonts (English), keyboard / Xbox / PlayStation button prompts
- Arabic language pack (experimental, partial translation, right-to-left menus)
- Extras (all optional, in the launcher's *Extras* tab): more blood, screen blood / dirt / rain effects,
  no weapon recoil / spread, sprint, cutscene skip
- Texture mods: drop a PNG into `mods\textures` and it replaces the game's texture
- A launcher with English and Arabic UI

## Requirements

- Windows 10 or 11, 64-bit; a GPU with Direct3D 11 (feature level 11.0)
- [Microsoft Visual C++ Redistributable 2015-2022 (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe)
  (if Windows says `vcruntime140.dll` / `MSVCP140.dll` is missing)
- Your own copy of **Medal of Honor: European Assault for Xbox, USA / NTSC-U**. Other regions are not supported.

## Install

1. Unzip this folder anywhere you can write to (not `Program Files`).
2. Put the game's files into the **`game`** folder, so that you have
   `game\DEFAULT.XBE`, `game\DATA\...` and `game\EAGLRM.O`.
   If you only have a disc image (ISO), unpack it with
   [xdvdfs](https://github.com/antangelo/xdvdfs) (`xdvdfs unpack <game.iso> game`) or any Xbox ISO extractor.
3. Run **`Launcher.exe`**. The *Play* page shows green dots when everything was found and the game version is
   recognised. Choose your settings, press **PLAY**.
4. The first start (and every new area) compiles shaders: short stutters are normal and go away
   (they are cached in `shader_cache\`).

You can also start `MOHEuropeanAssault.exe` directly: it reads the same `recomp_settings.ini` the launcher writes.

## Settings

Everything is in the launcher (Game, Controls, Mods, Extras, Advanced tabs) and also inside the game:
*Options → Credits* opens the **Recomp Settings** screen (`F1` opens it from any menu). `F11` / `Alt+Enter`
toggle fullscreen and `F12` saves a screenshot to `screenshots\`. A fresh package starts at 60 fps with the
picture size taken from your screen; the launcher's *Game* tab has two presets (Balanced / High performance).

## Mods

See [`mods\README.txt`](mods/README.txt) and [`mods\textures\README.txt`](mods/textures/README.txt).
The launcher's *Mods* tab opens the folders. Language packs live in `languages\<name>`.

## Problems

Please report them on the [Issues page](https://github.com/boodyandmustafa-create/MOH-EuropeanAssault-PC-Port/issues)
(or the launcher's **Report a problem** button, which copies your system info and opens the folder with `play_log.txt` selected). Attach `play_log.txt` (and `play_log_prev.txt`) from the game folder
and say which game version / GPU / Windows you use. Known problems are listed in
[KNOWN_ISSUES.txt](KNOWN_ISSUES.txt).

The executables are unsigned, so Windows SmartScreen or an antivirus may warn about them (false positive for
a new, unsigned program). The SHA-256 of the release zip is on the release page.

## Credits

- Medal of Honor: European Assault © Electronic Arts. Cover art, the launcher music (a menu track of the game) and the
  menu cursor are EA's property and are shown here only to credit the game.
- **xboxrecomp** by sp00nz (MIT) — the Xbox recompilation toolkit: <https://github.com/sp00nznet/xboxrecomp>
- **xemu** (LGPL-2.1+; espes, Jannik Vogel, Matt Borgerson and contributors) — NV2A / APU code: <https://github.com/xemu-project/xemu>
- **xdvdfs** by antangelo — Xbox ISO unpacking: <https://github.com/antangelo/xdvdfs>
- Written and debugged together with **Claude Code** (Anthropic): <https://claude.com/claude-code>
- Microsoft DirectX 11, XAudio2, GDI+

## License

Free to use for personal, non-commercial purposes, provided unchanged with all credits. See
[LICENSE.txt](LICENSE.txt) (the LGPL parts are covered by [SOURCE.txt](SOURCE.txt)).

---

# العربية

**ميدل أوف أونر: يوروبيان أسولت — ريماسترد (نسخة الحاسوب)** · الإصدار 0.9.0-beta · مشروع معجبين غير رسمي

نسخة أصلية لويندوز من اللعبة (إكس بوكس، الإصدار الأمريكي NTSC-U) مصنوعة بإعادة ترجمة برنامج اللعبة نفسه إلى
تنفيذي للحاسوب. **ليست محاكياً. ولا تتضمن أي ملفات من اللعبة** — تحتاج نسختك الخاصة.

**التثبيت**
1. فك الضغط في أي مجلد تقدر الكتابة فيه (ليس `Program Files`).
2. ضع ملفات اللعبة في مجلد **`game`** بحيث يوجد `game\DEFAULT.XBE` و`game\DATA` و`game\EAGLRM.O`.
   إن كانت عندك صورة قرص (ISO) فك ضغطها بأداة [xdvdfs](https://github.com/antangelo/xdvdfs) أو أي أداة مشابهة.
3. شغّل **`Launcher.exe`** (فيه زر العربية أسفل اليسار)، اضبط الخيارات ثم اضغط **PLAY / العب**.
4. أول تشغيل يبني الشيدرز فقد تلاحظ تقطعات بسيطة تزول لاحقاً.

إن ظهرت رسالة نقص `vcruntime140.dll` ثبّت [Visual C++ Redistributable x64](https://aka.ms/vs/17/release/vc_redist.x64.exe).

**المشاكل:** أبلغ عنها في [صفحة Issues](https://github.com/boodyandmustafa-create/MOH-EuropeanAssault-PC-Port/issues)
وأرفق الملف `play_log.txt`. التحذيرات من SmartScreen أو مضاد الفيروسات متوقعة لأن الملفات غير موقّعة.

**ملاحظات:** 60 إطاراً/ثانية فقط مستقرة؛ ما فوقها مُعلَّم "غير مستقر". الترجمة العربية تجريبية وجزئية.
