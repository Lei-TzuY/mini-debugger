# Phase 2 roadmap

Phase 2 starts only after the `v0.1.0` stabilization gates are satisfied. The goal is to add debugger capabilities that materially change what the project can demonstrate, not to maximize commit count or instruction-encoding coverage.

## Priority 0: decoder architecture gate — complete

The bounded source-`next` near-call recognizer has been extracted from `source_step.cpp` into the explicit `x86/call_decoder` component with direct unit tests that do not launch a tracee. Existing supported call classes retain their behavior, unsupported/truncated inputs remain explicit fallbacks, and the debugger-facing byte reader stays lazy instead of speculatively reading full instructions.

This closes the architecture gate for future decoder work without turning the project into a general-purpose disassembler. Evidence and scope boundaries are recorded in `docs/phase-2-p0-decoder.md`.

## Priority 1: dynamic-loader events and deferred breakpoints — complete

The debugger now retains symbol and source-location breakpoint intent when a target module is absent and drives bounded re-resolution from the runtime linker's real rendezvous lifecycle.

Completed slices:

- P1-A: unresolved symbol requests persist across `dlopen()` and resolve from real runtime-linker rendezvous events;
- P1-B: the CLI exposes one user breakpoint ID namespace across ordinary and deferred symbol breakpoints;
- P1-C: resolved deferred symbol requests return to pending on `dlclose()`, stale unmapped breakpoint ownership is discarded without a memory restore, and reload installs a fresh managed backend while preserving the user/request identity;
- P1-D: `file:line` requests use the same deferred controller, become pending while their module is absent, resolve from DWARF when loaded, and preserve user identity across unload/reload.

The completed integration coverage exercises pending state, loader-triggered resolution, managed breakpoint installation, unload cleanup, reload re-arming, and the shared symbol/source lifecycle. Ambiguous cross-module resolution remains an explicit error rather than an arbitrary choice.

## Priority 2: hardware watchpoints — complete

The first hardware-watchpoint milestone is intentionally bounded to one x86-64 local write watchpoint backed by DR0/DR6/DR7. Supported lengths are 1, 2, 4, and 8 bytes with natural alignment; an already occupied hardware slot is rejected rather than overwritten.

Completed capability:

- low-level `PTRACE_PEEKUSER` / `PTRACE_POKEUSER` debug-register access with explicit register bounds;
- write-watchpoint programming preserves and later restores the pre-existing DR0/DR6/DR7 state;
- DR6 B0 classification produces a distinct watchpoint stop before single-step classification, including when the watched store is the displaced instruction of a managed `INT3` breakpoint;
- deletion disables the watchpoint and restores prior hardware state;
- explicit detach and debugger-destructor detach restore debug-register ownership before the tracee resumes;
- PIE and non-PIE integration runs cover hit, delete/no-hit, software-breakpoint coexistence, bounded length/alignment rules, and lifecycle cleanup.

The one-slot/write-only bound is deliberate. Additional DR1-DR3 allocation or read/read-write mode matrices should be added only when a concrete debugging scenario requires them, not as variant farming.

## Priority 3: DWARF 5 line tables — complete

The line-table reader now supports the bounded GCC DWARF5 surface used by the repository while preserving the existing DWARF4 path.

Completed capability:

- DWARF5 address-size and segment-selector header fields are validated explicitly;
- descriptor-driven directory/file tables support bounded path and directory-index forms, including `.debug_line_str` references with section bounds;
- unsupported descriptor forms remain explicit errors instead of guessed byte widths;
- DWARF4 and DWARF5 both populate the same range representation used by address-to-source and file:line reverse lookup;
- the existing GCC `-gdwarf-5` executable fixture now passes virtual/runtime address-to-source, reverse source lookup, and a source-derived managed breakpoint hit;
- the shared CFI library is compiled as DWARF5 while its PIE/non-PIE drivers remain DWARF4, exercising mixed-version module routing, shared-object source breakpoints, source step/next, unwind source mapping, and finish.

This closes the major source-level interoperability gap without claiming support for arbitrary DWARF5 forms or DWARF64.

## Priority 4: multi-thread debugging — complete

The debugger now has an explicit bounded single-runner thread model instead of treating the thread-group leader as the process.

Completed slices:

