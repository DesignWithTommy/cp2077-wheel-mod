"""For each ordinal-only / mangled export of trueforce_sdk_x64.dll, dump
the first 30 instructions to characterize what it does. Goal: find any
init / start / register / connect entry point that isn't in the flat
logi* C exports.
"""

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Logi\Trueforce\1_3_11\trueforce_sdk_x64.dll"
pe = pefile.PE(DLL, fast_load=True)
pe.parse_data_directories(directories=[
    pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
    pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT'],
])
image_base = pe.OPTIONAL_HEADER.ImageBase

# IAT lookup
iat = {}
for entry in pe.DIRECTORY_ENTRY_IMPORT:
    dll_name = entry.dll.decode("ascii", "replace")
    for imp in entry.imports:
        if imp.address and imp.name:
            iat[imp.address] = f"{dll_name}!{imp.name.decode('ascii', 'replace')}"

text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
text_data = pe.get_data(text.VirtualAddress, text.SizeOfRawData)
text_va = image_base + text.VirtualAddress

cs = Cs(CS_ARCH_X86, CS_MODE_64)
cs.detail = True

def read_cstring(va, max_len=128):
    for s in pe.sections:
        sec_va = image_base + s.VirtualAddress
        sec_end = sec_va + s.Misc_VirtualSize
        if sec_va <= va < sec_end:
            offset = va - sec_va
            data = pe.get_data(s.VirtualAddress, s.SizeOfRawData)[offset:offset + max_len]
            nul = data.find(b"\x00")
            if nul == -1:
                return None
            try:
                return data[:nul].decode("ascii")
            except UnicodeDecodeError:
                return None
    return None

mangled_exports = []
for sym in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if sym.name and sym.name.decode().startswith("?"):
        mangled_exports.append((sym.address, sym.name.decode()))

print(f"Found {len(mangled_exports)} mangled exports. Disassembling first 25 instructions of each.\n")

for rva, name in mangled_exports:
    va = image_base + rva
    body = text_data[rva - text.VirtualAddress: rva - text.VirtualAddress + 256]
    print(f"---- {name}  RVA=0x{rva:X} ----")
    n = 0
    for insn in cs.disasm(body, va):
        line = f"  {insn.address:016x}: {insn.mnemonic:6s} {insn.op_str}"
        if insn.mnemonic == "call":
            op = insn.op_str
            if op.startswith("qword ptr [rip"):
                try:
                    mem = insn.operands[0].mem
                    target = insn.address + insn.size + mem.disp
                    if target in iat:
                        line += f"    ; {iat[target]}"
                except Exception:
                    pass
            elif op.startswith("0x"):
                try:
                    tgt = int(op, 16)
                    line += f"    ; sub_{tgt - image_base:X}"
                except Exception:
                    pass
        if insn.mnemonic == "lea":
            try:
                mem = insn.operands[1].mem
                if insn.reg_name(mem.base) == "rip":
                    target = insn.address + insn.size + mem.disp
                    s = read_cstring(target)
                    if s and len(s) >= 3 and s.isprintable():
                        line += f"    ; \"{s}\""
            except Exception:
                pass
        print(line)
        n += 1
        if insn.mnemonic == "ret" and n >= 5:
            break
        if n >= 25:
            break
    print()
