# mini-debugger

A compact x86-64 Linux debugger built directly on `ptrace(2)` with small in-tree ELF64 and DWARF parsers.

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
- bounded `.eh_frame` CFI backtraces with validated frame-pointer fallback
- bounded DWARF v4 `.debug_line` address <-> file:line resolution
- CLI breakpoints by numeric address, symbol, or source line (`break mapped_source.c:400`)
- bounded source-level `step` across DWARF v4 file:line transitions
- direct-call-aware source-level `next` for canonical x86-64 `E8 rel32` calls
- `finish` through bounded `.eh_frame` CFI return-address recovery, with validated RBP fallback
- deterministic PIE/non-PIE/stripped and omitted-frame-pointer fixture coverage

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
(mdbg) continue
breakpoint at 0x55... (main+0x...)
(mdbg) step
0x55... hello.c:13
(mdbg) next
0x55... hello.c:14
(mdbg) finish
0x55... caller.c:27
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

Commands currently implemented: `continue`, `step`, `next`, `finish`, `stepi`, `regs`, `bt`, `line <address|symbol>`, `reg <name>`, `x <address|symbol> [length]`, `break <address|symbol|file:line>`, `delete <id>`, `info breakpoints`, `symbols [filter]`, `detach`, and `quit`.

## Breakpoint invariant

```text
save original byte -> write 0xCC -> continue -> SIGTRAP
-> RIP -= 1 -> restore original byte -> expose breakpoint stop
-> single-step original instruction -> reinsert 0xCC -> continue/stop
```

The breakpoint table owns the saved byte. A breakpoint being stepped over is explicit debugger state, not a CLI convention. If an attached process is detached while stopped on a managed breakpoint, RIP already points back at the original instruction and detach leaves that original byte restored.

## Backtrace and frame model

`bt` first uses the executable's `.eh_frame`. It selects the FDE covering the current RIP, evaluates the same bounded x86-64 CFA rules used by `finish`, recovers the caller RIP and caller RSP (`RSP = CFA`), and recovers caller RBP when the CFI rule explicitly describes it. That recovered cursor is then fed back into the next FDE, so code compiled with `-fomit-frame-pointer` can produce multiple frames without mutating the tracee. The unwind is capped by a hard frame limit, requires stack-pointer progress toward higher addresses, and returns a bounded partial trace when later CFI needs an unrecovered register, memory is unreadable, or a rule is unsupported.

The implemented CFI subset deliberately targets ordinary GCC/Clang x86-64 CIE version 1 `zR` records using `DW_EH_PE_pcrel | DW_EH_PE_sdata4`, plus the common CFA location/offset/register/state opcodes exercised by deterministic fixtures. Unsupported encodings, augmentations, opcodes, malformed entries, or unreadable slots fail explicitly instead of being guessed. CFI is read only from the main executable in this milestone; shared-library unwind tables are not followed.

If `.eh_frame` is absent, or the current instruction has no matching FDE, `bt` falls back to the classic bounded RBP-chain unwinder. It does not silently switch to RBP after malformed or unsupported CFI has already been selected. The legacy RBP path still rejects non-monotonic, self-referential, or implausibly large frame-pointer jumps and returns a partial trace on unreadable memory.

`finish` uses the same current-frame CFI evaluator to recover its caller return address. If `.eh_frame` is absent or has no FDE for the current instruction, it falls back to the validated preserved-RBP frame record. Once a return address is known it continues to a normal managed temporary breakpoint there. Existing user breakpoints at the return address are preserved, while a breakpoint, signal, exit, or other stop before return interrupts `finish` and removes any temporary breakpoint it owned. The omitted-frame-pointer integration fixture deliberately corrupts RBP before `finish`, so its successful PIE/non-PIE return is dependent on CFI rather than the legacy frame chain.

## Source-line model

`line <address|symbol>` interprets 32-bit-format DWARF v4 `.debug_line` programs directly and applies the executable's PIE load bias before lookup. Rows are converted into bounded half-open address ranges; malformed units, compressed line sections, multi-operation instruction tables, DWARF64, and versions other than 4 are rejected instead of guessed.

Reverse `file:line -> address` lookup reuses those parsed ranges. When a line has multiple emitted rows, the debugger chooses the lowest virtual address deterministically. Basename lookup such as `mapped_source.c:400` is accepted only when all matching rows for that line refer to the same source path; ambiguous basenames fail explicitly. `break file.c:line` then installs a normal managed software breakpoint at the resolved runtime address.

`step` is intentionally source-level while `stepi` remains one machine instruction. Source stepping starts from the current mapped `file:line`, repeatedly delegates to the existing managed instruction-step path, and stops at the first different mapped `file:line`. That means a breakpoint currently pending displaced execution is stepped correctly and reinserted by the same breakpoint state machine. A hard instruction bound prevents an unbounded walk through code without line information; signals, exits, and other non-single-step stops interrupt the operation instead of being hidden.

`next` uses the same source-line bound, but recognizes the canonical x86-64 direct near-call opcode `E8 rel32` before executing it. For that form it places a normal managed temporary breakpoint at the five-byte call's return address, continues through the callee, removes only the breakpoint it owns, and then resumes source-line comparison in the caller. Existing user breakpoints take precedence and any breakpoint or signal encountered inside the callee interrupts `next` instead of being hidden. Indirect calls (`FF /2`), prefixed call encodings, tail calls, and other instruction forms are not decoded yet; on those forms `next` falls back to instruction-driven stepping and may enter the callee.

`finish` is frame-oriented rather than line-driven. After it reaches the caller's saved return address, the CLI resolves and prints a DWARF source location when one is available; execution itself does not depend on `.debug_line` information.

Source display, DWARF5 line tables, broader CFI encodings/register recovery, shared-library CFI, and broader instruction decoding remain future work.

## Current limits

One traced process/thread only. Source mapping, source breakpoints, `step`, and `next` are limited to DWARF v4 `.debug_line`; `next` only steps over canonical `E8 rel32` direct calls and does not yet decode indirect calls. `bt` and `finish` understand the bounded main-executable `.eh_frame` subset above; multi-frame CFI continues only while each next CFA can be computed from recovered `RSP`, `RBP`, or `RIP`, and `bt` falls back to RBP only when no CFI applies to the current frame. Hardware watchpoints are not implemented. ELF extended section numbering and non-x86-64/little-endian ELF are intentionally unsupported for now.
