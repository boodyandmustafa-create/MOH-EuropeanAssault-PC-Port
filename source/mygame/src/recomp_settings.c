/**
 * recomp_settings.c -- the in-game "Recomp Settings" screen.
 *
 * The game's Options menu (Game Settings, Audio/Video, Credits, Roll Call) is
 * EA Apt script inside mohfe.viv, which cannot be changed without shipping a
 * modified game file. So one of its entries is taken over at run time: when
 * the game asks for scrns\credits.big right after scrns\optionsMain.big (the
 * Credits item was picked in Options), a window of this program covers the
 * game and shows these settings; the game's own pad input is held at rest
 * meanwhile. Closing it presses B for the game so the Credits screen it
 * loaded behind backs out to Options.
 *
 * The choices live in recomp_settings.ini beside the exe. The ones that need
 * a restart (frame rate, resolution, fullscreen) become environment variables
 * at the next start (rs_startup, called first thing in WinMain); the mouse
 * speed and invert are read by input/mkb_input.c; the rest are live.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

extern void (*g_xbox_open_hook)(const char *path);
extern volatile long g_input_block;
extern volatile unsigned long g_input_inject_b_until;
double mkb_get_sens(void);
void   mkb_set_sens(double v);
int    mkb_get_invert_y(void);
void   mkb_set_invert_y(int on);
float  game_fov_get(void);
void   game_fov_set(float v);
float  game_draw_get(void);
void   game_draw_set(float v);
int    game_cutscene_mode_get(void);
void   game_cutscene_mode_set(int m);
int    game_super_accuracy_get(void);
void   game_super_accuracy_set(int on);
int    game_blood_get(void);
void   game_blood_set(int l);
const char *game_blood_name(int l);
int    game_blood_color_get(void);
void   game_blood_color_set(int i);
const char *game_blood_color_name(int i);
int    game_blood_hue_get(void);
void   game_blood_hue_set(int h);
int    game_blood_sat_get(void);
void   game_blood_sat_set(int s);
int    game_blood_bright_get(void);
void   game_blood_bright_set(int v);
void   game_blood_custom_rgb(int rgb[3]);
int    texdec_texmod_get(void);
void   texdec_texmod_set(int on);
extern void recomp_osd_show(const char *msg);
int    game_rain_cfg_get(int which);
void   game_rain_cfg_set(int which, int lv);
const char *game_rain_cfg_name(int which, int lv);
int    game_rain_cfg_pending(void);
int    game_blood_time_get(void);
void   game_blood_time_set(int i);
float  game_blood_time_seconds(void);
int    game_keep_get(void);
void   game_keep_set(int n);
extern volatile int g_show_fps;
int    recomp_screenfx_get(int kind);
void   recomp_screenfx_set(int kind, int level);
float  mkb_get_sprint_speed(void);
void   mkb_set_sprint_speed(float v);
float  mkb_get_sprint_time(void);
void   mkb_set_sprint_time(float v);
float  mkb_get_sprint_recover(void);
void   mkb_set_sprint_recover(float v);
float  nv2a_fog_mult_get(void);
void   nv2a_fog_mult_set(float v);
extern volatile int g_gpu_aniso;
#define MKB_ACT_COUNT 15
#define MKB_PB_COUNT  16
int  mkb_action_count(void);
const char *mkb_action_label(int a);
int  mkb_action_keys(int a, unsigned short out[4]);
void mkb_action_set_keys(int a, const unsigned short *keys, int n);
unsigned short mkb_move_key(int dir);
void mkb_set_move_key(int dir, unsigned short code);
int  mkb_pad_buttons(int a, signed char pb[2]);
void mkb_action_set_pad(int a, const signed char pb[2]);
const char *mkb_pad_label(int pb);
void mkb_key_label(unsigned code, char *out, size_t n);
void mkb_bindings_reset(void);
void mkb_bindings_write(void *file);
void mkb_release_all(void);
int  game_stambar_get(void);
void game_stambar_set(int on);
int  game_ec_get(void);                    /* ai_tuning.c: Enemy count 1..5 (1 = off) */
void game_ec_set(int k);
int  game_eclvl_get(int lvl);              /* per difficulty on/off: 0 Recruit, 1 Veteran, 2 Elite, 3 Hero */
void game_eclvl_set(int lvl, int v);
int  game_grentm_get(void);
void game_grentm_set(int on);
int  game_rain_on_get(void);
void game_rain_on_set(int on);
int    game_lod_get(void);
void   game_lod_set(int idx);
const char *game_lod_name(int idx);
extern volatile int g_gpu_fxaa;
int  mkb_get_icon_mode(void);
void mkb_set_icon_mode(int mode);
int  mkb_get_icon_style(void);
void mkb_set_icon_style(int style);

/* ---- the settings that need a restart ------------------------------------- */

#define RS_MAXLIST 12
static int  s_fps_list[RS_MAXLIST] = { 30, 60, 90, 120, 144, 240 };
static int  s_fps_n = 6;
static char s_res_list[RS_MAXLIST][16] = { "Default", "1280x720", "1920x1080", "2560x1440", "3840x2160" };
static int  s_res_n = 5;
static int  s_fps = 60, s_res = 0, s_full = 0;            /* chosen */
static int  s_split = 0, s_split_run = 0;                  /* split-screen pads: chosen / as this run started */
static int  s_fps_run, s_res_run, s_full_run;             /* as this run started */
static int  s_has_super = -1;                             /* the ini's Super Accuracy, -1 = not there */
static int  s_sdump, s_sdump_run;                         /* sound dump (RECOMP_SND_DUMP), chosen / as this run started */
static int  s_tdump, s_tdump_run;                        /* texture dump: 0 off, 1 PNG, 2 DDS; chosen / as this run started */
static int  s_enabled = 1;
#define RS_MAXLANG 8
static char s_lang_list[RS_MAXLANG][32] = { "english" };  /* "english" + every folder of languages\ */
static int  s_lang_n = 1;
static int  s_lang, s_lang_run;                           /* chosen / as this run started */
static int  s_rtl, s_curW, s_nomirror;
static char s_mlang_list[RS_MAXLANG][32] = { "english" };   /* "english" + the packs that have a recomp_menu.txt */
static int  s_mlang_n = 1;
static int  s_mlang;
static void rs_load_menu(int idx);

static int rs_list_fps(int v)
{
    int i;
    for (i = 0; i < s_fps_n; i++)
        if (s_fps_list[i] == v)
            return i;
    if (s_fps_n < RS_MAXLIST) {
        s_fps_list[s_fps_n] = v;
        return s_fps_n++;
    }
    return 1;
}

static int rs_list_res(const char *v)
{
    int i;
    if (!v || !*v)
        return 0;
    for (i = 0; i < s_res_n; i++)
        if (!_stricmp(s_res_list[i], v))
            return i;
    if (s_res_n < RS_MAXLIST) {
        snprintf(s_res_list[s_res_n], sizeof s_res_list[0], "%s", v);
        return s_res_n++;
    }
    return 0;
}

/* Lines of the ini this screen does not write itself (the launcher's keys: widescreen, mkb, pin_game ...)
 * are kept when it is saved. They are collected before the rewrite and appended after the known keys. */
#define RS_KEEP_MAX 64
typedef struct { char line[200]; } RsLine;

static int rs_has_key(FILE *f, const char *key, size_t klen)
{
    char l[200];
    rewind(f);
    while (fgets(l, sizeof l, f))
        if (!strncmp(l, key, klen) && l[klen] == '=')
            return 1;
    fseek(f, 0, SEEK_END);
    return 0;
}

static void rs_save_known(FILE *f);

static void rs_save(void)
{
    RsLine *old = (RsLine *)calloc(RS_KEEP_MAX, sizeof(RsLine));
    int nold = 0, i;
    FILE *in, *tmp, *out;
    char buf[512];
    size_t got;

    in = fopen("recomp_settings.ini", "r");
    while (in && old && nold < RS_KEEP_MAX && fgets(old[nold].line, sizeof old[nold].line, in)) {
        old[nold].line[strcspn(old[nold].line, "\r\n")] = 0;
        if (old[nold].line[0] && old[nold].line[0] != ';' && strchr(old[nold].line, '='))
            nold++;
    }
    if (in)
        fclose(in);

    tmp = fopen("recomp_settings.ini.tmp", "w+");
    if (!tmp) {
        free(old);
        return;
    }
    rs_save_known(tmp);
    fseek(tmp, 0, SEEK_END);
    for (i = 0; i < nold; i++) {
        size_t klen = (size_t)(strchr(old[i].line, '=') - old[i].line);
        if (!rs_has_key(tmp, old[i].line, klen))
            fprintf(tmp, "%s\n", old[i].line);
    }
    free(old);

    out = fopen("recomp_settings.ini", "w");
    if (out) {
        rewind(tmp);
        while ((got = fread(buf, 1, sizeof buf, tmp)) > 0)
            fwrite(buf, 1, got, out);
        fclose(out);
    }
    fclose(tmp);
    remove("recomp_settings.ini.tmp");
}

static void rs_save_known(FILE *f)
{
    fprintf(f, "; Written by the in-game Recomp Settings screen (Options > Credits). Safe to edit by hand.\n");
    fprintf(f, "sens=%.2f\n", mkb_get_sens());
    fprintf(f, "invert_y=%d\n", mkb_get_invert_y());
    fprintf(f, "super_accuracy=%d\n", game_super_accuracy_get());
    fprintf(f, "blood=%d\n", game_blood_get());
    fprintf(f, "blood_color=%d\n", game_blood_color_get());
    fprintf(f, "blood_hue=%d\n", game_blood_hue_get());
    fprintf(f, "blood_sat=%d\n", game_blood_sat_get());
    fprintf(f, "blood_bright=%d\n", game_blood_bright_get());
    fprintf(f, "blood_time=%d\n", game_blood_time_get());
    fprintf(f, "keep_corpses=%d\n", game_keep_get());
    fprintf(f, "fps_counter=%d\n", g_show_fps ? 1 : 0);
    fprintf(f, "screen_blood=%d\n", recomp_screenfx_get(0));
    fprintf(f, "screen_dirt=%d\n", recomp_screenfx_get(1));
    fprintf(f, "lod=%d\n", game_lod_get());
    fprintf(f, "stamina_bar=%d\n", game_stambar_get());
    fprintf(f, "grenade_timer=%d\n", game_grentm_get());
    fprintf(f, "enemy_count=%d\n", game_ec_get());
    fprintf(f, "enemy_count_recruit=%d\n", game_eclvl_get(0));
    fprintf(f, "enemy_count_veteran=%d\n", game_eclvl_get(1));
    fprintf(f, "enemy_count_elite=%d\n", game_eclvl_get(2));
    fprintf(f, "enemy_count_hero=%d\n", game_eclvl_get(3));
    fprintf(f, "rain=%d\n", game_rain_on_get());
    fprintf(f, "screen_rain=%d\n", recomp_screenfx_get(2));
    fprintf(f, "rain_amount=%d\n", game_rain_cfg_get(0));
    fprintf(f, "rain_size=%d\n", game_rain_cfg_get(1));
    fprintf(f, "rain_speed=%d\n", game_rain_cfg_get(2));
    fprintf(f, "texdump=%d\n", s_tdump);
    fprintf(f, "texmod=%d\n", texdec_texmod_get());
    fprintf(f, "sounddump=%d\n", s_sdump);
    fprintf(f, "cutscenes=%s\n", game_cutscene_mode_get() == 2 ? "off" : game_cutscene_mode_get() == 1 ? "play" : "skip");
    fprintf(f, "fps=%d\n", s_fps_list[s_fps]);
    fprintf(f, "res=%s\n", s_res ? s_res_list[s_res] : "");
    fprintf(f, "fullscreen=%d\n", s_full);
    fprintf(f, "split_pads=%d\n", s_split);
    fprintf(f, "sprint_speed=%.2f\n", mkb_get_sprint_speed());
    fprintf(f, "sprint_time=%.1f\n", mkb_get_sprint_time());
    fprintf(f, "sprint_recover=%.1f\n", mkb_get_sprint_recover());
    fprintf(f, "fog=%.1f\n", nv2a_fog_mult_get());
    fprintf(f, "aniso=%d\n", g_gpu_aniso);
    fprintf(f, "fxaa=%d\n", g_gpu_fxaa);
    fprintf(f, "language=%s\n", s_lang_list[s_lang]);
    fprintf(f, "menu_lang=%s\n", s_mlang_list[s_mlang]);
    fprintf(f, "icons=%s\n", mkb_get_icon_mode() == 1 ? "pad" : mkb_get_icon_mode() == 2 ? "keyboard" : "auto");
    fprintf(f, "icon_style=%s\n", mkb_get_icon_style() ? "ps" : "xbox");
    mkb_bindings_write(f);
}

