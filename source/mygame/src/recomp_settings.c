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
int         mkb_gren_macro_get(void);
void        mkb_gren_macro_set(int on);
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
int    game_expsize_get(void);            /* recomp_manual.c: explosion size 0..4 (Normal, x1.5, x2, x3, x4) */
void   game_expsize_set(int l);
const char *game_expsize_name(int l);
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
int    game_corpse60_get(void);           /* game_patches.c: Havok at a 60 Hz step (the flying corpses fix; may freeze the game) */
void   game_corpse60_set(int on);
extern volatile int g_show_fps;
int    recomp_screenfx_get(int kind);
int    game_stain_get(void);                 /* blood_stain.c: blood stains on walls and floor, 0 Off .. 3 High */
void   game_stain_set(int l);
unsigned game_stain_nodes(void);            /* blood_stain.c: decal node pool size (stain_nodes=, default 1500) */
void   game_stain_nodes_set(unsigned n);
int    game_stain_nodes_pending(void);
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
int  hudhide_get(int i);                   /* hud_hide.c: one switch per HUD element (Show / Hide) */
void hudhide_set(int i, int v);
void hudhide_ini_write(FILE *f);
int  hudhide_ini_line(const char *line);
void hudhide_rewrite(const WCHAR *rel, WCHAR *host, DWORD host_size);
int  game_friends_get(void);               /* ai_tuning.c: Squad mates 0 Normal, 1 Tough, 2 Weak */
void game_friends_set(int m);
int  game_dmgmode_get(void);               /* ai_tuning.c: Damage mode 0 Normal, 1 Low damage */
void game_dmgmode_set(int m);
int  game_hitmark_get(void);               /* ai_tuning.c: the X at the centre when the player's shot hits (white) or kills (red) */
void game_hitmark_set(int on);
int  game_unlock_get(void);                /* ai_tuning.c: Unlock all missions (mission select shows every mission as completed) */
void game_unlock_set(int on);
int  game_cheat_ammo_get(void);            /* game_patches.c: the game's own Unlimited Ammo cheat (byte 0x37279C) */
void game_cheat_ammo_set(int on);
int  game_cheat_god_get(void);             /* game_patches.c: the game's own Invincible cheat (byte 0x37279E) */
void game_cheat_god_set(int on);
void game_cheat_rally_request(void);       /* game_patches.c: the game's own Full adrenaline meter (Rally Bar Filled) cheat, same way */
void game_cheat_nemesis_request(void);     /* game_patches.c: the game's own Kill nemesis cheat, same way */
int  game_cheat_noshock_get(void);         /* game_patches.c: Disable shellshock (byte 0x51D6C4) */
void game_cheat_noshock_set(int on);
int  game_cheat_nohud_get(void);           /* game_patches.c: Disable HUD (byte 0x3785A5) */
void game_cheat_nohud_set(int on);
int  game_autoswitch_get(void);           /* game_patches.c: switch to the other weapon when the magazine runs dry */
void game_autoswitch_set(int on);
int  game_egren_get(void);                /* game_patches.c: enemy grenades 0 off, 1 low, 2 medium, 3 high */
void game_egren_set(int m);
int  game_pain_get(void);                  /* game_sfx.c: pain / scream sounds of soldiers and the player (files in mods\sounds\pain) */
void game_pain_set(int on);
int  game_breath_get(void);                /* game_sfx.c: breathing while sprinting and when worn out (files in mods\sounds\breath) */
void game_breath_set(int on);
int  game_breathvol_get(void);             /* 10..100 */
void game_breathvol_set(int v);
int  game_painvol_get(void);               /* 10..100 */
void game_painvol_set(int v);
int  apu_stereo_width_get(void);           /* apu_dsp.c: 0 (mono) .. 100 (the game's own mix) */
void apu_stereo_width_set(int pct);
int  game_sway_get(void);                  /* game_sfx.c: the aim sways in a scope while the player's health is low */
void game_sway_set(int on);
int  game_swaystr_get(void);               /* 1..5 */
void game_swaystr_set(int v);
int  game_weaponlight_get(void);            /* recomp_manual.c: first-person weapon lighting, 0 Off .. 4 Extreme */
void game_weaponlight_set(int v);
int  game_gunsway_get(void);                /* recomp_manual.c: the weapon lags behind the camera turn (spring) */
void game_gunsway_set(int on);
int  game_gunswaystr_get(void);             /* 1..5 */
void game_gunswaystr_set(int v);
int  game_freecam_get(void);                /* recomp_manual.c: the free camera (Insert) */
void game_freecam_set(int on);
int  game_freecamworld_get(void);           /* 0 = the world stands still while it flies, 1 = it keeps running */
void game_freecamworld_set(int v);
int  game_freecamspeed_get(void);           /* 1..5 */
void game_freecamspeed_set(int v);
int  game_editor_get(void);                 /* map_editor.c (inside game_patches.c): the in-game map editor keys work */
void game_editor_set(int on);
int  texdec_crosshair_hide_get(void);     /* nv2a_pb_exec.c: hide the game's own aiming crosshair */
void texdec_crosshair_hide_set(int on);
int  game_eclvl_get(int lvl);              /* per difficulty on/off: 0 Recruit, 1 Veteran, 2 Elite, 3 Hero */
void game_eclvl_set(int lvl, int v);
int  game_grentm_get(void);
void game_grentm_set(int on);
int  game_rain_on_get(void);
void game_rain_on_set(int on);
int    game_lod_get(void);
void   game_lod_set(int idx);
int    game_fire_get(void);
void   game_fire_set(int idx);
const char *game_fire_name(int idx);
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
/* Aspect ratio: 0 = 4:3 (widescreen off), 1 = 16:9 (the game's own widescreen), 2 = 21:9, 3 = auto (the window's shape) */
static const char *const k_asp_names[] = { "4:3", "16:9", "21:9", "Auto" };
static int  s_asp = 1, s_asp_run = 1;
static char s_asp_env[24] = "";                            /* a custom RECOMP_ASPECT value (2.37 ...) kept as it was while the row is untouched */
static int  s_has_super = -1;                             /* the ini's Super Accuracy, -1 = not there */
static int  s_sdump, s_sdump_run;                         /* sound dump (RECOMP_SND_DUMP), chosen / as this run started */
static int  s_tdump, s_tdump_run;                        /* texture dump: 0 off, 1 PNG, 2 DDS; chosen / as this run started */
static int  s_enabled = 1;
#define RS_MAXLANG 8
static char s_lang_list[RS_MAXLANG][32] = { "english" };  /* "english" + every folder of languages\ */
static int  s_lang_n = 1;
static int  s_lang, s_lang_run;                           /* chosen / as this run started */
static int  s_rtl, s_curW, s_nomirror;
/* F1 window: where the player left it (moved by the red bar, resized by the edges), as 1/10000 of the game picture:
 * x, y, width, height. 0 = never touched: the middle of the game, 62% x 78%. ini menu_rect=x,y,w,h. */
