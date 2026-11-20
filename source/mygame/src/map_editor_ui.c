/* IN-GAME EDITOR WINDOW (2026-11-20). Included at the END of map_editor.c (one translation unit with game_patches.c); nothing else includes it.
 *
 * A second topmost, no-activate Win32 window (own thread, like the F1 overlay in recomp_settings.c) docked at the left of the game window.
 * Numpad0 shows / hides it while the free camera is on. Tabs: Characters (groups Germans / Allies / Other, sub-folders taken from the folder after
 * "Characters\" in the type file path, items = the type files) and Props (empty: no spawn path known yet).
 * Mouse: while the window is up the pointer is free (g_mkb_free_cursor, mkb_input.c) and the RIGHT button held turns the camera (the pointer is
 * pinned meanwhile); a click on an item selects the type (NUMPAD2 places it), a double click places one at the screen centre, a drag drops one
 * where the mouse ray meets the ground plane at the player's height (the soldier falls to the floor by itself).
 * Threads: the window thread never touches guest memory. It posts requests (type path + normalised point inside the game window) into a
 * critical-section mailbox; ed_ui_frame (game thread, once a frame) answers them. The catalog goes the other way as a versioned snapshot. */
extern volatile long g_mkb_free_cursor;

#define ED_NUI 384
typedef struct { char path[ED_PATH]; int team; int loaded; } EdUiItem;

static CRITICAL_SECTION s_ui_cs;
static int s_ui_cs_ready;
static EdUiItem s_ui_items[ED_NUI];             /* the snapshot (written by the game thread under s_ui_cs, read by the window thread) */
static int s_ui_n;
static volatile LONG s_ui_ver;
static char s_ui_selpath[ED_PATH];
static volatile LONG s_ui_req;                  /* 0 none, 1 place, 2 select */
static char s_ui_req_path[ED_PATH];
static float s_ui_req_mx, s_ui_req_my;
static volatile LONG s_ui_rc[4];                /* the window's screen rectangle while it is up, else zeros */
/* drag preview: the window thread asks (on, mouse inside the game window), the game thread answers with the depth of the point under the mouse
 * (along the view direction) and the projection scales, so the window thread can size a translucent silhouette without touching guest memory */
static volatile LONG s_ui_pv_on, s_ui_pv_seq, s_ui_pv_hit;
static volatile float s_ui_pv_mx, s_ui_pv_my, s_ui_pv_depth, s_ui_pv_px = 1.0f, s_ui_pv_py = 1.0f;
/* zone rings: the game thread publishes the camera (eye, fwd, right, up, projection scales) and the zones each frame, the window thread projects the circles
 * onto the game window (a click-through colour-keyed window, GDI lines) */
typedef struct { float p[3], r; int team; char label[96]; } UiZone;
static float s_ui_cam[14];
static UiZone s_ui_zn[ED_NZN];
static volatile LONG s_ui_nzn;
static volatile LONG s_ui_thread_up;
static int s_ui_vis;                            /* the user wants it (Numpad0) */
static int s_ui_look;                           /* the right button is held: the mouse turns the camera */
static char s_ed_gcat[ED_NUI][ED_PATH];         /* every type file ever seen, kept in mods\maps\catalog.txt */
static int s_ed_ngcat, s_ed_gcat_loaded;

static int ed_anytype(void)
{
    static int any = -1;
    if (any < 0) {
        const char *e = getenv("RECOMP_EDITOR_ANYTYPE");
        any = e && *e == '1';
    }
    return any;
}

/* ---------- the global catalog file ---------- */
static void ed_gcat_load(void)
{
    FILE *f;
    char line[ED_PATH + 8];
    if (s_ed_gcat_loaded)
        return;
    s_ed_gcat_loaded = 1;
    f = fopen("mods\\maps\\catalog.txt", "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f) && s_ed_ngcat < ED_NUI) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
            line[--n] = 0;
        if (n > 3 && line[0] != '#' && n < ED_PATH)
            strcpy(s_ed_gcat[s_ed_ngcat++], line);
    }
    fclose(f);
}
static void ed_gcat_add(const char *path)
{
    int i;
    FILE *f;
    ed_gcat_load();
    for (i = 0; i < s_ed_ngcat; i++)
        if (!_stricmp(s_ed_gcat[i], path))
            return;
    if (s_ed_ngcat >= ED_NUI || strlen(path) >= ED_PATH)
        return;
    strcpy(s_ed_gcat[s_ed_ngcat++], path);
    CreateDirectoryA("mods", NULL);
    CreateDirectoryA("mods\\maps", NULL);
    f = fopen("mods\\maps\\catalog.txt", "a");
    if (f) {
        fprintf(f, "%s\n", path);
        fclose(f);
    }
}

/* ---------- game thread: the mouse ray ---------- */
static float s_ed_vr[3], s_ed_vu[3], s_ed_px = 1.0f, s_ed_py = 1.0f;
void game_editor_view(const float *r, const float *u, float px, float py)
{
    memcpy(s_ed_vr, r, sizeof s_ed_vr);
    memcpy(s_ed_vu, u, sizeof s_ed_vu);
    if (px > 0.05f && px < 50.0f && py > 0.05f && py < 50.0f) {
        s_ed_px = px;
        s_ed_py = py;
    }
}
/* the unit direction of the ray under the mouse (mx, my in 0..1 inside the game window) */
static void ed_mouse_dir(float mx, float my, const float *fwd, float *d)
{
    float nx = 2.0f * mx - 1.0f, ny = 1.0f - 2.0f * my, len;
    int i;
    for (i = 0; i < 3; i++)
        d[i] = fwd[i] + s_ed_vr[i] * (nx / s_ed_px) + s_ed_vu[i] * (ny / s_ed_py);
    len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-4f)
        len = 1.0f;
    for (i = 0; i < 3; i++)
        d[i] /= len;
}
/* the point of the world under the mouse (mx, my in 0..1 inside the game window): the ray meets the horizontal plane at the player's feet */
static void ed_ray_point(float mx, float my, const float *eye, const float *fwd, float *out)
{
    float nx = 2.0f * mx - 1.0f, ny = 1.0f - 2.0f * my, d[3], len, t, zref = eye[2] - 1.6f;
    uint32_t pc = game_pc_get();
    int i;
    for (i = 0; i < 3; i++)
        d[i] = fwd[i] + s_ed_vr[i] * (nx / s_ed_px) + s_ed_vu[i] * (ny / s_ed_py);
    len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-4f)
        len = 1.0f;
    for (i = 0; i < 3; i++)
        d[i] /= len;
    if (s_ed_dbg)
        fprintf(stderr, "[EDITOR] mouse %.3f %.3f: eye %.2f %.2f %.2f fwd %.2f %.2f %.2f right %.2f %.2f %.2f up %.2f %.2f %.2f proj %.3f %.3f -> ray %.3f %.3f %.3f\n", (double)mx,
                (double)my, (double)eye[0], (double)eye[1], (double)eye[2], (double)fwd[0], (double)fwd[1], (double)fwd[2], (double)s_ed_vr[0], (double)s_ed_vr[1],
                (double)s_ed_vr[2], (double)s_ed_vu[0], (double)s_ed_vu[1], (double)s_ed_vu[2], (double)s_ed_px, (double)s_ed_py, (double)d[0], (double)d[1], (double)d[2]);
    if (ed_world_point(eye, d, out)) {          /* the game's own ray: the floor, or the top of a crate, wherever the mouse is */
        out[2] += 0.15f;
        return;
    }
    if (ed_ptr(pc)) {
        float pz = ed_f(pc + 0x68);
        if (pz == pz && fabsf(pz - eye[2]) < 400.0f)
            zref = pz;
    }
    if (d[2] < -0.02f) {
        t = (zref - eye[2]) / d[2];
        if (!(t > 0.5f))
            t = 15.0f;
        if (t > 150.0f)
            t = 150.0f;
    } else {
        t = 15.0f;                              /* looking at or above the horizon: a point in front */
    }
    for (i = 0; i < 3; i++)
        out[i] = eye[i] + d[i] * t;
    out[2] += 1.0f;                             /* a little above the plane: the soldier falls onto whatever floor there is */
    if (s_ed_dbg)
        fprintf(stderr, "[EDITOR] mouse %.3f %.3f -> ray %.2f %.2f %.2f, plane z %.2f, t %.1f, point %.2f %.2f %.2f (proj %.3f %.3f)\n", (double)mx,
                (double)my, (double)d[0], (double)d[1], (double)d[2], (double)zref, (double)t, (double)out[0], (double)out[1], (double)out[2],
                (double)s_ed_px, (double)s_ed_py);
}

