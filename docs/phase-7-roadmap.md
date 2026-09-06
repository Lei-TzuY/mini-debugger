# Phase 7 roadmap

Phase 7 begins after Phase 6 separates live execution ownership from caller-frame inspection ownership. Its architectural hypothesis is **immutable post-mortem inspection**: source/symbol/unwind/value inspection should be able to operate on an owned ELF64 core snapshot without pretending that snapshot has a resumable ptrace task.

A core snapshot is not a `Process`. It cannot continue, single-step, receive a signal, mutate registers/memory, own software-breakpoint displacement, or program hardware debug registers. Its registers and memory are immutable evidence captured at one crash state. Phase 7 must preserve that distinction rather than adding a fake PID/TID behind the existing live debugger API.

## Priority 0: bounded ELF64 core snapshot ownership — complete

The first slice establishes a real post-mortem data plane before adding source-level commands.

Completed bounded capability:

- `CoreSnapshot` is an immutable ELF-domain type rather than a `Process`/`Debugger` mode; its public surface exposes only owned crash registers, captured-memory reads, fatal-signal/TID identity, and file-backed mapping lookup, so live resume/step/signal/mutation/breakpoint/watchpoint operations are unavailable by construction;
- the parser accepts only current little-endian x86-64 `ET_CORE`, validates the complete program-header table, `PT_LOAD` address/file bounds, ELF-note headers/alignment/payloads, and rejects overflowed, truncated, malformed, wrong-type, or unsupported core structure explicitly;
- Linux `CORE`/`NT_PRSTATUS` supplies the concrete crashed-thread `user_regs_struct` plus TID and fatal signal; a deterministic worker keeps `r12=0x13579bdf2468ace0` live in one assembly range so the captured register is evidence from the kernel core rather than a parser-authored fixture;
- captured `PT_LOAD` file bytes support bounded immutable reads while unmapped, cross-segment, overflowed, or `p_memsz`-only bytes absent from the core file fail explicitly;
- Linux `CORE`/`NT_FILE` mappings retain start/end, file offset, and pathname identity, and the real crash RIP is required to belong to a file mapping rather than being attributed from the host debugger process;
- permanent CI configures a deterministic kernel core pattern, raises the core-size limit, and the existing PIE and non-PIE debugger fixtures are genuinely terminated by `SIGSEGV`; `elf_tests` then parses those kernel-generated artifacts, verifies the crashing worker/TID/register marker, reads captured stack bytes, and proves RIP maps back to the actual fixture image;
- malformed parser coverage is derived from the real generated core and verifies truncated ELF headers, non-`ET_CORE` input, out-of-range program-header tables, and out-of-range `PT_NOTE` segments are rejected.

Priority 0 deliberately does not claim multi-thread note selection, shared-library reconstruction beyond the recorded `NT_FILE` mapping table, CFI/source-value reuse, or interactive core-file commands.

## Priority 1: snapshot module, symbol, and source inspection — current frontier

The next slice should translate the immutable snapshot's recorded mappings into the existing executable/DWARF inspection layers without routing through live `/proc/<pid>/maps` or a fake `Debugger`.

Acceptance criteria:

- derive the owning `NT_FILE` mapping and module-relative virtual address for the crashed RIP, including correct PIE/non-PIE load-bias handling from snapshot mapping/file offsets rather than a host PID;
- reuse `ElfFile` symbol tables to resolve the real crash PC to a module-qualified symbol, but do not add a second symbol parser or call the live `ElfFile::load_bias(pid)` path;
- resolve a real compiler-produced source location for the crash PC from the owning executable/DWARF data while preserving module identity and explicit source-path failure semantics;
- introduce the smallest read-only module-inspection boundary needed by both live and snapshot consumers, or a snapshot-specific adapter when sharing would conflate ownership; do not make `CoreSnapshot` impersonate `Process`/`Debugger`;
- PIE and non-PIE integration must prove the same kernel-generated core can identify `module!symbol` and source for the crash site using only snapshot mappings plus on-disk module debug information;
- missing/unavailable mapped module files, mismatched mapping arithmetic, and addresses outside recorded mappings must fail deterministically instead of guessing from the host filesystem/process.

Priority 1 is intentionally not backtrace recovery. Once crash-frame module/symbol/source ownership is proven, the following slice should adapt CFI to a read-only register/memory provider so unwind can consume `CoreSnapshot` evidence without inheriting live execution semantics.

## Selection rule

Choose the smallest real post-mortem workflow that advances immutable inspection ownership. Do not start generic note enumeration, every Linux note type, multi-thread core selection, CLI cosmetics, or a fake live-debugger facade. Every new inspection feature must demonstrate which bytes/registers/module mappings it owns and must remain non-executable by construction.
