# Phase 3 roadmap

Phase 3 starts after the controlled-mutation closure of Phase 2. Its goal is to remove the debugger's remaining assumption that one debugging session has one immutable executable image and one thread-group topology.

## Priority 0: exec image replacement — current frontier

Current architecture stops short of a real `execve()` lifecycle. Launch/attach configure `PTRACE_O_TRACECLONE`, but there is no `PTRACE_O_TRACEEXEC` event path. `Process` therefore has no explicit image-replacement event, while the CLI, symbol/source resolver, deferred-breakpoint controller, and managed breakpoint ownership are rooted in the executable image selected when the session starts. Treating the exec SIGTRAP as an ordinary trap would leave stale ELF/module/breakpoint state attached to mappings that no longer exist.

First coherent slice acceptance:

- enable and classify `PTRACE_O_TRACEEXEC` as an explicit traced lifecycle event with the concrete executing TID; it must not fall through ordinary SIGTRAP/breakpoint classification;
- prove a launched tracee can `execve()` a deterministic second executable and remain under debugger control after image replacement;
- invalidate old-image managed software-breakpoint ownership without attempting to restore bytes into mappings destroyed by exec;
- forbid or explicitly resolve exec while a software breakpoint displaced-step ownership window is pending; stale restored-byte state may not cross the image boundary;
- refresh the session's executable identity and ELF/module/source context so post-exec symbol lookup and a managed breakpoint in the new image are executable, not merely observable as a raw ptrace event;
- define cleanup for thread identity collapse across exec: obsolete sibling TIDs must not remain selectable after the new process image is installed;
- PIE and non-PIE integration must drive a real first-image -> `execve()` -> second-image workflow, resolve a symbol from the second image, hit it, and exit cleanly.

This is intentionally an image-lifecycle vertical slice, not an `Exec` enum/API placeholder. A process-level event without debugger/ELF/breakpoint re-binding does not complete the milestone.

## Priority 1: fork/vfork process topology — gated by exec lifecycle

Forked children are not threads in the current task registry: the existing model validates TIDs against one `/proc/<leader>/task` set and owns one process-wide software-breakpoint namespace. `PTRACE_O_TRACEFORK`/`PTRACE_O_TRACEVFORK` therefore require a process-topology abstraction rather than inserting child PIDs into the P4 thread map.

Do not begin this milestone by merely enabling ptrace flags. The first fork slice must define parent/child follow policy, independent thread registries, breakpoint ownership after address-space duplication, signal/teardown routing, and deterministic CLI identity before claiming fork support.

## Selection rule

Start with Priority 0. Fork/vfork, expression evaluation, richer register classes, and convenience UI are lower priority until exec image replacement has an executable end-to-end contract. As in Phase 2, compiler/kernel variants are added only when a reproducible real workflow demonstrates a gap.
