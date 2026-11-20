/*
 * hud_hide.c -- Show / Hide switches for the single HUD elements (Recomp Settings rows "HUD: ...").
 *
 * The HUD layout is data: DATA\HUD\shared\hud2.xml and DATA\HUD\SP\hud2.xml name every element and the <link>s that
 * place it next to its parent. When the game opens one of these two files, hudhide_rewrite() (called from the
 * path-rewrite hook of recomp_settings.c) writes a patched copy to <saves>\patched and hands the game that path.
 * A hidden element is moved 5000 units off the screen: through its own <link> (the element and everything linked below
 * it go together), through its own locx (elements with no link), or, for the damage-direction ring, by alpha 0.
 * The original files and any mods\ copy are never changed. The XML is read when the game builds its HUD, so a change
 * applies at the next mission load (or after a restart).
 *
 * ini: hud_hide_<id>=0/1 (id below), env RECOMP_HUDHIDE_<ID>. Default 0 = shown.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int game_save_root_w(WCHAR *out, DWORD n);

enum { OP_LINK, OP_OWN, OP_ALPHA, OP_REPARENT };
typedef struct { int file; int op; const char *name; const char *arg; } HhOp;   /* file 0 = shared\hud2.xml, 1 = SP\hud2.xml */
typedef struct { const char *id, *label, *tip; HhOp ops[3]; } HhItem;

static const HhItem k_items[] = {
    { "compass",    "HUD: Compass",            "the compass dial and its glass",
        { { 0, OP_LINK, "compass_center", NULL }, { 0, OP_LINK, "compass_glass", NULL } } },
    { "healthring", "HUD: Health ring",        "the health ring around the compass",
        { { 0, OP_LINK, "health_meter_all", NULL } } },
    { "medkit",     "HUD: Medkit count",       "the medkit number",
        { { 0, OP_LINK, "medkit_count", NULL } } },
    { "revive",     "HUD: Revive icon",        "the revive (second life) icon and count",
        { { 0, OP_LINK, "Revive", NULL }, { 0, OP_REPARENT, "stance_indicator", "parent=\"Revive\"|parent=\"compass_dummy\"" } } },
    { "stance",     "HUD: Stance icon",        "the stand / crouch / prone figure",
        { { 0, OP_LINK, "stance_indicator", NULL } } },
    { "ammo",       "HUD: Ammo",               "the ammo icon and the clip / reserve numbers",
        { { 0, OP_LINK, "Ammo", NULL } } },
    { "grenade",    "HUD: Grenades",           "the grenade icon and count",
        { { 0, OP_LINK, "Grenade", NULL } } },
    { "rally",      "HUD: Adrenaline bar",     "the adrenaline (rally) bar and its icons",
        { { 0, OP_LINK, "rally_dummy", NULL } } },
    { "overheat",   "HUD: Overheat meter",     "the heat meter of mounted guns",
        { { 0, OP_LINK, "overheat_indicator", NULL } } },
    { "stopwatch",  "HUD: Stopwatch",          "the timer dial (timed missions)",
        { { 0, OP_OWN, "stopwatchtimer", NULL } } },
    { "democharge", "HUD: Demolition charge",  "the charge bar",
        { { 0, OP_OWN, "democharge_indicator", NULL } } },
    { "hitdir",     "HUD: Damage direction",   "the red arcs that show where you were hit from (live: clears their cell of the HUD picture)",
        { { 0, 0, NULL, NULL } } },
    { "hints",      "HUD: Hint text",          "the hint line in the middle (controls tips)",
        { { 0, OP_OWN, "csa_richtext", NULL } } },
    { "outofammo",  "HUD: Out of ammo text",   "the out of ammo warning",
        { { 0, OP_OWN, "out_of_ammo", NULL } } },
    { "squadcmd",   "HUD: Squad commands",     "the squad command icons",
        { { 1, OP_LINK, "command_icons", NULL } } },
    { "nemesis",    "HUD: Nemesis card",       "the boss card at the right",
        { { 1, OP_OWN, "nemesis_card", NULL } } },
    { "leveltimer", "HUD: Mission timer text", "the mission timer number",
        { { 1, OP_OWN, "sp_timer_text", NULL } } },
    { "beads",      "HUD: Compass markers",    "the orange / blue / green arrows (objectives, mates); live: clears their cells of the HUD picture",
        { { 0, 0, NULL, NULL } } },
};
#define HH_N ((int)(sizeof k_items / sizeof k_items[0]))
static int s_hide[HH_N];
static int s_init;
extern void texdec_hudcells_set(int mask);        /* nv2a_pb_exec.c: cells of the HUD picture made see-through */

static int hh_idx(const char *id)
{
    int i;
    for (i = 0; i < HH_N; i++)
        if (!strcmp(k_items[i].id, id))
            return i;
    return -1;
}

