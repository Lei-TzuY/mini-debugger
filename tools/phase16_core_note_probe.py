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
NT_PRPSINFO = 3
NT_AUXV = 6
NT_SIGINFO = 0x53494749
NT_FILE = 0x46494c45
NT_X86_XSTATE = 0x202
XMM_BASE = 160
XMM_SIZE = 16
XMM15_OFFSET = XMM_BASE + 15 * XMM_SIZE


def align4(value):
    return (value + 3) & ~3


def main(path):
    data = open(path, 'rb').read()
    ehdr = struct.unpack_from(ELF64_EHDR, data, 0)
    ident, e_type, e_machine = ehdr[0], ehdr[1], ehdr[2]
    e_phoff, e_phentsize, e_phnum = ehdr[5], ehdr[9], ehdr[10]
    if ident[:4] != b'\x7fELF' or ident[4] != 2 or ident[5] != 1:
        raise SystemExit('not little-endian ELF64')
    if e_type != ET_CORE or e_machine != EM_X86_64:
        raise SystemExit('not x86-64 ET_CORE')
    if e_phentsize != struct.calcsize(ELF64_PHDR):
        raise SystemExit('unexpected program-header size')

    notes = []
    all_core = []
    fp_values = []
    for index in range(e_phnum):
        ph = struct.unpack_from(ELF64_PHDR, data, e_phoff + index * e_phentsize)
        p_type, p_offset, p_filesz = ph[0], ph[2], ph[5]
        if p_type != PT_NOTE:
            continue
        cursor = p_offset
        end = p_offset + p_filesz
        while cursor < end:
            namesz, descsz, n_type = struct.unpack_from(ELF64_NHDR, data, cursor)
            cursor += struct.calcsize(ELF64_NHDR)
            name = data[cursor:cursor + namesz].split(b'\0', 1)[0].decode('ascii', 'replace')
            cursor += align4(namesz)
            desc_offset = cursor
            cursor += align4(descsz)
            if name == 'CORE':
                all_core.append((n_type, descsz, desc_offset))
                if n_type in (NT_PRSTATUS, NT_FPREGSET, NT_X86_XSTATE):
                    notes.append((n_type, descsz, desc_offset))
                if n_type == NT_FPREGSET:
                    if descsz < XMM15_OFFSET + XMM_SIZE:
                        raise SystemExit('FPREGSET too small for x86-64 XMM15')
                    fp_values.append(data[desc_offset + XMM15_OFFSET:
                                          desc_offset + XMM15_OFFSET + XMM_SIZE].hex())

    labels = {
        NT_PRSTATUS: 'PRSTATUS', NT_FPREGSET: 'FPREGSET', NT_PRPSINFO: 'PRPSINFO',
        NT_AUXV: 'AUXV', NT_SIGINFO: 'SIGINFO', NT_FILE: 'FILE',
        NT_X86_XSTATE: 'XSTATE'
    }
    full = ', '.join(f'{labels.get(t, hex(t))}:{size}' for t, size, _ in all_core)
    print('PHASE16_ALL_CORE_NOTES=' + full)
    summary = ', '.join(f'{labels[t]}:{size}' for t, size, _ in notes)
    print('PHASE16_CORE_NOTE_EVIDENCE=' + summary)
    pr = sum(1 for t, _, _ in notes if t == NT_PRSTATUS)
    fp = sum(1 for t, _, _ in notes if t == NT_FPREGSET)
    xs = sum(1 for t, _, _ in notes if t == NT_X86_XSTATE)
    print(f'PHASE16_COUNTS prstatus={pr} fpregset={fp} xstate={xs}')
    print('PHASE16_XMM15=' + ','.join(fp_values))
    if pr == 0:
        raise SystemExit('no NT_PRSTATUS evidence')
    if fp == 0 and xs == 0:
        raise SystemExit('no extended thread-state note evidence')
    raise SystemExit(86)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: phase16_core_note_probe.py <core>')
    main(sys.argv[1])
