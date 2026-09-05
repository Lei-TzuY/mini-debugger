# Phase 3 roadmap

Phase 3 starts after the controlled-mutation closure of Phase 2. Its goal is to remove the debugger's remaining assumption that one debugging session has one immutable executable image and one thread-group topology.

## Priority 0: exec image replacement — complete

The first image-lifecycle milestone is complete for launched tracees. The implementation is an end-to-end image replacement path rather than a raw ptrace event surface:

- launch and attach tracing options include `PTRACE_O_TRACEEXEC`, and `PTRACE_EVENT_EXEC` is classified before ordinary SIGTRAP/breakpoint handling as an explicit `StopReason::Exec`;
- the exec event preserves the kernel-reported former TID, collapses the traced task registry to the surviving post-exec TID, and removes obsolete sibling-thread identity from thread selection;
- successful exec invalidates old-image managed breakpoints, hardware-watchpoint metadata, pending signals/thread starts, and pending displaced-breakpoint ownership without attempting to restore bytes or debug-register state into mappings/tasks destroyed by exec;
- if exec occurs while stepping the restored instruction behind a managed software breakpoint, the exec stop returns directly and the old-image `INT3` is never reinserted into the replacement image;
- `Debugger::executable_path()` is refreshed from `/proc/<pid>/exe`; the CLI invalidates old user/deferred breakpoint requests, rebuilds its `ElfFile` against the new image, and keeps user-breakpoint IDs monotonic across replacement;
- a real non-leader pthread executes `execve()` at a managed-breakpointed syscall instruction, exercising Linux de-thread/TID collapse rather than only leader-thread exec;
- PIE and non-PIE integration prove first image -> worker breakpoint -> displaced exec syscall -> explicit exec event -> new executable identity -> new-image symbol breakpoint -> clean exit;
- the same workflow is driven through a real `mdbg` subprocess, proving that the interactive session can set a breakpoint in the old image, cross exec, resolve and set `exec_target_probe` in the new image, hit it, and exit cleanly;
- the full GCC and Clang-large matrices cover the final executable workflow. During implementation Clang exposed an argument-evaluation-order bug in launch-time executable discovery; executable identity is now captured before moving the `Process` object.

This closes the bounded launched-exec milestone. The tracing option is also installed for attached tracees, but attached-process exec is not claimed as separately proven by this milestone.

## Priority 1: fork/vfork process topology — current frontier

Forked children are not threads in the current task registry: the existing model validates TIDs against one `/proc/<leader>/task` set and owns one process-wide software-breakpoint namespace. `PTRACE_O_TRACEFORK`/`PTRACE_O_TRACEVFORK` therefore require a process-topology abstraction rather than inserting child PIDs into the P4 thread map.

First coherent slice acceptance:

- choose and expose one deterministic parent/child follow policy before enabling fork events; do not silently trace both execution domains through one thread registry;
- classify a real `fork()`/`vfork()` event with parent and child process identity distinct from thread creation;
- maintain independent task registries and process lifecycle state for the followed execution domain while leaving the unfollowed side in a defined, testable state;
- define software-breakpoint ownership after address-space duplication so parent/child copies cannot corrupt each other's saved-byte/displaced-step state;
- route signals, detach/destructor cleanup, and terminal process exit through the chosen process identity without leaking ptrace ownership;
- expose enough session/CLI identity that stops after fork identify which process owns the event;
- PIE and non-PIE integration must run a real fork topology through the selected policy and prove breakpoint/lifecycle cleanup, not merely observe `PTRACE_EVENT_FORK`.

Do not begin this milestone by merely enabling ptrace flags. Fork/vfork is process topology, not another TID variant.

## Selection rule

Priority 1 is the current frontier. Expression evaluation, richer register classes, and convenience UI are lower priority until fork/vfork has an executable process-topology contract. As in Phase 2, compiler/kernel variants are added only when a reproducible real workflow demonstrates a gap.
