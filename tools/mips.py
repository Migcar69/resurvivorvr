"""MIPS R3000A (PS1) instruction decoder shared by the disassembler and the recompiler."""

REG = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3',
       't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
       't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


class Ins:
    __slots__ = ('addr', 'word', 'op', 'rs', 'rt', 'rd', 'sa', 'imm', 'simm', 'target', 'kind', 'mn')

    def __init__(self, addr, word):
        self.addr = addr
        self.word = word
        self.op = word >> 26
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.imm = word & 0xffff
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.target = None
        self.kind = 'alu'  # alu, load, store, branch, jump, jal, jr, jalr, cop, sys, invalid
        self.mn = '???'
        _decode(self)

    def branch_target(self):
        return (self.addr + 4 + (self.simm << 2)) & 0xffffffff

    def has_delay(self):
        return self.kind in ('branch', 'jump', 'jal', 'jr', 'jalr')

    def __str__(self):
        return f'{self.addr:08x}: {self.word:08x}  {disasm(self)}'


SPECIAL = {
    0x00: 'sll', 0x02: 'srl', 0x03: 'sra', 0x04: 'sllv', 0x06: 'srlv', 0x07: 'srav',
    0x08: 'jr', 0x09: 'jalr', 0x0c: 'syscall', 0x0d: 'break',
    0x10: 'mfhi', 0x11: 'mthi', 0x12: 'mflo', 0x13: 'mtlo',
    0x18: 'mult', 0x19: 'multu', 0x1a: 'div', 0x1b: 'divu',
    0x20: 'add', 0x21: 'addu', 0x22: 'sub', 0x23: 'subu',
    0x24: 'and', 0x25: 'or', 0x26: 'xor', 0x27: 'nor', 0x2a: 'slt', 0x2b: 'sltu',
}
PRIMARY = {
    0x02: 'j', 0x03: 'jal', 0x04: 'beq', 0x05: 'bne', 0x06: 'blez', 0x07: 'bgtz',
    0x08: 'addi', 0x09: 'addiu', 0x0a: 'slti', 0x0b: 'sltiu', 0x0c: 'andi', 0x0d: 'ori',
    0x0e: 'xori', 0x0f: 'lui',
    0x20: 'lb', 0x21: 'lh', 0x22: 'lwl', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu', 0x26: 'lwr',
    0x28: 'sb', 0x29: 'sh', 0x2a: 'swl', 0x2b: 'sw', 0x2e: 'swr',
    0x32: 'lwc2', 0x3a: 'swc2',
}


def _decode(i):
    op = i.op
    if op == 0:
        fn = i.word & 63
        i.mn = SPECIAL.get(fn, '???')
        if i.mn == '???':
            i.kind = 'invalid'
        elif i.mn == 'jr':
            i.kind = 'jr'
        elif i.mn == 'jalr':
            i.kind = 'jalr'
        elif i.mn in ('syscall', 'break'):
            i.kind = 'sys'
    elif op == 1:
        rt = i.rt
        names = {0: 'bltz', 1: 'bgez', 0x10: 'bltzal', 0x11: 'bgezal'}
        i.mn = names.get(rt, '???')
        i.kind = 'branch' if i.mn != '???' else 'invalid'
        i.target = i.branch_target()
    elif op in (2, 3):
        i.mn = PRIMARY[op]
        i.kind = 'jump' if op == 2 else 'jal'
        i.target = ((i.addr + 4) & 0xf0000000) | ((i.word & 0x3ffffff) << 2)
    elif op in (4, 5, 6, 7):
        i.mn = PRIMARY[op]
        i.kind = 'branch'
        i.target = i.branch_target()
    elif op in (0x10, 0x12):
        i.kind = 'cop'
        cop = op & 3
        if i.rs & 0x10:
            fn = i.word & 63
            if cop == 0:
                i.mn = {0x10: 'rfe', 0x01: 'tlbr', 0x02: 'tlbwi', 0x06: 'tlbwr', 0x08: 'tlbp'}.get(fn, '???')
            else:
                i.mn = 'cop2'
        else:
            i.mn = {0: f'mfc{cop}', 2: f'cfc{cop}', 4: f'mtc{cop}', 6: f'ctc{cop}'}.get(i.rs, '???')
            if i.mn == '???':
                i.kind = 'invalid'
    elif op in PRIMARY:
        i.mn = PRIMARY[op]
        if 0x20 <= op <= 0x26 or op == 0x32:
            i.kind = 'load'
        elif 0x28 <= op <= 0x2e or op == 0x3a:
            i.kind = 'store'
    else:
        i.kind = 'invalid'


def disasm(i):
    r = REG
    mn = i.mn
    if i.word == 0:
        return 'nop'
    if i.kind == 'invalid':
        return f'.word 0x{i.word:08x}'
    if i.op == 0:
        if mn in ('sll', 'srl', 'sra'):
            return f'{mn} {r[i.rd]}, {r[i.rt]}, {i.sa}'
        if mn in ('sllv', 'srlv', 'srav'):
            return f'{mn} {r[i.rd]}, {r[i.rt]}, {r[i.rs]}'
        if mn == 'jr':
            return f'jr {r[i.rs]}'
        if mn == 'jalr':
            return f'jalr {r[i.rd]}, {r[i.rs]}'
        if mn in ('syscall', 'break'):
            return f'{mn} 0x{(i.word >> 6) & 0xfffff:x}'
        if mn in ('mfhi', 'mflo'):
            return f'{mn} {r[i.rd]}'
        if mn in ('mthi', 'mtlo'):
            return f'{mn} {r[i.rs]}'
        if mn in ('mult', 'multu', 'div', 'divu'):
            return f'{mn} {r[i.rs]}, {r[i.rt]}'
        return f'{mn} {r[i.rd]}, {r[i.rs]}, {r[i.rt]}'
    if i.kind in ('jump', 'jal'):
        return f'{mn} 0x{i.target:08x}'
    if i.kind == 'branch':
        if mn in ('beq', 'bne'):
            return f'{mn} {r[i.rs]}, {r[i.rt]}, 0x{i.target:08x}'
        return f'{mn} {r[i.rs]}, 0x{i.target:08x}'
    if mn == 'lui':
        return f'lui {r[i.rt]}, 0x{i.imm:04x}'
    if mn in ('andi', 'ori', 'xori'):
        return f'{mn} {r[i.rt]}, {r[i.rs]}, 0x{i.imm:x}'
    if mn in ('addi', 'addiu', 'slti', 'sltiu'):
        return f'{mn} {r[i.rt]}, {r[i.rs]}, {i.simm}'
    if i.kind in ('load', 'store'):
        rt = f'${i.rt}' if mn in ('lwc2', 'swc2') else r[i.rt]
        return f'{mn} {rt}, {i.simm}({r[i.rs]})'
    if i.kind == 'cop':
        if mn == 'cop2':
            return f'cop2 0x{i.word & 0x1ffffff:07x}'
        if mn == 'rfe':
            return 'rfe'
        return f'{mn} {r[i.rt]}, ${i.rd}'
    return mn