static void rs_putenv(const char *name, const char *val)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%s=%s", name, val ? val : "");
    _putenv(buf);
}

/* Called first thing in WinMain, before anything reads the environment. */
void rs_startup(void)
{
    char line[160];
    FILE *f;
    int ini_fps = 0, ini_full = -1;
    int ini_cut = -1;                 /* cutscenes: 0 play + button skips, 1 play, 2 never show */
    int ini_sdump = 0, has_sdump = 0; /* sound dump: same rule as the texture dump */
    int ini_tdump = 0, has_tdump = 0; /* texture dump: the ini's choice (when it has one) beats RECOMP_TEX_DUMP */
    char ini_res[24] = "";
    int has_res = 0;
    char ini_lang[32] = "", ini_mlang[32] = "";
    const char *e;
    WIN32_FIND_DATAA fd;
    HANDLE hf;

    /* Started from Explorer or the launcher, stderr goes nowhere: keep the last log and write this run's to
     * play_log.txt (a .bat that already redirects stderr keeps doing so). */
    if (GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_UNKNOWN) {
        MoveFileExA("play_log.txt", "play_log_prev.txt", MOVEFILE_REPLACE_EXISTING);
        freopen("play_log.txt", "w", stderr);
    }

    hf = FindFirstFileA("languages\\*", &fd);          /* language packs: languages\<name>\DATA\... */
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.' && s_lang_n < RS_MAXLANG
                && strlen(fd.cFileName) < sizeof s_lang_list[0] && _stricmp(fd.cFileName, "english"))
                snprintf(s_lang_list[s_lang_n++], sizeof s_lang_list[0], "%s", fd.cFileName);
        } while (FindNextFileA(hf, &fd));
        FindClose(hf);
    }
    {
        const char *off = getenv("RECOMP_SETTINGS_MENU");
        if (off && *off == '0')
            s_enabled = 0;
    }
    f = fopen("recomp_settings.ini", "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "menu_lang=", 10)) {
            snprintf(ini_mlang, sizeof ini_mlang, "%s", line + 10);
        } else if (!strncmp(line, "language=", 9)) {
            snprintf(ini_lang, sizeof ini_lang, "%s", line + 9);
        } else if (!strncmp(line, "super_accuracy=", 15)) {
            s_has_super = atoi(line + 15) != 0;
        } else if (!strncmp(line, "blood=", 6)) {
            rs_putenv("RECOMP_BLOOD", line + 6);
        } else if (!strncmp(line, "blood_color=", 12)) {
            rs_putenv("RECOMP_BLOOD_COLOR", line + 12);
        } else if (!strncmp(line, "blood_hue=", 10)) {
            rs_putenv("RECOMP_BLOOD_HUE", line + 10);
        } else if (!strncmp(line, "blood_sat=", 10)) {
            rs_putenv("RECOMP_BLOOD_SAT", line + 10);
        } else if (!strncmp(line, "blood_bright=", 13)) {
            rs_putenv("RECOMP_BLOOD_BRIGHT", line + 13);
        } else if (!strncmp(line, "blood_time=", 11)) {
            rs_putenv("RECOMP_BLOOD_TIME", line + 11);
        } else if (!strncmp(line, "keep_corpses=", 13)) {
            rs_putenv("RECOMP_KEEP_CORPSES", atoi(line + 13) > 0 ? "24" : "0");
            if (atoi(line + 13) > 0)
                rs_putenv("RECOMP_KEEP_CORPSES_N", line + 13);
        } else if (!strncmp(line, "fps_counter=", 12)) {
            rs_putenv("RECOMP_FPS_COUNTER", line + 12);
        } else if (!strncmp(line, "screen_blood=", 13)) {
            rs_putenv("RECOMP_SCREEN_BLOOD", line + 13);
        } else if (!strncmp(line, "screen_dirt=", 12)) {
            rs_putenv("RECOMP_SCREEN_DIRT", line + 12);
        } else if (!strncmp(line, "lod=", 4)) {
            rs_putenv("RECOMP_LOD", line + 4);
        } else if (!strncmp(line, "stamina_bar=", 12)) {
            rs_putenv("RECOMP_STAMINA_BAR", line + 12);
        } else if (!strncmp(line, "enemy_count=", 12)) {
            rs_putenv("RECOMP_ENEMY_COUNT", line + 12);
        } else if (!strncmp(line, "enemy_count_recruit=", 20)) {
            rs_putenv("RECOMP_EC_RECRUIT", line + 20);
        } else if (!strncmp(line, "enemy_count_veteran=", 20)) {
            rs_putenv("RECOMP_EC_VETERAN", line + 20);
        } else if (!strncmp(line, "enemy_count_elite=", 18)) {
            rs_putenv("RECOMP_EC_ELITE", line + 18);
        } else if (!strncmp(line, "enemy_count_hero=", 17)) {
            rs_putenv("RECOMP_EC_HERO", line + 17);
        } else if (!strncmp(line, "grenade_timer=", 14)) {
            rs_putenv("RECOMP_GRENADE_TIMER", line + 14);
        } else if (!strncmp(line, "rain=", 5)) {
            rs_putenv("RECOMP_RAIN", line + 5);
        } else if (!strncmp(line, "screen_rain=", 12)) {
            rs_putenv("RECOMP_SCREEN_RAIN", line + 12);
        } else if (!strncmp(line, "rain_amount=", 12)) {
            rs_putenv("RECOMP_RAIN_AMOUNT", line + 12);
        } else if (!strncmp(line, "rain_size=", 10)) {
            rs_putenv("RECOMP_RAIN_SIZE", line + 10);
        } else if (!strncmp(line, "rain_speed=", 11)) {
            rs_putenv("RECOMP_RAIN_SPEED", line + 11);
        } else if (!strncmp(line, "texdump=", 8)) {
            ini_tdump = atoi(line + 8);
            if (ini_tdump < 0 || ini_tdump > 2)
                ini_tdump = 0;
            has_tdump = 1;
        } else if (!strncmp(line, "texmod=", 7)) {
            rs_putenv("RECOMP_TEXMOD", line + 7);
        } else if (!strncmp(line, "sounddump=", 10)) {
            ini_sdump = atoi(line + 10) != 0;
            has_sdump = 1;
        } else if (!strncmp(line, "cutscenes=", 10)) {
            ini_cut = !_stricmp(line + 10, "off") ? 2 : !_stricmp(line + 10, "play") ? 1 : 0;
        } else if (!strncmp(line, "fps=", 4)) {
            ini_fps = atoi(line + 4);
        } else if (!strncmp(line, "res=", 4)) {
            snprintf(ini_res, sizeof ini_res, "%s", line + 4);
            has_res = 1;
        } else if (!strncmp(line, "split_pads=", 11)) {
            rs_putenv("RECOMP_SPLIT_PADS", line + 11);
        } else if (!strncmp(line, "fullscreen=", 11)) {
            ini_full = atoi(line + 11) != 0;
        } else if (!strncmp(line, "fog=", 4)) {
            rs_putenv("RECOMP_FOG_MULT", line + 4);
        } else if (!strncmp(line, "aniso=", 6)) {
            int a = atoi(line + 6);
            g_gpu_aniso = a == 2 || a == 4 || a == 8 || a == 16 ? a : 1;
        } else if (!strncmp(line, "fxaa=", 5)) {
            g_gpu_fxaa = atoi(line + 5) != 0;
        } else if (!strncmp(line, "widescreen=", 11)) {          /* written by the launcher; the ini wins over the environment */
            rs_putenv("RECOMP_WIDESCREEN", atoi(line + 11) ? "1" : "0");
        } else if (!strncmp(line, "mkb=", 4)) {
            rs_putenv("RECOMP_MKB", atoi(line + 4) ? "1" : "0");
        } else if (!strncmp(line, "pin_game=", 9)) {
            rs_putenv("RECOMP_PIN_GAME", line + 9);
        } else if (!strncmp(line, "audio_host=", 11)) {
            rs_putenv("RECOMP_AUDIO_HOST", atoi(line + 11) ? "1" : "0");
        }
    }
    if (f)
        fclose(f);

    /* What the play_*.bat files set, as defaults for a plain double-click on the exe: used only where
     * neither the ini nor the environment has a value. */
    {
        static const char *const k_def[][2] = {
            { "RECOMP_FOV", "90" }, { "RECOMP_DRAW_MULT", "2" }, { "RECOMP_FOG_MULT", "2" },
            { "RECOMP_PIN_GAME", "2" }, { "RECOMP_SMALL_CAP", "2.5" }, { "RECOMP_SKIP_RT_WAIT", "1" },
            { "RECOMP_FLUSH_AHEAD", "2097152" }, { "RECOMP_PUSHBUF_KB", "2048" }, { "RECOMP_FOV_BLEND_MS", "160" },
            { "RECOMP_WIDESCREEN", "1" }, { "RECOMP_MKB", "1" }, { "RECOMP_NO_RECOIL", "1" },
            { "RECOMP_FULLSCREEN", "1" }, { "RECOMP_GPU_RES", "1920x1080" },
        };
        size_t di;
        for (di = 0; di < sizeof k_def / sizeof k_def[0]; di++)
            if (!getenv(k_def[di][0]))
                rs_putenv(k_def[di][0], k_def[di][1]);
    }

    if (s_has_super >= 0)
        rs_putenv("RECOMP_NO_RECOIL", s_has_super ? "1" : "0");
    if (ini_cut < 0) {                /* no ini choice: RECOMP_SKIP_MOVIES=1 still means "never show" */
        e = getenv("RECOMP_SKIP_MOVIES");
        ini_cut = e && *e == '1' ? 2 : 0;
    }
    game_cutscene_mode_set(ini_cut);
    if (has_tdump) {                  /* the ini decides, both ways (an old "only ON" rule left it on after the menu turned it off) */
        rs_putenv("RECOMP_TEX_DUMP", ini_tdump ? "tex_dump" : "");
        rs_putenv("RECOMP_TEX_DUMP_FMT", ini_tdump == 2 ? "dds" : "png");
    }
    if (has_sdump)
        rs_putenv("RECOMP_SND_DUMP", ini_sdump ? "sound_dump" : "");
    e = getenv("RECOMP_SND_DUMP");
    s_sdump = s_sdump_run = e && *e && *e != '0';
    e = getenv("RECOMP_TEX_DUMP");
    s_tdump = s_tdump_run = e && *e && *e != '0'
                          ? (getenv("RECOMP_TEX_DUMP_FMT") && !_stricmp(getenv("RECOMP_TEX_DUMP_FMT"), "dds") ? 2 : 1)
                          : 0;
    if (ini_fps <= 0 && !getenv("RECOMP_FPS") && !getenv("RECOMP_60FPS"))
        ini_fps = 60;                 /* nothing chosen anywhere: the steady 60 */
    if (ini_fps > 0) {
        if (ini_fps > 60) {
            char b[16];
            snprintf(b, sizeof b, "%d", ini_fps);
            rs_putenv("RECOMP_FPS", b);
            rs_putenv("RECOMP_60FPS", "");
        } else if (ini_fps == 60) {
            rs_putenv("RECOMP_FPS", "");
            rs_putenv("RECOMP_60FPS", "1");
        } else {
            rs_putenv("RECOMP_FPS", "");
            rs_putenv("RECOMP_60FPS", "");
        }
    }
    if (has_res)
        rs_putenv("RECOMP_GPU_RES", ini_res);
    if (ini_full >= 0)
        rs_putenv("RECOMP_FULLSCREEN", ini_full ? "1" : "0");
    if (ini_lang[0])                  /* the ini wins over a launcher's RECOMP_LANGUAGE */
        rs_putenv("RECOMP_LANGUAGE", !_stricmp(ini_lang, "english") ? "" : ini_lang);
    e = getenv("RECOMP_LANGUAGE");
    s_lang = 0;
    if (e && *e) {
        int li;
        for (li = 1; li < s_lang_n; li++)
            if (!_stricmp(s_lang_list[li], e))
                s_lang = li;
    }
    s_lang_run = s_lang;
    {   /* the menu's own language: the ini's choice, else the game's language when its pack has a menu file */
        int li, want = -1;
        char path[96];
        for (li = 1; li < s_lang_n && s_mlang_n < RS_MAXLANG; li++) {
            FILE *mf;
            snprintf(path, sizeof path, "languages\\%s\\recomp_menu.txt", s_lang_list[li]);
            mf = fopen(path, "rb");
            if (mf) {
                fclose(mf);
                snprintf(s_mlang_list[s_mlang_n++], sizeof s_mlang_list[0], "%s", s_lang_list[li]);
            }
        }
        if (ini_mlang[0]) {
            want = 0;
            for (li = 1; li < s_mlang_n; li++)
                if (!_stricmp(s_mlang_list[li], ini_mlang))
                    want = li;
        } else {
            want = 0;
            for (li = 1; li < s_mlang_n; li++)
                if (li && !_stricmp(s_mlang_list[li], s_lang_list[s_lang]))
                    want = li;
        }
        rs_load_menu(want);
    }

    /* what this run starts with, from the environment as it now stands */
    e = getenv("RECOMP_FPS");
    if (e && atoi(e) > 60)
        s_fps = rs_list_fps(atoi(e));
    else {
        const char *s6 = getenv("RECOMP_60FPS");
        s_fps = rs_list_fps(s6 && *s6 && *s6 != '0' ? 60 : 30);
    }
    s_res = rs_list_res(getenv("RECOMP_GPU_RES"));
    e = getenv("RECOMP_FULLSCREEN");
    s_full = e && *e && *e != '0';
    s_fps_run = s_fps;
    s_res_run = s_res;
    s_full_run = s_full;
    e = getenv("RECOMP_SPLIT_PADS");
    s_split = e && *e && *e != '0';
    s_split_run = s_split;
}

