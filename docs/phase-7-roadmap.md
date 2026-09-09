# Phase 7 roadmap

Phase 7 begins after Phase 6 separates live execution ownership from caller-frame inspection ownership. Its architectural hypothesis is **immutable post-mortem inspection**: source/symbol/unwind/value inspection should be able to operate on an owned ELF64 core snapshot without pretending that snapshot has a resumable ptrace task.

A core snapshot is not a `Process`. It cannot continue, single-step, receive a signal, mutate registers/memory, own software-breakpoint displacement, or program hardware debug registers. Its registers and memory are immutable evidence captured at one crash state. Phase 7 preserves that distinction rather than adding a fake PID/TID behind the existing live debugger API.

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

- `EhFrame` exposes the smallest read-only evaluation boundary needed by both domains: an already-resolved module virtual PC, an unwind cursor carrying only known register state, and a memory-reader callback; live `Debugger` wrappers preserve their stopped-tracee/load-bias contract while snapshots supply only captured `PT_LOAD` bytes;
- the snapshot unwind cursor is seeded directly from kernel-owned crash RIP/RSP/RBP/RBX evidence and recovers a real compiler caller through the same CIE/FDE parser, CFI rule evaluator, register recovery, and stack-slot decoding used by live unwind;
- every snapshot frame resolves its runtime PC through `NT_FILE` ownership and `resolve_snapshot_module_address`, then opens the owning module's `EhFrame`; PIE and non-PIE module virtual-address conversion therefore uses snapshot mappings rather than `/proc` or a manufactured PID;
- recovered caller state must advance the stack monotonically, change PC, and remain covered by recorded snapshot module evidence; unavailable module files, unmapped return addresses, missing CFI, unreadable captured stack slots, or unsupported rules converge to the existing bounded unwind stop reasons rather than falling back to host-process state;
- the existing kernel-generated PIE and non-PIE core workflow proves frame 0 preserves exact crash RIP/RSP/RBP and at least one caller is recovered with deterministic PC/SP ownership under both permanent GCC and Clang-large CI lanes while Priority 1 symbol/source resolution remains intact;
- the real same-width missing-module core variant also exercises unwind fail-closed behavior: the crash frame remains immutable evidence, but caller recovery stops as invalid rather than guessing a module or reading live memory;
- no CFI opcode or encoding support was broadened for this milestone, and no snapshot API can resume, signal, mutate, install breakpoints, or program watchpoints.

Priority 2 deliberately stops at immutable frame recovery. It does not transplant live stop-generation/TID freshness semantics onto a core snapshot.

## Priority 3: snapshot caller inspection ownership — complete

Priority 3 connects the completed Phase 6 caller-inspection model to immutable snapshot-backed caller contexts without pretending those frames belong to a live execution domain.

### P3-A: immutable inspection-frame ownership substrate — complete

Completed bounded capability:

- `SnapshotInspectionFrameContext` is owned by one concrete `CoreSnapshot` instance and records the crash TID/signal plus the originating crash RIP/RSP/RBP fingerprint; validation rejects the same frame when presented to a separately opened `CoreSnapshot`, even when both instances parse identical core bytes;
- frame identity retains the snapshot-owned `NT_FILE` pathname without requiring the module file to remain available on the host, preserving the Priority 2 rule that an unavailable module keeps frame 0 as immutable crash evidence and stops further unwind as invalid;
- `build_snapshot_inspection_frames` is the single snapshot CFI walk: it preserves runtime PC, stack/frame cursor, module ownership, and only the historical register state carried by the bounded unwind cursor (`RBX`, `RBP`, and `RSP`); the older `unwind_eh_frame` surface projects its `CfiStackFrame` view from this richer trace rather than maintaining a second unwind loop;
- a real kernel-generated core is produced from the existing compiler-proven Phase 6 formal-parameter fixture at the `clobber_argument_registers` callee; PIE and non-PIE tests recover the caller `inspect_entry_parameter` frame and require its historical `RBX=0x1020304050607080`, while exact-owner validation remains fail-closed under both permanent GCC and Clang-large lanes;
- the fixture crash path is opt-in (`--snapshot-crash`), so all existing live Phase 6 compiler-value coverage remains unchanged.

