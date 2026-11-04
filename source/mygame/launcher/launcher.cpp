/**
 * launcher.cpp -- Launcher.exe for the MoH: European Assault PC port.
 *
 * A small Win32 + GDI+ program, separate from the game. It reads/writes recomp_settings.ini (the same
 * file the in-game Recomp Settings screen uses) plus fov_saved.txt / draw_saved.txt, then starts
 * MOHEuropeanAssault.exe from its own folder. It sets no environment variables: the game reads the ini itself,
 * so double-clicking MOHEuropeanAssault.exe behaves the same as pressing Play here.
 *
 * Text and options are data, not code:
 *   launcher\options.txt   the options of every tab (tab|key|type|default|values)
 *   launcher\lang\*.txt    English and Arabic strings (UTF-8 key=value)
 *   launcher\art\*         the cover and the decals used for the backdrops
 *
 * Launcher.exe --shot <dir>   renders every tab, in both languages, to PNG files (no window).
 */
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <bcrypt.h>
#include <mmsystem.h>
#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <winhttp.h>

using namespace Gdiplus;

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

typedef std::wstring ws;

static const wchar_t *LAUNCHER_VERSION = L"0.9.0-beta";
/* SHA-256 of DEFAULT.XBE of the USA (NTSC-U) release. */
static const char *USA_XBE_SHA256 = "5a34d7f3a38293178c34865dc60819b0796f17e1c7091722079cf075d33335e8";

static const int W = 1180, H = 700, SB = 210;
enum { T_PLAY, T_GAME, T_CONTROLS, T_MODS, T_EXTRAS, T_RECOMP, T_SAVES, T_CREDITS, T_COUNT };
static const wchar_t *TABKEY[T_COUNT] = { L"play", L"game", L"controls", L"mods", L"extras", L"recomp", L"saves", L"credits" };

static ws   g_dir;                 /* folder of Launcher.exe, with a trailing backslash */
static bool g_rtl = false;
static int  g_lang = 0;            /* 0 en, 1 ar */
static int  g_tab = T_PLAY;
static float g_scale = 1.0f;
static float g_mx = -1, g_my = -1;
static float g_scroll[T_COUNT];
static float g_shotScroll;
static float g_maxScroll = 0;
static RectF g_trackR, g_thumbR;           /* the scroll bar of the option list, set while painting */
static bool  g_dragBar, g_pan;
static float g_dragOff, g_panY0, g_panS0;
static ws   g_toast;
static DWORD g_toastUntil = 0;
static HWND g_hwnd;

/* ---------------------------------------------------------------- helpers */

