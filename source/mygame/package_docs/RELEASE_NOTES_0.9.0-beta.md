# Medal of Honor: European Assault – PC Port 0.9.0-beta

(GitHub release text. Tick "Set as a pre-release" is fine: the launcher's update check reads the release list, so it sees
pre-releases too. Attach the zip and paste its SHA-256.)

An unofficial PC port of *Medal of Honor: European Assault* (Xbox, USA / NTSC-U) made by statically recompiling the
game for Windows. **No game files are included – you need your own copy** (copy `DEFAULT.XBE`, the `DATA` folder and
`EAGLRM.O` into the `game` folder, then start `Launcher.exe`).

## Highlights

- Direct3D 11 renderer, widescreen 16:9, any resolution up to 4K, FXAA, anisotropic filtering
- Wide field of view and a much longer draw distance (menu / launcher options)
- Mouse and keyboard with full key binding, native mouse aim, gamepad with rumble, Xbox / PlayStation / keyboard prompts
- Sharper Impact-style fonts for every menu, HUD and subtitle text
- Optional extras: more blood, screen blood / dirt / rain, no recoil / spread ("Super Accuracy"), sprint, cutscene skip
- Texture replacement (`mods\textures`), language packs (`languages\`), screenshots (`F12`)
- Arabic language pack (experimental, partly translated, right-to-left menus)
- The launcher: setup check, presets, key binding, saves backup, English + Arabic

## Please know

- Only the USA (NTSC-U) version is supported.
- Only **60 fps is steady**; 120 / 144 / 165 / 240 are experimental.
- Sound may very rarely vanish in a long session – please report it with `play_log.txt`.
- Needs a Direct3D 11 GPU and the Visual C++ Redistributable (the launcher tells you if it is missing).
- Windows SmartScreen / antivirus may warn about the unsigned executables.

Report problems with the launcher's **Report a problem** button (it copies your system info and opens the folder with
`play_log.txt` selected – drag the log into the issue).

---

# ميدل أوف أونر: يوروبيان أسولت – نسخة الكمبيوتر 0.9.0-beta

نسخة كمبيوتر غير رسمية من اللعبة (إكس بوكس، النسخة الأمريكية) بإعادة ترجمة اللعبة لتعمل على ويندوز.
**لا تحتوي الحزمة على ملفات اللعبة – تحتاج نسختك الخاصة.** انسخ `DEFAULT.XBE` ومجلد `DATA` والملف `EAGLRM.O` إلى مجلد
`game` ثم شغّل `Launcher.exe`.

**أهم المزايا:** رسم Direct3D 11 بشاشة عريضة وحتى 4K، مجال رؤية ومسافة رسم أكبر، ماوس وكيبورد بتخصيص كامل مع تصويب أصلي
واهتزاز اليد وأيقونات (إكس بوكس / بلايستيشن / كيبورد)، خطوط أوضح للقوائم والترجمة، إضافات اختيارية (دم أكثر، دم وغبار
ومطر على الشاشة، دقة عالية بلا ارتداد، ركض، تخطي الفيديو)، استبدال الخامات، حزم اللغات، لقطة شاشة بالمفتاح `F12`،
لغة عربية تجريبية، ومشغّل يفحص الملفات ويحفظ النسخ الاحتياطية للحفظ.

**ملاحظات:** النسخة الأمريكية فقط – 60 إطاراً فقط مستقرة – قد يختفي الصوت نادراً في الجلسات الطويلة – يلزم كرت Direct3D 11
وVisual C++ Redistributable – وقد يحذّر SmartScreen لأن الملفات غير موقّعة.

---

## Pictures to take for the release page (checklist)

1. Launcher – Play tab (English) and (Arabic) – `Launcher.exe --shot <folder>` renders every tab to PNG
2. A St. Nazaire / rain level at 2560x1440 with the extras on (rain + screen blood)
3. Same spot with FOV 70 vs 90, draw distance 1 vs 5 (side by side)
4. Blood: normal vs "Lots"
5. The HD fonts: objectives screen and a subtitle
6. Recomp Settings screen in the game (English and Arabic)
7. A 20–30 second clip (OBS): a firefight with the mouse aim + rain
