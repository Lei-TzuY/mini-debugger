# mini-debugger

A compact x86-64 Linux debugger built directly on `ptrace(2)` with small in-tree ELF64 and DWARF line-table parsers.

Implemented now:

- launch under `PTRACE_TRACEME` and attach to an existing PID with `PTRACE_ATTACH`
- explicit launched-vs-attached process ownership and lifecycle states
- safe detach that restores debugger-owned software breakpoints before releasing the tracee
- x86-64 general-purpose register inspection
- errno-correct memory reads
- software breakpoints with saved-byte ownership, RIP repair, displaced-instruction single-step, and reinsertion
- explicit signal suppression/forwarding policy
- ELF64 `.symtab` / `.dynsym` parsing
- PIE vs non-PIE load-bias resolution through `/proc/<pid>/maps`
- symbol -> runtime address and runtime address -> symbol resolution
- bounded x86-64 frame-pointer unwinding with symbolized `bt`
- bounded DWARF v4 `.debug_line` address <-> file:line resolution
- CLI breakpoints by numeric address, symbol, or source line (`break mapped_source.c:400`)
- deterministic PIE/non-PIE/stripped fixture coverage

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Requirements: Linux, x86-64, CMake 3.20+, and a C++17 compiler. The test environment must permit ptrace operations.

## CLI

Launch a new tracee:

```text
$ ./build/mdbg ./hello
stopped after exec
(mdbg) break main
Breakpoint 1 at 0x55... (main)
(mdbg) continue
breakpoint at 0x55... (main)
(mdbg) bt
#0 0x55... main
#1 0x7f...
(mdbg) line main
0x55... hello.c:12
(mdbg) break hello.c:12
Breakpoint 2 at 0x55... (hello.c:12)
```

Attach to an existing process:

```text
$ ./build/mdbg --attach 12345
attached to process 12345
(mdbg) regs
...
(mdbg) detach
detached
```

Attach permission is governed by the host kernel's ptrace policy. Detaching restores all debugger-owned `INT3` bytes first; quitting an attached session also detaches instead of killing the target.

Commands currently implemented: `continue`, `stepi`, `regs`, `bt`, `line <address|symbol>`, `reg <name>`, `x <address|symbol> [length]`, `break <address|symbol|file:line>`, `delete <id>`, `info breakpoints`, `symbols [filter]`, `detach`, and `quit`.

## Breakpoint invariant

```text
save original byte -> write 0xCC -> continue -> SIGTRAP
-> RIP -= 1 -> restore original byte -> expose breakpoint stop
-> single-step original instruction -> reinsert 0xCC -> continue/stop
```

The breakpoint table owns the saved byte. A breakpoint being stepped over is explicit debugger state, not a CLI convention. If an attached process is detached while stopped on a managed breakpoint, RIP already points back at the original instruction and detach leaves that original byte restored.

## Backtrace model

`bt` currently follows the classic x86-64 RBP chain. It is intentionally bounded and rejects non-monotonic, self-referential, or implausibly large frame-pointer jumps. An unreadable or malformed frame returns a partial trace rather than looping or claiming reliability.

This strategy is reliable only for code compiled with frame pointers preserved (for example `-fno-omit-frame-pointer`). Optimized code that omits or repurposes RBP requires DWARF CFI / `.eh_frame`, which is not implemented yet.

## Source-line model

`line <address|symbol>` interprets 32-bit-format DWARF v4 `.debug_line` programs directly and applies the executable's PIE load bias before lookup. Rows are converted into bounded half-open address ranges; malformed units, compressed line sections, multi-operation instruction tables, DWARF64, and versions other than 4 are rejected instead of guessed.

Reverse `file:line -> address` lookup reuses those parsed ranges. When a line has multiple emitted rows, the debugger chooses the lowest virtual address deterministically. Basename lookup such as `mapped_source.c:400` is accepted only when all matching rows for that line refer to the same source path; ambiguous basenames fail explicitly. `break file.c:line` then installs a normal managed software breakpoint at the resolved runtime address.

Source display, source-level `step`/`next`, DWARF5, and DWARF CFI remain future work.

## Current limits

One traced process/thread only. Source mapping and source breakpoints are limited to DWARF v4 `.debug_line`; there is no source-level `next/step/finish`, DWARF CFI unwinding, or hardware watchpoints yet. ELF extended section numbering and non-x86-64/little-endian ELF are intentionally unsupported for now.