static int  s_win_user, s_win_fx, s_win_fy, s_win_fw, s_win_fh;
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
    fprintf(f, "grenade_macro=%d\n", mkb_gren_macro_get());
    fprintf(f, "super_accuracy=%d\n", game_super_accuracy_get());
    fprintf(f, "blood=%d\n", game_blood_get());
    fprintf(f, "explosion_size=%d\n", game_expsize_get());
    fprintf(f, "blood_color=%d\n", game_blood_color_get());
    fprintf(f, "blood_hue=%d\n", game_blood_hue_get());
    fprintf(f, "blood_sat=%d\n", game_blood_sat_get());
    fprintf(f, "blood_bright=%d\n", game_blood_bright_get());
    fprintf(f, "blood_time=%d\n", game_blood_time_get());
    fprintf(f, "keep_corpses=%d\n", game_keep_get());
    fprintf(f, "corpse_60hz=%d\n", game_corpse60_get());
    fprintf(f, "fps_counter=%d\n", g_show_fps ? 1 : 0);
    fprintf(f, "screen_blood=%d\n", recomp_screenfx_get(0));
    fprintf(f, "screen_dirt=%d\n", recomp_screenfx_get(1));
    fprintf(f, "blood_stains=%d\n", game_stain_get());
    fprintf(f, "stain_nodes=%u\n", (unsigned)game_stain_nodes());
    fprintf(f, "lod=%d\n", game_lod_get());
    fprintf(f, "firecap=%d\n", game_fire_get());
    fprintf(f, "stamina_bar=%d\n", game_stambar_get());
    fprintf(f, "grenade_timer=%d\n", game_grentm_get());
    fprintf(f, "damage_mode=%d\n", game_dmgmode_get());
    fprintf(f, "hit_marker=%d\n", game_hitmark_get());
    fprintf(f, "unlock_all=%d\n", game_unlock_get());
    fprintf(f, "cheat_ammo=%d\n", game_cheat_ammo_get());
    fprintf(f, "cheat_god=%d\n", game_cheat_god_get());
    fprintf(f, "cheat_noshock=%d\n", game_cheat_noshock_get());
    fprintf(f, "cheat_nohud=%d\n", game_cheat_nohud_get());
    fprintf(f, "auto_switch=%d\n", game_autoswitch_get());
    fprintf(f, "enemy_grenades=%d\n", game_egren_get());
    fprintf(f, "pain_sounds=%d\n", game_pain_get());
    fprintf(f, "pain_volume=%d\n", game_painvol_get());
    fprintf(f, "audio_width=%d\n", apu_stereo_width_get());
    fprintf(f, "breath_sounds=%d\n", game_breath_get());
    fprintf(f, "breath_volume=%d\n", game_breathvol_get());
    fprintf(f, "scope_sway=%d\n", game_sway_get());
    fprintf(f, "scope_sway_strength=%d\n", game_swaystr_get());
    fprintf(f, "weapon_light=%d\n", game_weaponlight_get());
    fprintf(f, "gun_sway=%d\n", game_gunsway_get());
    fprintf(f, "gun_sway_strength=%d\n", game_gunswaystr_get());
    fprintf(f, "freecam_world=%d\n", game_freecamworld_get());
    fprintf(f, "freecam_speed=%d\n", game_freecamspeed_get());
    fprintf(f, "map_editor=%d\n", game_editor_get());
    fprintf(f, "hide_crosshair=%d\n", texdec_crosshair_hide_get());
    fprintf(f, "enemy_count=%d\n", game_ec_get());
    fprintf(f, "friends_mode=%d\n", game_friends_get());
    hudhide_ini_write(f);
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
    fprintf(f, "widescreen=%d\n", s_asp != 0);
    fprintf(f, "aspect=%s\n", s_asp == s_asp_run && s_asp_env[0] ? s_asp_env : k_asp_names[s_asp]);
    fprintf(f, "split_pads=%d\n", s_split);
    fprintf(f, "sprint_speed=%.2f\n", mkb_get_sprint_speed());
    fprintf(f, "sprint_time=%.1f\n", mkb_get_sprint_time());
    fprintf(f, "sprint_recover=%.1f\n", mkb_get_sprint_recover());
    fprintf(f, "fog=%.1f\n", nv2a_fog_mult_get());
    fprintf(f, "aniso=%d\n", g_gpu_aniso);
    fprintf(f, "fxaa=%d\n", g_gpu_fxaa);
    fprintf(f, "language=%s\n", s_lang_list[s_lang]);
    fprintf(f, "menu_lang=%s\n", s_mlang_list[s_mlang]);
    if (s_win_user)
        fprintf(f, "menu_rect=%d,%d,%d,%d\n", s_win_fx, s_win_fy, s_win_fw, s_win_fh);
    fprintf(f, "icons=%s\n", mkb_get_icon_mode() == 1 ? "pad" : mkb_get_icon_mode() == 2 ? "keyboard" : "auto");
    {
        static const char *const k_icon_style_ini[5] = { "xbox", "xbox360", "ps2", "ps4", "ps5" };
        fprintf(f, "icon_style=%s\n", k_icon_style_ini[mkb_get_icon_style() % 5]);
    }
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
        } else if (!strncmp(line, "explosion_size=", 15)) {
            rs_putenv("RECOMP_EXPLOSION_SIZE", line + 15);
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
        } else if (!strncmp(line, "corpse_60hz=", 12)) {
            rs_putenv("RECOMP_CORPSE_60HZ", line + 12);
        } else if (!strncmp(line, "fps_counter=", 12)) {
            rs_putenv("RECOMP_FPS_COUNTER", line + 12);
        } else if (!strncmp(line, "screen_blood=", 13)) {
            rs_putenv("RECOMP_SCREEN_BLOOD", line + 13);
        } else if (!strncmp(line, "screen_dirt=", 12)) {
            rs_putenv("RECOMP_SCREEN_DIRT", line + 12);
        } else if (!strncmp(line, "blood_stains=", 13)) {
            rs_putenv("RECOMP_BLOOD_STAINS", line + 13);
        } else if (!strncmp(line, "stain_nodes=", 12)) {
            rs_putenv("RECOMP_STAIN_NODES", line + 12);
        } else if (!strncmp(line, "firecap=", 8)) {
            rs_putenv("RECOMP_FIRE_CAP", line + 8);
        } else if (!strncmp(line, "lod=", 4)) {
            rs_putenv("RECOMP_LOD", line + 4);
        } else if (!strncmp(line, "stamina_bar=", 12)) {
            rs_putenv("RECOMP_STAMINA_BAR", line + 12);
        } else if (!strncmp(line, "enemy_count=", 12)) {
            rs_putenv("RECOMP_ENEMY_COUNT", line + 12);
        } else if (!strncmp(line, "menu_rect=", 10)) {
            int x, y, w, h;
            if (sscanf(line + 10, "%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w >= 1500 && w <= 10000 && h >= 1500 && h <= 10000
                && x >= 0 && y >= 0 && x + w <= 10050 && y + h <= 10050) {
                s_win_fx = x; s_win_fy = y; s_win_fw = w; s_win_fh = h;
                s_win_user = 1;
            }
        } else if (!strncmp(line, "enemy_count_", 12)) {
            /* the per-difficulty switches were removed (Enemy count applies on every difficulty); old lines are ignored */
        } else if (hudhide_ini_line(line)) {
            /* hud_hide_<id>= (hud_hide.c) */
        } else if (!strncmp(line, "friends_mode=", 13)) {
            rs_putenv("RECOMP_FRIENDS", line + 13);
        } else if (!strncmp(line, "damage_mode=", 12)) {
            rs_putenv("RECOMP_DAMAGE_MODE", line + 12);
        } else if (!strncmp(line, "hud_proportions=", 16)) {
            /* the option was removed again; an old line is ignored */
        } else if (!strncmp(line, "hit_marker=", 11)) {
            rs_putenv("RECOMP_HIT_MARKER", line + 11);
        } else if (!strncmp(line, "unlock_all=", 11)) {
            rs_putenv("RECOMP_UNLOCK_ALL", line + 11);
        } else if (!strncmp(line, "cheat_ammo=", 11)) {
            rs_putenv("RECOMP_CHEAT_AMMO", line + 11);
        } else if (!strncmp(line, "cheat_god=", 10)) {
            rs_putenv("RECOMP_CHEAT_GOD", line + 10);
        } else if (!strncmp(line, "cheat_noshock=", 14)) {
            rs_putenv("RECOMP_CHEAT_NOSHOCK", line + 14);
        } else if (!strncmp(line, "cheat_nohud=", 12)) {
            rs_putenv("RECOMP_CHEAT_NOHUD", line + 12);
        } else if (!strncmp(line, "wheel_up=", 9) || !strncmp(line, "wheel_down=", 11)) {
            /* the wheel options were removed again (the wheel is Change weapon); an old line is ignored */
        } else if (!strncmp(line, "grenade_macro=", 14)) {
            rs_putenv("RECOMP_GRENADE_MACRO", line + 14);
        } else if (!strncmp(line, "auto_switch=", 12)) {
            rs_putenv("RECOMP_AUTO_SWITCH", line + 12);
        } else if (!strncmp(line, "enemy_grenades=", 15)) {
            rs_putenv("RECOMP_ENEMY_GRENADES", line + 15);
        } else if (!strncmp(line, "pain_sounds=", 12)) {
            rs_putenv("RECOMP_PAIN_SOUNDS", line + 12);
        } else if (!strncmp(line, "audio_width=", 12)) {
            rs_putenv("RECOMP_AUDIO_WIDTH", line + 12);
        } else if (!strncmp(line, "pain_volume=", 12)) {
            rs_putenv("RECOMP_PAIN_VOLUME", line + 12);
        } else if (!strncmp(line, "breath_sounds=", 14)) {
            rs_putenv("RECOMP_BREATH", line + 14);
        } else if (!strncmp(line, "breath_volume=", 14)) {
            rs_putenv("RECOMP_BREATH_VOLUME", line + 14);
        } else if (!strncmp(line, "scope_sway=", 11)) {
            rs_putenv("RECOMP_SCOPE_SWAY", line + 11);
        } else if (!strncmp(line, "scope_sway_strength=", 20)) {
            rs_putenv("RECOMP_SCOPE_SWAY_STRENGTH", line + 20);
        } else if (!strncmp(line, "weapon_light=", 13)) {
            rs_putenv("RECOMP_WEAPON_LIGHT", line + 13);
        } else if (!strncmp(line, "gun_sway=", 9)) {
            rs_putenv("RECOMP_GUN_SWAY", line + 9);
        } else if (!strncmp(line, "gun_sway_strength=", 18)) {
            rs_putenv("RECOMP_GUN_SWAY_STRENGTH", line + 18);
        } else if (!strncmp(line, "freecam_world=", 14)) {
            rs_putenv("RECOMP_FREECAM_WORLD", line + 14);
        } else if (!strncmp(line, "freecam_speed=", 14)) {
            rs_putenv("RECOMP_FREECAM_SPEED", line + 14);
        } else if (!strncmp(line, "map_editor=", 11)) {
            rs_putenv("RECOMP_EDITOR", line + 11);
        } else if (!strncmp(line, "hide_crosshair=", 15)) {
            rs_putenv("RECOMP_HIDE_CROSSHAIR", line + 15);
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
        } else if (!strncmp(line, "aspect=", 7)) {              /* ultrawide: "21:9", "2.37", "auto" or "16:9" (off); the ini wins */
            rs_putenv("RECOMP_ASPECT", line + 7);
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
            { "RECOMP_WIDESCREEN", "1" }, { "RECOMP_MKB", "1" }, { "RECOMP_NO_RECOIL", "0" },
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
    {   /* aspect ratio, from RECOMP_WIDESCREEN / RECOMP_ASPECT (the ini lines already went through rs_putenv) */
        const char *ws = getenv("RECOMP_WIDESCREEN"), *as = getenv("RECOMP_ASPECT");
        float a = 0.0f, w, h;
        s_asp_env[0] = 0;
        if (ws && *ws == '0')
            s_asp = 0;
        else if (as && !_stricmp(as, "auto"))
            s_asp = 3;
        else {
            if (as && *as) {
                if (sscanf(as, "%f:%f", &w, &h) == 2 && h > 0.0f)
                    a = w / h;
                else
                    a = (float)atof(as);
            }
            s_asp = a < 1.8f ? 1 : 2;               /* any wider shape shows as 21:9 (a custom value is kept while the row is untouched) */
            if (s_asp > 1 && as && strlen(as) < sizeof s_asp_env)
                snprintf(s_asp_env, sizeof s_asp_env, "%s", as);
        }
        s_asp_run = s_asp;
    }
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
static void t_fire(char *o, size_t n) { snprintf(o, n, "%s", game_fire_name(game_fire_get())); }
static void a_fire(int d)             { game_fire_set(game_fire_get() + d); }
static void t_acc(char *o, size_t n)  { snprintf(o, n, "%s", game_super_accuracy_get() ? "On" : "Off"); }
static void a_acc(int d)              { (void)d; game_super_accuracy_set(!game_super_accuracy_get()); }
static void t_blood(char *o, size_t n) { snprintf(o, n, "%s", game_blood_name(game_blood_get())); }
static void a_blood(int d)            { game_blood_set((game_blood_get() + d + 4) % 4); }
static void t_expsz(char *o, size_t n) { snprintf(o, n, "%s", game_expsize_name(game_expsize_get())); }
static void a_expsz(int d)            { game_expsize_set((game_expsize_get() + d + 5) % 5); }
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
static void t_c60(char *o, size_t n) { snprintf(o, n, "%s", game_corpse60_get() ? "On" : "Off"); }
static void a_c60(int d)             { (void)d; game_corpse60_set(!game_corpse60_get()); }
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
static void t_stain(char *o, size_t n)
{
    static const char *const k_n[4] = { "Off", "Low", "Normal", "High" };
    snprintf(o, n, "%s", k_n[game_stain_get() & 3]);
}
static void a_stain(int d)            { game_stain_set((game_stain_get() + d + 4) % 4); }
static const unsigned k_stnodes[7] = { 100, 320, 600, 1000, 1500, 3000, 5000 };
static void t_stnodes(char *o, size_t n)
{
    snprintf(o, n, "%u", game_stain_nodes());
}
static void a_stnodes(int d)
{
    unsigned c = game_stain_nodes();
    int i, best = 0;
    for (i = 0; i < 7; i++)
        if (k_stnodes[i] <= c)
            best = i;
    game_stain_nodes_set(k_stnodes[(best + d + 7) % 7]);
}
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
static void t_hh0(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(0) ? "Hide" : "Show"); }
static void a_hh0(int d) { (void)d; hudhide_set(0, !hudhide_get(0)); }
static void t_hh1(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(1) ? "Hide" : "Show"); }
static void a_hh1(int d) { (void)d; hudhide_set(1, !hudhide_get(1)); }
static void t_hh2(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(2) ? "Hide" : "Show"); }
static void a_hh2(int d) { (void)d; hudhide_set(2, !hudhide_get(2)); }
static void t_hh3(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(3) ? "Hide" : "Show"); }
static void a_hh3(int d) { (void)d; hudhide_set(3, !hudhide_get(3)); }
static void t_hh4(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(4) ? "Hide" : "Show"); }
static void a_hh4(int d) { (void)d; hudhide_set(4, !hudhide_get(4)); }
static void t_hh5(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(5) ? "Hide" : "Show"); }
static void a_hh5(int d) { (void)d; hudhide_set(5, !hudhide_get(5)); }
static void t_hh6(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(6) ? "Hide" : "Show"); }
static void a_hh6(int d) { (void)d; hudhide_set(6, !hudhide_get(6)); }
static void t_hh7(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(7) ? "Hide" : "Show"); }
static void a_hh7(int d) { (void)d; hudhide_set(7, !hudhide_get(7)); }
static void t_hh8(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(8) ? "Hide" : "Show"); }
static void a_hh8(int d) { (void)d; hudhide_set(8, !hudhide_get(8)); }
static void t_hh9(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(9) ? "Hide" : "Show"); }
static void a_hh9(int d) { (void)d; hudhide_set(9, !hudhide_get(9)); }
static void t_hh10(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(10) ? "Hide" : "Show"); }
static void a_hh10(int d) { (void)d; hudhide_set(10, !hudhide_get(10)); }
static void t_hh11(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(11) ? "Hide" : "Show"); }
static void a_hh11(int d) { (void)d; hudhide_set(11, !hudhide_get(11)); }
static void t_hh12(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(12) ? "Hide" : "Show"); }
static void a_hh12(int d) { (void)d; hudhide_set(12, !hudhide_get(12)); }
static void t_hh13(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(13) ? "Hide" : "Show"); }
static void a_hh13(int d) { (void)d; hudhide_set(13, !hudhide_get(13)); }
static void t_hh14(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(14) ? "Hide" : "Show"); }
static void a_hh14(int d) { (void)d; hudhide_set(14, !hudhide_get(14)); }
static void t_hh15(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(15) ? "Hide" : "Show"); }
static void a_hh15(int d) { (void)d; hudhide_set(15, !hudhide_get(15)); }
static void t_hh16(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(16) ? "Hide" : "Show"); }
static void a_hh16(int d) { (void)d; hudhide_set(16, !hudhide_get(16)); }
static void t_hh17(char *o, size_t n) { snprintf(o, n, "%s", hudhide_get(17) ? "Hide" : "Show"); }
static void a_hh17(int d) { (void)d; hudhide_set(17, !hudhide_get(17)); }
static void t_frnd(char *o, size_t n)  { snprintf(o, n, "%s", game_friends_get() == 1 ? "Tough" : game_friends_get() == 2 ? "Weak" : "Normal"); }
static void a_frnd(int d)              { game_friends_set((game_friends_get() + d + 3) % 3); }
static void t_dmg(char *o, size_t n)   { snprintf(o, n, "%s", game_dmgmode_get() ? "Low damage" : "Normal"); }
static void a_dmg(int d)               { (void)d; game_dmgmode_set(!game_dmgmode_get()); }
static void t_hmark(char *o, size_t n) { snprintf(o, n, "%s", game_hitmark_get() ? "On" : "Off"); }
static void a_hmark(int d)             { (void)d; game_hitmark_set(!game_hitmark_get()); }
static void t_unlock(char *o, size_t n) { snprintf(o, n, "%s", game_unlock_get() ? "On" : "Off"); }
static void a_unlock(int d)            { (void)d; game_unlock_set(!game_unlock_get()); }
static void t_cammo(char *o, size_t n) { snprintf(o, n, "%s", game_cheat_ammo_get() ? "On" : "Off"); }
static void a_cammo(int d)             { (void)d; game_cheat_ammo_set(!game_cheat_ammo_get()); }
static void t_cgod(char *o, size_t n)  { snprintf(o, n, "%s", game_cheat_god_get() ? "On" : "Off"); }
static void a_cgod(int d)              { (void)d; game_cheat_god_set(!game_cheat_god_get()); }
static void t_cshock(char *o, size_t n) { snprintf(o, n, "%s", game_cheat_noshock_get() ? "On" : "Off"); }
static void a_cshock(int d)             { (void)d; game_cheat_noshock_set(!game_cheat_noshock_get()); }
static void t_chud(char *o, size_t n)   { snprintf(o, n, "%s", game_cheat_nohud_get() ? "Hide" : "Show"); }
static void a_chud(int d)               { (void)d; game_cheat_nohud_set(!game_cheat_nohud_get()); }
static void t_crally(char *o, size_t n) { snprintf(o, n, "%s", "Press A / Enter"); }
static void a_crally(int d)             { if (d > 0) game_cheat_rally_request(); }
static void a_cnem(int d)               { if (d > 0) game_cheat_nemesis_request(); }
static void t_autosw(char *o, size_t n)
{
    int m = game_autoswitch_get();
    snprintf(o, n, "%s", m == 2 ? "Out of ammo" : m == 1 ? "Empty magazine" : "Off");
}
static void a_autosw(int d)            { game_autoswitch_set((game_autoswitch_get() + (d < 0 ? 2 : 1)) % 3); }
static void t_egren(char *o, size_t n)
{
    static const char *k_n[4] = { "Off", "Low", "Medium", "High" };
    snprintf(o, n, "%s", k_n[game_egren_get() & 3]);
}
static void a_egren(int d)             { game_egren_set((game_egren_get() + (d < 0 ? 3 : 1)) % 4); }
static void t_gmac(char *o, size_t n)  { snprintf(o, n, "%s", mkb_gren_macro_get() ? "On" : "Off"); }
static void a_gmac(int d)              { (void)d; mkb_gren_macro_set(!mkb_gren_macro_get()); }
static void t_pain(char *o, size_t n)  { snprintf(o, n, "%s", game_pain_get() ? "On" : "Off"); }
static void a_pain(int d)              { (void)d; game_pain_set(!game_pain_get()); }
static int  pain_is_on(void)           { return game_pain_get(); }
static void t_awidth(char *o, size_t n) { snprintf(o, n, "%s", apu_stereo_width_get() > 0 ? "Original" : "Mono"); }
static void a_awidth(int d)            { (void)d; apu_stereo_width_set(apu_stereo_width_get() > 0 ? 0 : 100); }
static void t_painv(char *o, size_t n) { snprintf(o, n, "%d%%", game_painvol_get()); }
static void a_painv(int d)             { game_painvol_set(game_painvol_get() + 10 * d); }
static void t_breath(char *o, size_t n) { snprintf(o, n, "%s", game_breath_get() ? "On" : "Off"); }
static void a_breath(int d)            { (void)d; game_breath_set(!game_breath_get()); }
static int  breath_is_on(void)         { return game_breath_get(); }
static void t_breathv(char *o, size_t n) { snprintf(o, n, "%d%%", game_breathvol_get()); }
static void a_breathv(int d)           { game_breathvol_set(game_breathvol_get() + 10 * d); }
static void t_sway(char *o, size_t n)  { snprintf(o, n, "%s", game_sway_get() ? "On" : "Off"); }
static void a_sway(int d)              { (void)d; game_sway_set(!game_sway_get()); }
static int  sway_is_on(void)           { return game_sway_get(); }
static void t_swayst(char *o, size_t n) { snprintf(o, n, "%d", game_swaystr_get()); }
static void a_swayst(int d)            { game_swaystr_set(game_swaystr_get() + d); }
static void t_wlight(char *o, size_t n) { static const char *nm[5] = { "Off", "Low", "Normal", "High", "Extreme" }; int v = game_weaponlight_get(); snprintf(o, n, "%s", nm[v < 0 ? 0 : v > 4 ? 4 : v]); }
static void a_wlight(int d)            { game_weaponlight_set((game_weaponlight_get() + d + 5) % 5); }
static void t_gsway(char *o, size_t n) { snprintf(o, n, "%s", game_gunsway_get() ? "On" : "Off"); }
static void a_gsway(int d)             { (void)d; game_gunsway_set(!game_gunsway_get()); }
static int  gsway_is_on(void)          { return game_gunsway_get(); }
static void t_gswayst(char *o, size_t n) { snprintf(o, n, "%d", game_gunswaystr_get()); }
static void a_gswayst(int d)           { game_gunswaystr_set(game_gunswaystr_get() + d); }
static void t_fcam(char *o, size_t n)  { snprintf(o, n, "%s", game_freecam_get() ? "On" : "Off"); }
static void a_fcam(int d)              { (void)d; game_freecam_set(!game_freecam_get()); }
static void t_fcamw(char *o, size_t n) { snprintf(o, n, "%s", game_freecamworld_get() ? "Running" : "Frozen"); }
static void a_fcamw(int d)             { (void)d; game_freecamworld_set(!game_freecamworld_get()); }
static void t_fcams(char *o, size_t n) { snprintf(o, n, "%d", game_freecamspeed_get()); }
static void a_fcams(int d)             { game_freecamspeed_set(game_freecamspeed_get() + d); }
static void t_edit(char *o, size_t n)  { snprintf(o, n, "%s", game_editor_get() ? "On" : "Off"); }
static void a_edit(int d)              { (void)d; game_editor_set(!game_editor_get()); }
static void t_xhair(char *o, size_t n) { snprintf(o, n, "%s", texdec_crosshair_hide_get() ? "Hide" : "Show"); }
static void a_xhair(int d)             { (void)d; texdec_crosshair_hide_set(!texdec_crosshair_hide_get()); }
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
static void t_asp(char *o, size_t n)  { snprintf(o, n, "%s", k_asp_names[s_asp]); }
static void a_asp(int d)              { s_asp = (s_asp + d + 4) % 4; }
static int  p_asp(void)               { return s_asp != s_asp_run; }
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
static const char *const k_icon_style_name[5] = { "Xbox", "Xbox 360", "PS2 / PS3", "PS4", "PS5" };
static void t_ics(char *o, size_t n)  { snprintf(o, n, "%s", k_icon_style_name[mkb_get_icon_style() % 5]); }
static void a_ics(int d)              { mkb_set_icon_style((mkb_get_icon_style() + (d < 0 ? 4 : 1)) % 5); }

