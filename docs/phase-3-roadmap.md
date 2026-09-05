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

## Priority 1: fork/vfork follow-parent process topology — complete

Forked children are not threads in the current task registry: the existing model validates TIDs against one `/proc/<leader>/task` set and owns one process-wide software-breakpoint namespace. Fork/vfork therefore use an explicit follow-parent/unfollow-child process-topology policy rather than inserting child PIDs into the P4 thread map.

P1-A follow-parent `fork()` is complete:

- `PTRACE_O_TRACEFORK` is enabled, `StopInfo` exposes `ProcessEventKind::Fork` plus the distinct child PID, and the child is never inserted into the followed parent's TID registry;
- the debugger consumes the kernel-created child's initial ptrace stop before releasing it, so transient ptrace ownership cannot leak outside the topology transition;
- installed software breakpoints inherited across `fork()` are restored in the child's copy-on-write address space before detach while the parent's breakpoint metadata and physical `INT3` remain owned by the parent session;
- when the forking TID owns the bounded hardware watchpoint, the child's inherited DR0/DR6/DR7 state is restored to the pre-watchpoint snapshot before detach rather than silently extending parent watchpoint ownership to the child process;
- detach failure uses a bounded best-effort kill/detach cleanup path rather than releasing a partially restored child;
- a real fork fixture executes the same managed-breakpointed probe in both domains: the child reaches an independent SIGSTOP with `TracerPid: 0`, is externally resumed and exits, while the followed parent retains its one-process task registry, later hits the original managed breakpoint, removes it, and exits cleanly;
- the fixture blocks SIGCHLD only to isolate this topology test from the already independent signal-delivery policy; production does not swallow or special-case SIGCHLD;
- PIE and non-PIE run through the full GCC and Clang-large matrices.

P1-B shared-VM-safe `vfork()` ownership is complete for the launched follow-parent policy:

- tracing enables both `PTRACE_O_TRACEVFORK` and `PTRACE_O_TRACEVFORKDONE`, and completed transitions are surfaced as `ProcessEventKind::Vfork` with the transient child PID while ownership remains on the followed parent;
- the parent stays ptrace-stopped for the entire shared-address-space window; the transient child is waited directly by the debugger and never enters the parent's thread registry;
- unlike ordinary fork handling, the debugger never restores or rewrites inherited software-breakpoint bytes through the vfork child while VM ownership is shared, so a child cannot accidentally remove the followed parent's physical `INT3`;
- inherited hardware debug-register state is per-task rather than shared VM state, so a child of the watchpoint-owning TID is restored to the pre-watchpoint DR0/DR6/DR7 snapshot before it runs, without transferring the parent's logical watchpoint ownership;
- the transient child is internally advanced only until the shared VM is released by either `_exit()` or `exec`; an exec child is detached only after its `PTRACE_EVENT_EXEC` stop, when it owns an independent image, while an exited child is already reaped;
- only after child VM release does the debugger resume the parent and require the matching `PTRACE_EVENT_VFORK_DONE`; no completed topology event is exposed before that synchronization point;
- this ordering also keeps a restored byte behind any pending parent displaced-breakpoint step untouched throughout the shared-VM window; normal breakpoint reinsertion occurs only after the vfork handler returns past `VFORK_DONE`;
- a real fixture executes two transitions in one session—one vfork child releases via `_exit()`, another via `exec()`—while a managed parent breakpoint and thread-owned hardware watchpoint remain armed across both transitions; the parent then hits the original breakpoint, hits the original watchpoint on its data write, removes both, and exits cleanly;
- PIE and non-PIE run through the full GCC and Clang-large matrices. Test-first coverage failed only because no vfork process event existed; the final protocol proves both child-release paths rather than relying on synthetic ptrace-event injection.

P1-C interactive process-topology rendering is complete:

- the CLI recognizes process-topology metadata before generic `SIGTRAP` rendering, so a `Fork` stop is presented as `process fork: followed parent <pid>, child <pid> unfollowed` and a `Vfork` stop as `process vfork: followed parent <pid>, transient child <pid> unfollowed`;
- presentation uses the already-proven `StopInfo::process_event` and `child_pid` contract; it does not add a second process registry or alter ptrace ownership;
- a real `mdbg` subprocess drives the existing fork workflow, parses both identities from the stop text, verifies the child is independently stopped with `TracerPid: 0`, releases that unfollowed child, and then observes the followed parent exit cleanly;
- a real `mdbg` subprocess also drives both vfork release paths in one session and proves the followed parent identity stays stable while each transient child is identified as unfollowed;
- test-first coverage built cleanly in both compiler lanes and failed only because the topology marker was absent; the production renderer returns the full PIE/non-PIE suite to green under GCC and Clang-large.

Priority 1 is complete for the bounded launched follow-parent policy. Attached-process fork/vfork remains an explicit unproven evidence boundary rather than being implied by these launched workflows. Follow-child and simultaneous multi-process debugging are intentionally outside this completed policy.

## Priority 2: bounded follow-child fork handoff — current frontier

The next architectural gap is not another fork/vfork display variant. The session can now observe a child while deterministically retaining the parent, but it cannot transfer its single followed-process identity to a regular fork child. Priority 2 adds one bounded follow-child handoff without creating a simultaneous multi-process debugger.

First coherent slice acceptance:

- choose an explicit follow-child policy for a real `fork()` and surface that policy through executable API/CLI behavior; the default completed follow-parent policy must remain deterministic and unchanged;
- after `PTRACE_EVENT_FORK`, consume the child's initial ptrace stop, restore debugger-owned software-breakpoint bytes and any debugger-owned hardware debug-register state in the parent's copy-on-write execution domain, then detach the parent before allowing it to run independently;
- adopt the child as the session's sole process identity and task registry without temporarily pretending the child PID is a thread of the old parent;
- transfer the child copy's inherited managed-breakpoint ownership coherently: physical `INT3` state, saved original bytes, pending displaced-step constraints, and user-breakpoint identity may not diverge merely because process ownership moved;
- when the forking TID owns the bounded hardware watchpoint, transfer logical ownership only to the adopted child while restoring the detached parent's pre-watchpoint debug-register snapshot;
- subsequent register/memory operations, signals, breakpoints, detach/destructor cleanup, and terminal exit must route through the adopted child identity; the detached parent must remain externally owned and untraced;
- PIE and non-PIE integration must execute a real fork where the parent and child take distinguishable paths, prove the parent becomes untraced, prove the adopted child hits a retained managed breakpoint, and finish with no leaked ptrace ownership under both GCC and Clang-large.

Start with ordinary `fork()`. Do not generalize this slice to vfork follow-child: shared-VM handoff before `VFORK_DONE` has different ownership constraints and requires separate evidence. Do not introduce a multi-process registry merely to implement one ownership transfer.

## Selection rule

Priority 2 is the current frontier, with regular-fork follow-child handoff as the next coherent architectural slice. Attached-origin lifecycle evidence, vfork follow-child, expression evaluation, richer register classes, and convenience UI remain lower priority until one-process ownership can actually transfer across a real fork. As in earlier phases, kernel/compiler variants are added only when a reproducible workflow demonstrates a gap.