/* ---------- game thread: catalog snapshot, requests, look ---------- */
static EdUiItem s_ui_tmp[ED_NUI];
static void ed_ui_publish(void)
{
    int n = 0, i, j, p, changed;
    char sel[ED_PATH];
    ed_gcat_load();
    ed_pt_gload();
    ed_pt_sync_wtable();
    memset(s_ui_tmp, 0, sizeof s_ui_tmp);
    strcpy(s_ui_tmp[n].path, "ZONE");           /* the spawn zone tool (Zones tab): team 101 */
    s_ui_tmp[n].loaded = 1;
    s_ui_tmp[n++].team = 101;
    for (i = 0; i < s_ed_ncat && n < ED_NUI; i++) {
        EdUiItem *t = &s_ui_tmp[n++];
        strcpy(t->path, s_ed_cat[i]);
        t->loaded = 1;
        p = ed_sp_slot(s_ed_cat_sp[i], 0);
        t->team = p >= 0 ? game_editor_spawner_team(s_ed_sp[p]) : 0;
    }
    for (j = 0; j < s_ed_ngcat && n < ED_NUI; j++) {
        EdUiItem *t;
        if (ed_cat_find(s_ed_gcat[j]) >= 0)
            continue;
        t = &s_ui_tmp[n++];
        strcpy(t->path, s_ed_gcat[j]);
    }
    for (j = 0; j < s_ptpl_n && n < ED_NUI; j++) {      /* the pickup kinds this level has made (Props tab): team 100 marks them */
        EdUiItem *t = &s_ui_tmp[n++];
        strcpy(t->path, s_ptpl[j].path);
        t->loaded = 1;
        t->team = 100;
    }
    for (j = 0; j < s_pt_gn && n < ED_NUI; j++) {       /* kinds seen in other levels (grey): this level never made one */
        EdUiItem *t;
        if (ed_prop_find(s_pt_g[j].path) >= 0)
            continue;
        t = &s_ui_tmp[n++];
        strcpy(t->path, s_pt_g[j].path);
        t->loaded = 1;
        t->team = 100;
    }
    if (s_ed_selprop[0])
        strcpy(sel, s_ed_selprop);
    else
        strcpy(sel, s_ed_sel >= 0 && s_ed_sel < s_ed_ncat ? s_ed_cat[s_ed_sel] : "");
    changed = n != s_ui_n || memcmp(s_ui_tmp, s_ui_items, (size_t)n * sizeof s_ui_tmp[0]) != 0 || strcmp(sel, s_ui_selpath) != 0;
    if (!changed)
        return;
    EnterCriticalSection(&s_ui_cs);
    memcpy(s_ui_items, s_ui_tmp, (size_t)n * sizeof s_ui_tmp[0]);
    s_ui_n = n;
    strcpy(s_ui_selpath, sel);
    InterlockedIncrement(&s_ui_ver);
    LeaveCriticalSection(&s_ui_cs);
}
static void ed_ui_requests(const float *eye, const float *fwd)
{
    char path[ED_PATH], msg[256];
    int kind, ci;
    float mx, my;
    if (!s_ui_req)
        return;
    EnterCriticalSection(&s_ui_cs);
    kind = (int)s_ui_req;
    strcpy(path, s_ui_req_path);
    mx = s_ui_req_mx;
    my = s_ui_req_my;
    s_ui_req = 0;
    LeaveCriticalSection(&s_ui_cs);
    if (kind == 3) {
        ed_clear_all();
        return;
    }
    if (kind == 7) {                            /* the Edits button: this level's saved edits on / off */
        ed_apply_toggle();
        return;
    }
    if (kind == 6) {                            /* a zone setting: mx = which, my = -1 / +1 */
        int w = (int)(mx + 0.5f), d = my > 0.0f ? 1 : -1;
        if (w == 0) {
            s_zn_def_count += d;
            if (s_zn_def_count < 1) s_zn_def_count = 1;
            if (s_zn_def_count > ED_ZMAX) s_zn_def_count = ED_ZMAX;
        } else if (w == 1) {
            s_zn_def_r += (float)d;
            if (s_zn_def_r < 2.0f) s_zn_def_r = 2.0f;
            if (s_zn_def_r > 30.0f) s_zn_def_r = 30.0f;
        } else if (w == 2) {
            s_zn_def_waves += d;
            if (s_zn_def_waves < 1) s_zn_def_waves = 1;
            if (s_zn_def_waves > 9) s_zn_def_waves = 9;
        } else if (w == 5) {
            s_zn_def_cover = d > 0 ? 1 : 0;
        } else if (w == 4) {
            s_zn_def_wander = d > 0 ? 1 : 0;
        } else if (w == 3) {
            s_zn_def_trig += 10.0f * (float)d;
            if (s_zn_def_trig < 20.0f) s_zn_def_trig = 20.0f;
            if (s_zn_def_trig > 200.0f) s_zn_def_trig = 200.0f;
        }
        return;
    }
    if (!strcmp(path, "ZONE")) {                /* the zone tool: a drop makes a zone of the chosen soldier type at the point under the mouse */
        if (kind == 1) {
            float tgt[3];
            ed_ray_point(mx, my, eye, fwd, tgt);
            tgt[2] -= 0.15f;                    /* the circle sits on the ground; ed_ray_point lifts 0.15 m */
            (void)ed_zone_add(tgt, s_ed_sel >= 0 && s_ed_sel < s_ed_ncat ? s_ed_cat[s_ed_sel] : "");
        } else {
            recomp_osd_show("Editor: drag the zone tool into the world (first click a soldier type in Characters)");
        }
        return;
    }
    if (kind == 4) {
        if (!s_ed_apply) {
            recomp_osd_show("Editor: the edits of this level are OFF (press the Edits button first)");
        } else if (s_ed_npl == 0) {
            recomp_osd_show("Editor: nothing placed to replay");
        } else {
            snprintf(msg, sizeof msg, "Editor: placing the %d saved soldiers again", s_ed_npl);
            recomp_osd_show(msg);
            ed_replay_now();
        }
        return;
    }
    if (ed_prop_find(path) >= 0 || ed_prop_global(path) >= 0) {     /* a pickup kind (Props tab), of this level or seen in another */
        char nm[40];
        const char *b = strrchr(path, '\\');
        size_t l;
        b = b ? b + 1 : path;
        strncpy(nm, b, sizeof nm - 1);
        nm[sizeof nm - 1] = 0;
        l = strlen(nm);
        if (l > 4 && !_stricmp(nm + l - 4, ".xdd"))
            nm[l - 4] = 0;
        if (kind == 2) {
            strcpy(s_ed_selprop, path);
            snprintf(msg, sizeof msg, "Editor prop: %s (drag it into the world)", nm);
            recomp_osd_show(msg);
        } else if (kind == 1) {
            float tgt[3];
            ed_ray_point(mx, my, eye, fwd, tgt);
            tgt[2] -= 0.1f;                     /* ed_ray_point lifts a soldier 0.15 m; a pickup stands 0.05 m above what the mouse touches */
            if (s_ed_npp >= ED_NPP)
                snprintf(msg, sizeof msg, "Editor: the limit of %d props is reached (Clear removes the saved ones)", ED_NPP);
            else if (ed_place_prop(path, tgt, 1))
                snprintf(msg, sizeof msg, "Editor: %s placed (%d of %d props)", nm, s_ed_npp, ED_NPP);
            else if (ed_prop_find(path) < 0)
                snprintf(msg, sizeof msg, "Editor: %s needs data this level does not have (its weapon is made by another level only)", nm);
            else
                snprintf(msg, sizeof msg, "Editor: could not make %s here", nm);
            recomp_osd_show(msg);
        }
        return;
    }
    ci = ed_cat_find(path);
    if (ci < 0 && !ed_anytype()) {
        snprintf(msg, sizeof msg, "Editor: %s is not loaded in this level yet (walk until its soldiers appear)", ed_base(path));
        recomp_osd_show(msg);
        return;
    }
    if (kind == 1) {
        float tgt[3];
        uint32_t s;
        ed_ray_point(mx, my, eye, fwd, tgt);
        s = ed_spawn(0, tgt, path, 1, 1);
        if (s) {
            s_ed_last_sol = s;
            snprintf(msg, sizeof msg, "Editor: %s placed (%d of %d placed)", ed_base(path), ed_shown(), ed_cap());
        } else {
            snprintf(msg, sizeof msg, "Editor: could not make %s here (%d spawners known)", ed_base(path), s_ed_nsp);
        }
        recomp_osd_show(msg);
    } else if (kind == 2) {
        if (ci >= 0) {
            s_ed_sel = ci;
            s_ed_selprop[0] = 0;
            snprintf(msg, sizeof msg, "Editor type: %s (NUMPAD2 places it, or drag it into the world)", ed_base(path));
            recomp_osd_show(msg);
        }
    }
}
int game_editor_window_up(void)
{
    return s_ui_show != 0;
}
int game_editor_look_ok(void)
{
    return !s_ui_show || s_ui_look;             /* the window is not up: the mouse turns the camera as always */
}
static int ui_world_point(POINT p, float *mx, float *my);
static void ed_mouse_dir(float mx, float my, const float *fwd, float *d);
/* The right button: a short click (up again within 200 ms) removes the soldier or prop under the pointer; held longer it turns the camera (the
 * pointer is pinned where the press began). The look starts only after the 200 ms, so a click never jerks the view. */
