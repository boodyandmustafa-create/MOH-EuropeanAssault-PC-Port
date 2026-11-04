MODS FOLDER / مجلد المودات
==========================

How it works / كيف يعمل
-----------------------
Every file the game opens from its own folder (mygame\game) is looked up here first.
A file at  mods\<same path as in the game folder>  is opened INSTEAD of the original.
The original files are never touched, so removing a file from here brings back the original.

كل ملف تفتحه اللعبة من مجلدها (mygame\game) يُبحث عنه هنا أولاً. إن وُجد ملف بنفس المسار النسبي
هنا يُفتح بدل الأصلي. لا يُعدَّل الملف الأصلي أبداً، وحذف ملفك من هنا يعيد الأصلي.

Examples / أمثلة
----------------
  game\DATA\game.cfg                  ->  mods\DATA\game.cfg
  game\DATA\HUD\SP\hudSP.xml          ->  mods\DATA\HUD\SP\hudSP.xml
  game\DATA\1\1_1\strings.viv         ->  mods\DATA\1\1_1\strings.viv

Mod packs / حزم المودات
-----------------------
Put each mod in its own sub-folder, with the game's folder layout inside:
  mods\MyMod\DATA\game.cfg
Rule: a sub-folder whose name is also a folder of the game (DATA, lib, scrns ...) is NOT a pack,
it is part of the loose layout above. Packs are applied last name first, so with two packs touching the
same file the one whose name comes LATER alphabetically wins. Loose files directly under mods\DATA ...
win over every pack.

Weapon mods / مودات الأسلحة
----------------------------
mods\weapons\<WeaponName>.txt tunes that weapon's own stats (magazine size, damage, recoil...) --
see mods\weapons\README.txt for the full key list and what is confirmed vs. still experimental.
mods\weapons\<اسم السلاح>.txt يضبط خصائص ذلك السلاح (سعة المخزن، الضرر، الارتداد...) -- راجع
mods\weapons\README.txt لكل المفاتيح وما هو مؤكَّد أو لا يزال تجريبياً.

Notes / ملاحظات
---------------
* A file that exists only in mods (not in the game folder) can be opened by name; folder listings
  are not merged.
* Replacement must have the same internal format. Level archives (.str, .rez, .abk, .big, .viv)
  are indexed by offsets: a changed size breaks them unless you rebuild the archive properly.
  Plain text files (.xml, .cfg) are the safest to edit.
* The log (play_log.txt) lists every redirected file as   [MODS] <game path> -> <mod path>
  and the layers found at start-up. RECOMP_MODS=0 switches the folder off, RECOMP_MODS_DIR=<folder>
  moves it, RECOMP_MODS_LOG=0 hides the log lines.
* Do not ship copyrighted game files from here when publishing your port.
