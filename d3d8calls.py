"""Static extraction of D3D8 COM calls from a 32-bit PE.

D3D8 is COM: the only named import is Direct3DCreate8, everything else is an
indirect call through an interface vtable.  So we:
  1. parse d3d8.h to get the exact vtable layout of every interface,
  2. find the Direct3DCreate8 IAT slot and the global that receives IDirect3D8*,
  3. linear-sweep .text tracking "reg <- [global]" and resolving
     "call [reg+off]" against the vtable of whatever interface that global holds,
  4. whenever a resolved method has an out-param of interface type, tag the
     global passed there and iterate until fixpoint.
"""

import re
import struct
import sys
from collections import defaultdict

from capstone import CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM, CS_OP_REG, Cs

IUNKNOWN = ["QueryInterface", "AddRef", "Release"]

# sub-register -> the 32-bit register we track, so that a partial write
# (e.g. "mov al, 1") still invalidates the tracked pointer in eax.
_SUB = {}
for _full, _parts in {
    "eax": ("ax", "al", "ah"),
    "ebx": ("bx", "bl", "bh"),
    "ecx": ("cx", "cl", "ch"),
    "edx": ("dx", "dl", "dh"),
    "esi": ("si", "sil"),
    "edi": ("di", "dil"),
    "ebp": ("bp", "bpl"),
    "esp": ("sp", "spl"),
}.items():
    for _p in _parts:
        _SUB[_p] = _full


def reg32(name):
    return _SUB.get(name, name)


# ---------------------------------------------------------------- header parse
def parse_header(path):
    src = open(path, encoding="latin1").read()
    bases, methods = {}, {}
    for m in re.finditer(
        r"DECLARE_INTERFACE_\((\w+),\s*(\w+)\)\s*\{(.*?)\n\};", src, re.S
    ):
        name, base, body = m.group(1), m.group(2), m.group(3)
        body = re.sub(r"//[^\n]*", "", body)
        own = []
        for decl in re.finditer(
            r"virtual\s+[\w\*\s]+?STDMETHODCALLTYPE\s+(\w+)\s*\((.*?)\)\s*PURE", body, re.S
        ):
            own.append((decl.group(1), decl.group(2)))
        bases[name] = base
        methods[name] = own

    # d3d8.h redeclares every inherited method inside each DECLARE_INTERFACE_
    # block, so the declaration order already IS the full vtable order.
    for n, ms in methods.items():
        assert [x for x, _ in ms[:3]] == IUNKNOWN, (n, ms[:3])
    return methods


def produced_iface(params, ifaces):
    """Return the interface type of a trailing out-param, e.g. IDirect3DTexture8**."""
    if not params.strip():
        return None
    last = params.split(",")[-1]
    m = re.search(r"(IDirect3D\w+)\s*\*\s*\*", last)
    if m and m.group(1) in ifaces:
        return m.group(1)
    return None


# -------------------------------------------------------------------- PE parse
class PE:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.data = d
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        self.base = struct.unpack_from("<I", d, opt + 28)[0]
        ddir = opt + 96
        self.imp_rva = struct.unpack_from("<I", d, ddir + 8)[0]
        self.secs = []
        self.writable = []
        for i in range(nsec):
            s = opt + optsz + i * 40
            name = d[s : s + 8].rstrip(b"\0").decode("latin1")
            vsz, va, rsz, raw = struct.unpack_from("<IIII", d, s + 8)
            chars = struct.unpack_from("<I", d, s + 36)[0]
            self.secs.append((name, va, vsz, raw, rsz))
            if chars & 0x80000000:  # IMAGE_SCN_MEM_WRITE
                self.writable.append((self.base + va, self.base + va + max(vsz, rsz)))

    def off(self, rva):
        for _, va, vsz, raw, rsz in self.secs:
            if va <= rva < va + max(vsz, rsz):
                return raw + (rva - va)
        return None

    def section(self, name):
        for n, va, vsz, raw, rsz in self.secs:
            if n == name:
                return va, self.data[raw : raw + rsz]
        raise KeyError(name)

    def cstr(self, rva):
        o = self.off(rva)
        return self.data[o : self.data.index(b"\0", o)].decode("latin1")

    def iat_slot(self, dll_want, func_want):
        d, i = self.data, 0
        while True:
            ent = self.off(self.imp_rva) + i * 20
            oft, _, _, namerva, ft = struct.unpack_from("<IIIII", d, ent)
            if namerva == 0:
                return None
            dll = self.cstr(namerva)
            if dll.lower() == dll_want.lower():
                thunk = self.off(oft or ft)
                j = 0
                while True:
                    v = struct.unpack_from("<I", d, thunk + j * 4)[0]
                    if v == 0:
                        break
                    if not (v & 0x80000000):
                        if self.cstr(v + 2) == func_want:
                            return self.base + ft + j * 4
                    j += 1
            i += 1


