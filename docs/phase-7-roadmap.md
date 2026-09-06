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

## Priority 1: snapshot module, symbol, and source inspection — complete

The second slice connects immutable snapshot mapping evidence to the existing executable and DWARF inspection layers without routing through live `/proc/<pid>/maps` or a fake `Debugger`.

Completed bounded capability:

- `CoreSnapshot` exposes its already-owned `NT_FILE` mapping table as an immutable view, while `ElfFile` exposes the same zero-file-offset load virtual base already used by live load-bias recovery; neither type gains execution semantics;
- `resolve_snapshot_module_address` finds the owning `NT_FILE` mapping for a snapshot runtime address and, for ET_DYN/PIE images, derives load bias from the same module's offset-zero snapshot mapping plus the ELF load virtual base; ET_EXEC addresses remain their ELF virtual addresses;
- snapshot symbol inspection reuses `ElfFile::find_symbol_by_virtual_address` and returns module-qualified ownership rather than creating a second symbol parser or calling `ElfFile::load_bias(pid)`;
- snapshot source inspection reuses `DwarfLineTable::find_virtual_address`, preserving module path, file, line, and column without consulting a live process;
- the existing kernel-generated PIE and non-PIE cores must resolve their real crash RIP into the actual fixture module, the compiler-emitted `register_mutation_worker` symbol range, and a real `debugger_fixture.c` line-table row under both permanent GCC and Clang-large CI lanes;
- addresses outside `NT_FILE` mappings fail explicitly, and a real core variant whose recorded fixture module path is replaced by an unavailable same-width path must fail module inspection rather than guessing from the host filesystem or debugger process.

Priority 1 remains deliberately crash-frame-only. It does not claim stack unwinding, historical caller-register recovery, local-value evaluation, or an interactive core-file debugger facade.

## Priority 2: snapshot-backed CFI unwind — current frontier

The next slice should adapt the existing `.eh_frame` recovery machinery to immutable snapshot evidence without teaching `CoreSnapshot` any live execution behavior.

Acceptance criteria:

- introduce the smallest read-only register/memory provider boundary needed by CFI so live `Debugger` and immutable `CoreSnapshot` can supply equivalent evidence without sharing resume, signal, breakpoint, watchpoint, or mutation ownership;
- seed the unwind cursor from the snapshot's crashed-thread registers and recover at least one real caller frame from compiler-produced `.eh_frame` using captured `PT_LOAD` stack bytes;
- resolve each unwind cursor against snapshot `NT_FILE` module ownership and the owning module's `EhFrame`, including PIE/non-PIE load-bias conversion without `/proc`;
- preserve current bounded CFI rule semantics and fail closed when a required caller register, stack word, mapped module file, CFI record, or captured memory range is unavailable; do not broaden opcode support without compiler-produced evidence;
- kernel-generated PIE and non-PIE core integration must prove the crash frame plus at least one recovered caller has deterministic runtime PC/SP ownership and that the crash frame remains module/symbol/source resolvable through Priority 1;
- no API introduced by this slice may resume or mutate a snapshot, and no snapshot adapter may manufacture a PID solely to reuse live paths.

Priority 2 is not yet caller-local inspection. Once immutable CFI recovery is proven, a later slice may decide whether Phase 6 inspection-frame/source-value ownership can consume snapshot-backed caller contexts without violating freshness or process-domain invariants.

## Selection rule

Choose the smallest real post-mortem workflow that advances immutable inspection ownership. Do not start generic note enumeration, every Linux note type, multi-thread core selection, CLI cosmetics, or a fake live-debugger facade. Every new inspection feature must demonstrate which bytes/registers/module mappings it owns and must remain non-executable by construction.