/* the picture cells: 1 = damage direction arcs, 2 = compass markers (also gone when the whole compass is hidden) */
static void hh_cells(void)
{
    int m = 0;
    if (s_hide[hh_idx("hitdir")])
        m |= 1;
    if (s_hide[hh_idx("beads")] || s_hide[hh_idx("compass")])
        m |= 2;
    texdec_hudcells_set(m);
}

static void hh_init(void)
{
    int i;
    if (s_init)
        return;
    s_init = 1;
    for (i = 0; i < HH_N; i++) {
        char nm[64], *p;
        const char *e;
        snprintf(nm, sizeof nm, "RECOMP_HUDHIDE_%s", k_items[i].id);
        for (p = nm; *p; p++)
            *p = (char)toupper((unsigned char)*p);
        e = getenv(nm);
        s_hide[i] = e && e[0] && e[0] != '0';
    }
    hh_cells();
}

int hudhide_count(void)                  { return HH_N; }
const char *hudhide_label(int i)         { return k_items[i].label; }
const char *hudhide_tip(int i)           { return k_items[i].tip; }
int hudhide_get(int i)                   { hh_init(); return i >= 0 && i < HH_N ? s_hide[i] : 0; }
void hudhide_set(int i, int v)           { hh_init(); if (i >= 0 && i < HH_N) { s_hide[i] = v ? 1 : 0; hh_cells(); } }

void hudhide_ini_write(FILE *f)
{
    int i;
    hh_init();
    for (i = 0; i < HH_N; i++)
        fprintf(f, "hud_hide_%s=%d\n", k_items[i].id, s_hide[i]);
}

/* one ini line "hud_hide_<id>=v": 1 = taken (the env variable is set) */
int hudhide_ini_line(const char *line)
{
    int i;
    if (strncmp(line, "hud_hide_", 9))
        return 0;
    for (i = 0; i < HH_N; i++) {
        size_t n = strlen(k_items[i].id);
        if (!strncmp(line + 9, k_items[i].id, n) && line[9 + n] == '=') {
            char buf[96], *p;
            snprintf(buf, sizeof buf, "RECOMP_HUDHIDE_%s=%d", k_items[i].id, atoi(line + 10 + n) ? 1 : 0);
            for (p = buf; *p && *p != '='; p++)
                *p = (char)toupper((unsigned char)*p);
            _putenv(buf);
            return 1;
        }
    }
    return 0;
}

/* ---- XML patching ------------------------------------------------------------------------------------------ */

typedef struct { char *p; size_t n, cap; } Buf;

static int in_comment(const char *x, size_t pos)
{
    const char *s = x, *c;
    while ((c = strstr(s, "<!--")) != NULL && (size_t)(c - x) < pos) {
        const char *e = strstr(c + 4, "-->");
        if (!e || (size_t)(e - x) >= pos)
            return 1;
        s = e + 3;
    }
    return 0;
}

/* the tag holding `needle` that is not in a comment: [*ts, *te] = the '<' and the '>' . is_link: 1 want <link, 0 want not <link */
static int find_tag(const Buf *b, const char *needle, int is_link, size_t *ts, size_t *te)
{
    const char *s = b->p, *m;
    while ((m = strstr(s, needle)) != NULL) {
        size_t pos = (size_t)(m - b->p), a = pos, e;
        s = m + 1;
        while (a > 0 && b->p[a] != '<')
            a--;
        if (b->p[a] != '<' || in_comment(b->p, a))
            continue;
        if ((strncmp(b->p + a, "<link", 5) == 0) != (is_link != 0))
            continue;
        for (e = pos; e < b->n && b->p[e] != '>'; e++) {}
        if (e >= b->n)
            continue;
        *ts = a;
        *te = e;
        return 1;
    }
    return 0;
}

static void replace_range(Buf *b, size_t from, size_t to, const char *with)   /* [from, to) */
{
    size_t wl = strlen(with), nn = b->n - (to - from) + wl;
    if (nn + 1 > b->cap) {
        b->cap = nn + 4096;
        b->p = (char *)realloc(b->p, b->cap);
    }
    memmove(b->p + from + wl, b->p + to, b->n - to + 1);
    memcpy(b->p + from, with, wl);
    b->n = nn;
}

/* attribute attr="number" inside the tag [ts, te]: number += delta (or = set when absolute), else inserted */
static void tag_number(Buf *b, size_t ts, size_t te, const char *attr, int delta, int absolute)
{
    char key[24], out[40];
    char *m;
    size_t i;
    snprintf(key, sizeof key, " %s=\"", attr);
    m = NULL;
    for (i = ts; i + strlen(key) <= te; i++)
        if (!strncmp(b->p + i, key, strlen(key))) {
            m = b->p + i;
            break;
        }
    if (m) {
        size_t vs = (size_t)(m - b->p) + strlen(key), ve = vs;
        int cur = atoi(b->p + vs);
        while (ve < te && b->p[ve] != '"')
            ve++;
        snprintf(out, sizeof out, "%d", absolute ? delta : cur + delta);
        replace_range(b, vs, ve, out);
    } else {
        size_t at = ts + 1;
        while (at < te && !isspace((unsigned char)b->p[at]))
            at++;
        snprintf(out, sizeof out, " %s=\"%d\"", attr, absolute ? delta : delta);
        replace_range(b, at, at, out);
    }
}

