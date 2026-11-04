"""Builds the clean player package (what goes to the GitHub repo / release zip).

  python make_package.py [version]        -> ..\\release\\MOH-EuropeanAssault-PC-Port-<version>\\ + .zip + .sha256

Nothing of the private working folder gets in: no game files, logs, pdb, backups, tex_dump, shader cache, saves,
sources of the closed parts, dev .bat files.
"""
import hashlib
import os
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOLKIT = os.path.join(ROOT, "xboxrecomp-0.16.0")
VERSION = sys.argv[1] if len(sys.argv) > 1 else "0.9.1-beta"
NAME = "MOH-EuropeanAssault-PC-Port-" + VERSION
OUT = os.path.join(ROOT, "release", NAME)
DOCS = os.path.join(HERE, "package_docs")

XEMU_FILES = [
    "src/apu/apu.h", "src/apu/apu_core.c", "src/apu/apu_dsp.c", "src/apu/apu_vp.c", "src/apu/apu_regs.h",
    "src/apu/apu_state.h", "src/apu/apu_debug.h", "src/apu/fpconv.h",
    "src/nv2a/nv2a_regs.h", "src/nv2a/nv2a_core.c", "src/nv2a/nv2a_state.h",
]


def put(src, dst_rel):
    dst = os.path.join(OUT, dst_rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def put_tree(src_dir, dst_rel, skip=()):
    for dp, dn, fn in os.walk(src_dir):
        rel = os.path.relpath(dp, src_dir)
        for f in fn:
            if f in skip:
                continue
            put(os.path.join(dp, f), os.path.normpath(os.path.join(dst_rel, rel, f)))


def default_ini():
    """recomp_settings.ini with every option at its launcher default (options.txt)"""
    lines = []
    fov = draw = None
    with open(os.path.join(HERE, "launcher", "options.txt"), encoding="utf-8") as f:
        for l in f:
            l = l.rstrip("\r\n")
            if not l or l.startswith("#"):
                continue
            p = l.split("|")
            if len(p) < 4:
                continue
            tab, key, typ, dflt = p[0], p[1], p[2], p[3]
            if key == "@fov":
                fov = dflt
            elif key == "@draw":
                draw = dflt
            elif typ in ("choice", "toggle", "lang") and not key.startswith("@"):
                lines.append("%s=%s" % (key, dflt))
    # first_run=1: the launcher picks the picture size from the monitor once, then removes the line
    lines += ["menu_lang=english", "launcher_lang=en", "launcher_music=1", "first_run=1"]
    return "\r\n".join(lines) + "\r\n", fov, draw


def main():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)

    put(os.path.join(HERE, "MOHEuropeanAssault.exe"), "MOHEuropeanAssault.exe")
    put(os.path.join(HERE, "Launcher.exe"), "Launcher.exe")
    put(os.path.join(HERE, "menu_cursor.png"), "menu_cursor.png")
    put(os.path.join(HERE, "mkb.ini"), "mkb.ini")

    # launcher data only (its source stays private)
    for f in ("options.txt",):
        put(os.path.join(HERE, "launcher", f), os.path.join("launcher", f))
    put_tree(os.path.join(HERE, "launcher", "lang"), os.path.join("launcher", "lang"))
    put_tree(os.path.join(HERE, "launcher", "art"), os.path.join("launcher", "art"))
    put_tree(os.path.join(HERE, "launcher", "music"), os.path.join("launcher", "music"))

    # mods and languages
    put(os.path.join(HERE, "mods", "README.txt"), os.path.join("mods", "README.txt"))
    put(os.path.join(HERE, "mods", "weapons", "README.txt"), os.path.join("mods", "weapons", "README.txt"))
    put(os.path.join(HERE, "mods", "sounds", "README.txt"), os.path.join("mods", "sounds", "README.txt"))
    put(os.path.join(HERE, "mods", "ai", "README.txt"), os.path.join("mods", "ai", "README.txt"))
    put(os.path.join(HERE, "mods", "ai", "tuning.txt"), os.path.join("mods", "ai", "tuning.txt"))
    put_tree(os.path.join(HERE, "mods", "textures"), os.path.join("mods", "textures"))
    put_tree(os.path.join(HERE, "mods", "screenfx"), os.path.join("mods", "screenfx"))
    put_tree(os.path.join(HERE, "mods", "HDFont"), os.path.join("mods", "HDFont"))
    put_tree(os.path.join(HERE, "languages"), "languages")

    # the version the launcher shows and compares with the newest GitHub release
    with open(os.path.join(OUT, "launcher", "version.txt"), "w", newline="") as f:
        f.write(VERSION + "\n")

    # the empty game folder
    put(os.path.join(DOCS, "game_README.txt"), os.path.join("game", "README.txt"))

    # settings
    ini, fov, draw = default_ini()
    with open(os.path.join(OUT, "recomp_settings.ini"), "w", encoding="utf-8", newline="") as f:
        f.write(ini)
    if fov:
        with open(os.path.join(OUT, "fov_saved.txt"), "w", newline="") as f:
            f.write(fov + "\n")
    if draw:
        with open(os.path.join(OUT, "draw_saved.txt"), "w", newline="") as f:
            f.write(draw + "\n")

    # documents
    for d in ("README.md", "LICENSE.txt", "NOTICE.txt", "SOURCE.txt", "KNOWN_ISSUES.txt", "WHATS_NEW_" + VERSION + ".md"):
        put(os.path.join(DOCS, d), d)
    put(os.path.join(TOOLKIT, "LICENSES", "LGPL-2.1.txt"), os.path.join("LICENSES", "LGPL-2.1.txt"))
    put(os.path.join(TOOLKIT, "LICENSE"), os.path.join("LICENSES", "xboxrecomp-MIT.txt"))
    for rel in XEMU_FILES:
        put(os.path.join(TOOLKIT, rel), os.path.join("source", "xemu-derived", rel.replace("/", os.sep)))

    # zip + hash
    zpath = OUT + ".zip"
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dp, dn, fn in os.walk(OUT):
            for f in sorted(fn):
                full = os.path.join(dp, f)
                z.write(full, os.path.join(NAME, os.path.relpath(full, OUT)))
        z.writestr(zipfile.ZipInfo(NAME + "/game/"), "")      # the folder stays even when empty-looking
    h = hashlib.sha256()
    with open(zpath, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    with open(zpath + ".sha256", "w") as f:
        f.write("%s  %s\n" % (h.hexdigest(), os.path.basename(zpath)))
    total = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fn in os.walk(OUT) for f in fn)
    print("package:", OUT, "%.1f MB" % (total / 1e6))
    print("zip    :", zpath, "%.1f MB" % (os.path.getsize(zpath) / 1e6))
    print("sha256 :", h.hexdigest())


main()