static int s_page;                   /* 0 the settings, 1 the controls */
static int s_csel, s_ctop, s_ccol;   /* controls page: row, first visible row, 0 keyboard / 1 gamepad */
static HWND s_hwnd;
static HWND s_catch;                 /* window mode: a near-invisible window under the panel that closes it on a click outside */

/* ---- the rows --------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *tag;
    void (*text)(char *out, size_t n);
    void (*adj)(int dir);
    int  (*pending)(void);
    int  (*enabled)(void);      /* NULL = always; 0 = the row is greyed out and does not change */
    const char *off;            /* the note shown instead of the tag while the row is greyed out */
    void (*swatch)(int rgb[3]); /* NULL = no colour square; else the square beside the value shows this colour */
} RsRow;

static void t_fov(char *o, size_t n)  { snprintf(o, n, "%d\xC2\xB0", (int)(game_fov_get() + 0.5f)); }
static void a_fov(int d)              { game_fov_set(game_fov_get() + 5.0f * (float)d); }
static void t_draw(char *o, size_t n) { snprintf(o, n, "x%.1f", game_draw_get()); }
static void a_draw(int d)             { game_draw_set(game_draw_get() + 0.5f * (float)d); }
static void t_lod(char *o, size_t n)  { snprintf(o, n, "%s", game_lod_name(game_lod_get())); }
static void a_lod(int d)              { game_lod_set(game_lod_get() + d); }
static void t_acc(char *o, size_t n)  { snprintf(o, n, "%s", game_super_accuracy_get() ? "On" : "Off"); }
static void a_acc(int d)              { (void)d; game_super_accuracy_set(!game_super_accuracy_get()); }
static void t_blood(char *o, size_t n) { snprintf(o, n, "%s", game_blood_name(game_blood_get())); }
static void a_blood(int d)            { game_blood_set((game_blood_get() + d + 4) % 4); }
static void t_bcol(char *o, size_t n) { snprintf(o, n, "%s", game_blood_color_name(game_blood_color_get())); }
static void a_bcol(int d)             { game_blood_color_set((game_blood_color_get() + d + 4) % 4); }
static int  blood_is_custom(void)     { return game_blood_color_get() == 3; }      /* 3 = Custom */
static void t_bhue(char *o, size_t n) { snprintf(o, n, "%d\xC2\xB0", game_blood_hue_get()); }
static void a_bhue(int d)             { game_blood_hue_set(game_blood_hue_get() + 10 * d); }
static void t_bsat(char *o, size_t n) { snprintf(o, n, "%d%%", game_blood_sat_get()); }
static void a_bsat(int d)             { game_blood_sat_set(game_blood_sat_get() + 5 * d); }
static void t_bbri(char *o, size_t n) { snprintf(o, n, "%d%%", game_blood_bright_get()); }
static void a_bbri(int d)             { game_blood_bright_set(game_blood_bright_get() + 5 * d); }
static void sw_blood(int rgb[3])      { game_blood_custom_rgb(rgb); }
static void t_btime(char *o, size_t n)
{
    float s = game_blood_time_seconds();
    if (s <= 0.0f)
        snprintf(o, n, "Auto");
    else
        snprintf(o, n, "%g s", (double)s);
}
static void a_btime(int d)            { game_blood_time_set((game_blood_time_get() + d + 9) % 9); }
static const int k_keep[7] = { 0, 6, 10, 20, 30, 50, 100 };
static void t_keep(char *o, size_t n)
{
    int c = game_keep_get();
    if (c <= 0)
        snprintf(o, n, "Off");
    else
        snprintf(o, n, "%d", c);
}
static void a_keep(int d)
{
    int c = game_keep_get(), i, best = 0;
    for (i = 0; i < 7; i++)
        if (k_keep[i] <= c)
            best = i;
    game_keep_set(k_keep[(best + d + 7) % 7]);
}
static void t_sblood(char *o, size_t n)
{
    static const char *const k_n[4] = { "Off", "Low", "Normal", "High" };
    snprintf(o, n, "%s", k_n[recomp_screenfx_get(0) & 3]);
}
static void a_sblood(int d)           { recomp_screenfx_set(0, (recomp_screenfx_get(0) + d + 4) % 4); }
static void t_sdirt(char *o, size_t n)
{
    static const char *const k_n[4] = { "Off", "Low", "Normal", "High" };
    snprintf(o, n, "%s", k_n[recomp_screenfx_get(1) & 3]);
}
static void a_sdirt(int d)            { recomp_screenfx_set(1, (recomp_screenfx_get(1) + d + 4) % 4); }
static void t_srain(char *o, size_t n)
{
    static const char *const k_n[4] = { "Off", "Low", "Normal", "High" };
    snprintf(o, n, "%s", k_n[recomp_screenfx_get(2) & 3]);
}
static void a_srain(int d)            { recomp_screenfx_set(2, (recomp_screenfx_get(2) + d + 4) % 4); }
static void t_ramount(char *o, size_t n) { snprintf(o, n, "%s", game_rain_cfg_name(0, game_rain_cfg_get(0))); }
static void a_ramount(int d)          { game_rain_cfg_set(0, (game_rain_cfg_get(0) + d + 4) % 4); }
static void t_rsize(char *o, size_t n)   { snprintf(o, n, "%s", game_rain_cfg_name(1, game_rain_cfg_get(1))); }
static void a_rsize(int d)            { game_rain_cfg_set(1, (game_rain_cfg_get(1) + d + 4) % 4); }
static void t_rspeed(char *o, size_t n)  { snprintf(o, n, "%s", game_rain_cfg_name(2, game_rain_cfg_get(2))); }
static void a_rspeed(int d)           { game_rain_cfg_set(2, (game_rain_cfg_get(2) + d + 4) % 4); }
static void t_stbar(char *o, size_t n) { snprintf(o, n, "%s", game_stambar_get() ? "Show" : "Hide"); }
static void a_stbar(int d)            { (void)d; game_stambar_set(!game_stambar_get()); }
static void t_grtm(char *o, size_t n) { snprintf(o, n, "%s", game_grentm_get() ? "Show" : "Hide"); }
static void a_grtm(int d)             { (void)d; game_grentm_set(!game_grentm_get()); }
static void t_rain(char *o, size_t n) { snprintf(o, n, "%s", game_rain_on_get() ? "On" : "Off"); }
static void a_rain(int d)             { (void)d; game_rain_on_set(!game_rain_on_get()); }
static int  rain_is_on(void)          { return game_rain_on_get(); }
static void t_ecnt(char *o, size_t n)  { if (game_ec_get() <= 1) snprintf(o, n, "Off"); else snprintf(o, n, "x%d", game_ec_get()); }
static void a_ecnt(int d)              { game_ec_set((game_ec_get() - 1 + d + 5) % 5 + 1); }
static void t_ecl0(char *o, size_t n)  { snprintf(o, n, "%s", game_eclvl_get(0) ? "On" : "Off"); }
static void a_ecl0(int d)              { (void)d; game_eclvl_set(0, !game_eclvl_get(0)); }
static void t_ecl1(char *o, size_t n)  { snprintf(o, n, "%s", game_eclvl_get(1) ? "On" : "Off"); }
static void a_ecl1(int d)              { (void)d; game_eclvl_set(1, !game_eclvl_get(1)); }
static void t_ecl2(char *o, size_t n)  { snprintf(o, n, "%s", game_eclvl_get(2) ? "On" : "Off"); }
static void a_ecl2(int d)              { (void)d; game_eclvl_set(2, !game_eclvl_get(2)); }
static void t_ecl3(char *o, size_t n)  { snprintf(o, n, "%s", game_eclvl_get(3) ? "On" : "Off"); }
static void a_ecl3(int d)              { (void)d; game_eclvl_set(3, !game_eclvl_get(3)); }
static int  ecnt_is_on(void)           { return game_ec_get() > 1; }
static void t_fpsc(char *o, size_t n) { snprintf(o, n, "%s", g_show_fps ? "On" : "Off"); }
static void a_fpsc(int d)             { (void)d; g_show_fps = !g_show_fps; }
static void t_sens(char *o, size_t n) { snprintf(o, n, "%.1f", mkb_get_sens()); }
static void a_sens(int d)
{
    double v = mkb_get_sens() + 0.1 * d;
    mkb_set_sens(v < 0.1 ? 0.1 : v > 5.0 ? 5.0 : (double)(int)(v * 10.0 + 0.5) / 10.0);
}
static void t_inv(char *o, size_t n)  { snprintf(o, n, "%s", mkb_get_invert_y() ? "On" : "Off"); }
static void a_inv(int d)              { (void)d; mkb_set_invert_y(!mkb_get_invert_y()); }
static void t_fps(char *o, size_t n)  { snprintf(o, n, "%d", s_fps_list[s_fps]); }
static void a_fps(int d)              { s_fps = (s_fps + d + s_fps_n) % s_fps_n; }
static int  p_fps(void)               { return s_fps != s_fps_run; }
static void t_res(char *o, size_t n)  { snprintf(o, n, "%s", s_res_list[s_res]); }
static void a_res(int d)              { s_res = (s_res + d + s_res_n) % s_res_n; }
static int  p_res(void)               { return s_res != s_res_run; }
static void t_full(char *o, size_t n) { snprintf(o, n, "%s", s_full ? "On" : "Off"); }
static void a_full(int d)             { (void)d; s_full = !s_full; }
static int  p_full(void)              { return s_full != s_full_run; }
static void t_split(char *o, size_t n) { snprintf(o, n, "%s", s_split ? "On" : "Off"); }
static void a_split(int d)            { (void)d; s_split = !s_split; }
static int  p_split(void)             { return s_split != s_split_run; }
static void t_sdump(char *o, size_t n){ snprintf(o, n, "%s", s_sdump ? "On" : "Off"); }
static void a_sdump(int d)            { (void)d; s_sdump = !s_sdump; }
static int  p_sdump(void)             { return s_sdump != s_sdump_run; }
static void t_tdump(char *o, size_t n){ snprintf(o, n, "%s", s_tdump == 2 ? "DDS" : s_tdump ? "PNG" : "Off"); }
static void a_tdump(int d)            { s_tdump = (s_tdump + d + 3) % 3; }
static void t_tmod(char *o, size_t n) { snprintf(o, n, "%s", texdec_texmod_get() ? "On" : "Off"); }
static void a_tmod(int d)             { (void)d; texdec_texmod_set(!texdec_texmod_get()); }
static int  p_tdump(void)             { return s_tdump != s_tdump_run; }
static void t_lang(char *o, size_t n) { snprintf(o, n, "%s", s_lang_list[s_lang]); if (o[0] >= 'a' && o[0] <= 'z') o[0] -= 32; }
static void a_lang(int d)             { s_lang = (s_lang + d + s_lang_n) % s_lang_n; }
static int  p_lang(void)              { return s_lang != s_lang_run; }
static int  p_any(void)               { return p_fps() || p_res() || p_full() || p_split() || p_tdump() || p_sdump() || p_lang(); }
static void t_apply(char *o, size_t n){ snprintf(o, n, "%s", p_any() ? "Press A / Enter" : "Nothing to apply"); }
static void a_apply(int d);

