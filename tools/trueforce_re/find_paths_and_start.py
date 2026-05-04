"""Two-pronged probe:
  1. Find every string that might be a path prefix for trueforce_features.cfg
     (Logi, LGHUB, Trueforce, drive letters, env vars, temp dir hints)
  2. Cross-reference 'start' / 'remote' / 'init' related strings to see if
     there's a missing init call we should make after dllOpen.
"""

import re

DLL = r"C:\Program Files\Logi\Trueforce\1_3_11\trueforce_sdk_x64.dll"

with open(DLL, "rb") as f:
    data = f.read()

def extract_ascii(buf, min_len=4):
    out = []
    cur = []
    start = 0
    for i, b in enumerate(buf):
        if 0x20 <= b <= 0x7E:
            if not cur: start = i
            cur.append(b)
        else:
            if len(cur) >= min_len:
                out.append((start, bytes(cur).decode("ascii", "replace")))
            cur = []
    if len(cur) >= min_len:
        out.append((start, bytes(cur).decode("ascii", "replace")))
    return out

def extract_wide(buf, min_len=4):
    out = []
    cur = []
    start = 0
    i = 0
    while i + 1 < len(buf):
        b1, b2 = buf[i], buf[i+1]
        if b2 == 0 and 0x20 <= b1 <= 0x7E:
            if not cur: start = i
            cur.append(b1); i += 2
        else:
            if len(cur) >= min_len:
                out.append((start, bytes(cur).decode("ascii", "replace")))
            cur = []; i += 1
    if len(cur) >= min_len:
        out.append((start, bytes(cur).decode("ascii", "replace")))
    return out

ascii_strs = extract_ascii(data)
wide_strs  = extract_wide(data)
all_strs = sorted(ascii_strs + [(o, s) for o, s in wide_strs], key=lambda x: x[0])

def show(label, regex, max_show=80):
    print(f"===== {label} =====")
    pat = re.compile(regex, re.I)
    n = 0
    seen = set()
    for off, s in all_strs:
        if pat.search(s) and s not in seen and 4 <= len(s) <= 120:
            print(f"  0x{off:08X}: {s}")
            seen.add(s)
            n += 1
            if n >= max_show:
                print(f"  ... (capped at {max_show})")
                break
    print()

show("Logi / Logitech / LGHUB / Trueforce path components",
     r"\\Logi\\|/Logi/|\\Logitech\\|/Logitech/|\\LGHUB\\|/LGHUB/|\\Trueforce|/Trueforce")

show("Anything ending in .cfg / .bin / .json (potential path strings)",
     r"\.(cfg|bin|json|ini|txt|log)\b")

show("Environment-variable / well-known-folder references",
     r"%[A-Z][A-Z_]+%|ProgramData|ProgramFiles|LOCALAPPDATA|APPDATA|TEMP|TMP|USERPROFILE")

show("'start' / 'init' / 'register' / 'connect' / 'remote' / 'load' patterns suggesting init flow",
     r"\b(start|init|register|connect|remote|load|begin|launch|spin)[A-Za-z]*\b")

show("Function-name-like patterns in mangled exports (look for class methods)",
     r"::[a-z][a-zA-Z]+|@[a-zA-Z]+@@|[A-Z][a-z]+::[A-Z][a-z]+")

# Specifically: look at strings within 64 bytes of trueforce_features.cfg
print("===== ±256 bytes around trueforce_features.cfg / trueforce_data.bin =====")
target = None
for off, s in ascii_strs:
    if s in ("trueforce_features.cfg", "trueforce_data.bin"):
        target = off
        for o2, s2 in ascii_strs:
            if abs(o2 - off) <= 256 and 4 <= len(s2) <= 120:
                print(f"  0x{o2:08X}: {s2}")
        print()
