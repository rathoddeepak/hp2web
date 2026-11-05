"""hp2wsfix (xan1242/hp2wsfix, NFSHP2 v242) applied to the recompiled code, at lift time.

hp2wsfix patches code bytes at runtime (injector::WriteMemory / MakeCALL / MakeNOP / MakeJMP).
Lifted C has its immediates and call targets baked in, so each patch becomes an edit of the C
emitted for that one instruction:

  LIVE  the instruction's immediate is read from its own bytes in guest memory (MR32(operand VA)),
        so runtime/hp2_widescreen.c writes the value there exactly like WriteMemory did
  CALL  the call goes to a host hook (hp2ws_*) instead of the original callee (MakeCALL)
  DROP  the instruction's memory store is skipped (MakeNOP); for x87 fstp the pop is kept so
        the FPU stack stays balanced (the NOP left the value on the stack; nothing reads it)
  JMP   the instruction becomes a jump (MakeJMP), here inside the same function
DROP and JMP test g_hp2ws_on, so HP2_WS=0 (runtime) gives the original game back.

Only addresses from hp2wsfix's HP2_242_Addresses.h; every site is checked against the emitted
code and the lift fails if a site does not look as expected. No game bytes are stored here.
"""

# instruction VA -> (operand VA, literal the emitter writes for the original immediate)
LIVE = {
    0x0040C4FE: (0x0040C501, "0x3faaaaabu"),   # ASPECT_1  4/3
    0x0053EB83: (0x0053EB84, "0x3faaaaabu"),   # ASPECT_2
    0x0053ED7B: (0x0053ED7C, "0x3faaaaabu"),   # ASPECT_3
    0x0054160D: (0x00541610, "0x320u"),        # RESX_01 / RESY_01  mov
    0x00541614: (0x00541617, "0x258u"),
    0x00541488: (0x00541489, "0x320u"),        # RESX_02 / RESY_02  push
    0x00541483: (0x00541484, "0x258u"),
    0x005414A4: (0x005414A5, "0x320u"),        # RESX_03 / RESY_03  push
    0x0054149F: (0x005414A0, "0x258u"),
    0x005415F3: (0x005415F4, "0x320u"),        # RESX_04 / RESY_04  push
    0x005415EE: (0x005415EF, "0x258u"),
    0x004D6A36: (0x004D6A39, "0x320u"),        # RESX_05 / RESY_05  cmp
    0x004D6A3F: (0x004D6A42, "0x258u"),
    0x005414DA: (0x005414DD, "0x320u"),        # RESX_06 / RESY_06  mov
    0x005414E1: (0x005414E4, "0x258u"),
    0x00541897: (0x00541899, "0x320u"),        # RESX_07 / RESY_07  cmp
    0x0054189F: (0x005418A1, "0x258u"),
    0x0045A8E9: (0x0045A8EC, "0x44200000u"),   # RESX_11 / RESY_11  640.f / 480.f (HUD)
    0x0045A8F0: (0x0045A8F3, "0x43f00000u"),
    0x00445BE2: (0x00445BE3, "0x3f800000u"),   # FE_3DYSCALE_1 (3D map actor)
    0x0044D59C: (0x0044D59D, "0x3f800000u"),   # FE_3DYSCALE_2 (car actor)
    0x0049C3F2: (0x0049C3F3, "0x3f800000u"),   # FE_3DYSCALE_3 (event tree actor)
}

READINI_FLOAT = (0x0040EC2F, 0x0040ED5C, 0x0040EE83, 0x0040F624, 0x0040F751, 0x0040F878, 0x0040FF33,
                 0x0040FFFC, 0x004100C5, 0x004105E4, 0x00410626, 0x00410668, 0x00411092, 0x004110EA,
                 0x00411142, 0x00410D9B)
