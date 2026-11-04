"""Builds the public source tree in ..\\github_repo\\source from the working folders (run: python tools\\stage_source.py).

Included : the port code (mygame\\src without src\\recomp\\gen except recomp_types.h), CMake, the launcher source, the
           menu / launcher language files, the mods that are text only, the helper scripts, and the xboxrecomp toolkit
           tree WITH this port's changes (no build output).
Excluded : everything that is the game's (generated recompiled code, XBE, data, extracted textures / sounds / strings,
           the cover picture, the launcher music), build folders, backups, logs, captures, executables.
Also writes gen_hand_edits.txt: every hand edit this port made inside the generated code (marker comments), with the
           function it sits in, so the edits can be re-applied to a freshly generated gen\\.
"""
import os, re, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MY = os.path.dirname(HERE)                                  # ...\mygame
ROOT = os.path.dirname(MY)                                  # ...\xboxrecomp-0.16.0
TOOLKIT = os.path.join(ROOT, "xboxrecomp-0.16.0")
OUT = os.path.join(ROOT, "github_repo", "source")

SKIP_DIRS = {"ar", "build", "build_pgi", "build_train", "backups", "game", "xboxdata", "captures", "screenshots", "shader_cache",
             "tex_dump", "tools_dbg", "__pycache__", ".git", "release", "out", "bin", "art", "music", "DATA", "textures",
             "sounds", "HDFont", "screenfx"}
SKIP_EXT = {".exe", ".pdb", ".obj", ".o", ".lib", ".dll", ".zip", ".bmp", ".png", ".jpg", ".jpeg", ".wav", ".dds", ".gif",
            ".mp4", ".ilk", ".exp", ".pkl", ".cso", ".log"}
KEEP_PNG = set()


def want(path, name):
    ext = os.path.splitext(name)[1].lower()
    if ext in SKIP_EXT and name not in KEEP_PNG:
        return False
    if re.match(r"(play_.*|shot_probe_.*|reload_probe_.*|ai_.*|dump_.*|tex_dump.*)\.(txt|bat|log)$", name) and name != "play_ai_log.bat":
        return False
    return True


def copy_tree(src, dst, extra_skip=()):
    for dp, dn, fn in os.walk(src):
        rel = os.path.relpath(dp, src)
        dn[:] = [d for d in dn if d not in SKIP_DIRS and d not in extra_skip]
        for f in fn:
            if not want(dp, f):
                continue
            s = os.path.join(dp, f)
            d = os.path.join(dst, rel, f) if rel != "." else os.path.join(dst, f)
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copy2(s, d)


def main():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)

    # --- the port ---
    my_out = os.path.join(OUT, "mygame")
    os.makedirs(my_out)
    for f in ("CMakeLists.txt", "build.bat", "make_package.py"):
        p = os.path.join(MY, f)
        if os.path.isfile(p):
            shutil.copy2(p, my_out)
    # src: all but the generated game code
    src_out = os.path.join(my_out, "src")
    for dp, dn, fn in os.walk(os.path.join(MY, "src")):
        rel = os.path.relpath(dp, os.path.join(MY, "src"))
        if rel.replace("\\", "/").startswith("recomp/gen"):
            continue
        for f in fn:
            if not want(dp, f):
                continue
            d = os.path.join(src_out, rel, f)
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copy2(os.path.join(dp, f), d)
    gen_out = os.path.join(src_out, "recomp", "gen")
    os.makedirs(gen_out, exist_ok=True)
    shutil.copy2(os.path.join(MY, "src", "recomp", "gen", "recomp_types.h"), gen_out)   # the register model, not game code

    copy_tree(os.path.join(MY, "launcher"), os.path.join(my_out, "launcher"))
    for sub in ("tools", "package_docs"):
        copy_tree(os.path.join(MY, sub), os.path.join(my_out, sub))
    # menu translation only (the game-text pack stays out: it is derived from the game's own strings)
    ar = os.path.join(my_out, "languages", "arabic")
    os.makedirs(ar)
    shutil.copy2(os.path.join(MY, "languages", "arabic", "recomp_menu.txt"), ar)
    # text-only mods
    for rel in ("mods/README.txt", "mods/ai/README.txt", "mods/ai/tuning.txt", "mods/ai/defs.txt", "mods/weapons"):
        s = os.path.join(MY, rel)
        d = os.path.join(my_out, rel)
        if os.path.isdir(s):
            os.makedirs(d, exist_ok=True)
            for f in os.listdir(s):
                if f.lower().endswith(".txt") and f != "Thompson.txt":
                    shutil.copy2(os.path.join(s, f), d)
        elif os.path.isfile(s):
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copy2(s, d)

    # --- the toolkit with our changes ---
    copy_tree(TOOLKIT, os.path.join(OUT, "xboxrecomp-0.16.0"), extra_skip=("game_files",))
    for dp, dn, fn in os.walk(os.path.join(OUT, "xboxrecomp-0.16.0")):
        for f in fn:
            if f.endswith((".ringguard", ".bak", ".orig")):
                os.remove(os.path.join(dp, f))

    # --- hand edits inside the generated code ---
    marker = re.compile(r"/\*\s*(AI-[A-Z]+|[A-Z0-9-]*PROBE[A-Z, -]*|MOVIE-SKIP|BLOOD[A-Z-]*|[A-Z]+-[A-Z]+)[^*]*\*/")
    gdir = os.path.join(MY, "src", "recomp", "gen")
    rows = []
    for name in sorted(os.listdir(gdir)):
        if not re.match(r"recomp_00\d\d\.c$", name):
            continue
        fn = None
        with open(os.path.join(gdir, name), encoding="utf-8", errors="replace") as fh:
            for no, line in enumerate(fh, 1):
                m = re.match(r"void (sub_[0-9A-F]+)\(", line)
                if m:
                    fn = m.group(1)
                if marker.search(line) and "regconv" not in line:
                    rows.append("%s:%d  in %s\n    %s" % (name, no, fn, line.strip()))
    with open(os.path.join(OUT, "gen_hand_edits.txt"), "w", encoding="utf-8") as fh:
        fh.write("Hand edits made inside the generated code (src/recomp/gen), marked with comments.\n"
                 "Re-apply them to a freshly generated gen/ (the line numbers belong to this port's own gen).\n\n")
        fh.write("\n".join(rows) + "\n")
    for f in os.listdir(os.path.join(HERE, "source_docs")):
        shutil.copy2(os.path.join(HERE, "source_docs", f), OUT)
    print("staged", OUT, "| marked gen edits:", len(rows))


if __name__ == "__main__":
    sys.exit(main())