static void t_ctl(char *o, size_t n)  { snprintf(o, n, "Open..."); }
static void a_ctl(int d)              { (void)d; s_page = 1; s_csel = 0; s_ctop = 0; s_ccol = 0; }
static void t_fog(char *o, size_t n)  { snprintf(o, n, "x%.1f", nv2a_fog_mult_get()); }
static void a_fog(int d)              { nv2a_fog_mult_set(nv2a_fog_mult_get() + 0.5f * (float)d); }
static void t_aniso(char *o, size_t n){ if (g_gpu_aniso > 1) snprintf(o, n, "%dx", g_gpu_aniso); else snprintf(o, n, "Off"); }
static void a_aniso(int d)
{
    static const int lv[] = { 1, 2, 4, 8, 16 };
    int i, cur = 0;
    for (i = 0; i < 5; i++)
        if (lv[i] == g_gpu_aniso)
            cur = i;
    g_gpu_aniso = lv[(cur + d + 5) % 5];
}
static void t_fxaa(char *o, size_t n) { snprintf(o, n, "%s", g_gpu_fxaa ? "On" : "Off"); }
static void a_fxaa(int d)             { (void)d; g_gpu_fxaa = !g_gpu_fxaa; }
static void t_spd(char *o, size_t n)  { snprintf(o, n, "x%.1f", mkb_get_sprint_speed()); }
static void a_spd(int d)              { mkb_set_sprint_speed((float)(int)((mkb_get_sprint_speed() + 0.1f * (float)d) * 10.0f + 0.5f) / 10.0f); }
static void t_stam(char *o, size_t n) { snprintf(o, n, "%.0f s", mkb_get_sprint_time()); }
static void a_stam(int d)             { mkb_set_sprint_time(mkb_get_sprint_time() + (float)d); }
static void t_rec(char *o, size_t n)  { snprintf(o, n, "%.0f s", mkb_get_sprint_recover()); }
static void a_rec(int d)              { mkb_set_sprint_recover(mkb_get_sprint_recover() + (float)d); }

static void t_cut(char *o, size_t n)
{
    int m = game_cutscene_mode_get();
    snprintf(o, n, "%s", m == 2 ? "Never show" : m == 1 ? "Play, game's own skip" : "Play, button skips");
}
static void a_cut(int d)              { game_cutscene_mode_set((game_cutscene_mode_get() + d + 3) % 3); }

static void t_icm(char *o, size_t n)
{
    snprintf(o, n, "%s", mkb_get_icon_mode() == 1 ? "Gamepad" : mkb_get_icon_mode() == 2 ? "Keyboard" : "Auto (last used)");
}
static void a_icm(int d)              { mkb_set_icon_mode((mkb_get_icon_mode() + d + 3) % 3); }
static void t_ics(char *o, size_t n)  { snprintf(o, n, "%s", mkb_get_icon_style() ? "PlayStation" : "Xbox"); }
static void a_ics(int d)              { (void)d; mkb_set_icon_style(!mkb_get_icon_style()); }

static void t_mlang(char *o, size_t n);
static void a_mlang(int d);

static const RsRow k_rows[] = {
    { "Menu language",       "live",             t_mlang, a_mlang, NULL },
    { "Field of view",       "live",             t_fov,  a_fov,  NULL   },
    { "Draw distance",       "live",             t_draw, a_draw, NULL   },
    { "Model detail",        "live; Higher and Highest draw more polygons at a distance, so they can cost frames", t_lod, a_lod, NULL },
    { "Fog distance",        "live",             t_fog,  a_fog,  NULL   },
    { "Anisotropic filtering", "live",           t_aniso, a_aniso, NULL },
    { "FXAA anti-aliasing",  "live",             t_fxaa, a_fxaa, NULL   },
    { "Super Accuracy",      "applies in a mission", t_acc, a_acc, NULL },
    { "Enemy count (heavy)", "experimental; applies at the next mission load; x2 to x5 times the enemies that spawn points create (max x5). Heavy: costs frames, keep it low on a slower PC", t_ecnt, a_ecnt, NULL },
    { "Enemy count: Recruit", "extra enemies on this difficulty", t_ecl0, a_ecl0, NULL, ecnt_is_on, "set Enemy count first" },
    { "Enemy count: Veteran", "extra enemies on this difficulty", t_ecl1, a_ecl1, NULL, ecnt_is_on, "set Enemy count first" },
    { "Enemy count: Elite",   "extra enemies on this difficulty", t_ecl2, a_ecl2, NULL, ecnt_is_on, "set Enemy count first" },
    { "Enemy count: Hero",    "extra enemies on this difficulty", t_ecl3, a_ecl3, NULL, ecnt_is_on, "set Enemy count first" },
    { "Blood amount",        "applies in a mission", t_blood, a_blood, NULL },
    { "Blood colour",        "applies in a mission", t_bcol, a_bcol, NULL },
    { "Blood hue",           "applies in a mission", t_bhue, a_bhue, NULL, blood_is_custom, "choose Custom first", sw_blood },
    { "Blood saturation",    "applies in a mission", t_bsat, a_bsat, NULL, blood_is_custom, "choose Custom first", sw_blood },
    { "Blood brightness",    "applies in a mission", t_bbri, a_bbri, NULL, blood_is_custom, "choose Custom first", sw_blood },
    { "Blood duration",      "seconds the spray stays; Auto = by amount", t_btime, a_btime, NULL },
    { "Keep corpses (experimental)", "bodies that stay on the ground", t_keep, a_keep, NULL },
    { "Screen blood",        "splatters on your view when an enemy is hit close", t_sblood, a_sblood, NULL },
    { "Screen dirt",         "dust and dirt on your view when an explosion is near", t_sdirt, a_sdirt, NULL },
    { "Rain",                "the game's own rain; Off = none, and the rows below are greyed out", t_rain, a_rain, NULL, NULL },
    { "Screen rain",         "raindrops on your view while you are out in the rain", t_srain, a_srain, NULL, rain_is_on, "turn Rain on first" },
    { "Rain amount",         "how much rain falls in the game (a rise applies at the next level load)", t_ramount, a_ramount, game_rain_cfg_pending, rain_is_on, "turn Rain on first" },
    { "Rain size",           "size of the rain particles", t_rsize, a_rsize, NULL, rain_is_on, "turn Rain on first" },
    { "Rain speed",          "how fast the rain falls", t_rspeed, a_rspeed, NULL, rain_is_on, "turn Rain on first" },
    { "FPS counter",         "live",             t_fpsc, a_fpsc, NULL   },
    { "Mouse sensitivity",   "live",             t_sens, a_sens, NULL   },
    { "Invert mouse Y",      "live",             t_inv,  a_inv,  NULL   },
    { "Cutscenes",           "Enter / Space / Esc / A / Start skips", t_cut, a_cut, NULL },
    { "Dump textures",        "restart; PNG or DDS; to mod textures; costs fps while on", t_tdump, a_tdump, p_tdump },
    { "Replaced textures",    "live: On shows your pictures, Off the game's own", t_tmod, a_tmod, NULL },
    { "Dump sounds (WAV)",     "restart; saves sound effects to sound_dump", t_sdump, a_sdump, p_sdump },
    { "Controls",            "keyboard, mouse and gamepad", t_ctl, a_ctl, NULL },
    { "Button prompts",      "which device the icons follow", t_icm, a_icm, NULL },
    { "Gamepad icons",       "Xbox or PlayStation art", t_ics, a_ics, NULL },
    { "Sprint speed",        "live",             t_spd,  a_spd,  NULL   },
    { "Grenade timer",      "live; a countdown on screen from the moment a grenade is armed until it goes off", t_grtm, a_grtm, NULL },
    { "Stamina bar",         "live; the yellow sprint bar above the adrenaline meter", t_stbar, a_stbar, NULL },
    { "Sprint duration",     "seconds before tiring", t_stam, a_stam, NULL },
    { "Rest to recover",     "seconds to get it all back", t_rec, a_rec, NULL },
    { "Language",            "restart; packs in the languages folder", t_lang, a_lang, p_lang },
    { "Frame rate",          "restart",          t_fps,  a_fps,  p_fps  },
    { "Resolution",          "restart",          t_res,  a_res,  p_res  },
    { "Fullscreen",          "restart",          t_full, a_full, p_full },
    { "Split-screen pads",   "restart; keyboard and mouse = player 1, each gamepad = the next player", t_split, a_split, p_split },
    { "Apply and restart",   "restarts the game", t_apply, a_apply, p_any },
};
#define RS_ROWS ((int)(sizeof k_rows / sizeof k_rows[0]))

/* ---- the window ----------------------------------------------------------- */

static volatile LONG s_open;
static volatile LONG s_inject_b;     /* opened over the Credits screen: press B for the game on close */
static volatile LONG s_ctx_pause;    /* the last screen the game loaded belongs to the pause menu */
static volatile LONG s_quit;
static volatile DWORD s_closed_tick;
static int s_win_mode;               /* F1: a window in the middle of the game; the pause (Y) and Credits entries cover it all */
#define RS_VIS 10                    /* rows on screen at once */
static int   s_sel, s_top;
static RECT  s_row_rc[RS_ROWS];
static RECT  s_back_rc;

/* Start the game again with the saved settings and end this one: a hidden cmd waits a few seconds
 * (the sound, the window and the log file are released), then runs the same exe in the same folder,
 * its messages appended to play_log.txt. The environment is inherited; the ini wins at start-up. */
static void rs_restart_game(void)
{
    WCHAR exe[MAX_PATH], cmd[MAX_PATH * 2 + 96];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf_s(cmd, sizeof cmd / sizeof cmd[0],
               L"cmd.exe /c \"ping -n 4 127.0.0.1 >nul & \"%s\" 2>>play_log.txt\"", exe);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    fprintf(stderr, "[SETTINGS] restarting the game\n");
    fflush(stderr);
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);   /* ExitProcess could wait on a thread holding the log lock */
    }
}

static void a_apply(int d)
{
    (void)d;
    if (p_any())
        rs_restart_game();
}

static void rs_change(int dir)
{
    if (k_rows[s_sel].enabled && !k_rows[s_sel].enabled())
        return;                         /* greyed out (Rain is Off): nothing to change */
    k_rows[s_sel].adj(dir);
    rs_save();
    InvalidateRect(s_hwnd, NULL, FALSE);
}

static void rs_select(int d)
{
    s_sel = (s_sel + d + RS_ROWS) % RS_ROWS;
    if (s_sel < s_top)
        s_top = s_sel;
    if (s_sel >= s_top + RS_VIS)
        s_top = s_sel - RS_VIS + 1;
    InvalidateRect(s_hwnd, NULL, FALSE);
}

static HFONT rs_font(int h, int weight)
{
    return CreateFontA(-h, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
}

/* ---- the menu's own language ----------------------------------------------------
 * languages\<pack>\recomp_menu.txt (UTF-8) holds "English text|translation" lines for every string this
 * screen draws, and a "#rtl" line for right-to-left languages. With such a pack chosen (the top row, live)
 * the screen is drawn mirrored: text right aligned, rows and columns swapped, Arabic shaped by the system. */
#define RS_MAXTR 512
static struct { char *en, *tr; } s_tr[RS_MAXTR];
static int  s_tr_n;

static void rs_load_menu(int idx)
{
    char path[96], line[512];
    FILE *f;
    int i;
    for (i = 0; i < s_tr_n; i++) {
        free(s_tr[i].en);
        free(s_tr[i].tr);
    }
    s_tr_n = 0;
    s_rtl = 0;
    s_mlang = idx;
    if (idx <= 0 || idx >= s_mlang_n)
        return;
    snprintf(path, sizeof path, "languages\\%s\\recomp_menu.txt", s_mlang_list[idx]);
    f = fopen(path, "rb");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *bar;
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "\xEF\xBB\xBF", 3))
            memmove(line, line + 3, strlen(line + 2));
        if (!strcmp(line, "#rtl")) {
            s_rtl = 1;
            continue;
        }
        bar = strchr(line, '|');
        if (line[0] == '#' || !bar || s_tr_n >= RS_MAXTR)
            continue;
        *bar = 0;
        s_tr[s_tr_n].en = _strdup(line);
        s_tr[s_tr_n].tr = _strdup(bar + 1);
        s_tr_n++;
    }
    fclose(f);
}

