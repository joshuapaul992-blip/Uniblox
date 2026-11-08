#!/usr/bin/env python3
"""Uniblox PE compatibility checker.

Fails the build if an .exe could not load on Windows NT 3.51:
  * wrong machine / not a 32-bit GUI image
  * OS or subsystem version above 3.10
  * COFF symbol table or long (>8 char) section names
  * non-zero DllCharacteristics (reserved field on NT 3.51)
  * TLS directory (implicit TLS is unreliable on old NT)
  * imports from a DLL or of a function not on the vetted allowlist,
    or any import by ordinal

The allowlist (tools/nt351_imports.txt) lists only functions confirmed to
exist on NT 3.51. Anything newer must be loaded with GetProcAddress at
runtime, which this tool cannot see and does not need to.

Usage: checkpe.py --allow tools/nt351_imports.txt build/win32/uniblox.exe
"""
import argparse
import struct
import sys

IMAGE_FILE_MACHINE_I386 = 0x14C
PE32_MAGIC = 0x10B
SUBSYSTEM_GUI = 2
DIR_IMPORT = 1
DIR_TLS = 9
MAX_OS = (3, 10)


def load_allowlist(path):
    allowed = {}
    dll = None
    with open(path, encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                dll = line[1:-1].strip().upper()
                allowed.setdefault(dll, set())
            elif dll is None:
                sys.exit(f"{path}:{lineno}: function listed before any [DLL] header")
            else:
                allowed[dll].add(line)
    return allowed


class PE:
    def __init__(self, data):
        self.d = data
        if data[:2] != b"MZ":
            raise ValueError("not an MZ executable")
        (self.pe_off,) = struct.unpack_from("<I", data, 0x3C)
        if data[self.pe_off:self.pe_off + 4] != b"PE\0\0":
            raise ValueError("missing PE signature")
        coff = self.pe_off + 4
        (self.machine, self.nsections, _ts, self.symtab, self.nsyms,
         self.opt_size, self.characteristics) = struct.unpack_from("<HHIIIHH", data, coff)
        self.opt = coff + 20
        (self.magic,) = struct.unpack_from("<H", data, self.opt)
        o = self.opt
        self.os_ver = struct.unpack_from("<HH", data, o + 40)
        self.sub_ver = struct.unpack_from("<HH", data, o + 48)
        (self.subsystem, self.dll_chars) = struct.unpack_from("<HH", data, o + 68)
        (self.ndirs,) = struct.unpack_from("<I", data, o + 92)
        self.dirs = [struct.unpack_from("<II", data, o + 96 + 8 * i)
                     for i in range(min(self.ndirs, 16))]
        self.sections = []
        sec = self.opt + self.opt_size
        for i in range(self.nsections):
            name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", data, sec + 40 * i)
            self.sections.append((name.rstrip(b"\0").decode("latin-1"), va, vsize, rptr, rsize))

    def rva_to_off(self, rva):
        for _n, va, vsize, rptr, rsize in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + rptr
        raise ValueError(f"RVA {rva:#x} not in any section")

    def cstr(self, rva):
        off = self.rva_to_off(rva)
        end = self.d.index(b"\0", off)
        return self.d[off:end].decode("latin-1")

    def imports(self):
        rva, size = self.dirs[DIR_IMPORT] if len(self.dirs) > DIR_IMPORT else (0, 0)
        result = []
        if not rva:
            return result
        off = self.rva_to_off(rva)
        while True:
            oft, _ts, _fwd, name_rva, ft = struct.unpack_from("<IIIII", self.d, off)
            if not (oft or name_rva or ft):
                break
            dll = self.cstr(name_rva).upper()
            thunk = self.rva_to_off(oft or ft)
            funcs = []
            while True:
                (entry,) = struct.unpack_from("<I", self.d, thunk)
                if entry == 0:
                    break
                if entry & 0x80000000:
                    funcs.append(("ordinal", entry & 0xFFFF))
                else:
                    funcs.append(("name", self.cstr(entry + 2)))
                thunk += 4
            result.append((dll, funcs))
            off += 20
        return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--allow", required=True, help="allowlist of NT 3.51 imports")
    ap.add_argument("exe", nargs="+")
    args = ap.parse_args()
    allowed = load_allowlist(args.allow)

    failed = False
    for path in args.exe:
        errors = []
        with open(path, "rb") as f:
            pe = PE(f.read())
        if pe.machine != IMAGE_FILE_MACHINE_I386:
            errors.append(f"machine is {pe.machine:#x}, expected i386")
        if pe.magic != PE32_MAGIC:
            errors.append("not a PE32 (32-bit) image")
        if pe.subsystem != SUBSYSTEM_GUI:
            errors.append(f"subsystem {pe.subsystem}, expected Windows GUI (2)")
        if tuple(pe.os_ver) > MAX_OS:
            errors.append("OS version %d.%d > 3.10" % tuple(pe.os_ver))
        if tuple(pe.sub_ver) > MAX_OS:
            errors.append("subsystem version %d.%d > 3.10" % tuple(pe.sub_ver))
        if pe.dll_chars:
            errors.append(f"DllCharacteristics {pe.dll_chars:#06x} should be 0 "
                          "(field was reserved before Windows 2000)")
        if pe.symtab or pe.nsyms:
            errors.append("COFF symbol table present (strip the binary)")
        for name, *_ in pe.sections:
            if name.startswith("/"):
                errors.append(f"long section name {name!r}")
        if len(pe.dirs) > DIR_TLS and pe.dirs[DIR_TLS][0]:
            errors.append("TLS directory present")

        imports = pe.imports()
        count = 0
        for dll, funcs in imports:
            if dll not in allowed:
                errors.append(f"imports from non-allowed DLL {dll}")
                continue
            for kind, val in funcs:
                count += 1
                if kind == "ordinal":
                    errors.append(f"{dll}: import by ordinal {val}")
                elif val not in allowed[dll]:
                    errors.append(f"{dll}!{val} is not on the NT 3.51 allowlist")

        print(f"{path}:")
        print("  OS %d.%d, subsystem %d.%d, DllCharacteristics %#06x"
              % (*pe.os_ver, *pe.sub_ver, pe.dll_chars))
        print("  sections: " + ", ".join(n for n, *_ in pe.sections))
        print(f"  {count} imports from " + ", ".join(d for d, _ in imports))
        if errors:
            failed = True
            for e in errors:
                print(f"  FAIL: {e}")
        else:
            print("  OK: loadable on Windows NT 3.51 and later")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