### P3-B: snapshot caller source-value evaluation — complete

Completed bounded capability:

- `inspect_local_value` / `inspect_local_integer` accept a `CoreSnapshot` plus an exact-owner `SnapshotInspectionFrameContext`; frame 0 is rejected because this slice is specifically caller-frame inspection and ownership validation occurs before any DWARF evaluation;
- the evaluator resolves the frame PC only through snapshot `NT_FILE` ownership, verifies that resolved module identity still matches the inspection frame, opens that recorded module's compiler DWARF, and never calls `ElfFile::load_bias(pid)`, `/proc`, or a live `Debugger`;
- the first post-mortem source-value path deliberately reuses the already compiler-proven Phase 6 scalar expression family for caller local `transformed`: `DW_OP_breg3` followed by the existing bounded XOR/stack-value form; no new DWARF expression family is accepted for this milestone;
- evaluation requires the caller frame's CFI-recovered historical `RBX`; if that evidence is absent the request fails explicitly instead of substituting the crash-frame register file or any host/live process state;
- the existing private DWARF parser/evaluator implementation is factored into one internal translation-unit include so live and snapshot adapters reuse the same parser helpers without maintaining a second parser or widening their public surface;
- real kernel-generated PIE and non-PIE cores recover caller `inspect_entry_parameter`, resolve its module-qualified `transformed` local, preserve integer width/signedness and lexical ownership, and require the exact compiler-proven value `0x458a30bf63ac1619` under both permanent GCC and Clang-large lanes;
- removing recovered caller RBX from the immutable inspection frame deterministically makes the same source-value request fail, directly proving the result is owned by historical caller evidence rather than the crash sentinel.

No snapshot inspection context gains resume, single-step, signal delivery, register/memory mutation, software/hardware breakpoint ownership, live process/thread selection, or a fake PID/TID. Missing module files, missing historical registers, unsupported source-value forms, and absent snapshot evidence remain fail-closed.

## Phase 7 seal

Phase 7 is complete for the current bounded immutable post-mortem milestone. The repo now owns a real x86-64 ELF core snapshot, resolves snapshot module/symbol/source identity, unwinds a compiler caller from captured bytes, owns immutable caller inspection contexts, and evaluates one existing compiler-proven caller source value without crossing into live execution semantics.

Further post-mortem work must not grow by enumerating ELF notes, DWARF expressions, or historical registers without a concrete failing workflow. New capability should move upward into an integrated read-only inspection experience while preserving `CoreSnapshot` as immutable evidence.

## Phase 8 promotion: read-only core inspection session — current frontier

The next architectural hypothesis is a **read-only core inspection session** that composes the completed Phase 7 primitives into a coherent user workflow without disguising a snapshot as a live debugger.

First executable gate:

- open one real kernel-generated core through an explicit core-file session entry point backed by `CoreSnapshot`, not `Process` or `Debugger`;
- expose bounded snapshot backtrace and immutable frame selection so a user can move from the crash frame to recovered caller frame 1 without changing any execution owner;
- evaluate `print transformed` from the selected caller through the completed snapshot source-value path, preserving module/source/type ownership;
- prove the workflow end to end on real PIE and non-PIE kernel cores under GCC and Clang-large, including module-qualified frame output and the expected historical caller value;
- execution or mutation operations such as continue/step/signal delivery/register or memory writes/breakpoints/watchpoints must be unavailable or explicitly rejected by the core-session command surface, not routed through dummy PID/TID state;
- unavailable modules, unreadable snapshot bytes, unsupported unwind/value evidence, and invalid frame selection remain deterministic fail-closed outcomes.

Do not implement Phase 8 as a generic command-mode enum or presentation-only shell. The first slice must prove a real `core -> backtrace -> caller frame -> source value` workflow before the new phase is considered started.

### P8-A: immutable core inspection session — complete

Completed executable capability:

