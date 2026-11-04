DIFFICULTY TUNING (experimental) / ضبط الصعوبة (تجريبي)
========================================================

Edit mods\ai\tuning.txt: one change per line, name = value, '#' starts a comment. Restart the game.
عدّل mods\ai\tuning.txt: سطر لكل تعديل بصيغة الاسم = القيمة، و'#' يبدأ تعليقاً. أعد تشغيل اللعبة.

Tested in game / مجرّب داخل اللعبة
----------------------------------
MaxHealth.Recruit|Veteran|Elite|Hero|All = <number>   Player max health per difficulty (400 / 200 / 100 / 50).
                                                     صحة اللاعب القصوى لكل مستوى صعوبة.
Revives.Recruit|Veteran|Elite|Hero|All = <0..9>       Starting revives (2 / 1 / 0 / 0).
                                                     عدد مرات الإحياء عند البداية.

Experimental, not tested / تجريبي غير مجرّب
-------------------------------------------
HitZoneMult0..3 = <number>   Damage multiplier by body hit zone in the soldier hit handler (1 / 0.85 / 5 / 1).
                             مضاعف الضرر حسب منطقة الإصابة (غير مؤكد).
*= multiplies instead of sets, e.g.  MaxHealth.All *= 2
Raw form: <hex address> = <float> or <hex address> = i:<int>, only inside 376148-3766BF.

Not found: enemy accuracy, reaction time and sight range. The game's difficulty only changes the player (health, revives).
لم يُعثر على دقة الأعداء وزمن ردّهم؛ الصعوبة في اللعبة تغيّر اللاعب فقط (الصحة والإحياء).

EnemyCount = <k>                  Multiplies how many enemies each spawner may keep alive (1 = off, 2 = double). All difficulties.
EnemyCount.Recruit|Veteran|Elite|Hero = <k>   Same, for one difficulty only (overrides the general value).
عدد الأعداء: يضاعف عدد الأعداء الأحياء المسموح لكل نقطة ظهور (1 = معطّل). التأثير على الأعداء الذين تنتجهم نقاط الظهور فقط،
وقد لا يضاعف الجنود الموضوعين مسبقاً في المهمة. قد ينخفض الأداء. مجرّب: لا.
