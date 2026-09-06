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

## Priority 2: snapshot-backed CFI unwind — complete

The third slice reuses the bounded `.eh_frame` state machine against immutable snapshot evidence without teaching `CoreSnapshot` any live execution behavior.

Completed bounded capability:

- `EhFrame` now exposes the smallest read-only evaluation boundary needed by both domains: an already-resolved module virtual PC, an unwind cursor carrying only known register state, and a memory-reader callback; live `Debugger` wrappers preserve their stopped-tracee/load-bias contract while snapshots supply only captured `PT_LOAD` bytes;
- the snapshot unwind cursor is seeded directly from kernel-owned crash RIP/RSP/RBP/RBX evidence and recovers a real compiler caller through the same CIE/FDE parser, CFI rule evaluator, register recovery, and stack-slot decoding used by live unwind;
- every snapshot frame resolves its runtime PC through `NT_FILE` ownership and `resolve_snapshot_module_address`, then opens the owning module's `EhFrame`; PIE and non-PIE module virtual-address conversion therefore uses snapshot mappings rather than `/proc` or a manufactured PID;
- recovered caller state must advance the stack monotonically, change PC, and remain covered by recorded snapshot module evidence; unavailable module files, unmapped return addresses, missing CFI, unreadable captured stack slots, or unsupported rules converge to the existing bounded unwind stop reasons rather than falling back to host-process state;
- the existing kernel-generated PIE and non-PIE core workflow now proves frame 0 preserves exact crash RIP/RSP/RBP and at least one caller is recovered with deterministic PC/SP ownership under both permanent GCC and Clang-large CI lanes while Priority 1 symbol/source resolution remains intact;
- the real same-width missing-module core variant also exercises unwind fail-closed behavior: the crash frame remains immutable evidence, but caller recovery stops as invalid rather than guessing a module or reading live memory;
- no CFI opcode or encoding support was broadened for this milestone, and no snapshot API can resume, signal, mutate, install breakpoints, or program watchpoints.

Priority 2 deliberately stops at immutable frame recovery. It does not yet claim caller-local source-value inspection or transplant live stop-generation/TID freshness semantics onto a core snapshot.

## Priority 3: snapshot caller inspection ownership — current frontier

The next slice should decide how the completed Phase 6 caller-inspection model consumes immutable snapshot-backed caller contexts without pretending those frames belong to a live execution domain.

Acceptance criteria:

- introduce an immutable inspection-frame identity whose freshness is tied to one owned `CoreSnapshot` instance/crash state rather than a live debugger stop sequence or process-domain selection generation;
- construct frame 0 and at least one CFI-recovered caller context from Priority 2 while preserving runtime PC, stack/frame cursor, module ownership, and only those historical registers that CFI actually recovered;
- reuse one existing compiler-proven caller source-value path from Phase 6 against snapshot memory/register evidence; do not broaden DWARF expression support merely to manufacture a post-mortem example;
- stack/local memory reads must come only from captured snapshot `PT_LOAD` bytes, and required bytes/registers absent from the core or unwind cursor must fail explicitly rather than substituting live process state;
- kernel-generated PIE and non-PIE core integration must prove the caller value is owned by the snapshot frame and remains module/type/lexical-scope qualified under GCC and Clang-large;
- no snapshot inspection context may become an execution selector or gain resume, signal, register/memory mutation, breakpoint, watchpoint, or fake PID/TID behavior.

Do not start generic frame UI, arbitrary historical-register reconstruction, or additional DWARF-expression families unless a compiler-produced snapshot caller value demonstrates a concrete missing requirement.

## Selection rule

Choose the smallest real post-mortem workflow that advances immutable inspection ownership. Do not start generic note enumeration, every Linux note type, multi-thread core selection, CLI cosmetics, or a fake live-debugger facade. Every new inspection feature must demonstrate which bytes/registers/module mappings it owns and must remain non-executable by construction.