static void apply_op(Buf *b, const HhOp *op)
{
    char needle[96];
    size_t ts, te;
    if (op->op == OP_LINK) {
        snprintf(needle, sizeof needle, "child=\"%s\"", op->name);
        if (find_tag(b, needle, 1, &ts, &te))
            tag_number(b, ts, te, "locx", 5000, 0);
    } else if (op->op == OP_OWN) {
        snprintf(needle, sizeof needle, "name=\"%s\"", op->name);
        if (find_tag(b, needle, 0, &ts, &te))
            tag_number(b, ts, te, "locx", 5000, 0);
    } else if (op->op == OP_ALPHA) {
        snprintf(needle, sizeof needle, "name=\"%s\"", op->name);
        if (find_tag(b, needle, 0, &ts, &te))
            tag_number(b, ts, te, "alpha", 0, 1);
    } else if (op->op == OP_REPARENT) {
        char from[64], to[64];
        const char *bar = strchr(op->arg, '|');
        size_t fl, i;
        if (!bar)
            return;
        fl = (size_t)(bar - op->arg);
        memcpy(from, op->arg, fl);
        from[fl] = 0;
        snprintf(to, sizeof to, "%s", bar + 1);
        snprintf(needle, sizeof needle, "child=\"%s\"", op->name);
        if (find_tag(b, needle, 1, &ts, &te))
            for (i = ts; i + fl <= te; i++)
                if (!strncmp(b->p + i, from, fl)) {
                    replace_range(b, i, i + fl, to);
                    break;
                }
    }
}

static int tail_is(const WCHAR *rel, const WCHAR *tail)
{
    size_t rl = wcslen(rel), tl = wcslen(tail);
    return rl >= tl && !_wcsicmp(rel + rl - tl, tail);
}

/* g_xbox_path_rewrite step: rel = the path below the game folder, host = the file about to be opened (in / out) */
void hudhide_rewrite(const WCHAR *rel, WCHAR *host, DWORD host_size)
{
    static const WCHAR *k_tail[2] = { L"DATA\\HUD\\shared\\hud2.xml", L"DATA\\HUD\\SP\\hud2.xml" };
    static const WCHAR *k_out[2] = { L"hud2_shared_hide.xml", L"hud2_sp_hide.xml" };
    int file = -1, i, k, any = 0;
    Buf b = { 0 };
    FILE *f;
    long sz;
    WCHAR base[MAX_PATH], dir[MAX_PATH], out[MAX_PATH];
    const char *off = getenv("RECOMP_HUDHIDE");
    if (off && *off == '0')
        return;
    {   /* the game may ask with forward slashes */
        WCHAR nrel[MAX_PATH];
        size_t j;
        wcsncpy_s(nrel, MAX_PATH, rel, _TRUNCATE);
        for (j = 0; nrel[j]; j++)
            if (nrel[j] == L'/')
                nrel[j] = L'\\';
        for (i = 0; i < 2; i++)
            if (tail_is(nrel, k_tail[i]))
                file = i;
    }
    if (file < 0)
        return;
    hh_init();
    for (i = 0; i < HH_N; i++)
        for (k = 0; k < 3; k++)
            if (s_hide[i] && k_items[i].ops[k].name && k_items[i].ops[k].file == file)
                any = 1;
    if (!any)
        return;
    f = _wfopen(host, L"rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return;
    }
    b.cap = (size_t)sz + 4096;
    b.p = (char *)malloc(b.cap);
    if (!b.p) {
        fclose(f);
        return;
    }
    b.n = fread(b.p, 1, (size_t)sz, f);
    b.p[b.n] = 0;
    fclose(f);
    for (i = 0; i < HH_N; i++)
        if (s_hide[i])
            for (k = 0; k < 3; k++)
                if (k_items[i].ops[k].name && k_items[i].ops[k].file == file)
                    apply_op(&b, &k_items[i].ops[k]);
    if (!game_save_root_w(base, MAX_PATH)) {
        free(b.p);
        return;
    }
    swprintf_s(dir, MAX_PATH, L"%s\\patched", base);
    CreateDirectoryW(dir, NULL);
    swprintf_s(out, MAX_PATH, L"%s\\%s", dir, k_out[file]);
    f = _wfopen(out, L"wb");
    if (f) {
        fwrite(b.p, 1, b.n, f);
        fclose(f);
        wcscpy_s(host, host_size, out);
        {
            static int said[2];
            if (said[file]++ < 3)
                fprintf(stderr, "[HUDHIDE] %S patched -> %S\n", rel, out);
        }
    }
    free(b.p);
}
