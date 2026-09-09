#!/usr/bin/env python3
import struct
import sys

ELF64_EHDR = '<16sHHIQQQIHHHHHH'
ELF64_PHDR = '<IIQQQQQQ'
ELF64_NHDR = '<III'
PT_NOTE = 4
ET_CORE = 4
EM_X86_64 = 62
NT_PRSTATUS = 1
NT_FPREGSET = 2
XMM15_OFFSET = 160 + 15 * 16
EXPECTED = [
    'efcdab89674523011032547698badcfe',
    '78695a4b3c2d1e0f1122334455667788',
]


def align4(value):
    return (value + 3) & ~3


def main(path):
    data = open(path, 'rb').read()
    ehdr = struct.unpack_from(ELF64_EHDR, data, 0)
    ident, e_type, e_machine = ehdr[0], ehdr[1], ehdr[2]
    e_phoff, e_phentsize, e_phnum = ehdr[5], ehdr[9], ehdr[10]
    if ident[:4] != b'\x7fELF' or ident[4] != 2 or ident[5] != 1:
        raise SystemExit('oracle requires little-endian ELF64')
    if e_type != ET_CORE or e_machine != EM_X86_64:
        raise SystemExit('oracle requires x86-64 ET_CORE')
    if e_phentsize != struct.calcsize(ELF64_PHDR):
        raise SystemExit('unexpected program-header size')

    thread_context = -1
    fp_by_context = {}
    for index in range(e_phnum):
        ph = struct.unpack_from(ELF64_PHDR, data, e_phoff + index * e_phentsize)
        if ph[0] != PT_NOTE:
            continue
        cursor, end = ph[2], ph[2] + ph[5]
        while cursor < end:
            namesz, descsz, n_type = struct.unpack_from(ELF64_NHDR, data, cursor)
            cursor += struct.calcsize(ELF64_NHDR)
            owner = data[cursor:cursor + namesz].split(b'\0', 1)[0]
            cursor += align4(namesz)
            desc_offset = cursor
            cursor += align4(descsz)
            if owner != b'CORE':
                continue
            if n_type == NT_PRSTATUS:
                thread_context += 1
                continue
            if n_type != NT_FPREGSET:
                continue
            if thread_context < 0:
                raise SystemExit('FPREGSET appeared before PRSTATUS context')
            if thread_context in fp_by_context:
                raise SystemExit('duplicate FPREGSET in one PRSTATUS context')
            if descsz != 512:
                raise SystemExit(f'unexpected FPREGSET size: {descsz}')
            fp_by_context[thread_context] = data[
                desc_offset + XMM15_OFFSET:desc_offset + XMM15_OFFSET + 16
            ].hex()

    actual = [fp_by_context.get(0), fp_by_context.get(1)]
    if actual != EXPECTED:
        raise SystemExit(f'genuine per-thread XMM15 evidence mismatch: {actual}')
    print('genuine core FPREGSET oracle passed: ' + ','.join(actual))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: core_fpregset_oracle.py <core>')
    main(sys.argv[1])