- P4-A: launched tracees enable `PTRACE_O_TRACECLONE`; `Process` waits across traced tasks with `__WALL`, records the TID on every wait event, reads the kernel-reported child TID from `PTRACE_GETEVENTMSG`, tracks per-TID stopped/running state, and removes exited tasks from the registry. A real pthread fixture exercises clone discovery, the worker's initial ptrace stop, worker exit, leader exit, and empty-registry convergence in both PIE and non-PIE integration runs.
- P4-B: debugger execution is bound to the active stopped TID. Clone creator stops are held while the new worker's initial ptrace stop becomes the user-visible `ThreadCreated` event; register access, continue, single-step, breakpoint RIP repair, and displaced execution target that active TID. Process-wide software-breakpoint ownership remains singular while the restore/step/reinsert window executes with all other traced tasks stopped.
- P4-C: signal and teardown ownership follow the same per-TID model. Attached processes enumerate and attach every pre-existing `/proc/<pid>/task` entry, apply clone tracing per TID, detach every owned stopped task, and drain launched multi-thread teardown. Forwarded signals are injected only into the TID that owns the active signal-delivery decision.
- P4-D: traced TIDs are exposed through the debugger API and a specific stopped TID can become active only while every traced task is stopped. Register access and execution follow the selected TID; pending signal-delivery intent is retained per TID across selection changes; switching away during a process-wide software-breakpoint displaced-step window is rejected. A real attached fixture blocks the leader in `pthread_join`, selects and releases the stopped worker through a targeted signal, hits and steps its managed breakpoint, observes worker exit, then resumes the leader to clean process exit.

The completed PIE/non-PIE coverage also rejects unknown and exited TID selection deterministically and proves explicit detach plus debugger-destructor detach still release every traced task after active-thread changes.

This seals the current multi-thread milestone. The stop policy remains intentionally single-runner: one selected task executes while the other traced tasks remain stopped. Per-TID hardware debug-register ownership is handled separately by Priority 7 rather than being hidden inside the scheduler. CLI thread-list/selection presentation belongs with Priority 6 productization now that the underlying scheduling contract is stable.

## Priority 5: broader CFI recovery — complete for the current compiler-proven milestone

The bounded `.eh_frame` interpreter has been broadened only where real compiler output demonstrated an executable recovery gap.

Completed slices:

- P5-A: a real GCC C++ exception fixture proved the version-1 `zPLR` augmentation surface required for exception-bearing frames. The parser now accepts the bounded personality/LSDA/FDE metadata used by that fixture while retaining explicit malformed and unsupported-encoding failures.
- P5-B: Clang 18.1.3 with `-mcmodel=large` proved the signed 64-bit PC-relative encoding family used by the same exception recovery workflow. The interpreter accepts `pcrel|sdata4` and `pcrel|sdata8` FDE/LSDA widths plus the corresponding indirect personality form, and the CI permanently runs both the GCC baseline and Clang large-code-model lane.

The milestone remains deliberately bounded: this is not general `DW_EH_PE`, arbitrary CFI opcode, DWARF64, or arbitrary-register recovery support. Malformed/unsupported CFI remains distinguishable from "no applicable CFI", arbitrary register values are never invented, and cross-module unwind behavior remains bounded and deterministic.

Future CFI work is allowed when a new reproducible compiler-produced `.eh_frame` sequence breaks a real `bt`/`finish` workflow. In the absence of such evidence, adding encoding/opcode variants would be speculative enumeration and is not a continuation of this milestone.

## Priority 6: source display and CLI productization — complete for the current interactive milestone

The stable execution, loader, thread, source, and bounded unwind semantics are now exposed through a coherent interactive debugging surface rather than isolated library APIs.

Completed slices:

- P6-A: the completed P4 scheduling contract is exposed to users through `info threads` and `thread <tid>`. Thread listing reports each tracked TID and state with an explicit active marker; selection delegates to the existing single-runner debugger API rather than duplicating scheduling policy in the CLI. A real CLI subprocess integration drives a pthread tracee through thread creation, leader/worker selection, selected-thread register access, worker execution/exit, active ownership returning to the leader, and clean process exit in both PIE and non-PIE runs.
- P6-B: source locations render a bounded real-file excerpt with an explicit current-line marker while preserving the existing address and `file:line[:column]` output. The `list`/`l` command re-renders the active RIP, and source `step`, `next`, `finish`, plus explicit `line` lookup share the same rendering path. Missing or synthetic source paths remain non-fatal and produce a deterministic unavailable message. A real `mdbg` subprocess regression drives both PIE and non-PIE tracees through a managed breakpoint, manual listing, and source stepping against the repository's actual fixture source file.
- P6-C: module ownership survives the product boundary instead of being discarded after module-aware resolution. Breakpoint stops render `module!symbol`, backtrace frames independently qualify both symbol and source ownership, and `list`/`line`/`finish` render `module!file:line`. Source `step`/`next` preserve the resolved module path in `SourceStepResult`, so source motion uses the same qualification without re-resolving or duplicating routing policy in the CLI. The shared-object PIE/non-PIE integration drives a real `mdbg` subprocess through a shared-library breakpoint, `bt`, `list`, and source `step`, while the existing direct source-step/next checks verify the result API carries the owning ELF image.
- P6-D: source display can recover relocated source trees through explicit `set substitute-path <recorded-prefix> <local-prefix>` rules without changing the DWARF-recorded source identity or guessing filesystem locations. Prefix matching is path-component aware, repeated rules replace the same recorded prefix, and the most specific matching rule wins. The same source-display path is shared by `list`, `line`, `finish`, `step`, and `next`. A real shared-object workflow first proves the build-tree source path is unavailable, then installs an explicit substitution and reads the repository's actual `shared_cfi_library.c` context through both `list` and source motion.

