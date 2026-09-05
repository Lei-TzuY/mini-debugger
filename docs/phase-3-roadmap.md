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

Priority 1 is complete for the bounded launched follow-parent policy. Attached-process fork/vfork remains an explicit unproven evidence boundary rather than being implied by these launched workflows.

## Priority 2: bounded follow-child fork handoff — complete

Priority 2 closes the one-process ownership-transfer gap for a regular `fork()` without introducing simultaneous multi-process debugging.

- `ForkFollowPolicy` is explicit and defaults to `Parent`, preserving the completed follow-parent behavior; API callers and the CLI can select `Child`, and the interactive command is `set follow-fork-mode <parent|child>`;
- after a real `PTRACE_EVENT_FORK`, the debugger consumes the child's initial ptrace stop before either process is released and preserves distinct `parent_pid` / `child_pid` topology metadata;
- the proven follow-child slice is deliberately bounded to launched, single-thread parents: attached-origin and multi-thread-parent ownership transfer fail explicitly rather than pretending that one-thread detach semantics cover those cases;
- installed managed software breakpoints remain physically installed in the child copy while the corresponding bytes are restored in the parent's copy-on-write domain before parent detach; saved original-byte and breakpoint-ID ownership therefore stay coherent with the adopted child;
- if a bounded hardware watchpoint is owned by the forking TID, the debugger reads the currently armed parent DR0/DR6/DR7 state and explicitly installs that state into the stopped child, then restores the parent to its pre-watchpoint debug-register snapshot before detach; logical watchpoint ownership is retargeted to the child only after the parent is released;
- the detached parent is removed from debugger ownership before `Process::adopt_stopped_process()` atomically rebuilds the session around the child PID and a child-only task registry; the child is never represented as a thread of the old parent;
- pending signals/thread-start metadata are cleared at the domain handoff, while a pending displaced breakpoint can continue through the existing restored-byte single-step path and reinsert the managed `INT3` into the adopted child rather than the detached parent;
- a direct PIE/non-PIE integration arms both a process-wide managed breakpoint and a thread-owned hardware watchpoint before fork, proves the parent becomes `TracerPid: 0`, proves the adopted child retains both logical IDs, then observes child breakpoint -> child hardware watchpoint -> child SIGSTOP -> child clean exit while the detached parent also exits cleanly;
- the initial implementation exposed that x86 debug-register inheritance across fork could not be used as an implicit contract: the first runtime candidate transferred logical watchpoint metadata but missed the child watchpoint. The final implementation therefore performs explicit physical DR transfer, and the same direct workflow passes under GCC and Clang-large;
- a real `mdbg` subprocess proves the interactive policy end to end: select `follow-fork-mode child`, install a symbol breakpoint, observe `process fork: parent <pid> unfollowed, followed child <pid>`, verify the parent is untraced, hit the retained child breakpoint, surface the child's independent SIGSTOP, and exit without leaked process ownership;
- the final PIE/non-PIE suite runs through both GCC and Clang-large gates. The default follow-parent fork/vfork workflows remain in the same suite and continue to pass unchanged.

This completes the bounded regular-fork follow-child ownership-transfer milestone. It does **not** claim vfork follow-child, attached-origin follow-child, multi-thread-parent handoff, or simultaneous parent/child debugging.

## Priority 3: simultaneous regular-fork process ownership — complete for the bounded two-domain milestone

Priority 3 replaces the one-active-PID assumption with one executable two-domain regular-fork model rather than another follow-policy rendering variant.