# call VA -> (original callee, host hook)
CALL = {
    0x0045A8DC: (0x0059B840, "hp2ws_59b840_hook"),     # HOOK_1
    0x0059410A: (0x0059B840, "hp2ws_59b840_hook2"),    # HOOK_2 (per-element mBorders)
    0x00596F6A: (0x0059BAE0, "hp2ws_59bae0_hook"),     # HOOK_3
    0x005980D8: (0x0059BAE0, "hp2ws_59bae0_hook"),     # HOOK_4
    0x00462FDF: (0x005954A0, "hp2ws_5954a0"),          # HOOK_5
    0x00463501: (0x005954A0, "hp2ws_5954a0"),          # HOOK_6
    0x005A15A4: (0x00595550, "hp2ws_cursorpos"),       # FE_CURSORPOS
    **{a: (0x00527650, "hp2ws_readini_float") for a in READINI_FLOAT},   # FOV
}
# rendercaps.ini reader sub_00540eb0: its ReadIni_Int calls go through the host, which answers
# [Graphics]/[GraphicsFE] Width/Height/LimitResolution (hp2wsfix rewrote rendercaps.ini instead)
INI_READER = (0x00540EB0, 0x005410D0)

DROP = {
    0x00595304: "MRF32(0x6c2da8u) =",   # FE_XSCALENOP      fstp [FE x scale]
    0x00595351: "MRF32(0x6c2db4u) =",   # FE_YFUNCSCALENOP  fstp [FE functions y scale]
    0x0059533F: "MRF32(0x6c2db0u) =",   # FE_XFUNCSCALENOP  fstp [FE functions x scale]
    0x00595324: "MRF32(0x84bc48u) =",   # FE_XPOSNOP        fstp [FE x position]
    0x00453C95: "MR32(c->esi+0x68u) =",  # cuffometer mBounds writes
    0x00453C16: "MR32(c->esi+0x68u) =",
    0x00453C1F: "MR32(c->esi+0x64u) =",
    0x00453C98: "MR32(c->esi+0x64u) =",
}
JMP = {0x0046F3A5: 0x0046F3F1}          # LIMITRES: skip the resolution limiter

HOOKS = sorted({h for _, h in CALL.values()} | {"hp2ws_readini_int"})


def attach(lifter):
    inner = lifter.insn
    seen = set()

    def insn(ins, targets):
        a = ins.address
        lines = inner(ins, targets)
        if a in LIVE:
            op, lit = LIVE[a]
            if sum(l.count(lit) for l in lines) != 1:
                raise ValueError(f"hp2ws: 0x{a:08x} expected one {lit}: {lines}")
            lines = [l.replace(lit, f"MR32(0x{op:x}u)") for l in lines]
        elif a in CALL:
            callee, hook = CALL[a]
            tok = f"sub_{callee:08x}(c)"
            if ins.mnemonic != "call" or sum(l.count(tok) for l in lines) != 1:
                raise ValueError(f"hp2ws: 0x{a:08x} expected call {tok}: {lines}")
            lines = [l.replace(tok, f"{hook}(c)") for l in lines]
        elif INI_READER[0] <= a < INI_READER[1] and ins.mnemonic == "call":
            tok = "sub_00527560(c)"
            if any(tok in l for l in lines):
                lines = [l.replace(tok, "hp2ws_readini_int(c)") for l in lines]
                seen.add(("ini", a))
                return lines
            return lines
        elif a in DROP:
            store = DROP[a]
            if sum(store in l for l in lines) != 1:
                raise ValueError(f"hp2ws: 0x{a:08x} expected one store {store}: {lines}")
            lines = [f"if (!g_hp2ws_on) {{ {l} }}" if store in l else l for l in lines]
        elif a in JMP:
            if ins.mnemonic != "cmp":
                raise ValueError(f"hp2ws: 0x{a:08x} expected cmp, got {ins.mnemonic}")
            lines = [f"if (g_hp2ws_on) goto L_{JMP[a]:08x};"] + lines
        else:
            return lines
        seen.add(a)
        return lines

    lifter.insn = insn
    return seen


def expected():
    return set(LIVE) | set(CALL) | set(DROP) | set(JMP)