static void ed_look_update(int vis, const float *eye, const float *fwd)
{
    static int pend;
    static ULONGLONG t0;
    static POINT pc;
    int down = vis && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    if (down && !s_ui_look && !pend) {
        GetCursorPos(&pc);
        if (pc.x >= s_ui_rc[0] && pc.x < s_ui_rc[2] && pc.y >= s_ui_rc[1] && pc.y < s_ui_rc[3])
            return;                             /* pressed on the window itself: not a look, not a delete */
        pend = 1;
        t0 = GetTickCount64();
    } else if (down && pend && !s_ui_look && GetTickCount64() - t0 >= 200u) {
        RECT r;
        s_ui_look = 1;
        r.left = pc.x; r.top = pc.y; r.right = pc.x + 1; r.bottom = pc.y + 1;
        ClipCursor(&r);                         /* the pointer stays where the look started */
    } else if (!down && pend) {
        pend = 0;
        if (s_ui_look) {
            s_ui_look = 0;
            ClipCursor(NULL);
        } else if (eye && fwd) {                /* a click: delete under the pointer */
            float mx, my, d[3];
            if (ui_world_point(pc, &mx, &my)) {
                ed_mouse_dir(mx, my, fwd, d);
                ed_delete_aimed(eye, d, 0.993f);
            }
        }
    } else if (!down && s_ui_look) {
        s_ui_look = 0;
        ClipCursor(NULL);
    }
}

/* ---------- the window (its own thread) ---------- */
typedef struct { int kind; int item; char text[100]; unsigned key; } UiRow;     /* kind 0 group, 1 folder, 2 item */
typedef struct { int idx; int group; char folder[40]; char name[64]; } UiSort;
static volatile LONG s_ui_cnt[13];               /* placed, limit: written by the game thread each frame while the window is up */
static ULONGLONG s_u_clr_arm;                   /* tick the Clear button was first clicked (a second click within 3 s does it) */
static LONG s_u_cnt_seen[13] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static HWND s_uw, s_ugh, s_ush, s_urw;
static int s_u_tab, s_u_scroll, s_u_drag = -1, s_u_dragging, s_u_ghost_ok, s_u_dragteam;
static COLORREF s_u_sil_col;
static POINT s_u_dstart;
static float s_u_s = 1.0f;
static HFONT s_u_font, s_u_fontb;
static int s_u_font_h;
static EdUiItem s_u_items[ED_NUI];
static int s_u_n;
static char s_u_sel[ED_PATH];
static LONG s_u_ver = -1;
static UiRow s_u_rows[ED_NUI + 64];
static int s_u_nrows;
static unsigned s_u_coll[64];
static int s_u_ncoll;
static char s_u_ghost_text[100];
static char s_u_dragpath[ED_PATH];