static void rs_save(void);

static void t_mlang(char *o, size_t n)
{
    snprintf(o, n, "%s", s_mlang_list[s_mlang]);
    if (o[0] >= 'a' && o[0] <= 'z')
        o[0] -= 32;
}

static void a_mlang(int d)
{
    rs_load_menu((s_mlang + d + s_mlang_n) % s_mlang_n);
    rs_save();
}

static const char *rs_tr(const char *s)
{
    int i;
    if (s_tr_n)
        for (i = 0; i < s_tr_n; i++)
            if (!strcmp(s_tr[i].en, s))
                return s_tr[i].tr;
    return s;
}

static void rs_mirror(RECT *r)
{
    int l = r->left;
    r->left = s_curW - r->right;
    r->right = s_curW - l;
}

static void rs_fill(HDC dc, const RECT *rc, HBRUSH br)
{
    RECT r = *rc;
    if (s_rtl && !s_nomirror)
        rs_mirror(&r);
    FillRect(dc, &r, br);
}

static int rs_has_high(const char *t)
{
    for (; *t; t++)
        if ((unsigned char)*t >= 0x80)
            return 1;
    return 0;
}

static void rs_text(HDC dc, const char *s, RECT *rc, UINT fmt, COLORREF c)
{
    WCHAR w[512];
    RECT r = *rc;
    UINT al = fmt & (DT_CENTER | DT_RIGHT);
    const char *t = rs_tr(s);
    SetTextColor(dc, c);
    if (s_rtl && !s_nomirror) {
        rs_mirror(&r);
        if (al == 0)
            fmt |= DT_RIGHT;
        else if (al == DT_RIGHT)
            fmt &= ~DT_RIGHT;
    }
    if (!MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 512))
        w[0] = 0;
    DrawTextW(dc, w, -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX | (s_rtl && rs_has_high(t) ? DT_RTLREADING : 0));
}

/* ---- the Controls page ------------------------------------------------------ */

#define RS_MOUSE1 0x100u
#define RS_WHEEL  0x105u
#define CT_MOVE   4
#define CT_ROWS   (CT_MOVE + MKB_ACT_COUNT + 1)   /* movement, functions, reset */

static int   s_cap;                  /* 0 none, 1 waiting for a key / mouse button, 2 for a pad button */
static int   s_cap_add;              /* the new input is added to the function's others */
static DWORD s_cap_tick;
static RECT  s_crow_rc[CT_ROWS], s_ckey_rc[CT_ROWS], s_cpad_rc[CT_ROWS];

static const char *const k_move_label[4] = { "Move forward", "Move backward", "Strafe left", "Strafe right" };

static const char *ct_label(int r)
{
    if (r < CT_MOVE)
        return k_move_label[r];
    return r < CT_MOVE + MKB_ACT_COUNT ? mkb_action_label(r - CT_MOVE) : "Reset all controls to the defaults";
}

static void ct_key_text(int r, char *o, size_t n)
{
    unsigned short ks[4];
    char one[48];
    int c, i;
    o[0] = 0;
    if (r == CT_ROWS - 1)
        return;
    if (r < CT_MOVE) {
        char one[48];
        mkb_key_label(mkb_move_key(r), one, sizeof one);
        snprintf(o, n, "%s", rs_tr(one));
        return;
    }
    c = mkb_action_keys(r - CT_MOVE, ks);
    if (!c) {
        snprintf(o, n, "-");
        return;
    }
    for (i = 0; i < c; i++) {
        mkb_key_label(ks[i], one, sizeof one);
        if (i)
            strncat_s(o, n, " / ", _TRUNCATE);
        strncat_s(o, n, rs_tr(one), _TRUNCATE);
    }
}

static void ct_pad_text(int r, char *o, size_t n)
{
    signed char pb[2];
    int i;
    o[0] = 0;
    if (r == CT_ROWS - 1)
        return;
    if (r < CT_MOVE) {
        snprintf(o, n, "Left stick");
        return;
    }
    if (!mkb_pad_buttons(r - CT_MOVE, pb)) {
        snprintf(o, n, "-");
        return;
    }
    for (i = 0; i < 2; i++)
        if (pb[i] >= 0) {
            if (o[0])
                strncat_s(o, n, " / ", _TRUNCATE);
            strncat_s(o, n, rs_tr(mkb_pad_label(pb[i])), _TRUNCATE);
        }
}

static void ct_select(int d)
{
    s_csel = (s_csel + d + CT_ROWS) % CT_ROWS;
    if (s_csel < s_ctop)
        s_ctop = s_csel;
    if (s_csel >= s_ctop + RS_VIS)
        s_ctop = s_csel - RS_VIS + 1;
    InvalidateRect(s_hwnd, NULL, FALSE);
}

static void ct_begin(int add)
{
    if (s_csel == CT_ROWS - 1) {
        mkb_bindings_reset();
        rs_save();
    } else if (s_csel < CT_MOVE && (s_ccol == 1 || add)) {
        return;                                  /* the stick is fixed; one key per direction */
    } else {
        s_cap = s_ccol == 0 ? 1 : 2;
        s_cap_add = add;
        s_cap_tick = GetTickCount();
    }
    InvalidateRect(s_hwnd, NULL, FALSE);
}

static void ct_assign_key(unsigned code)
{
    int r = s_csel, n, i;
    unsigned short ks[4];
    if (code == VK_F12 || code == VK_F1 || code == VK_F11 || code == VK_F9) {     /* screenshot / Recomp Settings / fullscreen / textures keys */
        s_cap = 0;
        InvalidateRect(s_hwnd, NULL, FALSE);
        return;
    }
    if (r < CT_MOVE) {
        if (code < 0x100)
            mkb_set_move_key(r, (unsigned short)code);
    } else {
        n = s_cap_add ? mkb_action_keys(r - CT_MOVE, ks) : 0;
        for (i = 0; i < n; i++)
            if (ks[i] == code)
                break;
        if (i == n && n < 4)
            ks[n++] = (unsigned short)code;
        mkb_action_set_keys(r - CT_MOVE, ks, n);
    }
    s_cap = 0;
    rs_save();
    InvalidateRect(s_hwnd, NULL, FALSE);
}

static void ct_assign_pad(int pb)
{
    signed char cur[2];
    int a = s_csel - CT_MOVE;
    if (a >= 0 && a < MKB_ACT_COUNT) {
        mkb_pad_buttons(a, cur);
        if (s_cap_add && cur[0] >= 0 && cur[0] != pb) {
            cur[1] = (signed char)pb;
        } else {
            cur[0] = (signed char)pb;
            cur[1] = -1;
        }
        mkb_action_set_pad(a, cur);
        rs_save();
    }
    s_cap = 0;
    InvalidateRect(s_hwnd, NULL, FALSE);
}

static void ct_clear(void)
{
    int a = s_csel - CT_MOVE;
    signed char none[2] = { -1, -1 };
    if (s_csel < CT_MOVE) {
        if (s_ccol == 0)
            mkb_set_move_key(s_csel, 0);
    } else if (a < MKB_ACT_COUNT) {
        if (s_ccol == 0)
            mkb_action_set_keys(a, NULL, 0);
        else
            mkb_action_set_pad(a, none);
    }
    s_cap = 0;
    rs_save();
    InvalidateRect(s_hwnd, NULL, FALSE);
}

/* The one navigation the keyboard, the mouse and the pad share: 1 up, 2 down, 3 left, 4 right. */
static void rs_nav(int dir)
{
    if (s_cap)
        return;
    if (s_rtl && dir >= 3)                      /* mirrored screen: the physical left is the "next" side */
        dir = 7 - dir;
    if (s_page == 0) {
        if (dir == 1) rs_select(-1);
        else if (dir == 2) rs_select(1);
        else rs_change(dir == 3 ? -1 : 1);
    } else if (dir <= 2) {
        ct_select(dir == 1 ? -1 : 1);
    } else {
        s_ccol = dir == 3 ? 0 : 1;
        InvalidateRect(s_hwnd, NULL, FALSE);
    }
}

static void rs_activate(void)
{
    if (s_cap)
        return;
    if (s_page == 0)
        rs_change(1);
    else
        ct_begin(0);
}

static void rs_back(void)
{
    if (s_cap) {
        s_cap = 0;
        InvalidateRect(s_hwnd, NULL, FALSE);
    } else if (s_page == 1) {
        s_page = 0;
        InvalidateRect(s_hwnd, NULL, FALSE);
    } else {
        PostMessage(s_hwnd, WM_CLOSE, 0, 0);
    }
}

/* The slider at the side of the list: drag its handle (or click the track) to scroll through the rows with the
 * mouse alone. Coordinates are the unmirrored ones; rs_fill mirrors them for a right-to-left language. */
static RECT s_trk_rc, s_trk_hit;     /* the track, and a wider area that takes the mouse */
static int  s_trk_total, s_trk_vis, s_trk_on, s_drag;

static void rs_scrollbar(HDC mem, int x, int top, int hgt, int total, int first)
{
    RECT tr = { x, top, x + 10, top + hgt }, th;
    HBRUSH br;
    int th_h, span;
    s_trk_on = total > RS_VIS;
    if (!s_trk_on) {
        memset(&s_trk_rc, 0, sizeof s_trk_rc);
        memset(&s_trk_hit, 0, sizeof s_trk_hit);
        return;
    }
    s_trk_rc = tr;
    s_trk_hit = tr;
    s_trk_hit.left -= 14; s_trk_hit.right += 14;
    s_trk_total = total;
    s_trk_vis = RS_VIS;
    br = CreateSolidBrush(RGB(44, 40, 38));
    rs_fill(mem, &tr, br);
    DeleteObject(br);
    th_h = hgt * RS_VIS / total;
    if (th_h < 36)
        th_h = 36;
    span = hgt - th_h;
    th.left = tr.left; th.right = tr.right;
    th.top = top + (total > RS_VIS ? span * first / (total - RS_VIS) : 0);
    th.bottom = th.top + th_h;
    br = CreateSolidBrush(s_drag ? RGB(235, 90, 70) : RGB(200, 40, 34));
    rs_fill(mem, &th, br);
    DeleteObject(br);
}

/* the handle follows the mouse: y (client) -> first visible row, the selection stays inside the window */
static void rs_drag_to(int y)
{
    int hgt = s_trk_rc.bottom - s_trk_rc.top, th_h = hgt * s_trk_vis / s_trk_total, span, first, *top, *sel, total;
    if (th_h < 36)
        th_h = 36;
    span = hgt - th_h;
    if (!s_trk_on || span <= 0)
        return;
    first = (int)(((double)(y - s_trk_rc.top - th_h / 2) / span) * (s_trk_total - s_trk_vis) + 0.5);
    total = s_trk_total;
    top = s_page == 1 ? &s_ctop : &s_top;
    sel = s_page == 1 ? &s_csel : &s_sel;
    if (first < 0) first = 0;
    if (first > total - s_trk_vis) first = total - s_trk_vis;
    *top = first;
    if (*sel < first) *sel = first;
    if (*sel >= first + s_trk_vis) *sel = first + s_trk_vis - 1;
    InvalidateRect(s_hwnd, NULL, FALSE);
}