static void t_mlang(char *o, size_t n);
static void a_mlang(int d);

static const RsRow k_rows[] = {
    { "Menu language",       "live",             t_mlang, a_mlang, NULL },
    { "Field of view",       "live",             t_fov,  a_fov,  NULL   },
    { "Draw distance",       "live",             t_draw, a_draw, NULL   },
    { "Automatic fire speed", "live; how fast automatic guns (MP40, Thompson...) may fire. Fast = the game's own speed, which is quicker than intended above 30 fps", t_fire, a_fire, NULL },
    { "Model detail",        "live; Higher and Highest draw more polygons at a distance, so they can cost frames", t_lod, a_lod, NULL },
    { "Fog distance",        "live",             t_fog,  a_fog,  NULL   },
    { "Anisotropic filtering", "live",           t_aniso, a_aniso, NULL },
    { "FXAA anti-aliasing",  "live",             t_fxaa, a_fxaa, NULL   },
    { "Super Accuracy",      "applies in a mission", t_acc, a_acc, NULL },
    { "Enemy count (heavy)", "applies at the next mission load; x2 to x5 times the enemies that spawn points create (max x5), on every difficulty. Heavy: costs frames, keep it low on a slower PC", t_ecnt, a_ecnt, NULL },
    { "HUD: Compass", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh0, a_hh0, NULL },
    { "HUD: Health ring", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh1, a_hh1, NULL },
    { "HUD: Medkit count", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh2, a_hh2, NULL },
    { "HUD: Revive icon", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh3, a_hh3, NULL },
    { "HUD: Stance icon", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh4, a_hh4, NULL },
    { "HUD: Ammo", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh5, a_hh5, NULL },
    { "HUD: Grenades", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh6, a_hh6, NULL },
    { "HUD: Adrenaline bar", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh7, a_hh7, NULL },
    { "HUD: Overheat meter", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh8, a_hh8, NULL },
    { "HUD: Stopwatch", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh9, a_hh9, NULL },
    { "HUD: Demolition charge", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh10, a_hh10, NULL },
    { "HUD: Damage direction", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh11, a_hh11, NULL },
    { "HUD: Hint text", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh12, a_hh12, NULL },
    { "HUD: Out of ammo text", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh13, a_hh13, NULL },
    { "HUD: Squad commands", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh14, a_hh14, NULL },
    { "HUD: Nemesis card", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh15, a_hh15, NULL },
    { "HUD: Mission timer text", "applies at the next mission load (restart the game if nothing changes); Hide moves this part of the HUD off the screen", t_hh16, a_hh16, NULL },
    { "HUD: Compass markers", "live; hides the orange / blue / green arrows (objectives and squad mates) that the compass draws", t_hh17, a_hh17, NULL },
    { "Squad mates",         "live; Normal = the game's own. Tough: squad mates hit 3 times harder. Weak: they hit less than half as hard and miss more (wider aim error)", t_frnd, a_frnd, NULL },
    { "Damage mode",         "Normal: the game's own damage, and a Damage line in mods\\weapons works. Low damage: 7 hits (head 3); heavy guns 1 head / 2 body; the mods\\weapons Damage line is IGNORED. Live", t_dmg, a_dmg, NULL },
    { "Hit marker",          "live; a white X at the centre when you hit an enemy, a red X when you kill him", t_hmark, a_hmark, NULL },
    { "Enemy grenades", "live; enemies that are shooting at you throw a grenade from time to time: Low about every 20 s, Medium 12 s, High 7 s, at most one in the air. No throw animation, the grenade appears at the soldier's hand", t_egren, a_egren, NULL },
    { "Auto switch weapon",  "live; Empty magazine = the other weapon comes up when the magazine runs empty, instead of the reload. Out of ammo = only when the weapon has no rounds left at all (magazine and reserve)", t_autosw, a_autosw, NULL },
    { "Stereo sound",        "live; Original = the game's own mix: a sound at your side plays in one ear only, the other ear is silent (on some headphones it seems to vanish). Mono = both ears get the same sound, so nothing vanishes, but left / right / behind can no longer be told apart", t_awidth, a_awidth, NULL },
    { "Pain sounds",         "live; a pain sound when a soldier or you are hit, a scream when a soldier dies. Needs your WAV files in the mods folder, sounds, pain (see README there)", t_pain, a_pain, NULL },
    { "Pain volume",         "live; how loud the pain sounds are", t_painv, a_painv, NULL, pain_is_on, "turn Pain sounds on first" },
    { "Breathing sound",     "live; the player breathes after a moment of sprinting and while worn out. Uses the WAV files in the mods folder, sounds, breath", t_breath, a_breath, NULL },
    { "Breathing volume",    "live; how loud the breathing is", t_breathv, a_breathv, NULL, breath_is_on, "turn Breathing sound on first" },
    { "Scope sway",          "live; in a scope the aim moves in a slow wave while your health is low (red)", t_sway, a_sway, NULL },
    { "Scope sway strength", "live; 1 = faint, 5 = strong", t_swayst, a_swayst, NULL, sway_is_on, "turn Scope sway on first" },
    { "Weapon lighting",     "live; the weapon you hold reflects the light around you (a street lamp's colour, the sky), shows dust after explosions, blood after close hits and drops in the rain", t_wlight, a_wlight, NULL },
    { "Gun sway",            "live; the weapon has weight: it lags behind when you turn the camera and catches up with a small swing", t_gsway, a_gsway, NULL },
    { "Gun sway strength",   "live; 1 = light and quick, 5 = heavy and slow", t_gswayst, a_gswayst, NULL, gsway_is_on, "turn Gun sway on first" },
    { "Freecam",             "live, in a mission; the camera flies free of your soldier (also the Insert key). WASD fly along the view, mouse turns, Space up, C down, Shift fast, Ctrl slow. Pad: left stick fly, right stick turn, triggers up / down. The weapon is hidden", t_fcam, a_fcam, NULL },
    { "Freecam world",       "live; Frozen = enemies, physics and scripts stand still while you fly. Running = the game goes on around you", t_fcamw, a_fcamw, NULL },
    { "Freecam speed",       "live; 1 = slow, 5 = very fast (Shift makes it 4x faster)", t_fcams, a_fcams, NULL },
    { "Map editor",          "live, with the free camera on: Numpad 2 puts an enemy soldier 6 steps in front of the camera, Numpad 3 a friendly one, Numpad 7 takes the last one back, Numpad 4 kills the soldier in the crosshair, Numpad 6 clears the saved list. Placements are saved per mission in mods\\maps and come back by themselves when the mission starts", t_edit, a_edit, NULL },
    { "Game crosshair",      "live; Hide removes the game's own aiming mark (the scope's stays)", t_xhair, a_xhair, NULL },
    { "Explosion size",      "live; grenades, rockets, tank shells, barrels get a bigger fireball, smoke and debris. The blast damage is not changed", t_expsz, a_expsz, NULL },
    { "Blood amount",        "applies in a mission", t_blood, a_blood, NULL },
    { "Blood colour",        "applies in a mission", t_bcol, a_bcol, NULL },
    { "Blood hue",           "applies in a mission", t_bhue, a_bhue, NULL, blood_is_custom, "choose Custom first", sw_blood },
    { "Blood saturation",    "applies in a mission", t_bsat, a_bsat, NULL, blood_is_custom, "choose Custom first", sw_blood },
    { "Blood brightness",    "applies in a mission", t_bbri, a_bbri, NULL, blood_is_custom, "choose Custom first", sw_blood },
    { "Blood duration",      "seconds the spray stays; Auto = by amount", t_btime, a_btime, NULL },
    { "Keep corpses (experimental)", "bodies that stay on the ground", t_keep, a_keep, NULL },
    { "Corpse fix (60 Hz physics)", "live; On stops dead bodies flying into the air at high frame rates, but it can freeze or crash the game. Off: bodies may fly, no freeze", t_c60, a_c60, NULL },
    { "Screen blood",       "splatters on your view when an enemy is hit close", t_sblood, a_sblood, NULL },
    { "Blood stains",        "live; blood on walls and floor when a soldier is hit, when one dies and when you are hurt. Low = deaths only", t_stain, a_stain, NULL },
    { "Stain limit",         "how many blood stains and bullet holes stay on the walls and floor before the oldest start to vanish (the game itself keeps 100). More = longer-lasting blood but a little less speed when it fills up. Applies at the next level load", t_stnodes, a_stnodes, game_stain_nodes_pending },
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
    { "Grenade macro",       "live; On: hold the grenade key (4 / Up arrow, or the pad's D-pad up) to take the grenade out and pull the pin, release it to throw; your weapon then comes back by itself. Off: the grenade key works as before", t_gmac, a_gmac, NULL },
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
    { "Aspect ratio",        "restart; 4:3 and 16:9 are the game's own shapes. 21:9 widens the view (the HUD stays in 16:9 shape at the screen edges, menus stay 16:9). Auto follows your window or monitor. With a 16:9 resolution the width is made to fit", t_asp, a_asp, p_asp },
    { "Fullscreen",          "restart",          t_full, a_full, p_full },
    { "Split-screen pads",   "restart; keyboard and mouse = player 1, each gamepad = the next player", t_split, a_split, p_split },
    /* the cheats group: the game's own cheat codes go here, after this one */
    { "Cheats: Unlock all missions", "live; every mission can be picked in Mission Select (your save is not changed). Open Mission Select again after turning it on", t_unlock, a_unlock, NULL },
    { "Cheats: Unlimited ammo", "live; the game's own cheat: your weapons never run out of ammo (single player)", t_cammo, a_cammo, NULL },
    { "Cheats: Invincible",  "live; the game's own cheat: you cannot be killed (single player)", t_cgod, a_cgod, NULL },
    { "Cheats: Full adrenaline meter", "in a mission, single player: press to fill the adrenaline meter at once (the game's own cheat; its message says Rally Bar Filled)", t_crally, a_crally, NULL },
    { "Cheats: Kill nemesis", "in a mission, single player: press to kill the nemesis (the boss you have to hunt down) at once (the game's own cheat)", t_crally, a_cnem, NULL },
    { "Cheats: Disable shellshock", "live; the game's own cheat: no shellshock (the deafness and blur after a nearby explosion)", t_cshock, a_cshock, NULL },
    { "HUD: Whole HUD", "live; the game's own Disable HUD cheat: Hide switches the whole HUD off at once, Show brings it back", t_chud, a_chud, NULL },
    { "Apply and restart",   "restarts the game", t_apply, a_apply, p_any },
};
#define RS_ROWS ((int)(sizeof k_rows / sizeof k_rows[0]))