# ------------------------------------------------------------------- analysis
def main(exe, header, out):
    ifaces = parse_header(header)
    pe = PE(exe)
    text_va, text = pe.section(".text")
    text_base = pe.base + text_va

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    # MSVC pads between functions with 0xCC, so decoding each inter-padding
    # chunk from its own start keeps instruction alignment correct -- a plain
    # linear sweep desynchronises on jump tables and then mis-decodes the
    # argument pushes we depend on.
    insns = []
    i, n = 0, len(text)
    while i < n:
        while i < n and text[i] == 0xCC:
            i += 1
        j = i
        while j < n and text[j] != 0xCC:
            j += 1
        chunk, cva, pos = text[i:j], text_base + i, 0
        while pos < len(chunk):
            adv = 0
            for ins in md.disasm(chunk[pos:], cva + pos):
                insns.append(ins)
                adv += ins.size
            pos += adv + 1
        i = j
    insns.sort(key=lambda x: x.address)
    print("disassembled %d instructions over %d bytes" % (len(insns), len(text)),
          file=sys.stderr)

    create8 = pe.iat_slot("d3d8.dll", "Direct3DCreate8")
    print("Direct3DCreate8 IAT slot = 0x%08X" % create8, file=sys.stderr)

    # globals holding interface pointers: addr -> interface name
    tagged = {}
    tagged_vtbl = {}

    # import thunks: "jmp dword ptr [IAT]" -- MSVC calls these, not the IAT directly
    thunks = set()
    for ins in insns:
        if (
            ins.mnemonic == "jmp"
            and ins.operands
            and ins.operands[0].type == CS_OP_MEM
            and ins.operands[0].mem.base == 0
            and ins.operands[0].mem.index == 0
            and (ins.operands[0].mem.disp & 0xFFFFFFFF) == create8
        ):
            thunks.add(ins.address)
    print("Direct3DCreate8 thunks: %s" % [hex(t) for t in thunks], file=sys.stderr)

    def is_create8_call(ins):
        if ins.mnemonic != "call" or not ins.operands:
            return False
        o = ins.operands[0]
        if o.type == CS_OP_IMM:
            return (o.imm & 0xFFFFFFFF) in thunks
        if o.type == CS_OP_MEM and o.mem.base == 0 and o.mem.index == 0:
            return (o.mem.disp & 0xFFFFFFFF) == create8
        return False

    # seed: mov [G], eax right after "call [Direct3DCreate8]"
    for i, ins in enumerate(insns):
        if is_create8_call(ins):
            for nxt in insns[i + 1 : i + 6]:
                if nxt.mnemonic == "mov" and nxt.operands[0].type == CS_OP_MEM:
                    m = nxt.operands[0].mem
                    if m.base == 0 and m.index == 0 and nxt.operands[1].type == CS_OP_REG:
                        tagged[m.disp & 0xFFFFFFFF] = "IDirect3D8"
    print("seed globals: %s" % {hex(k): v for k, v in tagged.items()}, file=sys.stderr)

    def is_writable(v):
        return any(lo <= v < hi for lo, hi in pe.writable)

    VOLATILE = {"eax", "ecx", "edx"}
    hits = defaultdict(list)  # (iface, method) -> [call site va]

    for _ in range(6):
        before = len(tagged) + len(tagged_vtbl)
        hits.clear()
        regs = {}
        for i, ins in enumerate(insns):
            mn, ops = ins.mnemonic, ins.operands

            if mn in ("ret", "int3", "jmp"):
                regs.clear()

            if mn == "call":
                if len(ops) == 1 and ops[0].type == CS_OP_MEM:
                    m = ops[0].mem
                    if m.index == 0 and m.base != 0:
                        rn = reg32(ins.reg_name(m.base))
                        kind, iface = regs.get(rn, (None, None))
                        if kind == "vtbl":
                            idx, rem = divmod(m.disp, 4)
                            tbl = ifaces[iface]
                            if rem == 0 and 0 <= idx < len(tbl):
                                name, params = tbl[idx]
                                hits[(iface, name)].append(ins.address)
                                # propagate: tag the global passed as out-param
                                prod = produced_iface(params, ifaces)
                                if prod:
                                    # stdcall pushes right-to-left, so the
                                    # trailing out-param is pushed FIRST, i.e.
                                    # it is the furthest-back data address in
                                    # the argument sequence.  Positional
                                    # counting fails here because the argument
                                    # sequence can contain branches (e.g. the
                                    # BehaviorFlags of CreateDevice), which
                                    # inflate the number of push instructions.
                                    cand = None
                                    for back in range(i - 1, max(0, i - 80), -1):
                                        b = insns[back]
                                        if b.mnemonic in ("call", "ret", "int3"):
                                            break
                                        if b.mnemonic == "push":
                                            po = b.operands[0]
                                            if po.type == CS_OP_IMM:
                                                v = po.imm & 0xFFFFFFFF
                                                if is_writable(v) and v % 4 == 0:
                                                    cand = v
                                        elif (
                                            b.mnemonic == "lea"
                                            and b.operands[1].mem.base == 0
                                            and b.operands[1].mem.index == 0
                                        ):
                                            v = b.operands[1].mem.disp & 0xFFFFFFFF
                                            if is_writable(v) and v % 4 == 0:
                                                cand = v
                                    if cand is not None:
                                        tagged.setdefault(cand, prod)
                for v in VOLATILE:
                    regs.pop(v, None)
                continue

            # RenderWare caches both the device pointer and its vtable pointer
            # in extra globals; propagate tags through those stores.
            if (
                mn == "mov"
                and len(ops) == 2
                and ops[0].type == CS_OP_MEM
                and ops[0].mem.base == 0
                and ops[0].mem.index == 0
                and ops[1].type == CS_OP_REG
            ):
                src = reg32(ins.reg_name(ops[1].reg))
                if src in regs:
                    kind, iface = regs[src]
                    g = ops[0].mem.disp & 0xFFFFFFFF
                    (tagged if kind == "ptr" else tagged_vtbl).setdefault(g, iface)

            # track: reg <- [tagged global]           => ("ptr", iface)
            #        reg <- [reg holding a ptr]       => ("vtbl", iface)
            #        reg <- reg                       => copy
            if mn == "mov" and len(ops) == 2 and ops[0].type == CS_OP_REG:
                dst = reg32(ins.reg_name(ops[0].reg))
                if ops[1].type == CS_OP_MEM:
                    m = ops[1].mem
                    if m.base == 0 and m.index == 0:
                        g = m.disp & 0xFFFFFFFF
                        if g in tagged:
                            regs[dst] = ("ptr", tagged[g])
                            continue
                        if g in tagged_vtbl:
                            regs[dst] = ("vtbl", tagged_vtbl[g])
                            continue
                    elif m.index == 0 and m.disp == 0 and m.base != 0:
                        src = reg32(ins.reg_name(m.base))
                        kind, iface = regs.get(src, (None, None))
                        if kind == "ptr":
                            regs[dst] = ("vtbl", iface)
                            continue
                    regs.pop(dst, None)
                elif ops[1].type == CS_OP_REG:
                    src = reg32(ins.reg_name(ops[1].reg))
                    if src in regs:
                        regs[dst] = regs[src]
                    else:
                        regs.pop(dst, None)
                else:
                    regs.pop(dst, None)
            else:
                # invalidate only registers the instruction actually writes.
                # (`push eax` reads eax -- treating operand 0 as a write here
                # silently broke the common "push this; mov ecx,[this]" pattern.)
                try:
                    _, written = ins.regs_access()
                except Exception:
                    written = [o.reg for o in ops if o.type == CS_OP_REG]
                for r in written:
                    regs.pop(reg32(ins.reg_name(r)), None)

        if len(tagged) + len(tagged_vtbl) == before:
            break

    # ------------------------------------------------------------------ report
    with open(out, "w", encoding="utf-8") as f:
        f.write("D3D8 API calls statically resolved in:\n  %s\n" % exe)
        f.write("vtable layout from: %s\n" % header)
        f.write("image base 0x%08X, .text 0x%08X\n\n" % (pe.base, text_base))
        f.write("named import from d3d8.dll:\n  Direct3DCreate8  (IAT slot 0x%08X)\n\n" % create8)
        f.write("interface pointer globals discovered:\n")
        for a, n in sorted(tagged.items()):
            f.write("  0x%08X  %s\n" % (a, n))
        f.write("\n")

        order = [n for n in ifaces]
        total = 0
        for iface in order:
            rows = [(m, v) for (i2, m), v in hits.items() if i2 == iface]
            if not rows:
                continue
            names = [n for n, _ in ifaces[iface]]
            rows.sort(key=lambda r: names.index(r[0]))
            f.write("=" * 68 + "\n%s  (%d distinct methods)\n" % (iface, len(rows)) + "=" * 68 + "\n")
            for name, sites in rows:
                total += len(sites)
                idx = names.index(name)
                f.write(
                    "%-34s vtbl[%3d] +0x%03X  %4d call site(s)\n"
                    % (name, idx, idx * 4, len(sites))
                )
                for s in sorted(sites):
                    f.write("        0x%08X\n" % s)
            f.write("\n")
        f.write("total resolved call sites: %d\n" % total)
        f.write(
            "distinct methods resolved: %d\n\n"
            % len({k for k in hits if hits[k]})
        )

        f.write("=" * 68 + "\nNOT RESOLVED STATICALLY\n" + "=" * 68 + "\n")
        f.write(
            "Methods of the interfaces above with no call site found.  For\n"
            "IDirect3D8 / IDirect3DDevice8 this is strong evidence the game\n"
            "never calls them: both live in globals that this analysis tracks\n"
            "completely.  For the resource interfaces it is NOT evidence --\n"
            "see the caveat at the end.\n\n"
        )
        for iface in order:
            seen = {m for (i2, m) in hits if i2 == iface}
            if not seen:
                continue
            missing = [n for n, _ in ifaces[iface] if n not in seen]
            f.write("%s:\n" % iface)
            for m in missing:
                f.write("    %s\n" % m)
            f.write("\n")

        f.write("=" * 68 + "\nCAVEATS\n" + "=" * 68 + "\n")
        f.write(
            "* D3D8 is COM.  The only named import from d3d8.dll is\n"
            "  Direct3DCreate8; every other call is indirect through a vtable,\n"
            "  so it cannot appear in the PE import table.\n"
            "* This resolves calls made through interface pointers held in\n"
            "  GLOBAL variables, discovered transitively from Direct3DCreate8.\n"
            "  IDirect3D8 and IDirect3DDevice8 are globals, so their coverage\n"
            "  is essentially complete.\n"
            "* Textures, surfaces and buffers created into heap structures\n"
            "  (RenderWare stores them inside RwRaster) are NOT tracked, so\n"
            "  IDirect3DTexture8::LockRect and friends are under-reported.\n"
            "  Only the few resource pointers that happen to live in globals\n"
            "  show up above.\n"
            "* Call-site counts are STATIC (how many places in the code call\n"
            "  the method), not runtime frequency.  RenderWare funnels render\n"
            "  state through single wrapper functions, which is why e.g.\n"
            "  SetRenderState has one call site but runs thousands of times\n"
            "  per frame.\n"
            "* Reachability is not proven: a resolved call site may sit in\n"
            "  code that never executes.\n"
        )
    print("wrote %s" % out, file=sys.stderr)


main(sys.argv[1], sys.argv[2], sys.argv[3])