static void rs_paint_controls(HDC mem, int W, int H, HFONT fbig, HFONT fnorm, HFONT fsmall)
{
    RECT bar = { 0, H / 20, W, H / 20 + H / 9 }, tx, hd;
    HBRUSH br;
    int i, rowh, y0, xk, xp, xr = W - W / 12, xl = W / 12;
    char buf[160];

    br = CreateSolidBrush(RGB(139, 22, 20));
    rs_fill(mem, &bar, br);
    DeleteObject(br);
    tx.left = xl; tx.right = W; tx.top = bar.top; tx.bottom = bar.bottom;
    SelectObject(mem, fbig);
    rs_text(mem, "CONTROLS", &tx, DT_LEFT | DT_VCENTER, RGB(240, 232, 215));

    xk = xl + (xr - xl) * 36 / 100;
    xp = xl + (xr - xl) * 69 / 100;
    y0 = H * 33 / 100;
    rowh = H * 56 / 100 / RS_VIS;
    SelectObject(mem, fsmall);
    hd.top = y0 - rowh * 7 / 10; hd.bottom = y0; hd.left = xk; hd.right = xp - 10;
    rs_text(mem, "KEYBOARD AND MOUSE", &hd, DT_LEFT | DT_VCENTER, RGB(200, 120, 90));
    hd.left = xp; hd.right = xr;
    rs_text(mem, "GAMEPAD", &hd, DT_LEFT | DT_VCENTER, RGB(200, 120, 90));

    memset(s_crow_rc, 0, sizeof s_crow_rc);
    memset(s_ckey_rc, 0, sizeof s_ckey_rc);
    memset(s_cpad_rc, 0, sizeof s_cpad_rc);
    for (i = s_ctop; i < CT_ROWS && i < s_ctop + RS_VIS; i++) {
        RECT row = { xl, y0 + (i - s_ctop) * rowh, xr, y0 + (i - s_ctop + 1) * rowh - 3 };
        RECT lab = row, kc = row, pc = row;
        int sel = i == s_csel;
        s_crow_rc[i] = row;
        lab.left += 22; lab.right = xk - 8;
        kc.left = xk; kc.right = xp - 12;
        pc.left = xp; pc.right = xr;
        s_ckey_rc[i] = kc;
        s_cpad_rc[i] = pc;
        if (sel) {
            RECT stripe = { row.left, row.top, row.left + 6, row.bottom };
            br = CreateSolidBrush(RGB(60, 20, 18));
            rs_fill(mem, &row, br);
            DeleteObject(br);
            br = CreateSolidBrush(RGB(200, 40, 34));
            rs_fill(mem, &stripe, br);
            DeleteObject(br);
            {   /* the chosen cell */
                RECT cell = s_ccol == 0 ? kc : pc;
                RECT fr = cell;
                br = CreateSolidBrush(RGB(110, 36, 30));
                rs_fill(mem, &fr, br);
                DeleteObject(br);
            }
        }
        SelectObject(mem, fnorm);
        rs_text(mem, ct_label(i), &lab, DT_LEFT | DT_VCENTER, sel ? RGB(255, 245, 225) : RGB(205, 200, 188));
        if (i == CT_ROWS - 1)
            continue;
        ct_key_text(i, buf, sizeof buf);
        if (sel && s_cap == 1 && s_ccol == 0) {
            snprintf(buf, sizeof buf, "Press a key or mouse button...");
            rs_text(mem, buf, &kc, DT_LEFT | DT_VCENTER, RGB(235, 180, 70));
        } else {
            kc.left += 10;
            rs_text(mem, buf, &kc, DT_LEFT | DT_VCENTER, sel && s_ccol == 0 ? RGB(255, 245, 225) : RGB(215, 210, 196));
        }
        ct_pad_text(i, buf, sizeof buf);
        if (sel && s_cap == 2 && s_ccol == 1) {
            snprintf(buf, sizeof buf, "Press a gamepad button...");
            rs_text(mem, buf, &pc, DT_LEFT | DT_VCENTER, RGB(235, 180, 70));
        } else {
            pc.left += 10;
            rs_text(mem, buf, &pc, DT_LEFT | DT_VCENTER, sel && s_ccol == 1 ? RGB(255, 245, 225) : RGB(215, 210, 196));
        }
    }
    rs_scrollbar(mem, xr + 12, y0, RS_VIS * rowh - 3, CT_ROWS, s_ctop);
    {
        RECT ft = { xl, H * 92 / 100, xr, H };
        SelectObject(mem, fsmall);
        s_back_rc.left = xr - W / 7; s_back_rc.right = xr;
        s_back_rc.top = ft.top - 6; s_back_rc.bottom = H * 98 / 100;
        rs_text(mem, s_cap ? "Esc: cancel      Delete: clear"
                           : "Enter / click: change    Insert: add another    Delete: clear    Left / Right: keyboard or gamepad",
                &ft, DT_LEFT | DT_TOP, RGB(150, 146, 134));
        rs_text(mem, "[ Back ]", &s_back_rc, DT_RIGHT | DT_TOP, RGB(225, 100, 80));
        if (s_ctop > 0) {
            RECT up = { xl, y0 - rowh * 7 / 10, xr, y0 };
            rs_text(mem, "^ more above", &up, DT_RIGHT | DT_VCENTER, RGB(150, 146, 134));
        }
        if (s_ctop + RS_VIS < CT_ROWS) {
            RECT dn = { xl, y0 + RS_VIS * rowh, xr, y0 + RS_VIS * rowh + rowh * 2 / 3 };
            rs_text(mem, "v more below", &dn, DT_RIGHT | DT_VCENTER, RGB(150, 146, 134));
        }
    }
}

static void rs_paint(HWND hw)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hw, &ps), mem;
    HBITMAP bmp, old;
    RECT cl;
    int W, H, i, rowh, y0, fh;
    HFONT fbig, fnorm, fsmall, fo;
    HBRUSH br;
    char buf[64];

    GetClientRect(hw, &cl);
    W = cl.right;
    H = cl.bottom;
    s_curW = W;
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, W, H);
    old = (HBITMAP)SelectObject(mem, bmp);
    SetBkMode(mem, TRANSPARENT);

    br = CreateSolidBrush(RGB(16, 16, 15));
    rs_fill(mem, &cl, br);
    DeleteObject(br);

    fh = H / 26;
    fbig = rs_font(H / 13, FW_BOLD);
    fnorm = rs_font(fh, FW_SEMIBOLD);
    fsmall = rs_font(fh * 2 / 3, FW_NORMAL);
    fo = (HFONT)SelectObject(mem, fnorm);

    if (s_page == 1) {
        rs_paint_controls(mem, W, H, fbig, fnorm, fsmall);
        goto paint_done;
    }
    {   /* the red title band, like the game's menus */
        RECT bar = { 0, H / 20, W, H / 20 + H / 9 };
        RECT tx;
        br = CreateSolidBrush(RGB(139, 22, 20));
        rs_fill(mem, &bar, br);
        DeleteObject(br);
        tx.left = W / 12; tx.right = W; tx.top = bar.top; tx.bottom = bar.bottom;
        SelectObject(mem, fbig);
        rs_text(mem, "RECOMP SETTINGS", &tx, DT_LEFT | DT_VCENTER, RGB(240, 232, 215));
    }

    y0 = H * 28 / 100;
    rowh = H * 62 / 100 / RS_VIS;
    memset(s_row_rc, 0, sizeof s_row_rc);
    for (i = s_top; i < RS_ROWS && i < s_top + RS_VIS; i++) {
        RECT row = { W / 12, y0 + (i - s_top) * rowh, W - W / 12, y0 + (i - s_top + 1) * rowh - 4 };
        RECT lab, val, tag;
        int pend = k_rows[i].pending && k_rows[i].pending();
        int off = k_rows[i].enabled && !k_rows[i].enabled();     /* greyed out: shown, not changeable */
        s_row_rc[i] = row;
        if (i == s_sel) {
            RECT stripe = { row.left, row.top, row.left + 6, row.bottom };
            br = CreateSolidBrush(RGB(70, 22, 20));
            rs_fill(mem, &row, br);
            DeleteObject(br);
            br = CreateSolidBrush(RGB(200, 40, 34));
            rs_fill(mem, &stripe, br);
            DeleteObject(br);
        }
        lab = row; lab.left += 24; lab.right = row.left + (row.right - row.left) * 40 / 100;
        val = row; val.left = row.left + (row.right - row.left) * 40 / 100; val.right = row.left + (row.right - row.left) * 72 / 100;
        tag = row; tag.left = val.right + 12;
        if (k_rows[i].swatch) {       /* the colour square beside the value; dimmed while the row is greyed out */
            int sw_rgb[3], sz = (row.bottom - row.top) * 55 / 100;
            RECT sq, edge;
            k_rows[i].swatch(sw_rgb);
            sq.left = val.right + 12; sq.right = sq.left + sz;
            sq.top = row.top + (row.bottom - row.top - sz) / 2; sq.bottom = sq.top + sz;
            edge = sq;
            InflateRect(&edge, 2, 2);                                  /* a light edge keeps dark colours visible */
            br = CreateSolidBrush(RGB(150, 146, 136));
            rs_fill(mem, &edge, br);
            DeleteObject(br);
            br = CreateSolidBrush(off ? RGB(sw_rgb[0] / 2, sw_rgb[1] / 2, sw_rgb[2] / 2) : RGB(sw_rgb[0], sw_rgb[1], sw_rgb[2]));
            rs_fill(mem, &sq, br);
            DeleteObject(br);
            tag.left = edge.right + 10;
        }
        SelectObject(mem, fnorm);
        rs_text(mem, k_rows[i].name, &lab, DT_LEFT | DT_VCENTER,
                off ? RGB(105, 103, 96) : i == s_sel ? RGB(255, 245, 225) : RGB(205, 200, 188));
        k_rows[i].text(buf, sizeof buf);
        rs_text(mem, buf, &val, DT_CENTER | DT_VCENTER,
                off ? RGB(105, 103, 96) : i == s_sel ? RGB(255, 245, 225) : RGB(225, 220, 205));
        if (i == s_sel && !off) {
            RECT l = val, r = val;
            rs_text(mem, s_rtl ? ">" : "<", &l, DT_LEFT | DT_VCENTER, RGB(230, 90, 70));   /* mirrored: the left cell is drawn on the right */
            rs_text(mem, s_rtl ? "<" : ">", &r, DT_RIGHT | DT_VCENTER, RGB(230, 90, 70));
        }
        SelectObject(mem, fsmall);
        rs_text(mem, off && k_rows[i].off ? k_rows[i].off : pend && k_rows[i].adj != a_apply ? (k_rows[i].adj == a_ramount ? "next level load" : "restart to apply") : k_rows[i].tag, &tag, DT_LEFT | DT_VCENTER,
                pend && !off ? RGB(230, 175, 60) : RGB(120, 118, 108));
    }

    rs_scrollbar(mem, W - W / 12 + 12, y0, RS_VIS * rowh - 4, RS_ROWS, s_top);
    {   /* footer */
        RECT ft = { W / 12, H * 92 / 100, W - W / 12, H };
        SelectObject(mem, fsmall);
        s_back_rc.left = W - W / 12 - W / 7; s_back_rc.right = W - W / 12;
        s_back_rc.top = ft.top - 6; s_back_rc.bottom = H * 98 / 100;
        rs_text(mem, "Up / Down  select      Left / Right  change      B / Esc  back", &ft,
                DT_LEFT | DT_TOP, RGB(150, 146, 134));
        rs_text(mem, "[ Back ]", &s_back_rc, DT_RIGHT | DT_TOP, RGB(225, 100, 80));
        if (s_top > 0) {
            RECT up = { W / 12, y0 - rowh * 2 / 3, W - W / 12, y0 };
            rs_text(mem, "^ more above", &up, DT_RIGHT | DT_VCENTER, RGB(150, 146, 134));
        }
        if (s_top + RS_VIS < RS_ROWS) {
            RECT dn = { W / 12, y0 + RS_VIS * rowh - 2, W - W / 12, y0 + RS_VIS * rowh + rowh * 2 / 3 };
            rs_text(mem, "v more below", &dn, DT_RIGHT | DT_VCENTER, RGB(150, 146, 134));
        }
    }

paint_done:
    SelectObject(mem, fo);
    DeleteObject(fbig);
    DeleteObject(fnorm);
    DeleteObject(fsmall);
    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hw, &ps);
}

/* the pad, read here (the game's copy is held at rest while this is up) */
typedef struct { WORD wButtons; BYTE lt, rt; SHORT lx, ly, rx, ry; } RsPad;
typedef struct { DWORD packet; RsPad pad; } RsXState;
typedef DWORD (WINAPI *RsXGet)(DWORD, RsXState *);