/* ---- the categories ---------------------------------------------------------------
 * The first screen lists the categories (and "Apply and restart" at the end); a category shows its own rows. The rows
 * stay in k_rows above; this table says which row belongs where, by name ("HUD: *" = every row starting with "HUD: ").
 * A row that is in no category falls into the last one, so nothing is lost when a row is added. */
#define RS_NCAT 9
#define RS_CATMAX 40
static const struct { const char *name; const char *rows[RS_CATMAX]; } k_cat_def[RS_NCAT] = {
    { "Video", { "Field of view", "Draw distance", "Model detail", "Fog distance", "Anisotropic filtering",
                 "FXAA anti-aliasing", "Frame rate", "Resolution", "Aspect ratio", "Fullscreen", "FPS counter", NULL } },
    { "Gameplay", { "Automatic fire speed", "Auto switch weapon", "Damage mode",
                    "Enemy grenades", "Enemy count (heavy)", "Sprint speed", "Sprint duration", "Rest to recover", "Gun sway",
                    "Gun sway strength", "Scope sway", "Scope sway strength", NULL } },
    { "HUD", { "HUD: *", "Hit marker", "Game crosshair", "Grenade timer", "Stamina bar", NULL } },
    { "Effects", { "Weapon lighting", "Explosion size", "Blood amount", "Blood colour", "Blood hue", "Blood saturation", "Blood brightness",
                   "Blood duration", "Keep corpses (experimental)", "Corpse fix (60 Hz physics)", "Screen blood", "Blood stains", "Stain limit", "Screen dirt", "Rain", "Screen rain",
                   "Rain amount", "Rain size", "Rain speed", NULL } },
    { "Audio", { "Stereo sound", "Pain sounds", "Pain volume", "Breathing sound", "Breathing volume", NULL } },
    { "Controls", { "Controls", "Button prompts", "Gamepad icons", "Mouse sensitivity", "Invert mouse Y",
                    "Grenade macro", "Split-screen pads", NULL } },
    { "Mods and tools", { "Freecam", "Freecam world", "Freecam speed", "Map editor", "Replaced textures", "Dump textures", "Dump sounds (WAV)", NULL } },
    { "Cheats", { "Cheats: *", "Super Accuracy", "Squad mates", NULL } },
    { "General", { "Menu language", "Language", "Cutscenes", NULL } },
};
static int s_cat_idx[RS_NCAT][RS_CATMAX], s_cat_n[RS_NCAT];
static int s_cat_built, s_apply_row = -1;
static int s_cat = -1;               /* the open category, -1 = the list of categories */
static int s_cat_back;               /* the list's selection while a category is open */

