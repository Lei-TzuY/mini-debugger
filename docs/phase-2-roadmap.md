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

This seals the current multi-thread milestone. The stop policy remains intentionally single-runner: one selected task executes while the other traced tasks remain stopped. Hardware watchpoints remain explicitly single-thread-only because x86 debug-register ownership is per TID; broadening them requires a separate concrete debug-register allocation scenario rather than being hidden inside the thread milestone. CLI thread-list/selection presentation belongs with Priority 6 productization now that the underlying scheduling contract is stable.

## Priority 5: broader CFI recovery — complete for the current compiler-proven milestone

The bounded `.eh_frame` interpreter has been broadened only where real compiler output demonstrated an executable recovery gap.

Completed slices:

- P5-A: a real GCC C++ exception fixture proved the version-1 `zPLR` augmentation surface required for exception-bearing frames. The parser now accepts the bounded personality/LSDA/FDE metadata used by that fixture while retaining explicit malformed and unsupported-encoding failures.
- P5-B: Clang 18.1.3 with `-mcmodel=large` proved the signed 64-bit PC-relative encoding family used by the same exception recovery workflow. The interpreter accepts `pcrel|sdata4` and `pcrel|sdata8` FDE/LSDA widths plus the corresponding indirect personality form, and the CI permanently runs both the GCC baseline and Clang large-code-model lane.

The milestone remains deliberately bounded: this is not general `DW_EH_PE`, arbitrary CFI opcode, DWARF64, or arbitrary-register recovery support. Malformed/unsupported CFI remains distinguishable from "no applicable CFI", arbitrary register values are never invented, and cross-module unwind behavior remains bounded and deterministic.

Future CFI work is allowed when a new reproducible compiler-produced `.eh_frame` sequence breaks a real `bt`/`finish` workflow. In the absence of such evidence, adding encoding/opcode variants would be speculative enumeration and is not a continuation of this milestone.

## Priority 6: source display and CLI productization — current frontier

Improve presentation now that the underlying execution, loader, thread, source, and bounded unwind semantics are stable.

Completed slices:

- P6-A: the completed P4 scheduling contract is exposed to users through `info threads` and `thread <tid>`. Thread listing reports each tracked TID and state with an explicit active marker; selection delegates to the existing single-runner debugger API rather than duplicating scheduling policy in the CLI. A real CLI subprocess integration drives a pthread tracee through thread creation, leader/worker selection, selected-thread register access, worker execution/exit, active ownership returning to the leader, and clean process exit in both PIE and non-PIE runs.

Next productization candidates:

- source-context display around the current source location;
- clearer module-qualified symbol/source rendering;
- CLI help/usage consistency beyond the commands already exercised by integration;
- release packaging and examples after the interactive workflows are coherent.

The next slice should again be executable and user-visible. Prefer closing a complete interactive workflow over adding isolated presentation helpers.

## Selection rule

Choose the highest-priority milestone that has a concrete failing scenario or acceptance test and no overlapping active implementation. A milestone may be split into bounded PRs, but every PR must advance the milestone itself; repeated variant-only micro-PRs are not a roadmap strategy.
