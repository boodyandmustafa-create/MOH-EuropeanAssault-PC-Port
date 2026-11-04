WEAPON MODS / مودات الأسلحة
============================

Put a text file named after a weapon (e.g. Karabiner.txt, MP40.txt, Thompson.txt) in THIS folder
(mods\weapons) with key=value lines. Restart the game to apply. '#' starts a comment.
ضع ملفاً نصياً باسم السلاح (مثل Karabiner.txt) في هذا المجلد (mods\weapons) بأسطر مفتاح=قيمة. أعد تشغيل
اللعبة لتطبيقها. '#' يبدأ تعليقاً.

Keys that work / المفاتيح التي تعمل
-----------------------------------
MagazineSize=<n>   Rounds per magazine. Takes effect at the next reload.
                   عدد الطلقات في المخزن. يسري عند الإعادة التالية.
Damage=<float>     Damage of one bullet. 500 kills in one hit.
                   ضرر الرصاصة الواحدة. 500 تقتل بطلقة واحدة.
Spread=<float>     Scatter of the shots. Lower = tighter grouping, 0 = none.
                   تشتت الطلقات. الأقل = تجمّع أضيق، 0 = بلا تشتت.
Recoil=<float>     Camera kick when firing. Lower = steadier, 0 = none.
                   ارتداد الكاميرا عند الإطلاق. الأقل = أثبت، 0 = بلا ارتداد.
MaxAmmo=<float>    EX(MaxAmmo=999)the amount of ammo you can carry for each weapon كمية الذخيرة التي تجمعها للسلاح الواحد

MuzzleSpeed=<float>   the speed of the bullet التحكم في سرعة الرصاصة

Example: a one-shot-kill Karabiner, mods\weapons\Karabiner.txt:
مثال: كاربينر يقتل بطلقة، الملف mods\weapons\Karabiner.txt:
  Damage=500
  Spread=0

Other keys (StartingAmmo, FireRate, ZoomFOV, ZoomSensitivity, Range, Falloff) are still read
but do not change the game yet, so they are not listed. Do not rely on them.
مفاتيح أخرى (StartingAmmo وFireRate وZoomFOV وZoomSensitivity وRange وFalloff) تُقرأ لكنها لا
تغيّر اللعبة بعد، لذلك لم أذكرها. لا تعتمد عليها.

Packs / حزم
   mods\MyPack\weapons\...  works the same; a later pack name (alphabetically) wins.
   mods\MyPack\weapons\...  بنفس الطريقة؛ اسم الحزمة الأخير أبجدياً هو الذي يغلب.

Weapon names come from the game's own name string (not the pistol/rifle slot order, which is not
stable). Known names so far: 45Auto, MP45Auto, M1, Karabiner, Thompson, MG42, MP40, Brit, Stick,
Shotgun, enfieldScoped. The game also uses some of these weapons for the enemies, so a change may affect them too.
أسماء الأسلحة من اسم اللعبة الداخلي لها، لا من ترتيب حملها (غير ثابت). قد تستعمل اللعبة بعض هذه الأسلحة للأعداء
أيضاً، فقد يشملهم التعديل.
RECOMP_MODS=0 switches all mods (textures, sounds, weapons) off.
RECOMP_MODS=0 يعطّل كل المودات.