static void a_apply(int d);
static void rs_build_cats(void)
{
    int placed[RS_ROWS], c, i, k;
    if (s_cat_built)
        return;
    s_cat_built = 1;
    memset(placed, 0, sizeof placed);
    for (i = 0; i < RS_ROWS; i++)
        if (k_rows[i].adj == a_apply)
            s_apply_row = i;
    for (c = 0; c < RS_NCAT; c++) {
        s_cat_n[c] = 0;
        for (k = 0; k_cat_def[c].rows[k]; k++) {
            const char *nm = k_cat_def[c].rows[k];
            size_t L = strlen(nm);
            int star = L && nm[L - 1] == '*';
            for (i = 0; i < RS_ROWS; i++) {
                int hit = star ? !strncmp(k_rows[i].name, nm, L - 1) : !strcmp(k_rows[i].name, nm);
                if (hit && !placed[i] && i != s_apply_row && s_cat_n[c] < RS_CATMAX) {
                    placed[i] = 1;
                    s_cat_idx[c][s_cat_n[c]++] = i;
                    if (!star)
                        break;
                }
            }
        }
    }
    for (i = 0; i < RS_ROWS; i++)
        if (!placed[i] && i != s_apply_row && s_cat_n[RS_NCAT - 1] < RS_CATMAX) {
            s_cat_idx[RS_NCAT - 1][s_cat_n[RS_NCAT - 1]++] = i;
            fprintf(stderr, "[SETTINGS] row '%s' is in no category, put under General\n", k_rows[i].name);
        }
}

