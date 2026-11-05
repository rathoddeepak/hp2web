"""HP2-specific additions to the nfsu2-web lifter (wrapped around lifter.insn).

- `lock` prefix (lock inc/dec/add/sub/xadd/cmpxchg/...): the web runtime runs guest
  threads one at a time (cooperative scheduler), so a guest instruction is never
  interleaved with another guest instruction; the lock prefix is dropped and the
  plain instruction is emitted by the upstream emitter.
- fnstenv/fstenv/fldenv: 28-byte protected-mode x87 environment (Intel SDM vol.1 8.1.10),
  same layout and conventions as nfsu1web's fnsave/frstor (tag word written 0, FIP/FDP 0).
"""
from capstone.x86 import X86_OP_MEM


class _Unlocked:
    """Proxy for a capstone instruction with the `lock` prefix removed from the mnemonic."""
    def __init__(self, ins):
        self._ins = ins
        self.mnemonic = ins.mnemonic.split(" ", 1)[1]

    def __getattr__(self, name):
        return getattr(self._ins, name)


def _env(lifter, ins):
    o = ins.operands
    if len(o) != 1 or o[0].type != X86_OP_MEM:
        raise ValueError(f"unexpected {ins.mnemonic} operands")
    ea = f"(uint32_t){lifter.ea(o[0].mem)}"
    if ins.mnemonic in ("fnstenv", "fstenv"):
        return ["{", f"uint8_t *_hp2_p = g_mem + {ea};",
                "uint16_t _hp2_cw = c->fcw, _hp2_sw = fsw_get(c), _hp2_tw = 0;",
                "memset(_hp2_p, 0, 28);",
                "memcpy(_hp2_p + 0, &_hp2_cw, 2);",
                "memcpy(_hp2_p + 4, &_hp2_sw, 2);",
                "memcpy(_hp2_p + 8, &_hp2_tw, 2);",
                # fnstenv masks all FPU exceptions after storing
                "c->fcw |= 0x003f;", "}"]
    return ["{", f"uint8_t *_hp2_p = g_mem + {ea};",
            "uint16_t _hp2_cw, _hp2_sw;",
            "memcpy(&_hp2_cw, _hp2_p + 0, 2);",
            "memcpy(&_hp2_sw, _hp2_p + 4, 2);",
            "c->fcw = _hp2_cw;",
            "c->top = (int)((_hp2_sw >> 11) & 7);",
            "c->fsw = (uint16_t)(_hp2_sw & ~0x3800u);", "}"]


def attach(lifter):
    inner = lifter.insn

    def insn(ins, targets):
        m = ins.mnemonic
        if m.startswith("lock "):
            return inner(_Unlocked(ins), targets)
        if m in ("fnstenv", "fstenv", "fldenv"):
            return _env(lifter, ins)
        return inner(ins, targets)

    lifter.insn = insn