static unsigned ui_hash(const char *a, const char *b)
{
    unsigned h = 5381u;
    for (; *a; a++)
        h = h * 33u + (unsigned char)*a;
    h = h * 33u + 1u;
    for (; b && *b; b++)
        h = h * 33u + (unsigned char)*b;
    return h;
}
static int ui_is_coll(unsigned k)
{
    int i;
    for (i = 0; i < s_u_ncoll; i++)
        if (s_u_coll[i] == k)
            return 1;
    return 0;
}
static void ui_toggle_coll(unsigned k)
{
    int i;
    for (i = 0; i < s_u_ncoll; i++)
        if (s_u_coll[i] == k) {
            s_u_coll[i] = s_u_coll[--s_u_ncoll];
            return;
        }
    if (s_u_ncoll < 64)
        s_u_coll[s_u_ncoll++] = k;
}
static void ui_split(const char *path, int *group, char *folder, size_t fn, char *name, size_t nn)
{
    const char *p, *q, *b;
    size_t l;
    for (p = path; *p; p++)
        if (!_strnicmp(p, "\\characters\\", 12))
            break;
    strcpy(folder, "Other");
    *group = 2;
    if (*p) {
        p += 12;
        q = strchr(p, '\\');
        if (q) {
            l = (size_t)(q - p);
            if (l >= fn)
                l = fn - 1;
            memcpy(folder, p, l);
            folder[l] = 0;
            *group = !_strnicmp(folder, "German", 6) ? 0 : 1;
        }
    }
    b = strrchr(path, '\\');
    b = b ? b + 1 : path;
    strncpy(name, b, nn - 1);
    name[nn - 1] = 0;
    l = strlen(name);
    if (l > 4 && !_stricmp(name + l - 4, ".xdd"))
        name[l - 4] = 0;
}
static int ui_cmp(const void *a, const void *b)
{
    const UiSort *x = (const UiSort *)a, *y = (const UiSort *)b;
    int c = x->group - y->group;
    if (!c)
        c = _stricmp(x->folder, y->folder);
    if (!c)
        c = _stricmp(x->name, y->name);
    return c;
}
static void ui_rebuild(void)
{
    static UiSort k[ED_NUI];
    static const char *k_gname[3] = { "Germans", "Allies", "Other" };
    int i, g, lastg = -1, nk = 0, props = s_u_tab == 1;
    char lastf[40] = "";
    s_u_nrows = 0;
    if (s_u_tab == 2) {                         /* Zones: the tool to drag, then the settings (click - / + at the right end of a row) */
        UiRow *r;
        int tool = -1, k;
        for (i = 0; i < s_u_n; i++)
            if (s_u_items[i].team == 101)
                tool = i;
        r = &s_u_rows[s_u_nrows++];
        r->kind = 0;
        r->item = -1;
        r->key = ui_hash("Zone tool", NULL);
        snprintf(r->text, sizeof r->text, "%s", "Spawn zone");
        if (tool >= 0) {
            r = &s_u_rows[s_u_nrows++];
            r->kind = 2;
            r->item = tool;
            r->key = 0;
            snprintf(r->text, sizeof r->text, "%s", "New zone (drag into the world)");
        }
        for (k = 0; k < 7; k++) {
            if (k == 5 && !getenv("RECOMP_ZONE_COVER"))
                continue;                       /* the cover row is parked (the combat AI ignores the spot) */
            r = &s_u_rows[s_u_nrows++];
            r->kind = 3;
            r->item = -1;
            r->key = (unsigned)k;
            r->text[0] = 0;
        }
        s_u_scroll = 0;
        return;
    }
    for (i = 0; i < s_u_n; i++) {
        if (s_u_items[i].team == 101)
            continue;
        if ((s_u_items[i].team == 100) != props)        /* the Props tab lists the pickup kinds (team 100), the Characters tab the soldiers */
            continue;
        k[nk].idx = i;
        ui_split(s_u_items[i].path, &k[nk].group, k[nk].folder, sizeof k[nk].folder, k[nk].name, sizeof k[nk].name);
        if (props) {
            k[nk].group = 0;
            strcpy(k[nk].folder, "-");
        }
        nk++;
    }
    qsort(k, (size_t)nk, sizeof k[0], ui_cmp);
    for (i = 0; i < nk && s_u_nrows < ED_NUI + 60; i++) {
        UiRow *r;
        g = k[i].group;
        if (g != lastg) {
            r = &s_u_rows[s_u_nrows++];
            r->kind = 0;
            r->item = -1;
            r->key = ui_hash(props ? "Pickups" : k_gname[g], NULL);
            snprintf(r->text, sizeof r->text, "%s", props ? "Pickups" : k_gname[g]);
            lastg = g;
            lastf[0] = 0;
        }
        if (ui_is_coll(ui_hash(props ? "Pickups" : k_gname[g], NULL)))
            continue;
        if (!props && _stricmp(lastf, k[i].folder)) {
            r = &s_u_rows[s_u_nrows++];
            r->kind = 1;
            r->item = -1;
            r->key = ui_hash(k_gname[g], k[i].folder);
            snprintf(r->text, sizeof r->text, "%s", k[i].folder);
            strcpy(lastf, k[i].folder);
        }
        if (ui_is_coll(ui_hash(k_gname[g], k[i].folder)))
            continue;
        if (s_u_nrows >= ED_NUI + 62)
            break;
        r = &s_u_rows[s_u_nrows++];
        r->kind = 2;
        r->item = k[i].idx;
        r->key = 0;
        snprintf(r->text, sizeof r->text, "%s", k[i].name);
    }
    if (s_u_scroll > s_u_nrows)
        s_u_scroll = 0;
}
static void ui_sync(void)
{
    if (s_u_ver == s_ui_ver)
        return;
    EnterCriticalSection(&s_ui_cs);
    s_u_n = s_ui_n;
    memcpy(s_u_items, s_ui_items, (size_t)s_ui_n * sizeof s_ui_items[0]);
    strcpy(s_u_sel, s_ui_selpath);
    s_u_ver = s_ui_ver;
    LeaveCriticalSection(&s_ui_cs);
    ui_rebuild();
}
static int ui_game_rect(RECT *tl_cl, POINT *tl)
{
    HWND game = FindWindowA("XboxRecompFramebuffer", NULL);
    POINT o = { 0, 0 };
    if (!game || !IsWindowVisible(game) || IsIconic(game))
        return 0;
    GetClientRect(game, tl_cl);
    ClientToScreen(game, &o);
    *tl = o;
    return tl_cl->right > 0 && tl_cl->bottom > 0;
}
static void ui_post(int kind, const char *path, float mx, float my)
{
    EnterCriticalSection(&s_ui_cs);
    strncpy(s_ui_req_path, path, ED_PATH - 1);
    s_ui_req_path[ED_PATH - 1] = 0;
    s_ui_req_mx = mx;
    s_ui_req_my = my;
    s_ui_req = kind;
    LeaveCriticalSection(&s_ui_cs);
}
typedef struct { RECT tab[3], list, stat, btn[3], foot; int rowh; } UiLay;   /* btn: Clear, Replay, Edits on/off (leftmost, btn[2]) */
static void ui_layout(HWND hw, UiLay *L)
{
    RECT c;
    float s = s_u_s;
    int pad = (int)(8 * s), hh = (int)(40 * s), th = (int)(32 * s), fh = (int)(58 * s), sh = (int)(30 * s), bw = (int)(78 * s);
    GetClientRect(hw, &c);
    {
        int i, tw = (c.right - 2 * pad) / 3;
        for (i = 0; i < 3; i++) {
            L->tab[i].left = pad + i * tw;
            L->tab[i].right = i == 2 ? c.right - pad : pad + (i + 1) * tw;
            L->tab[i].top = hh + pad / 2;
            L->tab[i].bottom = hh + pad / 2 + th;
        }
    }
    L->list.left = pad;
    L->list.right = c.right - pad;
    L->list.top = L->tab[0].bottom + pad;
    L->foot.left = pad;
    L->foot.right = c.right - pad;
    L->foot.top = c.bottom - fh;
    L->foot.bottom = c.bottom - pad;
    L->stat.left = pad;                         /* the counter row, with the two buttons at its right end */
    L->stat.right = c.right - pad;
    L->stat.bottom = L->foot.top - pad / 2;
    L->stat.top = L->stat.bottom - sh;
    L->btn[1] = L->btn[0] = L->stat;
    L->btn[1].left = L->stat.right - bw;
    L->btn[0].right = L->btn[1].left - pad / 2;
    L->btn[0].left = L->btn[0].right - bw;
    L->btn[2] = L->stat;
    L->btn[2].right = L->btn[0].left - pad / 2;
    L->btn[2].left = L->btn[2].right - (int)(bw * 1.3f);
    L->list.bottom = L->stat.top - pad / 2;
    L->rowh = (int)(26 * s);
}
static void ui_fonts(void)
{
    int h = (int)(18 * s_u_s);
    if (s_u_font && s_u_font_h == h)
        return;
    if (s_u_font) {
        DeleteObject(s_u_font);
        DeleteObject(s_u_fontb);
    }
    s_u_font = CreateFontA(-h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    s_u_fontb = CreateFontA(-h, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    s_u_font_h = h;
}
static void ui_text(HDC dc, const char *t, RECT r, COLORREF col, UINT fmt)
{
    SetTextColor(dc, col);
    DrawTextA(dc, t, -1, &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}
static void ui_paint(HWND hw)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hw, &ps), md;
    HBITMAP bm, old;
    HBRUSH br;
    RECT c, r;
    UiLay L;
    int i, y, vis;
    float s = s_u_s;
    GetClientRect(hw, &c);
    md = CreateCompatibleDC(dc);
    bm = CreateCompatibleBitmap(dc, c.right, c.bottom);
    old = (HBITMAP)SelectObject(md, bm);
    ui_fonts();
    ui_layout(hw, &L);
    SelectObject(md, s_u_font);
    SetBkMode(md, TRANSPARENT);
    br = CreateSolidBrush(RGB(22, 24, 30));
    FillRect(md, &c, br);
    DeleteObject(br);
    r = c;
    r.bottom = (int)(40 * s);                   /* the red bar, like the F1 window */
    br = CreateSolidBrush(RGB(150, 25, 25));
    FillRect(md, &r, br);
    DeleteObject(br);
    r.left = (int)(12 * s);
    SelectObject(md, s_u_fontb);
    ui_text(md, "MAP EDITOR", r, RGB(255, 255, 255), DT_LEFT);
    SelectObject(md, s_u_font);
    r.right -= (int)(10 * s);
    ui_text(md, "Numpad0 hides", r, RGB(240, 200, 200), DT_RIGHT);
    for (i = 0; i < 3; i++) {                   /* the tabs */
        br = CreateSolidBrush(i == s_u_tab ? RGB(60, 64, 78) : RGB(34, 36, 44));
        FillRect(md, &L.tab[i], br);
        DeleteObject(br);
        ui_text(md, i == 0 ? "Characters" : i == 1 ? "Props" : "Zones", L.tab[i], i == s_u_tab ? RGB(255, 255, 255) : RGB(150, 150, 160), DT_CENTER);
    }
    br = CreateSolidBrush(RGB(16, 17, 22));
    FillRect(md, &L.list, br);
    DeleteObject(br);
    {
        vis = (L.list.bottom - L.list.top) / L.rowh;
        y = L.list.top;
        if (s_u_nrows == 0) {
            r = L.list;
            r.top += (int)(10 * s);
            r.left += (int)(10 * s);
            r.right -= (int)(10 * s);
            r.bottom = r.top + (int)(90 * s);
            SetTextColor(md, RGB(150, 150, 160));
            DrawTextA(md, s_u_tab == 1 ? "No pickups yet.\nOnly kinds this level already has (a medkit, an ammo box...) can be placed." : "No soldier types yet.\nWalk around until soldiers appear.", -1, &r,
                      DT_CENTER | DT_WORDBREAK);
        }
        for (i = s_u_scroll; i < s_u_nrows && i < s_u_scroll + vis; i++, y += L.rowh) {
            const UiRow *w = &s_u_rows[i];
            RECT rr;
            rr.left = L.list.left;
            rr.right = L.list.right;
            rr.top = y;
            rr.bottom = y + L.rowh;
            if (w->kind == 2) {
                const EdUiItem *it = &s_u_items[w->item];
                int sel = s_u_sel[0] && !_stricmp(s_u_sel, it->path), dot = (int)(5 * s);
                COLORREF col = it->loaded ? RGB(235, 235, 240) : RGB(105, 105, 115);
                if (sel || (i == s_u_drag && s_u_dragging)) {
                    br = CreateSolidBrush(RGB(45, 90, 160));
                    FillRect(md, &rr, br);
                    DeleteObject(br);
                }
                br = CreateSolidBrush(it->team == 100 ? RGB(225, 190, 70) : it->team == 2 ? RGB(205, 60, 60) : it->team == 1 ? RGB(70, 140, 230) : RGB(90, 90, 100));
                {
                    RECT d = { rr.left + (int)(26 * s) - dot, rr.top + L.rowh / 2 - dot, rr.left + (int)(26 * s) + dot, rr.top + L.rowh / 2 + dot };
                    FillRect(md, &d, br);
                }
                DeleteObject(br);
                rr.left += (int)(40 * s);
                ui_text(md, w->text, rr, col, DT_LEFT);
            } else if (w->kind == 3) {
                static const char *k_nm[6] = { "Soldiers per wave", "Radius (m)", "Waves", "Wake distance (m)", "Wander in the circle", "Seek cover" };
                char t[120];
                int bw = (int)(30 * s), k = (int)w->key;
                rr.left += (int)(10 * s);
                if (k < 6) {
                    RECT mn = rr, pl = rr;
                    if (k == 4 || k == 5)
                        snprintf(t, sizeof t, "%s: %s", k_nm[k], s_ui_cnt[6 + k] ? "On" : "Off");
                    else
                        snprintf(t, sizeof t, "%s: %ld", k_nm[k], (long)s_ui_cnt[6 + k]);
                    pl.left = rr.right - bw;
                    mn.right = pl.left - (int)(4 * s);
                    mn.left = mn.right - bw;
                    mn.top += (int)(2 * s); mn.bottom -= (int)(2 * s); pl.top = mn.top; pl.bottom = mn.bottom;
                    rr.right = mn.left;
                    ui_text(md, t, rr, RGB(235, 235, 240), DT_LEFT);
                    br = CreateSolidBrush(RGB(60, 64, 78));
                    FillRect(md, &mn, br);
                    FillRect(md, &pl, br);
                    DeleteObject(br);
                    SelectObject(md, s_u_fontb);
                    ui_text(md, "-", mn, RGB(255, 255, 255), DT_CENTER);
                    ui_text(md, "+", pl, RGB(255, 255, 255), DT_CENTER);
                    SelectObject(md, s_u_font);
                } else {
                    const char *b = strrchr(s_u_sel, '\\');
                    b = b ? b + 1 : s_u_sel;
                    snprintf(t, sizeof t, "Soldier type: %s", s_u_sel[0] ? b : "(click one in Characters)");
                    ui_text(md, t, rr, RGB(160, 200, 255), DT_LEFT);
                }
            } else {
                int coll = ui_is_coll(w->key);
                char t[120];
                rr.left += (int)((w->kind == 0 ? 6 : 18) * s);
                snprintf(t, sizeof t, "%s %s", coll ? "+" : "-", w->text);
                SelectObject(md, s_u_fontb);
                ui_text(md, t, rr, w->kind == 0 ? RGB(235, 190, 90) : RGB(170, 175, 190), DT_LEFT);
                SelectObject(md, s_u_font);
            }
        }
        if (s_u_nrows > vis) {                  /* a thin scroll mark */
            int th = (L.list.bottom - L.list.top), bh = th * vis / s_u_nrows, by = L.list.top + (th - bh) * s_u_scroll / (s_u_nrows - vis > 0 ? s_u_nrows - vis : 1);
            RECT sb = { L.list.right - (int)(4 * s), by, L.list.right, by + bh };
            br = CreateSolidBrush(RGB(90, 94, 110));
            FillRect(md, &sb, br);
            DeleteObject(br);
        }
    }
    {   /* the counter: how many soldiers are placed, how many more fit */
        char t[80];
        LONG n = s_ui_cnt[0], cap = s_ui_cnt[1];
        int armed = s_u_clr_arm && GetTickCount64() - s_u_clr_arm < 3000;
        br = CreateSolidBrush(RGB(28, 30, 38));
        FillRect(md, &L.stat, br);
        DeleteObject(br);
        r = L.stat;
        r.left += (int)(8 * s);
        r.right = L.btn[2].left;
        if (s_u_tab == 2) {
            snprintf(t, sizeof t, "Zones saved %ld / %ld", (long)s_ui_cnt[4], (long)s_ui_cnt[5]);
            n = s_ui_cnt[4];
            cap = s_ui_cnt[5];
        } else if (s_u_tab == 1) {
            snprintf(t, sizeof t, "Props saved %ld / %ld", (long)s_ui_cnt[2], (long)s_ui_cnt[3]);
            n = s_ui_cnt[2];
            cap = s_ui_cnt[3];
        } else {
            snprintf(t, sizeof t, "Placed %ld / %ld  (%ld free)", (long)n, (long)cap, (long)(cap - n > 0 ? cap - n : 0));
        }
        SelectObject(md, s_u_fontb);
        ui_text(md, t, r, n >= cap ? RGB(240, 160, 70) : RGB(120, 220, 140), DT_LEFT);
        SelectObject(md, s_u_font);
        for (i = 0; i < 2; i++) {
            br = CreateSolidBrush(i == 0 ? (armed ? RGB(190, 40, 40) : RGB(95, 45, 45)) : RGB(45, 70, 105));
            FillRect(md, &L.btn[i], br);
            DeleteObject(br);
            ui_text(md, i == 0 ? (armed ? "Sure?" : "Clear") : "Replay", L.btn[i], RGB(240, 240, 245), DT_CENTER);
        }
        br = CreateSolidBrush(s_ui_cnt[12] ? RGB(40, 105, 60) : RGB(120, 85, 35));       /* this level's edits on / off */
        FillRect(md, &L.btn[2], br);
        DeleteObject(br);
        ui_text(md, s_ui_cnt[12] ? "Edits: On" : "Edits: Off", L.btn[2], RGB(240, 240, 245), DT_CENTER);
    }
    r = L.foot;
    SetTextColor(md, RGB(150, 155, 170));
    DrawTextA(md, s_u_tab == 2 ? "Click a soldier type in Characters, set the numbers, then drag the zone into the world. Soldiers are born inside the circle when you come near. Right click a circle to remove it."
                               : "Drag a soldier into the world. Click selects. Hold the right mouse button to look. Right click a soldier or prop to delete it.", -1, &r, DT_LEFT | DT_WORDBREAK);
    BitBlt(dc, 0, 0, c.right, c.bottom, md, 0, 0, SRCCOPY);
    SelectObject(md, old);
    DeleteObject(bm);
    DeleteDC(md);
    EndPaint(hw, &ps);
}
static int ui_row_at(HWND hw, int x, int y)
{
    UiLay L;
    int i;
    ui_layout(hw, &L);
    if (x < L.list.left || x >= L.list.right || y < L.list.top || y >= L.list.bottom)
        return -1;
    i = s_u_scroll + (y - L.list.top) / L.rowh;
    return i < s_u_nrows ? i : -1;
}
static int ui_in_panel(POINT p)
{
    return p.x >= s_ui_rc[0] && p.x < s_ui_rc[2] && p.y >= s_ui_rc[1] && p.y < s_ui_rc[3];
}
/* the translucent preview over the world: a silhouette (soldier = body + head, prop = box) standing on a ground ellipse, feet at the pointer, sized by
 * the depth the game thread measured for the point under the mouse. It is a stand-in shape, not the real model. */
static void ui_sil(int on, POINT p)
{
    RECT cl;
    POINT tl;
    float mx, my, dep, sv, sh, bw, bh, hd, ew, eh;
    int w, h, ex, ey, bx, by, zone = s_u_dragteam == 101, soldier = s_u_dragteam != 100 && !zone;
    HRGN rg, part;
    if (!s_ush)
        return;
    if (!on || !ui_game_rect(&cl, &tl) || ui_in_panel(p) || p.x < tl.x || p.y < tl.y || p.x >= tl.x + cl.right || p.y >= tl.y + cl.bottom) {
        s_ui_pv_on = 0;
        ShowWindow(s_ush, SW_HIDE);
        return;
    }
    mx = (float)(p.x - tl.x) / (float)cl.right;
    my = (float)(p.y - tl.y) / (float)cl.bottom;
    s_ui_pv_mx = mx;
    s_ui_pv_my = my;
    s_ui_pv_on = 1;
    if (!s_ui_pv_seq)
        return;                                 /* the game thread has not answered yet */
    dep = s_ui_pv_depth;
    if (!(dep > 0.3f))
        dep = 0.3f;
    sv = 0.5f * (float)cl.bottom * s_ui_pv_py / dep;      /* pixels per metre, up */
    sh = 0.5f * (float)cl.right * s_ui_pv_px / dep;       /* pixels per metre, across */
    bw = soldier ? 0.55f : 0.5f;
    bh = soldier ? 1.45f : 0.35f;
    hd = soldier ? 0.26f : 0.0f;
    ew = 1.1f;
    eh = 0.45f;
    if (zone) {                                 /* the zone tool: just the circle on the ground, as wide as the zone will be */
        bw = bh = hd = 0.0f;
        ew = 2.0f * (float)s_ui_cnt[7];
        eh = ew * 0.4f;
    }
    w = (int)(ew * sh) + 2;
    eh = eh * sh * 0.5f;                        /* the ground ellipse is seen at a slant: half as tall as wide */
    h = (int)((bh + hd) * sv + eh) + 3 + (zone ? (int)eh : 0);
    if (w < 10) w = 10;
    if (w > 1400) w = 1400;
    if (h < 14) h = 14;
    if (h > 1400) h = 1400;
    ex = 0;
    ey = h - (int)eh * 2 - 1;
    if (ey < 0) ey = 0;
    bw = bw * sh;
    bx = (w - (int)bw) / 2;
    by = (int)(hd * sv);
    rg = CreateEllipticRgn(ex, ey, w, h);
    if (!zone) {
        part = CreateRoundRectRgn(bx, by, bx + (int)bw, h - (int)eh, (int)(bw * 0.4f), (int)(bw * 0.4f));
        CombineRgn(rg, rg, part, RGN_OR);
        DeleteObject(part);
    }
    if (soldier) {
        int hw = (int)(hd * sh);
        part = CreateEllipticRgn((w - hw) / 2, 0, (w + hw) / 2, hw > by ? hw : by);
        CombineRgn(rg, rg, part, RGN_OR);
        DeleteObject(part);
    }
    s_u_sil_col = zone ? RGB(160, 110, 230) : s_u_dragteam == 100 ? RGB(235, 180, 40) : s_u_dragteam == 1 ? RGB(70, 140, 235) : RGB(70, 210, 110);
    if (!s_ui_pv_hit)
        s_u_sil_col = RGB(200, 110, 60);        /* no world under the pointer: the preview shows a 15 m guess */
    SetWindowPos(s_ush, HWND_TOPMOST, p.x - w / 2, p.y - h + (int)eh, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowRgn(s_ush, rg, TRUE);              /* the window owns the region now */
    InvalidateRect(s_ush, NULL, FALSE);
}
static void ui_ghost(int on, POINT p)
{
    if (!s_ugh)
        return;
    if (!on) {
        ShowWindow(s_ugh, SW_HIDE);
        ui_sil(0, p);
        return;
    }
    s_u_ghost_ok = !ui_in_panel(p);
    SetWindowPos(s_ugh, HWND_TOPMOST, p.x + 16, p.y + 16, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_SHOWWINDOW);
    InvalidateRect(s_ugh, NULL, FALSE);
    ui_sil(1, p);
}
/* the drop / click on the world: normalised position inside the game window, or -1 when the point is not over it */
static int ui_world_point(POINT p, float *mx, float *my)
{
    RECT cl;
    POINT tl;
    if (!ui_game_rect(&cl, &tl) || ui_in_panel(p))
        return 0;
    if (p.x < tl.x || p.y < tl.y || p.x >= tl.x + cl.right || p.y >= tl.y + cl.bottom)
        return 0;
    *mx = (float)(p.x - tl.x) / (float)cl.right;
    *my = (float)(p.y - tl.y) / (float)cl.bottom;
    if (s_ed_dbg)
        fprintf(stderr, "[EDITOR] drop: cursor %ld %ld, game client %ld x %ld at %ld %ld -> mouse %.3f %.3f\n", (long)p.x, (long)p.y, (long)cl.right, (long)cl.bottom,
                (long)tl.x, (long)tl.y, (double)*mx, (double)*my);
    return 1;
}
static void ui_timer(HWND hw)
{
    RECT cl, cur;
    POINT tl = { 0, 0 };
    DWORD pid = 0;
    int show = s_ui_show != 0;
    LONG want[4];
    HWND fg = GetForegroundWindow();
    if (fg)
        GetWindowThreadProcessId(fg, &pid);
    if (!show || pid != GetCurrentProcessId() || !ui_game_rect(&cl, &tl)) {
        if (s_urw)
            ShowWindow(s_urw, SW_HIDE);
        if (IsWindowVisible(hw))
            ShowWindow(hw, SW_HIDE);
        s_ui_rc[0] = s_ui_rc[1] = s_ui_rc[2] = s_ui_rc[3] = 0;
        if (s_u_drag >= 0) {
            s_u_drag = -1;
            s_u_dragging = 0;
            ui_ghost(0, tl);
        }
        return;
    }
    if (s_u_drag >= 0 && s_u_dragging) {        /* the depth answer changes every game frame even when the mouse is still */
        POINT cp;
        GetCursorPos(&cp);
        ui_ghost(1, cp);
    }
    s_u_s = (float)cl.bottom / 1080.0f;
    if (s_u_s < 0.75f) s_u_s = 0.75f;
    if (s_u_s > 2.2f) s_u_s = 2.2f;
    want[0] = tl.x + (LONG)(12 * s_u_s);
    want[1] = tl.y + (LONG)(12 * s_u_s);
    want[2] = want[0] + (LONG)(330 * s_u_s);
    want[3] = tl.y + cl.bottom - (LONG)(12 * s_u_s);
    GetWindowRect(hw, &cur);
    if (cur.left != want[0] || cur.top != want[1] || cur.right != want[2] || cur.bottom != want[3] || !IsWindowVisible(hw)) {
        SetWindowPos(hw, HWND_TOPMOST, want[0], want[1], want[2] - want[0], want[3] - want[1], SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(hw, NULL, FALSE);
    }
    s_ui_rc[0] = want[0]; s_ui_rc[1] = want[1]; s_ui_rc[2] = want[2]; s_ui_rc[3] = want[3];
    if (s_urw) {
        if (s_ui_nzn > 0) {                     /* the zone circles: a window over the game, just below the panel in the z-order */
            SetWindowPos(s_urw, hw, tl.x, tl.y, cl.right, cl.bottom, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(s_urw, NULL, FALSE);
        } else if (IsWindowVisible(s_urw)) {
            ShowWindow(s_urw, SW_HIDE);
        }
    }
    if (s_u_ver != s_ui_ver) {
        ui_sync();
        InvalidateRect(hw, NULL, FALSE);
    }
    {
        static int armed_seen;
        LONG n = s_ui_cnt[0], cap = s_ui_cnt[1];
        int armed = s_u_clr_arm && GetTickCount64() - s_u_clr_arm < 3000;
        int ch = 0, q;
        for (q = 2; q < 13; q++)
            if (s_ui_cnt[q] != s_u_cnt_seen[q])
                ch = 1;
        if (n != s_u_cnt_seen[0] || cap != s_u_cnt_seen[1] || ch || armed != armed_seen) {
            s_u_cnt_seen[0] = n;
            s_u_cnt_seen[1] = cap;
            for (q = 2; q < 13; q++)
                s_u_cnt_seen[q] = s_ui_cnt[q];
            armed_seen = armed;
            InvalidateRect(hw, NULL, FALSE);
        }
    }
}
static LRESULT CALLBACK ui_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    switch (msg) {
    case WM_PAINT:
        ui_paint(hw);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        ui_timer(hw);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;                   /* the game keeps the keyboard (WASD keeps flying) */
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, s_u_dragging ? IDC_SIZEALL : IDC_ARROW));
        return TRUE;
    case WM_LBUTTONDOWN: {
        UiLay L;
        int row;
        ui_layout(hw, &L);
        if (x >= L.btn[0].left && x < L.btn[0].right && y >= L.btn[0].top && y < L.btn[0].bottom) {      /* Clear: the second click within 3 s does it */
            ULONGLONG now = GetTickCount64();
            if (s_u_clr_arm && now - s_u_clr_arm < 3000) {
                s_u_clr_arm = 0;
                ui_post(3, "", 0.5f, 0.5f);
            } else {
                s_u_clr_arm = now;
            }
            InvalidateRect(hw, NULL, FALSE);
            return 0;
        }
        if (x >= L.btn[1].left && x < L.btn[1].right && y >= L.btn[1].top && y < L.btn[1].bottom) {      /* Replay: the saved list again from the start */
            s_u_clr_arm = 0;
            ui_post(4, "", 0.5f, 0.5f);
            InvalidateRect(hw, NULL, FALSE);
            return 0;
        }
        if (x >= L.btn[2].left && x < L.btn[2].right && y >= L.btn[2].top && y < L.btn[2].bottom) {      /* Edits On / Off for this level */
            s_u_clr_arm = 0;
            ui_post(7, "", 0.5f, 0.5f);
            InvalidateRect(hw, NULL, FALSE);
            return 0;
        }
        if (x >= L.tab[0].left && x < L.tab[0].right && y >= L.tab[0].top && y < L.tab[0].bottom) {
            s_u_tab = 0;
            s_u_scroll = 0;
            ui_rebuild();
            InvalidateRect(hw, NULL, FALSE);
            return 0;
        }
        if (x >= L.tab[1].left && x < L.tab[1].right && y >= L.tab[1].top && y < L.tab[1].bottom) {
            s_u_tab = 1;
            s_u_scroll = 0;
            ui_rebuild();
            InvalidateRect(hw, NULL, FALSE);
            return 0;
        }
        if (x >= L.tab[2].left && x < L.tab[2].right && y >= L.tab[2].top && y < L.tab[2].bottom) {
            s_u_tab = 2;
            s_u_scroll = 0;
            ui_rebuild();
            InvalidateRect(hw, NULL, FALSE);
            return 0;
        }
        row = ui_row_at(hw, x, y);
        if (row < 0)
            return 0;
        if (s_u_rows[row].kind == 3) {          /* a zone setting: the - or + box at the right end */
            int bw = (int)(30 * s_u_s);
            if (s_u_rows[row].key < 6) {
                if (x >= L.list.right - bw)
                    ui_post(6, "", (float)s_u_rows[row].key, 1.0f);
                else if (x >= L.list.right - 2 * bw - (int)(4 * s_u_s))
                    ui_post(6, "", (float)s_u_rows[row].key, -1.0f);
            }
            return 0;
        }
        if (s_u_rows[row].kind != 2) {          /* a group / folder line folds or unfolds */
            ui_toggle_coll(s_u_rows[row].key);
            ui_rebuild();
            InvalidateRect(hw, NULL, FALSE);
            return 0;
        }
        s_u_drag = row;
        s_u_dragging = 0;
        s_u_dragteam = s_u_items[s_u_rows[row].item].team;
        s_ui_pv_seq = 0;                        /* no stale depth from the last drag */
        strcpy(s_u_dragpath, s_u_items[s_u_rows[row].item].path);   /* the type, not the row: the list can be rebuilt while the mouse is down */
        s_u_dstart.x = x;
        s_u_dstart.y = y;
        snprintf(s_u_ghost_text, sizeof s_u_ghost_text, "%s", s_u_rows[row].text);
        SetCapture(hw);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        s_u_drag = -1;                          /* no placing by double click any more: a type is chosen with one click and dragged into the world */
        return 0;
    }
    case WM_MOUSEMOVE:
        if (s_u_drag >= 0) {
            POINT p;
            GetCursorPos(&p);
            if (!s_u_dragging && (abs(x - s_u_dstart.x) > 6 || abs(y - s_u_dstart.y) > 6)) {
                s_u_dragging = 1;
                InvalidateRect(hw, NULL, FALSE);
            }
            if (s_u_dragging)
                ui_ghost(1, p);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (s_u_drag >= 0) {
            int row = s_u_drag, was = s_u_dragging;
            POINT p;
            s_u_drag = -1;
            s_u_dragging = 0;
            if (msg == WM_LBUTTONUP)
                ReleaseCapture();
            GetCursorPos(&p);
            ui_ghost(0, p);
            (void)row;
            if (msg == WM_LBUTTONUP && s_u_dragpath[0]) {
                const char *path = s_u_dragpath;
                float mx, my;
                if (was) {
                    if (ui_world_point(p, &mx, &my))
                        ui_post(1, path, mx, my);               /* dropped on the world */
                } else {
                    ui_post(2, path, 0.5f, 0.5f);               /* a click: select the type */
                }
            }
            InvalidateRect(hw, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        UiLay L;
        int vis, d = (short)HIWORD(wp) > 0 ? -3 : 3;
        ui_layout(hw, &L);
        vis = (L.list.bottom - L.list.top) / L.rowh;
        s_u_scroll += d;
        if (s_u_scroll > s_u_nrows - vis)
            s_u_scroll = s_u_nrows - vis;
        if (s_u_scroll < 0)
            s_u_scroll = 0;
        InvalidateRect(hw, NULL, FALSE);
        return 0;
    }
    }
    return DefWindowProcA(hw, msg, wp, lp);
}
static LRESULT CALLBACK ui_ghost_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        RECT c;
        HBRUSH br = CreateSolidBrush(s_u_ghost_ok ? RGB(40, 130, 70) : RGB(90, 90, 100));
        GetClientRect(hw, &c);
        FillRect(dc, &c, br);
        DeleteObject(br);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, s_u_font ? s_u_font : GetStockObject(DEFAULT_GUI_FONT));
        c.left += 10;
        ui_text(dc, s_u_ghost_text, c, RGB(255, 255, 255), DT_LEFT);
        EndPaint(hw, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND)
        return 1;
    return DefWindowProcA(hw, msg, wp, lp);
}
static int ui_proj(const float *c, const float *P, int W, int H, POINT *o)
{
    float v0 = P[0] - c[0], v1 = P[1] - c[1], v2 = P[2] - c[2];
    float z = v0 * c[3] + v1 * c[4] + v2 * c[5], x, y;
    if (z < 0.3f)
        return 0;
    x = (v0 * c[6] + v1 * c[7] + v2 * c[8]) / z * c[12];
    y = (v0 * c[9] + v1 * c[10] + v2 * c[11]) / z * c[13];
    x = (x * 0.5f + 0.5f) * (float)W;
    y = (0.5f - y * 0.5f) * (float)H;
    if (x < -30000.0f) x = -30000.0f;
    if (x > 30000.0f) x = 30000.0f;
    if (y < -30000.0f) y = -30000.0f;
    if (y > 30000.0f) y = 30000.0f;
    o->x = (LONG)x;
    o->y = (LONG)y;
    return 1;
}
static LRESULT CALLBACK ui_ring_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps), md;
        HBITMAP bm, old;
        HBRUSH br;
        RECT c;
        float cam[14];
        UiZone zn[ED_NZN];
        int nz, zi, k;
        GetClientRect(hw, &c);
        md = CreateCompatibleDC(dc);
        bm = CreateCompatibleBitmap(dc, c.right, c.bottom);
        old = (HBITMAP)SelectObject(md, bm);
        br = CreateSolidBrush(RGB(0, 0, 0));    /* the colour key: transparent */
        FillRect(md, &c, br);
        DeleteObject(br);
        EnterCriticalSection(&s_ui_cs);
        memcpy(cam, s_ui_cam, sizeof cam);
        nz = (int)s_ui_nzn;
        if (nz > ED_NZN) nz = ED_NZN;
        memcpy(zn, s_ui_zn, sizeof zn);
        LeaveCriticalSection(&s_ui_cs);
        SetBkMode(md, TRANSPARENT);
        SelectObject(md, s_u_font ? s_u_font : GetStockObject(DEFAULT_GUI_FONT));
        for (zi = 0; zi < nz; zi++) {
            COLORREF col = zn[zi].team == 2 ? RGB(235, 80, 80) : RGB(90, 160, 255);
            HPEN pen = CreatePen(PS_SOLID, 3, col), op = (HPEN)SelectObject(md, pen);
            POINT pt, top;
            int have = 0;
            float P[3];
            for (k = 0; k <= 48; k++) {         /* the circle on the ground */
                float a = (float)k * 6.2831853f / 48.0f;
                P[0] = zn[zi].p[0] + cosf(a) * zn[zi].r;
                P[1] = zn[zi].p[1] + sinf(a) * zn[zi].r;
                P[2] = zn[zi].p[2] + 0.1f;
                if (ui_proj(cam, P, c.right, c.bottom, &pt)) {
                    if (have)
                        LineTo(md, pt.x, pt.y);
                    else
                        MoveToEx(md, pt.x, pt.y, NULL);
                    have = 1;
                } else {
                    have = 0;
                }
            }
            P[0] = zn[zi].p[0]; P[1] = zn[zi].p[1]; P[2] = zn[zi].p[2];
            if (ui_proj(cam, P, c.right, c.bottom, &pt)) {      /* a post at the centre, as tall as a soldier, with the label on top */
                P[2] += 1.8f;
                if (ui_proj(cam, P, c.right, c.bottom, &top)) {
                    MoveToEx(md, pt.x, pt.y, NULL);
                    LineTo(md, top.x, top.y);
                    SetTextColor(md, col);
                    TextOutA(md, top.x - 90, top.y - 22, zn[zi].label, (int)strlen(zn[zi].label));
                }
            }
            SelectObject(md, op);
            DeleteObject(pen);
        }
        BitBlt(dc, 0, 0, c.right, c.bottom, md, 0, 0, SRCCOPY);
        SelectObject(md, old);
        DeleteObject(bm);
        DeleteDC(md);
        EndPaint(hw, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND)
        return 1;
    return DefWindowProcA(hw, msg, wp, lp);
}
static LRESULT CALLBACK ui_sil_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        RECT c;
        HBRUSH br = CreateSolidBrush(s_u_sil_col);
        GetClientRect(hw, &c);
        FillRect(dc, &c, br);
        DeleteObject(br);
        EndPaint(hw, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND)
        return 1;
    return DefWindowProcA(hw, msg, wp, lp);
}
static DWORD WINAPI ui_thread(LPVOID unused)
{
    WNDCLASSA wc;
    MSG m;
    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = ui_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "RecompEditorPanel";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    RegisterClassA(&wc);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = ui_ghost_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "RecompEditorGhost";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = ui_ring_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "RecompEditorRing";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = ui_sil_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "RecompEditorSil";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    s_uw = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "RecompEditorPanel", "Map Editor", WS_POPUP, 0, 0, 330, 600, NULL, NULL,
                           GetModuleHandleA(NULL), NULL);
    s_ugh = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, "RecompEditorGhost", "", WS_POPUP, 0, 0,
                            240, 30, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (s_ugh)
        SetLayeredWindowAttributes(s_ugh, 0, 215, LWA_ALPHA);
    s_ush = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, "RecompEditorSil", "", WS_POPUP, 0, 0,
                            40, 80, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (s_ush)
        SetLayeredWindowAttributes(s_ush, 0, 130, LWA_ALPHA);
    s_urw = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, "RecompEditorRing", "", WS_POPUP, 0, 0,
                            100, 100, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (s_urw)
        SetLayeredWindowAttributes(s_urw, RGB(0, 0, 0), 0, LWA_COLORKEY);
    if (!s_uw) {
        fprintf(stderr, "[EDITOR] window could not be made (%lu)\n", (unsigned long)GetLastError());
        return 0;
    }
    SetTimer(s_uw, 1, 33, NULL);
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}
static void ed_ui_start(void)
{
    if (!s_ui_cs_ready) {
        InitializeCriticalSection(&s_ui_cs);
        s_ui_cs_ready = 1;
    }
    if (InterlockedCompareExchange(&s_ui_thread_up, 1, 0) != 0)
        return;
    CloseHandle(CreateThread(NULL, 0, ui_thread, NULL, 0, NULL));
}

