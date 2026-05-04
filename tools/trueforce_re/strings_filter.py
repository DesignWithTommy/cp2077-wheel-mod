"""Extract every printable ASCII / wide string from trueforce_sdk_x64.dll
and bucket by category so we can find:
  - Config file names + paths
  - Pipe / IPC names
  - JSON keys, INI sections
  - Error messages that hint at registration/init flow
"""

import re
import sys

DLL = r"C:\Program Files\Logi\Trueforce\1_3_11\trueforce_sdk_x64.dll"

with open(DLL, "rb") as f:
    data = f.read()

def extract_ascii(buf, min_len=5):
    out = []
    cur = []
    start = 0
    for i, b in enumerate(buf):
        if 0x20 <= b <= 0x7E:
            if not cur:
                start = i
            cur.append(b)
        else:
            if len(cur) >= min_len:
                out.append((start, bytes(cur).decode("ascii", "replace")))
            cur = []
    if len(cur) >= min_len:
        out.append((start, bytes(cur).decode("ascii", "replace")))
    return out

def extract_wide(buf, min_len=5):
    out = []
    cur = []
    start = 0
    i = 0
    while i + 1 < len(buf):
        b1, b2 = buf[i], buf[i+1]
        if b2 == 0 and 0x20 <= b1 <= 0x7E:
            if not cur:
                start = i
            cur.append(b1)
            i += 2
        else:
            if len(cur) >= min_len:
                out.append((start, bytes(cur).decode("ascii", "replace")))
            cur = []
            i += 1
    if len(cur) >= min_len:
        out.append((start, bytes(cur).decode("ascii", "replace")))
    return out

ascii_strs = extract_ascii(data, min_len=5)
wide_strs  = extract_wide(data,  min_len=5)
print(f"Found {len(ascii_strs)} ASCII strings, {len(wide_strs)} wide strings")
print()

# Topic-based filters
def show(label, regex, strings, max_show=40, ignore_case=True):
    flags = re.I if ignore_case else 0
    print(f"===== {label} =====")
    pat = re.compile(regex, flags)
    seen = set()
    n = 0
    for off, s in strings:
        if pat.search(s) and s not in seen:
            print(f"  0x{off:08X}: {s}")
            seen.add(s)
            n += 1
            if n >= max_show:
                print(f"  ... (capped at {max_show})")
                break
    print()

show("config / cfg / .json / .ini / .bin file references",
     r"\.cfg|\.json|\.ini|\.bin|trueforce.*features|trueforce.*data",
     ascii_strs + wide_strs, max_show=30)

show("Path-like (drive letter, %ENV%, slashes)",
     r"^[a-z]:[\\/]|%[A-Z][A-Z_]+%|^[\\/].*[\\/]",
     ascii_strs + wide_strs, max_show=30)

show("Pipe / IPC names",
     r"pipe|\\\\\.\\|named.?pipe|connect|register|handshake|hello",
     ascii_strs + wide_strs, max_show=40)

show("Error messages hinting at init/registration",
     r"failed|unable|not.*found|missing|invalid|reject|denied|error.*register|register.*error|please|require",
     ascii_strs + wide_strs, max_show=30)

show("Mutex / event names",
     r"mutex|event|semaphore|Logi|TrueForce|\.connect",
     ascii_strs + wide_strs, max_show=40)

show("Game / process related (game ID, process name)",
     r"game|process|app.?id|product.?id|whitelist|allow",
     ascii_strs + wide_strs, max_show=30)

show("JSON-like keys (snake_case or camelCase patterns)",
     r"^(is|has|enable|disable|version|type|format|major|minor|build)[A-Z_]",
     ascii_strs + wide_strs, max_show=30)

# Dump all strings near 'trueforce_features.cfg' for context
print("===== Strings adjacent to 'trueforce_features.cfg' (file offset window) =====")
target_off = None
for off, s in ascii_strs:
    if s == "trueforce_features.cfg":
        target_off = off
        break
if target_off:
    window = (target_off - 1024, target_off + 1024)
    for off, s in ascii_strs:
        if window[0] <= off <= window[1] and 5 <= len(s) <= 80:
            print(f"  0x{off:08X}: {s}")
print()