/* the physical buttons as bits 0..15 in the order the bindings count them */
static unsigned rs_pad_mask(const RsXState *st)
{
    static const struct { WORD bit; int pb; } m[] = {
        { 0x1000, 0 }, { 0x2000, 1 }, { 0x4000, 2 }, { 0x8000, 3 }, { 0x0100, 4 }, { 0x0200, 5 },
        { 0x0040, 8 }, { 0x0080, 9 }, { 0x0010, 10 }, { 0x0020, 11 },
        { 0x0001, 12 }, { 0x0002, 13 }, { 0x0004, 14 }, { 0x0008, 15 },
    };
    unsigned r = 0;
    int i;
    for (i = 0; i < (int)(sizeof m / sizeof m[0]); i++)
        if (st->pad.wButtons & m[i].bit)
            r |= 1u << m[i].pb;
    if (st->pad.lt > 100) r |= 1u << 6;
    if (st->pad.rt > 100) r |= 1u << 7;
    return r;
}

static void rs_pad_tick(void)
{
    static RsXGet get;
    static int tried;
    static DWORD held_dir, held_since, last_rep;
    static unsigned prev_mask;
    RsXState st;
    DWORD dir = 0, now = GetTickCount();
    unsigned mask, rising;
    WORD b;

    if (!tried) {
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        tried = 1;
        if (m)
            get = (RsXGet)GetProcAddress(m, "XInputGetState");
    }
    if (!get || get(0, &st) != ERROR_SUCCESS)
        return;
    b = st.pad.wButtons;
    mask = rs_pad_mask(&st);
    rising = mask & ~prev_mask;
    prev_mask = mask;
    if (s_cap == 2) {                                   /* waiting for a button */
        int pb;
        if (now - s_cap_tick > 10000u) {
            s_cap = 0;
            InvalidateRect(s_hwnd, NULL, FALSE);
        } else if (now - s_cap_tick > 250u && rising) {
            for (pb = 0; pb < 16; pb++)
                if (rising >> pb & 1)
                    break;
            ct_assign_pad(pb);
        }
        return;
    }
    if ((b & 0x0001) || st.pad.ly > 16000) dir = 1;        /* up */
    else if ((b & 0x0002) || st.pad.ly < -16000) dir = 2;  /* down */
    else if ((b & 0x0004) || st.pad.lx < -16000) dir = 3;  /* left */
    else if ((b & 0x0008) || st.pad.lx > 16000) dir = 4;   /* right */
    if (dir && (dir != held_dir || (now - held_since > 380 && now - last_rep > 90))) {
        if (dir != held_dir)
            held_since = now;
        held_dir = dir;
        last_rep = now;
        rs_nav((int)dir);
    } else if (!dir) {
        held_dir = 0;
    }
    if (rising & 1u)                                        /* A */
        rs_activate();
    if (rising & 2u)                                        /* B */
        rs_back();
}

/* a window message's key as the binding code (the left / right Shift, Ctrl, Alt apart) */
static unsigned rs_vk_code(WPARAM wp, LPARAM lp)
{
    UINT sc = (UINT)((lp >> 16) & 0xFF);
    int ext = (int)((lp >> 24) & 1);
    if (wp == VK_SHIFT)
        return MapVirtualKeyA(sc, MAPVK_VSC_TO_VK_EX);
    if (wp == VK_CONTROL)
        return ext ? VK_RCONTROL : VK_LCONTROL;
    if (wp == VK_MENU)
        return ext ? VK_RMENU : VK_LMENU;
    return (unsigned)wp;
}

static void rs_hover_cell(POINT p)
{
    int i;
    if (s_page == 0) {
        for (i = 0; i < RS_ROWS; i++)
            if (PtInRect(&s_row_rc[i], p) && s_sel != i) {
                s_sel = i;
                InvalidateRect(s_hwnd, NULL, FALSE);
            }
    } else if (!s_cap) {
        for (i = 0; i < CT_ROWS; i++)
            if (PtInRect(&s_crow_rc[i], p)) {
                int col = p.x >= s_cpad_rc[i].left ? 1 : 0;
                if (s_csel != i || s_ccol != col) {
                    s_csel = i;
                    s_ccol = col;
                    InvalidateRect(s_hwnd, NULL, FALSE);
                }
            }
    }
}

static void rs_target(int gx, int gy, int gw, int gh, RECT *r)
{
    int ww = s_win_mode ? gw * 62 / 100 : gw, wh = s_win_mode ? gh * 78 / 100 : gh;

    r->left = gx + (gw - ww) / 2;
    r->top = gy + (gh - wh) / 2;
    r->right = r->left + ww;
    r->bottom = r->top + wh;
}

/* The overlay is a separate top-most window, so it has to follow the game's: hidden while the game is minimised
 * or another program is in front (Alt+Tab, F9 then another window ...), shown again when the game is back, and
 * moved/resized with the game's client area (F11 window <-> fullscreen, dragging the window).
 * Returns 1 while it is shown. */
static int rs_follow(HWND hw)
{
    static HWND game;
    DWORD pid = 0;
    HWND fg;
    RECT cl, cur, want;
    POINT tl = { 0, 0 };
    int show, vis = IsWindowVisible(hw);

    if (!game || !IsWindow(game))
        game = FindWindowA("XboxRecompFramebuffer", NULL);
    if (!game)
        return 1;
    fg = GetForegroundWindow();
    if (fg)
        GetWindowThreadProcessId(fg, &pid);
    show = IsWindowVisible(game) && !IsIconic(game) && pid == GetCurrentProcessId();
    if (!show) {
        if (vis)
            ShowWindow(hw, SW_HIDE);
        if (s_catch)
            ShowWindow(s_catch, SW_HIDE);
        return 0;
    }
    GetClientRect(game, &cl);
    ClientToScreen(game, &tl);
    rs_target(tl.x, tl.y, cl.right, cl.bottom, &want);
    GetWindowRect(hw, &cur);
    if (cur.left != want.left || cur.top != want.top || cur.right != want.right || cur.bottom != want.bottom) {
        SetWindowPos(hw, HWND_TOPMOST, want.left, want.top, want.right - want.left, want.bottom - want.top, SWP_NOACTIVATE);
        InvalidateRect(hw, NULL, FALSE);
    }
    if (s_catch) {
        GetWindowRect(s_catch, &cur);
        if (cur.left != tl.x || cur.top != tl.y || cur.right - cur.left != cl.right || cur.bottom - cur.top != cl.bottom)
            SetWindowPos(s_catch, hw, tl.x, tl.y, cl.right, cl.bottom, SWP_NOACTIVATE);
    }
    if (!vis) {
        if (s_catch)
            ShowWindow(s_catch, SW_SHOWNA);
        ShowWindow(hw, SW_SHOWNA);
        SetForegroundWindow(hw);
        SetFocus(hw);
    }
    return 1;
}