static ws U8(const std::string &s)
{
    if (s.empty()) return ws();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    ws w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string To8(const ws &s)
{
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string o(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &o[0], n, NULL, NULL);
    return o;
}
static bool ReadAll(const ws &path, std::string &out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    if (sz.QuadPart) ReadFile(h, &out[0], (DWORD)sz.QuadPart, &got, NULL);
    CloseHandle(h);
    out.resize(got);
    if (out.size() >= 3 && (uint8_t)out[0] == 0xEF && (uint8_t)out[1] == 0xBB && (uint8_t)out[2] == 0xBF) out.erase(0, 3);
    return true;
}
static bool WriteAll(const ws &path, const std::string &data)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = data.empty() ? TRUE : WriteFile(h, data.data(), (DWORD)data.size(), &put, NULL);
    CloseHandle(h);
    return ok != 0;
}
static std::vector<std::string> SplitLines(const std::string &s)
{
    std::vector<std::string> v;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('\n', i);
        if (j == std::string::npos) j = s.size();
        std::string l = s.substr(i, j - i);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (j < s.size() || !l.empty()) v.push_back(l);
        i = j + 1;
    }
    return v;
}
static std::vector<ws> SplitBy(const ws &s, wchar_t ch)
{
    std::vector<ws> v;
    size_t i = 0;
    for (;;) {
        size_t j = s.find(ch, i);
        if (j == ws::npos) { v.push_back(s.substr(i)); break; }
        v.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return v;
}
static bool Exists(const ws &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool IsDir(const ws &p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static ws EnvW(const wchar_t *n)
{
    wchar_t b[MAX_PATH * 2];
    DWORD k = GetEnvironmentVariableW(n, b, MAX_PATH * 2);
    return k && k < MAX_PATH * 2 ? ws(b) : ws();
}
static ws SavesDir()
{
    ws e = EnvW(L"RECOMP_SAVE_DIR");
    if (!e.empty()) return e;
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(NULL, exe, MAX_PATH)) {
        wchar_t *slash = wcsrchr(exe, L'\\');
        if (slash) {
            *slash = L'\0';
            return ws(exe) + L"\\xboxdata";
        }
    }
    return EnvW(L"LOCALAPPDATA") + L"\\xboxrecomp";
}

/* ---------------------------------------------------------------- strings */

static std::map<ws, ws> g_str[2];

static void LoadStrings(int which, const wchar_t *file)
{
    std::string d;
    if (!ReadAll(g_dir + L"launcher\\lang\\" + file, d)) return;
    for (auto &l : SplitLines(d)) {
        if (l.empty() || l[0] == '#') continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        g_str[which][U8(l.substr(0, eq))] = U8(l.substr(eq + 1));
    }
}
static ws T(const ws &k)
{
    auto it = g_str[g_lang].find(k);
    if (it != g_str[g_lang].end()) return it->second;
    it = g_str[0].find(k);
    if (it != g_str[0].end()) return it->second;
    return k;
}
static bool HasT(const ws &k)
{
    return g_str[g_lang].count(k) || g_str[0].count(k);
}

/* ---------------------------------------------------------------- ini + options */

struct Opt {
    ws tab, key, type, def, cur;
    std::vector<ws> vals;
    int tabIdx;
    ws dep;                                 /* optional 6th field: another option's key; while that one is 0 this row is greyed out */
};
static std::vector<Opt> g_opts;
static std::vector<std::string> g_ini;      /* recomp_settings.ini, line by line, unknown lines kept */

static int IniFind(const std::string &key)
{
    for (size_t i = 0; i < g_ini.size(); i++) {
        const std::string &l = g_ini[i];
        if (l.size() > key.size() && l.compare(0, key.size(), key) == 0 && l[key.size()] == '=') return (int)i;
    }
    return -1;
}
static bool IniGet(const std::string &key, std::string &val)
{
    int i = IniFind(key);
    if (i < 0) return false;
    val = g_ini[i].substr(key.size() + 1);
    return true;
}
static void IniSet(const std::string &key, const std::string &val)
{
    int i = IniFind(key);
    std::string line = key + "=" + val;
    if (i >= 0) g_ini[i] = line; else g_ini.push_back(line);
}
static void IniErase(const std::string &key)
{
    int i = IniFind(key);
    if (i >= 0) g_ini.erase(g_ini.begin() + i);
}
static std::string Trim(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}
static ws FileNum(const wchar_t *name)
{
    std::string d;
    if (!ReadAll(g_dir + name, d)) return ws();
    return U8(Trim(d.substr(0, d.find('\n'))));
}
static ws Fmt(double v)
{
    wchar_t b[32];
    swprintf(b, 32, L"%g", v);
    return b;
}

static int FindIdx(const Opt &o, const ws &v)
{
    for (size_t i = 0; i < o.vals.size(); i++) if (o.vals[i] == v) return (int)i;
    return -1;
}

/* key bindings: an option of type "bind" holds "kb keys;pad buttons" (kb_<id> / pad_<id> of the ini), "movekey" one key of move= */
static const wchar_t *const PADID[] = { L"a", L"b", L"x", L"y", L"lb", L"rb", L"lt", L"rt", L"l3", L"r3", L"start", L"back", L"dup", L"ddown", L"dleft", L"dright", L"none" };
static const wchar_t *const PADLBL[] = { L"A", L"B", L"X", L"Y", L"LB", L"RB", L"LT", L"RT", L"L3", L"R3", L"Menu", L"View", L"D-pad \u2191", L"D-pad \u2193", L"D-pad \u2190", L"D-pad \u2192", L"-" };
static const int PADN = 17;

static void BindSplit(const ws &cur, ws &kb, ws &pad)
{
    size_t p = cur.find(L';');
    kb = cur.substr(0, p);
    pad = p == ws::npos ? ws() : cur.substr(p + 1);
}
static ws KeyLabel(const ws &id)
{
    static const wchar_t *const nm[][2] = {
        { L"space", L"Space" }, { L"enter", L"Enter" }, { L"esc", L"Esc" }, { L"tab", L"Tab" }, { L"backspace", L"Backspace" },
        { L"lshift", L"Left Shift" }, { L"rshift", L"Right Shift" }, { L"lctrl", L"Left Ctrl" }, { L"rctrl", L"Right Ctrl" },
        { L"lalt", L"Left Alt" }, { L"ralt", L"Right Alt" }, { L"capslock", L"Caps Lock" }, { L"up", L"Up arrow" },
        { L"down", L"Down arrow" }, { L"left", L"Left arrow" }, { L"right", L"Right arrow" }, { L"insert", L"Insert" },
        { L"delete", L"Delete" }, { L"home", L"Home" }, { L"end", L"End" }, { L"pageup", L"Page Up" }, { L"pagedown", L"Page Down" },
        { L"mouse1", L"Left mouse" }, { L"mouse2", L"Right mouse" }, { L"mouse3", L"Middle mouse" }, { L"mouse4", L"Mouse 4" },
        { L"mouse5", L"Mouse 5" }, { L"wheel", L"Mouse wheel" }, { L"none", L"-" } };
    for (auto &n : nm) if (id == n[0]) return n[1];
    if (id.size() == 1) return ws(1, (wchar_t)towupper(id[0]));
    if (id.size() >= 2 && id[0] == L'f' && iswdigit(id[1])) return L"F" + id.substr(1);
    if (id.size() > 2 && id.compare(0, 2, L"vk") == 0) return L"Key " + id.substr(2);
    return id;
}
static ws KeysText(const ws &list)
{
    ws out;
    for (auto &k : SplitBy(list, L',')) {
        if (k.empty()) continue;
        if (!out.empty()) out += L" / ";
        out += KeyLabel(k);
    }
    return out.empty() ? ws(L"-") : ws(1, (wchar_t)0x202A) + out + ws(1, (wchar_t)0x202C);
}
static ws PadText(const ws &list)
{
    ws out;
    for (auto &k : SplitBy(list, L',')) {
        int i = 0;
        while (i < PADN && k != PADID[i]) i++;
        if (i >= PADN || i == PADN - 1) continue;
        if (!out.empty()) out += L" / ";
        out += PADLBL[i];
    }
    return out.empty() ? ws(L"-") : ws(1, (wchar_t)0x202A) + out + ws(1, (wchar_t)0x202C);
}

static void LoadOptions()
{
    std::string d;
    ReadAll(g_dir + L"launcher\\options.txt", d);
    for (auto &l : SplitLines(d)) {
        if (l.empty() || l[0] == '#') continue;
        std::vector<ws> p = SplitBy(U8(l), L'|');
        if (p.size() < 3) continue;
        Opt o;
        o.tab = p[0]; o.key = p[1]; o.type = p[2];
        o.def = p.size() > 3 ? p[3] : ws();
        if (p.size() > 4 && !p[4].empty()) o.vals = SplitBy(p[4], L',');
        if (p.size() > 5) o.dep = p[5];
        o.tabIdx = -1;
        for (int t = 0; t < T_COUNT; t++) if (o.tab == TABKEY[t]) o.tabIdx = t;
        if (o.tabIdx < 0) continue;
        g_opts.push_back(o);
    }
}

static ws g_langAtLoad;
struct Opt;
static Opt *OptByKey(const ws &k);
static ws AutoRes();
static bool g_firstRun;                /* recomp_settings.ini carries first_run=1 (a fresh package): pick the resolution once */
static bool g_musicOn = true;         /* the player's choice (speaker button); needs launcher\music\credits.wav/.mp3 to do anything */
static bool g_musicHave, g_musicPlaying;
static void LoadIni()
{
    std::string d;
    g_ini.clear();
    if (ReadAll(g_dir + L"recomp_settings.ini", d)) g_ini = SplitLines(d);

    std::string v;
    if (IniGet("launcher_lang", v)) g_lang = (v == "ar") ? 1 : 0;
    if (IniGet("launcher_music", v)) g_musicOn = atoi(v.c_str()) != 0;
    g_rtl = g_lang == 1;

    for (auto &o : g_opts) {
        if (o.type == L"lang") {
            o.vals.clear();
            o.vals.push_back(L"english");
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((g_dir + L"languages\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && _wcsicmp(fd.cFileName, L"english"))
                        o.vals.push_back(fd.cFileName);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        if (o.type == L"action" || o.type == L"link" || o.type == L"note") continue;
        o.cur = o.def;
        if (o.type == L"bind") {
            ws dkb, dpad, kb, pad;
            BindSplit(o.def, dkb, dpad);
            kb = dkb; pad = dpad;
            std::string id = To8(o.key.substr(5));
            if (IniGet("kb_" + id, v) && !Trim(v).empty()) kb = U8(Trim(v));
            if (IniGet("pad_" + id, v) && !Trim(v).empty()) pad = U8(Trim(v));
            o.cur = kb + L";" + pad;
            continue;
        }
        if (o.type == L"movekey") {
            if (IniGet("move", v)) {
                std::vector<ws> mk = SplitBy(U8(Trim(v)), L',');
                size_t ix = (size_t)(o.key.back() - L'0');
                if (ix < mk.size() && !mk[ix].empty()) o.cur = mk[ix];
            }
            continue;
        }
        if (o.key == L"@fov") { ws f = FileNum(L"fov_saved.txt"); if (!f.empty()) o.cur = Fmt(_wtof(f.c_str())); }
        else if (o.key == L"@draw") { ws f = FileNum(L"draw_saved.txt"); if (!f.empty()) o.cur = Fmt(_wtof(f.c_str())); }
        else if (IniGet(To8(o.key), v)) {
            ws w = U8(Trim(v));
            if (o.type == L"toggle") o.cur = (_wtoi(w.c_str()) != 0) ? L"1" : L"0";
            else if (!w.empty()) o.cur = w;
        }
        if (o.type != L"toggle" && FindIdx(o, o.cur) < 0) o.vals.push_back(o.cur);
    }
    if (Opt *lg = OptByKey(L"language")) g_langAtLoad = lg->cur;
    if (IniGet("first_run", v) && atoi(v.c_str()) != 0) {      /* the shipped defaults are modest; the picture size follows this screen */
        g_firstRun = true;
        if (Opt *r = OptByKey(L"res")) {
            r->cur = AutoRes();
            if (FindIdx(*r, r->cur) < 0) r->vals.push_back(r->cur);
        }
    }
}

static void SaveIni()
{
    if (g_firstRun) { IniErase("first_run"); g_firstRun = false; }
    IniSet("launcher_lang", g_lang ? "ar" : "en");
    IniSet("launcher_music", g_musicOn ? "1" : "0");
    {   /* a new game language brings its own menu language along when the pack has a recomp_menu.txt */
        Opt *lg = OptByKey(L"language");
        if (lg && lg->cur != g_langAtLoad) {
            bool has = lg->cur != L"english" && Exists(g_dir + L"languages\\" + lg->cur + L"\\recomp_menu.txt");
            IniSet("menu_lang", To8(has ? lg->cur : ws(L"english")));
            g_langAtLoad = lg->cur;
        }
    }
    {   /* key bindings: a line equal to the default is left out, so the game's own defaults stay in charge */
        ws mv[4] = { L"w", L"s", L"a", L"d" };
        bool moved = false;
        for (auto &o : g_opts) {
            if (o.type == L"bind") {
                ws kb, pad, dkb, dpad;
                BindSplit(o.cur, kb, pad); BindSplit(o.def, dkb, dpad);
                std::string id = To8(o.key.substr(5));
                if (kb == dkb || kb.empty()) IniErase("kb_" + id); else IniSet("kb_" + id, To8(kb));
                if (pad == dpad || pad.empty()) IniErase("pad_" + id); else IniSet("pad_" + id, To8(pad));
            } else if (o.type == L"movekey") {
                size_t ix = (size_t)(o.key.back() - L'0');
                if (ix < 4) { mv[ix] = o.cur; if (o.cur != o.def) moved = true; }
            }
        }
        if (moved) IniSet("move", To8(mv[0] + L"," + mv[1] + L"," + mv[2] + L"," + mv[3])); else IniErase("move");
    }
    for (auto &o : g_opts) {
        if (o.type == L"action" || o.type == L"link" || o.type == L"note" || o.type == L"bind" || o.type == L"movekey") continue;
        if (o.key[0] == L'@') {
            ws f = o.key == L"@fov" ? L"fov_saved.txt" : L"draw_saved.txt";
            WriteAll(g_dir + f, To8(o.cur) + "\n");
            continue;
        }
        IniSet(To8(o.key), To8(o.cur));
    }
    std::string out;
    for (auto &l : g_ini) { out += l; out += "\r\n"; }
    WriteAll(g_dir + L"recomp_settings.ini", out);
}

/* An option whose dep (6th field of options.txt) is set to 0 is shown greyed out and cannot be changed */
static bool OptOff(const Opt &o)
{
    if (o.dep.empty()) return false;
    size_t eq = o.dep.find(L'=');
    if (eq != ws::npos) {                       // "key=value": greyed out unless that option has this value
        Opt *d2 = OptByKey(o.dep.substr(0, eq));
        return d2 && d2->cur != o.dep.substr(eq + 1);
    }
    Opt *d = OptByKey(o.dep);
    return d && d->cur == L"0";
}

static Opt *OptByKey(const ws &k)
{
    for (auto &o : g_opts) if (o.key == k) return &o;
    return NULL;
}

/* ---------------------------------------------------------------- first run, presets, system check, updates */

/* The version of this package: launcher\version.txt (written by make_package.py), else the compiled-in one. */
static ws PackageVersion()
{
    static ws v;
    if (v.empty()) {
        std::string d;
        if (ReadAll(g_dir + L"launcher\\version.txt", d)) v = U8(Trim(SplitLines(d).empty() ? std::string() : SplitLines(d)[0]));
        if (v.empty()) v = LAUNCHER_VERSION;
    }
    return v;
}

static void SetOpt(const wchar_t *k, const ws &v)
{
    Opt *o = OptByKey(k);
    if (!o) return;
    o->cur = v;
    if (o->type != L"toggle" && FindIdx(*o, v) < 0) o->vals.push_back(v);
}

/* The biggest picture size of the list that fits the primary monitor, never above 2560x1440 (4K stays a manual choice). */
static ws AutoRes()
{
    POINT pt = { 0, 0 };
    MONITORINFO mi = { sizeof mi };
    int nw = 1920, nh = 1080;
    if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi)) {
        nw = mi.rcMonitor.right - mi.rcMonitor.left;
        nh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    nw = std::min(nw, 2560);
    nh = std::min(nh, 1440);
    ws best = L"1280x720";
    long bestA = 0;
    if (Opt *r = OptByKey(L"res"))
        for (auto &v : r->vals) {
            int w = 0, h = 0;
            if (swscanf(v.c_str(), L"%dx%d", &w, &h) == 2 && w <= nw && h <= nh && (long)w * h > bestA) {
                best = v;
                bestA = (long)w * h;
            }
        }
    return best;
}

/* The two presets of the Game tab: "Balanced" is what a fresh package starts with, "High performance" is the
 * author's own setting (fast PC). Both pick the picture size from the monitor. */
static void ApplyPreset(bool high)
{
    SetOpt(L"res", AutoRes());
    SetOpt(L"fps", high ? L"165" : L"60");
    SetOpt(L"@draw", high ? L"5" : L"2");
    SetOpt(L"fog", L"2.0");
}

/* System files the game program needs. A missing VCRUNTIME140.dll is the usual one: the game would die at start
 * with a dialog that tells a player nothing. (This launcher links its runtime statically, so it can say it.) */
static std::vector<ws> g_missingDlls;
static int g_missingKind = -1;               /* 0 Visual C++ runtime, 1 Media Foundation (Windows N), 2 other system DLL */
static void CheckSystem()
{
    static const struct { const wchar_t *dll; int kind; } need[] = {
        { L"vcruntime140.dll", 0 }, { L"mfplat.dll", 1 }, { L"mfreadwrite.dll", 1 },
        { L"d3d11.dll", 2 }, { L"d3dcompiler_47.dll", 2 }, { L"xinput1_4.dll", 2 } };
    g_missingDlls.clear();
    g_missingKind = -1;
    for (auto &n : need) {
        HMODULE m = LoadLibraryExW(n.dll, NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (m) { FreeLibrary(m); continue; }
        g_missingDlls.push_back(n.dll);
        if (g_missingKind < 0 || n.kind < g_missingKind) g_missingKind = n.kind;
    }
}
static ws SysFixUrl()
{
    if (g_missingKind == 0) return L"https://aka.ms/vs/17/release/vc_redist.x64.exe";
    if (g_missingKind == 1) return L"https://support.microsoft.com/windows/media-feature-pack-for-windows-n-editions";
    return L"https://support.microsoft.com/windows/update-windows";
}

/* Update check: asks GitHub (releases of the repository named by the "report" link) on a background thread, short
 * timeouts, silent on any failure (no release yet = 404). The player can switch it off (Advanced tab). */
static ws g_updTag, g_updUrl;
static volatile LONG g_updFound;
static ws RepoPath()
{
    Opt *o = OptByKey(L"report");
    ws u = o ? o->def : ws();
    size_t p = u.find(L"github.com/");
    if (p == ws::npos) return ws();
    ws rest = u.substr(p + 11);
    std::vector<ws> parts = SplitBy(rest, L'/');
    if (parts.size() < 2 || parts[0].empty() || parts[1].empty()) return ws();
    return L"/repos/" + parts[0] + L"/" + parts[1] + L"/releases?per_page=5";
}
static bool VerNewer(ws tag, ws cur)
{
    auto strip = [](ws s) { while (!s.empty() && (s[0] == L'v' || s[0] == L'V')) s.erase(0, 1); return s; };
    tag = strip(tag); cur = strip(cur);
    auto split = [](const ws &s, int n[3], ws &pre) {
        n[0] = n[1] = n[2] = 0;
        size_t d = s.find(L'-');
        pre = d == ws::npos ? ws() : s.substr(d + 1);
        std::vector<ws> p = SplitBy(s.substr(0, d), L'.');
        for (size_t i = 0; i < 3 && i < p.size(); i++) n[i] = _wtoi(p[i].c_str());
    };
    int a[3], b[3];
    ws pa, pb;
    split(tag, a, pa);
    split(cur, b, pb);
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
    if (pa.empty() && !pb.empty()) return true;          /* 0.9.0 is newer than 0.9.0-beta */
    if (!pa.empty() && !pb.empty()) return pa > pb;       /* beta2 > beta */
    return false;
}
static std::string JsonField(const std::string &body, const char *key, size_t from = 0)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = body.find(k, from);
    if (p == std::string::npos) return std::string();
    p = body.find('"', body.find(':', p + k.size()) + 1);
    if (p == std::string::npos) return std::string();
    size_t e = body.find('"', p + 1);
    return e == std::string::npos ? std::string() : body.substr(p + 1, e - p - 1);
}
static DWORD WINAPI UpdateThread(LPVOID)
{
    ws path = RepoPath();
    if (path.empty()) return 0;
    HINTERNET s = WinHttpOpen(L"MoH-EA-PC-Port-Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 0;
    WinHttpSetTimeouts(s, 4000, 4000, 4000, 6000);
    HINTERNET c = WinHttpConnect(s, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    std::string body;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, NULL, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        if (code == 200) {
            char buf[8192];
            DWORD got;
            while (body.size() < (1u << 18) && WinHttpReadData(r, buf, sizeof buf, &got) && got) body.append(buf, got);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    std::string tag = JsonField(body, "tag_name");
    if (!tag.empty() && VerNewer(U8(tag), PackageVersion())) {
        std::string url = JsonField(body, "html_url");      /* the first html_url of the list is the newest release's page */
        g_updTag = U8(tag);
        g_updUrl = url.compare(0, 19, "https://github.com/") == 0 ? U8(url) : ws();
        InterlockedExchange(&g_updFound, 1);
        if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
    }
    return 0;
}

static ws ValName(const Opt &o)
{
    if (o.type == L"toggle") return T(o.cur == L"1" ? L"on" : L"off");
    ws k = o.key + L".v." + o.cur;
    if (HasT(k)) return T(k);
    return o.cur;
}

/* ---------------------------------------------------------------- game files */

static std::string Sha256File(const ws &path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return std::string();
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hh = NULL;
    std::string res;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 && BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) == 0) {
        std::vector<uint8_t> buf(1 << 20);
        DWORD got;
        while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, NULL) && got) BCryptHashData(hh, buf.data(), got, 0);
        uint8_t dg[32];
        if (BCryptFinishHash(hh, dg, 32, 0) == 0) {
            char hex[65];
            for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", dg[i]);
            res = hex;
        }
    }
    if (hh) BCryptDestroyHash(hh);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return res;
}

static bool g_hasXbe, g_hasData, g_hasEagl, g_hasExe;
static int  g_ver;                 /* 0 unknown (no file), 1 USA ok, 2 other */
static bool g_ready;
static void CheckGame()
{
    g_hasXbe = Exists(g_dir + L"game\\DEFAULT.XBE");
    g_hasData = IsDir(g_dir + L"game\\DATA");
    g_hasEagl = Exists(g_dir + L"game\\EAGLRM.O");
    g_hasExe = Exists(g_dir + L"MOHEuropeanAssault.exe");
    g_ver = 0;
    if (g_hasXbe) g_ver = Sha256File(g_dir + L"game\\DEFAULT.XBE") == USA_XBE_SHA256 ? 1 : 2;
    g_ready = g_hasXbe && g_hasData && g_hasEagl && g_hasExe && g_ver == 1 && g_missingDlls.empty();
}

/* ---------------------------------------------------------------- drawing basics */

static std::map<int, Font *> g_fonts;
static FontFamily *g_famBody, *g_famHead;
static Font &Fnt(float px, bool bold = false, bool head = false)
{
    int key = (int)(px * 10) + (bold ? 100000 : 0) + (head ? 1000000 : 0) + g_lang * 10000000;
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return *it->second;
    FontFamily *fam = head && !g_rtl ? g_famHead : g_famBody;
    Font *f = new Font(fam, px, (bold || (head && g_rtl)) ? FontStyleBold : FontStyleRegular, UnitPixel);
    g_fonts[key] = f;
    return *f;
}

static const Color KH(255, 200, 181, 96), KHD(255, 150, 138, 80), RED(255, 168, 36, 28), REDH(255, 206, 54, 40),
    TXT(255, 232, 228, 214), DIM(255, 150, 150, 134), OKC(255, 122, 190, 100), BADC(255, 222, 84, 62);

static RectF M(float x, float y, float w, float h)
{
    if (g_rtl) x = W - x - w;
    return RectF(x, y, w, h);
}

static void Txt(Graphics &g, const ws &s, Font &f, Color c, RectF r, int al = 0, int va = 1, bool wrap = false)
{
    StringFormat sf;
    INT fl = g_rtl ? StringFormatFlagsDirectionRightToLeft : 0;
    if (!wrap) fl |= StringFormatFlagsNoWrap;
    sf.SetFormatFlags(fl);
    sf.SetAlignment(al == 0 ? StringAlignmentNear : al == 1 ? StringAlignmentCenter : StringAlignmentFar);
    sf.SetLineAlignment(va == 0 ? StringAlignmentNear : va == 1 ? StringAlignmentCenter : StringAlignmentFar);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush b(c);
    g.DrawString(s.c_str(), -1, &f, r, &sf, &b);
}

static void RRPath(GraphicsPath &p, RectF r, float rad)
{
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}
static void FillRR(Graphics &g, Color c, RectF r, float rad)
{
    GraphicsPath p; RRPath(p, r, rad);
    SolidBrush b(c);
    g.FillPath(&b, &p);
}
static void StrokeRR(Graphics &g, Color c, RectF r, float rad, float w = 1.0f)
{
    GraphicsPath p; RRPath(p, r, rad);
    Pen pen(c, w);
    g.DrawPath(&pen, &p);
}
static void Star(Graphics &g, float cx, float cy, float R, Color c)
{
    PointF p[10];
    for (int i = 0; i < 10; i++) {
        float a = -3.14159265f / 2 + i * 3.14159265f / 5;
        float r = (i & 1) ? R * 0.40f : R;
        p[i] = PointF(cx + r * cosf(a), cy + r * sinf(a));
    }
    SolidBrush b(c);
    g.FillPolygon(&b, p, 10);
}

/* ---------------------------------------------------------------- backdrops */

static Bitmap *g_bg[T_COUNT];
static Bitmap *g_cover, *g_coverR;
static Bitmap *g_blood[3];

static uint32_t g_rng = 12345;
static float Rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return (g_rng & 0xFFFFFF) / 16777216.0f; }

static void DrawDecal(Graphics &g, Bitmap *b, float cx, float cy, float size, float rot, float alpha)
{
    if (!b) return;
    ColorMatrix cm = { 1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,alpha,0, 0,0,0,0,1 };
    ImageAttributes ia;
    ia.SetColorMatrix(&cm);
    GraphicsState st = g.Save();
    g.TranslateTransform(cx, cy);
    g.RotateTransform(rot);
    g.DrawImage(b, Rect((INT)(-size / 2), (INT)(-size / 2), (INT)size, (INT)size), 0, 0, (INT)b->GetWidth(), (INT)b->GetHeight(), UnitPixel, &ia);
    g.Restore(st);
}

static void Stamp(Graphics &g, const wchar_t *text, float cx, float cy, float rot, Color c, float px)
{
    GraphicsState st = g.Save();
    g.TranslateTransform(cx, cy);
    g.RotateTransform(rot);
    Font f(g_famHead, px, FontStyleBold, UnitPixel);
    RectF bb;
    g.MeasureString(text, -1, &f, PointF(0, 0), &bb);
    Pen pen(c, 4);
    g.DrawRectangle(&pen, -bb.Width / 2 - 14, -bb.Height / 2 - 8, bb.Width + 28, bb.Height + 16);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush b(c);
    g.DrawString(text, -1, &f, PointF(0, 0), &sf, &b);
    g.Restore(st);
}

static void BuildBackdrop(int tab)
{
    Bitmap *bm = new Bitmap(W, H, PixelFormat32bppPARGB);
    Graphics g(bm);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g_rng = 9001 + tab * 77;

    LinearGradientBrush base(Point(0, 0), Point(0, H), Color(255, 40, 44, 34), Color(255, 15, 17, 12));
    g.FillRectangle(&base, 0, 0, W, H);

    switch (tab) {
    case T_PLAY: {                                  /* searchlights over a ruined skyline */
        for (int i = 0; i < 4; i++) {
            float bx = 640 + i * 150 + Rnd() * 40, tx = bx + (Rnd() - 0.4f) * 520, ty = -20;
            PointF tri[3] = { PointF(bx - 14, H), PointF(bx + 14, H), PointF(tx, ty) };
            GraphicsPath gp; gp.AddPolygon(tri, 3);
            PathGradientBrush pb(&gp);
            pb.SetCenterColor(Color(46, 220, 215, 190));
            Color sc(0, 220, 215, 190); int n = 1; pb.SetSurroundColors(&sc, &n);
            pb.SetCenterPoint(PointF(bx, H - 30));
            g.FillPath(&pb, &gp);
        }
        SolidBrush ruin(Color(150, 6, 7, 5));
        float x = 600;
        while (x < W) {
            float bw = 30 + Rnd() * 60, bh = 40 + Rnd() * 150;
            g.FillRectangle(&ruin, x, H - bh, bw, bh);
            for (int k = 0; k < 3; k++) {
                SolidBrush hole(Color(255, 14, 16, 12));
                g.FillRectangle(&hole, x + 6 + Rnd() * (bw - 20), H - bh + 10 + Rnd() * (bh - 30), 8.f, 12.f);
            }
            x += bw + 6;
        }
        break;
    }
    case T_GAME: {                                  /* topographic map + grid + crosshair */
        Pen grid(Color(14, 200, 181, 96), 1);
        for (int x = SB; x < W; x += 70) g.DrawLine(&grid, x, 0, x, H);
        for (int y = 0; y < H; y += 70) g.DrawLine(&grid, SB, y, W, y);
        Pen p(Color(34, 200, 181, 96), 1.3f);
        for (int k = 0; k < 18; k++) {
            std::vector<PointF> pts;
            for (int x = SB - 20; x <= W + 20; x += 22)
                pts.push_back(PointF((float)x, 30 + k * 40.f + 30 * sinf(x * 0.008f + k * 0.7f) + 18 * sinf(x * 0.021f + k * 1.9f)));
            g.DrawCurve(&p, pts.data(), (INT)pts.size(), 0.5f);
        }
        Pen red(Color(60, 190, 50, 40), 2);
        g.DrawEllipse(&red, 860.f, 180.f, 180.f, 180.f);
        g.DrawEllipse(&red, 890.f, 210.f, 120.f, 120.f);
        g.DrawLine(&red, 800, 270, 1100, 270);
        g.DrawLine(&red, 950, 140, 950, 400);
        Font f(g_famBody, 12, FontStyleRegular, UnitPixel);
        SolidBrush tb(Color(70, 200, 181, 96));
        g.DrawString(L"49°24'N  000°14'E", -1, &f, PointF(960, 372), &tb);
        g.DrawString(L"GRID 7-C", -1, &f, PointF(240, 660), &tb);
        break;
    }
    case T_CONTROLS: {                              /* radar */
        float cx = 900, cy = 380;
        Pen ring(Color(34, 140, 190, 110), 1.2f);
        for (int r = 60; r <= 420; r += 60) g.DrawEllipse(&ring, cx - r, cy - r, r * 2.f, r * 2.f);
        g.DrawLine(&ring, cx - 440, cy, cx + 440, cy);
        g.DrawLine(&ring, cx, cy - 440, cx, cy + 440);
        SolidBrush sw(Color(26, 140, 210, 110));
        g.FillPie(&sw, cx - 420, cy - 420, 840.f, 840.f, 300.f, 50.f);
        SolidBrush blip(Color(110, 150, 230, 120));
        for (int i = 0; i < 7; i++) g.FillEllipse(&blip, cx + (Rnd() - 0.5f) * 560, cy + (Rnd() - 0.5f) * 560, 7.f, 7.f);
        break;
    }
    case T_MODS: {                                  /* wooden crate planks */
        for (int x = SB; x < W; x += 118) {
            int shade = 28 + (int)(Rnd() * 16);
            SolidBrush pl(Color(150, shade + 12, shade, shade - 12));
            g.FillRectangle(&pl, x, 0, 116, H);
            Pen gr(Color(30, 0, 0, 0), 1);
            for (int k = 0; k < 40; k++) { float y = Rnd() * H; g.DrawLine(&gr, x + 4 + Rnd() * 20, y, x + 90 + Rnd() * 24, y + (Rnd() - 0.5f) * 6); }
            SolidBrush nail(Color(110, 90, 85, 70));
            g.FillEllipse(&nail, x + 12.f, 20.f, 7.f, 7.f); g.FillEllipse(&nail, x + 96.f, 20.f, 7.f, 7.f);
            g.FillEllipse(&nail, x + 12.f, H - 30.f, 7.f, 7.f); g.FillEllipse(&nail, x + 96.f, H - 30.f, 7.f, 7.f);
        }
        Pen band(Color(120, 20, 20, 16), 14);
        g.DrawLine(&band, SB, 90, W, 90);
        g.DrawLine(&band, SB, H - 90, W, H - 90);
        Stamp(g, L"FRAGILE", 960, 560, -9, Color(70, 200, 181, 96), 56);
        break;
    }
    case T_EXTRAS: {                                /* blood and bullet holes */
        DrawDecal(g, g_blood[0], 1010, 190, 520, 12, 0.5f);
        DrawDecal(g, g_blood[1], 560, 560, 420, -25, 0.4f);
        DrawDecal(g, g_blood[2], 1100, 600, 330, 70, 0.4f);
        for (int i = 0; i < 6; i++) {
            float x = 280 + Rnd() * 860, y = 60 + Rnd() * 580;
            SolidBrush hole(Color(190, 4, 4, 3)); g.FillEllipse(&hole, x, y, 9.f, 9.f);
            Pen crack(Color(60, 0, 0, 0), 1.2f);
            for (int k = 0; k < 6; k++) { float a = k * 1.05f; g.DrawLine(&crack, x + 4, y + 4, x + 4 + 24 * cosf(a), y + 4 + 24 * sinf(a)); }
        }
        break;
    }
    case T_RECOMP: {                                /* blueprint */
        Pen fine(Color(12, 120, 190, 190), 1), big(Color(26, 120, 190, 190), 1);
        for (int x = SB; x < W; x += 20) g.DrawLine(&fine, x, 0, x, H);
        for (int y = 0; y < H; y += 20) g.DrawLine(&fine, SB, y, W, y);
        for (int x = SB; x < W; x += 100) g.DrawLine(&big, x, 0, x, H);
        for (int y = 0; y < H; y += 100) g.DrawLine(&big, SB, y, W, y);
        Pen c(Color(60, 120, 200, 200), 1.5f);
        g.DrawEllipse(&c, 830.f, 300.f, 220.f, 220.f);
        g.DrawEllipse(&c, 880.f, 350.f, 120.f, 120.f);
        g.DrawRectangle(&c, 480.f, 520.f, 220.f, 110.f);
        g.DrawLine(&c, 700, 575, 830, 410);
        Font f(g_famBody, 12, FontStyleRegular, UnitPixel);
        SolidBrush tb(Color(80, 120, 200, 200));
        g.DrawString(L"CORE-2  /  PIN", -1, &f, PointF(860, 530), &tb);
        g.DrawString(L"RING-BUS", -1, &f, PointF(500, 636), &tb);
        break;
    }
    case T_SAVES: {                                 /* manila folder */
        SolidBrush man(Color(46, 200, 170, 100));
        g.FillRectangle(&man, SB + 40, 80, W - SB - 80, H - 130);
        GraphicsPath tabp; RRPath(tabp, RectF(SB + 40.f, 52.f, 200.f, 40.f), 8);
        g.FillPath(&man, &tabp);
        Pen ln(Color(18, 0, 0, 0), 1);
        for (int y = 120; y < H - 60; y += 28) g.DrawLine(&ln, SB + 60, y, W - 60, y);
        Stamp(g, L"CLASSIFIED", 930, 540, -12, Color(78, 190, 40, 32), 60);
        break;
    }
    case T_CREDITS: {                               /* medal and ribbon */
        float rx = 900;
        const Color rc[5] = { Color(70, 150, 30, 26), Color(70, 220, 215, 200), Color(70, 30, 50, 120), Color(70, 220, 215, 200), Color(70, 150, 30, 26) };
        for (int i = 0; i < 5; i++) { SolidBrush b(rc[i]); g.FillRectangle(&b, rx - 80 + i * 32, 0.f, 32.f, 330.f); }
        SolidBrush disc(Color(70, 200, 181, 96));
        g.FillEllipse(&disc, rx - 120, 300.f, 240.f, 240.f);
        Pen ring(Color(110, 200, 181, 96), 5);
        g.DrawEllipse(&ring, rx - 120, 300.f, 240.f, 240.f);
        Star(g, rx, 422, 92, Color(120, 200, 181, 96));
        break;
    }
    }

    /* film grain */
    BitmapData bd;
    Rect all(0, 0, W, H);
    if (bm->LockBits(&all, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppPARGB, &bd) == Ok) {
        for (int y = 0; y < H; y++) {
            uint8_t *row = (uint8_t *)bd.Scan0 + y * bd.Stride;
            for (int x = 0; x < W; x++) {
                int d = (int)(Rnd() * 14) - 7;
                for (int c = 0; c < 3; c++) { int v = row[x * 4 + c] + d; row[x * 4 + c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
            }
        }
        bm->UnlockBits(&bd);
    }
    /* vignette */
    GraphicsPath vp; vp.AddRectangle(RectF(-200, -200, W + 400.f, H + 400.f));
    GraphicsPath ell; ell.AddEllipse(RectF(-W * 0.2f, -H * 0.3f, W * 1.4f, H * 1.6f));
    PathGradientBrush vb(&ell);
    vb.SetCenterColor(Color(0, 0, 0, 0));
    Color edge(190, 0, 0, 0); int n = 1; vb.SetSurroundColors(&edge, &n);
    g.FillRectangle(&vb, 0, 0, W, H);
    g_bg[tab] = bm;
}

/* The cover fitted to the full height of the window, its right edge fading into the page. */
static float g_coverW;
static Bitmap *MakeCover(Bitmap &src, bool fadeLeft)
{
    int cw = (int)(g_coverW + 0.5f);
    Bitmap *c = new Bitmap(cw, H, PixelFormat32bppARGB);
    {
        Graphics g(c);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(&src, Rect(0, 0, cw, H), 0, 0, (INT)src.GetWidth(), (INT)src.GetHeight(), UnitPixel);
    }
    BitmapData bd;
    Rect all(0, 0, cw, H);
    if (c->LockBits(&all, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Ok) {
        const int fade = 100;
        for (int y = 0; y < H; y++) {
            uint8_t *row = (uint8_t *)bd.Scan0 + y * bd.Stride;
            for (int x = 0; x < cw; x++) {
                float a = 1.0f;
                int dx = fadeLeft ? x : cw - 1 - x;      /* distance from the edge that fades */
                if (dx < fade) { float t = (float)dx / fade; a = t * t * (3 - 2 * t); }
                float dark = 1.0f - 0.35f * (y > H - 160 ? (y - (H - 160)) / 160.0f : 0.0f);
                for (int k = 0; k < 3; k++) row[x * 4 + k] = (uint8_t)(row[x * 4 + k] * dark);
                row[x * 4 + 3] = (uint8_t)(255 * a);
            }
        }
        c->UnlockBits(&bd);
    }
    return c;
}

/* Used when launcher\art\cover.jpg is absent (a package without the game's box art): searchlights and an emblem. */
static void PaintFallbackCover(Bitmap &bm)
{
    int w = (int)bm.GetWidth();
    Graphics g(&bm);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    LinearGradientBrush base(Point(0, 0), Point(0, H), Color(255, 52, 58, 46), Color(255, 14, 16, 12));
    g.FillRectangle(&base, 0, 0, w, H);
    for (int i = 0; i < 3; i++) {
        float bx = 90.f + i * 140, tx = bx + (i - 1) * 90.f;
        PointF tri[3] = { PointF(bx - 12, (float)H), PointF(bx + 12, (float)H), PointF(tx, -20.f) };
        GraphicsPath gp; gp.AddPolygon(tri, 3);
        PathGradientBrush pb(&gp);
        pb.SetCenterColor(Color(60, 220, 215, 190));
        Color sc(0, 220, 215, 190); int n = 1; pb.SetSurroundColors(&sc, &n);
        pb.SetCenterPoint(PointF(bx, H - 30.f));
        g.FillPath(&pb, &gp);
    }
    float cx = w / 2.0f, cy = 300;
    Pen ring(Color(200, 200, 181, 96), 6);
    g.DrawEllipse(&ring, cx - 120, cy - 120, 240.f, 240.f);
    Pen ring2(Color(90, 200, 181, 96), 2);
    g.DrawEllipse(&ring2, cx - 138, cy - 138, 276.f, 276.f);
    Star(g, cx, cy + 4, 92, Color(220, 200, 181, 96));
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    SolidBrush tb(Color(255, 236, 232, 216)), kb(Color(255, 200, 181, 96));
    Font f1(g_famHead, 40, FontStyleBold, UnitPixel), f2(g_famHead, 24, FontStyleBold, UnitPixel);
    g.DrawString(L"MEDAL OF HONOR", -1, &f1, RectF(0, 470, (float)w, 54), &sf, &tb);
    g.DrawString(L"EUROPEAN ASSAULT", -1, &f2, RectF(0, 520, (float)w, 34), &sf, &kb);
}

static void BuildCover()
{
    Bitmap src((g_dir + L"launcher\\art\\cover.jpg").c_str());
    Bitmap fb(465, H, PixelFormat32bppARGB);
    Bitmap *use = &src;
    if (src.GetLastStatus() != Ok) { PaintFallbackCover(fb); use = &fb; }
    g_coverW = H * (float)use->GetWidth() / (float)use->GetHeight();
    g_cover = MakeCover(*use, false);      /* English: the picture sits left of the page */
    g_coverR = MakeCover(*use, true);      /* Arabic: mirrored layout, the fade faces the other way */
}

/* ---------------------------------------------------------------- UI state */

enum { K_NONE, K_TAB, K_LANG, K_CLOSE, K_MIN, K_PLAY, K_CHOICE, K_TOGGLE, K_ACTION, K_LINK, K_OPENGAME, K_REPORT, K_SCROLL, K_MUSIC, K_KEYBOX, K_PADBOX, K_SYSFIX, K_UPDATE };
static int  g_cap = -1;               /* option index whose key box waits for a key press, -1: none */
static bool g_capAdd;                 /* Ctrl+click: the key is added as a second one */

/* Background music: the first of launcher\music\credits.wav / .mp3 / .wma, looped. Not shipped with the
 * package (it is the game's own track); a player drops any file with that name there. */
static void MusicStop()
{
    if (g_musicPlaying) mciSendStringW(L"close bgm", NULL, 0, NULL);
    g_musicPlaying = false;
}
static void MusicStart()
{
    static const wchar_t *const ext[] = { L"wav", L"mp3", L"wma" };
    MusicStop();
    g_musicHave = false;
    if (!g_musicOn) {
        for (auto e : ext) if (Exists(g_dir + L"launcher\\music\\credits." + e)) g_musicHave = true;
        return;
    }
    for (auto e : ext) {
        ws p = g_dir + L"launcher\\music\\credits." + e;
        if (!Exists(p)) continue;
        g_musicHave = true;
        if (mciSendStringW((L"open \"" + p + L"\" type mpegvideo alias bgm").c_str(), NULL, 0, NULL) != 0 &&
            mciSendStringW((L"open \"" + p + L"\" alias bgm").c_str(), NULL, 0, NULL) != 0)
            continue;
        mciSendStringW(L"setaudio bgm volume to 350", NULL, 0, NULL);
        g_musicPlaying = mciSendStringW(L"play bgm repeat", NULL, 0, NULL) == 0;
        if (!g_musicPlaying) mciSendStringW(L"close bgm", NULL, 0, NULL);
        break;
    }
}
struct Hit { RectF r; int kind; int a; int b; };
static std::vector<Hit> g_hits;

static int HitAt(float x, float y)
{
    for (int i = (int)g_hits.size() - 1; i >= 0; i--)
        if (g_hits[i].r.Contains(x, y)) return i;
    return -1;
}
static void AddHit(RectF r, int kind, int a = 0, int b = 0) { Hit h = { r, kind, a, b }; g_hits.push_back(h); }
static bool Hover(RectF r) { return r.Contains(g_mx, g_my); }

static ws Summary()
{
    ws s;
    Opt *res = OptByKey(L"res"), *fps = OptByKey(L"fps"), *lg = OptByKey(L"language");
    if (res) s += res->cur;
    if (fps) s += L"  ·  " + fps->cur + L" fps";
    if (lg) s += L"  ·  " + ValName(*lg);
    return s;
}

static void Button(Graphics &g, RectF r, const ws &text, bool primary, bool enabled, int kind, int a = 0, int b = 0, float px = 15)
{
    bool hv = enabled && Hover(r);
    Color top = !enabled ? Color(255, 56, 56, 50) : primary ? (hv ? REDH : RED) : (hv ? Color(255, 70, 74, 58) : Color(255, 46, 50, 38));
    Color bot = !enabled ? Color(255, 40, 40, 36) : primary ? (hv ? Color(255, 140, 28, 22) : Color(255, 110, 22, 18)) : Color(255, 32, 35, 27);
    GraphicsPath p; RRPath(p, r, 5);
    LinearGradientBrush lb(PointF(r.X, r.Y), PointF(r.X, r.Y + r.Height), top, bot);
    g.FillPath(&lb, &p);
    StrokeRR(g, !enabled ? Color(80, 120, 120, 110) : primary ? Color(220, 230, 150, 130) : Color(140, 200, 181, 96), r, 5, 1.2f);
    Txt(g, text, Fnt(px, true, primary), enabled ? TXT : DIM, r, 1);
    if (enabled) AddHit(r, kind, a, b);
}

static void Toast(const ws &s)
{
    g_toast = s;
    g_toastUntil = GetTickCount() + 4500;
    if (g_hwnd) SetTimer(g_hwnd, 7, 4600, NULL);
}

/* ---------------------------------------------------------------- tabs */

static void PaintSidebar(Graphics &g)
{
    FillRR(g, Color(170, 8, 9, 6), M(0, 0, SB, H), 0);
    Pen edge(Color(70, 200, 181, 96), 1);
    float ex = (float)(g_rtl ? W - SB : SB);
    g.DrawLine(&edge, ex, 0.f, ex, (float)H);

    /* emblem */
    RectF logo = M(0, 18, SB, 110);
    float cx = logo.X + SB / 2.0f;
    Pen ring(KH, 2.5f);
    g.DrawEllipse(&ring, cx - 30, 22.f, 60.f, 60.f);
    Star(g, cx, 53, 21, KH);
    Txt(g, T(L"app.title"), Fnt(15, true, true), TXT, RectF(logo.X, 86, SB, 20), 1);
    Txt(g, T(L"app.sub"), Fnt(11, false, true), KHD, RectF(logo.X, 105, SB, 16), 1);

    for (int i = 0; i < T_COUNT; i++) {
        RectF r = M(10, 150.f + i * 50, SB - 20, 44);
        bool sel = i == g_tab, hv = Hover(r);
        if (sel) {
            FillRR(g, Color(70, 200, 181, 96), r, 4);
            SolidBrush bar(KH);
            g.FillRectangle(&bar, g_rtl ? r.X + r.Width - 4 : r.X, r.Y + 6, 4.f, r.Height - 12);
        } else if (hv) FillRR(g, Color(34, 200, 181, 96), r, 4);
        wchar_t num[8]; swprintf(num, 8, L"0%d", i + 1);
        RectF nr = M(22, 150.f + i * 50, 34, 44);
        Txt(g, num, Fnt(12, false, false), sel ? KH : KHD, nr, 0);
        RectF tr = M(58, 150.f + i * 50, SB - 78, 44);
        Txt(g, T(ws(L"tab.") + TABKEY[i]), Fnt(17, true, true), sel ? TXT : DIM, tr, 0);
        AddHit(r, K_TAB, i);
    }

    /* language switch + version */
    for (int l = 0; l < 2; l++) {
        RectF r = M(16.f + l * 92, H - 92.f, 86, 30);
        bool sel = l == g_lang;
        FillRR(g, sel ? Color(160, 200, 181, 96) : (Hover(r) ? Color(60, 200, 181, 96) : Color(40, 255, 255, 255)), r, 4);
        Txt(g, l ? L"عربي" : L"English", Fnt(14, true), sel ? Color(255, 18, 20, 14) : DIM, r, 1);
        AddHit(r, K_LANG, l);
    }
    ws ver = PackageVersion();
    if (g_rtl) ver = ws(1, (wchar_t)0x202A) + ver + ws(1, (wchar_t)0x202C);     /* keep "0.9.0-beta" left-to-right inside Arabic text */
    Txt(g, T(L"app.version") + L" " + ver, Fnt(12), DIM, M(16, H - 50.f, SB - 68, 20), 0);
    if (g_musicHave) {                              /* speaker button: music on / muted */
        RectF b = M(SB - 48.f, H - 54.f, 30, 28);
        bool hv = Hover(b);
        FillRR(g, hv ? Color(70, 200, 181, 96) : Color(30, 255, 255, 255), b, 5);
        Color c = g_musicOn ? KH : DIM;
        SolidBrush sb(c);
        float x = b.X + 7, y = b.Y + 9;
        g.FillRectangle(&sb, x, y + 2, 5.f, 6.f);
        PointF cone[3] = { PointF(x + 5, y + 2), PointF(x + 11, y - 2), PointF(x + 11, y + 12) };
        PointF cone2[4] = { PointF(x + 5, y + 2), PointF(x + 11, y - 2), PointF(x + 11, y + 12), PointF(x + 5, y + 8) };
        g.FillPolygon(&sb, cone2, 4);
        (void)cone;
        Pen p(c, 1.6f);
        if (g_musicOn) {
            g.DrawArc(&p, x + 8, y + 1, 8.f, 8.f, -50.f, 100.f);
            g.DrawArc(&p, x + 6, y - 2, 14.f, 14.f, -50.f, 100.f);
        } else {
            Pen x2(BADC, 1.8f);
            g.DrawLine(&x2, x + 13, y + 1, x + 20, y + 9);
            g.DrawLine(&x2, x + 20, y + 1, x + 13, y + 9);
        }
        AddHit(b, K_MUSIC);
    }
}

static void PaintWinButtons(Graphics &g)
{
    RectF mn = M(W - 92.f, 8, 40, 28), cl = M(W - 48.f, 8, 40, 28);
    FillRR(g, Hover(mn) ? Color(80, 255, 255, 255) : Color(0, 0, 0, 0), mn, 4);
    FillRR(g, Hover(cl) ? Color(220, 190, 40, 30) : Color(0, 0, 0, 0), cl, 4);
    Pen p(TXT, 1.6f);
    g.DrawLine(&p, mn.X + 13, mn.Y + 15, mn.X + 27, mn.Y + 15);
    g.DrawLine(&p, cl.X + 14, cl.Y + 8, cl.X + 26, cl.Y + 20);
    g.DrawLine(&p, cl.X + 26, cl.Y + 8, cl.X + 14, cl.Y + 20);
    AddHit(mn, K_MIN);
    AddHit(cl, K_CLOSE);
}

static void StatusLine(Graphics &g, float x, float y, float w, const ws &label, bool ok, const ws &state, float sw = 130)
{
    SolidBrush dot(ok ? OKC : BADC);
    RectF d = M(x, y + 8, 12, 12);
    g.FillEllipse(&dot, d);
    Txt(g, label, Fnt(15), TXT, M(x + 24, y, w - 24 - sw, 28), 0);
    Txt(g, state, Fnt(14, true), ok ? OKC : BADC, M(x + w - sw, y, sw, 28), 2);
}

static void PaintPlay(Graphics &g)
{
    if (g_cover) {
        RectF r = M((float)SB, 0, g_coverW, (float)H);
        g.DrawImage(g_rtl && g_coverR ? g_coverR : g_cover, r);
    }
    float px = SB + g_coverW + 6, pw = W - px - 40;
    Txt(g, T(L"app.tag"), Fnt(14, true, true), KH, M(px, 52, pw, 20), 0);
    Txt(g, T(L"play.sub"), Fnt(25, true, true), TXT, M(px, 72, pw, 66), 0, 0, true);
    Txt(g, T(L"play.edition"), Fnt(13), DIM, M(px, 140, pw, 24), 0, 0, true);

    FillRR(g, Color(110, 8, 9, 6), M(px - 8, 168, pw + 16, 220), 6);
    StrokeRR(g, Color(50, 200, 181, 96), M(px - 8, 168, pw + 16, 220), 6);
    Txt(g, T(L"play.files"), Fnt(13, true, true), KH, M(px + 8, 176, pw - 16, 22), 0);
    float y = 204;
    StatusLine(g, px + 8, y, pw - 16, T(L"play.f.exe"), g_hasExe, T(g_hasExe ? L"play.ok" : L"play.missing")); y += 30;
    StatusLine(g, px + 8, y, pw - 16, T(L"play.f.xbe"), g_hasXbe, T(g_hasXbe ? L"play.ok" : L"play.missing")); y += 30;
    StatusLine(g, px + 8, y, pw - 16, T(L"play.f.data"), g_hasData, T(g_hasData ? L"play.ok" : L"play.missing")); y += 30;
    StatusLine(g, px + 8, y, pw - 16, T(L"play.f.eagl"), g_hasEagl, T(g_hasEagl ? L"play.ok" : L"play.missing")); y += 30;
    ws vs = g_ver == 1 ? T(L"play.ver.usa") : g_ver == 2 ? T(L"play.ver.bad") : T(L"play.ver.unk");
    StatusLine(g, px + 8, y, pw - 16, T(L"play.f.ver"), g_ver == 1, g_ver == 1 ? ws(L"USA / NTSC-U") : vs, g_ver == 1 ? 130.f : 260.f);
    if (g_ver == 1) {
        /* the longer wording only when it does not fit on the line */
    }

    if (!g_missingDlls.empty()) {
        ws names;
        for (auto &n : g_missingDlls) { if (!names.empty()) names += L", "; names += n; }
        Txt(g, T(L"sys.missing") + L" " + names, Fnt(13), Color(255, 232, 170, 140), M(px, 400, pw, 40), 0, 0, true);
        RectF fb = M(px, 440, pw, 34);
        Button(g, fb, T(g_missingKind == 0 ? L"sys.fix.vc" : g_missingKind == 1 ? L"sys.fix.mf" : L"sys.fix.win"), false, true, K_SYSFIX, 0, 0, 13);
    } else if (g_ready) {
        Txt(g, T(L"play.summary"), Fnt(13, true, true), KH, M(px, 404, pw, 20), 0);
        Txt(g, Summary(), Fnt(16), TXT, M(px, 426, pw, 26), 0);
    } else {
        Txt(g, T(L"play.help") + L"\n" + T(L"play.help2"), Fnt(13), Color(255, 232, 170, 140), M(px, 400, pw, 70), 0, 0, true);
    }

    RectF pb = M(px, 478, pw, 88);
    Button(g, pb, T(g_ready ? L"play.btn" : L"play.btn.blocked"), true, g_ready, K_PLAY, 0, 0, g_ready ? 38.f : 20.f);
    if (g_ready) {
        /* a thin khaki rim for the main button */
        StrokeRR(g, Color(120, 200, 181, 96), RectF(pb.X - 3, pb.Y - 3, pb.Width + 6, pb.Height + 6), 8, 1);
    }
    float hw = (pw - 12) / 2;
    Button(g, M(px, 584, hw, 38), T(L"play.open_game"), false, true, K_OPENGAME, 0, 0, 14);
    Button(g, M(px + hw + 12, 584, hw, 38), T(L"play.report"), false, true, K_REPORT, 0, 0, 14);
    if (g_updFound) {
        RectF ur = M(px, 634, pw, 30);
        bool hot = Hover(ur);
        Txt(g, T(L"update.avail") + L" " + g_updTag, Fnt(14, true), hot ? KHD : KH, ur, 0);
        AddHit(ur, K_UPDATE);
    }
}

static void ActionName(const Opt &o, ws &btn)
{
    btn = HasT(o.key + L".btn") ? T(o.key + L".btn") : T(L"link.btn");
}

static void PaintOptions(Graphics &g, int tab)
{
    Txt(g, T(ws(L"tab.") + TABKEY[tab]), Fnt(34, true, true), TXT, M(SB + 36.f, 30, 700, 46), 0);
    Txt(g, T(ws(L"tab.") + TABKEY[tab] + L".sub"), Fnt(14), DIM, M(SB + 36.f, 78, 800, 24), 0);
    Pen ul(Color(160, 168, 36, 28), 3);
    RectF ur = M(SB + 36.f, 108, 70, 3);
    g.DrawLine(&ul, ur.X, ur.Y, ur.X + ur.Width, ur.Y);

    const float top = 128, rowH = 68, gap = 8;
    float listW = W - SB - 72.f;
    GraphicsState st = g.Save();
    g.SetClip(M((float)SB, top - 4, (float)(W - SB), (float)(H - top + 4)));
    float y = top - g_scroll[tab];
    for (size_t i = 0; i < g_opts.size(); i++) {
        Opt &o = g_opts[i];
        if (o.tabIdx != tab) continue;
        RectF row = M(SB + 36.f, y, listW, rowH);
        bool vis = y + rowH > top - 4 && y < H;
        if (vis) {
            FillRR(g, Color(120, 10, 12, 8), row, 6);
            StrokeRR(g, Color(34, 200, 181, 96), row, 6);
            float tw = listW - 20 - 270;
            ws d = T(o.key + L".d");
            bool hasD = d != o.key + L".d";
            Txt(g, T(o.key), Fnt(17, true), OptOff(o) ? DIM : TXT, M(SB + 36.f + 20, hasD ? y + 9 : y + 22, tw, 24), 0);
            if (d != o.key + L".d") Txt(g, d, Fnt(12), DIM, M(SB + 36.f + 20, y + 33, tw, 30), 0, 0, true);
            float cx = SB + 36.f + listW - 20 - 230;
            if (o.type == L"choice" || o.type == L"lang") {
                bool off = OptOff(o);
                RectF c = M(cx, y + (rowH - 36) / 2, 230, 36);
                FillRR(g, Color(220, 22, 24, 18), c, 5);
                StrokeRR(g, Hover(c) ? Color(200, 200, 181, 96) : Color(110, 200, 181, 96), c, 5);
                Txt(g, ValName(o), Fnt(15, true), off ? DIM : KH, M(cx + 34, y + (rowH - 36) / 2, 162, 36), 1);
                RectF la(c.X, c.Y, 36, 36), ra(c.X + c.Width - 36, c.Y, 36, 36);
                Txt(g, L"‹", Fnt(26, true), Hover(la) ? TXT : KHD, la, 1);
                Txt(g, L"›", Fnt(26, true), Hover(ra) ? TXT : KHD, ra, 1);
                if (vis && !off) { AddHit(la, K_CHOICE, (int)i, -1); AddHit(ra, K_CHOICE, (int)i, 1); AddHit(RectF(c.X + 36, c.Y, c.Width - 72, c.Height), K_CHOICE, (int)i, 1); }
            } else if (o.type == L"toggle") {
                RectF c = M(cx + 230 - 64, y + (rowH - 30) / 2, 64, 30);
                bool on = o.cur == L"1";
                FillRR(g, on ? Color(255, 150, 138, 80) : Color(255, 40, 42, 34), c, 15);
                StrokeRR(g, Hover(c) ? Color(220, 200, 181, 96) : Color(110, 200, 181, 96), c, 15);
                float kx = g_rtl ? (on ? c.X + 4 : c.X + c.Width - 26) : (on ? c.X + c.Width - 26 : c.X + 4);
                SolidBrush kb(on ? Color(255, 238, 232, 200) : Color(255, 130, 128, 112));
                g.FillEllipse(&kb, kx, c.Y + 4, 22.f, 22.f);
                Txt(g, T(on ? L"on" : L"off"), Fnt(14, true), on ? KH : DIM, M(cx, y + (rowH - 30) / 2, 150, 30), 2);
                AddHit(RectF(c.X - 160, row.Y, c.Width + 160, rowH), K_TOGGLE, (int)i);
            } else if (o.type == L"bind" || o.type == L"movekey") {
                ws kb, pad;
                if (o.type == L"bind") BindSplit(o.cur, kb, pad); else kb = o.cur;
                float right = cx + 230, padW = o.type == L"bind" ? 110.f : 0.f;
                float kbW = 210, ky = y + (rowH - 36) / 2;
                RectF kc = M(right - padW - (padW > 0 ? 8.f : 0.f) - kbW, ky, kbW, 36);
                bool cap = g_cap == (int)i;
                FillRR(g, Color(220, 22, 24, 18), kc, 5);
                StrokeRR(g, cap ? Color(255, 236, 220, 120) : Hover(kc) ? Color(200, 200, 181, 96) : Color(110, 200, 181, 96), kc, 5);
                Txt(g, cap ? T(L"bind.press") : KeysText(kb), Fnt(14, true), cap ? TXT : KH, kc, 1);
                AddHit(kc, K_KEYBOX, (int)i, 0);
                if (padW > 0) {
                    RectF pc = M(right - padW, ky, padW, 36);
                    FillRR(g, Color(220, 22, 24, 18), pc, 5);
                    StrokeRR(g, Hover(pc) ? Color(200, 200, 181, 96) : Color(110, 200, 181, 96), pc, 5);
                    Txt(g, PadText(pad), Fnt(14, true), KH, pc, 1);
                    AddHit(pc, K_PADBOX, (int)i, 1);
                }
            } else if (o.type == L"action" || o.type == L"link") {
                ws bt; ActionName(o, bt);
                Button(g, M(cx + 230 - 150, y + (rowH - 36) / 2, 150, 36), bt, false, true, o.type == L"link" ? K_LINK : K_ACTION, (int)i, 0, 14);
            }
        }
        y += rowH + gap;
    }
    g_maxScroll = max(0.0f, (y + g_scroll[tab]) - (float)(H - 24) );
    if (g_scroll[tab] > g_maxScroll) g_scroll[tab] = g_maxScroll;
    g.Restore(st);

    /* the scroll bar: drag the thumb, or click the track */
    g_trackR = g_thumbR = RectF(0, 0, 0, 0);
    if (g_maxScroll > 0) {
        float trackY = top, trackH = H - top - 14.f;
        float thumbH = max(40.0f, trackH * trackH / (trackH + g_maxScroll));
        float thumbY = trackY + (trackH - thumbH) * (g_scroll[tab] / g_maxScroll);
        g_trackR = M(W - 24.f, trackY, 14, trackH);
        g_thumbR = M(W - 24.f, thumbY, 14, thumbH);
        FillRR(g, Color(90, 0, 0, 0), g_trackR, 7);
        bool act = g_dragBar || Hover(g_thumbR);
        FillRR(g, act ? Color(235, 200, 181, 96) : Color(170, 150, 138, 80), g_thumbR, 7);
        AddHit(RectF(g_trackR.X - 4, g_trackR.Y, g_trackR.Width + 8, g_trackR.Height), K_SCROLL);
    }
}

static void Paint(Graphics &g)
{
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g_hits.clear();
    g.DrawImage(g_bg[g_tab], 0, 0, W, H);
    if (g_tab == T_PLAY) PaintPlay(g); else PaintOptions(g, g_tab);
    PaintSidebar(g);
    PaintWinButtons(g);
    if (!g_toast.empty() && (GetTickCount() < g_toastUntil || !g_hwnd)) {
        RectF tr = M(SB + (W - SB) / 2.0f - 200, H - 64.f, 400, 40);
        FillRR(g, Color(235, 18, 20, 14), tr, 6);
        StrokeRR(g, Color(200, 200, 181, 96), tr, 6);
        Txt(g, g_toast, Fnt(14, true), TXT, tr, 1);
    }
    Pen border(Color(120, 200, 181, 96), 1);
    g.DrawRectangle(&border, 0, 0, W - 1, H - 1);
}

/* ---------------------------------------------------------------- actions */

static void OpenPath(const ws &p, bool create)
{
    if (create && !Exists(p)) CreateDirectoryW(p.c_str(), NULL);
    if (!Exists(p)) { Toast(T(L"nofolder")); return; }
    ShellExecuteW(NULL, L"open", p.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

static ws BackupsDir() { return EnvW(L"LOCALAPPDATA") + L"\\xboxrecomp_backups"; }

static void DoBackup()
{
    ws saves = SavesDir();
    if (!IsDir(saves)) { Toast(T(L"nofolder")); return; }
    CreateDirectoryW(BackupsDir().c_str(), NULL);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"\\saves_%04d%02d%02d_%02d%02d%02d.zip", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    ws zip = BackupsDir() + name;
    ws cmd = L"tar.exe -a -c -f \"" + zip + L"\" -C \"" + saves + L"\"";
    int n = 0;
    const wchar_t *dirs[] = { L"UDATA", L"TDATA" };
    for (auto d : dirs) if (IsDir(saves + L"\\" + d)) { cmd += L" "; cmd += d; n++; }
    if (!n) { Toast(T(L"nofolder")); return; }
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 120000);
        DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        Toast(code == 0 ? T(L"backup.ok") : T(L"backup.fail"));
        return;
    }
    Toast(T(L"backup.fail"));
}

static ws ReportUrl();
static bool CopyInfo(bool withLog)
{
    ws s = L"MoH European Assault PC port " + PackageVersion() + L"\r\n";
    OSVERSIONINFOEXW vi = { sizeof vi };
    typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOEXW *);
    RtlGetVersionFn rgv = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    wchar_t b[128];
    if (rgv && rgv(&vi) == 0) { swprintf(b, 128, L"Windows %lu.%lu build %lu\r\n", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber); s += b; }
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    swprintf(b, 128, L"CPU threads: %lu\r\n", si.dwNumberOfProcessors); s += b;
    {
        DISPLAY_DEVICEW dd = { sizeof dd };
        if (EnumDisplayDevicesW(NULL, 0, &dd, 0)) s += L"GPU: " + ws(dd.DeviceString) + L"\r\n";
        POINT pt = { 0, 0 };
        MONITORINFO mi = { sizeof mi };
        if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            swprintf(b, 128, L"Screen: %ldx%ld\r\n", mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
            s += b;
        }
        for (auto &d : g_missingDlls) s += L"MISSING system file: " + d + L"\r\n";
    }
    s += L"Game files: xbe=" + ws(g_hasXbe ? L"yes" : L"NO") + L" data=" + (g_hasData ? L"yes" : L"NO") + L" eagl=" + (g_hasEagl ? L"yes" : L"NO") +
         L" version=" + (g_ver == 1 ? L"USA" : g_ver == 2 ? L"OTHER" : L"?") + L"\r\n\r\n--- recomp_settings.ini ---\r\n";
    for (auto &l : g_ini) { if (l.compare(0, 3, "kb_") && l.compare(0, 4, "pad_")) s += U8(l) + L"\r\n"; }
    std::string log;
    if (withLog && ReadAll(g_dir + L"play_log.txt", log)) {
        auto lines = SplitLines(log);
        s += L"\r\n--- play_log.txt (last 80 lines) ---\r\n";
        for (size_t i = lines.size() > 80 ? lines.size() - 80 : 0; i < lines.size(); i++) s += U8(lines[i]) + L"\r\n";
    }
    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * 2);
        if (h) {
            memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * 2);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
        CloseClipboard();
        if (withLog) Toast(T(L"copied"));
        return true;
    }
    return false;
}

/* "Report a problem": the system info (no log: an issue body is limited to about 65,000 characters) goes to the
 * clipboard, Explorer opens with play_log.txt selected to drag into the issue, and the new-issue page opens. */
static void ReportProblem()
{
    bool ok = CopyInfo(false);
    ws log = g_dir + L"play_log.txt";
    if (Exists(log)) {
        ws arg = L"/select,\"" + log + L"\"";
        ShellExecuteW(NULL, L"open", L"explorer.exe", arg.c_str(), NULL, SW_SHOWNORMAL);
    }
    ws url = ReportUrl();
    if (url.size() >= 7 && url.compare(url.size() - 7, 7, L"/issues") == 0) url += L"/new";
    ShellExecuteW(NULL, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
    if (ok) Toast(T(L"report.copied"));
}

static void DoAction(const ws &k)
{
    if (k == L"open_mods") OpenPath(g_dir + L"mods", true);
    else if (k == L"open_textures") { CreateDirectoryW((g_dir + L"mods").c_str(), NULL); OpenPath(g_dir + L"mods\\textures", true); }
    else if (k == L"open_sounds") { CreateDirectoryW((g_dir + L"mods").c_str(), NULL); OpenPath(g_dir + L"mods\\sounds", true); }
    else if (k == L"open_languages") OpenPath(g_dir + L"languages", true);
    else if (k == L"open_log") OpenPath(g_dir + L"play_log.txt", false);
    else if (k == L"copy_info") CopyInfo(true);
    else if (k == L"preset_safe") { ApplyPreset(false); Toast(T(L"preset.done")); }
    else if (k == L"preset_high") { ApplyPreset(true); Toast(T(L"preset.done")); }
    else if (k == L"open_screenshots") OpenPath(g_dir + L"screenshots", true);
    else if (k == L"open_saves") OpenPath(SavesDir(), false);
    else if (k == L"backup_saves") DoBackup();
    else if (k == L"open_backups") OpenPath(BackupsDir(), true);
    else if (k == L"reset_controls") {
        for (auto &o : g_opts) if (o.type == L"bind" || o.type == L"movekey") o.cur = o.def;
        Toast(T(L"controls_reset"));
    }
}

static ws ReportUrl() { Opt *o = OptByKey(L"report"); return o ? o->def : L"https://github.com/"; }

static void PlayGame()
{
    if (!g_ready) return;
    SaveIni();
    ws exe = g_dir + L"MOHEuropeanAssault.exe";
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    std::vector<wchar_t> cmd(exe.begin(), exe.end());
    cmd.insert(cmd.begin(), L'"'); cmd.push_back(L'"'); cmd.push_back(0);
    if (CreateProcessW(exe.c_str(), cmd.data(), NULL, NULL, FALSE, 0, NULL, g_dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        PostQuitMessage(0);
    } else {
        Toast(T(L"play.fail"));
    }
}

static void PadStep(Opt &o, int d)
{
    ws kb, pad;
    BindSplit(o.cur, kb, pad);
    std::vector<ws> pl = SplitBy(pad, L',');
    ws first = pl.empty() || pl[0].empty() ? ws(L"none") : pl[0];
    int i = 0;
    while (i < PADN && first != PADID[i]) i++;
    if (i >= PADN) i = PADN - 1;
    i = (i + d + PADN) % PADN;
    if (pl.empty()) pl.push_back(PADID[i]); else pl[0] = PADID[i];
    if (pl[0] == L"none") pl.erase(pl.begin());
    ws np;
    for (auto &x : pl) { if (!np.empty()) np += L","; np += x; }
    if (np.empty()) np = L"none";
    o.cur = kb + L";" + np;
}

static void BindKey(const ws &id)
{
    if (g_cap < 0 || g_cap >= (int)g_opts.size()) { g_cap = -1; return; }
    if (id == L"f1" || id == L"f11" || id == L"f12") { Toast(T(L"key.reserved")); return; }   /* settings / fullscreen / screenshot */
    Opt &o = g_opts[g_cap];
    if (o.type == L"movekey") {
        if (id.size() == 1 || id.size() >= 3) o.cur = id;     /* movement keys are keyboard keys, not the mouse */
    } else {
        ws kb, pad;
        BindSplit(o.cur, kb, pad);
        bool add = g_capAdd && o.key != L"bind_sprint" && kb != L"none" && !kb.empty();
        if (add) {
            std::vector<ws> ks = SplitBy(kb, L',');
            if (std::find(ks.begin(), ks.end(), id) == ks.end() && ks.size() < 3) kb += L"," + id;
        } else kb = id;
        o.cur = kb + L";" + pad;
    }
    g_cap = -1;
}

static ws VkToId(WPARAM vk, LPARAM lp)
{
    UINT scan = (lp >> 16) & 0xFF;
    bool ext = ((lp >> 24) & 1) != 0;
    if (vk == VK_SHIFT) vk = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
    else if (vk == VK_CONTROL) vk = ext ? VK_RCONTROL : VK_LCONTROL;
    else if (vk == VK_MENU) vk = ext ? VK_RMENU : VK_LMENU;
    if (vk >= 'A' && vk <= 'Z') return ws(1, (wchar_t)(vk + 32));
    if (vk >= '0' && vk <= '9') return ws(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F12) return L"f" + std::to_wstring((int)(vk - VK_F1 + 1));
    static const struct { UINT vk; const wchar_t *n; } nm[] = {
        { VK_SPACE, L"space" }, { VK_RETURN, L"enter" }, { VK_TAB, L"tab" }, { VK_BACK, L"backspace" },
        { VK_LSHIFT, L"lshift" }, { VK_RSHIFT, L"rshift" }, { VK_LCONTROL, L"lctrl" }, { VK_RCONTROL, L"rctrl" },
        { VK_LMENU, L"lalt" }, { VK_RMENU, L"ralt" }, { VK_CAPITAL, L"capslock" }, { VK_UP, L"up" }, { VK_DOWN, L"down" },
        { VK_LEFT, L"left" }, { VK_RIGHT, L"right" }, { VK_INSERT, L"insert" }, { VK_DELETE, L"delete" }, { VK_HOME, L"home" },
        { VK_END, L"end" }, { VK_PRIOR, L"pageup" }, { VK_NEXT, L"pagedown" } };
    for (auto &n : nm) if (n.vk == vk) return n.n;
    wchar_t b[16];
    swprintf(b, 16, L"vk%02x", (unsigned)vk);
    return b;
}

static void Click(int idx)
{
    if (idx < 0) return;
    Hit h = g_hits[idx];
    switch (h.kind) {
    case K_TAB: g_tab = h.a; g_cap = -1; break;
    case K_LANG:
        g_lang = h.a; g_rtl = g_lang == 1;
        IniSet("launcher_lang", g_lang ? "ar" : "en");
        break;
    case K_MUSIC: g_musicOn = !g_musicOn; MusicStart(); break;
    case K_CLOSE: SaveIni(); PostQuitMessage(0); break;
    case K_MIN: ShowWindow(g_hwnd, SW_MINIMIZE); break;
    case K_PLAY: PlayGame(); break;
    case K_OPENGAME: OpenPath(g_dir + L"game", true); break;
    case K_REPORT: ReportProblem(); break;
    case K_SYSFIX: ShellExecuteW(NULL, L"open", SysFixUrl().c_str(), NULL, NULL, SW_SHOWNORMAL); break;
    case K_UPDATE: if (!g_updUrl.empty()) ShellExecuteW(NULL, L"open", g_updUrl.c_str(), NULL, NULL, SW_SHOWNORMAL); break;
    case K_CHOICE: {
        Opt &o = g_opts[h.a];
        int n = (int)o.vals.size(), i = FindIdx(o, o.cur);
        if (n) o.cur = o.vals[((i < 0 ? 0 : i) + h.b + n) % n];
        break;
    }
    case K_TOGGLE: { Opt &o = g_opts[h.a]; o.cur = o.cur == L"1" ? L"0" : L"1"; break; }
    case K_KEYBOX: g_cap = h.a; g_capAdd = GetKeyState(VK_CONTROL) < 0; break;
    case K_PADBOX: PadStep(g_opts[h.a], h.b); break;
    case K_ACTION: DoAction(g_opts[h.a].key); break;
    case K_LINK: ShellExecuteW(NULL, L"open", g_opts[h.a].def.c_str(), NULL, NULL, SW_SHOWNORMAL); break;
    }
}

/* ---------------------------------------------------------------- window */

static void ToLogical(LPARAM lp, float &x, float &y)
{
    x = (float)(short)LOWORD(lp) / g_scale;
    y = (float)(short)HIWORD(lp) / g_scale;
}

static LRESULT CALLBACK WndProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        RECT rc; GetClientRect(hw, &rc);
        int pw = rc.right, ph = rc.bottom;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, pw, ph);
        HGDIOBJ old = SelectObject(mem, bmp);
        {
            Graphics g(mem);
            g.ScaleTransform(g_scale, g_scale);
            Paint(g);
        }
        BitBlt(dc, 0, 0, pw, ph, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(hw, &pt);
        float x = pt.x / g_scale, y = pt.y / g_scale;
        if (y < 44) {
            for (auto &h : g_hits) if ((h.kind == K_MIN || h.kind == K_CLOSE) && h.r.Contains(x, y)) return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        ToLogical(lp, g_mx, g_my);
        if (g_dragBar) {
            float trackY = g_trackR.Y, trackH = g_trackR.Height, thumbH = g_thumbR.Height;
            float t = (g_my - g_dragOff - trackY) / std::max(1.0f, trackH - thumbH);
            g_scroll[g_tab] = std::min(g_maxScroll, std::max(0.0f, t * g_maxScroll));
        } else if (g_pan) {
            g_scroll[g_tab] = std::min(g_maxScroll, std::max(0.0f, g_panS0 - (g_my - g_panY0)));
        }
        TRACKMOUSEEVENT te = { sizeof te, TME_LEAVE, hw, 0 };
        TrackMouseEvent(&te);
        InvalidateRect(hw, NULL, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE: g_mx = g_my = -1; InvalidateRect(hw, NULL, FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, HitAt(g_mx, g_my) >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_cap >= 0) {
            if (wp == VK_ESCAPE) g_cap = -1; else BindKey(VkToId(wp, lp));
            InvalidateRect(hw, NULL, FALSE);
            return 0;
        }
        break;
    case WM_RBUTTONDOWN: {
        float x, y; ToLogical(lp, x, y);
        if (g_cap >= 0) { BindKey(L"mouse2"); InvalidateRect(hw, NULL, FALSE); return 0; }
        int hi = HitAt(x, y);
        if (hi >= 0 && (g_hits[hi].kind == K_KEYBOX || g_hits[hi].kind == K_PADBOX)) {   /* right click: back to the default / previous button */
            Opt &o = g_opts[g_hits[hi].a];
            if (g_hits[hi].kind == K_PADBOX) PadStep(o, -1);
            else if (o.type == L"movekey") o.cur = o.def;
            else { ws kb, pad, dkb, dpad; BindSplit(o.cur, kb, pad); BindSplit(o.def, dkb, dpad); o.cur = dkb + L";" + pad; }
            InvalidateRect(hw, NULL, FALSE);
        }
        return 0;
    }
    case WM_MBUTTONDOWN:
        if (g_cap >= 0) { BindKey(L"mouse3"); InvalidateRect(hw, NULL, FALSE); }
        return 0;
    case WM_XBUTTONDOWN:
        if (g_cap >= 0) { BindKey(HIWORD(wp) == XBUTTON1 ? L"mouse4" : L"mouse5"); InvalidateRect(hw, NULL, FALSE); }
        return TRUE;
    case WM_LBUTTONDOWN: {
        float x, y; ToLogical(lp, x, y);
        int hi = HitAt(x, y);
        if (g_cap >= 0) {            /* waiting for a key: a left click on that same box binds the left mouse button, anywhere else cancels */
            if (hi >= 0 && g_hits[hi].kind == K_KEYBOX && g_hits[hi].a == g_cap) BindKey(L"mouse1"); else g_cap = -1;
            InvalidateRect(hw, NULL, FALSE);
            return 0;
        }
        if (hi >= 0 && g_hits[hi].kind == K_SCROLL) {          /* press on the bar: drag it (a click on the track jumps there) */
            g_dragBar = true;
            g_dragOff = g_thumbR.Contains(x, y) ? y - g_thumbR.Y : g_thumbR.Height / 2;
            SetCapture(hw);
            float t = (y - g_dragOff - g_trackR.Y) / std::max(1.0f, g_trackR.Height - g_thumbR.Height);
            g_scroll[g_tab] = std::min(g_maxScroll, std::max(0.0f, t * g_maxScroll));
        } else if (hi < 0 && g_tab != T_PLAY && y > 100 && g_maxScroll > 0) {   /* press on empty space: drag the page itself */
            g_pan = true;
            g_panY0 = y;
            g_panS0 = g_scroll[g_tab];
            SetCapture(hw);
        } else {
            Click(hi);
        }
        InvalidateRect(hw, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_dragBar || g_pan) { g_dragBar = g_pan = false; ReleaseCapture(); InvalidateRect(hw, NULL, FALSE); }
        return 0;
    case WM_CAPTURECHANGED: g_dragBar = g_pan = false; return 0;
    case WM_MOUSEWHEEL: {
        if (g_cap >= 0) { BindKey(L"wheel"); InvalidateRect(hw, NULL, FALSE); return 0; }
        float d = (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        g_scroll[g_tab] = std::min(g_maxScroll, std::max(0.0f, g_scroll[g_tab] - d * 60));
        InvalidateRect(hw, NULL, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (wp == 7) { KillTimer(hw, 7); g_toast.clear(); InvalidateRect(hw, NULL, FALSE); }
        return 0;
    case WM_DPICHANGED: {
        g_scale = HIWORD(wp) / 96.0f;
        RECT *r = (RECT *)lp;
        SetWindowPos(hw, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

static bool SavePng(Bitmap &bm, const ws &path)
{
    UINT n = 0, sz = 0;
    GetImageEncodersSize(&n, &sz);
    std::vector<uint8_t> buf(sz);
    ImageCodecInfo *ci = (ImageCodecInfo *)buf.data();
    GetImageEncoders(n, sz, ci);
    for (UINT i = 0; i < n; i++)
        if (!wcscmp(ci[i].MimeType, L"image/png")) return bm.Save(path.c_str(), &ci[i].Clsid, NULL) == Ok;
    return false;
}

static void Shots(const ws &dir)
{
    CreateDirectoryW(dir.c_str(), NULL);
    for (int l = 0; l < 2; l++) {
        g_lang = l; g_rtl = l == 1;
        for (int t = 0; t < T_COUNT; t++) {
            g_tab = t;
            for (int t = 0; t < T_COUNT; t++) g_scroll[t] = g_shotScroll;
            Bitmap bm(W, H, PixelFormat32bppPARGB);
            { Graphics g(&bm); Paint(g); }
            wchar_t nm[64];
            swprintf(nm, 64, L"\\%s_%d_%s.png", l ? L"ar" : L"en", t, TABKEY[t]);
            SavePng(bm, dir + nm);
        }
    }
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int)
{
    typedef BOOL (WINAPI *SetCtx)(HANDLE);
    SetCtx sc = (SetCtx)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (sc) sc((HANDLE)-4); else SetProcessDPIAware();

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    g_dir = exe;
    g_dir = g_dir.substr(0, g_dir.find_last_of(L'\\') + 1);

    ULONG_PTR tok;
    GdiplusStartupInput gi;
    GdiplusStartup(&tok, &gi, NULL);

    FontFamily *hd = new FontFamily(L"Bahnschrift");
    if (!hd->IsAvailable()) { delete hd; hd = new FontFamily(L"Segoe UI"); }
    g_famHead = hd;
    g_famBody = new FontFamily(L"Segoe UI");

    LoadStrings(0, L"en.txt");
    LoadStrings(1, L"ar.txt");
    LoadOptions();
    LoadIni();
    CheckSystem();
    CheckGame();
    for (int i = 0; i < 3; i++) {
        wchar_t nm[64]; swprintf(nm, 64, L"launcher\\art\\blood_0%d.png", i * 2 + 1);
        Bitmap *b = new Bitmap((g_dir + nm).c_str());
        g_blood[i] = b->GetLastStatus() == Ok ? b : NULL;
    }
    BuildCover();
    for (int t = 0; t < T_COUNT; t++) BuildBackdrop(t);

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3 && !wcscmp(argv[1], L"--shot")) {
        g_musicHave = Exists(g_dir + L"launcher\\music\\credits.wav") || Exists(g_dir + L"launcher\\music\\credits.mp3");
        if (argc >= 4) g_shotScroll = (float)_wtof(argv[3]);
        Shots(argv[2]);
        GdiplusShutdown(tok);
        return 0;
    }

    {   /* update check: only in a real window run, and only when the player has not switched it off */
        Opt *uc = OptByKey(L"check_updates");
        if (!uc || uc->cur != L"0") {
            HANDLE th = CreateThread(NULL, 0, UpdateThread, NULL, 0, NULL);
            if (th) CloseHandle(th);
        }
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"MohLauncher";
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);

    HWND tmp = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, hi, NULL);
    g_scale = GetDpiForWindow(tmp) / 96.0f;
    DestroyWindow(tmp);
    int ww = (int)(W * g_scale), wh = (int)(H * g_scale);
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"MohLauncher", L"Medal of Honor: European Assault - Launcher",
                             WS_POPUP | WS_MINIMIZEBOX | WS_VISIBLE, (sx - ww) / 2, (sy - wh) / 2, ww, wh, NULL, NULL, hi, NULL);
    MusicStart();
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    MusicStop();
    GdiplusShutdown(tok);
    return 0;
}