Help wording and release packaging remain useful maintenance/product work, but they are no longer the highest-value architecture frontier and should not keep Priority 6 artificially open.

## Priority 7: thread-scoped hardware watchpoints — complete for the current bounded milestone

The existing single-slot x86-64 write-watchpoint capability now follows the explicit P4 thread model instead of treating debug registers as process-global or rejecting every multi-thread tracee.

Completed capability:

- `add_write_watchpoint()` binds DR0/DR6/DR7 ownership to the explicitly selected stopped TID and preserves that TID's exact pre-existing debug-register snapshot;
- selecting a different stopped TID while the watchpoint is armed neither copies the owner's debug-register state nor mutates the owner, and reselecting the owner preserves the armed watchpoint;
- a real attached pthread fixture arms the worker, proves the leader's debug registers remain unchanged, then drives a targeted worker signal through a process-wide `INT3` at the watched store and surfaces the displaced store as a worker-owned `Watchpoint` stop;
- the watched write commits before the stop, and the leader later executes the same store without inheriting the dead worker's hardware watchpoint;
- removal, explicit detach, and debugger-destructor detach restore the exact saved owner DR0/DR6/DR7 state while leaving non-owner debug registers unchanged;
- worker exit or signal termination discards stale watchpoint ownership for the dead TID, so later teardown never attempts to restore debug registers through an exited task;
- the one-slot/write-only bound remains explicit, and thread creation while a hardware watchpoint is active remains an explicit unsupported transition rather than silently guessing debug-register inheritance.

This seals the current watchpoint milestone. DR1-DR3 allocation, read/read-write mode matrices, and clone-time watchpoint propagation are not follow-up checklists; they should be revisited only when a concrete debugging workflow requires them.

## Priority 8: controlled inferior state mutation — complete

Controlled mutation now extends the existing thread, breakpoint, and watchpoint ownership model instead of exposing raw ptrace writes as a parallel subsystem.

Completed slices:

- P8-A: `Debugger::set_register()` performs a GETREGS/read-modify-write/SETREGS cycle on `stopped_tid()`, so assignment follows the explicitly selected stopped TID and leaves every non-selected TID untouched;
- register mutation is deliberately limited to the sixteen x86-64 general-purpose data/stack registers; `rip` and `eflags` remain read-only so assignment cannot silently bypass breakpoint program-counter ownership or flag semantics;
- P8-B: `Debugger::write_memory()` requires a stopped selected TID, rejects empty writes, address overflow, and writes larger than 4096 bytes, and preserves underlying ptrace failures rather than swallowing them;
- writes overlapping an installed managed software breakpoint leave the physical `INT3` in place and update its saved program byte only after all required physical writes have succeeded, so breakpoint removal/displaced execution cannot restore stale state;
- any write spanning the currently restored byte of a pending displaced-breakpoint step is rejected before the first ptrace write, preventing partial mutation of the restore/step/reinsert ownership window;
- debugger-originated memory writes do not synthesize hardware-watchpoint stops because no inferior instruction executed; integration explicitly mutates a watched data address and verifies stop ownership is unchanged;
- CLI `set register <name> <value>` and `set memory <address|symbol> <byte> [byte...]` both delegate to the debugger ownership layer rather than calling ptrace directly;
- dedicated PIE/non-PIE integration proves ordinary 64-bit data mutation/readback/restoration, deterministic bounds and ptrace errors, installed-breakpoint overlap and stale-byte prevention, pending displaced-step rejection without partial writes, watchpoint coexistence, and an actual `mdbg` subprocess write/`x` readback/restore/clean-exit workflow.

This seals Priority 8. Additional register classes or memory-format conveniences are not follow-up checklists; they should be added only when a later executable workflow requires them.

## Phase 2 closure

Priorities 0 through 8 now form a coherent debugger milestone: bounded decoding, deferred loader-aware breakpoints, hardware watchpoints, DWARF5 source mapping, single-runner multi-thread ownership, compiler-proven CFI recovery, interactive source/thread productization, thread-scoped debug-register ownership, and controlled inferior state mutation all have executable integration evidence.

The next architectural gap is no longer another variant inside those surfaces. The process layer still assumes one thread-group identity and one executable image for the lifetime of a debugging session: ptrace options cover clone events but not exec/fork/vfork lifecycle, while CLI/symbol/source state is rooted in the executable selected at launch or attach. Phase 3 promotes to process-image and process-topology lifecycle rather than farming Phase 2 corner cases. See `docs/phase-3-roadmap.md`.

## Selection rule

Phase 2 is sealed. New work should start from the highest executable Phase 3 frontier unless a reproducible regression proves a Phase 2 invariant is broken.
