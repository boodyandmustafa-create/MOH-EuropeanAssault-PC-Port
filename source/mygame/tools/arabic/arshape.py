"""Arabic shaping + visual reordering for engines that only draw glyphs left to right.

shape_line(text)   -> string of presentation-form code points in VISUAL (left-to-right) order.
visual(text)       -> splits on the game's line separators ('|' and the literal two characters \\n),
                      shapes + reorders every line on its own and joins them again.

LTR islands (Latin words, digits, [$ACTION], %1, %s, &amp; entities ...) keep their internal order.
"""
import re
import unicodedata

# ---- joining tables built from the Unicode decompositions of the presentation forms -------------
FORMS = {}      # base char -> {'iso':c, 'fin':c, 'ini':c, 'med':c}
LIGS = {}       # (lam, alef variant) -> {'iso':c, 'fin':c}
for cp in range(0xFE70, 0xFEFD):
    ch = chr(cp)
    dec = unicodedata.decomposition(ch)
    if not dec or not dec.startswith("<"):
        continue
    tag, rest = dec.split(" ", 1)
    parts = [chr(int(p, 16)) for p in rest.split()]
    form = {"<isolated>": "iso", "<final>": "fin", "<initial>": "ini", "<medial>": "med"}.get(tag)
    if form is None:
        continue
    if len(parts) == 1:
        FORMS.setdefault(parts[0], {})[form] = ch
    elif len(parts) == 2 and parts[0] == "ل":
        LIGS.setdefault(tuple(parts), {})[form] = ch

# letters that join to the previous letter only (right-joining)
DUAL = {c for c, f in FORMS.items() if "ini" in f and "med" in f}
RIGHT = {c for c, f in FORMS.items() if "fin" in f and c not in DUAL}
# a few base letters have no isolated presentation form in the FExx block under their own name
HARAKAT = set(chr(c) for c in range(0x064B, 0x0653)) | {"ٰ"}


def is_arabic_letter(c):
    return c in FORMS or c == "ـ" or ("؀" <= c <= "ۿ" and unicodedata.category(c).startswith("L"))


def _joins_next(c):      # letter c connects to the letter AFTER it (visually to its left in LTR storage)
    return c in DUAL or c == "ـ"


def _joins_prev(c):      # letter c connects to the letter BEFORE it
    return c in DUAL or c in RIGHT or c == "ـ"


def shape_run(run):
    """run: logical-order string of Arabic letters (and harakat/neutrals inside). Returns shaped
    presentation forms, still in logical order."""
    run = "".join(c for c in run if c not in HARAKAT)
    out = []
    i = 0
    n = len(run)
    # first pass: lam + alef ligatures
    toks = []
    while i < n:
        c = run[i]
        if c == "ل" and i + 1 < n and (c, run[i + 1]) in LIGS:
            toks.append((c + run[i + 1], True))
            i += 2
            continue
        toks.append((c, False))
        i += 1
    for k, (t, lig) in enumerate(toks):
        # does the previous token connect forward? does the next token connect backward?
        prev = toks[k - 1][0] if k > 0 else None
        nxt = toks[k + 1][0] if k + 1 < len(toks) else None
        prev_conn = prev is not None and _joins_next(prev[0])
        next_conn = nxt is not None and _joins_prev(nxt[0])
        if lig:
            f = LIGS[(t[0], t[1])]
            out.append(f["fin"] if prev_conn and "fin" in f else f["iso"])
            continue
        c = t
        forms = FORMS.get(c)
        if not forms:
            out.append(c)
            continue
        if c in DUAL:
            if prev_conn and next_conn:
                form = "med"
            elif prev_conn:
                form = "fin"
            elif next_conn:
                form = "ini"
            else:
                form = "iso"
        elif c in RIGHT:
            form = "fin" if prev_conn else "iso"
        else:
            form = "iso"
        out.append(forms.get(form) or forms.get("iso") or c)
    return "".join(out)


MIRROR = {"(": ")", ")": "(", "[": "]", "]": "[", "{": "}", "}": "{", "<": ">", ">": "<", "«": "»", "»": "«"}
# tokens that must stay intact and left-to-right: button/placeholder markers and printf-like fields
ATOMIC = re.compile(r"\[(?:&gt;|>|\$)[A-Za-z0-9_]+\]|%[0-9]*\.?[0-9]*[a-zA-Z]|%[0-9]+|\^[0-9A-Za-z]")
# Latin words / numbers: stay left to right as one island
LTR_WORD = re.compile(r"[A-Za-z0-9][A-Za-z0-9'._:/%+\-]*[A-Za-z0-9%]|[A-Za-z0-9]")