- `ForkFollowPolicy::Both` retains a launched single-thread parent and its one regular-fork child as distinct ptrace-owned process domains after consuming the child's initial stop; the child is never inserted into the parent's TID registry;
- `Debugger::processes()` exposes both process identities and one active process, while `select_process(pid)` switches only between stopped retained domains and remains separate from thread selection;
- each domain preserves its own `Process`/TID registry, stop metadata, executable identity, pending thread starts, and pending signal-delivery state; register, memory, resume, and single-step operations therefore route through the selected process/TID pair;
- process exit/signal terminal events are surfaced independently as `ProcessExited` / `ProcessSignaled`; when one retained domain terminates, the surviving stopped domain becomes active and the session closes only after the final retained process terminates;
- managed software-breakpoint maps, breakpoint IDs, pending displaced-step state, logical hardware-watchpoint state, and debug-register restore snapshots are process-domain state rather than one ambiguous session-global namespace; process selection swaps the complete debug-ownership state together with the selected `Process`;
- managed breakpoints are deliberately armed after fork retention in this first slice, once copy-on-write domains exist; parent and child can place a breakpoint at the same virtual address with distinct session IDs, hit and remove them independently, and one domain's displaced-step/remove path cannot rewrite the sibling domain;
- one pre-fork bounded write watchpoint is explicitly split at the fork stop: the parent's armed DR0/DR6/DR7 remain parent-owned, the debugger explicitly programs the child's DR state rather than relying on kernel inheritance, and the child receives an independent logical watchpoint plus restore snapshot for its own TID;
- direct PIE/non-PIE integration switches to the child, observes child managed-breakpoint then child hardware-watchpoint stops, removes both child-owned resources, surfaces child SIGSTOP and independent exit, then proves the retained parent still owns and hits its own breakpoint/watchpoint before clean final exit;
- a real `mdbg` subprocess independently proves retained parent/child identities, both `TracerPid` relationships, `info processes`, explicit process selection, selected-child single-step and SIGSTOP, copy-on-write memory divergence, child terminal promotion, and leak-free final process completion;
- test-first ownership coverage built cleanly and left 38/40 tests green, failing only because hardware watchpoints were explicitly not process-scoped; the completed production model returns the full 40-test GCC and Clang-large matrices to green.

The bounded model intentionally remains one launched, single-thread regular-fork parent plus one child. Attached-origin simultaneous fork ownership, multi-thread-parent retention, nested process trees, simultaneous vfork ownership, and pre-fork managed software-breakpoint duplication are not claimed.

## Priority 4: per-process exec image divergence — current frontier

The next architectural boundary is no longer retaining two ordinary fork domains; it is allowing those retained domains to stop sharing one executable-image assumption after one side calls `exec`.

First coherent slice acceptance:

- start from the proven two-domain regular-fork session, select one retained domain, and let that process replace its image with a real `execve()` while the sibling remains traced in the old executable;
- classify the exec stop only in the process domain that executed it: collapse that domain's task registry, refresh only its executable identity, clear only its old-image breakpoint/watchpoint/pending execution ownership, and leave the sibling's executable path, TIDs, pending signals, and debug state untouched;
- preserve explicit process selection after image divergence so switching to the old-image sibling restores its old executable identity/resolution context, while switching back to the exec'd process exposes the replacement image;
- make CLI symbol/source resolution and user/deferred breakpoint bookkeeping follow the selected process image rather than one session-global `ElfFile`; image replacement in one process may not invalidate the sibling process's user breakpoint state;
- prove a new-image symbol breakpoint in the exec'd process and an old-image managed/symbol breakpoint in the sibling can coexist and be hit independently after switching process domains;
- surface exec'd-process and old-image-sibling terminal events independently and end the session only after both domains complete;
- run one real PIE/non-PIE fork -> child exec -> divergent-image workflow through GCC and Clang-large, including a real `mdbg` subprocess rather than only direct API state inspection.

Do not expand this slice to arbitrary nested process trees, attached-origin multi-process exec, or simultaneous vfork ownership. The goal is to prove that process ownership and image ownership are independently scoped before increasing process-tree breadth.

## Selection rule

Priority 4 is the current frontier. The next work must demonstrate one retained fork domain replacing its executable without corrupting the sibling domain's image/debug state. Arbitrary process trees, attached-origin lifecycle evidence, vfork follow-child/both, expression evaluation, richer register classes, and convenience UI remain lower priority until divergent executable images work inside the two-domain session. As in earlier phases, kernel/compiler variants are added only when a reproducible workflow demonstrates a gap.