/* once a frame (from game_editor_tick) */
static void ed_ui_frame(const float *eye, const float *fwd, int fc_on)
{
    static int dn0;
    int k0 = s_ed_enabled && fc_on && (GetAsyncKeyState(VK_NUMPAD0) & 0x8000) != 0, vis;
    if (k0 && !dn0)
        s_ui_vis = !s_ui_vis;
    dn0 = k0;
    vis = s_ed_enabled && fc_on && s_ui_vis;
    if (vis && !s_ui_thread_up)
        ed_ui_start();
    if (vis != (s_ui_show != 0)) {
        s_ui_show = vis;
        g_mkb_free_cursor = vis;
        if (!vis)
            ed_look_update(0, NULL, NULL);
        else if (s_ui_cs_ready)
            ed_ui_publish();
    }
    if (!vis)
        return;
    ed_look_update(1, eye, fwd);
    s_ui_cnt[0] = ed_shown();
    s_ui_cnt[1] = ed_cap();
    s_ui_cnt[2] = s_ed_npp;
    s_ui_cnt[3] = ED_NPP;
    s_ui_cnt[4] = s_ed_nzn;
    s_ui_cnt[5] = ED_NZN;
    s_ui_cnt[6] = s_zn_def_count;
    s_ui_cnt[7] = (LONG)(s_zn_def_r + 0.5f);
    s_ui_cnt[8] = s_zn_def_waves;
    s_ui_cnt[9] = (LONG)(s_zn_def_trig + 0.5f);
    s_ui_cnt[10] = s_zn_def_wander;
    s_ui_cnt[11] = s_zn_def_cover;
    s_ui_cnt[12] = s_ed_apply;
    if ((g_game_frames % 20u) == 0u)
        ed_ui_publish();
    ed_ui_requests(eye, fwd);
    if (s_ui_pv_on) {                           /* the drag preview: where does the mouse ray meet the world, and how far is that */
        float d[3], pt[3];
        int hit;
        ed_mouse_dir(s_ui_pv_mx, s_ui_pv_my, fwd, d);
        hit = ed_world_point(eye, d, pt);
        if (!hit) {
            pt[0] = eye[0] + d[0] * 15.0f;
            pt[1] = eye[1] + d[1] * 15.0f;
            pt[2] = eye[2] + d[2] * 15.0f;
        }
        s_ui_pv_depth = (pt[0] - eye[0]) * fwd[0] + (pt[1] - eye[1]) * fwd[1] + (pt[2] - eye[2]) * fwd[2];
        s_ui_pv_px = s_ed_px;
        s_ui_pv_py = s_ed_py;
        s_ui_pv_hit = hit;
        InterlockedIncrement(&s_ui_pv_seq);
    }
    if (s_ui_cs_ready) {                        /* the camera and the zones for the ring window */
        int zi, k;
        EnterCriticalSection(&s_ui_cs);
        memcpy(s_ui_cam, eye, 12);
        memcpy(s_ui_cam + 3, fwd, 12);
        memcpy(s_ui_cam + 6, s_ed_vr, 12);
        memcpy(s_ui_cam + 9, s_ed_vu, 12);
        s_ui_cam[12] = s_ed_px;
        s_ui_cam[13] = s_ed_py;
        for (zi = 0; zi < s_ed_nzn; zi++) {
            const EdZone *z = &s_ed_zn[zi];
            int alive = 0, quota = z->count * z->waves;
            for (k = 0; k < ED_ZMAX; k++)
                if (s_zn_sol[zi][k] && game_editor_is_placed(s_zn_sol[zi][k]) && ed_is_soldier(s_zn_sol[zi][k]) && ed_alive(s_zn_sol[zi][k]))
                    alive++;
            memcpy(s_ui_zn[zi].p, z->p, sizeof z->p);
            s_ui_zn[zi].r = z->r;
            s_ui_zn[zi].team = strstr(z->name, "\\German") ? 2 : 1;
            snprintf(s_ui_zn[zi].label, sizeof s_ui_zn[zi].label, "Zone %d: %s  alive %d, killed %d / %d  (wake %.0f m)", zi + 1, ed_base(z->name), alive, s_zn_killed[zi], quota,
                     (double)z->trig);
        }
        s_ui_nzn = s_ed_nzn;
        LeaveCriticalSection(&s_ui_cs);
    }
}