static int rs_n(void)                { rs_build_cats(); return s_cat < 0 ? RS_NCAT + 1 : s_cat_n[s_cat]; }
static const RsRow *rs_rowp(int i)   { return &k_rows[s_cat_idx[s_cat][i]]; }

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
        {
            extern void diag_clean_exit(void);
            diag_clean_exit();      /* a restart is a clean end: the new run must not see a crash flag */
        }
        TerminateProcess(GetCurrentProcess(), 0);   /* ExitProcess could wait on a thread holding the log lock */
    }
}

static void a_apply(int d)
{
    (void)d;
    if (p_any())
        rs_restart_game();
}

/* the list of categories: Enter / A / a click opens one (or restarts the game on the last entry) */
static void rs_open_cat(int c)
{
    if (c < 0 || c >= RS_NCAT || !s_cat_n[c])
        return;
    s_cat_back = s_sel;
    s_cat = c;
    s_sel = 0;
    s_top = 0;
    InvalidateRect(s_hwnd, NULL, FALSE);
}

static void rs_change(int dir)
{
    const RsRow *r;
    if (s_cat < 0) {
        if (s_sel < RS_NCAT)
            rs_open_cat(s_sel);
        else if (s_apply_row >= 0)
            k_rows[s_apply_row].adj(1);
        return;
    }
    r = rs_rowp(s_sel);
    if (r->enabled && !r->enabled())
        return;                         /* greyed out (Rain is Off): nothing to change */
    r->adj(dir);
    rs_save();
    InvalidateRect(s_hwnd, NULL, FALSE);
}

static void rs_select_to(int s)
{
    int n = rs_n();
    if (s < 0) s = 0;
    if (s > n - 1) s = n - 1;
    s_sel = s;
    if (s_sel < s_top)
        s_top = s_sel;
    if (s_sel >= s_top + RS_VIS)
        s_top = s_sel - RS_VIS + 1;
    InvalidateRect(s_hwnd, NULL, FALSE);
}

static void rs_select(int d)
{
    int n = rs_n();
    rs_select_to((s_sel + d + n) % n);
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
    if (code == VK_F12 || code == VK_F1 || code == VK_F2 || code == VK_F11 || code == VK_F9) {     /* screenshot / Recomp Settings / report / fullscreen / textures keys */
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
        else if (s_cat >= 0 || dir == 4) rs_change(dir == 3 ? -1 : 1);    /* the list of categories: Right opens, Left does nothing */
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
    } else if (s_cat >= 0) {                    /* back from a category to the list of categories */
        s_cat = -1;
        s_sel = s_cat_back;
        s_top = s_sel >= RS_VIS ? s_sel - RS_VIS + 1 : 0;
        InvalidateRect(s_hwnd, NULL, FALSE);
    } else {
        PostMessage(s_hwnd, WM_CLOSE, 0, 0);
    }
}