static LRESULT CALLBACK rs_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        rs_paint(hw);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (rs_follow(hw))
            rs_pad_tick();
        return 0;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN: {
        unsigned code = rs_vk_code(wp, lp);
        if (s_cap == 1) {
            if (lp & (1 << 30))                         /* the repeat of the key that started it */
                return 0;
            if (code == VK_ESCAPE)
                rs_back();
            else if (code == VK_DELETE)
                ct_clear();
            else
                ct_assign_key(code);
            return 0;
        }
        if (msg == WM_SYSKEYDOWN)
            break;
        switch (wp) {
        case VK_UP: case 'W': rs_nav(1); break;
        case VK_DOWN: case 'S': rs_nav(2); break;
        case VK_LEFT: case 'A': rs_nav(3); break;
        case VK_RIGHT: case 'D': rs_nav(4); break;
        case VK_RETURN: case VK_SPACE: rs_activate(); break;
        case VK_ESCAPE: case VK_BACK: rs_back(); break;
        case VK_INSERT: if (s_page == 1 && !s_cap) ct_begin(1); break;
        case VK_DELETE: if (s_page == 1 && !s_cap) ct_clear(); break;
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = { s_rtl ? s_curW - 1 - (short)LOWORD(lp) : (short)LOWORD(lp), (short)HIWORD(lp) };
        if (s_drag) {
            rs_drag_to(p.y);
            return 0;
        }
        rs_hover_cell(p);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (s_drag) {
            s_drag = 0;
            if (msg == WM_LBUTTONUP)
                ReleaseCapture();
            InvalidateRect(hw, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN: {
        POINT p = { s_rtl ? s_curW - 1 - (short)LOWORD(lp) : (short)LOWORD(lp), (short)HIWORD(lp) };
        int i, btn = msg == WM_LBUTTONDOWN ? 0 : msg == WM_RBUTTONDOWN ? 1 : msg == WM_MBUTTONDOWN ? 2
                   : HIWORD(wp) == XBUTTON1 ? 3 : 4;
        if (s_cap == 1) {
            ct_assign_key(RS_MOUSE1 + (unsigned)btn);
            return msg == WM_XBUTTONDOWN ? TRUE : 0;
        }
        if (msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN)
            return msg == WM_XBUTTONDOWN ? TRUE : 0;
        if (msg == WM_LBUTTONDOWN && !s_cap && s_trk_on && PtInRect(&s_trk_hit, p)) {
            s_drag = 1;
            SetCapture(hw);
            rs_drag_to(p.y);
            return 0;
        }
        if (PtInRect(&s_back_rc, p) || (msg == WM_RBUTTONDOWN)) {
            rs_back();
            return 0;
        }
        if (s_page == 0) {
            for (i = 0; i < RS_ROWS; i++)
                if (PtInRect(&s_row_rc[i], p)) {
                    int mid = s_row_rc[i].left + (s_row_rc[i].right - s_row_rc[i].left) * 56 / 100;
                    s_sel = i;
                    rs_change(p.x < mid ? -1 : 1);
                }
        } else {
            for (i = 0; i < CT_ROWS; i++)
                if (PtInRect(&s_crow_rc[i], p)) {
                    s_csel = i;
                    s_ccol = p.x >= s_cpad_rc[i].left ? 1 : 0;
                    ct_begin(0);
                }
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (s_cap == 1)
            ct_assign_key(RS_WHEEL);
        else if (s_page == 0)
            rs_change((short)HIWORD(wp) > 0 ? 1 : -1);
        else
            ct_select((short)HIWORD(wp) > 0 ? -1 : 1);
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return TRUE;
    case WM_CLOSE:
        InterlockedExchange(&s_quit, 1);
        DestroyWindow(hw);
        return 0;
    case WM_DESTROY:
        KillTimer(hw, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

static LRESULT CALLBACK rs_catch_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    if ((msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN) && s_hwnd)
        PostMessage(s_hwnd, WM_CLOSE, 0, 0);
    return DefWindowProcA(hw, msg, wp, lp);
}

static DWORD WINAPI rs_thread(LPVOID unused)
{
    static int registered;
    HWND game = FindWindowA("XboxRecompFramebuffer", NULL);
    RECT rc = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    MSG m;
    (void)unused;

    if (game) {
        POINT tl = { 0, 0 };
        RECT cl;
        GetClientRect(game, &cl);
        ClientToScreen(game, &tl);
        rc.left = tl.x; rc.top = tl.y; rc.right = tl.x + cl.right; rc.bottom = tl.y + cl.bottom;
    }
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = rs_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "RecompSettingsOverlay";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassA(&wc);
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = rs_catch_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "RecompSettingsCatcher";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassA(&wc);
        registered = 1;
    }
    s_sel = 0;
    s_top = 0;
    s_page = 0;
    s_cap = 0;
    {
        RECT want;
        rs_target(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, &want);
        if (s_win_mode) {
            s_catch = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
                                      "RecompSettingsCatcher", "", WS_POPUP | WS_VISIBLE, rc.left, rc.top,
                                      rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, GetModuleHandle(NULL), NULL);
            if (s_catch)
                SetLayeredWindowAttributes(s_catch, 0, 1, LWA_ALPHA);
        }
        s_hwnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "RecompSettingsOverlay", "Recomp Settings",
                                 WS_POPUP | WS_VISIBLE, want.left, want.top, want.right - want.left, want.bottom - want.top,
                                 NULL, NULL, GetModuleHandle(NULL), NULL);
    }
    if (s_hwnd) {
        SetForegroundWindow(s_hwnd);
        SetFocus(s_hwnd);
        SetTimer(s_hwnd, 1, 16, NULL);
        while (GetMessageA(&m, NULL, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
    }
    if (s_catch) {
        DestroyWindow(s_catch);
        s_catch = NULL;
    }
    s_hwnd = NULL;
    g_input_block = 0;
    mkb_release_all();
    if (s_inject_b)
        g_input_inject_b_until = GetTickCount() + 400;  /* the screen behind (Credits) backs out */
    if (game)
        SetForegroundWindow(game);
    s_closed_tick = GetTickCount();
    InterlockedExchange(&s_quit, 0);
    InterlockedExchange(&s_open, 0);
    fprintf(stderr, "[SETTINGS] closed\n");
    return 0;
}

/* ---- the entries ---------------------------------------------------------------
 * 1. the Credits item of Options (front end); 2. F1, or the pad's Y button on a pause screen,
 * while any menu is up (hotkey thread below), so a player in a mission need not quit to the main menu. */

static void rs_open(int over_credits, int window, const char *why)
{
    /* The Credits hook can fire again while its screen reloads, so only that one waits 3 s after a close;
     * F1 and Y reopen at once (a key held through the close is not a new press, see the hotkey thread). */
    if (s_open || (over_credits && GetTickCount() - s_closed_tick <= 3000))
        return;
    InterlockedExchange(&s_open, 1);
    s_inject_b = over_credits;
    s_win_mode = window;
    g_input_block = 1;
    fprintf(stderr, "[SETTINGS] opened (%s)\n", why);
    CloseHandle(CreateThread(NULL, 0, rs_thread, NULL, 0, NULL));
}

static void rs_open_hook(const char *path)
{
    static char last[48];
    char low[160], name[48];
    const char *p;
    size_t i;

    for (i = 0; path[i] && i < sizeof low - 1; i++)
        low[i] = (char)((path[i] >= 'A' && path[i] <= 'Z') ? path[i] + 32 : path[i] == '/' ? '\\' : path[i]);
    low[i] = 0;
    p = strstr(low, "scrns\\");
    if (!p)
        return;
    p += 6;
    for (i = 0; p[i] && p[i] != '.' && i < sizeof name - 1; i++)
        name[i] = p[i];
    name[i] = 0;
    /* The Credits item loads the producers' plaque (BGplaque) and then credits; after a return from
     * the overlay Options may not be loaded again at all. So credits opens the overlay unless the
     * end-of-game flow (gameComplete) asked for it. */
    if (!strcmp(name, "credits")) {
        if (strcmp(last, "gamecomplete") && strcmp(last, "contestpart2"))
            rs_open(1, 0, "Credits item of Options");
        return;
    }
    if (!strncmp(name, "pause", 5))
        s_ctx_pause = 1;
    else if (strcmp(name, "bgplaque") && strcmp(name, "helpbar") && strcmp(name, "dialoguewindow")
             && strcmp(name, "mcfx"))
        s_ctx_pause = 0;
    if (strcmp(name, "bgplaque"))
        snprintf(last, sizeof last, "%s", name);
}

/* ---- the label: "Credits" -> "Recomp Settings" -------------------------------
 *
 * The button's text is the key $SP_Credits, looked up in DATA\1\1_99\strings.viv: a small container
 * around a UTF-16 XML table (<localstring name="SP_Credits"><english value="Credits" />...). The game's
 * file is never changed: when the game asks for it, a relabelled copy is made once in
 * xboxdata\patched (next to the game) and opened instead (g_xbox_path_rewrite). The copy has the same
 * size: the 8 extra characters are paid for with the indentation of the following language lines. */
extern void (*g_xbox_path_rewrite)(const WCHAR *rel, WCHAR *host, DWORD host_size);
int game_save_root_w(WCHAR *out, DWORD n);

static size_t rs_wfind(const WCHAR *h, size_t hn, const WCHAR *n)
{
    size_t nl = wcslen(n), i;
    if (nl > hn)
        return (size_t)-1;
    for (i = 0; i + nl <= hn; i++)
        if (h[i] == n[0] && !memcmp(h + i, n, nl * sizeof(WCHAR)))
            return i;
    return (size_t)-1;
}

typedef struct { const WCHAR *key, *old_en, *new_en; } RsPatch;

static const RsPatch k_patch_fe[] = {
    { L"SP_Credits", L"Credits", L"Recomp Settings" },                     /* Options: the Credits item */
};
static const struct { const WCHAR *tail; const char *name; const RsPatch *p; int n; } k_files[] = {
    { L"DATA\\1\\1_99\\strings.viv", "fe",     k_patch_fe,     1 },
};
#define RS_NFILES ((int)(sizeof k_files / sizeof k_files[0]))
static WCHAR s_cached[1][MAX_PATH];

/* In one string table (UTF-16 XML after a byte order mark): the english text of one entry becomes
 * new_en, without changing the file's size -- a longer text takes the indentation of the entry's
 * following language lines, a shorter one is padded with spaces after the element. */
static int rs_patch_entry(unsigned char *buf, size_t size, const RsPatch *pt)
{
    WCHAR k_open[96], k_old[160], k_new[160];
    size_t bom, n, gs, ge, ge2, e, rel, i, o, removed, ol, nl;
    WCHAR *t, *tmp;

    swprintf_s(k_open, 96, L"name=\"%s\">", pt->key);
    swprintf_s(k_old, 160, L"<english value=\"%s\" />", pt->old_en);
    swprintf_s(k_new, 160, L"<english value=\"%s\" />", pt->new_en);
    ol = wcslen(k_old);
    nl = wcslen(k_new);
    for (bom = 0; bom + 1 < size && !(buf[bom] == 0xFF && buf[bom + 1] == 0xFE); bom++)
        ;
    if (bom + 2 >= size || (bom + 2) % 2)
        return 0;
    t = (WCHAR *)(buf + bom + 2);
    n = (size - bom - 2) / 2;
    rel = rs_wfind(t, n, k_open);
    if (rel == (size_t)-1)
        return 0;
    gs = rel + wcslen(k_open);
    rel = rs_wfind(t + gs, n - gs, L"</localstring>");
    ge = rel == (size_t)-1 ? n : gs + rel;
    rel = rs_wfind(t + gs, n - gs, L"</globalstring>");
    ge2 = rel == (size_t)-1 ? n : gs + rel;
    if (ge2 < ge)
        ge = ge2;
    if (ge >= n)
        return 0;
    rel = rs_wfind(t + gs, ge - gs, k_old);
    if (rel == (size_t)-1)
        return 0;
    e = gs + rel;
    tmp = (WCHAR *)malloc((ge - gs + nl + 8) * sizeof(WCHAR));
    if (!tmp)
        return 0;
    o = 0;
    for (i = gs; i < e; i++)
        tmp[o++] = t[i];
    for (i = 0; i < nl; i++)
        tmp[o++] = k_new[i];
    removed = 0;
    for (i = e + ol; i < ge; i++) {
        if (nl > ol && removed < nl - ol && t[i] == L' ' && (t[i - 1] == L'\n' || t[i - 1] == L' ')) {
            removed++;                          /* indentation of the next language lines */
            continue;
        }
        tmp[o++] = t[i];
    }
    if (nl > ol && removed != nl - ol) {
        free(tmp);
        return 0;
    }
    while (o < ge - gs)                         /* a shorter text: pad */
        tmp[o++] = L' ';
    if (o != ge - gs) {
        free(tmp);
        return 0;
    }
    memcpy(t + gs, tmp, o * sizeof(WCHAR));
    free(tmp);
    return 1;
}

static void rs_path_rewrite(const WCHAR *rel, WCHAR *host, DWORD host_size)
{
    static volatile LONG busy;
    size_t rl = wcslen(rel), tl;
    int f, j;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    WCHAR dir[MAX_PATH], out[MAX_PATH], base[MAX_PATH];
    HANDLE h;
    unsigned char *buf;
    DWORD got = 0, wrote = 0;
    ULARGE_INTEGER sz;

    if (!s_enabled)
        return;
    for (f = 0; f < RS_NFILES; f++) {
        tl = wcslen(k_files[f].tail);
        if (rl >= tl && !_wcsicmp(rel + rl - tl, k_files[f].tail))
            break;
    }
    if (f == RS_NFILES)
        return;
    {
        const char *off = getenv("RECOMP_RELABEL");
        if (off && *off == '0')
            return;
    }
    while (InterlockedCompareExchange(&busy, 1, 0))
        Sleep(1);
    if (s_cached[f][0]) {
        wcscpy_s(host, host_size, s_cached[f]);
        InterlockedExchange(&busy, 0);
        return;
    }
    if (!GetFileAttributesExW(host, GetFileExInfoStandard, &fa))
        goto done;
    sz.LowPart = fa.nFileSizeLow;
    sz.HighPart = fa.nFileSizeHigh;
    if (!game_save_root_w(base, MAX_PATH))
        goto done;
    swprintf_s(dir, MAX_PATH, L"%s\\patched", base);
    CreateDirectoryW(dir, NULL);
    swprintf_s(out, MAX_PATH, L"%s\\strings_%S_%lu_%08lx_v2.viv", dir, k_files[f].name, (unsigned long)sz.LowPart,
               (unsigned long)fa.ftLastWriteTime.dwLowDateTime);
    if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) {
        int okn = 0;
        h = CreateFileW(host, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            goto done;
        buf = (unsigned char *)malloc(sz.LowPart);
        if (!buf || !ReadFile(h, buf, sz.LowPart, &got, NULL) || got != sz.LowPart) {
            CloseHandle(h);
            free(buf);
            goto done;
        }
        CloseHandle(h);
        for (j = 0; j < k_files[f].n; j++) {
            int ok = rs_patch_entry(buf, got, &k_files[f].p[j]);
            okn += ok;
            fprintf(stderr, "[SETTINGS] relabel %S in %s: %s\n", k_files[f].p[j].key, k_files[f].name,
                    ok ? "done" : "text not found");
        }
        if (okn) {
            WCHAR tmpf[MAX_PATH];
            swprintf_s(tmpf, MAX_PATH, L"%s.tmp", out);
            h = CreateFileW(tmpf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                int ok = WriteFile(h, buf, got, &wrote, NULL) && wrote == got;
                CloseHandle(h);
                if (ok)
                    MoveFileExW(tmpf, out, MOVEFILE_REPLACE_EXISTING);
            }
        }
        free(buf);
    }
    if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) {
        wcscpy_s(s_cached[f], MAX_PATH, out);
        wcscpy_s(host, host_size, s_cached[f]);
    }
done:
    InterlockedExchange(&busy, 0);
}

extern ptrdiff_t g_xbox_mem_offset;

static int rs_game_in_front(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* F1 in any screen, in play too (the game keeps running under the panel, its controls are held); the pad's
 * Y on a pause screen (it is free there). */
static DWORD WINAPI rs_hotkey_thread(LPVOID unused)
{
    HMODULE xm = LoadLibraryA("xinput1_4.dll");
    RsXGet get = xm ? (RsXGet)GetProcAddress(xm, "XInputGetState") : NULL;
    int prev_y = 0, prev_f1 = 0, prev_f9 = 0;
    (void)unused;
    for (;;) {
        int f1, f9, y = 0, menu;
        Sleep(30);
        if (s_open || !rs_game_in_front()) {
            prev_y = prev_f1 = prev_f9 = 1; /* a key held while this was up is not a press */
            continue;
        }
        menu = g_xbox_mem_offset && *(const volatile unsigned char *)((uintptr_t)g_xbox_mem_offset + 0x378964u) == 1;
        f1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        if (get && s_ctx_pause) {
            RsXState st;
            if (get(0, &st) == ERROR_SUCCESS)
                y = (st.pad.wButtons & 0x8000) != 0;
        }
        if ((f1 && !prev_f1) || (menu && y && !prev_y))
            rs_open(0, f1, f1 ? (menu ? "F1 in a menu" : "F1 in play") : "Y on a pause screen");
        f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;                     /* replaced textures On/Off, same as the settings row */
        if (f9 && !prev_f9) {
            texdec_texmod_set(!texdec_texmod_get());
            recomp_osd_show(texdec_texmod_get() ? "Replaced textures: On  (F9)" : "Replaced textures: Off  (F9)");
            rs_save();
        }
        prev_f9 = f9;
        prev_f1 = f1;
        prev_y = y;
    }
}

void rs_install(void)
{
    if (s_enabled) {
        g_xbox_open_hook = rs_open_hook;
        g_xbox_path_rewrite = rs_path_rewrite;
        CloseHandle(CreateThread(NULL, 0, rs_hotkey_thread, NULL, 0, NULL));
    }
}