def _classify(line):
    """split into runs: ('ar', text) | ('ltr', text) | ('neu', text)"""
    runs = []
    i = 0
    n = len(line)
    while i < n:
        m = ATOMIC.match(line, i) or LTR_WORD.match(line, i)
        if m and m.group(0):
            runs.append(["ltr", m.group(0)])
            i = m.end()
            continue
        c = line[i]
        if is_arabic_letter(c) or c in HARAKAT:
            j = i
            while j < n and (is_arabic_letter(line[j]) or line[j] in HARAKAT):
                j += 1
            runs.append(["ar", line[i:j]])
            i = j
            continue
        runs.append(["neu", c])
        i += 1
    return runs


def shape_line(line):
    runs = _classify(line)
    toks = []
    for kind, t in runs:
        if kind == "ar":
            toks.append(("ar", shape_run(t)))
        else:
            toks.append((kind, t))
    # neutrals between two LTR tokens stay in the LTR island (e.g. "Level 3")
    merged = [list(tk) for tk in toks]
    k = 1
    while k + 1 < len(merged):
        if merged[k][0] == "neu" and merged[k][1] in " -/:.," and merged[k - 1][0] == "ltr" and merged[k + 1][0] == "ltr":
            merged[k - 1][1] += merged[k][1] + merged[k + 1][1]
            del merged[k:k + 2]
        else:
            k += 1
    out = []
    for kind, t in reversed(merged):
        if kind == "ar":
            out.append(t[::-1])
        elif kind == "ltr":
            out.append(t)
        else:
            out.append(MIRROR.get(t, t))
    return "".join(out)


SEP = re.compile(r"(\||\\n|\[\d+(?:\.\d+)?\])")      # line breaks and the [5.9] page-timing marks of the briefings


def visual(text):
    """text is the raw XML-escaped value from the string table"""
    # protect XML entities: treat &gt; etc. as a single neutral by temporarily mapping to private use
    ents = {}

    def keep(m):
        key = chr(0xE000 + len(ents))
        ents[key] = m.group(0)
        return key

    atoms = {}

    def keep_atom(m):
        key = chr(0xF000 + len(atoms))
        atoms[key] = m.group(0)
        return key

    t = ATOMIC.sub(keep_atom, text)          # [&gt;Aim], [$ACTION], %1 ... stay one piece
    t = re.sub(r"&[a-zA-Z#0-9]+;", keep, t)
    parts = SEP.split(t)
    res = []
    for p in parts:
        if p == "|" or p == "\\n" or re.fullmatch(r"\[\d+(?:\.\d+)?\]", p):
            res.append(p)
        else:
            res.append(shape_line(p))
    out = "".join(res)
    for k, v in ents.items():
        out = out.replace(k, v)
    for k, v in atoms.items():
        out = out.replace(k, v)
    return out


NBSP = " "


def visual_chunks(text, limit=46):
    """Dialogue / subtitle version of visual(). The engine word-wraps the stored (visually ordered) string at
    plain spaces, which puts the END of an Arabic sentence on the top line. Here the sentence is cut into chunks
    of at most `limit` characters in READING order, every chunk is shaped on its own and its inner spaces become
    NBSP (the fonts draw 0xA0 as a space), and only the chunks are joined with a real space: the engine can only
    break between chunks, so the lines come out top to bottom in the right order."""
    out = []
    for p in SEP.split(text):
        if p == "" or SEP.fullmatch(p):
            out.append(p)
            continue
        chunks, cur, n = [], [], 0
        for w in p.split(" "):
            if cur and n + 1 + len(w) > limit:
                chunks.append(" ".join(cur))
                cur, n = [w], len(w)
            else:
                n += (1 if cur else 0) + len(w)
                cur.append(w)
        if cur:
            chunks.append(" ".join(cur))
        out.append(" ".join(visual(c).replace(" ", NBSP) for c in chunks))
    return "".join(out)


if __name__ == "__main__":
    import sys
    sys.stdout.reconfigure(encoding="utf-8")
    for s in ["الخيارات", "اضغط [$ACTION] للمواجهة", "المرحلة 3: لا تتوقف!", "لعبة (جديدة) &amp; سريعة", "السطر الأول|السطر الثاني", "ابدأ", "خروج"]:
        v = visual(s)
        print(s, "->", v, [hex(ord(c)) for c in v[:6]])