/* the mouse wheel: one row up or down, no wrapping (the pages with rows) */
static void rs_wheel(int up)
{
    if (s_page == 0) {
        rs_select_to(s_sel + (up ? -1 : 1));
    } else {
        int s = s_csel + (up ? -1 : 1);
        if (s < 0) s = 0;
        if (s > CT_ROWS - 1) s = CT_ROWS - 1;
        s_csel = s;
        if (s_csel < s_ctop)
            s_ctop = s_csel;
        if (s_csel >= s_ctop + RS_VIS)
            s_ctop = s_csel - RS_VIS + 1;
        InvalidateRect(s_hwnd, NULL, FALSE);
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

/* The restart button in the red band, on every page: a player who changed something never has to walk back to the
 * top. The first click arms it (so a stray click in a mission does not end the game), the second one restarts. */
static RECT  s_rst_rc;
static DWORD s_rst_arm;              /* tick of the first click, 0 = not armed */

static void rs_paint_restart(HDC mem, int W, int H, HFONT fsmall)
{
    HFONT old = (HFONT)SelectObject(mem, fsmall);
    int armed = s_rst_arm && GetTickCount() - s_rst_arm < 3000u;
    s_rst_rc.left = W / 2; s_rst_rc.right = W - W / 12;
    s_rst_rc.top = H / 20 + 3; s_rst_rc.bottom = H / 20 + H / 20 + 3;
    rs_text(mem, armed ? "[ Click again to restart ]" : "[ Restart game ]", &s_rst_rc, DT_RIGHT | DT_VCENTER,
            armed ? RGB(255, 235, 150) : p_any() ? RGB(255, 205, 90) : RGB(240, 215, 200));
    SelectObject(mem, old);
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
    rs_paint_restart(mem, W, H, fsmall);

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
    int W, H, i, rowh, y0, fh, nrows;
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
        rs_text(mem, s_cat < 0 ? "RECOMP SETTINGS" : k_cat_def[s_cat].name, &tx, DT_LEFT | DT_VCENTER, RGB(240, 232, 215));
        rs_paint_restart(mem, W, H, fsmall);
        if (s_win_mode) {                          /* how the window is handled */
            RECT hn = bar;
            hn.top = bar.bottom - H / 22; hn.right = W - W / 40;
            SelectObject(mem, fsmall);
            rs_text(mem, "drag this bar to move, drag the edges to resize", &hn, DT_RIGHT | DT_VCENTER, RGB(225, 170, 150));
        }
    }

    y0 = H * 28 / 100;
    rowh = H * 62 / 100 / RS_VIS;
    memset(s_row_rc, 0, sizeof s_row_rc);
    nrows = rs_n();
    for (i = s_top; i < nrows && i < s_top + RS_VIS; i++) {
        RECT row = { W / 12, y0 + (i - s_top) * rowh, W - W / 12, y0 + (i - s_top + 1) * rowh - 4 };
        RECT lab, val, tag;
        const RsRow *R = s_cat < 0 ? &k_rows[s_apply_row] : rs_rowp(i);   /* the last entry of the list is Apply and restart */
        int is_cat = s_cat < 0 && i < RS_NCAT;
        int pend = !is_cat && R->pending && R->pending();
        int off = !is_cat && R->enabled && !R->enabled();        /* greyed out: shown, not changeable */
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
        if (!is_cat && R->swatch) {   /* the colour square beside the value; dimmed while the row is greyed out */
            int sw_rgb[3], sz = (row.bottom - row.top) * 55 / 100;
            RECT sq, edge;
            R->swatch(sw_rgb);
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
        if (is_cat) {                 /* the list of categories: a name, how many rows it holds, an arrow */
            RECT whole = row;
            whole.left += 24;
            rs_text(mem, k_cat_def[i].name, &whole, DT_LEFT | DT_VCENTER,
                    i == s_sel ? RGB(255, 245, 225) : RGB(205, 200, 188));
            snprintf(buf, sizeof buf, "%d", s_cat_n[i]);
            tag = row; tag.right -= 20;
            SelectObject(mem, fsmall);
            rs_text(mem, buf, &tag, DT_RIGHT | DT_VCENTER, RGB(120, 118, 108));
            SelectObject(mem, fnorm);
            continue;
        }
        rs_text(mem, R->name, &lab, DT_LEFT | DT_VCENTER,
                off ? RGB(105, 103, 96) : i == s_sel ? RGB(255, 245, 225) : RGB(205, 200, 188));
        R->text(buf, sizeof buf);
        rs_text(mem, buf, &val, DT_CENTER | DT_VCENTER,
                off ? RGB(105, 103, 96) : i == s_sel ? RGB(255, 245, 225) : RGB(225, 220, 205));
        if (i == s_sel && !off && R->adj != a_apply) {
            RECT l = val, r = val;
            rs_text(mem, s_rtl ? ">" : "<", &l, DT_LEFT | DT_VCENTER, RGB(230, 90, 70));   /* mirrored: the left cell is drawn on the right */
            rs_text(mem, s_rtl ? "<" : ">", &r, DT_RIGHT | DT_VCENTER, RGB(230, 90, 70));
        }
        SelectObject(mem, fsmall);
        rs_text(mem, off && R->off ? R->off : pend && R->adj != a_apply ? (R->adj == a_ramount ? "next level load" : "restart to apply") : R->tag, &tag, DT_LEFT | DT_VCENTER,
                pend && !off ? RGB(230, 175, 60) : RGB(120, 118, 108));
    }

    rs_scrollbar(mem, W - W / 12 + 12, y0, RS_VIS * rowh - 4, nrows, s_top);
    {   /* footer */
        RECT ft = { W / 12, H * 92 / 100, W - W / 12, H };
        SelectObject(mem, fsmall);
        s_back_rc.left = W - W / 12 - W / 7; s_back_rc.right = W - W / 12;
        s_back_rc.top = ft.top - 6; s_back_rc.bottom = H * 98 / 100;
        rs_text(mem, s_cat < 0 ? "Up / Down / wheel  select      Enter / click  open      B / Esc  close"
                               : "Up / Down / wheel  select      Left / Right  change      B / Esc  back", &ft,
                DT_LEFT | DT_TOP, RGB(150, 146, 134));
        rs_text(mem, "[ Back ]", &s_back_rc, DT_RIGHT | DT_TOP, RGB(225, 100, 80));
        if (s_top > 0) {
            RECT up = { W / 12, y0 - rowh * 2 / 3, W - W / 12, y0 };
            rs_text(mem, "^ more above", &up, DT_RIGHT | DT_VCENTER, RGB(150, 146, 134));
        }
        if (s_top + RS_VIS < nrows) {
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

    if (s_win_mode && s_win_user) {                 /* where the player left it */
        r->left = gx + (int)((long long)gw * s_win_fx / 10000);
        r->top = gy + (int)((long long)gh * s_win_fy / 10000);
        r->right = r->left + (int)((long long)gw * s_win_fw / 10000);
        r->bottom = r->top + (int)((long long)gh * s_win_fh / 10000);
        return;
    }
    r->left = gx + (gw - ww) / 2;
    r->top = gy + (gh - wh) / 2;
    r->right = r->left + ww;
    r->bottom = r->top + wh;
}

/* ---- F1 window: move by the red bar, resize by the edges ---------------------------
 * Done by hand (capture + SetWindowPos) so the popup needs no system frame. The place is kept as fractions of the
 * game picture (menu_rect in the ini), so it survives a change of resolution or window size. */
#define RS_EDGE 8
static int   s_wdrag;                /* 0 none; else the grabbed parts: 1 left, 2 right, 4 top, 8 bottom, 16 the bar (move) */
static POINT s_wd_c;                 /* the cursor when the grab started */
static RECT  s_wd_r;                 /* the window then */

static void rs_game_rect(RECT *g)
{
    HWND game = FindWindowA("XboxRecompFramebuffer", NULL);
    RECT cl;
    POINT tl = { 0, 0 };
    if (game) {
        GetClientRect(game, &cl);
        ClientToScreen(game, &tl);
        g->left = tl.x; g->top = tl.y; g->right = tl.x + cl.right; g->bottom = tl.y + cl.bottom;
    } else {
        g->left = 0; g->top = 0; g->right = GetSystemMetrics(SM_CXSCREEN); g->bottom = GetSystemMetrics(SM_CYSCREEN);
    }
}

static int rs_win_hit(HWND hw, int x, int y)
{
    RECT cl;
    int m = 0;
    if (!s_win_mode)
        return 0;
    GetClientRect(hw, &cl);
    if (x < RS_EDGE) m |= 1;
    if (x >= cl.right - RS_EDGE) m |= 2;
    if (y < RS_EDGE) m |= 4;
    if (y >= cl.bottom - RS_EDGE) m |= 8;
    if (!m && s_page == 0 && y >= cl.bottom / 20 && y < cl.bottom / 20 + cl.bottom / 9)
        m = 16;
    return m;
}

static void rs_win_drag(HWND hw)
{
    POINT c;
    RECT r = s_wd_r, g;
    int dx, dy, gw, gh, minw, minh;

    GetCursorPos(&c);
    rs_game_rect(&g);
    gw = g.right - g.left;
    gh = g.bottom - g.top;
    if (gw < 1 || gh < 1)
        return;
    dx = c.x - s_wd_c.x;
    dy = c.y - s_wd_c.y;
    minw = gw * 20 / 100;
    minh = gh * 30 / 100;
    if (s_wdrag == 16) {
        OffsetRect(&r, dx, dy);
        if (r.left < g.left) OffsetRect(&r, g.left - r.left, 0);
        if (r.right > g.right) OffsetRect(&r, g.right - r.right, 0);
        if (r.top < g.top) OffsetRect(&r, 0, g.top - r.top);
        if (r.bottom > g.bottom) OffsetRect(&r, 0, g.bottom - r.bottom);
    } else {
        if (s_wdrag & 1) { r.left += dx; if (r.left < g.left) r.left = g.left; if (r.right - r.left < minw) r.left = r.right - minw; }
        if (s_wdrag & 2) { r.right += dx; if (r.right > g.right) r.right = g.right; if (r.right - r.left < minw) r.right = r.left + minw; }
        if (s_wdrag & 4) { r.top += dy; if (r.top < g.top) r.top = g.top; if (r.bottom - r.top < minh) r.top = r.bottom - minh; }
        if (s_wdrag & 8) { r.bottom += dy; if (r.bottom > g.bottom) r.bottom = g.bottom; if (r.bottom - r.top < minh) r.bottom = r.top + minh; }
    }
    SetWindowPos(hw, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_NOZORDER);
    s_win_fx = (int)((long long)(r.left - g.left) * 10000 / gw);
    s_win_fy = (int)((long long)(r.top - g.top) * 10000 / gh);
    s_win_fw = (int)((long long)(r.right - r.left) * 10000 / gw);
    s_win_fh = (int)((long long)(r.bottom - r.top) * 10000 / gh);
    s_win_user = 1;
    InvalidateRect(hw, NULL, FALSE);
}

static LPCSTR rs_win_cursor(int m)
{
    if (m == 16) return IDC_SIZEALL;
    if (m == 3 || m == 1 || m == 2) return IDC_SIZEWE;
    if (m == 4 || m == 8 || m == 12) return IDC_SIZENS;
    if (m == 5 || m == 10) return IDC_SIZENWSE;
    if (m == 6 || m == 9) return IDC_SIZENESW;
    return IDC_ARROW;
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
    if (!s_wdrag && (cur.left != want.left || cur.top != want.top || cur.right != want.right || cur.bottom != want.bottom)) {
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
        if (s_rst_arm && GetTickCount() - s_rst_arm >= 3000u) {      /* the restart button was not confirmed */
            s_rst_arm = 0;
            InvalidateRect(hw, NULL, FALSE);
        }
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
        if (s_wdrag) {
            rs_win_drag(hw);
            return 0;
        }
        if (s_drag) {
            rs_drag_to(p.y);
            return 0;
        }
        rs_hover_cell(p);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (s_wdrag) {
            s_wdrag = 0;
            if (msg == WM_LBUTTONUP)
                ReleaseCapture();
            rs_save();                                   /* the new place and size go to the ini */
            InvalidateRect(hw, NULL, FALSE);
            return 0;
        }
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
        if (msg == WM_LBUTTONDOWN && !s_cap && PtInRect(&s_rst_rc, p)) {     /* [ Restart game ]: two clicks */
            if (s_rst_arm && GetTickCount() - s_rst_arm < 3000u) {
                rs_save();
                rs_restart_game();
            }
            s_rst_arm = GetTickCount();
            InvalidateRect(hw, NULL, FALSE);
            return 0;
        }
        if (msg == WM_LBUTTONDOWN && !s_cap) {          /* the red bar moves the window, its edges resize it */
            int hit = rs_win_hit(hw, (short)LOWORD(lp), (short)HIWORD(lp));
            if (hit) {
                s_wdrag = hit;
                GetCursorPos(&s_wd_c);
                GetWindowRect(hw, &s_wd_r);
                SetCapture(hw);
                return 0;
            }
        }
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
        else
            rs_wheel((short)HIWORD(wp) > 0);      /* wheel up = the row above; values change with Left / Right or a click */
        return 0;
    case WM_SETCURSOR: {
        POINT c;
        int m = s_wdrag;
        if (!m && s_win_mode && !s_cap) {
            GetCursorPos(&c);
            ScreenToClient(hw, &c);
            m = rs_win_hit(hw, c.x, c.y);
        }
        SetCursor(LoadCursor(NULL, rs_win_cursor(m)));
        return TRUE;
    }
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
    s_cat = -1;
    s_rst_arm = 0;
    rs_build_cats();
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
    if (strstr(low, "level_a.str") || strstr(low, "loading.viv")) {     /* a level is loading: the free camera ends (new world) */
        extern void game_freecam_levelload(void);
        extern void game_editor_levelload(const char *lowpath);
        game_freecam_levelload();
        game_editor_levelload(low);
    }
    if (strstr(low, "\\sp\\hud2.xml")) {                                  /* the mission HUD is built: a second time in one level = a mission restart */
        extern void game_editor_hudload(void);
        game_editor_hudload();
    }
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

/* CROSS-LEVEL WEAPONS (2026-11-21 experiment, RECOMP_LEVELPATCH=1): a level's archive is replaced by mods\levelpatch\<level>_level_a.str, the same file
 * with the records of a weapon of another level appended (tools\str\merge.py). */
static void rs_levelpatch(const WCHAR *rel, WCHAR *host, DWORD host_size)
{
    static const WCHAR tail[] = L"level_a.str";
    WCHAR nrel[MAX_PATH], lvl[40], file[MAX_PATH], full[MAX_PATH];
    size_t rl, j, e, s, k;
    const char *on = getenv("RECOMP_LEVELPATCH");
    if (!on || *on != '1')
        return;
    wcsncpy_s(nrel, MAX_PATH, rel, _TRUNCATE);
    for (j = 0; nrel[j]; j++)
        if (nrel[j] == L'/')
            nrel[j] = L'\\';
    rl = wcslen(nrel);
    if (rl < 14 || _wcsicmp(nrel + rl - 11, tail) || nrel[rl - 12] != L'\\')
        return;
    e = rl - 12;
    for (s = e; s > 0 && nrel[s - 1] != L'\\'; s--)
        ;
    if (e - s < 1 || e - s >= 39)
        return;
    for (k = 0; k < e - s; k++)
        lvl[k] = nrel[s + k];
    lvl[k] = 0;
    swprintf_s(file, MAX_PATH, L"mods\\levelpatch\\%s_level_a.str", lvl);
    if (!GetFullPathNameW(file, MAX_PATH, full, NULL) || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES)
        return;
    wcscpy_s(host, host_size, full);
    fprintf(stderr, "[LEVELPATCH] %ls -> %ls\n", rel, full);
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

    hudhide_rewrite(rel, host, host_size);
    rs_levelpatch(rel, host, host_size);
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
        (void)hudhide_get(0);               /* reads the switches and sets the HUD picture cells before the first draw */
        CloseHandle(CreateThread(NULL, 0, rs_hotkey_thread, NULL, 0, NULL));
    }
}
