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

P1-A follow-parent `fork()` is complete as the first bounded process-topology slice:

- the policy is explicit follow-parent/unfollow-child: `PTRACE_O_TRACEFORK` is enabled, `StopInfo` exposes `ProcessEventKind::Fork` plus the distinct child PID, and the child is never inserted into the followed parent's TID registry;
- the debugger consumes the kernel-created child's initial ptrace stop before releasing it, so transient ptrace ownership cannot leak outside the topology transition;
- installed software breakpoints inherited across `fork()` are restored in the child's copy-on-write address space before detach while the parent's breakpoint metadata and physical `INT3` remain owned by the parent session;
- when the forking TID owns the bounded hardware watchpoint, the child's inherited DR0/DR6/DR7 state is restored to the pre-watchpoint snapshot before detach rather than silently extending parent watchpoint ownership to the child process;
- detach failure uses a bounded best-effort kill/detach cleanup path rather than releasing a partially restored child;
- a real fork fixture executes the same managed-breakpointed probe in both domains: the child reaches an independent SIGSTOP with `TracerPid: 0`, is externally resumed and exits, while the followed parent retains its one-process task registry, later hits the original managed breakpoint, removes it, and exits cleanly;
- the fixture blocks SIGCHLD only to isolate this topology test from the already independent signal-delivery policy; production does not swallow or special-case SIGCHLD;
- PIE and non-PIE run through the full GCC and Clang-large matrices. Test-first coverage initially failed because no process-event identity existed, and the first production candidate then exposed the real child-exit/SIGCHLD ordering before the fixture isolated that orthogonal signal stop.

Priority 1 remains in progress. P1-A deliberately does not enable `PTRACE_O_TRACEVFORK`: a vfork child shares the parent's address space until exec/exit, so restoring inherited `INT3` bytes in the child would also mutate the followed parent. The next topology slice must define a shared-VM ownership protocol around `PTRACE_EVENT_VFORK`/`PTRACE_EVENT_VFORK_DONE` before enabling those events. Process-event identity is currently exposed through the debugger API; dedicated CLI rendering for fork/vfork ownership remains part of Priority 1 product integration and is not claimed complete by P1-A.

Remaining Priority 1 acceptance:

- define and prove the `vfork()` shared-address-space policy without corrupting parent breakpoint/displaced-step/watchpoint ownership;
- preserve independent lifecycle and cleanup semantics across `VFORK_DONE`, child exec/exit, signals, detach, and terminal parent exit;
- expose process-topology ownership clearly through the interactive CLI once fork/vfork semantics are stable;
- add real PIE/non-PIE vfork integration rather than relying on synthetic ptrace-event injection.

Do not complete this milestone by merely enabling `PTRACE_O_TRACEVFORK`. vfork is shared-address-space ownership, not a fork flag variant.

## Selection rule

Priority 1 remains the current frontier, with vfork/VFORK_DONE ownership as the next architectural slice. Expression evaluation, richer register classes, and convenience UI are lower priority until fork/vfork has an executable process-topology contract. As in Phase 2, compiler/kernel variants are added only when a reproducible real workflow demonstrates a gap.