- `CoreInspectionSession` owns one `CoreSnapshot`, one bounded immutable snapshot frame trace, and one selected frame index; it has no `Process`, `Debugger`, PID execution owner, resume path, or mutation API;
- the dedicated `mdbg-core <core-file>` entry point is separate from the live `mdbg` command loop and exposes only read-only `bt`, `frame`, `print`, `help`, and `quit` commands; unknown live commands such as `continue` are rejected explicitly rather than routed through dummy execution state;
- frame output preserves recorded module ownership and layers existing snapshot symbol/source resolution on top when that evidence is available; invalid immutable frame indices remain deterministic errors;
- `print` delegates directly to the selected frame's existing snapshot local-value evaluator, so historical register/module/type ownership is unchanged and unsupported evidence keeps the Phase 7 fail-closed behavior;
- real kernel-generated PIE and non-PIE cores from the compiler-proven formal-parameter fixture are driven through an actual `mdbg-core` subprocess under both permanent GCC and Clang-large lanes; the workflow requires crash and caller frames, selects caller frame 1, prints module-qualified `transformed = 0x458a30bf63ac1619`, rejects `continue`, rejects an out-of-range frame, and exits cleanly.

### P8-B: immutable multi-thread core ownership — complete

Completed executable capability:

- `CoreSnapshot` retains every bounded x86-64 `CORE/NT_PRSTATUS` record as an immutable `CoreThreadSnapshot` containing TID, note-owned signal field, full `user_regs_struct`, and explicit crash-owner identity; duplicate or invalid TIDs fail closed instead of silently aliasing one thread context;
- the first real `NT_PRSTATUS` remains the compatibility crash owner for `crashed_tid()`, `signal_number()`, and `registers()`. Existing targeted-worker core coverage already proves that this ordering identifies the worker deliberately killed with `tgkill(..., SIGSEGV)`, while later PRSTATUS records are no longer discarded;
- snapshot inspection frames are now explicitly thread-owned: their TID, signal field, and origin RIP/RSP/RBP fingerprint are validated against one exact `CoreThreadSnapshot`, while the older crash-thread build/unwind overload delegates to `crashed_thread()` and preserves all Phase 7 behavior;
- `CoreInspectionSession` owns an immutable selected-thread TID. Selecting another recorded TID rebuilds its bounded CFI trace from that thread's captured registers and the same snapshot memory, resets frame selection to frame 0, and never creates a live `Process`, performs ptrace, or changes snapshot bytes;
- `mdbg-core` adds read-only `threads` and `thread <tid>` commands. The catalogue marks the currently selected thread and crash owner, reports note-owned signal/RIP evidence, rejects unavailable TIDs deterministically, and keeps execution commands such as `continue` unsupported;
- a snapshot-only formal-parameter fixture mode creates a real blocked pthread sibling before the existing deterministic crash path. PIE and non-PIE `mdbg-core` integration therefore operate on genuine multi-thread kernel cores under GCC and Clang-large, require both the crash TID and published sibling TID in the catalogue, select the sibling and build its immutable frame trace, switch back to the crash thread, recover caller frame 1, and still evaluate the exact historical `transformed = 0x458a30bf63ac1619` value;
- normal non-snapshot compiler-value fixtures remain unchanged, and no generic ELF-note enumeration, live thread state, resume/signal semantics, or mutation surface is introduced.

Phase 8 remains the current frontier, but further thread work needs a new concrete post-mortem behavior rather than more catalogue metadata. A possible P8-C is thread-scoped source-value inspection only if a real compiler-generated sibling frame exposes a stable local/parameter ownership case that the selected immutable thread can evaluate. If such evidence is unavailable, do not farm thread names, signal variants, or arbitrary PRSTATUS fields; perform an architecture audit and promote to the next post-mortem subsystem gap instead.

## Selection rule

Choose the smallest real post-mortem workflow that advances immutable inspection ownership. Do not start generic note enumeration, every Linux note type, CLI cosmetics, or a fake live-debugger facade. Every new inspection feature must demonstrate which bytes/registers/module mappings it owns and must remain non-executable by construction.
