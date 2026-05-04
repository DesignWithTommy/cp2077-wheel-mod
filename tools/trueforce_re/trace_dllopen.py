"""Trace what trueforce_sdk_x64.dll!dllOpen does.

Outputs:
  1. All ASCII / wide string constants referenced from dllOpen's body
  2. All Win32 API calls dllOpen makes (resolved via IAT)
  3. The disassembled body up to its `ret` (or first ~400 instructions)

Goal: identify where the SDK looks for trueforce_features.cfg, what other
config it reads, what registration handshake (if any) it performs, and what
makes Available() return false on a healthy install.
"""

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from collections import OrderedDict

DLL = r"C:\Program Files\Logi\Trueforce\1_3_11\trueforce_sdk_x64.dll"

pe = pefile.PE(DLL, fast_load=True)
pe.parse_data_directories(directories=[
    pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
    pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT'],
])

image_base = pe.OPTIONAL_HEADER.ImageBase

# Build IAT lookup: VA → "DLL!Function"
iat = {}
for entry in pe.DIRECTORY_ENTRY_IMPORT:
    dll_name = entry.dll.decode("ascii", "replace")
    for imp in entry.imports:
        if imp.address and imp.name:
            iat[imp.address] = f"{dll_name}!{imp.name.decode('ascii', 'replace')}"

# Find dllOpen RVA + size from exports
dll_open_rva = None
for sym in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if sym.name and sym.name.decode() == "dllOpen":
        dll_open_rva = sym.address
        break
assert dll_open_rva is not None, "dllOpen export not found"

dll_open_va = image_base + dll_open_rva
print(f"dllOpen RVA=0x{dll_open_rva:X} VA=0x{dll_open_va:X}")
print()

# Read the function body (up to ~4 KB; we'll stop at the first natural ret).
# Locate the .text section first.
text_section = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
text_start_va = image_base + text_section.VirtualAddress
text_end_va   = text_start_va + text_section.Misc_VirtualSize
text_data     = pe.get_data(text_section.VirtualAddress, text_section.SizeOfRawData)

# Slice out enough bytes from dllOpen onward to disassemble the body.
offset_into_text = dll_open_rva - text_section.VirtualAddress
body = text_data[offset_into_text:offset_into_text + 0x4000]

# Helper: read a NUL-terminated ASCII string at a given VA (any section).
def read_cstring(va, max_len=512):
    for s in pe.sections:
        sec_va = image_base + s.VirtualAddress
        sec_end = sec_va + s.Misc_VirtualSize
        if sec_va <= va < sec_end:
            offset = va - sec_va
            data = pe.get_data(s.VirtualAddress, s.SizeOfRawData)[offset:offset + max_len]
            nul = data.find(b"\x00")
            if nul == -1:
                return None
            return data[:nul].decode("ascii", "replace")
    return None

def read_wstring(va, max_len=512):
    for s in pe.sections:
        sec_va = image_base + s.VirtualAddress
        sec_end = sec_va + s.Misc_VirtualSize
        if sec_va <= va < sec_end:
            offset = va - sec_va
            data = pe.get_data(s.VirtualAddress, s.SizeOfRawData)[offset:offset + max_len * 2]
            try:
                s2 = data.decode("utf-16-le", "replace")
            except UnicodeDecodeError:
                return None
            nul = s2.find("\x00")
            if nul == -1:
                return None
            return s2[:nul]
    return None

cs = Cs(CS_ARCH_X86, CS_MODE_64)
cs.detail = True

# Disassemble. Stop at first ret encountered after at least 32 instructions
# (skip ret instructions inside conditional paths early on).
strings_seen = OrderedDict()
calls_seen   = []
print("===== Disassembly (truncated at first ret past min-32 instructions) =====")
inst_count = 0
for insn in cs.disasm(body, dll_open_va):
    inst_count += 1
    line = f"  {insn.address:016x}: {insn.mnemonic:8s} {insn.op_str}"

    # Resolve LEA-to-string-pool patterns: lea reg, [rip+disp]
    if insn.mnemonic == "lea":
        # The op[1] is the memory operand; effective address = insn.address + insn.size + disp
        try:
            mem = insn.operands[1].mem
            if mem.base == 0 or insn.reg_name(mem.base) == "rip":
                target = insn.address + insn.size + mem.disp
                # Try ASCII first
                s = read_cstring(target, 256)
                if s and len(s) >= 3 and s.isprintable():
                    line += f"    ; -> \"{s}\""
                    strings_seen[target] = ("ascii", s)
                else:
                    w = read_wstring(target, 256)
                    if w and len(w) >= 3 and w.isprintable():
                        line += f"    ; -> L\"{w}\""
                        strings_seen[target] = ("wide", w)
        except Exception:
            pass

    # Resolve calls to imports: call [rip+disp] (qword ptr)
    if insn.mnemonic == "call":
        op = insn.op_str
        if op.startswith("qword ptr [rip"):
            # call [rip+disp] -> indirect via IAT
            try:
                # Parse the displacement
                mem = insn.operands[0].mem
                target = insn.address + insn.size + mem.disp
                if target in iat:
                    line += f"    ; -> {iat[target]}"
                    calls_seen.append((insn.address, "import", iat[target]))
            except Exception:
                pass
        elif op.startswith("0x"):
            # Direct internal call
            try:
                tgt = int(op, 16)
                line += f"    ; -> internal sub_{tgt - image_base:X}"
                calls_seen.append((insn.address, "internal", f"sub_{tgt - image_base:X}"))
            except Exception:
                pass

    print(line)

    if insn.mnemonic == "ret" and inst_count >= 32:
        print(f"  ...stopped at ret after {inst_count} instructions")
        break
    if inst_count > 600:
        print("  ...stopped at instruction limit")
        break

print()
print("===== Strings referenced from dllOpen =====")
for va, (kind, s) in strings_seen.items():
    print(f"  0x{va:X}  {kind}: {s!r}")

print()
print("===== Imports called from dllOpen =====")
for addr, kind, name in calls_seen:
    if kind == "import":
        print(f"  @0x{addr:X}: {name}")
print()
print("===== Internal calls from dllOpen =====")
internal_targets = OrderedDict()
for addr, kind, name in calls_seen:
    if kind == "internal":
        internal_targets[name] = addr
for name, addr in internal_targets.items():
    print(f"  @0x{addr:X}: {name}")
